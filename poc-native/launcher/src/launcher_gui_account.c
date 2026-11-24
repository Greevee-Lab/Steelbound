/* launcher_gui_account.c - Pilotenkonto steelnet (W328/W399/W418) + LoC-Testpartner-Einbindung (W336).
 * W462 LAUNCHER-SPLIT: aus launcher_gui.c verschoben (unveraendert), gemeinsamer Zustand
 * in launcher_gui_int.h. */
#include "launcher_gui_int.h"
/* W328 STEELNET-ACCOUNT: Pilotenkonto im Bereich LoC (Logik + Netz in  */
/* launcher_account.c). Karte "Pilot" auf LoC > Erweitert (statt des    */
/* Platzhalters "Online spielen (spaeter)", sobald der Dienst einge-    */
/* richtet ist = Fingerabdruck gesetzt) und eine Verwaltungsansicht auf */
/* derselben Seite ("Konto verwalten" / "Auf diesem PC anmelden").      */
/* Netzaufrufe laufen in einem Thread (WM_APP_ACC), nie im UI-Thread.   */
/* Test-/Shot-Betrieb: kein Netz (Fixture-Zustaende loc_acc_*).         */
/* ------------------------------------------------------------------ */
#define ID_ACC_FIRST ID_ACC_NAME
#define ID_ACC_LAST ID_ACC_V_FORGET
/* W418 LAUNCHER-ACCOUNT-UX: Unterseite Multiplayer > Account ("steelnet account"), ein Zustand, ein naechster
 * Schritt. Kein Konto: Kopf + zwei Karten (Neu | Schon ein Konto). Angemeldet, ungesichert: Kopf (Name, bereit,
 * Warnzeile) + eine Karte "Ein Schritt: sichern". Gesichert bzw. Status unbekannt: kompakte Kopfzeile + "Verwalten"
 * (aufklappbar). Datenschutzzeile klein am Seitenende. Lage je Zustand in acc_layout() bzw. acc_geo(). */
#define ACC_LINK_H 24        /* Aufklapper (Textlink mit Pfeil) */
#define ACC_HALF_W ((CW - 36 - 24) / 2) /* Halbe Breite in einer vollen Karte (ungesichert: Login | Code) */
#define ACC_RX (CX0 + 18 + ACC_HALF_W + 24)
#define ACC_M_LW 300         /* Verwalten: Breite der Textspalte links */
#define ACC_CHECK_MS 700



lacc_store_t g_acc;        /* gespeichertes Konto (K nur im Speicher, nie angezeigt) */
int g_acc_loaded;          /* Ladeversuch gemacht */
lacc_status_t g_acc_st;    /* Status vom Dienst */
int g_acc_st_ok;           /* 1 = Status geladen */
int g_acc_view;            /* 1 = Verwaltungsansicht auf LoC > Erweitert */
int g_acc_busy;            /* Auftrag laeuft */
int g_acc_gen;             /* Auftragszaehler (veraltete Ergebnisse verwerfen) */
int g_acc_fixture;         /* Shots/Selbsttest: Zustand vorgegeben, kein Netz */
int g_acc_fix_srv;         /* Fixture: Dienst gilt als eingerichtet */
static int g_acc_avail = -1;      /* Pruefergebnis des Namens: -1 unbekannt, 0 vergeben, 1 frei */
static char g_acc_avail_name[40];
char g_acc_rec[48];        /* angezeigter Wiederherstellungscode ("" = verdeckt) */
char g_acc_dir_test[1200]; /* Selbsttest: eigener Profilordner */
static volatile long g_acc_cancel;
int g_acc_last_op, g_acc_last_rc; /* Selbsttest: letztes Ergebnis */
int g_acc_try_op;          /* W399 Selbsttest: zuletzt angestossener Auftrag im Testmodus (ohne Netz) */
char g_acc_last_err[64];
/* W418: Aufklapper (nur pro Sitzung gemerkt, nie gespeichert), Fehler der letzten Statusabfrage, Meldung unter
 * einem Feld (Name: frei/vergeben/ungueltig; Code: Format; Login: Fehler) */
int g_acc_x_login, g_acc_x_code, g_acc_x_manage;
char g_acc_st_err[24];
char g_acc_fmsg[200];
int g_acc_fmsg_id;         /* Feld der Meldung (ID_ACC_NAME, ID_ACC_V_CODE, ID_ACC_V_LOGIN), 0 = keine */
COLORREF g_acc_fmsg_col;

static void acc_dir(char *out, size_t cap)
{
    if (g_acc_dir_test[0]) {
        strcpy_s(out, cap, g_acc_dir_test);
        return;
    }
    lacc_default_dir(g_client_dir, out, cap);
}

static int acc_srv_on(void)
{
    lacc_srv_t s;
    if (g_acc_fixture) {
        return g_acc_fix_srv;
    }
    return lacc_server(&s);
}

static void acc_load(void)
{
    char d[1200];
    if (g_acc_loaded || g_acc_fixture) {
        return;
    }
    g_acc_loaded = 1;
    acc_dir(d, sizeof d);
    if (lacc_store_load(d, &g_acc) != 1) {
        memset(&g_acc, 0, sizeof g_acc);
    }
}

/* W472 LOBBIES-AUTH: steelnet > Info braucht das eigene Konto fuer die Lobbyliste (einmal laden, wie acc_load) */
void acc_ensure_loaded(void)
{
    acc_load();
}

/* W403: Multiplayer-Karte (LoC-Spielen) ohne steelnet: nur der Konto-Stand dieses PCs (kein Netz). */
void mp_card_acc_line(char *t, size_t cap)
{
    if (g_o.shot == 0 && !g_in_gui_selftest) { /* Shots/Selbsttest: nie das echte Konto (nur Fixture/leer) */
        acc_load();
    }
    if (g_acc.have && g_acc.name[0]) {
        _snprintf_s(t, cap, _TRUNCATE, LT("gui.mp_card_pilot"), g_acc.name);
    } else {
        strcpy_s(t, cap, LT(acc_srv_on() ? "gui.mp_card_nopilot" : "gui.loc_onl_nosrv"));
    }
}

static int acc_secured(void)
{
    return g_acc_st_ok && (g_acc_st.discord || g_acc_st.login);
}

int acc_state(void)
{
    if (!acc_srv_on()) {
        return ACCS_OFF;
    }
    if (!g_acc.have) {
        return ACCS_NONE;
    }
    if (!g_acc_st_ok) {
        return ACCS_UNK; /* Status laeuft/unbekannt (Server nicht erreichbar, Pin, nicht verbunden) */
    }
    return acc_secured() ? ACCS_SEC : ACCS_UNSEC;
}

/* W418: Lage der Kontoseite je Zustand und Aufklapper (eine Rechnung fuer Steuerelemente und Zeichnen).
 * Alle y logisch; -1 = Zeile/Teil nicht da. */

void acc_geo(acc_geo_t *g)
{
    int y;
    memset(g, 0xff, sizeof *g); /* alles -1 */
    g->st = acc_state();
    g->hh = 88;
    g->ah = 0;
    g->mh = 0;
    switch (g->st) {
    case ACCS_NONE:
        g->hh = 70;
        g->ay = CY0 + g->hh + 12;
        g->lh = 166;
        g->xl = g->ay + 86;
        y = g->xl + ACC_LINK_H + 6;
        if (g_acc_x_login) {
            g->lg = g->xl + 30;
            y = g->xl + 170;
        }
        g->xc = y;
        y = g->xc + ACC_LINK_H;
        if (g_acc_x_code) {
            g->cd = g->xc + 30;
            y = g->xc + 112;
        }
        g->ah = y - g->ay + 14;
        if (g->ah < g->lh) {
            g->ah = g->lh;
        }
        break;
    case ACCS_UNSEC:
        g->hh = 88;
        g->ay = CY0 + g->hh + 12;
        g->xl = g->ay + 96;
        g->rec = g->ay + 96;
        y = g->rec + 78; /* Titel, Code, Knoepfe */
        if (g_acc_x_login) {
            g->lg = g->xl + 30;
            if (g->xl + 192 > y) {
                y = g->xl + 192;
            }
        }
        g->ah = y - g->ay + 12;
        if (g_acc_x_manage) { /* nur Code, Abmelden, Loeschen (Discord/Login/Code stehen in der Karte darueber) */
            g->my = g->ay + g->ah + 12;
            y = g->my + 14;
            g->r_code = y;
            y += 58 + 8;
            g->r_out = y;
            y += 36;
            g->r_del = y;
            y += 36;
            g->mh = y - g->my + 6;
        }
        break;
    case ACCS_SEC:
    case ACCS_UNK:
        g->hh = 64;
        if (g_acc_x_manage) {
            g->my = CY0 + g->hh + 12;
            y = g->my + 14;
            g->r_dc = y;
            y += 48;
            g->r_lg = y;
            y += 48;
            if (g_acc_x_login) {
                g->r_lgx = y;
                g->lg = y;
                y += 72;
            }
            g->r_rec = y;
            y += 48;
            g->r_code = y;
            y += 58 + 8;
            g->r_out = y;
            y += 36;
            g->r_del = y;
            y += 36;
            g->mh = y - g->my + 6;
        }
        break;
    default:
        break;
    }
}

/* W418: Meldung unter einem Feld (Name/Code/Login); "" = keine */
static void acc_fmsg(int id, const char *t, COLORREF c)
{
    g_acc_fmsg_id = t && *t ? id : 0;
    strncpy_s(g_acc_fmsg, sizeof g_acc_fmsg, t ? t : "", _TRUNCATE);
    g_acc_fmsg_col = c;
}

static const char *acc_fmsg_for(int id, COLORREF *c)
{
    if (g_acc_fmsg_id != id || !g_acc_fmsg[0]) {
        return NULL;
    }
    *c = g_acc_fmsg_col;
    return g_acc_fmsg;
}

/* W418: Aufklapper zu (neuer Kontozustand: Anlegen, Anmelden, Abmelden, Loeschen) */
void acc_x_reset(void)
{
    g_acc_x_login = g_acc_x_code = g_acc_x_manage = 0;
    acc_fmsg(0, "", 0);
}

/* W418: ein Codefeld fuer beide Codes. Das Format entscheidet: Wiederherstellungscode = 32 Zeichen Base32
 * (lacc_recovery_parse), Uebertragungscode des Admins = 12 Zeichen aus ABCDEFGHJKLMNPQRSTUVWXYZ23456789
 * (PROTOKOLL W328; "-", Leerzeichen, Kleinbuchstaben toleriert). 1 = Wiederherstellung, 2 = Uebertragung, 0 = keins. */
int acc_code_kind(const char *s)
{
    static const char kTr[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    char c[64];
    size_t i, n = 0;
    uint32_t acct;
    uint8_t k16[16];
    if (!s) {
        return 0;
    }
    if (lacc_recovery_parse(s, &acct, k16)) {
        SecureZeroMemory(k16, sizeof k16);
        return 1;
    }
    for (i = 0; s[i]; i++) {
        char ch = s[i];
        if (ch == '-' || ch == ' ') {
            continue;
        }
        if (ch >= 'a' && ch <= 'z') {
            ch = (char)(ch - 'a' + 'A');
        }
        if (!strchr(kTr, ch) || ch == '\0' || n + 1 >= sizeof c) {
            return 0;
        }
        c[n++] = ch;
    }
    return n == 12 ? 2 : 0;
}

/* Fehlercode -> Text (aktive Sprache) */
static void acc_err_text(const char *code, char *out, size_t cap)
{
    static const struct {
        const char *code, *key;
    } kErr[] = {
        { "net", "gui.acc_err_net" },           { "pin", "gui.acc_err_pin" },
        { "off", "gui.acc_err_off" },           { "accounts_off", "gui.acc_err_off" },
        { "invalid_name", "gui.acc_err_name" }, { "name_taken", "gui.acc_err_taken" },
        { "invalid_login", "gui.acc_err_login" }, { "login_taken", "gui.acc_err_login_taken" },
        { "weak_password", "gui.acc_err_pw" },  { "login_failed", "gui.acc_err_login_failed" },
        { "code_invalid", "gui.acc_err_code" }, { "not_found", "gui.acc_err_not_found" },
        { "discord_in_use", "gui.acc_err_discord_in_use" }, { "discord_failed", "gui.acc_err_discord" },
        { "discord_off", "gui.acc_err_discord_off" }, { "denied", "gui.acc_err_denied" },
        { "state", "gui.acc_err_denied" },      { "timeout", "gui.acc_err_timeout" },
        { "cancel", "gui.acc_err_cancel" },     { "port_busy", "gui.acc_err_port" },
        { "rate_limited", "gui.acc_err_rate" }, { "auth_failed", "gui.acc_err_auth" },
        { "no_account", "gui.acc_err_auth" },   { "server_busy", "gui.acc_err_busy" },
        { "store", "gui.acc_err_store" },       { "browser", "gui.acc_err_browser" },
    };
    int i;
    for (i = 0; i < (int)(sizeof kErr / sizeof kErr[0]); i++) {
        if (strcmp(code, kErr[i].code) == 0) {
            strncpy_s(out, cap, LT(kErr[i].key), _TRUNCATE);
            return;
        }
    }
    _snprintf_s(out, cap, _TRUNCATE, LT("gui.acc_err_other"), code);
}

void acc_refresh(void)
{
    if (!g_hwnd) {
        return;
    }
    acc_layout(); /* W399/W418: Lage je Zustand und Aufklapper */
    ui_refresh_visibility();
    nav_sub_layout(); /* W399: Markierung des Unterpunkts */
    {
        static const int kBtn[] = { ID_ACC_CREATE, ID_ACC_SIGNIN, ID_ACC_DISCORD, ID_ACC_MANAGE,
                                    ID_ACC_V_DISCORD, ID_ACC_V_LOGINBTN, ID_ACC_V_CODEBTN, ID_ACC_X_LOGIN,
                                    ID_ACC_X_CODE, ID_ACC_V_DELETE, ID_ACC_V_FORGET };
        int i;
        for (i = 0; i < (int)(sizeof kBtn / sizeof kBtn[0]); i++) {
            HWND b = ctl(kBtn[i]);
            BOOL en = !g_acc_busy;
            if ((kBtn[i] == ID_ACC_DISCORD || kBtn[i] == ID_ACC_V_DISCORD) && g_acc.have && g_acc_st_ok &&
                g_acc_st.discord) {
                en = FALSE; /* schon verbunden */
            }
            if (b) {
                EnableWindow(b, en);
            }
        }
    }
    {
        int st = acc_state(), lg = g_acc_st_ok && g_acc_st.login;
        if (ctl(ID_ACC_DISCORD)) {
            ui_settext(ctl(ID_ACC_DISCORD),
                       LT(g_acc_st_ok && g_acc_st.discord ? "gui.acc_discord_done" : "gui.acc_discord_link"));
        }
        if (ctl(ID_ACC_V_DISCORD)) { /* kein Konto: anmelden; ungesichert: sichern (Hauptknopf); sonst verbinden */
            ui_settext(ctl(ID_ACC_V_DISCORD), LT(st == ACCS_NONE    ? "gui.acc_discord_login"
                                                 : st == ACCS_UNSEC ? "gui.acc_sec_dc_btn"
                                                                    : "gui.acc_discord_link"));
        }
        if (ctl(ID_ACC_V_LOGINBTN)) {
            ui_settext(ctl(ID_ACC_V_LOGINBTN), LT(st == ACCS_NONE    ? "gui.acc_login_go"
                                                  : st == ACCS_UNSEC ? "gui.acc_login_save"
                                                                     : "gui.acc_save"));
        }
        if (ctl(ID_ACC_X_LOGIN)) {
            ui_settext(ctl(ID_ACC_X_LOGIN), LT(st == ACCS_NONE ? "gui.acc_x_login_in"
                                               : lg            ? "gui.acc_login_change"
                                                               : "gui.acc_x_login_set"));
        }
        if (ctl(ID_LOC_MP_ACC)) { /* W418: Karte Multiplayer (Spielen) */
            ui_settext(ctl(ID_LOC_MP_ACC), LT(g_acc.have ? "gui.acc_mp_go" : "gui.acc_mp_new"));
        }
    }
    redraw_all();
}

static DWORD WINAPI acc_thread(LPVOID p)
{
    acc_job_t *j = (acc_job_t *)p;
    char *e = j->err;
    size_t ec = sizeof j->err;
    switch (j->op) {
    case ACC_OP_STATUS: j->rc = lacc_op_status(&j->srv, &j->st, &j->status, e, ec); break;
    case ACC_OP_CHECK: j->rc = lacc_op_check(&j->srv, j->a, &j->avail, e, ec); break;
    case ACC_OP_CREATE: j->rc = lacc_op_create(&j->srv, j->dir, j->a, &j->st, e, ec); break;
    case ACC_OP_DISCORD_LINK: j->rc = lacc_op_discord(&j->srv, j->dir, 0, &j->st, 180000, &g_acc_cancel, e, ec); break;
    case ACC_OP_DISCORD_LOGIN: j->rc = lacc_op_discord(&j->srv, j->dir, 1, &j->st, 180000, &g_acc_cancel, e, ec); break;
    case ACC_OP_LOGIN_SET: j->rc = lacc_op_login_set(&j->srv, &j->st, j->a, j->b, e, ec); break;
    case ACC_OP_LOGIN: j->rc = lacc_op_login(&j->srv, j->dir, j->a, j->b, &j->st, e, ec); break;
    case ACC_OP_TRANSFER: j->rc = lacc_op_transfer(&j->srv, j->dir, j->a, &j->st, e, ec); break;
    case ACC_OP_RECOVERY: j->rc = lacc_op_recovery(&j->srv, j->dir, j->a, &j->st, e, ec); break;
    case ACC_OP_DELETE: j->rc = lacc_op_delete(&j->srv, j->dir, &j->st, e, ec); break;
    default: j->rc = 1; strcpy_s(e, ec, "bad"); break;
    }
    SecureZeroMemory(j->b, sizeof j->b);
    if (!PostMessageW(j->h, WM_APP_ACC, (WPARAM)j->gen, (LPARAM)j)) {
        SecureZeroMemory(j, sizeof *j);
        free(j);
    }
    return 0;
}

/* Auftrag starten (a/b duerfen NULL sein). 0 = gestartet. */
static int acc_start(int op, const char *a, const char *b)
{
    acc_job_t *j;
    HANDLE th;
    if (g_acc_busy || !g_hwnd) {
        return 1;
    }
    if (g_acc_fixture || (g_o.shot != 0 && !g_acc_dir_test[0])) { /* Shots: nie Netz */
        g_acc_try_op = op; /* W399: Selbsttest prueft die Verdrahtung der Knoepfe */
        printf("[acc] Auftrag %d (Testmodus, kein Netz)\n", op);
        return 1;
    }
    j = (acc_job_t *)calloc(1, sizeof *j);
    if (!j) {
        return 1;
    }
    j->h = g_hwnd;
    j->op = op;
    j->gen = ++g_acc_gen;
    if (a) strcpy_s(j->a, sizeof j->a, a);
    if (b) strcpy_s(j->b, sizeof j->b, b);
    acc_dir(j->dir, sizeof j->dir);
    lacc_server(&j->srv);
    j->st = g_acc;
    g_acc_cancel = 0;
    th = CreateThread(NULL, 0, acc_thread, j, 0, NULL);
    if (!th) {
        SecureZeroMemory(j, sizeof *j);
        free(j);
        return 1;
    }
    CloseHandle(th);
    g_acc_busy = op;
    if (op != ACC_OP_STATUS && op != ACC_OP_CHECK) { /* W418: Namenspruefung still (Meldung unter dem Feld) */
        set_status_k(LT(op == ACC_OP_DISCORD_LINK || op == ACC_OP_DISCORD_LOGIN ? "gui.acc_wait_discord"
                                                                                  : "gui.acc_working"), 0);
    }
    acc_refresh();
    return 0;
}

/* Ergebnis (UI-Thread) */
void acc_done(acc_job_t *j)
{
    char m[400], e[300];
    int op = j->op;
    g_acc_busy = 0;
    g_acc_last_op = op;
    g_acc_last_rc = j->rc;
    strcpy_s(g_acc_last_err, sizeof g_acc_last_err, j->rc ? j->err : "");
    if (j->rc != 0) {
        acc_err_text(j->err, e, sizeof e);
        if (op == ACC_OP_STATUS) {
            strcpy_s(g_acc_st_err, sizeof g_acc_st_err, j->err); /* W418: Kopfzeile (Server nicht erreichbar, Pin) */
            if (strcmp(j->err, "auth_failed") == 0) { /* Konto geloescht/abgemeldet (neue Generation) */
                set_status_k(LT("gui.acc_err_auth"), 2);
            }
        } else if (op == ACC_OP_CHECK) {
            acc_fmsg(0, "", 0); /* W418: stille Pruefung - ohne Antwort keine Meldung (Erstellen prueft erneut) */
        } else {
            set_status_k(e, 2);
            /* W418: Fehler direkt unter dem betroffenen Feld */
            if (op == ACC_OP_CREATE) acc_fmsg(ID_ACC_NAME, e, C_ERR);
            else if (op == ACC_OP_TRANSFER || op == ACC_OP_RECOVERY) acc_fmsg(ID_ACC_V_CODE, e, C_ERR);
            else if (op == ACC_OP_LOGIN || op == ACC_OP_LOGIN_SET) acc_fmsg(ID_ACC_V_LOGIN, e, C_ERR);
        }
        acc_refresh();
        return;
    }
    switch (op) {
    case ACC_OP_STATUS:
        g_acc_st = j->status;
        g_acc_st_ok = 1;
        g_acc_st_err[0] = '\0';
        break;
    case ACC_OP_CHECK: { /* W418: Ergebnis unter dem Feld, nur wenn der Name noch im Feld steht */
        char cur[80] = "";
        if (ctl(ID_ACC_NAME)) ui_gettext(ctl(ID_ACC_NAME), cur, sizeof cur);
        g_acc_avail = j->avail;
        strcpy_s(g_acc_avail_name, sizeof g_acc_avail_name, j->a);
        if (strcmp(cur, j->a) == 0) {
            _snprintf_s(m, sizeof m, _TRUNCATE, LT(j->avail ? "gui.acc_name_free" : "gui.acc_name_taken"), j->a);
            acc_fmsg(ID_ACC_NAME, m, j->avail ? C_OK : C_ERR);
        }
        break;
    }
    case ACC_OP_CREATE:
    case ACC_OP_DISCORD_LOGIN:
    case ACC_OP_LOGIN:
    case ACC_OP_TRANSFER:
    case ACC_OP_RECOVERY:
        g_acc = j->st;
        g_acc_st_ok = 0;
        g_acc_rec[0] = '\0';
        g_acc_avail = -1;
        g_acc_st_err[0] = '\0';
        acc_x_reset(); /* W418: neuer Zustand -> Aufklapper zu */
        _snprintf_s(m, sizeof m, _TRUNCATE, LT(op == ACC_OP_CREATE ? "gui.acc_created" : "gui.acc_signed_in"), g_acc.name);
        set_status_k(m, 1);
        if (ctl(ID_ACC_V_PASS)) ui_settext(ctl(ID_ACC_V_PASS), "");
        if (ctl(ID_ACC_V_CODE)) ui_settext(ctl(ID_ACC_V_CODE), "");
        if (op != ACC_OP_CREATE) g_acc_view = 0; /* zurueck zur Karte */
        break;
    case ACC_OP_DISCORD_LINK:
        g_acc_st.discord = 1;
        set_status_k(LT("gui.acc_discord_ok"), 1);
        break;
    case ACC_OP_LOGIN_SET:
        g_acc_st.login = 1;
        strcpy_s(g_acc_st.login_name, sizeof g_acc_st.login_name, j->a);
        if (ctl(ID_ACC_V_PASS)) ui_settext(ctl(ID_ACC_V_PASS), "");
        g_acc_x_login = 0; /* W418: Felder zu, Zeile zeigt den Login */
        acc_fmsg(0, "", 0);
        set_status_k(LT("gui.acc_login_ok"), 1);
        break;
    case ACC_OP_DELETE:
        memset(&g_acc, 0, sizeof g_acc);
        memset(&g_acc_st, 0, sizeof g_acc_st);
        g_acc_st_ok = 0;
        g_acc_rec[0] = '\0';
        g_acc_view = 0;
        acc_x_reset(); /* W418 */
        set_status_k(LT("gui.acc_deleted"), 1);
        break;
    }
    acc_refresh();
    /* nach einer Anmeldung gleich den Status holen (gesichert?) */
    if (g_acc.have && !g_acc_st_ok && op != ACC_OP_STATUS) {
        acc_start(ACC_OP_STATUS, NULL, NULL);
    }
}

/* Seite LoC > Erweitert gezeigt: Konto laden, Status einmal holen. */
void acc_page_shown(void)
{
    acc_load();
    /* W397: automatische Statusabfrage nur mit Stufe "Mit steelnet verbinden" (Knoepfe bleiben Nutzeraktionen) */
    if (g_acc.have && !g_acc_st_ok && !g_acc_busy && acc_srv_on() && netm_get() == NETM_STEEL && g_o.shot == 0) {
        acc_start(ACC_OP_STATUS, NULL, NULL);
    }
}

/* W418: LoC > Spielen gezeigt: Knopftext der Karte Multiplayer zum Konto dieses PCs (Shots/Selbsttest: Fixture) */
void acc_play_shown(void)
{
    if (g_o.shot == 0 && !g_in_gui_selftest) {
        acc_load();
    }
    if (ctl(ID_LOC_MP_ACC)) {
        ui_settext(ctl(ID_LOC_MP_ACC), LT(g_acc.have ? "gui.acc_mp_go" : "gui.acc_mp_new"));
    }
}

/* W418: Aufklapper (Textlink, Pfeil rechts = zu, unten = offen) und roter Knopf "Konto loeschen" */
int acc_link_id(int id)
{
    return id == ID_ACC_X_LOGIN || id == ID_ACC_X_CODE || id == ID_ACC_X_MANAGE;
}

int acc_danger_id(int id)
{
    return id == ID_ACC_V_DELETE;
}

void acc_link_paint(canvas_t *c, const uictl_t *u, int w, int h, const wchar_t *txt, int n, int focus, int dis)
{
    int open = u->id == ID_ACC_X_LOGIN ? g_acc_x_login : u->id == ID_ACC_X_CODE ? g_acc_x_code : g_acc_x_manage;
    COLORREF fg = dis ? C_FAINT : u->hover ? C_ACCENT_HI : C_ACCENT;
    wchar_t ic = g_has_icons ? (open ? 0xE70D : 0xE76C) : (open ? L'v' : L'>');
    int iw = SX(16), tw = 0;
    SIZE sz;
    cv_textw(c, g_has_icons ? g_f[F_ICONSM] : g_f[F_SEMI], &ic, 1, 0, 0, iw, h, fg,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(c->dc, g_f[F_SEMI]);
    if (GetTextExtentPoint32W(c->dc, txt, n, &sz)) {
        tw = sz.cx;
    }
    cv_textw(c, g_f[F_SEMI], txt, n, iw + SX(4), 0, w - iw - SX(4), h, fg,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (u->hover && !dis) {
        int uw = tw < w - iw - SX(4) ? tw : w - iw - SX(4);
        cv_fill(c, iw + SX(4), h - SX(4), uw, 1, fg, 200, 0);
    }
    if (focus) {
        cv_ring(c, 0, 0, w, h, C_ACCENT_HI, 200, SX(4), SX(2) < 2 ? 2 : SX(2));
    }
}

/* Sichtbarkeit (-1 = nicht zustaendig) */
int acc_visible(int id, int page)
{
    int on, have, view, st, man;
    if (id < ID_ACC_FIRST || id > ID_ACC_LAST) {
        return -1;
    }
    if (id == ID_LOC_MP_ACC) { /* W418: Karte Multiplayer auf LoC > Spielen (ohne steelnet: Konto-Zeile + Knopf) */
        return g_game == LGAME_LOC && page == P_PLAY && !g_upd_ask && netm_get() != NETM_STEEL && acc_srv_on();
    }
    if (g_game != LGAME_LOC || page != P_LNET) { /* W397: Multiplayer statt Erweitert */
        return 0;
    }
    on = acc_srv_on();
    have = g_acc.have;
    view = g_acc_view && on;
    if (!view) { /* Karte auf Match & network */
        switch (id) {
        case ID_ACC_NAME: case ID_ACC_CREATE: case ID_ACC_SIGNIN:
            return on && !have;
        case ID_ACC_DISCORD: /* W473: nur ungesichert (Hauptaktion); gesichert/unbekannt nur "Manage account" */
            return on && have && acc_state() == ACCS_UNSEC;
        case ID_ACC_MANAGE:
            return on && have;
        }
        return 0;
    }
    /* W418: Kontoseite - nur die Elemente des Zustands und der offenen Aufklapper */
    st = acc_state();
    man = g_acc_x_manage;
    switch (id) {
    case ID_ACC_NAME: case ID_ACC_CREATE: case ID_ACC_X_CODE:
        return st == ACCS_NONE;
    case ID_ACC_V_DISCORD: /* verbunden -> nur Zustand, kein Knopf */
        return st == ACCS_NONE || st == ACCS_UNSEC || (man && !(g_acc_st_ok && g_acc_st.discord));
    case ID_ACC_X_LOGIN:
        return st == ACCS_NONE || st == ACCS_UNSEC || man;
    case ID_ACC_V_LOGIN: case ID_ACC_V_PASS: case ID_ACC_V_LOGINBTN:
        return g_acc_x_login && (st == ACCS_NONE || st == ACCS_UNSEC || man);
    case ID_ACC_V_CODE: case ID_ACC_V_CODEBTN:
        return st == ACCS_NONE ? g_acc_x_code : man;
    case ID_ACC_X_MANAGE:
        return have;
    case ID_ACC_V_RECSHOW: case ID_ACC_V_RECCOPY:
        return st == ACCS_UNSEC || (have && man);
    case ID_ACC_V_DELETE: case ID_ACC_V_FORGET:
        return have && man;
    }
    return 0;
}

/* In der Verwaltungsansicht: alle uebrigen Elemente der Seite aus. W399: Unterpunkt Account der Leiste, auch ohne
 * eingerichteten Dienst (dann nur der Hinweis). */
int acc_view_active(void)
{
    return g_game == LGAME_LOC && g_acc_view;
}

int acc_sub_active(void)
{
    return acc_view_active();
}

/* W418: Element an Lage und Groesse setzen (logisch; Edits innen wie beim Aufbau, ui_place) */
static void acc_put(int id, int x, int y, int w, int h)
{
    uictl_t *u = ui_find(id);
    if (!u || (u->x == x && u->y == y && u->w == w && u->h_ == h)) {
        return;
    }
    u->x = x;
    u->y = y;
    u->w = w;
    u->h_ = h;
    ui_place(u);
}

/* W473: Zeilen der Karte "steelnet account" (Match & network), logisch ab LOC_ONL_Y */
#define ACC_C_L1 40  /* Zeile 1: angemeldet als / nicht angemeldet */
#define ACC_C_L2 62  /* Zeile 2: gesichert ja/nein */
#define ACC_C_WHY 86 /* Erklaerung (bis 3 kurze Zeilen) */
#define ACC_C_WHY_H 48
#define ACC_C_BTN 140 /* Knoepfe */
/* W399/W418: Lage der Konto-Elemente je Ansicht (Karte auf Match & network bzw. Kontoseite), Zustand und
 * Aufklapper (acc_geo). Unsichtbare Elemente behalten ihre letzte Lage. */
void acc_layout(void)
{
    acc_geo_t g;
    uictl_t *dc = ui_find(ID_ACC_V_DISCORD);
    int x0 = CX0 + 18, xr = CX0 + CW - 18, rx = COL2 + 18, rw = COLW - 36;
    if (!g_acc_view) { /* Karte "steelnet account" auf Match & network (W473: Zustand oben, Knoepfe unten) */
        int y = LOC_ONL_Y, xc = ACC_X + 18, unsec = acc_state() == ACCS_UNSEC;
        acc_put(ID_ACC_NAME, xc, y + 86, COLW - 36 - 8 - 130, 28);
        acc_put(ID_ACC_CREATE, ACC_X + COLW - 18 - 130, y + 86, 130, 28);
        acc_put(ID_ACC_SIGNIN, xc, y + 122, COLW - 36, 26);
        acc_put(ID_ACC_DISCORD, xc, y + ACC_C_BTN, 168, 28);
        acc_put(ID_ACC_MANAGE, unsec ? xc + 174 : xc, y + ACC_C_BTN, unsec ? COLW - 36 - 174 : 180, 28);
        return;
    }
    acc_geo(&g);
    acc_put(ID_ACC_X_MANAGE, xr - 120, CY0 + 12, 120, ACC_LINK_H);
    if (dc) {
        dc->kind = g.st == ACCS_UNSEC ? K_ACCENT : K_BTN; /* ein Hauptknopf je Zustand */
    }
    switch (g.st) {
    case ACCS_NONE:
        acc_put(ID_ACC_NAME, x0, g.ay + 62, COLW - 36, 28);
        acc_put(ID_ACC_CREATE, x0, g.ay + 120, 180, 30);
        acc_put(ID_ACC_V_DISCORD, rx, g.ay + 44, rw, 30);
        acc_put(ID_ACC_X_LOGIN, rx, g.xl, rw, ACC_LINK_H);
        acc_put(ID_ACC_X_CODE, rx, g.xc, rw, ACC_LINK_H);
        if (g.lg >= 0) {
            acc_put(ID_ACC_V_LOGIN, rx, g.lg + 18, rw, 28);
            acc_put(ID_ACC_V_PASS, rx, g.lg + 68, rw, 28);
            acc_put(ID_ACC_V_LOGINBTN, rx, g.lg + 102, 160, 28);
        }
        if (g.cd >= 0) {
            acc_put(ID_ACC_V_CODE, rx, g.cd + 18, rw - 104, 28);
            acc_put(ID_ACC_V_CODEBTN, rx + rw - 96, g.cd + 18, 96, 28);
        }
        break;
    case ACCS_UNSEC:
        acc_put(ID_ACC_V_DISCORD, x0, g.ay + 44, 240, 32);
        acc_put(ID_ACC_X_LOGIN, x0, g.xl, ACC_HALF_W, ACC_LINK_H);
        acc_put(ID_ACC_V_RECSHOW, ACC_RX, g.rec + 48, 76, 26);
        acc_put(ID_ACC_V_RECCOPY, ACC_RX + 82, g.rec + 48, 76, 26);
        if (g.lg >= 0) {
            acc_put(ID_ACC_V_LOGIN, x0, g.lg + 18, ACC_HALF_W, 28);
            acc_put(ID_ACC_V_PASS, x0, g.lg + 68, ACC_HALF_W, 28);
            acc_put(ID_ACC_V_LOGINBTN, x0, g.lg + 102, 180, 28);
        }
        break;
    case ACCS_SEC:
    case ACCS_UNK:
        if (g.r_dc >= 0) {
            acc_put(ID_ACC_V_DISCORD, xr - 210, g.r_dc + 6, 210, 28);
            acc_put(ID_ACC_X_LOGIN, xr - 210, g.r_lg + 8, 210, ACC_LINK_H);
            acc_put(ID_ACC_V_RECSHOW, xr - 210, g.r_rec + 4, 102, 26);
            acc_put(ID_ACC_V_RECCOPY, xr - 102, g.r_rec + 4, 102, 26);
        }
        if (g.lg >= 0) {
            acc_put(ID_ACC_V_LOGIN, x0, g.lg + 18, 214, 28);
            acc_put(ID_ACC_V_PASS, x0 + 226, g.lg + 18, 214, 28);
            acc_put(ID_ACC_V_LOGINBTN, x0 + 452, g.lg + 18, xr - x0 - 452, 28);
        }
        break;
    default:
        break;
    }
    if (g.r_code >= 0) { /* Verwalten (beide Zustaende mit Konto): Code, Abmelden, Loeschen */
        acc_put(ID_ACC_V_CODE, xr - 96 - 8 - 200, g.r_code + 4, 200, 28);
        acc_put(ID_ACC_V_CODEBTN, xr - 96, g.r_code + 4, 96, 28);
        acc_put(ID_ACC_V_FORGET, x0, g.r_out + 4, 210, 28);
        acc_put(ID_ACC_V_DELETE, x0, g.r_del + 4, 210, 28);
    }
}

static void acc_open_view(int on)
{
    g_acc_view = on;
    acc_layout(); /* W399 */
    g_acc_rec[0] = '\0';
    if (!on) {
        g_acc_cancel = 1; /* laufendes Discord-Warten abbrechen */
        if (ctl(ID_ACC_V_PASS)) ui_settext(ctl(ID_ACC_V_PASS), "");
    } else if (g_acc.have && g_acc_st_ok && g_acc_st.login && ctl(ID_ACC_V_LOGIN)) {
        ui_settext(ctl(ID_ACC_V_LOGIN), g_acc_st.login_name);
    }
    acc_refresh();
}

/* W399: Unterpunkt von Multiplayer waehlen (Leiste, "Account verwalten", "Auf diesem PC anmelden"). */
void acc_nav_sub(int acc)
{
    if ((g_acc_view != 0) != (acc != 0)) {
        acc_open_view(acc);
    }
    show_page(P_LNET);
}

static void acc_copy_code(void)
{
    char c[48];
    wchar_t w[48];
    HGLOBAL hg;
    if (!g_acc.have) {
        return;
    }
    lacc_recovery_code(g_acc.account, g_acc.key, c, sizeof c);
    if (g_o.shot != 0) { /* Test: Zwischenablage des Nutzers nie anfassen */
        printf("[clipboard] Wiederherstellungscode (%u Zeichen)\n", (unsigned)strlen(c));
        SecureZeroMemory(c, sizeof c);
        return;
    }
    u8_to_w(c, w, 48);
    SecureZeroMemory(c, sizeof c);
    if (OpenClipboard(g_hwnd)) {
        EmptyClipboard();
        hg = GlobalAlloc(GMEM_MOVEABLE, (wcslen(w) + 1) * sizeof(wchar_t));
        if (hg) {
            memcpy(GlobalLock(hg), w, (wcslen(w) + 1) * sizeof(wchar_t));
            GlobalUnlock(hg);
            if (!SetClipboardData(CF_UNICODETEXT, hg)) GlobalFree(hg);
        }
        CloseClipboard();
        set_status_k(LT("gui.acc_rec_copied"), 1);
    }
    SecureZeroMemory(w, sizeof w);
}

static int acc_command(int id, int code)
{
    char a[80], b[300], q[400];
    if (id < ID_ACC_FIRST || id > ID_ACC_LAST) {
        return 0;
    }
    if (id == ID_ACC_NAME || id == ID_ACC_V_LOGIN || id == ID_ACC_V_PASS || id == ID_ACC_V_CODE) {
        if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
            g_focus_id = code == EN_SETFOCUS ? id : 0;
            redraw_all();
        } else if (code == EN_CHANGE && id == ID_ACC_NAME) {
            /* W418: kein Knopf "Name pruefen" mehr - Pruefung entprellt nach dem Tippen (acc_name_tick) */
            g_acc_avail = -1;
            if (g_acc_fmsg_id == ID_ACC_NAME) {
                acc_fmsg(0, "", 0);
                redraw_all();
            }
            if (g_hwnd) {
                SetTimer(g_hwnd, ACC_TIMER, ACC_CHECK_MS, NULL);
            }
        } else if (code == EN_CHANGE && id == ID_ACC_V_CODE && g_acc_fmsg_id == ID_ACC_V_CODE) {
            acc_fmsg(0, "", 0);
            redraw_all();
        }
        return 1;
    }
    if (code != BN_CLICKED) {
        return 1;
    }
    switch (id) {
    case ID_ACC_CREATE:
        ui_gettext(ctl(ID_ACC_NAME), a, sizeof a);
        if (g_hwnd) KillTimer(g_hwnd, ACC_TIMER); /* Erstellen prueft den Namen selbst (Server) */
        if (!lacc_valid_name(a)) {
            set_status_k(LT("gui.acc_err_name"), 2);
            acc_fmsg(ID_ACC_NAME, LT("gui.acc_err_name"), C_ERR); /* W418: Meldung unter dem Feld */
            redraw_all();
            return 1;
        }
        acc_start(ACC_OP_CREATE, a, NULL);
        return 1;
    case ID_ACC_SIGNIN:
    case ID_ACC_MANAGE:
    case ID_LOC_MP_ACC: /* W418: Karte Multiplayer auf Spielen */
        acc_nav_sub(1); /* W399: Unterpunkt Account */
        return 1;
    case ID_ACC_X_LOGIN: /* W418: Aufklapper (pro Sitzung) */
        g_acc_x_login = !g_acc_x_login;
        if (g_acc_x_login) {
            int st = acc_state();
            if (st == ACCS_NONE) g_acc_x_code = 0;          /* kein Konto: Login oder Code */
            if (st == ACCS_UNSEC) g_acc_x_manage = 0;       /* ungesichert: Sichern oder Verwalten */
            if (g_acc_st_ok && g_acc_st.login && ctl(ID_ACC_V_LOGIN)) ui_settext(ctl(ID_ACC_V_LOGIN), g_acc_st.login_name);
        } else if (ctl(ID_ACC_V_PASS)) {
            ui_settext(ctl(ID_ACC_V_PASS), "");
        }
        if (g_acc_fmsg_id == ID_ACC_V_LOGIN) acc_fmsg(0, "", 0);
        acc_refresh();
        return 1;
    case ID_ACC_X_CODE:
        g_acc_x_code = !g_acc_x_code;
        if (g_acc_x_code) g_acc_x_login = 0;
        if (g_acc_fmsg_id == ID_ACC_V_CODE) acc_fmsg(0, "", 0);
        acc_refresh();
        return 1;
    case ID_ACC_X_MANAGE:
        g_acc_x_manage = !g_acc_x_manage;
        if (g_acc_x_manage && acc_state() == ACCS_UNSEC) g_acc_x_login = 0;
        if (!g_acc_x_manage && acc_state() != ACCS_UNSEC) g_acc_x_login = 0; /* Login-Felder liegen in Verwalten */
        acc_refresh();
        return 1;
    case ID_ACC_DISCORD:
    case ID_ACC_V_DISCORD:
        acc_start(g_acc.have ? ACC_OP_DISCORD_LINK : ACC_OP_DISCORD_LOGIN, NULL, NULL);
        return 1;
    case ID_ACC_V_LOGINBTN:
        ui_gettext(ctl(ID_ACC_V_LOGIN), a, sizeof a);
        ui_gettext(ctl(ID_ACC_V_PASS), b, sizeof b);
        if (!g_acc.have) {
            acc_start(ACC_OP_LOGIN, a, b);
        } else if (!lacc_valid_login(a)) {
            set_status_k(LT("gui.acc_err_login"), 2);
            acc_fmsg(ID_ACC_V_LOGIN, LT("gui.acc_err_login"), C_ERR); /* W418 */
            redraw_all();
        } else if (!lacc_valid_password(b)) {
            set_status_k(LT("gui.acc_err_pw"), 2);
            acc_fmsg(ID_ACC_V_LOGIN, LT("gui.acc_err_pw"), C_ERR);
            redraw_all();
        } else {
            acc_start(ACC_OP_LOGIN_SET, a, b);
        }
        SecureZeroMemory(b, sizeof b);
        return 1;
    case ID_ACC_V_CODEBTN: { /* W418: ein Feld fuer beide Codes, das Format entscheidet */
        int kind;
        ui_gettext(ctl(ID_ACC_V_CODE), a, sizeof a);
        kind = acc_code_kind(a);
        if (!kind) {
            set_status_k(LT("gui.acc_err_code_fmt"), 2);
            acc_fmsg(ID_ACC_V_CODE, LT("gui.acc_err_code_fmt"), C_ERR);
            redraw_all();
            SecureZeroMemory(a, sizeof a);
            return 1;
        }
        if (g_acc.have) { /* ersetzt das Konto auf diesem PC (bestehende Rueckfrage) */
            _snprintf_s(q, sizeof q, _TRUNCATE, LT("gui.acc_transfer_q"), g_acc.name);
            if (!ui_confirm(q, LT("gui.acc_transfer_title"))) {
                SecureZeroMemory(a, sizeof a);
                return 1;
            }
        }
        acc_start(kind == 1 ? ACC_OP_RECOVERY : ACC_OP_TRANSFER, a, NULL);
        SecureZeroMemory(a, sizeof a);
        return 1;
    }
    case ID_ACC_V_RECSHOW:
        if (g_acc_rec[0]) {
            SecureZeroMemory(g_acc_rec, sizeof g_acc_rec);
        } else if (g_acc.have) {
            lacc_recovery_code(g_acc.account, g_acc.key, g_acc_rec, sizeof g_acc_rec);
        }
        ui_settext(ctl(ID_ACC_V_RECSHOW), LT(g_acc_rec[0] ? "gui.acc_rec_hide" : "gui.acc_rec_show"));
        redraw_all();
        return 1;
    case ID_ACC_V_RECCOPY:
        acc_copy_code();
        return 1;
    case ID_ACC_V_DELETE:
        _snprintf_s(q, sizeof q, _TRUNCATE, LT("gui.acc_delete_q"), g_acc.name);
        if (ui_confirm(q, LT("gui.acc_delete_title"))) {
            acc_start(ACC_OP_DELETE, NULL, NULL);
        }
        return 1;
    case ID_ACC_V_FORGET:
        if (ui_confirm(LT(acc_secured() ? "gui.acc_forget_q" : "gui.acc_forget_q_unsec"), LT("gui.acc_forget"))) {
            char d[1200];
            acc_dir(d, sizeof d);
            if (!g_acc_fixture) lacc_store_forget(d);
            SecureZeroMemory(&g_acc, sizeof g_acc);
            g_acc_st_ok = 0;
            g_acc_view = 0;
            acc_x_reset(); /* W418 */
            set_status_k(LT("gui.acc_forgotten"), 1);
            acc_refresh();
        }
        return 1;
    }
    return 1;
}

/* W418: entprellte Namenspruefung (vorhandene Abfrage check, still). Ungueltige Namen meldet die Seite ohne Netz;
 * die Abfrage laeuft nur mit Stufe "Mit steelnet verbinden" (wie die Statusabfrage, W397). */
void acc_name_tick(void)
{
    char a[80] = "";
    if (g_hwnd) {
        KillTimer(g_hwnd, ACC_TIMER);
    }
    if (g_acc.have || !acc_srv_on() || !ctl(ID_ACC_NAME)) {
        return;
    }
    ui_gettext(ctl(ID_ACC_NAME), a, sizeof a);
    if (!a[0]) {
        return;
    }
    if (!lacc_valid_name(a)) {
        acc_fmsg(ID_ACC_NAME, LT("gui.acc_err_name"), C_ERR);
        redraw_all();
        return;
    }
    if (netm_get() != NETM_STEEL && !g_acc_fixture) {
        return; /* ohne steelnet: Erstellen prueft */
    }
    if (acc_start(ACC_OP_CHECK, a, NULL) == 0) {
        acc_fmsg(ID_ACC_NAME, LT("gui.acc_name_checking"), C_DIM);
        redraw_all();
    }
}

static void acc_build(HWND w)
{
    int x = ACC_X + 18, y = LOC_ONL_Y; /* W397: rechts oben auf Multiplayer */
    /* Karte "steelnet account" (W397: LoC > Multiplayer rechts oben). W418: ohne "Name pruefen". */
    /* W473: Lage wie acc_layout (Zustand oben, Knoepfe unten; Manage ohne Discord-Knopf links, s. acc_layout) */
    mk(w, K_EDIT, ID_ACC_NAME, P_LNET, x, y + 86, COLW - 36 - 8 - 130, 28, "", 0);
    mk(w, K_ACCENT, ID_ACC_CREATE, P_LNET, ACC_X + COLW - 18 - 130, y + 86, 130, 28, LT("gui.acc_create"), 0);
    mk(w, K_BTN, ID_ACC_SIGNIN, P_LNET, x, y + 122, COLW - 36, 26, LT("gui.acc_signin"), 0);
    mk(w, K_ACCENT, ID_ACC_DISCORD, P_LNET, x, y + ACC_C_BTN, 168, 28, LT("gui.acc_discord_link"), 0);
    mk(w, K_BTN, ID_ACC_MANAGE, P_LNET, x + 174, y + ACC_C_BTN, COLW - 36 - 174, 28, LT("gui.acc_manage"), 0);
    /* W418: Kontoseite. Lage je Zustand setzt acc_layout (Reihenfolge = Tab-Reihenfolge). Ein Hauptknopf je
     * Zustand (Erstellen bzw. Mit Discord sichern); Aufklapper als Textlinks. */
    mk(w, K_BTN, ID_ACC_X_MANAGE, P_LNET, CX0 + CW - 18 - 120, CY0 + 12, 120, ACC_LINK_H, LT("gui.acc_x_manage"), 0);
    mk(w, K_BTN, ID_ACC_V_DISCORD, P_LNET, COL2 + 18, CY0 + 126, COLW - 36, 30, LT("gui.acc_discord_login"), 0);
    mk(w, K_BTN, ID_ACC_X_LOGIN, P_LNET, COL2 + 18, CY0 + 168, COLW - 36, ACC_LINK_H, LT("gui.acc_x_login_in"), 0);
    mk(w, K_EDIT, ID_ACC_V_LOGIN, P_LNET, COL2 + 18, CY0 + 216, COLW - 36, 28, "", 0);
    mk(w, K_EDIT, ID_ACC_V_PASS, P_LNET, COL2 + 18, CY0 + 266, COLW - 36, 28, "", ES_PASSWORD);
    mk(w, K_BTN, ID_ACC_V_LOGINBTN, P_LNET, COL2 + 18, CY0 + 300, 160, 28, LT("gui.acc_login_go"), 0);
    mk(w, K_BTN, ID_ACC_V_RECSHOW, P_LNET, ACC_RX, CY0 + 244, 76, 26, LT("gui.acc_rec_show"), 0);
    mk(w, K_BTN, ID_ACC_V_RECCOPY, P_LNET, ACC_RX + 82, CY0 + 244, 76, 26, LT("gui.acc_rec_copy"), 0);
    mk(w, K_BTN, ID_ACC_X_CODE, P_LNET, COL2 + 18, CY0 + 198, COLW - 36, ACC_LINK_H, LT("gui.acc_x_code_in"), 0);
    mk(w, K_EDIT, ID_ACC_V_CODE, P_LNET, COL2 + 18, CY0 + 246, COLW - 36 - 104, 28, "", 0);
    mk(w, K_BTN, ID_ACC_V_CODEBTN, P_LNET, COL2 + COLW - 18 - 96, CY0 + 246, 96, 28, LT("gui.acc_redeem"), 0);
    mk(w, K_BTN, ID_ACC_V_FORGET, P_LNET, CX0 + 18, CY0 + 400, 210, 28, LT("gui.acc_forget"), 0);
    mk(w, K_BTN, ID_ACC_V_DELETE, P_LNET, CX0 + 18, CY0 + 436, 210, 28, LT("gui.acc_delete"), 0);
    if (ctl(ID_ACC_NAME)) SendMessageW(ctl(ID_ACC_NAME), EM_LIMITTEXT, 16, 0);
    if (ctl(ID_ACC_V_LOGIN)) SendMessageW(ctl(ID_ACC_V_LOGIN), EM_LIMITTEXT, 32, 0);
    if (ctl(ID_ACC_V_PASS)) SendMessageW(ctl(ID_ACC_V_PASS), EM_LIMITTEXT, 256, 0);
    if (ctl(ID_ACC_V_CODE)) SendMessageW(ctl(ID_ACC_V_CODE), EM_LIMITTEXT, 48, 0); /* W418: beide Codes */
    /* W418: Karte Multiplayer auf LoC > Spielen: Knopf zur Kontoseite (links neben "Multiplayer ...") */
    mk(w, K_BTN, ID_LOC_MP_ACC, P_PLAY, LOC_NET_X + 18, LOC_NET_Y + LOC_NET_H - 40, LOC_NET_W - 36 - 150 - 8, 28,
       LT("gui.acc_mp_new"), 0);
}

static void acc_relabel(void)
{
    static const struct {
        int id;
        const char *key;
    } kL[] = {
        { ID_ACC_CREATE, "gui.acc_create" }, { ID_ACC_SIGNIN, "gui.acc_signin" },
        { ID_ACC_MANAGE, "gui.acc_manage" }, { ID_ACC_V_DELETE, "gui.acc_delete" },
        { ID_ACC_V_FORGET, "gui.acc_forget" }, { ID_ACC_V_RECCOPY, "gui.acc_rec_copy" },
        { ID_ACC_V_CODEBTN, "gui.acc_redeem" }, { ID_ACC_X_CODE, "gui.acc_x_code_in" },
        { ID_ACC_X_MANAGE, "gui.acc_x_manage" },
    };
    int i;
    for (i = 0; i < (int)(sizeof kL / sizeof kL[0]); i++) {
        if (ctl(kL[i].id)) ui_settext(ctl(kL[i].id), LT(kL[i].key));
    }
    if (ctl(ID_ACC_V_RECSHOW)) ui_settext(ctl(ID_ACC_V_RECSHOW), LT(g_acc_rec[0] ? "gui.acc_rec_hide" : "gui.acc_rec_show"));
    acc_refresh();
}

/* Start LoC: mit gespeichertem Konto Pilotname (--set) + Umgebungsblock mit
 * SB_LOC_AUTH=<account>:<exp>:<G> (nur Kindprozess, nie Datei/Kommandozeile).
 * Ohne Konto: NULL (Client meldet sich als Gast). Transport/Treffpunkt bleiben. */
wchar_t *acc_launch_env(char *set, size_t cap)
{
    char a[200];
    wchar_t *env;
    acc_load();
    if (!g_acc.have) {
        return NULL;
    }
    _snprintf_s(set + strlen(set), cap - strlen(set), _TRUNCATE, ";loc_lobby_pilot=%s", g_acc.name);
    lacc_game_auth(&g_acc, (long long)time(NULL), a, sizeof a);
    env = lacc_env_block(LACC_ENV_AUTH, a);
    SecureZeroMemory(a, sizeof a);
    return env;
}

/* W454 LOC-LAN-MODE: Name des auf diesem PC gespeicherten Kontos (lokal, kein Netz, nie der Schluessel).
 * 1 = vorhanden. */
int acc_local_name(char *out, size_t cap)
{
    if (!out || !cap) return 0;
    out[0] = '\0';
    acc_load();
    if (!g_acc.have || !g_acc.name[0]) return 0;
    strncpy_s(out, cap, g_acc.name, _TRUNCATE);
    return 1;
}

/* Statuszeile "Pilot" (Spielen > Bereitschaft, Zeile Online) */
int acc_sum_row(char *val, size_t cap, COLORREF *col)
{
    if (!acc_srv_on()) {
        return 0;
    }
    acc_load();
    if (!g_acc.have) {
        strncpy_s(val, cap, LT("gui.acc_sum_none"), _TRUNCATE);
        *col = C_WARN;
    } else {
        int st = acc_state(); /* W473: Status unbekannt nicht als "nicht gesichert" (gleich auf steelnet > Info) */
        _snprintf_s(val, cap, _TRUNCATE,
                    LT(st == ACCS_SEC ? "gui.acc_sum_ok" : st == ACCS_UNSEC ? "gui.acc_sum_unsec" : "gui.acc_sum_unk"),
                    g_acc.name);
        *col = st == ACCS_SEC ? C_OK : st == ACCS_UNSEC ? C_WARN : C_DIM;
    }
    return 1;
}

/* Karte "steelnet account" auf Match & network (W418: ohne "Name pruefen"). 0 = Dienst nicht eingerichtet.
 * W473 MPPOLISH: Zustand in zwei Zeilen (angemeldet als X / gesichert ja-nein), darunter in Worten, was das heisst
 * und wie man es erreicht (passend zu "Manage account": Discord, Login, Wiederherstellungscode). Eine Hauptaktion:
 * ungesichert "Connect with Discord", sonst nur "Manage account". Gleiche Woerter wie die Zeile Konto auf
 * steelnet > Info (acc_sum_row). */
const char *acc_unk_text(COLORREF *c);
int acc_card_state(void)
{
    acc_load();
    return acc_state();
}

/* W473: Erklaerungstext der Karte je Zustand (Selbsttest misst ihn); NULL = keiner */
const char *acc_card_why(int st)
{
    return st == ACCS_UNSEC ? LT("gui.acc_c_unsec_why") : st == ACCS_SEC ? LT("gui.acc_c_sec_why") : NULL;
}

int acc_paint_card(void)
{
    char t[300];
    COLORREF c = C_DIM;
    const char *m;
    int y = LOC_ONL_Y, x0 = ACC_X + 18, w0 = COLW - 36, st;
    if (!acc_srv_on()) {
        return 0;
    }
    st = acc_card_state();
    card(ACC_X, y, COLW, LOC_ONL_H, LT("gui.acc_title"));
    if (st == ACCS_NONE) {
        dot(x0, y + ACC_C_L1 + 6, C_FAINT);
        label1(F_SMALL, LT("gui.acc_c_none"), x0 + 18, y + ACC_C_L1, w0 - 18, 22, C_TEXT);
        label1(F_TINY, LT("gui.acc_name_long"), x0, y + 66, w0, 18, C_DIM);
        m = acc_fmsg_for(ID_ACC_NAME, &c);
        label1(F_TINY, m ? m : LT("gui.acc_head_none2"), x0, y + 154, w0, 20, m ? c : C_DIM);
        return 1;
    }
    _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.acc_signed_as"), g_acc.name);
    dot(x0, y + ACC_C_L1 + 6, C_OK);
    label1(F_SMALL, t, x0 + 18, y + ACC_C_L1, w0 - 18, 22, C_TEXT);
    if (st == ACCS_SEC) {
        const char *k = g_acc_st.discord && g_acc_st.login ? "gui.acc_c_sec_both"
                        : g_acc_st.discord                 ? "gui.acc_c_sec_dc"
                                                           : "gui.acc_c_sec_lg";
        _snprintf_s(t, sizeof t, _TRUNCATE, "\xE2\x9C\x93  %s", LT(k)); /* gruener Haken wie auf der Kontoseite */
        label1(F_SMALL, t, x0, y + ACC_C_L2, w0, 22, C_OK);
    } else if (st == ACCS_UNSEC) {
        dot(x0, y + ACC_C_L2 + 6, C_WARN);
        label1(F_SMALL, LT("gui.acc_c_unsec"), x0 + 18, y + ACC_C_L2, w0 - 18, 22, C_WARN);
    } else { /* Status laeuft/unbekannt */
        const char *k = acc_unk_text(&c);
        dot(x0, y + ACC_C_L2 + 6, c);
        label1(F_SMALL, k, x0 + 18, y + ACC_C_L2, w0 - 18, 22, c);
    }
    m = acc_card_why(st);
    if (m) {
        label(F_TINY, m, x0, y + ACC_C_WHY, w0, ACC_C_WHY_H, C_DIM);
    }
    return 1;
}
/* W473: Selbsttest - passen die Texte der Karte im aktuellen Zustand (Zeilen 1/2 einzeilig, Erklaerung im Kasten)?
 * 1 = ja, sonst why gesetzt. */
int acc_card_fits(char *why, size_t cap, const char *tag)
{
    char t[300];
    COLORREF c;
    const char *l2 = NULL, *m;
    int st = acc_card_state(), x0 = ACC_X + 18, w0 = COLW - 36, y = LOC_ONL_Y, w2 = w0 - 18;
    if (st == ACCS_NONE) {
        strcpy_s(t, sizeof t, LT("gui.acc_c_none"));
    } else {
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.acc_signed_as"), g_acc.name);
    }
    if (!ui_text_fits(F_SMALL, t, x0 + 18, y + ACC_C_L1, w0 - 18, 22, 1)) {
        _snprintf_s(why, cap, _TRUNCATE, "%s Konto-Karte Zeile 1 '%s' zu breit", tag, t);
        return 0;
    }
    if (st == ACCS_SEC) {
        static char s2[200];
        _snprintf_s(s2, sizeof s2, _TRUNCATE, "\xE2\x9C\x93  %s",
                    LT(g_acc_st.discord && g_acc_st.login ? "gui.acc_c_sec_both"
                       : g_acc_st.discord                 ? "gui.acc_c_sec_dc"
                                                          : "gui.acc_c_sec_lg"));
        l2 = s2;
        w2 = w0;
    } else if (st == ACCS_UNSEC) {
        l2 = LT("gui.acc_c_unsec");
    } else if (st == ACCS_UNK) {
        l2 = acc_unk_text(&c);
    }
    if (l2 && !ui_text_fits(F_SMALL, l2, x0 + (w2 == w0 ? 0 : 18), y + ACC_C_L2, w2, 22, 1)) {
        _snprintf_s(why, cap, _TRUNCATE, "%s Konto-Karte Zeile 2 '%s' zu breit", tag, l2);
        return 0;
    }
    m = acc_card_why(st);
    if (m && !ui_text_fits(F_TINY, m, x0, y + ACC_C_WHY, w0, ACC_C_WHY_H, 0)) {
        _snprintf_s(why, cap, _TRUNCATE, "%s Konto-Karte Erklaerung abgeschnitten", tag);
        return 0;
    }
    return 1;
}

/* W418: Wiederherstellungscode verdeckt bzw. angezeigt (Monospace) */
static void acc_paint_code(int x, int y, int w)
{
    L_text(F_MONO, g_acc_rec[0] ? g_acc_rec : "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2-\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2-\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2-\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2",
           x, y, w, 22, g_acc_rec[0] ? C_TEXT : C_FAINT, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

/* W418: Kopfzeile bei unbekanntem Status (laeuft, nicht verbunden, Server nicht erreichbar, Pin, abgemeldet) */
const char *acc_unk_text(COLORREF *c)
{
    *c = C_DIM;
    if (g_acc_busy == ACC_OP_STATUS) {
        return LT("gui.acc_st_loading");
    }
    if (strcmp(g_acc_st_err, "pin") == 0) {
        *c = C_ERR;
        return LT("gui.acc_err_pin");
    }
    if (strcmp(g_acc_st_err, "auth_failed") == 0) {
        *c = C_ERR;
        return LT("gui.acc_err_auth");
    }
    if (g_acc_st_err[0]) { /* net, timeout, server_busy, ... */
        *c = C_WARN;
        return LT("gui.acc_st_net");
    }
    if (netm_get() != NETM_STEEL) {
        return LT("gui.acc_st_offline");
    }
    return LT("gui.acc_st_unknown");
}

/* W418: Beschriftungen der Login-Felder (Feld darunter bei +18 bzw. +68) */
static void acc_paint_login_lbl(int x, int y, int w, int set)
{
    label1(F_TINY, LT("gui.acc_l_login"), x, y, w, 18, C_DIM);
    label1(F_TINY, LT(set ? "gui.acc_l_pw_new" : "gui.acc_l_pw"), x, y + 50, w, 18, C_DIM);
}

/* W418: Verwalten (aufgeklappt): Discord, Login, Wiederherstellungscode, Code von einem anderen PC oder vom
 * Admin, Abmelden, Loeschen (ganz unten, rot). Ungesichert nur die letzten drei (Rest steht in der Karte darueber). */
static void acc_paint_manage(const acc_geo_t *g)
{
    char t[300];
    COLORREF c = C_DIM;
    const char *m;
    int x0 = CX0 + 18, xr = CX0 + CW - 18;
    card(CX0, g->my, CW, g->mh, NULL);
    if (g->r_dc >= 0) {
        int on = g_acc_st_ok && g_acc_st.discord;
        label1(F_SMALL, LT("gui.acc_m_discord"), x0, g->r_dc, ACC_M_LW, 22, C_TEXT);
        label1(F_TINY, LT(on ? "gui.acc_sec_dc_on" : g_acc_st_ok ? "gui.acc_sec_dc_off" : "gui.acc_st_unknown"), x0,
               g->r_dc + 22, xr - x0 - 226, 18, on ? C_OK : C_DIM);
    }
    if (g->r_lg >= 0) {
        L_fill(x0, g->r_lg - 6, xr - x0, 1, RGB(255, 255, 255), 12, 0);
        label1(F_SMALL, LT("gui.acc_m_login"), x0, g->r_lg, ACC_M_LW, 22, C_TEXT);
        if (g_acc_st_ok && g_acc_st.login) {
            _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.acc_sec_lg_on"), g_acc_st.login_name);
            label1(F_TINY, t, x0, g->r_lg + 22, xr - x0 - 226, 18, C_OK);
        } else {
            label1(F_TINY, LT(g_acc_st_ok ? "gui.acc_sec_lg_off" : "gui.acc_st_unknown"), x0, g->r_lg + 22,
                   xr - x0 - 226, 18, C_DIM);
        }
    }
    if (g->r_lgx >= 0) {
        label1(F_TINY, LT("gui.acc_l_login"), x0, g->lg, 214, 18, C_DIM);
        label1(F_TINY, LT("gui.acc_l_pw_new"), x0 + 226, g->lg, 214, 18, C_DIM);
        m = acc_fmsg_for(ID_ACC_V_LOGIN, &c);
        label1(F_TINY, m ? m : LT("gui.acc_login_rules"), x0, g->lg + 50, xr - x0, 18, m ? c : C_FAINT);
    }
    if (g->r_rec >= 0) {
        L_fill(x0, g->r_rec - 6, xr - x0, 1, RGB(255, 255, 255), 12, 0);
        label1(F_SMALL, LT("gui.acc_rec_title"), x0, g->r_rec, 150, 22, C_TEXT);
        acc_paint_code(x0 + 160, g->r_rec, xr - 226 - x0 - 160);
        label1(F_TINY, LT("gui.acc_rec_text"), x0, g->r_rec + 24, xr - x0 - 226, 18, C_DIM);
    }
    if (g->r_code >= 0) {
        if (g->r_rec >= 0) {
            L_fill(x0, g->r_code - 6, xr - x0, 1, RGB(255, 255, 255), 12, 0);
        }
        label1(F_SMALL, LT("gui.acc_m_code"), x0, g->r_code, xr - x0 - 320, 22, C_TEXT);
        m = acc_fmsg_for(ID_ACC_V_CODE, &c);
        label(F_TINY, m ? m : LT("gui.acc_m_code_text"), x0, g->r_code + 24, xr - x0 - 320, 30, m ? c : C_DIM);
    }
    if (g->r_out >= 0) {
        L_fill(x0, g->r_out - 4, xr - x0, 1, RGB(255, 255, 255), 12, 0);
        label1(F_TINY, LT("gui.acc_m_forget_text"), x0 + 226, g->r_out + 4, xr - x0 - 226, 28, C_DIM);
        label1(F_TINY, LT("gui.acc_m_delete_text"), x0 + 226, g->r_del + 4, xr - x0 - 226, 28, C_DIM);
    }
}

/* W418 LAUNCHER-ACCOUNT-UX: Unterseite Multiplayer > Account ("steelnet account"), ein Zustand, ein naechster
 * Schritt (acc_geo): kein Konto = Neu | Schon ein Konto; ungesichert = Name, bereit, Warnzeile + "Ein Schritt:
 * sichern"; gesichert/unbekannt = kompakte Kopfzeile + Verwalten. Datenschutzzeile immer am Seitenende. */
void acc_paint_view(void)
{
    char t[300];
    COLORREF c = C_DIM;
    const char *m;
    acc_geo_t g;
    int x0 = CX0 + 18, rx = COL2 + 18, rw = COLW - 36;
    acc_geo(&g);
    switch (g.st) {
    case ACCS_OFF: /* steelnet in dieser Ausgabe nicht eingerichtet */
        card(CX0, CY0, CW, 88, LT("gui.acc_title"));
        label(F_TINY, LT("gui.loc_onl_nosrv"), x0, CY0 + 40, CW - 36, 40, C_DIM);
        break;
    case ACCS_NONE:
        card(CX0, CY0, CW, g.hh, LT("gui.acc_head_none"));
        label1(F_SMALL, LT("gui.acc_head_none2"), x0, CY0 + 38, CW - 36, 22, C_DIM);
        /* links: "Ich bin neu" (hervorgehoben) */
        card(CX0, g.ay, COLW, g.ah, LT("gui.acc_new_title"));
        L_ring(CX0, g.ay, COLW, g.ah, C_ACCENT, 150, 10);
        label1(F_TINY, LT("gui.acc_name_long"), x0, g.ay + 42, rw, 18, C_DIM);
        m = acc_fmsg_for(ID_ACC_NAME, &c);
        label1(F_TINY, m ? m : LT("gui.acc_name_hint"), x0, g.ay + 94, rw, 20, m ? c : C_FAINT);
        /* rechts: "Ich habe schon ein Konto" */
        card(COL2, g.ay, COLW, g.ah, LT("gui.acc_have_title"));
        if (g.lg >= 0) {
            acc_paint_login_lbl(rx, g.lg, rw, 0);
            m = acc_fmsg_for(ID_ACC_V_LOGIN, &c);
            if (m) {
                label(F_TINY, m, rx + 170, g.lg + 100, rw - 170, 32, c);
            }
        }
        if (g.cd >= 0) {
            label1(F_TINY, LT("gui.acc_code_label"), rx, g.cd, rw, 18, C_DIM);
            m = acc_fmsg_for(ID_ACC_V_CODE, &c);
            label(F_TINY, m ? m : LT("gui.acc_code_hint"), rx, g.cd + 50, rw, 30, m ? c : C_FAINT);
        }
        break;
    case ACCS_UNSEC:
        card(CX0, CY0, CW, g.hh, g_acc.name);
        dot(x0, CY0 + 46, C_OK);
        label1(F_SMALL, LT("gui.acc_ready"), x0 + 18, CY0 + 40, CW - 54, 22, C_OK);
        dot(x0, CY0 + 68, C_WARN);
        label1(F_SMALL, LT("gui.acc_unsecured"), x0 + 18, CY0 + 62, CW - 54, 22, C_WARN);
        card(CX0, g.ay, CW, g.ah, LT("gui.acc_sec_one"));
        label1(F_TINY, LT("gui.acc_sec_dc_rec"), x0 + 256, g.ay + 44, CW - 36 - 256, 32, C_DIM);
        L_fill(x0, g.ay + 86, CW - 36, 1, RGB(255, 255, 255), 16, 0);
        L_fill(ACC_RX - 12, g.ay + 96, 1, g.ah - 108, RGB(255, 255, 255), 16, 0);
        /* rechts: Wiederherstellungscode notieren (Anzeigen/Kopieren) */
        label1(F_SMALL, LT("gui.acc_rec_write"), ACC_RX, g.rec, ACC_HALF_W, 22, C_TEXT);
        acc_paint_code(ACC_RX, g.rec + 24, ACC_HALF_W);
        label(F_TINY, LT("gui.acc_rec_text"), ACC_RX + 166, g.rec + 46, ACC_HALF_W - 166, 32, C_DIM);
        /* links: Login und Passwort festlegen (aufklappbar) */
        if (g.lg >= 0) {
            acc_paint_login_lbl(x0, g.lg, ACC_HALF_W, 1);
            m = acc_fmsg_for(ID_ACC_V_LOGIN, &c);
            label(F_TINY, m ? m : LT("gui.acc_login_rules"), x0, g.lg + 134, ACC_HALF_W, 30, m ? c : C_FAINT);
        }
        if (g.my >= 0) {
            acc_paint_manage(&g);
        }
        break;
    case ACCS_SEC:
    case ACCS_UNK:
        card(CX0, CY0, CW, g.hh, g_acc.name);
        if (g.st == ACCS_SEC) {
            const char *k = g_acc_st.discord && g_acc_st.login ? "gui.acc_st_both"
                            : g_acc_st.discord                 ? "gui.acc_st_discord"
                                                               : "gui.acc_st_login";
            _snprintf_s(t, sizeof t, _TRUNCATE, "\xE2\x9C\x93  %s", LT(k)); /* gruener Haken */
            label1(F_SMALL, t, x0, CY0 + 36, CW - 36 - 130, 22, C_OK);
        } else {
            const char *k = acc_unk_text(&c);
            dot(x0, CY0 + 42, c);
            label1(F_SMALL, k, x0 + 18, CY0 + 36, CW - 54 - 130, 22, c);
        }
        if (g.my >= 0) {
            acc_paint_manage(&g);
        }
        break;
    default:
        break;
    }
    /* Datenschutz: was auf dem Server liegt (klein, alle Zustaende) */
    label(F_TINY, LT("gui.acc_privacy"), x0, ACC_PRIV_Y, CW - 36, 32, C_FAINT);
}

/* W293: Bereitschaft LoC: Spiel, Grafik, Anzeige, Steuerung, Online (spaeter).
 * "Change" fuehrt zur Seite der Ursache (Spiel/Online = LoC > Erweitert). */
static const int kLocSumPage[LOC_SUM_N] = { P_ADV, P_GFX, P_DISPLAY, P_CTRL, P_LNET }; /* W397: Online -> Multiplayer */

int loctab_command(int id, int code)
{
    if (acc_command(id, code)) { /* W328 */
        return 1;
    }
#if defined(SB_HAVE_LOCTP) && SB_HAVE_LOCTP
    if (loctp_command(id, code)) { /* W336 */
        return 1;
    }
#endif
    if (id >= ID_LOC_GO0 && id < ID_LOC_GO0 + LOC_SUM_N) {
        if (code == BN_CLICKED) {
            if (id - ID_LOC_GO0 == LOC_SUM_N - 1 && !loc_net_ok()) {
                lset_open(); /* W400: Online ohne steelnet -> Zahnrad > Online & Updates */
            } else {
                show_page(kLocSumPage[id - ID_LOC_GO0]);
            }
        }
        return 1;
    }
    if (id == ID_LOC_GATE_GO) { /* W400 */
        if (code == BN_CLICKED) {
            lset_open();
        }
        return 1;
    }
    if (id == ID_LOC_VER_GO) { /* W421: Release-Seite im Browser (kein Download, kein Selbst-Update) */
        if (code == BN_CLICKED) {
            ver_open_page();
        }
        return 1;
    }
    if (id == ID_LOC_IMG_GO) { /* W403: zur Disc-Auswahl (LoC > Erweitert, Feld fokussiert) */
        if (code == BN_CLICKED) {
            show_page(P_ADV);
            if (g_o.shot == 0 && ctl(ID_LOC_IMAGE)) {
                SetFocus(ctl(ID_LOC_IMAGE));
            }
        }
        return 1;
    }
    switch (id) {
    case ID_GAME_SB:
    case ID_GAME_LOC: {
        /* W470 LAUNCHER-HUB: aus Optionen/steelnet fuehrt ein Spiel-Reiter zurueck in den Spielbereich (zuletzt
         * offene Spielseite); im Spielbereich wie bisher nur Spielwechsel */
        int back = hub_area_of(g_page) != HUB_GAME;
        if (back) {
            g_page = hub_game_page();
        }
        loctab_set_game(id == ID_GAME_LOC ? LGAME_LOC : LGAME_SB);
        if (back) {
            show_page(g_page);
        }
        return 1;
    }
    case ID_LOC_CHECK:
        loctab_check();
        return 1;
    case ID_LOC_BROWSE:
        loctab_browse();
        return 1;
    case ID_LOC_FOLDER:
        loctab_open_folder();
        return 1;
    case ID_LOC_START:
        loctab_start();
        return 1;
    case ID_LOC_WIDE: /* W326 LOC-WIDE */
        if (code == CBN_SELCHANGE && !g_updating) {
            loctab_wide_pick();
        }
        return 1;
    case ID_LNET_KIND: /* W454: Online (steelnet) / LAN */
        if (code == CBN_SELCHANGE && !g_updating) {
            lan_kind_pick();
        }
        return 1;
    case ID_LNET_LANROOM: /* W454: Raumname bzw. Raumpasswort (LAN) */
    case ID_LNET_LANPASS:
        if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
            g_focus_id = code == EN_SETFOCUS ? id : 0;
            redraw_all();
        } else if (code == EN_CHANGE && !g_updating) {
            lan_field_changed(id);
        }
        return 1;
    case ID_LOC_VIS: /* W373: Oeffentlich / Privat */
        if (code == CBN_SELCHANGE && !g_updating) {
            onl_vis_pick();
        }
        return 1;
    case ID_LOC_SMOOTH: /* W387: Gegner glaetten Aus / Auto / Fest */
        if (code == CBN_SELCHANGE && !g_updating) {
            loc_smooth_pick();
            onl_sync_ui(); /* W392: "Standard wiederherstellen" an/aus */
        }
        return 1;
    case ID_LNET_RB: /* W447: Auswahl Netzmodus (BN_CLICKED = Selbsttest W392: Lockstep an/aus) */
        if (code == CBN_SELCHANGE && !g_updating) {
            lnet_netmode_pick(ComboBox_GetCurSel(ctl(ID_LNET_RB)));
        } else if (code == BN_CLICKED) {
            lnet_toggle(id);
        }
        return 1;
    case ID_LNET_OWN:
    case ID_LNET_LOWD:
    case ID_LNET_CLIP: /* W420 */
    case ID_LNET_CHAT: /* W437 */
    case ID_LNET_SNCHAT: /* W446 */
    case ID_LNET_MSN:    /* W473: Chat im Spiel */
    case ID_LNET_MLOBBY:
    case ID_LNET_MTEAM:
        if (code == BN_CLICKED) {
            lnet_toggle(id);
        }
        return 1;
    case ID_LNET_TAB0: /* W473: Reiter der Netzwerk-Karte */
    case ID_LNET_TAB1:
        if (code == BN_CLICKED) {
            lnet_tab_pick(id == ID_LNET_TAB1);
        }
        return 1;
    case ID_LNET_MODE: /* W440 */
        if (code == CBN_SELCHANGE && !g_updating) {
            lnet_mode_pick();
            onl_sync_ui(); /* "Standard wiederherstellen" an/aus */
        }
        return 1;
    case ID_LNET_VERPAGE: /* W420: Release-Seite im Browser (kein Download, kein Selbst-Update) */
        if (code == BN_CLICKED) {
            ver_open_page(); /* W421: auch ohne Ergebnis der Versionspruefung (dann .../latest) */
        }
        return 1;
    case ID_LNET_RESET:
        if (code == BN_CLICKED) {
            lnet_reset();
        }
        return 1;
    case ID_LNET_PRIV: /* W450: Datenschutz-Hinweis zu Chat und steelnet (kein Schreiben, nur Anzeige) */
        if (code == BN_CLICKED) {
            privacy_dialog_modal(g_o.shot != 0 ? 1 : 0);
        }
        return 1;
    case ID_LOC_CODE_COPY: /* W334: Raumcode der eigenen Lobby kopieren */
        if (code == BN_CLICKED) {
            onl_copy();
        }
        return 1;
    case ID_LOC_CODE: /* W334: Raumcode (Eingabe tolerant, Pruefung in der Infozeile) */
        if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
            g_focus_id = code == EN_SETFOCUS ? id : 0;
            redraw_all();
        } else if (code == EN_CHANGE && !g_updating) {
            ui_refresh_visibility();
            onl_redraw();
        }
        return 1;
    case ID_LOC_IMAGE:
        if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
            g_focus_id = code == EN_SETFOCUS ? id : 0;
            redraw_all();
        } else if (code == EN_CHANGE && id == ID_LOC_IMAGE && !g_updating) {
            redraw_rect(CX0, LOC_DISC_Y, COLW, LOC_DISC_H); /* Ergebnis gilt nur fuer den geprueften Pfad */
        }
        return 1;
    case IDOK:
    case ID_START_BIG:
    case ID_START:
    case ID_START_VR:
    case ID_START_MENU:
        if (g_game == LGAME_LOC) { /* nie Steel Battalion starten, solange LoC gewaehlt ist */
            if (id == IDOK) {
                HWND f = GetFocus();
                wchar_t cls[32] = { 0 };
                if (f) {
                    GetClassNameW(f, cls, 32);
                }
                if (_wcsicmp(cls, L"Edit") == 0) {
                    if (f == ctl(ID_LOC_IMAGE)) {
                        loctab_check();
                    }
                    return 1;
                }
            }
            loctab_start();
            return 1;
        }
        return 0;
    default:
        return 0;
    }
}

/* Ergebniszeile der LoC-Disc-Karte: Text + Farbe. */
void loctab_result(char *msg, size_t cap, COLORREF *col)
{
    char p[512];
    int k;
    ui_gettext(ctl(ID_LOC_IMAGE), p, sizeof p);
    if (!g_loc_checked || strcmp(p, g_loc_res_path) != 0) {
        strncpy_s(msg, cap, LT(p[0] ? "gui.loc_unchecked" : "gui.loc_img_none"), _TRUNCATE);
        *col = C_DIM;
        return;
    }
    k = lgame_img_msg(&g_loc_res, g_loc_res_path, msg, cap);
    *col = k == 1 ? C_OK : k == 2 ? C_ERR : k == 3 ? C_WARN : C_DIM;
}

/* W293 LOC-LAYOUT: Bereitschaftszeilen LoC. Spiel = Disc-Status (kein
 * Abbild / erkannt mit Title-ID / Fehler), Grafik/Anzeige/Steuerung wie SB
 * (gemeinsame Werte, update_summary), Online kommt spaeter (grau). */

static void loctab_sum_game(char *out, size_t cap, COLORREF *col)
{
    char p[512];
    p[0] = 0;
    if (ctl(ID_LOC_IMAGE)) {
        ui_gettext(ctl(ID_LOC_IMAGE), p, sizeof p);
    }
    if (!p[0]) {
        strncpy_s(out, cap, LT("gui.loc_sum_none"), _TRUNCATE);
        *col = C_WARN;
    } else if (!g_loc_checked || strcmp(p, g_loc_res_path) != 0) {
        strncpy_s(out, cap, LT("gui.loc_sum_unchecked"), _TRUNCATE);
        *col = C_WARN;
    } else {
        switch (g_loc_res.kind) {
        case LGAME_IMG_LOC:
            _snprintf_s(out, cap, _TRUNCATE, LT("gui.loc_sum_ok"), g_loc_res.title_id);
            *col = C_OK;
            break;
        case LGAME_IMG_LOC_JP:
            _snprintf_s(out, cap, _TRUNCATE, LT("gui.loc_sum_jp"), g_loc_res.title_id);
            *col = C_WARN;
            break;
        case LGAME_IMG_SB:
            strncpy_s(out, cap, LT("gui.loc_sum_sb"), _TRUNCATE);
            *col = C_WARN;
            break;
        case LGAME_IMG_MISSING:
        case LGAME_IMG_UNREADABLE:
            strncpy_s(out, cap, LT("gui.loc_sum_err"), _TRUNCATE);
            *col = C_ERR;
            break;
        case LGAME_IMG_NONE:
            strncpy_s(out, cap, LT("gui.loc_sum_none"), _TRUNCATE);
            *col = C_WARN;
            break;
        default:
            _snprintf_s(out, cap, _TRUNCATE, LT("gui.loc_sum_other"), g_loc_res.title_id);
            *col = C_ERR;
            break;
        }
    }
}

/* W363 LOC-ORIGINAL: Zeile "Anzeige" im LoC-Bereich. LoC nutzt dieselben Bild-Optionen wie SB (user.cfg:
 * Fenster/Vollbild, Aufloesung, Bildrate mit Zwischenbildern, VSync), aber Original-Takt = Spieltakt von LoC
 * (Mission 20 Hz, Menues 30 Hz) statt 30 FPS; dazu VSync und das LoC-Bildformat (4:3 mit Balken = Original). */
void loctab_display_value(const launcher_settings_t *s, char *out, size_t cap)
{
    char fpsb[96], scr[160];
    if (s->interp_fps == 0) { strncpy_s(fpsb, sizeof fpsb, LT("gui.loc_sum_fps_orig"), _TRUNCATE); }
    else if (s->interp_fps == SB_FPS_AUTO) { strncpy_s(fpsb, sizeof fpsb, LT("gui.sum_auto"), _TRUNCATE); }
    else { _snprintf_s(fpsb, sizeof fpsb, _TRUNCATE, LT("gui.loc_sum_fps_ip"), s->interp_fps); }
    if (s->window_mode == 1 || s->window_mode == 2) {
        strncpy_s(scr, sizeof scr, s->window_mode == 1 ? LT("gui.wm_borderless") : LT("gui.wm_excl"), _TRUNCATE);
    } else {
        _snprintf_s(scr, sizeof scr, _TRUNCATE, LT("gui.loc_sum_win"), s->width, s->height);
    }
    _snprintf_s(out, cap, _TRUNCATE, "%s \xC2\xB7 %s \xC2\xB7 %s \xC2\xB7 %s", scr, fpsb,
                LT(s->present_interval ? "gui.loc_sum_vsync_on" : "gui.loc_sum_vsync_off"),
                LT(lgame_wide_label_key(loctab_wide_val())));
}

void loctab_sum_rows(loc_sumrow_t *r)
{
    static const int kShared[3] = { 1, 2, 4 }; /* g_sum: Grafik, Anzeige, Steuerung */
    int i;
    memset(r, 0, sizeof(loc_sumrow_t) * LOC_SUM_N);
    r[0].label = LT("gui.sum_game");
    loctab_sum_game(r[0].value, sizeof r[0].value, &r[0].col);
    for (i = 0; i < 3; i++) {
        const sumrow_t *g = &g_sum[kShared[i]];
        r[1 + i].label = g->label ? g->label : "";
        strcpy_s(r[1 + i].value, sizeof r[1 + i].value, g->value);
        r[1 + i].col = g->col;
    }
    loctab_display_value(&g_s, r[2].value, sizeof r[2].value); /* W363: LoC-Takt, VSync, Bildformat */
    r[4].label = LT("gui.loc_sum_online");
    if (g_loc_lan) { /* W454: LAN-Modus - kein steelnet, keine Konto-Zeile */
        strncpy_s(r[4].value, sizeof r[4].value, LT("gui.mp_sum_lan"), _TRUNCATE);
        r[4].col = C_OK;
    } else if (!loc_net_ok()) { /* W400: ohne steelnet kein LoC-Spiel -> Warnpunkt, "Change" fuehrt zum Zahnrad */
        strncpy_s(r[4].value, sizeof r[4].value, LT("gui.loc_sum_net_off"), _TRUNCATE);
        r[4].col = C_WARN;
    } else if (!acc_sum_row(r[4].value, sizeof r[4].value, &r[4].col)) { /* W328: Pilot, sobald der Dienst eingerichtet ist */
        strncpy_s(r[4].value, sizeof r[4].value, LT("gui.loc_server_cue"), _TRUNCATE);
        r[4].col = C_FAINT; /* grauer Punkt: kommt spaeter */
    } else if (netm_get() != NETM_STEEL) { /* W397: nicht mit steelnet verbunden -> lokales Spiel, keine Konto-Warnung */
        strncpy_s(r[4].value, sizeof r[4].value, LT("gui.mp_sum_off"), _TRUNCATE);
        r[4].col = C_FAINT;
    }
}

void paint_loc_play(void)
{
    SetTextCharacterExtra(g_bb.dc, SX(3));
    L_text(F_HERO, "LINE OF CONTACT", CX0 - 2, CY0 - 2, CW, 50, C_TEXT,
           DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SetTextCharacterExtra(g_bb.dc, 0);
    /* "In development" als Plakette vor der Unterzeile */
    L_fill(CX0, CY0 + 52, 132, 20, C_ACCENT, 46, 3);
    L_fill(CX0, CY0 + 52, 2, 20, C_ACCENT, 255, 0);
    /* W389 RELEASE-030: mit freiem LoC-Start "Experimental" statt "In development" */
    L_text(F_TINY, LT(lgame_loc_client_ready() ? "gui.loc_badge_ready" : "gui.loc_badge"), CX0, CY0 + 52, 132, 20,
           C_ACCENT_HI, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    label1(F_BODY, LT(lgame_loc_client_ready() ? "gui.loc_sub_ready" : "gui.loc_sub"), CX0 + 144, CY0 + 50,
           CW - 144, 22, C_DIM);
    /* Sperre unter dem Startknopf (Muster D: Schloss) */
    if (!lgame_loc_client_ready()) {
        if (g_has_icons) {
            wchar_t ic = 0xE72E;
            RECT q; /* W281 */
            lay_px(CX0, CY0 + 148, 14, 16, 0, 0, &q);
            cv_textw(&g_bb, g_f[F_ICONSM], &ic, 1, q.left, q.top, SX(14), SX(16),
                     C_ACCENT_HI, DT_LEFT | DT_TOP | DT_SINGLELINE);
        }
        label(F_TINY, LT("gui.loc_soon"), CX0 + 18, CY0 + 146, 312, 40, C_ACCENT_HI);
    } else if (loc_ver_gate() || loc_img_gate(NULL, 0) || loc_gate_shown()) { /* W421: Version zuerst */
        /* W400: Grund unter dem grauen Knopf, darunter Sprung zum Zahnrad (ID_LOC_GATE_GO). W403: Disc zuerst
         * (Sprung ID_LOC_IMG_GO), steelnet nur noch kurz (ein Satz; Zeile "Online" bleibt Status) */
        char m[700];
        loc_gate_msg(m, sizeof m);
        label(F_TINY, m, CX0, CY0 + 146, 308, 34, C_WARN);
    } else if (!lnet_is_default()) { /* W392: unter dem Start (frueher Gegner glaetten): Netzwerk-Testeinstellungen */
        label(F_TINY, LT(g_loc_rb_on ? "gui.lnet_play_custom" : "gui.lnet_play_lockstep"), CX0, CY0 + 148, 308, 34,
              C_WARN);
    }
    /* rechts: W334 Karte "Online (steelnet)" (frueher "Gemeinsam mit Steel Battalion";
     * der Hinweis steht jetzt dezent im Kopf von "Startbereit") */
    onl_paint();
    /* W293: Bereitschaft wie SB (Punkt, Bezeichnung, Wert, "Change") */
    {
        loc_sumrow_t r[LOC_SUM_N];
        int i, y;
        loctab_sum_rows(r);
        card(CX0, LOC_SUM_Y, CW, LOC_SUM_H, LT("gui.sum_title"));
        L_text(F_TINY, LT("gui.loc_shared_rows"), CX0 + 200, LOC_SUM_Y + 12, CW - 218, 24, C_FAINT,
               DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS); /* W334 (frueher eigene Karte) */
        for (i = 0, y = LOC_SUM_Y + 40; i < LOC_SUM_N; i++, y += 36) {
            dot(CX0 + 20, y + 6, r[i].col);
            label1(F_SMALL, r[i].label, CX0 + 40, y, 100, 22, C_DIM);
            label1(F_SMALL, r[i].value, CX0 + 142, y, CW - 142 - 18 - 112, 22,
                   r[i].col == C_FAINT ? C_DIM : C_TEXT);
        }
    }
    if (orig_banner_on()) { /* W278/W293: Hinweisband unten wie bei SB */
        orig_paint_play(1);
    }
}

/* W293 LOC-LAYOUT: Erweitert im Bereich LoC. Links die LoC-eigenen Karten
 * (Disc, Spielstaende & Daten, Online spaeter), rechts die gemeinsamen
 * Teile (Feinabstimmung, Launcher). */
void paint_loc_adv(void)
{
    char t[1200];
    COLORREF col;
    /* Disc: Pfad, Check/Browse, Ergebnis, Hinweis */
    card(CX0, LOC_DISC_Y, COLW, LOC_DISC_H, LT("gui.loc_disc_title"));
    loctab_result(t, sizeof t, &col);
    label(F_SMALL, t, CX0 + 18, LOC_DISC_Y + 118, COLW - 36, 38, col);
    label(F_TINY, LT("gui.loc_disc_hint"), CX0 + 18, LOC_DISC_Y + 160, COLW - 36, 50, C_DIM);
    /* Spielstaende & Daten (getrennt von Steel Battalion) */
    card(CX0, LOC_DATA_Y, COLW, LOC_DATA_H, LT("gui.loc_data_title"));
    label1(F_TINY, LT("gui.loc_data_text"), CX0 + 18, LOC_DATA_Y + 38, COLW - 36, 18, C_DIM);
    loctab_data_dir(t, sizeof t);
    L_text(F_SMALL, t, CX0 + 18, LOC_DATA_Y + 56, COLW - 36, 22, C_TEXT,
           DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS);
    /* W397: Karte Account liegt jetzt auf LoC > Multiplayer */
    paint_adv_shared();
    /* W326 LOC-WIDE: Bildformat (Auswahl ID_LOC_WIDE, Kolben ID_LOC_WIDE_XI im Kopf) */
    {
        int v = lgame_wide_clamp(g_loc.loc_widescreen);
        card(COL2, LOC_WIDE_Y, COLW, LOC_WIDE_H, LT("gui.loc_wide_title"));
        label(F_TINY, LT("gui.loc_wide_txt"), COL2 + 18, LOC_WIDE_Y + 84, COLW - 36, 48, C_DIM);
        /* W414: 16:9 ist Standard und nicht experimentell (Text ohne "Experimental", nicht als Warnung) */
        label(F_TINY, LT(v == LGAME_WIDE_720P ? "gui.loc_wide_exp720"
                         : v == LGAME_WIDE_169 ? "gui.loc_wide_169_txt" : "gui.loc_wide_orig"),
              COL2 + 18, LOC_WIDE_Y + 140, COLW - 36, 64, lgame_wide_experimental(v) ? C_WARN : C_DIM);
    }
    /* W465 LOC-ENHANCED: Karte "SB look (Experimental)" (W459) entfernt - das Preset Enhanced ersetzt sie */
}

#if defined(SB_HAVE_LOCTP) && SB_HAVE_LOCTP
#include "launcher_loctp_gui.inc" /* W336 LOC-TESTPARTNER: Knoepfe + Dialog (nur interne Ausgabe) */
#endif

void loctab_build(HWND w)
{
    int i;
    /* Titelleiste: Umschalter (zwischen Edition und Fensterknoepfen) */
    /* W398: Spielnamen als Wortmarken-Tabs (Fenstertext = zugaenglicher Name, gezeichnet in Grossbuchstaben) */
    mk(w, K_SEG, ID_GAME_SB, PG_ALL, HDR_GAME_X, HDR_GAME_Y, HDR_GAME_W, HDR_GAME_H, LT("gui.loc_game_sb"), 0);
    mk(w, K_SEG, ID_GAME_LOC, PG_ALL, HDR_GAME_X + HDR_GAME_W + HDR_GAME_GAP, HDR_GAME_Y, HDR_GAME_W, HDR_GAME_H,
       LT("gui.loc_game_loc"), 0);
    /* Seite Spielen (LoC): Start, "Change" je Bereitschaftszeile (wie SB) */
    mk(w, K_BIG, ID_LOC_START, P_PLAY, CX0, CY0 + 84, 308, 56, LT("gui.loc_btn_start"), 0);
    for (i = 0; i < LOC_SUM_N; i++) {
        mk(w, K_BTN, ID_LOC_GO0 + i, P_PLAY, CX0 + CW - 18 - 104, LOC_SUM_Y + 36 + i * 36, 104, 28,
           LT("gui.play_change"), 0);
    }
    /* W400: Sprung zum Zahnrad in der Zeile ueber "Bereit zum Spielen" (intern neben "LoC test partner",
     * dort statt des dann ungenutzten "Stop") */
#if defined(SB_HAVE_LOCTP) && SB_HAVE_LOCTP
    mk(w, K_BTN, ID_LOC_GATE_GO, P_PLAY, CX0 + 158, LOC_SUM_Y - 32, 150, 28, LT("gui.loc_gate_go"), 0);
    mk(w, K_BTN, ID_LOC_IMG_GO, P_PLAY, CX0 + 158, LOC_SUM_Y - 32, 150, 28, LT("gui.loc_img_go"), 0); /* W403 */
    mk(w, K_BTN, ID_LOC_VER_GO, P_PLAY, CX0 + 158, LOC_SUM_Y - 32, 150, 28, LT("gui.lnet_verpage"), 0); /* W421 */
#else
    mk(w, K_BTN, ID_LOC_GATE_GO, P_PLAY, CX0, LOC_SUM_Y - 32, 150, 28, LT("gui.loc_gate_go"), 0);
    mk(w, K_BTN, ID_LOC_IMG_GO, P_PLAY, CX0, LOC_SUM_Y - 32, 150, 28, LT("gui.loc_img_go"), 0); /* W403 */
    mk(w, K_BTN, ID_LOC_VER_GO, P_PLAY, CX0, LOC_SUM_Y - 32, 150, 28, LT("gui.lnet_verpage"), 0); /* W421 */
#endif
    /* W293: Seite Erweitert (LoC): Disc und Datenordner */
    mk(w, K_EDIT, ID_LOC_IMAGE, P_ADV, CX0 + 18, LOC_DISC_Y + 42, COLW - 36, 32, g_loc.loc_image, 0);
    mk(w, K_BTN, ID_LOC_CHECK, P_ADV, CX0 + 18, LOC_DISC_Y + 80, 122, 30, LT("gui.btn_check"), 0);
    mk(w, K_BTN, ID_LOC_BROWSE, P_ADV, CX0 + 18 + 130, LOC_DISC_Y + 80, 122, 30, LT("gui.btn_browse"), 0);
    mk(w, K_BTN, ID_LOC_FOLDER, P_ADV, CX0 + 18, LOC_DATA_Y + 82, 150, 28, LT("gui.loc_open_folder"), 0);
    /* W326 LOC-WIDE: Bildformat rechts (Auswahl ueber die Kartenbreite, Kolben im Kopf) */
    mk(w, K_COMBO, ID_LOC_WIDE, P_ADV, COL2 + 18, LOC_WIDE_Y + 44, COLW - 36, 30, "", 0);
    mk(w, K_ICON, ID_LOC_WIDE_XI, P_ADV, COL2 + COLW - 18 - XI_W, LOC_WIDE_Y + 12, XI_W, 24 /* XI_H */, "", 0);
    loctab_wide_fill();
    loctab_sblook_sync();
    onl_build(w); /* W334: Online (steelnet) auf LoC-Spielen */
    acc_build(w); /* W328 */
#if defined(SB_HAVE_LOCTP) && SB_HAVE_LOCTP
    loctp_tab_build(w); /* W336 */
#endif
    loctab_nav_layout();
}

void loctab_relabel(void)
{
    ui_settext(ctl(ID_GAME_SB), LT("gui.loc_game_sb"));
    ui_settext(ctl(ID_GAME_LOC), LT("gui.loc_game_loc"));
    ui_settext(ctl(ID_LOC_START), LT("gui.loc_btn_start"));
    ui_settext(ctl(ID_LOC_GATE_GO), LT("gui.loc_gate_go")); /* W400 */
    ui_settext(ctl(ID_LOC_IMG_GO), LT("gui.loc_img_go"));   /* W403 */
    ui_settext(ctl(ID_LOC_VER_GO), LT("gui.lnet_verpage")); /* W421 */
    ui_settext(ctl(ID_LOC_CHECK), LT("gui.btn_check"));
    ui_settext(ctl(ID_LOC_BROWSE), LT("gui.btn_browse"));
    ui_settext(ctl(ID_LOC_FOLDER), LT("gui.loc_open_folder"));
    loctab_wide_fill(); /* W326: Eintraege in der neuen Sprache, Wahl bleibt */
    onl_relabel(); /* W334 */
    acc_relabel(); /* W328 */
#if defined(SB_HAVE_LOCTP) && SB_HAVE_LOCTP
    loctp_relabel(); /* W336 */
#endif
    {
        int i;
        for (i = 0; i < LOC_SUM_N; i++) { /* W293 */
            ui_settext(ctl(ID_LOC_GO0 + i), LT("gui.play_change"));
        }
    }
}

/* Nach gui_fill: LoC gewaehlt -> Navigation + Statuszeile passend. */
void loctab_fill_done(void)
{
    loctab_wide_sync(); /* W326 */
    onl_sync_ui();      /* W334 */
    loctab_nav_layout();
    if (!g_loc_checked && g_loc.loc_image[0] &&
        u8_GetFileAttributes(g_loc.loc_image) != INVALID_FILE_ATTRIBUTES) {
        lgame_loc_check(g_loc.loc_image, &g_loc_res); /* gemerktes Abbild gleich pruefen (nur lesen) */
        strcpy_s(g_loc_res_path, sizeof g_loc_res_path, g_loc.loc_image);
        g_loc_checked = 1;
    }
    if (g_game == LGAME_LOC) {
        show_page(g_page);
        if (g_has_ldr) { /* fehlender Client bleibt als Fehler stehen */
            set_status_k(LT("gui.loc_switched"), 0);
        }
    }
}

/* --ui-shot --state loc_*: Bereich LoC mit Pruef-Fixture (bereinigte
 * Beispielpfade, kein echtes Abbild, nichts gespeichert). Vor build_controls. */
int loctab_apply_state(const char *name)
{
    if (strncmp(name, "loc_", 4) != 0) {
        return 0;
    }
    g_game = LGAME_LOC;
    g_loc.game = LGAME_LOC;
    g_loc_smooth_opp = 1; /* W387: Shots immer mit Standard (Auto), nie die Wahl aus user.cfg */
    g_loc_rb_on = g_loc_own_pred = g_loc_lowd = 1; /* W392: ebenso Netzwerk-Schalter (loc_net_off: Rollback aus) */
    g_loc_net_auto = 1; /* W447 */
    g_loc_clip = 1; /* W420 */
    g_loc_chat = 1; /* W437 */
    g_loc_snchat = 1; /* W446 */
    g_loc_arcade = 0; /* W440 */
    g_loc_online = 0;   /* W397: Shots ohne loc_onl* immer Stufe "nicht steelnet" (nie die Wahl aus user.cfg) */
    g_upd_ask = 0;
    if (strcmp(name, "loc_netq") == 0) { /* W397: Erstabfrage (wie update_ask) */
        g_upd_ver = "0.3.0";
        g_upd_cfg = -1;
        g_upd_ask = 1;
    }
    if (strcmp(name, "loc_net_off") == 0) {
        g_loc_rb_on = 0;
    }
    /* W473: Reiter der Netzwerk-Karte, LAN, Chat im Spiel (Standard), nie die Datei */
    g_loc_msn = 0;
    g_loc_mlobby = g_loc_mteam = 1;
    g_lnet_tab = strcmp(name, "loc_net_chat") == 0 || strcmp(name, "loc_net_chatoff") == 0 ||
                 strcmp(name, "loc_net_lanchat") == 0;
    g_loc_lan = strncmp(name, "loc_net_lan", 11) == 0;
    if (g_loc_lan) {
        strcpy_s(g_loc_lan_room, sizeof g_loc_lan_room, "Party");
    }
    if (strcmp(name, "loc_net_chatoff") == 0) {
        g_loc_chat = g_loc_snchat = 0;
    }
    memset(&g_locfix, 0, sizeof g_locfix);
    g_locfix_path[0] = '\0';
    /* W326: loc_wide = Erweitert mit "16:9 (Spielmodus)" (Warnzeile), sonst 4:3 */
    g_loc.loc_widescreen = strcmp(name, "loc_wide") == 0 || strcmp(name, "loc_pg") == 0 || strcmp(name, "loc_169") == 0 ||
                           strcmp(name, "loc_enh") == 0 /* W465 */ || strcmp(name, "loc_vivid") == 0 /* W478 */
                               ? LGAME_WIDE_169 : LGAME_WIDE_43; /* W414: loc_169 = LoC-Standard */
    g_loc.loc_sblook = 0; /* W459: Shots nie mit SB look (W465: Schalter entfernt, nie die Datei) */
    if (strcmp(name, "loc_ok") == 0 || strcmp(name, "loc_start") == 0 || strcmp(name, "loc_exp") == 0 ||
        strcmp(name, "loc_enh") == 0 || /* W465 */
        strcmp(name, "loc_vivid") == 0 || /* W478 */
        strcmp(name, "loc_pg") == 0 || /* W402 */
        strcmp(name, "loc_169") == 0 || /* W414 */
        strcmp(name, "loc_wide") == 0 || strncmp(name, "loc_onl", 7) == 0 || /* W334: loc_onl* wie loc_ok */
        strcmp(name, "loc_netq") == 0 || /* W397 */
        strcmp(name, "loc_hover") == 0 || /* W398 */
        strcmp(name, "loc_adv") == 0 || /* W427 */
        strncmp(name, "loc_net", 7) == 0) { /* W392: loc_net* wie loc_ok */
        strcpy_s(g_locfix_path, sizeof g_locfix_path, LOC_ISO);
        g_locfix.kind = LGAME_IMG_LOC;
        g_locfix.title_id = LGAME_TID_LOC;
    } else if (strcmp(name, "loc_sb") == 0) {
        strcpy_s(g_locfix_path, sizeof g_locfix_path, LOC_SB_ISO);
        g_locfix.kind = LGAME_IMG_SB;
        g_locfix.title_id = LGAME_TID_SB;
    }
    strcpy_s(g_loc.loc_image, sizeof g_loc.loc_image, g_locfix_path);
    acc_apply_state(name); /* W328: loc_acc_* */
    if (strncmp(name, "loc_onl", 7) == 0) { /* W334: Beispielzustand, kein Netz, keine Datei */
        g_onl_fix_srv = 1;
        g_onl_fixture = 1;
        g_loc_online = 1;
        strcpy_s(g_onl_sec, sizeof g_onl_sec, strcmp(name, "loc_onl_err") == 0 ? "pin_failed" : "ok");
        strcpy_s(g_onl_code, sizeof g_onl_code, strcmp(name, "loc_onl_err") == 0 ? "" : "7K3M-Q9TX");
        if (strcmp(name, "loc_onl_pub") == 0 || strcmp(name, "loc_onl_priv") == 0) { /* W373: ohne eigenen Code */
            g_onl_code[0] = '\0';
            g_loc_public = strcmp(name, "loc_onl_pub") == 0;
            onl_sync_ui();
        }
        if (strncmp(name, "loc_onl_ver", 11) == 0) { /* W421: Mindestversion 0.3.3 (veraltet) bzw. 0.3.2 (aktuell) */
            g_onl_code[0] = '\0';
            locver_fixture(strcmp(name, "loc_onl_verold") == 0 ? "0.3.3" : "0.3.2");
        }
    }
    loc_gate_sync(); /* W400: loc_onl* = steelnet (Start frei), sonst gesperrt */
    return 1;
}

/* W328: Fixture-Konto fuer Shots/Selbsttest (Beispielschluessel, kein echtes Konto). */
void acc_fixture(int srv, int have, int discord, int login, int view)
{
    int i;
    g_acc_fixture = 1;
    g_acc_fix_srv = srv;
    g_acc_loaded = 1;
    memset(&g_acc, 0, sizeof g_acc);
    memset(&g_acc_st, 0, sizeof g_acc_st);
    g_acc_st_ok = 0;
    g_acc_rec[0] = '\0';
    g_acc_avail = -1;
    g_acc_view = view;
    g_acc_st_err[0] = '\0';
    acc_x_reset(); /* W418 */
    if (have) {
        g_acc.have = 1;
        g_acc.account = 4711;
        g_acc.gen = 1;
        strcpy_s(g_acc.name, sizeof g_acc.name, "Ace Pilot");
        strcpy_s(g_acc.host, sizeof g_acc.host, "127.0.0.1:47001");
        for (i = 0; i < 32; i++) g_acc.key[i] = (uint8_t)(0x11 * (i % 15) + i);
        g_acc_st_ok = 1;
        g_acc_st.discord = discord;
        g_acc_st.login = login;
        if (login) strcpy_s(g_acc_st.login_name, sizeof g_acc_st.login_name, "ace-login");
    }
}

void acc_apply_state(const char *name)
{
    if (strcmp(name, "loc_acc_none") == 0) acc_fixture(1, 0, 0, 0, 0);
    else if (strcmp(name, "loc_acc_unsec") == 0) acc_fixture(1, 1, 0, 0, 0);
    else if (strcmp(name, "loc_acc_ok") == 0) acc_fixture(1, 1, 1, 1, 0);
    else if (strcmp(name, "loc_acc_dc") == 0) acc_fixture(1, 1, 1, 0, 0); /* W473 */
    else if (strcmp(name, "loc_acc_lg") == 0) acc_fixture(1, 1, 0, 1, 0); /* W473 */
    else if (strcmp(name, "loc_acc_net") == 0) { /* W473: Karte, Status unbekannt */
        acc_fixture(1, 1, 0, 0, 0);
        g_acc_st_ok = 0;
        memset(&g_acc_st, 0, sizeof g_acc_st);
        strcpy_s(g_acc_st_err, sizeof g_acc_st_err, "net");
    }
    else if (strcmp(name, "loc_acc_view") == 0) {
        acc_fixture(1, 1, 1, 0, 1);
        lacc_recovery_code(g_acc.account, g_acc.key, g_acc_rec, sizeof g_acc_rec);
    } else if (strcmp(name, "loc_acc_signin") == 0) acc_fixture(1, 0, 0, 0, 1);
    else if (strcmp(name, "loc_acc_view_unsec") == 0) acc_fixture(1, 1, 0, 0, 1); /* W399 */
    else if (strcmp(name, "loc_acc_view_login") == 0) acc_fixture(1, 1, 0, 1, 1);
    else if (strcmp(name, "loc_acc_view_ok") == 0) acc_fixture(1, 1, 1, 1, 1);
    else if (strcmp(name, "loc_acc_view_manage") == 0) { /* W418: gesichert, Verwalten offen */
        acc_fixture(1, 1, 1, 1, 1);
        g_acc_x_manage = 1;
    } else if (strcmp(name, "loc_acc_view_net") == 0) { /* W418: Server nicht erreichbar (Status unbekannt) */
        acc_fixture(1, 1, 0, 0, 1);
        g_acc_st_ok = 0;
        memset(&g_acc_st, 0, sizeof g_acc_st);
        strcpy_s(g_acc_st_err, sizeof g_acc_st_err, "net");
    } else if (strcmp(name, "loc_acc_signin_code") == 0) { /* W418: kein Konto, "Mit einem Code" offen */
        acc_fixture(1, 0, 0, 0, 1);
        g_acc_x_code = 1;
    } else if (strcmp(name, "loc_acc_view_unsec_login") == 0) { /* W418: ungesichert, Login-Felder offen */
        acc_fixture(1, 1, 0, 0, 1);
        g_acc_x_login = 1;
    }
}

/* Nach dem Aufbau (Shot): Pruefung bzw. Startversuch wie der Nutzer. */
void loctab_state_after(const char *name)
{
    if (strncmp(name, "loc_", 4) != 0 ||
        (g_page != P_PLAY && g_page != P_ADV && g_page != P_LNET)) { /* W293: Disc auf Erweitert; W399: Konto auf MP */
        return;
    }
    if (g_page == P_LNET && strncmp(name, "loc_acc", 7) != 0) { /* W399: auf Multiplayer nur die Konto-Zustaende */
        return;
    }
    if (g_locfix_path[0]) {
        loctab_check();
    }
    if (strncmp(name, "loc_acc", 7) == 0) {
        acc_refresh(); /* W328: Knopftexte/Sichtbarkeit zum Fixture */
        if (g_acc_rec[0] && ctl(ID_ACC_V_RECSHOW)) ui_settext(ctl(ID_ACC_V_RECSHOW), LT("gui.acc_rec_hide"));
        if (g_acc_st.login && ctl(ID_ACC_V_LOGIN)) ui_settext(ctl(ID_ACC_V_LOGIN), g_acc_st.login_name);
        if (!g_acc.have && !g_acc_view && ctl(ID_ACC_NAME)) ui_settext(ctl(ID_ACC_NAME), "Ace Pilot");
    }
    if (strcmp(name, "loc_start") == 0) {
        loctab_start();
    }
}

/* ------------------------------------------------------------------ */
