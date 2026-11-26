/* launcher_gui_account.c - Pilotenkonto steelnet (W328/W399/W418) + LoC-Testpartner-Einbindung (W336).
 * W462 LAUNCHER-SPLIT: aus launcher_gui.c verschoben (unveraendert), gemeinsamer Zustand
 * in launcher_gui_int.h. */
#include "launcher_gui_int.h"
#include "launcher_camp.h" /* W491: LCAMP_R_* (Shot-Zustaende) */
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
/* W418 LAUNCHER-ACCOUNT-UX: Unterseite Multiplayer > Account ("steelnet account").
 * W502 ACCOUNT-PAGE: neu gegliedert, eine Seite ohne Aufklapper-Textlinks:
 *   1. Uebersicht "Was du spielen kannst": Online (System Link ueber steelnet), Xbox Live & Kampagne (Gamertag),
 *      Konto gesichert - je Zeile Haken bzw. Hinweis und rechts ein echter Knopf zur naechsten Aktion.
 *   2. Gamertag-Karte (mit Konto direkt nach der Uebersicht): Feld + Setzen bzw. Anzeige + "Aendern", 7-Tage-Regel
 *      einmal.
 *   3. unten links "Konto sichern" (Discord, Login, Wiederherstellungscode als gleichwertige Zeilen; das Login-Formular
 *      ersetzt die Zeilen in der Karte), rechts "Kontoeinstellungen" (Code einloesen, Abmelden, Loeschen rot abgesetzt).
 *   Kein Konto: Uebersicht, darunter "Konto erstellen" (Name, Gamertag optional) | "Ich habe schon ein Konto".
 *   Datenschutzzeile unveraendert am Seitenende. Lage je Zustand in acc_geo() (eine Rechnung fuer Elemente und Bild). */
#define ACC_CHECK_MS 700
#define ACC_GAP 10           /* Abstand zwischen Karten */
#define ACC_OV_ROW 32        /* Zeile der Uebersicht */
#define ACC_OV_TOP 42        /* erste Zeile unter dem Kartentitel */
#define ACC_OV_H (ACC_OV_TOP + 3 * ACC_OV_ROW + 8)
#define ACC_OV_BTN_W 160     /* Knopf rechts in der Uebersicht */
#define ACC_OV_LBL_W 184     /* Spalte Bezeichnung (nach dem Symbol) */
#define ACC_GT_H 92          /* Gamertag-Karte */
#define ACC_SEC_W 420        /* Karte Sichern (links); rechts Kontoeinstellungen */
#define ACC_ROW_BTN_W 170    /* Knopf rechts in einer Zeile der Karte Sichern */
#define ACC_NONE_H 262       /* kein Konto: Karten Erstellen | Schon ein Konto */
#define ACC_HL_MS 1600       /* Hervorhebung nach einem Sprung */



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
int g_acc_x_login, g_acc_x_code; /* W502: eingebettete Formulare (Login | Code) statt Aufklapper */
int g_acc_focus_id;        /* W502: zuletzt angeforderter Fokus (Selbsttest; im Shot/Selbsttest kein SetFocus) */
int g_acc_hl;              /* W502: hervorgehobene Karte nach einem Sprung (ACC_CARD_*) */
char g_acc_st_err[24];
char g_acc_fmsg[200];
int g_acc_fmsg_id;         /* Feld der Meldung (ID_ACC_NAME, ID_ACC_V_CODE, ID_ACC_V_LOGIN), 0 = keine */
COLORREF g_acc_fmsg_col;
/* W487 LIVE-GAMERTAG: Gamertag beim Erstellen (Vorschlag des Dienstes, solange der Spieler das Feld nicht selbst
 * geaendert hat) und Gamertag-Karte der Kontoseite. Eigene Meldung am Gamertag-Feld (neben der Namensmeldung).
 * W502: g_acc_x_gt = "Aendern" offen (nur mit Gamertag; ohne Gamertag steht das Feld immer da). */
int g_acc_x_gt;
int g_acc_gt_user;         /* 1 = Spieler hat das Gamertag-Feld beim Erstellen selbst geaendert (kein Vorschlag mehr) */
char g_acc_gtmsg[200];
int g_acc_gtmsg_id;        /* ID_ACC_GT / ID_ACC_V_GT, 0 = keine */
static COLORREF g_acc_gtmsg_col;
static int g_acc_gt_tick_fid; /* Feld der laufenden Entprellung */
int g_acc_live_pending;    /* 1 = Live-Start wartet auf den Status (Gamertag) */
#define ACC_GT_ID(id) ((id) >= ID_ACC_GT && (id) <= ID_ACC_V_GTBTN)

static void acc_gtmsg(int id, const char *t, COLORREF c)
{
    g_acc_gtmsg_id = t && *t ? id : 0;
    strncpy_s(g_acc_gtmsg, sizeof g_acc_gtmsg, t ? t : "", _TRUNCATE);
    g_acc_gtmsg_col = c;
}

static const char *acc_gtmsg_for(int id, COLORREF *c)
{
    if (g_acc_gtmsg_id != id || !g_acc_gtmsg[0]) {
        return NULL;
    }
    *c = g_acc_gtmsg_col;
    return g_acc_gtmsg;
}

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

/* W418/W502: Lage der Kontoseite je Zustand (eine Rechnung fuer Steuerelemente und Zeichnen). Alle y logisch;
 * -1 = Teil nicht da. Die Seite passt ohne Rollen ueber die Datenschutzzeile (ACC_PRIV_Y). */
void acc_geo(acc_geo_t *g)
{
    memset(g, 0xff, sizeof *g); /* alles -1 */
    g->st = acc_state();
    if (g->st == ACCS_OFF) {
        return;
    }
    g->ov_y = CY0;
    g->ov_h = ACC_OV_H;
    if (g->st == ACCS_NONE) { /* Erstellen | Schon ein Konto */
        g->lo_y = g->ov_y + g->ov_h + ACC_GAP;
        g->lo_h = ACC_NONE_H;
        g->lx = CX0;
        g->lw = COLW;
        g->rx = COL2;
        g->rw = COLW;
        return;
    }
    g->gt_y = g->ov_y + g->ov_h + ACC_GAP;
    g->gt_h = ACC_GT_H;
    g->lo_y = g->gt_y + g->gt_h + ACC_GAP;
    g->lo_h = ACC_PRIV_Y - 8 - g->lo_y;
    g->lx = CX0;
    g->lw = ACC_SEC_W;
    g->rx = CX0 + ACC_SEC_W + 12;
    g->rw = CW - ACC_SEC_W - 12;
    g->r_dc = g->lo_y + 42;
    g->r_lg = g->r_dc + 46;
    g->r_rec = g->r_lg + 46;
}

/* W502: gezeichnete Karten der Kontoseite (Selbsttest: jedes sichtbare Element liegt in einer Karte). Anzahl. */
int acc_page_cards(RECT *r, int *kind, int cap)
{
    acc_geo_t g;
    int n = 0;
    acc_geo(&g);
#define ACC_CARD_ADD(k, x, y, w, h)                                                                                    \
    do {                                                                                                               \
        if (n < cap) {                                                                                                 \
            SetRect(&r[n], (x), (y), (x) + (w), (y) + (h));                                                            \
            kind[n] = (k);                                                                                             \
        }                                                                                                              \
        n++;                                                                                                           \
    } while (0)
    if (g.st == ACCS_OFF) {
        ACC_CARD_ADD(ACC_CARD_OFF, CX0, CY0, CW, 88);
        return n < cap ? n : cap;
    }
    ACC_CARD_ADD(ACC_CARD_OV, CX0, g.ov_y, CW, g.ov_h);
    if (g.gt_y >= 0) {
        ACC_CARD_ADD(ACC_CARD_GT, CX0, g.gt_y, CW, g.gt_h);
    }
    ACC_CARD_ADD(g.st == ACCS_NONE ? ACC_CARD_NEW : ACC_CARD_SEC, g.lx, g.lo_y, g.lw, g.lo_h);
    ACC_CARD_ADD(g.st == ACCS_NONE ? ACC_CARD_HAVE : ACC_CARD_SET, g.rx, g.lo_y, g.rw, g.lo_h);
#undef ACC_CARD_ADD
    return n < cap ? n : cap;
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

/* W418: Formulare zu (neuer Kontozustand: Anlegen, Anmelden, Abmelden, Loeschen) */
void acc_x_reset(void)
{
    g_acc_x_login = g_acc_x_code = 0;
    g_acc_x_gt = 0; /* W487 */
    g_acc_hl = 0;   /* W502 */
    g_acc_gt_user = 0;
    acc_fmsg(0, "", 0);
    acc_gtmsg(0, "", 0);
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
        { "gamertag_off", "gui.acc_gt_off" },   { "invalid_gamertag", "gui.acc_gt_rules" }, /* W487 */
        { "gamertag_refused", "gui.acc_gt_refused" },
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

/* W487: Datum (lokal, JJJJ-MM-TT) eines Unix-Zeitpunkts */
static void acc_gt_date(long long t, char *out, size_t cap)
{
    struct tm tm;
    time_t tt = (time_t)t;
    if (t <= 0 || localtime_s(&tm, &tt) != 0) {
        strcpy_s(out, cap, "?");
        return;
    }
    strftime(out, cap, "%Y-%m-%d", &tm);
}

/* W487: Gamertag-Statuscode (gamertag_check, gamertag_refused, gamertag_error) -> Text; next_at fuer change_limit */
void acc_gt_status_text(const char *status, long long next_at, const char *name, char *out, size_t cap)
{
    static const struct {
        const char *st, *key;
    } kSt[] = {
        { "ok", "gui.acc_gt_st_ok" },
        { "taken", "gui.acc_gt_st_taken" },
        { "similar", "gui.acc_gt_st_similar" },
        { "reserved", "gui.acc_gt_st_reserved" },
        { "blocked", "gui.acc_gt_st_blocked" },
        { "invalid_length", "gui.acc_gt_st_invalid_length" },
        { "invalid_characters", "gui.acc_gt_st_invalid_characters" },
        { "change_limit", "gui.acc_gt_st_change_limit" },
        { "unchanged", "gui.acc_gt_st_unchanged" },
        { "off", "gui.acc_gt_off" },
        { "unavailable", "gui.acc_gt_st_unavailable" },
    };
    const char *k = NULL;
    char d[32];
    int i;
    for (i = 0; status && i < (int)(sizeof kSt / sizeof kSt[0]); i++) {
        if (strcmp(status, kSt[i].st) == 0) k = kSt[i].key;
    }
    if (!k) {
        strncpy_s(out, cap, LT("gui.acc_gt_refused"), _TRUNCATE);
        return;
    }
    if (strcmp(status, "change_limit") == 0) {
        acc_gt_date(next_at, d, sizeof d);
        _snprintf_s(out, cap, _TRUNCATE, LT(k), d);
    } else if (strcmp(status, "ok") == 0 || strcmp(status, "taken") == 0 || strcmp(status, "similar") == 0) {
        _snprintf_s(out, cap, _TRUNCATE, LT(k), name && name[0] ? name : "?");
    } else {
        strncpy_s(out, cap, LT(k), _TRUNCATE);
    }
}

/* W487: Fehler eines Gamertag-Auftrags (create/gamertag_set/gamertag_check) -> Text */
static void acc_gt_err_text(const acc_job_t *j, char *out, size_t cap)
{
    const char *nm = j->op == ACC_OP_CREATE ? j->c : j->a;
    if (strcmp(j->err, "gamertag_refused") == 0 && j->gts.status[0]) {
        acc_gt_status_text(j->gts.status, j->gts.next_at, nm, out, cap);
    } else if (j->op != ACC_OP_CREATE && (strcmp(j->err, "not_found") == 0 || strcmp(j->err, "bad_request") == 0)) {
        strncpy_s(out, cap, LT("gui.acc_gt_off"), _TRUNCATE); /* aelterer Dienst ohne Gamertag-Aktionen */
    } else {
        acc_err_text(j->err, out, cap);
    }
}

/* W502: Gamertag-Aenderung laut letztem Status gesperrt (7-Tage-Regel; der Dienst prueft selbst) */
static int acc_gt_locked(void)
{
    return g_acc_st_ok && g_acc_st.gamertag[0] && g_acc_st.gt_next > (long long)time(NULL);
}

void acc_refresh(void)
{
    if (!g_hwnd) {
        return;
    }
    acc_layout(); /* W399/W418/W502: Lage je Zustand und Formular */
    ui_refresh_visibility();
    nav_sub_layout(); /* W399: Markierung des Unterpunkts */
    {
        static const int kBtn[] = { ID_ACC_CREATE, ID_ACC_SIGNIN, ID_ACC_DISCORD, ID_ACC_MANAGE,
                                    ID_ACC_V_DISCORD, ID_ACC_V_LOGINBTN, ID_ACC_V_CODEBTN, ID_ACC_X_LOGIN,
                                    ID_ACC_X_CODE, ID_ACC_V_DELETE, ID_ACC_V_FORGET, ID_ACC_V_GTBTN, ID_ACC_X_GT,
                                    ID_ACC_O_ONLINE };
        int i;
        for (i = 0; i < (int)(sizeof kBtn / sizeof kBtn[0]); i++) {
            HWND b = ctl(kBtn[i]);
            BOOL en = !g_acc_busy;
            if ((kBtn[i] == ID_ACC_DISCORD || kBtn[i] == ID_ACC_V_DISCORD) && g_acc.have && g_acc_st_ok &&
                g_acc_st.discord) {
                en = FALSE; /* schon verbunden */
            }
            if (kBtn[i] == ID_ACC_X_GT && acc_gt_locked()) {
                en = FALSE; /* W502: naechste Aenderung erst am Datum der Karte */
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
        if (ctl(ID_ACC_X_LOGIN)) { /* W502: Knopf in der Karte (kein Textlink) */
            ui_settext(ctl(ID_ACC_X_LOGIN), LT(st == ACCS_NONE ? "gui.acc_b_login_in"
                                               : lg            ? "gui.acc_b_login_chg"
                                                               : "gui.acc_b_login_set"));
        }
        if (ctl(ID_LOC_MP_ACC)) { /* W418: Karte Multiplayer (Spielen) */
            ui_settext(ctl(ID_LOC_MP_ACC), LT(g_acc.have ? "gui.acc_mp_go" : "gui.acc_mp_new"));
        }
        if (ctl(ID_ACC_V_GTBTN)) {
            ui_settext(ctl(ID_ACC_V_GTBTN), LT(g_acc_st_ok && g_acc_st.gamertag[0] ? "gui.acc_gt_change" : "gui.acc_gt_set"));
        }
        if (ctl(ID_ACC_O_ONLINE)) { /* W502: Uebersicht, Zeile Online */
            ui_settext(ctl(ID_ACC_O_ONLINE), LT(st == ACCS_NONE ? "gui.acc_create" : "gui.acc_ov_retry"));
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
    case ACC_OP_CREATE: j->rc = lacc_op_create_gt(&j->srv, j->dir, j->a, j->c, &j->st, &j->gts, e, ec); break; /* W487 */
    case ACC_OP_GT_CHECK:
        j->rc = lacc_op_gamertag_check(&j->srv, j->a[0] ? j->a : NULL, j->c[0] ? j->c : NULL, &j->gtc, e, ec);
        break;
    case ACC_OP_GT_SET: j->rc = lacc_op_gamertag_set(&j->srv, &j->st, j->a, &j->gts, e, ec); break;
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

/* Auftrag starten (a/b/c duerfen NULL sein; W487: c = Gamertag beim Erstellen bzw. Vorschlagsbasis, fid = Feld der
 * Gamertag-Pruefung). 0 = gestartet. */
static int acc_start3(int op, const char *a, const char *b, const char *c, int fid)
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
    j->fid = fid;
    if (a) strcpy_s(j->a, sizeof j->a, a);
    if (b) strcpy_s(j->b, sizeof j->b, b);
    if (c) strncpy_s(j->c, sizeof j->c, c, _TRUNCATE);
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
    if (op != ACC_OP_STATUS && op != ACC_OP_CHECK && op != ACC_OP_GT_CHECK) { /* W418/W487: Pruefungen still */
        set_status_k(LT(op == ACC_OP_DISCORD_LINK || op == ACC_OP_DISCORD_LOGIN ? "gui.acc_wait_discord"
                                                                                  : "gui.acc_working"), 0);
    }
    acc_refresh();
    return 0;
}

static int acc_start(int op, const char *a, const char *b)
{
    return acc_start3(op, a, b, NULL, 0);
}

/* W487: Gamertag-Feld setzen, ohne es als Spieler-Aenderung zu zaehlen */
static void acc_gt_settext(int id, const char *t)
{
    if (!ctl(id)) return;
    g_updating++;
    ui_settext(ctl(id), t);
    g_updating--;
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
            if (g_acc_live_pending == 1) { /* W487/W501: Start wartete auf den Status - trotzdem starten (System Link) */
                g_acc_live_pending = 2;
                acc_refresh();
                loctab_start();
                return;
            }
        } else if (op == ACC_OP_CHECK) {
            acc_fmsg(0, "", 0); /* W418: stille Pruefung - ohne Antwort keine Meldung (Erstellen prueft erneut) */
        } else if (op == ACC_OP_GT_CHECK) { /* W487: still, nur "Dienst ohne Gamertags" zeigen */
            acc_gtmsg(j->fid, strcmp(j->err, "gamertag_off") == 0 ? e : "", C_WARN);
        } else {
            int gtf = op == ACC_OP_GT_SET || (op == ACC_OP_CREATE && (strcmp(j->err, "gamertag_refused") == 0 ||
                                                                       strcmp(j->err, "invalid_gamertag") == 0));
            if (gtf) acc_gt_err_text(j, e, sizeof e); /* W487: Grund der Ablehnung (vergeben, aehnlich, Grenze ...) */
            set_status_k(e, 2);
            /* W418: Fehler direkt unter dem betroffenen Feld */
            if (op == ACC_OP_CREATE && gtf) acc_gtmsg(ID_ACC_GT, e, C_ERR);
            else if (op == ACC_OP_GT_SET) acc_gtmsg(ID_ACC_V_GT, e, C_ERR);
            else if (op == ACC_OP_CREATE) acc_fmsg(ID_ACC_NAME, e, C_ERR);
            else if (op == ACC_OP_TRANSFER || op == ACC_OP_RECOVERY) acc_fmsg(ID_ACC_V_CODE, e, C_ERR);
            else if (op == ACC_OP_LOGIN || op == ACC_OP_LOGIN_SET) acc_fmsg(ID_ACC_V_LOGIN, e, C_ERR);
            if (op == ACC_OP_GT_SET && strcmp(j->err, "gamertag_refused") == 0 && j->gts.next_at > 0 && g_acc_st_ok)
                g_acc_st.gt_next = j->gts.next_at; /* Grenze vom Dienst uebernehmen */
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
    case ACC_OP_GT_CHECK: { /* W487: Ergebnis unter dem Feld, nur wenn der Text noch im Feld steht */
        char cur[80] = "", bas[80] = "";
        if (ctl(j->fid)) ui_gettext(ctl(j->fid), cur, sizeof cur);
        if (j->a[0]) { /* Pruefung eines Namens */
            if (strcmp(cur, j->a) == 0) {
                acc_gt_status_text(j->gtc.status, 0, j->a, m, sizeof m);
                acc_gtmsg(j->fid, m, j->gtc.available ? C_OK : C_ERR);
            }
        } else if (j->fid == ID_ACC_GT && !g_acc_gt_user && j->gtc.suggestion[0]) { /* Vorschlag zum Kontonamen */
            if (ctl(ID_ACC_NAME)) ui_gettext(ctl(ID_ACC_NAME), bas, sizeof bas);
            if (strcmp(bas, j->c) == 0) {
                acc_gt_settext(ID_ACC_GT, j->gtc.suggestion);
                acc_gt_status_text("ok", 0, j->gtc.suggestion, m, sizeof m);
                acc_gtmsg(ID_ACC_GT, m, C_OK);
            }
        }
        break;
    }
    case ACC_OP_GT_SET:
        strcpy_s(g_acc_st.gamertag, sizeof g_acc_st.gamertag, j->gts.gamertag);
        g_acc_st.gt_next = j->gts.next_at;
        acc_gt_settext(ID_ACC_V_GT, "");
        _snprintf_s(m, sizeof m, _TRUNCATE, LT("gui.acc_gt_done"), j->gts.gamertag);
        acc_gtmsg(ID_ACC_V_GT, m, C_OK);
        set_status_k(m, 1);
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
        if (op == ACC_OP_CREATE && j->c[0] && (j->gts.status[0] || !j->gts.gamertag[0])) {
            /* W487: Konto angelegt, Gamertag nicht gesetzt (Dienst ohne Gamertags/Kern nicht erreichbar) */
            _snprintf_s(m, sizeof m, _TRUNCATE, LT("gui.acc_created_nogt"), g_acc.name);
            set_status_k(m, 2);
        } else if (op == ACC_OP_CREATE && j->gts.gamertag[0]) { /* W487: mit Gamertag */
            _snprintf_s(m, sizeof m, _TRUNCATE, LT("gui.acc_created_gt"), g_acc.name, j->gts.gamertag);
            set_status_k(m, 1);
        } else {
            set_status_k(m, 1);
        }
        if (op == ACC_OP_CREATE) acc_gt_settext(ID_ACC_GT, "");
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
    /* W487: Namenspruefung ok -> Gamertag-Vorschlag zum Kontonamen (solange der Spieler das Feld nicht geaendert hat) */
    if (op == ACC_OP_CHECK && !g_acc.have && !g_acc_gt_user && lacc_valid_name(j->a)) {
        char cur[80] = "";
        if (ctl(ID_ACC_NAME)) ui_gettext(ctl(ID_ACC_NAME), cur, sizeof cur);
        if (strcmp(cur, j->a) == 0) acc_start3(ACC_OP_GT_CHECK, NULL, NULL, j->a, ID_ACC_GT);
    }
    /* W487/W501: Start wartete auf den Status (Gamertag) - jetzt starten (mit Gamertag auch Xbox Live, sonst System Link) */
    if (op == ACC_OP_STATUS && g_acc_live_pending == 1) {
        g_acc_live_pending = 2; /* acc_live_prestart laesst den Start dann ohne neue Abfrage durch */
        loctab_start();
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

/* W418: roter Knopf "Konto loeschen" (W502: die Aufklapper-Textlinks entfielen, alles echte Knoepfe) */
int acc_danger_id(int id)
{
    return id == ID_ACC_V_DELETE;
}

/* W502: Sichtbarkeit der Elemente der Kontoseite mit Konto (Uebersicht, Gamertag, Sichern, Einstellungen) */
static int acc_vis_have(int id, int st)
{
    int ok = g_acc_st_ok, gt = ok && g_acc_st.gamertag[0];
    int gtoff = ok && !gt && (strcmp(g_acc_st.gt_state, "off") == 0 || strcmp(g_acc_st.gt_state, "unavailable") == 0);
    switch (id) {
    case ID_ACC_O_ONLINE: return st == ACCS_UNK && netm_get() == NETM_STEEL; /* Erneut versuchen */
    case ID_ACC_O_LIVE: return ok && !gt && !gtoff;                         /* Gamertag waehlen */
    case ID_ACC_O_SEC: return st == ACCS_UNSEC;                              /* Konto sichern */
    case ID_ACC_V_GT: case ID_ACC_V_GTBTN: return ok && !gtoff && (!gt || g_acc_x_gt);
    case ID_ACC_X_GT: return gt && !g_acc_x_gt;
    case ID_ACC_V_GTCANCEL: return gt && g_acc_x_gt;
    case ID_ACC_V_LOGIN: case ID_ACC_V_PASS: case ID_ACC_V_LOGINBTN: case ID_ACC_V_LGCANCEL: return g_acc_x_login;
    case ID_ACC_V_DISCORD: return !g_acc_x_login && !(ok && g_acc_st.discord); /* verbunden -> nur Zustand */
    case ID_ACC_X_LOGIN: case ID_ACC_V_RECSHOW: case ID_ACC_V_RECCOPY: return !g_acc_x_login;
    case ID_ACC_V_CODE: case ID_ACC_V_CODEBTN: case ID_ACC_V_CDCANCEL: return g_acc_x_code;
    case ID_ACC_X_CODE: case ID_ACC_V_FORGET: case ID_ACC_V_DELETE: return !g_acc_x_code;
    }
    return 0;
}

/* W502: kein Konto - Erstellen (links) | Schon ein Konto (rechts: Knoepfe bzw. Login- oder Codeformular) */
static int acc_vis_none(int id)
{
    int form = g_acc_x_login || g_acc_x_code;
    switch (id) {
    case ID_ACC_NAME: case ID_ACC_GT: case ID_ACC_CREATE: case ID_ACC_O_ONLINE: return 1;
    case ID_ACC_V_DISCORD: case ID_ACC_X_LOGIN: case ID_ACC_X_CODE: return !form;
    case ID_ACC_V_LOGIN: case ID_ACC_V_PASS: case ID_ACC_V_LOGINBTN: case ID_ACC_V_LGCANCEL: return g_acc_x_login;
    case ID_ACC_V_CODE: case ID_ACC_V_CODEBTN: case ID_ACC_V_CDCANCEL: return g_acc_x_code;
    }
    return 0;
}

/* Sichtbarkeit (-1 = nicht zustaendig) */
int acc_visible(int id, int page)
{
    int on, have, view, st;
    if (ACC_GT_ID(id) || ACC_W502_ID(id)) { /* W487/W502: nur auf der Kontoseite */
        if (g_game != LGAME_LOC || page != P_LNET || !g_acc_view || !acc_srv_on()) {
            return 0;
        }
        st = acc_state();
        return st == ACCS_NONE ? acc_vis_none(id) : g_acc.have ? acc_vis_have(id, st) : 0;
    }
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
    /* W502: Kontoseite - nur die Elemente des Zustands und des offenen Formulars */
    st = acc_state();
    if (st == ACCS_NONE) {
        return acc_vis_none(id);
    }
    return have ? acc_vis_have(id, st) : 0;
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
/* W502: Lage innerhalb der Karten der Kontoseite (relativ zur Kartenoberkante) */
#define ACC_N_NAME 80   /* kein Konto, Erstellen: Feld Kontoname (Beschriftung -18, Meldung +30) */
#define ACC_N_GT 152    /* Feld Gamertag (Beschriftung -18, Meldung +30, zwei Zeilen) */
#define ACC_N_BTN 218   /* Erstellen */
#define ACC_F_L1 80     /* Formular "Schon ein Konto": erstes Feld (Beschriftung -18) */
#define ACC_F_L2 132    /* zweites Feld (Passwort) */
#define ACC_G_ROW 40    /* Gamertag-Karte: Feld/Knoepfe bzw. Anzeige */
#define ACC_S_FLD 60    /* Sichern, Login-Formular: Felder (Beschriftung -18) */
#define ACC_S_BTN 140   /* Sichern, Login-Formular: Knoepfe */
#define ACC_E_FLD 58    /* Einstellungen, Codeformular: Feld (Beschriftung -18) */
#define ACC_E_BTN 94    /* Einstellungen, Codeformular: Knoepfe */
#define ACC_E_CODE 62   /* Einstellungen: Code einloesen */
#define ACC_E_OUT 98    /* Abmelden */
#define ACC_E_SEP 140   /* Trennlinie */
#define ACC_E_DEL 152   /* Loeschen */
/* W399/W418/W502: Lage der Konto-Elemente je Ansicht (Karte auf Match & network bzw. Kontoseite), Zustand und
 * Formular (acc_geo). Unsichtbare Elemente behalten ihre letzte Lage. */
void acc_layout(void)
{
    acc_geo_t g;
    uictl_t *dc = ui_find(ID_ACC_V_DISCORD);
    int x0 = CX0 + 18, xr = CX0 + CW - 18, i;
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
    if (dc) {
        dc->kind = g.st == ACCS_UNSEC ? K_ACCENT : K_BTN; /* ein Hauptknopf je Zustand */
    }
    if (g.st == ACCS_OFF) {
        return;
    }
    { /* Uebersicht: je Zeile ein Knopf rechts */
        static const int kOv[3] = { ID_ACC_O_ONLINE, ID_ACC_O_LIVE, ID_ACC_O_SEC };
        for (i = 0; i < 3; i++) {
            acc_put(kOv[i], xr - ACC_OV_BTN_W, g.ov_y + ACC_OV_TOP + i * ACC_OV_ROW + 2, ACC_OV_BTN_W, 28);
        }
    }
    if (g.st == ACCS_NONE) {
        int nx = g.lx + 18, nw = g.lw - 36, hx = g.rx + 18, hw = g.rw - 36, y = g.lo_y;
        acc_put(ID_ACC_NAME, nx, y + ACC_N_NAME, nw, 28);
        acc_put(ID_ACC_GT, nx, y + ACC_N_GT, nw, 28); /* W487 */
        acc_put(ID_ACC_CREATE, nx, y + ACC_N_BTN, 180, 30);
        acc_put(ID_ACC_V_DISCORD, hx, y + 62, hw, 30);
        acc_put(ID_ACC_X_LOGIN, hx, y + 100, hw, 28);
        acc_put(ID_ACC_X_CODE, hx, y + 136, hw, 28);
        acc_put(ID_ACC_V_LOGIN, hx, y + ACC_F_L1, hw, 28);
        acc_put(ID_ACC_V_PASS, hx, y + ACC_F_L2, hw, 28);
        acc_put(ID_ACC_V_LOGINBTN, hx, y + 172, 150, 28);
        acc_put(ID_ACC_V_LGCANCEL, hx + 158, y + 172, 110, 28);
        acc_put(ID_ACC_V_CODE, hx, y + ACC_F_L1, hw, 28);
        acc_put(ID_ACC_V_CODEBTN, hx, y + 116, 150, 28);
        acc_put(ID_ACC_V_CDCANCEL, hx + 158, y + 116, 110, 28);
        return;
    }
    { /* Gamertag-Karte: Feld + Setzen (+ Abbrechen beim Aendern) bzw. Anzeige + Aendern */
        int y = g.gt_y + ACC_G_ROW;
        acc_put(ID_ACC_V_GT, x0, y, 240, 28);
        acc_put(ID_ACC_V_GTBTN, x0 + 250, y, 160, 28);
        acc_put(ID_ACC_V_GTCANCEL, x0 + 418, y, 110, 28);
        acc_put(ID_ACC_X_GT, xr - 150, y, 150, 28);
    }
    { /* Sichern: Zeilen Discord, Login, Wiederherstellungscode (Knoepfe rechts) bzw. Login-Formular */
        int sx = g.lx + 18, sxr = g.lx + g.lw - 18, fw = (g.lw - 36 - 12) / 2;
        acc_put(ID_ACC_V_DISCORD, sxr - ACC_ROW_BTN_W, g.r_dc + 4, ACC_ROW_BTN_W, 28);
        acc_put(ID_ACC_X_LOGIN, sxr - ACC_ROW_BTN_W, g.r_lg + 4, ACC_ROW_BTN_W, 28);
        acc_put(ID_ACC_V_RECSHOW, sxr - 166, g.r_rec, 80, 26);
        acc_put(ID_ACC_V_RECCOPY, sxr - 80, g.r_rec, 80, 26);
        acc_put(ID_ACC_V_LOGIN, sx, g.lo_y + ACC_S_FLD, fw, 28);
        acc_put(ID_ACC_V_PASS, sx + fw + 12, g.lo_y + ACC_S_FLD, fw, 28);
        acc_put(ID_ACC_V_LOGINBTN, sx, g.lo_y + ACC_S_BTN, ACC_ROW_BTN_W, 28);
        acc_put(ID_ACC_V_LGCANCEL, sx + ACC_ROW_BTN_W + 8, g.lo_y + ACC_S_BTN, 110, 28);
    }
    { /* Kontoeinstellungen: Code einloesen, Abmelden, Loeschen bzw. Codeformular */
        int tx = g.rx + 18, tw = g.rw - 36, bw = (tw - 8) / 2;
        acc_put(ID_ACC_X_CODE, tx, g.lo_y + ACC_E_CODE, tw, 28);
        acc_put(ID_ACC_V_FORGET, tx, g.lo_y + ACC_E_OUT, tw, 28);
        acc_put(ID_ACC_V_DELETE, tx, g.lo_y + ACC_E_DEL, tw, 28);
        acc_put(ID_ACC_V_CODE, tx, g.lo_y + ACC_E_FLD, tw, 28);
        acc_put(ID_ACC_V_CODEBTN, tx, g.lo_y + ACC_E_BTN, bw, 28);
        acc_put(ID_ACC_V_CDCANCEL, tx + bw + 8, g.lo_y + ACC_E_BTN, bw, 28);
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

/* ------------------------------------------------------------------ */
/* W487 LIVE-GAMERTAG: Gamertag beim Erstellen, Gamertag-Karte, Live-Start (W501: acc_gt_open entfiel mit der   */
/* Startsperre; Weg zum Gamertag = Multiplayer > Konto bzw. "Konto verwalten")                                    */
/* ------------------------------------------------------------------ */
/* W502: Fokus auf ein Element der Kontoseite (nur im eigenen Fenster, nie Vordergrund; Shot/Selbsttest: nur merken) */
static void acc_focus(int id)
{
    g_acc_focus_id = id;
    if (g_o.shot == 0 && !g_in_gui_selftest && ctl(id) && IsWindowVisible(ctl(id)) && IsWindowEnabled(ctl(id))) {
        SetFocus(ctl(id));
    }
}

/* W502: Zielkarte nach einem Sprung kurz hervorheben (Akzentrahmen, endet per ACC_HL_TIMER) */
static void acc_highlight(int card)
{
    g_acc_hl = card;
    if (g_hwnd && g_o.shot == 0 && !g_in_gui_selftest) {
        SetTimer(g_hwnd, ACC_HL_TIMER, ACC_HL_MS, NULL);
    }
    redraw_all();
}

void acc_hl_tick(void)
{
    if (g_hwnd) {
        KillTimer(g_hwnd, ACC_HL_TIMER);
    }
    if (g_acc_hl) {
        g_acc_hl = 0;
        redraw_all();
    }
}

/* W502: Einsprung "Kontoseite oeffnen und Gamertag fokussieren" (auch fuer andere Seiten, z. B. W501). Ohne Gamertag
 * steht das Feld immer in der Karte; mit Gamertag liegt der Fokus auf "Aendern" (die 7-Tage-Regel bleibt sichtbar).
 * Ohne Konto: Kontoseite mit Feld Kontoname (der Gamertag gehoert dort zum Erstellen). */
void acc_open_gamertag(void)
{
    acc_load();
    g_acc_x_login = 0;
    g_acc_x_code = 0;
    if (!g_acc_view) {
        acc_open_view(1);
    }
    show_page(P_LNET);
    acc_refresh();
    if (acc_state() == ACCS_NONE) {
        acc_highlight(ACC_CARD_NEW);
        acc_focus(ID_ACC_NAME);
        return;
    }
    acc_highlight(ACC_CARD_GT);
    acc_focus(g_acc_st_ok && g_acc_st.gamertag[0] && !g_acc_x_gt ? ID_ACC_X_GT : ID_ACC_V_GT);
}

static int acc_gt_command(int id, int code)
{
    char a[80];
    if (id == ID_ACC_GT || id == ID_ACC_V_GT) {
        if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
            g_focus_id = code == EN_SETFOCUS ? id : 0;
            redraw_all();
        } else if (code == EN_CHANGE && !g_updating) {
            if (id == ID_ACC_GT) { /* Spieler tippt selbst: kein Vorschlag mehr; leer = wieder Vorschlag */
                ui_gettext(ctl(id), a, sizeof a);
                g_acc_gt_user = a[0] != '\0';
            }
            if (g_acc_gtmsg_id == id) {
                acc_gtmsg(0, "", 0);
                redraw_all();
            }
            g_acc_gt_tick_fid = id;
            if (g_hwnd) {
                SetTimer(g_hwnd, ACC_GT_TIMER, ACC_CHECK_MS, NULL);
            }
        }
        return 1;
    }
    if (code != BN_CLICKED) {
        return 1;
    }
    if (id == ID_ACC_X_GT || id == ID_ACC_V_GTCANCEL) { /* W502: Aendern oeffnet das Feld in der Karte, Abbrechen schliesst */
        g_acc_x_gt = id == ID_ACC_X_GT;
        acc_gt_settext(ID_ACC_V_GT, "");
        if (g_hwnd) KillTimer(g_hwnd, ACC_GT_TIMER);
        if (g_acc_gtmsg_id == ID_ACC_V_GT) acc_gtmsg(0, "", 0);
        acc_refresh();
        acc_focus(g_acc_x_gt ? ID_ACC_V_GT : ID_ACC_X_GT);
        return 1;
    }
    if (id == ID_ACC_V_GTBTN) {
        char q[400], d[32];
        ui_gettext(ctl(ID_ACC_V_GT), a, sizeof a);
        if (g_hwnd) KillTimer(g_hwnd, ACC_GT_TIMER);
        if (!lacc_valid_gamertag(a)) {
            acc_gtmsg(ID_ACC_V_GT, LT("gui.acc_gt_rules"), C_ERR);
            set_status_k(LT("gui.acc_gt_rules"), 2);
            redraw_all();
            return 1;
        }
        if (g_acc_st_ok && g_acc_st.gamertag[0]) {
            if (strcmp(a, g_acc_st.gamertag) == 0) {
                acc_gt_status_text("unchanged", 0, a, q, sizeof q);
                acc_gtmsg(ID_ACC_V_GT, q, C_WARN);
                redraw_all();
                return 1;
            }
            if (g_acc_st.gt_next > (long long)time(NULL)) { /* Grenze laut letztem Status (Dienst prueft selbst) */
                acc_gt_status_text("change_limit", g_acc_st.gt_next, a, q, sizeof q);
                acc_gtmsg(ID_ACC_V_GT, q, C_ERR);
                set_status_k(q, 2);
                redraw_all();
                return 1;
            }
            acc_gt_date((long long)time(NULL) + 86400LL * (g_acc_st.gt_days > 0 ? g_acc_st.gt_days : 7), d, sizeof d);
            _snprintf_s(q, sizeof q, _TRUNCATE, LT("gui.acc_gt_change_q"), g_acc_st.gamertag, a, d);
            if (!ui_confirm(q, LT("gui.acc_gt_title"))) {
                return 1;
            }
        }
        acc_start3(ACC_OP_GT_SET, a, NULL, NULL, ID_ACC_V_GT);
        return 1;
    }
    return 1;
}

/* W487: entprellte Gamertag-Pruefung (gamertag_check, still). Ungueltige Namen meldet die Seite ohne Netz; die
 * Abfrage laeuft nur mit Stufe "Mit steelnet verbinden" (wie die Namenspruefung). */
void acc_gt_tick(void)
{
    char a[80] = "";
    int fid = g_acc_gt_tick_fid;
    if (g_hwnd) {
        KillTimer(g_hwnd, ACC_GT_TIMER);
    }
    if (!acc_srv_on() || !ctl(fid) || (fid != ID_ACC_GT && fid != ID_ACC_V_GT)) {
        return;
    }
    ui_gettext(ctl(fid), a, sizeof a);
    if (!a[0]) {
        if (fid == ID_ACC_GT) { /* leer: wieder Vorschlag zum Kontonamen (beim naechsten Namens-Tick) */
            g_acc_gt_user = 0;
        }
        return;
    }
    if (!lacc_valid_gamertag(a)) {
        acc_gtmsg(fid, LT("gui.acc_gt_rules"), C_ERR);
        redraw_all();
        return;
    }
    if (netm_get() != NETM_STEEL && !g_acc_fixture) {
        return; /* ohne steelnet: Erstellen/Setzen prueft */
    }
    if (acc_start3(ACC_OP_GT_CHECK, a, NULL, NULL, fid) == 0) {
        acc_gtmsg(fid, LT("gui.acc_gt_checking"), C_DIM);
        redraw_all();
    } else if (g_acc_busy && g_hwnd) {
        SetTimer(g_hwnd, ACC_GT_TIMER, ACC_CHECK_MS, NULL); /* anderer Auftrag laeuft: spaeter erneut */
    }
}

/* W487/W501: Xbox Live verfuegbar (Online mit Gamertag)? Aus dem gespeicherten Konto und dem letzten status (kein Netz). */
int acc_live_state(void)
{
    if (!acc_srv_on()) {
        return ACC_LIVE_OFF;
    }
    if (g_o.shot == 0 && !g_in_gui_selftest) {
        acc_load();
    }
    if (!g_acc.have) {
        return ACC_LIVE_NOACC;
    }
    if (!g_acc_st_ok) {
        return ACC_LIVE_UNK;
    }
    if (strcmp(g_acc_st.gt_state, "off") == 0 && !g_acc_st.gamertag[0]) {
        return ACC_LIVE_GTOFF;
    }
    return g_acc_st.gamertag[0] ? ACC_LIVE_OK : ACC_LIVE_NOGT;
}

const char *acc_live_text(int st)
{
    switch (st) {
    case ACC_LIVE_OFF: return LT("gui.loc_onl_nosrv");
    case ACC_LIVE_NOACC: return LT("gui.live_sl_only"); /* W501: nur System Link, Weg zum Gamertag */
    case ACC_LIVE_UNK: return LT("gui.live_need_status");
    case ACC_LIVE_NOGT: return LT("gui.live_sl_only");
    case ACC_LIVE_GTOFF: return LT("gui.acc_gt_off");
    default: return NULL;
    }
}

/* W501 ONLINE-BOTH: vor jedem LoC-Start (ersetzt die W487-Startsperre). Online (steelnet, nicht LAN) mit Konto, aber
 * Gamertag-Stand unbekannt: Status einmal holen und danach starten (acc_done) - mit Gamertag System Link + Xbox Live,
 * sonst nur System Link. Sperrt nie: 1 = jetzt starten, 0 = Start folgt nach dem Status. */
int acc_live_prestart(void)
{
    if (g_acc_live_pending == 2) { /* Status fuer diesen Start schon geholt (Ergebnis egal) */
        g_acc_live_pending = 0;
        return 1;
    }
    if (netm_get() != NETM_STEEL || g_loc_lan || g_acc_fixture || g_o.shot != 0 || g_in_gui_selftest ||
        acc_live_state() != ACC_LIVE_UNK) {
        return 1;
    }
    if (g_acc_busy == ACC_OP_STATUS || acc_start(ACC_OP_STATUS, NULL, NULL) == 0) {
        g_acc_live_pending = 1;
        set_status_k(LT("gui.live_checking"), 0);
        return 0;
    }
    return 1; /* anderer Auftrag laeuft: nicht warten, nur System Link */
}

/* W502: Knoepfe der Uebersicht (Sprung zur Karte der naechsten Aktion) und Abbrechen der eingebetteten Formulare */
static int acc_w502_command(int id, int code)
{
    if (code != BN_CLICKED) {
        return 1;
    }
    switch (id) {
    case ID_ACC_O_ONLINE:
        if (acc_state() == ACCS_NONE) { /* Konto erstellen: Feld Kontoname */
            acc_highlight(ACC_CARD_NEW);
            acc_focus(ID_ACC_NAME);
        } else if (!g_acc_busy) { /* Status unbekannt: erneut abfragen (gleicher Auftrag wie beim Seitenaufruf) */
            g_acc_st_err[0] = '\0';
            acc_start(ACC_OP_STATUS, NULL, NULL);
            acc_refresh();
        }
        return 1;
    case ID_ACC_O_LIVE:
        acc_open_gamertag();
        return 1;
    case ID_ACC_O_SEC:
        g_acc_x_login = 0;
        acc_refresh();
        acc_highlight(ACC_CARD_SEC);
        acc_focus(ID_ACC_V_DISCORD);
        return 1;
    case ID_ACC_V_LGCANCEL:
        g_acc_x_login = 0;
        if (ctl(ID_ACC_V_PASS)) ui_settext(ctl(ID_ACC_V_PASS), "");
        if (g_acc_fmsg_id == ID_ACC_V_LOGIN) acc_fmsg(0, "", 0);
        acc_refresh();
        acc_focus(ID_ACC_X_LOGIN);
        return 1;
    case ID_ACC_V_CDCANCEL:
        g_acc_x_code = 0;
        if (ctl(ID_ACC_V_CODE)) ui_settext(ctl(ID_ACC_V_CODE), "");
        if (g_acc_fmsg_id == ID_ACC_V_CODE) acc_fmsg(0, "", 0);
        acc_refresh();
        acc_focus(ID_ACC_X_CODE);
        return 1;
    }
    return 1;
}

static int acc_command(int id, int code)
{
    char a[80], b[300], q[400];
    if (ACC_GT_ID(id) || id == ID_ACC_V_GTCANCEL) { /* W487 */
        return acc_gt_command(id, code);
    }
    if (ACC_W502_ID(id)) { /* W502: Uebersicht, Abbrechen */
        return acc_w502_command(id, code);
    }
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
        { /* W487: Gamertag (Feld bzw. Vorschlag); leer = ohne (spaeter unter Konto setzen) */
            char g[64] = "";
            if (ctl(ID_ACC_GT)) ui_gettext(ctl(ID_ACC_GT), g, sizeof g);
            if (g_hwnd) KillTimer(g_hwnd, ACC_GT_TIMER);
            if (g[0] && !lacc_valid_gamertag(g)) {
                set_status_k(LT("gui.acc_gt_rules"), 2);
                acc_gtmsg(ID_ACC_GT, LT("gui.acc_gt_rules"), C_ERR);
                redraw_all();
                return 1;
            }
            acc_start3(ACC_OP_CREATE, a, NULL, g, 0);
        }
        return 1;
    case ID_ACC_SIGNIN:
    case ID_ACC_MANAGE:
    case ID_LOC_MP_ACC: /* W418: Karte Multiplayer auf Spielen */
        acc_nav_sub(1); /* W399: Unterpunkt Account */
        return 1;
    case ID_ACC_X_LOGIN: /* W502: Login-Formular ersetzt den Inhalt der Karte (Abbrechen: ID_ACC_V_LGCANCEL) */
        g_acc_x_login = 1;
        if (acc_state() == ACCS_NONE) g_acc_x_code = 0; /* kein Konto: Login oder Code (gleiche Karte) */
        if (ctl(ID_ACC_V_LOGIN)) ui_settext(ctl(ID_ACC_V_LOGIN), g_acc_st_ok && g_acc_st.login ? g_acc_st.login_name : "");
        if (g_acc_fmsg_id == ID_ACC_V_LOGIN) acc_fmsg(0, "", 0);
        acc_refresh();
        acc_focus(ID_ACC_V_LOGIN);
        return 1;
    case ID_ACC_X_CODE: /* W502: Codeformular in der Karte (Abbrechen: ID_ACC_V_CDCANCEL) */
        g_acc_x_code = 1;
        if (acc_state() == ACCS_NONE) g_acc_x_login = 0;
        if (g_acc_fmsg_id == ID_ACC_V_CODE) acc_fmsg(0, "", 0);
        acc_refresh();
        acc_focus(ID_ACC_V_CODE);
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
    /* W502: Reihenfolge = Tab-Reihenfolge der Kontoseite (Uebersicht, Erstellen, Gamertag, Sichern, Einstellungen);
     * Lage je Zustand setzt acc_layout. Die Karte "steelnet account" auf Match & network nutzt Name/Erstellen und
     * die Knoepfe am Ende (W473). */
    mk(w, K_BTN, ID_ACC_O_ONLINE, P_LNET, CX0 + CW - 18 - ACC_OV_BTN_W, CY0 + 44, ACC_OV_BTN_W, 28, LT("gui.acc_create"), 0);
    mk(w, K_BTN, ID_ACC_O_LIVE, P_LNET, CX0 + CW - 18 - ACC_OV_BTN_W, CY0 + 76, ACC_OV_BTN_W, 28, LT("gui.acc_ov_gt_btn"), 0);
    mk(w, K_BTN, ID_ACC_O_SEC, P_LNET, CX0 + CW - 18 - ACC_OV_BTN_W, CY0 + 108, ACC_OV_BTN_W, 28, LT("gui.acc_ov_sec_btn"), 0);
    mk(w, K_EDIT, ID_ACC_NAME, P_LNET, x, y + 86, COLW - 36 - 8 - 130, 28, "", 0);
    /* W487: Gamertag beim Erstellen (nur Kontoseite), Tab-Reihenfolge Name -> Gamertag */
    mk(w, K_EDIT, ID_ACC_GT, P_LNET, CX0 + 18, CY0 + 222, COLW - 36, 28, "", 0);
    mk(w, K_ACCENT, ID_ACC_CREATE, P_LNET, ACC_X + COLW - 18 - 130, y + 86, 130, 28, LT("gui.acc_create"), 0);
    /* W487/W502: Gamertag-Karte (Feld + Setzen + Abbrechen bzw. Aendern) */
    mk(w, K_EDIT, ID_ACC_V_GT, P_LNET, CX0 + 18, CY0 + 196, 240, 28, "", 0);
    mk(w, K_BTN, ID_ACC_V_GTBTN, P_LNET, CX0 + 18 + 250, CY0 + 196, 160, 28, LT("gui.acc_gt_set"), 0);
    mk(w, K_BTN, ID_ACC_V_GTCANCEL, P_LNET, CX0 + 18 + 418, CY0 + 196, 110, 28, LT("gui.acc_cancel"), 0);
    mk(w, K_BTN, ID_ACC_X_GT, P_LNET, CX0 + CW - 18 - 150, CY0 + 196, 150, 28, LT("gui.acc_b_gt_change"), 0);
    /* Sichern: Discord, Login (Formular), Wiederherstellungscode; kein Konto: Anmelden mit Discord/Login/Code */
    mk(w, K_BTN, ID_ACC_V_DISCORD, P_LNET, COL2 + 18, CY0 + 300, ACC_ROW_BTN_W, 28, LT("gui.acc_discord_login"), 0);
    mk(w, K_BTN, ID_ACC_X_LOGIN, P_LNET, COL2 + 18, CY0 + 340, ACC_ROW_BTN_W, 28, LT("gui.acc_b_login_in"), 0);
    mk(w, K_EDIT, ID_ACC_V_LOGIN, P_LNET, COL2 + 18, CY0 + 316, COLW - 36, 28, "", 0);
    mk(w, K_EDIT, ID_ACC_V_PASS, P_LNET, COL2 + 18, CY0 + 366, COLW - 36, 28, "", ES_PASSWORD);
    mk(w, K_BTN, ID_ACC_V_LOGINBTN, P_LNET, COL2 + 18, CY0 + 400, 160, 28, LT("gui.acc_login_go"), 0);
    mk(w, K_BTN, ID_ACC_V_LGCANCEL, P_LNET, COL2 + 186, CY0 + 400, 110, 28, LT("gui.acc_cancel"), 0);
    mk(w, K_BTN, ID_ACC_V_RECSHOW, P_LNET, CX0 + 236, CY0 + 380, 80, 26, LT("gui.acc_rec_show"), 0);
    mk(w, K_BTN, ID_ACC_V_RECCOPY, P_LNET, CX0 + 322, CY0 + 380, 80, 26, LT("gui.acc_rec_copy"), 0);
    /* Kontoeinstellungen: Code einloesen (Formular), Abmelden, Loeschen */
    mk(w, K_BTN, ID_ACC_X_CODE, P_LNET, COL2 + 18, CY0 + 376, COLW - 36, 28, LT("gui.acc_b_code"), 0);
    mk(w, K_EDIT, ID_ACC_V_CODE, P_LNET, COL2 + 18, CY0 + 346, COLW - 36, 28, "", 0);
    mk(w, K_BTN, ID_ACC_V_CODEBTN, P_LNET, COL2 + 18, CY0 + 382, 110, 28, LT("gui.acc_redeem"), 0);
    mk(w, K_BTN, ID_ACC_V_CDCANCEL, P_LNET, COL2 + 136, CY0 + 382, 110, 28, LT("gui.acc_cancel"), 0);
    mk(w, K_BTN, ID_ACC_V_FORGET, P_LNET, COL2 + 18, CY0 + 412, COLW - 36, 28, LT("gui.acc_forget"), 0);
    mk(w, K_BTN, ID_ACC_V_DELETE, P_LNET, COL2 + 18, CY0 + 466, COLW - 36, 28, LT("gui.acc_delete"), 0);
    /* Karte "steelnet account" (W397: LoC > Multiplayer rechts oben). W418: ohne "Name pruefen".
     * W473: Lage wie acc_layout (Zustand oben, Knoepfe unten; Manage ohne Discord-Knopf links, s. acc_layout) */
    mk(w, K_BTN, ID_ACC_SIGNIN, P_LNET, x, y + 122, COLW - 36, 26, LT("gui.acc_signin"), 0);
    mk(w, K_ACCENT, ID_ACC_DISCORD, P_LNET, x, y + ACC_C_BTN, 168, 28, LT("gui.acc_discord_link"), 0);
    mk(w, K_BTN, ID_ACC_MANAGE, P_LNET, x + 174, y + ACC_C_BTN, COLW - 36 - 174, 28, LT("gui.acc_manage"), 0);
    if (ctl(ID_ACC_GT)) SendMessageW(ctl(ID_ACC_GT), EM_LIMITTEXT, 15, 0);
    if (ctl(ID_ACC_V_GT)) SendMessageW(ctl(ID_ACC_V_GT), EM_LIMITTEXT, 15, 0);
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
        { ID_ACC_V_CODEBTN, "gui.acc_redeem" }, { ID_ACC_X_CODE, "gui.acc_b_code" },
        { ID_ACC_O_LIVE, "gui.acc_ov_gt_btn" }, { ID_ACC_O_SEC, "gui.acc_ov_sec_btn" }, /* W502 */
        { ID_ACC_X_GT, "gui.acc_b_gt_change" }, { ID_ACC_V_GTCANCEL, "gui.acc_cancel" },
        { ID_ACC_V_LGCANCEL, "gui.acc_cancel" }, { ID_ACC_V_CDCANCEL, "gui.acc_cancel" },
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
    if (st == ACCS_SEC) { /* W487: Gamertag rechts neben "Manage account" (bzw. deutlich: noch keiner) */
        m = acc_card_gt(t, sizeof t, &c);
        label1(F_TINY, m, x0 + 188, y + ACC_C_BTN, w0 - 188, 28, c);
    }
    return 1;
}

/* W487: Gamertag-Zeile der Karte (Match & network), gesichert: "Gamertag: X" bzw. "Gamertag: not set" */
const char *acc_card_gt(char *buf, size_t cap, COLORREF *c)
{
    *c = C_DIM;
    if (g_acc_st_ok && g_acc_st.gamertag[0]) {
        _snprintf_s(buf, cap, _TRUNCATE, LT("gui.acc_gt_head"), g_acc_st.gamertag);
        return buf;
    }
    *c = C_WARN;
    return LT("gui.acc_gt_card_none");
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

/* ------------------------------------------------------------------ */
/* W502 ACCOUNT-PAGE: Zeichnen der Kontoseite. Alle Texte laufen ueber acc_t(): im Pruefmodus (Selbsttest,         */
/* g_acc_chk) wird nichts gezeichnet, sondern gemessen, ob jeder Text in seinen Kasten passt (EN/DE, je Zustand).   */
/* ------------------------------------------------------------------ */
enum { ACC_T1, ACC_T2, ACC_TR, ACC_TC }; /* einzeilig links / umbrechend / einzeilig rechts / mittig */
static int g_acc_chk;          /* 1 = nur messen */
static char g_acc_chk_why[200];

static void acc_t(int font, const char *t, int x, int y, int w, int h, COLORREF c, int mode)
{
    if (!t || !*t) {
        return;
    }
    if (g_acc_chk) {
        if (!g_acc_chk_why[0] && !ui_text_fits(font, t, x, y, w, h, mode != ACC_T2)) {
            _snprintf_s(g_acc_chk_why, sizeof g_acc_chk_why, _TRUNCATE, "'%.80s' passt nicht (%dx%d)", t, w, h);
        }
        return;
    }
    if (mode == ACC_T2) {
        if (ui_text_fits(font, t, x, y, w, h, 1)) { /* eine Zeile: mittig im Kasten wie die einzeiligen */
            L_text(font, t, x, y, w, h, c, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        } else {
            L_text(font, t, x, y, w, h, c, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
        }
        return;
    }
    L_text(font, t, x, y, w, h, c,
           (mode == ACC_TR ? DT_RIGHT : mode == ACC_TC ? DT_CENTER : DT_LEFT) | DT_SINGLELINE | DT_VCENTER |
               DT_END_ELLIPSIS);
}

static void acc_card(int x, int y, int w, int h, const char *title)
{
    if (g_acc_chk) {
        if (title) acc_t(F_CARD, title, x + 18, y + 12, w - 36, 24, C_TEXT, ACC_T1);
        return;
    }
    card(x, y, w, h, title);
}

static void acc_line(int x, int y, int w)
{
    if (!g_acc_chk) {
        L_fill(x, y, w, 1, RGB(255, 255, 255), 14, 0);
    }
}

/* W418: Wiederherstellungscode verdeckt bzw. angezeigt (Monospace) */
static void acc_paint_code(int x, int y, int w)
{
    acc_t(F_MONO, g_acc_rec[0] ? g_acc_rec : "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2-\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2-\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2-\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2",
          x, y, w, 20, g_acc_rec[0] ? C_TEXT : C_FAINT, ACC_T1);
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

/* W487: Zeile 1 der Gamertag-Karte: "Gamertag: X" bzw. deutlich "noch kein Gamertag" / Dienst ohne Gamertags */
const char *acc_gt_line1(char *buf, size_t cap, COLORREF *c)
{
    *c = C_TEXT;
    if (g_acc_st_ok && g_acc_st.gamertag[0]) {
        _snprintf_s(buf, cap, _TRUNCATE, LT("gui.acc_gt_head"), g_acc_st.gamertag);
        return buf;
    }
    *c = C_WARN;
    if (!g_acc_st_ok) {
        *c = C_DIM;
        return LT("gui.acc_gt_unk");
    }
    if (strcmp(g_acc_st.gt_state, "off") == 0 || strcmp(g_acc_st.gt_state, "unavailable") == 0) {
        return LT("gui.acc_gt_off");
    }
    return LT("gui.acc_gt_none");
}

/* W487: Grenze der Gamertag-Karte (W502: rechts in der Titelzeile, einmal): naechste Aenderung bzw. "alle N Tage" */
const char *acc_gt_limit(char *buf, size_t cap)
{
    char d[32];
    if (acc_gt_locked()) {
        acc_gt_date(g_acc_st.gt_next, d, sizeof d);
        _snprintf_s(buf, cap, _TRUNCATE, LT("gui.acc_gt_next"), d);
    } else {
        _snprintf_s(buf, cap, _TRUNCATE, LT("gui.acc_gt_can"),
                    g_acc_st_ok && g_acc_st.gt_days > 0 ? g_acc_st.gt_days : 7);
    }
    return buf;
}

/* W502: Symbol einer Zeile der Uebersicht: erledigt (gruener Haken), offen (Hinweis), unbekannt, spaeter */
enum { ACC_I_OK, ACC_I_TODO, ACC_I_UNK, ACC_I_LATER };
static void acc_ov_icon(int x, int y, int kind)
{
    COLORREF c = kind == ACC_I_OK ? C_OK : kind == ACC_I_TODO ? C_WARN : kind == ACC_I_UNK ? C_DIM : C_FAINT;
    if (g_acc_chk) {
        return;
    }
    if (kind == ACC_I_OK) {
        L_fill(x, y, 20, 20, c, 255, 10);
        if (g_has_icons) {
            L_text(F_ICONSM, "\xEE\x9C\xBE", x, y, 20, 20, RGB(16, 22, 18), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            L_text(F_SEMI, "\xE2\x9C\x93", x, y, 20, 20, RGB(16, 22, 18), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        return;
    }
    L_fill(x, y, 20, 20, c, 34, 10);
    L_ring(x, y, 20, 20, c, 230, 10);
    if (kind != ACC_I_LATER) {
        L_text(F_SEMI, kind == ACC_I_TODO ? "!" : "?", x, y, 20, 20, c, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

/* W502: eine Zeile der Uebersicht (Symbol, Bezeichnung, Zustand in Worten; Knopf rechts setzt acc_layout) */
static void acc_ov_row(const acc_geo_t *g, int i, int icon, const char *lbl, const char *det, COLORREF dc, int btn)
{
    int x0 = CX0 + 18, xr = CX0 + CW - 18, y = g->ov_y + ACC_OV_TOP + i * ACC_OV_ROW;
    int dx = x0 + 30 + ACC_OV_LBL_W + 4, dr = btn ? xr - ACC_OV_BTN_W - 12 : xr;
    if (i > 0) {
        acc_line(x0, y - 1, xr - x0);
    }
    acc_ov_icon(x0, y + 6, icon);
    acc_t(F_SEMI, lbl, x0 + 30, y, ACC_OV_LBL_W, ACC_OV_ROW, C_TEXT, ACC_T1);
    acc_t(F_TINY, det, dx, y, dr - dx, ACC_OV_ROW, dc, ACC_T2);
}

/* W502: Zeile 2 mit Gamertag: "Bereit als X" plus Kampagnenstand, soweit der Launcher ihn kennt (launcher_camp.c) */
static const char *acc_ov_live_ok(char *buf, size_t cap)
{
    int cs = camp_status();
    const char *k = cs == LCAMP_OK ? "gui.acc_ov_camp_ok" : cs == LCAMP_OLD ? "gui.acc_ov_camp_old"
                    : cs == LCAMP_UNREACH ? "gui.acc_ov_camp_unreach" : NULL;
    char a[160];
    _snprintf_s(a, sizeof a, _TRUNCATE, LT("gui.acc_ov_live_ok"), g_acc_st.gamertag);
    if (k) {
        _snprintf_s(buf, cap, _TRUNCATE, "%s \xC2\xB7 %s", a, LT(k));
    } else {
        strcpy_s(buf, cap, a);
    }
    return buf;
}

/* W502: Uebersicht "Was du spielen kannst" */
static void acc_paint_ov(const acc_geo_t *g)
{
    char t[300], t2[300];
    COLORREF c = C_DIM;
    int ok = g_acc_st_ok, gt = ok && g_acc_st.gamertag[0];
    acc_card(CX0, g->ov_y, CW, g->ov_h, LT("gui.acc_ov_title"));
    if (g->st == ACCS_NONE) {
        acc_ov_row(g, 0, ACC_I_TODO, LT("gui.acc_ov_online"), LT("gui.acc_ov_online_none"), C_WARN, 1);
        acc_ov_row(g, 1, ACC_I_LATER, LT("gui.acc_ov_live"), LT("gui.acc_ov_live_noacc"), C_DIM, 0);
        acc_ov_row(g, 2, ACC_I_LATER, LT("gui.acc_ov_sec"), LT("gui.acc_ov_sec_none"), C_DIM, 0);
        return;
    }
    _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.acc_ov_name"), g_acc.name); /* Titelzeile rechts: welches Konto */
    acc_t(F_SMALL, t, CX0 + 18 + 260, g->ov_y + 12, CW - 36 - 260, 24, C_DIM, ACC_TR);
    if (g->st == ACCS_UNK) {
        const char *k = acc_unk_text(&c);
        acc_ov_row(g, 0, c == C_DIM ? ACC_I_UNK : ACC_I_TODO, LT("gui.acc_ov_online"), k, c,
                   acc_vis_have(ID_ACC_O_ONLINE, g->st));
    } else {
        acc_ov_row(g, 0, ACC_I_OK, LT("gui.acc_ov_online"), LT("gui.acc_ov_online_ok"), C_OK, 0);
    }
    if (!ok) {
        acc_ov_row(g, 1, ACC_I_UNK, LT("gui.acc_ov_live"), LT("gui.acc_ov_wait"), C_DIM, 0);
    } else if (gt) {
        acc_ov_row(g, 1, ACC_I_OK, LT("gui.acc_ov_live"), acc_ov_live_ok(t2, sizeof t2), C_OK, 0);
    } else if (!acc_vis_have(ID_ACC_O_LIVE, g->st)) { /* Dienst ohne Gamertags */
        acc_ov_row(g, 1, ACC_I_UNK, LT("gui.acc_ov_live"), LT("gui.acc_ov_live_gtoff"), C_WARN, 0);
    } else {
        acc_ov_row(g, 1, ACC_I_TODO, LT("gui.acc_ov_live"), LT("gui.acc_ov_live_nogt"), C_WARN, 1);
    }
    if (g->st == ACCS_SEC) {
        const char *k = g_acc_st.discord && g_acc_st.login ? "gui.acc_c_sec_both"
                        : g_acc_st.discord                 ? "gui.acc_c_sec_dc"
                                                           : "gui.acc_c_sec_lg";
        acc_ov_row(g, 2, ACC_I_OK, LT("gui.acc_ov_sec"), LT(k), C_OK, 0);
    } else if (g->st == ACCS_UNSEC) {
        acc_ov_row(g, 2, ACC_I_TODO, LT("gui.acc_ov_sec"), LT("gui.acc_ov_sec_no"), C_WARN, 1);
    } else {
        acc_ov_row(g, 2, ACC_I_UNK, LT("gui.acc_ov_sec"), LT("gui.acc_ov_wait"), C_DIM, 0);
    }
}

/* W502: Gamertag-Karte (mit Konto): Feld + Setzen bzw. Anzeige + Aendern; Regel einmal rechts im Titel; Meldung
 * neben den Knoepfen; unten ein Satz, wofuer */
static void acc_paint_gt(const acc_geo_t *g)
{
    char t[300];
    COLORREF c = C_DIM;
    const char *m;
    int x0 = CX0 + 18, xr = CX0 + CW - 18, y = g->gt_y + ACC_G_ROW;
    int ok = g_acc_st_ok, gt = ok && g_acc_st.gamertag[0], field = acc_vis_have(ID_ACC_V_GT, g->st);
    acc_card(CX0, g->gt_y, CW, g->gt_h, LT("gui.acc_gt_title"));
    if (field || gt) {
        acc_t(F_TINY, acc_gt_limit(t, sizeof t), x0 + 200, g->gt_y + 12, xr - x0 - 200, 24,
              acc_gt_locked() ? C_WARN : C_DIM, ACC_TR);
    }
    m = field ? acc_gtmsg_for(ID_ACC_V_GT, &c) : NULL; /* Pruef-/Fehlermeldung unter dem Feld (statt des Satzes) */
    if (field && !gt) { /* Regel neben dem Knopf (beim Aendern ist dort Abbrechen) */
        acc_t(F_TINY, LT("gui.acc_gt_rules"), x0 + 420, y - 2, xr - x0 - 420, 32, C_FAINT, ACC_T2);
    } else if (gt && !field) {
        acc_t(F_CARD, g_acc_st.gamertag, x0, y, xr - x0 - 170, 28, C_TEXT, ACC_T1);
    } else if (!field) {
        const char *l1 = acc_gt_line1(t, sizeof t, &c); /* unbekannt bzw. Dienst ohne Gamertags */
        acc_t(F_SMALL, l1, x0, y, xr - x0, 28, c, ACC_T2);
    }
    if (m) {
        acc_t(F_TINY, m, x0, g->gt_y + 72, xr - x0, 18, c, ACC_T1);
    } else {
        acc_t(F_TINY, LT("gui.acc_gt_why"), x0, g->gt_y + 72, xr - x0, 18, C_FAINT, ACC_T1);
    }
}

/* W502: Karte Konto sichern: Discord, Login und Passwort, Wiederherstellungscode als gleichwertige Zeilen; das
 * Login-Formular ersetzt die Zeilen in der Karte */
static void acc_paint_sec(const acc_geo_t *g)
{
    char t[300];
    COLORREF c = C_DIM;
    const char *m;
    int ok = g_acc_st_ok, sx = g->lx + 18, sxr = g->lx + g->lw - 18, sw = sxr - sx;
    int lw = sw - ACC_ROW_BTN_W - 10, fw = (sw - 12) / 2;
    acc_card(g->lx, g->lo_y, g->lw, g->lo_h, LT(g->st == ACCS_UNSEC ? "gui.acc_sec_one" : "gui.acc_sec_title_ok"));
    acc_t(F_TINY, LT("gui.acc_sec_why"), sx + 190, g->lo_y + 12, sw - 190, 24, C_DIM, ACC_TR);
    if (g_acc_x_login) {
        acc_t(F_TINY, LT("gui.acc_l_login_s"), sx, g->lo_y + ACC_S_FLD - 18, fw, 16, C_DIM, ACC_T1);
        acc_t(F_TINY, LT("gui.acc_l_pw_new"), sx + fw + 12, g->lo_y + ACC_S_FLD - 18, fw, 16, C_DIM, ACC_T1);
        m = acc_fmsg_for(ID_ACC_V_LOGIN, &c);
        acc_t(F_TINY, m ? m : LT("gui.acc_login_rules"), sx, g->lo_y + ACC_S_FLD + 34, sw, 40, m ? c : C_FAINT, ACC_T2);
        return;
    }
    /* Discord */
    acc_t(F_SEMI, LT("gui.acc_m_discord"), sx, g->r_dc, lw, 20, C_TEXT, ACC_T1);
    if (ok && g_acc_st.discord) {
        _snprintf_s(t, sizeof t, _TRUNCATE, "\xE2\x9C\x93  %s", LT("gui.acc_sec_dc_on"));
        acc_t(F_TINY, t, sx, g->r_dc + 20, sw, 18, C_OK, ACC_T1);
    } else {
        acc_t(F_TINY, LT(ok ? "gui.acc_sec_dc_sub" : "gui.acc_st_unknown_s"), sx, g->r_dc + 20, lw, 18, C_DIM, ACC_T1);
    }
    /* Login und Passwort */
    acc_line(sx, g->r_lg - 4, sw);
    acc_t(F_SEMI, LT("gui.acc_m_login"), sx, g->r_lg, lw, 20, C_TEXT, ACC_T1);
    if (ok && g_acc_st.login) {
        char l[160];
        _snprintf_s(l, sizeof l, _TRUNCATE, LT("gui.acc_sec_lg_on"), g_acc_st.login_name);
        _snprintf_s(t, sizeof t, _TRUNCATE, "\xE2\x9C\x93  %s", l);
        acc_t(F_TINY, t, sx, g->r_lg + 20, lw, 18, C_OK, ACC_T1);
    } else {
        acc_t(F_TINY, LT(ok ? "gui.acc_sec_lg_off" : "gui.acc_st_unknown_s"), sx, g->r_lg + 20, lw, 18, C_DIM, ACC_T1);
    }
    /* Wiederherstellungscode: Zeile 1 Titel + Anzeigen/Kopieren, Zeile 2 Code (verdeckt mit Hinweis) */
    acc_line(sx, g->r_rec - 4, sw);
    acc_t(F_SEMI, LT("gui.acc_rec_title"), sx, g->r_rec, sw - 176, 26, C_TEXT, ACC_T1);
    if (g_acc_rec[0]) {
        acc_paint_code(sx, g->r_rec + 28, sw);
    } else {
        acc_paint_code(sx, g->r_rec + 28, 140);
        acc_t(F_TINY, LT("gui.acc_rec_hint"), sx + 146, g->r_rec + 28, sw - 146, 20, C_DIM, ACC_T1);
    }
}

/* W502: Karte Kontoeinstellungen: Code einloesen, Abmelden, (Trennlinie) Loeschen bzw. Codeformular */
static void acc_paint_set(const acc_geo_t *g)
{
    COLORREF c = C_DIM;
    const char *m;
    int tx = g->rx + 18, tw = g->rw - 36;
    acc_card(g->rx, g->lo_y, g->rw, g->lo_h, LT("gui.acc_set_title"));
    if (g_acc_x_code) {
        acc_t(F_TINY, LT("gui.acc_code_label"), tx, g->lo_y + ACC_E_FLD - 18, tw, 16, C_DIM, ACC_T1);
        m = acc_fmsg_for(ID_ACC_V_CODE, &c);
        acc_t(F_TINY, m ? m : LT("gui.acc_m_code_text"), tx, g->lo_y + ACC_E_BTN + 36, tw, 58, m ? c : C_DIM, ACC_T2);
        return;
    }
    acc_t(F_TINY, LT("gui.acc_set_why"), tx, g->lo_y + 38, tw, 18, C_DIM, ACC_T1);
    acc_line(tx, g->lo_y + ACC_E_SEP, tw);
}

/* W502: kein Konto - Erstellen (Name, Gamertag optional) | Ich habe schon ein Konto (Discord, Login, Code) */
static void acc_paint_none(const acc_geo_t *g)
{
    COLORREF c = C_DIM;
    const char *m;
    int nx = g->lx + 18, nw = g->lw - 36, hx = g->rx + 18, hw = g->rw - 36, y = g->lo_y;
    acc_card(g->lx, y, g->lw, g->lo_h, LT("gui.acc_new_title"));
    if (!g_acc_chk) {
        L_ring(g->lx, y, g->lw, g->lo_h, C_ACCENT, 150, 10);
    }
    acc_t(F_TINY, LT("gui.acc_head_none2"), nx, y + 38, nw, 16, C_DIM, ACC_T1);
    acc_t(F_TINY, LT("gui.acc_name_long"), nx, y + ACC_N_NAME - 18, nw, 16, C_DIM, ACC_T1);
    m = acc_fmsg_for(ID_ACC_NAME, &c);
    acc_t(F_TINY, m ? m : LT("gui.acc_name_hint"), nx, y + ACC_N_NAME + 30, nw, 18, m ? c : C_FAINT, ACC_T1);
    acc_t(F_TINY, LT("gui.acc_gt_label"), nx, y + ACC_N_GT - 18, nw, 16, C_DIM, ACC_T1);
    m = acc_gtmsg_for(ID_ACC_GT, &c);
    acc_t(F_TINY, m ? m : LT("gui.acc_gt_hint"), nx, y + ACC_N_GT + 30, nw, 34, m ? c : C_FAINT, ACC_T2);
    acc_card(g->rx, y, g->rw, g->lo_h, LT("gui.acc_have_title"));
    if (g_acc_x_login) {
        acc_t(F_TINY, LT("gui.acc_l_login_s"), hx, y + ACC_F_L1 - 18, hw, 16, C_DIM, ACC_T1);
        acc_t(F_TINY, LT("gui.acc_l_pw"), hx, y + ACC_F_L2 - 18, hw, 16, C_DIM, ACC_T1);
        m = acc_fmsg_for(ID_ACC_V_LOGIN, &c);
        if (m) acc_t(F_TINY, m, hx, y + 206, hw, 40, c, ACC_T2);
    } else if (g_acc_x_code) {
        acc_t(F_TINY, LT("gui.acc_code_label"), hx, y + ACC_F_L1 - 18, hw, 16, C_DIM, ACC_T1);
        m = acc_fmsg_for(ID_ACC_V_CODE, &c);
        acc_t(F_TINY, m ? m : LT("gui.acc_code_hint"), hx, y + 152, hw, 46, m ? c : C_FAINT, ACC_T2);
    } else {
        acc_t(F_TINY, LT("gui.acc_have_why"), hx, y + 38, hw, 16, C_DIM, ACC_T1);
    }
}

/* W418/W502: Unterseite Multiplayer > Account ("steelnet account"): Uebersicht, dann die Karten des Zustands
 * (acc_geo). Datenschutzzeile immer am Seitenende. */
void acc_paint_view(void)
{
    acc_geo_t g;
    int x0 = CX0 + 18;
    acc_geo(&g);
    if (g.st == ACCS_OFF) { /* steelnet in dieser Ausgabe nicht eingerichtet */
        acc_card(CX0, CY0, CW, 88, LT("gui.acc_title"));
        acc_t(F_TINY, LT("gui.loc_onl_nosrv"), x0, CY0 + 40, CW - 36, 40, C_DIM, ACC_T2);
    } else {
        acc_paint_ov(&g);
        if (g.st == ACCS_NONE) {
            acc_paint_none(&g);
        } else {
            acc_paint_gt(&g);
            acc_paint_sec(&g);
            acc_paint_set(&g);
        }
        if (g_acc_hl && !g_acc_chk) { /* Zielkarte nach einem Sprung (Uebersicht -> Karte) */
            RECT r[8];
            int k[8], n = acc_page_cards(r, k, 8), i;
            for (i = 0; i < n; i++) {
                if (k[i] == g_acc_hl) {
                    L_ring(r[i].left, r[i].top, r[i].right - r[i].left, r[i].bottom - r[i].top, C_ACCENT, 255, 10);
                    L_ring(r[i].left + 1, r[i].top + 1, r[i].right - r[i].left - 2, r[i].bottom - r[i].top - 2,
                           C_ACCENT, 160, 9);
                }
            }
        }
    }
    /* Datenschutz: was auf dem Server liegt (klein, alle Zustaende) */
    acc_t(F_TINY, LT("gui.acc_privacy"), x0, ACC_PRIV_Y, CW - 36, 32, C_FAINT, ACC_T2);
}

/* W502 Selbsttest: passen alle Texte der Kontoseite im aktuellen Zustand (EN/DE)? 1 = ja, sonst why gesetzt. */
int acc_page_texts_fit(char *why, size_t cap)
{
    g_acc_chk = 1;
    g_acc_chk_why[0] = '\0';
    acc_paint_view();
    g_acc_chk = 0;
    if (g_acc_chk_why[0]) {
        strncpy_s(why, cap, g_acc_chk_why, _TRUNCATE);
        return 0;
    }
    return 1;
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
    case ID_LNET_NETREP: /* W496: anonyme Netzwerkstatistik */
    case ID_LNET_MENUBG: /* W518b: Menue-Hintergrundebene */
    case ID_LNET_CHATBAR: /* W508: Chat in den Seitenbalken */
    case ID_LNET_HOSTMIG: /* W520 */
    case ID_LNET_AUTOHOST:
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
    case ID_LNET_CAMP: /* W491: nur intern (Saison / Testkampagne) */
        if (code == CBN_SELCHANGE && !g_updating) {
            camp_pick();
            onl_sync_ui();
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
    g_loc_chatbar = 1; /* W508 */
    g_loc_hostmig = 1; /* W520 */
    g_loc_autohost = 0;
    g_loc_menubg = 1; /* W518b */
    g_loc_arcade = 0; /* W440 */
    g_loc_online = 0;   /* W397: Shots ohne loc_onl* immer Stufe "nicht steelnet" (nie die Wahl aus user.cfg) */
    g_upd_ask = 0;
    g_nrq_cfg = strcmp(name, "loc_net_nrq") == 0 ? 1 : -1; /* W496: Shots nie mit der echten Antwort */
    g_nrq_dismiss = 0;
    g_nrq_open = strcmp(name, "loc_net_nrqask") == 0;
    if (g_nrq_open) g_loc_online = 1; /* Stufe steelnet */
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
    if (strncmp(name, "loc_net_live", 12) == 0) { /* W487/W501: Online mit Konto-Fixture (mit/ohne Gamertag) */
        g_loc_online = 1; /* Stufe steelnet (Live braucht den Treffpunkt) */
        acc_fixture(1, 1, 1, 1, 0);
        if (strcmp(name, "loc_net_live_nogt") == 0) g_acc_st.gamertag[0] = '\0';
        /* W491: Kampagne - erreichbar (Standard), nicht erreichbar, veraltet; intern Testkampagne im Reiter Lobby & Chat */
        camp_fixture(strcmp(name, "loc_net_live_campx") == 0 ? LCAMP_R_FAIL : LCAMP_R_OK);
        if (strcmp(name, "loc_net_live_campold") == 0) locver_fixture("0.3.3");
        g_camp_test = strcmp(name, "loc_net_live_campt") == 0;
        if (g_camp_test) g_lnet_tab = 1;
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
        strcpy_s(g_acc_st.gamertag, sizeof g_acc_st.gamertag, "Ace Pilot"); /* W487: Beispiel-Gamertag */
        g_acc_st.gt_days = 7;
        strcpy_s(g_acc_st.gt_state, sizeof g_acc_st.gt_state, "ok");
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
    else if (strcmp(name, "loc_acc_view_manage") == 0) { /* W418: gesichert (W502: Kontoeinstellungen, Code offen) */
        acc_fixture(1, 1, 1, 1, 1);
        g_acc_x_code = 1;
    } else if (strcmp(name, "loc_acc_view_net") == 0) { /* W418: Server nicht erreichbar (Status unbekannt) */
        acc_fixture(1, 1, 0, 0, 1);
        g_acc_st_ok = 0;
        memset(&g_acc_st, 0, sizeof g_acc_st);
        strcpy_s(g_acc_st_err, sizeof g_acc_st_err, "net");
    } else if (strcmp(name, "loc_acc_signin_code") == 0) { /* W418: kein Konto, "Mit einem Code" offen */
        acc_fixture(1, 0, 0, 0, 1);
        g_acc_x_code = 1;
    } else if (strcmp(name, "loc_acc_signin_login") == 0) { /* W502: kein Konto, Anmelden mit Login offen */
        acc_fixture(1, 0, 0, 0, 1);
        g_acc_x_login = 1;
    } else if (strcmp(name, "loc_acc_view_unsec_login") == 0 || strcmp(name, "loc_acc_view_login_err") == 0) {
        acc_fixture(1, 1, 0, 0, 1); /* W418: ungesichert, Login-Formular offen (W502: mit Fehlermeldung) */
        g_acc_x_login = 1;
    } else if (strcmp(name, "loc_acc_view_gt") == 0) { /* W487/W502: gesichert, Gamertag-Wechsel gesperrt (3 Tage) */
        acc_fixture(1, 1, 1, 1, 1);
        g_acc_st.gt_next = (long long)time(NULL) + 3 * 86400LL;
    } else if (strcmp(name, "loc_acc_view_gt_edit") == 0) { /* W502: gesichert, Gamertag aendern offen */
        acc_fixture(1, 1, 1, 0, 1);
        g_acc_x_gt = 1;
    } else if (strcmp(name, "loc_acc_view_nogt") == 0) { /* W487: gesichert, noch kein Gamertag */
        acc_fixture(1, 1, 1, 0, 1);
        g_acc_st.gamertag[0] = '\0';
    } else if (strcmp(name, "loc_acc_view_unsec_nogt") == 0 || strcmp(name, "loc_acc_view_gt_taken") == 0) {
        acc_fixture(1, 1, 0, 0, 1); /* W502: ungesichert, ohne Gamertag (Fall des Nutzers; _taken: Pruefmeldung) */
        g_acc_st.gamertag[0] = '\0';
    } else if (strcmp(name, "loc_acc_view_gtoff") == 0) { /* W502: Dienst ohne Gamertags */
        acc_fixture(1, 1, 1, 0, 1);
        g_acc_st.gamertag[0] = '\0';
        strcpy_s(g_acc_st.gt_state, sizeof g_acc_st.gt_state, "off");
    } else if (strcmp(name, "loc_acc_view_off") == 0) { /* W502: steelnet nicht eingerichtet */
        acc_fixture(0, 0, 0, 0, 1);
    } else if (strcmp(name, "loc_acc_view_code") == 0) { /* W502: Kontoeinstellungen, Code mit Formatfehler */
        acc_fixture(1, 1, 1, 1, 1);
        g_acc_x_code = 1;
    } else if (strcmp(name, "loc_acc_view_unsec_gt") == 0) { /* W487: ungesichert (W502: Gamertag aendern offen) */
        acc_fixture(1, 1, 0, 0, 1);
        g_acc_x_gt = 1;
    } else if (strcmp(name, "loc_acc_nogt") == 0) { /* W487: Karte Match & network, gesichert ohne Gamertag */
        acc_fixture(1, 1, 1, 0, 0);
        g_acc_st.gamertag[0] = '\0';
    } else if (strcmp(name, "loc_acc_new_gt") == 0) { /* W487: Kontoseite "Ich bin neu" mit Gamertag-Vorschlag */
        acc_fixture(1, 0, 0, 0, 1);
    }
    if (strcmp(name, "loc_acc_view_ok") == 0) {
        camp_fixture(LCAMP_R_OK); /* W502: Kampagnenstand in der Uebersicht (Fixture, nie Netz) */
    }
}
/* Nach dem Aufbau (Shot): Pruefung bzw. Startversuch wie der Nutzer. */
void loctab_state_after(const char *name)
{
    if (strncmp(name, "loc_", 4) != 0 ||
        (g_page != P_PLAY && g_page != P_ADV && g_page != P_LNET)) { /* W293: Disc auf Erweitert; W399: Konto auf MP */
        return;
    }
    if (g_page == P_LNET && strncmp(name, "loc_net_live", 12) == 0) { /* W487: Konto-Karte zum Fixture (Lage) */
        acc_refresh();
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
        if (strcmp(name, "loc_acc_new_gt") == 0 && ctl(ID_ACC_NAME)) { /* W487: Vorschlag wie vom Dienst */
            char m[200];
            ui_settext(ctl(ID_ACC_NAME), "Ace Pilot");
            if (g_hwnd) KillTimer(g_hwnd, ACC_TIMER);
            acc_gt_settext(ID_ACC_GT, "Ace Pilot2");
            acc_gt_status_text("ok", 0, "Ace Pilot2", m, sizeof m);
            acc_gtmsg(ID_ACC_GT, m, C_OK);
            redraw_all();
        }
        { /* W502: Pruef- und Fehlermeldungen wie nach einer Eingabe (nur Anzeige, kein Netz) */
            char m[300];
            if (strcmp(name, "loc_acc_view_gt_taken") == 0) {
                acc_gt_settext(ID_ACC_V_GT, "Ace Pilot");
                acc_gt_status_text("taken", 0, "Ace Pilot", m, sizeof m);
                acc_gtmsg(ID_ACC_V_GT, m, C_ERR);
            } else if (strcmp(name, "loc_acc_view_gt_edit") == 0) {
                acc_gt_settext(ID_ACC_V_GT, "Iron Wolf");
                acc_gt_status_text("ok", 0, "Iron Wolf", m, sizeof m);
                acc_gtmsg(ID_ACC_V_GT, m, C_OK);
            } else if (strcmp(name, "loc_acc_view_login_err") == 0) {
                if (ctl(ID_ACC_V_LOGIN)) ui_settext(ctl(ID_ACC_V_LOGIN), "ace-login");
                acc_fmsg(ID_ACC_V_LOGIN, LT("gui.acc_err_pw"), C_ERR);
            } else if (strcmp(name, "loc_acc_view_code") == 0) {
                if (ctl(ID_ACC_V_CODE)) ui_settext(ctl(ID_ACC_V_CODE), "ABCD-EFGH");
                acc_fmsg(ID_ACC_V_CODE, LT("gui.acc_err_code_fmt"), C_ERR);
            }
            redraw_all();
        }
    }
    if (strcmp(name, "loc_start") == 0) {
        loctab_start();
    }
}

/* ------------------------------------------------------------------ */
