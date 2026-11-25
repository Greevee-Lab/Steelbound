/* launcher_gui_hub.c - W470 LAUNCHER-HUB: Bereiche Spiel / Optionen / steelnet.
 *
 * Optionen (Zahnrad, Seite P_LSET) und steelnet (Kopf-Chip, Seite P_NETI) gelten fuer beide Spiele. Solange einer
 * dieser Bereiche offen ist, ist die Spiel-Navigation links ausgeblendet; dort stehen die eigenen Reiter des
 * Bereichs. Die Reiter kommen aus einer Tabelle je Bereich (kHub), weitere Reiter = weitere Tabellenzeile + Seite.
 * Die Spiel-Reiter oben fuehren zurueck in den Spielbereich (zuletzt offene Spielseite, hub_game_page).
 *
 * steelnet > Info: Status aus der Kopf-Pruefung (W403, GET /v1/info), oeffentliche Zahlen und die Liste offener
 * Lobbys. W472 LOBBIES-AUTH: die Liste nur mit eigenem steelnet-Konto (angemeldete API-Aktion "lobbies", Challenge +
 * HMAC-Nachweis ueber post_auth); ohne Konto bzw. bei abgelehnter Anmeldung ein Hinweis mit Weg zur Konto-Seite,
 * Status/Zahlen bleiben sichtbar. Die Liste wird nur geholt, solange die Seite sichtbar ist, und hoechstens alle
 * 12 s (wie das Lobby-Widget im Spiel, W460); rate_limited wartet retry_after (mind. 60 s), andere Fehler mit
 * wachsendem Abstand (bis 120 s), auth_failed fragt erst beim naechsten Oeffnen der Seite wieder. Netz nur im echten
 * Fensterbetrieb (g_snp_live), nie in Bildern/Selbsttest (g_hubl_dry zaehlt nur). Keine Adressen/Schluessel in UI
 * oder Log. */
#include "launcher_gui_int.h"

#define WM_APP_HUBL (WM_APP + 47)
#define HUB_TIMER 470
#define HUB_TICK_MS 1000
#define HUB_LOB_MS 12000      /* hoechstens alle 12 s (W460: TUN_WATCH_MS) */
#define HUB_LOB_RATE_MS 60000 /* W472: rate_limited -> mindestens 60 s Pause (SECURITY Ae45) */
#define HUB_LOB_MAX_MS 300000 /* W472: laengste Pause (retry_after gedeckelt) */
#define HUB_LOB_ERR_MS 120000 /* W472: Netz-/Serverfehler: Abstand waechst 12 -> 24 -> 48 -> 96 -> 120 s */
#define HUB_TAB_Y (TITLE_H + 16 + 30) /* erster Reiter (darueber der Bereichstitel) */

typedef struct {
    int page;
    const char *key;
    wchar_t icon;
} hub_tab_t;

static const hub_tab_t kTabsOpts[] = {
    { P_LSET, "gui.hub_general", 0xE713 }, /* Allgemein = heutige Launcher-Einstellungen */
};
static const hub_tab_t kTabsNet[] = {
    { P_NETI, "gui.hub_info", 0xE946 }, /* Info */
};
static const struct {
    const hub_tab_t *tab;
    int n;
    const char *title;
} kHub[HUB_N] = {
    { NULL, 0, NULL }, /* Spielbereich: Navigation je Spiel (nav_order) */
    { kTabsOpts, (int)(sizeof kTabsOpts / sizeof kTabsOpts[0]), "gui.hub_opts" },
    { kTabsNet, (int)(sizeof kTabsNet / sizeof kTabsNet[0]), "gui.hub_net" },
};

static int g_hub_gpage = P_PLAY; /* zuletzt offene Spielseite */

/* Reiter-Id -> Bereich/Index (0 = keine Reiter-Id). Ids fortlaufend ueber alle Bereiche. */
static int hub_tab_of(int id, int *area, int *idx)
{
    int a, k = ID_HUBNAV0;
    for (a = 0; a < HUB_N; a++) {
        if (id >= k && id < k + kHub[a].n) {
            if (area) *area = a;
            if (idx) *idx = id - k;
            return 1;
        }
        k += kHub[a].n;
    }
    return 0;
}

int hub_area_of(int page)
{
    int a, i;
    for (a = 1; a < HUB_N; a++) {
        for (i = 0; i < kHub[a].n; i++) {
            if (kHub[a].tab[i].page == page) {
                return a;
            }
        }
    }
    return HUB_GAME;
}

int hub_tab_page(int id)
{
    int a, i;
    return hub_tab_of(id, &a, &i) ? kHub[a].tab[i].page : -1;
}

wchar_t hub_tab_icon(int id)
{
    int a, i;
    return hub_tab_of(id, &a, &i) ? kHub[a].tab[i].icon : 0;
}

int hub_game_page(void)
{
    return g_hub_gpage;
}

void hub_build(HWND w)
{
    int a, i, id = ID_HUBNAV0;
    for (a = 0; a < HUB_N; a++) {
        for (i = 0; i < kHub[a].n; i++, id++) {
            mk(w, K_NAV, id, PG_ALL, 12, HUB_TAB_Y + i * 44, NAV_W - 24, 38, LT(kHub[a].tab[i].key), 0);
        }
    }
    /* steelnet > Info ohne Stufe steelnet: Weg in die Einstellungen (unten in der Hinweiskarte) */
    mk(w, K_BTN, ID_HUB_GOOPT, P_NETI, CX0 + 18, CY0 + 156 - 46, 190, 28, LT("gui.hub_goopt"), 0);
    /* W472: Lobbykarte ohne (gueltige) Anmeldung: Weg zur Konto-Seite (unter dem Hinweis) */
    mk(w, K_BTN, ID_HUB_GOACC, P_NETI, COL2 + 18, CY0 + 104, 190, 28, LT("gui.hub_goacc"), 0);
}

void hub_relabel(void)
{
    int a, i, id = ID_HUBNAV0;
    for (a = 0; a < HUB_N; a++) {
        for (i = 0; i < kHub[a].n; i++, id++) {
            if (ctl(id)) ui_settext(ctl(id), LT(kHub[a].tab[i].key));
        }
    }
    if (ctl(ID_HUB_GOOPT)) ui_settext(ctl(ID_HUB_GOOPT), LT("gui.hub_goopt"));
    if (ctl(ID_HUB_GOACC)) ui_settext(ctl(ID_HUB_GOACC), LT("gui.hub_goacc"));
}

static int hub_net_ready(void)
{
    return netm_get() == NETM_STEEL && onl_srv_on();
}

int hub_visible(int id, int page)
{
    int area = hub_area_of(page), ta;
    if ((id >= ID_NAV0 && id < ID_NAV0 + P_N) || id == ID_NAVSUB_MATCH || id == ID_NAVSUB_ACC) {
        return area == HUB_GAME ? -1 : 0; /* Spiel-Navigation nur im Spielbereich */
    }
    if (id >= ID_HUBNAV0 && id < ID_HUBNAV_END) {
        return hub_tab_of(id, &ta, NULL) && ta == area;
    }
    if (id == ID_HUB_GOOPT) {
        return page == P_NETI && !hub_net_ready();
    }
    if (id == ID_HUB_GOACC) { /* W472: ohne Konto / Anmeldung abgelehnt */
        return page == P_NETI && hub_net_ready() && (g_hubl_state == -4 || g_hubl_state == -5);
    }
    return -1;
}

int hub_command(int id, int code)
{
    if (id >= ID_HUBNAV0 && id < ID_HUBNAV_END) {
        int p = hub_tab_page(id);
        if (code == BN_CLICKED && p >= 0) {
            show_page(p);
        }
        return 1;
    }
    if (id == ID_HUB_GOOPT) {
        if (code == BN_CLICKED) {
            show_page(P_LSET);
        }
        return 1;
    }
    if (id == ID_HUB_GOACC) { /* W472: Konto-Seite = LoC > Multiplayer > Account (Spielwechsel wie die Spiel-Reiter) */
        if (code == BN_CLICKED) {
            if (g_game != LGAME_LOC) {
                g_page = hub_game_page();
                loctab_set_game(LGAME_LOC);
            }
            acc_nav_sub(1);
        }
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Lobbyliste (GET /v1/lobbies)                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    HWND h;
    int gen;
    lacc_store_t acc; /* W472: Kopie des eigenen Kontos fuer den Nachweis (nach dem Abruf geloescht) */
    lacc_lobbies_t r;
} hubl_job_t;

lacc_lobbies_t g_hubl;
/* -2 wartet (noch keine Liste), -1 Server ohne Liste, 0 Fehler, 1 ok; W472: -4 ohne Konto, -5 Anmeldung abgelehnt
 * (auth_failed), -6 zu viele Anfragen (rate_limited) */
int g_hubl_state = -2;
ULONGLONG g_hubl_wait = HUB_LOB_MS; /* W472: Abstand bis zum naechsten Abruf */
static int g_hubl_fails;            /* W472: Fehler in Folge (wachsender Abstand) */
int g_hubl_dry, g_hubl_dry_n;
ULONGLONG g_hubl_t0; /* GetTickCount64 des letzten Anstosses (0 = noch keiner) */
static int g_hubl_busy, g_hubl_gen;
static SYSTEMTIME g_hubl_when;

static DWORD WINAPI hubl_thread(LPVOID p)
{
    hubl_job_t *j = (hubl_job_t *)p;
    lacc_lobbies(&j->acc, &j->r);
    SecureZeroMemory(&j->acc, sizeof j->acc);
    if (!PostMessageW(j->h, WM_APP_HUBL, (WPARAM)j->gen, (LPARAM)j)) {
        free(j);
    }
    return 0;
}

/* Rein (Selbsttest): faellig, wenn seit dem letzten Anstoss mindestens 12 s vergangen sind. */
int hub_lobby_due(ULONGLONG since_ms)
{
    return since_ms >= (ULONGLONG)HUB_LOB_MS;
}

static void hubl_set_state(int st)
{
    if (st == g_hubl_state) {
        return;
    }
    g_hubl_state = st;
    if (st != 1) {
        memset(&g_hubl, 0, sizeof g_hubl);
    }
    if (g_hwnd && g_page == P_NETI) {
        ui_refresh_visibility(); /* Knopf zur Konto-Seite */
        redraw_all();
    }
}

/* Abruf anstossen, wenn die Seite sichtbar ist, Stufe steelnet gilt, ein eigenes Konto da ist und der Abstand um
 * ist. 1 = gestartet (Selbsttest: gezaehlt). W472: ohne Konto kein Netz (Zustand -4), nach auth_failed kein neuer
 * Versuch bis zum naechsten Oeffnen der Seite. */
int hub_lobby_kick(void)
{
    hubl_job_t *j;
    HANDLE th;
    ULONGLONG now = GetTickCount64();
    if (g_page != P_NETI || !hub_net_ready() || g_hubl_busy || !g_hwnd) {
        return 0;
    }
    if (g_o.shot != 0 && !g_hubl_dry) { /* Bilder: Zustand aus hub_lobby_fixture bleibt, nie Kontodatei/Netz */
        return 0;
    }
    if (g_o.shot == 0) { /* Selbsttest (g_o.shot != 0): Konto vorgegeben, nie die Datei des Nutzers */
        acc_ensure_loaded();
    }
    if (!g_acc.have) {
        hubl_set_state(-4);
        return 0;
    }
    if (g_hubl_state == -4) { /* Konto inzwischen da */
        hubl_set_state(-2);
        g_hubl_t0 = 0;
        g_hubl_wait = HUB_LOB_MS;
    }
    if (g_hubl_state == -5) {
        return 0;
    }
    if (g_hubl_t0 != 0 && (!hub_lobby_due(now - g_hubl_t0) || now - g_hubl_t0 < g_hubl_wait)) {
        return 0;
    }
    if (g_hubl_dry) { /* Selbsttest: nie Netz */
        g_hubl_dry_n++;
        g_hubl_t0 = now;
        return 1;
    }
    if (!g_snp_live || g_o.shot != 0 || !IsWindowVisible(g_hwnd) || IsIconic(g_hwnd)) {
        return 0;
    }
    j = (hubl_job_t *)calloc(1, sizeof *j);
    if (!j) {
        return 0;
    }
    j->h = g_hwnd;
    j->gen = ++g_hubl_gen;
    j->acc = g_acc;
    th = CreateThread(NULL, 0, hubl_thread, j, 0, NULL);
    if (!th) {
        SecureZeroMemory(&j->acc, sizeof j->acc);
        free(j);
        return 0;
    }
    CloseHandle(th);
    g_hubl_busy = 1;
    g_hubl_t0 = now;
    return 1;
}

void hub_tick(void)
{
    if (g_page != P_NETI) {
        if (g_hwnd) KillTimer(g_hwnd, HUB_TIMER);
        return;
    }
    hub_lobby_kick();
}

/* Ergebnis uebernehmen (W472: Zustand + Abstand bis zum naechsten Abruf). */
static void hubl_apply(const lacc_lobbies_t *r)
{
    int st;
    if (r->ok) {
        st = 1;
        g_hubl_fails = 0;
        g_hubl_wait = HUB_LOB_MS;
    } else if (!strcmp(r->error, "no_account")) {
        st = -4;
    } else if (r->http == 401 || !strcmp(r->error, "auth_failed")) {
        st = -5; /* kein Endlos-Wiederholen: erst beim naechsten Oeffnen der Seite */
    } else if (r->http == 429 || !strcmp(r->error, "rate_limited")) {
        ULONGLONG ra = (ULONGLONG)(r->retry_after > 0 ? r->retry_after : 0) * 1000;
        st = -6;
        g_hubl_wait = ra < HUB_LOB_RATE_MS ? HUB_LOB_RATE_MS : ra > HUB_LOB_MAX_MS ? HUB_LOB_MAX_MS : ra;
    } else { /* Server ohne Aktion (404) oder Netz-/Serverfehler: wachsender Abstand */
        st = r->http == 404 || !strcmp(r->error, "not_found") ? -1 : 0;
        if (g_hubl_fails < 8) g_hubl_fails++;
        g_hubl_wait = (ULONGLONG)HUB_LOB_MS << (g_hubl_fails - 1 < 4 ? g_hubl_fails - 1 : 4);
        if (g_hubl_wait > HUB_LOB_ERR_MS) g_hubl_wait = HUB_LOB_ERR_MS;
    }
    GetLocalTime(&g_hubl_when);
    if (st == 1) {
        g_hubl = *r;
        g_hubl_state = 0; /* neu zeichnen, auch wenn schon eine Liste stand */
    }
    hubl_set_state(st);
}

/* Selbsttest: Ergebnis wie aus dem Abruf (ohne Netz) */
void hub_lobby_result(const lacc_lobbies_t *r)
{
    hubl_apply(r);
}

int hub_wndproc(UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_TIMER && wp == HUB_TIMER) {
        hub_tick();
        return 1;
    }
    if (msg == WM_APP_HUBL) {
        hubl_job_t *j = (hubl_job_t *)lp;
        if (j && (int)wp == g_hubl_gen && g_hubl_busy) {
            g_hubl_busy = 0;
            hubl_apply(&j->r);
        }
        free(j);
        return 1;
    }
    return 0;
}

void hub_page_shown(int page)
{
    if (hub_area_of(page) == HUB_GAME && page != P_ALPHA) {
        g_hub_gpage = page;
    }
    if (!g_hwnd) {
        return;
    }
    if (ctl(ID_GAME_SB)) InvalidateRect(ctl(ID_GAME_SB), NULL, FALSE); /* W473: Unterstreichung nur im Spielbereich */
    if (ctl(ID_GAME_LOC)) InvalidateRect(ctl(ID_GAME_LOC), NULL, FALSE);
    if (page == P_NETI) {
        SetTimer(g_hwnd, HUB_TIMER, HUB_TICK_MS, NULL);
        acc_page_shown(); /* W473: Zeile Konto - Konto laden, Status einmal holen (nur Stufe steelnet, nie im Bild) */
        if ((g_hubl_state == -5 || g_hubl_state == -4) && (g_o.shot == 0 || g_hubl_dry)) {
            /* W472: neues Oeffnen = ein neuer Versuch (Konto geaendert?); Bilder behalten den Zustand der Attrappe */
            g_hubl_state = -2;
            g_hubl_t0 = 0;
            g_hubl_wait = HUB_LOB_MS;
        }
        hub_lobby_kick();
    } else {
        KillTimer(g_hwnd, HUB_TIMER);
    }
}

/* Stufe gewechselt: Liste vergessen (laufendes Ergebnis verwerfen), auf der Seite gleich neu holen. */
void hub_net_changed(void)
{
    g_hubl_gen++;
    g_hubl_busy = 0;
    g_hubl_state = -2;
    g_hubl_t0 = 0;
    g_hubl_wait = HUB_LOB_MS;
    g_hubl_fails = 0;
    memset(&g_hubl, 0, sizeof g_hubl);
    hub_lobby_kick();
}

/* Bilder/Selbsttest: Beispiel-Liste (erfundene Namen wie in der Suchliste, kein Netz). */
void hub_lobby_fixture(int n)
{
    static const char *const kName[LACC_LOB_MAX] = { "Kestrel", "Guest-Rook", "Ironside", "Halberd", "Mako",
                                                      "Vanguard", "Talon", "Guest-Wren", "Basalt", "Corvid",
                                                      "Sable", "Onager" };
    int i;
    memset(&g_hubl, 0, sizeof g_hubl);
    g_hubl_state = n >= 0 ? 1 : n == -1 ? -1 : n == -3 ? 0 : (n <= -4 && n >= -6) ? n : -2;
    for (i = 0; i < n && i < LACC_LOB_MAX; i++) {
        lacc_lobby_t *e = &g_hubl.l[i];
        strcpy_s(e->host, sizeof e->host, kName[i]);
        e->arcade = i == 1 || i == 4;
        e->max_players = 4 + 2 * (i % 4);
        e->players = 1 + i % 3;
        e->started = n >= 6 && i == n - 1; /* bei 6: fuenf offen, eine laeuft (Server: offene zuerst) */
        if (e->started) e->players = e->max_players;
    }
    g_hubl.n = n > 0 ? (n < LACC_LOB_MAX ? n : LACC_LOB_MAX) : 0;
    g_hubl.ok = n >= 0;
    if (n >= 0 && n <= 4) { /* W485: Bilder/Selbsttest mit Xbox-Live-Abschnitt (zwei Sessions, eine laeuft) */
        static const char *const kLive[2] = { "Harrier", "Guest-Lynx" };
        for (i = 0; i < 2; i++) {
            lacc_lobby_t *e = &g_hubl.lv[i];
            strcpy_s(e->host, sizeof e->host, kLive[i]);
            e->max_players = 10;
            e->players = i == 0 ? 2 : 10;
            e->started = i == 1;
        }
        g_hubl.nlive = 2;
    }
    g_hubl_when.wHour = 12;
    g_hubl_when.wMinute = 5;
}

/* ------------------------------------------------------------------ */
/* Zeichnen                                                           */
/* ------------------------------------------------------------------ */
void hub_paint_nav(void)
{
    int a = hub_area_of(g_page);
    if (a == HUB_GAME || !kHub[a].title) {
        return;
    }
    L_text(F_TINY, LT(kHub[a].title), 26, TITLE_H + 16 + 2, NAV_W - 40, 20, C_FAINT,
           DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

/* Zeile Schluessel | Wert (Karten Status/Piloten) */
static void hub_kv(int x, int y, int w, const char *key, const char *val, COLORREF c)
{
    label1(F_TINY, LT(key), x + 18, y, 150, 22, C_DIM);
    label1(F_TINY, val, x + 172, y, w - 190, 22, c);
}

#define NETI_ST_H 206
#define NETI_PIL_Y (CY0 + NETI_ST_H + 12)
#define NETI_PIL_H 274
#define NETI_LOB_H 318
#define NETI_LAN_Y (CY0 + NETI_LOB_H + 12)
#define NETI_LAN_H 162
#define NETI_ROW 26
#define NETI_LOB_ROWS 8

static void paint_neti_status(const snp_view_t *v)
{
    char t[160];
    int x = CX0, y = CY0 + 40; /* W473: 4 hoeher, Platz fuer die Zeile Konto */
    COLORREF ac = C_DIM;
    card(x, CY0, COLW, NETI_ST_H, LT("gui.hub_st_title"));
    hub_kv(x, y, COLW, "gui.hub_st_conn", LT(v->state > 0 ? "gui.hub_st_on" : v->state == 0 ? "gui.hub_st_off"
                                                                                           : "gui.hub_st_wait"),
           v->state > 0 ? C_OK : v->state == 0 ? C_WARN : C_DIM);
    y += NETI_ROW;
    if (v->own >= 0) {
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.hub_st_ms_v"), v->ms);
        hub_kv(x, y, COLW, "gui.hub_st_ms", t, C_TEXT);
    } else {
        hub_kv(x, y, COLW, "gui.hub_st_ms", "-", C_FAINT);
    }
    y += NETI_ROW;
    hub_kv(x, y, COLW, "gui.hub_st_ver", v->ver && v->ver[0] ? v->ver : "-", v->ver && v->ver[0] ? C_TEXT : C_FAINT);
    y += NETI_ROW;
    if (loc_ver_gate()) {
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.hub_st_old"), g_locver_min);
        hub_kv(x, y, COLW, "gui.hub_st_min", t, C_WARN);
    } else {
        hub_kv(x, y, COLW, "gui.hub_st_min", g_locver_min[0] ? g_locver_min : LT("gui.hub_st_none"),
               g_locver_min[0] ? C_TEXT : C_FAINT);
    }
    y += NETI_ROW;
    if (v->own >= 0) {
        _snprintf_s(t, sizeof t, _TRUNCATE, "%02d:%02d" HDR_SUB_SEP "%s", v->hh, v->mm, LT("gui.hub_st_every"));
        hub_kv(x, y, COLW, "gui.hub_st_when", t, C_DIM);
    } else {
        hub_kv(x, y, COLW, "gui.hub_st_when", "-", C_FAINT);
    }
    y += NETI_ROW;
    /* W473: Konto dieses PCs in denselben Worten wie die Konto-Karte unter Multiplayer */
    if (hub_acc_value(t, sizeof t, &ac)) {
        hub_kv(x, y, COLW, "gui.hub_st_acc", t, ac);
    }
}

/* W473: Wert der Zeile Konto (steelnet > Info): ohne Konto "Not signed in" wie die Konto-Karte, sonst acc_sum_row
 * ("Name · secured / not secured / status unknown"). 0 = Dienst nicht eingerichtet. Lage fuer den Selbsttest. */
int hub_acc_value(char *t, size_t cap, COLORREF *c)
{
    if (!acc_sum_row(t, cap, c)) {
        return 0;
    }
    if (acc_card_state() == ACCS_NONE) {
        strcpy_s(t, cap, LT("gui.acc_c_none"));
        *c = C_DIM;
    }
    return 1;
}
int hub_acc_fits(const char *t)
{
    return ui_text_fits(F_TINY, t, CX0 + 172, CY0 + 40 + 5 * NETI_ROW, COLW - 190, 22, 1);
}

static void paint_neti_pilots(const snp_view_t *v)
{
    char t[400];
    int x = CX0, y = NETI_PIL_Y + 44, i;
    size_t k = 0;
    card(x, NETI_PIL_Y, COLW, NETI_PIL_H, LT("gui.hub_pil_title"));
    if (!v->has) {
        label(F_TINY, LT(v->state < 0 ? "gui.hub_st_wait" : "gui.hub_pil_nonum"), x + 18, y, COLW - 36, 40, C_DIM);
        y += 4 * NETI_ROW;
    } else {
        static const char *const kKey[5] = { "gui.hub_pil_on", "gui.hub_pil_peak", "gui.hub_pil_lob",
                                             "gui.hub_pil_gam", "gui.hub_pil_today" };
        int val[5];
        val[0] = v->players;
        val[1] = v->peak;
        val[2] = v->lobbies;
        val[3] = v->games;
        val[4] = v->today;
        for (i = 0; i < 5; i++) {
            if (val[i] < 0) {
                continue; /* aelterer Server ohne den Wert */
            }
            _snprintf_s(t, sizeof t, _TRUNCATE, "%d", val[i]);
            hub_kv(x, y, COLW, kKey[i], t, i == 0 ? C_TEXT : C_DIM);
            y += NETI_ROW;
        }
    }
    /* Hosts der offenen Lobbys (nur aus der oeffentlichen Liste) */
    t[0] = '\0';
    if (g_hubl_state == 1) {
        for (i = 0; i < g_hubl.n; i++) {
            if (g_hubl.l[i].started) continue;
            _snprintf_s(t + k, sizeof t - k, _TRUNCATE, "%s%s", k ? ", " : "", g_hubl.l[i].host);
            k = strlen(t);
        }
        label1(F_TINY, LT("gui.hub_pil_hosts"), x + 18, y, 150, 22, C_DIM);
        L_text(F_TINY, k ? t : LT("gui.hub_pil_nohost"), x + 172, y + 3, COLW - 190, 40, k ? C_TEXT : C_FAINT,
               DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    label(F_TINY, LT("gui.hub_pil_note"), x + 18, NETI_PIL_Y + NETI_PIL_H - 46, COLW - 36, 34, C_FAINT);
}

static void paint_neti_lobbies(void)
{
    char t[160];
    int x = COL2, y0 = CY0, i, shown = 0, rest;
    int sl_rows = NETI_LOB_ROWS, lv_rows = 0, lv_shown = 0, ly = y0 + 44; /* W485 */
    card(x, y0, COLW, NETI_LOB_H, LT("gui.hub_lob_title"));
    if (g_hubl_state != 1) {
        const char *m;
        switch (g_hubl_state) {
        case -2: m = "gui.hub_lob_wait"; break;
        case -1: m = "gui.hub_lob_nosrv"; break;
        case -4: m = "gui.hub_lob_noacc"; break; /* W472: nur mit steelnet-Konto (+ Knopf ID_HUB_GOACC) */
        case -5: m = "gui.hub_lob_auth"; break;
        case -6: m = "gui.hub_lob_rate"; break;
        default: m = "gui.hub_lob_err"; break;
        }
        label(F_TINY, LT(m), x + 18, y0 + 44, COLW - 36, (g_hubl_state == -4 || g_hubl_state == -5) ? 56 : 80,
              (g_hubl_state == 0 || g_hubl_state == -5) ? C_WARN : C_DIM);
        return;
    }
    if (g_hubl.n > 0) { /* Zahl rechts = offene Lobbys (laufende Spiele stehen gedimmt darunter) */
        int open = g_hubl.more;
        for (i = 0; i < g_hubl.n; i++) {
            open += !g_hubl.l[i].started;
        }
        _snprintf_s(t, sizeof t, _TRUNCATE, "%d", open);
        L_text(F_CARD, t, x + 18, y0 + 12, COLW - 36, 24, C_ACCENT, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }
    /* W485 LIVE-QOS: Xbox-Live-Sessions (Gruppe "live", W482) als eigener, markierter Abschnitt unter den
     * System-Link-Lobbys (Nutzerentscheid 25.11.); hoechstens 3 Zeilen, die System-Link-Liste ruecken zusammen. */
    if (g_hubl.nlive > 0) {
        lv_rows = g_hubl.nlive < 3 ? g_hubl.nlive : 3;
        sl_rows = NETI_LOB_ROWS - 1 - lv_rows;
    }
    if (g_hubl.n == 0) {
        label(F_TINY, LT("gui.hub_lob_none"), x + 18, y0 + 44, COLW - 36, 40, C_DIM);
        ly = y0 + 44 + NETI_ROW + 4;
    } else {
        int hy = y0 + 44;
        label1(F_TINY, LT("gui.hub_lob_host"), x + 18, hy, 130, 20, C_FAINT);
        label1(F_TINY, LT("gui.hub_lob_mode"), x + 156, hy, 62, 20, C_FAINT);
        label1(F_TINY, LT("gui.hub_lob_pl"), x + 224, hy, 52, 20, C_FAINT);
        label1(F_TINY, LT("gui.hub_lob_st"), x + 278, hy, 52, 20, C_FAINT);
        L_fill(x + 18, hy + 21, COLW - 36, 1, RGB(255, 255, 255), 22, 0);
        for (i = 0; i < g_hubl.n && shown < sl_rows; i++, shown++) {
            const lacc_lobby_t *e = &g_hubl.l[i];
            int ry = hy + 26 + shown * NETI_ROW;
            COLORREF bg = e->arcade ? RGB(0xD2, 0x78, 0x0F) : RGB(0x3C, 0x6E, 0x9B); /* Farben der Suchliste (W443) */
            L_text(F_TINY, e->host, x + 18, ry, 132, 22, e->started ? C_DIM : C_TEXT,
                   DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            L_fill(x + 156, ry + 3, 58, 16, bg, e->started ? 140 : 255, 4);
            L_text(F_TINY, LT(e->arcade ? "gui.hub_lob_arc" : "gui.hub_lob_sim"), x + 156, ry + 3, 58, 16,
                   RGB(255, 255, 255), DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            _snprintf_s(t, sizeof t, _TRUNCATE, "%d/%d", e->players, e->max_players);
            label1(F_TINY, t, x + 224, ry, 52, 22, e->started || e->players >= e->max_players ? C_DIM : C_TEXT);
            label1(F_TINY, LT(e->started ? "gui.hub_lob_run" : "gui.hub_lob_open"), x + 278, ry, 52, 22,
                   e->started ? C_FAINT : C_OK);
        }
        ly = hy + 26 + shown * NETI_ROW + 4;
    }
    if (lv_rows > 0) { /* W485: Kopf mit Kennzeichen, dann die Sessions (Host, LIVE/CHAT, Plaetze, Zustand) */
        L_fill(x + 18, ly, COLW - 36, 1, RGB(255, 255, 255), 22, 0);
        L_fill(x + 18, ly + 6, 4, 14, RGB(0x6A, 0xB0, 0x2E), 255, 1);
        label1(F_TINY, LT("gui.hub_lob_live"), x + 28, ly + 2, COLW - 46, 22, C_ACCENT);
        for (i = 0; i < g_hubl.nlive && lv_shown < lv_rows; i++, lv_shown++) {
            const lacc_lobby_t *e = &g_hubl.lv[i];
            int ry = ly + 26 + lv_shown * NETI_ROW;
            L_text(F_TINY, e->host, x + 18, ry, 132, 22, e->started ? C_DIM : C_TEXT,
                   DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            L_fill(x + 156, ry + 3, 58, 16, RGB(0x4E, 0x8A, 0x22), e->started ? 140 : 255, 4);
            L_text(F_TINY, LT(e->chat ? "gui.hub_lob_live_chat" : "gui.hub_lob_live_match"), x + 156, ry + 3, 58, 16,
                   RGB(255, 255, 255), DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            _snprintf_s(t, sizeof t, _TRUNCATE, "%d/%d", e->players, e->max_players);
            label1(F_TINY, t, x + 224, ry, 52, 22, e->started || e->players >= e->max_players ? C_DIM : C_TEXT);
            label1(F_TINY, LT(e->started ? "gui.hub_lob_run" : "gui.hub_lob_open"), x + 278, ry, 52, 22,
                   e->started ? C_FAINT : C_OK);
        }
    }
    rest = g_hubl.n - shown + g_hubl.more + g_hubl.nlive - lv_shown + g_hubl.live_more;
    if (rest > 0) {
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.hub_lob_more"), rest);
        label1(F_TINY, t, x + 18, y0 + NETI_LOB_H - 54, COLW - 36, 20, C_DIM);
    }
    _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.hub_lob_upd"), g_hubl_when.wHour, g_hubl_when.wMinute);
    label1(F_TINY, t, x + 18, y0 + NETI_LOB_H - 32, COLW - 36, 20, C_FAINT);
}

/* LAN-Karte: verbunden rechts unten (COLW), ohne steelnet volle Breite unter dem Hinweis */
static void paint_neti_lan(int x, int y, int w, int h)
{
    int wide = w > COLW;
    card(x, y, w, h, LT("gui.hub_lan_title"));
    label(F_TINY, LT("gui.hub_lan_text"), x + 18, y + 40, w - 36, wide ? 40 : 72, C_DIM);
    if (g_loc_lan) {
        label(F_TINY, LT("gui.hub_lan_on"), x + 18, y + (wide ? 82 : 114), w - 36, wide ? 20 : 36, C_ACCENT_HI);
    }
}

void paint_neti(void)
{
    snp_view_t v;
    if (!hub_net_ready()) { /* nicht verbunden: kurzer Hinweis + Weg in die Einstellungen (Knopf ID_HUB_GOOPT) */
        card(CX0, CY0, CW, 156, LT("gui.hub_off_title"));
        label(F_TINY, LT(onl_srv_on() ? "gui.hub_off" : "gui.loc_onl_nosrv"), CX0 + 18, CY0 + 42, CW - 36, 56,
              C_DIM);
        paint_neti_lan(CX0, CY0 + 156 + 12, CW, 116);
        return;
    }
    snp_view(&v);
    paint_neti_status(&v);
    paint_neti_pilots(&v);
    paint_neti_lobbies();
    paint_neti_lan(COL2, NETI_LAN_Y, COLW, NETI_LAN_H);
}
