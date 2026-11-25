/* launcher_gui_netrep.c - W496 NET-TELEMETRY: Launcher-Frage "anonyme Netzwerkstatistik" (Stufe 2) und Schalter.
 *
 * Einmaliger Dialog (Karte ueber dem Fenster, alles andere ausgeblendet) mit zwei Knoepfen; "Yes" ist der
 * Standard-Knopf (Fokus, Enter). Keine vorab gesetzte Zustimmung: bis zur Antwort sendet der Client nichts
 * (net_report=0 im Start-Set). Esc bzw. Fenster schliessen = "No" fuer jetzt; dann beim naechsten Start noch genau
 * einmal fragen (launcher_net_report_dismiss), danach nicht mehr. Gefragt wird nur mit Stufe "Mit steelnet verbinden"
 * und nach der Erstabfrage "Online & Updates" (Netzbericht geht nur ueber steelnet).
 *
 * user.cfg: launcher_net_report (fehlt = nie beantwortet, 0 nein, 1 ja), launcher_net_report_dismiss (Zahl).
 * LoC-Start: net_report=0/1 im --set (launcher_gui_loc.c), der Client liest sonst nichts davon.
 * Schalter: LoC-Bereich Multiplayer > Match & Netzwerk > Reiter Netzwerk, Zeile mit dem Dialogtitel als Kurztext. */
#include "launcher_gui_int.h"

#define NRQ_KEY "launcher_net_report"
#define NRQ_DISMISS_KEY "launcher_net_report_dismiss"
#define NRQ_X (CX0 + 8)
#define NRQ_W (CW - 16)
#define NRQ_Y (CY0 + 56)
#define NRQ_H 312
#define NRQ_YES_W 280
#define NRQ_NO_W 160

int g_nrq_cfg = -1;      /* -1 nie beantwortet, 0 nein, 1 ja */
int g_nrq_dismiss = 0;   /* Schliessen ohne Antwort (Esc/X) */
int g_nrq_open = 0;      /* Dialog sichtbar */
int g_nrq_asked_now = 0; /* in diesem Launcher-Lauf schon gezeigt (nur einmal je Start) */

/* Frage zeigen? Rein, fuer den Selbsttest. */
int nrq_should_ask(int cfg, int dismiss, int steel, int upd_ask)
{
    return cfg < 0 && dismiss < 2 && steel && !upd_ask;
}

/* Wert fuer den LoC-Start: nur genau "ja" sendet. */
int nrq_consent(void)
{
    return g_nrq_cfg == 1;
}

/* ";net_report=0/1" an den --set des LoC-Starts: 1 nur online und mit Zustimmung, sonst ausdruecklich 0 (ein alter
 * Wert in user_loc.cfg zaehlt nicht). */
void nrq_set_append(char *set, size_t cap, int onl)
{
    size_t k = strlen(set);
    _snprintf_s(set + k, cap - k, _TRUNCATE, "%snet_report=%d", k ? ";" : "", onl && nrq_consent() ? 1 : 0);
}

void nrq_load(void)
{
    char up[1200], line[256];
    FILE *f = NULL;
    g_nrq_cfg = -1;
    g_nrq_dismiss = 0;
    user_cfg_path(up, sizeof up);
    if (u8_fopen_s(&f, up, "r") != 0 || !f) return;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, NRQ_KEY "=", sizeof NRQ_KEY) == 0) { /* letzte Zeile gewinnt; nur 0/1 */
            const char *v = line + sizeof NRQ_KEY;
            g_nrq_cfg = v[0] == '1' ? 1 : v[0] == '0' ? 0 : -1;
        } else if (strncmp(line, NRQ_DISMISS_KEY "=", sizeof NRQ_DISMISS_KEY) == 0) {
            int d = atoi(line + sizeof NRQ_DISMISS_KEY);
            g_nrq_dismiss = d < 0 ? 0 : d > 9 ? 9 : d;
        }
    }
    fclose(f);
}

static void nrq_show(int on)
{
    g_nrq_open = on;
    ui_refresh_visibility();
    if (on && g_hwnd && ctl(ID_NRQ_YES) && g_o.shot == 0) {
        SetFocus(ctl(ID_NRQ_YES)); /* Standard-Knopf im eigenen Fenster (kein Fensterwechsel) */
    }
    redraw_all();
}

/* Pruefen und ggf. zeigen (Start, nach der Erstabfrage, nach Stufenwechsel). */
void nrq_eval(void)
{
    if (g_nrq_open || g_nrq_asked_now) return;
    if (nrq_should_ask(g_nrq_cfg, g_nrq_dismiss, netm_get() == NETM_STEEL, g_upd_ask)) {
        g_nrq_asked_now = 1;
        nrq_show(1);
    }
}

void nrq_set(int yes)
{
    g_nrq_cfg = yes ? 1 : 0;
    onl_save_key(NRQ_KEY, g_nrq_cfg); /* Shots/Selbsttest: nie geschrieben */
    if (ctl(ID_LNET_NETREP)) ui_set_checked(ID_LNET_NETREP, g_nrq_cfg == 1);
}

static void nrq_answer(int yes)
{
    nrq_set(yes);
    set_status_k(LT(yes ? "gui.nrq_done_yes" : "gui.nrq_done_no"), 1);
    nrq_show(0);
}

/* Esc bzw. Fenster zu ohne Antwort: wie "No" (nichts senden), naechster Start fragt hoechstens noch einmal. */
void nrq_dismiss(void)
{
    if (!g_nrq_open) return;
    g_nrq_dismiss++;
    onl_save_key(NRQ_DISMISS_KEY, g_nrq_dismiss);
    g_nrq_open = 0;
    if (g_hwnd) {
        ui_refresh_visibility();
        redraw_all();
    }
}

void nrq_build(HWND w)
{
    int by = NRQ_Y + NRQ_H - 52;
    mk(w, K_ACCENT, ID_NRQ_YES, P_PLAY, NRQ_X + NRQ_W - 22 - NRQ_YES_W, by, NRQ_YES_W, 34, LT("gui.nrq_yes"), 0);
    mk(w, K_BTN, ID_NRQ_NO, P_PLAY, NRQ_X + NRQ_W - 22 - NRQ_YES_W - 12 - NRQ_NO_W, by, NRQ_NO_W, 34, LT("gui.nrq_no"), 0);
}

void nrq_relabel(void)
{
    if (ctl(ID_NRQ_YES)) ui_settext(ctl(ID_NRQ_YES), LT("gui.nrq_yes"));
    if (ctl(ID_NRQ_NO)) ui_settext(ctl(ID_NRQ_NO), LT("gui.nrq_no"));
}

/* Sichtbarkeit: offen = nur die beiden Knoepfe (und Fenster-Knoepfe), alles andere aus; sonst Knoepfe aus. */
int nrq_visible(int id)
{
    if (id == ID_NRQ_YES || id == ID_NRQ_NO) return g_nrq_open;
    if (!g_nrq_open || id == ID_CLOSE || id == ID_MIN) return -1;
    return 0;
}

int nrq_command(int id)
{
    if (id == ID_NRQ_YES || id == ID_NRQ_NO) {
        nrq_answer(id == ID_NRQ_YES);
        return 1;
    }
    return 0;
}

void nrq_paint(void)
{
    if (!g_nrq_open) return;
    L_fill(0, TITLE_H, WIN_W, WIN_H - TITLE_H, C_DIALOG, 235, 0);
    L_fill(NRQ_X, NRQ_Y, NRQ_W, NRQ_H, C_DIALOG, 255, 12); /* deckender Grund unter der Karte */
    card(NRQ_X, NRQ_Y, NRQ_W, NRQ_H, LT("gui.nrq_title"));
    label(F_SMALL, LT("gui.nrq_text"), NRQ_X + 22, NRQ_Y + 50, NRQ_W - 44, NRQ_H - 50 - 70, C_TEXT);
}
