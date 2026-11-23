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
#include "sb_lang.h"
#include "loc_chat.h"
#include "loc_chatnet.h"
#include "loc_net.h"
#include "loc_net_core.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef uint32_t u32;

void imouse_release_keys(void); /* input_mouse.c (UI-Thread) */

static volatile LONG s_inst, s_typing, s_avail, s_wiped;
static CRITICAL_SECTION s_cs;
static lch_state s_st;
static lch_input s_in;
static volatile LONG s_in_gen, s_hist_t; /* Eingabe geaendert / Zeit der neuesten Zeile des aktiven Tabs (Sichtbarkeit) */
/* W446 STEELNET-CHAT: zwei Tabs. TAB_LOBBY = Mitspieler der Lobby/des Matches (SBCH ueber SBT2, W437), TAB_SN = alle,
 * die mit steelnet verbunden sind (Steuerkanal des Treffpunkts). Lobby nur mit verbundenem Mitspieler. */
enum { TAB_LOBBY = 0, TAB_SN = 1 };
static lch_sn s_sn;                         /* Verlauf + Zaehler des steelnet-Tabs (unter s_cs) */
static volatile LONG s_tab, s_av_lobby, s_av_sn;
static volatile LONG s_tab_t[2];            /* Zeit der neuesten Zeile je Tab */
static volatile LONG s_unread[2];           /* neue Zeilen im jeweils anderen Tab (Kopfzeile) */
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

static lch_hist *tab_hist(int tab) { return tab == TAB_SN ? &s_sn.hist : &s_st.hist; }

/* W446: neue Zeile im Tab tab (unter s_cs): Sichtbarkeit bzw. Ungelesen-Zaehler des anderen Tabs */
static void tab_note(int tab)
{
    u32 now = now_ms();
    InterlockedExchange(&s_tab_t[tab], (LONG)now);
    if (tab == (int)s_tab) InterlockedExchange(&s_hist_t, (LONG)now);
    else if (s_unread[tab] < 99) InterlockedIncrement(&s_unread[tab]);
}

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
    EnterCriticalSection(&s_cs);
    if (lch_rx(&s_st, now_ms(), vip, pilot, d, len)) tab_note(TAB_LOBBY);
    LeaveCriticalSection(&s_cs);
}

/* W446: steelnet-Chat vom Steuerkanal (Transport-Thread, unter dessen Sperre; hier nur s_cs). */
static void sn_rx(int kind, const char *from, const uint8_t *text, int len)
{
    if (s_wiped || !g_cfg.loc_snchat) return;
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
    InterlockedExchange(&s_hist_t, s_tab_t[tab]);
    InterlockedIncrement(&s_in_gen);
    if (why) s_tab_auto++;
    else s_tab_key++;
    HLOG_I("LOCCHAT: Tab %s (%s)", tab == TAB_SN ? "steelnet" : "Lobby", why ? "automatisch" : "Taste");
}

static void chat_close(const char *why) /* UI-Thread */
{
    (void)why;
    EnterCriticalSection(&s_cs);
    lch_input_clear(&s_in);
    LeaveCriticalSection(&s_cs);
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

int loc_chat_wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!s_inst) return 0;
    if (w && !s_hwnd) s_hwnd = w;
    if (msg == CHAT_MSG_TEST_OPEN) { /* TEST: nur mit Testfolge, sonst ohne Wirkung */
        if (g_cfg.loc_chat_test[0] && !s_typing && s_avail && !s_wiped && !sb_ovl_open()) chat_open();
        return 1;
    }
    if (!s_typing) {
        if (msg == WM_KEYDOWN && s_avail && !s_wiped && !sb_ovl_open() && !(lp & 0x40000000) && chat_is_key(wp)) {
            chat_open();
            return 1;
        }
        return 0;
    }
    /* Zeile offen: Tastatur gehoert dem Chat (Tastenloslassen geht weiter, die Tasten sind ohnehin geloest) */
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (!(lp & 0x40000000)) chat_close("Escape");
        } else if (wp == VK_RETURN) {
            if (!(lp & 0x40000000)) chat_send();
        } else if (wp == VK_TAB) { /* W446: Tab wechseln, wenn der andere verfuegbar ist */
            int other = s_tab == TAB_SN ? TAB_LOBBY : TAB_SN;
            if (!(lp & 0x40000000) && (other == TAB_SN ? s_av_sn : s_av_lobby)) tab_set(other, 0);
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
    int last_avail = -1, last_lobby = 0, last_sn = -1;
    u32 tel_sig = 0;
    (void)unused;
    steps_parse(&s_seq[0], g_cfg.loc_chat_test);
    steps_parse(&s_seq[1], g_cfg.loc_snchat_test); /* W446 */
    for (;; loop++) {
        locnet *n;
        int av = 0, up = 0, av_lobby = 0, av_sn = 0;
        u32 now;
        Sleep(100);
        if (s_wiped) continue;
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
            /* W446: Lobby-Tab nur mit verbundenem Mitspieler (Lobby/Match), steelnet-Tab sobald abonniert */
            av_lobby = g_cfg.loc_chat && sec == 0 && up > 0 && (lb >= 2 || g_cfg.loc_chat_direct);
            av_sn = g_cfg.loc_snchat && sec == 0 && lb >= 2 && loctun_snchat_state(n->tun) == 2;
            if (av_sn && (loop % 10u) == 0u) { /* eigener Name beim Dienst (Echo als eigene Zeile markieren) */
                char me[LCH_NAME_MAX + 1] = "";
                loctun_my_pilot(n->tun, me, (int)sizeof me);
                EnterCriticalSection(&s_cs);
                memcpy(s_self, me, sizeof s_self);
                LeaveCriticalSection(&s_cs);
            }
        }
        av = av_lobby || av_sn;
        InterlockedExchange(&s_av_lobby, av_lobby);
        InterlockedExchange(&s_av_sn, av_sn);
        if (av_lobby && !last_lobby) tab_set(TAB_LOBBY, 1); /* Lobby betreten: Tab springt auf Lobby */
        else if (s_tab == TAB_LOBBY && !av_lobby && av_sn) tab_set(TAB_SN, 1);
        else if (s_tab == TAB_SN && !av_sn && av_lobby) tab_set(TAB_LOBBY, 1);
        last_lobby = av_lobby;
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
                  s_tab_key * 71u + s_sn.tx_rate_drop * 73u;
            if (sig != tel_sig) {
                HLOG_I("LOCCHAT-TEL: gesendet=%u empfangen=%u verworfen: ungueltig=%u rate=%u stumm=%u leer=%u doppelt=%u | "
                       "eigene: rate=%u leer=%u sendefehler=%u | geoeffnet=%u befehle=%u stumm_liste=%d",
                       s_st.tx_ok, s_st.rx_ok, s_st.rx_bad, s_st.rx_rate_drop, s_st.rx_muted, s_st.rx_empty, s_st.rx_dup,
                       s_st.tx_rate_drop, s_st.tx_empty, s_tx_send_fail, s_open_n, s_cmd_n, s_st.mute.n);
                HLOG_I("LOCCHAT-TEL steelnet: gesendet=%u empfangen=%u (verlauf=%u) verworfen: ungueltig=%u stumm=%u leer=%u "
                       "| eigene: rate=%u leer=%u | dienst-status=%u | tab: aktiv=%s auto=%u taste=%u",
                       s_sn.tx_ok, s_sn.rx_ok, s_sn.rx_hist, s_sn.rx_bad, s_sn.rx_muted, s_sn.rx_empty, s_sn.tx_rate_drop,
                       s_sn.tx_empty, s_sn.status_n, s_tab == TAB_SN ? "steelnet" : "Lobby", s_tab_auto, s_tab_key);
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

/* ------------------------------------------------------------------ Anzeige (Render-Thread) */
static HDC s_dc;
static HBITMAP s_bmp, s_bmp_old;
static unsigned *s_bits, *s_pm;
static unsigned s_dw, s_dh;          /* DIB-Groesse */
static unsigned s_w, s_h;            /* Inhalt */
static ID3D11Texture2D *s_tex;
static ID3D11ShaderResourceView *s_srv;
static unsigned s_tw, s_th;
static sb_ovl_save_t s_save;
static u32 s_sig[6];
static int s_tex_dirty;

/* W446: kind LCH_L_TAB = Kopfzeile der Tabs (name = Lobby, text = steelnet), tab = Tab der Zeile (Farben) */
#define LCH_L_TAB 3
typedef struct { wchar_t name[LCH_NAME_MAX + 1]; wchar_t text[LCH_LINE_W]; int kind; float a; int tab; } chat_row;
#define CHAT_ROWS (LCH_HIST + 2)
static chat_row s_rows[CHAT_ROWS];
static int s_hdr_act, s_hdr_av[2];   /* Kopfzeile: aktiver Tab, verfuegbare Tabs */

/* W446: eigene Farbe je Tab (Kennung, Eingabeaufforderung, Text der Zeilen) */
static COLORREF tab_color(int tab) { return tab == TAB_SN ? RGB(110, 190, 255) : RGB(120, 235, 150); }
static COLORREF tab_text(int tab, int kind)
{
    if (kind == LCH_L_SYS) return tab == TAB_SN ? RGB(195, 215, 235) : RGB(200, 230, 205);
    return tab == TAB_SN ? RGB(220, 238, 255) : RGB(230, 255, 235);
}

int loc_chat_visible(void)
{
    if (!s_inst || s_wiped || !(g_cfg.loc_chat || g_cfg.loc_snchat) || g_cfg.vr_enable || sb_ovl_open()) return 0;
    if (s_typing) return 1;
    return s_hist_t && (u32)(now_ms() - (u32)s_hist_t) < LCH_SHOW_MS;
}

static void chat_box(unsigned *bx, unsigned *by, unsigned *bw, unsigned *bh)
{
    float ga = g_loc_wide ? 16.0f / 9.0f : 4.0f / 3.0f, ba = (float)g_bb_w / (float)(g_bb_h ? g_bb_h : 1);
    if (ga >= ba) {
        *bw = g_bb_w;
        *bh = (unsigned)((float)g_bb_w / ga + 0.5f);
        if (*bh > g_bb_h) *bh = g_bb_h;
        *bx = 0;
        *by = (g_bb_h - *bh) / 2;
    } else {
        *bh = g_bb_h;
        *bw = (unsigned)((float)g_bb_h * ga + 0.5f);
        if (*bw > g_bb_w) *bw = g_bb_w;
        *bx = (g_bb_w - *bw) / 2;
        *by = 0;
    }
}

static int SC(float f, float x)
{
    int v = (int)(x * f + 0.5f);
    return v < 1 ? 1 : v;
}

static COLORREF name_color(const wchar_t *n, int kind)
{
    static const COLORREF pal[8] = { RGB(255, 196, 92), RGB(120, 210, 255), RGB(255, 140, 140), RGB(170, 255, 140),
                                     RGB(230, 160, 255), RGB(255, 230, 110), RGB(120, 255, 220), RGB(255, 170, 210) };
    u32 h = 2166136261u;
    if (kind == LCH_L_SELF) return RGB(255, 255, 255);
    if (kind == LCH_L_SYS) return RGB(150, 190, 160);
    for (; *n; n++) h = (h ^ (u32)(*n | 0x20)) * 16777619u;
    return pal[h % 8u];
}

/* Zeilen holen (unter s_cs kurz), Sichtbarkeit/Ausblenden je Zeile. Rueckgabe Anzahl; *typ = Eingabezeile dabei. */
static int chat_collect(int *typ, u32 *sig)
{
    int k, n = 0, typing = s_typing != 0, tab = (int)s_tab;
    u32 now = now_ms(), h = 0;
    const lch_hist *hs;
    EnterCriticalSection(&s_cs);
    hs = tab_hist(tab);
    if (typing) { /* W446: Kopfzeile mit beiden Tabs, der aktive markiert */
        chat_row *r = &s_rows[n];
        wchar_t lb[24], sn[24];
        long un[2];
        un[0] = s_unread[0];
        un[1] = s_unread[1];
        sb_lang_w("chat.tab_lobby", lb, 24);
        sb_lang_w("chat.tab_steelnet", sn, 24);
        s_hdr_act = tab;
        s_hdr_av[0] = s_av_lobby != 0;
        s_hdr_av[1] = s_av_sn != 0;
        if (tab == TAB_LOBBY) _snwprintf_s(r->name, LCH_NAME_MAX + 1, _TRUNCATE, L"[%s]", lb);
        else if (un[0] > 0) _snwprintf_s(r->name, LCH_NAME_MAX + 1, _TRUNCATE, L"%s (%ld)", lb, un[0]);
        else wcsncpy_s(r->name, LCH_NAME_MAX + 1, lb, _TRUNCATE);
        if (tab == TAB_SN) _snwprintf_s(r->text, LCH_LINE_W, _TRUNCATE, L"[%s]", sn);
        else if (un[1] > 0) _snwprintf_s(r->text, LCH_LINE_W, _TRUNCATE, L"%s (%ld)", sn, un[1]);
        else wcsncpy_s(r->text, LCH_LINE_W, sn, _TRUNCATE);
        r->kind = LCH_L_TAB;
        r->a = 1.0f;
        r->tab = tab;
        h = h * 31u + (u32)un[0] * 3u + (u32)un[1] * 5u + (u32)s_hdr_av[0] * 7u + (u32)s_hdr_av[1] * 11u;
        n++;
    }
    for (k = LCH_HIST - 1; k >= 0; k--) { /* aelteste zuerst */
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
        s_rows[n].a = a;
        s_rows[n].tab = tab;
        h = h * 31u + l->t + (u32)(a * 8.0f) * 7u;
        n++;
    }
    if (typing) {
        chat_row *r = &s_rows[n];
        sb_lang_w("chat.prompt", r->name, LCH_NAME_MAX + 1);
        lch_input_w(&s_in, r->text, LCH_LINE_W - 2);
        wcsncat_s(r->text, LCH_LINE_W, L"_", _TRUNCATE);
        r->kind = LCH_L_SYS;
        r->a = 1.0f;
        r->tab = tab;
        n++;
    }
    h = h * 31u + hs->gen + (u32)tab * 977u;
    LeaveCriticalSection(&s_cs);
    *typ = typing;
    sig[0] = h;
    sig[1] = (u32)s_in_gen;
    sig[2] = (u32)typing;
    return n;
}

static void chat_gdi_free(void)
{
    if (s_bmp) {
        if (s_bits) cz(s_bits, (size_t)s_dw * s_dh * 4u);
        SelectObject(s_dc, s_bmp_old);
        DeleteObject(s_bmp);
        s_bmp = NULL;
        s_bits = NULL;
    }
}

/* Inhalt in s_pm rendern (vormultipliziertes BGRA, s_w x s_h). 1 = ok. */
static int chat_render(int nrows, float f, unsigned maxw, unsigned maxh)
{
    BITMAPINFO bi;
    HFONT fn, fold;
    int px = SC(f, 10.0f), pad = SC(f, 4.0f), gap = SC(f, 2.0f), y, i, top[CHAT_ROWS + 1], first = 0, hdr = 0;
    int nmw[CHAT_ROWS], th[CHAT_ROWS], wmax = 0;
    unsigned x, yy, w, h;
    RECT r;
    if (!s_dc) {
        s_dc = CreateCompatibleDC(NULL);
        if (!s_dc) return 0;
    }
    fn = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                     ANTIALIASED_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    if (!fn) return 0;
    fold = (HFONT)SelectObject(s_dc, fn);
    /* Masse: Name + ": " haengend, Text umgebrochen (auch lange Woerter) */
    for (i = 0; i < nrows; i++) {
        wchar_t nb[LCH_NAME_MAX + 4];
        SIZE sz;
        int tw;
        _snwprintf_s(nb, LCH_NAME_MAX + 4, _TRUNCATE, s_rows[i].kind == LCH_L_TAB ? L"%s   " : L"%s: ", s_rows[i].name);
        if (!GetTextExtentPoint32W(s_dc, nb, (int)wcslen(nb), &sz)) sz.cx = px * 6;
        nmw[i] = sz.cx;
        tw = (int)maxw - 2 * pad - nmw[i];
        if (tw < px * 4) tw = px * 4;
        r.left = 0; r.top = 0; r.right = tw; r.bottom = 10000;
        DrawTextW(s_dc, s_rows[i].text, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
        th[i] = r.bottom > px ? r.bottom : px + px / 3;
        if (nmw[i] + r.right > wmax) wmax = nmw[i] + r.right;
    }
    /* zu hoch fuer die untere Bildhaelfte: aelteste weglassen (W446: die Tab-Kopfzeile bleibt immer) */
    hdr = nrows > 1 && s_rows[0].kind == LCH_L_TAB;
    first = hdr;
    for (;;) {
        int tot = 2 * pad + (hdr ? th[0] + gap : 0);
        for (i = first; i < nrows; i++) tot += th[i] + (i > first ? gap : 0);
        if ((unsigned)tot <= maxh || first >= nrows - 1) break;
        first++;
    }
    w = (unsigned)(wmax + 2 * pad);
    if (w > maxw) w = maxw;
    h = (unsigned)(2 * pad + (hdr ? th[0] + gap : 0));
    for (i = first; i < nrows; i++) h += (unsigned)(th[i] + (i > first ? gap : 0));
    if (w < 8u || h < 8u) {
        SelectObject(s_dc, fold);
        DeleteObject(fn);
        return 0;
    }
    if (!s_bmp || s_dw != w || s_dh != h) {
        chat_gdi_free();
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = (LONG)w;
        bi.bmiHeader.biHeight = -(LONG)h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        s_bmp = CreateDIBSection(s_dc, &bi, DIB_RGB_COLORS, (void **)&s_bits, NULL, 0);
        if (!s_bmp || !s_bits) {
            s_bmp = NULL;
            SelectObject(s_dc, fold);
            DeleteObject(fn);
            return 0;
        }
        s_bmp_old = (HBITMAP)SelectObject(s_dc, s_bmp);
        s_dw = w;
        s_dh = h;
    }
    memset(s_bits, 0, (size_t)w * h * 4u);
    SetBkMode(s_dc, TRANSPARENT);
    y = pad;
    if (hdr) { /* W446: [aktiver Tab] in seiner Farbe, der andere grau (nicht verfuegbar: dunkler) */
        static const COLORREF off = RGB(150, 160, 155), na = RGB(95, 105, 100);
        wchar_t nb[LCH_NAME_MAX + 4];
        _snwprintf_s(nb, LCH_NAME_MAX + 4, _TRUNCATE, L"%s   ", s_rows[0].name);
        top[0] = y;
        SetTextColor(s_dc, s_hdr_act == TAB_LOBBY ? tab_color(TAB_LOBBY) : s_hdr_av[0] ? off : na);
        r.left = pad; r.top = y; r.right = pad + nmw[0]; r.bottom = y + th[0];
        DrawTextW(s_dc, nb, -1, &r, DT_SINGLELINE | DT_NOPREFIX);
        SetTextColor(s_dc, s_hdr_act == TAB_SN ? tab_color(TAB_SN) : s_hdr_av[1] ? off : na);
        r.left = pad + nmw[0]; r.right = (LONG)w - pad;
        DrawTextW(s_dc, s_rows[0].text, -1, &r, DT_SINGLELINE | DT_NOPREFIX);
        y += th[0] + gap;
    }
    for (i = first; i < nrows; i++) {
        wchar_t nb[LCH_NAME_MAX + 4];
        int prompt = hdr && i == nrows - 1; /* Kopfzeile nur beim Tippen */
        _snwprintf_s(nb, LCH_NAME_MAX + 4, _TRUNCATE, L"%s: ", s_rows[i].name);
        top[i] = y;
        SetTextColor(s_dc, prompt ? tab_color(s_rows[i].tab) : name_color(s_rows[i].name, s_rows[i].kind));
        r.left = pad; r.top = y; r.right = pad + nmw[i]; r.bottom = y + th[i];
        DrawTextW(s_dc, nb, -1, &r, DT_SINGLELINE | DT_NOPREFIX);
        SetTextColor(s_dc, tab_text(s_rows[i].tab, s_rows[i].kind));
        r.left = pad + nmw[i]; r.right = (LONG)w - pad;
        DrawTextW(s_dc, s_rows[i].text, -1, &r, DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
        y += th[i] + gap;
    }
    top[nrows] = (int)h;
    GdiFlush();
    if (!s_pm || s_w * s_h < w * h) {
        if (s_pm) cz(s_pm, (size_t)s_w * s_h * 4u);
        free(s_pm);
        s_pm = (unsigned *)malloc((size_t)w * h * 4u);
        if (!s_pm) {
            SelectObject(s_dc, fold);
            DeleteObject(fn);
            return 0;
        }
    }
    /* Text (Farben mit vollem Hoechstkanal) -> Abdeckung = groesster Kanal; halbtransparenter Hintergrund je Zeile */
    for (yy = 0; yy < h; yy++) {
        float fa = 1.0f;
        int row = -1;
        for (i = first; i < nrows && !hdr; i++) { /* W446: mit Kopfzeile (Tippen) alles deckend */
            int t0 = i == first ? 0 : top[i] - gap / 2, t1 = i == nrows - 1 ? (int)h : top[i + 1] - gap / 2;
            if ((int)yy >= t0 && (int)yy < t1) { row = i; break; }
        }
        if (row >= 0) fa = s_rows[row].a;
        for (x = 0; x < w; x++) {
            unsigned p = s_bits[yy * w + x];
            float rr = (float)((p >> 16) & 255u), gg = (float)((p >> 8) & 255u), bb = (float)(p & 255u);
            float c = (rr > gg ? (rr > bb ? rr : bb) : (gg > bb ? gg : bb)) / 255.0f;
            float ba = 0.58f, br = 6.0f * ba, bgc = 18.0f * ba, bbb = 10.0f * ba;
            float oa = c + ba * (1.0f - c), orr = rr + br * (1.0f - c), og = gg + bgc * (1.0f - c), ob = bb + bbb * (1.0f - c);
            oa *= fa; orr *= fa; og *= fa; ob *= fa;
            if (orr > 255.0f) orr = 255.0f;
            if (og > 255.0f) og = 255.0f;
            if (ob > 255.0f) ob = 255.0f;
            s_pm[yy * w + x] = ((unsigned)(oa * 255.0f + 0.5f) << 24) | ((unsigned)(orr + 0.5f) << 16) |
                               ((unsigned)(og + 0.5f) << 8) | (unsigned)(ob + 0.5f);
        }
    }
    cz(s_bits, (size_t)w * h * 4u); /* Textpixel nicht im GDI-Puffer liegen lassen */
    SelectObject(s_dc, fold);
    DeleteObject(fn);
    s_w = w;
    s_h = h;
    s_tex_dirty = 1;
    return 1;
}

static int chat_upload(void)
{
    D3D11_MAPPED_SUBRESOURCE map;
    unsigned y;
    if (!s_tex || s_tw < s_w || s_th < s_h) {
        D3D11_TEXTURE2D_DESC td;
        if (s_srv) { ID3D11ShaderResourceView_Release(s_srv); s_srv = NULL; }
        if (s_tex) { ID3D11Texture2D_Release(s_tex); s_tex = NULL; }
        memset(&td, 0, sizeof td);
        td.Width = s_w;
        td.Height = s_h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &s_tex)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)s_tex, NULL, &s_srv))) {
            if (s_tex) { ID3D11Texture2D_Release(s_tex); s_tex = NULL; }
            return 0;
        }
        s_tw = s_w;
        s_th = s_h;
        s_tex_dirty = 1;
    }
    if (!s_tex_dirty) return 1;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)s_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return 0;
    for (y = 0; y < s_h; y++) memcpy((unsigned char *)map.pData + (size_t)y * map.RowPitch, s_pm + (size_t)y * s_w, s_w * 4u);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)s_tex, 0);
    s_tex_dirty = 0;
    return 1;
}

void loc_chat_draw(ID3D11RenderTargetView *rtv, int save)
{
    unsigned bx, by, bw, bh, maxw, maxh, m, x0, y0;
    float f;
    int typ, nrows;
    u32 sig[6];
    if (!loc_chat_visible() || !rtv || !g_dev || !g_ctx || !g_bb_w || !g_bb_h) return;
    chat_box(&bx, &by, &bw, &bh);
    f = (float)bh / 540.0f;
    if (f < 1.0f) f = 1.0f;
    m = (unsigned)SC(f, 10.0f);
    maxw = (unsigned)((float)bw * 0.42f);
    maxh = bh / 2u > 2u * m ? bh / 2u - 2u * m : bh / 2u;
    if (maxw < 64u || maxh < 32u) return;
    nrows = chat_collect(&typ, sig);
    if (nrows <= 0) return;
    sig[3] = (u32)(f * 100.0f);
    sig[4] = maxw;
    sig[5] = maxh;
    if (memcmp(sig, s_sig, sizeof sig) != 0 || !s_pm) {
        if (!chat_render(nrows, f, maxw, maxh)) return;
        memcpy(s_sig, sig, sizeof sig);
    }
    cz(s_rows, sizeof s_rows);
    if (!chat_upload()) return;
    /* links am Rand, untere Bildhaelfte, dort senkrecht mittig */
    x0 = bx + m;
    y0 = by + (bh * 3u) / 4u > s_h / 2u ? by + (bh * 3u) / 4u - s_h / 2u : by;
    if (y0 < by + bh / 2u) y0 = by + bh / 2u;
    if (y0 + s_h > by + bh - m) y0 = by + bh > s_h + m ? by + bh - m - s_h : by;
    if (save) sb_ovl_save_take(&s_save, rtv, x0, y0, s_w, s_h);
    sb_ovl_quad(rtv, s_srv, x0, y0, s_w, s_h);
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
    cz(s_self, sizeof s_self);
    cz(&s_in, sizeof s_in);
    cz(s_rows, sizeof s_rows);
    if (s_pm) cz(s_pm, (size_t)s_w * s_h * 4u);
    if (s_bits) cz(s_bits, (size_t)s_dw * s_dh * 4u);
    cz(s_seq, sizeof s_seq);
}
