/* launcher_gui_test_loc.c - GUI-Selbsttests LoC/Konto/Netz/Layout (Faelle aus W276-W414).
 * W462 LAUNCHER-SPLIT: aus launcher_gui.c verschoben (unveraendert), gemeinsamer Zustand
 * in launcher_gui_int.h. */
#include "launcher_gui_int.h"
#include "sb_gfxpreset.h" /* W465: SB_GP_LOC_REV */
#include "launcher_camp.h" /* W491: LCAMP_* */
/* Selbsttest (Faelle 20-25)                                            */
/* ------------------------------------------------------------------ */

void write_file(const char *path, const char *text)
{
    FILE *f = NULL;
    if (u8_fopen_s(&f, path, "wb") == 0 && f) {
        fputs(text, f);
        fclose(f);
    }
}

/* W259: Datei ganz lesen (Selbsttest, bis cap-1 Bytes). */
void gt_read(const char *path, char *buf, size_t cap)
{
    FILE *f = NULL;
    size_t n = 0;
    buf[0] = '\0';
    if (u8_fopen_s(&f, path, "rb") == 0 && f) {
        n = fread(buf, 1, cap - 1, f);
        fclose(f);
    }
    buf[n] = '\0';
    { /* CRLF (lcfg_save_user schreibt im Textmodus) -> LF */
        char *r = buf, *w = buf;
        for (; *r; r++) {
            if (*r != '\r') { *w++ = *r; }
        }
        *w = '\0';
    }
}

/* W259: "Als Standard speichern" im ESC-Menue nachstellen (sb_ovl_cfg_write:
 * erste Zeile des Keys ersetzen, sonst unter [render] anhaengen). */
void gt_cfg_set(const char *path, const char *key, const char *val)
{
    static char in[16384], out[17000];
    char *p, *line;
    size_t kl = strlen(key), o = 0;
    int done = 0;
    gt_read(path, in, sizeof in);
    for (line = in; *line; line = p) {
        size_t ll;
        p = strchr(line, '\n');
        p = p ? p + 1 : line + strlen(line);
        ll = (size_t)(p - line);
        if (!done && strncmp(line, key, kl) == 0 && line[kl] == '=') {
            o += (size_t)_snprintf_s(out + o, sizeof out - o, _TRUNCATE, "%s=%s\n", key, val);
            done = 1;
        } else if (o + ll < sizeof out) {
            memcpy(out + o, line, ll);
            o += ll;
            out[o] = '\0';
        }
    }
    if (!done) {
        o += (size_t)_snprintf_s(out + o, sizeof out - o, _TRUNCATE, "[render]\n%s=%s\n", key, val);
    }
    write_file(path, out);
}

/* W172: Umgebungsvariable (UTF-8) fuer die Selbsttests setzen. */
void gt_setenv(const char *name, const char *v)
{
    wchar_t wn[64], wv[1100];
    u8_to_w(name, wn, 64);
    u8_to_w(v, wv, 1100);
    SetEnvironmentVariableW(wn, wv);
}

/* --- 65 (W276 LOC-LAUNCH): Umschalten, getrennte Abbild-/Datenpfade,
 * gemeinsame Einstellungen, LoC-Start gesperrt mit Meldung (EN/DE).
 * Laeuft im offenen Test-Fenster (ausserhalb des Bildschirms, ohne Fokus);
 * user.cfg = Temp-Datei (g_ucfg_test), Abbilder = Pruef-Fixture. --- */
static void loctab_vis(int id, int want, const char *what, char *why, size_t cap)
{
    HWND h = ctl(id);
    int has = h && (GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE) != 0;
    if (!why[0] && has != want) {
        _snprintf_s(why, cap, _TRUNCATE, "%s: %s %s", what, id >= ID_NAV0 && id < ID_NAV0 + P_N
                    ? kPageKey[id - ID_NAV0] : "Element", want ? "unsichtbar" : "sichtbar");
    }
}

void loctab_gui_selftest(int *pass, int *fail)
{
    char why[300] = "", tmp[1024], cfg[1200], buf[8192], t[1200], hp[1200], keep[16];
    launcher_settings_t s0, s1;
    COLORREF col = 0;
    int k;
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw276_gui_user.cfg", tmp);
    {
        FILE *f = NULL;
        if (u8_fopen_s(&f, cfg, "w") == 0 && f) {
            fputs("[display]\nwidth=1280\n# fremde Zeile\n", f);
            fclose(f);
        }
    }
    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, cfg);
    strcpy_s(keep, sizeof keep, sb_lang_code());
    sb_lang_set("en");
    ui_apply_language();
    loctab_load();
    g_game = LGAME_SB;
    loctab_nav_layout();
    /* SB-Abbild auf der SB-Seite (eigener Pfad) */
    lcfg_defaults(&s0);
    s0.xbe_path[0] = s0.media_root[0] = '\0';
    strcpy_s(s0.game_image, sizeof s0.game_image, LOC_SB_ISO);
    s0.width = 1600;
    s0.height = 900;
    ui_from_settings(&s0);
    show_page(P_DISPLAY);
    pump();
    /* 1: Umschalten auf LoC */
    on_command(ID_GAME_LOC, BN_CLICKED);
    pump();
    if (g_game != LGAME_LOC || !ui_checked(ID_GAME_LOC) || ui_checked(ID_GAME_SB)) {
        strcpy_s(why, sizeof why, "Umschalter LoC: Zustand");
    }
    if (!why[0] && g_page != P_DISPLAY) {
        strcpy_s(why, sizeof why, "Umschalten verlaesst die gemeinsame Seite Anzeige");
    }
    for (k = 0; k < P_N; k++) { /* gemeinsame Seiten bleiben, W293: Erweitert auch; W392: Packs-Platz = Netzwerk */
        loctab_vis(ID_NAV0 + k, k != P_ALPHA, "LoC", why, sizeof why); /* W397: Test tools unter dem Zahnrad */
    }
    if (!why[0]) { /* W397: LoC-Leiste Play, Display, Graphics, Controls, Audio & extras, Multiplayer, Advanced */
        uictl_t *c = ui_find(ID_NAV0 + P_CTRL), *a = ui_find(ID_NAV0 + P_AUDIO), *mp = ui_find(ID_NAV0 + P_LNET);
        uictl_t *ad = ui_find(ID_NAV0 + P_ADV);
        char nt[300];
        ui_gettext(ctl(ID_NAV0 + P_PACKS), nt, sizeof nt);
        if (!c || !a || !mp || !ad || c->y != TITLE_H + 16 + 3 * 44 || a->y != TITLE_H + 16 + 4 * 44 ||
            mp->y != TITLE_H + 16 + 5 * 44 || ad->y != TITLE_H + 16 + 6 * 44) {
            strcpy_s(why, sizeof why, "Navigation LoC: Reihenfolge/Luecke");
        } else if (strncmp(nt, LT("gui.page_mp"), strlen(LT("gui.page_mp"))) != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Navigation LoC: Packs-Platz heisst '%s'", nt);
        } else if (nav_next(P_AUDIO, 1) != P_LNET || nav_next(P_LNET, 1) != P_ADV || nav_next(P_GFX, 1) != P_CTRL) {
            strcpy_s(why, sizeof why, "Navigation LoC: Strg+Tab-Reihenfolge");
        }
    }
    loctab_vis(ID_WINMODE, 1, "LoC Anzeige (gemeinsam)", why, sizeof why);
    gt_read(cfg, buf, sizeof buf);
    if (!why[0] && (!strstr(buf, "launcher_game=loc\n") || !strstr(buf, "# fremde Zeile\n"))) {
        _snprintf_s(why, sizeof why, _TRUNCATE, "Wahl nicht gemerkt: %.120s", buf);
    }
    /* W392: Packs-Platz im Bereich LoC = Seite Netzwerk (keine Pack-Elemente); W293: Erweitert erreichbar */
    if (!why[0]) {
        show_page(P_PACKS);
        pump();
        if (g_page != P_LNET) {
            strcpy_s(why, sizeof why, "LoC: Seite Netzwerk nicht erreichbar");
        }
    }
    loctab_vis(ID_LNET_RB, 1, "LoC-Netzwerk", why, sizeof why);
    loctab_vis(ID_PK_LIST, 0, "LoC-Netzwerk (Pack-Liste)", why, sizeof why);
    loctab_vis(ID_PK_ADD, 0, "LoC-Netzwerk (Pack hinzufuegen)", why, sizeof why);
    if (!why[0]) {
        show_page(P_PLAY);
        pump();
    }
    loctab_vis(ID_LOC_START, 1, "LoC-Spielen", why, sizeof why);
    loctab_vis(ID_LOC_GO0, 1, "LoC-Spielen (Bereitschaft)", why, sizeof why);
    loctab_vis(ID_LOC_IMAGE, 0, "LoC-Spielen (Disc liegt auf Erweitert)", why, sizeof why);
    loctab_vis(ID_LOC_FOLDER, 0, "LoC-Spielen (Ordner liegt auf Erweitert)", why, sizeof why);
    loctab_vis(ID_START_BIG, 0, "LoC-Spielen (SB-Start)", why, sizeof why);
    loctab_vis(ID_START_VR, 0, "LoC-Spielen (SB-VR)", why, sizeof why);
    loctab_vis(ID_IMAGE, 0, "LoC-Spielen (SB-Abbild)", why, sizeof why);
    loctab_vis(ID_SUM_GO0, 0, "LoC-Spielen (SB-Bereitschaft)", why, sizeof why);
    if (!why[0] && !IsWindowEnabled(ctl(ID_LOC_START)) != !(lgame_loc_client_ready() && !loc_img_gate(NULL, 0))) {
        /* W353: je Ausgabe; W403: ohne gepruefte Disc grau */
        strcpy_s(why, sizeof why, "LoC-Start-Freigabe passt nicht zur Ausgabe");
    }
    if (!why[0]) { /* W293: Disc/Ordner auf LoC > Erweitert */
        show_page(P_ADV);
        pump();
        if (g_page != P_ADV) {
            strcpy_s(why, sizeof why, "LoC: Erweitert nicht erreichbar");
        }
    }
    loctab_vis(ID_LOC_IMAGE, 1, "LoC-Erweitert", why, sizeof why);
    loctab_vis(ID_LOC_CHECK, 1, "LoC-Erweitert", why, sizeof why);
    loctab_vis(ID_LOC_FOLDER, 1, "LoC-Erweitert", why, sizeof why);
    /* 2: getrennte Abbild-Pfade (Fixture: LoC erkannt) */
    if (!why[0]) {
        strcpy_s(g_locfix_path, sizeof g_locfix_path, LOC_ISO);
        memset(&g_locfix, 0, sizeof g_locfix);
        g_locfix.kind = LGAME_IMG_LOC;
        g_locfix.title_id = LGAME_TID_LOC;
        ui_settext(ctl(ID_LOC_IMAGE), LOC_ISO);
        on_command(ID_LOC_CHECK, BN_CLICKED);
        pump();
        loctab_result(t, sizeof t, &col);
        if (!g_loc_checked || g_loc_res.kind != LGAME_IMG_LOC || !strstr(t, "43430009") ||
            col != C_OK) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "LoC-Pruefung: '%s'", t);
        }
    }
    if (!why[0]) { /* Ergebnis gilt nur fuer den geprueften Pfad */
        ui_settext(ctl(ID_LOC_IMAGE), "D:\\Games\\anders.iso");
        loctab_result(t, sizeof t, &col);
        if (strcmp(t, LT("gui.loc_unchecked")) != 0) {
            strcpy_s(why, sizeof why, "altes Ergebnis bei neuem Pfad");
        }
        ui_settext(ctl(ID_LOC_IMAGE), LOC_ISO);
    }
    ui_to_settings(&s1);
    if (!why[0] && strcmp(s1.game_image, LOC_SB_ISO) != 0) {
        _snprintf_s(why, sizeof why, _TRUNCATE, "SB-Abbild veraendert: '%s'", s1.game_image);
    }
    gt_read(cfg, buf, sizeof buf);
    if (!why[0] && !strstr(buf, "launcher_loc_image=" LOC_ISO "\n")) {
        _snprintf_s(why, sizeof why, _TRUNCATE, "LoC-Abbild nicht gemerkt: %.160s", buf);
    }
    /* getrennte Datenordner: LoC = <...>\loc, SB-Spielstaende anderswo */
    if (!why[0]) {
        size_t n;
        loctab_data_dir(t, sizeof t);
        adv_hdd_path(hp, sizeof hp);
        n = strlen(t);
        if (n < 4 || strcmp(t + n - 4, "\\loc") != 0 || _stricmp(t, hp) == 0 ||
            _strnicmp(hp, t, n) == 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Datenordner LoC '%s' / SB '%s'", t, hp);
        }
    }
    /* 3: LoC-Start gesperrt mit Meldung (alle Startwege), kein SB-Start */
    if (!why[0]) {
        static const int ids[] = { ID_LOC_START, ID_START, ID_START_MENU, ID_START_BIG, IDOK };
        int li;
        gt_setenv("SB_TEST_LOC_READY", "0"); /* W353: Sperre erzwingen (intern/Testpaket sonst frei) */
        for (li = 0; li < 2 && !why[0]; li++) {
            sb_lang_set(li ? "de" : "en");
            ui_apply_language();
            for (k = 0; k < 5 && !why[0]; k++) {
                set_status_k("x", 0);
                if (ids[k] == IDOK) {
                    SetFocus(NULL);
                }
                on_command(ids[k], BN_CLICKED);
                if (strcmp(g_status, LT("gui.loc_soon")) != 0 || g_loc_last_cmd[0] ||
                    !strstr(g_status, li ? "Demn" : "Coming soon")) {
                    _snprintf_s(why, sizeof why, _TRUNCATE, "%s Start %d: '%s'", li ? "de" : "en",
                                ids[k], g_status);
                }
            }
            ui_gettext(ctl(ID_GAME_LOC), t, sizeof t);
            if (!why[0] && strcmp(t, "Line of Contact") != 0) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Umschalter-Text '%s'", t);
            }
            ui_gettext(ctl(ID_LOC_START), t, sizeof t);
            if (!why[0] && strcmp(t, LT("gui.loc_btn_start")) != 0) {
                strcpy_s(why, sizeof why, "Startknopf-Text");
            }
        }
        sb_lang_set("en");
        ui_apply_language();
        SetEnvironmentVariableA("SB_TEST_LOC_READY", NULL);
    }
    /* spaeterer Weg: Client bereit -> Startweg mit LoC-Abbild + eigenem Datenordner */
    if (!why[0]) {
        gt_setenv("SB_TEST_LOC_READY", "1");
        on_command(ID_LOC_START, BN_CLICKED);
        SetEnvironmentVariableA("SB_TEST_LOC_READY", NULL);
        if (!strstr(g_loc_last_cmd, LOC_ISO) || !strstr(g_loc_last_cmd, "\\loc\\hdd") ||
            !strstr(g_loc_last_cmd, "loc_boot=1;game_image=")) { /* W353: Kette wie Testpartner */
            _snprintf_s(why, sizeof why, _TRUNCATE, "bereit: Startweg '%.160s'", g_loc_last_cmd);
        }
        g_loc_last_cmd[0] = '\0';
    }
    /* 4: zurueck zu Steel Battalion: alles wie vorher, gemeinsame Werte gleich */
    if (!why[0]) {
        show_page(P_DISPLAY);
        on_command(ID_GAME_SB, BN_CLICKED);
        pump();
        ui_to_settings(&s1);
        if (g_game != LGAME_SB || !ui_checked(ID_GAME_SB)) {
            strcpy_s(why, sizeof why, "zurueck: Zustand");
        } else if (s1.width != 1600 || s1.height != 900 || strcmp(s1.game_image, LOC_SB_ISO) != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "gemeinsame Werte/SB-Abbild: %ux%u '%s'", s1.width,
                        s1.height, s1.game_image);
        }
    }
    for (k = 0; k < P_N; k++) {
        loctab_vis(ID_NAV0 + k, k != P_ALPHA, "SB", why, sizeof why); /* W397: Test tools unter dem Zahnrad */
    }
    if (!why[0]) {
        uictl_t *c = ui_find(ID_NAV0 + P_CTRL);
        if (!c || c->y != TITLE_H + 16 + P_CTRL * 44) {
            strcpy_s(why, sizeof why, "Navigation SB verschoben");
        }
    }
    if (!why[0]) {
        show_page(P_PLAY);
        pump();
    }
    loctab_vis(ID_START_BIG, 1, "SB-Spielen", why, sizeof why);
    loctab_vis(ID_LOC_START, 0, "SB-Spielen (LoC-Start)", why, sizeof why);
    loctab_vis(ID_LOC_IMAGE, 0, "SB-Spielen (LoC-Abbild)", why, sizeof why);
    gt_read(cfg, buf, sizeof buf);
    if (!why[0] && (!strstr(buf, "launcher_game=sb\n") ||
                    !strstr(buf, "launcher_loc_image=" LOC_ISO "\n"))) {
        _snprintf_s(why, sizeof why, _TRUNCATE, "zurueck: user.cfg %.160s", buf);
    }
    /* Neustart des Launchers liest die Wahl */
    if (!why[0]) {
        g_game = LGAME_LOC;
        loctab_load();
        if (g_game != LGAME_SB || strcmp(g_loc.loc_image, LOC_ISO) != 0) {
            strcpy_s(why, sizeof why, "Laden nach Neustart");
        }
    }
    /* aufraeumen */
    g_game = LGAME_SB;
    loctab_nav_layout();
    g_locfix_path[0] = '\0';
    g_loc_checked = 0;
    lgame_defaults(&g_loc);
    ui_settext(ctl(ID_LOC_IMAGE), "");
    u8_DeleteFile(cfg);
    g_ucfg_test[0] = '\0';
    sb_lang_set(keep);
    ui_apply_language();
    lcfg_defaults(&s0);
    ui_from_settings(&s0);
    show_page(P_PLAY);
    if (!why[0]) {
        GT_OK("65 W276 LOC-LAUNCH (Umschalten + gemerkt, Navigation, getrennte Abbild-/Datenpfade, gemeinsame Einstellungen, LoC-Start gesperrt EN/DE, spaeterer Startweg)");
    } else {
        GT_FAIL("65 W276 LOC-LAUNCH", "%s", why);
    }
}

/* --- 65c (W398 LAUNCHER-GAMESWITCH): Spielnamen als Wortmarken-Tabs im Kopf. Klick auf den inaktiven Namen
 * wechselt (wie bisher), der aktive bleibt; Handcursor nur auf dem inaktiven; Tabstopp + zugaenglicher Name;
 * Tabs im Kopf ohne Ueberlappung, Name passt (keine Kuerzung), Plaketten rechts frei von Wortmarken/Zahnrad. */
void gameswitch_gui_selftest(int *pass, int *fail)
{
    char why[300] = "", t[128];
    const char *keep_ver = g_upd_ver;
    HWND hs = ctl(ID_GAME_SB), hl = ctl(ID_GAME_LOC);
    HCURSOR hand = LoadCursor(NULL, IDC_HAND);
    RECT rs, rl;
    int k;
    g_game = LGAME_SB;
    loctab_nav_layout();
    show_page(P_DISPLAY);
    pump();
    if (!hs || !hl) {
        strcpy_s(why, sizeof why, "Tabs fehlen");
    }
    if (!why[0] && (!(GetWindowLongW(hs, GWL_STYLE) & WS_TABSTOP) || !(GetWindowLongW(hl, GWL_STYLE) & WS_TABSTOP))) {
        strcpy_s(why, sizeof why, "kein Tabstopp");
    }
    if (!why[0]) {
        ui_gettext(hs, t, sizeof t);
        if (strcmp(t, "Steel Battalion") != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "zugaenglicher Name SB '%s'", t);
        }
    }
    if (!why[0] && (!ui_checked(ID_GAME_SB) || ui_checked(ID_GAME_LOC))) {
        strcpy_s(why, sizeof why, "Start: SB nicht aktiv");
    }
    /* Handcursor: nur auf dem inaktiven Namen */
    if (!why[0]) {
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        if (!SendMessageW(hl, WM_SETCURSOR, (WPARAM)hl, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE)) || GetCursor() != hand) {
            strcpy_s(why, sizeof why, "kein Handcursor auf dem inaktiven Namen");
        }
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        SendMessageW(hs, WM_SETCURSOR, (WPARAM)hs, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
        if (!why[0] && GetCursor() == hand) {
            strcpy_s(why, sizeof why, "Handcursor auf dem aktiven Namen");
        }
    }
    /* Klick (BM_CLICK = echter Knopfweg) auf den inaktiven Namen wechselt, Seite bleibt */
    if (!why[0]) {
        SendMessageW(hl, BM_CLICK, 0, 0);
        pump();
        if (g_game != LGAME_LOC || !ui_checked(ID_GAME_LOC) || ui_checked(ID_GAME_SB)) {
            strcpy_s(why, sizeof why, "Klick LoC: Zustand");
        } else if (g_page != P_DISPLAY) {
            strcpy_s(why, sizeof why, "Klick LoC: Seite gewechselt");
        }
    }
    if (!why[0]) { /* Klick auf den aktiven Namen: bleibt */
        SendMessageW(hl, BM_CLICK, 0, 0);
        pump();
        if (g_game != LGAME_LOC || !ui_checked(ID_GAME_LOC)) {
            strcpy_s(why, sizeof why, "Klick auf aktiven Namen wechselt");
        }
    }
    if (!why[0]) {
        SendMessageW(hs, BM_CLICK, 0, 0);
        pump();
        if (g_game != LGAME_SB || !ui_checked(ID_GAME_SB) || ui_checked(ID_GAME_LOC)) {
            strcpy_s(why, sizeof why, "Klick SB: Zustand");
        }
    }
    /* Geometrie: im Kopf, nebeneinander, Name passt ungekuerzt */
    if (!why[0]) {
        GetWindowRect(hs, &rs);
        GetWindowRect(hl, &rl);
        MapWindowPoints(NULL, g_hwnd, (POINT *)&rs, 2);
        MapWindowPoints(NULL, g_hwnd, (POINT *)&rl, 2);
        if (rs.right > rl.left || rs.bottom > SX(TITLE_H) || rl.bottom > SX(TITLE_H) ||
            abs((rs.bottom - rs.top) - (rl.bottom - rl.top)) > 1 || /* gleiche Groesse (Rundung +-1) */
            abs((rs.right - rs.left) - (rl.right - rl.left)) > 1) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Lage SB %ld..%ld LoC %ld..%ld (Kopf %d)", rs.left, rs.right,
                        rl.left, rl.right, SX(TITLE_H));
        }
    }
    for (k = 0; k < 2 && !why[0]; k++) {
        HWND hw = k ? hl : hs;
        RECT r;
        int f = -1, tw = gametab_tw(hw, &f);
        GetClientRect(hw, &r);
        if (tw <= 0 || tw > r.right - SX(2 * HDR_GAME_PAD) || f != F_GAME) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Name %d: Breite %d von %ld, Schrift %d", k, tw,
                        r.right - SX(2 * HDR_GAME_PAD), f);
        }
    }
    /* W401: Edition/Beta stehen jetzt in der Projektzeile (65d), rechts nur Status-Chip + Zahnrad */
    (void)keep_ver;
    show_page(P_PLAY);
    if (!why[0]) {
        GT_OK("65c W398 GAMESWITCH (Wortmarken-Tabs: Klick inaktiv wechselt, aktiv bleibt, Handcursor nur inaktiv, Tabstopp, Name passt)");
    } else {
        GT_FAIL("65c W398 GAMESWITCH", "%s", why);
    }
}

/* --- 65d (W401 LAUNCHER-HEADER-POLISH): Projektzeile mit Edition/Beta passt ungekuerzt vor _ x; rechts
 * [Update-Chip] [Status-Chip] [Zahnrad] ohne Ueberlappung, rechts der Wortmarken, gleiche Hoehe, mittig zur
 * Wortmarkenzeile; _ x in der Ecke getrennt; Chips anklickbar (Trefferflaeche = Client statt Ziehen, Status
 * oeffnet das Zahnrad), Projektzeile bleibt Ziehflaeche. Kein Netz, Browser wird nie geoeffnet. --- */
void header_polish_selftest(int *pass, int *fail)
{
    char why[300] = "";
    const char *keep_ver = g_upd_ver;
    int keep_cfg = g_upd_cfg, k, i;
    lupd_res_t keep_res = g_upd_res;
    RECT rg, rm, rs, rl;
    show_page(P_PLAY);
    pump();
    for (k = 0; k < 4 && !why[0]; k++) { /* k&1: Release-Version, k&2: Update-Chip */
        const char *txt[3];
        COLORREF col[3];
        int w[3], sum;
        hdr_lay_t L;
        g_upd_ver = k & 1 ? "0.3.0" : "";
        g_upd_cfg = k & 2 ? 1 : keep_cfg;
        memset(&g_upd_res, 0, sizeof g_upd_res);
        if (k & 2) {
            g_upd_res.result = LUPD_R_NEWER;
            strcpy_s(g_upd_res.rel.tag, sizeof g_upd_res.rel.tag, "v0.3.1");
        }
        sum = hdr_sub_parts(txt, col, w);
        if (!w[1] || (k & 1 && !w[2]) || HDR_GAME_X + HDR_GAME_PAD + sum > HDR_WB_X - 16) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Projektzeile %d: Breite %d (bis %d)", k, sum,
                        HDR_WB_X - 16 - HDR_GAME_X - HDR_GAME_PAD);
            break;
        }
        hdr_layout(&L);
        if (L.n != (k & 2 ? 2 : 1) || L.shortened) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Chips %d: n %d kurz %d", k, L.n, L.shortened);
            break;
        }
        for (i = 0; i < L.n && !why[0]; i++) {
            if (L.x[i] < HDR_GAME_END + 12 || L.x[i] + L.w[i] > HDR_GEAR_X - 8 ||
                (i && L.x[i] + L.w[i] > L.x[i - 1] - 6)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Chip %d/%d: x %d w %d (Wortmarken bis %d, Zahnrad %d)", k, i,
                            L.x[i], L.w[i], HDR_GAME_END, HDR_GEAR_X);
            }
        }
        if (!why[0] && k == 2) { /* Trefferflaechen nach dem Zeichnen */
            POINT pu, ps, pt;
            LRESULT hu, hst, hsub;
            hdr_paint_status();
            pu.x = (g_hdr_hit[1].left + g_hdr_hit[1].right) / 2;
            pu.y = (g_hdr_hit[1].top + g_hdr_hit[1].bottom) / 2;
            ps.x = (g_hdr_hit[0].left + g_hdr_hit[0].right) / 2;
            ps.y = (g_hdr_hit[0].top + g_hdr_hit[0].bottom) / 2;
            pt.x = SX(HDR_GAME_X + 40);
            pt.y = SX(HDR_SUB_Y + HDR_SUB_H / 2);
            if (g_hdr_hit_n != 2 || hdr_hit(pu) != HDR_CHIP_UPD || hdr_hit(ps) != HDR_CHIP_ST || hdr_hit(pt) != -1) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Treffer: n %d upd %d st %d sub %d", g_hdr_hit_n, hdr_hit(pu),
                            hdr_hit(ps), hdr_hit(pt));
            } else {
                ClientToScreen(g_hwnd, &pu);
                ClientToScreen(g_hwnd, &ps);
                ClientToScreen(g_hwnd, &pt);
                hu = SendMessageW(g_hwnd, WM_NCHITTEST, 0, MAKELPARAM(pu.x, pu.y));
                hst = SendMessageW(g_hwnd, WM_NCHITTEST, 0, MAKELPARAM(ps.x, ps.y));
                hsub = SendMessageW(g_hwnd, WM_NCHITTEST, 0, MAKELPARAM(pt.x, pt.y));
                if (hu != HTCLIENT || hst != HTCLIENT || hsub != HTCAPTION) {
                    _snprintf_s(why, sizeof why, _TRUNCATE, "NCHITTEST upd %ld st %ld sub %ld", (long)hu, (long)hst,
                                (long)hsub);
                }
            }
            if (!why[0]) {
                hdr_click(HDR_CHIP_ST);
                pump();
                if (g_page != P_NETI) { /* W470: Chip oeffnet den Bereich steelnet (vorher Zahnrad) */
                    _snprintf_s(why, sizeof why, _TRUNCATE, "Status-Chip oeffnet Seite %d statt steelnet", g_page);
                }
                show_page(P_PLAY);
            }
        }
    }
    g_upd_ver = keep_ver;
    g_upd_cfg = keep_cfg;
    g_upd_res = keep_res;
    /* Zahnrad gleich hoch wie der Chip, _ x klein in der Ecke, getrennt von der Gruppe */
    if (!why[0]) {
        GetWindowRect(ctl(ID_LSET_OPEN), &rg);
        GetWindowRect(ctl(ID_MIN), &rm);
        GetWindowRect(ctl(ID_GAME_SB), &rs);
        GetWindowRect(ctl(ID_GAME_LOC), &rl);
        MapWindowPoints(NULL, g_hwnd, (POINT *)&rg, 2);
        MapWindowPoints(NULL, g_hwnd, (POINT *)&rm, 2);
        MapWindowPoints(NULL, g_hwnd, (POINT *)&rs, 2);
        MapWindowPoints(NULL, g_hwnd, (POINT *)&rl, 2);
        if (abs((rg.bottom - rg.top) - SX(HDR_CHIP_H)) > 1 || abs(rg.top - SX(HDR_CHIP_Y)) > 1 ||
            /* W403: Zahnrad unter _ x, rechtsbuendig mit der Inhaltskante (frueher links von _ x) */
            rg.top < rm.bottom || abs(rg.right - SX(CX0 + CW)) > 1 || rm.bottom > SX(HDR_CHIP_Y) + 2 || rs.top < SX(HDR_SUB_Y + HDR_SUB_H) ||
            rs.bottom > SX(TITLE_H) - 2 || rl.left - rs.right < SX(HDR_GAME_GAP) - 1) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Lage Zahnrad %ld..%ld/%ld..%ld, _ %ld..%ld/%ld, Tabs %ld..%ld/%ld..%ld",
                        rg.left, rg.right, rg.top, rg.bottom, rm.left, rm.right, rm.bottom, rs.top, rs.bottom,
                        rs.right, rl.left);
        }
    }
    show_page(P_PLAY);
    if (!why[0]) {
        GT_OK("65d W401 HEADER (Projektzeile + Edition/Beta ungekuerzt, Chips rechts frei und gleich hoch wie das Zahnrad, _ x getrennt, Chips anklickbar, Status -> Zahnrad)");
    } else {
        GT_FAIL("65d W401 HEADER", "%s", why);
    }
}

/* --- 65b (W375, GitHub #50): LoC-Tab -> Grafik aendern -> Speichern ohne SB-Abbild. Vorher meldete do_save
 * "No game data set" und schrieb nichts (nach dem Neustart alte Werte). Jetzt: gespeichert, nach erneutem Lesen
 * da; Gegenprobe SB-Tab ohne Spieldaten lehnt weiter ab. user.cfg = Temp-Datei (g_ucfg_test). --- */
/* --- 65d (W400 LAUNCHER-LOC-ONLINE-GATE): LoC-Start nur mit Stufe "Mit steelnet verbinden". Offline bzw. Nur
 * Version: Knopf grau, Grund + Sprung sichtbar, Zeile Online warnt ("Change" -> Zahnrad), kein Start ueber
 * Knopf/Launch-Leiste/Enter (Status = Grund); ohne Disc beide Gruende, Disc zuerst. Auf steelnet gestellt
 * (Zahnrad) sofort frei. Testpartner bleibt sichtbar/nutzbar, SB-Start unabhaengig von der Stufe. */
void loc_gate_gui_selftest(int *pass, int *fail)
{
    static const int kIds[] = { ID_LOC_START, ID_START, ID_START_MENU, ID_START_BIG, IDOK };
    char why[300] = "", tmp[1024], cfg[1200], keep[16];
    int on0 = g_loc_online, fs0 = g_onl_fix_srv, uc0 = g_upd_cfg, ask0 = g_upd_ask, m, k, li;
    loc_sumrow_t r[LOC_SUM_N];
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw400_gui_user_%lu.cfg", tmp, GetCurrentProcessId());
    write_file(cfg, "[display]\nwidth=1280\n");
    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, cfg);
    strcpy_s(keep, sizeof keep, sb_lang_code());
    g_locgate_skip = 0;
    g_onl_fix_srv = 1; /* Dienst eingerichtet (steelnet waehlbar) */
    g_upd_cfg = 1;     /* kein Update-Abruf beim Waehlen */
    g_upd_ask = 0;
    gt_setenv("SB_TEST_LOC_READY", "1");
    strcpy_s(g_locfix_path, sizeof g_locfix_path, LOC_ISO);
    memset(&g_locfix, 0, sizeof g_locfix);
    g_locfix.kind = LGAME_IMG_LOC;
    g_locfix.title_id = LGAME_TID_LOC;
    g_game = LGAME_LOC;
    loctab_nav_layout();
    show_page(P_PLAY);
    ui_settext(ctl(ID_LOC_IMAGE), LOC_ISO);
    loctab_check();
    ui_settext(ctl(ID_LOC_CODE), "");
    for (m = NETM_VER; m <= NETM_OFF && !why[0]; m++) { /* 1: Nur Version / Offline -> gesperrt */
        for (li = 0; li < 2 && !why[0]; li++) {
            sb_lang_set(li ? "de" : "en");
            ui_apply_language();
            on_command(ID_NETM0 + m, BN_CLICKED); /* Zahnrad > Online & Updates */
            show_page(P_PLAY);
            pump();
            if (netm_get() != m) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Stufe %d nicht gewaehlt (%d)", m, netm_get());
            } else if (IsWindowEnabled(ctl(ID_LOC_START))) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Stufe %d: LoC-Start nicht grau", m);
            } else if (!IsWindowVisible(ctl(ID_LOC_GATE_GO))) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Stufe %d: Sprung zum Zahnrad fehlt", m);
            } else if (!strstr(LT("gui.loc_gate_why"), "steelnet")) {
                strcpy_s(why, sizeof why, "Grund ohne steelnet");
            }
            loctab_sum_rows(r);
            if (!why[0] && (strcmp(r[4].value, LT("gui.loc_sum_net_off")) != 0 || r[4].col != C_WARN))
                _snprintf_s(why, sizeof why, _TRUNCATE, "%s Zeile Online '%s'", li ? "de" : "en", r[4].value);
            for (k = 0; k < 5 && !why[0]; k++) { /* jeder Startweg: gleiche Sperre, keine Hintertuer */
                set_status_k("x", 0);
                g_loc_last_cmd[0] = '\0';
                if (kIds[k] == IDOK) {
                    SetFocus(NULL);
                }
                on_command(kIds[k], BN_CLICKED);
                if (g_loc_last_cmd[0] || strcmp(g_status, LT("gui.loc_gate_why")) != 0)
                    _snprintf_s(why, sizeof why, _TRUNCATE, "Stufe %d Start %d: '%.120s' '%.80s'", m, kIds[k],
                                g_status, g_loc_last_cmd);
            }
#if defined(SB_HAVE_LOCTP) && SB_HAVE_LOCTP
            if (!why[0] && (!IsWindowVisible(ctl(ID_LOCTP_OPEN)) || !IsWindowEnabled(ctl(ID_LOCTP_OPEN))))
                strcpy_s(why, sizeof why, "Testpartner gesperrt");
#endif
        }
    }
    if (!why[0]) { /* 2: ohne Disc beide Gruende, Disc zuerst */
        ui_settext(ctl(ID_LOC_IMAGE), "");
        loctab_check();
        set_status_k("x", 0);
        on_command(ID_LOC_START, BN_CLICKED);
        {
            const char *a = strstr(g_status, LT("gui.loc_start_noimg")), *b = strstr(g_status, LT("gui.loc_gate_why"));
            if (!a || !b || a > b || g_loc_last_cmd[0])
                _snprintf_s(why, sizeof why, _TRUNCATE, "ohne Disc: '%.200s'", g_status);
        }
        loctab_sum_rows(r);
        if (!why[0] && (r[0].col == C_OK || r[4].col != C_WARN))
            strcpy_s(why, sizeof why, "ohne Disc: Zeilen Spiel/Online");
        ui_settext(ctl(ID_LOC_IMAGE), LOC_ISO);
        loctab_check();
    }
    if (!why[0]) { /* 3: "Change" bei Online und der Sprung fuehren zum Zahnrad */
        show_page(P_PLAY);
        on_command(ID_LOC_GO0 + LOC_SUM_N - 1, BN_CLICKED);
        if (g_page != P_LSET)
            _snprintf_s(why, sizeof why, _TRUNCATE, "Change Online -> Seite %d", g_page);
        show_page(P_PLAY);
        on_command(ID_LOC_GATE_GO, BN_CLICKED);
        if (!why[0] && g_page != P_LSET)
            _snprintf_s(why, sizeof why, _TRUNCATE, "Sprung -> Seite %d", g_page);
    }
    if (!why[0]) { /* 4: SB-Start unabhaengig von der Stufe (Startweg nicht durch die LoC-Sperre) */
        int en[2], vis[2], j;
        g_game = LGAME_SB;
        loctab_nav_layout();
        for (j = 0; j < 2; j++) { /* Offline, dann steelnet: SB-Start gleich (grau/frei je Ausgabe/Client) */
            g_upd_cfg = 1;
            on_command(ID_NETM0 + (j ? NETM_STEEL : NETM_OFF), BN_CLICKED);
            show_page(P_PLAY);
            pump();
            en[j] = IsWindowEnabled(ctl(ID_START_BIG)) ? 1 : 0;
            vis[j] = IsWindowVisible(ctl(ID_START_BIG)) && !IsWindowVisible(ctl(ID_LOC_GATE_GO));
        }
        if (en[0] != en[1] || !vis[0] || !vis[1])
            _snprintf_s(why, sizeof why, _TRUNCATE, "SB-Start von der LoC-Sperre betroffen (%d/%d %d/%d)", en[0], en[1],
                        vis[0], vis[1]);
        on_command(ID_NETM0 + NETM_OFF, BN_CLICKED);
        g_game = LGAME_LOC;
        loctab_nav_layout();
    }
    if (!why[0]) { /* 5: auf steelnet gestellt -> sofort frei, Startweg online */
        show_page(P_LSET);
        g_upd_cfg = 1; /* kein Update-Abruf beim Waehlen */
        on_command(ID_NETM0 + NETM_STEEL, BN_CLICKED);
        show_page(P_PLAY);
        pump();
        g_loc_last_cmd[0] = '\0';
        on_command(ID_LOC_START, BN_CLICKED);
        loctab_sum_rows(r);
        if (netm_get() != NETM_STEEL || !IsWindowEnabled(ctl(ID_LOC_START)) || IsWindowVisible(ctl(ID_LOC_GATE_GO)))
            strcpy_s(why, sizeof why, "steelnet: Start nicht frei");
        else if (!strstr(g_loc_last_cmd, "loc_transport=udp") || !strstr(g_loc_last_cmd, LOC_ISO))
            _snprintf_s(why, sizeof why, _TRUNCATE, "steelnet: Startweg '%.200s'", g_loc_last_cmd);
        else if (strcmp(r[4].value, LT("gui.loc_sum_net_off")) == 0)
            strcpy_s(why, sizeof why, "steelnet: Zeile Online warnt weiter");
    }
    if (!why[0]) { /* 6: zurueck auf Offline -> sofort wieder gesperrt */
        on_command(ID_NETM0 + NETM_OFF, BN_CLICKED);
        show_page(P_PLAY);
        if (IsWindowEnabled(ctl(ID_LOC_START)))
            strcpy_s(why, sizeof why, "Offline danach: Start nicht grau");
    }
    /* aufraeumen */
    SetEnvironmentVariableA("SB_TEST_LOC_READY", NULL);
    g_locgate_skip = 1;
    g_loc_online = on0;
    g_onl_fix_srv = fs0;
    g_upd_cfg = uc0;
    g_upd_ask = ask0;
    g_loc_last_cmd[0] = '\0';
    g_locfix_path[0] = '\0';
    g_loc_checked = 0;
    ui_settext(ctl(ID_LOC_IMAGE), "");
    u8_DeleteFile(cfg);
    g_ucfg_test[0] = '\0';
    sb_lang_set(keep);
    ui_apply_language();
    g_game = LGAME_SB;
    loctab_nav_layout();
    onl_sync_ui();
    netm_sync_ui();
    show_page(P_PLAY);
    if (!why[0]) {
        GT_OK("65d W400 LOC-ONLINE-GATE (LoC nur mit steelnet: Version/Offline grau + Grund + Sprung, alle Startwege gesperrt, ohne Disc Disc-Grund zuerst, steelnet sofort frei, Testpartner und SB frei)");
    } else {
        GT_FAIL("65d W400 LOC-ONLINE-GATE", "%s", why);
    }
}

/* --- 65g (W454 LOC-LAN-MODE, GitHub #91): Multiplayer > Match: Auswahl Online/LAN. LAN gibt den LoC-Start auch ohne
 * Stufe steelnet frei (Offline), Kette loc_transport=udp;loc_lan=1;loc_lan_room=... ohne Treffpunkt/Raumcode/Konto,
 * Passwort nie in Kette/user.cfg (nur Umgebung des Kindprozesses, danach geloescht), steelnet-Teile ausgeblendet bzw.
 * grau (Sichtbarkeit/Raumcode, steelnet-Chat), Versionssperre aus /v1/info gilt im LAN nicht, ungueltiger Raumname
 * verweigert, gemerkt in user.cfg; zurueck auf Online -> wieder gesperrt (Offline). --- */
void loc_lan_gui_selftest(int *pass, int *fail)
{
    char why[300] = "", tmp[1024], cfg[1200], txt[4096] = "", ev[8];
    int on0 = g_loc_online, fs0 = g_onl_fix_srv, uc0 = g_upd_cfg, ask0 = g_upd_ask, lan0 = g_loc_lan;
    loc_sumrow_t r[LOC_SUM_N];
    FILE *f = NULL;
    size_t n;
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw454_gui_user_%lu.cfg", tmp, GetCurrentProcessId());
    write_file(cfg, "[display]\nwidth=1280\n");
    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, cfg);
    g_locgate_skip = 0;
    g_onl_fix_srv = 1;
    g_upd_cfg = 1;
    g_upd_ask = 0;
    gt_setenv("SB_TEST_LOC_READY", "1");
    strcpy_s(g_locfix_path, sizeof g_locfix_path, LOC_ISO);
    memset(&g_locfix, 0, sizeof g_locfix);
    g_locfix.kind = LGAME_IMG_LOC;
    g_locfix.title_id = LGAME_TID_LOC;
    g_game = LGAME_LOC;
    loctab_nav_layout();
    show_page(P_PLAY);
    ui_settext(ctl(ID_LOC_IMAGE), LOC_ISO);
    loctab_check();
    lan_kind_set(0);
    on_command(ID_NETM0 + NETM_OFF, BN_CLICKED);
    show_page(P_PLAY);
    pump();
    if (IsWindowEnabled(ctl(ID_LOC_START))) strcpy_s(why, sizeof why, "Offline ohne LAN: Start nicht grau");
    if (!why[0]) { /* 1: LAN waehlen (Karte Match) */
        show_page(P_LNET);
        pump();
        if (!ctl(ID_LNET_KIND) || !IsWindowVisible(ctl(ID_LNET_KIND))) strcpy_s(why, sizeof why, "Auswahl Online/LAN fehlt");
        else {
            ComboBox_SetCurSel(ctl(ID_LNET_KIND), 1);
            on_command(ID_LNET_KIND, CBN_SELCHANGE);
            pump();
            if (!g_loc_lan) strcpy_s(why, sizeof why, "LAN nicht gewaehlt");
            else if (IsWindowVisible(ctl(ID_LOC_VIS)) || IsWindowVisible(ctl(ID_LOC_CODE)) || !IsWindowVisible(ctl(ID_LNET_LANROOM)) ||
                     !IsWindowVisible(ctl(ID_LNET_LANPASS)) || IsWindowVisible(ctl(ID_MP_GOSET)))
                strcpy_s(why, sizeof why, "LAN: Felder der Karte Match falsch sichtbar");
            else if (IsWindowEnabled(ctl(ID_LNET_SNCHAT)))
                strcpy_s(why, sizeof why, "LAN: steelnet-Chat nicht grau");
        }
    }
    if (!why[0]) { /* 2: Raum + Passwort, Start ohne steelnet */
        ui_settext(ctl(ID_LNET_LANROOM), "Party");
        on_command(ID_LNET_LANROOM, EN_CHANGE);
        ui_settext(ctl(ID_LNET_LANPASS), "geheim42");
        on_command(ID_LNET_LANPASS, EN_CHANGE);
        show_page(P_PLAY);
        pump();
        g_loc_last_cmd[0] = '\0';
        on_command(ID_LOC_START, BN_CLICKED);
        if (!IsWindowEnabled(ctl(ID_LOC_START)) || IsWindowVisible(ctl(ID_LOC_GATE_GO)))
            strcpy_s(why, sizeof why, "LAN offline: Start nicht frei");
        else if (!strstr(g_loc_last_cmd, "loc_transport=udp;loc_lan=1;loc_lan_room=Party") || strstr(g_loc_last_cmd, "loc_lobby=") ||
                 strstr(g_loc_last_cmd, "loc_lobby_code") || strstr(g_loc_last_cmd, "geheim42") || !strstr(g_loc_last_cmd, LOC_ISO))
            _snprintf_s(why, sizeof why, _TRUNCATE, "LAN: Kette '%.220s'", g_loc_last_cmd);
        else if (GetEnvironmentVariableA("SB_LAN_PASS", ev, sizeof ev) != 0)
            strcpy_s(why, sizeof why, "LAN: SB_LAN_PASS nach dem Start nicht geloescht");
        loctab_sum_rows(r);
        if (!why[0] && (strcmp(r[4].value, LT("gui.mp_sum_lan")) != 0 || r[4].col != C_OK))
            _snprintf_s(why, sizeof why, _TRUNCATE, "LAN: Zeile Online '%s'", r[4].value);
    }
    if (!why[0]) { /* 3: gemerkt (Auswahl + Raum), Passwort nie in user.cfg */
        if (u8_fopen_s(&f, cfg, "rb") == 0 && f) {
            n = fread(txt, 1, sizeof txt - 1, f);
            txt[n] = 0;
            fclose(f);
        }
        if (!strstr(txt, "launcher_loc_lan=1") || !strstr(txt, "launcher_loc_lan_room=Party") || strstr(txt, "geheim42"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "user.cfg: '%.200s'", txt);
    }
    if (!why[0]) { /* 4: steelnet-Stufe + veraltet laut /v1/info: im LAN kein Gate, keine Treffpunkt-Teile */
        on_command(ID_NETM0 + NETM_STEEL, BN_CLICKED);
        locver_fixture("9.9.9");
        show_page(P_PLAY);
        pump();
        g_loc_last_cmd[0] = '\0';
        on_command(ID_LOC_START, BN_CLICKED);
        if (loc_ver_gate() || !strstr(g_loc_last_cmd, "loc_lan=1") || strstr(g_loc_last_cmd, "loc_lobby="))
            _snprintf_s(why, sizeof why, _TRUNCATE, "LAN + steelnet-Stufe: Gate %d, Kette '%.160s'", loc_ver_gate(), g_loc_last_cmd);
        locver_fixture("0.0.1");
    }
    if (!why[0]) { /* 5: ungueltiger Raumname -> kein Start */
        strcpy_s(g_loc_lan_room, sizeof g_loc_lan_room, "a;b");
        g_loc_last_cmd[0] = '\0';
        set_status_k("x", 0);
        on_command(ID_LOC_START, BN_CLICKED);
        if (g_loc_last_cmd[0] || strcmp(g_status, LT("gui.lan_room_bad")) != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "Raum a;b: '%.100s' '%.100s'", g_status, g_loc_last_cmd);
        strcpy_s(g_loc_lan_room, sizeof g_loc_lan_room, "Party");
    }
    if (!why[0]) { /* 6: zurueck auf Online + Offline -> wieder gesperrt */
        show_page(P_LNET);
        ComboBox_SetCurSel(ctl(ID_LNET_KIND), 0);
        on_command(ID_LNET_KIND, CBN_SELCHANGE);
        on_command(ID_NETM0 + NETM_OFF, BN_CLICKED);
        show_page(P_PLAY);
        pump();
        if (g_loc_lan || IsWindowEnabled(ctl(ID_LOC_START))) strcpy_s(why, sizeof why, "Online/Offline danach: Start nicht grau");
    }
    /* aufraeumen */
    SetEnvironmentVariableA("SB_TEST_LOC_READY", NULL);
    lan_kind_set(lan0);
    g_loc_lan_room[0] = 0;
    g_locgate_skip = 1;
    g_loc_online = on0;
    g_onl_fix_srv = fs0;
    g_upd_cfg = uc0;
    g_upd_ask = ask0;
    g_loc_last_cmd[0] = '\0';
    g_locfix_path[0] = '\0';
    g_loc_checked = 0;
    ui_settext(ctl(ID_LOC_IMAGE), "");
    if (ctl(ID_LNET_LANPASS)) ui_settext(ctl(ID_LNET_LANPASS), "");
    lan_field_changed(ID_LNET_LANPASS);
    u8_DeleteFile(cfg);
    g_ucfg_test[0] = '\0';
    g_game = LGAME_SB;
    loctab_nav_layout();
    onl_sync_ui();
    netm_sync_ui();
    show_page(P_PLAY);
    if (!why[0]) {
        GT_OK("65g W454 LOC-LAN-MODE (Auswahl Online/LAN, LAN-Start ohne steelnet frei, Kette ohne Treffpunkt/Konto/Passwort, Passwort nur Umgebung und danach geloescht, steelnet-Teile aus/grau, keine Versionssperre im LAN, Raumname geprueft, gemerkt, zurueck = gesperrt)");
    } else {
        GT_FAIL("65g W454 LOC-LAN-MODE", "%s", why);
    }
}

/* --- 65h (W487 LIVE-GAMERTAG, W501 ONLINE-BOTH): Multiplayer > Match: Verbindung nur Online/LAN (kein Eintrag Xbox
 * Live mehr). Online mit Konto + Gamertag: Kette = Online (loc_transport=udp;loc_lobby=...) + loc_live=1 + loc_camp=1
 * je genau einmal (im Spiel System Link oder Xbox Live). Online ohne Gamertag / ohne Konto / Stand unbekannt: Start
 * frei, nur System Link (kein loc_live), Hinweis mit Weg zur Kontoseite, kein Seitenwechsel. LAN nie loc_live.
 * Migration: launcher_loc_live=1 in user.cfg -> einmalig Online (auch gegen launcher_loc_lan=1), Schluessel danach 0. --- */
void loc_live_gui_selftest(int *pass, int *fail)
{
    char why[300] = "", tmp[1024], cfg[1200], txt[4096] = "", s1[64] = "", s2[64] = "a=1";
    int on0 = g_loc_online, fs0 = g_onl_fix_srv, uc0 = g_upd_cfg, ask0 = g_upd_ask, lan0 = g_loc_lan;
    lacc_store_t a0 = g_acc;
    FILE *f = NULL;
    size_t n;
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw487_gui_user_%lu.cfg", tmp, GetCurrentProcessId());
    write_file(cfg, "[display]\nwidth=1280\n");
    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, cfg);
    g_locgate_skip = 0;
    g_onl_fix_srv = 1;
    g_upd_cfg = 1;
    g_upd_ask = 0;
    gt_setenv("SB_TEST_LOC_READY", "1");
    strcpy_s(g_locfix_path, sizeof g_locfix_path, LOC_ISO);
    memset(&g_locfix, 0, sizeof g_locfix);
    g_locfix.kind = LGAME_IMG_LOC;
    g_locfix.title_id = LGAME_TID_LOC;
    g_game = LGAME_LOC;
    loctab_nav_layout();
    show_page(P_PLAY);
    ui_settext(ctl(ID_LOC_IMAGE), LOC_ISO);
    loctab_check();
    lan_kind_set(0);
    on_command(ID_NETM0 + NETM_STEEL, BN_CLICKED);
    locver_fixture("0.0.1");
    acc_fixture(1, 1, 1, 1, 0); /* Konto mit Gamertag "Ace Pilot" */
    { /* 0 (W485 MERGEFIX/W501): eine Pruefung lnet_live_ok (nicht LAN, Konto, Gamertag) + ein live_set_append (nur
       * online). Faelle: mit Gamertag, offline, LAN, ohne Gamertag, ohne Konto, Stand unbekannt. */
        static const char *const kCase[6] = { "Gamertag", "offline", "LAN", "ohne Gamertag", "ohne Konto", "unbekannt" };
        int k;
        for (k = 0; k < 6 && !why[0]; k++) {
            int onl = k != 1, r, ok, on;
            acc_fixture(1, 1, 1, 1, 0);
            g_loc_lan = k == 2;
            if (k == 3) g_acc_st.gamertag[0] = '\0';
            if (k == 4) memset(&g_acc, 0, sizeof g_acc);
            if (k == 5) g_acc_st_ok = 0;
            strcpy_s(s1, sizeof s1, "");
            strcpy_s(s2, sizeof s2, "a=1");
            ok = lnet_live_ok();
            on = lnet_live_on();
            r = live_set_append(s1, sizeof s1, onl);
            r += live_set_append(s2, sizeof s2, onl);
            if (k == 0 && (!ok || !on || r != 2 || strcmp(s1, "loc_live=1") != 0 || strcmp(s2, "a=1;loc_live=1") != 0))
                _snprintf_s(why, sizeof why, _TRUNCATE, "live_set_append %s: %d '%s' '%s'", kCase[k], r, s1, s2);
            else if (k != 0 && (r != 0 || s1[0] || strcmp(s2, "a=1") != 0 || (k >= 2 && (ok || on))))
                _snprintf_s(why, sizeof why, _TRUNCATE, "live_set_append %s: ok %d on %d r %d '%s' '%s'", kCase[k], ok, on, r, s1, s2);
        }
        g_loc_lan = 1;
        onl_load(); /* user.cfg ohne Schluessel: Standard Online */
        if (!why[0] && (g_loc_lan || lnet_kind_get() != 0)) strcpy_s(why, sizeof why, "Standard ist nicht Online");
        on_command(ID_NETM0 + NETM_STEEL, BN_CLICKED);
        locver_fixture("0.0.1");
        acc_fixture(1, 1, 1, 1, 0);
    }
    if (!why[0]) { /* 1: Auswahl hat genau 2 Eintraege (Online, LAN); LAN -> Online, steelnet-Teile wieder an */
        show_page(P_LNET);
        pump();
        if (!ctl(ID_LNET_KIND) || ComboBox_GetCount(ctl(ID_LNET_KIND)) != 2)
            _snprintf_s(why, sizeof why, _TRUNCATE, "Auswahl hat %d Eintraege (erwartet 2)",
                        ctl(ID_LNET_KIND) ? (int)ComboBox_GetCount(ctl(ID_LNET_KIND)) : -1);
        else {
            ComboBox_SetCurSel(ctl(ID_LNET_KIND), 1);
            on_command(ID_LNET_KIND, CBN_SELCHANGE);
            if (!g_loc_lan || lnet_kind_get() != 1) strcpy_s(why, sizeof why, "LAN nicht gewaehlt");
            ComboBox_SetCurSel(ctl(ID_LNET_KIND), 0);
            on_command(ID_LNET_KIND, CBN_SELCHANGE);
            pump();
            if (!why[0] && (g_loc_lan || lnet_kind_get() != 0 || !lnet_live_on()))
                strcpy_s(why, sizeof why, "Online nicht gewaehlt bzw. Live nicht verfuegbar");
            else if (!why[0] && (IsWindowVisible(ctl(ID_LNET_LANROOM)) || !IsWindowEnabled(ctl(ID_LNET_SNCHAT))))
                strcpy_s(why, sizeof why, "Online: LAN-Felder sichtbar bzw. steelnet-Chat grau");
        }
    }
    if (!why[0]) { /* 2: Online mit Gamertag: Kette Online + loc_live=1 + loc_camp=1 je genau einmal; Infozeile */
        char t[400];
        COLORREF col;
        const char *p;
        camp_fixture(LCAMP_R_OK);
        show_page(P_PLAY);
        pump();
        g_loc_last_cmd[0] = '\0';
        on_command(ID_LOC_START, BN_CLICKED);
        p = strstr(g_loc_last_cmd, "loc_live=1");
        if (!p || strstr(p + 10, "loc_live") || !strstr(g_loc_last_cmd, "loc_camp=1") ||
            strstr(strstr(g_loc_last_cmd, "loc_camp=1") + 10, "loc_camp=") ||
            !strstr(g_loc_last_cmd, "loc_transport=udp") || !strstr(g_loc_last_cmd, "loc_lobby=") || strstr(g_loc_last_cmd, "loc_lan=1"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Online+Gamertag: Kette '%.220s'", g_loc_last_cmd);
        live_info(t, sizeof t, &col);
        if (!why[0] && (!strstr(t, "Ace Pilot") || col == C_WARN)) _snprintf_s(why, sizeof why, _TRUNCATE, "Live-Info '%s'", t);
    }
    if (!why[0]) { /* 3: Migration launcher_loc_live=1 -> Online (einmalig, auch gegen LAN), Schluessel danach 0 */
        static const char *const kIn[4] = { "launcher_loc_live=1\n", "launcher_loc_lan=1\nlauncher_loc_live=1\n",
                                            "launcher_loc_live=0\nlauncher_loc_lan=1\n", "launcher_loc_live=2\n" };
        static const int kLan[4] = { 0, 0, 1, 0 };
        static const char *const kWant[4] = { "launcher_loc_live=0", "launcher_loc_live=0", "launcher_loc_live=0",
                                              "launcher_loc_live=2" };
        int k;
        for (k = 0; k < 4 && !why[0]; k++) {
            write_file(cfg, kIn[k]);
            onl_load();
            txt[0] = 0;
            if (u8_fopen_s(&f, cfg, "rb") == 0 && f) {
                n = fread(txt, 1, sizeof txt - 1, f);
                txt[n] = 0;
                fclose(f);
            }
            if (g_loc_lan != kLan[k] || lnet_kind_get() != kLan[k] || !strstr(txt, kWant[k]) ||
                strstr(txt, "launcher_loc_live=1") || (k == 1 && !strstr(txt, "launcher_loc_lan=0")))
                _snprintf_s(why, sizeof why, _TRUNCATE, "Migration %d: LAN %d '%.160s'", k, g_loc_lan, txt);
        }
        write_file(cfg, "[display]\nwidth=1280\n"); /* Auswahl schreibt launcher_loc_live nie */
        lnet_kind_set(1);
        lnet_kind_set(0);
        txt[0] = 0;
        if (u8_fopen_s(&f, cfg, "rb") == 0 && f) {
            n = fread(txt, 1, sizeof txt - 1, f);
            txt[n] = 0;
            fclose(f);
        }
        if (!why[0] && (strstr(txt, "launcher_loc_live") || !strstr(txt, "launcher_loc_lan=0")))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Auswahl: user.cfg '%.200s'", txt);
        on_command(ID_NETM0 + NETM_STEEL, BN_CLICKED);
        locver_fixture("0.0.1");
    }
    if (!why[0]) { /* 4: ohne Gamertag / ohne Konto / Stand unbekannt: Start frei, nur System Link, Hinweis, kein Sprung */
        static const int kWant[3] = { ACC_LIVE_NOGT, ACC_LIVE_NOACC, ACC_LIVE_UNK };
        static const char *const kMsg[3] = { "gui.live_sl_only", "gui.live_sl_only", "gui.live_need_status" };
        int k;
        for (k = 0; k < 3 && !why[0]; k++) {
            char t[400];
            COLORREF col;
            acc_fixture(1, 1, 1, 1, 0);
            if (k == 0) g_acc_st.gamertag[0] = '\0';
            else if (k == 1) memset(&g_acc, 0, sizeof g_acc);
            else g_acc_st_ok = 0;
            show_page(P_PLAY);
            pump();
            g_loc_last_cmd[0] = '\0';
            on_command(ID_LOC_START, BN_CLICKED);
            pump();
            live_info(t, sizeof t, &col);
            if (acc_live_state() != kWant[k] || !g_loc_last_cmd[0] || strstr(g_loc_last_cmd, "loc_live") ||
                strstr(g_loc_last_cmd, "loc_camp") || !strstr(g_loc_last_cmd, "loc_transport=udp") || g_page != P_PLAY)
                _snprintf_s(why, sizeof why, _TRUNCATE, "Fall %d: Zustand %d Seite %d '%.180s'", k, acc_live_state(), g_page,
                            g_loc_last_cmd);
            else if (strcmp(t, LT(kMsg[k])) != 0 || col != C_WARN)
                _snprintf_s(why, sizeof why, _TRUNCATE, "Fall %d: Hinweis '%.200s'", k, t);
        }
        acc_fixture(1, 1, 1, 1, 0);
        g_acc_live_pending = 2; /* Start nach dem Status: genau einmal durchlassen, dann wieder normal */
        if (!why[0] && (!acc_live_prestart() || g_acc_live_pending != 0)) strcpy_s(why, sizeof why, "acc_live_prestart nach Status");
        if (!why[0] && !acc_live_prestart()) strcpy_s(why, sizeof why, "acc_live_prestart sperrt");
    }
    if (!why[0]) { /* 5: LAN: nie loc_live/loc_camp (auch mit Gamertag), Live nicht verfuegbar */
        acc_fixture(1, 1, 1, 1, 0);
        lnet_kind_set(1);
        show_page(P_PLAY);
        g_loc_last_cmd[0] = '\0';
        on_command(ID_LOC_START, BN_CLICKED);
        if (lnet_live_on() || !strstr(g_loc_last_cmd, "loc_lan=1") || strstr(g_loc_last_cmd, "loc_live") ||
            strstr(g_loc_last_cmd, "loc_camp"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "LAN: '%.200s'", g_loc_last_cmd);
    }
    /* aufraeumen */
    SetEnvironmentVariableA("SB_TEST_LOC_READY", NULL);
    lnet_kind_set(0);
    camp_fixture(LCAMP_R_UNKNOWN);
    g_acc_live_pending = 0;
    g_loc_lan = lan0;
    g_loc_lan_room[0] = 0;
    g_locgate_skip = 1;
    g_loc_online = on0;
    g_onl_fix_srv = fs0;
    g_upd_cfg = uc0;
    g_upd_ask = ask0;
    g_loc_last_cmd[0] = '\0';
    g_locfix_path[0] = '\0';
    g_loc_checked = 0;
    ui_settext(ctl(ID_LOC_IMAGE), "");
    u8_DeleteFile(cfg);
    g_ucfg_test[0] = '\0';
    g_acc_fixture = 0;
    g_acc_loaded = 0;
    g_acc_view = 0;
    acc_x_reset();
    g_acc = a0;
    memset(&g_acc_st, 0, sizeof g_acc_st);
    g_acc_st_ok = 0;
    g_game = LGAME_SB;
    loctab_nav_layout();
    onl_sync_ui();
    netm_sync_ui();
    show_page(P_PLAY);
    if (!why[0]) {
        GT_OK("65h W487/W501 ONLINE-BOTH (connection Online/LAN only, 2 entries; one check account+gamertag+not LAN; Online with gamertag: loc_live=1 + loc_camp=1 exactly once; without gamertag/account/status: start allowed, System Link only, hint, no page jump; LAN never loc_live; migration launcher_loc_live=1 -> Online once, key 0, choice never writes it)");
    } else {
        GT_FAIL("65h W487/W501 ONLINE-BOTH", "%s", why);
    }
}

/* --- 65i (W491 CAMP-CLIENT): Kampagnenstatus fuer Online mit Gamertag (W501). (0) lcamp_state alle Kombinationen Konto/Gamertag/
 * Version/Erreichbarkeit (6 x 2 x 3); (1) camp_status aus den echten Quellen (acc_fixture, locver_fixture,
 * camp_fixture) -> richtiger Hinweis EN/DE, Texte passen einzeilig (Karte Match + Karte Multiplayer); (2) Pruefung nur
 * Online mit Gamertag (steelnet), gecacht (Selbsttest zaehlt nur, nie Netz); Ergebnis (camp_apply) wirkt; (3) Start
 * Online mit Gamertag: loc_live=1 + loc_camp=1 genau einmal, auch wenn nicht erreichbar (Start nie gesperrt); ohne Gamertag/LAN ohne
 * loc_camp; (4) intern: Auswahl Saison/Testkampagne (Lobby & Chat), gemerkt in user.cfg, Test -> loc_camp_id=test,
 * Standard wiederherstellen = Saison; Community: keine Auswahl, nie loc_camp_id (auch mit Schluessel in user.cfg). --- */
void camp_gui_selftest(int *pass, int *fail)
{
    char why[300] = "", tmp[1024], cfg[1200], t[300], w[300];
    int on0 = g_loc_online, fs0 = g_onl_fix_srv, uc0 = g_upd_cfg, ask0 = g_upd_ask, lan0 = g_loc_lan;
    int ct0 = g_camp_test, tab0 = g_lnet_tab;
    lacc_store_t a0 = g_acc;
#if !SB_EDITION_COMMUNITY
    char txt[4096] = "";
    FILE *f = NULL;
    size_t n;
#endif
    int li;
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw491_gui_user_%lu.cfg", tmp, GetCurrentProcessId());
    write_file(cfg, "[display]\nwidth=1280\n");
    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, cfg);
    { /* 0: reine Logik, alle Kombinationen */
        static const int kAcc[6] = { ACC_LIVE_OK, ACC_LIVE_OFF, ACC_LIVE_NOACC, ACC_LIVE_UNK, ACC_LIVE_NOGT, ACC_LIVE_GTOFF };
        static const int kWant[6] = { -1, LCAMP_OFF, LCAMP_NOACC, LCAMP_UNK, LCAMP_NOGT, LCAMP_GTOFF };
        static const int kReach[3] = { LCAMP_R_UNKNOWN, LCAMP_R_OK, LCAMP_R_FAIL };
        static const int kReachWant[3] = { LCAMP_CHECK, LCAMP_OK, LCAMP_UNREACH };
        int a, o, r;
        for (a = 0; a < 6 && !why[0]; a++) {
            for (o = 0; o < 2 && !why[0]; o++) {
                for (r = 0; r < 3 && !why[0]; r++) {
                    int want = kWant[a] >= 0 ? kWant[a] : o ? LCAMP_OLD : kReachWant[r];
                    int got = lcamp_state(kAcc[a], o, kReach[r]);
                    if (got != want)
                        _snprintf_s(why, sizeof why, _TRUNCATE, "lcamp_state(acc %d, alt %d, reach %d) = %d, erwartet %d", kAcc[a],
                                    o, kReach[r], got, want);
                }
            }
        }
        if (!why[0] && lcamp_state(ACC_LIVE_OK, 0, LCAMP_R_OK) != LCAMP_OK) strcpy_s(why, sizeof why, "alles erfuellt != verfuegbar");
    }
    if (!why[0]) { /* W534 AVPRE: Erreichbarkeit = reines TCP (kein SChannel im Launcher) */
        char tw[160];
        lcamp_selftest_tcp(tw, sizeof tw);
        if (tw[0]) _snprintf_s(why, sizeof why, _TRUNCATE, "lcamp_probe TCP: %s", tw);
    }
    g_locgate_skip = 0;
    g_onl_fix_srv = 1;
    g_upd_cfg = 1;
    g_upd_ask = 0;
    gt_setenv("SB_TEST_LOC_READY", "1");
    strcpy_s(g_locfix_path, sizeof g_locfix_path, LOC_ISO);
    memset(&g_locfix, 0, sizeof g_locfix);
    g_locfix.kind = LGAME_IMG_LOC;
    g_locfix.title_id = LGAME_TID_LOC;
    g_game = LGAME_LOC;
    loctab_nav_layout();
    show_page(P_PLAY);
    ui_settext(ctl(ID_LOC_IMAGE), LOC_ISO);
    loctab_check();
    lan_kind_set(0);
    on_command(ID_NETM0 + NETM_STEEL, BN_CLICKED);
    locver_fixture("0.0.1");
    acc_fixture(1, 1, 1, 1, 0);
    g_camp_test = 0;
    for (li = 0; li < 2 && !why[0]; li++) { /* 1: echte Quellen -> Zustand + Text (EN/DE), Texte passen */
        static const char *const kKey[LCAMP_N] = { "gui.camp_ok",   "gui.camp_off",   "gui.camp_noacc",
                                                   "gui.camp_unk",  "gui.camp_gtoff", "gui.camp_nogt",
                                                   "gui.camp_old",  "gui.camp_check", "gui.camp_unreach" };
        int k;
        const char *bad;
        sb_lang_set(li ? "de" : "en");
        ui_apply_language();
        for (k = 0; k < 9 && !why[0]; k++) {
            int want;
            acc_fixture(k == 0 ? 0 : 1, k == 1 ? 0 : 1, 1, 1, 0);
            locver_fixture(k == 5 ? "0.3.3" : "0.0.1");
            camp_fixture(k == 6 ? LCAMP_R_UNKNOWN : k == 7 ? LCAMP_R_FAIL : LCAMP_R_OK);
            if (k == 2) g_acc_st_ok = 0;
            if (k == 3) g_acc_st.gamertag[0] = '\0';
            if (k == 4) {
                g_acc_st.gamertag[0] = '\0';
                strcpy_s(g_acc_st.gt_state, sizeof g_acc_st.gt_state, "off");
            }
            want = k == 0 ? LCAMP_OFF : k == 1 ? LCAMP_NOACC : k == 2 ? LCAMP_UNK : k == 3 ? LCAMP_NOGT : k == 4 ? LCAMP_GTOFF
                 : k == 5 ? LCAMP_OLD : k == 6 ? LCAMP_CHECK : k == 7 ? LCAMP_UNREACH : LCAMP_OK;
            camp_text(camp_status(), t, sizeof t);
            _snprintf_s(w, sizeof w, _TRUNCATE, LT(kKey[want]), LT("gui.camp_name"));
            if (camp_status() != want || strcmp(t, w) != 0 || !strchr(t, ':'))
                _snprintf_s(why, sizeof why, _TRUNCATE, "%s Fall %d: Zustand %d (erwartet %d) '%.120s'", li ? "de" : "en", k,
                            camp_status(), want, t);
        }
        bad = camp_texts_fit();
        if (!why[0] && bad) _snprintf_s(why, sizeof why, _TRUNCATE, "%s: Zeile passt nicht einzeilig: '%.200s'", li ? "de" : "en", bad);
        if (!why[0]) { /* Infozeile: Live bereit -> 2 Zeilen (Gamertag, Kampagne gruen); ohne Gamertag -> 0 (live_info) */
            COLORREF c2 = 0;
            acc_fixture(1, 1, 1, 1, 0);
            locver_fixture("0.0.1");
            camp_fixture(LCAMP_R_OK);
            if (!camp_info2(t, sizeof t, w, sizeof w, &c2) || !strstr(t, "Ace Pilot") || c2 != C_OK)
                _snprintf_s(why, sizeof why, _TRUNCATE, "%s Infozeile bereit '%.100s' '%.100s'", li ? "de" : "en", t, w);
            camp_fixture(LCAMP_R_FAIL);
            if (!why[0] && (!camp_info2(t, sizeof t, w, sizeof w, &c2) || c2 != C_WARN)) strcpy_s(why, sizeof why, "Infozeile Server weg");
            g_acc_st.gamertag[0] = '\0';
            if (!why[0] && camp_info2(t, sizeof t, w, sizeof w, &c2)) strcpy_s(why, sizeof why, "Infozeile ohne Gamertag != live_info");
        }
    }
    sb_lang_set("en");
    ui_apply_language();
    acc_fixture(1, 1, 1, 1, 0);
    locver_fixture("0.0.1");
    if (!why[0]) { /* 2 (W501): Pruefung nur Online mit Gamertag (steelnet), gecacht; Ergebnis wirkt */
        int n0;
        g_camp_dry = 1;
        g_camp_dry_n = 0;
        lnet_kind_set(0);
        g_acc_st.gamertag[0] = '\0';
        camp_fixture(LCAMP_R_UNKNOWN);
        if (camp_kick() || g_camp_dry_n) strcpy_s(why, sizeof why, "Pruefung Online ohne Gamertag");
        acc_fixture(1, 1, 1, 1, 0);
        lnet_kind_set(1);
        if (!why[0] && (camp_kick() || g_camp_dry_n)) strcpy_s(why, sizeof why, "Pruefung im LAN");
        lnet_kind_set(0);
        camp_fixture(LCAMP_R_UNKNOWN);
        n0 = g_camp_dry_n;
        if (!why[0] && (!camp_kick() || g_camp_dry_n != n0 + 1)) strcpy_s(why, sizeof why, "keine Pruefung Online mit Gamertag");
        camp_apply(0, LCAMP_R_OK);
        if (!why[0] && (camp_kick() || camp_status() != LCAMP_OK)) strcpy_s(why, sizeof why, "Ergebnis nicht gecacht bzw. nicht verfuegbar");
        camp_apply(0, LCAMP_R_FAIL);
        if (!why[0] && (camp_kick() || camp_status() != LCAMP_UNREACH)) strcpy_s(why, sizeof why, "Fehler nicht gecacht (60 s)");
        on_command(ID_NETM0 + NETM_OFF, BN_CLICKED); /* ohne Stufe steelnet: nie */
        camp_fixture(LCAMP_R_UNKNOWN);
        if (!why[0] && camp_kick()) strcpy_s(why, sizeof why, "Pruefung ohne Stufe steelnet");
        on_command(ID_NETM0 + NETM_STEEL, BN_CLICKED);
        locver_fixture("0.0.1");
        g_camp_dry = 0;
    }
    if (!why[0]) { /* 3 (W501): Start Online mit Gamertag: loc_camp=1 genau einmal, auch nicht erreichbar (Start nie
                    * gesperrt); Online ohne Gamertag und LAN ohne */
        int k;
        for (k = 0; k < 2 && !why[0]; k++) {
            const char *p;
            acc_fixture(1, 1, 1, 1, 0);
            lnet_kind_set(0);
            g_camp_test = 0;
            camp_fixture(k ? LCAMP_R_FAIL : LCAMP_R_OK);
            show_page(P_PLAY);
            pump();
            g_loc_last_cmd[0] = '\0';
            on_command(ID_LOC_START, BN_CLICKED);
            p = strstr(g_loc_last_cmd, "loc_camp=1");
            if (!strstr(g_loc_last_cmd, "loc_live=1") || !p || strstr(p + 10, "loc_camp=") || strstr(g_loc_last_cmd, "loc_camp_id"))
                _snprintf_s(why, sizeof why, _TRUNCATE, "Live-Start (%s): '%.220s'", k ? "nicht erreichbar" : "erreichbar", g_loc_last_cmd);
        }
        lnet_kind_set(0);
        g_acc_st.gamertag[0] = '\0'; /* W501: Online ohne Gamertag - Start frei, nur System Link */
        g_loc_last_cmd[0] = '\0';
        on_command(ID_LOC_START, BN_CLICKED);
        if (!why[0] && (!g_loc_last_cmd[0] || strstr(g_loc_last_cmd, "loc_camp") || strstr(g_loc_last_cmd, "loc_live")))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Online ohne Gamertag: '%.200s'", g_loc_last_cmd);
        acc_fixture(1, 1, 1, 1, 0);
        lnet_kind_set(1);
        g_loc_last_cmd[0] = '\0';
        on_command(ID_LOC_START, BN_CLICKED);
        if (!why[0] && (!g_loc_last_cmd[0] || strstr(g_loc_last_cmd, "loc_camp"))) _snprintf_s(why, sizeof why, _TRUNCATE, "LAN: '%.200s'", g_loc_last_cmd);
        lnet_kind_set(0);
    }
#if !SB_EDITION_COMMUNITY
    if (!why[0]) { /* 4 intern: Auswahl, gemerkt, Testkampagne an den Start, Hinweis in der Zeile, Standard = Saison */
        acc_fixture(1, 1, 1, 1, 0);
        lnet_kind_set(0); /* W501: Online (mit Gamertag) statt Live-Modus */
        show_page(P_LNET);
        lnet_tab_pick(1);
        pump();
        if (!ctl(ID_LNET_CAMP) || ComboBox_GetCount(ctl(ID_LNET_CAMP)) != 2 || !IsWindowVisible(ctl(ID_LNET_CAMP)) ||
            ComboBox_GetCurSel(ctl(ID_LNET_CAMP)) != 0)
            strcpy_s(why, sizeof why, "intern: Auswahl Kampagne fehlt bzw. nicht Saison");
        else {
            int i, found = 0;
            ComboBox_SetCurSel(ctl(ID_LNET_CAMP), 1);
            on_command(ID_LNET_CAMP, CBN_SELCHANGE);
            camp_fixture(LCAMP_R_OK);
            if (!g_camp_test || camp_sel() != 1) strcpy_s(why, sizeof why, "intern: Testkampagne nicht gewaehlt");
            for (i = 0; i < lnet_row_count() && !why[0]; i++) { /* Hinweis der Zeile = Status der Testkampagne, passt */
                RECT rc;
                const char *ht = NULL;
                int id = 0;
                if (lnet_hint_rect(i, &rc, &ht, &id) == 1 && id == ID_LNET_CAMP) {
                    _snprintf_s(w, sizeof w, _TRUNCATE, LT("gui.camp_ok"), LT("gui.camp_name_test"));
                    found = 1;
                    if (!ht || strcmp(ht, w) != 0 ||
                        !ui_text_fits(F_TINY, ht, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, 0))
                        _snprintf_s(why, sizeof why, _TRUNCATE, "intern: Hinweis '%.150s'", ht ? ht : "-");
                }
            }
            if (!why[0] && !found) strcpy_s(why, sizeof why, "intern: Zeile Kampagne nicht im Reiter Lobby & Chat");
        }
        if (!why[0]) {
            if (u8_fopen_s(&f, cfg, "rb") == 0 && f) {
                n = fread(txt, 1, sizeof txt - 1, f);
                txt[n] = 0;
                fclose(f);
            }
            if (!strstr(txt, "launcher_loc_camp_test=1")) _snprintf_s(why, sizeof why, _TRUNCATE, "intern user.cfg: '%.200s'", txt);
        }
        if (!why[0]) {
            const char *p;
            show_page(P_PLAY);
            pump();
            g_loc_last_cmd[0] = '\0';
            on_command(ID_LOC_START, BN_CLICKED);
            p = strstr(g_loc_last_cmd, "loc_camp_id=test");
            if (!strstr(g_loc_last_cmd, "loc_camp=1") || !p || strstr(p + 16, "loc_camp_id"))
                _snprintf_s(why, sizeof why, _TRUNCATE, "intern Test-Start: '%.220s'", g_loc_last_cmd);
        }
        if (!why[0]) { /* Laden: nur genau 1; Standard wiederherstellen = Saison */
            write_file(cfg, "launcher_loc_camp_test=1\n");
            onl_load();
            if (!g_camp_test) strcpy_s(why, sizeof why, "intern Laden: Test nicht gemerkt");
            write_file(cfg, "launcher_loc_camp_test=2\n");
            onl_load();
            if (!why[0] && g_camp_test) strcpy_s(why, sizeof why, "intern Laden: ungueltig als Test");
            g_camp_test = 1;
            if (!why[0] && lnet_is_default()) strcpy_s(why, sizeof why, "intern: Test gilt als Standard");
            lnet_reset();
            if (!why[0] && (g_camp_test || !lnet_is_default())) strcpy_s(why, sizeof why, "intern: Standard wiederherstellen != Saison");
        }
    }
#else
    if (!why[0]) { /* 4 Community: keine Auswahl, nie loc_camp_id (auch mit Schluessel in user.cfg bzw. gesetzter Variable) */
        acc_fixture(1, 1, 1, 1, 0);
        write_file(cfg, "launcher_loc_camp_test=1\n");
        onl_load();
        if (g_camp_test) strcpy_s(why, sizeof why, "Community: Schluessel gelesen");
        on_command(ID_NETM0 + NETM_STEEL, BN_CLICKED); /* onl_load setzt die Stufe aus der (Test-)cfg zurueck */
        locver_fixture("0.0.1");
        acc_fixture(1, 1, 1, 1, 0);
        lnet_kind_set(0); /* W501: Online (mit Gamertag) statt Live-Modus */
        show_page(P_LNET);
        lnet_tab_pick(1);
        pump();
        if (!why[0] && ctl(ID_LNET_CAMP)) strcpy_s(why, sizeof why, "Community: Auswahl Kampagne vorhanden");
        g_camp_test = 1;
        if (!why[0] && camp_sel() != 0) strcpy_s(why, sizeof why, "Community: camp_sel != Saison");
        show_page(P_PLAY);
        pump();
        g_loc_last_cmd[0] = '\0';
        on_command(ID_LOC_START, BN_CLICKED);
        if (!why[0] && (!strstr(g_loc_last_cmd, "loc_camp=1") || strstr(g_loc_last_cmd, "loc_camp_id")))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Community-Start: '%.220s'", g_loc_last_cmd);
        g_camp_test = 0;
    }
#endif
    /* aufraeumen */
    SetEnvironmentVariableA("SB_TEST_LOC_READY", NULL);
    lnet_kind_set(0);
    lnet_tab_pick(tab0);
    camp_fixture(LCAMP_R_UNKNOWN);
    g_camp_dry = 0;
    g_camp_test = ct0;
    g_loc_lan = lan0;
    g_loc_lan_room[0] = 0;
    g_locgate_skip = 1;
    g_loc_online = on0;
    g_onl_fix_srv = fs0;
    g_upd_cfg = uc0;
    g_upd_ask = ask0;
    g_loc_last_cmd[0] = '\0';
    g_locfix_path[0] = '\0';
    g_loc_checked = 0;
    ui_settext(ctl(ID_LOC_IMAGE), "");
    u8_DeleteFile(cfg);
    g_ucfg_test[0] = '\0';
    g_acc_fixture = 0;
    g_acc_loaded = 0;
    g_acc_view = 0;
    acc_x_reset();
    g_acc = a0;
    memset(&g_acc_st, 0, sizeof g_acc_st);
    g_acc_st_ok = 0;
    g_game = LGAME_SB;
    loctab_nav_layout();
    onl_sync_ui();
    netm_sync_ui();
    show_page(P_PLAY);
    if (!why[0]) {
        GT_OK("65i W491 CAMP-CLIENT campaign status (all 36 combinations account/gamertag/version/reachability, real sources -> right hint EN/DE, one-line fit on Match + Multiplayer cards, check only Online with gamertag (steelnet) and cached, never network in test, start Online with gamertag: loc_camp=1 exactly once even if unreachable, without gamertag/LAN without; internal: season/test choice saved, loc_camp_id=test only with test, reset = season; community: no choice, never loc_camp_id)");
    } else {
        GT_FAIL("65i W491 CAMP-CLIENT campaign status", "%s", why);
    }
}

/* --- 65e (W403 LAUNCHER-R1-POLISH): (1) LoC ohne Disc: Knopf grau, Sprung "Disc-Abbild waehlen" (statt steelnet),
 * Grund = Disc zuerst, Start verweigert, Sprung fuehrt nach Erweitert; mit Disc + steelnet frei. SB ohne Abbild: alle
 * Startwege gesperrt, Grund = R3. (2) Kopf-Chip: grau "steelnet ..." bis zur ersten Pruefung, gruen online, warnfarben
 * offline, "steelnet aus" (Nur Version), "offline"; Live-Zahlen ("steelnet · 5 online", Tooltip-Zeilen Ein-/Mehrzahl,
 * alter Server ohne Zahlen), Takt snp_due. (3) Tooltips (Status, Update). (4) Pruefung nie im Test-/Shot-
 * Betrieb (g_snp_dry zaehlt nur; ohne steelnet kein Anstoss). (5) Eject-Standard Umschalt+Entf, ohne Konflikt.
 * (6) Kopf rechtsbuendig zur Inhaltskante. (7) Multiplayer-Karte ohne steelnet-Satz. */
void r1_polish_gui_selftest(int *pass, int *fail)
{
    char why[300] = "", tmp[1024], cfg[1200], keep[16], t[1000], m[700];
    int on0 = g_loc_online, fs0 = g_onl_fix_srv, uc0 = g_upd_cfg, ask0 = g_upd_ask, li;
    lacc_probe_t pr;
    hdr_lay_t L;
    COLORREF col, dot;
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw403_gui_user_%lu.cfg", tmp, GetCurrentProcessId());
    write_file(cfg, "[display]\nwidth=1280\n");
    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, cfg);
    strcpy_s(keep, sizeof keep, sb_lang_code());
    g_locgate_skip = 0;
    g_onl_fix_srv = 1;
    g_upd_cfg = 1;
    g_upd_ask = 0;
    g_snp_dry = 1; /* nie Netz: Anstoss nur zaehlen */
    g_snp_dry_n = 0;
    gt_setenv("SB_TEST_LOC_READY", "1");
    strcpy_s(g_locfix_path, sizeof g_locfix_path, LOC_ISO);
    memset(&g_locfix, 0, sizeof g_locfix);
    g_locfix.kind = LGAME_IMG_LOC;
    g_locfix.title_id = LGAME_TID_LOC;
    g_game = LGAME_LOC;
    loctab_nav_layout();
    on_command(ID_NETM0 + NETM_STEEL, BN_CLICKED);
    show_page(P_PLAY);
    ui_settext(ctl(ID_LOC_IMAGE), "");
    loctab_check();
    show_page(P_PLAY);
    pump();
    /* 1a: LoC ohne Disc (steelnet an): grau, Disc-Sprung statt steelnet-Sprung, Start verweigert */
    if (IsWindowEnabled(ctl(ID_LOC_START)) || !IsWindowVisible(ctl(ID_LOC_IMG_GO)) ||
        IsWindowVisible(ctl(ID_LOC_GATE_GO)) || IsWindowEnabled(ctl(ID_START)) || IsWindowEnabled(ctl(ID_START_MENU))) {
        _snprintf_s(why, sizeof why, _TRUNCATE, "ohne Disc: en %d img %d gate %d fuss %d/%d", IsWindowEnabled(ctl(ID_LOC_START)),
                    IsWindowVisible(ctl(ID_LOC_IMG_GO)), IsWindowVisible(ctl(ID_LOC_GATE_GO)),
                    IsWindowEnabled(ctl(ID_START)), IsWindowEnabled(ctl(ID_START_MENU)));
    } else if (!loc_img_gate(m, sizeof m) || strcmp(m, LT("gui.loc_sum_none")) != 0) {
        _snprintf_s(why, sizeof why, _TRUNCATE, "ohne Disc: Grund '%.200s'", m);
    }
    if (!why[0]) {
        static const int kIds[] = { ID_LOC_START, ID_START, ID_START_MENU, ID_START_BIG };
        int k;
        for (k = 0; k < 4 && !why[0]; k++) {
            g_loc_last_cmd[0] = '\0';
            set_status_k("x", 0);
            on_command(kIds[k], BN_CLICKED);
            if (g_loc_last_cmd[0] || !strstr(g_status, LT("gui.loc_start_noimg")))
                _snprintf_s(why, sizeof why, _TRUNCATE, "ohne Disc Start %d: '%.150s'", kIds[k], g_status);
        }
    }
    if (!why[0]) { /* 1b: Sprung -> Erweitert (Disc-Karte) */
        on_command(ID_LOC_IMG_GO, BN_CLICKED);
        if (g_page != P_ADV || !IsWindowVisible(ctl(ID_LOC_IMAGE)))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Disc-Sprung -> Seite %d", g_page);
    }
    if (!why[0]) { /* 1c: ohne Disc und ohne steelnet: beide Gruende, Disc zuerst, ein Sprung (Disc) */
        on_command(ID_NETM0 + NETM_OFF, BN_CLICKED);
        show_page(P_PLAY);
        pump();
        if (!IsWindowVisible(ctl(ID_LOC_IMG_GO)) || IsWindowVisible(ctl(ID_LOC_GATE_GO)))
            strcpy_s(why, sizeof why, "ohne Disc + offline: falscher Sprung");
    }
    if (!why[0]) { /* 1d: Disc da, offline -> steelnet-Sprung; Disc + steelnet -> frei, kein Sprung */
        ui_settext(ctl(ID_LOC_IMAGE), LOC_ISO);
        loctab_check();
        show_page(P_PLAY);
        pump();
        if (IsWindowEnabled(ctl(ID_LOC_START)) || IsWindowVisible(ctl(ID_LOC_IMG_GO)) ||
            !IsWindowVisible(ctl(ID_LOC_GATE_GO)) || IsWindowEnabled(ctl(ID_START)) || IsWindowEnabled(ctl(ID_START_MENU))) {
            strcpy_s(why, sizeof why, "Disc + offline: Knopf/Sprung/Fussleiste");
        } else {
            on_command(ID_NETM0 + NETM_STEEL, BN_CLICKED);
            show_page(P_PLAY);
            pump();
            if (!IsWindowEnabled(ctl(ID_LOC_START)) || IsWindowVisible(ctl(ID_LOC_IMG_GO)) ||
                IsWindowVisible(ctl(ID_LOC_GATE_GO)) || !IsWindowEnabled(ctl(ID_START)) || !IsWindowEnabled(ctl(ID_START_MENU)))
                strcpy_s(why, sizeof why, "Disc + steelnet: nicht frei (Knopf/Fussleiste)");
        }
    }
    if (!why[0]) { /* 7: Multiplayer-Karte ohne steelnet: Konto-Zeile statt steelnet-Satz */
        char w1[200]; /* W418: Konto-Zeile heisst jetzt "steelnet account: <Name>" - geprueft wird, dass es die
                       * Konto-Zeile ist (nicht der steelnet-Grund unter dem Startknopf) */
        mp_card_acc_line(t, sizeof t);
        _snprintf_s(w1, sizeof w1, _TRUNCATE, LT("gui.mp_card_pilot"), g_acc.name);
        if ((strcmp(t, w1) != 0 && strcmp(t, LT("gui.mp_card_nopilot")) != 0 && strcmp(t, LT("gui.loc_onl_nosrv")) != 0) ||
            strstr(t, LT("gui.loc_gate_short")) || strstr(LT("gui.mp_card_off"), "steelnet") || strstr(LT("gui.loc_gate_short"), "Zahnrad") ||
            strlen(LT("gui.loc_gate_short")) > 60)
            _snprintf_s(why, sizeof why, _TRUNCATE, "Karte/Grund: '%.100s'", t);
    }
    if (!why[0]) { /* 1e: SB ohne Abbild: R3 sperrt alle Startwege, Grund = R3 */
        launcher_settings_t ks = g_s;
        g_game = LGAME_SB;
        loctab_nav_layout();
        g_s.game_image[0] = g_s.xbe_path[0] = g_s.media_root[0] = '\0';
        ui_from_settings(&g_s);
        ui_apply_rules();
        show_page(P_PLAY);
        pump();
        if (!g_rr[3].active || IsWindowEnabled(ctl(ID_START_BIG)) || IsWindowEnabled(ctl(ID_START)) ||
            IsWindowEnabled(ctl(ID_START_MENU)) || strcmp(g_rr[3].text, LT("gui.rule_r3")) != 0 ||
            !strstr(LT("gui.rule_r3"), LT("gui.btn_browse"))) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "SB ohne Abbild: r3 %d en %d '%.100s'", g_rr[3].active,
                        IsWindowEnabled(ctl(ID_START_BIG)), g_rr[3].text);
        } else {
            set_status_k("x", 0);
            g_loc_last_cmd[0] = '\0';
            on_command(ID_START_BIG, BN_CLICKED);
            if (strcmp(g_status, LT("gui.rule_r3")) != 0)
                _snprintf_s(why, sizeof why, _TRUNCATE, "SB Start ohne Abbild: '%.150s'", g_status);
        }
        g_s = ks;
        ui_from_settings(&g_s);
        ui_apply_rules();
    }
    /* 2 + 3 + 4: Chip-Zustaende, Tooltips, kein Netz */
    for (li = 0; li < 2 && !why[0]; li++) {
        sb_lang_set(li ? "de" : "en");
        ui_apply_language();
        g_snp_dry_n = 0;
        on_command(ID_NETM0 + NETM_VER, BN_CLICKED);
        hdr_chip_text(HDR_CHIP_ST, 0, t, sizeof t, &col, &dot);
        if (strcmp(t, LT("gui.hdr_steel_none")) != 0 || col != C_DIM || g_snp_state != -1)
            _snprintf_s(why, sizeof why, _TRUNCATE, "%s Nur Version: '%s'", li ? "de" : "en", t);
        hdr_tip_text(HDR_CHIP_ST, t, sizeof t);
        if (!why[0] && strcmp(t, LT("gui.hdr_tip_none")) != 0 && !strstr(t, LT("gui.hdr_tip_none")))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Tooltip Nur Version '%.100s'", t);
        if (!why[0] && g_snp_dry_n != 0)
            strcpy_s(why, sizeof why, "Nur Version: Pruefung angestossen");
        on_command(ID_NETM0 + NETM_OFF, BN_CLICKED);
        hdr_chip_text(HDR_CHIP_ST, 0, t, sizeof t, &col, &dot);
        if (!why[0] && strcmp(t, LT("gui.hdr_offline")) != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "Offline: '%s'", t);
        on_command(ID_NETM0 + NETM_STEEL, BN_CLICKED); /* -> genau ein Anstoss (Attrappe), grau bis Ergebnis */
        hdr_chip_text(HDR_CHIP_ST, 0, t, sizeof t, &col, &dot);
        if (!why[0] && (g_snp_dry_n != 1 || strcmp(t, LT("gui.hdr_steel_wait")) != 0 || col != C_DIM))
            _snprintf_s(why, sizeof why, _TRUNCATE, "steelnet vor Pruefung: n %d '%s'", g_snp_dry_n, t);
        hdr_tip_text(HDR_CHIP_ST, t, sizeof t);
        if (!why[0] && (!strstr(t, LT("gui.hdr_tip_wait")) || !strstr(t, LT("gui.hdr_tip_click"))))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Tooltip wartend '%.100s'", t);
        memset(&pr, 0, sizeof pr);
        pr.ok = 1;
        pr.ms = 38;
        strcpy_s(pr.version, sizeof pr.version, "2.4");
        snp_apply(&pr);
        hdr_chip_text(HDR_CHIP_ST, 0, t, sizeof t, &col, &dot);
        if (!why[0] && (strcmp(t, LT("gui.hdr_steel_on")) != 0 || col != C_OK || dot != C_OK))
            _snprintf_s(why, sizeof why, _TRUNCATE, "online: '%s'", t);
        hdr_tip_text(HDR_CHIP_ST, t, sizeof t);
        if (!why[0]) {
            char a[80], b[80], c[80];
            _snprintf_s(a, sizeof a, _TRUNCATE, LT("gui.hdr_tip_reach"), LT("gui.hdr_tip_yes"));
            _snprintf_s(b, sizeof b, _TRUNCATE, LT("gui.hdr_tip_ms"), 38);
            _snprintf_s(c, sizeof c, _TRUNCATE, LT("gui.hdr_tip_ver"), "2.4");
            if (!strstr(t, a) || !strstr(t, b) || !strstr(t, c) || !strstr(t, LT("gui.hdr_tip_click")) ||
                strstr(t, "relay") || strstr(t, SB_STEELNET_HOST))
                _snprintf_s(why, sizeof why, _TRUNCATE, "Tooltip online '%.200s'", t);
            _snprintf_s(a, sizeof a, _TRUNCATE, LT("gui.hdr_tip_pil"), 0); /* alter Server: keine Zahlenzeilen */
            if (!why[0] && (g_snp_has || strstr(t, a)))
                _snprintf_s(why, sizeof why, _TRUNCATE, "ohne online-Objekt Zahlen '%.200s'", t);
        }
        if (!why[0]) { /* Live-Zahlen: Chip "steelnet · 5 online" (eng: "steelnet · 5"), Tooltip-Zeilen Ein-/Mehrzahl */
            static const int kN[2][3] = { { 5, 2, 0 }, { 1, 1, 1 } };
            int v;
            for (v = 0; v < 2 && !why[0]; v++) {
                char a[80], b[80], c[80], e[80];
                pr.has_counts = 1;
                pr.players = kN[v][0];
                pr.lobbies = kN[v][1];
                pr.games = kN[v][2];
                snp_apply(&pr);
                _snprintf_s(e, sizeof e, _TRUNCATE, LT("gui.hdr_steel_cnt"), kN[v][0]);
                hdr_chip_text(HDR_CHIP_ST, 0, t, sizeof t, &col, &dot);
                if (strcmp(t, e) != 0 || col != C_OK || dot != C_OK)
                    _snprintf_s(why, sizeof why, _TRUNCATE, "Zahlen Chip '%s'", t);
                hdr_chip_text(HDR_CHIP_ST, 1, t, sizeof t, &col, &dot);
                _snprintf_s(e, sizeof e, _TRUNCATE, "%s" HDR_SUB_SEP "%d", LT("gui.hdr_steel"), kN[v][0]);
                if (!why[0] && strcmp(t, e) != 0)
                    _snprintf_s(why, sizeof why, _TRUNCATE, "Zahlen Kurzform '%s'", t);
                hdr_layout(&L);
                if (!why[0] && (L.kind[0] != HDR_CHIP_ST || L.shortened || L.x[L.n - 1] < HDR_GAME_END + 12))
                    _snprintf_s(why, sizeof why, _TRUNCATE, "Zahlen Lage: kurz %d x %d", L.shortened, L.x[L.n - 1]);
                hdr_tip_text(HDR_CHIP_ST, t, sizeof t);
                _snprintf_s(a, sizeof a, _TRUNCATE, LT(v ? "gui.hdr_tip_pil_1" : "gui.hdr_tip_pil"), kN[v][0]);
                _snprintf_s(b, sizeof b, _TRUNCATE, LT(v ? "gui.hdr_tip_lob_1" : "gui.hdr_tip_lob"), kN[v][1]);
                _snprintf_s(c, sizeof c, _TRUNCATE, LT(v ? "gui.hdr_tip_gam_1" : "gui.hdr_tip_gam"), kN[v][2]);
                _snprintf_s(e, sizeof e, _TRUNCATE, LT("gui.hdr_tip_ms"), 38);
                if (!why[0] && (!strstr(t, a) || !strstr(t, b) || !strstr(t, c) || !strstr(t, e) ||
                                !strstr(t, LT("gui.hdr_tip_click")) || strstr(t, b) < strstr(t, a) ||
                                strstr(t, c) < strstr(t, b) || strstr(t, e) < strstr(t, c)))
                    _snprintf_s(why, sizeof why, _TRUNCATE, "Zahlen Tooltip '%.250s'", t);
            }
            pr.has_counts = pr.players = pr.lobbies = pr.games = 0;
            snp_apply(&pr); /* wieder ohne Zahlen: Anzeige wie vorher */
            hdr_chip_text(HDR_CHIP_ST, 0, t, sizeof t, &col, &dot);
            if (!why[0] && (g_snp_has || strcmp(t, LT("gui.hdr_steel_on")) != 0))
                _snprintf_s(why, sizeof why, _TRUNCATE, "Zahlen nicht vergessen '%s'", t);
        }
        if (!why[0]) { /* Takt: sichtbar 60 s, minimiert/verborgen 5 min (Ungenauigkeit des Timers zaehlt mit) */
            if (snp_due(1, 30000) || !snp_due(1, SNP_FAST_MS - 100) || !snp_due(1, SNP_FAST_MS) ||
                snp_due(0, SNP_FAST_MS) || snp_due(0, 4 * 60 * 1000) || !snp_due(0, SNP_SLOW_MS - 100))
                strcpy_s(why, sizeof why, "Takt snp_due");
        }
        pr.ok = 0;
        pr.ms = 5000;
        pr.version[0] = '\0';
        snp_apply(&pr);
        hdr_chip_text(HDR_CHIP_ST, 0, t, sizeof t, &col, &dot);
        if (!why[0] && (strcmp(t, LT("gui.hdr_steel_off")) != 0 || col != C_WARN))
            _snprintf_s(why, sizeof why, _TRUNCATE, "offline-Ergebnis: '%s'", t);
        hdr_tip_text(HDR_CHIP_ST, t, sizeof t);
        if (!why[0]) {
            char a[80];
            _snprintf_s(a, sizeof a, _TRUNCATE, LT("gui.hdr_tip_reach"), LT("gui.hdr_tip_no"));
            if (!strstr(t, a))
                _snprintf_s(why, sizeof why, _TRUNCATE, "Tooltip offline '%.200s'", t);
        }
        if (!why[0]) { /* Update-Chip: installiert/neu/Klick */
            const char *v0 = g_upd_ver;
            lupd_res_t r0 = g_upd_res;
            g_upd_ver = "0.3.0";
            memset(&g_upd_res, 0, sizeof g_upd_res);
            g_upd_res.result = LUPD_R_NEWER;
            strcpy_s(g_upd_res.rel.tag, sizeof g_upd_res.rel.tag, "v0.3.1");
            hdr_tip_text(HDR_CHIP_UPD, t, sizeof t);
            if (!strstr(t, "0.3.0") || !strstr(t, "0.3.1") || !strstr(t, LT("gui.hdr_tip_rel")))
                _snprintf_s(why, sizeof why, _TRUNCATE, "Tooltip Update '%.200s'", t);
            g_upd_ver = v0;
            g_upd_res = r0;
        }
        if (!why[0]) { /* 4: Fensterbetrieb aus (g_snp_live 0) und ohne Attrappe -> nie ein Thread */
            int busy0 = g_snp_busy, live0 = g_snp_live;
            g_snp_dry = 0;
            g_snp_live = 0;
            if (snp_kick() != 0 || g_snp_busy != busy0)
                strcpy_s(why, sizeof why, "Pruefung ausserhalb des Fensterbetriebs");
            g_snp_live = live0;
            g_snp_dry = 1;
        }
        on_command(ID_NETM0 + NETM_OFF, BN_CLICKED); /* Ergebnis vergessen */
        if (!why[0] && g_snp_state != -1)
            strcpy_s(why, sizeof why, "Offline: Ergebnis nicht vergessen");
    }
    if (!why[0]) { /* 6: Kopf rechtsbuendig: Zahnrad endet an der Inhaltskante, Chips links daneben, unter _ x */
        hdr_layout(&L);
        if (HDR_GEAR_X + HDR_GEAR_W != CX0 + CW || HDR_CHIP_Y < HDR_WB_H || L.x[0] + L.w[0] != HDR_CHIP_R)
            _snprintf_s(why, sizeof why, _TRUNCATE, "Kopf: Zahnrad %d..%d Inhalt %d Chip %d", HDR_GEAR_X,
                        HDR_GEAR_X + HDR_GEAR_W, CX0 + CW, L.x[0] + L.w[0]);
    }
    if (!why[0]) { /* 5: Eject-Standard (Launcher-Kopie der Standardbelegung) */
        static lctrl_bind_t b; /* gross (~31 KB): nicht auf den Stapel des Selbsttests */
        char cf[4][160];
        int ix = lctrl_bind_elem_index("bi3"), n, k;
        lctrl_bind_init(&b);
        lctrl_bind_load_text(ks_legacy_text, &b);
        if (ix < 0 || b.row[ix].n_alt != 1 || strcmp(b.row[ix].alt[0], "KEY:SHIFT+KEY:DELETE") != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Eject-Standard '%s'", ix >= 0 ? b.row[ix].alt[0] : "?");
        } else {
            n = lctrl_bind_conflicts(&b, cf, 4);
            for (k = 0; k < n && !why[0]; k++) {
                if (strstr(cf[k], "DELETE"))
                    _snprintf_s(why, sizeof why, _TRUNCATE, "Eject-Konflikt '%s'", cf[k]);
            }
        }
    }
    /* aufraeumen */
    SetEnvironmentVariableA("SB_TEST_LOC_READY", NULL);
    g_snp_dry = 0;
    g_snp_state = -1;
    g_locgate_skip = 1;
    g_loc_online = on0;
    g_onl_fix_srv = fs0;
    g_upd_cfg = uc0;
    g_upd_ask = ask0;
    g_loc_last_cmd[0] = '\0';
    g_locfix_path[0] = '\0';
    g_loc_checked = 0;
    ui_settext(ctl(ID_LOC_IMAGE), "");
    u8_DeleteFile(cfg);
    g_ucfg_test[0] = '\0';
    sb_lang_set(keep);
    ui_apply_language();
    g_game = LGAME_SB;
    loctab_nav_layout();
    onl_sync_ui();
    netm_sync_ui();
    show_page(P_PLAY);
    if (!why[0]) {
        GT_OK("65e W403 R1-POLISH (Disc-Sperre LoC/SB + Sprung, Chip grau/gruen/offline/aus/offline, Live-Zahlen + Takt 60 s/5 min, Tooltips, keine Pruefung im Test, Eject Umschalt+Entf, Kopf buendig)");
    } else {
        GT_FAIL("65e W403 R1-POLISH", "%s", why);
    }
}

/* --- 65f (W421 LOC-VERSION-GATE): Launcher laut steelnet (/v1/info min_client_version, Attrappe ohne Netz) veraltet
 * -> LoC-Start gesperrt (Knopf, Fussleiste, Startmenue, Enter), Grund "needs the latest version (X)" EN/DE unter dem
 * Knopf, in der Fussleiste und auf der Multiplayer-Karte, Knopf zur Release-Seite (nur gezaehlt), Chip-Tooltip.
 * Gleich/neuer, Feld fehlt/leer/kaputt/sehr lang, Server nicht erreichbar -> frei. SB nie gesperrt. Stufe Offline
 * vergisst die Sperre (nur Sitzung, nichts gespeichert). */
__declspec(noinline) void locver_gui_selftest(int *pass, int *fail)
{
    static const int kIds[] = { ID_LOC_START, ID_START, ID_START_MENU, ID_START_BIG, IDOK };
    static const char *const kFree[] = { "0.3.2", "0.3.1", "0.2.9", "", "1.2", "abc", "0.3.3-beta", "1.2.3.4",
                                         "99999.0.0", "0.3.3 " };
    char why[300] = "", tmp[1024], cfg[1200], keep[16], t[1000], m[700], w[300];
    int on0 = g_loc_online, fs0 = g_onl_fix_srv, uc0 = g_upd_cfg, ask0 = g_upd_ask, li, k, sb_en[2];
    lacc_probe_t pr;
    COLORREF col;
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw421_gui_user_%lu.cfg", tmp, GetCurrentProcessId());
    write_file(cfg, "[display]\nwidth=1280\n");
    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, cfg);
    strcpy_s(keep, sizeof keep, sb_lang_code());
    g_locgate_skip = 0;
    g_onl_fix_srv = 1;
    g_upd_cfg = 1;
    g_upd_ask = 0;
    g_snp_dry = 1; /* nie Netz */
    g_ver_open_n = 0;
    gt_setenv("SB_TEST_LOC_READY", "1");
    strcpy_s(g_locfix_path, sizeof g_locfix_path, LOC_ISO);
    memset(&g_locfix, 0, sizeof g_locfix);
    g_locfix.kind = LGAME_IMG_LOC;
    g_locfix.title_id = LGAME_TID_LOC;
    g_game = LGAME_LOC;
    loctab_nav_layout();
    on_command(ID_NETM0 + NETM_STEEL, BN_CLICKED);
    show_page(P_PLAY);
    ui_settext(ctl(ID_LOC_IMAGE), LOC_ISO);
    loctab_check();
    ui_settext(ctl(ID_LOC_CODE), "");
    show_page(P_PLAY);
    pump();
    /* 0: eingebaute Version = poc-native/VERSION (strikt), ohne Pruefung frei */
    if (!sb_netver_strict(SB_NET_VERSION) || loc_ver_gate() || !IsWindowEnabled(ctl(ID_LOC_START)))
        _snprintf_s(why, sizeof why, _TRUNCATE, "Start: Version '%s' Sperre %d", SB_NET_VERSION, loc_ver_gate());
    /* 1: veraltet (Launcher 0.3.2, Dienst 0.3.3) -> gesperrt, EN/DE */
    for (li = 0; li < 2 && !why[0]; li++) {
        sb_lang_set(li ? "de" : "en");
        ui_apply_language();
        locver_fixture("0.3.3");
        show_page(P_PLAY);
        pump();
        _snprintf_s(w, sizeof w, _TRUNCATE, LT("gui.loc_ver_gate"), "0.3.3");
        loc_gate_msg(m, sizeof m);
        if (!loc_ver_gate() || IsWindowEnabled(ctl(ID_LOC_START)) || IsWindowEnabled(ctl(ID_START)) ||
            IsWindowEnabled(ctl(ID_START_MENU)) || !IsWindowVisible(ctl(ID_LOC_VER_GO)) ||
            IsWindowVisible(ctl(ID_LOC_GATE_GO)) || IsWindowVisible(ctl(ID_LOC_IMG_GO)) || loc_launch_ok())
            _snprintf_s(why, sizeof why, _TRUNCATE, "%s veraltet: en %d fuss %d/%d ver %d gate %d img %d", li ? "de" : "en",
                        IsWindowEnabled(ctl(ID_LOC_START)), IsWindowEnabled(ctl(ID_START)),
                        IsWindowEnabled(ctl(ID_START_MENU)), IsWindowVisible(ctl(ID_LOC_VER_GO)),
                        IsWindowVisible(ctl(ID_LOC_GATE_GO)), IsWindowVisible(ctl(ID_LOC_IMG_GO)));
        else if (strcmp(m, w) != 0 || !strstr(m, "0.3.3") || !strstr(m, "Line of Contact"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "%s Grund '%.200s'", li ? "de" : "en", m);
        for (k = 0; k < 5 && !why[0]; k++) { /* jeder Startweg: gleiche Sperre */
            set_status_k("x", 0);
            g_loc_last_cmd[0] = '\0';
            if (kIds[k] == IDOK) {
                SetFocus(NULL);
            }
            on_command(kIds[k], BN_CLICKED);
            if (g_loc_last_cmd[0] || strcmp(g_status, w) != 0)
                _snprintf_s(why, sizeof why, _TRUNCATE, "veraltet Start %d: '%.120s' '%.80s'", kIds[k], g_status,
                            g_loc_last_cmd);
        }
        if (!why[0]) { /* Multiplayer-Karte (Infozeile rot) + Knopf Release-Seite auf Multiplayer */
            if (onl_info(t, sizeof t, &col) != 0 || strcmp(t, w) != 0 || col != C_ERR)
                _snprintf_s(why, sizeof why, _TRUNCATE, "Karte '%.150s'", t);
            show_page(P_LNET);
            pump();
            if (!why[0] && !IsWindowVisible(ctl(ID_LNET_VERPAGE)))
                strcpy_s(why, sizeof why, "Multiplayer: Knopf Release-Seite fehlt");
            show_page(P_PLAY);
        }
        if (!why[0]) { /* Chip-Tooltip */
            char w2[200];
            _snprintf_s(w2, sizeof w2, _TRUNCATE, LT("gui.hdr_tip_locver"), "0.3.3", "0.3.2");
            hdr_tip_text(HDR_CHIP_ST, t, sizeof t);
            if (!strstr(t, w2))
                _snprintf_s(why, sizeof why, _TRUNCATE, "Tooltip '%.200s'", t);
        }
        if (!why[0]) { /* Knopf: Release-Seite (Browser; hier nur gezaehlt), kein Seitenwechsel, kein Start */
            int n0 = g_ver_open_n;
            g_loc_last_cmd[0] = '\0';
            on_command(ID_LOC_VER_GO, BN_CLICKED);
            on_command(ID_LNET_VERPAGE, BN_CLICKED);
            if (g_ver_open_n != n0 + 2 || g_page != P_PLAY || g_loc_last_cmd[0])
                _snprintf_s(why, sizeof why, _TRUNCATE, "Release-Seite %d/%d Seite %d", g_ver_open_n - n0, 2, g_page);
        }
    }
    /* 2: SB nie betroffen: SB-Start mit Sperre wie ohne */
    if (!why[0]) {
        int j;
        g_game = LGAME_SB;
        loctab_nav_layout();
        for (j = 0; j < 2; j++) {
            locver_fixture(j ? "0.3.3" : "0.3.2");
            show_page(P_PLAY);
            pump();
            sb_en[j] = IsWindowEnabled(ctl(ID_START_BIG)) ? 1 : 0;
            if (IsWindowVisible(ctl(ID_LOC_VER_GO)))
                strcpy_s(why, sizeof why, "SB: Knopf Release-Seite sichtbar");
        }
        if (!why[0] && sb_en[0] != sb_en[1])
            _snprintf_s(why, sizeof why, _TRUNCATE, "SB-Start von der Versionssperre betroffen (%d/%d)", sb_en[0], sb_en[1]);
        g_game = LGAME_LOC;
        loctab_nav_layout();
    }
    /* 3: gleich/aelter als eigene, Feld fehlt/leer/kaputt/sehr lang -> frei (snp_apply prueft erneut strikt) */
    for (k = 0; k < (int)(sizeof kFree / sizeof kFree[0]) && !why[0]; k++) {
        locver_fixture("0.3.3"); /* vorher gesperrt: jede neue Antwort ersetzt das Ergebnis */
        memset(&pr, 0, sizeof pr);
        pr.ok = 1;
        strncpy_s(pr.min_ver, sizeof pr.min_ver, kFree[k], _TRUNCATE);
        snp_apply(&pr);
        show_page(P_PLAY);
        pump();
        if (loc_ver_gate() || !IsWindowEnabled(ctl(ID_LOC_START)) || IsWindowVisible(ctl(ID_LOC_VER_GO)) ||
            !loc_launch_ok())
            _snprintf_s(why, sizeof why, _TRUNCATE, "frei '%s': Sperre %d en %d", kFree[k], loc_ver_gate(),
                        IsWindowEnabled(ctl(ID_LOC_START)));
    }
    if (!why[0]) { /* 3b: frei -> Startweg geht wirklich durch */
        g_loc_last_cmd[0] = '\0';
        on_command(ID_LOC_START, BN_CLICKED);
        if (!strstr(g_loc_last_cmd, LOC_ISO))
            _snprintf_s(why, sizeof why, _TRUNCATE, "frei: Startweg '%.200s'", g_loc_last_cmd);
    }
    /* 4: Server nicht erreichbar (auch mit Feld in einer kaputten Antwort) -> frei */
    if (!why[0]) {
        locver_fixture("0.3.3");
        memset(&pr, 0, sizeof pr);
        pr.ok = 0;
        strcpy_s(pr.error, sizeof pr.error, "net");
        strcpy_s(pr.min_ver, sizeof pr.min_ver, "0.3.3");
        snp_apply(&pr);
        show_page(P_PLAY);
        pump();
        if (loc_ver_gate() || g_locver_min[0] || !IsWindowEnabled(ctl(ID_LOC_START)))
            strcpy_s(why, sizeof why, "nicht erreichbar: gesperrt");
    }
    /* 5: Stufe Offline vergisst die Sperre; zurueck auf steelnet ohne neue Pruefung frei; nichts in user.cfg */
    if (!why[0]) {
        locver_fixture("0.3.3");
        on_command(ID_NETM0 + NETM_OFF, BN_CLICKED);
        on_command(ID_NETM0 + NETM_STEEL, BN_CLICKED);
        show_page(P_PLAY);
        pump();
        gt_read(cfg, t, sizeof t);
        if (loc_ver_gate() || g_locver_min[0] || !IsWindowEnabled(ctl(ID_LOC_START)) || strstr(t, "0.3.3") ||
            strstr(t, "min_client"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Stufenwechsel: Sperre %d cfg '%.120s'", loc_ver_gate(), t);
    }
    /* aufraeumen */
    SetEnvironmentVariableA("SB_TEST_LOC_READY", NULL);
    g_snp_dry = 0;
    g_snp_state = -1;
    g_locver_min[0] = '\0';
    g_locver_own_fix = NULL;
    g_locgate_skip = 1;
    g_loc_online = on0;
    g_onl_fix_srv = fs0;
    g_upd_cfg = uc0;
    g_upd_ask = ask0;
    g_loc_last_cmd[0] = '\0';
    g_locfix_path[0] = '\0';
    g_loc_checked = 0;
    ui_settext(ctl(ID_LOC_IMAGE), "");
    u8_DeleteFile(cfg);
    g_ucfg_test[0] = '\0';
    sb_lang_set(keep);
    ui_apply_language();
    g_game = LGAME_SB;
    loctab_nav_layout();
    onl_sync_ui();
    netm_sync_ui();
    show_page(P_PLAY);
    if (!why[0]) {
        GT_OK("65f W421 LOC-VERSION-GATE (veraltet laut /v1/info: LoC-Start gesperrt auf allen Startwegen, Grund EN/DE, Karte, Release-Knopf, Tooltip; gleich/neuer, Feld fehlt/leer/kaputt/lang, Server weg -> frei; SB nie; nur Sitzung)");
    } else {
        GT_FAIL("65f W421 LOC-VERSION-GATE", "%s", why);
    }
}

/* W402 LAUNCHER-PERGAME (Fall 65e): jedes Spiel hat eigene Werte (user.cfg | user_loc.cfg), LoC-Start nur mit
 * der LoC-Datei, PAL und SB-Optionen in LoC gesperrt, 16:9 in den Zusammenfassungen. */
static int pg_has(const char *path, const char *needle)
{
    static char b[16384];
    gt_read(path, b, sizeof b);
    return strstr(b, needle) != NULL;
}

void pergame_gui_selftest(int *pass, int *fail)
{
    char why[300] = "", tmp[1024], cfg[1200], lcf[1200], t[800];
    launcher_settings_t a, keep_s = g_s, keep_base = g_disk_base;
    int keep_ok = g_disk_base_ok, keep_game = g_game, keep_page = g_page, keep_skip = g_locgate_skip;
    lgame_state_t keep_loc = g_loc;
    loc_sumrow_t r[LOC_SUM_N];
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw402_pg_user_%lu.cfg", tmp, GetCurrentProcessId());
    lgame_cfg_path(cfg, LGAME_LOC, lcf, sizeof lcf);
    u8_DeleteFile(lcf);
    write_file(cfg, "[display]\nwidth=1280\n# fremde Zeile\nlauncher_pergame=1\n");
    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, cfg);
    memset(g_pg_ok, 0, sizeof g_pg_ok);
    g_loc.loc_widescreen = LGAME_WIDE_43;
    /* a: Steel Battalion setzen + speichern */
    g_game = LGAME_SB;
    show_page(P_DISPLAY);
    lcfg_defaults(&a);
    strcpy_s(a.xbe_path, sizeof a.xbe_path, "C:\\w402\\default.xbe");
    a.width = 1600;
    a.height = 900;
    a.volume_music = 40;
    a.aim_assist = 1;
    a.game_tick = 25;
    g_s = a;
    disk_base_sync(&g_s);
    ui_from_settings(&g_s);
    ui_to_settings(&g_s);
    if (save_user_merged(cfg) < 0 || !pg_has(cfg, "width=1600\n") || !pg_has(cfg, "volume_music=40\n") ||
        !pg_has(cfg, "game_tick=25\n"))
        strcpy_s(why, sizeof why, "a: SB nicht gespeichert");
    /* b: nach LoC: eigene (Standard-)Werte, keine SB-Werte */
    if (!why[0]) {
        loctab_set_game(LGAME_LOC);
        if (g_game != LGAME_LOC || g_s.width == 1600 || g_s.volume_music == 40 || g_s.game_tick != 30)
            _snprintf_s(why, sizeof why, _TRUNCATE, "b: LoC zeigt SB-Werte (%ux%u, Musik %d, Takt %d)", g_s.width,
                        g_s.height, g_s.volume_music, g_s.game_tick);
        else if (strcmp(g_s.xbe_path, a.xbe_path) != 0)
            strcpy_s(why, sizeof why, "b: SB-Pfade (global) fehlen im LoC-Stand");
    }
    /* c: LoC setzen + speichern -> nur user_loc.cfg, SB-Datei unveraendert */
    if (!why[0]) {
        a = g_s;
        a.width = 1920;
        a.height = 1080;
        a.volume_music = 70;
        a.game_tick = 25; /* PAL kommt nie in die LoC-Datei */
        ui_from_settings(&a);
        ui_set_checked(ID_PAL50, 1);
        ui_to_settings(&g_s);
        if (g_s.game_tick != 30)
            strcpy_s(why, sizeof why, "c: PAL in LoC aus der Oberflaeche uebernommen");
        else if (do_save(0) != 0 || !pg_has(lcf, "width=1920\n") || !pg_has(lcf, "volume_music=70\n") ||
                 !pg_has(lcf, "game_tick=30\n") || pg_has(lcf, "xbe_path") || pg_has(lcf, "launcher_game") ||
                 pg_has(lcf, "launcher_loc_") || pg_has(lcf, "launcher_pergame") || pg_has(lcf, "launcher_update") ||
                 !pg_has(lcf, "loc_widescreen=0\n") || !pg_has(lcf, "launcher_loc169=1\n")) /* W414: Bildformat + Markierung */
            _snprintf_s(why, sizeof why, _TRUNCATE, "c: user_loc.cfg falsch (%.100s)", g_status);
        else if (!pg_has(cfg, "width=1600\n") || pg_has(cfg, "1920") || !pg_has(cfg, "volume_music=40\n") ||
                 !pg_has(cfg, "# fremde Zeile\n"))
            strcpy_s(why, sizeof why, "c: user.cfg (SB) mitgeaendert");
        else {
            char lw[32];
            _snprintf_s(lw, sizeof lw, _TRUNCATE, "lang=%s\n", g_s.lang);
            if (!pg_has(cfg, lw))
                strcpy_s(why, sizeof why, "c: Sprache (global) nicht in user.cfg");
        }
    }
    /* d: hin und her: jedes Spiel zeigt seine Werte (auch ungespeicherte bleiben je Spiel) */
    if (!why[0]) {
        loctab_set_game(LGAME_SB);
        if (g_s.width != 1600 || g_s.volume_music != 40 || g_s.game_tick != 25 || get_num(ID_W) != 1600)
            _snprintf_s(why, sizeof why, _TRUNCATE, "d: SB nach Rueckwechsel %ux%u Musik %d Takt %d Feld %u", g_s.width, g_s.height,
                        g_s.volume_music, g_s.game_tick, get_num(ID_W));
    }
    if (!why[0]) {
        ui_set_checked(ID_PAL50, 1);
        set_num(ID_W, 1700); /* ungespeichert */
        loctab_set_game(LGAME_LOC);
        if (g_s.width != 1920 || g_s.volume_music != 70 || get_num(ID_W) != 1920)
            _snprintf_s(why, sizeof why, _TRUNCATE, "d: LoC nach Wechsel %u Musik %d", g_s.width, g_s.volume_music);
        loctab_set_game(LGAME_SB);
        if (!why[0] && get_num(ID_W) != 1700)
            strcpy_s(why, sizeof why, "d: ungespeicherte SB-Aenderung verloren");
        set_num(ID_W, 1600);
        loctab_set_game(LGAME_LOC);
    }
    /* e: PAL und SB-Optionen in LoC gesperrt (mit Grund), in SB frei */
    if (!why[0]) {
        show_page(P_AUDIO);
        ui_apply_rules();
        pump();
        tip_text(ID_PAL50, t, sizeof t);
        if (IsWindowEnabled(ctl(ID_PAL50)) || strcmp(t, LT("gui.loc_lock_tick")) != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "e: PAL in LoC nicht gesperrt ('%.80s')", t);
        else if (ctl(ID_RP_LABEL) && IsWindowEnabled(ctl(ID_RP_LABEL)))
            strcpy_s(why, sizeof why, "e: Replay-Beschriftung in LoC frei");
    }
    if (!why[0]) {
        show_page(P_CTRL);
        ui_apply_rules();
        xtip_text(ID_CTRL_AUTOGEAR, t, sizeof t);
        if (!IsWindowEnabled(ctl(ID_CTRL_AIMASSIST)) || !IsWindowEnabled(ctl(ID_CTRL_AUTOGEAR)) ||
            loc_lock_key(ID_CTRL_AIMASSIST) || loc_lock_key(ID_CTRL_AUTOGEAR)) /* W440: frei mit Hinweis */
            strcpy_s(why, sizeof why, "e: Zielhilfe/Automatik in LoC gesperrt (W440: frei, nur Arcade)");
        else if (strcmp(t, LT("gui.loc_arcade_tip")) != 0 || !loc_tip_id(ID_CTRL_AIMASSIST))
            _snprintf_s(why, sizeof why, _TRUNCATE, "e: W440 Hinweis Arcade fehlt ('%.80s')", t);
        else if (IsWindowEnabled(ctl(ID_CTRL_ALLY))) /* W424 */
            strcpy_s(why, sizeof why, "e: Verbuendete Trupps in LoC frei");
        else if (IsWindowEnabled(ctl(ID_REM_LABEL)) || !loc_lock_key(ID_REM_LABEL)) /* W427: HUD-Etikett wie im ESC-Menue */
            strcpy_s(why, sizeof why, "e: HUD-Etikett in LoC nicht gesperrt");
        loctab_set_game(LGAME_SB);
        show_page(P_AUDIO);
        ui_apply_rules();
        if (!why[0] && (!IsWindowEnabled(ctl(ID_PAL50)) || !ui_checked(ID_PAL50)))
            strcpy_s(why, sizeof why, "e: PAL in SB gesperrt oder Wert verloren");
        if (!why[0]) { /* W440: in SB kein Arcade-Hinweis */
            xtip_text(ID_CTRL_AIMASSIST, t, sizeof t);
            if (t[0]) _snprintf_s(why, sizeof why, _TRUNCATE, "e: W440 Arcade-Hinweis in SB ('%.80s')", t);
        }
        loctab_set_game(LGAME_LOC);
    }
    /* f: Startweg: LoC startet mit user_loc.cfg (SB_USER_CFG), nie PAL; die SB-Kette kennt keine LoC-Werte */
    if (!why[0]) {
        g_locgate_skip = 1;
        strcpy_s(g_locfix_path, sizeof g_locfix_path, LOC_ISO);
        memset(&g_locfix, 0, sizeof g_locfix);
        g_locfix.kind = LGAME_IMG_LOC;
        g_locfix.title_id = LGAME_TID_LOC;
        ui_settext(ctl(ID_LOC_IMAGE), LOC_ISO);
        loctab_check();
        g_loc_online = 0;
        g_loc_last_cmd[0] = g_loc_last_cfg[0] = '\0';
        on_command(ID_LOC_START, BN_CLICKED);
        if (!strstr(g_loc_last_cmd, "loc_boot=1") || !strstr(g_loc_last_cmd, "game_tick=30") ||
            strstr(g_loc_last_cmd, "game_tick=25") || strstr(g_loc_last_cmd, "user.cfg"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "f: LoC-Kette '%.160s'", g_loc_last_cmd);
        else if (_stricmp(g_loc_last_cfg, lcf) != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "f: LoC-Datei '%.120s'", g_loc_last_cfg);
        else if (!pg_has(lcf, "input_test_hook=0\n") || pg_has(cfg, "input_test_hook"))
            strcpy_s(why, sizeof why, "f: Spielerbetrieb-Block nicht (nur) in user_loc.cfg");
        else if (GetEnvironmentVariableA("SB_USER_CFG", t, sizeof t) > 0)
            strcpy_s(why, sizeof why, "f: SB_USER_CFG bleibt im Launcher gesetzt");
        g_locfix_path[0] = '\0';
    }
    /* g: 16:9 ueberall statt 4:3 (Startbereit, Anzeige-Vorschau/Text, Warnband), Texte ohne "geteilt" */
    if (!why[0]) {
        g_loc.loc_widescreen = LGAME_WIDE_169;
        update_aspect_info();
        ui_apply_rules();
        loctab_sum_rows(r);
        if (!strstr(r[2].value, LT("gui.loc_wide_169")) || strstr(r[2].value, LT("gui.loc_wide_43")))
            _snprintf_s(why, sizeof why, _TRUNCATE, "g: Startbereit '%.120s'", r[2].value);
        else if (!strstr(g_aspect_info, "16:9") || strstr(g_aspect_info, "4:3"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "g: Anzeige '%.120s'", g_aspect_info);
        else if (g_orig_mask & (1u << SB_OFG_WIDE)) /* W414: 16:9 in LoC nicht mehr experimentell (W402: Warnband) */
            strcpy_s(why, sizeof why, "g: Warnband Breitbild trotz LoC-16:9 (W414)");
        else if (!loc_lock_key(ID_EDGE))
            strcpy_s(why, sizeof why, "g: Raender bei 16:9 nicht gesperrt");
        g_loc.loc_widescreen = LGAME_WIDE_43;
        update_aspect_info();
        if (!why[0] && (strstr(g_aspect_info, "16:9") || loc_lock_key(ID_EDGE)))
            strcpy_s(why, sizeof why, "g: 4:3 zeigt 16:9 bzw. sperrt die Raender");
    }
    if (!why[0]) {
        static const char *const kKeys[] = { "gui.loc_shared_rows", "gui.loc_switched", "gui.of_banner_loc", "gui.vol_status" };
        int li, k;
        for (li = 0; li < 2 && !why[0]; li++) {
            sb_lang_set(li ? "de" : "en");
            for (k = 0; k < 4 && !why[0]; k++) {
                const char *v = LT(kKeys[k]);
                if (strstr(v, "shared") || strstr(v, "also apply") || strstr(v, "teilt es") || strstr(v, "auch f\xC3\xBCr Steel"))
                    _snprintf_s(why, sizeof why, _TRUNCATE, "g: Text %s '%.80s'", kKeys[k], v);
            }
        }
        sb_lang_set("en");
    }
    u8_DeleteFile(cfg);
    u8_DeleteFile(lcf);
    g_ucfg_test[0] = '\0';
    g_locgate_skip = keep_skip;
    g_loc = keep_loc;
    memset(g_pg_ok, 0, sizeof g_pg_ok);
    g_game = keep_game;
    g_s = keep_s;
    g_disk_base = keep_base;
    g_disk_base_ok = keep_ok;
    ui_from_settings(&g_s);
    loctab_nav_layout();
    show_page(keep_page);
    ui_apply_rules();
    if (!why[0]) {
        GT_OK("65e W402 LAUNCHER-PERGAME (SB/LoC getrennt gespeichert und angezeigt, hin und her inkl. ungespeicherter Werte, "
              "LoC-Start mit user_loc.cfg + Spielerblock, nie PAL, PAL/Replay in LoC gesperrt mit Grund, Zielhilfe/Automatik frei mit Arcade-Hinweis (W440), "
              "SB frei, 16:9 in Startbereit/Anzeige/Warnband, Texte DE/EN ohne 'geteilt')");
    } else {
        GT_FAIL("65e W402 LAUNCHER-PERGAME", "%s", why);
    }
}

void loc_save_gui_selftest(int *pass, int *fail)
{
    char why[300] = "", tmp[1024], cfg[1200], lcf[1200], buf[8192];
    launcher_settings_t s0, s1, keep_s = g_s, keep_base = g_disk_base;
    int keep_ok = g_disk_base_ok, keep_game = g_game, r;
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw375_locsave_user.cfg", tmp);
    {
        FILE *f = NULL;
        if (u8_fopen_s(&f, cfg, "w") == 0 && f) {
            fputs("[display]\nwidth=1280\nheight=720\n# fremde Zeile\n", f);
            fclose(f);
        }
    }
    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, cfg);
    lgame_cfg_path(cfg, LGAME_LOC, lcf, sizeof lcf); /* W402: LoC speichert in user_loc.cfg */
    u8_DeleteFile(lcf);
    lcfg_defaults(&s0);
    s0.game_image[0] = s0.xbe_path[0] = s0.media_root[0] = '\0'; /* kein SB-Abbild, kein Ordner */
    s0.width = 1280;
    s0.height = 720;
    g_game = LGAME_LOC;
    disk_base_sync(&s0);
    s0.width = 1920; /* Grafik geaendert */
    s0.height = 1080;
    ui_from_settings(&s0);
    r = do_save(0);
    gt_read(lcf, buf, sizeof buf);
    if (r != 0) {
        _snprintf_s(why, sizeof why, _TRUNCATE, "LoC-Tab: Speichern abgelehnt (%.120s)", g_status);
    } else if (!strstr(buf, "width=1920\n") || !strstr(buf, "height=1080\n") || strstr(buf, "xbe_path=")) {
        _snprintf_s(why, sizeof why, _TRUNCATE, "LoC-Tab: nicht geschrieben: %.120s", buf);
    }
    if (!why[0]) { /* W402: SB-Datei unveraendert */
        gt_read(cfg, buf, sizeof buf);
        if (!strstr(buf, "width=1280\n") || strstr(buf, "width=1920") || !strstr(buf, "# fremde Zeile\n"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "SB-Datei mitgeaendert: %.120s", buf);
    }
    if (!why[0]) { /* Launcher-Neustart: Werte aus der Datei */
        lcfg_defaults(&s1);
        if (lcfg_load_file(lcf, &s1) != 0 || s1.width != 1920 || s1.height != 1080) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "nach Neustart %dx%d", s1.width, s1.height);
        }
    }
    if (!why[0]) { /* Gegenprobe: SB-Tab ohne Spieldaten lehnt weiter ab, Datei bleibt */
        g_game = LGAME_SB;
        s0.width = 1600;
        s0.height = 900;
        ui_from_settings(&s0);
        if (do_save(0) == 0) {
            strcpy_s(why, sizeof why, "SB-Tab ohne Spieldaten gespeichert");
        } else {
            gt_read(lcf, buf, sizeof buf);
            if (!strstr(buf, "width=1920\n")) strcpy_s(why, sizeof why, "SB-Tab: Datei trotz Ablehnung geaendert");
        }
    }
    u8_DeleteFile(cfg);
    u8_DeleteFile(lcf);
    g_ucfg_test[0] = '\0';
    g_game = keep_game;
    g_s = keep_s;
    g_disk_base = keep_base;
    g_disk_base_ok = keep_ok;
    ui_from_settings(&g_s);
    if (!why[0]) {
        GT_OK("65b W375 LOC-SAVE #50 (LoC-Tab: Grafik ohne SB-Abbild gespeichert, W402: in user_loc.cfg ohne SB-Pfade, user.cfg unveraendert, nach Neustart da; SB-Tab lehnt weiter ab)");
    } else {
        GT_FAIL("65b W375 LOC-SAVE #50", "%s", why);
    }
}

/* --- 66 (W278 ORIG-FLAG): Hinweis an/aus auf Spielen (SB + LoC), "Back to
 * original" setzt nur Grafik zurueck und speichert, Einmal-Start (Trockenlauf)
 * schreibt die Original-Werte nur in die --set-Kette, nie in user.cfg.
 * user.cfg = Temp-Datei (g_ucfg_test). Community ohne Spieldaten: Startweg
 * gesperrt -> Knoepfe unsichtbar (Einrichtungskarte), Trockenlauf entfaellt. --- */
static int orig_file_has(const char *path, const char *line)
{
    FILE *f = NULL;
    char ln[600];
    size_t n = strlen(line);
    int hit = 0;
    if (u8_fopen_s(&f, path, "r") != 0 || !f) {
        return 0;
    }
    while (fgets(ln, sizeof ln, f)) {
        if (strncmp(ln, line, n) == 0 && (ln[n] == '\r' || ln[n] == '\n' || ln[n] == '\0')) {
            hit = 1;
        }
    }
    fclose(f);
    return hit;
}

void orig_gui_selftest(int *pass, int *fail)
{
    char why[300] = "", tmp[1024], cfg[1200], keep[16];
    launcher_settings_t s, r;
    const lr_env_t *fix = g_env_fix;
    int setup, rc; /* W392: rc = Einmal-Start (Trockenlauf) */
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw278_gui_user.cfg", tmp);
    {
        FILE *f = NULL;
        if (u8_fopen_s(&f, cfg, "w") == 0 && f) {
            fputs("[render]\nedge_mode=2\nedge_fade_width=10\nworld_sharpen=60\ncockpit_hide=1\n"
                  "interp_fps=60\nrt_scale=2\n[input]\naim_assist=1\n# fremde Zeile\n", f);
            fclose(f);
        }
    }
    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, cfg);
    strcpy_s(keep, sizeof keep, sb_lang_code());
    sb_lang_set("en");
    ui_apply_language();
    g_env_fix = NULL;
    g_game = LGAME_SB;
    loctab_nav_layout();
    /* 1: alte cfg mit Breitbild -> Hinweis + Knoepfe */
    lcfg_defaults(&s);
    lcfg_load_file(cfg, &s);
    g_s = s;
    ui_from_settings(&s);
    show_page(P_PLAY);
    ui_apply_rules();
    pump();
    setup = play_setup_shown();
    if (!(g_orig_mask & (1u << SB_OFG_WIDE)) || !(g_orig_mask & (1u << SB_OFG_COCKPIT)) ||
        !(g_orig_mask & (1u << SB_OFG_POST))) {
        _snprintf_s(why, sizeof why, _TRUNCATE, "Maske 0x%X ohne Breitbild/Cockpit/Nachbearbeitung", g_orig_mask);
    }
    loctab_vis(ID_ORIG_RESET, !setup, "Back to original (SB)", why, sizeof why);
    loctab_vis(ID_ORIG_ONCE, !setup, "Einmal-Start (SB)", why, sizeof why);
    loctab_vis(ID_ORIG_RESET_LOC, 0, "Back to original (LoC) im SB-Bereich", why, sizeof why);
    /* 2: Einmal-Start (Trockenlauf): Kette mit Original-Werten, user.cfg behaelt Breitbild */
    if (!why[0] && !launch_blocked()) {
        g_start_args[0] = '\0';
        g_loc_rb_on = g_loc_own_pred = g_loc_lowd = 0; /* W392: Nicht-Standard darf nie in den SB-Start */
        g_loc_smooth_opp = 2;
        rc = orig_once_start(1);
        g_loc_rb_on = g_loc_own_pred = g_loc_lowd = 1;
        g_loc_smooth_opp = 1;
        if (rc != 0) {
            strcpy_s(why, sizeof why, "Einmal-Start (Trockenlauf) gescheitert");
        } else if (!strstr(g_start_args, "edge_mode=1;") || !strstr(g_start_args, "cockpit_hide=0;") ||
                   !strstr(g_start_args, "world_sharpen=0;") || !strstr(g_start_args, "pack_list=;") ||
                   !strstr(g_start_args, "orig_once=1") || strstr(g_start_args, "interp_fps") ||
                   strstr(g_start_args, "loc_smooth_opp") || /* W387: nie im SB-Start */
                   strstr(g_start_args, "loc_rb_input") || strstr(g_start_args, "loc_own_predict") ||
                   strstr(g_start_args, "loc_rb_opts")) { /* W392 */
            _snprintf_s(why, sizeof why, _TRUNCATE, "Kette falsch: %.180s", g_start_args);
        } else if (!orig_file_has(cfg, "edge_mode=2") || orig_file_has(cfg, "edge_mode=1") ||
                   !orig_file_has(cfg, "cockpit_hide=1") || orig_file_has(cfg, "orig_once=1") ||
                   !orig_file_has(cfg, "# fremde Zeile")) {
            strcpy_s(why, sizeof why, "Einmal-Start hat user.cfg veraendert");
        } else if (strlen(g_start_args) > 1400) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Kette zu lang (%u)", (unsigned)strlen(g_start_args));
        }
    }
    /* 3: LoC-Bereich zeigt den Hinweis ebenfalls (ohne Einmal-Start) */
    if (!why[0]) {
        g_game = LGAME_LOC;
        loctab_nav_layout();
        show_page(P_PLAY);
        ui_apply_rules();
        pump();
        loctab_vis(ID_ORIG_RESET_LOC, 1, "Back to original (LoC)", why, sizeof why);
        loctab_vis(ID_ORIG_ONCE, 0, "Einmal-Start im LoC-Bereich", why, sizeof why);
        g_game = LGAME_SB;
        loctab_nav_layout();
        show_page(P_PLAY);
    }
    /* 4: Back to original: nur Grafik, gespeichert, Hinweis weg */
    if (!why[0]) {
        on_command(ID_ORIG_RESET, BN_CLICKED);
        pump();
        r = g_s;
        ui_to_settings(&r);
        if (g_orig_mask != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "nach Back to original Maske 0x%X", g_orig_mask);
        } else if (r.edge_mode != 1 || r.cockpit_hide != 0 || r.shx[LSHX_SHARP] != 0) {
            strcpy_s(why, sizeof why, "Grafik nicht zurueckgesetzt");
        } else if (r.interp_fps != 60 || r.rt_scale != 2 || r.aim_assist != 1) {
            strcpy_s(why, sizeof why, "Bildrate/Aufloesung/Steuerung veraendert");
        } else if (!setup && (!orig_file_has(cfg, "edge_mode=1") || !orig_file_has(cfg, "cockpit_hide=0") ||
                              !orig_file_has(cfg, "interp_fps=60") || !orig_file_has(cfg, "aim_assist=1") ||
                              !orig_file_has(cfg, "# fremde Zeile"))) {
            strcpy_s(why, sizeof why, "user.cfg nach Back to original falsch");
        } else if (!g_undo_on) {
            strcpy_s(why, sizeof why, "kein Rueckgaengig");
        }
        loctab_vis(ID_ORIG_RESET, 0, "Back to original nach dem Zuruecksetzen", why, sizeof why);
        loctab_vis(ID_ORIG_ONCE, 0, "Einmal-Start nach dem Zuruecksetzen", why, sizeof why);
    }
    /* aufraeumen */
    u8_DeleteFile(cfg);
    g_ucfg_test[0] = '\0';
    g_env_fix = fix;
    sb_lang_set(keep);
    ui_apply_language();
    lcfg_defaults(&s);
    g_s = s;
    ui_from_settings(&s);
    show_page(P_PLAY);
    if (!why[0]) {
        GT_OK(setup ? "66 W278 ORIG-FLAG (Hinweis SB/LoC, Back to original nur Grafik; ohne Spieldaten: Knoepfe verborgen, kein Speichern/Trockenlauf)"
                    : "66 W278 ORIG-FLAG (Hinweis SB/LoC, Einmal-Start nur per --set (user.cfg unveraendert), Back to original nur Grafik + gespeichert)");
    } else {
        GT_FAIL("66 W278 ORIG-FLAG", "%s", why);
    }
}

/* --- 67 (W281 LAUNCHER-SIZE): Layout 0/a/b umschalten (Vorschau-Kombi),
 * Fenster passt auf eine kleine Arbeitsflaeche, keine ueberlappenden
 * sichtbaren Elemente je Seite (echte Fensterrechtecke), Elemente im
 * Fenster, b auch bei Mindest- und grosser Flaeche, UI-cfg-Roundtrip. */
static int lay_gt_page_check(char *why, size_t cap, const char *tag)
{
    RECT cr, r[UI_MAX];
    int id[UI_MAX], n = 0, i, j, p;
    GetClientRect(g_hwnd, &cr);
    for (p = 0; p <= P_HUB_LAST; p++) { /* W397: + Launcher-Einstellungen; W470: + steelnet */
        if (p == P_ALPHA && !g_alpha_tools) {
            continue;
        }
        /* W293/W392: bei LoC zaehlt auch die Seite Netzwerk (Packs-Platz) */
        show_page(p);
        pump();
        n = 0;
        for (i = 0; i < g_ui_n && n < UI_MAX; i++) {
            uictl_t *u = &g_ui[i];
            RECT q;
            if (u->parent != g_hwnd || !IsWindowVisible(u->h)) {
                continue;
            }
            GetWindowRect(u->h, &q);
            MapWindowPoints(NULL, g_hwnd, (POINT *)&q, 2);
            if (q.right <= q.left || q.bottom <= q.top) {
                continue;
            }
            if (q.left < 0 || q.top < 0 || q.right > cr.right || q.bottom > cr.bottom) {
                _snprintf_s(why, cap, _TRUNCATE, "%s Seite %s: Element %d ausserhalb (%ld,%ld-%ld,%ld)",
                            tag, gui_page_key(p), u->id, q.left, q.top, q.right, q.bottom);
                return 0;
            }
            r[n] = q;
            id[n++] = u->id;
        }
        if (n < 4) { /* Pruefung greift nur mit sichtbaren Elementen */
            _snprintf_s(why, cap, _TRUNCATE, "%s Seite %s: nur %d sichtbare Elemente", tag,
                        gui_page_key(p), n);
            return 0;
        }
        for (i = 0; i < n; i++) {
            for (j = 0; j < i; j++) {
                RECT x;
                if (IntersectRect(&x, &r[i], &r[j])) {
                    _snprintf_s(why, cap, _TRUNCATE, "%s Seite %s: Elemente %d/%d ueberlappen",
                                tag, gui_page_key(p), id[i], id[j]);
                    return 0;
                }
            }
        }
    }
    return 1;
}

/* --- 68 (W287 EXP-ICON): Experimentell-Symbol (Kolben) statt Plakette nur an
 * experimentellen Einstellungen (Karten, Presets) mit Tooltip und
 * zugaenglichem Namen; Warn-Symbol an Hinweis (SB/LoC) und Navigation nur bei
 * aktiven experimentellen Einstellungen; keine sichtbare Textplakette;
 * Groessen-Kombi 3 Eintraege, alte Wahl 0/b -> A (Groesse bleibt). */
static void xicon_gt_vis(int id, int want, const char *what, char *why, size_t cap)
{
    HWND h = ctl(id);
    int has = h && IsWindowVisible(h);
    if (!why[0] && has != want) {
        _snprintf_s(why, cap, _TRUNCATE, "%s: %s", what, want ? "unsichtbar" : "sichtbar");
    }
}

/* Sichtbarer Text eines Elements (Navigation/Presets: wie gezeichnet). */
static void xicon_gt_shown(uictl_t *u, char *out, size_t cap)
{
    wchar_t w[600], *nl;
    int n = GetWindowTextW(u->h, w, 600), n1;
    nl = wcschr(w, L'\n');
    n1 = nl ? (int)(nl - w) : n;
    if (u->kind == K_NAV) {
        w[n1] = 0;
    } else if (u->kind == K_RADIO) {
        int nt = xexp_title_len(u->id, w, n1);
        if (nt != n1) {
            memmove(w + nt, w + n1, (size_t)(n - n1 + 1) * sizeof(wchar_t));
        }
    } else if (u->kind == K_ICON) {
        w[0] = 0; /* gezeichnet wird nur das Symbol */
    }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
}

static void xicon_gt_noplaque(const char *tag, char *why, size_t cap)
{
    static const char *const kBad[] = { "EXPERIMENTAL", "EXPERIMENTELL", "(experimental)", "(experimentell)" };
    int i, k;
    for (i = 0; i < g_ui_n && !why[0]; i++) {
        uictl_t *u = &g_ui[i];
        char t[1200];
        if (u->parent != g_hwnd || !IsWindowVisible(u->h)) {
            continue;
        }
        xicon_gt_shown(u, t, sizeof t);
        for (k = 0; k < 4; k++) {
            if (strstr(t, kBad[k])) {
                _snprintf_s(why, cap, _TRUNCATE, "%s: Element %d zeigt '%s'", tag, u->id, kBad[k]);
                break;
            }
        }
    }
}

void xicon_gui_selftest(int *pass, int *fail)
{
    char why[300] = "", t[800], keep[16], tmp[1024], dir[1100];
    launcher_settings_t s0 = g_s, s;
    const lr_env_t *fix = g_env_fix;
    int setup, i, p;
    strcpy_s(keep, sizeof keep, sb_lang_code());
    g_env_fix = NULL;
    g_game = LGAME_SB;
    loctab_nav_layout();
    for (i = 0; i < 2 && !why[0]; i++) { /* EN, DE */
        const char *lc = i ? "de" : "en";
        char tag[24];
        sb_lang_set(lc);
        ui_apply_language();
        /* 1: Original -> kein Warn-Symbol, keine Warnung in der Navigation */
        lcfg_defaults(&s);
        g_s = s;
        ui_from_settings(&s);
        show_page(P_PLAY);
        ui_apply_rules();
        pump();
        if (g_orig_mask != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "%s Original: Maske 0x%X", lc, g_orig_mask);
        }
        xicon_gt_vis(ID_XWARN_SB, 0, "Warn-Symbol (SB) bei Original", why, sizeof why);
        xicon_gt_vis(ID_XWARN_LOC, 0, "Warn-Symbol (LoC) bei Original", why, sizeof why);
        for (p = 0; p < P_N && !why[0]; p++) {
            xtip_text(ID_NAV0 + p, t, sizeof t);
            ui_gettext(ctl(ID_NAV0 + p), tmp, sizeof tmp);
            if (xwarn_page_mask(p) || t[0] || strchr(tmp, '\n')) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "%s Original: Navigation %s warnt", lc, kPageKey[p]);
            }
        }
        /* Kolben nur an experimentellen Karten/Presets, mit Tooltip = Name */
        show_page(P_GFX);
        pump();
        xicon_gt_vis(ID_XEXP_IMAGE, 1, "Symbol Bild", why, sizeof why);
        xicon_gt_vis(ID_XEXP_WORLD, 1, "Symbol Welt", why, sizeof why);
        xicon_gt_vis(ID_XEXP_HUD, 1, "Symbol HUD", why, sizeof why);
        xicon_gt_vis(ID_XEXP_DISP, 0, "Symbol Anzeige auf Grafik", why, sizeof why);
        xicon_gt_vis(ID_XEXP_PACKS, 0, "Symbol Packs auf Grafik", why, sizeof why);
        for (p = 0; p < LGP_CUSTOM && !why[0]; p++) { /* Original/Performance ohne, dezent/stark/Klare Sicht mit */
            int want = p == LGP_SUBTLE || p == LGP_STRONG || p == LGP_CLEAR ||
                       ((p == LGP_ENHANCED || p == LGP_VIVID) && !lcfg_gfx_loc()); /* W465: Enhanced nur in LoC ohne Kolben, W478: Vivid ebenso */
            char sfx[80];
            wchar_t w[600];
            int n;
            xtip_text(ID_GP0 + p, t, sizeof t);
            ui_gettext(ctl(ID_GP0 + p), tmp, sizeof tmp);
            n = GetWindowTextW(ctl(ID_GP0 + p), w, 600);
            xexp_sfx(sfx, sizeof sfx);
            if (xexp_preset(p) != want || (t[0] != 0) != want || (strstr(tmp, sfx) != NULL) != want) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "%s Preset %d: Symbol/Tooltip/Name falsch", lc, p);
            } else if (want && (!strstr(t, LT("gui.exp_tip")) || xexp_title_len(ID_GP0 + p, w,
                                (int)(wcschr(w, L'\n') ? wcschr(w, L'\n') - w : n)) ==
                                (int)(wcschr(w, L'\n') ? wcschr(w, L'\n') - w : n))) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "%s Preset %d: Suffix sichtbar oder Tooltip ohne Beschreibung", lc, p);
            }
        }
        for (p = ID_XEXP_FIRST; p <= ID_XEXP_LAST && !why[0]; p++) {
            xtip_text(p, t, sizeof t);
            ui_gettext(ctl(p), tmp, sizeof tmp);
            if (!strstr(t, LT("gui.exp_tip")) || strcmp(tmp, t) != 0) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "%s Symbol %d: Tooltip/Name '%.60s'", lc, p, tmp);
            }
        }
        _snprintf_s(tag, sizeof tag, _TRUNCATE, "%s Grafik", lc);
        xicon_gt_noplaque(tag, why, sizeof why);
        show_page(P_DISPLAY);
        pump();
        xicon_gt_vis(ID_XEXP_DISP, 1, "Symbol Anzeige", why, sizeof why);
        xicon_gt_vis(ID_XEXP_IMAGE, 0, "Symbol Bild auf Anzeige", why, sizeof why);
        _snprintf_s(tag, sizeof tag, _TRUNCATE, "%s Anzeige", lc);
        xicon_gt_noplaque(tag, why, sizeof why);
        show_page(P_PACKS);
        pump();
        xicon_gt_vis(ID_XEXP_PACKS, 1, "Symbol Packs", why, sizeof why);
        /* 2: Breitbild + Cockpit aus -> Warn-Symbol am Hinweis, Navigation Grafik + Anzeige */
        lcfg_defaults(&s);
        lcfg_apply_set(&s, "edge_mode=2;cockpit_hide=1");
        g_s = s;
        ui_from_settings(&s);
        show_page(P_PLAY);
        ui_apply_rules();
        pump();
        setup = play_setup_shown();
        if (!why[0] && g_orig_mask != ((1u << SB_OFG_WIDE) | (1u << SB_OFG_COCKPIT))) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "%s Breitbild+Cockpit: Maske 0x%X", lc, g_orig_mask);
        }
        xicon_gt_vis(ID_XWARN_SB, !setup, "Warn-Symbol (SB) bei Experimentellem", why, sizeof why);
        xicon_gt_vis(ID_XWARN_LOC, 0, "Warn-Symbol (LoC) im SB-Bereich", why, sizeof why);
        xtip_text(ID_XWARN_SB, t, sizeof t);
        ui_gettext(ctl(ID_XWARN_SB), tmp, sizeof tmp);
        if (!why[0] && (!strstr(t, LT("ovl.of_g_wide")) || !strstr(t, LT("ovl.of_g_cockpit")) ||
                        !strchr(t, '\n') || strcmp(tmp, t) != 0)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "%s Warn-Tooltip '%.80s'", lc, t);
        }
        for (p = 0; p < P_N && !why[0]; p++) {
            int want = p == P_GFX || p == P_DISPLAY;
            xtip_text(ID_NAV0 + p, t, sizeof t);
            ui_gettext(ctl(ID_NAV0 + p), tmp, sizeof tmp);
            if ((xwarn_page_mask(p) != 0) != want || (t[0] != 0) != want || (strchr(tmp, '\n') != NULL) != want) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "%s Breitbild+Cockpit: Navigation %s warnt=%d", lc,
                            kPageKey[p], !want);
            }
        }
        if (!why[0]) {
            xtip_text(ID_NAV0 + P_DISPLAY, t, sizeof t);
            if (!strstr(t, LT("ovl.of_g_wide")) || strstr(t, LT("ovl.of_g_cockpit"))) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "%s Navigation Anzeige: '%.80s'", lc, t);
            }
        }
        _snprintf_s(tag, sizeof tag, _TRUNCATE, "%s Spielen", lc);
        xicon_gt_noplaque(tag, why, sizeof why);
        /* LoC-Bereich: Warn-Symbol an der Hinweiskarte */
        if (!why[0]) {
            g_game = LGAME_LOC;
            loctab_nav_layout();
            show_page(P_PLAY);
            ui_apply_rules();
            pump();
            xicon_gt_vis(ID_XWARN_LOC, 1, "Warn-Symbol (LoC)", why, sizeof why);
            xicon_gt_vis(ID_XWARN_SB, 0, "Warn-Symbol (SB) im LoC-Bereich", why, sizeof why);
            g_game = LGAME_SB;
            loctab_nav_layout();
            show_page(P_PLAY);
        }
        /* 3: zurueck auf Original -> alles weg */
        lcfg_defaults(&s);
        g_s = s;
        ui_from_settings(&s);
        ui_apply_rules();
        pump();
        xicon_gt_vis(ID_XWARN_SB, 0, "Warn-Symbol nach Original", why, sizeof why);
        ui_gettext(ctl(ID_NAV0 + P_GFX), tmp, sizeof tmp);
        if (!why[0] && strchr(tmp, '\n')) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "%s nach Original: Navigation warnt", lc);
        }
    }
    /* 4: Groessen-Kombi 3 Eintraege (Groesse von A), alte Wahl 0/b -> A */
    if (!why[0]) {
        HWND c = ctl(ID_LAYOUT);
        char o_lay[4], o_sz[12], o_ovr[4], o_ovr_sz[12], o_dir[1024];
        int id0 = g_lay.id, sz0 = g_lay.size, cid0 = g_lay_cfg_id, csz0 = g_lay_cfg_size, shot0 = g_o.shot;
        static const struct { const char *lay, *sz; int want_sz; } kOld[] = {
            { "b", "compact", LLAY_SZ_COMPACT }, { "0", "large", LLAY_SZ_LARGE }, { "a", "normal", LLAY_SZ_NORMAL },
        };
        g_o.shot = 2;
        if (!c || ComboBox_GetCount(c) != LLAY_SZ_N) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Groessen-Kombi: %d Eintraege", c ? ComboBox_GetCount(c) : -1);
        } else {
            wchar_t wt[80] = L"";
            SendMessageW(c, CB_GETLBTEXT, 0, (LPARAM)wt);
            WideCharToMultiByte(CP_UTF8, 0, wt, -1, t, (int)sizeof t, NULL, NULL);
            if (strcmp(t, LT("gui.lay_sz_compact")) != 0) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Groessen-Kombi: erster Eintrag '%s'", t);
            }
        }
        if (!why[0]) {
            lay_switch(LLAY_A, LLAY_SZ_NORMAL);
            ui_gettext(ctl(ID_LAYOUT_LBL), tmp, sizeof tmp);
            show_page(P_LSET); /* W397: Groesse liegt auf den Launcher-Einstellungen */
            pump();
            xicon_gt_vis(ID_LAYOUT, 1, "Groessen-Kombi in A", why, sizeof why);
            xicon_gt_vis(ID_LAYOUT_LBL, 1, "Beschriftung Groesse in A", why, sizeof why);
            show_page(P_PLAY);
            if (!why[0] && (strcmp(tmp, LT("gui.lay_size")) != 0 || ComboBox_GetCurSel(c) != LLAY_SZ_NORMAL)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Groessen-Kombi: Beschriftung '%s' / Wahl %d", tmp,
                            ComboBox_GetCurSel(c));
            }
        }
        strcpy_s(o_lay, sizeof o_lay, g_o.layout);
        strcpy_s(o_sz, sizeof o_sz, g_o.ui_size);
        strcpy_s(o_ovr, sizeof o_ovr, g_lay_ovr);
        strcpy_s(o_ovr_sz, sizeof o_ovr_sz, g_lay_ovr_size);
        strcpy_s(o_dir, sizeof o_dir, g_client_dir);
        g_o.layout[0] = g_o.ui_size[0] = g_lay_ovr[0] = g_lay_ovr_size[0] = '\0';
        u8_GetTempPath(tmp, sizeof tmp);
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%sw287_uicfg_%lu", tmp, GetCurrentProcessId());
        lcfg_mkdir_p(dir);
        strcpy_s(g_client_dir, sizeof g_client_dir, dir);
        for (i = 0; i < 3 && !why[0]; i++) {
            char pth[1200];
            FILE *f = NULL;
            int id, sz;
            ui_cfg_path(dir, pth, sizeof pth);
            if (u8_fopen_s(&f, pth, "w") == 0 && f) {
                fprintf(f, "bg_image=1\nlauncher_layout=%s\nui_size=%s\n", kOld[i].lay, kOld[i].sz);
                fclose(f);
            }
            id = lay_cfg_id();
            sz = lay_cfg_size();
            if (id != LLAY_A || sz != kOld[i].want_sz || g_lay_cfg_id != LLAY_A) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "alte Wahl %s/%s -> Layout %d Groesse %d (cfg %d)", kOld[i].lay,
                            kOld[i].sz, id, sz, g_lay_cfg_id);
            }
            u8_DeleteFile(pth);
        }
        { wchar_t wd[1100]; u8_to_w(dir, wd, 1100); RemoveDirectoryW(wd); }
        strcpy_s(g_client_dir, sizeof g_client_dir, o_dir);
        strcpy_s(g_o.layout, sizeof g_o.layout, o_lay);
        strcpy_s(g_o.ui_size, sizeof g_o.ui_size, o_sz);
        strcpy_s(g_lay_ovr, sizeof g_lay_ovr, o_ovr);
        strcpy_s(g_lay_ovr_size, sizeof g_lay_ovr_size, o_ovr_sz);
        g_lay_cfg_id = cid0;
        g_lay_cfg_size = csz0;
        lay_switch(id0, sz0);
        g_o.shot = shot0;
    }
    /* aufraeumen */
    g_env_fix = fix;
    sb_lang_set(keep);
    ui_apply_language();
    g_s = s0;
    ui_from_settings(&s0);
    ui_apply_rules();
    show_page(P_PLAY);
    if (!why[0]) {
        GT_OK("68 W287 EXP-ICON (Kolben nur an experimentellen Karten/Presets mit Tooltip + Name, Warn-Symbol an "
              "Hinweis SB/LoC und Navigation nur bei Aktivem, keine Textplakette, EN/DE, Groessen-Kombi 3, alte Wahl 0/b -> A)");
    } else {
        GT_FAIL("68 W287 EXP-ICON", "%s", why);
    }
}

void lay_gui_selftest(int *pass, int *fail)
{
    char why[300] = "", tmp[1024], dir[1100];
    int shot0 = g_o.shot, id0 = g_lay.id, sz0 = g_lay.size, cid0 = g_lay_cfg_id,
        csz0 = g_lay_cfg_size, ww0 = g_lay_work_w, wh0 = g_lay_work_h, k;
    static const int kSet[][2] = { { LLAY_0, LLAY_SZ_NORMAL }, { LLAY_A, LLAY_SZ_COMPACT },
                                   { LLAY_A, LLAY_SZ_NORMAL },  { LLAY_A, LLAY_SZ_LARGE },
                                   { LLAY_B, LLAY_SZ_NORMAL },  { LLAY_B, LLAY_SZ_LARGE } };
    g_o.shot = 2; /* nichts speichern, Einpassen nur mit --work */
    g_lay_work_w = g_lay_work_h = 0;
    /* Ueberlappung/innen je Layout und Seite */
    for (k = 0; k < (int)(sizeof kSet / sizeof kSet[0]) && !why[0]; k++) {
        char tag[32];
        _snprintf_s(tag, sizeof tag, _TRUNCATE, "%s/%s", llay_name(kSet[k][0]),
                    llay_size_name(kSet[k][1]));
        lay_switch(kSet[k][0], kSet[k][1]);
        if (g_lay.id != kSet[k][0] || g_lay.size != kSet[k][1]) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "%s: Layout nicht gewechselt", tag);
            break;
        }
        if (ui_visible_on(ui_find(ID_LAYOUT), P_LSET) != (kSet[k][0] != LLAY_0)) { /* W397: unter dem Zahnrad */
            _snprintf_s(why, sizeof why, _TRUNCATE, "%s: Layout-Kombi sichtbar=%d", tag,
                        ui_visible_on(ui_find(ID_LAYOUT), P_LSET));
            break;
        }
        lay_gt_page_check(why, sizeof why, tag);
    }
    /* b: Mindestflaeche und grosse Flaeche (Ziehen nachgestellt) */
    if (!why[0]) {
        static const int kSz[2][2] = { { 1040, 660 }, { 1700, 1040 } };
        lay_switch(LLAY_B, LLAY_SZ_NORMAL);
        for (k = 0; k < 2 && !why[0]; k++) {
            RECT cr;
            char tag[32];
            SetWindowPos(g_hwnd, NULL, 0, 0, SX(kSz[k][0]), SX(kSz[k][1]),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            pump();
            GetClientRect(g_hwnd, &cr);
            if (g_lay.tw < kSz[k][0] - 1 || g_lay.th < kSz[k][1] - 1) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "b gezogen %dx%d: Flaeche %dx%d",
                            kSz[k][0], kSz[k][1], g_lay.tw, g_lay.th);
                break;
            }
            _snprintf_s(tag, sizeof tag, _TRUNCATE, "b %dx%d", kSz[k][0], kSz[k][1]);
            lay_gt_page_check(why, sizeof why, tag);
        }
    }
    /* Groessen-Kombi (W287): nur Layout A, Index = Groesse (wie der Nutzer) */
    if (!why[0]) {
        HWND c = ctl(ID_LAYOUT);
        ComboBox_SetCurSel(c, LLAY_SZ_LARGE);
        on_command(ID_LAYOUT, CBN_SELCHANGE);
        if (g_lay.id != LLAY_A || g_lay.size != LLAY_SZ_LARGE || ComboBox_GetCount(c) != LLAY_SZ_N) {
            strcpy_s(why, sizeof why, "Kombi: a/gross nicht gewaehlt");
        }
        ComboBox_SetCurSel(c, LLAY_SZ_COMPACT);
        on_command(ID_LAYOUT, CBN_SELCHANGE);
        if (!why[0] && (g_lay.id != LLAY_A || g_lay.size != LLAY_SZ_COMPACT)) {
            strcpy_s(why, sizeof why, "Kombi: a/kompakt nicht gewaehlt");
        }
    }
    /* Kleine Arbeitsflaeche (1366x768, 100 %): Fenster passt (a und b) */
    if (!why[0]) {
        int id;
        g_lay_work_w = 1366;
        g_lay_work_h = 728;
        for (id = LLAY_A; id <= LLAY_B && !why[0]; id++) {
            int sz;
            for (sz = 0; sz < LLAY_SZ_N && !why[0]; sz++) {
                RECT wr;
                lay_switch(id, sz);
                GetWindowRect(g_hwnd, &wr);
                if (wr.right - wr.left > 1366 || wr.bottom - wr.top > 728) {
                    _snprintf_s(why, sizeof why, _TRUNCATE, "%s/%s: Fenster %ldx%ld > 1366x728",
                                llay_name(id), llay_size_name(sz), wr.right - wr.left,
                                wr.bottom - wr.top);
                }
            }
        }
        if (!why[0]) {
            lay_switch(LLAY_A, LLAY_SZ_NORMAL);
            lay_gt_page_check(why, sizeof why, "a 1366x728");
        }
        g_lay_work_w = g_lay_work_h = 0;
    }
    /* launcher_ui.cfg: launcher_layout/ui_size schreiben + lesen (Temp-Ordner) */
    if (!why[0]) {
        char v1[16], v2[16];
        u8_GetTempPath(tmp, sizeof tmp);
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%sw281_uicfg_%lu", tmp, GetCurrentProcessId());
        lcfg_mkdir_p(dir);
        g_lay_cfg_id = LLAY_B;
        g_lay_cfg_size = LLAY_SZ_LARGE;
        ui_cfg_save_bg(dir, 1);
        ui_cfg_load_str(dir, "launcher_layout", v1, sizeof v1);
        ui_cfg_load_str(dir, "ui_size", v2, sizeof v2);
        if (strcmp(v1, "b") != 0 || strcmp(v2, "large") != 0 || ui_cfg_load_bg(dir) != 1) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "UI-cfg: '%s' '%s'", v1, v2);
        }
        g_lay_cfg_id = -1; /* nie gewaehlt: keine Zeilen (Layout 0 byte-gleich) */
        ui_cfg_save_bg(dir, 1);
        ui_cfg_load_str(dir, "launcher_layout", v1, sizeof v1);
        if (!why[0] && v1[0]) {
            strcpy_s(why, sizeof why, "UI-cfg: Layoutzeile ohne Wahl");
        }
        {
            char pth[1200];
            ui_cfg_path(dir, pth, sizeof pth);
            u8_DeleteFile(pth);
            { wchar_t wd[1100]; u8_to_w(dir, wd, 1100); RemoveDirectoryW(wd); }
        }
    }
    /* zurueck */
    g_lay_cfg_id = cid0;
    g_lay_cfg_size = csz0;
    g_lay_work_w = ww0;
    g_lay_work_h = wh0;
    lay_switch(id0, sz0);
    g_o.shot = shot0;
    show_page(P_PLAY);
    if (!why[0]) {
        GT_OK("67 W281 LAUNCHER-SIZE (Layout 0/a/b + Groessen umschalten, keine Ueberlappung je Seite, b gezogen, Vorschau-Kombi, passt auf 1366x768, UI-cfg)");
    } else {
        GT_FAIL("67 W281 LAUNCHER-SIZE", "%s", why);
    }
}

/* W291 ESC-REWORK (Fall 69): "Erweiterte Einstellungen zeigen" blendet nur
 * Feinregler auf Anzeige/Grafik ein und aus (wie "Erweitert" im ESC-Menue),
 * merkt die Wahl in launcher_ui.cfg und aendert keinen Wert. */
void adv_gui_selftest(int *pass, int *fail)
{
    static const int kAdv[] = { ID_GFX_SOFT, ID_REM_LABEL, ID_CPERSP, ID_CPANINI, ID_CWIDTH }; /* W296: Verlauf s. Fall 71 */
    static const int kMain[] = { ID_GFX_LIGHT, ID_GFX_SHADOW, ID_GFX_FX, ID_AAMODE, ID_GFX_TEX, ID_MONFX, ID_CHIDE,
                                 ID_EDGE, ID_IPFPS, ID_RTSCALE };
    char why[300] = "", tmp[1024], dir[1100], cd0[sizeof g_client_dir];
    launcher_settings_t s0, s1;
    int adv0 = g_show_adv, shot0 = g_o.shot, round, k;
    strcpy_s(cd0, sizeof cd0, g_client_dir);
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%sw291_uicfg_%lu", tmp, GetCurrentProcessId());
    lcfg_mkdir_p(dir);
    strcpy_s(g_client_dir, sizeof g_client_dir, dir); /* launcher_ui.cfg nur im Temp-Ordner */
    g_o.shot = 0;
    memset(&s0, 0, sizeof s0);
    memset(&s1, 0, sizeof s1);
    ui_to_settings(&s0);
    for (round = 0; round < 3 && !why[0]; round++) {
        int want = round == 1; /* aus -> an -> aus */
        if (g_show_adv != want) {
            on_command(ID_SHOWADV, BN_CLICKED);
            pump();
        }
        if (g_show_adv != want || ui_checked(ID_SHOWADV) != want) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Schalter %d statt %d", g_show_adv, want);
            break;
        }
        if (round > 0 && ui_cfg_load_key(dir, "show_advanced", -1) != want) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "launcher_ui.cfg show_advanced != %d", want);
            break;
        }
        for (k = 0; k < (int)(sizeof kAdv / sizeof kAdv[0]) && !why[0]; k++) {
            uictl_t *u = ui_find(kAdv[k]);
            if (!u) { continue; }
            show_page(u->page);
            pump();
            if ((IsWindowVisible(u->h) != 0) != want) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Feinregler %d sichtbar=%d (Erweitert %d)", kAdv[k],
                            IsWindowVisible(u->h) != 0, want);
            }
            if (!why[0] && !IsWindowVisible(ctl(ID_SHOWADV))) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Schalter fehlt auf Seite %s", gui_page_key(u->page));
            }
        }
        for (k = 0; k < (int)(sizeof kMain / sizeof kMain[0]) && !why[0]; k++) {
            uictl_t *u = ui_find(kMain[k]);
            if (!u) { continue; }
            show_page(u->page);
            pump();
            if (!IsWindowVisible(u->h)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Hauptregler %d unsichtbar (Erweitert %d)", kMain[k], want);
            }
        }
        if (!why[0]) {
            show_page(P_PLAY);
            pump();
            if (IsWindowVisible(ctl(ID_SHOWADV))) {
                strcpy_s(why, sizeof why, "Schalter auf Spielen sichtbar");
            }
        }
        if (!why[0]) {
            lay_gt_page_check(why, sizeof why, want ? "erweitert" : "einfach");
        }
    }
    ui_to_settings(&s1);
    if (!why[0] && memcmp(&s0, &s1, sizeof s0) != 0) {
        strcpy_s(why, sizeof why, "Werte nach dem Umschalten veraendert");
    }
    if (g_show_adv != adv0) { /* zurueck */
        on_command(ID_SHOWADV, BN_CLICKED);
    }
    {
        char pth[1200];
        wchar_t wd[1100];
        ui_cfg_path(dir, pth, sizeof pth);
        u8_DeleteFile(pth);
        u8_to_w(dir, wd, 1100);
        RemoveDirectoryW(wd);
    }
    strcpy_s(g_client_dir, sizeof g_client_dir, cd0);
    g_o.shot = shot0;
    show_page(P_PLAY);
    if (!why[0]) {
        GT_OK("69 W291 ADVANCED (Schalter nur auf Anzeige/Grafik, Feinregler aus/an/aus, Hauptregler immer, launcher_ui.cfg, "
              "keine Ueberlappung, Werte unveraendert)");
    } else {
        GT_FAIL("69 W291 ADVANCED", "%s", why);
    }
}

/* --- 71 (W296 PRESET-ORIG): Grafik-Seite "Original" ohne Bildrate (Wahl
 * setzt Monitor auto), "Performance"; Anzeige: Verlaufsregler bei "Fade out"
 * auch ohne "Erweitert" sichtbar, bei 4:3 nicht; Vorschau darunter, keine
 * Ueberlappung in Layout A Kompakt/Normal/Gross. Nichts gespeichert. --- */
/* --- 72 (W309 FPS-MONITOR): Bildratenliste der Seite Anzeige mit simulierten
 * Monitoren (60/144/165/240 Hz, zwei Monitore, Alt-cfg 240 auf 144 Hz):
 * Eintraege, Kennzeichen, Auswahl bleibt, Speichern ohne Datenverlust, R10,
 * Statuszeile. Nichts gespeichert. --- */
static void ip_item_text(int i, char *o, size_t cap)
{
    wchar_t w[128];
    o[0] = 0;
    if (SendMessageW(ctl(ID_IPFPS), CB_GETLBTEXTLEN, (WPARAM)i, 0) < 120 &&
        SendMessageW(ctl(ID_IPFPS), CB_GETLBTEXT, (WPARAM)i, (LPARAM)w) != CB_ERR) {
        w_to_u8(w, o, (int)cap);
    }
}

/* --- 73 (W323 LOC-INTERP): Bildrate im LoC-Profil - "Original" heisst dort "20 (Original)" (Missionstakt
 * 20 Hz), in SB "30 (Original)"; die gewaehlte Rate bleibt beim Spielwechsel. Nichts gespeichert. --- */
__declspec(noinline) void locip_gui_selftest(int *pass, int *fail)
{
    char why[200] = "", t[200];
    int g0 = g_game, keep0 = g_ip_keep, pal = ctl(ID_PAL50) && ui_checked(ID_PAL50);
    HWND c = ctl(ID_IPFPS);
    if (!c) {
        GT_FAIL("73 W323 LOC-INTERP", "%s", "ID_IPFPS fehlt");
        return;
    }
    ip_set(120);
    g_game = LGAME_LOC;
    ip_fill(c);
    ip_item_text(ip_sel_of(0), t, sizeof t);
    if (strcmp(t, LT("gui.ip_20")) != 0) { _snprintf_s(why, sizeof why, _TRUNCATE, "LoC: Original als '%s'", t); }
    if (!why[0] && ip_val(ComboBox_GetCurSel(c), -2) != 120) {
        _snprintf_s(why, sizeof why, _TRUNCATE, "LoC: Wahl %d statt 120", ip_val(ComboBox_GetCurSel(c), -2));
    }
    g_game = LGAME_SB;
    ip_fill(c);
    ip_item_text(ip_sel_of(0), t, sizeof t);
    if (!why[0] && strcmp(t, LT(pal ? "gui.ip_25" : "gui.ip_30")) != 0) {
        _snprintf_s(why, sizeof why, _TRUNCATE, "SB: Original als '%s'", t);
    }
    if (!why[0] && ip_val(ComboBox_GetCurSel(c), -2) != 120) {
        _snprintf_s(why, sizeof why, _TRUNCATE, "SB: Wahl %d statt 120", ip_val(ComboBox_GetCurSel(c), -2));
    }
    g_game = g0;
    g_ip_keep = keep0;
    ip_set(keep0);
    if (!why[0]) {
        GT_OK("73 W323 LOC-INTERP (Bildrate: LoC 20 (Original), SB 30/25 (Original), Wahl bleibt beim Spielwechsel)");
    } else {
        GT_FAIL("73 W323 LOC-INTERP", "%s", why);
    }
}

/* --- 74 (W326 LOC-WIDE): Bildformat im LoC-Profil (LoC > Erweitert):
 * Eintraege = angebotene Modi (3, ohne LOC_WIDE_720P 2), Texte EN/DE, Wahl
 * bleibt beim Sprachwechsel; Rundweg 0/1/2 ueber die Auswahl in die user.cfg-
 * Testdatei (loc_widescreen, fremde Zeilen bleiben, keine Doppelten); fehlt
 * = 0, ungueltig = 0; Kolben (experimentell) mit Tooltip, keine Textplakette;
 * nur im Bereich LoC auf Erweitert sichtbar, nichts ueberlappt. --- */
static int locwide_cfg_count(const char *buf, const char *line)
{
    int n = 0;
    const char *p = buf;
    size_t l = strlen(line);
    while ((p = strstr(p, line)) != NULL) {
        if (p == buf || p[-1] == '\n') {
            n++;
        }
        p += l;
    }
    return n;
}

/* W328 STEELNET-ACCOUNT (Fall 75/76): Karte/Ansicht je Zustand (Fixture), Sichtbarkeit,
 * Wiederherstellungscode, Eingabepruefung ohne Netz, Startweg (Pilot + SB_LOC_AUTH nur im
 * Kindblock); 76 nur mit SB_STEELNET_API (lokaler Dienst): Anlegen/Status/Loeschen ueber den
 * UI-Thread-Weg (Auftrag + WM_APP_ACC). */
static int acc_gt_vis(int id)
{
    HWND h = ctl(id);
    return h && IsWindowVisible(h);
}

static int acc_gt_wait(int ms)
{
    ULONGLONG end = GetTickCount64() + (ULONGLONG)ms;
    while (g_acc_busy && GetTickCount64() < end) {
        pump();
        Sleep(20);
    }
    pump();
    return !g_acc_busy;
}

__declspec(noinline) void acc_gui_selftest(int *pass, int *fail)
{
    char why[300] = "", keep[16], set[400], t[200];
    int g0 = g_game, p0 = g_page;
    lacc_store_t a0 = g_acc;
    wchar_t *env;
    if (!ctl(ID_ACC_NAME) || !ctl(ID_ACC_V_PASS)) {
        GT_FAIL("75 W328 STEELNET-ACCOUNT", "%s", "Elemente fehlen");
        return;
    }
    strcpy_s(keep, sizeof keep, sb_lang_code());
    sb_lang_set("en");
    ui_apply_language();
    g_game = LGAME_LOC;
    loctab_nav_layout();
    /* a: Dienst nicht eingerichtet -> alte Karte, keine Konto-Elemente */
    acc_fixture(0, 0, 0, 0, 0);
    show_page(P_LNET);
    pump();
    if (acc_gt_vis(ID_ACC_NAME) || acc_gt_vis(ID_ACC_MANAGE) || acc_gt_vis(ID_ACC_V_LOGIN) || !acc_gt_vis(ID_LNET_RB))
        strcpy_s(why, sizeof why, "ohne Dienst: Konto-Elemente sichtbar");
    {
        COLORREF c = 0;
        if (!why[0] && acc_sum_row(t, sizeof t, &c)) strcpy_s(why, sizeof why, "ohne Dienst: Zeile Online belegt");
    }
    /* b: eingerichtet, kein Pilot -> Name/Anlegen/Pruefen/Anmelden */
    acc_fixture(1, 0, 0, 0, 0);
    acc_refresh();
    pump();
    if (!why[0] && (!acc_gt_vis(ID_ACC_NAME) || !acc_gt_vis(ID_ACC_CREATE) || acc_gt_vis(ID_ACC_X_LOGIN) ||
                    !acc_gt_vis(ID_ACC_SIGNIN) || acc_gt_vis(ID_ACC_DISCORD) || acc_gt_vis(ID_ACC_V_LOGIN) ||
                    !acc_gt_vis(ID_LNET_RB)))
        strcpy_s(why, sizeof why, "kein Pilot: Karte falsch");
    {
        COLORREF c = 0;
        if (!why[0] && (!acc_sum_row(t, sizeof t, &c) || strcmp(t, LT("gui.acc_sum_none")) != 0 || c != C_WARN))
            strcpy_s(why, sizeof why, "kein Pilot: Zeile Online");
    }
    /* ungueltiger Name: Meldung, kein Auftrag */
    ui_settext(ctl(ID_ACC_NAME), "Guest-Ace");
    on_command(ID_ACC_CREATE, BN_CLICKED);
    if (!why[0] && (g_acc_busy || strcmp(g_status, LT("gui.acc_err_name")) != 0 || g_acc_fmsg_id != ID_ACC_NAME))
        strcpy_s(why, sizeof why, "Guest-Name nicht abgewiesen (Status + Meldung unter dem Feld)");
    /* c: Pilot ungesichert -> Discord + Verwalten, Hinweis in der Zeile */
    acc_fixture(1, 1, 0, 0, 0);
    acc_refresh();
    pump();
    if (!why[0] && (acc_gt_vis(ID_ACC_NAME) || !acc_gt_vis(ID_ACC_DISCORD) || !acc_gt_vis(ID_ACC_MANAGE) ||
                    !IsWindowEnabled(ctl(ID_ACC_DISCORD))))
        strcpy_s(why, sizeof why, "Pilot: Karte falsch");
    {
        COLORREF c = 0;
        char want[200];
        _snprintf_s(want, sizeof want, _TRUNCATE, LT("gui.acc_sum_unsec"), "Ace Pilot");
        if (!why[0] && (!acc_sum_row(t, sizeof t, &c) || strcmp(t, want) != 0 || c != C_WARN))
            _snprintf_s(why, sizeof why, _TRUNCATE, "ungesichert: Zeile '%s'", t);
    }
    /* d: Verwaltungsansicht deckt die Seite, Code anzeigen/verbergen, Passwort maskiert */
    on_command(ID_ACC_MANAGE, BN_CLICKED);
    pump();
    /* W418/W502: ungesichert = Karte Sichern (Discord, Login als Knopf, Code anzeigen) und Kontoeinstellungen
     * (Code einloesen, Abmelden, Loeschen) gleichzeitig sichtbar; Formulare ersetzen den Inhalt ihrer Karte */
    if (!why[0] && (!g_acc_view || acc_gt_vis(ID_LNET_RB) || acc_gt_vis(ID_LOC_VIS) || acc_gt_vis(ID_VSYNC) ||
                    acc_gt_vis(ID_ACC_DISCORD) || acc_gt_vis(ID_ACC_V_LOGIN) || !acc_gt_vis(ID_ACC_X_LOGIN) ||
                    !acc_gt_vis(ID_ACC_V_DELETE) || !acc_gt_vis(ID_ACC_V_RECSHOW) || !acc_gt_vis(ID_ACC_X_CODE) ||
                    !acc_gt_vis(ID_NAVSUB_ACC) || !acc_sub_active() || acc_gt_vis(ID_ACC_V_CODE) ||
                    !acc_gt_vis(ID_ACC_V_DISCORD) || !acc_gt_vis(ID_ACC_O_SEC)))
        strcpy_s(why, sizeof why, "Ansicht: Sichtbarkeit");
    on_command(ID_ACC_X_CODE, BN_CLICKED);
    pump();
    if (!why[0] && (acc_gt_vis(ID_ACC_V_DELETE) || acc_gt_vis(ID_ACC_V_FORGET) || !acc_gt_vis(ID_ACC_V_CODE) ||
                    !acc_gt_vis(ID_ACC_V_CDCANCEL)))
        strcpy_s(why, sizeof why, "Einstellungen: Codeformular");
    on_command(ID_ACC_V_CDCANCEL, BN_CLICKED);
    pump();
    if (!why[0] && (!acc_gt_vis(ID_ACC_V_DELETE) || acc_gt_vis(ID_ACC_V_CODE)))
        strcpy_s(why, sizeof why, "Einstellungen: Abbrechen");
    on_command(ID_ACC_X_LOGIN, BN_CLICKED); /* ungesichert: Login-Formular in der Karte Sichern */
    pump();
    if (!why[0] && (!acc_gt_vis(ID_ACC_V_LOGIN) || !acc_gt_vis(ID_ACC_V_PASS) || acc_gt_vis(ID_ACC_V_RECSHOW) ||
                    !acc_gt_vis(ID_ACC_V_DELETE)))
        strcpy_s(why, sizeof why, "Login-Formular");
    if (!why[0] && !(GetWindowLongW(ctl(ID_ACC_V_PASS), GWL_STYLE) & ES_PASSWORD))
        strcpy_s(why, sizeof why, "Passwortfeld nicht maskiert");
    if (!why[0] && g_acc_rec[0]) strcpy_s(why, sizeof why, "Code ohne Klick sichtbar");
    on_command(ID_ACC_V_RECSHOW, BN_CLICKED);
    {
        char want[48];
        lacc_recovery_code(4711, g_acc.key, want, sizeof want);
        if (!why[0] && strcmp(g_acc_rec, want) != 0) strcpy_s(why, sizeof why, "Code anzeigen");
    }
    on_command(ID_ACC_V_RECSHOW, BN_CLICKED);
    if (!why[0] && g_acc_rec[0]) strcpy_s(why, sizeof why, "Code verbergen");
    /* zu kurzes Passwort: Meldung, kein Auftrag */
    ui_settext(ctl(ID_ACC_V_LOGIN), "ace-login");
    ui_settext(ctl(ID_ACC_V_PASS), "kurz");
    on_command(ID_ACC_V_LOGINBTN, BN_CLICKED);
    if (!why[0] && (g_acc_busy || strcmp(g_status, LT("gui.acc_err_pw")) != 0 || g_acc_fmsg_id != ID_ACC_V_LOGIN))
        strcpy_s(why, sizeof why, "kurzes Passwort nicht abgewiesen");
    ui_settext(ctl(ID_ACC_V_PASS), "");
    /* e: zurueck (W399: Unterpunkt Match & network der Leiste statt Zurueck-Knopf) */
    on_command(ID_NAVSUB_MATCH, BN_CLICKED);
    pump();
    if (!why[0] && (g_acc_view || !acc_gt_vis(ID_LNET_RB) || !acc_gt_vis(ID_ACC_MANAGE)))
        strcpy_s(why, sizeof why, "Zurueck");
    /* f: Anmelden-Ansicht ohne Pilot (Wiederherstellungscode eingeben statt anzeigen) */
    acc_fixture(1, 0, 0, 0, 1);
    acc_refresh();
    pump();
    if (!why[0] && (!acc_gt_vis(ID_ACC_X_CODE) || acc_gt_vis(ID_ACC_V_CODE) || acc_gt_vis(ID_ACC_V_RECSHOW) ||
                    acc_gt_vis(ID_ACC_V_DELETE) || !acc_gt_vis(ID_ACC_V_DISCORD) || !acc_gt_vis(ID_ACC_X_LOGIN) ||
                    acc_gt_vis(ID_ACC_V_LOGIN) || acc_gt_vis(ID_ACC_V_FORGET) ||
                    !acc_gt_vis(ID_ACC_NAME) || !acc_gt_vis(ID_ACC_CREATE) || acc_gt_vis(ID_ACC_SIGNIN)))
        strcpy_s(why, sizeof why, "Anmelden-Ansicht");
    on_command(ID_ACC_X_LOGIN, BN_CLICKED);
    pump();
    {
        char lbl[64];
        ui_gettext(ctl(ID_ACC_V_LOGINBTN), lbl, sizeof lbl);
        if (!why[0] && (strcmp(lbl, LT("gui.acc_login_go")) != 0 || !acc_gt_vis(ID_ACC_V_LOGIN)))
            strcpy_s(why, sizeof why, "Knopf Anmelden");
    }
    /* g: Startweg: mit Pilot -> loc_lobby_pilot + SB_LOC_AUTH nur im Kindblock; ohne -> nichts */
    acc_fixture(1, 0, 0, 0, 0);
    strcpy_s(set, sizeof set, "vr_enable=0");
    env = acc_launch_env(set, sizeof set);
    if (!why[0] && (env || strcmp(set, "vr_enable=0") != 0)) strcpy_s(why, sizeof why, "ohne Pilot: Startweg veraendert");
    free(env);
    acc_fixture(1, 1, 1, 1, 0);
    env = acc_launch_env(set, sizeof set);
    if (!why[0] && (!env || strcmp(set, "vr_enable=0;loc_lobby_pilot=Ace Pilot") != 0))
        _snprintf_s(why, sizeof why, _TRUNCATE, "mit Pilot: Startweg '%s'", set);
    if (!why[0]) {
        wchar_t *p;
        int n = 0;
        char u8[600], kh[65];
        lacc_hex(g_acc.key, 32, kh);
        for (p = env; *p; p += wcslen(p) + 1) {
            w_to_u8(p, u8, sizeof u8);
            if (strncmp(u8, "SB_LOC_AUTH=4711:", 17) == 0 && strlen(u8) == 17 + 10 + 1 + 64) n++;
            if (strstr(u8, kh)) n = 100;
        }
        if (n != 1) _snprintf_s(why, sizeof why, _TRUNCATE, "Kindblock SB_LOC_AUTH (%d)", n);
        if (!why[0] && GetEnvironmentVariableA("SB_LOC_AUTH", u8, 8) > 0) strcpy_s(why, sizeof why, "Launcher-Umgebung veraendert");
    }
    if (env) free(env);
    /* Aufraeumen */
    g_acc_fixture = 0;
    g_acc_loaded = 0;
    g_acc_view = 0;
    g_acc = a0;
    g_game = g0;
    loctab_nav_layout();
    show_page(p0);
    sb_lang_set(keep);
    ui_apply_language();
    if (!why[0]) {
        GT_OK("75 W328 STEELNET-ACCOUNT pilot card/manage view (fixtures: no service, no pilot, unsecured, view, sign-in), recovery code show/hide, input checks, start path with SB_LOC_AUTH only in child env");
    } else {
        GT_FAIL("75 W328 STEELNET-ACCOUNT", "%s", why);
    }
    /* 76: optional gegen den lokalen Dienst (SB_STEELNET_API/SB_STEELNET_PIN) */
    {
        lacc_srv_t s;
        char tmp[1024], nm[40];
        why[0] = '\0';
        if (!lacc_server(&s) || !s.test) {
            printf("PASS 76 W328 account UI over the network (skipped: no SB_STEELNET_API)\n");
            (*pass)++;
            return;
        }
        u8_GetTempPath(tmp, sizeof tmp);
        _snprintf_s(g_acc_dir_test, sizeof g_acc_dir_test, _TRUNCATE, "%sw328_gui_%lu", tmp, GetCurrentProcessId());
        lacc_store_forget(g_acc_dir_test);
        g_acc_fixture = 0;
        g_acc_loaded = 1;
        memset(&g_acc, 0, sizeof g_acc);
        g_acc_st_ok = 0;
        g_game = LGAME_LOC;
        loctab_nav_layout();
        show_page(P_LNET);
        _snprintf_s(nm, sizeof nm, _TRUNCATE, "Gui%lu", GetCurrentProcessId() % 100000);
        ui_settext(ctl(ID_ACC_NAME), nm);
        on_command(ID_ACC_CREATE, BN_CLICKED);
        if (!acc_gt_wait(20000) || !g_acc.have || strcmp(g_acc.name, nm) != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "anlegen (%s)", g_acc_last_err);
        if (!why[0] && (!acc_gt_wait(20000) || !g_acc_st_ok || g_acc_st.discord || g_acc_st.login))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Status (%s)", g_acc_last_err);
        if (!why[0]) {
            lacc_store_t chk;
            if (lacc_store_load(g_acc_dir_test, &chk) != 1 || chk.account != g_acc.account)
                strcpy_s(why, sizeof why, "Schluessel nicht im Testprofil");
            SecureZeroMemory(&chk, sizeof chk);
        }
        if (!why[0]) { /* W537: Konto neu laden, Spielschluessel beim Start, Wiederherstellungscode - ueber sb_account.exe */
            lacc_store_t keep = g_acc;
            char set[200] = "", u8[600];
            wchar_t *env, *p;
            int i, nz = 0, n = 0;
            g_acc_loaded = 0;
            memset(&g_acc, 0, sizeof g_acc);
            acc_ensure_loaded();
            for (i = 0; i < 32; i++) nz |= g_acc.key[i];
            if (!g_acc.have || g_acc.account != keep.account || strcmp(g_acc.name, keep.name) != 0)
                strcpy_s(why, sizeof why, "W537 neu laden (load)");
            else if (nz) strcpy_s(why, sizeof why, "W537 Schluessel im Launcher-Speicher");
            if (!why[0]) {
                char want[32];
                env = acc_launch_env(set, sizeof set);
                _snprintf_s(want, sizeof want, _TRUNCATE, "SB_LOC_AUTH=%lu:", (unsigned long)keep.account);
                for (p = env; p && *p; p += wcslen(p) + 1) {
                    w_to_u8(p, u8, sizeof u8);
                    if (strncmp(u8, want, strlen(want)) == 0 && strlen(u8) > strlen(want) + 64) n++;
                }
                if (n != 1) _snprintf_s(why, sizeof why, _TRUNCATE, "W537 Spielschluessel beim Start (%d)", n);
                if (env) lacc_env_free(env);
                SecureZeroMemory(u8, sizeof u8);
            }
            if (!why[0]) {
                g_acc_view = 1;
                acc_refresh();
                g_acc_rec[0] = '\0';
                on_command(ID_ACC_V_RECSHOW, BN_CLICKED);
                if (strlen(g_acc_rec) != 39) strcpy_s(why, sizeof why, "W537 Wiederherstellungscode anzeigen");
                on_command(ID_ACC_V_RECSHOW, BN_CLICKED);
                if (!why[0] && g_acc_rec[0]) strcpy_s(why, sizeof why, "W537 Wiederherstellungscode verbergen");
                g_acc_view = 0;
            }
            SecureZeroMemory(&keep, sizeof keep);
        }
        if (!why[0] && !g_acc_st.gamertag[0]) { /* W487: ersten Gamertag ueber die Gamertag-Karte setzen (gamertag_set) */
            char gt[16];
            _snprintf_s(gt, sizeof gt, _TRUNCATE, "Gt%lu", GetCurrentProcessId() % 100000);
            g_acc_view = 1;
            g_acc_x_gt = 1;
            acc_refresh();
            ui_settext(ctl(ID_ACC_V_GT), gt);
            if (g_hwnd) KillTimer(g_hwnd, ACC_GT_TIMER);
            on_command(ID_ACC_V_GTBTN, BN_CLICKED);
            if (!acc_gt_wait(20000)) {
                strcpy_s(why, sizeof why, "Gamertag setzen: Zeitueberschreitung");
            } else if (strcmp(g_acc_last_err, "gamertag_off") == 0) {
                printf("     (76: Dienst ohne Gamertags - Setzen uebersprungen)\n");
            } else if (strcmp(g_acc_st.gamertag, gt) != 0 || acc_live_state() != ACC_LIVE_OK || g_acc_st.gt_next <= 0) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Gamertag setzen (%s, '%s')", g_acc_last_err, g_acc_st.gamertag);
            }
            g_acc_x_gt = 0;
            g_acc_view = 0;
        }
        if (!why[0]) {
            int c0 = g_confirm_auto;
            g_confirm_auto = 1;
            g_acc_view = 1;
            on_command(ID_ACC_V_DELETE, BN_CLICKED);
            g_confirm_auto = c0;
            if (!acc_gt_wait(20000) || g_acc.have || g_acc_view)
                _snprintf_s(why, sizeof why, _TRUNCATE, "loeschen (%s)", g_acc_last_err);
        }
        {
            lacc_store_t chk;
            if (!why[0] && lacc_store_load(g_acc_dir_test, &chk) != 0) strcpy_s(why, sizeof why, "Datei nach dem Loeschen noch da");
        }
        lacc_store_forget(g_acc_dir_test);
        {
            wchar_t w[1200];
            u8_to_w(g_acc_dir_test, w, 1200);
            RemoveDirectoryW(w);
        }
        g_acc_dir_test[0] = '\0';
        g_acc_loaded = 0;
        g_acc_view = 0;
        g_acc = a0;
        g_game = g0;
        loctab_nav_layout();
        show_page(p0);
        if (!why[0]) {
            GT_OK("76 W328 account UI over the network (create via worker thread + WM_APP_ACC, status, key in test profile, W487 first gamertag via gamertag card, delete with confirm; W537: all via sb_account.exe, reload/game key/recovery code via helper, no key in launcher memory)");
        } else {
            GT_FAIL("76 W328 account UI", "%s", why);
        }
    }
}

/* --- 75b (W399 LAUNCHER-ACCOUNT-PAGE): Unterpunkte Match & network / Account nur bei LoC auf Multiplayer
 * (eingerueckt unter dem Elternpunkt, folgende Punkte ruecken nach), "Account verwalten" springt auf Account,
 * Elternpunkt auf Match & network, Knoepfe der Account-Seite weiter verdrahtet (Fixture, Testmodus ohne Netz:
 * nur der angestossene Auftrag wird geprueft), Anlegen/Discord am Platz der Ansicht, DE-Texte. --- */
static int accpage_gt_y(int id)
{
    uictl_t *u = ui_find(id);
    return u ? u->y : -1;
}

__declspec(noinline) void accpage_gui_selftest(int *pass, int *fail)
{
    char why[300] = "", keep[16], t[120];
    int g0 = g_game, p0 = g_page, c0 = g_confirm_auto, ya0, ymp;
    lacc_store_t a0 = g_acc;
    if (!ctl(ID_NAVSUB_MATCH) || !ctl(ID_NAVSUB_ACC) || !ctl(ID_ACC_V_DISCORD)) {
        GT_FAIL("75b W399 LAUNCHER-ACCOUNT-PAGE", "%s", "Elemente fehlen");
        return;
    }
    strcpy_s(keep, sizeof keep, sb_lang_code());
    sb_lang_set("en");
    ui_apply_language();
    g_confirm_auto = 1;
    g_game = LGAME_LOC;
    loctab_nav_layout();
    acc_fixture(1, 1, 0, 0, 0);
    /* a: zugeklappt ausserhalb von Multiplayer */
    show_page(P_GFX);
    pump();
    ya0 = accpage_gt_y(ID_NAV0 + P_ADV);
    if (acc_gt_vis(ID_NAVSUB_MATCH) || acc_gt_vis(ID_NAVSUB_ACC))
        strcpy_s(why, sizeof why, "Unterpunkte ausserhalb von Multiplayer sichtbar");
    /* b: auf Multiplayer aufgeklappt, eingerueckt zwischen Multiplayer und Advanced, Match aktiv */
    on_command(ID_NAV0 + P_LNET, BN_CLICKED);
    pump();
    ymp = accpage_gt_y(ID_NAV0 + P_LNET);
    if (!why[0] && (!acc_gt_vis(ID_NAVSUB_MATCH) || !acc_gt_vis(ID_NAVSUB_ACC) || acc_sub_active() ||
                    g_page != P_LNET))
        strcpy_s(why, sizeof why, "Multiplayer: Unterpunkte/Match");
    if (!why[0] && !(accpage_gt_y(ID_NAVSUB_MATCH) >= ymp + 38 &&
                     accpage_gt_y(ID_NAVSUB_ACC) >= accpage_gt_y(ID_NAVSUB_MATCH) + NAVSUB_H &&
                     accpage_gt_y(ID_NAV0 + P_ADV) >= accpage_gt_y(ID_NAVSUB_ACC) + NAVSUB_H &&
                     accpage_gt_y(ID_NAV0 + P_ADV) > ya0))
        _snprintf_s(why, sizeof why, _TRUNCATE, "Lage Leiste (MP %d, Sub %d/%d, Adv %d vorher %d)", ymp,
                    accpage_gt_y(ID_NAVSUB_MATCH), accpage_gt_y(ID_NAVSUB_ACC), accpage_gt_y(ID_NAV0 + P_ADV), ya0);
    {
        uictl_t *s0 = ui_find(ID_NAVSUB_MATCH), *mp = ui_find(ID_NAV0 + P_LNET);
        if (!why[0] && (!s0 || !mp || s0->x != mp->x || s0->h_ >= mp->h_))
            strcpy_s(why, sizeof why, "Unterpunkt nicht kleiner/buendig");
    }
    /* c: "Account verwalten" -> Unterpunkt Account; Elternpunkt -> Match & network */
    on_command(ID_ACC_MANAGE, BN_CLICKED);
    pump();
    if (!why[0] && (!acc_sub_active() || g_page != P_LNET || acc_gt_vis(ID_LNET_RB) || !acc_gt_vis(ID_ACC_V_RECSHOW) ||
                    !acc_gt_vis(ID_NAVSUB_ACC)))
        strcpy_s(why, sizeof why, "Account verwalten springt nicht auf Account");
    on_command(ID_NAV0 + P_LNET, BN_CLICKED);
    pump();
    if (!why[0] && (acc_sub_active() || !acc_gt_vis(ID_LNET_RB) || !acc_gt_vis(ID_ACC_MANAGE)))
        strcpy_s(why, sizeof why, "Elternpunkt nicht Match & network");
    on_command(ID_NAVSUB_ACC, BN_CLICKED);
    pump();
    if (!why[0] && (!acc_sub_active() || acc_gt_vis(ID_LNET_RB)))
        strcpy_s(why, sizeof why, "Unterpunkt Account");
    /* d: andere Seite -> zugeklappt, Advanced wieder an alter Stelle; SB nie */
    show_page(P_CTRL);
    pump();
    if (!why[0] && (acc_gt_vis(ID_NAVSUB_MATCH) || acc_gt_vis(ID_NAVSUB_ACC) || accpage_gt_y(ID_NAV0 + P_ADV) != ya0))
        strcpy_s(why, sizeof why, "Controls: Leiste nicht zugeklappt");
    g_game = LGAME_SB;
    loctab_nav_layout();
    show_page(P_PACKS);
    pump();
    if (!why[0] && (acc_gt_vis(ID_NAVSUB_MATCH) || acc_gt_vis(ID_NAVSUB_ACC)))
        strcpy_s(why, sizeof why, "SB: Unterpunkte sichtbar");
    g_game = LGAME_LOC;
    loctab_nav_layout();
    /* e: ungesichert -> Discord als einziger Hauptknopf im Status; Knoepfe angestossen wie bisher */
    acc_fixture(1, 1, 0, 0, 1);
    show_page(P_LNET);
    acc_refresh();
    pump();
    {
        acc_geo_t gg;
        uictl_t *du = ui_find(ID_ACC_V_DISCORD);
        acc_geo(&gg);
        if (!why[0] && (!acc_gt_vis(ID_ACC_V_DISCORD) || accpage_gt_y(ID_ACC_V_DISCORD) != gg.r_dc + 4 || !du ||
                        du->kind != K_ACCENT))
            strcpy_s(why, sizeof why, "ungesichert: Discord nicht Hauptknopf der Sichern-Karte");
    }
    g_acc_try_op = 0;
    on_command(ID_ACC_V_DISCORD, BN_CLICKED);
    if (!why[0] && g_acc_try_op != ACC_OP_DISCORD_LINK) strcpy_s(why, sizeof why, "Discord verbinden");
    g_acc_try_op = 0;
    ui_settext(ctl(ID_ACC_V_CODE), "ABCD-EFGH-JKLM"); /* W418: Uebertragungscode des Admins (12 Zeichen) */
    on_command(ID_ACC_V_CODEBTN, BN_CLICKED);
    ui_settext(ctl(ID_ACC_V_CODE), "");
    if (!why[0] && g_acc_try_op != ACC_OP_TRANSFER) strcpy_s(why, sizeof why, "Uebertragungscode einloesen");
    g_acc_try_op = 0;
    ui_settext(ctl(ID_ACC_V_LOGIN), "ace-login");
    ui_settext(ctl(ID_ACC_V_PASS), "0123456789ab");
    on_command(ID_ACC_V_LOGINBTN, BN_CLICKED);
    ui_settext(ctl(ID_ACC_V_PASS), "");
    if (!why[0] && g_acc_try_op != ACC_OP_LOGIN_SET) strcpy_s(why, sizeof why, "Login sichern");
    if (!why[0] && (!acc_gt_vis(ID_ACC_V_RECCOPY) || !IsWindowEnabled(ctl(ID_ACC_V_RECCOPY)) || g_acc_rec[0]))
        strcpy_s(why, sizeof why, "Code kopieren / verdeckt");
    g_acc_try_op = 0;
    on_command(ID_ACC_V_DELETE, BN_CLICKED);
    if (!why[0] && g_acc_try_op != ACC_OP_DELETE) strcpy_s(why, sizeof why, "Loeschen (mit Rueckfrage)");
    on_command(ID_ACC_V_FORGET, BN_CLICKED); /* Fixture: nichts gespeichert, nur der Speicherzustand */
    if (!why[0] && g_acc.have) strcpy_s(why, sizeof why, "Abmelden (mit Rueckfrage)");
    /* f: nur Login gesichert -> Discord in der Zeile Discord; mit Discord verbunden -> kein Knopf */
    acc_fixture(1, 1, 0, 1, 1);
    acc_refresh();
    pump();
    { /* W502: Zeile Discord der Karte Sichern (Knopf "Mit Discord verbinden", kein Hauptknopf) */
        acc_geo_t gg;
        uictl_t *du = ui_find(ID_ACC_V_DISCORD);
        acc_geo(&gg);
        if (!why[0] && (!acc_gt_vis(ID_ACC_V_DISCORD) || accpage_gt_y(ID_ACC_V_DISCORD) != gg.r_dc + 4 || !du ||
                        du->kind != K_BTN))
            strcpy_s(why, sizeof why, "Login: Discord nicht in der Zeile Discord");
    }
    acc_fixture(1, 1, 1, 1, 1);
    acc_refresh();
    pump();
    if (!why[0] && acc_gt_vis(ID_ACC_V_DISCORD)) strcpy_s(why, sizeof why, "verbunden: Discord-Knopf sichtbar");
    /* g: ohne Account -> "Ich bin neu" (Name + Erstellen) links, "Schon ein Konto" rechts; Anlegen zurueck auf die Karte */
    acc_fixture(1, 0, 0, 0, 1);
    acc_refresh();
    pump();
    {
        acc_geo_t gg;
        acc_geo(&gg);
        if (!why[0] && (accpage_gt_y(ID_ACC_NAME) <= gg.lo_y || ui_find(ID_ACC_NAME)->x >= COL2 ||
                        !acc_gt_vis(ID_ACC_NAME) || acc_gt_vis(ID_ACC_SIGNIN)))
            strcpy_s(why, sizeof why, "ohne Account: Anlegen nicht in der Karte Neu");
    }
    g_acc_try_op = 0;
    on_command(ID_ACC_V_DISCORD, BN_CLICKED);
    if (!why[0] && g_acc_try_op != ACC_OP_DISCORD_LOGIN) strcpy_s(why, sizeof why, "Mit Discord anmelden");
    g_acc_try_op = 0;
    ui_settext(ctl(ID_ACC_V_LOGIN), "ace-login");
    ui_settext(ctl(ID_ACC_V_PASS), "0123456789ab");
    on_command(ID_ACC_V_LOGINBTN, BN_CLICKED);
    ui_settext(ctl(ID_ACC_V_PASS), "");
    ui_settext(ctl(ID_ACC_V_LOGIN), "");
    if (!why[0] && g_acc_try_op != ACC_OP_LOGIN) strcpy_s(why, sizeof why, "Mit Login anmelden");
    g_acc_try_op = 0;
    { /* W418: dasselbe Codefeld, Format Wiederherstellungscode -> Wiederherstellung */
        char rc[48];
        uint8_t k[32];
        int i;
        for (i = 0; i < 32; i++) k[i] = (uint8_t)(i * 7 + 1);
        lacc_recovery_code(4242, k, rc, sizeof rc);
        ui_settext(ctl(ID_ACC_V_CODE), rc);
        on_command(ID_ACC_V_CODEBTN, BN_CLICKED);
        ui_settext(ctl(ID_ACC_V_CODE), "");
    }
    if (!why[0] && g_acc_try_op != ACC_OP_RECOVERY) strcpy_s(why, sizeof why, "Wiederherstellungscode einloesen");
    g_acc_try_op = 0;
    ui_settext(ctl(ID_ACC_NAME), "Ace Pilot");
    on_command(ID_ACC_CREATE, BN_CLICKED);
    ui_settext(ctl(ID_ACC_NAME), "");
    if (!why[0] && g_acc_try_op != ACC_OP_CREATE) strcpy_s(why, sizeof why, "Anlegen");
    on_command(ID_NAVSUB_MATCH, BN_CLICKED);
    pump();
    if (!why[0] && (accpage_gt_y(ID_ACC_NAME) != LOC_ONL_Y + 86 || /* W473: Zeile Zustand darueber */ !acc_gt_vis(ID_ACC_SIGNIN)))
        strcpy_s(why, sizeof why, "Match: Anlegen nicht auf der Karte");
    on_command(ID_ACC_SIGNIN, BN_CLICKED); /* "Ich habe schon ein Konto" -> Account */
    pump();
    if (!why[0] && (!acc_sub_active() || !acc_gt_vis(ID_ACC_X_CODE)))
        strcpy_s(why, sizeof why, "Anmelden springt nicht auf Account");
    /* h: Dienst nicht eingerichtet -> Account-Seite nur mit Hinweis */
    acc_fixture(0, 0, 0, 0, 1);
    acc_refresh();
    pump();
    if (!why[0] && (acc_gt_vis(ID_ACC_NAME) || acc_gt_vis(ID_ACC_V_DISCORD) || acc_gt_vis(ID_ACC_V_LOGIN) ||
                    acc_gt_vis(ID_LNET_RB) || !acc_gt_vis(ID_NAVSUB_ACC)))
        strcpy_s(why, sizeof why, "ohne Dienst: Account-Seite");
    /* i: DE-Texte */
    sb_lang_set("de");
    ui_apply_language();
    ui_gettext(ctl(ID_NAVSUB_MATCH), t, sizeof t);
    if (!why[0] && strcmp(t, "Match & Netzwerk") != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "de Unterpunkt '%s'", t);
    ui_gettext(ctl(ID_NAVSUB_ACC), t, sizeof t);
    if (!why[0] && strcmp(t, "Konto") != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "de Unterpunkt '%s'", t); /* W418 */
    /* Aufraeumen */
    g_confirm_auto = c0;
    g_acc_fixture = 0;
    g_acc_loaded = 0;
    g_acc_view = 0;
    g_acc = a0;
    g_game = g0;
    loctab_nav_layout();
    acc_layout();
    show_page(p0);
    sb_lang_set(keep);
    ui_apply_language();
    if (!why[0]) {
        GT_OK("75b W399 LAUNCHER-ACCOUNT-PAGE sidebar sub-items only on LoC Multiplayer (indented, rest moves down, collapsed elsewhere/SB), Manage account/Sign in -> Account, parent -> Match & network, account buttons still wired (Discord link/login, login set/sign-in, transfer, recovery, create, delete/sign out with confirm), Discord button placement, no-service page, DE texts");
    } else {
        GT_FAIL("75b W399 LAUNCHER-ACCOUNT-PAGE", "%s", why);
    }
}

/* --- 75c (W418 LAUNCHER-ACCOUNT-UX): Kontoseite "steelnet account" je Zustand (kein Konto, ungesichert,
 * gesichert zu/Verwalten offen, Server nicht erreichbar): sichtbare Elemente, ein Hauptknopf, keine
 * Ueberlappung und alles ueber der Datenschutzzeile (EN + DE, mit Aufklappern), kein "Name pruefen",
 * entprellte Namenspruefung, ein Codefeld erkennt Wiederherstellungs- bzw. Uebertragungscode, Begriffe ohne
 * "Pilot", Karte Multiplayer (Spielen) mit Knopf zur Kontoseite, Verwalten pro Sitzung gemerkt. --- */
static int accux_gt_layout(char *why, size_t cap, const char *tag)
{
    int i, j, n = 0, id[64];
    uictl_t *r[64];
    for (i = 0; i < g_ui_n && n < 64; i++) {
        uictl_t *u = &g_ui[i];
        if (u->parent != g_hwnd || !IsWindowVisible(u->h) || u->page != P_LNET) {
            continue;
        }
        if (u->x < CX0 || u->x + u->w > CX0 + CW || u->y < CY0 || u->y + u->h_ > ACC_PRIV_Y - 4) {
            _snprintf_s(why, cap, _TRUNCATE, "%s: Element %d ausserhalb (%d,%d %dx%d)", tag, u->id, u->x, u->y, u->w,
                        u->h_);
            return 0;
        }
        id[n] = u->id;
        r[n++] = u;
    }
    for (i = 0; i < n; i++) {
        for (j = 0; j < i; j++) {
            if (r[i]->x < r[j]->x + r[j]->w && r[j]->x < r[i]->x + r[i]->w && r[i]->y < r[j]->y + r[j]->h_ &&
                r[j]->y < r[i]->y + r[i]->h_) {
                _snprintf_s(why, cap, _TRUNCATE, "%s: Elemente %d/%d ueberlappen", tag, id[i], id[j]);
                return 0;
            }
        }
    }
    return 1;
}

/* W502: alle Texte der Kontoseite passen in ihre Kaesten (acc_page_texts_fit misst statt zu zeichnen) */
static int page_texts_fit_tag(char *why, size_t cap, const char *tag)
{
    char w[200];
    if (acc_page_texts_fit(w, sizeof w)) {
        return 1;
    }
    _snprintf_s(why, cap, _TRUNCATE, "%s: Text %s", tag, w);
    return 0;
}

/* Zahl der sichtbaren Akzentknoepfe auf der Kontoseite (ein Hauptknopf je Zustand) */
static int accux_gt_accents(void)
{
    int i, n = 0;
    for (i = 0; i < g_ui_n; i++) {
        uictl_t *u = &g_ui[i];
        if (u->parent == g_hwnd && u->page == P_LNET && IsWindowVisible(u->h) && u->kind == K_ACCENT) {
            n++;
        }
    }
    return n;
}

/* kein sichtbarer Knopf mit dem Text "Check name" / "Name pruefen" */
static int accux_gt_nocheck(void)
{
    int i;
    char t[96];
    for (i = 0; i < g_ui_n; i++) {
        uictl_t *u = &g_ui[i];
        if (u->parent != g_hwnd || !IsWindowVisible(u->h)) {
            continue;
        }
        ui_gettext(u->h, t, sizeof t);
        if (_stricmp(t, "Check name") == 0 || strcmp(t, "Name pr\xC3\xBC" "fen") == 0) {
            return 0;
        }
    }
    return 1;
}

__declspec(noinline) void accux_gui_selftest(int *pass, int *fail)
{
    static const char *const kTerms[] = {
        "gui.acc_title", "gui.acc_name", "gui.acc_name_long", "gui.acc_create", "gui.acc_signin",
        "gui.acc_head_none2", "gui.acc_new_title", "gui.acc_have_title", "gui.acc_b_login_in", "gui.acc_b_code",
        "gui.acc_code_label", "gui.acc_code_hint", "gui.acc_ov_online_ok", "gui.acc_ov_sec_no", "gui.acc_sec_one",
        "gui.acc_sec_dc_btn", "gui.acc_rec_title", "gui.acc_set_title", "gui.acc_m_code_text", "gui.acc_l_login_s",
        "gui.acc_st_both", "gui.acc_st_discord", "gui.acc_st_login", "gui.acc_st_net", "gui.acc_created",
        "gui.mp_card_pilot", "gui.mp_card_nopilot", "gui.acc_mp_new", "gui.acc_mp_go", "gui.mp_sub_acc",
        "gui.acc_privacy", "gui.acc_err_name", "gui.acc_err_taken", "gui.acc_sum_none", "gui.acc_ov_title",
    };
    /* W502: Zustaende der Kontoseite (xl = Login-Formular, xc = Codeformular) */
    static const struct {
        int srv, have, dc, lg, xl, xc, net;
        const char *tag;
    } kSt[] = {
        { 1, 0, 0, 0, 0, 0, 0, "kein Konto" },          { 1, 0, 0, 0, 1, 0, 0, "kein Konto + Login" },
        { 1, 0, 0, 0, 0, 1, 0, "kein Konto + Code" },   { 1, 1, 0, 0, 0, 0, 0, "ungesichert" },
        { 1, 1, 0, 0, 1, 0, 0, "ungesichert + Login" }, { 1, 1, 0, 0, 0, 1, 0, "ungesichert + Code" },
        { 1, 1, 1, 1, 0, 0, 0, "gesichert" },           { 1, 1, 1, 0, 0, 1, 0, "gesichert + Code" },
        { 1, 1, 0, 1, 1, 1, 0, "Login + Passwort + Code" }, { 1, 1, 0, 0, 0, 0, 1, "nicht erreichbar" },
        { 0, 0, 0, 0, 0, 0, 0, "nicht eingerichtet" },
    };
    char why[300] = "", keep[16], t[300], tag[80];
    int g0 = g_game, p0 = g_page, c0 = g_confirm_auto, li, s, i;
    lacc_store_t a0 = g_acc;
    if (!ctl(ID_ACC_X_LOGIN) || !ctl(ID_ACC_X_CODE) || !ctl(ID_ACC_O_SEC) || !ctl(ID_LOC_MP_ACC)) {
        GT_FAIL("75c W418 LAUNCHER-ACCOUNT-UX", "%s", "Elemente fehlen");
        return;
    }
    strcpy_s(keep, sizeof keep, sb_lang_code());
    g_confirm_auto = 1;
    g_game = LGAME_LOC;
    loctab_nav_layout();
    /* 1: je Zustand (EN + DE) sichtbare Elemente, ein Hauptknopf, Lage */
    for (li = 0; li < 2 && !why[0]; li++) {
        sb_lang_set(li ? "de" : "en");
        ui_apply_language();
        for (s = 0; s < (int)(sizeof kSt / sizeof kSt[0]) && !why[0]; s++) {
            int st, acc_n, want_acc;
            acc_fixture(kSt[s].srv, kSt[s].have, kSt[s].dc, kSt[s].lg, 1);
            if (kSt[s].net) {
                g_acc_st_ok = 0;
                memset(&g_acc_st, 0, sizeof g_acc_st);
                strcpy_s(g_acc_st_err, sizeof g_acc_st_err, "net");
            }
            g_acc_x_login = kSt[s].xl;
            g_acc_x_code = kSt[s].xc && !(kSt[s].xl && !kSt[s].have); /* kein Konto: ein Formular */
            show_page(P_LNET);
            acc_refresh();
            pump();
            _snprintf_s(tag, sizeof tag, _TRUNCATE, "%s %s", li ? "de" : "en", kSt[s].tag);
            st = acc_state();
            if (!accux_gt_nocheck()) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "%s: Knopf 'Check name' sichtbar", tag);
                break;
            }
            if (!accux_gt_layout(why, sizeof why, tag)) {
                break;
            }
            acc_n = accux_gt_accents();
            /* Erstellen bzw. Mit Discord sichern; Login-Formular statt der Zeilen -> keiner */
            want_acc = st == ACCS_NONE || (st == ACCS_UNSEC && !kSt[s].xl);
            if (acc_n != want_acc) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "%s: %d Hauptknoepfe (erwartet %d)", tag, acc_n, want_acc);
                break;
            }
            if (!page_texts_fit_tag(why, sizeof why, tag)) { /* W502 */
                break;
            }
            switch (st) {
            case ACCS_NONE:
                if (!acc_gt_vis(ID_ACC_NAME) || !acc_gt_vis(ID_ACC_CREATE) || !acc_gt_vis(ID_ACC_O_ONLINE) ||
                    acc_gt_vis(ID_ACC_V_DISCORD) != !(kSt[s].xl || kSt[s].xc) ||
                    acc_gt_vis(ID_ACC_X_LOGIN) != !(kSt[s].xl || kSt[s].xc) ||
                    acc_gt_vis(ID_ACC_V_RECSHOW) || acc_gt_vis(ID_ACC_V_DELETE) || acc_gt_vis(ID_ACC_V_FORGET) ||
                    acc_gt_vis(ID_ACC_O_LIVE) || acc_gt_vis(ID_ACC_O_SEC) ||
                    acc_gt_vis(ID_ACC_V_LOGIN) != kSt[s].xl || acc_gt_vis(ID_ACC_V_CODE) != (kSt[s].xc && !kSt[s].xl) ||
                    ui_find(ID_ACC_NAME)->x >= COL2 || ui_find(ID_ACC_V_DISCORD)->x < COL2)
                    _snprintf_s(why, sizeof why, _TRUNCATE, "%s: Elemente (Erstellen links, Schon ein Konto rechts)", tag);
                ui_gettext(ctl(ID_ACC_V_DISCORD), t, sizeof t);
                if (!why[0] && strcmp(t, LT("gui.acc_discord_login")) != 0)
                    _snprintf_s(why, sizeof why, _TRUNCATE, "%s: Discord '%s'", tag, t);
                break;
            case ACCS_UNSEC:
            case ACCS_SEC:
            case ACCS_UNK:
                /* Sichern links (Zeilen bzw. Login-Formular), Einstellungen rechts (Knoepfe bzw. Codeformular) */
                if (acc_gt_vis(ID_ACC_NAME) || acc_gt_vis(ID_ACC_CREATE) ||
                    acc_gt_vis(ID_ACC_X_LOGIN) != !kSt[s].xl || acc_gt_vis(ID_ACC_V_RECSHOW) != !kSt[s].xl ||
                    acc_gt_vis(ID_ACC_V_LOGIN) != kSt[s].xl || acc_gt_vis(ID_ACC_V_LGCANCEL) != kSt[s].xl ||
                    acc_gt_vis(ID_ACC_V_DISCORD) != (!kSt[s].xl && !kSt[s].dc) ||
                    acc_gt_vis(ID_ACC_X_CODE) != !kSt[s].xc || acc_gt_vis(ID_ACC_V_FORGET) != !kSt[s].xc ||
                    acc_gt_vis(ID_ACC_V_DELETE) != !kSt[s].xc || acc_gt_vis(ID_ACC_V_CODE) != kSt[s].xc ||
                    acc_gt_vis(ID_ACC_O_SEC) != (st == ACCS_UNSEC) ||
                    acc_gt_vis(ID_ACC_O_ONLINE) != (st == ACCS_UNK && netm_get() == NETM_STEEL))
                    _snprintf_s(why, sizeof why, _TRUNCATE, "%s: Elemente (Sichern | Einstellungen)", tag);
                if (!why[0] && st == ACCS_UNSEC && !kSt[s].xl) {
                    ui_gettext(ctl(ID_ACC_V_DISCORD), t, sizeof t);
                    if (strcmp(t, LT("gui.acc_sec_dc_btn")) != 0)
                        _snprintf_s(why, sizeof why, _TRUNCATE, "%s: Discord '%s'", tag, t);
                }
                if (!why[0] && !kSt[s].xc && ui_find(ID_ACC_V_DELETE)->y <= ui_find(ID_ACC_V_FORGET)->y + 36)
                    _snprintf_s(why, sizeof why, _TRUNCATE, "%s: Loeschen nicht abgesetzt", tag);
                if (!why[0] && kSt[s].net) {
                    COLORREF c = 0;
                    const char *k = acc_unk_text(&c);
                    if (st != ACCS_UNK || strcmp(k, LT("gui.acc_st_net")) != 0 || c != C_WARN)
                        _snprintf_s(why, sizeof why, _TRUNCATE, "%s: Kopfzeile '%s'", tag, k);
                }
                break;
            default: /* nicht eingerichtet: nur der Hinweis */
                if (acc_gt_vis(ID_ACC_NAME) || acc_gt_vis(ID_ACC_V_DISCORD) || acc_gt_vis(ID_ACC_O_ONLINE) ||
                    acc_gt_vis(ID_ACC_X_LOGIN))
                    _snprintf_s(why, sizeof why, _TRUNCATE, "%s: Elemente ohne Dienst", tag);
                break;
            }
        }
    }
    sb_lang_set("en");
    ui_apply_language();
    /* 2: ein Codefeld - Format entscheidet (Wiederherstellung 32 Zeichen, Admin-Code 12), sonst Meldung, kein Auftrag */
    if (!why[0]) {
        char rc[48];
        uint8_t k[32];
        for (i = 0; i < 32; i++) k[i] = (uint8_t)(i * 5 + 3);
        lacc_recovery_code(77, k, rc, sizeof rc);
        if (acc_code_kind(rc) != 1 || acc_code_kind("abcd-efgh-jkmn") != 2 || acc_code_kind(" ABCD EFGH JKMN ") != 2 ||
            acc_code_kind("ABCD-EFGH") != 0 || acc_code_kind("ABCD-EFGH-IJKL") != 0 || acc_code_kind("") != 0 ||
            acc_code_kind("ABCD-EFGH-JK0M") != 0)
            strcpy_s(why, sizeof why, "Code-Format erkannt falsch");
        acc_fixture(1, 0, 0, 0, 1);
        g_acc_x_code = 1;
        acc_refresh();
        pump();
        g_acc_try_op = 0;
        ui_settext(ctl(ID_ACC_V_CODE), "ABCD-EFGH");
        on_command(ID_ACC_V_CODEBTN, BN_CLICKED);
        if (!why[0] && (g_acc_try_op != 0 || g_acc_fmsg_id != ID_ACC_V_CODE ||
                        strcmp(g_acc_fmsg, LT("gui.acc_err_code_fmt")) != 0))
            strcpy_s(why, sizeof why, "falsches Format: kein Auftrag, Meldung unter dem Feld");
        ui_settext(ctl(ID_ACC_V_CODE), rc);
        on_command(ID_ACC_V_CODEBTN, BN_CLICKED);
        if (!why[0] && g_acc_try_op != ACC_OP_RECOVERY) strcpy_s(why, sizeof why, "Wiederherstellungscode");
        g_acc_try_op = 0;
        ui_settext(ctl(ID_ACC_V_CODE), "abcd efgh jkmn");
        on_command(ID_ACC_V_CODEBTN, BN_CLICKED);
        if (!why[0] && g_acc_try_op != ACC_OP_TRANSFER) strcpy_s(why, sizeof why, "Uebertragungscode");
        /* mit Konto: Kontoeinstellungen (Codeformular), Rueckfrage (ersetzt das Konto) - abgelehnt -> kein Auftrag */
        acc_fixture(1, 1, 1, 1, 1);
        g_acc_x_code = 1;
        acc_refresh();
        pump();
        g_acc_try_op = 0;
        g_confirm_auto = 0; /* Rueckfrage mit Nein beantworten */
        ui_settext(ctl(ID_ACC_V_CODE), rc);
        on_command(ID_ACC_V_CODEBTN, BN_CLICKED);
        if (!why[0] && g_acc_try_op != 0) strcpy_s(why, sizeof why, "mit Konto: ohne Rueckfrage eingeloest");
        g_confirm_auto = 1;
        on_command(ID_ACC_V_CODEBTN, BN_CLICKED);
        ui_settext(ctl(ID_ACC_V_CODE), "");
        if (!why[0] && g_acc_try_op != ACC_OP_RECOVERY) strcpy_s(why, sizeof why, "mit Konto: nach Rueckfrage");
        SecureZeroMemory(rc, sizeof rc);
    }
    /* 3: Name wird beim Tippen geprueft (entprellt, ohne Knopf); ungueltig -> Meldung ohne Netz */
    if (!why[0]) {
        acc_fixture(1, 0, 0, 0, 1);
        acc_refresh();
        pump();
        g_acc_try_op = 0;
        ui_settext(ctl(ID_ACC_NAME), "Guest-Ace");
        acc_name_tick();
        if (g_acc_try_op != 0 || g_acc_fmsg_id != ID_ACC_NAME || g_acc_fmsg_col != C_ERR)
            strcpy_s(why, sizeof why, "ungueltiger Name beim Tippen");
        ui_settext(ctl(ID_ACC_NAME), "Ace Pilot");
        if (!why[0] && g_acc_fmsg_id == ID_ACC_NAME) strcpy_s(why, sizeof why, "Meldung bleibt beim Tippen stehen");
        acc_name_tick();
        if (!why[0] && g_acc_try_op != ACC_OP_CHECK) strcpy_s(why, sizeof why, "Namenspruefung nicht angestossen");
        ui_settext(ctl(ID_ACC_NAME), "");
        if (g_hwnd) KillTimer(g_hwnd, ACC_TIMER);
    }
    /* 4: Begriffe ohne "Pilot" (EN + DE), "steelnet account" / "steelnet-Konto" */
    for (li = 0; li < 2 && !why[0]; li++) {
        sb_lang_set(li ? "de" : "en");
        for (i = 0; i < (int)(sizeof kTerms / sizeof kTerms[0]); i++) {
            const char *v = LT(kTerms[i]);
            if (strstr(v, "Pilot") || strstr(v, "pilot") || !strcmp(v, kTerms[i])) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "%s %s: '%.80s'", li ? "de" : "en", kTerms[i], v);
                break;
            }
        }
        if (!why[0] && strcmp(LT("gui.acc_title"), li ? "steelnet-Konto" : "steelnet account") != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "Titel '%s'", LT("gui.acc_title"));
    }
    sb_lang_set("en");
    ui_apply_language();
    /* 5: Karte Multiplayer auf Spielen (ohne steelnet): Zeile + Knopf zur Kontoseite */
    if (!why[0]) {
        int nm0 = netm_get();
        acc_fixture(1, 0, 0, 0, 0);
        if (nm0 == NETM_STEEL) on_command(ID_NETM0 + NETM_OFF, BN_CLICKED);
        show_page(P_PLAY);
        pump();
        mp_card_acc_line(t, sizeof t);
        ui_gettext(ctl(ID_LOC_MP_ACC), tag, sizeof tag);
        if (!acc_gt_vis(ID_LOC_MP_ACC) || strcmp(t, LT("gui.mp_card_nopilot")) != 0 ||
            strcmp(tag, LT("gui.acc_mp_new")) != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "Karte Multiplayer ohne Konto: '%s' / '%s'", t, tag);
        on_command(ID_LOC_MP_ACC, BN_CLICKED);
        pump();
        if (!why[0] && (g_page != P_LNET || !acc_sub_active())) strcpy_s(why, sizeof why, "Knopf -> Kontoseite");
        acc_fixture(1, 1, 1, 1, 0);
        show_page(P_PLAY);
        pump();
        mp_card_acc_line(t, sizeof t);
        ui_gettext(ctl(ID_LOC_MP_ACC), tag, sizeof tag);
        if (!why[0] && (strcmp(t, "steelnet account: Ace Pilot") != 0 || strcmp(tag, LT("gui.acc_mp_go")) != 0))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Karte Multiplayer mit Konto: '%s' / '%s'", t, tag);
        on_command(ID_NETM0 + NETM_STEEL, BN_CLICKED);
        show_page(P_PLAY);
        pump();
        if (!why[0] && acc_gt_vis(ID_LOC_MP_ACC)) strcpy_s(why, sizeof why, "Knopf auch mit steelnet sichtbar");
        on_command(ID_NETM0 + nm0, BN_CLICKED);
    }
    /* 6: W502: offenes Codeformular bleibt in der Sitzung offen (Seitenwechsel), Abbrechen schliesst, nichts gespeichert */
    if (!why[0]) {
        acc_fixture(1, 1, 1, 1, 1);
        show_page(P_LNET);
        on_command(ID_ACC_X_CODE, BN_CLICKED);
        on_command(ID_NAVSUB_MATCH, BN_CLICKED);
        on_command(ID_NAVSUB_ACC, BN_CLICKED);
        pump();
        if (!g_acc_x_code || !acc_gt_vis(ID_ACC_V_CODE)) strcpy_s(why, sizeof why, "Codeformular nicht gemerkt");
        on_command(ID_ACC_V_CDCANCEL, BN_CLICKED);
        pump();
        if (!why[0] && (g_acc_x_code || !acc_gt_vis(ID_ACC_V_DELETE))) strcpy_s(why, sizeof why, "Codeformular abbrechen");
    }
    /* Aufraeumen */
    g_confirm_auto = c0;
    g_acc_fixture = 0;
    g_acc_loaded = 0;
    g_acc_view = 0;
    acc_x_reset();
    g_acc = a0;
    g_game = g0;
    loctab_nav_layout();
    acc_layout();
    show_page(p0);
    sb_lang_set(keep);
    ui_apply_language();
    if (!why[0]) {
        GT_OK("75c W418 LAUNCHER-ACCOUNT-UX (per state EN/DE: visible elements, one main button, no overlap above the privacy line incl. expanders; no Check name; debounced name check; one code field detects recovery/transfer code, confirm with account; terms without Pilot; Multiplayer card button to the account page; Manage kept per session)");
    } else {
        GT_FAIL("75c W418 LAUNCHER-ACCOUNT-UX", "%s", why);
    }
}

/* --- 75d (W487 LIVE-GAMERTAG): Gamertag auf der Kontoseite. Je Zustand (EN + DE) sichtbare Elemente und keine
 * Ueberlappung (Erstellen mit Gamertag-Feld; Kopf "Gamertag: X"; Gamertag-Karte statt Verwalten, ungesichert,
 * gesichert, nicht erreichbar, ohne Gamertag), Aufklapper schliessen sich aus, lokale Vorpruefung ohne Netz
 * (Erstellen und Setzen), gleicher Name/Grenze laut Status, Spieler-Aenderung beendet den Vorschlag, Statustexte
 * (change_limit mit Datum, unbekannt), Karte Match & network mit Gamertag-Zeile. --- */
__declspec(noinline) void w487_gt_gui_selftest(int *pass, int *fail)
{
    static const struct {
        int have, dc, lg, gt, net, nogt;
        const char *tag;
    } kSt[] = {
        { 0, 0, 0, 0, 0, 0, "kein Konto" },          { 1, 0, 0, 1, 0, 0, "ungesichert + Gamertag" },
        { 1, 1, 1, 1, 0, 0, "gesichert + Gamertag" }, { 1, 1, 0, 1, 0, 1, "gesichert ohne Gamertag" },
        { 1, 0, 0, 1, 1, 0, "nicht erreichbar + Gamertag" }, { 1, 1, 1, 0, 0, 0, "gesichert, Karte zu" },
    };
    char why[300] = "", keep[16], t[300], tag[80], w2[300];
    int g0 = g_game, p0 = g_page, c0 = g_confirm_auto, li, s;
    lacc_store_t a0 = g_acc;
    COLORREF c;
    if (!ctl(ID_ACC_GT) || !ctl(ID_ACC_X_GT) || !ctl(ID_ACC_V_GT) || !ctl(ID_ACC_V_GTBTN)) {
        GT_FAIL("75d W487 LIVE-GAMERTAG account page", "%s", "Elemente fehlen");
        return;
    }
    strcpy_s(keep, sizeof keep, sb_lang_code());
    g_confirm_auto = 1;
    g_game = LGAME_LOC;
    loctab_nav_layout();
    /* 1: je Zustand (EN + DE) - W502: Gamertag-Karte immer da (mit Konto), Feld ohne Gamertag, sonst Anzeige +
     * "Aendern"; Aendern offen = Feld + Abbrechen */
    for (li = 0; li < 2 && !why[0]; li++) {
        sb_lang_set(li ? "de" : "en");
        ui_apply_language();
        for (s = 0; s < (int)(sizeof kSt / sizeof kSt[0]) && !why[0]; s++) {
            int field, chg;
            acc_fixture(1, kSt[s].have, kSt[s].dc, kSt[s].lg, 1);
            if (kSt[s].net) {
                g_acc_st_ok = 0;
                memset(&g_acc_st, 0, sizeof g_acc_st);
                strcpy_s(g_acc_st_err, sizeof g_acc_st_err, "net");
            }
            if (kSt[s].nogt) g_acc_st.gamertag[0] = '\0';
            g_acc_x_gt = kSt[s].gt;
            show_page(P_LNET);
            acc_refresh();
            pump();
            _snprintf_s(tag, sizeof tag, _TRUNCATE, "%s %s", li ? "de" : "en", kSt[s].tag);
            if (!accux_gt_layout(why, sizeof why, tag)) break;
            if (!kSt[s].have) {
                if (!acc_gt_vis(ID_ACC_GT) || acc_gt_vis(ID_ACC_X_GT) || acc_gt_vis(ID_ACC_V_GT) ||
                    ui_find(ID_ACC_GT)->y <= ui_find(ID_ACC_NAME)->y || ui_find(ID_ACC_CREATE)->y <= ui_find(ID_ACC_GT)->y)
                    _snprintf_s(why, sizeof why, _TRUNCATE, "%s: Gamertag-Feld beim Erstellen (Lage/Sichtbarkeit)", tag);
                continue;
            }
            field = !kSt[s].net && (kSt[s].nogt || kSt[s].gt);
            chg = !kSt[s].net && !kSt[s].nogt && !kSt[s].gt;
            if (acc_gt_vis(ID_ACC_GT) || acc_gt_vis(ID_ACC_V_GT) != field || acc_gt_vis(ID_ACC_V_GTBTN) != field ||
                acc_gt_vis(ID_ACC_X_GT) != chg || acc_gt_vis(ID_ACC_V_GTCANCEL) != (field && !kSt[s].nogt) ||
                acc_gt_vis(ID_ACC_O_LIVE) != (!kSt[s].net && kSt[s].nogt) || !acc_gt_vis(ID_ACC_V_DELETE)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "%s: Sichtbarkeit Gamertag-Karte", tag);
                break;
            }
            {
                acc_geo_t gg;
                acc_geo(&gg);
                if (field && (ui_find(ID_ACC_V_GT)->y < gg.gt_y || ui_find(ID_ACC_V_GT)->y + 28 > gg.gt_y + gg.gt_h ||
                              gg.gt_y >= gg.lo_y)) {
                    _snprintf_s(why, sizeof why, _TRUNCATE, "%s: Gamertag-Feld nicht in der Karte direkt unter der Uebersicht", tag);
                    break;
                }
            }
            if (kSt[s].nogt) {
                const char *l1 = acc_gt_line1(w2, sizeof w2, &c);
                if (strcmp(l1, LT("gui.acc_gt_none")) != 0 || c != C_WARN)
                    _snprintf_s(why, sizeof why, _TRUNCATE, "%s: ohne Gamertag nicht deutlich ('%s')", tag, l1);
            }
            if (!why[0] && !page_texts_fit_tag(why, sizeof why, tag)) break;
        }
    }
    sb_lang_set("en");
    ui_apply_language();
    /* 2: W502 - "Aendern" oeffnet das Feld in der Karte (Fokus), Abbrechen schliesst und leert es */
    if (!why[0]) {
        acc_fixture(1, 1, 1, 1, 1);
        show_page(P_LNET);
        acc_refresh();
        on_command(ID_ACC_X_GT, BN_CLICKED);
        pump();
        if (!g_acc_x_gt || !acc_gt_vis(ID_ACC_V_GT) || !acc_gt_vis(ID_ACC_V_GTCANCEL) || acc_gt_vis(ID_ACC_X_GT) ||
            g_acc_focus_id != ID_ACC_V_GT)
            strcpy_s(why, sizeof why, "Aendern oeffnet das Feld nicht");
        ui_settext(ctl(ID_ACC_V_GT), "Iron Wolf");
        if (g_hwnd) KillTimer(g_hwnd, ACC_GT_TIMER);
        on_command(ID_ACC_V_GTCANCEL, BN_CLICKED);
        pump();
        ui_gettext(ctl(ID_ACC_V_GT), t, sizeof t);
        if (!why[0] && (g_acc_x_gt || acc_gt_vis(ID_ACC_V_GT) || !acc_gt_vis(ID_ACC_X_GT) || t[0] ||
                        g_acc_focus_id != ID_ACC_X_GT))
            strcpy_s(why, sizeof why, "Abbrechen schliesst das Feld nicht");
        g_acc_st.gt_next = (long long)time(NULL) + 2 * 86400LL; /* gesperrt: Aendern grau, Datum in der Karte */
        acc_refresh();
        if (!why[0] && (IsWindowEnabled(ctl(ID_ACC_X_GT)) || !strstr(acc_gt_limit(t, sizeof t), "-")))
            _snprintf_s(why, sizeof why, _TRUNCATE, "gesperrt: Aendern aktiv bzw. ohne Datum ('%s')", t);
        g_acc_st.gt_next = 0;
        acc_refresh();
    }
    /* 3: Setzen - lokale Vorpruefung, gleicher Name, Grenze laut Status, sonst Auftrag (Testmodus ohne Netz) */
    if (!why[0]) {
        acc_fixture(1, 1, 1, 1, 1);
        g_acc_x_gt = 1;
        show_page(P_LNET);
        acc_refresh();
        g_acc_try_op = 0;
        ui_settext(ctl(ID_ACC_V_GT), "1 bad");
        on_command(ID_ACC_V_GTBTN, BN_CLICKED);
        if (g_acc_try_op || g_acc_gtmsg_id != ID_ACC_V_GT || strcmp(g_acc_gtmsg, LT("gui.acc_gt_rules")) != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "ungueltig: op %d '%s'", g_acc_try_op, g_acc_gtmsg);
        ui_settext(ctl(ID_ACC_V_GT), "Ace Pilot");
        on_command(ID_ACC_V_GTBTN, BN_CLICKED);
        acc_gt_status_text("unchanged", 0, "Ace Pilot", t, sizeof t);
        if (!why[0] && (g_acc_try_op || strcmp(g_acc_gtmsg, t) != 0))
            _snprintf_s(why, sizeof why, _TRUNCATE, "gleicher Name: op %d '%s'", g_acc_try_op, g_acc_gtmsg);
        g_acc_st.gt_next = (long long)time(NULL) + 2 * 86400LL;
        ui_settext(ctl(ID_ACC_V_GT), "Ace Two");
        on_command(ID_ACC_V_GTBTN, BN_CLICKED);
        acc_gt_status_text("change_limit", g_acc_st.gt_next, "Ace Two", t, sizeof t);
        if (!why[0] && (g_acc_try_op || strcmp(g_acc_gtmsg, t) != 0 || !strstr(t, "-")))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Grenze: op %d '%s'", g_acc_try_op, g_acc_gtmsg);
        g_acc_st.gt_next = 0;
        on_command(ID_ACC_V_GTBTN, BN_CLICKED);
        if (!why[0] && g_acc_try_op != ACC_OP_GT_SET) _snprintf_s(why, sizeof why, _TRUNCATE, "Setzen: op %d", g_acc_try_op);
        /* erster Gamertag: ohne Rueckfrage, ohne Grenze */
        g_acc_st.gamertag[0] = '\0';
        g_acc_st.gt_next = (long long)time(NULL) + 86400LL;
        g_acc_try_op = 0;
        on_command(ID_ACC_V_GTBTN, BN_CLICKED);
        if (!why[0] && g_acc_try_op != ACC_OP_GT_SET) strcpy_s(why, sizeof why, "erster Gamertag gesperrt");
        ui_settext(ctl(ID_ACC_V_GT), "");
    }
    /* 4: Erstellen mit Gamertag - ungueltig lokal abgelehnt, sonst Auftrag; Spieler-Aenderung beendet den Vorschlag */
    if (!why[0]) {
        acc_fixture(1, 0, 0, 0, 1);
        show_page(P_LNET);
        acc_refresh();
        ui_settext(ctl(ID_ACC_NAME), "Ace Pilot");
        if (g_hwnd) KillTimer(g_hwnd, ACC_TIMER);
        if (g_acc_gt_user) strcpy_s(why, sizeof why, "Vorschlag vor der Eingabe beendet");
        ui_settext(ctl(ID_ACC_GT), "Ace;Pilot");
        if (!why[0] && !g_acc_gt_user) strcpy_s(why, sizeof why, "Eingabe beendet den Vorschlag nicht");
        g_acc_try_op = 0;
        on_command(ID_ACC_CREATE, BN_CLICKED);
        if (!why[0] && (g_acc_try_op || g_acc_gtmsg_id != ID_ACC_GT))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Erstellen ungueltig: op %d '%s'", g_acc_try_op, g_acc_gtmsg);
        ui_settext(ctl(ID_ACC_GT), "");
        if (!why[0] && g_acc_gt_user) strcpy_s(why, sizeof why, "leeres Feld: Vorschlag nicht wieder an");
        ui_settext(ctl(ID_ACC_GT), "Ace Pilot");
        on_command(ID_ACC_CREATE, BN_CLICKED);
        if (!why[0] && g_acc_try_op != ACC_OP_CREATE) _snprintf_s(why, sizeof why, _TRUNCATE, "Erstellen: op %d", g_acc_try_op);
        if (g_hwnd) KillTimer(g_hwnd, ACC_GT_TIMER);
        ui_settext(ctl(ID_ACC_NAME), "");
        ui_settext(ctl(ID_ACC_GT), "");
        if (g_hwnd) {
            KillTimer(g_hwnd, ACC_TIMER);
            KillTimer(g_hwnd, ACC_GT_TIMER);
        }
    }
    /* 5: Statustexte (alle bekannten Codes, change_limit mit Datum, unbekannt = allgemein) */
    if (!why[0]) {
        static const char *const kCodes[] = { "ok", "taken", "similar", "reserved", "blocked", "invalid_length",
                                              "invalid_characters", "change_limit", "unchanged", "off", "unavailable" };
        int k;
        for (k = 0; k < (int)(sizeof kCodes / sizeof kCodes[0]) && !why[0]; k++) {
            acc_gt_status_text(kCodes[k], 1764000000LL, "Ace", t, sizeof t);
            if (!t[0] || strcmp(t, LT("gui.acc_gt_refused")) == 0 || strstr(t, "%"))
                _snprintf_s(why, sizeof why, _TRUNCATE, "Statustext %s: '%s'", kCodes[k], t);
        }
        acc_gt_status_text("change_limit", 1764000000LL, "Ace", t, sizeof t);
        if (!why[0] && !strstr(t, "2025-11-2")) _snprintf_s(why, sizeof why, _TRUNCATE, "change_limit ohne Datum: '%s'", t);
        acc_gt_status_text("weird", 0, "Ace", t, sizeof t);
        if (!why[0] && strcmp(t, LT("gui.acc_gt_refused")) != 0) strcpy_s(why, sizeof why, "unbekannter Status");
    }
    /* 6: Karte Match & network (gesichert): Gamertag-Zeile bzw. "nicht gesetzt" (warnfarben), passt */
    if (!why[0]) {
        acc_fixture(1, 1, 1, 1, 0);
        acc_card_gt(t, sizeof t, &c);
        _snprintf_s(w2, sizeof w2, _TRUNCATE, LT("gui.acc_gt_head"), "Ace Pilot");
        if (strcmp(t, w2) != 0 || !ui_text_fits(F_TINY, t, 0, 0, COLW - 36 - 188, 28, 1))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Karte: '%s'", t);
        g_acc_st.gamertag[0] = '\0';
        if (!why[0] && (strcmp(acc_card_gt(t, sizeof t, &c), LT("gui.acc_gt_card_none")) != 0 || c != C_WARN))
            strcpy_s(why, sizeof why, "Karte ohne Gamertag");
    }
    /* Aufraeumen */
    g_confirm_auto = c0;
    g_acc_fixture = 0;
    g_acc_loaded = 0;
    g_acc_view = 0;
    acc_x_reset();
    g_acc = a0;
    g_game = g0;
    loctab_nav_layout();
    acc_layout();
    show_page(p0);
    sb_lang_set(keep);
    ui_apply_language();
    if (!why[0]) {
        GT_OK("75d W487 LIVE-GAMERTAG account page (create with gamertag field, header Gamertag: X, gamertag card instead of Manage per state EN/DE without overlap, expanders exclusive, local check without network for create/set, unchanged/limit from status, typing ends the suggestion, status texts incl. change_limit date, card line)");
    } else {
        GT_FAIL("75d W487 LIVE-GAMERTAG account page", "%s", why);
    }
}

/* --- 75e (W502 ACCOUNT-PAGE): Kontoseite neu gegliedert. Je Zustand (EN + DE): jedes sichtbare Element liegt in
 * einer gezeichneten Karte, keine Ueberlappung, nur echte Knoepfe/Felder (keine Textlinks), Karten in der
 * Reihenfolge Uebersicht -> Gamertag -> Sichern | Einstellungen, Tab-Reihenfolge folgt den Karten, Knoepfe der
 * Uebersicht in ihrer Zeile, alle Texte passen. Spruenge der Uebersicht (Gamertag waehlen -> Feld + Hervorhebung,
 * Konto sichern -> Discord, Konto erstellen -> Name, Erneut versuchen -> Statusabfrage), Einsprung
 * acc_open_gamertag() von einer anderen Seite (mit/ohne Gamertag, ohne Konto). --- */
static int w502_card_of(const uictl_t *u, const RECT *r, const int *k, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (u->x >= r[i].left + 4 && u->x + u->w <= r[i].right - 4 && u->y >= r[i].top + 4 &&
            u->y + u->h_ <= r[i].bottom - 4) {
            return i;
        }
    }
    (void)k;
    return -1;
}

static int w502_check_page(char *why, size_t cap, const char *tag)
{
    RECT r[8];
    int k[8], n = acc_page_cards(r, k, 8), i, last = -1, j;
    acc_geo_t g;
    acc_geo(&g);
    for (j = 1; j < n; j++) { /* Reihenfolge der Karten: oben nach unten, links nach rechts, ohne Ueberlappung */
        if (r[j].top < r[j - 1].top || (r[j].top == r[j - 1].top && r[j].left < r[j - 1].right) ||
            (r[j].top > r[j - 1].top && r[j].top < r[j - 1].bottom)) {
            _snprintf_s(why, cap, _TRUNCATE, "%s: Karten %d/%d Reihenfolge/Ueberlappung", tag, j - 1, j);
            return 0;
        }
        if (r[j].bottom > ACC_PRIV_Y - 4) {
            _snprintf_s(why, cap, _TRUNCATE, "%s: Karte %d reicht in die Datenschutzzeile", tag, j);
            return 0;
        }
    }
    for (i = 0; i < g_ui_n; i++) {
        uictl_t *u = &g_ui[i];
        int c;
        if (u->parent != g_hwnd || u->page != P_LNET || !IsWindowVisible(u->h)) {
            continue;
        }
        if (u->id >= ID_NAVSUB_MATCH && u->id <= ID_NAVSUB_ACC) {
            continue; /* Leiste */
        }
        if (u->kind != K_BTN && u->kind != K_ACCENT && u->kind != K_EDIT) {
            _snprintf_s(why, cap, _TRUNCATE, "%s: Element %d ist kein Knopf/Feld (Art %d)", tag, u->id, u->kind);
            return 0;
        }
        c = w502_card_of(u, r, k, n);
        if (c < 0) {
            _snprintf_s(why, cap, _TRUNCATE, "%s: Element %d (%d,%d %dx%d) liegt in keiner Karte", tag, u->id, u->x, u->y,
                        u->w, u->h_);
            return 0;
        }
        if (c < last) { /* Tab-Reihenfolge = Erzeugungsreihenfolge, folgt den Karten */
            _snprintf_s(why, cap, _TRUNCATE, "%s: Tab-Reihenfolge springt zurueck (Element %d)", tag, u->id);
            return 0;
        }
        last = c;
        if (u->id == ID_ACC_O_ONLINE || u->id == ID_ACC_O_LIVE || u->id == ID_ACC_O_SEC) {
            int row = u->id == ID_ACC_O_ONLINE ? 0 : u->id == ID_ACC_O_LIVE ? 1 : 2;
            if (k[c] != ACC_CARD_OV || u->y != g.ov_y + 42 + row * 32 + 2 || u->x + u->w != CX0 + CW - 18) {
                _snprintf_s(why, cap, _TRUNCATE, "%s: Knopf %d nicht rechts in seiner Zeile der Uebersicht", tag, u->id);
                return 0;
            }
        }
    }
    if (!accux_gt_layout(why, cap, tag)) {
        return 0;
    }
    return page_texts_fit_tag(why, cap, tag);
}

__declspec(noinline) void w502_accpage_gui_selftest(int *pass, int *fail)
{
    static const struct {
        int have, dc, lg, nogt, lock, xg, xl, xc, net, gtoff;
        const char *tag;
    } kSt[] = {
        { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "kein Konto" },          { 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, "kein Konto + Login" },
        { 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, "kein Konto + Code" },   { 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, "ungesichert ohne Gamertag" },
        { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, "ungesichert mit Gamertag" }, { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, "Discord" },
        { 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, "Login" },               { 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, "gesperrt (7 Tage)" },
        { 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, "Gamertag aendern" },    { 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, "nicht erreichbar" },
        { 1, 1, 0, 1, 0, 0, 0, 0, 0, 1, "ohne Gamertags" },      { 1, 0, 0, 1, 0, 0, 1, 1, 0, 0, "Login + Code offen" },
    };
    static const int kSz[3] = { LLAY_SZ_COMPACT, LLAY_SZ_NORMAL, LLAY_SZ_LARGE };
    char why[300] = "", keep[16], tag[96], t[200];
    int g0 = g_game, p0 = g_page, c0 = g_confirm_auto, li, s, nm0 = netm_get(), sz;
    int id0 = g_lay.id, sz0 = g_lay.size, shot0 = g_o.shot;
    lacc_store_t a0 = g_acc;
    if (!ctl(ID_ACC_O_ONLINE) || !ctl(ID_ACC_O_LIVE) || !ctl(ID_ACC_O_SEC) || !ctl(ID_ACC_V_GTCANCEL) ||
        !ctl(ID_ACC_V_LGCANCEL) || !ctl(ID_ACC_V_CDCANCEL)) {
        GT_FAIL("75e W502 ACCOUNT-PAGE", "%s", "Elemente fehlen");
        return;
    }
    strcpy_s(keep, sizeof keep, sb_lang_code());
    g_confirm_auto = 1;
    g_game = LGAME_LOC;
    loctab_nav_layout();
    /* 1: je Groesse (Kompakt/Normal/Gross, ohne Speichern), Sprache (EN + DE) und Zustand */
    g_o.shot = 2;
    for (sz = 0; sz < 3 && !why[0]; sz++)
    for (li = 0; li < 2 && !why[0]; li++) {
        lay_switch(LLAY_A, kSz[sz]);
        sb_lang_set(li ? "de" : "en");
        ui_apply_language();
        for (s = 0; s < (int)(sizeof kSt / sizeof kSt[0]) && !why[0]; s++) {
            acc_fixture(1, kSt[s].have, kSt[s].dc, kSt[s].lg, 1);
            if (kSt[s].net) {
                g_acc_st_ok = 0;
                memset(&g_acc_st, 0, sizeof g_acc_st);
                strcpy_s(g_acc_st_err, sizeof g_acc_st_err, "net");
            }
            if (kSt[s].nogt) g_acc_st.gamertag[0] = '\0';
            if (kSt[s].gtoff) strcpy_s(g_acc_st.gt_state, sizeof g_acc_st.gt_state, "off");
            if (kSt[s].lock) g_acc_st.gt_next = (long long)time(NULL) + 3 * 86400LL;
            g_acc_x_gt = kSt[s].xg;
            g_acc_x_login = kSt[s].xl;
            g_acc_x_code = kSt[s].xc && !(kSt[s].xl && !kSt[s].have);
            show_page(P_LNET);
            acc_refresh();
            pump();
            _snprintf_s(tag, sizeof tag, _TRUNCATE, "%s %s %s", sz == 0 ? "kompakt" : sz == 1 ? "normal" : "gross",
                        li ? "de" : "en", kSt[s].tag);
            if (!w502_check_page(why, sizeof why, tag)) break;
            /* Uebersicht: Knopf genau dort, wo etwas fehlt */
            if (kSt[s].have && (acc_gt_vis(ID_ACC_O_LIVE) != (kSt[s].nogt && !kSt[s].gtoff && !kSt[s].net) ||
                                acc_gt_vis(ID_ACC_O_SEC) != (!kSt[s].dc && !kSt[s].lg && !kSt[s].net))) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "%s: Knoepfe der Uebersicht", tag);
                break;
            }
            if (kSt[s].lock && (IsWindowEnabled(ctl(ID_ACC_X_GT)) || !acc_gt_vis(ID_ACC_X_GT))) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "%s: Aendern trotz Sperre aktiv", tag);
                break;
            }
        }
    }
    lay_switch(id0, sz0);
    g_o.shot = shot0;
    sb_lang_set("en");
    ui_apply_language();
    /* 2: Spruenge der Uebersicht (Fokus nur gemerkt, im Selbsttest kein SetFocus) */
    if (!why[0]) {
        acc_fixture(1, 1, 0, 0, 1);
        g_acc_st.gamertag[0] = '\0';
        show_page(P_LNET);
        acc_refresh();
        g_acc_focus_id = 0;
        on_command(ID_ACC_O_LIVE, BN_CLICKED);
        if (g_acc_focus_id != ID_ACC_V_GT || g_acc_hl != ACC_CARD_GT || !acc_gt_vis(ID_ACC_V_GT))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Gamertag waehlen: Fokus %d, Karte %d", g_acc_focus_id, g_acc_hl);
        g_acc_x_login = 1;
        on_command(ID_ACC_O_SEC, BN_CLICKED);
        if (!why[0] && (g_acc_focus_id != ID_ACC_V_DISCORD || g_acc_hl != ACC_CARD_SEC || g_acc_x_login ||
                        !acc_gt_vis(ID_ACC_V_DISCORD)))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Konto sichern: Fokus %d, Karte %d", g_acc_focus_id, g_acc_hl);
        acc_hl_tick();
        if (!why[0] && g_acc_hl) strcpy_s(why, sizeof why, "Hervorhebung endet nicht");
        acc_fixture(1, 0, 0, 0, 1);
        acc_refresh();
        on_command(ID_ACC_O_ONLINE, BN_CLICKED);
        if (!why[0] && (g_acc_focus_id != ID_ACC_NAME || g_acc_hl != ACC_CARD_NEW))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Konto erstellen: Fokus %d, Karte %d", g_acc_focus_id, g_acc_hl);
        /* Status unbekannt mit steelnet: Erneut versuchen stoesst die vorhandene Statusabfrage an */
        acc_fixture(1, 1, 0, 0, 1);
        g_acc_st_ok = 0;
        strcpy_s(g_acc_st_err, sizeof g_acc_st_err, "net");
        if (nm0 != NETM_STEEL) on_command(ID_NETM0 + NETM_STEEL, BN_CLICKED);
        show_page(P_LNET);
        acc_refresh();
        pump();
        ui_gettext(ctl(ID_ACC_O_ONLINE), t, sizeof t);
        g_acc_try_op = 0;
        on_command(ID_ACC_O_ONLINE, BN_CLICKED);
        if (!why[0] && (!acc_gt_vis(ID_ACC_O_ONLINE) || strcmp(t, LT("gui.acc_ov_retry")) != 0 ||
                        g_acc_try_op != ACC_OP_STATUS))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Erneut versuchen: '%s' op %d", t, g_acc_try_op);
        if (nm0 != NETM_STEEL) on_command(ID_NETM0 + nm0, BN_CLICKED);
    }
    /* 3: Einsprung acc_open_gamertag() von einer anderen Seite (W501: Match ohne Gamertag) */
    if (!why[0]) {
        acc_fixture(1, 1, 1, 1, 0);
        g_acc_st.gamertag[0] = '\0';
        show_page(P_PLAY);
        pump();
        g_acc_focus_id = 0;
        acc_open_gamertag();
        pump();
        if (g_page != P_LNET || !g_acc_view || !acc_sub_active() || g_acc_focus_id != ID_ACC_V_GT ||
            g_acc_hl != ACC_CARD_GT || !acc_gt_vis(ID_ACC_V_GT))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Einsprung ohne Gamertag: Seite %d Fokus %d", g_page, g_acc_focus_id);
        acc_fixture(1, 1, 1, 1, 0);
        show_page(P_PLAY);
        acc_open_gamertag();
        pump();
        if (!why[0] && (g_acc_focus_id != ID_ACC_X_GT || !acc_gt_vis(ID_ACC_X_GT) || acc_gt_vis(ID_ACC_V_GT)))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Einsprung mit Gamertag: Fokus %d", g_acc_focus_id);
        acc_fixture(1, 0, 0, 0, 0);
        show_page(P_PLAY);
        acc_open_gamertag();
        pump();
        if (!why[0] && (g_page != P_LNET || g_acc_focus_id != ID_ACC_NAME || g_acc_hl != ACC_CARD_NEW))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Einsprung ohne Konto: Fokus %d", g_acc_focus_id);
    }
    /* Aufraeumen */
    g_confirm_auto = c0;
    g_acc_fixture = 0;
    g_acc_loaded = 0;
    g_acc_view = 0;
    acc_x_reset();
    g_acc = a0;
    g_game = g0;
    loctab_nav_layout();
    acc_layout();
    show_page(p0);
    sb_lang_set(keep);
    ui_apply_language();
    if (!why[0]) {
        GT_OK("75e W502 ACCOUNT-PAGE (per state EN/DE: every element inside a drawn card, no overlap, buttons/fields only, card order overview -> gamertag -> secure | settings, tab order follows the cards, overview buttons in their row, all texts fit; jumps: choose gamertag/secure/create/try again; entry acc_open_gamertag from another page with/without gamertag and without account)");
    } else {
        GT_FAIL("75e W502 ACCOUNT-PAGE", "%s", why);
    }
}

/* --- 77 (W334 LOC-SEC): Karte "Online (steelnet)": Raumcode-Normalisierung,
 * --set-Zusammenbau mit/ohne Code, loc_net_status.txt (sec=, lobby_code=) aus
 * einer Temp-Datei, Schalter grau ohne Dienst, gemerkt in einer Temp-user.cfg,
 * Startweg (Trockenlauf) mit/ohne Online, ungueltiger Code = kein Start. --- */
static void onl_gt_status(const char *path, const char *text)
{
    write_file(path, text);
}

__declspec(noinline) void onl_gui_selftest(int *pass, int *fail)
{
    static const struct {
        const char *in;
        int rc;
        const char *out;
    } kN[] = {
        { "", 0, "" },                     { "   ", 0, "" },
        { "7K3M-Q9TX", 1, "7K3M-Q9TX" },   { "7k3m-q9tx", 1, "7K3M-Q9TX" },
        { " 7k3m q9tx ", 1, "7K3M-Q9TX" }, { "7K3MQ9TX", 1, "7K3M-Q9TX" },
        { "ilo0-abcd", 1, "1100-ABCD" },   { "7K3M-Q9TU", -1, "" },
        { "7K3M-Q9T", -1, "" },            { "7K3MQ9TXA", -1, "" },
        { "7K3M--Q9TX", -1, "" },          { "7K3M-Q9TX-", -1, "" },
        { "7K3M_Q9TX", -1, "" },           { "7K3M-Q9T\xC3\x84", -1, "" },
        { "-", -1, "" },
    };
    static const char *const kSecIn[] = { "selftest_failed", "pin_failed", "no_pin", "upgrade_required" };
    static const char *const kSecKey[] = { "gui.loc_onl_sec_selftest", "gui.loc_onl_sec_pin", "gui.loc_onl_nosrv",
                                           "gui.loc_onl_sec_upgrade" };
    char why[300] = "", keep[16], tmp[1024], cfg[1200], stp[1200], set[600], want[600], t[400], sec[24], code[16];
    char buf[4096];
    int i, rc, g0 = g_game, p0 = g_page, on0 = g_loc_online, uc0 = g_upd_cfg;
    lgame_state_t loc0 = g_loc;
    lacc_store_t a0 = g_acc;
    COLORREF col;
    /* W397: kein Schalter "Online-Spiel" mehr im LoC-Bereich (Stufe steelnet unter dem Zahnrad) */
    if (ctl(ID_LOC_ONL) || !ctl(ID_NETM0 + NETM_STEEL) || !ctl(ID_LOC_CODE) || !ctl(ID_LOC_CODE_COPY)) {
        GT_FAIL("77 W334 LOC-SEC online card", "%s", "Elemente fehlen");
        return;
    }
    /* a: Raumcode normalisieren */
    for (i = 0; i < (int)(sizeof kN / sizeof kN[0]) && !why[0]; i++) {
        rc = onl_code_norm(kN[i].in, code, sizeof code);
        if (rc != kN[i].rc || strcmp(code, kN[i].out) != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "norm '%s' -> %d '%s' (soll %d '%s')", kN[i].in, rc, code,
                        kN[i].rc, kN[i].out);
        }
    }
    /* b: --set mit/ohne Code, aus, ohne Dienst, ungueltig */
    if (!why[0]) {
        strcpy_s(set, sizeof set, "vr_enable=0");
        rc = onl_set_append(set, sizeof set, 1, 1, " 7k3m q9tx", 1); /* W373: Code tritt immer privat bei */
        _snprintf_s(want, sizeof want, _TRUNCATE, "vr_enable=0;loc_transport=udp;loc_lobby=%s:%d;loc_lobby_code=7K3M-Q9TX",
                    SB_STEELNET_HOST, (int)SB_STEELNET_PORT);
        if (rc != 0 || strcmp(set, want) != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "set mit Code '%s'", set);
    }
    if (!why[0]) {
        strcpy_s(set, sizeof set, "vr_enable=0");
        rc = onl_set_append(set, sizeof set, 1, 1, "", 0);
        _snprintf_s(want, sizeof want, _TRUNCATE, "vr_enable=0;loc_transport=udp;loc_lobby=%s:%d;loc_lobby_code=",
                    SB_STEELNET_HOST, (int)SB_STEELNET_PORT);
        if (rc != 0 || strcmp(set, want) != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "set ohne Code '%s'", set);
    }
    if (!why[0]) { /* W373: oeffentlich ohne Code -> loc_lobby_vis=public */
        strcpy_s(set, sizeof set, "vr_enable=0");
        rc = onl_set_append(set, sizeof set, 1, 1, "  ", 1);
        _snprintf_s(want, sizeof want, _TRUNCATE,
                    "vr_enable=0;loc_transport=udp;loc_lobby=%s:%d;loc_lobby_code=;loc_lobby_vis=public",
                    SB_STEELNET_HOST, (int)SB_STEELNET_PORT);
        if (rc != 0 || strcmp(set, want) != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "W373 set oeffentlich '%s'", set);
    }
    if (!why[0]) {
        strcpy_s(set, sizeof set, "vr_enable=0");
        if (onl_set_append(set, sizeof set, 0, 1, "7K3M-Q9TX", 1) != 0 || strcmp(set, "vr_enable=0") != 0 ||
            onl_set_append(set, sizeof set, 1, 0, "7K3M-Q9TX", 1) != 0 || strcmp(set, "vr_enable=0") != 0 ||
            onl_set_append(set, sizeof set, 0, 1, "", 1) != 0 || strcmp(set, "vr_enable=0") != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "set aus/ohne Dienst veraendert '%s'", set);
        else if (onl_set_append(set, sizeof set, 1, 1, "7K3M-Q9TU", 1) != -1 || strcmp(set, "vr_enable=0") != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "set ungueltig nicht abgewiesen '%s'", set);
    }
    /* c: Statusdatei lesen (Temp-Ordner) */
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(stp, sizeof stp, _TRUNCATE, "%sw334_loc_net_status_%lu.txt", tmp, GetCurrentProcessId());
    _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw334_gui_user_%lu.cfg", tmp, GetCurrentProcessId());
    u8_DeleteFile(stp);
    if (!why[0] && onl_status_parse(stp, sec, sizeof sec, code, sizeof code) != 0)
        strcpy_s(why, sizeof why, "fehlende Statusdatei gelesen");
    if (!why[0]) {
        onl_gt_status(stp, "transport=udp\nlobby=lobby\nroute=direkt\npeers=1\nping_ms=31\nsec=ok\nlobby_code=7k3m-q9tx\n");
        if (onl_status_parse(stp, sec, sizeof sec, code, sizeof code) != 1 || strcmp(sec, "ok") != 0 ||
            strcmp(code, "7K3M-Q9TX") != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "Status ok: '%s' '%s'", sec, code);
    }
    for (i = 0; i < 4 && !why[0]; i++) {
        char txt[200];
        _snprintf_s(txt, sizeof txt, _TRUNCATE, "lobby=aus\nsec=%s\nlobby_code=\n", kSecIn[i]);
        onl_gt_status(stp, txt);
        if (onl_status_parse(stp, sec, sizeof sec, code, sizeof code) != 1 || strcmp(sec, kSecIn[i]) != 0 || code[0])
            _snprintf_s(why, sizeof why, _TRUNCATE, "Status %s: '%s' '%s'", kSecIn[i], sec, code);
    }
    if (!why[0]) {
        onl_gt_status(stp, "sec=gehackt\nlobby_code=<b>X</b>\n");
        if (onl_status_parse(stp, sec, sizeof sec, code, sizeof code) != 1 || sec[0] || code[0])
            _snprintf_s(why, sizeof why, _TRUNCATE, "fremde Werte uebernommen: '%s' '%s'", sec, code);
    }
    /* W353: err= je Grund gelesen und als Meldung (de/en) vor dem Raumcode gezeigt; fremde Werte nie */
    for (i = 0; i <= ONL_ERR_N && !why[0]; i++) {
        char txt[200], er[12], info[400], ks[8];
        COLORREF ic = 0;
        _snprintf_s(txt, sizeof txt, _TRUNCATE, "lobby=verbinde\nsec=ok\nlobby_code=7K3M-Q9TX\nerr=%s\n",
                    i < ONL_ERR_N ? kOnlErr[i][0] : "<script>");
        onl_gt_status(stp, txt);
        onl_status_err(stp, er, sizeof er);
        if (strcmp(er, i < ONL_ERR_N ? kOnlErr[i][0] : "") != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "err %d gelesen als '%s'", i, er);
            break;
        }
        strcpy_s(ks, sizeof ks, sb_lang_code());
        sb_lang_set(i & 1 ? "de" : "en");
        strcpy_s(g_onl_err, sizeof g_onl_err, er);
        strcpy_s(g_onl_sec, sizeof g_onl_sec, "ok");
        strcpy_s(g_onl_code, sizeof g_onl_code, "7K3M-Q9TX");
        g_onl_fix_srv = 1;
        g_loc_online = 0;
        onl_info(info, sizeof info, &ic);
        if (i < ONL_ERR_N && (strcmp(info, LT(kOnlErr[i][1])) != 0 || !LT(kOnlErr[i][1])[0] ||
                              strncmp(LT(kOnlErr[i][1]), "gui.", 4) == 0))
            _snprintf_s(why, sizeof why, _TRUNCATE, "err %s: Anzeige '%.80s'", er, info);
        if (i == ONL_ERR_N && !strstr(info, "7K3M-Q9TX"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "ohne err: Anzeige '%.80s'", info);
        sb_lang_set(ks);
        g_onl_err[0] = g_onl_sec[0] = g_onl_code[0] = '\0';
        g_onl_fix_srv = -1;
    }
    /* d: Karte im Fenster (Temp-user.cfg, Testdatei, kein Netz, kein echtes Konto) */
    write_file(cfg, "[display]\nwidth=1280\n# fremde Zeile\nlauncher_loc_online=0\nlauncher_loc_online=0\n");
    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, cfg);
    strcpy_s(keep, sizeof keep, sb_lang_code());
    sb_lang_set("en");
    ui_apply_language();
    acc_fixture(0, 0, 0, 0, 0); /* ohne Pilot: Startweg ohne loc_lobby_pilot */
    g_game = LGAME_LOC;
    loctab_nav_layout();
    g_loc_online = 0;
    g_onl_fix_srv = 0;
    g_onl_fixture = 0;
    g_onl_since = 1;
    strcpy_s(g_onl_status_test, sizeof g_onl_status_test, stp);
    u8_DeleteFile(stp);
    onl_sync_ui();
    ui_settext(ctl(ID_LOC_CODE), "");
    show_page(P_LNET); /* W397: Match auf LoC > Multiplayer */
    pump();
    /* ohne Dienst: grau, Text, Klick aendert nichts */
    if (!why[0] && (!acc_gt_vis(ID_LOC_VIS) || !acc_gt_vis(ID_LOC_CODE) || acc_gt_vis(ID_LOC_CODE_COPY) ||
                    IsWindowEnabled(ctl(ID_NETM0 + NETM_STEEL)) || IsWindowEnabled(ctl(ID_LOC_CODE))))
        strcpy_s(why, sizeof why, "ohne Dienst: Schalter/Feld nicht grau bzw. Sichtbarkeit");
    if (!why[0] && (onl_info(t, sizeof t, &col) != 0 || strcmp(t, LT("gui.loc_onl_nosrv")) != 0))
        _snprintf_s(why, sizeof why, _TRUNCATE, "ohne Dienst: Infozeile '%s'", t);
    if (!why[0]) {
        on_command(ID_NETM0 + NETM_STEEL, BN_CLICKED);
        if (g_loc_online || ui_checked(ID_NETM0 + NETM_STEEL)) strcpy_s(why, sizeof why, "ohne Dienst: steelnet laesst sich waehlen");
    }
    /* mit Dienst: Schalter an -> gemerkt (Doppelte weg, fremde Zeilen bleiben) */
    if (!why[0]) {
        g_onl_fix_srv = 1;
        onl_sync_ui();
        netm_sync_ui();
        on_command(ID_NETM0 + NETM_STEEL, BN_CLICKED); /* W397: Stufe "Mit steelnet verbinden" */
        gt_read(cfg, buf, sizeof buf);
        if (!g_loc_online || !ui_checked(ID_NETM0 + NETM_STEEL) || !IsWindowEnabled(ctl(ID_LOC_CODE)) ||
            acc_gt_vis(ID_MP_GOSET))
            strcpy_s(why, sizeof why, "steelnet an");
        else if (!strstr(buf, "launcher_loc_online=1\n") || strstr(strstr(buf, "launcher_loc_online=") + 1, "launcher_loc_online=") ||
                 !strstr(buf, "# fremde Zeile\n") || !strstr(buf, "width=1280\n"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "user.cfg: %.120s", buf);
        if (!why[0]) {
            g_loc_online = 0;
            onl_load();
            if (!g_loc_online) strcpy_s(why, sizeof why, "launcher_loc_online nicht gelesen");
        }
    }
    /* W373 LOC-PUBLIC-LOBBY: Standard oeffentlich (Vertrauenshinweis), Wahl privat gemerkt und gelesen */
    if (!why[0]) {
        if (!g_loc_public || !ctl(ID_LOC_VIS) || ComboBox_GetCurSel(ctl(ID_LOC_VIS)) != 0 ||
            !IsWindowEnabled(ctl(ID_LOC_VIS)) || !acc_gt_vis(ID_LOC_VIS) || ComboBox_GetCount(ctl(ID_LOC_VIS)) != 2)
            strcpy_s(why, sizeof why, "W373: Standard nicht oeffentlich bzw. Auswahl fehlt/grau");
        else if (onl_info(t, sizeof t, &col) != 0 || strcmp(t, LT("gui.loc_onl_trust")) != 0 || col != C_WARN ||
                 !strstr(t, "don't know"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "W373: Vertrauenshinweis oeffentlich '%s'", t);
        else {
            ui_settext(ctl(ID_LOC_CODE), "7K3M-Q9TX"); /* Code eingegeben: tritt privat bei, Hinweis dazu */
            if (onl_info(t, sizeof t, &col) != 0 || strcmp(t, LT("gui.loc_code_hint_pub")) != 0)
                _snprintf_s(why, sizeof why, _TRUNCATE, "W373: Hinweis mit Code '%s'", t);
            ui_settext(ctl(ID_LOC_CODE), "");
        }
    }
    if (!why[0]) {
        ComboBox_SetCurSel(ctl(ID_LOC_VIS), 1);
        on_command(ID_LOC_VIS, CBN_SELCHANGE);
        gt_read(cfg, buf, sizeof buf);
        if (g_loc_public || !strstr(buf, "launcher_loc_public=0\n") || !strstr(buf, "launcher_loc_online=1\n"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "W373: privat nicht gemerkt: %.120s", buf);
        else {
            g_loc_public = 1;
            onl_load();
            if (g_loc_public || !g_loc_online) strcpy_s(why, sizeof why, "W373: launcher_loc_public=0 nicht gelesen");
            onl_sync_ui();
            if (!why[0] && ComboBox_GetCurSel(ctl(ID_LOC_VIS)) != 1) strcpy_s(why, sizeof why, "W373: Auswahl nach Laden");
        }
    }
    /* Hinweis, ungueltige Eingabe */
    if (!why[0] && (onl_info(t, sizeof t, &col) != 0 || strcmp(t, LT("gui.loc_code_hint")) != 0))
        _snprintf_s(why, sizeof why, _TRUNCATE, "Hinweis '%s'", t);
    if (!why[0]) {
        ui_settext(ctl(ID_LOC_CODE), "7K3M-Q9TU");
        if (onl_info(t, sizeof t, &col) != 0 || strcmp(t, LT("gui.loc_code_bad")) != 0 || col != C_WARN)
            _snprintf_s(why, sizeof why, _TRUNCATE, "ungueltig '%s'", t);
        ui_settext(ctl(ID_LOC_CODE), "");
    }
    /* Status: eigener Raumcode + Kopieren, dann Sicherheitsmeldungen */
    if (!why[0]) {
        onl_gt_status(stp, "lobby=lobby\nsec=ok\nlobby_code=7K3MQ9TX\n");
        show_page(P_LNET); /* W397: Kopieren auf Multiplayer */
        pump();
        _snprintf_s(want, sizeof want, _TRUNCATE, LT("gui.loc_onl_code"), "7K3M-Q9TX");
        if (onl_info(t, sizeof t, &col) != 1 || strcmp(t, want) != 0 || !acc_gt_vis(ID_LOC_CODE_COPY))
            _snprintf_s(why, sizeof why, _TRUNCATE, "eigener Raumcode '%s'", t);
        else {
            on_command(ID_LOC_CODE_COPY, BN_CLICKED); /* Testbetrieb: Zwischenablage bleibt unberuehrt */
            if (strcmp(g_status, LT("gui.loc_code_copied")) != 0) strcpy_s(why, sizeof why, "Kopieren");
        }
    }
    for (i = 0; i < 4 && !why[0]; i++) {
        char txt[200];
        _snprintf_s(txt, sizeof txt, _TRUNCATE, "lobby=getrennt\nsec=%s\nlobby_code=7K3M-Q9TX\n", kSecIn[i]);
        onl_gt_status(stp, txt);
        if (onl_status_refresh() != 1) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "%s: keine Aenderung erkannt", kSecIn[i]);
            break;
        }
        ui_refresh_visibility();
        if (onl_info(t, sizeof t, &col) != 0 || strcmp(t, LT(kSecKey[i])) != 0 || acc_gt_vis(ID_LOC_CODE_COPY))
            _snprintf_s(why, sizeof why, _TRUNCATE, "%s: '%s'", kSecIn[i], t);
    }
    /* alter Lauf (Datei vor g_onl_since) zaehlt nicht */
    if (!why[0]) {
        onl_gt_status(stp, "sec=ok\nlobby_code=7K3M-Q9TX\n");
        g_onl_since = onl_now_ft() + 3600ULL * 10000000ULL;
        onl_status_refresh();
        if (g_onl_code[0] || g_onl_sec[0]) strcpy_s(why, sizeof why, "alte Statusdatei angezeigt");
        g_onl_since = 1;
    }
    /* e: Startweg (Trockenlauf, Pruef-Fixture, Client "bereit") */
    if (!why[0]) {
        strcpy_s(g_locfix_path, sizeof g_locfix_path, LOC_ISO);
        memset(&g_locfix, 0, sizeof g_locfix);
        g_locfix.kind = LGAME_IMG_LOC;
        g_locfix.title_id = LGAME_TID_LOC;
        ui_settext(ctl(ID_LOC_IMAGE), LOC_ISO);
        loctab_check();
        gt_setenv("SB_TEST_LOC_READY", "1");
        ui_settext(ctl(ID_LOC_CODE), "7k3m q9tx");
        on_command(ID_LOC_START, BN_CLICKED);
        _snprintf_s(want, sizeof want, _TRUNCATE, "loc_transport=udp;loc_lobby=%s:%d;loc_lobby_code=7K3M-Q9TX",
                    SB_STEELNET_HOST, (int)SB_STEELNET_PORT);
        if (!strstr(g_loc_last_cmd, want) || !strstr(g_loc_last_cmd, LOC_ISO))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Start mit Code '%.200s'", g_loc_last_cmd);
        if (!why[0]) {
            const char *q;
            ui_settext(ctl(ID_LOC_CODE), "");
            on_command(ID_LOC_START, BN_CLICKED);
            q = strstr(g_loc_last_cmd, "loc_lobby_code=");
            if (!strstr(g_loc_last_cmd, "loc_transport=udp") || !q || (q[15] != ';' && q[15] != '"') ||
                !strstr(g_loc_last_cmd, "loc_rb_input=1") || !strstr(g_loc_last_cmd, "loc_boot=1")) /* W353 */
                _snprintf_s(why, sizeof why, _TRUNCATE, "Start eigene Lobby '%.200s'", g_loc_last_cmd);
        }
        if (!why[0]) {
            ui_settext(ctl(ID_LOC_CODE), "ABC");
            set_status_k("x", 0);
            on_command(ID_LOC_START, BN_CLICKED);
            if (g_loc_last_cmd[0] || strcmp(g_status, LT("gui.loc_code_bad")) != 0)
                _snprintf_s(why, sizeof why, _TRUNCATE, "ungueltiger Code gestartet '%.120s'", g_loc_last_cmd);
        }
        if (!why[0]) {
            g_loc_online = 0;
            onl_sync_ui();
            on_command(ID_LOC_START, BN_CLICKED); /* aus: Code im Feld egal, kein Online */
            if (!g_loc_last_cmd[0] || strstr(g_loc_last_cmd, "loc_transport") || strstr(g_loc_last_cmd, "loc_lobby"))
                _snprintf_s(why, sizeof why, _TRUNCATE, "Start ohne Online '%.200s'", g_loc_last_cmd);
        }
        SetEnvironmentVariableA("SB_TEST_LOC_READY", NULL);
        g_loc_last_cmd[0] = '\0';
    }
    /* f: Sprachwechsel (Relabel) */
    if (!why[0]) {
        sb_lang_set("de");
        ui_apply_language();
        ui_gettext(ctl(ID_NETM0 + NETM_STEEL), t, sizeof t); /* W397: Stufe statt Schalter */
        if (strncmp(t, "Mit steelnet verbinden", 22) != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "de Stufe '%s'", t);
        ui_gettext(ctl(ID_LOC_CODE_COPY), t, sizeof t);
        if (!why[0] && strcmp(t, LT("gui.loc_code_copy")) != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "de Kopieren '%s'", t);
    }
    /* Aufraeumen */
    ui_settext(ctl(ID_LOC_CODE), "");
    u8_DeleteFile(stp);
    u8_DeleteFile(cfg);
    g_ucfg_test[0] = '\0';
    g_onl_status_test[0] = '\0';
    g_onl_fix_srv = -1;
    g_onl_sec[0] = g_onl_code[0] = g_onl_err[0] = '\0';
    g_onl_since = 0;
    g_loc_online = on0;
    g_upd_cfg = uc0; /* W397: Stufe steelnet setzt auch update_check (nur im Speicher, Testbetrieb) */
    g_loc_public = 1; /* W373: Fixture-Zustand nicht stehen lassen (echte Wahl liest onl_load) */
    g_loc = loc0;
    g_locfix_path[0] = '\0';
    g_loc_checked = 0;
    g_acc_fixture = 0;
    g_acc_loaded = 0;
    g_acc = a0;
    g_game = g0;
    onl_sync_ui();
    loctab_nav_layout();
    show_page(p0);
    sb_lang_set(keep);
    ui_apply_language();
    if (!why[0]) {
        GT_OK("77 W334 LOC-SEC online card (room code normalisation, --set with/without code, loc_net_status.txt sec=/lobby_code=, "
              "greyed without service, saved in user.cfg, own code + copy, security messages, start path, relabel)");
    } else {
        GT_FAIL("77 W334 LOC-SEC online card", "%s", why);
    }
}

/* --- 77b (W387 LOC-SMOOTH-OPP): Auswahl "Gegner glaetten" auf LoC-Spielen: Standard Auto (Index 1, Schluessel
 * fehlt/ungueltig = 1), Wahl 0/2 gemerkt in einer Temp-user.cfg und gelesen, loc_smooth_opp=N in jedem LoC-Start
 * (Trockenlauf ohne/mit Online, intern auch Testpartner-Nutzer-Spiel), Tooltip, Relabel DE. SB-Start ohne den
 * Schluessel prueft Fall 66 (Einmal-Start-Kette). --- */
__declspec(noinline) void smooth_gui_selftest(int *pass, int *fail)
{
    static const struct {
        const char *in;
        int out;
    } kP[] = { { "0", 0 }, { "1", 1 }, { "2", 2 }, { "2\n", 2 }, { "0\r\n", 0 }, { "3", 1 },
               { "-1", 1 }, { "abc", 1 }, { "", 1 }, { "1x", 1 }, { "99", 1 } };
    char why[300] = "", tmp[1024], cfg[1200], buf[4096], t[400], keep[16], want[300];
    int i, g0 = g_game, p0 = g_page, on0 = g_loc_online;
    lgame_state_t loc0 = g_loc;
    lacc_store_t a0 = g_acc;
    HWND c = ctl(ID_LOC_SMOOTH);
    if (!c) {
        GT_FAIL("77b W387 LOC-SMOOTH-OPP", "%s", "ID_LOC_SMOOTH fehlt");
        return;
    }
    /* a: Wert aus user.cfg */
    for (i = 0; i < (int)(sizeof kP / sizeof kP[0]) && !why[0]; i++) {
        if (loc_smooth_parse(kP[i].in) != kP[i].out)
            _snprintf_s(why, sizeof why, _TRUNCATE, "parse '%s' -> %d (soll %d)", kP[i].in, loc_smooth_parse(kP[i].in),
                        kP[i].out);
    }
    /* b: --set-Baustein */
    if (!why[0]) {
        char s[64] = "";
        loc_smooth_set_append(s, sizeof s, 1);
        if (strcmp(s, "loc_smooth_opp=1") != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "set leer '%s'", s);
        strcpy_s(s, sizeof s, "vr_enable=0");
        loc_smooth_set_append(s, sizeof s, 2);
        if (!why[0] && strcmp(s, "vr_enable=0;loc_smooth_opp=2") != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "set 2 '%s'", s);
        strcpy_s(s, sizeof s, "a=1");
        loc_smooth_set_append(s, sizeof s, 7);
        if (!why[0] && strcmp(s, "a=1;loc_smooth_opp=1") != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "set ungueltig '%s'", s);
    }
    /* c: Fenster (Temp-user.cfg ohne Schluessel -> Auto) */
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw387_gui_user_%lu.cfg", tmp, GetCurrentProcessId());
    write_file(cfg, "[display]\nwidth=1280\n# fremde Zeile\nlauncher_loc_online=0\n");
    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, cfg);
    strcpy_s(keep, sizeof keep, sb_lang_code());
    sb_lang_set("en");
    ui_apply_language();
    acc_fixture(0, 0, 0, 0, 0);
    gt_setenv("SB_TEST_LOC_READY", "1");
    g_game = LGAME_LOC;
    loctab_nav_layout();
    g_onl_fix_srv = 0;
    g_onl_fixture = 0;
    g_loc_smooth_opp = 0; /* onl_load muss auf 1 setzen */
    onl_load();
    onl_sync_ui();
    show_page(P_LNET); /* W392: Gegner glaetten liegt jetzt auf der Seite Netzwerk */
    pump();
    if (!why[0] && (g_loc_smooth_opp != 1 || ComboBox_GetCount(c) != LOC_SMOOTH_N || ComboBox_GetCurSel(c) != 1 ||
                    !IsWindowEnabled(c)))
        _snprintf_s(why, sizeof why, _TRUNCATE, "Standard: Wert %d, %d Eintraege, Auswahl %d", g_loc_smooth_opp,
                    ComboBox_GetCount(c), ComboBox_GetCurSel(c));
    loctab_vis(ID_LOC_SMOOTH, 1, "Gegner glaetten (LoC Netzwerk)", why, sizeof why);
    if (!why[0]) {
        wchar_t wt[120] = L"";
        SendMessageW(c, CB_GETLBTEXT, 1, (LPARAM)wt);
        WideCharToMultiByte(CP_UTF8, 0, wt, -1, t, (int)sizeof t, NULL, NULL);
        if (strcmp(t, LT("gui.loc_smooth_auto")) != 0 || strcmp(t, "Auto (recommended)") != 0) /* W473: nur der Wert */
            _snprintf_s(why, sizeof why, _TRUNCATE, "Eintrag Auto '%s'", t);
    }
    if (!why[0]) {
        xtip_text(ID_LOC_SMOOTH, t, sizeof t);
        if (strcmp(t, LT("gui.loc_smooth_tip")) != 0 || !strstr(t, "network corrections"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Tooltip '%s'", t);
    }
    if (!why[0]) { /* ungueltiger Wert = Auto */
        write_file(cfg, "launcher_loc_smooth_opp=7\n");
        g_loc_smooth_opp = 0;
        onl_load();
        if (g_loc_smooth_opp != 1) _snprintf_s(why, sizeof why, _TRUNCATE, "ungueltig gelesen als %d", g_loc_smooth_opp);
        write_file(cfg, "[display]\nwidth=1280\n# fremde Zeile\nlauncher_loc_smooth_opp=1\nlauncher_loc_smooth_opp=1\n");
    }
    /* d: Wahl 0 und 2 gemerkt (Doppelte weg, fremde Zeilen bleiben) und gelesen */
    for (i = 0; i < 2 && !why[0]; i++) {
        int v = i ? 2 : 0;
        const char *p;
        ComboBox_SetCurSel(c, v);
        on_command(ID_LOC_SMOOTH, CBN_SELCHANGE);
        gt_read(cfg, buf, sizeof buf);
        _snprintf_s(want, sizeof want, _TRUNCATE, "launcher_loc_smooth_opp=%d\n", v);
        p = strstr(buf, want);
        if (g_loc_smooth_opp != v || !p || strstr(p + 1, "launcher_loc_smooth_opp=") || !strstr(buf, "# fremde Zeile\n") ||
            !strstr(buf, "width=1280\n"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Wahl %d nicht gemerkt: %.120s", v, buf);
        if (!why[0]) {
            _snprintf_s(want, sizeof want, _TRUNCATE, LT("gui.loc_smooth_set"), LT(kLocSmooth[v]));
            if (strcmp(g_status, want) != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "Status '%.120s'", g_status);
        }
        if (!why[0]) {
            g_loc_smooth_opp = 1;
            onl_load();
            onl_sync_ui();
            if (g_loc_smooth_opp != v || ComboBox_GetCurSel(c) != v)
                _snprintf_s(why, sizeof why, _TRUNCATE, "Wahl %d nach Laden: %d / Auswahl %d", v, g_loc_smooth_opp,
                            ComboBox_GetCurSel(c));
        }
    }
    /* e: Startweg (Trockenlauf): jeder LoC-Start traegt loc_smooth_opp=N (Standard 1, Wahl 2, auch online) */
    if (!why[0]) {
        strcpy_s(g_locfix_path, sizeof g_locfix_path, LOC_ISO);
        memset(&g_locfix, 0, sizeof g_locfix);
        g_locfix.kind = LGAME_IMG_LOC;
        g_locfix.title_id = LGAME_TID_LOC;
        ui_settext(ctl(ID_LOC_IMAGE), LOC_ISO);
        loctab_check();
        g_loc_online = 0;
        g_loc_smooth_opp = 1;
        onl_sync_ui();
        ui_settext(ctl(ID_LOC_CODE), "");
        on_command(ID_LOC_START, BN_CLICKED);
        if (!strstr(g_loc_last_cmd, "loc_smooth_opp=1") || !strstr(g_loc_last_cmd, "loc_boot=1") ||
            strstr(g_loc_last_cmd, "loc_transport"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Start Standard '%.200s'", g_loc_last_cmd);
        if (!why[0]) {
            ComboBox_SetCurSel(c, 2);
            on_command(ID_LOC_SMOOTH, CBN_SELCHANGE);
            on_command(ID_LOC_START, BN_CLICKED);
            if (!strstr(g_loc_last_cmd, "loc_smooth_opp=2") || strstr(g_loc_last_cmd, "loc_smooth_opp=1"))
                _snprintf_s(why, sizeof why, _TRUNCATE, "Start Wahl 2 '%.200s'", g_loc_last_cmd);
        }
        if (!why[0]) { /* online (Dienst als eingerichtet vorgegeben, eigene Lobby) */
            g_onl_fix_srv = 1;
            g_loc_online = 1;
            onl_sync_ui();
            on_command(ID_LOC_START, BN_CLICKED);
            if (!strstr(g_loc_last_cmd, "loc_smooth_opp=2") || !strstr(g_loc_last_cmd, "loc_transport=udp"))
                _snprintf_s(why, sizeof why, _TRUNCATE, "Start online '%.200s'", g_loc_last_cmd);
            g_loc_online = 0;
            g_onl_fix_srv = 0;
        }
#if defined(SB_HAVE_LOCTP) && SB_HAVE_LOCTP
        if (!why[0]) { /* Testpartner: Nutzer-Spiel mit der Wahl, Partner ohne */
            static loctp_cfg_t tc;
            static char ts[4096];
            loctp_link_t lk;
            char err[300] = "";
            memset(&lk, 0, sizeof lk);
            lk.port_partner = 47100;
            lk.port_user = 47101;
            if (loctp_cfg_from_ui(&tc, err, sizeof err) != 0) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Testpartner-Konfiguration: %s", err);
            } else if (loctp_build_set(&tc, 1, 0, &lk, "", ts, sizeof ts) != 0 || !strstr(ts, ";loc_smooth_opp=2")) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Testpartner Nutzer-Spiel '%.200s'", ts);
            } else if (loctp_build_set(&tc, 0, 0, &lk, "", ts, sizeof ts) != 0 || strstr(ts, "loc_smooth_opp")) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Testpartner Partner '%.200s'", ts);
            }
        }
#endif
        g_loc_last_cmd[0] = '\0';
    }
    /* f: Relabel DE (Auswahl bleibt) */
    if (!why[0]) {
        wchar_t wt[120] = L"";
        sb_lang_set("de");
        ui_apply_language();
        SendMessageW(c, CB_GETLBTEXT, 1, (LPARAM)wt);
        WideCharToMultiByte(CP_UTF8, 0, wt, -1, t, (int)sizeof t, NULL, NULL);
        if (strcmp(t, "Auto (empfohlen)") != 0 || ComboBox_GetCurSel(c) != 2) /* W473: nur der Wert */
            _snprintf_s(why, sizeof why, _TRUNCATE, "de Eintrag '%s' / Auswahl %d", t, ComboBox_GetCurSel(c));
    }
    /* g: W392: auf der Seite Netzwerk auch ohne startbaren Client sichtbar (Einstellung, frueher unter dem
     * gesperrten Start ausgeblendet) */
    if (!why[0]) {
        gt_setenv("SB_TEST_LOC_READY", "0");
        ui_refresh_visibility();
        loctab_vis(ID_LOC_SMOOTH, 1, "Gegner glaetten ohne startbaren Client (Netzwerk)", why, sizeof why);
    }
    /* Aufraeumen */
    SetEnvironmentVariableA("SB_TEST_LOC_READY", NULL);
    u8_DeleteFile(cfg);
    g_ucfg_test[0] = '\0';
    g_onl_fix_srv = -1;
    g_loc_online = on0;
    g_loc_smooth_opp = 1; /* echte Wahl liest onl_load */
    g_loc = loc0;
    g_locfix_path[0] = '\0';
    g_loc_checked = 0;
    g_acc_fixture = 0;
    g_acc_loaded = 0;
    g_acc = a0;
    g_game = g0;
    onl_sync_ui();
    loctab_nav_layout();
    show_page(p0);
    sb_lang_set(keep);
    ui_apply_language();
    if (!why[0]) {
        GT_OK("77b W387 LOC-SMOOTH-OPP smooth opponents (default Auto, key missing/invalid = 1, 0/2 saved + read, "
              "loc_smooth_opp=N in every LoC start incl. online (internal: test partner user game), tooltip, relabel, "
              "W392: on the Network page, also without a startable client)");
    } else {
        GT_FAIL("77b W387 LOC-SMOOTH-OPP", "%s", why);
    }
}

/* --- 77c (W392 LAUNCHER-NETTAB): Seite Netzwerk im LoC-Bereich. a Lesen (fehlt/ungueltig = an), b loc_rb_opts-
 * Zusammenfuehrung (vorhandene Optionen bleiben, lowd ersetzt, 127-Zeichen-Grenze), c Standard im Fenster, d Schalter
 * speichern/laden (Temp-user.cfg), e Ausgrauen ohne Rollback, f Uebergabe an jeden LoC-Start (lokal, online, Nutzer-Spiel
 * des Testpartners; Partner nie), g nie an Steel Battalion (Trockenlauf), h Standard wiederherstellen, i Sprache,
 * j SB-Bereich zeigt auf dem Platz weiter Packs. */
__declspec(noinline) void lnet_gui_selftest(int *pass, int *fail)
{
    static const struct {
        const char *in;
        int out;
    } kP[] = { { "0", 0 }, { "1", 1 }, { "0\r\n", 0 }, { "1\n", 1 }, { "2", 1 }, { "-1", 1 }, { "abc", 1 }, { "", 1 },
               { "0x", 1 } };
    static const struct {
        const char *base;
        int lowd, ret;
        const char *out;
    } kM[] = {
        { "", 1, 0, "" },
        { "lowd=0;hot=1", 1, 0, "" }, /* an: nichts anhaengen, der Client behaelt seinen Wert */
        { "", 0, 1, "lowd=0" },
        { "k=1,2;render=0", 0, 1, "k=1,2\\;render=0\\;lowd=0" },
        { "lowd=1;hot=1", 0, 1, "hot=1\\;lowd=0" },
        { " ;hot=1; ;lowd=1;cool=2;", 0, 1, "hot=1\\;cool=2\\;lowd=0" },
    };
    char why[300] = "", tmp[1024], cfg[1200], buf[4096], out[300], s[1024], keep[16];
    int i, g0 = g_game, p0 = g_page, on0 = g_loc_online;
    lgame_state_t loc0 = g_loc;
    lacc_store_t a0 = g_acc;
    if (!ctl(ID_LNET_RB) || !ctl(ID_LNET_OWN) || !ctl(ID_LNET_LOWD) || !ctl(ID_LNET_RESET) || !ctl(ID_LOC_SMOOTH)) {
        GT_FAIL("77c W392 LAUNCHER-NETTAB", "%s", "Elemente fehlen");
        return;
    }
    /* a */
    for (i = 0; i < (int)(sizeof kP / sizeof kP[0]) && !why[0]; i++) {
        if (lnet_bool_parse(kP[i].in) != kP[i].out)
            _snprintf_s(why, sizeof why, _TRUNCATE, "parse '%s' -> %d", kP[i].in, lnet_bool_parse(kP[i].in));
    }
    /* b */
    for (i = 0; i < (int)(sizeof kM / sizeof kM[0]) && !why[0]; i++) {
        int r = lnet_rbopts_merge(kM[i].base, kM[i].lowd, out, sizeof out);
        if (r != kM[i].ret || strcmp(out, kM[i].out) != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "merge '%s' lowd=%d -> %d '%s'", kM[i].base, kM[i].lowd, r, out);
    }
    if (!why[0]) { /* zu lang: Restoptionen fallen von hinten weg, unmaskiert <= 127, lowd=0 am Ende */
        char big[400] = "", one[16], *q;
        size_t raw = 0;
        for (i = 0; i < 30; i++) {
            _snprintf_s(one, sizeof one, _TRUNCATE, "%sopt%02d=1", i ? ";" : "", i);
            strcat_s(big, sizeof big, one);
        }
        lnet_rbopts_merge(big, 0, out, sizeof out);
        for (q = out; *q; q++) {
            raw += !(q[0] == '\\' && q[1] == ';');
        }
        q = strstr(out, "lowd=0");
        if (raw > LNET_RBOPTS_MAX || raw < LNET_RBOPTS_MAX - 12 || !q || q[6] || strncmp(out, "opt00=1\\;", 9) != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "merge lang: %u Zeichen '%.80s'", (unsigned)raw, out);
    }
    if (!why[0] && (!lnet_set_last("a=1;loc_rb_opts=x=1\\;y=2;b=3;loc_rb_opts=z=4\\;w=5;c=1", "loc_rb_opts", out,
                                   sizeof out) || strcmp(out, "z=4;w=5") != 0 ||
                    lnet_set_last("a=1;xloc_rb_opts=1", "loc_rb_opts", out, sizeof out)))
        _snprintf_s(why, sizeof why, _TRUNCATE, "set_last '%s'", out);
    /* c: Fenster mit Temp-user.cfg ohne Schluessel */
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw392_gui_user_%lu.cfg", tmp, GetCurrentProcessId());
    write_file(cfg, "[display]\nwidth=1280\n# fremde Zeile\nlauncher_loc_online=0\n");
    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, cfg);
    strcpy_s(keep, sizeof keep, sb_lang_code());
    sb_lang_set("en");
    ui_apply_language();
    acc_fixture(0, 0, 0, 0, 0);
    gt_setenv("SB_TEST_LOC_READY", "1");
    g_game = LGAME_LOC;
    loctab_nav_layout();
    g_onl_fix_srv = 0;
    g_onl_fixture = 0;
    g_loc_online = 0;
    g_loc_rb_on = g_loc_own_pred = g_loc_lowd = 0; /* onl_load muss auf 1 setzen */
    g_loc_smooth_opp = 0;
    onl_load();
    show_page(P_LNET);
    pump();
    if (!why[0] && (g_page != P_LNET || !g_loc_rb_on || !g_loc_own_pred || !g_loc_lowd || g_loc_smooth_opp != 1 ||
                    !lnet_rb_ui() || !ui_checked(ID_LNET_OWN) || !ui_checked(ID_LNET_LOWD) ||
                    !IsWindowEnabled(ctl(ID_LNET_OWN)) || !IsWindowEnabled(ctl(ID_LNET_LOWD)) ||
                    !IsWindowEnabled(ctl(ID_LOC_SMOOTH)) || IsWindowEnabled(ctl(ID_LNET_RESET))))
        _snprintf_s(why, sizeof why, _TRUNCATE, "Standard: Seite %d rb %d own %d lowd %d smooth %d", g_page, g_loc_rb_on,
                    g_loc_own_pred, g_loc_lowd, g_loc_smooth_opp);
    loctab_vis(ID_LNET_RB, 1, "Netzwerk Rollback", why, sizeof why);
    loctab_vis(ID_LNET_OWN, 1, "Netzwerk eigene Bewegung", why, sizeof why);
    loctab_vis(ID_LNET_LOWD, 1, "Netzwerk niedrige Verzoegerung", why, sizeof why);
    loctab_vis(ID_LNET_RESET, 1, "Netzwerk Standard", why, sizeof why);
    loctab_vis(ID_LOC_SMOOTH, 1, "Netzwerk Gegner glaetten", why, sizeof why);
    loctab_vis(ID_PK_LIST, 0, "Netzwerk ohne Pack-Liste", why, sizeof why);
    if (!why[0]) { /* Standard-Kette (lowd an: kein loc_rb_opts) */
        strcpy_s(s, sizeof s, "vr_enable=0");
        loc_net_set_append(s, sizeof s);
        if (strcmp(s, "vr_enable=0;loc_rb_input=1;loc_own_predict=1;loc_smooth_opp=1") != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "Kette Standard '%s'", s);
    }
    /* d/e: Rollback aus -> gemerkt, Rest grau, Klick auf grauen Schalter aendert nichts */
    if (!why[0]) {
        on_command(ID_LNET_RB, BN_CLICKED);
        pump();
        gt_read(cfg, buf, sizeof buf);
        if (g_loc_rb_on || lnet_rb_ui() || !strstr(buf, "launcher_loc_rollback=0\n") ||
            !strstr(buf, "# fremde Zeile\n"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Rollback aus nicht gemerkt: %.120s", buf);
        else if (IsWindowEnabled(ctl(ID_LNET_OWN)) || IsWindowEnabled(ctl(ID_LNET_LOWD)) ||
                 IsWindowEnabled(ctl(ID_LOC_SMOOTH)) || !IsWindowEnabled(ctl(ID_LNET_RESET)) ||
                 !IsWindowEnabled(ctl(ID_LNET_RB)))
            strcpy_s(why, sizeof why, "Rollback aus: Ausgrauen falsch");
        if (!why[0]) {
            on_command(ID_LNET_OWN, BN_CLICKED);
            if (!g_loc_own_pred || !ui_checked(ID_LNET_OWN))
                strcpy_s(why, sizeof why, "grauer Schalter liess sich umlegen");
        }
        if (!why[0]) {
            g_loc_rb_on = 1;
            onl_load();
            onl_sync_ui();
            if (g_loc_rb_on || lnet_rb_ui() || IsWindowEnabled(ctl(ID_LNET_OWN)))
                strcpy_s(why, sizeof why, "Rollback aus nach Laden");
        }
        if (!why[0]) {
            s[0] = '\0';
            loc_net_set_append(s, sizeof s);
            if (strcmp(s, "loc_rb_input=0;loc_own_predict=1;loc_smooth_opp=1") != 0)
                _snprintf_s(why, sizeof why, _TRUNCATE, "Kette Rollback aus '%s'", s);
        }
        if (!why[0]) { /* wieder an, dann eigene Bewegung + niedrige Verzoegerung aus */
            on_command(ID_LNET_RB, BN_CLICKED);
            on_command(ID_LNET_OWN, BN_CLICKED);
            on_command(ID_LNET_LOWD, BN_CLICKED);
            gt_read(cfg, buf, sizeof buf);
            if (!g_loc_rb_on || g_loc_own_pred || g_loc_lowd || !strstr(buf, "launcher_loc_rollback=1\n") ||
                !strstr(buf, "launcher_loc_own_predict=0\n") || !strstr(buf, "launcher_loc_low_delay=0\n") ||
                strstr(strstr(buf, "launcher_loc_rollback=") + 1, "launcher_loc_rollback="))
                _snprintf_s(why, sizeof why, _TRUNCATE, "Schalter gemerkt: %.160s", buf);
        }
        if (!why[0]) {
            g_loc_own_pred = g_loc_lowd = 1;
            onl_load();
            onl_sync_ui();
            if (g_loc_own_pred || g_loc_lowd || ui_checked(ID_LNET_OWN) || ui_checked(ID_LNET_LOWD))
                strcpy_s(why, sizeof why, "eigene Bewegung/niedrige Verzoegerung nach Laden");
        }
        if (!why[0]) { /* ungueltig = an */
            write_file(cfg, "launcher_loc_rollback=5\nlauncher_loc_own_predict=x\nlauncher_loc_low_delay=\n");
            onl_load();
            if (!g_loc_rb_on || !g_loc_own_pred || !g_loc_lowd)
                strcpy_s(why, sizeof why, "ungueltige Werte nicht als an gelesen");
        }
    }
    /* b2: lowd aus mit vorhandenem loc_rb_opts (W402: user_loc.cfg bzw. Kette) */
    if (!why[0]) {
        char lcf2[1200];
        lgame_cfg_path(cfg, LGAME_LOC, lcf2, sizeof lcf2);
        write_file(cfg, "loc_rb_opts=k=9\nlauncher_loc_low_delay=0\n"); /* SB-Datei zaehlt fuer LoC nicht */
        write_file(lcf2, "loc_rb_opts=k=1,2;hot=1\n");
        onl_load();
        strcpy_s(s, sizeof s, "vr_enable=0");
        loc_net_set_append(s, sizeof s);
        u8_DeleteFile(lcf2);
        if (strcmp(s, "vr_enable=0;loc_rb_input=1;loc_own_predict=1;loc_smooth_opp=1;loc_rb_opts=k=1,2\\;hot=1\\;lowd=0") != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "Kette lowd aus (user_loc.cfg) '%s'", s);
        strcpy_s(s, sizeof s, "loc_rb_opts=cool=3\\;lowd=1");
        loc_net_set_append(s, sizeof s);
        if (!why[0] && !strstr(s, ";loc_rb_opts=cool=3\\;lowd=0"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Kette lowd aus (Kette) '%s'", s);
    }
    /* f: Startweg (Trockenlauf) mit Rollback aus, eigene Bewegung aus, lowd aus, Gegner glaetten Fest */
    if (!why[0]) {
        write_file(cfg, "# fremde Zeile\n");
        onl_load();
        on_command(ID_LNET_OWN, BN_CLICKED);
        on_command(ID_LNET_LOWD, BN_CLICKED);
        ComboBox_SetCurSel(ctl(ID_LOC_SMOOTH), 2);
        on_command(ID_LOC_SMOOTH, CBN_SELCHANGE);
        on_command(ID_LNET_RB, BN_CLICKED);
        strcpy_s(g_locfix_path, sizeof g_locfix_path, LOC_ISO);
        memset(&g_locfix, 0, sizeof g_locfix);
        g_locfix.kind = LGAME_IMG_LOC;
        g_locfix.title_id = LGAME_TID_LOC;
        ui_settext(ctl(ID_LOC_IMAGE), LOC_ISO);
        loctab_check();
        ui_settext(ctl(ID_LOC_CODE), "");
        on_command(ID_LOC_START, BN_CLICKED);
        if (!strstr(g_loc_last_cmd, "loc_rb_input=0;loc_own_predict=0;loc_smooth_opp=2;loc_rb_opts=lowd=0") ||
            strstr(g_loc_last_cmd, "loc_rb_input=1") || !strstr(g_loc_last_cmd, "loc_boot=1"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Start lokal '%.240s'", g_loc_last_cmd);
        if (!why[0]) { /* online: kein festes loc_rb_input=1 mehr */
            g_onl_fix_srv = 1;
            g_loc_online = 1;
            onl_sync_ui();
            on_command(ID_LOC_START, BN_CLICKED);
            if (!strstr(g_loc_last_cmd, "loc_rb_input=0") || strstr(g_loc_last_cmd, "loc_rb_input=1") ||
                !strstr(g_loc_last_cmd, "loc_transport=udp") || !strstr(g_loc_last_cmd, "loc_own_predict=0"))
                _snprintf_s(why, sizeof why, _TRUNCATE, "Start online '%.240s'", g_loc_last_cmd);
            g_loc_online = 0;
            g_onl_fix_srv = 0;
        }
#if defined(SB_HAVE_LOCTP) && SB_HAVE_LOCTP
        if (!why[0]) { /* Testpartner: Nutzer-Spiel mit den Schaltern, Partner ohne */
            static loctp_cfg_t tc;
            static char ts[4096];
            loctp_link_t lk;
            char err[300] = "";
            memset(&lk, 0, sizeof lk);
            lk.port_partner = 47100;
            lk.port_user = 47101;
            if (loctp_cfg_from_ui(&tc, err, sizeof err) != 0) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Testpartner-Konfiguration: %s", err);
            } else if (loctp_build_set(&tc, 1, 0, &lk, "", ts, sizeof ts) != 0 ||
                       !strstr(ts, ";loc_rb_input=0;loc_own_predict=0;loc_smooth_opp=2;loc_rb_opts=lowd=0")) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Testpartner Nutzer-Spiel '%.240s'", ts);
            } else if (loctp_build_set(&tc, 0, 0, &lk, "", ts, sizeof ts) != 0 || strstr(ts, "loc_rb_input=0") ||
                       strstr(ts, "loc_own_predict") || strstr(ts, "loc_rb_opts")) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Testpartner Partner '%.240s'", ts);
            }
        }
#endif
        g_loc_last_cmd[0] = '\0';
    }
    /* g: Steel Battalion (Trockenlauf) traegt keinen LoC-Netzschluessel, auch mit Nicht-Standard */
    if (!why[0]) {
        g_game = LGAME_SB;
        loctab_nav_layout();
        g_start_args[0] = '\0';
        if (do_start_ex(1, 0, "") == 0 &&
            (strstr(g_start_args, "loc_rb_input") || strstr(g_start_args, "loc_own_predict") ||
             strstr(g_start_args, "loc_smooth_opp") || strstr(g_start_args, "loc_rb_opts")))
            _snprintf_s(why, sizeof why, _TRUNCATE, "SB-Start mit LoC-Netz: %.200s", g_start_args);
        /* j: SB-Bereich: Packs-Platz zeigt Packs, keine Netzwerk-Elemente */
        if (!why[0]) {
            char nt[300];
            show_page(P_PACKS);
            pump();
            ui_gettext(ctl(ID_NAV0 + P_PACKS), nt, sizeof nt);
            if (g_page != P_PACKS || strncmp(nt, LT("gui.page_packs"), strlen(LT("gui.page_packs"))) != 0)
                _snprintf_s(why, sizeof why, _TRUNCATE, "SB Packs-Platz '%s' Seite %d", nt, g_page);
        }
        loctab_vis(ID_LNET_RB, 0, "SB Packs ohne Netzwerk-Schalter", why, sizeof why);
        loctab_vis(ID_LNET_RESET, 0, "SB Packs ohne Netzwerk-Knopf", why, sizeof why);
        loctab_vis(ID_LOC_SMOOTH, 0, "SB Packs ohne Gegner glaetten", why, sizeof why);
        loctab_vis(ID_PK_ADD, 1, "SB Pack hinzufuegen", why, sizeof why);
        g_game = LGAME_LOC;
        loctab_nav_layout();
        show_page(P_LNET);
        pump();
    }
    /* W420: Host-Regel Clip dumping - Standard an, unabhaengig vom Rollback, gemerkt, an den Start */
    lnet_tab_pick(1); /* W473: Host-Regeln und Chat liegen im Reiter "Lobby & chat" */
    if (!why[0]) {
        char s4[200] = "vr_enable=0";
        loc_rule_set_append(s4, sizeof s4);
        if (!g_loc_clip || !ui_checked(ID_LNET_CLIP) || strcmp(s4, "vr_enable=0;loc_rule_clipdump_block=1;loc_chat=1;loc_snchat=1;loc_play_mode=sim") != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "W420 Standard '%s'", s4);
        loctab_vis(ID_LNET_CLIP, 1, "W420 Schalter Clip dumping", why, sizeof why);
        loctab_vis(ID_LNET_VERPAGE, 0, "W420 Release-Knopf ohne Versionsablehnung", why, sizeof why);
        if (!why[0]) {
            on_command(ID_LNET_CLIP, BN_CLICKED);
            gt_read(cfg, buf, sizeof buf);
            strcpy_s(s4, sizeof s4, "vr_enable=0");
            loc_rule_set_append(s4, sizeof s4);
            if (g_loc_clip || ui_checked(ID_LNET_CLIP) || !strstr(buf, "launcher_loc_clipdump_block=0\n") ||
                strcmp(s4, "vr_enable=0;loc_rule_clipdump_block=0;loc_chat=1;loc_snchat=1;loc_play_mode=sim") != 0 || lnet_is_default() ||
                !IsWindowEnabled(ctl(ID_LNET_RESET)))
                _snprintf_s(why, sizeof why, _TRUNCATE, "W420 aus: '%s' / %.120s", s4, buf);
        }
    }
    /* W437: Chat - Standard an, unabhaengig vom Rollback, gemerkt, als loc_chat an den Start */
    if (!why[0]) {
        char s4[200] = "vr_enable=0";
        loctab_vis(ID_LNET_CHAT, 1, "W437 Schalter Chat", why, sizeof why);
        if (!why[0] && (!g_loc_chat || !ui_checked(ID_LNET_CHAT) || !IsWindowEnabled(ctl(ID_LNET_CHAT))))
            _snprintf_s(why, sizeof why, _TRUNCATE, "W437 Chat nicht an");
        if (!why[0]) {
            on_command(ID_LNET_CHAT, BN_CLICKED);
            gt_read(cfg, buf, sizeof buf);
            loc_rule_set_append(s4, sizeof s4);
            if (g_loc_chat || ui_checked(ID_LNET_CHAT) || !strstr(buf, "launcher_loc_chat=0\n") || !strstr(s4, ";loc_chat=0") ||
                lnet_is_default())
                _snprintf_s(why, sizeof why, _TRUNCATE, "W437 Chat aus: '%s' / %.120s", s4, buf);
        }
        if (!why[0]) { /* neu laden: gemerkter Wert gilt */
            g_loc_chat = 1;
            onl_load();
            if (g_loc_chat) _snprintf_s(why, sizeof why, _TRUNCATE, "W437 Chat aus nicht gemerkt");
        }
    }
    /* W446 STEELNET-CHAT: steelnet-Chat - Standard an, getrennt vom Chat-Schalter, unabhaengig vom Rollback, gemerkt,
     * als loc_snchat an den Start */
    if (!why[0]) {
        char s4[200] = "vr_enable=0";
        loctab_vis(ID_LNET_SNCHAT, 1, "W446 Schalter steelnet chat", why, sizeof why);
        if (!why[0] && (!g_loc_snchat || !ui_checked(ID_LNET_SNCHAT) || !IsWindowEnabled(ctl(ID_LNET_SNCHAT))))
            _snprintf_s(why, sizeof why, _TRUNCATE, "W446 steelnet chat nicht an");
        if (!why[0]) {
            int chat_before = g_loc_chat;
            on_command(ID_LNET_SNCHAT, BN_CLICKED);
            gt_read(cfg, buf, sizeof buf);
            loc_rule_set_append(s4, sizeof s4);
            if (g_loc_snchat || ui_checked(ID_LNET_SNCHAT) || !strstr(buf, "launcher_loc_snchat=0\n") ||
                !strstr(s4, ";loc_snchat=0") || lnet_is_default() || g_loc_chat != chat_before)
                _snprintf_s(why, sizeof why, _TRUNCATE, "W446 steelnet chat aus: '%s' / %.120s", s4, buf);
        }
        if (!why[0]) { /* neu laden: gemerkter Wert gilt */
            g_loc_snchat = 1;
            onl_load();
            if (g_loc_snchat) _snprintf_s(why, sizeof why, _TRUNCATE, "W446 steelnet chat aus nicht gemerkt");
        }
        if (!why[0]) { /* zurueck auf an (eigener Schluessel, der Chat-Schalter bleibt wie er ist) */
            on_command(ID_LNET_SNCHAT, BN_CLICKED);
            gt_read(cfg, buf, sizeof buf);
            if (!g_loc_snchat || !strstr(buf, "launcher_loc_snchat=1\n"))
                _snprintf_s(why, sizeof why, _TRUNCATE, "W446 steelnet chat wieder an: %.120s", buf);
        }
    }
    /* W508 CHATBAR: Chat in den Seitenbalken - Standard an (auto, Startzeile unveraendert), gemerkt, aus = ;loc_chat_pos=1 */
    if (!why[0]) {
        char s4[200] = "vr_enable=0";
        loctab_vis(ID_LNET_CHATBAR, 1, "W508 Schalter Chat in den Seitenbalken", why, sizeof why);
        loc_rule_set_append(s4, sizeof s4);
        if (!why[0] && (!g_loc_chatbar || !ui_checked(ID_LNET_CHATBAR) || strstr(s4, "loc_chat_pos")))
            _snprintf_s(why, sizeof why, _TRUNCATE, "W508 Seitenbalken nicht an bzw. Startzeile '%s'", s4);
        if (!why[0]) {
            RECT rc;
            if (GetWindowRect(ctl(ID_LNET_CHATBAR), &rc) && rc.bottom <= rc.top)
                _snprintf_s(why, sizeof why, _TRUNCATE, "W508 Schalter ohne Hoehe");
        }
        if (!why[0]) {
            strcpy_s(s4, sizeof s4, "vr_enable=0");
            on_command(ID_LNET_CHATBAR, BN_CLICKED);
            gt_read(cfg, buf, sizeof buf);
            loc_rule_set_append(s4, sizeof s4);
            if (g_loc_chatbar || ui_checked(ID_LNET_CHATBAR) || !strstr(buf, "launcher_loc_chat_bar=0\n") ||
                !strstr(s4, ";loc_chat_pos=1") || lnet_is_default())
                _snprintf_s(why, sizeof why, _TRUNCATE, "W508 Seitenbalken aus: '%s' / %.120s", s4, buf);
        }
        if (!why[0]) { /* neu laden: gemerkter Wert gilt */
            g_loc_chatbar = 1;
            onl_load();
            if (g_loc_chatbar) _snprintf_s(why, sizeof why, _TRUNCATE, "W508 Seitenbalken aus nicht gemerkt");
        }
        if (!why[0]) { /* zurueck auf an */
            on_command(ID_LNET_CHATBAR, BN_CLICKED);
            gt_read(cfg, buf, sizeof buf);
            if (!g_loc_chatbar || !strstr(buf, "launcher_loc_chat_bar=1\n"))
                _snprintf_s(why, sizeof why, _TRUNCATE, "W508 Seitenbalken wieder an: %.120s", buf);
        }
    }
    /* W518b MENUBG: Menue-Hintergrundebene - Standard an (Startzeile unveraendert), gemerkt, aus = ;loc_menu_bg=0 */
    if (!why[0]) {
        char s4[200] = "vr_enable=0";
        loctab_vis(ID_LNET_MENUBG, 1, "W518b Schalter Menue-Hintergrund", why, sizeof why);
        loc_rule_set_append(s4, sizeof s4);
        if (!why[0] && (!g_loc_menubg || !ui_checked(ID_LNET_MENUBG) || strstr(s4, "loc_menu_bg")))
            _snprintf_s(why, sizeof why, _TRUNCATE, "W518b Menue-Hintergrund nicht an bzw. Startzeile '%s'", s4);
        if (!why[0]) {
            strcpy_s(s4, sizeof s4, "vr_enable=0");
            on_command(ID_LNET_MENUBG, BN_CLICKED);
            gt_read(cfg, buf, sizeof buf);
            loc_rule_set_append(s4, sizeof s4);
            if (g_loc_menubg || ui_checked(ID_LNET_MENUBG) || !strstr(buf, "launcher_loc_menu_bg=0\n") ||
                !strstr(s4, ";loc_menu_bg=0") || lnet_is_default())
                _snprintf_s(why, sizeof why, _TRUNCATE, "W518b Menue-Hintergrund aus: '%s' / %.120s", s4, buf);
        }
        if (!why[0]) { /* neu laden: gemerkter Wert gilt */
            g_loc_menubg = 1;
            onl_load();
            if (g_loc_menubg) _snprintf_s(why, sizeof why, _TRUNCATE, "W518b Menue-Hintergrund aus nicht gemerkt");
        }
        if (!why[0]) { /* zurueck auf an */
            on_command(ID_LNET_MENUBG, BN_CLICKED);
            gt_read(cfg, buf, sizeof buf);
            if (!g_loc_menubg || !strstr(buf, "launcher_loc_menu_bg=1\n"))
                _snprintf_s(why, sizeof why, _TRUNCATE, "W518b Menue-Hintergrund wieder an: %.120s", buf);
        }
    }
    /* W520 HOSTMIG: Host-Wechsel (Standard an, Startzeile ohne Zusatz, aus = ;loc_host_migration=0), gemerkt, im Reiter
     * Network; W523: Auto-Host ausgeblendet und wirkungslos */
    if (!why[0]) {
        char s4[300] = "vr_enable=0";
        int tabw = g_lnet_tab;
        lnet_tab_pick(0);
        onl_sync_ui();
        /* W523 HOSTMIG-SCOPE: Host-Wechsel heisst „im Match“; Auto-Host ausgeblendet, Klick wirkungslos, gemerkte 1
         * wird ignoriert, nie loc_auto_host in der Startzeile */
        loctab_vis(ID_LNET_HOSTMIG, 1, "W520 Schalter Host-Wechsel", why, sizeof why);
        loctab_vis(ID_LNET_AUTOHOST, 0, "W523 Schalter Auto-Host (ausgeblendet)", why, sizeof why);
        loc_rule_set_append(s4, sizeof s4);
        if (!why[0] && (!g_loc_hostmig || g_loc_autohost || !ui_checked(ID_LNET_HOSTMIG) ||
                        strstr(s4, "loc_host_migration") || strstr(s4, "loc_auto_host")))
            _snprintf_s(why, sizeof why, _TRUNCATE, "W520 Standard falsch bzw. Startzeile '%s'", s4);
        if (!why[0] && (strstr(LT("gui.lnet_hostmig"), "auto") || strstr(LT("gui.lnet_hostmig"), "Auto")))
            _snprintf_s(why, sizeof why, _TRUNCATE, "W523 Beschriftung nennt Auto-Host: %s", LT("gui.lnet_hostmig"));
        if (!why[0]) { /* Klick auf Auto-Host (ausgeblendet) aendert nichts */
            on_command(ID_LNET_AUTOHOST, BN_CLICKED);
            gt_read(cfg, buf, sizeof buf);
            strcpy_s(s4, sizeof s4, "vr_enable=0");
            loc_rule_set_append(s4, sizeof s4);
            if (g_loc_autohost || strstr(buf, "launcher_loc_auto_host=1\n") || strstr(s4, "loc_auto_host"))
                _snprintf_s(why, sizeof why, _TRUNCATE, "W523 Auto-Host liess sich schalten: '%s' / %.120s", s4, buf);
        }
        if (!why[0]) { /* gemerkte 1 (aus 0.6.2/W520) wird ignoriert */
            onl_save_key("launcher_loc_auto_host", 1);
            onl_load();
            strcpy_s(s4, sizeof s4, "vr_enable=0");
            loc_rule_set_append(s4, sizeof s4);
            if (g_loc_autohost || strstr(s4, "loc_auto_host"))
                _snprintf_s(why, sizeof why, _TRUNCATE, "W523 gemerkter Auto-Host wirkt: '%s'", s4);
            onl_save_key("launcher_loc_auto_host", 0);
        }
        if (!why[0]) { /* Host-Wechsel aus: Startzeile */
            on_command(ID_LNET_HOSTMIG, BN_CLICKED);
            gt_read(cfg, buf, sizeof buf);
            strcpy_s(s4, sizeof s4, "vr_enable=0");
            loc_rule_set_append(s4, sizeof s4);
            if (g_loc_hostmig || ui_checked(ID_LNET_HOSTMIG) || !strstr(buf, "launcher_loc_host_migration=0\n") ||
                !strstr(s4, ";loc_host_migration=0") || strstr(s4, "loc_auto_host"))
                _snprintf_s(why, sizeof why, _TRUNCATE, "W520 Host-Wechsel aus: '%s' / %.120s", s4, buf);
        }
        if (!why[0]) { /* neu laden: gemerkt */
            g_loc_hostmig = 1;
            onl_load();
            if (g_loc_hostmig || g_loc_autohost) _snprintf_s(why, sizeof why, _TRUNCATE, "W520 Werte nicht gemerkt");
        }
        if (!why[0]) { /* zurueck auf Standard */
            on_command(ID_LNET_HOSTMIG, BN_CLICKED);
            gt_read(cfg, buf, sizeof buf);
            if (!g_loc_hostmig || g_loc_autohost || !strstr(buf, "launcher_loc_host_migration=1\n"))
                _snprintf_s(why, sizeof why, _TRUNCATE, "W520 Standard wieder: %.120s", buf);
        }
        lnet_tab_pick(tabw);
    }
    /* W440 LOC-LOBBY-MODES: Lobby-Modus - Standard Sim, unabhaengig vom Rollback, gemerkt, als loc_play_mode an den
     * Start; ungueltige Werte = Sim; "Standard wiederherstellen" setzt Sim (unten h) */
    if (!why[0]) {
        char s4[200] = "vr_enable=0";
        loctab_vis(ID_LNET_MODE, 1, "W440 Auswahl Lobby-Modus", why, sizeof why);
        loc_rule_set_append(s4, sizeof s4);
        if (!why[0] && (g_loc_arcade || ComboBox_GetCurSel(ctl(ID_LNET_MODE)) != 0 || !IsWindowEnabled(ctl(ID_LNET_MODE)) ||
                        ComboBox_GetCount(ctl(ID_LNET_MODE)) != 2 || !strstr(s4, ";loc_play_mode=sim")))
            _snprintf_s(why, sizeof why, _TRUNCATE, "W440 Standard nicht Sim: '%s'", s4);
        if (!why[0]) {
            g_updating++;
            ComboBox_SetCurSel(ctl(ID_LNET_MODE), 1);
            g_updating--;
            on_command(ID_LNET_MODE, CBN_SELCHANGE);
            gt_read(cfg, buf, sizeof buf);
            strcpy_s(s4, sizeof s4, "vr_enable=0");
            loc_rule_set_append(s4, sizeof s4);
            if (!g_loc_arcade || !strstr(buf, "launcher_loc_play_mode=1\n") || !strstr(s4, ";loc_play_mode=arcade") ||
                lnet_is_default() || !IsWindowEnabled(ctl(ID_LNET_RESET)))
                _snprintf_s(why, sizeof why, _TRUNCATE, "W440 Arcade: '%s' / %.120s", s4, buf);
        }
        if (!why[0]) { /* neu laden: gemerkter Wert gilt */
            g_loc_arcade = 0;
            onl_load();
            if (!g_loc_arcade) _snprintf_s(why, sizeof why, _TRUNCATE, "W440 Arcade nicht gemerkt");
        }
        if (!why[0]) { /* ungueltige Werte in user.cfg = Sim */
            static const char *const kBad[] = { "2", "arcade", "", "-1", "1x" };
            static const char kLine[] = "launcher_loc_play_mode=1\n";
            int bi;
            for (bi = 0; bi < 5 && !why[0]; bi++) {
                char *p;
                FILE *wf = NULL;
                onl_save_key(LNET_MODE_KEY, 1);
                gt_read(cfg, buf, sizeof buf);
                p = strstr(buf, kLine);
                if (p && u8_fopen_s(&wf, cfg, "w") == 0 && wf) {
                    fwrite(buf, 1, (size_t)(p - buf), wf);
                    fprintf(wf, "launcher_loc_play_mode=%s\n%s", kBad[bi], p + sizeof kLine - 1);
                    fclose(wf);
                } else {
                    _snprintf_s(why, sizeof why, _TRUNCATE, "W440 Testdatei nicht schreibbar");
                }
                g_loc_arcade = 1;
                onl_load();
                if (!why[0] && g_loc_arcade) _snprintf_s(why, sizeof why, _TRUNCATE, "W440 ungueltig '%s' nicht Sim", kBad[bi]);
            }
            g_loc_arcade = 1; /* fuer h: Standard wiederherstellen muss Sim setzen */
            onl_save_key(LNET_MODE_KEY, 1);
            onl_sync_ui();
        }
    }
    /* W447 LOC-NET-AUTO: Auswahl Netzmodus Auto (Standard) / Rollback / Lockstep, gemerkt, Kette */
    lnet_tab_pick(0); /* W473: Netzmodus im Reiter "Network" */
    if (!why[0]) {
        char w4[300] = "", s7[1024];
        write_file(cfg, "# fremde Zeile\n");
        onl_load();
        onl_sync_ui();
        strcpy_s(s7, sizeof s7, "vr_enable=0");
        loc_net_set_append(s7, sizeof s7);
        if (!g_loc_net_auto || !g_loc_rb_on || ComboBox_GetCount(ctl(ID_LNET_RB)) != 3 ||
            ComboBox_GetCurSel(ctl(ID_LNET_RB)) != 0 || strstr(s7, "loc_net_auto") || !lnet_is_default())
            _snprintf_s(w4, sizeof w4, _TRUNCATE, "Standard Auto: auto %d rb %d sel %d '%s'", g_loc_net_auto, g_loc_rb_on,
                        ComboBox_GetCurSel(ctl(ID_LNET_RB)), s7);
        if (!w4[0]) { /* Rollback manuell */
            ComboBox_SetCurSel(ctl(ID_LNET_RB), 1);
            on_command(ID_LNET_RB, CBN_SELCHANGE);
            gt_read(cfg, buf, sizeof buf);
            s7[0] = '\0';
            loc_net_set_append(s7, sizeof s7);
            if (g_loc_net_auto || !g_loc_rb_on || !strstr(buf, "launcher_loc_net_auto=0\n") ||
                !strstr(buf, "launcher_loc_rollback=1\n") || !strstr(buf, "# fremde Zeile\n") ||
                strcmp(s7, "loc_rb_input=1;loc_own_predict=1;loc_smooth_opp=1;loc_net_auto=0") != 0 || lnet_is_default() ||
                !IsWindowEnabled(ctl(ID_LNET_OWN)) || !IsWindowEnabled(ctl(ID_LNET_RESET)))
                _snprintf_s(w4, sizeof w4, _TRUNCATE, "Rollback manuell: '%s' / %.120s", s7, buf);
        }
        if (!w4[0]) { /* Laden: Rollback bleibt */
            g_loc_net_auto = 1;
            onl_load();
            onl_sync_ui();
            if (g_loc_net_auto || ComboBox_GetCurSel(ctl(ID_LNET_RB)) != 1)
                strcpy_s(w4, sizeof w4, "Rollback manuell nach Laden");
        }
        if (!w4[0]) { /* Lockstep: loc_rb_input=0, abhaengige Schalter grau, kein loc_net_auto in der Kette */
            ComboBox_SetCurSel(ctl(ID_LNET_RB), 2);
            on_command(ID_LNET_RB, CBN_SELCHANGE);
            gt_read(cfg, buf, sizeof buf);
            s7[0] = '\0';
            loc_net_set_append(s7, sizeof s7);
            if (g_loc_rb_on || lnet_rb_ui() || !strstr(buf, "launcher_loc_rollback=0\n") ||
                strcmp(s7, "loc_rb_input=0;loc_own_predict=1;loc_smooth_opp=1") != 0 || IsWindowEnabled(ctl(ID_LNET_OWN)))
                _snprintf_s(w4, sizeof w4, _TRUNCATE, "Lockstep: '%s' / %.120s", s7, buf);
        }
        if (!w4[0]) { /* zurueck auf Auto */
            ComboBox_SetCurSel(ctl(ID_LNET_RB), 0);
            on_command(ID_LNET_RB, CBN_SELCHANGE);
            gt_read(cfg, buf, sizeof buf);
            s7[0] = '\0';
            loc_net_set_append(s7, sizeof s7);
            if (!g_loc_rb_on || !g_loc_net_auto || !strstr(buf, "launcher_loc_net_auto=1\n") ||
                !strstr(buf, "launcher_loc_rollback=1\n") || strstr(s7, "loc_net_auto") || !lnet_is_default())
                _snprintf_s(w4, sizeof w4, _TRUNCATE, "Auto: '%s' / %.120s", s7, buf);
        }
        if (!w4[0] && (strcmp(LT("gui.lnet_nm_auto"), "Auto (recommended)") != 0 || strcmp(LT("gui.lnet_nm_ls"), "Lockstep") != 0)) /* W473: nur der Wert */
            _snprintf_s(w4, sizeof w4, _TRUNCATE, "Texte '%s'", LT("gui.lnet_nm_auto"));
        if (!w4[0])
            GT_OK("77d W447 LOC-NET-AUTO network mode Auto (default) / Rollback / Lockstep: saved, read back, chain "
                  "(Auto = nothing extra, Rollback = loc_net_auto=0, Lockstep = loc_rb_input=0), dependent switches greyed in Lockstep");
        else
            GT_FAIL("77d W447 LOC-NET-AUTO", "%s", w4);
    }
    /* h: Standard wiederherstellen */
    if (!why[0]) {
        on_command(ID_LNET_RESET, BN_CLICKED);
        gt_read(cfg, buf, sizeof buf);
        if (!lnet_is_default() || !lnet_rb_ui() || !ui_checked(ID_LNET_OWN) || !ui_checked(ID_LNET_LOWD) ||
            ComboBox_GetCurSel(ctl(ID_LOC_SMOOTH)) != 1 || IsWindowEnabled(ctl(ID_LNET_RESET)) ||
            !IsWindowEnabled(ctl(ID_LNET_OWN)) || !strstr(buf, "launcher_loc_rollback=1\n") ||
            !strstr(buf, "launcher_loc_own_predict=1\n") || !strstr(buf, "launcher_loc_low_delay=1\n") ||
            !strstr(buf, "launcher_loc_smooth_opp=1\n") || !strstr(buf, "# fremde Zeile\n") ||
            g_loc_arcade || ComboBox_GetCurSel(ctl(ID_LNET_MODE)) != 0 || !strstr(buf, "launcher_loc_play_mode=0\n")) /* W440 */
            _snprintf_s(why, sizeof why, _TRUNCATE, "Standard wiederherstellen: %.160s", buf);
    }
    /* i: Sprache DE (Navigation + Schalter) */
    if (!why[0]) {
        char nt[300], tt[300];
        sb_lang_set("de");
        ui_apply_language();
        ui_gettext(ctl(ID_NAV0 + P_PACKS), nt, sizeof nt);
        ui_gettext(ctl(ID_LNET_RB), tt, sizeof tt);
        if (strncmp(nt, "Multiplayer", 11) != 0 || strcmp(tt, "Auto (empfohlen)") != 0) /* W397; W447: Netzmodus; W473: nur der Wert */
            _snprintf_s(why, sizeof why, _TRUNCATE, "de '%s' / '%s'", nt, tt);
        /* W397: im Launcher "Account" statt "Pilot" (Piloten sind Spielobjekte), DE und EN;
         * W418: Name "steelnet account" / "steelnet-Konto" (Nutzerentscheid) */
        if (!why[0] && (strcmp(LT("gui.acc_title"), "steelnet-Konto") != 0 ||
                        strcmp(LT("gui.acc_manage"), "Konto verwalten \xE2\x80\xA6") != 0 ||
                        strstr(LT("gui.acc_sum_none"), "Pilot") || strstr(LT("gui.acc_unsecured"), "Pilot")))
            _snprintf_s(why, sizeof why, _TRUNCATE, "de Account-Texte '%s' / '%s'", LT("gui.acc_title"), LT("gui.acc_manage"));
        if (!why[0]) {
            sb_lang_set("en");
            if (strcmp(LT("gui.acc_title"), "steelnet account") != 0 || strcmp(LT("gui.page_mp"), "Multiplayer") != 0 ||
                strstr(LT("gui.acc_name"), "ilot") || strstr(LT("gui.acc_sum_none"), "ilot"))
                _snprintf_s(why, sizeof why, _TRUNCATE, "en Account-Texte '%s' / '%s'", LT("gui.acc_title"), LT("gui.acc_name"));
            sb_lang_set("de");
        }
    }
    /* Aufraeumen */
    SetEnvironmentVariableA("SB_TEST_LOC_READY", NULL);
    u8_DeleteFile(cfg);
    g_ucfg_test[0] = '\0';
    g_onl_fix_srv = -1;
    g_loc_online = on0;
    g_loc_smooth_opp = 1; /* echte Werte liest onl_load */
    g_loc_rb_on = g_loc_own_pred = g_loc_lowd = 1;
    g_loc = loc0;
    g_locfix_path[0] = '\0';
    g_loc_checked = 0;
    g_acc_fixture = 0;
    g_acc_loaded = 0;
    g_acc = a0;
    g_game = g0;
    onl_sync_ui();
    loctab_nav_layout();
    show_page(p0);
    sb_lang_set(keep);
    ui_apply_language();
    if (!why[0]) {
        GT_OK("77c W392 LAUNCHER-NETTAB Network page (defaults on, missing/invalid = on, saved + read, greyed without "
              "rollback, loc_rb_input/loc_own_predict/loc_smooth_opp/loc_rb_opts lowd=0 merge in every LoC start incl. "
              "online (internal: test partner user game only), never in SB start, restore defaults, relabel, SB keeps Packs)");
    } else {
        GT_FAIL("77c W392 LAUNCHER-NETTAB", "%s", why);
    }
}

/* --- 77d (W450 LAUNCHER-PRIVACY-INFO): Knopf neben den Chat-Schaltern, Fenster oeffnet und schliesst (Schliessen-Knopf),
 * Texte EN und DE vorhanden und verschieden, Knopf nur auf LoC (Seite Netzwerk) sichtbar, Aufruf aendert nichts. --- */
__declspec(noinline) void privacy_gui_selftest(int *pass, int *fail)
{
    char why[300] = "", a[2400], b[2400], keep[16];
    int g0 = g_game, p0 = g_page, chat0 = g_loc_chat, sn0 = g_loc_snchat, i;
    HWND bt = ctl(ID_LNET_PRIV);
    if (!bt || !ctl(ID_LNET_CHAT) || !ctl(ID_LNET_SNCHAT)) {
        GT_FAIL("77d W450 LAUNCHER-PRIVACY-INFO", "%s", "Knopf fehlt");
        return;
    }
    strcpy_s(keep, sizeof keep, sb_lang_code());
    sb_lang_set("en");
    ui_apply_language();
    if (!privacy_texts(a, (int)sizeof a)) strcpy_s(why, sizeof why, "EN Text fehlt");
    sb_lang_set("de");
    ui_apply_language();
    if (!why[0] && !privacy_texts(b, (int)sizeof b)) strcpy_s(why, sizeof why, "DE Text fehlt");
    if (!why[0] && (strcmp(a, b) == 0 || strlen(a) < 300 || strlen(a) > 1100)) strcpy_s(why, sizeof why, "Texte gleich/Laenge");
    for (i = 0; i < 2 && !why[0]; i++) { /* oeffnet und schliesst, in beiden Sprachen */
        sb_lang_set(i ? "de" : "en");
        ui_apply_language();
        if (!privacy_dialog_modal(2)) strcpy_s(why, sizeof why, "Fenster oeffnet nicht");
        else if (!IsWindowEnabled(g_hwnd) || g_pv_done != 1) strcpy_s(why, sizeof why, "Fenster schliesst nicht");
    }
    sb_lang_set("en");
    ui_apply_language();
    if (!why[0]) {
        char t[200];
        t[0] = 0;
        ui_gettext(bt, t, sizeof t);
        if (strcmp(t, "Chat privacy") != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "Knopftext '%s'", t);
    }
    g_game = LGAME_LOC;
    loctab_nav_layout();
    show_page(P_LNET);
    if (!why[0] && IsWindowVisible(bt) != (g_lnet_tab == 1)) strcpy_s(why, sizeof why, "W473: Knopf nur im Reiter Lobby & chat");
    lnet_tab_pick(1); /* W473: Chat privacy im Reiter "Lobby & chat" */
    if (!why[0] && !IsWindowVisible(bt)) strcpy_s(why, sizeof why, "Knopf nicht sichtbar auf LoC Netzwerk");
    lnet_tab_pick(0);
    g_game = LGAME_SB;
    loctab_nav_layout();
    show_page(P_PACKS);
    if (!why[0] && IsWindowVisible(bt)) strcpy_s(why, sizeof why, "Knopf bei SB sichtbar");
    if (!why[0] && (g_loc_chat != chat0 || g_loc_snchat != sn0)) strcpy_s(why, sizeof why, "Schalter veraendert");
    g_game = g0;
    loctab_nav_layout();
    show_page(p0);
    sb_lang_set(keep);
    ui_apply_language();
    if (!why[0]) {
        GT_OK("77d W450 LAUNCHER-PRIVACY-INFO Chat privacy button (LoC network page only), window opens/closes via Close, "
              "EN/DE text present, no setting changed)");
    } else {
        GT_FAIL("77d W450 LAUNCHER-PRIVACY-INFO", "%s", why);
    }
}

__declspec(noinline) void locwide_gui_selftest(int *pass, int *fail)
{
    static char buf[16384];
    char why[300] = "", tmp[1024], cfg[1200], lcf[1200], t[400], keep[16];
    int g0 = g_game, i, n, li;
    lgame_state_t loc0 = g_loc, rd;
    HWND c = ctl(ID_LOC_WIDE);
    if (!c || !ctl(ID_LOC_WIDE_XI)) {
        GT_FAIL("74 W326 LOC-WIDE", "%s", "ID_LOC_WIDE/ID_LOC_WIDE_XI fehlt");
        return;
    }
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw326_gui_user_%lu.cfg", tmp, GetCurrentProcessId());
    write_file(cfg, "[display]\nwidth=1280\n# fremde Zeile\n");
    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, cfg);
    lgame_cfg_path(cfg, LGAME_LOC, lcf, sizeof lcf); /* W402: Bildformat steht in der LoC-Datei */
    u8_DeleteFile(lcf);
    strcpy_s(keep, sizeof keep, sb_lang_code());
    sb_lang_set("en");
    ui_apply_language();
    /* 1: fehlender Schluessel -> W414: 16:9 (Spielmodus) ist LoC-Standard (W326: 4:3) */
    g_loc.loc_widescreen = LGAME_WIDE_43;
    loctab_load();
    g_game = LGAME_LOC;
    loctab_nav_layout();
    if (g_loc.loc_widescreen != LGAME_WIDE_169 || ComboBox_GetCurSel(c) != 1) {
        _snprintf_s(why, sizeof why, _TRUNCATE, "fehlt: Wert %d, Auswahl %d", g_loc.loc_widescreen,
                    ComboBox_GetCurSel(c));
    }
    /* 2: Eintraege = angebotene Modi, Texte EN/DE, Wahl bleibt beim Sprachwechsel */
    n = ComboBox_GetCount(c);
    if (!why[0] && (n != lgame_wide_count() || n != (LOC_WIDE_720P ? 3 : 2))) {
        _snprintf_s(why, sizeof why, _TRUNCATE, "%d Eintraege (erwartet %d)", n, LOC_WIDE_720P ? 3 : 2);
    }
    ComboBox_SetCurSel(c, 1);
    on_command(ID_LOC_WIDE, CBN_SELCHANGE);
    for (li = 0; li < 2 && !why[0]; li++) {
        static const char *const kWant[2][3] = { { "4:3 (Original)", "16:9 (game mode)", "720p (16:9)" },
                                                 { "4:3 (Original)", "16:9 (Spielmodus)", "720p (16:9)" } };
        sb_lang_set(li ? "de" : "en");
        ui_apply_language();
        for (i = 0; i < n && !why[0]; i++) {
            wchar_t w[128];
            t[0] = 0;
            if (SendMessageW(c, CB_GETLBTEXTLEN, (WPARAM)i, 0) < 120 &&
                SendMessageW(c, CB_GETLBTEXT, (WPARAM)i, (LPARAM)w) != CB_ERR) {
                w_to_u8(w, t, (int)sizeof t);
            }
            if (strcmp(t, kWant[li][i]) != 0 || strcmp(t, LT(lgame_wide_label_key(i))) != 0) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "%s Eintrag %d '%s'", li ? "de" : "en", i, t);
            }
        }
        if (!why[0] && (ComboBox_GetCount(c) != n || ComboBox_GetCurSel(c) != 1)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "%s: Wahl %d nach Sprachwechsel", li ? "de" : "en",
                        ComboBox_GetCurSel(c));
        }
    }
    sb_lang_set("en");
    ui_apply_language();
    /* 3: Rundweg ueber die Auswahl -> user.cfg (1 -> 2 -> 0 -> 1, jeder Schritt eine Aenderung) */
    for (i = 0; i < 3 && !why[0]; i++) {
        static const int kSeq[3] = { 2, 0, 1 };
        int v = kSeq[i];
        char line[40];
        if (v >= n) {
            continue; /* 720p nicht angeboten */
        }
        ComboBox_SetCurSel(c, v);
        on_command(ID_LOC_WIDE, CBN_SELCHANGE);
        gt_read(lcf, buf, sizeof buf);
        _snprintf_s(line, sizeof line, _TRUNCATE, "loc_widescreen=%d\n", v);
        if (locwide_cfg_count(buf, line) != 1 || locwide_cfg_count(buf, "loc_widescreen=") != 1) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Wahl %d: user_loc.cfg '%.120s'", v, buf);
        } else if (gt_read(cfg, buf, sizeof buf), !strstr(buf, "width=1280\n") || !strstr(buf, "# fremde Zeile\n")) {
            strcpy_s(why, sizeof why, "fremde Zeilen verloren");
        } else if (strstr(buf, "loc_widescreen=")) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Wahl %d: Bildformat in user.cfg (SB)", v);
        } else if (lgame_cfg_load(lcf, &rd) != 0 || rd.loc_widescreen != v || g_loc.loc_widescreen != v) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Wahl %d: geladen %d", v, rd.loc_widescreen);
        } else if (v && (g_status_kind != 1 || strcmp(g_status, LT("gui.loc_wide_saved")) != 0)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Wahl %d: Statuszeile '%s'", v, g_status);
        }
        if (!why[0]) { /* Rueckweg: Datei -> Oberflaeche */
            g_loc.loc_widescreen = -1;
            loctab_load();
            g_game = LGAME_LOC;
            if (g_loc.loc_widescreen != v || ComboBox_GetCurSel(c) != v) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Wahl %d: neu geladen %d/%d", v, g_loc.loc_widescreen,
                            ComboBox_GetCurSel(c));
            }
        }
    }
    /* 4: ungueltige Werte -> 0 (auch 2, wenn 720p nicht angeboten wird) */
    {
        static const char *const kBad[] = { "7", "-1", "abc", "", "3", LOC_WIDE_720P ? "99" : "2" };
        for (i = 0; i < (int)(sizeof kBad / sizeof kBad[0]) && !why[0]; i++) {
            char body[200];
            _snprintf_s(body, sizeof body, _TRUNCATE,
                        "[display]\nwidth=1280\nloc_widescreen=1\nloc_widescreen = %s\nlauncher_loc169=1\n", /* W414: schon umgestellt */
                        kBad[i]);
            write_file(lcf, body); /* W402 */
            loctab_load();
            g_game = LGAME_LOC;
            if (g_loc.loc_widescreen != 0 || ComboBox_GetCurSel(c) != 0) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "ungueltig '%s' -> %d", kBad[i], g_loc.loc_widescreen);
            }
        }
        if (!why[0] && (lgame_wide_clamp(1) != 1 || lgame_wide_clamp(2) != (LOC_WIDE_720P ? 2 : 0) ||
                        lgame_wide_experimental(0) || lgame_wide_experimental(1) || /* W414: 16:9 nicht experimentell */
                        lgame_wide_experimental(2) != LOC_WIDE_720P)) {
            strcpy_s(why, sizeof why, "lgame_wide_clamp/experimental");
        }
    }
    /* 5: Sichtbarkeit (nur LoC > Erweitert), Kolben mit Tooltip, keine Textplakette, keine Ueberlappung */
    if (!why[0]) {
        ComboBox_SetCurSel(c, 1);
        on_command(ID_LOC_WIDE, CBN_SELCHANGE);
        show_page(P_ADV);
        pump();
        loctab_vis(ID_LOC_WIDE, 1, "Bildformat (LoC Erweitert)", why, sizeof why);
        /* W414: Kolben nur mit experimentellem Bildformat in der Auswahl (720p), 16:9 ist nicht experimentell */
        loctab_vis(ID_LOC_WIDE_XI, LOC_WIDE_720P ? 1 : 0, "Kolben Bildformat (LoC Erweitert)", why, sizeof why);
        xicon_gt_noplaque("LoC Erweitert 16:9", why, sizeof why);
        if (!why[0] && LOC_WIDE_720P) {
            char tt[800];
            GetWindowTextA(ctl(ID_LOC_WIDE_XI), tt, sizeof tt); /* zugaenglicher Name (ASCII-Teil reicht) */
            xtip_text(ID_LOC_WIDE_XI, t, sizeof t);
            if (!strstr(t, LT("gui.exp_tip")) || !strstr(t, LT("gui.exp_tip_locwide")) || !tt[0]) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Kolben-Tooltip '%.80s'", t);
            }
        }
        if (!why[0]) {
            lay_gt_page_check(why, sizeof why, "LoC W326");
        }
        if (!why[0]) { /* W414: 16:9 kein Breitbild-Hinweis mehr (W326: experimentell); SB/LoC-4:3 unveraendert */
            unsigned m0, m1, ms;
            int w0 = g_loc.loc_widescreen;
            g_loc.loc_widescreen = LGAME_WIDE_43;
            ui_apply_rules();
            m0 = g_orig_mask;
            g_loc.loc_widescreen = LGAME_WIDE_169;
            ui_apply_rules();
            m1 = g_orig_mask;
            g_game = LGAME_SB;
            ui_apply_rules();
            ms = g_orig_mask;
            g_game = LGAME_LOC;
            if ((m1 & (1u << SB_OFG_WIDE)) || ((ms ^ m0) & (1u << SB_OFG_WIDE))) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Hinweis-Maske 4:3 0x%X / 16:9 0x%X / SB 0x%X", m0, m1, ms);
            }
            g_loc.loc_widescreen = w0;
            ui_apply_rules();
        }
        show_page(P_DISPLAY);
        pump();
        loctab_vis(ID_LOC_WIDE, 0, "Bildformat auf Anzeige", why, sizeof why);
        g_game = LGAME_SB;
        loctab_nav_layout();
        show_page(P_ADV);
        pump();
        loctab_vis(ID_LOC_WIDE, 0, "Bildformat im SB-Bereich", why, sizeof why);
        loctab_vis(ID_LOC_WIDE_XI, 0, "Kolben Bildformat im SB-Bereich", why, sizeof why);
    }
    /* aufraeumen (nur die Temp-Datei wurde geschrieben) */
    u8_DeleteFile(cfg);
    u8_DeleteFile(lcf);
    g_ucfg_test[0] = '\0';
    g_loc = loc0;
    g_game = g0;
    sb_lang_set(keep);
    ui_apply_language();
    loctab_wide_sync();
    loctab_nav_layout();
    ui_apply_rules();
    show_page(P_PLAY);
    if (!why[0]) {
        _snprintf_s(t, sizeof t, _TRUNCATE,
                    "74 W326 LOC-WIDE (Bildformat %d Eintraege EN/DE, Rundweg ueber user_loc.cfg (W402), W414: fehlt = 16:9, "
                    "ungueltig = 4:3, 16:9 ohne Kolben/Experimental-Hinweis, nur LoC > Erweitert, keine Ueberlappung)", n);
        GT_OK(t);
    } else {
        GT_FAIL("74 W326 LOC-WIDE", "%s", why);
    }
}

/* W459 LOC-FX-LIGHT (Fall 74d), W465 LOC-ENHANCED: der Schalter "SB look (Experimental)" ist aus der Oberflaeche
 * genommen (Enhanced ersetzt ihn). Geprueft: kein Schalter/keine Karte auf LoC > Erweitert, loc_sblook wird weiter
 * gelesen (fehlt = 0, letzte Zeile gewinnt, mit Marke gfx_loc_rev bleibt 1 = Experimental-Maske Licht+Shader nur
 * in LoC), ohne Marke stellt die Umstellung auf Enhanced loc_sblook=0 (nie user.cfg), keine Ueberlappung. */
__declspec(noinline) void locsblook_gui_selftest(int *pass, int *fail)
{
    static char buf[16384];
    char why[300] = "", tmp[1024], cfg[1200], lcf[1200];
    int g0 = g_game;
    lgame_state_t loc0 = g_loc, rd;
    unsigned m_on = 0, m_sb = 0;
    if (ctl(ID_LOC_SBLOOK)) {
        GT_FAIL("74d W459/W465 SB look", "%s", "Schalter ID_LOC_SBLOOK noch in der Oberflaeche");
        return;
    }
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw459_gui_user_%lu.cfg", tmp, GetCurrentProcessId());
    write_file(cfg, "[display]\nwidth=1280\n# fremde Zeile\n");
    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, cfg);
    lgame_cfg_path(cfg, LGAME_LOC, lcf, sizeof lcf);
    u8_DeleteFile(lcf);
    /* 1: fehlende Datei/Schluessel = 0; letzte Zeile gewinnt */
    g_loc.loc_sblook = 1;
    loctab_load();
    g_game = LGAME_LOC;
    loctab_nav_layout();
    if (g_loc.loc_sblook != 0) {
        strcpy_s(why, sizeof why, "fehlt: nicht 0");
    }
    write_file(lcf, "[display]\nwidth=1280\nloc_widescreen=1\nlauncher_loc169=1\nloc_sblook=1\nloc_sblook = 0\n");
    if (!why[0] && (lgame_cfg_load(lcf, &rd) != 0 || rd.loc_sblook != 0)) {
        strcpy_s(why, sizeof why, "letzte Zeile gewinnt (0)");
    }
    /* 2: mit Marke gfx_loc_rev wird loc_sblook=1 weiter gelesen (Maske Licht+Shader nur LoC) */
    if (!why[0]) {
        write_file(lcf, "[display]\nwidth=1280\nloc_widescreen=1\nlauncher_loc169=1\nloc_sblook=1\n[render]\ngfx_loc_rev=1\n");
        loctab_load();
        g_game = LGAME_LOC;
        if (g_loc.loc_sblook != 1) {
            strcpy_s(why, sizeof why, "mit Marke: nicht 1");
        }
        ui_apply_rules();
        m_on = g_orig_mask;
        g_game = LGAME_SB;
        ui_apply_rules();
        m_sb = g_orig_mask;
        g_game = LGAME_LOC;
        if (!why[0] && (!(m_on & (1u << SB_OFG_LIGHT)) || (m_sb & (1u << SB_OFG_LIGHT)))) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Experimental-Maske an 0x%X / SB 0x%X", m_on, m_sb);
        }
    }
    /* 3: ohne Marke -> Umstellung auf Enhanced setzt loc_sblook=0 (Datei), user.cfg unberuehrt */
    if (!why[0]) {
        write_file(lcf, "[display]\nwidth=1280\nloc_widescreen=1\nlauncher_loc169=1\nloc_sblook=1\n");
        loctab_load();
        g_game = LGAME_LOC;
        gt_read(lcf, buf, sizeof buf);
        if (g_loc.loc_sblook != 0 || lgame_cfg_load(lcf, &rd) != 0 || rd.loc_sblook != 0 ||
            locwide_cfg_count(buf, "loc_sblook=0\n") != 1 || locwide_cfg_count(buf, "loc_sblook=") != 1) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "ohne Marke: loc_sblook %d '%.120s'", g_loc.loc_sblook, buf);
        } else if (gt_read(cfg, buf, sizeof buf), strstr(buf, "loc_sblook") || !strstr(buf, "# fremde Zeile\n")) {
            strcpy_s(why, sizeof why, "user.cfg beruehrt");
        }
    }
    /* 4: kein Schalter auf LoC > Erweitert, keine Ueberlappung/Fensterrand */
    if (!why[0]) {
        show_page(P_ADV);
        pump();
        lay_gt_page_check(why, sizeof why, "LoC W465");
    }
    u8_DeleteFile(cfg);
    u8_DeleteFile(lcf);
    g_ucfg_test[0] = '\0';
    g_loc = loc0;
    g_game = g0;
    loctab_wide_sync();
    loctab_sblook_sync();
    loctab_nav_layout();
    ui_apply_rules();
    show_page(P_PLAY);
    if (!why[0]) {
        GT_OK("74d W459/W465 SB look (Schalter aus der Oberflaeche, loc_sblook gelesen: fehlt = 0, mit Marke 1 = "
              "Licht+Shader nur LoC, ohne Marke -> 0 per Umstellung, nie user.cfg, keine Ueberlappung)");
    } else {
        GT_FAIL("74d W459/W465 SB look", "%s", why);
    }
}

/* W465 LOC-ENHANCED (Fall 74e): Presets je Spiel und einmalige Umstellung alter Dateien.
 * a: SB user.cfg exakt Remastered dezent/stark -> Original, Benutzerdefiniert bleibt.
 * b: LoC user_loc.cfg ohne Marke mit Original/Performance/Klare Sicht -> Optik Enhanced, Bildrate bleibt, Marke
 *    gfx_loc_rev, zweiter Lauf nichts; mit Marke + Original bleibt Original; loc_sblook=1 ohne Marke -> 0.
 * c: frische LoC-Installation zeigt Enhanced (ohne Experimental), Seite Grafik ohne Luecken (SB: Original,
 *    Performance, Klare Sicht, Custom; LoC: Original, Enhanced, Custom), Klick Original -> Original (nicht
 *    Custom), Speichern schreibt die Marke, Back to original -> Original + loc_sblook=0. */
static int w465_look_is(const launcher_settings_t *s, int p)
{
    int k;
    for (k = 0; k < LGK_HD; k++) {
        if (lcfg_gfx_owned(k) && lcfg_gfx_value(s, k) != lcfg_gfx_preset_value(p, k)) {
            return 0;
        }
    }
    return 1;
}

/* W471: dasselbe fuer ein bestimmtes Spiel (LoC-Original = Schatten an) */
static int w471_look_is(const launcher_settings_t *s, int p, int loc)
{
    int k;
    for (k = 0; k < LGK_HD; k++) {
        if (lcfg_gfx_owned(k) && lcfg_gfx_value(s, k) != lcfg_gfx_preset_value_loc(p, k, loc)) {
            return 0;
        }
    }
    return 1;
}

/* W471: cfg-Text Optik-Schluessel (k < LGK_HD) eines Presets fuer LoC (p >= 0) bzw. des alten Enhanced (p < 0:
 * -1 = Rev 2, -2 = Rev 1 ohne Schatten; ohne shadow_sun-Zeile wie die Dateien bis W471), dazu extra. */
static void w471_cfg_text(char *out, size_t cap, int p, const char *extra)
{
    int k;
    _snprintf_s(out, cap, _TRUNCATE, "# W471 Test\n[render]\n");
    for (k = 0; k < LGK_HD; k++) {
        char l[80];
        int r = sb_gp_find(lcfg_gfx_key(k)), v;
        if (!lcfg_gfx_owned(k) || r < 0 || (p < 0 && k == LSHX_SUN)) {
            continue;
        }
        v = p >= 0 ? lcfg_gfx_preset_value_loc(p, k, 1) : sb_gp_enh_old(r, p == -1 ? 2 : 1);
        _snprintf_s(l, sizeof l, _TRUNCATE, "%s=%d\n", lcfg_gfx_key(k), v);
        strncat_s(out, cap, l, _TRUNCATE);
    }
    strncat_s(out, cap, extra, _TRUNCATE);
}

/* cfg-Text eines Presets (alle Preset-Schluessel aus der Tabelle) + Zusatzzeilen */
static void w465_cfg_text(char *out, size_t cap, int p, const char *extra)
{
    int k;
    _snprintf_s(out, cap, _TRUNCATE, "# W465 Test\n[render]\n");
    for (k = 0; k < LGK_N; k++) {
        char l[80];
        if (!lcfg_gfx_owned(k)) {
            continue;
        }
        _snprintf_s(l, sizeof l, _TRUNCATE, "%s=%d\n", lcfg_gfx_key(k), lcfg_gfx_preset_value(p, k));
        strncat_s(out, cap, l, _TRUNCATE);
    }
    if (lcfg_gfx_preset_value(p, LGK_HD) > 0) {
        strncat_s(out, cap, "remaster=1\n", _TRUNCATE);
    }
    strncat_s(out, cap, extra, _TRUNCATE);
}

static int w465_radio_y(int i)
{
    uictl_t *u = ui_find(ID_GP0 + i);
    return u ? u->y : -1;
}

/* sichtbare Optionsfelder in Reihenfolge, gleichmaessiger Abstand ohne Luecke */
static void w465_layout_check(const int *want, int n, const char *tag, char *why, size_t cap)
{
    int i, k = 0, y0 = -1, dy = -1;
    for (i = 0; i <= LGP_CUSTOM && !why[0]; i++) {
        int vis = ui_visible_on(ui_find(ID_GP0 + i), P_GFX);
        int exp = k < n && want[k] == i;
        if (vis != exp) {
            _snprintf_s(why, cap, _TRUNCATE, "%s: Preset %d sichtbar %d", tag, i, vis);
            break;
        }
        if (!vis) {
            continue;
        }
        if (k == 0) {
            y0 = w465_radio_y(i);
        } else if (k == 1) {
            dy = w465_radio_y(i) - y0;
        } else if (w465_radio_y(i) != y0 + k * dy) {
            _snprintf_s(why, cap, _TRUNCATE, "%s: Luecke vor Preset %d (y %d)", tag, i, w465_radio_y(i));
        }
        k++;
    }
    if (!why[0] && (k != n || dy <= 0)) {
        _snprintf_s(why, cap, _TRUNCATE, "%s: %d Felder, Abstand %d", tag, k, dy);
    }
    if (!why[0]) {
        uictl_t *r = ui_find(ID_GP_RESET);
        if (!r || r->y != w465_radio_y(LGP_CUSTOM) + 4) {
            _snprintf_s(why, cap, _TRUNCATE, "%s: Zuruecksetzen nicht in der Zeile Benutzerdefiniert", tag);
        }
    }
}

__declspec(noinline) void w465_gui_selftest(int *pass, int *fail)
{
    static char txt[8192], buf[16384];
    static const int kSb[] = { LGP_ORIGINAL, LGP_PERFORMANCE, LGP_CLEAR, LGP_CUSTOM };
    static const int kLoc[] = { LGP_ORIGINAL, LGP_ENHANCED, LGP_VIVID, LGP_CUSTOM }; /* W478: + Vivid */
    char why[300] = "", tmp[1024], cfg[1200], lcf[1200];
    launcher_settings_t a, keep_s = g_s, keep_base = g_disk_base;
    int keep_ok = g_disk_base_ok, keep_game = g_game, keep_page = g_page, i;
    lgame_state_t keep_loc = g_loc, rd;
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw465_user_%lu.cfg", tmp, GetCurrentProcessId());
    lgame_cfg_path(cfg, LGAME_LOC, lcf, sizeof lcf);
    /* a: SB - Remastered dezent/stark -> Original, Custom bleibt (Laden wie app_load_effective) */
    for (i = 0; i < 3 && !why[0]; i++) {
        int p = i == 2 ? LGP_SUBTLE : (i ? LGP_STRONG : LGP_SUBTLE);
        w465_cfg_text(txt, sizeof txt, p, i == 2 ? "color_sat=77\n" : "");
        write_file(cfg, txt);
        lcfg_defaults(&a);
        a.gfx_preset_rev = 0;
        lcfg_load_file(cfg, &a);
        lcfg_gfx_migrate_fps(&a);
        if (lcfg_gfx_migrate_remaster(&a) != (i < 2)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "a%d: Umstellung falsch", i);
        } else if (i < 2 && (lcfg_gfx_diff(&a, LGP_ORIGINAL, 0, NULL) != 0 || a.remaster || a.hd_textures)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "a%d: nicht Original", i);
        } else if (i == 2 && lcfg_gfx_diff(&a, LGP_SUBTLE, 0, NULL) != 1) {
            strcpy_s(why, sizeof why, "a2: Benutzerdefiniert veraendert");
        }
    }
    /* b: LoC-Dateien ohne Marke -> Enhanced (Optik), Bildrate/Skalierung/monitor_fx bleiben */
    for (i = 0; i < 3 && !why[0]; i++) {
        static const int kP[3] = { LGP_ORIGINAL, LGP_PERFORMANCE, LGP_CLEAR };
        int p = kP[i];
        w465_cfg_text(txt, sizeof txt, p, "# eigene Zeile\nvolume_music=70\n");
        write_file(lcf, txt);
        if (lgame_loc_gfx_migrate(lcf) != 1) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "b%d: keine Umstellung", i);
            break;
        }
        gt_read(lcf, buf, sizeof buf);
        lcfg_defaults(&a);
        lcfg_load_file(lcf, &a);
        if (!w465_look_is(&a, LGP_ENHANCED) || a.interp_fps != lcfg_gfx_preset_value(p, LGK_FPS) ||
            a.rt_scale != lcfg_gfx_preset_value(p, LGK_RT) || a.monitor_fx != lcfg_gfx_preset_value(p, LGK_MFX) ||
            a.volume_music != 70) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "b%d: Werte (fps %d)", i, a.interp_fps);
        } else if (locwide_cfg_count(buf, "gfx_loc_rev=3\n") != 1 || locwide_cfg_count(buf, "loc_sblook=0\n") != 1 ||
                   locwide_cfg_count(buf, "shading=") != 1 || !strstr(buf, "# eigene Zeile\n")) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "b%d: Datei '%.120s'", i, buf);
        } else if (lgame_loc_gfx_migrate(lcf) != 0 || lgame_loc_rev(lcf) != SB_GP_LOC_REV) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "b%d: zweiter Lauf", i);
        }
    }
    if (!why[0]) { /* mit aktueller Marke + Original (LoC, mit Schatten) bleibt Original */
        w471_cfg_text(txt, sizeof txt, LGP_ORIGINAL, "gfx_loc_rev=3\n");
        write_file(lcf, txt);
        if (lgame_loc_gfx_migrate(lcf) != 0 || (lcfg_defaults(&a), lcfg_load_file(lcf, &a), !w471_look_is(&a, LGP_ORIGINAL, 1)))
            strcpy_s(why, sizeof why, "b: mit Marke umgestellt");
    }
    if (!why[0] && (SB_GP_LOC_REV != 3 || lcfg_gfx_preset_value_loc(LGP_ORIGINAL, LSHX_SHADOW, 1) != 1 ||
                    lcfg_gfx_preset_value_loc(LGP_ORIGINAL, LSHX_SOFT, 1) != 0 ||
                    lcfg_gfx_preset_value_loc(LGP_ORIGINAL, LSHX_SUN, 1) != 0 ||
                    lcfg_gfx_preset_value_loc(LGP_ORIGINAL, LSHX_SHADOW, 0) != 0 ||
                    lcfg_gfx_preset_value_loc(LGP_ENHANCED, LSHX_CON, 1) != 100 ||
                    lcfg_gfx_preset_value_loc(LGP_ENHANCED, LSHX_SUN, 1) <= 0))
        strcpy_s(why, sizeof why, "W471: Tabelle (LoC-Original mit Schatten, SB ohne, Enhanced Kontrast 100)");
    /* W466/W471: Einmal-Umstellung Marke 1/2 -> 3. m0 Rev 2 altes Enhanced (ohne shadow_sun-Zeile), m1 dasselbe mit
     * shadow_sun=140, m2 Rev 1 W465-Enhanced (Schatten 0/0), m3 Rev 2 altes Original (+ shadow_sun=140 vom ESC),
     * m4 Rev 1 Original, m5 eigene Werte ohne shadow_sun (-> 140, Rest bleibt), m6 eigene Werte mit shadow_sun=90. */
    for (i = 0; i < 7 && !why[0]; i++) {
        static const int kSrc[7] = { -1, -1, -2, 0, 0, -1, -1 };
        static const char *const kEx[7] = { "gfx_loc_rev=2\n", "shadow_sun=140\ngfx_loc_rev=2\n", "gfx_loc_rev=1\n",
                                            "shadow_sun=140\ngfx_loc_rev=2\n", "gfx_loc_rev=1\n",
                                            "color_sat=77\ngfx_loc_rev=2\n", "color_sat=77\nshadow_sun=90\ngfx_loc_rev=2\n" };
        if (kSrc[i] == 0) { /* altes Original = Spalte 0 ohne Schatten (SB-Werte) */
            int k;
            _snprintf_s(txt, sizeof txt, _TRUNCATE, "# W471 Test\n[render]\n");
            for (k = 0; k < LGK_HD; k++) {
                char l[80];
                if (!lcfg_gfx_owned(k) || k == LSHX_SUN) { continue; }
                _snprintf_s(l, sizeof l, _TRUNCATE, "%s=%d\n", lcfg_gfx_key(k), lcfg_gfx_preset_value_loc(LGP_ORIGINAL, k, 0));
                strncat_s(txt, sizeof txt, l, _TRUNCATE);
            }
            strncat_s(txt, sizeof txt, kEx[i], _TRUNCATE);
        } else {
            w471_cfg_text(txt, sizeof txt, kSrc[i], kEx[i]);
        }
        write_file(lcf, txt);
        if (lgame_loc_gfx_migrate(lcf) != 1 || lgame_loc_rev(lcf) != SB_GP_LOC_REV) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "W471 m%d: keine Umstellung/Marke", i);
            break;
        }
        lcfg_defaults(&a);
        lcfg_load_file(lcf, &a);
        if (i < 3 && !w471_look_is(&a, LGP_ENHANCED, 1)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "W471 m%d: altes Enhanced nicht neues Enhanced (Kontrast %d)", i,
                        a.shx[LSHX_CON]);
        } else if ((i == 3 || i == 4) && (!w471_look_is(&a, LGP_ORIGINAL, 1) || a.shx[LSHX_SHADOW] != 1 ||
                                          a.shx[LSHX_SOFT] != 0 || a.shx[LSHX_SUN] != 0)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "W471 m%d: Original ohne Schatten nicht Original mit Schatten", i);
        } else if (i >= 5 && (a.shx[LSHX_SAT] != 77 || a.shx[LSHX_CON] != 110 || a.shx[LSHX_SHADOW] != 1 ||
                              a.shx[LSHX_SUN] != (i == 5 ? SB_GP_SUN_REV2 : 90))) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "W471 m%d: eigene Werte veraendert (sat %d con %d sun %d)", i,
                        a.shx[LSHX_SAT], a.shx[LSHX_CON], a.shx[LSHX_SUN]);
        } else if (lgame_loc_gfx_migrate(lcf) != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "W471 m%d: zweiter Lauf", i);
        }
    }
    if (!why[0]) { /* loc_sblook=1 ohne Marke -> Enhanced + 0 */
        w465_cfg_text(txt, sizeof txt, LGP_ORIGINAL, "loc_sblook=1\n");
        write_file(lcf, txt);
        lcfg_defaults(&a);
        if (lgame_loc_gfx_migrate(lcf) != 1 || lgame_cfg_load(lcf, &rd) != 0 || rd.loc_sblook != 0 ||
            (lcfg_load_file(lcf, &a), !w465_look_is(&a, LGP_ENHANCED)))
            strcpy_s(why, sizeof why, "b: loc_sblook=1 nicht umgestellt");
    }
    /* c: Oberflaeche - frische LoC-Installation (keine user_loc.cfg) */
    write_file(cfg, "[display]\nwidth=1280\n# fremde Zeile\nlauncher_pergame=1\nlauncher_loc169=1\n");
    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, cfg);
    u8_DeleteFile(lcf);
    memset(g_pg_ok, 0, sizeof g_pg_ok);
    g_loc.loc_widescreen = LGAME_WIDE_169;
    g_loc.loc_sblook = 0;
    if (!why[0]) {
        g_game = LGAME_SB;
        lcfg_defaults(&g_s);
        disk_base_sync(&g_s);
        ui_from_settings(&g_s);
        show_page(P_GFX);
        pump();
        w465_layout_check(kSb, 4, "SB", why, sizeof why);
        if (!why[0] && !ui_checked(ID_GP0 + LGP_ORIGINAL))
            strcpy_s(why, sizeof why, "SB: Standard nicht Original");
    }
    if (!why[0]) {
        loctab_set_game(LGAME_LOC);
        show_page(P_GFX);
        pump();
        w465_layout_check(kLoc, 4, "LoC", why, sizeof why);
        if (!why[0] && (!ui_checked(ID_GP0 + LGP_ENHANCED) || lcfg_gfx_preset(&g_gfx, gfx_lock_now()) != LGP_ENHANCED))
            _snprintf_s(why, sizeof why, _TRUNCATE, "LoC frisch: nicht Enhanced (%d)", lcfg_gfx_preset(&g_gfx, gfx_lock_now()));
        if (!why[0] && (xexp_preset(LGP_ENHANCED) || g_orig_mask != 0 || ui_visible_on(ui_find(ID_XWARN_LOC), P_PLAY)))
            _snprintf_s(why, sizeof why, _TRUNCATE, "LoC Enhanced experimentell (Maske 0x%X)", g_orig_mask);
        if (!why[0] && u8_GetFileAttributes(lcf) != INVALID_FILE_ATTRIBUTES)
            strcpy_s(why, sizeof why, "LoC: Laden legt user_loc.cfg an");
    }
    if (!why[0]) { /* Original waehlen -> Original (nicht Custom), Enhanced zurueck */
        char t[400];
        on_command(ID_GP0 + LGP_ORIGINAL, BN_CLICKED);
        if (!ui_checked(ID_GP0 + LGP_ORIGINAL) || ui_checked(ID_GP0 + LGP_CUSTOM))
            strcpy_s(why, sizeof why, "LoC: Klick Original zeigt nicht Original");
        if (!why[0] && (g_gfx.shx[LSHX_SHADOW] != 1 || g_gfx.shx[LSHX_SOFT] != 0 || g_gfx.shx[LSHX_SUN] != 0 ||
                        (lorig_mask(&g_gfx, 0) & (1u << SB_OFG_LIGHT)))) /* W471: LoC-Original = Schatten des Spiels, nicht Experimental */
            _snprintf_s(why, sizeof why, _TRUNCATE, "W471 LoC Original: Schatten %d/%d/%d Maske 0x%X", g_gfx.shx[LSHX_SHADOW],
                        g_gfx.shx[LSHX_SOFT], g_gfx.shx[LSHX_SUN], lorig_mask(&g_gfx, 0));
        on_command(ID_GP0 + LGP_ENHANCED, BN_CLICKED);
        ui_gettext(ctl(ID_GP0 + LGP_ENHANCED), t, sizeof t);
        if (!why[0] && (!ui_checked(ID_GP0 + LGP_ENHANCED) || strncmp(t, LT("gui.gfx_p_enh"), strlen(LT("gui.gfx_p_enh"))) != 0 ||
                        !strstr(t, LT("gui.gfx_p_enh_txt"))))
            _snprintf_s(why, sizeof why, _TRUNCATE, "LoC: Enhanced '%.80s'", t);
        on_command(ID_GP0 + LGP_STRONG, BN_CLICKED); /* unsichtbar: keine Wirkung */
        if (!why[0] && !ui_checked(ID_GP0 + LGP_ENHANCED))
            strcpy_s(why, sizeof why, "LoC: unsichtbares Preset waehlbar");
        /* W466: Effekte zeigt bei Enhanced "Enhanced" (W478: 3 Stufen mit Vivid, kein "Benutzerdefiniert"), Original -> "Original" */
        if (!why[0] && (ComboBox_GetCount(ctl(ID_GFX_FX)) != 3 || ComboBox_GetCurSel(ctl(ID_GFX_FX)) != 1))
            _snprintf_s(why, sizeof why, _TRUNCATE, "LoC: Effekte bei Enhanced %d/%d", ComboBox_GetCurSel(ctl(ID_GFX_FX)),
                        ComboBox_GetCount(ctl(ID_GFX_FX)));
        if (!why[0]) {
            ComboBox_SetCurSel(ctl(ID_GFX_FX), 0);
            on_command(ID_GFX_FX, CBN_SELCHANGE);
            if (ComboBox_GetCurSel(ctl(ID_GFX_FX)) != 0 || ComboBox_GetCount(ctl(ID_GFX_FX)) != 3 ||
                g_gfx.fx[LFX_LCKP] != lcfg_gfx_preset_value(LGP_ORIGINAL, LGK_FX0 + LFX_LCKP) || !ui_checked(ID_GP0 + LGP_CUSTOM))
                strcpy_s(why, sizeof why, "LoC: Effekte Original");
            on_command(ID_GP0 + LGP_ENHANCED, BN_CLICKED);
        }
    }
    if (!why[0]) { /* Custom basiert auf Enhanced; Speichern schreibt die Marke */
        ComboBox_SetCurSel(ctl(ID_AAMODE), lcfg_gfx_preset_value(LGP_ENHANCED, LSHX_AA) == 3 ? 2 : 3);
        on_command(ID_AAMODE, CBN_SELCHANGE);
        if (!ui_checked(ID_GP0 + LGP_CUSTOM) || lcfg_gfx_base(&g_gfx, gfx_lock_now()) != LGP_ENHANCED)
            strcpy_s(why, sizeof why, "LoC: Custom nicht auf Enhanced");
        on_command(ID_GP_RESET, BN_CLICKED);
        if (!why[0] && !ui_checked(ID_GP0 + LGP_ENHANCED))
            strcpy_s(why, sizeof why, "LoC: Zuruecksetzen nicht Enhanced");
        if (!why[0] && (do_save(0) != 0 || (gt_read(lcf, buf, sizeof buf), locwide_cfg_count(buf, "gfx_loc_rev=3\n") != 1)))
            _snprintf_s(why, sizeof why, _TRUNCATE, "LoC: Speichern ohne Marke '%.100s'", buf);
        if (!why[0] && (gt_read(cfg, buf, sizeof buf), strstr(buf, "gfx_loc_rev")))
            strcpy_s(why, sizeof why, "LoC: Marke in user.cfg (SB)");
    }
    if (!why[0]) { /* Back to original (LoC): Original-Preset + loc_sblook=0 */
        g_loc.loc_sblook = 1;
        ComboBox_SetCurSel(ctl(ID_AAMODE), lcfg_gfx_preset_value(LGP_ENHANCED, LSHX_AA) == 3 ? 2 : 3);
        on_command(ID_AAMODE, CBN_SELCHANGE);
        orig_reset_now();
        ui_to_settings(&a);
        if (!w471_look_is(&a, LGP_ORIGINAL, 1) || a.shx[LSHX_SHADOW] != 1 ||
            a.monitor_fx != lcfg_gfx_preset_value(LGP_ORIGINAL, LGK_MFX) || g_loc.loc_sblook != 0 ||
            lcfg_gfx_preset(&a, gfx_lock_now()) != LGP_ORIGINAL)
            _snprintf_s(why, sizeof why, _TRUNCATE, "Back to original: Preset %d, sblook %d",
                        lcfg_gfx_preset(&a, gfx_lock_now()), g_loc.loc_sblook);
    }
    if (!why[0]) { /* zurueck nach SB: dessen Liste ohne Luecken */
        loctab_set_game(LGAME_SB);
        show_page(P_GFX);
        pump();
        w465_layout_check(kSb, 4, "SB zurueck", why, sizeof why);
        if (!why[0]) { /* W471: SB-Original bleibt ohne Schatten, Back to original (SB) -> Schatten aus */
            launcher_settings_t b2;
            lcfg_defaults(&b2);
            b2.shx[LSHX_SHADOW] = 1;
            lorig_reset(&b2);
            if (b2.shx[LSHX_SHADOW] != 0 || lcfg_gfx_preset(&b2, 0) != LGP_ORIGINAL)
                strcpy_s(why, sizeof why, "W471 SB: Back to original mit Schatten");
        }
        if (!why[0])
            lay_gt_page_check(why, sizeof why, "W465 Grafik SB");
    }
    u8_DeleteFile(cfg);
    u8_DeleteFile(lcf);
    g_ucfg_test[0] = '\0';
    g_loc = keep_loc;
    memset(g_pg_ok, 0, sizeof g_pg_ok);
    g_game = keep_game;
    g_s = keep_s;
    g_disk_base = keep_base;
    g_disk_base_ok = keep_ok;
    ui_from_settings(&g_s);
    loctab_nav_layout();
    show_page(keep_page);
    ui_apply_rules();
    if (!why[0]) {
        GT_OK("74e W465 LOC-ENHANCED (SB Remastered -> Original, LoC ohne Marke -> Enhanced mit Bildrate, Marke gfx_loc_rev, "
              "loc_sblook -> 0, frisch Enhanced ohne Experimental, Seite Grafik ohne Luecken SB/LoC, Custom auf Enhanced, "
              "Back to original; W471: LoC-Original mit Schatten, Umstellung Rev 1/2 Original/Enhanced/Custom)");
    } else {
        GT_FAIL("74e W465 LOC-ENHANCED", "%s", why);
    }
}

/* W476 LAUNCHER-CUSTOM-PRESET (Fall 74f): "Custom" ist anklickbar. Klick uebernimmt die wirksamen Werte bitgleich,
 * die Wahl wird gemerkt (launcher_gfx_custom, Neustart), Einzeloption -> Custom, Original/Enhanced/Performance
 * setzen ihre Werte (ohne Rueckfrage, wenn Custom nur Preset-Werte hat), Migrationen W296/W465/W466/W471 lassen
 * ausdruecklich gewaehltes Custom stehen; SB und LoC. user.cfg/user_loc.cfg = Temp-Dateien (g_ucfg_test). */
static int w476_same(const launcher_settings_t *a, const launcher_settings_t *b)
{
    launcher_settings_t x = *a, y = *b;
    x.gfx_custom = y.gfx_custom = 0;
    return memcmp(&x, &y, sizeof x) == 0;
}

static void w476_now(launcher_settings_t *r)
{
    *r = g_s;
    ui_to_settings(r);
}

/* SB speichern wie do_save, ohne Spieldaten-Pruefung (Community-Selbsttest hat kein Abbild) */
static int w476_save_sb(const char *path)
{
    ui_to_settings(&g_s);
    return save_user_merged(path) < 0 ? -1 : 0;
}

/* Optik-Schluessel (k < LGK_HD) von a und b gleich; with_sun = shadow_sun mitpruefen */
static int w476_look_same(const launcher_settings_t *a, const launcher_settings_t *b, int with_sun)
{
    int k;
    for (k = 0; k < LGK_HD; k++) {
        if (lcfg_gfx_owned(k) && (with_sun || k != LSHX_SUN) && lcfg_gfx_value(a, k) != lcfg_gfx_value(b, k)) {
            return 0;
        }
    }
    return 1;
}

/* Klick auf Custom im aktiven Spiel (vorher Preset from): Werte bitgleich, Auswahl, Erkennung, Text, Reset */
static void w476_click_custom(int from, const char *tag, char *why, size_t cap)
{
    launcher_settings_t r0, r1;
    lgp_mask_t lock = gfx_lock_now();
    char t[600], n[160], sub[300];
    w476_now(&r0);
    ui_gettext(ctl(ID_GP0 + LGP_CUSTOM), t, sizeof t);
    if (!ui_checked(ID_GP0 + from) || ui_checked(ID_GP0 + LGP_CUSTOM) || r0.gfx_custom) {
        _snprintf_s(why, cap, _TRUNCATE, "%s: vorher nicht Preset %d", tag, from);
        return;
    }
    if (!strstr(t, LT("gui.gfx_p_custom_off"))) {
        _snprintf_s(why, cap, _TRUNCATE, "%s: Text vorher '%.80s'", tag, t);
        return;
    }
    on_command(ID_GP0 + LGP_CUSTOM, BN_CLICKED);
    pump();
    w476_now(&r1);
    gfx_name(from, &r1, n, sizeof n);
    _snprintf_s(sub, sizeof sub, _TRUNCATE, LT("gui.gfx_p_custom_txt"), n);
    ui_gettext(ctl(ID_GP0 + LGP_CUSTOM), t, sizeof t);
    if (!w476_same(&r0, &r1)) {
        _snprintf_s(why, cap, _TRUNCATE, "%s: Klick auf Custom veraendert Werte", tag);
    } else if (!r1.gfx_custom || !ui_checked(ID_GP0 + LGP_CUSTOM) || ui_checked(ID_GP0 + from)) {
        _snprintf_s(why, cap, _TRUNCATE, "%s: Custom nicht gewaehlt (%d)", tag, r1.gfx_custom);
    } else if (lcfg_gfx_preset(&r1, lock) != LGP_CUSTOM || lcfg_gfx_match(&r1, lock) != from ||
               lcfg_gfx_base(&r1, lock) != from) {
        _snprintf_s(why, cap, _TRUNCATE, "%s: Erkennung %d/%d/%d", tag, lcfg_gfx_preset(&r1, lock),
                    lcfg_gfx_match(&r1, lock), lcfg_gfx_base(&r1, lock));
    } else if (!strstr(t, sub) || !ui_visible_on(ui_find(ID_GP_RESET), P_GFX)) {
        _snprintf_s(why, cap, _TRUNCATE, "%s: Text/Reset nachher '%.80s'", tag, t);
    }
    if (!why[0]) { /* zweiter Klick: bleibt Custom, nichts aendert sich */
        on_command(ID_GP0 + LGP_CUSTOM, BN_CLICKED);
        pump();
        w476_now(&r0);
        if (memcmp(&r0, &r1, sizeof r0) != 0 || !ui_checked(ID_GP0 + LGP_CUSTOM)) {
            _snprintf_s(why, cap, _TRUNCATE, "%s: zweiter Klick auf Custom", tag);
        }
    }
}

/* Preset p waehlen: Werte des Presets, keine Custom-Wahl mehr */
static void w476_pick(int p, const char *tag, char *why, size_t cap)
{
    launcher_settings_t r;
    on_command(ID_GP0 + p, BN_CLICKED);
    pump();
    w476_now(&r);
    if (!ui_checked(ID_GP0 + p) || ui_checked(ID_GP0 + LGP_CUSTOM) || r.gfx_custom ||
        lcfg_gfx_diff(&r, p, gfx_lock_now(), NULL) != 0) {
        _snprintf_s(why, cap, _TRUNCATE, "%s: Preset %d nicht gesetzt (custom %d, diff %d)", tag, p, r.gfx_custom,
                    lcfg_gfx_diff(&r, p, gfx_lock_now(), NULL));
    }
}

__declspec(noinline) void w476_gui_selftest(int *pass, int *fail)
{
    static char txt[8192], buf[16384];
    char why[300] = "", tmp[1024], cfg[1200], lcf[1200], ex[200];
    launcher_settings_t a, b, keep_s = g_s, keep_base = g_disk_base;
    int keep_ok = g_disk_base_ok, keep_game = g_game, keep_page = g_page, keep_conf = g_confirm_auto, i;
    lgame_state_t keep_loc = g_loc;
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw476_user_%lu.cfg", tmp, GetCurrentProcessId());
    lgame_cfg_path(cfg, LGAME_LOC, lcf, sizeof lcf);
    g_game = LGAME_SB;
    /* a: Erkennung mit Wahl, Preset loescht die Wahl, Speichern/Laden (Neustart) */
    lcfg_defaults(&a);
    a.gfx_custom = 1;
    if (lcfg_gfx_preset(&a, 0) != LGP_CUSTOM || lcfg_gfx_match(&a, 0) != LGP_ORIGINAL || lcfg_gfx_base(&a, 0) != LGP_ORIGINAL)
        strcpy_s(why, sizeof why, "a: Erkennung mit Custom-Wahl");
    b = a;
    lcfg_gfx_apply(&b, LGP_PERFORMANCE);
    if (!why[0] && (b.gfx_custom || lcfg_gfx_preset(&b, 0) != LGP_PERFORMANCE))
        strcpy_s(why, sizeof why, "a: Preset loescht die Custom-Wahl nicht");
    for (i = 0; i < 2 && !why[0]; i++) {
        u8_DeleteFile(cfg);
        a.gfx_custom = i == 0;
        if (lcfg_save_user(cfg, &a) != 0) {
            strcpy_s(why, sizeof why, "a: lcfg_save_user");
            break;
        }
        gt_read(cfg, buf, sizeof buf);
        if (locwide_cfg_count(buf, i == 0 ? "launcher_gfx_custom=1\n" : "launcher_gfx_custom=0\n") != 1) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "a%d: Datei ohne launcher_gfx_custom", i);
            break;
        }
        lcfg_defaults(&b); /* wie app_load_effective */
        b.gfx_preset_rev = 0;
        lcfg_load_file(cfg, &b);
        lcfg_gfx_migrate_fps(&b);
        (void)lcfg_gfx_migrate_remaster(&b);
        if (b.gfx_custom != (i == 0) || lcfg_gfx_preset(&b, 0) != (i == 0 ? LGP_CUSTOM : LGP_ORIGINAL) || !w476_same(&a, &b))
            _snprintf_s(why, sizeof why, _TRUNCATE, "a%d: nach Neustart custom %d preset %d", i, b.gfx_custom,
                        lcfg_gfx_preset(&b, 0));
    }
    /* m: W465 Remastered -> Original und W296 altes Original 120 -> auto greifen nicht bei Custom */
    for (i = 0; i < 2 && !why[0]; i++) {
        w465_cfg_text(txt, sizeof txt, LGP_SUBTLE, i ? "launcher_gfx_custom=1\n" : "");
        write_file(cfg, txt);
        lcfg_defaults(&a);
        lcfg_load_file(cfg, &a);
        b = a;
        if (lcfg_gfx_migrate_remaster(&a) != (i == 0) || (i && (!w476_same(&a, &b) || !a.gfx_custom)))
            _snprintf_s(why, sizeof why, _TRUNCATE, "m%d: W465 Remastered -> Original (Custom %d)", i, i);
    }
    for (i = 0; i < 2 && !why[0]; i++) {
        _snprintf_s(ex, sizeof ex, _TRUNCATE, "interp_fps=%d\nfs_interp_match=1\n%s", SB_GP_LEGACY_ORIG_FPS,
                    i ? "launcher_gfx_custom=1\n" : "");
        w465_cfg_text(txt, sizeof txt, LGP_ORIGINAL, ex);
        write_file(cfg, txt);
        lcfg_defaults(&a);
        a.gfx_preset_rev = 0;
        lcfg_load_file(cfg, &a);
        if (lcfg_gfx_migrate_fps(&a) != (i == 0) || a.interp_fps != (i ? SB_GP_LEGACY_ORIG_FPS : SB_FPS_AUTO))
            _snprintf_s(why, sizeof why, _TRUNCATE, "m%d: W296 120 -> auto (Custom %d, fps %d)", i + 2, i, a.interp_fps);
    }
    /* l: LoC-Umstellung (W465 Rev 0, W466/W471 Rev 1/2) mit launcher_gfx_custom=1: Werte bleiben, Marke 3 */
    for (i = 0; i < 4 && !why[0]; i++) {
        static const char *const kEx[4] = { "launcher_gfx_custom=1\n", "launcher_gfx_custom=1\ngfx_loc_rev=2\n",
                                            "launcher_gfx_custom=1\nshadow_sun=140\ngfx_loc_rev=2\n",
                                            "launcher_gfx_custom=1\ngfx_loc_rev=1\n" };
        if (i == 0) {
            w465_cfg_text(txt, sizeof txt, LGP_ORIGINAL, kEx[i]);
        } else {
            w471_cfg_text(txt, sizeof txt, i == 3 ? -2 : -1, kEx[i]);
        }
        write_file(lcf, txt);
        lcfg_defaults(&b);
        lcfg_load_file(lcf, &b);
        if (lgame_loc_gfx_migrate(lcf) != 1 || lgame_loc_rev(lcf) != SB_GP_LOC_REV) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "l%d: keine Marke", i);
            break;
        }
        lcfg_defaults(&a);
        lcfg_load_file(lcf, &a);
        gt_read(lcf, buf, sizeof buf);
        if (!a.gfx_custom || !w476_look_same(&a, &b, i == 0 || i == 2)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "l%d: Custom umgestellt (con %d -> %d)", i, b.shx[LSHX_CON],
                        a.shx[LSHX_CON]);
        } else if (i == 0 && (locwide_cfg_count(buf, "loc_sblook=") != 0 || locwide_cfg_count(buf, "shading=") != 1)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "l0: Datei '%.120s'", buf);
        } else if (lgame_loc_gfx_migrate(lcf) != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "l%d: zweiter Lauf", i);
        }
    }
    /* g: Oberflaeche SB (Original, Performance) und LoC (Enhanced) */
    write_file(cfg, "[display]\nwidth=1280\n# fremde Zeile\nlauncher_pergame=1\nlauncher_loc169=1\n");
    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, cfg);
    u8_DeleteFile(lcf);
    memset(g_pg_ok, 0, sizeof g_pg_ok);
    g_loc.loc_widescreen = LGAME_WIDE_169;
    g_loc.loc_sblook = 0;
    if (!why[0]) {
        g_game = LGAME_SB;
        lcfg_defaults(&g_s);
        disk_base_sync(&g_s);
        ui_from_settings(&g_s);
        show_page(P_GFX);
        pump();
        w476_click_custom(LGP_ORIGINAL, "SB", why, sizeof why);
    }
    if (!why[0]) { /* gemerkt: Speichern, Neustart (Laden wie app_load_effective, ohne Client-Ordner) */
        if (w476_save_sb(cfg) != 0 || (gt_read(cfg, buf, sizeof buf), locwide_cfg_count(buf, "launcher_gfx_custom=1\n") != 1)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "SB: Speichern '%.100s'", buf);
        } else {
            lcfg_defaults(&g_s);
            g_s.gfx_preset_rev = 0;
            lcfg_load_file(cfg, &g_s);
            lcfg_gfx_migrate_fps(&g_s);
            (void)lcfg_gfx_migrate_remaster(&g_s);
            disk_base_sync(&g_s);
            ui_from_settings(&g_s);
            show_page(P_GFX);
            pump();
            w476_now(&a);
            if (!ui_checked(ID_GP0 + LGP_CUSTOM) || ui_checked(ID_GP0 + LGP_ORIGINAL) || !a.gfx_custom ||
                lcfg_gfx_match(&a, gfx_lock_now()) != LGP_ORIGINAL)
                strcpy_s(why, sizeof why, "SB: nach Neustart nicht Custom");
        }
    }
    if (!why[0]) { /* Custom mit Preset-Werten: Performance ohne Rueckfrage (Antwort Nein), dann Custom ab Performance */
        g_confirm_auto = 0;
        w476_pick(LGP_PERFORMANCE, "SB Performance", why, sizeof why);
        if (!why[0])
            w476_click_custom(LGP_PERFORMANCE, "SB ab Performance", why, sizeof why);
        if (!why[0])
            w476_pick(LGP_ORIGINAL, "SB Original", why, sizeof why);
    }
    if (!why[0]) { /* Einzeloption -> Custom (aus den Werten), Original setzt Werte (Rueckfrage Ja) */
        ComboBox_SetCurSel(ctl(ID_GFX_LIGHT), g_gfx.shx[LSHX_SHADING] ? 1 : 0);
        on_command(ID_GFX_LIGHT, CBN_SELCHANGE);
        pump();
        w476_now(&a);
        if (!ui_checked(ID_GP0 + LGP_CUSTOM) || ui_checked(ID_GP0 + LGP_ORIGINAL) ||
            lcfg_gfx_match(&a, gfx_lock_now()) != LGP_CUSTOM)
            strcpy_s(why, sizeof why, "SB: Einzeloption nicht Custom");
        g_confirm_auto = 1;
        if (!why[0])
            w476_pick(LGP_ORIGINAL, "SB Original nach Einzeloption", why, sizeof why);
        if (!why[0] && (w476_save_sb(cfg) != 0 || (gt_read(cfg, buf, sizeof buf), locwide_cfg_count(buf, "launcher_gfx_custom=0\n") != 1)))
            _snprintf_s(why, sizeof why, _TRUNCATE, "SB: Speichern nach Original '%.100s'", buf);
    }
    if (!why[0]) { /* LoC: frisch Enhanced -> Custom, gemerkt in user_loc.cfg (nicht user.cfg) */
        loctab_set_game(LGAME_LOC);
        show_page(P_GFX);
        pump();
        w476_click_custom(LGP_ENHANCED, "LoC", why, sizeof why);
        if (!why[0] && (do_save(0) != 0 || (gt_read(lcf, buf, sizeof buf), locwide_cfg_count(buf, "launcher_gfx_custom=1\n") != 1)))
            _snprintf_s(why, sizeof why, _TRUNCATE, "LoC: Speichern '%.100s'", buf);
        if (!why[0] && (gt_read(cfg, buf, sizeof buf), locwide_cfg_count(buf, "launcher_gfx_custom=0\n") != 1))
            strcpy_s(why, sizeof why, "LoC: Custom-Wahl in user.cfg (SB)");
        if (!why[0]) {
            if (lgame_loc_gfx_migrate(lcf) != 0) {
                strcpy_s(why, sizeof why, "LoC: Umstellung nach Speichern");
            } else {
                pergame_load(LGAME_LOC, &g_s);
                disk_base_sync(&g_s);
                ui_from_settings(&g_s);
                show_page(P_GFX);
                pump();
                w476_now(&a);
                if (!ui_checked(ID_GP0 + LGP_CUSTOM) || ui_checked(ID_GP0 + LGP_ENHANCED) || !a.gfx_custom ||
                    lcfg_gfx_match(&a, gfx_lock_now()) != LGP_ENHANCED)
                { /* match != Enhanced: ein Preset-Schluessel wird nicht gespeichert (W476: shadow_sun fehlte) */
                    lgp_mask_t dm = 0;
                    lcfg_gfx_diff(&a, LGP_ENHANCED, gfx_lock_now(), &dm);
                    _snprintf_s(why, sizeof why, _TRUNCATE, "LoC: nach Neustart nicht Custom (%d%d custom %d match %d diff 0x%llX)",
                                ui_checked(ID_GP0 + LGP_CUSTOM), ui_checked(ID_GP0 + LGP_ENHANCED), a.gfx_custom,
                                lcfg_gfx_match(&a, gfx_lock_now()), dm);
                }
            }
        }
        if (!why[0]) {
            g_confirm_auto = 0;
            w476_pick(LGP_ORIGINAL, "LoC Original", why, sizeof why);
            w476_now(&a);
            if (!why[0] && !w471_look_is(&a, LGP_ORIGINAL, 1))
                strcpy_s(why, sizeof why, "LoC: Original nicht LoC-Original");
            if (!why[0])
                w476_click_custom(LGP_ORIGINAL, "LoC ab Original", why, sizeof why);
            if (!why[0])
                w476_pick(LGP_ENHANCED, "LoC Enhanced", why, sizeof why);
        }
        loctab_set_game(LGAME_SB);
    }
    u8_DeleteFile(cfg);
    u8_DeleteFile(lcf);
    g_ucfg_test[0] = '\0';
    g_loc = keep_loc;
    memset(g_pg_ok, 0, sizeof g_pg_ok);
    g_confirm_auto = keep_conf;
    g_game = keep_game;
    g_s = keep_s;
    g_disk_base = keep_base;
    g_disk_base_ok = keep_ok;
    ui_from_settings(&g_s);
    loctab_nav_layout();
    show_page(keep_page);
    ui_apply_rules();
    if (!why[0]) {
        GT_OK("74f W476 CUSTOM (Klick auf Custom uebernimmt Werte bitgleich, bleibt nach Neustart, Einzeloption -> Custom, "
              "Original/Performance/Enhanced setzen Werte, Migrationen W296/W465/W466/W471 lassen Custom stehen, SB + LoC)");
    } else {
        GT_FAIL("74f W476 CUSTOM", "%s", why);
    }
}

/* W478 LOC-ENHANCED-VIVID (Fall 74g): zweites Enhanced-Preset "Enhanced Vivid" nur in LoC.
 * t: Tabelle - Vivid = Enhanced bis W471 (kSbGpEnhRev2) in allen Zeilen, weicht von Enhanced genau in Kontrast,
 *    Bloom, Saettigung, Farbtemperatur, Gain, Sonne ab (110/150/110/6/105/140); Enhanced/Original unveraendert;
 *    LoC sichtbar, SB nicht; Vivid-Werte in SB = Custom; nicht experimentell in LoC.
 * m: Migration - Rev 2 altes Enhanced (gleiche Werte wie Vivid) -> Enhanced, nicht Vivid; Rev 3 mit Vivid bleibt.
 * g: Oberflaeche LoC (Temp-user_loc.cfg) - Klick Vivid (Werte bitgleich Tabelle, Text, Effekte "Vivid"), Seite
 *    ohne Ueberlappung (normal + gross), Speichern -> Neustart = Vivid, Custom aus Vivid, Effekte Enhanced/Vivid,
 *    Back to original; SB: Vivid unsichtbar und ohne Wirkung. */
static void w478_lbtext(int id, int i, char *out, int cap)
{
    wchar_t w[200];
    out[0] = '\0';
    if (SendMessageW(ctl(id), CB_GETLBTEXTLEN, (WPARAM)i, 0) < 200 &&
        SendMessageW(ctl(id), CB_GETLBTEXT, (WPARAM)i, (LPARAM)w) != CB_ERR) {
        w_to_u8(w, out, cap);
    }
}

__declspec(noinline) void w478_gui_selftest(int *pass, int *fail)
{
    static char txt[8192], buf[16384];
    static const int kDiffRow[6] = { 2, 5, 17, 18, 21, SB_GP_SUN }; /* world_contrast, bloom, sat, temp, gain, sun */
    static const int kDiffVal[6] = { 110, 150, 110, 6, 105, 140 };
    static const int kEnhVal[6] = { 100, 130, 95, 0, 100, 115 };   /* Enhanced W471 (unveraendert) */
    char why[300] = "", tmp[1024], cfg[1200], lcf[1200], t[600];
    launcher_settings_t a, keep_s = g_s, keep_base = g_disk_base;
    int keep_ok = g_disk_base_ok, keep_game = g_game, keep_page = g_page, keep_conf = g_confirm_auto, i, k;
    int shot0 = g_o.shot, id0 = g_lay.id, sz0 = g_lay.size;
    lgame_state_t keep_loc = g_loc;
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw478_user_%lu.cfg", tmp, GetCurrentProcessId());
    lgame_cfg_path(cfg, LGAME_LOC, lcf, sizeof lcf);
    /* t: Tabelle */
    if (sb_gp_vivid_check() != -1) {
        _snprintf_s(why, sizeof why, _TRUNCATE, "t: Vivid != Enhanced bis W471 in Zeile %d", sb_gp_vivid_check());
    }
    for (k = 0, i = 0; k < SB_GP_NKEY && !why[0]; k++) {
        int j, d = kSbGfxPreset[k].v[SB_GP_VIVID] != kSbGfxPreset[k].v[SB_GP_ENHANCED];
        for (j = 0; j < 6 && kDiffRow[j] != k; j++) { }
        if (d != (j < 6) || (j < 6 && (kSbGfxPreset[k].v[SB_GP_VIVID] != kDiffVal[j] ||
                                       kSbGfxPreset[k].v[SB_GP_ENHANCED] != kEnhVal[j]))) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "t: Zeile %s Vivid %d Enhanced %d", kSbGfxPreset[k].key,
                        kSbGfxPreset[k].v[SB_GP_VIVID], kSbGfxPreset[k].v[SB_GP_ENHANCED]);
        }
        i += d;
    }
    if (!why[0] && (i != 6 || !sb_gp_visible(SB_GP_VIVID, 1) || sb_gp_visible(SB_GP_VIVID, 0) || LGP_VIVID != SB_GP_VIVID ||
                    LGP_CUSTOM != SB_GP_NPRE))
        _snprintf_s(why, sizeof why, _TRUNCATE, "t: %d Abweichungen / Sichtbarkeit", i);
    for (k = 0; k < LGK_N && !why[0]; k++) { /* Launcher-Werte = Tabelle (bitgleich), LoC */
        int r = sb_gp_find(lcfg_gfx_key(k)), want;
        if (!lcfg_gfx_owned(k) || r < 0) { continue; }
        want = kSbGfxPreset[r].v[SB_GP_VIVID];
        if (k == LGK_FX0 + LFX_INTERP) { want = want != 0; }
        if (lcfg_gfx_preset_value_loc(LGP_VIVID, k, 1) != want)
            _snprintf_s(why, sizeof why, _TRUNCATE, "t: Launcher %s=%d, Tabelle %d", lcfg_gfx_key(k),
                        lcfg_gfx_preset_value_loc(LGP_VIVID, k, 1), want);
    }
    if (!why[0]) { /* SB: Vivid-Werte sind Custom (nicht waehlbar); LoC: Vivid erkannt, nicht experimentell */
        lcfg_gfx_set_loc(0);
        lcfg_defaults(&a);
        lcfg_gfx_apply(&a, LGP_VIVID);
        if (lcfg_gfx_match(&a, 0) != LGP_CUSTOM || lcfg_gfx_visible(LGP_VIVID))
            strcpy_s(why, sizeof why, "t: SB erkennt Vivid");
        lcfg_gfx_set_loc(1);
        lcfg_defaults(&a);
        lcfg_gfx_apply(&a, LGP_VIVID);
        if (!why[0] && (lcfg_gfx_preset(&a, 0) != LGP_VIVID || lorig_mask(&a, 0) != 0 ||
                        lcfg_fx_level_game(&a, 0, 1) != 2))
            _snprintf_s(why, sizeof why, _TRUNCATE, "t: LoC Vivid Erkennung %d Maske 0x%X Effekte %d", lcfg_gfx_preset(&a, 0),
                        lorig_mask(&a, 0), lcfg_fx_level_game(&a, 0, 1));
        lcfg_gfx_apply(&a, LGP_ENHANCED);
        if (!why[0] && (lcfg_gfx_preset(&a, 0) != LGP_ENHANCED || lcfg_fx_level_game(&a, 0, 1) != 1))
            strcpy_s(why, sizeof why, "t: LoC Enhanced Erkennung");
        lcfg_gfx_set_loc(0);
    }
    /* m: Migration (Launcher). Rev 2 altes Enhanced -> Enhanced (nicht Vivid), Rev 3 Vivid bleibt */
    for (i = 0; i < 3 && !why[0]; i++) {
        static const char *const kEx[3] = { "gfx_loc_rev=2\n", "shadow_sun=140\ngfx_loc_rev=2\n", "gfx_loc_rev=3\n" };
        if (i < 2) {
            w471_cfg_text(txt, sizeof txt, -1, kEx[i]);
        } else {
            w471_cfg_text(txt, sizeof txt, LGP_VIVID, kEx[i]);
        }
        write_file(lcf, txt);
        if (lgame_loc_gfx_migrate(lcf) != (i < 2) || lgame_loc_rev(lcf) != SB_GP_LOC_REV) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "m%d: Umstellung/Marke", i);
            break;
        }
        lcfg_defaults(&a);
        lcfg_load_file(lcf, &a);
        lcfg_gfx_set_loc(1);
        if (!w471_look_is(&a, i < 2 ? LGP_ENHANCED : LGP_VIVID, 1) || lcfg_gfx_match(&a, 0) != (i < 2 ? LGP_ENHANCED : LGP_VIVID))
            _snprintf_s(why, sizeof why, _TRUNCATE, "m%d: nach Umstellung Preset %d (Kontrast %d)", i, lcfg_gfx_match(&a, 0),
                        a.shx[LSHX_CON]);
        lcfg_gfx_set_loc(0);
    }
    lcfg_gfx_bind_game(&g_game, LGAME_LOC); /* lcfg_gfx_set_loc loest die Bindung der Oberflaeche - wiederherstellen */
    /* g: Oberflaeche */
    write_file(cfg, "[display]\nwidth=1280\n# fremde Zeile\nlauncher_pergame=1\nlauncher_loc169=1\n");
    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, cfg);
    u8_DeleteFile(lcf);
    memset(g_pg_ok, 0, sizeof g_pg_ok);
    g_loc.loc_widescreen = LGAME_WIDE_169;
    g_loc.loc_sblook = 0;
    if (!why[0]) { /* SB: Vivid unsichtbar, Klick ohne Wirkung, Effekte ohne Vivid */
        g_game = LGAME_SB;
        lcfg_defaults(&g_s);
        disk_base_sync(&g_s);
        ui_from_settings(&g_s);
        show_page(P_GFX);
        pump();
        on_command(ID_GP0 + LGP_VIVID, BN_CLICKED);
        pump();
        w478_lbtext(ID_GFX_FX, 2, t, sizeof t);
        if (!why[0] && strcmp(t, LT("gui.gfx_fx_strong")) != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "SB: Effekte Eintrag 3 '%.60s'", t);
        if (!why[0] && (ui_visible_on(ui_find(ID_GP0 + LGP_VIVID), P_GFX) || ui_checked(ID_GP0 + LGP_VIVID) ||
            !ui_checked(ID_GP0 + LGP_ORIGINAL) || g_gfx.shx[LSHX_CON] != lcfg_gfx_preset_value(LGP_ORIGINAL, LSHX_CON)))
            strcpy_s(why, sizeof why, "SB: Vivid sichtbar/waehlbar");
        if (!why[0] && ComboBox_GetCount(ctl(ID_GFX_FX)) != 3)
            strcpy_s(why, sizeof why, "SB: Effekte-Liste");
    }
    if (!why[0]) { /* LoC frisch: Enhanced; Klick Vivid -> Werte = Tabelle, Text, Effekte "Vivid", nicht experimentell */
        launcher_settings_t r;
        g_confirm_auto = 0;
        loctab_set_game(LGAME_LOC);
        show_page(P_GFX);
        pump();
        if (!ui_checked(ID_GP0 + LGP_ENHANCED) || !ui_visible_on(ui_find(ID_GP0 + LGP_VIVID), P_GFX))
            strcpy_s(why, sizeof why, "LoC frisch: nicht Enhanced / Vivid unsichtbar");
        if (!why[0])
            w476_pick(LGP_VIVID, "LoC Vivid", why, sizeof why);
        w476_now(&r);
        ui_gettext(ctl(ID_GP0 + LGP_VIVID), t, sizeof t);
        if (!why[0] && (!w471_look_is(&r, LGP_VIVID, 1) || strncmp(t, LT("gui.gfx_p_vivid"), strlen(LT("gui.gfx_p_vivid"))) != 0 ||
                        !strstr(t, LT("gui.gfx_p_vivid_txt"))))
            _snprintf_s(why, sizeof why, _TRUNCATE, "LoC Vivid: Werte/Text '%.80s'", t);
        if (!why[0] && (ComboBox_GetCount(ctl(ID_GFX_FX)) != 3 || ComboBox_GetCurSel(ctl(ID_GFX_FX)) != 2))
            _snprintf_s(why, sizeof why, _TRUNCATE, "LoC Vivid: Effekte %d/%d", ComboBox_GetCurSel(ctl(ID_GFX_FX)),
                        ComboBox_GetCount(ctl(ID_GFX_FX)));
        if (!why[0]) {
            w478_lbtext(ID_GFX_FX, 2, t, sizeof t);
            if (strcmp(t, LT("gui.gfx_p_vivid")) != 0)
                _snprintf_s(why, sizeof why, _TRUNCATE, "LoC Vivid: Effekte zeigt '%.60s'", t);
        }
        ui_apply_rules();
        pump();
        /* LoC-Raender 16:9 (Breitbild-Gruppe) zaehlen in LoC nicht (W414) - wie 74e ueber g_orig_mask */
        if (!why[0] && (xexp_preset(LGP_VIVID) || (lorig_mask(&r, 0) & ~(1u << SB_OFG_WIDE)) != 0 || g_orig_mask != 0 ||
                        ui_visible_on(ui_find(ID_XWARN_LOC), P_PLAY)))
            _snprintf_s(why, sizeof why, _TRUNCATE, "LoC Vivid experimentell (Maske 0x%X/0x%X)", lorig_mask(&r, 0), g_orig_mask);
    }
    if (!why[0]) { /* Layout: Seiten ohne Ueberlappung, normal und gross */
        g_o.shot = 2;
        lay_gt_page_check(why, sizeof why, "W478 LoC normal");
        if (!why[0]) {
            lay_switch(LLAY_A, LLAY_SZ_LARGE);
            lay_gt_page_check(why, sizeof why, "W478 LoC gross");
        }
        lay_switch(id0, sz0);
        g_o.shot = shot0;
        show_page(P_GFX);
        pump();
        if (!why[0] && !ui_checked(ID_GP0 + LGP_VIVID))
            strcpy_s(why, sizeof why, "LoC: nach Layoutwechsel nicht Vivid");
    }
    if (!why[0]) { /* Speichern -> Neustart: Vivid erkannt (keine Umstellung), Datei mit aktueller Marke */
        if (do_save(0) != 0 || (gt_read(lcf, buf, sizeof buf), locwide_cfg_count(buf, "gfx_loc_rev=3\n") != 1 ||
                                locwide_cfg_count(buf, "world_contrast=110\n") != 1 || locwide_cfg_count(buf, "shadow_sun=140\n") != 1)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "LoC: Speichern '%.100s'", buf);
        } else if (gt_read(cfg, buf, sizeof buf), strstr(buf, "world_contrast=110")) {
            strcpy_s(why, sizeof why, "LoC: Vivid in user.cfg (SB)");
        } else if (lgame_loc_gfx_migrate(lcf) != 0) {
            strcpy_s(why, sizeof why, "LoC: Umstellung nach Speichern");
        } else {
            pergame_load(LGAME_LOC, &g_s);
            disk_base_sync(&g_s);
            ui_from_settings(&g_s);
            show_page(P_GFX);
            pump();
            w476_now(&a);
            if (!ui_checked(ID_GP0 + LGP_VIVID) || ui_checked(ID_GP0 + LGP_ENHANCED) || a.gfx_custom ||
                lcfg_gfx_preset(&a, gfx_lock_now()) != LGP_VIVID || ComboBox_GetCurSel(ctl(ID_GFX_FX)) != 2) {
                lgp_mask_t dm = 0;
                lcfg_gfx_diff(&a, LGP_VIVID, gfx_lock_now(), &dm);
                _snprintf_s(why, sizeof why, _TRUNCATE, "LoC: nach Neustart nicht Vivid (preset %d diff 0x%llX)",
                            lcfg_gfx_preset(&a, gfx_lock_now()), dm);
            }
        }
    }
    if (!why[0]) { /* Custom aus Vivid (W476): Werte bitgleich, Basis Vivid; zurueck auf Vivid */
        w476_click_custom(LGP_VIVID, "LoC ab Vivid", why, sizeof why);
        if (!why[0])
            w476_pick(LGP_VIVID, "LoC Vivid nach Custom", why, sizeof why);
    }
    if (!why[0]) { /* Effekte: Enhanced -> Preset Enhanced, Vivid -> Preset Vivid, Original -> Custom */
        ComboBox_SetCurSel(ctl(ID_GFX_FX), 1);
        on_command(ID_GFX_FX, CBN_SELCHANGE);
        pump();
        if (!ui_checked(ID_GP0 + LGP_ENHANCED) || ComboBox_GetCurSel(ctl(ID_GFX_FX)) != 1)
            strcpy_s(why, sizeof why, "LoC: Effekte Enhanced -> nicht Preset Enhanced");
        if (!why[0]) {
            ComboBox_SetCurSel(ctl(ID_GFX_FX), 2);
            on_command(ID_GFX_FX, CBN_SELCHANGE);
            pump();
            if (!ui_checked(ID_GP0 + LGP_VIVID) || ComboBox_GetCurSel(ctl(ID_GFX_FX)) != 2)
                strcpy_s(why, sizeof why, "LoC: Effekte Vivid -> nicht Preset Vivid");
        }
        if (!why[0]) {
            ComboBox_SetCurSel(ctl(ID_GFX_FX), 0);
            on_command(ID_GFX_FX, CBN_SELCHANGE);
            pump();
            w476_now(&a);
            if (!ui_checked(ID_GP0 + LGP_CUSTOM) || ComboBox_GetCurSel(ctl(ID_GFX_FX)) != 0 ||
                lcfg_gfx_base(&a, gfx_lock_now()) != LGP_VIVID)
                strcpy_s(why, sizeof why, "LoC: Effekte Original aus Vivid");
            g_confirm_auto = 1; /* eigene Werte (Effekte Original) -> R14-Rueckfrage: Ja */
            if (!why[0])
                w476_pick(LGP_VIVID, "LoC Vivid nach Effekte", why, sizeof why);
        }
    }
    if (!why[0]) { /* Back to original aus Vivid -> LoC-Original (mit Schatten) */
        orig_reset_now();
        ui_to_settings(&a);
        if (!w471_look_is(&a, LGP_ORIGINAL, 1) || lcfg_gfx_preset(&a, gfx_lock_now()) != LGP_ORIGINAL)
            _snprintf_s(why, sizeof why, _TRUNCATE, "Back to original aus Vivid: Preset %d", lcfg_gfx_preset(&a, gfx_lock_now()));
    }
    loctab_set_game(LGAME_SB);
    u8_DeleteFile(cfg);
    u8_DeleteFile(lcf);
    g_ucfg_test[0] = '\0';
    g_loc = keep_loc;
    memset(g_pg_ok, 0, sizeof g_pg_ok);
    g_confirm_auto = keep_conf;
    g_game = keep_game;
    g_s = keep_s;
    g_disk_base = keep_base;
    g_disk_base_ok = keep_ok;
    ui_from_settings(&g_s);
    loctab_nav_layout();
    show_page(keep_page);
    ui_apply_rules();
    if (!why[0]) {
        GT_OK("74g W478 VIVID (Tabelle = Enhanced bis W471, nur 6 Abweichungen, Enhanced/Original unveraendert, nur LoC, "
              "nicht experimentell, Rev 2 altes Enhanced -> Enhanced, Rev 3 Vivid bleibt, Klick/Werte/Text/Effekte, "
              "Layout normal+gross, Neustart = Vivid, Custom aus Vivid, Back to original, SB ohne Vivid)");
    } else {
        GT_FAIL("74g W478 VIVID", "%s", why);
    }
}

/* W414 LOC-DEFAULT-169/-FADE (Fall 74c): LoC-Standard 16:9 + Fade out 8/100, einmalige Umstellung je user_loc.cfg
 * (alte Datei mit 4:3 -> 16:9 + Fade, zweiter Start laesst manuelles 4:3 stehen), SB unveraendert (user.cfg
 * bytegleich, Standard 4:3), 16:9 ohne Experimental-Hinweis, Preset/Back to original lassen 16:9 + Raender. */
__declspec(noinline) void loc169_gui_selftest(int *pass, int *fail)
{
    static char sb0[16384], buf[16384];
    char why[300] = "", tmp[1024], cfg[1200], lcf[1200];
    int g0 = g_game, keep_ok = g_disk_base_ok;
    lgame_state_t loc0 = g_loc;
    launcher_settings_t s0 = g_s, base0 = g_disk_base, t;
    HWND c = ctl(ID_LOC_WIDE);
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw414_user_%lu.cfg", tmp, GetCurrentProcessId());
    write_file(cfg, "[display]\nwidth=1280\n# SB-Zeile\nlauncher_pergame=1\n[render]\nedge_mode=1\n");
    gt_read(cfg, sb0, sizeof sb0);
    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, cfg);
    lgame_cfg_path(cfg, LGAME_LOC, lcf, sizeof lcf);
    u8_DeleteFile(lcf);
    /* a: Standard (keine LoC-Datei): 16:9, Raender Fade out 8/100, Laden legt nichts an */
    g_loc.loc_widescreen = LGAME_WIDE_43;
    loctab_load();
    pergame_load(LGAME_LOC, &t);
    if (g_loc.loc_widescreen != LGAME_WIDE_169 || g_loc169_migrated != 0 || (c && ComboBox_GetCurSel(c) != 1))
        _snprintf_s(why, sizeof why, _TRUNCATE, "a: Standard %d (umgestellt %d)", g_loc.loc_widescreen, g_loc169_migrated);
    else if (t.edge_mode != 2 || t.edge_fade_width != 8 || t.edge_fade_strength != 100)
        _snprintf_s(why, sizeof why, _TRUNCATE, "a: Raender %d/%d/%d", t.edge_mode, t.edge_fade_width, t.edge_fade_strength);
    else if (u8_GetFileAttributes(lcf) != INVALID_FILE_ATTRIBUTES)
        strcpy_s(why, sizeof why, "a: Laden legt user_loc.cfg an");
    /* b: alte LoC-Datei (4:3, eigene Raender) -> genau einmal 16:9 + Fade out, sofort gespeichert, fremde Zeilen bleiben */
    if (!why[0]) {
        write_file(lcf, "# eigene Zeile\n[render]\nedge_mode=1\nedge_fade_width=12\nedge_fade_strength=50\n"
                        "loc_widescreen=0\nvolume_music=70\n");
        g_loc.loc_widescreen = LGAME_WIDE_43;
        loctab_load();
        gt_read(lcf, buf, sizeof buf);
        pergame_load(LGAME_LOC, &t);
        if (g_loc169_migrated != 1 || g_loc.loc_widescreen != LGAME_WIDE_169)
            _snprintf_s(why, sizeof why, _TRUNCATE, "b: Umstellung %d, Bildformat %d", g_loc169_migrated, g_loc.loc_widescreen);
        else if (locwide_cfg_count(buf, "loc_widescreen=1\n") != 1 || locwide_cfg_count(buf, "loc_widescreen=") != 1 ||
                 locwide_cfg_count(buf, "edge_mode=2\n") != 1 || locwide_cfg_count(buf, "edge_mode=") != 1 ||
                 locwide_cfg_count(buf, "edge_fade_width=8\n") != 1 || locwide_cfg_count(buf, "edge_fade_strength=100\n") != 1 ||
                 locwide_cfg_count(buf, "launcher_loc169=1\n") != 1 || !strstr(buf, "# eigene Zeile\n") ||
                 !strstr(buf, "volume_music=70\n"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "b: user_loc.cfg '%.160s'", buf);
        else if (t.edge_mode != 2 || t.edge_fade_width != 8 || t.edge_fade_strength != 100 || t.volume_music != 70)
            _snprintf_s(why, sizeof why, _TRUNCATE, "b: geladen %d/%d/%d", t.edge_mode, t.edge_fade_width, t.edge_fade_strength);
        else if (gt_read(cfg, buf, sizeof buf), strcmp(buf, sb0) != 0) /* Umstellung schreibt nie in user.cfg */
            _snprintf_s(why, sizeof why, _TRUNCATE, "b: user.cfg (SB) veraendert '%.160s'", buf);
    }
    /* c: manuell 4:3 + harte Raender -> zweiter Start stellt nicht erneut um */
    if (!why[0] && c) {
        g_game = LGAME_LOC;
        ComboBox_SetCurSel(c, 0);
        on_command(ID_LOC_WIDE, CBN_SELCHANGE);
        gt_cfg_set(lcf, "edge_mode", "1");
        g_loc.loc_widescreen = -1;
        loctab_load();
        gt_read(lcf, buf, sizeof buf);
        if (g_loc169_migrated != 0 || g_loc.loc_widescreen != LGAME_WIDE_43 || ComboBox_GetCurSel(c) != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "c: zweiter Start %d/%d", g_loc169_migrated, g_loc.loc_widescreen);
        else if (locwide_cfg_count(buf, "loc_widescreen=0\n") != 1 || locwide_cfg_count(buf, "edge_mode=1\n") != 1 ||
                 locwide_cfg_count(buf, "launcher_loc169=1\n") != 1)
            _snprintf_s(why, sizeof why, _TRUNCATE, "c: user_loc.cfg '%.160s'", buf);
    }
    /* d: neue Datei durch eine andere Wahl (4:3 ohne LoC-Datei) traegt Markierung + Rand-Standard */
    if (!why[0] && c) {
        u8_DeleteFile(lcf);
        g_loc.loc_widescreen = LGAME_WIDE_43;
        loctab_load();
        ComboBox_SetCurSel(c, 0);
        on_command(ID_LOC_WIDE, CBN_SELCHANGE);
        gt_read(lcf, buf, sizeof buf);
        if (!strstr(buf, "loc_widescreen=0\n") || !strstr(buf, "launcher_loc169=1\n") || !strstr(buf, "edge_mode=2\n"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "d: neue user_loc.cfg '%.160s'", buf);
        g_loc.loc_widescreen = -1;
        loctab_load();
        if (!why[0] && g_loc.loc_widescreen != LGAME_WIDE_43)
            _snprintf_s(why, sizeof why, _TRUNCATE, "d: neu geladen %d", g_loc.loc_widescreen);
    }
    /* e: SB unveraendert: SB-Zeilen in user.cfg wie vorher (nur die Spielwahl launcher_game kommt hinzu, W276),
     * kein LoC-Bildformat/keine Markierung dort, SB-Standard 4:3 (edge_mode=1), kein 16:9-Kasten im SB-Bereich */
    if (!why[0]) {
        gt_read(cfg, buf, sizeof buf);
        lcfg_defaults(&t);
        if (strncmp(buf, sb0, strlen(sb0)) != 0 || strstr(buf, "loc_widescreen") || strstr(buf, "launcher_loc169") ||
            strstr(buf, "edge_mode=2"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "e: user.cfg (SB) veraendert '%.160s'", buf);
        else if (t.edge_mode != 1 || (g_game = LGAME_SB, lorig_mask(&t, 0) != 0)) /* W471: Einordnung im SB-Bereich */
            strcpy_s(why, sizeof why, "e: SB-Standard nicht 4:3/original");
        else {
            g_game = LGAME_SB;
            g_loc.loc_widescreen = LGAME_WIDE_169;
            if (pergame_box_169())
                strcpy_s(why, sizeof why, "e: 16:9-Kasten im SB-Bereich");
        }
    }
    /* f: Experimental-Hinweis: LoC 16:9 + Fade out ohne Breitbild, LoC 4:3 + Fade out und SB mit Fade out weiter Breitbild */
    if (!why[0]) {
        unsigned m169, m43, msb;
        g_game = LGAME_LOC;
        g_loc.loc_widescreen = LGAME_WIDE_169;
        edge_ui_set(2, 8, 100);
        ui_apply_rules();
        m169 = g_orig_mask;
        if (ui_visible_on(ui_find(ID_XEXP_DISP), P_DISPLAY) || ui_visible_on(ui_find(ID_LOC_WIDE_XI), P_ADV) != LOC_WIDE_720P)
            strcpy_s(why, sizeof why, "f: Kolben an Breitbild/Bildformat bei LoC 16:9");
        g_loc.loc_widescreen = LGAME_WIDE_43;
        ui_apply_rules();
        m43 = g_orig_mask;
        if (!why[0] && !ui_visible_on(ui_find(ID_XEXP_DISP), P_DISPLAY))
            strcpy_s(why, sizeof why, "f: Kolben Breitbild fehlt bei LoC 4:3 (Hor+ experimentell)");
        g_game = LGAME_SB;
        ui_apply_rules();
        msb = g_orig_mask;
        if (!why[0] && ((m169 & (1u << SB_OFG_WIDE)) || !(m43 & (1u << SB_OFG_WIDE)) || !(msb & (1u << SB_OFG_WIDE))))
            _snprintf_s(why, sizeof why, _TRUNCATE, "f: Maske 16:9 0x%X / 4:3 0x%X / SB 0x%X", m169, m43, msb);
    }
    /* g: Preset "Original" bleibt in LoC 16:9 trotz Fade out erkannt (Raender gesperrt), in SB nicht */
    if (!why[0]) {
        lcfg_defaults(&t);
        t.edge_mode = 2;
        t.shx[LSHX_SHADOW] = 1; /* W471: LoC-Original = mit den Schatten des Spiels */
        g_game = LGAME_LOC;
        g_loc.loc_widescreen = LGAME_WIDE_169;
        if (lcfg_gfx_preset(&t, gfx_lock_now()) != LGP_ORIGINAL)
            strcpy_s(why, sizeof why, "g: LoC 16:9 + Fade out nicht als Original erkannt");
        g_game = LGAME_SB;
        t.shx[LSHX_SHADOW] = 0;
        if (!why[0] && lcfg_gfx_preset(&t, gfx_lock_now()) == LGP_ORIGINAL)
            strcpy_s(why, sizeof why, "g: SB mit Fade out als Original erkannt");
    }
    /* h: Back to original im LoC-Tab: Bildgebung original, 16:9 und Fade out bleiben */
    if (!why[0]) {
        g_game = LGAME_LOC;
        g_loc.loc_widescreen = LGAME_WIDE_169;
        edge_ui_set(2, 8, 100);
        orig_reset_now();
        if (g_loc.loc_widescreen != LGAME_WIDE_169 || g_s.edge_mode != 2 || g_s.edge_fade_width != 8)
            _snprintf_s(why, sizeof why, _TRUNCATE, "h: nach Back to original %d, Raender %d/%d", g_loc.loc_widescreen,
                        g_s.edge_mode, g_s.edge_fade_width);
    }
    u8_DeleteFile(lcf);
    u8_DeleteFile(cfg);
    g_ucfg_test[0] = '\0';
    g_loc = loc0;
    g_game = g0;
    g_s = s0;
    g_disk_base = base0;
    g_disk_base_ok = keep_ok;
    ui_from_settings(&g_s);
    loctab_wide_sync();
    loctab_nav_layout();
    ui_apply_rules();
    show_page(P_PLAY);
    if (!why[0]) {
        GT_OK("74c W414 LOC-DEFAULT-169 (LoC-Standard 16:9 + Fade out 8/100, Einmal-Umstellung alte user_loc.cfg 4:3 -> 16:9 + Fade, "
              "zweiter Start laesst 4:3 stehen, neue Datei markiert, user.cfg/SB unveraendert, 16:9 ohne Experimental-Hinweis, "
              "Preset/Back to original lassen 16:9 + Raender)");
    } else {
        GT_FAIL("74c W414 LOC-DEFAULT-169", "%s", why);
    }
}

__declspec(noinline) void fpsmon_gui_selftest(int *pass, int *fail)
{
    static const struct { const char *sim; int n; } kL[] = {
        { "60", 3 }, { "144", 8 }, { "165", 9 }, { "240", 10 }, { "60/144,165", 3 }, { "0", 10 },
    };
    char why[300] = "", t[300], f[400], cd0[sizeof g_client_dir], tmp[1024], dir[1100];
    static launcher_settings_t s0, s1, b; /* static: Stapel der Selbsttest-Kette ist knapp */
    int hz0 = g_disp_hz, mx0 = g_disp_hz_max, alt0 = g_mon_alt_hz, shot0 = g_o.shot, keep0 = g_ip_keep, k;
    strcpy_s(cd0, sizeof cd0, g_client_dir);
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%sw309_uicfg_%lu", tmp, GetCurrentProcessId());
    lcfg_mkdir_p(dir);
    strcpy_s(g_client_dir, sizeof g_client_dir, dir); /* launcher_ui.cfg nur im Temp-Ordner */
    g_o.shot = 2; /* nichts speichern */
    memset(&s0, 0, sizeof s0);
    ui_to_settings(&s0);
    show_page(P_DISPLAY);
    for (k = 0; k < (int)(sizeof kL / sizeof kL[0]) && !why[0]; k++) {
        s1 = s0;
        s1.interp_fps = SB_FPS_AUTO;
        mon_sim_apply(kL[k].sim);
        ui_from_settings(&s1);
        if (ComboBox_GetCount(ctl(ID_IPFPS)) != kL[k].n || ComboBox_GetCurSel(ctl(ID_IPFPS)) != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "%s Hz: %d Eintraege (Wahl %d) statt %d", kL[k].sim,
                        ComboBox_GetCount(ctl(ID_IPFPS)), ComboBox_GetCurSel(ctl(ID_IPFPS)), kL[k].n);
        }
    }
    /* 165 Hz: 60/120 gekennzeichnet, 165 ohne Kennzeichen */
    if (!why[0]) {
        mon_sim_apply("165");
        s1 = s0;
        s1.interp_fps = 120;
        s1.window_mode = 0;
        ui_from_settings(&s1);
        ip_item_text(ComboBox_GetCurSel(ctl(ID_IPFPS)), t, sizeof t);
        _snprintf_s(f, sizeof f, _TRUNCATE, LT("gui.ip_judder"), 120);
        if (strcmp(t, f) != 0) { _snprintf_s(why, sizeof why, _TRUNCATE, "165 Hz: 120 als '%s'", t); }
        ip_item_text(ip_sel_of(165), t, sizeof t);
        if (!why[0] && strcmp(t, "165") != 0) { _snprintf_s(why, sizeof why, _TRUNCATE, "165 Hz: 165 als '%s'", t); }
        ip_item_text(ip_sel_of(60), t, sizeof t);
        _snprintf_s(f, sizeof f, _TRUNCATE, LT("gui.ip_judder"), 60);
        if (!why[0] && strcmp(t, f) != 0) { _snprintf_s(why, sizeof why, _TRUNCATE, "165 Hz: 60 als '%s'", t); }
        ui_apply_rules();
        if (!why[0] && !g_rr[10].active) { strcpy_s(why, sizeof why, "165 Hz: R10 fehlt bei 120"); }
        ComboBox_SetCurSel(ctl(ID_IPFPS), ip_sel_of(165));
        on_command(ID_IPFPS, CBN_SELCHANGE);
        if (!why[0] && g_rr[10].active) { strcpy_s(why, sizeof why, "165 Hz: R10 bei 165"); }
        ComboBox_SetCurSel(ctl(ID_IPFPS), ip_sel_of(120));
        on_command(ID_IPFPS, CBN_SELCHANGE);
        _snprintf_s(f, sizeof f, _TRUNCATE, LT("gui.ip_st_judder"), 120, 165);
        if (!why[0] && strcmp(g_status, f) != 0) { _snprintf_s(why, sizeof why, _TRUNCATE, "Status 120@165 '%.80s'", g_status); }
    }
    /* Alt-cfg 240 auf 144 Hz: eigener Eintrag, gewaehlt, Speichern behaelt 240 */
    if (!why[0]) {
        mon_sim_apply("144");
        s1 = s0;
        s1.interp_fps = 240;
        s1.window_mode = 0;
        ui_from_settings(&s1);
        ip_item_text(ComboBox_GetCurSel(ctl(ID_IPFPS)), t, sizeof t);
        _snprintf_s(f, sizeof f, _TRUNCATE, LT("gui.ip_above"), 240);
        if (strcmp(t, f) != 0 || ComboBox_GetCount(ctl(ID_IPFPS)) != 9) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Alt-cfg 240@144: '%s' (%d Eintraege)", t, ComboBox_GetCount(ctl(ID_IPFPS)));
        }
        ip_fill(ctl(ID_IPFPS)); /* z. B. Sprachwechsel: Wahl bleibt */
        ui_to_settings(&b);
        if (!why[0] && b.interp_fps != 240) { _snprintf_s(why, sizeof why, _TRUNCATE, "Alt-cfg 240 -> %d", b.interp_fps); }
        ui_apply_rules();
        if (!why[0] && !g_rr[10].active) { strcpy_s(why, sizeof why, "Alt-cfg 240@144: R10 fehlt"); }
        on_command(ID_IPFPS, CBN_SELCHANGE);
        _snprintf_s(f, sizeof f, _TRUNCATE, LT("gui.ip_st_above"), 240, 144);
        if (!why[0] && strcmp(g_status, f) != 0) { _snprintf_s(why, sizeof why, _TRUNCATE, "Status 240@144 '%.80s'", g_status); }
        ComboBox_SetCurSel(ctl(ID_IPFPS), ip_sel_of(144)); /* bewusst gewechselt */
        on_command(ID_IPFPS, CBN_SELCHANGE);
        ui_to_settings(&b);
        if (!why[0] && b.interp_fps != 144) { _snprintf_s(why, sizeof why, _TRUNCATE, "144 gewaehlt -> %d", b.interp_fps); }
    }
    /* Zwei Monitore: Hauptmonitor 60 Hz (bis 144), zweiter 165 Hz -> Liste nach dem Startmonitor, Hinweis */
    if (!why[0]) {
        char f2[300];
        mon_sim_apply("60/144,165");
        s1 = s0;
        s1.interp_fps = 60;
        s1.window_mode = 0;
        ui_from_settings(&s1);
        on_command(ID_IPFPS, CBN_SELCHANGE);
        _snprintf_s(f, sizeof f, _TRUNCATE, LT("gui.ip_st_max"), 60, 144);
        _snprintf_s(f2, sizeof f2, _TRUNCATE, LT("gui.ip_st_multi"), 60, 165);
        if (g_disp_hz != 60 || g_disp_hz_max != 144 || g_mon_alt_hz != 165 || ComboBox_GetCount(ctl(ID_IPFPS)) != 3 ||
            strncmp(g_status, f, strlen(f)) != 0 || !strstr(g_status, f2)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "2 Monitore: hz %d/%d alt %d, %d Eintraege, Status '%.90s'", g_disp_hz,
                        g_disp_hz_max, g_mon_alt_hz, ComboBox_GetCount(ctl(ID_IPFPS)), g_status);
        }
        mon_sim_apply("144,144"); /* gleiche Rate: kein Hinweis auf einen zweiten Monitor */
        if (!why[0] && g_mon_alt_hz != 0) { strcpy_s(why, sizeof why, "144,144: zweiter Monitor gemeldet"); }
        mon_sim_apply("143,144");
        if (!why[0] && g_mon_alt_hz != 0) { strcpy_s(why, sizeof why, "143,144: zweiter Monitor gemeldet"); }
        if (!why[0] && mon_sim_apply("abc") == 0) { strcpy_s(why, sizeof why, "Sim 'abc' angenommen"); }
    }
    g_disp_hz = hz0;
    g_disp_hz_max = mx0;
    g_mon_alt_hz = alt0;
    g_ip_keep = keep0;
    ComboBox_SetCurSel(ctl(ID_IPFPS), -1);
    ui_from_settings(&s0);
    ui_apply_rules();
    g_o.shot = shot0;
    strcpy_s(g_client_dir, sizeof g_client_dir, cd0);
    {
        char pth[1200];
        wchar_t wd[1100];
        ui_cfg_path(dir, pth, sizeof pth);
        u8_DeleteFile(pth);
        u8_to_w(dir, wd, 1100);
        RemoveDirectoryW(wd);
    }
    ui_to_settings(&s1);
    if (!why[0] && memcmp(&s0, &s1, sizeof s0) != 0) {
        strcpy_s(why, sizeof why, "Werte nach dem Test veraendert");
    }
    if (!why[0]) {
        GT_OK("72 W309 fps-monitor (Liste 60/144/165/240 Hz + 2 Monitore, Kennzeichen, Alt-cfg 240 bleibt, R10, Status)");
    } else {
        GT_FAIL("72 W309 fps-monitor", "%s", why);
    }
}

void porig_gui_selftest(int *pass, int *fail)
{
    static const int kSz[3] = { LLAY_SZ_COMPACT, LLAY_SZ_NORMAL, LLAY_SZ_LARGE };
    char why[300] = "", t[600], tmp[1024], dir[1100], cd0[sizeof g_client_dir];
    launcher_settings_t s0, s1;
    int adv0 = g_show_adv, shot0 = g_o.shot, conf0 = g_confirm_auto, id0 = g_lay.id, sz0 = g_lay.size, k, m;
    strcpy_s(cd0, sizeof cd0, g_client_dir);
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%sw296_uicfg_%lu", tmp, GetCurrentProcessId());
    lcfg_mkdir_p(dir);
    strcpy_s(g_client_dir, sizeof g_client_dir, dir); /* launcher_ui.cfg nur im Temp-Ordner */
    g_o.shot = 2; /* nichts speichern */
    g_confirm_auto = 1;
    memset(&s0, 0, sizeof s0);
    memset(&s1, 0, sizeof s1);
    ui_to_settings(&s0);
    /* Grafik: Namen und Wahl */
    show_page(P_GFX);
    on_command(ID_GP0 + LGP_PERFORMANCE, BN_CLICKED);
    pump();
    if (ComboBox_GetCurSel(ctl(ID_IPFPS)) != ip_sel_of(60) || !ui_checked(ID_GP0 + LGP_PERFORMANCE)) {
        strcpy_s(why, sizeof why, "Performance: 60 nicht gesetzt");
    }
    on_command(ID_GP0, BN_CLICKED);
    pump();
    if (!why[0] && (ComboBox_GetCurSel(ctl(ID_IPFPS)) != ip_sel_of(SB_FPS_AUTO) || !ui_checked(ID_GP0))) {
        _snprintf_s(why, sizeof why, _TRUNCATE, "Original: Bildrate Index %d statt auto", ComboBox_GetCurSel(ctl(ID_IPFPS)));
    }
    for (k = 0; k < 2 && !why[0]; k++) {
        const char *want = k ? LT("gui.gfx_p_performance") : LT("gui.gfx_p_orig");
        size_t n = strlen(want);
        ui_gettext(ctl(ID_GP0 + (k ? LGP_PERFORMANCE : LGP_ORIGINAL)), t, sizeof t);
        if (strncmp(t, want, n) != 0 || t[n] != '\n' || /* Name ohne Bildrate (der Text darf "60 FPS" nennen) */
            strcmp(want, k ? "Performance" : "Original") != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Optionsfeld %d: '%.60s'", k, t);
        }
    }
    /* Anzeige: Verlaufszeile folgt "Fade out" ohne Erweitert */
    g_show_adv = 0;
    lay_gfx_apply();
    for (k = 0; k < 3 && !why[0]; k++) {
        char tag[48];
        lay_switch(LLAY_A, kSz[k]);
        for (m = 0; m < 3 && !why[0]; m++) { /* Fade out ohne / 4:3 ohne / 4:3 mit Erweitert */
            const int fade = m == 0, adv = m == 2, want = fade || adv;
            g_show_adv = adv;
            lay_gfx_apply();
            show_page(P_DISPLAY);
            ComboBox_SetCurSel(ctl(ID_EDGE), fade ? 2 : 1);
            on_command(ID_EDGE, CBN_SELCHANGE);
            pump();
            _snprintf_s(tag, sizeof tag, _TRUNCATE, "a/%s %s%s", llay_size_name(kSz[k]), fade ? "fade" : "4:3",
                        adv ? "+erweitert" : "");
            if ((IsWindowVisible(ctl(ID_EDGE_W)) != 0) != want || (IsWindowVisible(ctl(ID_EDGE_S)) != 0) != want) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "%s: Verlauf sichtbar=%d/%d", tag,
                            IsWindowVisible(ctl(ID_EDGE_W)) != 0, IsWindowVisible(ctl(ID_EDGE_S)) != 0);
                break;
            }
            if (fade && (!IsWindowEnabled(ctl(ID_EDGE_W)) || !IsWindowEnabled(ctl(ID_EDGE_S)))) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "%s: Verlauf gesperrt", tag);
                break;
            }
            if (want && !adv && disp_preview_y() < 136 + 30 + 16 + 8) { /* Zeile 136..166, Regelzeile 168..184 */
                _snprintf_s(why, sizeof why, _TRUNCATE, "%s: Vorschau %d ueber der Verlaufszeile", tag, disp_preview_y());
                break;
            }
            if (!want && disp_preview_y() != 150) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "%s: Vorschau %d statt 150", tag, disp_preview_y());
                break;
            }
            if (m != 1) { /* Ueberlappung/innen je Seite (4:3 ohne Erweitert pruefen Fall 67/69) */
                lay_gt_page_check(why, sizeof why, tag);
            }
        }
    }
    /* zurueck: Werte, Erweitert, Layout */
    g_show_adv = adv0;
    lay_gfx_apply();
    lay_switch(id0, sz0);
    ui_from_settings(&s0);
    gfx_sync_ui();
    ui_to_settings(&s1);
    if (!why[0] && memcmp(&s0, &s1, sizeof s0) != 0) {
        strcpy_s(why, sizeof why, "Werte nach dem Test veraendert");
    }
    {
        char pth[1200];
        wchar_t wd[1100];
        ui_cfg_path(dir, pth, sizeof pth);
        u8_DeleteFile(pth);
        u8_to_w(dir, wd, 1100);
        RemoveDirectoryW(wd);
    }
    strcpy_s(g_client_dir, sizeof g_client_dir, cd0);
    g_o.shot = shot0;
    g_confirm_auto = conf0;
    show_page(P_PLAY);
    if (!why[0]) {
        GT_OK("71 W296 PRESET-ORIG (Original ohne FPS = Monitor auto, Performance 60, Verlauf bei Fade out ohne Erweitert "
              "sichtbar, bei 4:3 nicht, Vorschau darunter, keine Ueberlappung a/kompakt/normal/gross)");
    } else {
        GT_FAIL("71 W296 PRESET-ORIG", "%s", why);
    }
}

/* --- 70 (W293 LOC-LAYOUT): LoC-Spielen wie SB - Bereitschaft (Spiel,
 * Grafik, Anzeige, Steuerung, Online spaeter) mit "Change", Hinweisband
 * unten (ohne Einmal-Start), Erweitert bei LoC erreichbar mit den LoC-Karten
 * (Disc, Datenordner) und den gemeinsamen Teilen, ohne SB-Teile; Start grau;
 * keine Ueberlappung in Layout A (alle Groessen). user.cfg = Temp-Datei. --- */
static void locl_gt_row(const loc_sumrow_t *r, int i, const char *label, const char *val_part, COLORREF col,
                        const char *tag, char *why, size_t cap)
{
    if (why[0]) {
        return;
    }
    if (!r[i].label || !label || strcmp(r[i].label, label) != 0 || !r[i].value[0] || !strstr(r[i].value, val_part) ||
        r[i].col != col) {
        _snprintf_s(why, cap, _TRUNCATE, "%s Zeile %d: '%s' '%s' Farbe %06lX", tag, i, r[i].label ? r[i].label : "-",
                    r[i].value, (unsigned long)r[i].col);
    }
}

void locl_gui_selftest(int *pass, int *fail)
{
    static const int kAdvLoc[] = { ID_LOC_IMAGE, ID_LOC_CHECK, ID_LOC_BROWSE, ID_LOC_FOLDER, ID_VSYNC, ID_FPS }; /* W397: ID_BG unter dem Zahnrad */
    static const int kAdvSb[] = { ID_MEDIA, ID_MEDIA_B, ID_XBE, ID_HDD, ID_HDD_OPEN, ID_DATA_OPEN, ID_SAVES_RESET,
                                  ID_SAVES_IMPORT, ID_SAVES_EXPORT, ID_SAVES_REPLAYS, ID_PROF_COMBO, ID_PROF_LOAD,
                                  ID_PROF_SAVE, ID_PROF_DEL, ID_PROF_DFLT, ID_DIAG, ID_SAVE_LOCK, ID_LIVE };
    static const int kGo[LOC_SUM_N] = { P_ADV, P_GFX, P_DISPLAY, P_CTRL, P_LNET }; /* W397: Online -> Multiplayer */
    char why[300] = "", tmp[1024], cfg[1200], keep[16], dir[1100], cd0[sizeof g_client_dir];
    launcher_settings_t s;
    loc_sumrow_t r[LOC_SUM_N];
    int k, li, id0 = g_lay.id, sz0 = g_lay.size, shot0 = g_o.shot;
    int af0 = g_acc_fixture, as0 = g_acc_fix_srv;
    g_acc_fixture = 1; /* W328: Online-Zeile wie vor dem Konto (Dienst gilt als nicht eingerichtet, auch mit SB_STEELNET_API) */
    g_acc_fix_srv = 0;
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw293_gui_user.cfg", tmp);
    {
        FILE *f = NULL;
        if (u8_fopen_s(&f, cfg, "w") == 0 && f) { /* experimentell: Breitbild + Cockpit aus */
            fputs("[render]\nedge_mode=2\ncockpit_hide=1\n# fremde Zeile\n", f);
            fclose(f);
        }
    }
    strcpy_s(cd0, sizeof cd0, g_client_dir);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%sw293_uicfg_%lu", tmp, GetCurrentProcessId());
    lcfg_mkdir_p(dir);
    strcpy_s(g_client_dir, sizeof g_client_dir, dir); /* launcher_ui.cfg nur im Temp-Ordner */
    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, cfg);
    strcpy_s(keep, sizeof keep, sb_lang_code());
    sb_lang_set("en");
    ui_apply_language();
    g_o.shot = 2; /* Layoutwechsel ohne Speichern */
    lay_switch(LLAY_A, LLAY_SZ_NORMAL);
    lcfg_defaults(&s);
    lcfg_load_file(cfg, &s);
    g_s = s;
    ui_from_settings(&s);
    g_game = LGAME_LOC;
    g_loc_checked = 0;
    g_locfix_path[0] = '\0';
    ui_settext(ctl(ID_LOC_IMAGE), "");
    loctab_nav_layout();
    show_page(P_PLAY);
    ui_apply_rules();
    pump();
    /* 1: Bereitschaft (ohne Abbild) + "Change" je Zeile; SB-Teile weg */
    loctab_sum_rows(r);
    locl_gt_row(r, 0, LT("gui.sum_game"), LT("gui.loc_sum_none"), C_WARN, "en", why, sizeof why);
    locl_gt_row(r, 1, g_sum[1].label, g_sum[1].value, g_sum[1].col, "en", why, sizeof why);
    { /* W363: Anzeige-Zeile mit LoC-Takt, VSync und Bildformat */
        char dv[sizeof r[2].value];
        launcher_settings_t t = g_s;
        loctab_display_value(&g_s, dv, sizeof dv);
        locl_gt_row(r, 2, g_sum[2].label, dv, g_sum[2].col, "en", why, sizeof why);
        t.interp_fps = 0; t.present_interval = 1; t.window_mode = 0; t.width = 960; t.height = 720;
        loctab_display_value(&t, dv, sizeof dv);
        if (!why[0] && (!strstr(dv, "960 \xC3\x97 720") || !strstr(dv, "20 Hz") || !strstr(dv, "VSync on") ||
                        !strstr(dv, LT(lgame_wide_label_key(loctab_wide_val()))))) { /* W414: Standard 16:9 */
            _snprintf_s(why, sizeof why, _TRUNCATE, "W363 Anzeige LoC: %s", dv);
        }
        t.interp_fps = 120; t.present_interval = 0;
        loctab_display_value(&t, dv, sizeof dv);
        if (!why[0] && (!strstr(dv, "120") || !strstr(dv, "VSync off"))) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "W363 Anzeige LoC 120: %s", dv);
        }
    }
    locl_gt_row(r, 3, g_sum[4].label, g_sum[4].value, g_sum[4].col, "en", why, sizeof why);
    locl_gt_row(r, 4, LT("gui.loc_sum_online"), LT("gui.loc_server_cue"), C_FAINT, "en", why, sizeof why);
    if (!why[0] && (strcmp(r[1].label, LT("gui.page_graphics")) != 0 || strcmp(r[3].label, LT("gui.page_controls")) != 0)) {
        strcpy_s(why, sizeof why, "Zeilen Grafik/Steuerung falsch belegt");
    }
    for (k = 0; k < LOC_SUM_N; k++) {
        loctab_vis(ID_LOC_GO0 + k, 1, "LoC-Bereitschaft Change", why, sizeof why);
        loctab_vis(ID_SUM_GO0 + k, 0, "SB-Bereitschaft im LoC-Bereich", why, sizeof why);
    }
    loctab_vis(ID_LOC_START, 1, "LoC-Start", why, sizeof why);
    if (!why[0] && !IsWindowEnabled(ctl(ID_LOC_START)) != !(lgame_loc_client_ready() && !loc_img_gate(NULL, 0))) {
        /* W353: je Ausgabe; W403: ohne gepruefte Disc grau */
        strcpy_s(why, sizeof why, "LoC-Start grau/frei passt nicht zur Ausgabe");
    }
    /* 2: Hinweisband unten wie SB (nach der Liste), ohne Einmal-Start */
    if (!why[0] && (!g_orig_mask || !orig_banner_on())) {
        _snprintf_s(why, sizeof why, _TRUNCATE, "kein Hinweis (Maske 0x%X)", g_orig_mask);
    }
    loctab_vis(ID_ORIG_RESET_LOC, 1, "Back to original (LoC)", why, sizeof why);
    loctab_vis(ID_XWARN_LOC, 1, "Warn-Symbol (LoC)", why, sizeof why);
    loctab_vis(ID_ORIG_ONCE, 0, "Einmal-Start im LoC-Bereich", why, sizeof why);
    loctab_vis(ID_ORIG_RESET, 0, "Back to original (SB) im LoC-Bereich", why, sizeof why);
    if (!why[0]) {
        uictl_t *b = ui_find(ID_ORIG_RESET_LOC), *wi = ui_find(ID_XWARN_LOC), *g = ui_find(ID_LOC_GO0 + LOC_SUM_N - 1);
        if (!b || !wi || !g || b->y < LOC_SUM_Y + LOC_SUM_H || wi->y < LOC_SUM_Y + LOC_SUM_H ||
            b->y + b->h_ > OB_Y_LOC + OB_H_LOC || g->y + g->h_ > LOC_SUM_Y + LOC_SUM_H ||
            b->x + b->w != CX0 + CW - 12 || wi->x != CX0 + 10) {
            strcpy_s(why, sizeof why, "Band nicht unten unter der Liste");
        }
    }
    if (!why[0]) { /* Tooltip/Name des Warn-Symbols wie SB: eine Gruppe je Zeile */
        char tt[700];
        ui_gettext(ctl(ID_XWARN_LOC), tt, sizeof tt);
        if (!strstr(tt, "\xE2\x80\xA2")) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Warn-Tooltip LoC '%.120s'", tt);
        }
    }
    /* 3: "Change" fuehrt zur Seite der Ursache */
    for (k = 0; k < LOC_SUM_N && !why[0]; k++) {
        show_page(P_PLAY);
        on_command(ID_LOC_GO0 + k, BN_CLICKED);
        pump();
        if (g_page != kGo[k] || g_game != LGAME_LOC) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Change %d -> Seite %d (soll %d)", k, g_page, kGo[k]);
        }
    }
    /* 4: Erweitert bei LoC: LoC-Karten + gemeinsame Teile, keine SB-Teile */
    if (!why[0]) {
        show_page(P_ADV);
        pump();
        if (g_page != P_ADV) {
            strcpy_s(why, sizeof why, "Erweitert bei LoC nicht erreichbar");
        }
    }
    loctab_vis(ID_NAV0 + P_ADV, 1, "LoC", why, sizeof why);
    loctab_vis(ID_NAV0 + P_PACKS, 1, "LoC (Netzwerk)", why, sizeof why); /* W392 */
    for (k = 0; k < (int)(sizeof kAdvLoc / sizeof kAdvLoc[0]); k++) {
        loctab_vis(kAdvLoc[k], 1, "LoC-Erweitert", why, sizeof why);
    }
    for (k = 0; k < (int)(sizeof kAdvSb / sizeof kAdvSb[0]); k++) {
        if (ctl(kAdvSb[k])) {
            loctab_vis(kAdvSb[k], 0, "LoC-Erweitert (SB-Teil)", why, sizeof why);
        }
    }
    loctab_vis(ID_LOC_START, 0, "LoC-Erweitert (Start)", why, sizeof why);
    /* 5: Disc erkannt (Fixture) -> Spiel-Zeile gruen mit Title-ID, EN/DE */
    if (!why[0]) {
        strcpy_s(g_locfix_path, sizeof g_locfix_path, LOC_ISO);
        memset(&g_locfix, 0, sizeof g_locfix);
        g_locfix.kind = LGAME_IMG_LOC;
        g_locfix.title_id = LGAME_TID_LOC;
        ui_settext(ctl(ID_LOC_IMAGE), LOC_ISO);
        on_command(ID_LOC_CHECK, BN_CLICKED);
        for (li = 0; li < 2 && !why[0]; li++) {
            sb_lang_set(li ? "de" : "en");
            ui_apply_language();
            show_page(P_PLAY);
            pump();
            loctab_sum_rows(r);
            locl_gt_row(r, 0, LT("gui.sum_game"), "43430009", C_OK, li ? "de" : "en", why, sizeof why);
            locl_gt_row(r, 4, LT("gui.loc_sum_online"), LT("gui.loc_server_cue"), C_FAINT, li ? "de" : "en", why,
                        sizeof why);
            if (!why[0]) {
                char tb[64];
                ui_gettext(ctl(ID_LOC_GO0), tb, sizeof tb);
                if (strcmp(tb, LT("gui.play_change")) != 0) {
                    _snprintf_s(why, sizeof why, _TRUNCATE, "%s Change-Text '%s'", li ? "de" : "en", tb);
                }
            }
        }
        sb_lang_set("en");
        ui_apply_language();
        ui_settext(ctl(ID_LOC_IMAGE), "D:\\Games\\anders.iso"); /* Ergebnis gilt nur fuer den geprueften Pfad */
        loctab_sum_rows(r);
        locl_gt_row(r, 0, LT("gui.sum_game"), LT("gui.loc_sum_unchecked"), C_WARN, "neu", why, sizeof why);
        g_locfix.kind = LGAME_IMG_SB; /* SB-Abbild: Hinweis */
        g_locfix.title_id = LGAME_TID_SB;
        ui_settext(ctl(ID_LOC_IMAGE), LOC_ISO);
        on_command(ID_LOC_CHECK, BN_CLICKED);
        loctab_sum_rows(r);
        locl_gt_row(r, 0, LT("gui.sum_game"), LT("gui.loc_sum_sb"), C_WARN, "sb", why, sizeof why);
        g_locfix.kind = LGAME_IMG_UNREADABLE; /* Fehler */
        on_command(ID_LOC_CHECK, BN_CLICKED);
        loctab_sum_rows(r);
        locl_gt_row(r, 0, LT("gui.sum_game"), LT("gui.loc_sum_err"), C_ERR, "kaputt", why, sizeof why);
    }
    /* 6: keine Ueberlappung, alles im Fenster: Layout A in allen Groessen (mit Band) */
    for (k = LLAY_SZ_COMPACT; k <= LLAY_SZ_LARGE && !why[0]; k++) {
        char tag[40];
        lay_switch(LLAY_A, k);
        _snprintf_s(tag, sizeof tag, _TRUNCATE, "LoC a/%s", llay_size_name(k));
        lay_gt_page_check(why, sizeof why, tag);
    }
    /* 7: Original -> kein Band, Liste bleibt */
    if (!why[0]) {
        lcfg_defaults(&s);
        lcfg_gfx_apply(&s, LGP_ORIGINAL); /* W471: LoC-Original = mit den Schatten des Spiels */
        g_s = s;
        ui_from_settings(&s);
        show_page(P_PLAY);
        ui_apply_rules();
        pump();
        loctab_vis(ID_ORIG_RESET_LOC, 0, "Back to original (LoC) bei Original", why, sizeof why);
        loctab_vis(ID_XWARN_LOC, 0, "Warn-Symbol (LoC) bei Original", why, sizeof why);
        loctab_vis(ID_LOC_GO0, 1, "LoC-Bereitschaft bei Original", why, sizeof why);
    }
    /* aufraeumen (die Wahl LoC wurde nur in die Temp-Datei geschrieben) */
    lay_switch(id0, sz0);
    g_o.shot = shot0;
    g_game = LGAME_SB;
    loctab_nav_layout();
    g_locfix_path[0] = '\0';
    g_loc_checked = 0;
    lgame_defaults(&g_loc);
    ui_settext(ctl(ID_LOC_IMAGE), "");
    u8_DeleteFile(cfg);
    g_ucfg_test[0] = '\0';
    {
        char pth[1200];
        wchar_t wd[1100];
        ui_cfg_path(dir, pth, sizeof pth);
        u8_DeleteFile(pth);
        u8_to_w(dir, wd, 1100);
        RemoveDirectoryW(wd);
    }
    strcpy_s(g_client_dir, sizeof g_client_dir, cd0);
    sb_lang_set(keep);
    ui_apply_language();
    lcfg_defaults(&s);
    g_s = s;
    ui_from_settings(&s);
    ui_apply_rules();
    show_page(P_PLAY);
    g_acc_fixture = af0;
    g_acc_fix_srv = as0;
    if (!why[0]) {
        GT_OK("70 W293 LOC-LAYOUT (LoC-Bereitschaft Spiel/Grafik/Anzeige/Steuerung/Online mit Change, Band unten ohne "
              "Einmal-Start, Erweitert bei LoC mit Disc/Ordner + gemeinsamen Teilen ohne SB-Teile, Start grau, EN/DE, "
              "keine Ueberlappung in Layout A Kompakt/Normal/Gross)");
    } else {
        GT_FAIL("70 W293 LOC-LAYOUT", "%s", why);
    }
}


/* --- 77e (W470 LAUNCHER-HUB): Bereiche Spiel / Optionen / steelnet. Spiel-Navigation nur im Spielbereich, eigene
 * Reiter je Bereich, Rueckweg ueber die Spiel-Reiter (zuletzt offene Spielseite, auch mit Spielwechsel), Fussleiste
 * bleibt, steelnet > Info ohne Stufe steelnet mit Weg in die Einstellungen, Lobbyliste nur sichtbar + hoechstens alle
 * 12 s (Trockenlauf, nie Netz), Antwort-Parser (0/1/6 Lobbys wie der devserver, kaputte Eintraege), Seite ohne
 * Ueberlappung mit 0/1/6 Lobbys in Layout A Normal/Gross, EN/DE. */
static int hub_gt_vis(int id)
{
    HWND h = ctl(id);
    return h && IsWindowVisible(h);
}

static void hub_gt_area(int area, const char *tag, char *why, size_t cap)
{
    const int *o = nav_order();
    int k, gnav = 0, opt = hub_gt_vis(ID_HUBNAV0), net = hub_gt_vis(ID_HUBNAV0 + 1);
    if (why[0]) return;
    for (k = 0; k < NAV_ORDER_N; k++) {
        gnav += hub_gt_vis(ID_NAV0 + o[k]);
    }
    if (area == HUB_GAME && (gnav != NAV_ORDER_N || opt || net)) {
        _snprintf_s(why, cap, _TRUNCATE, "%s: Spielbereich Navigation %d/%d, Reiter %d/%d", tag, gnav, NAV_ORDER_N, opt, net);
    } else if (area != HUB_GAME && (gnav || hub_gt_vis(ID_NAVSUB_MATCH) || hub_gt_vis(ID_NAVSUB_ACC) ||
                                    opt != (area == HUB_OPTS) || net != (area == HUB_NET))) {
        _snprintf_s(why, cap, _TRUNCATE, "%s: Bereich %d mit Spiel-Navigation %d / Reiter %d/%d", tag, area, gnav, opt, net);
    } else if (area != HUB_GAME && (!hub_gt_vis(ID_START) || !hub_gt_vis(ID_SAVE) || !hub_gt_vis(ID_GAME_SB) ||
                                    !hub_gt_vis(ID_GAME_LOC))) {
        _snprintf_s(why, cap, _TRUNCATE, "%s: Fussleiste/Spiel-Reiter fehlen", tag);
    }
}

__declspec(noinline) void hub_gui_selftest(int *pass, int *fail)
{
    char why[300] = "", t[200];
    char keep[8];
    int g0 = g_game, p0 = g_page, on0 = g_loc_online, uc0 = g_upd_cfg, fs0 = g_onl_fix_srv, fx0 = g_onl_fixture;
    int dry0 = g_hubl_dry, shot0 = g_o.shot, id0 = g_lay.id, sz0 = g_lay.size, cid0 = g_lay_cfg_id,
        csz0 = g_lay_cfg_size, k;
    lacc_lobbies_t lb;
    lacc_store_t acc0 = g_acc; /* W472: Konto im Speicher (Testwerte, nie die Datei) */
    int av0 = g_acc_view;
    strcpy_s(keep, sizeof keep, sb_lang_code());
    g_hubl_dry = 1;
    g_hubl_dry_n = 0;
    g_o.shot = 2; /* nichts speichern (Layout) */
    g_onl_fix_srv = 1; /* Dienst eingerichtet (Attrappe, keine Datei) */
    g_onl_fixture = 1;
    g_loc_online = 0;
    g_upd_cfg = 0;
    g_game = LGAME_SB;
    loctab_nav_layout();
    /* 1: Bereiche und Navigation */
    show_page(P_DISPLAY);
    pump();
    hub_gt_area(HUB_GAME, "Anzeige", why, sizeof why);
    if (!why[0]) {
        on_command(ID_LSET_OPEN, BN_CLICKED);
        pump();
        if (g_page != P_LSET || !ui_checked(ID_LSET_OPEN) || !hub_gt_vis(ID_NETM0) || !hub_gt_vis(ID_LANG))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Zahnrad -> Seite %d (Inhalt General fehlt)", g_page);
        hub_gt_area(HUB_OPTS, "Optionen", why, sizeof why);
    }
    if (!why[0]) {
        hdr_click(HDR_CHIP_ST);
        pump();
        if (g_page != P_NETI || ui_checked(ID_LSET_OPEN))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Chip -> Seite %d", g_page);
        hub_gt_area(HUB_NET, "steelnet", why, sizeof why);
    }
    if (!why[0]) { /* Reiter des Bereichs (Tabelle) */
        on_command(ID_HUBNAV0 + 1, BN_CLICKED);
        if (g_page != P_NETI || hub_tab_page(ID_HUBNAV0) != P_LSET || hub_tab_page(ID_HUBNAV0 + 1) != P_NETI ||
            hub_tab_page(ID_NAV0) != -1 || hub_area_of(P_LSET) != HUB_OPTS || hub_area_of(P_NETI) != HUB_NET ||
            hub_area_of(P_ADV) != HUB_GAME)
            strcpy_s(why, sizeof why, "Reiter-Tabelle");
    }
    /* 2: Rueckweg ueber die Spiel-Reiter: gleiches Spiel -> zuletzt offene Spielseite */
    if (!why[0]) {
        on_command(ID_GAME_SB, BN_CLICKED);
        pump();
        if (g_page != P_DISPLAY || g_game != LGAME_SB)
            _snprintf_s(why, sizeof why, _TRUNCATE, "Rueckweg SB -> Seite %d Spiel %d", g_page, g_game);
        hub_gt_area(HUB_GAME, "Rueckweg", why, sizeof why);
    }
    /* anderes Spiel aus den Optionen: Spielwechsel + Spielbereich; Launch bleibt beim gewaehlten Spiel */
    if (!why[0]) {
        show_page(P_LSET);
        on_command(ID_GAME_LOC, BN_CLICKED);
        pump();
        if (g_page != P_DISPLAY || g_game != LGAME_LOC)
            _snprintf_s(why, sizeof why, _TRUNCATE, "Rueckweg LoC -> Seite %d Spiel %d", g_page, g_game);
        hub_gt_area(HUB_GAME, "Rueckweg LoC", why, sizeof why);
        if (!why[0]) {
            show_page(P_NETI);
            if (g_game != LGAME_LOC) strcpy_s(why, sizeof why, "steelnet wechselt das Spiel");
            hub_gt_area(HUB_NET, "steelnet LoC", why, sizeof why);
        }
        on_command(ID_GAME_SB, BN_CLICKED);
        pump();
        if (!why[0] && (g_game != LGAME_SB || g_page != P_DISPLAY))
            strcpy_s(why, sizeof why, "zurueck zu SB");
    }
    /* 3: steelnet ohne Stufe steelnet: Hinweis + Weg in die Einstellungen, kein Abruf */
    if (!why[0]) {
        show_page(P_NETI);
        pump();
        if (!hub_gt_vis(ID_HUB_GOOPT) || g_hubl_dry_n != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "offline: Knopf %d Abrufe %d", hub_gt_vis(ID_HUB_GOOPT), g_hubl_dry_n);
        on_command(ID_HUB_GOOPT, BN_CLICKED);
        if (!why[0] && g_page != P_LSET) strcpy_s(why, sizeof why, "Open settings -> nicht Optionen");
    }
    /* 4a (W472 LOBBIES-AUTH): Stufe steelnet ohne Konto: Hinweis + Knopf zur Konto-Seite, kein Abruf */
    if (!why[0]) {
        memset(&g_acc, 0, sizeof g_acc);
        g_loc_online = 1;
        g_upd_cfg = 1;
        hub_net_changed();
        show_page(P_NETI);
        pump();
        if (g_hubl_state != -4 || g_hubl_dry_n != 0 || !hub_gt_vis(ID_HUB_GOACC) || hub_gt_vis(ID_HUB_GOOPT))
            _snprintf_s(why, sizeof why, _TRUNCATE, "ohne Konto: Zustand %d Abrufe %d Knopf %d/%d", g_hubl_state,
                        g_hubl_dry_n, hub_gt_vis(ID_HUB_GOACC), hub_gt_vis(ID_HUB_GOOPT));
        g_hubl_t0 -= 60000;
        hub_tick();
        if (!why[0] && g_hubl_dry_n != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "ohne Konto abgerufen: %d", g_hubl_dry_n);
        on_command(ID_HUB_GOACC, BN_CLICKED);
        pump();
        if (!why[0] && (g_game != LGAME_LOC || g_page != P_LNET || !g_acc_view))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Open account -> Spiel %d Seite %d Konto %d", g_game, g_page, g_acc_view);
        acc_nav_sub(0);
        on_command(ID_GAME_SB, BN_CLICKED);
        pump();
        if (!why[0] && g_game != LGAME_SB) strcpy_s(why, sizeof why, "zurueck zu SB (Konto)");
        show_page(P_LSET);
    }
    /* 4: Stufe steelnet mit Konto: Abruf nur sichtbar, hoechstens alle 12 s */
    if (!why[0]) {
        g_acc.have = 1; /* Testkonto (erfundene Werte, nur im Speicher; Trockenlauf ohne Netz) */
        g_acc.account = 4242;
        strcpy_s(g_acc.name, sizeof g_acc.name, "Tester");
        memset(g_acc.key, 0x5A, sizeof g_acc.key);
        g_loc_online = 1;
        g_upd_cfg = 1;
        hub_net_changed();
        show_page(P_NETI);
        pump();
        if (hub_gt_vis(ID_HUB_GOOPT) || g_hubl_dry_n != 1)
            _snprintf_s(why, sizeof why, _TRUNCATE, "steelnet: Knopf %d Abrufe %d (1)", hub_gt_vis(ID_HUB_GOOPT), g_hubl_dry_n);
        show_page(P_NETI);
        hub_tick();
        if (!why[0] && g_hubl_dry_n != 1) _snprintf_s(why, sizeof why, _TRUNCATE, "vor 12 s erneut: %d", g_hubl_dry_n);
        g_hubl_t0 -= 11000;
        hub_tick();
        if (!why[0] && g_hubl_dry_n != 1) _snprintf_s(why, sizeof why, _TRUNCATE, "nach 11 s erneut: %d", g_hubl_dry_n);
        g_hubl_t0 -= 1000;
        hub_tick();
        if (!why[0] && g_hubl_dry_n != 2) _snprintf_s(why, sizeof why, _TRUNCATE, "nach 12 s: %d", g_hubl_dry_n);
        show_page(P_LSET);
        g_hubl_t0 -= 30000;
        hub_tick();
        hub_lobby_kick();
        if (!why[0] && g_hubl_dry_n != 2) _snprintf_s(why, sizeof why, _TRUNCATE, "unsichtbar abgerufen: %d", g_hubl_dry_n);
        if (!why[0] && (hub_lobby_due(11999) || !hub_lobby_due(12000))) strcpy_s(why, sizeof why, "hub_lobby_due");
        if (!why[0] && (g_hubl_state != -2 || hub_gt_vis(ID_HUB_GOACC)))
            _snprintf_s(why, sizeof why, _TRUNCATE, "mit Konto: Zustand %d Knopf %d", g_hubl_state, hub_gt_vis(ID_HUB_GOACC));
    }
    /* 4b (W472): Fehler ruhig behandeln - auth_failed stoppt bis zum naechsten Oeffnen, rate_limited wartet
     * retry_after (mind. 60 s, hoechstens 300 s), andere Fehler mit wachsendem Abstand, Erfolg wieder alle 12 s */
    if (!why[0]) {
        int n0;
        show_page(P_NETI);
        pump();
        g_hubl_t0 = GetTickCount64();
        n0 = g_hubl_dry_n;
        memset(&lb, 0, sizeof lb);
        strcpy_s(lb.error, sizeof lb.error, "auth_failed");
        lb.http = 401;
        hub_lobby_result(&lb);
        pump();
        if (g_hubl_state != -5 || !hub_gt_vis(ID_HUB_GOACC))
            _snprintf_s(why, sizeof why, _TRUNCATE, "auth_failed: Zustand %d Knopf %d", g_hubl_state, hub_gt_vis(ID_HUB_GOACC));
        g_hubl_t0 -= 600000;
        hub_tick();
        if (!why[0] && g_hubl_dry_n != n0) _snprintf_s(why, sizeof why, _TRUNCATE, "auth_failed wiederholt: %d", g_hubl_dry_n - n0);
        show_page(P_LSET);
        show_page(P_NETI);
        pump();
        if (!why[0] && (g_hubl_dry_n != n0 + 1 || g_hubl_state != -2 || hub_gt_vis(ID_HUB_GOACC)))
            _snprintf_s(why, sizeof why, _TRUNCATE, "neu geoeffnet nach auth_failed: Abrufe %d Zustand %d", g_hubl_dry_n - n0,
                        g_hubl_state);
        memset(&lb, 0, sizeof lb);
        strcpy_s(lb.error, sizeof lb.error, "rate_limited");
        lb.http = 429;
        lb.retry_after = 30;
        hub_lobby_result(&lb);
        pump();
        if (!why[0] && (g_hubl_state != -6 || g_hubl_wait != 60000 || hub_gt_vis(ID_HUB_GOACC)))
            _snprintf_s(why, sizeof why, _TRUNCATE, "rate_limited: Zustand %d Abstand %u", g_hubl_state, (unsigned)g_hubl_wait);
        g_hubl_t0 = GetTickCount64() - 59000;
        hub_tick();
        if (!why[0] && g_hubl_dry_n != n0 + 1) strcpy_s(why, sizeof why, "rate_limited: vor 60 s erneut");
        g_hubl_t0 -= 1500;
        hub_tick();
        if (!why[0] && g_hubl_dry_n != n0 + 2) strcpy_s(why, sizeof why, "rate_limited: nach 60 s kein Abruf");
        lb.retry_after = 900;
        hub_lobby_result(&lb);
        if (!why[0] && g_hubl_wait != 300000) _snprintf_s(why, sizeof why, _TRUNCATE, "retry_after 900: %u", (unsigned)g_hubl_wait);
        memset(&lb, 0, sizeof lb);
        strcpy_s(lb.error, sizeof lb.error, "net");
        for (k = 0; k < 6 && !why[0]; k++) {
            static const unsigned kWait[6] = { 12000, 24000, 48000, 96000, 120000, 120000 };
            hub_lobby_result(&lb);
            if (g_hubl_state != 0 || g_hubl_wait != kWait[k])
                _snprintf_s(why, sizeof why, _TRUNCATE, "Fehler %d: Zustand %d Abstand %u", k, g_hubl_state, (unsigned)g_hubl_wait);
        }
        memset(&lb, 0, sizeof lb);
        lb.ok = 1;
        lb.n = 1;
        strcpy_s(lb.l[0].host, sizeof lb.l[0].host, "Guest-One");
        lb.l[0].players = 1;
        lb.l[0].max_players = 4;
        hub_lobby_result(&lb);
        if (!why[0] && (g_hubl_state != 1 || g_hubl_wait != 12000 || g_hubl.n != 1))
            _snprintf_s(why, sizeof why, _TRUNCATE, "ok nach Fehlern: Zustand %d Abstand %u n %d", g_hubl_state,
                        (unsigned)g_hubl_wait, g_hubl.n);
        memset(&lb, 0, sizeof lb);
        strcpy_s(lb.error, sizeof lb.error, "no_account");
        hub_lobby_result(&lb);
        if (!why[0] && (g_hubl_state != -4 || g_hubl.n != 0))
            _snprintf_s(why, sizeof why, _TRUNCATE, "no_account: Zustand %d n %d", g_hubl_state, g_hubl.n);
        show_page(P_LSET);
    }
    /* 5: Parser (Antworten wie der devserver: 0, 1, 6 Lobbys; kaputte Eintraege fallen weg) */
    if (!why[0]) {
        static const char *const kBody[4] = {
            "{\"ok\":true,\"lobbies\":[],\"more\":0}",
            "{\"ok\":true,\"lobbies\":[{\"host\":\"Guest-One\",\"game\":\"loc\",\"play_mode\":\"arcade\",\"players\":1,"
            "\"max_players\":4,\"state\":\"open\"}],\"more\":0}",
            "{\"ok\":true,\"lobbies\":[{\"host\":\"A\",\"play_mode\":\"sim\",\"players\":1,\"max_players\":2,\"state\":\"open\"},"
            "{\"host\":\"B\",\"play_mode\":\"arcade\",\"players\":2,\"max_players\":4,\"state\":\"open\"},"
            "{\"host\":\"C\",\"play_mode\":\"sim\",\"players\":3,\"max_players\":6,\"state\":\"open\"},"
            "{\"host\":\"D\",\"play_mode\":\"sim\",\"players\":1,\"max_players\":8,\"state\":\"open\"},"
            "{\"host\":\"E\",\"play_mode\":\"arcade\",\"players\":2,\"max_players\":10,\"state\":\"open\"},"
            "{\"host\":\"F\",\"play_mode\":\"sim\",\"players\":4,\"max_players\":4,\"state\":\"started\"}],\"more\":0}",
            "{\"ok\":true,\"lobbies\":[{\"host\":\"X\\u0007Y\",\"play_mode\":\"sim\",\"players\":1,\"max_players\":2,\"state\":\"open\"},"
            "{\"host\":\"\",\"players\":1,\"max_players\":2},{\"host\":\"Z\",\"players\":5,\"max_players\":2},"
            "{\"host\":\"W\",\"players\":1,\"max_players\":99}],\"more\":3}" };
        static const int kN[4] = { 0, 1, 6, 1 }, kMore[4] = { 0, 0, 0, 3 };
        for (k = 0; k < 4 && !why[0]; k++) {
            memset(&lb, 0, sizeof lb);
            if (lacc_lobbies_parse(kBody[k], &lb) != 0 || lb.n != kN[k] || lb.more != kMore[k])
                _snprintf_s(why, sizeof why, _TRUNCATE, "Parser %d: n %d more %d", k, lb.n, lb.more);
        }
        if (!why[0]) {
            memset(&lb, 0, sizeof lb);
            lacc_lobbies_parse(kBody[1], &lb);
            if (strcmp(lb.l[0].host, "Guest-One") || !lb.l[0].arcade || lb.l[0].players != 1 || lb.l[0].max_players != 4 ||
                lb.l[0].started)
                strcpy_s(why, sizeof why, "Parser: Felder");
            lacc_lobbies_parse(kBody[2], &lb);
            if (!why[0] && (!lb.l[5].started || lb.l[5].arcade || !lb.l[1].arcade))
                strcpy_s(why, sizeof why, "Parser: Zustand/Modus");
            lacc_lobbies_parse(kBody[3], &lb);
            if (!why[0] && strcmp(lb.l[0].host, "X?Y") != 0) strcpy_s(why, sizeof why, "Parser: Steuerzeichen im Namen");
            if (!why[0] && (lacc_lobbies_parse("{\"ok\":true}", &lb) == 0 ||
                            lacc_lobbies_parse("{\"lobbies\":[{\"host\":\"A\"", &lb) == 0))
                strcpy_s(why, sizeof why, "Parser: kaputte Antwort angenommen");
        }
        if (!why[0]) { /* W485 LIVE-QOS: Gruppe "live" getrennt (Eintraege tragen selbst "live":true), Chatraum, Rest */
            static const char kLive[] =
                "{\"ok\":true,\"lobbies\":[{\"host\":\"A\",\"play_mode\":\"sim\",\"players\":1,\"max_players\":2,"
                "\"state\":\"open\"}],\"more\":0,\"live\":[{\"host\":\"LiveA\",\"game\":\"loc\",\"kind\":\"match\","
                "\"players\":2,\"max_players\":10,\"state\":\"open\",\"live\":true},{\"host\":\"LiveB\",\"kind\":\"chat\","
                "\"players\":1,\"max_players\":4,\"state\":\"started\",\"live\":true},{\"host\":\"\",\"players\":1,"
                "\"max_players\":2}],\"live_more\":4}";
            memset(&lb, 0, sizeof lb);
            if (lacc_lobbies_parse(kLive, &lb) != 0 || lb.n != 1 || lb.nlive != 2 || lb.live_more != 4 ||
                strcmp(lb.lv[0].host, "LiveA") || lb.lv[0].chat || lb.lv[0].started || lb.lv[0].players != 2 ||
                strcmp(lb.lv[1].host, "LiveB") || !lb.lv[1].chat || !lb.lv[1].started || strcmp(lb.l[0].host, "A"))
                _snprintf_s(why, sizeof why, _TRUNCATE, "W485 Live-Gruppe: n %d nlive %d live_more %d", lb.n, lb.nlive,
                            lb.live_more);
            memset(&lb, 0, sizeof lb);
            if (!why[0] && (lacc_lobbies_parse(kBody[2], &lb) != 0 || lb.nlive != 0 || lb.live_more != 0))
                strcpy_s(why, sizeof why, "W485: aelterer Server ohne live -> leer");
            memset(&lb, 0, sizeof lb);
            if (!why[0] && (lacc_lobbies_parse("{\"lobbies\":[],\"more\":0,\"live\":[{\"host\":\"Q\"", &lb) != 0 ||
                            lb.nlive != 0))
                strcpy_s(why, sizeof why, "W485: kaputte Live-Gruppe darf die Liste nicht kippen");
        }
    }
    /* 6: Seite mit 0/1/6 Lobbys (und ohne Liste/Fehler) ohne Ueberlappung in Layout A Normal/Gross */
    for (k = 0; k < 9 && !why[0]; k++) {
        static const int kFix[9] = { 0, 1, 6, -1, -2, -3, -4, -5, -6 }; /* W472: ohne Konto / abgelehnt / Rate */
        int sz;
        hub_lobby_fixture(kFix[k]);
        for (sz = LLAY_SZ_NORMAL; sz <= LLAY_SZ_LARGE && !why[0]; sz++) {
            char tag[48];
            lay_switch(LLAY_A, sz);
            _snprintf_s(tag, sizeof tag, _TRUNCATE, "W470 a/%s %d Lobbys", llay_size_name(sz), kFix[k]);
            lay_gt_page_check(why, sizeof why, tag);
            show_page(P_NETI);
            redraw_all();
            pump();
            if (!why[0] && g_page != P_NETI) strcpy_s(why, sizeof why, "Seite steelnet verloren");
        }
    }
    /* 7: EN/DE */
    if (!why[0]) {
        sb_lang_set("de");
        ui_apply_language();
        ui_gettext(ctl(ID_HUBNAV0), t, sizeof t);
        if (strcmp(t, "Allgemein") != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "DE Reiter '%s'", t);
        ui_gettext(ctl(ID_HUB_GOOPT), t, sizeof t);
        if (!why[0] && strcmp(t, LT("gui.hub_goopt")) != 0) strcpy_s(why, sizeof why, "DE Knopf");
        ui_gettext(ctl(ID_HUB_GOACC), t, sizeof t);
        if (!why[0] && (strcmp(t, LT("gui.hub_goacc")) != 0 || strcmp(t, "Open account") == 0)) strcpy_s(why, sizeof why, "DE Konto-Knopf");
        sb_lang_set("en");
        ui_apply_language();
        ui_gettext(ctl(ID_HUBNAV0 + 1), t, sizeof t);
        if (!why[0] && strcmp(t, "Info") != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "EN Reiter '%s'", t);
        ui_gettext(ctl(ID_HUBNAV0), t, sizeof t);
        if (!why[0] && strcmp(t, "General") != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "EN Reiter '%s'", t);
    }
    /* aufraeumen */
    sb_lang_set(keep);
    ui_apply_language();
    g_lay_cfg_id = cid0;
    g_lay_cfg_size = csz0;
    lay_switch(id0, sz0);
    g_o.shot = shot0;
    g_loc_online = on0;
    g_upd_cfg = uc0;
    g_onl_fix_srv = fs0;
    g_onl_fixture = fx0;
    g_acc = acc0;
    g_acc_view = av0;
    hub_net_changed();
    g_hubl_dry = dry0;
    g_game = g0;
    loctab_nav_layout();
    show_page(p0);
    if (!why[0]) {
        GT_OK("77e W470 LAUNCHER-HUB (Bereiche Spiel/Optionen/steelnet, Spiel-Navigation nur im Spielbereich, Reiter je "
              "Bereich, Rueckweg ueber die Spiel-Reiter auch mit Spielwechsel, Fussleiste bleibt, steelnet offline mit Weg "
              "in die Einstellungen, Lobbyliste nur sichtbar + hoechstens alle 12 s, Parser 0/1/6 + kaputt, Layout A "
              "Normal/Gross ohne Ueberlappung, EN/DE)");
    } else {
        GT_FAIL("77e W470 LAUNCHER-HUB", "%s", why);
    }
}

/* --- 77f (W473 LAUNCHER-MP-POLISH): Netzwerk-Karte mit zwei Reitern (Sichtbarkeit je Reiter, keine Ueberlappung),
 * Rundweg aller Einstellungen byte-gleich (user.cfg + user_loc.cfg, Schluessel/Standards unveraendert), Chat im Spiel
 * (Standard, vorgemerkt ohne user_loc.cfg, nie im --set), Auswahlfelder zeigen nur den Wert und schneiden nichts ab,
 * Beschriftungen/Hinweise passen (EN/DE, Layout A normal/gross), Spiel-Reiter ohne Unterstreichung in Optionen/steelnet,
 * Konto-Karte in den Zustaenden nicht angemeldet / ungesichert / Discord / Login / unbekannt. --- */
static void mp_gt_raw(const char *path, char *buf, size_t cap)
{
    FILE *f = NULL;
    size_t n = 0;
    if (u8_fopen_s(&f, path, "rb") == 0 && f) {
        n = fread(buf, 1, cap - 1, f);
        fclose(f);
    }
    buf[n] = '\0';
}

/* Datei im Textmodus schreiben (CRLF wie die echten Einstellungsdateien des Launchers) */
static void mp_gt_write(const char *path, const char *text)
{
    FILE *f = NULL;
    if (u8_fopen_s(&f, path, "w") == 0 && f) {
        fputs(text, f);
        fclose(f);
    }
}

/* Auswahlfeld: jeder Eintrag passt in die sichtbare Flaeche (rcItem, 4 px Rand), kein "Name:"-Vorsatz */
static void mp_gt_combo(int id, const char *lbl, const char *tag, char *why, size_t cap)
{
    HWND c = ctl(id);
    COMBOBOXINFO ci;
    HDC dc;
    HGDIOBJ o;
    int i, n, avail;
    if (why[0]) return;
    memset(&ci, 0, sizeof ci);
    ci.cbSize = sizeof ci;
    if (!c || !GetComboBoxInfo(c, &ci)) {
        _snprintf_s(why, cap, _TRUNCATE, "%s Auswahl %d fehlt", tag, id);
        return;
    }
    avail = ci.rcItem.right - ci.rcItem.left - 4;
    dc = GetDC(c);
    o = SelectObject(dc, (HFONT)SendMessageW(c, WM_GETFONT, 0, 0));
    n = ComboBox_GetCount(c);
    for (i = 0; i < n && !why[0]; i++) {
        wchar_t wt[200] = L"";
        char t[400];
        SIZE sz = { 0, 0 };
        SendMessageW(c, CB_GETLBTEXT, (WPARAM)i, (LPARAM)wt);
        GetTextExtentPoint32W(dc, wt, (int)wcslen(wt), &sz);
        WideCharToMultiByte(CP_UTF8, 0, wt, -1, t, (int)sizeof t, NULL, NULL);
        if (sz.cx > avail)
            _snprintf_s(why, cap, _TRUNCATE, "%s Auswahl %d: '%s' %ld px > %d px", tag, id, t, sz.cx, avail);
        else if (lbl && (strstr(t, LT(lbl)) || strchr(t, ':')))
            _snprintf_s(why, cap, _TRUNCATE, "%s Auswahl %d zeigt den Namen: '%s'", tag, id, t);
    }
    SelectObject(dc, o);
    ReleaseDC(c, dc);
    if (!why[0] && n < 2) _snprintf_s(why, cap, _TRUNCATE, "%s Auswahl %d: %d Eintraege", tag, id, n);
}

/* Beschriftungen und Hinweise des gewaehlten Reiters, Karte Match, Konto-Karte */
static void mp_gt_texts(const char *tag, char *why, size_t cap)
{
    int i;
    const char *bad;
    for (i = 0; i < lnet_row_count() && !why[0]; i++) {
        RECT rc;
        const char *t = NULL;
        char lb[160];
        int id = 0, r = lnet_hint_rect(i, &rc, &t, &id);
        if (r == 1 && !ui_text_fits(F_TINY, t, rc.left, rc.top + 1, rc.right - rc.left, 28, 1) &&
            !ui_text_fits(F_TINY, t, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, 0))
            _snprintf_s(why, cap, _TRUNCATE, "%s Hinweis %d abgeschnitten: '%s'", tag, id, t);
        if (!why[0] && lnet_label_rect(i, &rc, lb, sizeof lb) &&
            !ui_text_fits(F_SMALL, lb, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, 1))
            _snprintf_s(why, cap, _TRUNCATE, "%s Beschriftung '%s' zu breit", tag, lb);
        if (!why[0] && r == 1) { /* Schalter: Text passt in das Element (kein Abschneiden am Hinweis) */
            uictl_t *u = ui_find(id);
            char tt[200];
            if (u && u->kind == K_TOGGLE) {
                ui_gettext(u->h, tt, sizeof tt);
                if (!ui_text_fits(F_SMALL, tt, u->x + 46, u->y, u->w - 46, u->h_, 1))
                    _snprintf_s(why, cap, _TRUNCATE, "%s Schalter '%s' zu breit", tag, tt);
            }
        }
    }
    if (!why[0] && !ui_text_fits(F_TINY, LT("gui.lnet_intro"), CX0 + 18, CY0 + 196 + 46, CW - 36, 18, 1))
        _snprintf_s(why, cap, _TRUNCATE, "%s Einleitung zu breit", tag);
    bad = why[0] ? NULL : mp_labels_fit();
    if (bad) _snprintf_s(why, cap, _TRUNCATE, "%s Match-Beschriftung '%s' zu breit", tag, bad);
    if (!why[0]) acc_card_fits(why, cap, tag);
}

__declspec(noinline) void mppolish_gui_selftest(int *pass, int *fail)
{
    static const int kTab0[] = { ID_LNET_RB, ID_LNET_OWN, ID_LOC_SMOOTH, ID_LNET_LOWD };
    static const int kTab1[] = { ID_LNET_MODE, ID_LNET_CLIP, ID_LNET_CHAT, ID_LNET_SNCHAT,
                                 ID_LNET_MSN,  ID_LNET_MLOBBY, ID_LNET_PRIV }; /* Team-Schalter ohne Team-Reiter verborgen (24.11.) */
    static const char kUser[] =
        "[display]\nwidth=1280\n# fremde Zeile\nlauncher_loc_online=0\nlauncher_loc_rollback=1\nlauncher_loc_net_auto=0\n"
        "launcher_loc_own_predict=0\nlauncher_loc_low_delay=0\nlauncher_loc_smooth_opp=2\nlauncher_loc_clipdump_block=0\n"
        "launcher_loc_chat=1\nlauncher_loc_snchat=1\nlauncher_loc_play_mode=1\nlauncher_loc_public=0\n";
    static const char kLoc[] = "# user_loc.cfg Test\nloc_widescreen=2\nloc_chat_mission_sn=1\nloc_chat_mission_lobby=0\n"
                               "loc_chat_mission_team=0\n# Ende\n";
    char why[300] = "", tmp[1024], cfg[1200], lcfg[1200], a[4096], b[4096], a0[4096], b0[4096], keep[16], s[600];
    int g0 = g_game, p0 = g_page, on0 = g_loc_online, shot0 = g_o.shot, id0 = g_lay.id, sz0 = g_lay.size,
        cid0 = g_lay_cfg_id, csz0 = g_lay_cfg_size, av0 = g_acc_view, lan0 = g_loc_lan, i, k;
    lgame_state_t loc0 = g_loc;
    lacc_store_t acc0 = g_acc;
    for (i = 0; i < (int)(sizeof kTab1 / sizeof kTab1[0]); i++) {
        if (!ctl(kTab1[i]) || !ctl(ID_LNET_TAB0) || !ctl(ID_LNET_TAB1)) {
            GT_FAIL("77f W473 LAUNCHER-MP-POLISH", "%s", "Elemente fehlen");
            return;
        }
    }
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw473_gui_user_%lu.cfg", tmp, GetCurrentProcessId());
    mp_gt_write(cfg, kUser);
    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, cfg);
    game_cfg_path(LGAME_LOC, lcfg, sizeof lcfg);
    mp_gt_write(lcfg, kLoc);
    mp_gt_raw(cfg, a0, sizeof a0); /* Vergleich byte-genau gegen den Stand auf der Platte (CRLF) */
    mp_gt_raw(lcfg, b0, sizeof b0);
    strcpy_s(keep, sizeof keep, sb_lang_code());
    sb_lang_set("en");
    ui_apply_language();
    acc_fixture(1, 0, 0, 0, 0);
    gt_setenv("SB_TEST_LOC_READY", "1");
    g_acc_view = 0;
    g_game = LGAME_LOC;
    loctab_nav_layout();
    g_loc_online = 0;
    g_loc_lan = 0;
    onl_load();
    lnet_tab_pick(0);
    show_page(P_LNET);
    pump();
    /* 1: Reiter - Network gewaehlt (Standard), Elemente je Reiter */
    if (!ui_checked(ID_LNET_TAB0) || ui_checked(ID_LNET_TAB1) || g_lnet_tab != 0) strcpy_s(why, sizeof why, "Reiter Standard");
    for (i = 0; i < (int)(sizeof kTab0 / sizeof kTab0[0]); i++) loctab_vis(kTab0[i], 1, "Reiter Network", why, sizeof why);
    for (i = 0; i < (int)(sizeof kTab1 / sizeof kTab1[0]); i++) loctab_vis(kTab1[i], 0, "Reiter Network (Chat-Teil)", why, sizeof why);
    loctab_vis(ID_LNET_RESET, 1, "Reiter Network: Standard", why, sizeof why);
    on_command(ID_LNET_TAB1, BN_CLICKED);
    pump();
    if (!why[0] && (ui_checked(ID_LNET_TAB0) || !ui_checked(ID_LNET_TAB1) || g_lnet_tab != 1)) strcpy_s(why, sizeof why, "Reiter Lobby & chat");
    for (i = 0; i < (int)(sizeof kTab0 / sizeof kTab0[0]); i++) loctab_vis(kTab0[i], 0, "Reiter Lobby & chat (Netz-Teil)", why, sizeof why);
    for (i = 0; i < (int)(sizeof kTab1 / sizeof kTab1[0]); i++) loctab_vis(kTab1[i], 1, "Reiter Lobby & chat", why, sizeof why);
    loctab_vis(ID_LNET_RESET, 1, "Reiter Lobby & chat: Standard", why, sizeof why);
    /* 2: Werte aus beiden Dateien in der Oberflaeche */
    if (!why[0] && (ComboBox_GetCurSel(ctl(ID_LNET_RB)) != 1 || ui_checked(ID_LNET_OWN) || ui_checked(ID_LNET_LOWD) ||
                    ComboBox_GetCurSel(ctl(ID_LOC_SMOOTH)) != 2 || ui_checked(ID_LNET_CLIP) || !ui_checked(ID_LNET_CHAT) ||
                    !ui_checked(ID_LNET_SNCHAT) || ComboBox_GetCurSel(ctl(ID_LNET_MODE)) != 1 || !ui_checked(ID_LNET_MSN) ||
                    ui_checked(ID_LNET_MLOBBY) || ui_checked(ID_LNET_MTEAM) || !IsWindowEnabled(ctl(ID_LNET_MSN)) ||
                    !IsWindowEnabled(ctl(ID_LNET_MLOBBY)) || IsWindowEnabled(ctl(ID_LNET_MTEAM)) || lnet_is_default()))
        _snprintf_s(why, sizeof why, _TRUNCATE, "Werte geladen: msn %d lobby %d team %d", g_loc_msn, g_loc_mlobby, g_loc_mteam);
    /* 3: Rundweg - jede Einstellung hin und zurueck, beide Dateien danach byte-gleich (Schluessel, Werte, Reihenfolge,
     * fremde Zeilen); Team-Schalter grau (Team-Reiter im Spiel noch nicht da) -> Klick aendert nichts */
    if (!why[0]) {
        static const int kTg[] = { ID_LNET_OWN, ID_LNET_LOWD, ID_LNET_CLIP, ID_LNET_CHAT, ID_LNET_SNCHAT,
                                   ID_LNET_MSN, ID_LNET_MLOBBY, ID_LNET_MTEAM };
        static const struct {
            int id, other, back;
        } kCb[] = { { ID_LNET_RB, 0, 1 }, { ID_LOC_SMOOTH, 1, 2 }, { ID_LNET_MODE, 0, 1 } };
        for (i = 0; i < (int)(sizeof kTg / sizeof kTg[0]); i++) {
            on_command(kTg[i], BN_CLICKED);
            on_command(kTg[i], BN_CLICKED);
        }
        for (i = 0; i < (int)(sizeof kCb / sizeof kCb[0]); i++) {
            ComboBox_SetCurSel(ctl(kCb[i].id), kCb[i].other);
            on_command(kCb[i].id, CBN_SELCHANGE);
            ComboBox_SetCurSel(ctl(kCb[i].id), kCb[i].back);
            on_command(kCb[i].id, CBN_SELCHANGE);
        }
        mp_gt_raw(cfg, a, sizeof a);
        mp_gt_raw(lcfg, b, sizeof b);
        if (strcmp(a, a0) != 0 || !strchr(a0, '\r')) _snprintf_s(why, sizeof why, _TRUNCATE, "Rundweg user.cfg: %.200s", a);
        else if (strcmp(b, b0) != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "Rundweg user_loc.cfg: %.200s", b);
    }
    if (!why[0]) { /* ein Umlegen schreibt genau den Schluessel des Clients (W474), 1 = erlaubt */
        on_command(ID_LNET_MLOBBY, BN_CLICKED);
        gt_read(lcfg, b, sizeof b);
        if (!g_loc_mlobby || !strstr(b, "\nloc_chat_mission_lobby=1\n") || strstr(b, "loc_chat_mission_lobby=0"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Lobby-Chat im Spiel: %.160s", b);
        on_command(ID_LNET_MLOBBY, BN_CLICKED);
    }
    if (!why[0]) { /* nie im --set (der Client liest user_loc.cfg selbst) */
        s[0] = '\0';
        loc_rule_set_append(s, sizeof s);
        loc_net_set_append(s, sizeof s);
        if (strstr(s, "mission")) _snprintf_s(why, sizeof why, _TRUNCATE, "Chat im Spiel im --set: '%s'", s);
    }
    /* 4: grau - ohne Chat bzw. steelnet-Chat und im LAN; Wert bleibt gemerkt */
    if (!why[0]) {
        on_command(ID_LNET_CHAT, BN_CLICKED);
        on_command(ID_LNET_SNCHAT, BN_CLICKED);
        if (IsWindowEnabled(ctl(ID_LNET_MLOBBY)) || IsWindowEnabled(ctl(ID_LNET_MSN)) || !g_loc_msn)
            strcpy_s(why, sizeof why, "grau ohne Chat/steelnet-Chat");
        on_command(ID_LNET_MSN, BN_CLICKED); /* grau: aendert nichts */
        if (!why[0] && !g_loc_msn) strcpy_s(why, sizeof why, "grauer Schalter hat umgeschaltet");
        on_command(ID_LNET_CHAT, BN_CLICKED);
        on_command(ID_LNET_SNCHAT, BN_CLICKED);
        g_loc_lan = 1;
        onl_sync_ui();
        if (!why[0] && (IsWindowEnabled(ctl(ID_LNET_MSN)) || IsWindowEnabled(ctl(ID_LNET_SNCHAT)) || !IsWindowEnabled(ctl(ID_LNET_MLOBBY))))
            strcpy_s(why, sizeof why, "LAN: steelnet-Schalter nicht grau");
        g_loc_lan = 0;
        onl_sync_ui();
        mp_gt_raw(cfg, a, sizeof a);
        mp_gt_raw(lcfg, b, sizeof b);
        if (!why[0] && (strcmp(a, a0) != 0 || strcmp(b, b0) != 0)) strcpy_s(why, sizeof why, "grau: Dateien veraendert");
    }
    /* 5: Standard (Datei ohne Schluessel), Standard wiederherstellen, vorgemerkt ohne user_loc.cfg */
    if (!why[0]) {
        write_file(lcfg, "# leer\n");
        onl_load();
        onl_sync_ui();
        if (g_loc_msn || !g_loc_mlobby || !g_loc_mteam || ui_checked(ID_LNET_MSN) || !ui_checked(ID_LNET_MLOBBY))
            strcpy_s(why, sizeof why, "Standard Chat im Spiel (steelnet 0, Lobby 1, Team 1)");
        write_file(lcfg, "loc_chat_mission_sn=yes\nloc_chat_mission_lobby=2\nloc_chat_mission_team=\n");
        onl_load();
        if (!why[0] && (g_loc_msn || !g_loc_mlobby || !g_loc_mteam)) strcpy_s(why, sizeof why, "ungueltig = Standard");
    }
    if (!why[0]) {
        mp_gt_write(lcfg, kLoc);
        onl_load();
        on_command(ID_LNET_RESET, BN_CLICKED);
        gt_read(lcfg, b, sizeof b);
        if (g_loc_msn || !g_loc_mlobby || !g_loc_mteam || !lnet_is_default() ||
            !strstr(b, "loc_chat_mission_sn=0") || !strstr(b, "loc_chat_mission_lobby=1") || !strstr(b, "loc_chat_mission_team=1") ||
            !strstr(b, "loc_widescreen=2") || !strstr(b, "# Ende"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Standard wiederherstellen: %.160s", b);
    }
    if (!why[0]) {
        u8_DeleteFile(lcfg);
        on_command(ID_LNET_MSN, BN_CLICKED);
        if (!g_loc_msn || !(g_lnet_m_pending & 1) || u8_GetFileAttributes(lcfg) != INVALID_FILE_ATTRIBUTES)
            strcpy_s(why, sizeof why, "ohne user_loc.cfg: nicht vorgemerkt bzw. Datei angelegt");
        write_file(lcfg, "# entsteht beim Speichern\n");
        lnet_mission_flush();
        gt_read(lcfg, b, sizeof b);
        if (!why[0] && (g_lnet_m_pending || !strstr(b, "loc_chat_mission_sn=1")))
            _snprintf_s(why, sizeof why, _TRUNCATE, "vorgemerkt nicht nachgetragen: %.120s", b);
    }
    /* 5b: echter Weg - Speichern (do_save, auch vor jedem LoC-Start) legt user_loc.cfg an und traegt die Vormerkung
     * nach; Speichern und Spielerbetrieb-Block (LoC-Start) lassen die Schluessel stehen */
    if (!why[0]) {
        u8_DeleteFile(lcfg);
        on_command(ID_LNET_MSN, BN_CLICKED); /* 1 -> 0, vorgemerkt */
        if (do_save(0) != 0) {
            strcpy_s(why, sizeof why, "do_save (LoC) fehlgeschlagen");
        } else {
            gt_read(lcfg, b, sizeof b);
            if (g_lnet_m_pending || !strstr(b, "loc_chat_mission_sn=0\n"))
                _snprintf_s(why, sizeof why, _TRUNCATE, "do_save: Vormerkung fehlt: %.160s", b);
        }
        if (!why[0]) {
            mp_gt_write(lcfg, kLoc);
            if (do_save(0) != 0 || lcfg_write_player_block(lcfg) != 0) {
                strcpy_s(why, sizeof why, "do_save/Spielerbetrieb-Block fehlgeschlagen");
            } else {
                gt_read(lcfg, b, sizeof b);
                if (!strstr(b, "loc_chat_mission_sn=1\n") || !strstr(b, "loc_chat_mission_lobby=0\n") ||
                    !strstr(b, "loc_chat_mission_team=0\n"))
                    _snprintf_s(why, sizeof why, _TRUNCATE, "Speichern/Start verliert Chat im Spiel: %.200s", b);
            }
        }
    }
    /* 6: Darstellung - Auswahl nur Wert, nichts abgeschnitten, keine Ueberlappung (EN/DE, Layout A normal/gross) */
    g_o.shot = 2; /* Layout nicht speichern */
    for (k = 0; k < 4 && !why[0]; k++) {
        int sz = k & 1 ? LLAY_SZ_LARGE : LLAY_SZ_NORMAL, tb, ln;
        char tag[48];
        sb_lang_set(k & 2 ? "de" : "en");
        ui_apply_language();
        lay_switch(LLAY_A, sz);
        for (ln = 0; ln < 2 && !why[0]; ln++) { /* steelnet / LAN */
            g_loc_lan = ln;
            for (tb = 0; tb < 2 && !why[0]; tb++) {
                _snprintf_s(tag, sizeof tag, _TRUNCATE, "W473 %s/%s %s %s", k & 2 ? "de" : "en", llay_size_name(sz),
                            ln ? "LAN" : "online", tb ? "Lobby&chat" : "Network");
                lnet_tab_pick(tb);
                lay_gt_page_check(why, sizeof why, tag);
                show_page(P_LNET);
                pump();
                mp_gt_combo(ID_LNET_KIND, NULL, tag, why, sizeof why);
                mp_gt_combo(ID_LOC_VIS, NULL, tag, why, sizeof why);
                mp_gt_combo(ID_LNET_RB, "gui.lnet_rb", tag, why, sizeof why);
                mp_gt_combo(ID_LOC_SMOOTH, "gui.lnet_smooth", tag, why, sizeof why);
                mp_gt_combo(ID_LNET_MODE, "gui.lnet_mode", tag, why, sizeof why);
                mp_gt_texts(tag, why, sizeof why);
            }
        }
        g_loc_lan = 0;
        /* Konto-Karte: alle Zustaende */
        for (i = 0; i < 5 && !why[0]; i++) {
            static const int kAcc[5][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 1, 0, 1 }, { 1, 0, 0 } };
            acc_fixture(1, kAcc[i][0], kAcc[i][1], kAcc[i][2], 0);
            if (i == 4) { /* Server nicht erreichbar */
                g_acc_st_ok = 0;
                memset(&g_acc_st, 0, sizeof g_acc_st);
                strcpy_s(g_acc_st_err, sizeof g_acc_st_err, "net");
            }
            acc_refresh();
            show_page(P_LNET);
            pump();
            _snprintf_s(tag, sizeof tag, _TRUNCATE, "W473 %s/%s Konto %d", k & 2 ? "de" : "en", llay_size_name(sz), i);
            acc_card_fits(why, sizeof why, tag);
            if (!why[0]) { /* Zeile Konto auf steelnet > Info passt in die Wertspalte */
                char hv[200] = "";
                COLORREF hc;
                if (!hub_acc_value(hv, sizeof hv, &hc) || !hub_acc_fits(hv))
                    _snprintf_s(why, sizeof why, _TRUNCATE, "%s steelnet-Zeile Konto '%s' zu breit", tag, hv);
            }
            lay_gt_page_check(why, sizeof why, tag);
            show_page(P_LNET);
            pump();
        }
    }
    sb_lang_set("en");
    ui_apply_language();
    lay_switch(LLAY_A, LLAY_SZ_NORMAL);
    lnet_tab_pick(0);
    /* 7: Konto-Karte - eine Hauptaktion, Zustand, gleiche Woerter wie steelnet > Info (acc_sum_row) */
    for (i = 0; i < 5 && !why[0]; i++) {
        static const int kAcc[5][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 1, 0, 1 }, { 1, 0, 0 } };
        static const int kSt[5] = { ACCS_NONE, ACCS_UNSEC, ACCS_SEC, ACCS_SEC, ACCS_UNK };
        static const char *const kSum[5] = { "No steelnet account yet", "Ace Pilot \xC2\xB7 not secured",
                                             "Ace Pilot \xC2\xB7 secured", "Ace Pilot \xC2\xB7 secured",
                                             "Ace Pilot \xC2\xB7 status unknown" };
        static const COLORREF kCol[5] = { C_WARN, C_WARN, C_OK, C_OK, C_DIM };
        char sum[200] = "";
        COLORREF col = 0;
        uictl_t *mg;
        acc_fixture(1, kAcc[i][0], kAcc[i][1], kAcc[i][2], 0);
        if (i == 4) {
            g_acc_st_ok = 0;
            memset(&g_acc_st, 0, sizeof g_acc_st);
            strcpy_s(g_acc_st_err, sizeof g_acc_st_err, "net");
        }
        acc_refresh();
        show_page(P_LNET);
        pump();
        mg = ui_find(ID_ACC_MANAGE);
        if (acc_card_state() != kSt[i]) _snprintf_s(why, sizeof why, _TRUNCATE, "Konto %d: Zustand %d", i, acc_card_state());
        loctab_vis(ID_ACC_NAME, i == 0, "Konto: Name", why, sizeof why);
        loctab_vis(ID_ACC_CREATE, i == 0, "Konto: Erstellen", why, sizeof why);
        loctab_vis(ID_ACC_SIGNIN, i == 0, "Konto: schon ein Konto", why, sizeof why);
        loctab_vis(ID_ACC_DISCORD, i == 1, "Konto: Discord nur ungesichert", why, sizeof why);
        loctab_vis(ID_ACC_MANAGE, i != 0, "Konto: Verwalten", why, sizeof why);
        if (!why[0] && i == 1 && (ui_find(ID_ACC_DISCORD)->kind != K_ACCENT || !mg || mg->x != ACC_X + 18 + 174))
            strcpy_s(why, sizeof why, "Konto ungesichert: Discord Hauptknopf, Verwalten daneben");
        if (!why[0] && i >= 2 && (!mg || mg->x != ACC_X + 18))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Konto %d: Verwalten links (einziger Knopf)", i);
        if (!why[0] && (!acc_sum_row(sum, sizeof sum, &col) || strncmp(sum, kSum[i], strlen(kSum[i])) != 0 || col != kCol[i]))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Konto %d: Zeile Spielen/Bereitschaft '%s'", i, sum);
        if (!why[0] && (!hub_acc_value(sum, sizeof sum, &col) ||
                        (i == 0 ? strcmp(sum, "Not signed in") != 0 : strncmp(sum, kSum[i], strlen(kSum[i])) != 0)))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Konto %d: Zeile steelnet '%s'", i, sum);
    }
    if (!why[0]) { /* Erklaerung nennt die Wege aus "Manage account" (Discord, Login, Wiederherstellungscode) */
        const char *u = LT("gui.acc_c_unsec_why");
        if (!strstr(u, "Discord") || !strstr(u, "login") || !strstr(u, "recovery code") || !strstr(u, "Manage account") ||
            !strstr(LT("gui.acc_manage"), "Manage account"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Erklaerung ungesichert '%s'", u);
    }
    /* 8: Spiel-Reiter - Unterstreichung nur im Spielbereich, Rueckweg mit Unterstreichung */
    if (!why[0]) {
        uictl_t *gs = ui_find(ID_GAME_SB), *gl = ui_find(ID_GAME_LOC);
        show_page(P_LNET);
        pump();
        if (!gametab_active(gl) || gametab_active(gs)) strcpy_s(why, sizeof why, "Spielbereich: LoC unterstrichen");
        on_command(ID_LSET_OPEN, BN_CLICKED);
        pump();
        if (!why[0] && (g_page != P_LSET || gametab_active(gl) || gametab_active(gs) || !ui_checked(ID_GAME_LOC)))
            strcpy_s(why, sizeof why, "Optionen: Unterstreichung weg (Spiel bleibt gewaehlt)");
        hdr_click(HDR_CHIP_ST);
        pump();
        if (!why[0] && (g_page != P_NETI || gametab_active(gl) || gametab_active(gs)))
            _snprintf_s(why, sizeof why, _TRUNCATE, "steelnet: Unterstreichung weg (Seite %d)", g_page);
        on_command(ID_GAME_LOC, BN_CLICKED);
        pump();
        if (!why[0] && (hub_area_of(g_page) != HUB_GAME || !gametab_active(gl) || gametab_active(gs)))
            strcpy_s(why, sizeof why, "Rueckweg: Unterstreichung wieder da");
    }
    /* aufraeumen */
    SetEnvironmentVariableA("SB_TEST_LOC_READY", NULL);
    u8_DeleteFile(cfg);
    u8_DeleteFile(lcfg);
    g_ucfg_test[0] = '\0';
    sb_lang_set(keep);
    ui_apply_language();
    g_lay_cfg_id = cid0;
    g_lay_cfg_size = csz0;
    lay_switch(id0, sz0);
    g_o.shot = shot0;
    g_onl_fix_srv = -1;
    g_loc_online = on0;
    g_loc_lan = lan0;
    g_loc_smooth_opp = 1; /* Standard (echte Werte liest onl_load beim naechsten Start) */
    g_loc_rb_on = g_loc_own_pred = g_loc_lowd = g_loc_net_auto = g_loc_clip = g_loc_chat = g_loc_snchat = 1;
    g_loc_arcade = 0;
    g_loc_msn = 0;
    g_loc_mlobby = g_loc_mteam = 1;
    g_lnet_m_pending = 0;
    g_lnet_tab = 0;
    g_loc = loc0;
    g_acc_fixture = 0;
    g_acc_loaded = 0;
    g_acc = acc0;
    g_acc_view = av0;
    g_game = g0;
    onl_sync_ui();
    loctab_nav_layout();
    show_page(p0);
    if (!why[0]) {
        GT_OK("77f W473 LAUNCHER-MP-POLISH (Netzwerk-Karte mit Reitern Network / Lobby & chat, Rundweg aller "
              "Einstellungen byte-gleich in user.cfg + user_loc.cfg, Chat im Spiel loc_chat_mission_* Standard 0/1/1, "
              "grau ohne Chat/steelnet/LAN/Team-Reiter, vorgemerkt ohne user_loc.cfg, nie im --set; Auswahl nur Wert und "
              "ohne Abschneiden, Hinweise/Beschriftungen passen, keine Ueberlappung - EN/DE, Layout A normal/gross, online/"
              "LAN; Konto-Karte nicht angemeldet/ungesichert/Discord/Login/unbekannt mit einer Hauptaktion, gleiche Woerter "
              "wie steelnet > Info; Spiel-Reiter ohne Unterstreichung in Optionen/steelnet)");
    } else {
        GT_FAIL("77f W473 LAUNCHER-MP-POLISH", "%s", why);
    }
}

/* W486 SB-GFX-OPTIONS (Fall 74h, GitHub #98): jedes Auswahlfeld der Grafikseite (Karten rechts) in SB und LoC je
 * Preset einmal aendern wie mit der Maus. Die Auswahl laeuft durch die echte user32-Kette des Kombifelds: Zeile der
 * Liste setzen, die Liste meldet LBN_SELCHANGE an das Kombifeld, user32 sendet daraufhin CBN_SELENDOK, CBN_CLOSEUP
 * und CBN_SELCHANGE per WM_COMMAND an das Hauptfenster (keine Eingabe, kein Fokus, Fenster unsichtbar). In 0.5.0 lief
 * nach CBN_SELENDOK die Regelauswertung mit gfx_sync_ui und setzte das Feld auf den alten Wert zurueck; CBN_SELCHANGE
 * las danach den alten Wert. Pruefung je Feld: Auswahl bleibt, Preset springt auf Custom (Cockpit gehoert nicht zum
 * Preset: Preset bleibt), Speichern + Neustart zeigt den Wert. user.cfg/user_loc.cfg = Temp-Dateien (g_ucfg_test). */
#define W486_NIDS 8
static const int kW486Ids[W486_NIDS] = { ID_MONFX, ID_CHIDE, ID_GFX_LIGHT, ID_GFX_SHADOW, ID_GFX_SOFT, ID_GFX_FX,
                                         ID_AAMODE, ID_GFX_TEX };
static int g_w486_seq[16], g_w486_nseq, g_w486_id;

static LRESULT CALLBACK w486_spy(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR sid, DWORD_PTR ref)
{
    LRESULT r;
    int note = msg == WM_COMMAND && (int)LOWORD(wp) == g_w486_id ? (int)HIWORD(wp) : -1;
    (void)sid;
    (void)ref;
    if (note >= 0 && g_w486_nseq < 16) {
        g_w486_seq[g_w486_nseq++] = note;
    }
    r = DefSubclassProc(h, msg, wp, lp);
    if (note == CBN_SELENDOK) { /* offene Liste: user32 schliesst sie zwischen SELENDOK und SELCHANGE (CBN_CLOSEUP) */
        SendMessageW(h, WM_COMMAND, MAKEWPARAM(g_w486_id, CBN_CLOSEUP), lp);
    }
    return r;
}

/* Auswahl wie mit der Maus (Liste oeffnet, Zeile, Liste schliesst); Rueckgabe: Auswahl danach */
static int w486_pick_real(int id, int sel)
{
    HWND c = ctl(id), par;
    COMBOBOXINFO ci;
    memset(&ci, 0, sizeof ci);
    ci.cbSize = sizeof ci;
    if (!c || !GetComboBoxInfo(c, &ci) || !ci.hwndList) {
        return -2;
    }
    par = GetParent(c);
    g_w486_id = id;
    g_w486_nseq = 0;
    SetWindowSubclass(par, w486_spy, 486, 0);
    SendMessageW(par, WM_COMMAND, MAKEWPARAM(id, CBN_DROPDOWN), (LPARAM)c); /* Liste oeffnet */
    SendMessageW(ci.hwndList, LB_SETCURSEL, (WPARAM)sel, 0);                /* Zeile unter der Maus */
    SendMessageW(c, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(ci.hwndList), LBN_SELCHANGE), (LPARAM)ci.hwndList);
    pump();
    RemoveWindowSubclass(par, w486_spy, 486);
    return ComboBox_GetCurSel(c);
}

static int w486_saw(int code)
{
    int i;
    for (i = 0; i < g_w486_nseq; i++) {
        if (g_w486_seq[i] == code) {
            return 1;
        }
    }
    return 0;
}

static const char *w486_name(int id)
{
    return id == ID_MONFX ? "Monitor effect" : id == ID_CHIDE ? "Cockpit" : id == ID_GFX_LIGHT ? "World lighting"
           : id == ID_GFX_SHADOW ? "Shadows" : id == ID_GFX_SOFT ? "Soft shadows" : id == ID_GFX_FX ? "Effects"
           : id == ID_AAMODE ? "Anti-aliasing" : "Textures";
}

/* Neustart des aktiven Spiels (SB wie app_load_effective, LoC wie pergame_load) */
static void w486_reload(const char *cfg)
{
    if (g_game == LGAME_LOC) {
        pergame_load(LGAME_LOC, &g_s);
    } else {
        lcfg_defaults(&g_s);
        g_s.gfx_preset_rev = 0;
        lcfg_load_file(cfg, &g_s);
        lcfg_gfx_migrate_fps(&g_s);
        (void)lcfg_gfx_migrate_remaster(&g_s);
    }
    disk_base_sync(&g_s);
    ui_from_settings(&g_s);
    show_page(P_GFX);
    pump();
}

/* Ein Feld im Preset p aendern, Custom pruefen, speichern, neu laden, Anzeige pruefen, Preset wieder setzen */
static void w486_field(int p, int id, const char *cfg, const char *tag, char *why, size_t cap, int *done)
{
    HWND c = ctl(id);
    lgp_mask_t lock = gfx_lock_now();
    launcher_settings_t r0, r1;
    int cur, n, want, got, i, m, sel[W486_NIDS], chk;
    char t[600];
    if (!c || !ui_visible_on(ui_find(id), P_GFX)) {
        return; /* nicht auf der Seite (z. B. Texturen) */
    }
    if (!IsWindowEnabled(c)) {
        tip_text(id, t, sizeof t);
        if (!t[0]) {
            _snprintf_s(why, cap, _TRUNCATE, "%s %s: grau ohne Begruendung", tag, w486_name(id));
        }
        return; /* gesperrt mit Grund (LoC-Sperre W402, Regel R15/R16) */
    }
    w476_now(&r0);
    cur = ComboBox_GetCurSel(c);
    n = id == ID_GFX_FX ? 3 : ComboBox_GetCount(c); /* Effekte: ohne Eintrag "Custom" */
    want = (cur + 1) % n;
    got = w486_pick_real(id, want);
    w476_now(&r1);
    m = lcfg_gfx_match(&r1, lock);
    /* Erwartetes Preset: Custom (bzw. ein anderes Preset mit genau diesen Werten). Nicht im Preset sind Cockpit
     * (W268/W319) und die Monitor-Staerken Grain/Interference (W105; im Preset nur an/aus = monitor_fx, W252). */
    if (id == ID_CHIDE || (id == ID_MONFX && cur < 3 && want < 3)) {
        chk = p;
    } else {
        chk = (m != LGP_CUSTOM && m != p && lcfg_gfx_visible(m)) ? m : LGP_CUSTOM;
    }
    if (got != want) {
        _snprintf_s(why, cap, _TRUNCATE, "%s %s: Auswahl %d nicht uebernommen (zeigt %d, Folge %d Meldungen)", tag,
                    w486_name(id), want, got, g_w486_nseq);
    } else if (!w486_saw(CBN_SELCHANGE)) {
        _snprintf_s(why, cap, _TRUNCATE, "%s %s: keine CBN_SELCHANGE-Meldung", tag, w486_name(id));
    } else if (memcmp(&r0, &r1, sizeof r0) == 0) {
        _snprintf_s(why, cap, _TRUNCATE, "%s %s: Wert unveraendert", tag, w486_name(id));
    } else if (!ui_checked(ID_GP0 + chk) || (chk != p && ui_checked(ID_GP0 + p)) ||
               (id != ID_CHIDE && lcfg_gfx_preset(&r1, lock) != chk)) {
        _snprintf_s(why, cap, _TRUNCATE, "%s %s: Preset nicht %d (Custom %d, alt %d)", tag, w486_name(id), chk,
                    ui_checked(ID_GP0 + LGP_CUSTOM), ui_checked(ID_GP0 + p));
    }
    for (i = 0; i < W486_NIDS; i++) {
        sel[i] = ctl(kW486Ids[i]) ? ComboBox_GetCurSel(ctl(kW486Ids[i])) : -1;
    }
    if (!why[0]) { /* Speichern, Neustart: Wert und Custom stehen noch da */
        int rc = g_game == LGAME_LOC ? do_save(0) : w476_save_sb(cfg);
        if (rc != 0) {
            _snprintf_s(why, cap, _TRUNCATE, "%s %s: Speichern rc %d", tag, w486_name(id), rc);
        } else {
            w486_reload(cfg);
            for (i = 0; i < W486_NIDS && !why[0]; i++) {
                int s = ctl(kW486Ids[i]) ? ComboBox_GetCurSel(ctl(kW486Ids[i])) : -1;
                if (s != sel[i]) {
                    _snprintf_s(why, cap, _TRUNCATE, "%s %s: nach Neustart %s %d statt %d", tag, w486_name(id),
                                w486_name(kW486Ids[i]), s, sel[i]);
                }
            }
            if (!why[0] && !ui_checked(ID_GP0 + chk)) {
                _snprintf_s(why, cap, _TRUNCATE, "%s %s: nach Neustart Preset nicht %d", tag, w486_name(id), chk);
            }
        }
    }
    g_confirm_auto = 1; /* R14: eigene Werte ersetzen */
    if (!ui_checked(ID_GP0 + p)) {
        on_command(ID_GP0 + p, BN_CLICKED);
        pump();
    }
    if (ComboBox_GetCurSel(c) != cur) {
        (void)w486_pick_real(id, cur); /* nicht im Preset (Cockpit, Monitor-Staerke): Feld selbst zurueck */
    }
    if (!why[0] && (!ui_checked(ID_GP0 + p) || ComboBox_GetCurSel(c) != cur)) {
        _snprintf_s(why, cap, _TRUNCATE, "%s %s: Preset %d nicht wieder gesetzt", tag, w486_name(id), p);
    }
    (*done)++;
}

__declspec(noinline) void w486_gui_selftest(int *pass, int *fail)
{
    static const int kSbP[3] = { LGP_ORIGINAL, LGP_PERFORMANCE, LGP_CLEAR };
    static const int kLocP[3] = { LGP_ORIGINAL, LGP_ENHANCED, LGP_VIVID };
    static const int kSbNeed[6] = { ID_MONFX, ID_CHIDE, ID_GFX_LIGHT, ID_GFX_SHADOW, ID_GFX_FX, ID_AAMODE };
    char why[300] = "", tmp[1024], cfg[1200], lcf[1200], seq[80] = "";
    launcher_settings_t keep_s = g_s, keep_base = g_disk_base;
    int keep_ok = g_disk_base_ok, keep_game = g_game, keep_page = g_page, keep_conf = g_confirm_auto;
    int keep_adv = g_show_adv, g, i, k, done = 0;
    lgame_state_t keep_loc = g_loc;
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw486_user_%lu.cfg", tmp, GetCurrentProcessId());
    lgame_cfg_path(cfg, LGAME_LOC, lcf, sizeof lcf);
    write_file(cfg, "[display]\nwidth=1280\nlauncher_pergame=1\nlauncher_loc169=1\n");
    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, cfg);
    u8_DeleteFile(lcf);
    memset(g_pg_ok, 0, sizeof g_pg_ok);
    g_loc.loc_widescreen = LGAME_WIDE_169;
    g_loc.loc_sblook = 0;
    g_game = LGAME_SB;
    g_show_adv = 1; /* Feinregler sichtbar: auch "Weiche Schatten" (ohne launcher_ui.cfg zu schreiben) */
    lcfg_defaults(&g_s);
    disk_base_sync(&g_s);
    ui_from_settings(&g_s);
    lay_gfx_apply();
    show_page(P_GFX);
    pump();
    for (g = 0; g < 2 && !why[0]; g++) {
        if (g == 1) {
            loctab_set_game(LGAME_LOC);
            show_page(P_GFX);
            pump();
        }
        for (i = 0; i < 3 && !why[0]; i++) {
            int p = g ? kLocP[i] : kSbP[i];
            char tag[40];
            _snprintf_s(tag, sizeof tag, _TRUNCATE, "%s P%d", g ? "LoC" : "SB", p);
            g_confirm_auto = 1;
            if (!ui_checked(ID_GP0 + p)) {
                on_command(ID_GP0 + p, BN_CLICKED);
                pump();
            }
            if (!ui_checked(ID_GP0 + p)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "%s: Preset nicht waehlbar", tag);
                break;
            }
            if (!g && p == LGP_ORIGINAL) { /* #98: in SB Original sind alle sechs Felder frei */
                for (k = 0; k < 6 && !why[0]; k++) {
                    HWND c = ctl(kSbNeed[k]);
                    if (!c || !ui_visible_on(ui_find(kSbNeed[k]), P_GFX) || !IsWindowEnabled(c))
                        _snprintf_s(why, sizeof why, _TRUNCATE, "%s %s: nicht sichtbar/frei", tag,
                                    w486_name(kSbNeed[k]));
                }
            }
            if (g && (IsWindowEnabled(ctl(ID_MONFX)) || IsWindowEnabled(ctl(ID_CHIDE)))) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "%s: Monitor/Cockpit in LoC nicht gesperrt (W402)", tag);
            }
            for (k = 0; k < W486_NIDS && !why[0]; k++) {
                w486_field(p, kW486Ids[k], cfg, tag, why, sizeof why, &done);
                if (!seq[0] && g_w486_nseq > 0) { /* beobachtete Folge einmal ausgeben (Beleg) */
                    int j;
                    for (j = 0; j < g_w486_nseq; j++) {
                        char x[8];
                        _snprintf_s(x, sizeof x, _TRUNCATE, "%s%d", j ? "," : "", g_w486_seq[j]);
                        strcat_s(seq, sizeof seq, x);
                    }
                }
            }
        }
    }
    if (!why[0] && done < 24) {
        _snprintf_s(why, sizeof why, _TRUNCATE, "nur %d Felder geprueft", done);
    }
    printf("     (74h: %d Felder geaendert, Meldungen je Auswahl %s)\n", done, seq);
    if (g_game == LGAME_LOC) {
        loctab_set_game(LGAME_SB);
    }
    u8_DeleteFile(cfg);
    u8_DeleteFile(lcf);
    g_ucfg_test[0] = '\0';
    g_loc = keep_loc;
    memset(g_pg_ok, 0, sizeof g_pg_ok);
    g_confirm_auto = keep_conf;
    g_show_adv = keep_adv;
    g_game = keep_game;
    g_s = keep_s;
    g_disk_base = keep_base;
    g_disk_base_ok = keep_ok;
    ui_from_settings(&g_s);
    lay_gfx_apply();
    loctab_nav_layout();
    show_page(keep_page);
    ui_apply_rules();
    if (!why[0]) {
        GT_OK("74h W486 GFX-OPTIONS (#98: jedes Auswahlfeld der Grafikseite per echter Kombifeld-Kette geaendert, SB "
              "Original/Performance/Clear view + LoC Original/Enhanced/Vivid: Auswahl bleibt, Preset -> Custom, "
              "Speichern + Neustart zeigt den Wert, LoC-Sperren bleiben)");
    } else {
        GT_FAIL("74h W486 GFX-OPTIONS", "%s", why);
    }
}

/* W496 NET-TELEMETRY (Fall 77g): Frage "anonyme Netzwerkstatistik" (einmal, Ja = Standard, Esc/X = Nein und hoechstens
 * noch einmal fragen), Schalter auf der Netzwerk-Seite, net_report im LoC-Start nur online + Zustimmung. Temp-user.cfg. */
static int w496_has(const char *path, const char *needle)
{
    char b[4096] = "";
    FILE *f = NULL;
    size_t n;
    if (u8_fopen_s(&f, path, "rb") != 0 || !f) return 0;
    n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    return strstr(b, needle) != NULL;
}
__declspec(noinline) void w496_gui_selftest(int *pass, int *fail)
{
    char why[300] = "", tmp[1024], cfg[1200], keep_ucfg[1200], set[200];
    int keep_on = g_loc_online, keep_fix = g_onl_fix_srv, keep_ask = g_upd_ask, keep_page = g_page;
    int keep_cfg = g_nrq_cfg, keep_dis = g_nrq_dismiss, keep_open = g_nrq_open, keep_now = g_nrq_asked_now;
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw496_user_%lu.cfg", tmp, GetCurrentProcessId());
    strcpy_s(keep_ucfg, sizeof keep_ucfg, g_ucfg_test);
    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, cfg);
    g_onl_fix_srv = 1;
    g_upd_ask = 0;
    /* 1. Regel */
    if (!nrq_should_ask(-1, 0, 1, 0) || !nrq_should_ask(-1, 1, 1, 0) || nrq_should_ask(-1, 2, 1, 0) ||
        nrq_should_ask(0, 0, 1, 0) || nrq_should_ask(1, 0, 1, 0) || nrq_should_ask(-1, 0, 0, 0) || nrq_should_ask(-1, 0, 1, 1))
        strcpy_s(why, sizeof why, "Regel nrq_should_ask");
    /* 2. nie beantwortet + steelnet -> Dialog, nur seine Knoepfe; ohne Antwort net_report=0 */
    if (!why[0]) {
        write_file(cfg, "launcher_loc_online=1\n");
        onl_load();
        nrq_load();
        g_nrq_asked_now = 0;
        nrq_eval();
        pump();
        set[0] = 0;
        nrq_set_append(set, sizeof set, 1);
        if (!g_nrq_open || !IsWindowVisible(ctl(ID_NRQ_YES)) || !IsWindowVisible(ctl(ID_NRQ_NO)) ||
            (ctl(ID_START_BIG) && IsWindowVisible(ctl(ID_START_BIG))))
            strcpy_s(why, sizeof why, "Dialog nicht offen bzw. nicht modal");
        else if (strcmp(set, "net_report=0") != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "ohne Antwort gesendet: %s", set);
    }
    /* 3. Nein danke */
    if (!why[0]) {
        on_command(ID_NRQ_NO, BN_CLICKED);
        pump();
        if (g_nrq_open || g_nrq_cfg != 0 || IsWindowVisible(ctl(ID_NRQ_YES)) || !w496_has(cfg, "launcher_net_report=0"))
            strcpy_s(why, sizeof why, "Nein: Zustand/Datei");
        nrq_load();
        if (!why[0] && (g_nrq_cfg != 0 || nrq_should_ask(g_nrq_cfg, g_nrq_dismiss, 1, 0)))
            strcpy_s(why, sizeof why, "Nein: nach Neustart erneut gefragt");
    }
    /* 4. Esc/X ohne Antwort: Nein fuer jetzt, hoechstens noch einmal fragen */
    if (!why[0]) {
        int r;
        write_file(cfg, "launcher_loc_online=1\n");
        for (r = 0; r < 3 && !why[0]; r++) {
            nrq_load();
            g_nrq_asked_now = 0;
            nrq_eval();
            if (r < 2 && !g_nrq_open) _snprintf_s(why, sizeof why, _TRUNCATE, "Esc: Start %d nicht gefragt", r + 1);
            if (r == 2 && g_nrq_open) strcpy_s(why, sizeof why, "Esc: dritter Start fragt noch");
            nrq_dismiss();
            pump();
        }
        set[0] = 0;
        nrq_set_append(set, sizeof set, 1);
        if (!why[0] && (g_nrq_cfg != -1 || !w496_has(cfg, "launcher_net_report_dismiss=2") || strcmp(set, "net_report=0")))
            strcpy_s(why, sizeof why, "Esc: Zaehler/Zustimmung");
    }
    /* 5. Ja (Standard-Knopf) -> net_report=1 nur online */
    if (!why[0]) {
        write_file(cfg, "launcher_loc_online=1\n");
        nrq_load();
        g_nrq_asked_now = 0;
        nrq_eval();
        on_command(ID_NRQ_YES, BN_CLICKED);
        pump();
        set[0] = 0;
        nrq_set_append(set, sizeof set, 1);
        if (g_nrq_open || g_nrq_cfg != 1 || !w496_has(cfg, "launcher_net_report=1") || strcmp(set, "net_report=1"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Ja: Zustand/Datei/Start (%s)", set);
        set[0] = 0;
        nrq_set_append(set, sizeof set, 0);
        if (!why[0] && strcmp(set, "net_report=0") != 0) strcpy_s(why, sizeof why, "Ja: offline gesendet");
    }
    /* 6. Schalter (gleicher Kurztext) */
    if (!why[0]) {
        lnet_toggle(ID_LNET_NETREP);
        if (g_nrq_cfg != 0 || ui_checked(ID_LNET_NETREP) || !w496_has(cfg, "launcher_net_report=0"))
            strcpy_s(why, sizeof why, "Schalter aus");
        lnet_toggle(ID_LNET_NETREP);
        if (!why[0] && (g_nrq_cfg != 1 || !ui_checked(ID_LNET_NETREP)))
            strcpy_s(why, sizeof why, "Schalter an");
        if (!why[0] && strcmp(LT("gui.lnet_netrep"), LT("gui.nrq_title")) != 0)
            strcpy_s(why, sizeof why, "Schalter: anderer Kurztext als der Dialog");
    }
    /* 7. Text: "keine persoenlichen Daten", nicht "keine Daten" */
    if (!why[0] && (!strstr(LT("gui.nrq_text"), "no personal data") || strstr(LT("gui.nrq_text"), "no data")))
        strcpy_s(why, sizeof why, "Text: no personal data");
    DeleteFileA(cfg);
    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, keep_ucfg);
    g_loc_online = keep_on;
    g_onl_fix_srv = keep_fix;
    g_upd_ask = keep_ask;
    g_nrq_cfg = keep_cfg;
    g_nrq_dismiss = keep_dis;
    g_nrq_open = keep_open;
    g_nrq_asked_now = keep_now;
    show_page(keep_page);
    ui_refresh_visibility();
    onl_sync_ui();
    if (!why[0]) {
        GT_OK("77g W496 NET-TELEMETRY (Frage anonyme Netzwerkstatistik: einmal, modal, Ja/Nein, Esc/X = Nein und hoechstens "
              "noch einmal, Schalter mit gleichem Kurztext, net_report=1 nur online mit Zustimmung)");
    } else {
        GT_FAIL("77g W496 NET-TELEMETRY", "%s", why);
    }
}

/* W522 CRASH-TELEMETRY (Fall 77h): Exitcode des Spiels -> Datensatz nur mit Zustimmung (Temp-Ordner), SB-Start bekommt
 * net_report=1 nur mit Zustimmung + Stufe steelnet, Zustimmungstext nennt Absturzart und Ort (EN/DE). */
__declspec(noinline) void w522_gui_selftest(int *pass, int *fail)
{
    char why[300] = "", tmp[1024], dir[1200], set[200];
    wchar_t cp[700];
    sbct_rec r;
    int keep_cfg = g_nrq_cfg, keep_on = g_loc_online, keep_fix = g_onl_fix_srv;
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%sw522_ct_%lu", tmp, GetCurrentProcessId());
    CreateDirectoryA(dir, NULL);
    sbct_path(cp, 700, dir, 1, SBCT_FILE);
    DeleteFileW(cp);
    /* 1. ohne Zustimmung: nichts */
    g_nrq_cfg = 0;
    if (nrq_crash_note_dir(dir, 1, SBCT_G_LOC, 100) != 0 || sbct_read(cp, &r) != 0)
        strcpy_s(why, sizeof why, "ohne Zustimmung Datensatz geschrieben");
    /* 2. mit Zustimmung: von aussen beendet -> killed (LoC, Laufzeitklasse) */
    g_nrq_cfg = 1;
    if (!why[0] && (nrq_crash_note_dir(dir, 1, SBCT_G_LOC, 100) != 1 || sbct_read(cp, &r) != 1 || r.cls != SBCT_C_KILLED ||
                    r.game != SBCT_G_LOC || r.rt != 2))
        strcpy_s(why, sizeof why, "Exit 1 -> killed");
    DeleteFileW(cp);
    if (!why[0] && (nrq_crash_note_dir(dir, 0, SBCT_G_SB, 100) != 0 || sbct_read(cp, &r) != 0))
        strcpy_s(why, sizeof why, "geordnetes Ende -> nichts");
    /* 3. SB-Start: net_report=1 nur mit Zustimmung + steelnet */
    g_loc_online = 1;
    g_onl_fix_srv = 1;
    set[0] = 0;
    if (!why[0] && (!nrq_sb_set_append(set, sizeof set) || strcmp(set, "net_report=1") != 0))
        strcpy_s(why, sizeof why, "SB-Start mit Zustimmung ohne net_report=1");
    g_nrq_cfg = 0;
    set[0] = 0;
    if (!why[0] && (nrq_sb_set_append(set, sizeof set) || set[0]))
        strcpy_s(why, sizeof why, "SB-Start ohne Zustimmung mit net_report");
    g_nrq_cfg = 1;
    g_loc_online = 0;
    set[0] = 0;
    if (!why[0] && (nrq_sb_set_append(set, sizeof set) || set[0]))
        strcpy_s(why, sizeof why, "SB-Start offline mit net_report");
    /* 4. Text nennt Absturzart und Ort, bleibt "no personal data" */
    if (!why[0] && (!strstr(LT("gui.nrq_text"), "crash type and location") || !strstr(LT("gui.nrq_text"), "no personal data")))
        strcpy_s(why, sizeof why, "Text: crash type and location");
    DeleteFileW(cp);
    RemoveDirectoryA(dir);
    g_nrq_cfg = keep_cfg;
    g_loc_online = keep_on;
    g_onl_fix_srv = keep_fix;
    if (!why[0]) {
        GT_OK("77h W522 CRASH-TELEMETRY (Exitcode -> Datensatz nur mit Zustimmung, SB-Start net_report=1 nur mit Zustimmung "
              "+ steelnet, Text nennt Absturzart und Ort)");
    } else {
        GT_FAIL("77h W522 CRASH-TELEMETRY", "%s", why);
    }
}
