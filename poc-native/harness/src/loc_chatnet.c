/* loc_chatnet.c - W437 LOC-CHAT: Textchat in LoC-Lobby und -Match (GitHub #77, net/SECURITY.md Abschnitt W437).
 *
 * Kanal: SBCH-Paket (loc_chat.c) als SBT2-Datagramm des eigenen Transports mit Quell- und Zielspielport 1262
 * (loctun_set_chat, nie an das Spiel, nicht ueber den Treffpunkt gespeichert, an alle verbundenen Mitspieler der
 * Lobby bzw. des Matches). Absendername = Pilotname der Sitzung (peer_info, an die Schluessel gebunden). Nur mit
 * Treffpunkt und sec=ok; Original-Transport (LAN) und Steel Battalion: kein Chat. Ausserhalb der Simulation: der
 * Chat aendert keinen Spielzustand; bei offener Zeile bekommt das Spiel keine Eingabe (hooks_input/hooks_bots).
 * Datenschutz: keine Inhalte, keine Namen in Logs (nur Zaehler), Puffer werden geloescht (Senden, Empfang,
 * Absturz vor dem Minidump), die Anzeige wird nach Clip/Video/F11-Bild gezeichnet (nicht darin enthalten). */
#include "loc_tun.h"
#include "render_int.h"
#include "sb_overlay.h"
#include "overlay_style.h" /* W461 OVERLAY-CORE */
#include "sb_lang.h"
#include "loc_chat.h"
#include "loc_chatnet.h"
#include "loc_net.h"
#include "loc_net_core.h"
#include <imm.h>     /* W452: IME nur bei offener Chatzeile */
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef uint32_t u32;
#pragma comment(lib, "imm32.lib")

void imouse_release_keys(void); /* input_mouse.c (UI-Thread) */
HWND sb_render_hwnd(void);       /* d3d11_render.c (W452: Fenster ohne eigene Nachricht kennen) */

static volatile LONG s_inst, s_typing, s_avail, s_wiped;
static CRITICAL_SECTION s_cs;
static lch_state s_st;
static lch_input s_in;
static volatile LONG s_in_gen, s_hist_t; /* Eingabe geaendert / Zeit der neuesten Zeile des aktiven Tabs (Sichtbarkeit) */
/* W446 STEELNET-CHAT: zwei Tabs. TAB_LOBBY = Mitspieler der Lobby/des Matches (SBCH ueber SBT2, W437), TAB_SN = alle,
 * die mit steelnet verbunden sind (Steuerkanal des Treffpunkts). Lobby nur mit verbundenem Mitspieler. */
/* W474 CHATTABS: Reihenfolge steelnet | Lobby | Team (Anzeige und Tab-Taste, loc_chat.h LCH_TAB_*). Lobby/Team nur in
 * einer Lobby bzw. im Match dieser Lobby (loc_net_lobby_state), im Match je Reiter abschaltbar (loc_chat_mission_*).
 * Team: vorbereitet, nie verfuegbar (Zuordnung Platz -> Seite nicht belegt, BERICHT_M2 W474 Teil B). */
enum { TAB_SN = LCH_TAB_SN, TAB_LOBBY = LCH_TAB_LOBBY, TAB_TEAM = LCH_TAB_TEAM, TAB_N = LCH_TAB_N };
static lch_sn s_sn;                         /* Verlauf + Zaehler des steelnet-Tabs (unter s_cs) */
static lch_hist s_team_hist;                /* W474: Verlauf des Team-Reiters (vorbereitet) */
static volatile LONG s_tab, s_av_mask;      /* aktiver Reiter / verfuegbare Reiter (Bit 1 << TAB_*) */
static volatile LONG s_ses, s_gate_sn;      /* W474: Lobby-Zustand (LCH_SES_*) / steelnet-Empfang im Match verwerfen */
static u32 s_rx_off_lobby, s_rx_off_sn, s_ses_clear; /* W474: verworfen (Reiter nicht verfuegbar) / Verlauf geleert */
static volatile LONG s_tab_t[TAB_N];        /* Zeit der neuesten Zeile je Tab */
static volatile LONG s_unread[TAB_N];       /* neue Zeilen im jeweils anderen Tab (Reiter-Zaehler) */
static volatile LONG s_blink_t[TAB_N];      /* W452: Zeit der letzten neuen Zeile im nicht aktiven Tab (Pulsen) */
#define CHAT_PULSE_MS 500u                  /* W452: ein sanfter Puls des Reiters ... */
#define CHAT_PULSES 3u                      /* ... drei Mal, danach bleibt die Markierung bis zum Wechsel */
/* W452 IME: Komposition (unter s_cs), Lage der Eingabezeile im Backbuffer (Render-Thread schreibt, UI-Thread liest) */
static wchar_t s_comp[64];
static volatile LONG s_composing, s_in_rect[4];
static int s_ime_on = -1;                   /* UI-Thread: IME-Kontext am Fenster (-1 = noch nie gesetzt) */
static int s_ime_pending;                   /* UI-Thread: UTF-16-Einheiten aus GCS_RESULTSTR, deren WM_IME_CHAR verworfen wird */
static u32 s_ime_res_n, s_ime_dup_n, s_ime_comp_n, s_ime_key_n; /* Zaehler (nur Zahlen) */
static wchar_t s_ime_test_comp[64], s_ime_test_res[64]; /* TEST (Schritte J/R): Ersatz fuer ImmGetCompositionString */
static char s_self[LCH_NAME_MAX + 1];       /* eigener Pilotname beim Dienst (unter s_cs, vom Chat-Thread) */
static u32 s_tab_auto, s_tab_key;           /* Tab-Wechsel automatisch / per Taste (Zaehler) */
/* Belegung der Aktion "chat": erst mit geladenem Profil (Standard default_full: Enter). Ohne Profil (Legacy, Enter =
 * START) keine Chat-Taste - wie der Launcher die Legacy-Belegung zeigt (keyboard_default ohne chat-Zeile). */
static volatile LONG s_vk[4], s_mod[4];
#define CHAT_MSG_TEST_OPEN (WM_APP + 0x437) /* TEST (loc_chat_test, Schritt O): Zeile oeffnen wie die Chat-Taste */
static HWND volatile s_hwnd;
static int s_tun_set;
static u32 s_tx_send_fail, s_open_n, s_cmd_n;

/* ------------------------------------------------------------------ Hilfen */
static void cz(void *p, size_t n) { SecureZeroMemory(p, n); }

static u32 now_ms(void) { return GetTickCount(); }

static int chat_mods_now(void)
{
    return ((GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 0) | ((GetKeyState(VK_CONTROL) & 0x8000) ? 2 : 0) |
           ((GetKeyState(VK_MENU) & 0x8000) ? 4 : 0);
}

static lch_hist *tab_hist(int tab) { return tab == TAB_SN ? &s_sn.hist : tab == TAB_TEAM ? &s_team_hist : &s_st.hist; }

/* W446: neue Zeile im Tab tab (unter s_cs): Sichtbarkeit bzw. Ungelesen-Zaehler des anderen Tabs */
static void tab_note(int tab)
{
    u32 now = now_ms();
    InterlockedExchange(&s_tab_t[tab], (LONG)now);
    if (tab == (int)s_tab) InterlockedExchange(&s_hist_t, (LONG)now);
    else {
        if (s_unread[tab] < 99) InterlockedIncrement(&s_unread[tab]);
        InterlockedExchange(&s_blink_t[tab], (LONG)(now | 1u)); /* W452: Reiter pulst (0 = nie) */
    }
}

static const char *tab_name(int tab) { return tab == TAB_SN ? "steelnet" : tab == TAB_TEAM ? "Team" : "Lobby"; }
static int tab_avail(int tab) { return tab >= 0 && tab < TAB_N && (((u32)s_av_mask >> tab) & 1u) != 0; }

static void sys_line_tab(int tab, const wchar_t *t) /* unter s_cs */
{
    wchar_t nm[24];
    char a[LCH_NAME_MAX + 1];
    int i;
    sb_lang_w("chat.sys", nm, 24);
    for (i = 0; nm[i] && i < LCH_NAME_MAX; i++) a[i] = (nm[i] >= 0x20 && nm[i] < 0x7F) ? (char)nm[i] : '?';
    a[i] = 0;
    lch_hist_push(tab_hist(tab), LCH_L_SYS, a, t, now_ms());
    tab_note(tab);
}
static void sys_line_w(const wchar_t *t) { sys_line_tab((int)s_tab, t); } /* unter s_cs, aktiver Tab */
static void sys_line_key(const char *key, const char *arg) /* unter s_cs */
{
    wchar_t fmt[200], t[300], wa[LCH_NAME_MAX + 1];
    int i;
    sb_lang_w(key, fmt, 200);
    for (i = 0; arg && arg[i] && i < LCH_NAME_MAX; i++) wa[i] = (wchar_t)(unsigned char)arg[i];
    wa[i] = 0;
    if (wcsstr(fmt, L"%s")) _snwprintf_s(t, 300, _TRUNCATE, fmt, wa);
    else wcsncpy_s(t, 300, fmt, _TRUNCATE);
    sys_line_w(t);
    cz(t, sizeof t);
    cz(wa, sizeof wa);
}

/* ------------------------------------------------------------------ Kanal */
static void chat_rx(uint32_t vip, const char *pilot, const uint8_t *d, int len) /* Transport-Thread */
{
    if (s_wiped || !g_cfg.loc_chat) return;
    if (!tab_avail(TAB_LOBBY)) { /* W474: nicht in einer Lobby bzw. im Match abgeschaltet -> verwerfen (nie nachliefern) */
        s_rx_off_lobby++;
        return;
    }
    EnterCriticalSection(&s_cs);
    if (lch_rx(&s_st, now_ms(), vip, pilot, d, len)) tab_note(TAB_LOBBY);
    LeaveCriticalSection(&s_cs);
}

/* W446: steelnet-Chat vom Steuerkanal (Transport-Thread, unter dessen Sperre; hier nur s_cs). */
static void sn_rx(int kind, const char *from, const uint8_t *text, int len)
{
    if (s_wiped || !g_cfg.loc_snchat) return;
    if (s_gate_sn) { /* W474: im Match mit loc_chat_mission_sn=0 -> verwerfen (kein Puls, kein Zaehler, nie nachliefern) */
        s_rx_off_sn++;
        return;
    }
    EnterCriticalSection(&s_cs);
    if (kind == 2) { /* Statuscode des Dienstes -> Systemzeile im steelnet-Tab (nur bekannte Codes) */
        static const char *const codes[] = { "rate", "busy", "muted", "account_required", "too_long", "empty", "off" };
        static const char *const keys[] = { "chat.sn_rate", "chat.sn_busy", "chat.sn_muted", "chat.sn_account_required",
                                            "chat.sn_too_long", "chat.sn_empty", "chat.sn_off" }; /* lang_check: Literale */
        char code[24] = "";
        int i;
        for (i = 0; i < len && i < (int)sizeof code - 1; i++) code[i] = (char)text[i];
        code[i] = 0;
        s_sn.status_n++;
        for (i = 0; i < (int)(sizeof codes / sizeof codes[0]); i++) {
            if (strcmp(code, codes[i]) == 0) {
                wchar_t w[200];
                sb_lang_w(keys[i], w, 200);
                sys_line_tab(TAB_SN, w);
                break;
            }
        }
    } else if (lch_sn_rx(&s_sn, &s_st.mute, now_ms(), from, text, len, kind == 1, s_self)) {
        tab_note(TAB_SN);
    }
    LeaveCriticalSection(&s_cs);
}

/* W446: Tab wechseln (UI- bzw. Chat-Thread). why 0 = Taste, 1 = automatisch. */
static void tab_set(int tab, int why)
{
    if ((int)s_tab == tab) return;
    InterlockedExchange(&s_tab, tab);
    InterlockedExchange(&s_unread[tab], 0);
    InterlockedExchange(&s_blink_t[tab], 0);
    InterlockedExchange(&s_hist_t, s_tab_t[tab]);
    InterlockedIncrement(&s_in_gen);
    if (why) s_tab_auto++;
    else s_tab_key++;
    HLOG_I("LOCCHAT: Tab %s (%s)", tab_name(tab), why ? "automatisch" : "Taste");
}

static void chat_close(const char *why) /* UI-Thread */
{
    (void)why;
    EnterCriticalSection(&s_cs);
    lch_input_clear(&s_in);
    cz(s_comp, sizeof s_comp);
    LeaveCriticalSection(&s_cs);
    InterlockedExchange(&s_composing, 0);
    InterlockedExchange(&s_typing, 0);
    InterlockedIncrement(&s_in_gen);
    imouse_release_keys(); /* waehrend des Tippens gedrueckte Tasten nicht als gehalten weitergeben */
}

static void chat_open(void) /* UI-Thread */
{
    imouse_release_keys(); /* gehaltene Spieltasten loslassen: Spiel bekommt ab jetzt keine Eingabe */
    EnterCriticalSection(&s_cs);
    lch_input_clear(&s_in);
    LeaveCriticalSection(&s_cs);
    InterlockedExchange(&s_typing, 1);
    InterlockedIncrement(&s_in_gen);
    s_open_n++;
}

static void chat_command(int cmd, const char *arg) /* unter s_cs */
{
    int r;
    s_cmd_n++;
    switch (cmd) {
    case LCH_CMD_MUTE:
        r = lch_mute_add(&s_st.mute, arg);
        sys_line_key(r > 0 || r == 0 ? "chat.muted" : "chat.mute_full", arg);
        break;
    case LCH_CMD_UNMUTE:
        r = lch_mute_del(&s_st.mute, arg);
        sys_line_key(r ? "chat.unmuted" : "chat.not_muted", arg);
        break;
    case LCH_CMD_MUTES: {
        char all[LCH_MUTE_MAX * (LCH_NAME_MAX + 2) + 1];
        wchar_t fmt[200], t[400], wa[sizeof all];
        int i, o = 0;
        if (!s_st.mute.n) {
            sys_line_key("chat.mutes_none", NULL);
            break;
        }
        all[0] = 0;
        for (i = 0; i < s_st.mute.n; i++) {
            size_t l = strlen(s_st.mute.name[i]);
            if (o && o + 2 < (int)sizeof all) { all[o++] = ','; all[o++] = ' '; }
            if (o + (int)l < (int)sizeof all) { memcpy(all + o, s_st.mute.name[i], l); o += (int)l; }
            all[o] = 0;
        }
        for (i = 0; all[i] && i < (int)(sizeof wa / sizeof wa[0]) - 1; i++) wa[i] = (wchar_t)(unsigned char)all[i];
        wa[i] = 0;
        sb_lang_w("chat.mutes", fmt, 200);
        if (wcsstr(fmt, L"%s")) _snwprintf_s(t, 400, _TRUNCATE, fmt, wa);
        else wcsncpy_s(t, 400, fmt, _TRUNCATE);
        sys_line_w(t);
        cz(all, sizeof all);
        cz(wa, sizeof wa);
        cz(t, sizeof t);
        break;
    }
    default:
        sys_line_key("chat.help", NULL);
        break;
    }
}

static void chat_send(void) /* UI-Thread: Enter in der offenen Zeile */
{
    uint8_t pkt[LCH_PKT_MAX];
    char self[LCH_NAME_MAX + 1] = "", arg[LCH_NAME_MAX + 1], line[LCH_SN_LINE_MAX];
    locnet *n = loc_net_core();
    int len = 0, cmd, sn = s_tab == TAB_SN;
    if (n && n->tun) loctun_my_pilot(n->tun, self, (int)sizeof self); /* vor s_cs (Transport-Sperre) */
    EnterCriticalSection(&s_cs);
    cmd = lch_cmd(&s_in, arg, (int)sizeof arg);
    if (cmd != LCH_CMD_NONE) {
        chat_command(cmd, arg);
    } else if (sn) { /* W446: steelnet-Tab - die eigene Zeile kommt mit dem Echo des Dienstes */
        len = lch_sn_tx(&s_sn, now_ms(), &s_in, line, (int)sizeof line);
        if (len < 0) sys_line_key("chat.rate", NULL);
    } else {
        len = lch_tx(&s_st, now_ms(), &s_in, self, pkt, (int)sizeof pkt);
        if (len < 0) sys_line_key("chat.rate", NULL);
        else if (len > 0) InterlockedExchange(&s_hist_t, (LONG)now_ms());
    }
    LeaveCriticalSection(&s_cs);
    cz(arg, sizeof arg);
    if (len < 0) { /* Ratenbremse: Zeile bleibt offen und gefuellt */
        InterlockedIncrement(&s_in_gen);
        cz(pkt, sizeof pkt);
        cz(line, sizeof line);
        return;
    }
    if (len > 0 && sn) {
        if (!n || !n->tun || !loctun_snchat_send(n->tun, line, len)) s_tx_send_fail++;
        cz(line, sizeof line);
        cz(self, sizeof self);
        chat_close("gesendet");
        return;
    }
    if (len > 0) {
        if (!n || !n->tun ||
            loctun_dgram(n->tun, INADDR_BROADCAST, (uint16_t)LCH_PORT_GP, (uint16_t)LCH_PORT_GP, (const char *)pkt, len) < 0)
            s_tx_send_fail++;
    }
    cz(pkt, sizeof pkt);
    cz(self, sizeof self);
    chat_close("gesendet");
}

/* ------------------------------------------------------------------ Fenster (UI-Thread) */
void loc_chat_set_binding(const int *vks, const int *mods, int n)
{
    int i;
    for (i = 0; i < 4; i++) {
        InterlockedExchange(&s_vk[i], i < n && vks ? vks[i] : 0);
        InterlockedExchange(&s_mod[i], i < n && mods ? mods[i] : 0);
    }
    HLOG_I("LOCCHAT: Belegung chat: Tasten 0x%02X/%ld 0x%02X/%ld (%d)", (unsigned)s_vk[0], s_mod[0], (unsigned)s_vk[1],
           s_mod[1], n);
}

static int chat_is_key(WPARAM wp)
{
    int i, m = -1;
    for (i = 0; i < 4; i++) {
        if (!s_vk[i] || (WPARAM)s_vk[i] != wp) continue;
        if (m < 0) m = chat_mods_now();
        if (s_mod[i] == m) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ W452 IME (UI-Thread)
 * Der IME ist nur bei offener Chatzeile am Fenster (ImmAssociateContextEx), sonst ist kein Eingabekontext
 * zugeordnet: Spieltasten landen nie im IME. Fertige Zeichen kommen aus WM_IME_COMPOSITION/GCS_RESULTSTR; die
 * Nachricht geht nicht an DefWindowProc, daher entstehen keine WM_IME_CHAR/WM_CHAR-Doppel (trotzdem eintreffende
 * WM_IME_CHAR derselben Zeichen werden gezaehlt verworfen). Die Komposition zeigt die Chatzeile selbst (eigene Zeile
 * in Akzentfarbe), das Kandidatenfenster des IME steht an der Eingabezeile (CFS_EXCLUDE). */
static void ime_place(HWND w)
{
    HIMC c;
    RECT rc;
    LONG x, y, wd, ht;
    if (!w || !g_bb_w || !g_bb_h || !GetClientRect(w, &rc) || rc.right <= 0 || rc.bottom <= 0) return;
    x = (LONG)((long long)s_in_rect[0] * rc.right / (long long)g_bb_w);
    y = (LONG)((long long)s_in_rect[1] * rc.bottom / (long long)g_bb_h);
    wd = (LONG)((long long)s_in_rect[2] * rc.right / (long long)g_bb_w);
    ht = (LONG)((long long)s_in_rect[3] * rc.bottom / (long long)g_bb_h);
    if (ht <= 0) return;
    c = ImmGetContext(w);
    if (!c) return;
    {
        COMPOSITIONFORM cf;
        CANDIDATEFORM cd;
        memset(&cf, 0, sizeof cf);
        cf.dwStyle = CFS_POINT;
        cf.ptCurrentPos.x = x;
        cf.ptCurrentPos.y = y;
        ImmSetCompositionWindow(c, &cf);
        memset(&cd, 0, sizeof cd);
        cd.dwIndex = 0;
        cd.dwStyle = CFS_EXCLUDE;
        cd.ptCurrentPos.x = x;
        cd.ptCurrentPos.y = y;
        cd.rcArea.left = x;
        cd.rcArea.top = y;
        cd.rcArea.right = x + wd;
        cd.rcArea.bottom = y + ht;
        ImmSetCandidateWindow(c, &cd);
    }
    ImmReleaseContext(w, c);
}

static void ime_sync(HWND w) /* IME-Kontext nur bei offener Zeile */
{
    int want = s_typing && !s_wiped;
    if (!w || want == s_ime_on) return;
    s_ime_on = want; /* vor dem Aufruf: ImmAssociateContextEx schickt WM_IME_SETCONTEXT (Wiedereintritt) */
    ImmAssociateContextEx(w, NULL, want ? IACE_DEFAULT : 0);
    if (!want) {
        EnterCriticalSection(&s_cs);
        cz(s_comp, sizeof s_comp);
        LeaveCriticalSection(&s_cs);
        InterlockedExchange(&s_composing, 0);
        s_ime_pending = 0;
    }
    HLOG_I("LOCCHAT: IME %s", want ? "an (Chatzeile offen)" : "aus (Spieltasten gehen nie in den IME)");
}

void loc_chat_ime_tick(HWND w) /* sb_ovl_wndproc vor jeder Nachricht (auch bei offenem ESC-Menue) */
{
    if (!s_inst || !w) return;
    if (!s_hwnd) s_hwnd = w;
    if (s_ime_on < 0 || (s_typing != 0) != s_ime_on) ime_sync(w);
}

/* Zeichenkette aus dem IME bzw. der Testfolge (TEST nur mit loc_chat_test und Markierung im lParam). Rueckgabe
 * Anzahl UTF-16-Einheiten (ohne Null). */
static int ime_str(HWND w, DWORD what, LPARAM lp, wchar_t *out, int cap)
{
    LONG b = 0;
    HIMC c;
    out[0] = 0;
    if ((lp & 0x40000000) && g_cfg.loc_chat_test[0]) {
        const wchar_t *src = what == GCS_RESULTSTR ? s_ime_test_res : s_ime_test_comp;
        wcsncpy_s(out, (size_t)cap, src, _TRUNCATE);
        return (int)wcslen(out);
    }
    c = ImmGetContext(w);
    if (!c) return 0;
    b = ImmGetCompositionStringW(c, what, out, (DWORD)((cap - 1) * (int)sizeof(wchar_t)));
    ImmReleaseContext(w, c);
    if (b <= 0) return 0;
    b /= (LONG)sizeof(wchar_t);
    if (b > cap - 1) b = cap - 1;
    out[b] = 0;
    return (int)b;
}

static int ime_msg(HWND w, UINT msg, WPARAM wp, LPARAM lp) /* Zeile offen. 1 = verbraucht */
{
    wchar_t buf[64];
    int n, i;
    switch (msg) {
    case WM_IME_SETCONTEXT: /* eigene Kompositionsanzeige: Kompositionsfenster des IME aus, Kandidaten bleiben */
        DefWindowProcW(w, msg, wp, lp & ~(LPARAM)ISC_SHOWUICOMPOSITIONWINDOW);
        return 1;
    case WM_IME_STARTCOMPOSITION:
        InterlockedExchange(&s_composing, 1);
        ime_place(w);
        return 1;
    case WM_IME_COMPOSITION:
        ime_place(w);
        if (lp & GCS_RESULTSTR) {
            n = ime_str(w, GCS_RESULTSTR, lp, buf, 64);
            EnterCriticalSection(&s_cs);
            for (i = 0; i < n; i++) lch_input_wchar(&s_in, (unsigned)buf[i]); /* Bereinigung + 240-Grenze wie WM_CHAR */
            LeaveCriticalSection(&s_cs);
            s_ime_pending = n;
            s_ime_res_n++;
            cz(buf, sizeof buf);
        }
        n = (lp & GCS_COMPSTR) ? ime_str(w, GCS_COMPSTR, lp, buf, 64) : 0;
        EnterCriticalSection(&s_cs);
        cz(s_comp, sizeof s_comp);
        for (i = 0; i < n && i < 63; i++) s_comp[i] = (buf[i] >= 0x20 && buf[i] != 0x7F) ? buf[i] : L' ';
        LeaveCriticalSection(&s_cs);
        if (n) s_ime_comp_n++;
        cz(buf, sizeof buf);
        InterlockedIncrement(&s_in_gen);
        return 1; /* nie an DefWindowProc: keine WM_IME_CHAR-Doppel */
    case WM_IME_ENDCOMPOSITION:
        EnterCriticalSection(&s_cs);
        cz(s_comp, sizeof s_comp);
        LeaveCriticalSection(&s_cs);
        InterlockedExchange(&s_composing, 0);
        InterlockedIncrement(&s_in_gen);
        return 1;
    case WM_IME_CHAR: /* nur ohne vorherige GCS_RESULTSTR (IME ohne Komposition) uebernehmen */
        if (s_ime_pending > 0) {
            s_ime_pending--;
            s_ime_dup_n++;
            return 1;
        }
        if (wp >= 0x20 && wp != 0x7F) {
            int ok;
            EnterCriticalSection(&s_cs);
            ok = lch_input_wchar(&s_in, (unsigned)wp);
            LeaveCriticalSection(&s_cs);
            if (ok) InterlockedIncrement(&s_in_gen);
        }
        return 1;
    case WM_IME_NOTIFY:
        if (wp == IMN_OPENCANDIDATE || wp == IMN_CHANGECANDIDATE) ime_place(w);
        return 0; /* Kandidatenfenster zeichnet der IME */
    default:
        return 0;
    }
}

int loc_chat_wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!s_inst) return 0;
    if (w && !s_hwnd) s_hwnd = w;
    if (msg == CHAT_MSG_TEST_OPEN) { /* TEST: nur mit Testfolge, sonst ohne Wirkung */
        if (g_cfg.loc_chat_test[0] && !s_typing && s_avail && !s_wiped && !sb_ovl_open()) chat_open();
        ime_sync(w);
        return 1;
    }
    if (!s_typing) {
        if (msg == WM_KEYDOWN && s_avail && !s_wiped && !sb_ovl_open() && !(lp & 0x40000000) && chat_is_key(wp)) {
            chat_open();
            ime_sync(w);
            return 1;
        }
        return 0;
    }
    if ((msg >= WM_IME_STARTCOMPOSITION && msg <= WM_IME_KEYLAST) || msg == WM_IME_SETCONTEXT ||
        msg == WM_IME_NOTIFY || msg == WM_IME_CHAR)
        return ime_msg(w, msg, wp, lp);
    /* Zeile offen: Tastatur gehoert dem Chat (Tastenloslassen geht weiter, die Tasten sind ohnehin geloest) */
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_PROCESSKEY || s_composing) { /* W452: Taste gehoert dem IME (Umwandeln, Bestaetigen) */
            s_ime_key_n++;
            return 1;
        }
        if (wp == VK_ESCAPE) {
            if (!(lp & 0x40000000)) {
                chat_close("Escape");
                ime_sync(w);
            }
        } else if (wp == VK_RETURN) {
            if (!(lp & 0x40000000)) {
                chat_send();
                ime_sync(w);
            }
        } else if (wp == VK_TAB) { /* W446/W452/W474: naechster verfuegbarer Tab (steelnet | Lobby | Team) */
            if (!(lp & 0x40000000)) {
                int o = lch_tab_next((int)s_tab, (unsigned)s_av_mask);
                if (o != (int)s_tab) tab_set(o, 0);
            }
        } else if (wp == VK_BACK) {
            EnterCriticalSection(&s_cs);
            lch_input_back(&s_in);
            LeaveCriticalSection(&s_cs);
            InterlockedIncrement(&s_in_gen);
        }
        return 1;
    case WM_CHAR:
        if (wp >= 0x20 && wp != 0x7F) {
            int ok;
            EnterCriticalSection(&s_cs);
            ok = lch_input_wchar(&s_in, (unsigned)wp);
            LeaveCriticalSection(&s_cs);
            if (ok) InterlockedIncrement(&s_in_gen);
        }
        return 1;
    case WM_KEYUP:
        return wp == VK_ESCAPE || wp == VK_RETURN || wp == VK_TAB; /* ESC-/Enter-/Tab-Loslassen nicht ans Menue bzw. Spiel */
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
        return 1; /* Maustasten nicht ans Spiel, solange getippt wird */
    default:
        return 0;
    }
}

int loc_chat_typing(void) { return s_typing != 0; }
int loc_chat_avail(void) { return s_avail != 0; }

/* ------------------------------------------------------------------ Testfolge (TEST, loc_chat_test) */
/* "ms:AKTION;..." ab dem ersten Moment mit Chat + verbundenem Mitspieler. AKTION: O (Chat-Taste), E (Enter),
 * X (Escape), B (Backspace), T<Text> (Zeichen als WM_CHAR, \uXXXX = UTF-16-Einheit). Nur Nachrichten an das EIGENE
 * Fenster (PostMessage, fokusneutral); nie echte Tasten. */
/* W446: zweite Folge loc_snchat_test ab dem ersten Moment mit steelnet-Chat (ohne Mitspieler); AKTION zusaetzlich
 * K (Tab-Taste). Bilder: w437_chat_NN bzw. w446_sn_NN (der Bildabgriff in d3d11_render braucht loc_chat_test). */
typedef struct { u32 at; char act; char text[200]; } chat_step;
#define CHAT_STEPS 24
typedef struct { chat_step st[CHAT_STEPS]; int n, i; } chat_seq;
static chat_seq s_seq[2];
static volatile LONG s_shot_req, s_shot_n; /* P: Bild aufnehmen (Render-Thread holt es ab); 1 = W437, 2 = W446 */

int loc_chat_shot_due(char *name, unsigned cap)
{
    LONG r = s_shot_req;
    if (!r || !name || cap < 16u) return 0;
    InterlockedExchange(&s_shot_req, 0);
    _snprintf_s(name, cap, _TRUNCATE, r == 2 ? "w446_sn_%02ld" : "w437_chat_%02ld", InterlockedIncrement(&s_shot_n));
    return 1;
}

static void steps_parse(chat_seq *q, const char *p)
{
    q->n = q->i = 0;
    while (p && *p && q->n < CHAT_STEPS) {
        chat_step *s = &q->st[q->n];
        const char *e = strchr(p, ';');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        const char *c = (const char *)memchr(p, ':', l);
        if (c && c + 1 < p + l) {
            s->at = (u32)strtoul(p, NULL, 10);
            s->act = c[1];
            l = (size_t)(p + l - (c + 2));
            if (l >= sizeof s->text) l = sizeof s->text - 1;
            memcpy(s->text, c + 2, l);
            s->text[l] = 0;
            q->n++;
        }
        if (!e) break;
        p = e + 1;
    }
}

static void steps_run(chat_seq *q, int which, u32 since)
{
    HWND w = s_hwnd;
    while (w && q->i < q->n && since >= q->st[q->i].at) {
        chat_step *s = &q->st[q->i++];
        HLOG_I("LOCCHAT TEST: %s Schritt %d (%c) bei %u ms", which ? "steelnet" : "Lobby", q->i, s->act,
               since); /* nur Art, nie Text */
        if (s->act == 'K') {
            PostMessageW(w, WM_KEYDOWN, VK_TAB, 1);
        } else if (s->act == 'O') {
            if (s_vk[0] && !s_mod[0]) PostMessageW(w, WM_KEYDOWN, (WPARAM)s_vk[0], 1); /* belegte Taste (Profil) */
            else PostMessageW(w, CHAT_MSG_TEST_OPEN, 0, 0);                          /* Legacy: ohne Chat-Taste */
        } else if (s->act == 'E') {
            PostMessageW(w, WM_KEYDOWN, VK_RETURN, 1);
        } else if (s->act == 'X') {
            PostMessageW(w, WM_KEYDOWN, VK_ESCAPE, 1);
        } else if (s->act == 'B') {
            PostMessageW(w, WM_KEYDOWN, VK_BACK, 1);
        } else if (s->act == 'P') {
            InterlockedExchange(&s_shot_req, which ? 2 : 1);
        } else if (s->act == 'J' || s->act == 'R') {
            /* W452 TEST IME: J<Text> = Komposition laeuft (WM_IME_STARTCOMPOSITION + WM_IME_COMPOSITION/GCS_COMPSTR),
             * R<Text> = fertige Zeichen (GCS_RESULTSTR + WM_IME_ENDCOMPOSITION) und danach dieselben Zeichen als
             * WM_IME_CHAR, wie sie DefWindowProc erzeugen wuerde (muessen verworfen werden: kein doppeltes Einfuegen).
             * Markierung 0x40000000 im lParam: Text aus der Testfolge statt ImmGetCompositionString. */
            wchar_t wt[64];
            int k = 0, nw = 0;
            const char *tp = s->text;
            while (*tp && nw < 63) { /* \uXXXX = UTF-16-Einheit (Kommandozeile ohne UTF-8), sonst ASCII */
                if (tp[0] == '\\' && tp[1] == 'u' && tp[2] && tp[3] && tp[4] && tp[5]) {
                    char hx[5] = { tp[2], tp[3], tp[4], tp[5], 0 };
                    wt[nw++] = (wchar_t)strtoul(hx, NULL, 16);
                    tp += 6;
                } else {
                    wt[nw++] = (wchar_t)(unsigned char)*tp++;
                }
            }
            wt[nw] = 0;
            if (s->act == 'J') {
                wcsncpy_s(s_ime_test_comp, 64, wt, _TRUNCATE);
                PostMessageW(w, WM_IME_STARTCOMPOSITION, 0, 0);
                PostMessageW(w, WM_IME_COMPOSITION, 0, (LPARAM)(GCS_COMPSTR | 0x40000000));
            } else {
                wcsncpy_s(s_ime_test_res, 64, wt, _TRUNCATE);
                PostMessageW(w, WM_IME_COMPOSITION, 0, (LPARAM)(GCS_RESULTSTR | 0x40000000));
                PostMessageW(w, WM_IME_ENDCOMPOSITION, 0, 0);
                for (k = 0; wt[k]; k++) PostMessageW(w, WM_IME_CHAR, (WPARAM)wt[k], 1);
            }
            cz(wt, sizeof wt);
        } else if (s->act == 'T') { /* UTF-8 -> UTF-16, \uXXXX roh */
            const unsigned char *q = (const unsigned char *)s->text;
            while (*q) {
                unsigned u;
                if (q[0] == '\\' && q[1] == 'u' && q[2] && q[3] && q[4] && q[5]) {
                    char hx[5] = { (char)q[2], (char)q[3], (char)q[4], (char)q[5], 0 };
                    u = (unsigned)strtoul(hx, NULL, 16);
                    q += 6;
                    PostMessageW(w, WM_CHAR, (WPARAM)u, 1);
                    continue;
                }
                if (q[0] < 0x80) {
                    u = q[0];
                    q++;
                } else if ((q[0] & 0xE0) == 0xC0 && q[1]) {
                    u = ((q[0] & 0x1Fu) << 6) | (q[1] & 0x3Fu);
                    q += 2;
                } else if ((q[0] & 0xF0) == 0xE0 && q[1] && q[2]) {
                    u = ((q[0] & 0x0Fu) << 12) | ((q[1] & 0x3Fu) << 6) | (q[2] & 0x3Fu);
                    q += 3;
                } else {
                    q++;
                    continue;
                }
                PostMessageW(w, WM_CHAR, (WPARAM)u, 1);
            }
        }
    }
}

/* ------------------------------------------------------------------ Thread */
static DWORD WINAPI chat_thread(LPVOID unused)
{
    u32 loop = 0, ready_at[2] = { 0, 0 }, last_tel = 0;
    int ready[2] = { 0, 0 };
    int last_avail = -1, last_sn = -1, last_ses = LCH_SES_NONE, hist_kept = 0;
    unsigned last_mask = 0;
    u32 tel_sig = 0;
    (void)unused;
    steps_parse(&s_seq[0], g_cfg.loc_chat_test);
    steps_parse(&s_seq[1], g_cfg.loc_snchat_test); /* W446 */
    for (;; loop++) {
        locnet *n;
        int av = 0, up = 0, av_lobby = 0, av_sn = 0, ses, link = 0, snr = 0, nt, out = 1;
        unsigned mask;
        lch_tabin ti;
        u32 now;
        Sleep(100);
        if (s_wiped) continue;
        if (!s_hwnd) { /* W452: unsichtbares Fenster ohne Nachrichten nach dem Start -> Handle vom Renderer holen; die
                        * Nachricht stoesst den UI-Thread an (loc_chat_ime_tick: IME bis zum Oeffnen aus) */
            HWND hw = sb_render_hwnd();
            if (hw) {
                s_hwnd = hw;
                PostMessageW(hw, WM_NULL, 0, 0);
            }
        }
        now = now_ms();
        n = loc_net_core();
        if (n && n->tun) {
            int sec = loctun_sec_state(n->tun), lb = loctun_lobby_state(n->tun);
            if (!s_tun_set) {
                loctun_set_chat(n->tun, (uint16_t)LCH_PORT_GP, chat_rx);
                loctun_set_snchat(n->tun, sn_rx, g_cfg.loc_snchat); /* W446: ohne Schalter kein Abonnement */
                s_tun_set = 1;
                HLOG_I("LOCCHAT: Chatkanal ueber den eigenen Transport (SBT2-Datagramm, Spielport %u, nicht an das Spiel, "
                       "Schalter loc_chat=%d); steelnet-Chat ueber den Steuerkanal (Schalter loc_snchat=%d)", LCH_PORT_GP,
                       g_cfg.loc_chat, g_cfg.loc_snchat);
            }
            up = loctun_links_up(n->tun);
            /* W446: Lobby-Chat technisch nur mit verbundenem Mitspieler, steelnet sobald abonniert. W474: der Reiter
             * "Lobby" zusaetzlich nur in einer Lobby bzw. im Match (Spielzustand, nicht der Raum des Transports). */
            link = g_cfg.loc_chat && sec == 0 && up > 0 && (lb >= 2 || g_cfg.loc_chat_direct);
            snr = g_cfg.loc_snchat && sec == 0 && lb >= 2 && loctun_snchat_state(n->tun) == 2;
            if (snr && (loop % 10u) == 0u) { /* eigener Name beim Dienst (Echo als eigene Zeile markieren) */
                char me[LCH_NAME_MAX + 1] = "";
                loctun_my_pilot(n->tun, me, (int)sizeof me);
                EnterCriticalSection(&s_cs);
                memcpy(s_self, me, sizeof s_self);
                LeaveCriticalSection(&s_cs);
            }
        }
        ses = loc_net_lobby_state_ex(&out); /* W474: zentral (auch Lobby-Infopanel) */
        memset(&ti, 0, sizeof ti);
        ti.ses = ses;
        ti.lobby_link = link;
        ti.sn_ready = snr;
        ti.team_known = 0; /* Teil B: Zuordnung Platz -> Seite nicht sicher belegt */
        ti.mis_sn = g_cfg.loc_chat_mission_sn;
        ti.mis_lobby = g_cfg.loc_chat_mission_lobby;
        ti.mis_team = g_cfg.loc_chat_mission_team;
        mask = lch_tabs_avail(&ti);
        InterlockedExchange(&s_ses, ses);
        InterlockedExchange(&s_gate_sn, ses == LCH_SES_MATCH && !g_cfg.loc_chat_mission_sn);
        InterlockedExchange(&s_av_mask, (LONG)mask);
        av_lobby = (int)((mask >> TAB_LOBBY) & 1u);
        av_sn = (int)((mask >> TAB_SN) & 1u);
        av = mask != 0u;
        if (ses != LCH_SES_NONE) hist_kept = 1;
        if (ses == LCH_SES_NONE && hist_kept && out) { /* selbst verlassen: Verlauf leeren, naechster Beitritt beginnt leer */
            hist_kept = 0;
            EnterCriticalSection(&s_cs);
            lch_hist_clear(&s_st.hist);
            lch_hist_clear(&s_team_hist);
            LeaveCriticalSection(&s_cs);
            InterlockedExchange(&s_unread[TAB_LOBBY], 0);
            InterlockedExchange(&s_unread[TAB_TEAM], 0);
            InterlockedExchange(&s_blink_t[TAB_LOBBY], 0);
            InterlockedExchange(&s_blink_t[TAB_TEAM], 0);
            InterlockedExchange(&s_tab_t[TAB_LOBBY], 0);
            InterlockedExchange(&s_tab_t[TAB_TEAM], 0);
            if (s_tab == TAB_LOBBY || s_tab == TAB_TEAM) InterlockedExchange(&s_hist_t, 0);
            s_ses_clear++;
            HLOG_I("LOCCHAT: Lobby verlassen - Lobby-Verlauf geleert (Reiter weg)");
        }
        if (ses != last_ses) HLOG_I("LOCCHAT: Lobby-Zustand %d -> %d, Reiter 0x%X", last_ses, ses, mask);
        last_ses = ses;
        nt = lch_tab_auto((int)s_tab, mask, last_mask); /* Lobby betreten: Sprung auf Lobby; Reiter weg: erster verfuegbarer */
        if (nt != (int)s_tab) {
            if (s_typing) { /* angefangene Zeile nie still in einen anderen Reiter (z. B. Lobby -> alle im steelnet) */
                EnterCriticalSection(&s_cs);
                lch_input_clear(&s_in);
                cz(s_comp, sizeof s_comp);
                LeaveCriticalSection(&s_cs);
                InterlockedExchange(&s_composing, 0);
                InterlockedExchange(&s_typing, 0);
                if (s_hwnd) PostMessageW(s_hwnd, WM_NULL, 0, 0);
            }
            tab_set(nt, 1);
        }
        if (mask != last_mask) InterlockedIncrement(&s_in_gen);
        last_mask = mask;
        if (av_sn != last_sn) {
            HLOG_I("LOCCHAT: steelnet-Tab %s", av_sn ? "verfuegbar" : (g_cfg.loc_snchat ? "nicht verfuegbar" : "aus (loc_snchat=0)"));
            last_sn = av_sn;
        }
        if (av != last_avail) {
            HLOG_I("LOCCHAT: %s", av ? "verfuegbar (LoC online, Ende-zu-Ende ok)"
                                     : (g_cfg.loc_chat || g_cfg.loc_snchat ? "nicht verfuegbar (kein Treffpunkt bzw. nicht online)"
                                                                           : "aus (loc_chat=0, loc_snchat=0)"));
            last_avail = av;
        }
        InterlockedExchange(&s_avail, av);
        if (s_typing && (!av || sb_ovl_open())) { /* nicht mehr online bzw. Menue offen: Zeile schliessen (ohne ESC) */
            EnterCriticalSection(&s_cs);
            lch_input_clear(&s_in);
            LeaveCriticalSection(&s_cs);
            InterlockedExchange(&s_typing, 0);
            InterlockedIncrement(&s_in_gen);
            if (s_hwnd) PostMessageW(s_hwnd, WM_NULL, 0, 0);
        }
        if (av_lobby && up > 0 && !ready[0]) { /* Testfolge zaehlt ab dem ersten verbundenen Mitspieler */
            ready[0] = 1;
            ready_at[0] = now;
        }
        if (av_sn && !ready[1]) { /* W446: steelnet-Folge ab dem Abonnement */
            ready[1] = 1;
            ready_at[1] = now;
        }
        if (ready[0] && s_seq[0].n) steps_run(&s_seq[0], 0, now - ready_at[0]);
        if (ready[1] && s_seq[1].n) steps_run(&s_seq[1], 1, now - ready_at[1]);
        if (loop % 600u == 0u || (u32)(now - last_tel) > 60000u) {
            u32 sig;
            EnterCriticalSection(&s_cs);
            sig = s_st.rx_ok * 7u + s_st.rx_bad * 13u + s_st.rx_rate_drop * 17u + s_st.rx_muted * 19u + s_st.tx_ok * 23u +
                  s_st.tx_rate_drop * 29u + s_st.rx_dup * 31u + s_st.rx_empty * 37u + s_open_n * 41u + s_cmd_n * 43u +
                  s_sn.rx_ok * 47u + s_sn.tx_ok * 53u + s_sn.rx_muted * 59u + s_sn.status_n * 61u + s_tab_auto * 67u +
                  s_tab_key * 71u + s_sn.tx_rate_drop * 73u + s_ime_res_n * 79u + s_ime_dup_n * 83u + s_ime_comp_n * 89u +
                  s_ime_key_n * 97u + s_rx_off_lobby * 101u + s_rx_off_sn * 103u + s_ses_clear * 107u;
            if (sig != tel_sig) {
                HLOG_I("LOCCHAT-TEL: gesendet=%u empfangen=%u verworfen: ungueltig=%u rate=%u stumm=%u leer=%u doppelt=%u | "
                       "eigene: rate=%u leer=%u sendefehler=%u | geoeffnet=%u befehle=%u stumm_liste=%d",
                       s_st.tx_ok, s_st.rx_ok, s_st.rx_bad, s_st.rx_rate_drop, s_st.rx_muted, s_st.rx_empty, s_st.rx_dup,
                       s_st.tx_rate_drop, s_st.tx_empty, s_tx_send_fail, s_open_n, s_cmd_n, s_st.mute.n);
                HLOG_I("LOCCHAT-TEL steelnet: gesendet=%u empfangen=%u (verlauf=%u) verworfen: ungueltig=%u stumm=%u leer=%u "
                       "| eigene: rate=%u leer=%u | dienst-status=%u | tab: aktiv=%s auto=%u taste=%u",
                       s_sn.tx_ok, s_sn.rx_ok, s_sn.rx_hist, s_sn.rx_bad, s_sn.rx_muted, s_sn.rx_empty, s_sn.tx_rate_drop,
                       s_sn.tx_empty, s_sn.status_n, tab_name((int)s_tab), s_tab_auto, s_tab_key);
                HLOG_I("LOCCHAT-TEL ime: ergebnisse=%u doppelt_verworfen=%u kompositionen=%u ime_tasten=%u", s_ime_res_n,
                       s_ime_dup_n, s_ime_comp_n, s_ime_key_n);
                HLOG_I("LOCCHAT-TEL reiter: verworfen lobby=%u steelnet=%u | lobby_verlassen=%u | zustand=%ld maske=0x%lX",
                       s_rx_off_lobby, s_rx_off_sn, s_ses_clear, s_ses, s_av_mask);
                tel_sig = sig;
            }
            LeaveCriticalSection(&s_cs);
            last_tel = now;
        }
    }
}

void loc_chat_install(void)
{
    LARGE_INTEGER q;
    if (InterlockedExchange(&s_inst, 1)) return;
    InitializeCriticalSection(&s_cs);
    QueryPerformanceCounter(&q);
    lch_init(&s_st, (u32)(q.QuadPart ^ (q.QuadPart >> 29) ^ ((u32)GetCurrentProcessId() << 11)) | 1u);
    HLOG_I("LOCCHAT: Textchat LoC online (Schalter loc_chat=%d, Taste Enter bzw. Profil-Aktion chat, max %d Zeichen, "
           "%d je %u s je Absender, Inhalte nie im Log)", g_cfg.loc_chat, LCH_MAX_CHARS, LCH_RATE_N, LCH_RATE_MS / 1000u);
    if (!CreateThread(NULL, 0, chat_thread, NULL, 0, NULL)) HLOG_W("LOCCHAT: Thread fehlt - Chat aus");
}

/* ------------------------------------------------------------------ Anzeige (Render-Thread)
 * W452 CHATUX (GitHub #89): Chatfenster in der linken unteren Ecke des Bildschirms (Backbuffer, nicht der Spielbox;
 * Abstand OVS_MARGIN, Belegungsplan ovs_layout_place: weicht allem aus, was dort schon angemeldet ist). Oben eine
 * Reiterleiste im Overlay-Look (aktiver Reiter gefuellt, hell, orange unterstrichen; inaktive gedaempft; nicht
 * verfuegbare dunkel), darunter die dunkle, halbtransparente Flaeche mit dem Verlauf NUR des aktiven Tabs und der
 * Eingabezeile (bei laufender IME-Komposition eine Zeile darunter in Akzentfarbe). Neues im nicht aktiven Tab: der
 * Reiter pulst drei Mal sanft, danach bleibt der Zaehler in Akzentfarbe bis zum Wechsel. W474: nur verfuegbare Reiter
 * erscheinen (steelnet | Lobby | Team); das Fenster steht immer unten links, auch im Match (kein Ausweichen nach oben). */
static ovs_canvas_t s_cv;            /* W461: GDI-Flaeche, Schrift und Textur aus overlay_style */
#define s_dc (s_cv.dc)
#define s_bits (s_cv.bits)
static ovs_font_t s_fn, s_fn_tab;
static ovs_tex_t s_tx;
static unsigned *s_pm;
static unsigned s_pm_n;              /* Pixel in s_pm */
static unsigned s_w, s_h;            /* Inhalt */
static sb_ovl_save_t s_save;
static u32 s_sig[8];
static int s_tex_dirty;
static int s_in_loc[4];              /* Eingabezeile im Inhalt (x, y, w, h); h = 0: keine */

enum { ROW_HIST = 0, ROW_INPUT = 1, ROW_COMP = 2 };
typedef struct { wchar_t name[LCH_NAME_MAX + 1]; wchar_t text[LCH_LINE_W]; int kind, role; float a; } chat_row;
#define CHAT_ROWS (LCH_HIST + 2)
static chat_row s_rows[CHAT_ROWS];
typedef struct { wchar_t label[40]; int act, av, unread; float pulse; } chat_tabv;
static chat_tabv s_tv[TAB_N];
static float s_strip_a;              /* Deckung der Reiterleiste (Ausblenden bei geschlossener Zeile) */
static int s_typ_r;
typedef struct { int x0, y0, x1, y1; unsigned rgb; float a; } chat_bg;
#define CHAT_BG_MAX 16
static chat_bg s_bg[CHAT_BG_MAX];
static int s_nbg;

#define CHAT_BLINK_SHOW_MS 3000u     /* geschlossene Zeile: Reiterleiste nach Neuem im anderen Tab so lange sichtbar */
#define CHAT_W_UNITS 280.0f          /* Breite (Einheiten, 1080p: 560 px), hoechstens 45 % der Bildschirmbreite */
/* Farben aus dem Overlay-Stil (overlay_style.h) */
#define CHAT_ACC OVS_RGB_ACCENT
#define CHAT_TAB_BG OVS_RGB_TAB
#define CHAT_TAB_ACT OVS_RGB_TAB_ACT
#define CHAT_TAB_HOT OVS_RGB_TAB_HOT

/* Reiter tab: 0 = nichts Neues, 1 = pulst (lvl 0..1 in 4 Stufen), 2 = Pulsen vorbei, Markierung bleibt */
static int tab_state(int tab, u32 now, float *lvl)
{
    u32 t0 = (u32)s_blink_t[tab], age;
    float x, tri;
    *lvl = 0.0f;
    if (tab == (int)s_tab || !tab_avail(tab) || s_unread[tab] <= 0 || !t0) return 0;
    age = now - t0;
    if (age >= CHAT_PULSES * CHAT_PULSE_MS) return 2;
    x = (float)(age % CHAT_PULSE_MS) / (float)CHAT_PULSE_MS;
    tri = 1.0f - (x < 0.5f ? 1.0f - 2.0f * x : 2.0f * x - 1.0f); /* 0 -> 1 -> 0 */
    *lvl = (float)((int)(tri * 4.0f + 0.5f)) / 4.0f;
    return 1;
}

static int blink_recent(u32 now)
{
    int t;
    for (t = 0; t < TAB_N; t++) {
        if (t != (int)s_tab && tab_avail(t) && s_unread[t] > 0 && s_blink_t[t] &&
            (u32)(now - (u32)s_blink_t[t]) < CHAT_BLINK_SHOW_MS)
            return 1;
    }
    return 0;
}

int loc_chat_visible(void)
{
    u32 now;
    if (!s_inst || s_wiped || !(g_cfg.loc_chat || g_cfg.loc_snchat) || g_cfg.vr_enable || sb_ovl_open()) return 0;
    if (!s_avail) return 0; /* W474: kein verfuegbarer Reiter (Hauptmenue ohne steelnet, Match mit allem aus) */
    if (s_typing) return 1;
    now = now_ms();
    if (s_hist_t && (u32)(now - (u32)s_hist_t) < LCH_SHOW_MS) return 1;
    return blink_recent(now);
}

#define SC ovs_px

static COLORREF name_color(const wchar_t *n, int kind)
{
    static const COLORREF pal[8] = { RGB(255, 196, 92), RGB(120, 210, 255), RGB(255, 140, 140), RGB(170, 255, 140),
                                     RGB(230, 160, 255), RGB(255, 230, 110), RGB(120, 255, 220), RGB(255, 170, 210) };
    u32 h = 2166136261u;
    if (kind == LCH_L_SELF) return RGB(255, 255, 255);
    if (kind == LCH_L_SYS) return OVS_CREF(OVS_RGB_SYS);
    for (; *n; n++) h = (h ^ (u32)(*n | 0x20)) * 16777619u;
    return pal[h % 8u];
}

static COLORREF rgbc(unsigned v) { return RGB((v >> 16) & 255u, (v >> 8) & 255u, v & 255u); }

static unsigned rgb_mix(unsigned a, unsigned b, float t)
{
    unsigned o = 0, k;
    for (k = 0; k < 24; k += 8) {
        float ca = (float)((a >> k) & 255u), cb = (float)((b >> k) & 255u);
        o |= ((unsigned)(ca + (cb - ca) * t + 0.5f) & 255u) << k;
    }
    return o;
}

static void bg_add(int x0, int y0, int x1, int y1, unsigned rgb, float a)
{
    chat_bg *b;
    if (s_nbg >= CHAT_BG_MAX || x1 <= x0 || y1 <= y0) return;
    b = &s_bg[s_nbg++];
    b->x0 = x0; b->y0 = y0; b->x1 = x1; b->y1 = y1;
    b->rgb = rgb;
    b->a = a;
}

/* Zeilen und Reiter holen (unter s_cs kurz), Sichtbarkeit/Ausblenden je Zeile. Rueckgabe Anzahl Zeilen (0 = nur
 * Reiterleiste), -1 = nichts zu zeigen. */
static int chat_collect(u32 *sig)
{
    int k, n = 0, t, typing = s_typing != 0, tab = (int)s_tab;
    u32 now = now_ms(), h = 0;
    float amax = 0.0f, blink_a = 0.0f;
    const lch_hist *hs;
    EnterCriticalSection(&s_cs);
    hs = tab_hist(tab);
    for (k = LCH_HIST - 1; k >= 0; k--) { /* aelteste zuerst, nur der aktive Tab */
        const lch_line *l = lch_hist_get(hs, k);
        u32 age;
        float a = 1.0f;
        if (!l) continue;
        age = now - l->t;
        if (!typing) {
            if (age >= LCH_SHOW_MS) continue;
            if (age > LCH_SHOW_MS - LCH_FADE_MS) a = (float)(LCH_SHOW_MS - age) / (float)LCH_FADE_MS;
            a = (float)((int)(a * 8.0f + 0.999f)) / 8.0f; /* 8 Stufen: seltener neu hochladen */
        }
        memcpy(s_rows[n].name, l->name, sizeof s_rows[n].name);
        memcpy(s_rows[n].text, l->text, sizeof s_rows[n].text);
        s_rows[n].kind = l->kind;
        s_rows[n].role = ROW_HIST;
        s_rows[n].a = a;
        if (a > amax) amax = a;
        h = h * 31u + l->t + (u32)(a * 8.0f) * 7u;
        n++;
    }
    if (typing) {
        chat_row *r = &s_rows[n++];
        sb_lang_w("chat.prompt", r->name, LCH_NAME_MAX + 1);
        lch_input_w(&s_in, r->text, LCH_LINE_W - 2);
        if (!s_comp[0]) wcsncat_s(r->text, LCH_LINE_W, L"_", _TRUNCATE);
        r->kind = LCH_L_SYS;
        r->role = ROW_INPUT;
        r->a = 1.0f;
        if (s_comp[0]) { /* W452: laufende IME-Komposition (noch nicht uebernommen) */
            r = &s_rows[n++];
            sb_lang_w("chat.ime", r->name, LCH_NAME_MAX + 1);
            wcsncpy_s(r->text, LCH_LINE_W, s_comp, _TRUNCATE);
            wcsncat_s(r->text, LCH_LINE_W, L"_", _TRUNCATE);
            r->kind = LCH_L_SYS;
            r->role = ROW_COMP;
            r->a = 1.0f;
        }
    }
    h = h * 31u + hs->gen + (u32)tab * 977u;
    LeaveCriticalSection(&s_cs);
    for (t = 0; t < TAB_N; t++) {
        chat_tabv *v = &s_tv[t];
        wchar_t lb[24];
        float lvl = 0.0f;
        int st = tab_state(t, now, &lvl);
        sb_lang_w(t == TAB_SN ? "chat.tab_steelnet" : t == TAB_TEAM ? "chat.tab_team" : "chat.tab_lobby", lb, 24);
        v->act = t == tab;
        v->av = tab_avail(t);
        v->unread = t == tab ? 0 : (int)s_unread[t];
        v->pulse = st == 1 ? (lvl > 0.0f ? lvl : 0.01f) : 0.0f;
        if (v->unread > 0) _snwprintf_s(v->label, 40, _TRUNCATE, L"%s (%d)", lb, v->unread);
        else wcsncpy_s(v->label, 40, lb, _TRUNCATE);
        if (st && (u32)(now - (u32)s_blink_t[t]) < CHAT_BLINK_SHOW_MS) blink_a = 1.0f;
        h = h * 31u + (u32)v->act * 3u + (u32)v->av * 5u + (u32)v->unread * 7u + (u32)(lvl * 4.0f) * 11u + (u32)st * 13u;
    }
    s_strip_a = typing ? 1.0f : (amax > blink_a ? amax : blink_a);
    h = h * 31u + (u32)(s_strip_a * 8.0f);
    s_typ_r = typing;
    sig[0] = h;
    sig[1] = (u32)s_in_gen;
    sig[2] = (u32)typing;
    if (!typing && n == 0 && blink_a <= 0.0f) return -1;
    return n;
}

/* Inhalt in s_pm rendern (vormultipliziertes BGRA, s_w x s_h). 1 = ok. */
static int chat_render(int nrows, float f, unsigned W, unsigned maxh)
{
    HFONT fn, ft, fold;
    int px = SC(f * (OVS_REF_UI / OVS_REF_LOC), OVS_LP_FS_ROW), tpx = SC(f * (OVS_REF_UI / OVS_REF_LOC), OVS_LP_FS_TAG), pad = SC(f, 5.0f), gap = SC(f, 2.0f), tpadx = SC(f, 7.0f);
    int tpy = SC(f, 2.5f), ul = SC(f, 1.5f), tgap = SC(f, 2.0f), ln = SC(f, 0.5f);
    int nmw[CHAT_ROWS], th[CHAT_ROWS], top[CHAT_ROWS + 1], tw[TAB_N], i, t, y, x, first, tab_h, body_h, tabs_w = 0, b;
    unsigned w, h, xx, yy;
    RECT r;
    SIZE sz;
    if (!ovs_canvas_dc(&s_cv)) return 0;
    fn = ovs_font_get(&s_fn, OVS_FACE_SEGOE, FW_SEMIBOLD, px);
    ft = ovs_font_get(&s_fn_tab, OVS_FACE_SEGOE, FW_BOLD, tpx);
    if (!fn || !ft) return 0;
    fold = (HFONT)SelectObject(s_dc, ft);
    /* Reiter: Breite je Beschriftung, gemeinsame Hoehe */
    if (!GetTextExtentPoint32W(s_dc, L"Ag", 2, &sz) || sz.cy <= 0) sz.cy = tpx;
    tab_h = sz.cy + 2 * tpy + ul;
    for (t = 0, b = 0; t < TAB_N; t++) {
        SIZE ts;
        tw[t] = 0;
        if (!s_tv[t].av && !s_tv[t].act) continue; /* W474: nicht verfuegbare Reiter erscheinen nicht */
        if (!GetTextExtentPoint32W(s_dc, s_tv[t].label, (int)wcslen(s_tv[t].label), &ts)) ts.cx = tpx * 5;
        tw[t] = ts.cx + 2 * tpadx;
        tabs_w += tw[t] + (b++ ? tgap : 0);
    }
    /* Zeilen: Name + ": " haengend, Text umgebrochen (auch lange Woerter) */
    SelectObject(s_dc, fn);
    for (i = 0; i < nrows; i++) {
        wchar_t nb[LCH_NAME_MAX + 4];
        int avail;
        _snwprintf_s(nb, LCH_NAME_MAX + 4, _TRUNCATE, L"%s: ", s_rows[i].name);
        if (!GetTextExtentPoint32W(s_dc, nb, (int)wcslen(nb), &sz)) sz.cx = px * 6;
        nmw[i] = sz.cx;
        avail = (int)W - 2 * pad - nmw[i];
        if (avail < px * 4) avail = px * 4;
        r.left = 0; r.top = 0; r.right = avail; r.bottom = 10000;
        DrawTextW(s_dc, s_rows[i].text, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
        th[i] = r.bottom > px ? r.bottom : px + px / 3;
    }
    /* zu hoch: aelteste Verlaufszeilen weglassen (Eingabe- und Kompositionszeile bleiben) */
    first = 0;
    for (;;) {
        int tot = tab_h + (first < nrows ? 2 * pad : 0);
        for (i = first; i < nrows; i++) tot += th[i] + (i > first ? gap : 0);
        if ((unsigned)tot <= maxh || first >= nrows || s_rows[first].role != ROW_HIST) break;
        first++;
    }
    body_h = 0;
    if (first < nrows) {
        body_h = 2 * pad;
        for (i = first; i < nrows; i++) body_h += th[i] + (i > first ? gap : 0);
    }
    w = body_h ? W : (unsigned)tabs_w;
    if (w < (unsigned)tabs_w) w = (unsigned)tabs_w;
    h = (unsigned)(tab_h + body_h);
    if (w < 8u || h < 8u) {
        SelectObject(s_dc, fold);
        return 0;
    }
    SelectObject(s_dc, fold);
    if (s_bits && (s_cv.w != w || s_cv.h != h)) cz(s_bits, (size_t)s_cv.w * s_cv.h * 4u); /* vor dem Freigeben loeschen */
    if (!ovs_canvas_size(&s_cv, w, h)) return 0;
    fold = (HFONT)SelectObject(s_dc, ft);
    memset(s_bits, 0, (size_t)w * h * 4u);
    s_nbg = 0;
    /* Flaechen: Fenster, Trennlinie unter der Reiterleiste, dann Reiter (spaetere gewinnen) */
    if (body_h) bg_add(0, tab_h, (int)w, (int)h, OVS_RGB_PANEL, s_typ_r ? OVS_LP_ALPHA : OVS_LP_ALPHA_SOFT); /* W467: Deckung wie alle LoC-Panels */
    if (body_h) bg_add(0, tab_h, SC(f * (OVS_REF_UI / OVS_REF_LOC), OVS_LP_BAR), (int)h, CHAT_ACC, OVS_LP_BAR_ALPHA); /* W467: Akzentlinie links wie Infopanel/Widget */
    bg_add(0, tab_h - ln, body_h ? (int)w : tabs_w, tab_h, CHAT_ACC, 0.45f);
    x = 0;
    for (t = 0; t < TAB_N; t++) {
        const chat_tabv *v = &s_tv[t];
        COLORREF tc;
        if (!tw[t]) continue; /* W474: Reiter nicht verfuegbar */
        if (v->act) {
            bg_add(x, 0, x + tw[t], tab_h - ul, CHAT_TAB_ACT, 0.94f);
            bg_add(x, tab_h - ul, x + tw[t], tab_h, CHAT_ACC, 1.0f); /* Unterstrich */
            tc = OVS_CREF(OVS_RGB_TAB_SEL);
        } else if (v->pulse > 0.0f) { /* sanfter Puls: Flaeche und Unterstrich gehen Richtung Akzent */
            bg_add(x, 0, x + tw[t], tab_h - ln, rgb_mix(CHAT_TAB_BG, CHAT_TAB_HOT, v->pulse), 0.66f + 0.26f * v->pulse);
            bg_add(x, tab_h - ul, x + tw[t], tab_h, CHAT_ACC, 0.25f + 0.70f * v->pulse);
            tc = rgbc(CHAT_ACC);
        } else {
            bg_add(x, 0, x + tw[t], tab_h - ln, CHAT_TAB_BG, v->av ? 0.66f : 0.52f);
            tc = v->unread > 0 ? rgbc(CHAT_ACC) : v->av ? OVS_CREF(OVS_RGB_TAB_TXT) : OVS_CREF(OVS_RGB_TAB_OFF);
        }
        SetTextColor(s_dc, tc);
        r.left = x + tpadx; r.top = tpy; r.right = x + tw[t]; r.bottom = tab_h - ul;
        DrawTextW(s_dc, v->label, -1, &r, DT_SINGLELINE | DT_NOPREFIX | DT_LEFT);
        x += tw[t] + tgap;
    }
    /* Zeilen */
    SelectObject(s_dc, fn);
    s_in_loc[3] = 0;
    y = tab_h + pad;
    for (i = first; i < nrows; i++) {
        wchar_t nb[LCH_NAME_MAX + 4];
        const chat_row *rw = &s_rows[i];
        int ed = rw->role != ROW_HIST;
        top[i] = y;
        if (rw->role == ROW_INPUT && i > first) /* feine Linie ueber der Eingabezeile */
            bg_add(pad, y - gap / 2 - ln, (int)w - pad, y - gap / 2, OVS_RGB_RULE, 0.75f);
        _snwprintf_s(nb, LCH_NAME_MAX + 4, _TRUNCATE, L"%s: ", rw->name);
        SetTextColor(s_dc, ed ? rgbc(CHAT_ACC) : name_color(rw->name, rw->kind));
        r.left = pad; r.top = y; r.right = pad + nmw[i]; r.bottom = y + th[i];
        DrawTextW(s_dc, nb, -1, &r, DT_SINGLELINE | DT_NOPREFIX);
        SetTextColor(s_dc, rw->role == ROW_COMP ? OVS_CREF(OVS_RGB_COMP)
                           : (rw->kind == LCH_L_SYS && !ed) ? OVS_CREF(OVS_RGB_SYS) : OVS_CREF(OVS_RGB_TEXT));
        r.left = pad + nmw[i]; r.right = (LONG)w - pad;
        DrawTextW(s_dc, rw->text, -1, &r, DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
        if (rw->role == ROW_INPUT) {
            s_in_loc[0] = pad + nmw[i];
            s_in_loc[1] = y;
            s_in_loc[2] = (int)w - 2 * pad - nmw[i];
            s_in_loc[3] = th[i];
        }
        y += th[i] + gap;
    }
    top[nrows] = (int)h;
    GdiFlush();
    if (!s_pm || s_pm_n < w * h) {
        if (s_pm) cz(s_pm, (size_t)s_pm_n * 4u);
        free(s_pm);
        s_pm_n = 0;
        s_pm = (unsigned *)malloc((size_t)w * h * 4u);
        if (!s_pm) {
            SelectObject(s_dc, fold);
            return 0;
        }
        s_pm_n = w * h;
    }
    /* Text (Farbe auf Schwarz, Abdeckung = groesster Kanal) ueber den Flaechen; ohne offene Zeile blendet jede
     * Zeile (samt ihrer Flaeche) einzeln aus, die Reiterleiste mit der neuesten */
    for (yy = 0; yy < h; yy++) {
        float fa = 1.0f;
        const chat_bg *rb[CHAT_BG_MAX];
        int nrb = 0;
        if (!s_typ_r) {
            if ((int)yy < tab_h) fa = s_strip_a;
            else {
                for (i = first; i < nrows; i++) {
                    int t0 = i == first ? tab_h : top[i] - gap / 2, t1 = i == nrows - 1 ? (int)h : top[i + 1] - gap / 2;
                    if ((int)yy >= t0 && (int)yy < t1) { fa = s_rows[i].a; break; }
                }
            }
        }
        for (b = 0; b < s_nbg; b++) {
            if ((int)yy >= s_bg[b].y0 && (int)yy < s_bg[b].y1) rb[nrb++] = &s_bg[b];
        }
        for (xx = 0; xx < w; xx++) {
            unsigned p = s_bits[yy * w + xx], bc = 0;
            float rr = (float)((p >> 16) & 255u), gg = (float)((p >> 8) & 255u), bb = (float)(p & 255u);
            float c = (rr > gg ? (rr > bb ? rr : bb) : (gg > bb ? gg : bb)) / 255.0f, ba = 0.0f, k;
            float oa, orr, og, ob;
            for (b = nrb - 1; b >= 0; b--) {
                if ((int)xx >= rb[b]->x0 && (int)xx < rb[b]->x1) {
                    bc = rb[b]->rgb;
                    ba = rb[b]->a;
                    break;
                }
            }
            k = ba * (1.0f - c);
            oa = (c + k) * fa;
            orr = (rr + (float)((bc >> 16) & 255u) * k) * fa;
            og = (gg + (float)((bc >> 8) & 255u) * k) * fa;
            ob = (bb + (float)(bc & 255u) * k) * fa;
            if (orr > 255.0f) orr = 255.0f;
            if (og > 255.0f) og = 255.0f;
            if (ob > 255.0f) ob = 255.0f;
            if (oa > 1.0f) oa = 1.0f;
            s_pm[yy * w + xx] = ((unsigned)(oa * 255.0f + 0.5f) << 24) | ((unsigned)(orr + 0.5f) << 16) |
                                ((unsigned)(og + 0.5f) << 8) | (unsigned)(ob + 0.5f);
        }
    }
    cz(s_bits, (size_t)w * h * 4u); /* Textpixel nicht im GDI-Puffer liegen lassen */
    SelectObject(s_dc, fold);
    s_w = w;
    s_h = h;
    s_tex_dirty = 1;
    return 1;
}

static int chat_upload(void)
{
    int r = ovs_tex_ensure(&s_tx, s_w, s_h);
    if (!r) return 0;
    if (r == 2) s_tex_dirty = 1;
    if (!s_tex_dirty) return 1;
    if (!ovs_tex_write(&s_tx, s_pm)) return 0;
    s_tex_dirty = 0;
    return 1;
}

void loc_chat_draw(ID3D11RenderTargetView *rtv, int save)
{
    unsigned bx, by, bw, bh, cw, maxh, m, gp, x0, y0;
    float f;
    int nrows;
    u32 sig[8];
    if (!loc_chat_visible() || !rtv || !g_dev || !g_ctx || !g_bb_w || !g_bb_h) return;
    ovs_loc_box(&bx, &by, &bw, &bh);
    f = ovs_scale_ui(bh);
    m = (unsigned)SC(f, OVS_MARGIN);
    gp = (unsigned)SC(f, OVS_LP_GAP);
    cw = (unsigned)SC(f, CHAT_W_UNITS);
    if (cw > g_bb_w * 45u / 100u) cw = g_bb_w * 45u / 100u;
    maxh = g_bb_h / 2u;
    if (cw < 96u || maxh < 48u) return;
    nrows = chat_collect(sig);
    if (nrows < 0) return;
    sig[3] = (u32)(f * 100.0f);
    sig[4] = cw;
    sig[5] = maxh;
    sig[6] = 0;
    sig[7] = 0;
    if (memcmp(sig, s_sig, sizeof sig) != 0 || !s_pm) {
        if (!chat_render(nrows, f, cw, maxh)) return;
        memcpy(s_sig, sig, sizeof sig);
    }
    cz(s_rows, sizeof s_rows);
    if (!chat_upload()) return;
    /* Bildschirmecke unten links, hinter allem, was dort in diesem Bild schon angemeldet ist */
    if (!ovs_layout_place(OVS_SLOT_CHAT, OVS_BOTTOM_LEFT, s_w, s_h, m, gp, &x0, &y0)) {
        x0 = m;
        y0 = g_bb_h > s_h + m ? g_bb_h - m - s_h : 0;
    }
    /* W474 Teil F: immer unten links, auch im Match (kein Anheben ueber die Instrumente, Nutzer 24.11.). Breitbild mit
     * schmalerer Spielbox: im freien linken Rand nur, wenn der Chat dort ganz Platz hat, sonst unten links im Spielbild
     * (nicht halb im Rand, halb im Bild). */
    if (bx > 0u && x0 < bx && x0 + s_w + m > bx) {
        unsigned nx = bx + m;
        if (nx + s_w + m <= g_bb_w && !ovs_layout_hits(OVS_SLOT_CHAT, nx, y0, s_w, s_h)) {
            x0 = nx;
            ovs_layout_claim(OVS_SLOT_CHAT, x0, y0, s_w, s_h);
        }
    }
    (void)bw;
    (void)by;
    if (s_in_loc[3] > 0) { /* IME: Kandidatenfenster an der Eingabezeile (UI-Thread rechnet in Fensterkoordinaten) */
        InterlockedExchange(&s_in_rect[0], (LONG)(x0 + (unsigned)s_in_loc[0]));
        InterlockedExchange(&s_in_rect[1], (LONG)(y0 + (unsigned)s_in_loc[1]));
        InterlockedExchange(&s_in_rect[2], (LONG)s_in_loc[2]);
        InterlockedExchange(&s_in_rect[3], (LONG)s_in_loc[3]);
    }
    if (save) sb_ovl_save_take(&s_save, rtv, x0, y0, s_w, s_h);
    sb_ovl_quad(rtv, s_tx.srv, x0, y0, s_w, s_h);
}

void loc_chat_restore(void)
{
    if (!s_save.saved) return;
    sb_ctx_enter();
    sb_ovl_save_put(&s_save);
    sb_ctx_leave();
}

void loc_chat_crash_wipe(void)
{
    if (!s_inst) return;
    InterlockedExchange(&s_wiped, 1);
    InterlockedExchange(&s_typing, 0);
    /* ohne Sperre (der abgestuerzte Thread kann sie halten): nur loeschen */
    lch_wipe(&s_st);
    lch_sn_wipe(&s_sn); /* W446: steelnet-Verlauf */
    lch_hist_clear(&s_team_hist); /* W474 */
    cz(s_self, sizeof s_self);
    cz(&s_in, sizeof s_in);
    cz(s_rows, sizeof s_rows);
    if (s_pm) cz(s_pm, (size_t)s_pm_n * 4u);
    cz(s_comp, sizeof s_comp);              /* W452: IME-Komposition und Testtexte */
    cz(s_ime_test_comp, sizeof s_ime_test_comp);
    cz(s_ime_test_res, sizeof s_ime_test_res);
    if (s_bits) cz(s_bits, (size_t)s_cv.w * s_cv.h * 4u);
    cz(s_seq, sizeof s_seq);
}
