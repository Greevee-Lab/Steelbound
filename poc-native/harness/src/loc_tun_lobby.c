/* loc_tun_lobby.c - W464 NET-CORE: aus loc_tun.c verschoben (W324/W334/W446 u. a.), unveraendert.
 * Treffpunkt steelnet: Mini-JSON mit Grenzpruefung, Steuerkanal (TLS mit SPKI-Pin, HELLO/Anmeldung, BIND),
 * Relays und Wegwahl (route), peer_info, lobby_state, Lobby betreten, steelnet-Chat, Zeilenleser lb_on_line,
 * Steuerkanal-Zyklus lb_poll, Relay-/Bindungs-Antworten (SBN) auf dem UDP-Socket. ONLINE-Pfad (vgl. LAN, §W464). */
#include "loc_tun_int.h"

/* ====================================================================== Treffpunkt (steelnet) */
/* --- Mini-JSON (nur fuer die Antworten unseres eigenen Dienstes, mit Grenzpruefung) --- */
static const char *js_ws(const char *p, const char *e)
{
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

const char *js_skip(const char *p, const char *e, int depth)
{
    p = js_ws(p, e);
    if (p >= e || depth > 16) return NULL;
    if (*p == '"') {
        for (p++; p < e; p++) {
            if (*p == '\\') { p++; continue; }
            if (*p == '"') return p + 1;
        }
        return NULL;
    }
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        p = js_ws(p + 1, e);
        if (p < e && *p == close) return p + 1;
        for (;;) {
            if (close == '}') {
                p = js_skip(p, e, depth + 1); /* Schluessel */
                if (!p) return NULL;
                p = js_ws(p, e);
                if (p >= e || *p != ':') return NULL;
                p++;
            }
            p = js_skip(p, e, depth + 1);
            if (!p) return NULL;
            p = js_ws(p, e);
            if (p >= e) return NULL;
            if (*p == ',') { p = js_ws(p + 1, e); continue; }
            if (*p == close) return p + 1;
            return NULL;
        }
    }
    while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\r' && *p != '\n') p++;
    return p;
}

/* Wert zu key im Objekt ab obj ('{'); *ve = Ende des Wertes. */
const char *js_get(const char *obj, const char *e, const char *key, const char **ve)
{
    const char *p = js_ws(obj, e);
    size_t kl = strlen(key);
    if (p >= e || *p != '{') return NULL;
    p = js_ws(p + 1, e);
    while (p < e && *p == '"') {
        const char *ks = p + 1, *kend = js_skip(p, e, 0), *v, *vend;
        if (!kend) return NULL;
        p = js_ws(kend, e);
        if (p >= e || *p != ':') return NULL;
        v = js_ws(p + 1, e);
        vend = js_skip(v, e, 0);
        if (!vend) return NULL;
        if ((size_t)(kend - 1 - ks) == kl && memcmp(ks, key, kl) == 0) {
            if (ve) *ve = vend;
            return v;
        }
        p = js_ws(vend, e);
        if (p < e && *p == ',') p = js_ws(p + 1, e);
    }
    return NULL;
}

int js_num(const char *v, const char *ve, double *out)
{
    char b[48];
    size_t l;
    if (!v || v >= ve) return 0;
    l = (size_t)(ve - v);
    if (l >= sizeof b || *v == '"' || *v == 'n' || *v == '{' || *v == '[') return 0;
    memcpy(b, v, l);
    b[l] = 0;
    *out = atof(b);
    return 1;
}

int js_str(const char *v, const char *ve, char *out, int cap)
{
    int o = 0;
    if (!v || v >= ve || *v != '"') return 0;
    for (v++; v < ve - 1 && o < cap - 1; v++) {
        if (*v == '\\' && v + 1 < ve - 1) {
            v++;
            out[o++] = *v == 'n' ? '\n' : *v;
            continue;
        }
        out[o++] = *v;
    }
    out[o] = 0;
    return 1;
}

/* W487 LIVE-GAMERTAG: Gamertag-Regel wie steelcore/loclive_tag_valid (1-15, A-Z a-z 0-9, einzelne Leerzeichen,
 * vorn Buchstabe, hinten kein Leerzeichen); hier eigene Kopie, weil loc_tun ohne loc_live gelinkt wird. */
static int lb_tag_valid(const char *s)
{
    size_t n = strlen(s), i;
    if (n < 1 || n > 15 || !((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) || s[n - 1] == ' ') return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c == ' ' ? s[i + 1] == ' ' : !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            return 0;
    }
    return 1;
}

const char *js_arr_first(const char *v, const char *ve)
{
    const char *p;
    if (!v || v >= ve || *v != '[') return NULL;
    p = js_ws(v + 1, ve);
    return (p < ve && *p != ']') ? p : NULL;
}

const char *js_arr_next(const char *el, const char *ve, const char **elend)
{
    const char *p = js_skip(el, ve, 0);
    if (elend) *elend = p;
    if (!p) return NULL;
    p = js_ws(p, ve);
    if (p < ve && *p == ',') return js_ws(p + 1, ve);
    return NULL;
}

/* [ip, port] */
static int js_ep(const char *v, const char *ve, struct sockaddr_in *out)
{
    const char *a = js_arr_first(v, ve), *ae, *b, *be;
    char ip[48];
    double port;
    struct in_addr in;
    if (!a) return 0;
    b = js_arr_next(a, ve, &ae);
    if (!b || !js_str(a, ae, ip, sizeof ip)) return 0;
    js_arr_next(b, ve, &be);
    if (!be || !js_num(b, be, &port) || port < 1 || port > 65535) return 0;
    if (inet_pton(AF_INET, ip, &in) != 1) return 0;
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    out->sin_addr = in;
    out->sin_port = htons((uint16_t)port);
    return 1;
}

/* W521: IPv6-Kandidat ["addr", port] aus peer_info.eps6 -> Ersatzadresse 0.254.0.i (tl_v6_intern). Nur global unicast
 * (TEST v6_loop: nur ::1); die Adresse selbst wird nie protokolliert. */
static int lb_js_ep6(loctun *t, const char *v, const char *ve, struct sockaddr_in *out)
{
    const char *a = js_arr_first(v, ve), *ae, *b, *be;
    char ip[64];
    double port;
    struct sockaddr_in6 a6;
    if (!a) return 0;
    b = js_arr_next(a, ve, &ae);
    if (!b || !js_str(a, ae, ip, sizeof ip)) return 0;
    js_arr_next(b, ve, &be);
    if (!be || !js_num(b, be, &port) || port < 1 || port > 65535) return 0;
    memset(&a6, 0, sizeof a6);
    a6.sin6_family = AF_INET6;
    if (inet_pton(AF_INET6, ip, &a6.sin6_addr) != 1 || !tl_v6_cand_ok(t, &a6.sin6_addr)) return 0;
    a6.sin6_port = htons((uint16_t)port);
    SecureZeroMemory(ip, sizeof ip);
    return tl_v6_intern(t, &a6, out);
}

/* W440 LOC-LOBBY-MODES: Modus eigener Lobbys beim Anlegen (Einstellung des Hosts; ungueltig = Sim). */
const char *lb_my_play_mode(const loctun *t)
{
    return las_parse(t->c.play_mode) == LAS_ARCADE ? "arcade" : "sim";
}

/* W440: Modus der aktuellen Lobby setzen (0 = keine); Logzeile nur bei Aenderung (keine Namen, kein Raumcode). */
void lb_play_mode_set(loctun *t, int mode, const char *why)
{
    LONG old = InterlockedExchange(&t->play_mode, (LONG)mode);
    if (old != (LONG)mode) {
        tl_logf(t, "LOCMODE: Lobby-Modus %s (%s)", mode ? las_name(mode) : "keiner", why);
    }
}

static void lb_close(loctun *t, const char *why, double now);

/* W373 TEST: gesendete Steuerzeile mitschneiden */
static void lb_cap(loctun *t, const char *b, int l)
{
    if (l > 0 && t->test_cap && t->test_cap_len + l < t->test_cap_max) {
        memcpy(t->test_cap + t->test_cap_len, b, (size_t)l);
        t->test_cap_len += l;
        t->test_cap[t->test_cap_len] = 0;
    }
}

/* W500: eigene Zeile nach dem welcome bis zur Bestaetigung des Dienstes behalten (Nachsenden nach einer Fortsetzung).
 * Ring voll: die aelteste faellt heraus (rs_base steigt); hat der Dienst sie nicht, scheitert die Fortsetzung sauber. */
static void lb_rs_keep(loctun *t, const char *b, int l)
{
    int i;
    if (l <= 0 || l > TUN_RS_LINE) return;
    if (t->rs_qn == TUN_RS_Q) {
        SecureZeroMemory(t->rs_q[t->rs_qh], (size_t)t->rs_ql[t->rs_qh]);
        t->rs_qh = (t->rs_qh + 1) % TUN_RS_Q;
        t->rs_qn--;
        t->rs_base++;
    }
    i = (t->rs_qh + t->rs_qn) % TUN_RS_Q;
    memcpy(t->rs_q[i], b, (size_t)l);
    t->rs_ql[i] = l;
    t->rs_qn++;
}

/* W500: der Dienst hat alle eigenen Zeilen < rx (pong bzw. welcome der Fortsetzung). 0 = rx ausserhalb. */
static int lb_rs_ack(loctun *t, uint64_t rx)
{
    if (rx < t->rs_base || rx > t->rs_base + (uint64_t)t->rs_qn) return 0;
    while (t->rs_base < rx) { /* Klartext (z. B. Chat) sofort loeschen */
        SecureZeroMemory(t->rs_q[t->rs_qh], (size_t)t->rs_ql[t->rs_qh]);
        t->rs_qh = (t->rs_qh + 1) % TUN_RS_Q;
        t->rs_qn--;
        t->rs_base++;
    }
    return 1;
}

/* W334: eine JSON-Zeile ueber TLS (nie vor bestandener Pin-Pruefung; sbtls_write lehnt das selbst ab). */
void lb_send(loctun *t, const char *json)
{
    char b[4200];
    int l = _snprintf_s(b, sizeof b, _TRUNCATE, "%s\n", json);
    lb_cap(t, b, l);
    if (l > 0 && t->rs_susp) { /* W500: Verbindung weg, Fortsetzung laeuft - danach nachsenden */
        if (t->rs_count) lb_rs_keep(t, b, l);
    } else if (l > 0 && t->lb_state == LB_HELLO && !t->my_cid && t->acct && t->c.resume) {
        /* W500 (Pruefung F4): vor dem welcome halten - der Dienst zaehlt Zeilen erst nach seinem welcome; ginge die
         * Zeile vorher hinaus, kaeme sie dort ggf. nach dem welcome an und beide Zaehler liefen auseinander */
        lb_rs_keep(t, b, l);
    } else if (l > 0 && (t->lb_state == LB_HELLO || t->lb_state == LB_LOBBY)) {
        if (t->rs_count) lb_rs_keep(t, b, l); /* W500: zaehlt wie beim Dienst jede Zeile nach dem welcome */
        if (t->tls && t->lb_s != INVALID_SOCKET && sbtls_write(t->tls, b, l) != SBTLS_OK) lb_lost(t, "tls-senden", tl_now(t));
    }
    SecureZeroMemory(b, sizeof b);
}

/* W500: Anmeldezeilen (hello, auth) direkt senden - nie in die Nachsende-Schlange (waehrend einer Fortsetzung landete
 * lb_send dort) und nicht mitgezaehlt (der Dienst zaehlt erst ab seinem welcome). */
static void lb_send_now(loctun *t, const char *json)
{
    char b[1024];
    int l = _snprintf_s(b, sizeof b, _TRUNCATE, "%s\n", json);
    lb_cap(t, b, l);
    if (l > 0 && t->tls && t->lb_s != INVALID_SOCKET && sbtls_write(t->tls, b, l) != SBTLS_OK) {
        SecureZeroMemory(b, sizeof b);
        lb_lost(t, "tls-senden", tl_now(t));
        return;
    }
    SecureZeroMemory(b, sizeof b);
}

/* W500 LOBBY-RESUME (SECURITY.md §47) ---------------------------------------------------------------------------
 * Reisst die TLS-Steuerverbindung ab (Feld: Reset von aussen 20 s nach dem Verbinden), bleibt die Sitzung stehen:
 * gleiche client_id, gleiches E (kp/my_pub), alle Mitspieler-Verbindungen und Paarschluessel unveraendert. Der Client
 * verbindet neu (TLS mit Pin), meldet sich mit dem Spielschluessel an und legt das Einmal-Token vor. Lehnt der Dienst ab
 * (welcome ohne "resumed") oder laeuft die Frist ab: lb_close wie bisher. */
void lb_rs_reset(loctun *t)
{
    int i;
    for (i = 0; i < TUN_RS_Q; i++)
        if (t->rs_ql[i]) SecureZeroMemory(t->rs_q[i], (size_t)t->rs_ql[i]);
    memset(t->rs_ql, 0, sizeof t->rs_ql);
    t->rs_qn = t->rs_qh = 0;
    t->rs_base = t->rs_rx = 0;
    t->rs_count = t->rs_susp = t->rs_try = 0;
    t->rs_cut_next = 0;
    SecureZeroMemory(t->rs_tok, sizeof t->rs_tok);
    if (t->rs_old_tls) {
        sbtls_free(t->rs_old_tls);
        t->rs_old_tls = NULL;
    }
    if (t->rs_old_s != INVALID_SOCKET) {
        closesocket(t->rs_old_s);
        t->rs_old_s = INVALID_SOCKET;
    }
}

static int lb_rs_can(const loctun *t, const char *why)
{
    if (!t->c.resume || !t->acct || !t->rs_tok[0] || !t->my_cid || !t->rs_count) return 0;
    if (!t->rs_susp && t->lb_state != LB_HELLO && t->lb_state != LB_LOBBY) return 0;
    /* nur Transportfehler; Pin, Protokollfehler, Ablehnungen des Dienstes -> lb_close */
    return strcmp(why, "eof") == 0 || strncmp(why, "tls", 3) == 0 || strcmp(why, "connect") == 0 ||
           strcmp(why, "timeout") == 0 || strcmp(why, "welcome-timeout") == 0;
}

void lb_lost(loctun *t, const char *why, double now)
{
    static const double backoff[4] = {500.0, 1000.0, 2000.0, 4000.0};
    double nt;
    if (!lb_rs_can(t, why)) {
        lb_close(t, why, now);
        return;
    }
    if (!t->rs_susp) {
        /* Frist: W517 - der Dienst haelt die Sitzung rttl s ab dem Zeitpunkt, an dem er den Abriss bemerkt (seit W517
         * 120 s; vorher 90 s ab der letzten Zeile). Er bemerkt ihn nie spaeter als wir selbst (Reset sofort, stiller
         * Abriss: ping_timeout 45 s bzw. unsere Stille-Erkennung 40 s) -> bis rttl - 10 s versuchen (hoechstens 150 s).
         * Ein zu spaeter Versuch kostet nichts: der Dienst antwortet mit einem normalen Einstieg -> lb_close wie bisher. */
        double f = (t->rs_ttl_ms > 0 ? (double)t->rs_ttl_ms : 90000.0) - 10000.0;
        if (f > 150000.0) f = 150000.0;
        if (f < 3000.0) f = 3000.0;
        t->rs_susp = 1;
        t->rs_try = 0;
        t->rs_since = now;
        t->rs_deadline = now + f;
        tl_logf(t, "LOCLOBBY: Treffpunkt-Verbindung abgerissen (%s) - setze Sitzung c%u fort, Mitspieler bleiben verbunden",
                why, t->my_cid);
    } else {
        t->rs_try++;
        tl_logf(t, "LOCLOBBY: Fortsetzung Versuch %d gescheitert (%s)", t->rs_try, why);
    }
    if (now >= t->rs_deadline) {
        lb_close(t, "resume-frist", now);
        return;
    }
    if (t->tls) {
        sbtls_free(t->tls);
        t->tls = NULL;
    }
    if (t->lb_s != INVALID_SOCKET) {
        WSAEventSelect(t->lb_s, NULL, 0);
        closesocket(t->lb_s);
        t->lb_s = INVALID_SOCKET;
    }
    t->lb_state = LB_OFF;
    t->lb_inlen = 0;
    nt = now + backoff[t->rs_try < 3 ? t->rs_try : 3];
    t->lb_next_try = nt < t->rs_deadline ? nt : t->rs_deadline;
}

/* W500 TEST loc_lobby_cut: Abriss wie im Feld nachstellen. Modus 0: harter Reset (RST, Dienst sieht den Fehler sofort);
 * Modus 1: Verbindung still liegenlassen (Dienst merkt nichts, die Fortsetzung muss die alte selbst schliessen). */
static void lb_cut_test(loctun *t, double now)
{
    tl_logf(t, "LOCLOBBY: TEST loc_lobby_cut - Steuerverbindung %s", t->c.cut_mode == 1 ? "still liegengelassen" : "hart getrennt");
    t->rs_cut_next = 0;
    if (t->c.cut_mode == 1 && t->lb_s != INVALID_SOCKET) {
        if (t->rs_old_tls) sbtls_free(t->rs_old_tls);
        if (t->rs_old_s != INVALID_SOCKET) closesocket(t->rs_old_s);
        WSAEventSelect(t->lb_s, NULL, 0);
        t->rs_old_s = t->lb_s;
        t->rs_old_tls = t->tls;
        t->lb_s = INVALID_SOCKET;
        t->tls = NULL;
    } else if (t->lb_s != INVALID_SOCKET) {
        struct linger lg0 = {1, 0};
        setsockopt(t->lb_s, SOL_SOCKET, SO_LINGER, (const char *)&lg0, sizeof lg0);
    }
    lb_lost(t, "tls", now);
}

static void lb_bind(loctun *t, double now);

/* W500: welcome auf eine Fortsetzung. Nur mit resumed:true, derselben client_id, neuem Token und neuem udp_key gilt sie;
 * sonst hat der Dienst einen normalen neuen Einstieg gemacht -> lb_close (neues E, neue Mitspieler-Schluessel). */
static void lb_rs_on_welcome(loctun *t, const char *m, const char *e, int resumed, double now)
{
    const char *v, *ve;
    double cid = 0, rx = -1, ttl = 0;
    char key[80] = "", tok[80] = "";
    uint8_t k[32], tk[32];
    int ok, i, n = 0;
    v = js_get(m, e, "client_id", &ve);
    js_num(v, ve, &cid);
    v = js_get(m, e, "udp_key", &ve);
    js_str(v, ve, key, sizeof key);
    v = js_get(m, e, "rtok", &ve);
    js_str(v, ve, tok, sizeof tok);
    v = js_get(m, e, "rx", &ve);
    js_num(v, ve, &rx);
    v = js_get(m, e, "rttl", &ve);
    js_num(v, ve, &ttl);
    ok = resumed && cid > 0 && cid <= 4294967295.0 && (uint32_t)cid == t->my_cid && strlen(tok) == 64 && unhex(tok, tk, 32) &&
         unhex(key, k, 32) && rx >= 0 && rx <= 9007199254740992.0 && lb_rs_ack(t, (uint64_t)rx);
    SecureZeroMemory(tk, sizeof tk);
    SecureZeroMemory(key, sizeof key);
    if (!ok) {
        SecureZeroMemory(k, sizeof k);
        SecureZeroMemory(tok, sizeof tok);
        tl_logf(t, "LOCLOBBY: Fortsetzung abgelehnt - sauberer Neuaufbau (Mitspieler werden neu verbunden)");
        /* W517: der Dienst hat einen normalen Einstieg gemacht, den wir nicht benutzen (neues E noetig) - abmelden,
         * damit er ihn nicht 120 s als abgerissene Sitzung haelt und die naechste Anmeldung ihn "ersetzt" */
        lb_send_now(t, "{\"t\":\"bye\"}");
        lb_close(t, "resume", now);
        t->lb_next_try = now + 500.0;
        return;
    }
    memcpy(t->udp_key, k, 32); /* neues Sitzungsgeheimnis fuer BIND2/KEEPALIVE2, UDP neu binden */
    SecureZeroMemory(k, sizeof k);
    strcpy_s(t->rs_tok, sizeof t->rs_tok, tok);
    SecureZeroMemory(tok, sizeof tok);
    if (ttl >= 10 && ttl <= 3600) t->rs_ttl_ms = (int)(ttl * 1000.0);
    t->bctr = 0;
    t->bound = 0;
    t->rs_susp = 0;
    t->rs_n_ok++;
    t->lb_rx_last = now; /* W517 */
    if (t->rs_hint == 1) { /* W517: Hinweis lief -> "wieder verbunden" */
        InterlockedExchange(&t->rs_hint_pub, 2);
        tl_logf(t, "LOCLOBBY: Hinweis - wieder mit dem Treffpunkt verbunden");
    }
    t->rs_hint = 0;
    t->lb_state = t->lb_joined ? LB_LOBBY : LB_HELLO;
    t->lb_ping_last = now;
    t->rs_cut_next = t->c.cut_s > 0 ? now + 1000.0 * t->c.cut_s : 0;
    if (t->rs_old_tls) { /* TEST Modus 1: der Dienst hat die alte Verbindung schon geschlossen */
        sbtls_free(t->rs_old_tls);
        t->rs_old_tls = NULL;
    }
    if (t->rs_old_s != INVALID_SOCKET) {
        closesocket(t->rs_old_s);
        t->rs_old_s = INVALID_SOCKET;
    }
    for (i = 0; i < t->rs_qn; i++) { /* was der Dienst nicht hat, in Reihenfolge nachsenden */
        int j = (t->rs_qh + i) % TUN_RS_Q;
        lb_cap(t, t->rs_q[j], t->rs_ql[j]);
        if (t->tls && t->lb_s != INVALID_SOCKET && sbtls_write(t->tls, t->rs_q[j], t->rs_ql[j]) != SBTLS_OK) {
            lb_lost(t, "tls-senden", now);
            return;
        }
        n++;
    }
    tl_logf(t, "LOCLOBBY: Sitzung c%u fortgesetzt nach %.1f s (Versuch %d, %d Zeilen nachgesendet) - Mitspieler unveraendert",
            t->my_cid, (now - t->rs_since) / 1000.0, t->rs_try + 1, n);
    lb_bind(t, now);
}
/* Trennung vom Treffpunkt: Schluessel dieser Steuerverbindung loeschen; alle Lobby-Verbindungen enden (neue
 * Steuerverbindung = neue client_id, neues E, neue Paarschluessel - SECURITY.md §4.2 Punkte 4/5). */
static void lb_close(loctun *t, const char *why, double now)
{
    int i, susp = t->rs_susp; /* W500: gescheiterte Fortsetzung = Trennung einer stehenden Anmeldung */
    nr_on_drop(t, why, now); /* W496: Trennung einer stehenden Anmeldung -> Bericht (Warteschlange) */
    if (t->rs_hint == 1) { /* W517: Hinweis lief, Fortsetzung gescheitert */
        InterlockedExchange(&t->rs_hint_pub, 3);
        tl_logf(t, "LOCLOBBY: Hinweis - Fortsetzung gescheitert, Neuaufbau");
    }
    t->rs_hint = 0;
    if (t->lb_state == LB_HELLO || t->lb_state == LB_LOBBY || susp) /* W496 Stufe 1: Grund fuer das naechste hello ("rc") */
        strcpy_s(t->lb_last_drop, sizeof t->lb_last_drop, tl_drop_code(why));
    lb_rs_reset(t);
    if (t->tls) {
        sbtls_free(t->tls);
        t->tls = NULL;
    }
    if (t->lb_s != INVALID_SOCKET) {
        WSAEventSelect(t->lb_s, NULL, 0);
        closesocket(t->lb_s);
        t->lb_s = INVALID_SOCKET;
    }
    if (t->lb_state != LB_OFF || susp) tl_logf(t, "LOCLOBBY: Treffpunkt getrennt (%s) - neuer Versuch in 5 s", why);
    /* W353: Grund fuer den Launcher - Aufbau gescheitert (Netz/Firewall/Dienst aus) bzw. Verbindung verloren */
    if (susp && strcmp(why, "upgrade_required") != 0 && strcmp(why, "e2e_required") != 0) {
        strcpy_s(t->lb_err, sizeof t->lb_err, "lost"); /* W500: Fortsetzung gescheitert */
    } else if (t->lb_state == LB_CONNECTING || t->lb_state == LB_TLS) {
        strcpy_s(t->lb_err, sizeof t->lb_err, "connect");
    } else if (t->lb_state != LB_OFF && strcmp(why, "upgrade_required") != 0 && strcmp(why, "e2e_required") != 0) {
        strcpy_s(t->lb_err, sizeof t->lb_err, "lost");
    }
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        if (!L->used || L->cid == 0) continue;
        tl_link_drop(t, L, "Treffpunkt getrennt");
        L->used = 0;
    }
    sbe2e_kp_free(&t->kp);
    SecureZeroMemory(t->my_pub, sizeof t->my_pub);
    SecureZeroMemory(t->udp_key, sizeof t->udp_key);
    t->lobby_code[0] = 0;
    lb_play_mode_set(t, 0, "Treffpunkt getrennt"); /* W440 */
    if (t->live) lv_reset(t, "Treffpunkt getrennt"); /* W482: offene Live-Anfragen scheitern, Session endet beim Dienst */
    InterlockedExchange(&t->sn_state, 0);          /* W446: neue Verbindung = neues Abonnement */
    t->sn_sub_sent = -1;
    for (i = 0; i < t->sn_out_n; i++) SecureZeroMemory(t->sn_out[i], sizeof t->sn_out[i]);
    t->sn_out_n = 0;
    if (t->pub) { /* W373: neue Steuerverbindung = neue Lobby; offene Beitritte scheitern */
        if (t->pub_joining) pub_reject(t, t->pub_join, "Treffpunkt getrennt", now);
        pub_ghosts_clear(t);
        t->pub_host = t->pub_started = t->pub_creating = 0;
        t->pub_host_vip = t->pub_host_cid = 0;
    }
    t->my_cid = 0;
    t->bctr = 0;
    t->lb_state = LB_OFF;
    t->lb_joined = 0;
    t->lb_listed = 0;
    t->bound = 0;
    t->lb_next_try = now + 5000.0;
}

/* W329: Konfigurationswerte (Raum, Pilotname) JSON-sicher in einen String einsetzen. */
void tl_js_esc(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; in && *in && o + 7 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20 || c == 0x7F) {
            o += (size_t)_snprintf_s(out + o, cap - o, _TRUNCATE, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
}

/* W329: Lobbyliste nur fuer den eigenen Raum (Server filtert mit "name"; alte Server ignorieren das Feld).
 * Ungefiltert kann die Liste aller Lobbys eine Steuerzeile sprengen - dann faende der Client seinen Raum nie. */
static void lb_list(loctun *t)
{
    char b[300], r[200];
    tl_js_esc(t->c.room, r, sizeof r);
    _snprintf_s(b, sizeof b, _TRUNCATE, "{\"t\":\"lobby_list\",\"game\":\"loc\",\"name\":\"%s\"}", r);
    lb_send(t, b);
}

/* W334: Pins fuer den Treffpunkt (SECURITY.md §3.2/§3.6). 0 = keiner -> keine Verbindung. */
static int lb_pins(loctun *t, const struct sockaddr_in *a)
{
    char ev[80];
    DWORD gl;
    int lo = (ntohl(a->sin_addr.s_addr) >> 24) == 127;
    t->npins = 0;
    memset(t->pins, 0, sizeof t->pins);
    if (_stricmp(t->lb_host, SB_STEELNET_HOST) == 0) {
        if (sbtls_parse_pin(SB_STEELNET_SPKI1, t->pins[t->npins])) t->npins++;
        if (sbtls_parse_pin(SB_STEELNET_SPKI2, t->pins[t->npins])) t->npins++;
    }
    if (t->npins < 2 && t->c.spki[0] && sbtls_parse_pin(t->c.spki, t->pins[t->npins])) t->npins++; /* loc_lobby_spki */
    gl = GetEnvironmentVariableA("SB_STEELNET_SPKI", ev, sizeof ev);
    if (lo && gl > 0 && gl < sizeof ev && t->npins < 2 && sbtls_parse_pin(ev, t->pins[t->npins])) t->npins++; /* TEST */
    return t->npins;
}

static void lb_connect(loctun *t, double now)
{
    struct sockaddr_in a;
    u_long nb = 1;
    if (!tl_resolve(t->lb_host, t->lb_port, &a)) {
        tl_logf(t, "LOCLOBBY: Treffpunkt nicht aufloesbar - neuer Versuch in 5 s");
        strcpy_s(t->lb_err, sizeof t->lb_err, "dns"); /* W353: kein Netz bzw. Name unbekannt */
        t->lb_next_try = now + 5000.0;
        return;
    }
    if (!tl_dest_ok(t, a.sin_addr.s_addr)) {
        tl_logf(t, "LOCLOBBY: Treffpunkt-Adresse nicht erlaubt");
        t->lb_next_try = now + 60000.0;
        return;
    }
    if (lb_pins(t, &a) == 0) {
        /* ohne Pin kein ungepruefter Kanal - auch nicht fuer private Server (SECURITY.md §3.6) */
        if (InterlockedExchange(&t->sec, SEC_NOPIN) != SEC_NOPIN)
            tl_logf(t, "LOCSEC: kein Server-Pin fuer den Treffpunkt (Dienst nicht eingerichtet bzw. loc_lobby_spki fehlt) - "
                       "Online-Spiel aus");
        t->lb_next_try = now + 60000.0;
        return;
    }
    t->lb_addr = a;
    t->lb_s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (t->lb_s == INVALID_SOCKET) return;
    tl_nodelay(t->lb_s);
    ioctlsocket(t->lb_s, FIONBIO, &nb);
    tl_sock_events(t, t->lb_s, FD_READ | FD_WRITE | FD_CONNECT | FD_CLOSE);
    t->lb_inlen = 0;
    t->lb_state = LB_CONNECTING;
    t->lb_next_try = now + 5000.0; /* Zeitlimit fuer den Aufbau */
    if (connect(t->lb_s, (struct sockaddr *)&a, sizeof a) != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        lb_lost(t, "connect", now); /* W500 */
    }
}

static void lb_hello(loctun *t)
{
    char b[900], ip[16], pe[120], pub[65];
    struct sockaddr_in me;
    int l = sizeof me;
    size_t bl;
    tl_js_esc(t->c.pilot, pe, sizeof pe); /* W329 */
    getsockname(t->us, (struct sockaddr *)&me, &l);
    if (me.sin_addr.s_addr == INADDR_ANY) {
        /* eigene LAN-Adresse fuer den Durchstich im selben Netz: die zum Treffpunkt benutzte */
        struct sockaddr_in la;
        int ll = sizeof la;
        if (getsockname(t->lb_s, (struct sockaddr *)&la, &ll) == 0) me.sin_addr = la.sin_addr;
    }
    tohex(t->my_pub, 32, pub);
    _snprintf_s(b, sizeof b, _TRUNCATE,
                "{\"t\":\"hello\",\"v\":2,\"client\":\"steelbound-loc\",\"pilot\":\"%s\",\"controller\":\"kbm\","
                "\"local_eps\":[[\"%s\",%u]],\"e2e\":{\"suite\":1,\"pub\":\"%s\"},\"cver\":\"%s\"}",
                pe, locnet_ip_str(me.sin_addr.s_addr, ip), t->uport, pub, tl_myver(t)); /* W420: cver */
    if (t->acct) { /* W328: Konto -> auth_challenge statt welcome */
        bl = strlen(b);
        _snprintf_s(b + bl - 1, sizeof b - bl + 1, _TRUNCATE, ",\"account\":%u,\"exp\":%lld}", t->acct, t->acct_exp);
    }
    if (t->lb_last_drop[0]) { /* W496 Stufe 1 (Betrieb, ohne Zustimmung): Code der letzten Trennung bei der Neuverbindung */
        bl = strlen(b);
        _snprintf_s(b + bl - 1, sizeof b - bl + 1, _TRUNCATE, ",\"rc\":\"%s\"}", t->lb_last_drop);
    }
    if (t->acct && t->c.resume) { /* W500: fortsetzbare Sitzung erbeten; bei laufender Fortsetzung Token + Zaehler */
        bl = strlen(b);
        if (t->rs_susp && t->rs_tok[0])
            _snprintf_s(b + bl - 1, sizeof b - bl + 1, _TRUNCATE, ",\"rs\":1,\"resume\":{\"cid\":%u,\"tok\":\"%s\",\"rx\":%llu}}",
                        t->my_cid, t->rs_tok, (unsigned long long)t->rs_rx);
        else
            _snprintf_s(b + bl - 1, sizeof b - bl + 1, _TRUNCATE, ",\"rs\":1}");
    }
    { /* W521 (#119): eigene globale IPv6-Adresse fuer den Durchstich zwischen Spielern (der Treffpunkt reicht sie nur
       * an Lobby-Partner weiter, braucht selbst kein IPv6; nie ins Log, SECURITY.md §53) */
        char f6[96];
        tl_v6_refresh(t, tl_now(t));
        if (tl_v6_hello_field(t, f6, sizeof f6)) {
            bl = strlen(b);
            _snprintf_s(b + bl - 1, sizeof b - bl + 1, _TRUNCATE, "%s}", f6);
        }
        SecureZeroMemory(f6, sizeof f6);
    }
    t->lb_state = LB_HELLO;
    t->lb_rx_last = tl_now(t); /* W517: Stille-Erkennung ab der neuen Verbindung */
    lb_send_now(t, b); /* W500: direkt, nicht ueber lb_send (Nachsende-Schlange) */
    SecureZeroMemory(b, sizeof b);
}

/* W517 HOSTRESUME: das Spiel endet geordnet (Fenster zu, Ende des Testlaufs) -> "bye" an den Treffpunkt. Der Dienst
 * beendet die Sitzung sofort (normaler Austritt, keine 120 s gehaltene Sitzung, kein "Host verloren" fuer die Lobby).
 * Danach keine Neuverbindung mehr (lb_poll). Aufruf mit t->cs bzw. nach dem Ende des Transport-Threads. */
void lb_bye(loctun *t)
{
    static const char bye[] = "{\"t\":\"bye\"}\n";
    char sink[512];
    int k;
    if (t->lb_bye) return;
    t->lb_bye = 1;
    if (t->rs_susp || !t->my_cid || (t->lb_state != LB_HELLO && t->lb_state != LB_LOBBY)) return;
    lb_cap(t, bye, (int)sizeof bye - 1); /* Selbsttest: Mitschnitt */
    if (!t->tls || t->lb_s == INVALID_SOCKET) return;
    if (sbtls_write(t->tls, bye, (int)sizeof bye - 1) != SBTLS_OK) return;
    sbtls_flush(t->tls);
    shutdown(t->lb_s, SD_SEND);
    /* ungelesene Daten verwerfen: sonst setzt closesocket ein RST, das das "bye" beim Dienst ueberholen kann */
    for (k = 0; k < 64 && recv(t->lb_s, sink, (int)sizeof sink, 0) > 0; k++) {
    }
    SecureZeroMemory(sink, sizeof sink);
    tl_logf(t, "LOCLOBBY: Abmeldung beim Treffpunkt (Spiel endet)");
}

/* W334: BIND2/KEEPALIVE2 (SECURITY.md §5.1): "SBN1" | Typ | client_id u32 | bctr u64 | HMAC(udp_key, 0..16)[:16] */
static void lb_bind(loctun *t, double now)
{
    uint8_t p[33], mac[32];
    memcpy(p, "SBN1", 4);
    p[4] = t->bound ? SN_KEEPALIVE2 : SN_BIND2;
    put32be(p + 5, t->my_cid);
    t->bctr++;
    put32be(p + 9, (uint32_t)(t->bctr >> 32));
    put32be(p + 13, (uint32_t)t->bctr);
    sbe2e_hmac(t->udp_key, 32, p, 17, mac);
    memcpy(p + 17, mac, 16);
    SecureZeroMemory(mac, sizeof mac);
    sendto(t->us, (const char *)p, 33, 0, (struct sockaddr *)&t->srv_udp, sizeof t->srv_udp); /* nie verzoegert */
    t->lb_bind_last = now;
}

static trelay *lb_relay(loctun *t, const char *name, int create)
{
    int i;
    for (i = 0; i < t->nrelays; i++) {
        if (strcmp(t->relays[i].name, name) == 0) return &t->relays[i];
    }
    if (!create || t->nrelays >= TUN_MAX_RELAYS) return NULL;
    memset(&t->relays[t->nrelays], 0, sizeof t->relays[0]);
    strncpy_s(t->relays[t->nrelays].name, sizeof t->relays[0].name, name, _TRUNCATE);
    return &t->relays[t->nrelays++];
}

void lb_relay_set(loctun *t, const char *name, const char *host, uint16_t port)
{
    trelay *r = lb_relay(t, name, 1);
    if (!r) return;
    strncpy_s(r->host, sizeof r->host, host && host[0] ? host : t->lb_host, _TRUNCATE);
    r->port = port;
    r->resolved = tl_resolve(r->host, r->port, &r->addr) && tl_dest_ok(t, r->addr.sin_addr.s_addr);
    r->pings = 0;
    r->nrtt = 0;
    r->ms = -1;
    if (!r->resolved) tl_logf(t, "LOCLOBBY: Relay %s nicht erreichbar/aufloesbar", r->name);
}

void lb_report(loctun *t)
{
    char b[2048];
    int o, i, first = 1;
    o = _snprintf_s(b, sizeof b, _TRUNCATE, "{\"t\":\"route_report\",\"relays\":[");
    for (i = 0; i < t->nrelays && o > 0; i++) {
        if (t->relays[i].ms < 0) continue;
        o += _snprintf_s(b + o, sizeof b - o, _TRUNCATE, "%s[\"%s\",%.1f]", first ? "" : ",", t->relays[i].name,
                         t->relays[i].ms);
        first = 0;
    }
    o += _snprintf_s(b + o, sizeof b - o, _TRUNCATE, "],\"peers\":[");
    first = 1;
    for (i = 0; i < TUN_MAX_LINKS && o > 0; i++) {
        tlink *L = &t->links[i];
        if (!L->used || L->cid == 0) continue;
        if (L->direct_ok && L->direct_rtt > 0) {
            o += _snprintf_s(b + o, sizeof b - o, _TRUNCATE, "%s[%u,%.1f]", first ? "" : ",", L->cid, L->direct_rtt);
        } else if (L->direct_failed) {
            o += _snprintf_s(b + o, sizeof b - o, _TRUNCATE, "%s[%u,null]", first ? "" : ",", L->cid);
        } else {
            continue;
        }
        first = 0;
        L->reported = 1;
    }
    if (o > 0) _snprintf_s(b + o, sizeof b - o, _TRUNCATE, "]}");
    lb_send(t, b);
}

static void lb_on_route(loctun *t, const char *m, const char *e)
{
    const char *v, *ve, *el, *ee;
    double d;
    char mode[16] = "";
    v = js_get(m, e, "mode", &ve);
    js_str(v, ve, mode, sizeof mode);
    v = js_get(m, e, "max_ms", &ve);
    if (js_num(v, ve, &d)) t->route_max = d;
    strcpy_s(t->route_mode, sizeof t->route_mode, mode);
    v = js_get(m, e, "relays", &ve); /* W395: waehlbare Relays (fehlt beim alten Treffpunkt = alle) */
    if (v) {
        unsigned mask = 0;
        for (el = js_arr_first(v, ve); el; el = js_arr_next(el, ve, NULL)) {
            char rn[24] = "";
            trelay *r;
            ee = js_skip(el, ve, 0);
            if (!ee || !js_str(el, ee, rn, sizeof rn)) continue;
            r = lb_relay(t, rn, 0);
            if (r) mask |= 1u << (unsigned)(r - t->relays);
        }
        t->relay_usable = mask;
        t->relay_usable_set = 1;
    }
    v = js_get(m, e, "links", &ve);
    for (el = js_arr_first(v, ve); el; el = js_arr_next(el, ve, NULL)) {
        const char *x, *xe;
        char via[12] = "", rname[24] = "", host[96] = "", tk[80] = "";
        double peer = 0, port = 0, lid = 0, ms = -1;
        tlink *L;
        ee = js_skip(el, ve, 0);
        if (!ee) break;
        x = js_get(el, ee, "peer", &xe);
        js_num(x, xe, &peer);
        x = js_get(el, ee, "via", &xe);
        js_str(x, xe, via, sizeof via);
        x = js_get(el, ee, "ms", &xe);
        js_num(x, xe, &ms);
        if (!(peer > 0 && peer <= 4294967295.0)) continue; /* W329: Bereich vor (uint32_t) */
        L = tl_link_by_cid(t, (uint32_t)peer, 0);
        if (!L) continue;
        L->route_ms = ms;
        L->route_seen = 1;
        /* W395: link_id + Ticket kommen (neuer Treffpunkt) fuer jedes Paar, auch "direct" - Grundlage fuer die
         * Bindung an allen Relays; ein alter Treffpunkt nennt sie nur bei "relay". */
        x = js_get(el, ee, "link_id", &xe);
        js_num(x, xe, &lid);
        x = js_get(el, ee, "ticket", &xe);
        js_str(x, xe, tk, sizeof tk);
        if (lid >= 1 && lid <= 4294967295.0 && tk[0]) {
            uint8_t tkb[TICKET_LEN];
            if (unhex(tk, tkb, TICKET_LEN)) {
                if (!L->have_ticket || L->relay_link != (uint32_t)lid) {
                    int ri;
                    for (ri = 0; ri < TUN_MAX_RELAYS; ri++) { /* neue link_id: alle Bindungen neu */
                        L->rb[ri].ok = 0;
                        L->rb[ri].have_cookie = 0;
                        L->rb[ri].bind_last = 0;
                    }
                }
                memcpy(L->ticket, tkb, TICKET_LEN);
                L->relay_link = (uint32_t)lid;
                L->have_ticket = 1;
                SecureZeroMemory(tkb, sizeof tkb);
            }
        }
        if (strcmp(via, "direct") == 0) {
            if (L->want != 1) tl_logf(t, "LOCROUTE: c%u -> direkt (%.1f ms, Modus %s)", L->cid, ms, mode);
            L->want = 1;
            continue;
        }
        x = js_get(el, ee, "relay", &xe);
        js_str(x, xe, rname, sizeof rname);
        x = js_get(el, ee, "host", &xe);
        js_str(x, xe, host, sizeof host);
        x = js_get(el, ee, "port", &xe);
        js_num(x, xe, &port);
        if (!(port >= 0 && port <= 65535) || !(lid >= 0 && lid <= 4294967295.0)) continue; /* W329 */
        {
            trelay *r = lb_relay(t, rname, 0);
            int idx;
            if (!r || (host[0] && strcmp(host, r->host) != 0) || (port > 0 && (uint16_t)port != r->port)) {
                lb_relay_set(t, rname, host, (uint16_t)port);
                r = lb_relay(t, rname, 0);
            }
            if (!r || !r->resolved || !L->have_ticket || L->relay_link != (uint32_t)lid) {
                tl_logf(t, "LOCROUTE: c%u -> Relay %s unbrauchbar", L->cid, rname);
                continue;
            }
            idx = (int)(r - t->relays);
            if (L->want != 2 || L->want_ri != idx) {
                tl_logf(t, "LOCROUTE: c%u -> Relay %s (%.1f ms, Modus %s)%s", L->cid, rname, ms, mode,
                        L->rt_on ? " - Vorschlag, Wahl im Client (W395)" : "");
            }
            L->want_ri = idx;
            L->want = 2;
        }
    }
}

/* W334: Verbindung auf Anfang setzen (neues Paar): alles abbauen, Eintrag mit derselben cid neu belegen. */
static tlink *tl_link_fresh(loctun *t, tlink *L)
{
    uint32_t cid = L->cid;
    if (L->up || L->e2e.valid || L->lsn != INVALID_SOCKET) {
        tl_link_drop(t, L, "neues Paar vom Treffpunkt");
    }
    L->used = 0;
    return tl_link_by_cid(t, cid, 1);
}

/* W519 STALLHOLD: {"t":"peer_conn","cid":N,"st":"lost","why":"closed|reset|timeout"} bzw. "st":"back" - der Treffpunkt
 * meldet, dass die Steuerverbindung eines Mitspielers abriss (Sitzung wird fuer die Fortsetzung gehalten) bzw. fortgesetzt
 * wurde. "closed"/"reset" heisst: der Rechner des Mitspielers hat die Verbindung selbst beendet (Spiel zu, Absturz) -
 * ein gleichzeitiger Stillstand im Spielstrom ist dann ein Abgang, kein Stau. "timeout" (Stille) bleibt ein Stau. Nur
 * fuer bestehende Verbindungen, feste Werte, sonst verworfen. */
static void lb_on_peer_conn(loctun *t, const char *m, const char *e)
{
    const char *v, *ve;
    double cid = 0;
    char st[8] = "", why[12] = "";
    tlink *L;
    v = js_get(m, e, "cid", &ve);
    if (!js_num(v, ve, &cid) || !(cid > 0 && cid <= 4294967295.0)) return;
    v = js_get(m, e, "st", &ve);
    if (!js_str(v, ve, st, sizeof st)) return;
    v = js_get(m, e, "why", &ve);
    js_str(v, ve, why, sizeof why);
    L = tl_link_by_cid(t, (uint32_t)cid, 0);
    if (!L || !L->used) return;
    if (strcmp(st, "lost") == 0 && (strcmp(why, "closed") == 0 || strcmp(why, "reset") == 0)) {
        if (!L->gone_hint) tl_logf(t, "LOCLOBBY: c%u Steuerverbindung beendet (%s) - Stillstand gilt als Abgang", L->cid, why);
        L->gone_hint = 1;
    } else if (strcmp(st, "lost") == 0 && strcmp(why, "timeout") == 0) {
        tl_logf(t, "LOCLOBBY: c%u Steuerverbindung still (timeout) - Stillstand wird gehalten", L->cid);
    } else if (strcmp(st, "back") == 0) {
        if (L->gone_hint) tl_logf(t, "LOCLOBBY: c%u Sitzung fortgesetzt - wieder verbunden", L->cid);
        L->gone_hint = 0;
    }
}

static void lb_on_peer_info(loctun *t, const char *m, const char *e)
{
    const char *v, *ve, *el, *ee, *x = NULL, *xe = NULL;   /* W332: /sdl C4703 */
    double peer = 0, acct = 0, lobby = 0, suite = 0;
    char pn_hex[40] = "", pub_hex[80] = "", pilot[24] = "";
    uint8_t pn[16], pub[32];
    tlink *L;
    struct sockaddr_in ep, cand[TUN_MAX_CAND];
    int ncand = 0, k;
    v = js_get(m, e, "peer", &ve);
    if (!js_num(v, ve, &peer) || !(peer > 0 && peer <= 4294967295.0)) return; /* W329: Bereich vor (uint32_t) */
    v = js_get(m, e, "pair_nonce", &ve);
    js_str(v, ve, pn_hex, sizeof pn_hex);
    v = js_get(m, e, "e2e", &ve);
    x = v ? js_get(v, ve, "suite", &xe) : NULL;
    js_num(x, xe, &suite);
    x = v ? js_get(v, ve, "pub", &xe) : NULL;
    js_str(x, xe, pub_hex, sizeof pub_hex);
    v = js_get(m, e, "account", &ve);
    js_num(v, ve, &acct);
    v = js_get(m, e, "lobby_id", &ve);
    js_num(v, ve, &lobby);
    v = js_get(m, e, "pilot", &ve);
    js_str(v, ve, pilot, sizeof pilot);
    if ((uint32_t)peer == t->my_cid || !unhex(pn_hex, pn, 16) || suite != 1.0 || !unhex(pub_hex, pub, 32) ||
        !(acct >= 0 && acct <= 4294967295.0) || !(lobby >= 0 && lobby <= 4294967295.0) || !pilot[0] ||
        strlen(pilot) > 16) {
        tl_logf(t, "LOCSEC: peer_info fuer c%u unvollstaendig/ungueltig - verworfen", (unsigned)peer);
        return;
    }
    /* Adressen fuer den Durchstich - W521: IPv6 zuerst (meist nur eine Firewall-Regel, kein NAT), nur mit eigenem
     * IPv6; hoechstens 2, nur global unicast (tl_v6_cand_ok) */
    if (t->have6 && !t->c.punch_legacy) {
        v = js_get(m, e, "eps6", &ve);
        for (el = js_arr_first(v, ve); el && ncand < 2; el = js_arr_next(el, ve, &ee)) {
            const char *y = js_skip(el, ve, 0);
            if (y && lb_js_ep6(t, el, y, &ep)) cand[ncand++] = ep;
        }
    }
    v = js_get(m, e, "public_ep", &ve);
    if (js_ep(v, ve, &ep) && tl_dest_ok(t, ep.sin_addr.s_addr)) cand[ncand++] = ep;
    v = js_get(m, e, "local_eps", &ve);
    for (el = js_arr_first(v, ve); el && ncand < TUN_MAX_CAND; el = js_arr_next(el, ve, &ee)) {
        const char *y = js_skip(el, ve, 0);
        int dup = 0;
        if (!y || !js_ep(el, y, &ep) || !tl_dest_ok(t, ep.sin_addr.s_addr)) continue;
        for (k = 0; k < ncand; k++) dup |= sa_eq(&cand[k], &ep);
        if (!dup) cand[ncand++] = ep;
    }
    if (t->c.cand_skew) { /* W521 TEST NAT-Nachbildung: Partner hinter symmetrischem NAT (gemeldete Zuordnung falsch) */
        for (k = 0; k < ncand; k++) {
            if (!tl_is_v6(&cand[k])) cand[k].sin_port = htons((uint16_t)(ntohs(cand[k].sin_port) + t->c.cand_skew));
        }
    }
    L = tl_link_by_cid(t, (uint32_t)peer, 1);
    if (!L) return;
    if (L->e2e.valid || L->blocked) {
        if (memcmp(L->pair_nonce, pn, 16) == 0 && memcmp(L->peer_pub, pub, 32) == 0) {
            /* gleiches Paar (z. B. neue NAT-Adresse nach BIND2 des Partners): nur Adressen auffrischen. Ein wegen
             * Verstoss gesperrter Partner bleibt gesperrt, bis der Treffpunkt ein neues Paar bildet. */
            if (!L->blocked) {
                memcpy(L->cand, cand, sizeof cand);
                L->ncand = ncand;
                memset(L->probes_tx, 0, sizeof L->probes_tx); /* W521: neue Adressen, neues Budget (Treffpunkt-Takt) */
                if (!L->direct_ok && !t->c.nodirect && !L->punching) tl_punch_start(t, L, 1, tl_now(t));
            }
            return;
        }
        if (memcmp(L->pair_nonce, pn, 16) == 0) {
            /* W333 (SECURITY.md §14, Fuer W334 Nr. 2): gleiches pair_nonce, aber anderer Schluessel -> Abbruch,
             * nie still neu ableiten (Treffpunkt fehlerhaft oder Eingriff) */
            L->blocked = 1;
            sbe2e_link_free(&L->e2e);
            tl_logf(t, "LOCSEC: peer_info fuer c%u mit gleichem pair_nonce, aber anderem Schluessel - getrennt", L->cid);
            return;
        }
        L = tl_link_fresh(t, L);
        if (!L) return;
    }
    /* Schluessel je Paar (SECURITY.md §4.3) - W464: Paarbildung in tl_link_pair (loc_tun_link.c), Meldung an den
     * Treffpunkt bleibt hier */
    k = tl_link_pair(t, L, (uint32_t)peer, (uint32_t)lobby, pn, pub, (uint32_t)acct, pilot, cand, ncand);
    if (k == TL_PAIR_KEY || k == TL_PAIR_SID) {
        char b[96];
        _snprintf_s(b, sizeof b, _TRUNCATE, "{\"t\":\"link_report\",\"peer\":%u,\"kind\":\"failed\"}", L->cid);
        lb_send(t, b);
    }
    if (k != TL_PAIR_OK) return;
    tl_logf(t, "LOCLOBBY: Mitspieler c%u (%s, %s), %d Adressen fuer den Durchstich, Ende-zu-Ende-Schluessel bereit%s",
            L->cid, L->pilot, L->account ? "Konto" : "Gast", L->ncand, t->c.nodirect ? " (direkt aus: TEST)" : "");
}

/* ====================================================================== W520 HOSTMIG (LOC_BOOT §96)
 * Host-Wechsel der steelnet-Lobby: der Dienst waehlt (SECURITY.md §49), der Client merkt sich Host und Eigenschaften
 * und reicht host_changed einmal an das Spiel weiter (Hinweis, Neuaufbau der Spiel-Session). Namen nie ins Log. */
const char *lb_hostmig_fields(const loctun *t)
{
    if (!t->c.host_mig) return "";
    return t->c.auto_host ? ",\"host_migration\":true,\"auto_host\":true" : ",\"host_migration\":true";
}

static int hm_true(const char *v, const char *ve)
{
    return v && ve - v == 4 && memcmp(v, "true", 4) == 0;
}

/* Name fuer die Anzeige: nur druckbares ASCII, gekuerzt (wie die Lobby-Liste). */
static void hm_name(const char *v, const char *ve, char *out, size_t cap)
{
    char tmp[64] = "";
    size_t i, k = 0;
    out[0] = 0;
    if (!js_str(v, ve, tmp, sizeof tmp)) return;
    for (i = 0; tmp[i] && k + 1 < cap; i++) {
        if (tmp[i] >= 0x20 && tmp[i] < 0x7F) out[k++] = tmp[i];
    }
    out[k] = 0;
}

static void hm_on_state(loctun *t, const char *lb, const char *lbe)
{
    const char *v, *ve, *el;
    double x = 0;
    v = js_get(lb, lbe, "host_migration", &ve);
    t->hm_mig = hm_true(v, ve);
    v = js_get(lb, lbe, "auto_host", &ve);
    t->hm_auto = hm_true(v, ve);
    v = js_get(lb, lbe, "suggest", &ve);
    t->hm_sugg = (js_num(v, ve, &x) && x > 0 && x <= 4294967295.0) ? (uint32_t)x : 0;
    v = js_get(lb, lbe, "members", &ve);
    t->hm_nmem = 0;
    for (el = js_arr_first(v, ve); el; el = js_arr_next(el, ve, NULL)) {
        const char *ee = js_skip(el, ve, 0), *h, *he, *c, *ce;
        if (t->hm_nmem < 64) t->hm_nmem++;
        double cid = 0;
        if (!ee) break;
        h = js_get(el, ee, "host", &he);
        c = js_get(el, ee, "client_id", &ce);
        if (hm_true(h, he) && js_num(c, ce, &cid) && cid > 0 && cid <= 4294967295.0) {
            if (t->hm_host != (uint32_t)cid) t->hm_gone_sent = 0;
            t->hm_host = (uint32_t)cid;
        }
    }
}

static void hm_on_changed(loctun *t, const char *m, const char *e)
{
    const char *v, *ve;
    double h = 0, o = 0;
    char why[12] = "";
    loctun_hostmig_ev ev;
    memset(&ev, 0, sizeof ev);
    v = js_get(m, e, "host", &ve);
    if (!js_num(v, ve, &h) || !(h > 0 && h <= 4294967295.0)) return;
    v = js_get(m, e, "old", &ve);
    if (!js_num(v, ve, &o) || !(o > 0 && o <= 4294967295.0)) o = 0;
    ev.host_cid = (uint32_t)h;
    ev.old_cid = (uint32_t)o;
    v = js_get(m, e, "pilot", &ve);
    hm_name(v, ve, ev.host, sizeof ev.host);
    v = js_get(m, e, "old_pilot", &ve);
    hm_name(v, ve, ev.old, sizeof ev.old);
    v = js_get(m, e, "reason", &ve);
    js_str(v, ve, why, sizeof why);
    ev.reason = strcmp(why, "auto") == 0 ? 3 : strcmp(why, "lost") == 0 ? 2 : 1;
    ev.me_new = ev.host_cid == t->my_cid;
    ev.me_old = ev.old_cid != 0 && ev.old_cid == t->my_cid;
    t->hm_host = ev.host_cid;
    t->hm_gone_sent = 0;
    t->hm_hand_sent = 0;
    if (t->pub) { /* W373: oeffentliche Lobby - der neue Host veroeffentlicht (lobby_update), der alte nicht mehr */
        t->pub_host = ev.me_new;
        t->pub_started = 0;
    }
    t->hm_ev = ev;
    t->hm_have = 1;
    tl_logf(t, "LOCHOSTMIG: Host der Lobby gewechselt (%s) - c%u -> c%u%s", ev.reason == 3 ? "Auto-Host" :
            ev.reason == 2 ? "Host verloren" : "Host gegangen", ev.old_cid, ev.host_cid,
            ev.me_new ? ", dieser Client ist der neue Host" : ev.me_old ? ", dieser Client gibt den Host ab" : "");
}

int loctun_hostmig_poll(loctun *t, loctun_hostmig_ev *ev)
{
    int r = 0;
    if (!t || !ev) return 0;
    EnterCriticalSection(&t->cs);
    if (t->hm_have) {
        *ev = t->hm_ev;
        t->hm_have = 0;
        r = 1;
    }
    LeaveCriticalSection(&t->cs);
    return r;
}

int loctun_hostmig_info(loctun *t, int *mig, int *autoh, int *sugg_me, int *am_host)
{
    int r;
    if (!t) return 0;
    EnterCriticalSection(&t->cs);
    r = t->lb_joined && t->lb_state == LB_LOBBY;
    if (mig) *mig = r && t->hm_mig;
    if (autoh) *autoh = r && t->hm_auto;
    if (sugg_me) *sugg_me = r && t->hm_sugg != 0 && t->hm_sugg == t->my_cid;
    if (am_host) *am_host = r && t->hm_host != 0 && t->hm_host == t->my_cid;
    LeaveCriticalSection(&t->cs);
    return r;
}

void loctun_hostmig_sugg_name(loctun *t, char *out, int cap)
{
    int i, k = 0;
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!t) return;
    EnterCriticalSection(&t->cs);
    if (t->hm_sugg && t->hm_sugg != t->my_cid) {
        tlink *L = tl_link_by_cid(t, t->hm_sugg, 0);
        if (L) {
            for (i = 0; L->pilot[i] && k + 1 < cap; i++) {
                if (L->pilot[i] >= 0x20 && L->pilot[i] < 0x7F) out[k++] = L->pilot[i];
            }
            out[k] = 0;
        }
    }
    LeaveCriticalSection(&t->cs);
}

int loctun_hostmig_members(loctun *t)
{
    int n;
    if (!t) return 0;
    EnterCriticalSection(&t->cs);
    n = (t->lb_joined && t->lb_state == LB_LOBBY) ? t->hm_nmem : 0;
    LeaveCriticalSection(&t->cs);
    return n;
}

void loctun_host_gone(loctun *t)
{
    char b[80];
    if (!t) return;
    EnterCriticalSection(&t->cs);
    if (t->lb_joined && t->lb_state == LB_LOBBY && t->hm_mig && t->hm_host && t->hm_host != t->my_cid && !t->hm_gone_sent) {
        _snprintf_s(b, sizeof b, _TRUNCATE, "{\"t\":\"host_gone\",\"host\":%u}", t->hm_host);
        lb_send(t, b);
        t->hm_gone_sent = 1;
        tl_logf(t, "LOCHOSTMIG: Spiel hat den Host c%u verloren - an den Treffpunkt gemeldet", t->hm_host);
    }
    LeaveCriticalSection(&t->cs);
}

void loctun_handover(loctun *t)
{
    if (!t) return;
    EnterCriticalSection(&t->cs);
    if (t->lb_joined && t->lb_state == LB_LOBBY && t->hm_auto && t->hm_host == t->my_cid && !t->hm_hand_sent) {
        lb_send(t, "{\"t\":\"lobby_handover\"}");
        t->hm_hand_sent = 1;
        tl_logf(t, "LOCHOSTMIG: Auto-Host - Uebergabe an den stabilsten Spieler angefragt");
    }
    LeaveCriticalSection(&t->cs);
}

static void lb_on_lobby_state(loctun *t, const char *m, const char *e)
{
    const char *lb, *lbe, *v, *ve, *el;
    double id = 0;
    int i;
    char code[24] = "";
    lb = js_get(m, e, "lobby", &lbe);
    if (!lb) return;
    v = js_get(lb, lbe, "id", &ve);
    js_num(v, ve, &id);
    if (!(id >= 0 && id <= 2147483647.0)) return;
    t->lb_lobby_id = (int)id;
    /* W334: Raumcode der Lobby (nur an Mitglieder), Anzeige im Launcher; nur Crockford-Zeichen und '-' */
    v = js_get(lb, lbe, "code", &ve);
    if (!v) v = js_get(m, e, "code", &ve);
    if (js_str(v, ve, code, sizeof code)) {
        size_t k, l = strlen(code);
        int ok = l >= 8 && l <= 9;
        for (k = 0; k < l && ok; k++) ok = (code[k] >= '0' && code[k] <= '9') || (code[k] >= 'A' && code[k] <= 'Z') || code[k] == '-';
        if (ok && strcmp(code, t->lobby_code) != 0) {
            strcpy_s(t->lobby_code, sizeof t->lobby_code, code);
            tl_logf(t, "LOCLOBBY: Raumcode der Lobby bereit (im Launcher sichtbar)");
        }
    }
    { /* W440 LOC-LOBBY-MODES: Modus der Lobby (fehlt = aelterer Dienst = Sim; nur "sim"/"arcade", sonst Sim) */
        char pm[12] = "";
        int mode = LAS_SIM;
        v = js_get(lb, lbe, "play_mode", &ve);
        if (v && js_str(v, ve, pm, sizeof pm) && las_parse(pm)) mode = las_parse(pm);
        lb_play_mode_set(t, mode, v ? (las_parse(pm) ? "lobby_state" : "lobby_state, unbekannter Wert") : "lobby_state ohne Feld");
    }
    hm_on_state(t, lb, lbe); /* W520 HOSTMIG */
    if (!t->lb_joined) {
        t->lb_joined = 1;
        t->lb_state = LB_LOBBY;
        t->lb_err[0] = 0; /* W353: in der Lobby - alter Fehlergrund gilt nicht mehr */
        t->lb_host_left = 0; /* W366 */
        tl_logf(t, "LOCLOBBY: in Lobby %d", t->lb_lobby_id);
    }
    /* Mitspieler, die gegangen sind: Verbindung beenden */
    v = js_get(lb, lbe, "members", &ve);
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        int found = 0;
        if (!L->used || L->cid == 0) continue;
        for (el = js_arr_first(v, ve); el; el = js_arr_next(el, ve, NULL)) {
            const char *ee = js_skip(el, ve, 0), *x, *xe;
            double cid = 0;
            if (!ee) break;
            x = js_get(el, ee, "client_id", &xe);
            if (js_num(x, xe, &cid) && cid > 0 && cid <= 4294967295.0 && (uint32_t)cid == L->cid) found = 1;
        }
        if (!found && !L->up) {
            tl_link_drop(t, L, "nicht mehr in der Lobby");
            L->used = 0; /* noch nicht verbunden: verwerfen */
        } else if (!found && !L->down_logged) {
            tl_logf(t, "LOCLOBBY: c%u hat die Lobby verlassen", L->cid);
            L->down_logged = 1;
        }
        if (!found && L->up) L->gone_hint = 1; /* W519: kein Halten eines Stillstands fuer Gegangene */
    }
}

/* W334: Lobby betreten - mit Raumcode beitreten, sonst eigene Code-Lobby anlegen (Standard, SECURITY.md §3.5);
 * Rolle "join" ohne Code: oeffentliche Lobby gleichen Namens suchen. */
static void lb_enter(loctun *t)
{
    char b[480], r[200];
    if (t->pub) { /* W373: oeffentlich - Lobby erst, wenn das Spiel ein Match anbietet (pub_tick) */
        t->lb_listed = 1;
        return;
    }
    if (t->c.code[0]) {
        tl_js_esc(t->c.code, r, sizeof r);
        _snprintf_s(b, sizeof b, _TRUNCATE, "{\"t\":\"lobby_join\",\"code\":\"%s\"}", r);
        lb_send(t, b);
    } else if (_stricmp(t->c.role, "join") == 0) {
        lb_list(t);
    } else {
        tl_js_esc(t->c.room, r, sizeof r); /* W329; v2: nur Anzeigename */
        _snprintf_s(b, sizeof b, _TRUNCATE,
                    "{\"t\":\"lobby_create\",\"name\":\"%s\",\"game\":\"loc\",\"mode\":\"syslink\",\"map\":\"any\","
                    "\"max_players\":10,\"rules\":{},\"visibility\":\"code\",\"play_mode\":\"%s\"%s}",
                    r, lb_my_play_mode(t), lb_hostmig_fields(t)); /* W440, W520 */
        lb_send(t, b);
    }
    t->lb_listed = 1;
}

/* W446 STEELNET-CHAT: Antworten und Nachrichten des Dienstes (Steuerkanal). Inhalte nie ins Log; der Klartext liegt nur
 * kurz in einem lokalen Puffer, der danach geloescht wird. */
static void lb_on_snchat(loctun *t, const char *typ, const char *m, const char *e)
{
    const char *v, *ve;
    loctun_snchat_fn fn = t->sn_fn;
    if (strcmp(typ, "chat_sub") == 0) {
        char code[16] = "";
        v = js_get(m, e, "code", &ve);
        js_str(v, ve, code, sizeof code);
        v = js_get(m, e, "on", &ve);
        if (v && ve - v == 4 && memcmp(v, "true", 4) == 0) {
            InterlockedExchange(&t->sn_state, 2);
            tl_logf(t, "LOCSNCHAT: steelnet-Chat abonniert");
        } else {
            InterlockedExchange(&t->sn_state, strcmp(code, "off") == 0 ? 0 : 1);
            tl_logf(t, "LOCSNCHAT: steelnet-Chat nicht abonniert%s", strcmp(code, "off") == 0 ? " (Dienst: aus)" : "");
        }
        return;
    }
    if (strcmp(typ, "chat_status") == 0) {
        char code[24] = "";
        size_t i;
        v = js_get(m, e, "code", &ve);
        js_str(v, ve, code, sizeof code);
        for (i = 0; code[i]; i++) {
            if (!((code[i] >= 'a' && code[i] <= 'z') || code[i] == '_')) return; /* nur bekannte Form */
        }
        if (code[0] && strcmp(code, "ok") != 0 && fn) fn(2, NULL, (const uint8_t *)code, (int)i);
        return;
    }
    /* "chat": nur abonniert; Absender = Pilotname der Sitzung beim Dienst (druckbares ASCII, <= 16) */
    if (t->sn_state != 2 || !fn) return;
    {
        char from[24] = "";
        uint8_t txt[LCH_MAX_TEXT_BYTES * 2];
        int n, k, hist = 0;
        const char *tv, *tve;
        v = js_get(m, e, "from", &ve);
        if (!v || *v != '"' || ve - v > 18 || memchr(v, '\\', (size_t)(ve - v)) || !js_str(v, ve, from, sizeof from) ||
            !from[0])
            return; /* Dienst-Namen sind [A-Za-z0-9 _-]: kein Escape */
        for (k = 0; from[k]; k++) {
            if ((unsigned char)from[k] < 0x20 || (unsigned char)from[k] > 0x7E || from[k] == '\\') return;
        }
        v = js_get(m, e, "hist", &ve);
        if (v && ve - v == 1 && *v == '1') hist = 1;
        tv = js_get(m, e, "text", &tve);
        n = lch_json_str_u8(tv, tve, txt, (int)sizeof txt);
        if (n > 0) fn(hist, from, txt, n);
        SecureZeroMemory(txt, sizeof txt);
    }
}

/* W446: Abonnement nach dem Schalter halten, eingereihte chat-Zeilen senden (Transport-Thread, unter cs). */
void lb_snchat_tick(loctun *t)
{
    int want = t->sn_want ? 1 : 0, i;
    if (t->sn_state == 0 || !t->my_cid) {
        for (i = 0; i < t->sn_out_n; i++) SecureZeroMemory(t->sn_out[i], sizeof t->sn_out[i]);
        t->sn_out_n = 0;
        return;
    }
    if (!want && t->sn_sub_sent < 0) t->sn_sub_sent = 0; /* nie abonniert: nichts abzubestellen */
    if (want != t->sn_sub_sent) {
        char b[64];
        _snprintf_s(b, sizeof b, _TRUNCATE, "{\"t\":\"chat_sub\",\"on\":%s,\"hist\":%s}", want ? "true" : "false",
                    want && !t->sn_hist_done ? "true" : "false");
        if (want) t->sn_hist_done = 1;
        t->sn_sub_sent = want;
        lb_send(t, b);
        if (t->sn_state == 0) return; /* Senden gescheitert -> lb_close hat alles zurueckgesetzt */
    }
    for (i = 0; i < t->sn_out_n; i++) { /* lb_close (Sendefehler) leert die Schlange: Schleife endet dann */
        if (t->sn_state == 2) lb_send(t, t->sn_out[i]);
        if (i < t->sn_out_n) SecureZeroMemory(t->sn_out[i], sizeof t->sn_out[i]);
    }
    t->sn_out_n = 0;
}

static void lb_on_line_x(loctun *t, const char *m, const char *e, double now);

/* W500: jede Zeile nach dem welcome zaehlt (wie beim Dienst); die Zahl geht als "rx" in ping und Fortsetzung. */
void lb_on_line(loctun *t, const char *m, const char *e, double now)
{
    int cnt = t->rs_count && !t->rs_susp;
    t->lb_rx_last = now; /* W517: Stille-Erkennung */
    lb_on_line_x(t, m, e, now);
    if (cnt && t->rs_count) t->rs_rx++;
}

static void lb_on_line_x(loctun *t, const char *m, const char *e, double now)
{
    const char *v, *ve;
    char typ[24] = "";
    v = js_get(m, e, "t", &ve);
    if (!js_str(v, ve, typ, sizeof typ)) return;
    if (strcmp(typ, "auth_challenge") == 0 && t->acct) { /* W328: Beweis mit dem Spielschluessel */
        char nonce[40] = "", msg[96], b[128], hex[65];
        double cid = 0;
        uint8_t mac[32];
        v = js_get(m, e, "nonce", &ve);
        js_str(v, ve, nonce, sizeof nonce);
        v = js_get(m, e, "client_id", &ve);
        js_num(v, ve, &cid);
        if (strlen(nonce) != 32 || !(cid > 0 && cid <= 4294967295.0)) {
            lb_close(t, "auth", now);
            return;
        }
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "steelnet-auth-v1|%s|%u", nonce, (unsigned)cid);
        tl_hmac(t->gkey, 32, msg, strlen(msg), mac);
        sb_sha256_hex(mac, hex);
        _snprintf_s(b, sizeof b, _TRUNCATE, "{\"t\":\"auth\",\"proof\":\"%s\"}", hex);
        lb_send_now(t, b); /* W500: auch waehrend einer Fortsetzung sofort */
        SecureZeroMemory(b, sizeof b);
        SecureZeroMemory(mac, sizeof mac);
        SecureZeroMemory(hex, sizeof hex);
        return;
    }
    if (strcmp(typ, "welcome") == 0) {
        double cid = 0, up = 0, ver = 0;
        char key[80] = "", eff[24] = "";
        const char *el;
        size_t k;
        v = js_get(m, e, "v", &ve);
        js_num(v, ve, &ver);
        if (ver != 2.0) { /* W334: nur v2 */
            lb_close(t, "welcome v != 2", now);
            return;
        }
        if (t->rs_susp) { /* W500: Antwort auf eine Fortsetzung */
            v = js_get(m, e, "resumed", &ve);
            lb_rs_on_welcome(t, m, e, v && ve - v == 4 && memcmp(v, "true", 4) == 0, now);
            return;
        }
        v = js_get(m, e, "client_id", &ve);
        js_num(v, ve, &cid);
        v = js_get(m, e, "udp_key", &ve);
        js_str(v, ve, key, sizeof key);
        v = js_get(m, e, "udp_port", &ve);
        js_num(v, ve, &up);
        v = js_get(m, e, "pilot", &ve); /* W328: wirksamer Name (Kontoname bzw. Guest-...), geht ins Transkript */
        js_str(v, ve, eff, sizeof eff);
        /* W329: Zahlen vom Netz vor der Umwandlung begrenzen (double -> uint ausserhalb des Bereichs ist UB) */
        if (!(cid > 0 && cid <= 4294967295.0) || !(up >= 1 && up <= 65535) || !unhex(key, t->udp_key, 32) || !eff[0] ||
            strlen(eff) > 16) {
            SecureZeroMemory(key, sizeof key);
            lb_close(t, "welcome", now);
            return;
        }
        SecureZeroMemory(key, sizeof key);
        for (k = 0; eff[k]; k++) {
            if ((unsigned char)eff[k] < 0x20 || (unsigned char)eff[k] > 0x7E) {
                lb_close(t, "welcome", now);
                return;
            }
        }
        strcpy_s(t->my_pilot, sizeof t->my_pilot, eff);
        { /* W487 LIVE-GAMERTAG: Gamertag des Kontos (steelcore), nur Konto-Sitzungen; ungueltig = keiner */
            char gt[24] = "";
            t->my_gamertag[0] = 0;
            v = js_get(m, e, "gamertag", &ve);
            if (t->acct && v && js_str(v, ve, gt, sizeof gt) && lb_tag_valid(gt))
                strcpy_s(t->my_gamertag, sizeof t->my_gamertag, gt);
            else if (t->acct && v)
                tl_logf(t, "LOCLOBBY: Gamertag im welcome ungueltig - ignoriert");
        }
        t->my_cid = (uint32_t)cid;
        t->bctr = 0;
        t->srv_udp = t->lb_addr;
        t->srv_udp.sin_port = htons((uint16_t)up);
        t->lb_state = LB_HELLO; /* bis zur Lobby */
        tl_logf(t, "LOCLOBBY: angemeldet als c%u", t->my_cid);
        tl_logf(t, "LOCLOBBY: Pilot \"%s\" (%s)", eff, t->acct ? "Konto" : "Gast");
        if (t->acct) InterlockedExchange(&t->auth_state, 2);
        lb_bind(t, now);
        /* Relays des Treffpunkts */
        v = js_get(m, e, "relays", &ve);
        for (el = js_arr_first(v, ve); el; el = js_arr_next(el, ve, NULL)) {
            const char *ee = js_skip(el, ve, 0), *x, *xe;
            char name[24] = "", host[96] = "";
            double port = 0;
            if (!ee) break;
            x = js_get(el, ee, "name", &xe);
            js_str(x, xe, name, sizeof name);
            x = js_get(el, ee, "host", &xe);
            js_str(x, xe, host, sizeof host);
            x = js_get(el, ee, "port", &xe);
            js_num(x, xe, &port);
            if (name[0] && port >= 1 && port <= 65535) lb_relay_set(t, name, host, (uint16_t)port);
        }
        t->relays_start = now;
        t->relays_done = 0;
        /* W446: Dienst bietet den steelnet-Chat an (Feld fehlt = aelterer Dienst) */
        v = js_get(m, e, "chat", &ve);
        InterlockedExchange(&t->sn_state, (v && ve - v == 4 && memcmp(v, "true", 4) == 0) ? 1 : 0);
        t->sn_sub_sent = -1;
        t->lb_welcome_at = now;
        { /* W500: fortsetzbare Sitzung (nur Konto, nur wenn der Dienst ein Token gibt) - ab hier zaehlen beide Seiten */
            char tok[80] = "";
            uint8_t tk[32];
            double ttl = 0;
            int held = t->rs_qn, k; /* vor dem welcome gehaltene Zeilen (Folgenummern 0..held-1) */
            t->rs_count = 0;
            t->rs_rx = t->rs_base = 0;
            SecureZeroMemory(t->rs_tok, sizeof t->rs_tok);
            if (t->rs_old_tls) {
                sbtls_free(t->rs_old_tls);
                t->rs_old_tls = NULL;
            }
            if (t->rs_old_s != INVALID_SOCKET) {
                closesocket(t->rs_old_s);
                t->rs_old_s = INVALID_SOCKET;
            }
            v = js_get(m, e, "rtok", &ve);
            if (t->acct && t->c.resume && v && js_str(v, ve, tok, sizeof tok) && strlen(tok) == 64 && unhex(tok, tk, 32)) {
                strcpy_s(t->rs_tok, sizeof t->rs_tok, tok);
                v = js_get(m, e, "rttl", &ve);
                js_num(v, ve, &ttl);
                t->rs_ttl_ms = (ttl >= 10 && ttl <= 3600) ? (int)(ttl * 1000.0) : 90000;
                t->rs_count = 1;
            }
            SecureZeroMemory(tok, sizeof tok);
            SecureZeroMemory(tk, sizeof tk);
            t->rs_cut_next = t->c.cut_s > 0 ? now + 1000.0 * t->c.cut_s : 0; /* TEST loc_lobby_cut */
            for (k = 0; k < held; k++) { /* gehaltene Zeilen jetzt senden (gezaehlt, bis zur Bestaetigung behalten) */
                int j = (t->rs_qh + k) % TUN_RS_Q;
                lb_cap(t, t->rs_q[j], t->rs_ql[j]);
                if (t->tls && t->lb_s != INVALID_SOCKET && sbtls_write(t->tls, t->rs_q[j], t->rs_ql[j]) != SBTLS_OK) {
                    lb_lost(t, "tls-senden", now);
                    return;
                }
            }
            if (!t->rs_count) { /* Dienst ohne Fortsetzung: nichts behalten */
                for (k = 0; k < TUN_RS_Q; k++)
                    if (t->rs_ql[k]) SecureZeroMemory(t->rs_q[k], (size_t)t->rs_ql[k]);
                memset(t->rs_ql, 0, sizeof t->rs_ql);
                t->rs_qn = t->rs_qh = 0;
            }
        }
        nr_on_welcome(t, m, e, now); /* W496: Dienst nimmt Netzberichte ("netlog") */
        if (t->c.join_delay_ms <= 0) lb_enter(t); /* W446 TEST: sonst spaeter aus lb_poll */
        t->lb_list_last = now;
    } else if (strcmp(typ, "pong") == 0) { /* W500: Bestaetigung der eigenen Zeilen -> Nachsende-Schlange kuerzen */
        double rx = -1;
        v = js_get(m, e, "rx", &ve);
        if (t->rs_count && js_num(v, ve, &rx) && rx >= 0 && rx <= 9007199254740992.0) lb_rs_ack(t, (uint64_t)rx);
    } else if (strcmp(typ, "chat") == 0 || strcmp(typ, "chat_sub") == 0 || strcmp(typ, "chat_status") == 0) {
        lb_on_snchat(t, typ, m, e); /* W446 */
    } else if (strcmp(typ, "lobby_list") == 0 && t->pub) { /* W373 */
        pub_on_list(t, m, e, now);
    } else if (strcmp(typ, "lobby_list") == 0 && !t->lb_joined) {
        const char *el;
        double found = 0;
        v = js_get(m, e, "lobbies", &ve);
        for (el = js_arr_first(v, ve); el; el = js_arr_next(el, ve, NULL)) {
            const char *ee = js_skip(el, ve, 0), *x, *xe;
            char name[40] = "", state[12] = "";
            double id = 0;
            if (!ee) break;
            x = js_get(el, ee, "name", &xe);
            js_str(x, xe, name, sizeof name);
            x = js_get(el, ee, "state", &xe);
            js_str(x, xe, state, sizeof state);
            x = js_get(el, ee, "id", &xe);
            js_num(x, xe, &id);
            if (strcmp(name, t->c.room) == 0 && strcmp(state, "open") == 0) found = id;
        }
        if (found > 2147483647.0) found = 0; /* W329: Bereich vor (int) */
        if (found > 0) {
            char b[96];
            _snprintf_s(b, sizeof b, _TRUNCATE, "{\"t\":\"lobby_join\",\"lobby_id\":%d}", (int)found);
            lb_send(t, b);
        }
        t->lb_listed = 0; /* ohne Treffer spaeter erneut */
    } else if (strcmp(typ, "lobby_state") == 0) {
        lb_on_lobby_state(t, m, e);
        if (t->pub) pub_on_state(t, m, e); /* W373 */
    } else if (strcmp(typ, "peer_conn") == 0) { /* W519 */
        lb_on_peer_conn(t, m, e);
    } else if (strcmp(typ, "peer_info") == 0) {
        lb_on_peer_info(t, m, e);
    } else if (strcmp(typ, "route") == 0) {
        lb_on_route(t, m, e);
    } else if (strcmp(typ, "host_changed") == 0) {
        hm_on_changed(t, m, e); /* W520 HOSTMIG */
    } else if (strcmp(typ, "lobby_handover") == 0) {
        tl_logf(t, "LOCHOSTMIG: Auto-Host - Antwort des Treffpunkts erhalten"); /* W520 */
    } else if (strcmp(typ, "lobby_closed") == 0) {
        int i;
        t->hm_host = t->hm_sugg = 0; /* W520 */
        t->hm_nmem = 0;
        t->hm_mig = t->hm_auto = 0;
        tl_logf(t, "LOCLOBBY: Lobby geschlossen (Host gegangen)");
        nr_on_host_left(t); /* W496 */
        for (i = 0; i < TUN_MAX_LINKS; i++) { /* §4.2 Punkt 5: Schluessel loeschen, Stellvertreter schliessen */
            tlink *L = &t->links[i];
            if (!L->used || L->cid == 0) continue;
            tl_link_drop(t, L, "Lobby geschlossen");
            L->used = 0;
        }
        t->lobby_code[0] = 0;
        lb_play_mode_set(t, 0, "Lobby geschlossen"); /* W440 */
        t->lb_joined = 0;
        t->lb_listed = 0;
        t->lb_list_last = now;
        if (t->lb_state == LB_LOBBY) t->lb_state = LB_HELLO;
        if (t->pub) { /* W373: oeffentlich - das Spiel meldet den Abgang selbst, zurueck zur Suche */
            t->pub_host = t->pub_started = 0;
            t->pub_host_vip = 0;
            if (t->pub_joining) pub_reject(t, t->pub_join, "Lobby geschlossen", now);
        } else {
            t->lb_host_left = 1; /* W366: Launcher meldet "Host hat die Lobby geschlossen" (nicht "Code falsch") */
            strcpy_s(t->lb_err, sizeof t->lb_err, "closed");
        }
    } else if (strncmp(typ, "live_", 5) == 0 || strcmp(typ, "campaign_ticket") == 0) {
        if (t->live) lv_on_line(t, typ, m, e, now); /* W482 LIVE-MATCH, W491 CAMP-CLIENT (Ticket) */
    } else if (strcmp(typ, "error") == 0) {
        char code[32] = "";
        if (t->live && lv_on_error(t, m, e)) return; /* W482: Antwort auf eine Live-Anfrage (ref lv<n>) */
        v = js_get(m, e, "code", &ve);
        js_str(v, ve, code, sizeof code);
        tl_logf(t, "LOCLOBBY: Fehler vom Treffpunkt: %s", code);
        if (strcmp(code, "auth_failed") == 0) { /* W328: Konto abgelehnt (abgelaufen/abgemeldet) -> als Gast weiter */
            tl_logf(t, "LOCLOBBY: Konto-Anmeldung abgelehnt - weiter als Gast (Launcher: neu anmelden)");
            t->acct = 0;
            SecureZeroMemory(t->gkey, sizeof t->gkey);
            InterlockedExchange(&t->auth_state, -1);
        }
        if (strcmp(code, "version_required") == 0) { /* W420: Treffpunkt verlangt eine neuere Client-Version */
            char msg[96] = "", nv[16] = "";
            const char *q;
            v = js_get(m, e, "msg", &ve);
            js_str(v, ve, msg, sizeof msg);
            q = strstr(msg, "client version ");
            if (q) {
                char tmp[24];
                int i;
                q += 15;
                for (i = 0; i < 23 && q[i] && q[i] != ' '; i++) tmp[i] = q[i];
                tmp[i] = 0;
                tl_ver_clean(tmp, i, nv, (int)sizeof nv);
            }
            EnterCriticalSection(&t->cs);
            strcpy_s(t->need_ver, sizeof t->need_ver, nv);
            t->peer_ver[0] = 0;
            LeaveCriticalSection(&t->cs);
            InterlockedExchange(&t->ver_state, 1);
            InterlockedExchange(&t->sec, SEC_VERSION);
            tl_logf(t, "LOCVER: Treffpunkt verlangt Client-Version %s (eigene %s) - Online-Spiel aus", nv[0] ? nv : "?",
                    SB_NET_VERSION);
            lb_close(t, code, now);
            t->lb_next_try = now + 600000.0;
            return;
        }
        if (strcmp(code, "upgrade_required") == 0 || strcmp(code, "e2e_required") == 0) {
            InterlockedExchange(&t->sec, SEC_UPGRADE);
            tl_logf(t, "LOCSEC: Treffpunkt lehnt die Protokollversion ab - Online-Spiel aus");
            lb_close(t, code, now);
            t->lb_next_try = now + 600000.0;
            return;
        }
        if (strcmp(code, "rate_limited") == 0) {
            /* W333 (SECURITY.md §14, Fuer W334 Nr. 5): zu viele falsche Raumcodes von dieser Adresse */
            tl_logf(t, "LOCLOBBY: zu viele Versuche - bitte kurz warten (neuer Versuch in 60 s)");
            strcpy_s(t->lb_err, sizeof t->lb_err, "rate"); /* W353 */
            t->lb_list_last = now + 50000.0;
        }
        if (strcmp(code, "lobby_not_found") == 0 && t->c.code[0]) {
            tl_logf(t, "LOCLOBBY: Raumcode nicht gefunden (Lobby zu oder Code falsch) - neuer Versuch in 10 s");
            strcpy_s(t->lb_err, sizeof t->lb_err, t->lb_host_left ? "closed" : "code"); /* W353, W366 */
        }
        if (strcmp(code, "lobby_full") == 0) strcpy_s(t->lb_err, sizeof t->lb_err, "full"); /* W353 */
        if (t->pub && t->pub_joining && !t->lb_joined && /* W373: Beitritt gescheitert -> Spiel verlaesst den Wartezustand */
            (strcmp(code, "lobby_full") == 0 || strcmp(code, "lobby_started") == 0 ||
             strcmp(code, "lobby_not_found") == 0 || strcmp(code, "rate_limited") == 0 ||
             strcmp(code, "already_in_lobby") == 0)) {
            pub_reject(t, t->pub_join, code, now);
        }
        if (t->pub && t->pub_creating && !t->lb_joined) t->pub_creating = 0; /* W373: Anlegen abgelehnt */
        if (!t->lb_joined) t->lb_listed = 0; /* z. B. lobby_full: spaeter erneut */
    }
}

void lb_poll(loctun *t, double now)
{
    if (!t->c.lobby[0] || t->lb_bye) return; /* W517: nach "bye" (Spiel endet) keine Neuverbindung */
    if (t->rs_susp && !t->rs_hint && now - t->rs_since >= 3000.0) { /* W517: Hinweis erst nach 3 s (kurze Luecken still) */
        t->rs_hint = 1;
        InterlockedExchange(&t->rs_hint_pub, 1);
        tl_logf(t, "LOCLOBBY: Hinweis - verbinde neu mit dem Treffpunkt (Mitspieler bleiben verbunden)");
    }
    if (t->lb_state == LB_OFF) {
        if (t->rs_susp && now >= t->rs_deadline) { /* W500: Frist vorbei -> sauberer Neuaufbau wie bisher */
            lb_close(t, "resume-frist", now);
            return;
        }
        if (now >= t->lb_next_try) lb_connect(t, now);
        return;
    }
    /* W517: stiller Abriss (WLAN-Pause, Router neu verbunden): Der Dienst antwortet auf jeden Ping (alle 15 s); kommt
     * 40 s lang keine Zeile, ist die Verbindung tot, auch wenn TCP noch nichts meldet -> fortsetzen statt warten. Nur
     * fortsetzbare Sitzungen (Gaeste/aeltere Dienste unveraendert). */
    if (t->rs_count && t->my_cid && t->lb_rx_last != 0.0 && now - t->lb_rx_last >= 40000.0 &&
        (t->lb_state == LB_HELLO || t->lb_state == LB_LOBBY) && !t->rs_susp) {
        tl_logf(t, "LOCLOBBY: seit %.0f s keine Zeile vom Treffpunkt - Verbindung gilt als abgerissen (stille)",
                (now - t->lb_rx_last) / 1000.0);
        lb_lost(t, "timeout", now);
        return;
    }
    if (t->lb_s == INVALID_SOCKET) return;
    if (t->lb_state == LB_CONNECTING) {
        fd_set w, x;
        struct timeval tv = {0, 0};
        FD_ZERO(&w);
        FD_ZERO(&x);
        FD_SET(t->lb_s, &w);
        FD_SET(t->lb_s, &x);
        if (select(0, NULL, &w, &x, &tv) > 0) {
            if (FD_ISSET(t->lb_s, &x)) {
                lb_lost(t, "connect", now);
                return;
            }
            /* W334: TLS mit SPKI-Pin; vor bestandener Pruefung geht kein Anwendungsbyte hinaus */
            if (sbtls_client_new(&t->tls, t->lb_s, t->lb_host, (const uint8_t(*)[32])t->pins, t->npins) != SBTLS_OK) {
                tl_logf(t, "LOCSEC: %s", sbtls_error(t->tls));
                lb_lost(t, "tls", now);
                return;
            }
            t->lb_state = LB_TLS;
            t->lb_next_try = now + 10000.0; /* Handshake + welcome: 10 s (SECURITY.md §3.3) */
        } else if (now >= t->lb_next_try) {
            lb_lost(t, "timeout", now);
            return;
        } else {
            return;
        }
    }
    if (t->lb_state == LB_TLS) {
        int r = sbtls_handshake(t->tls);
        if (r == SBTLS_WANT_IO) {
            if (now >= t->lb_next_try) lb_lost(t, "tls-timeout", now);
            return;
        }
        if (r != SBTLS_OK) {
            if (sbtls_pin_failed(t->tls)) { /* W500: nie fortsetzen - Sitzung endet wie bisher */
                InterlockedExchange(&t->sec, SEC_PIN);
                tl_logf(t, "LOCSEC: Server-Pruefung fehlgeschlagen (Pin passt nicht) - keine Daten gesendet");
                lb_close(t, "tls", now);
                return;
            }
            tl_logf(t, "LOCSEC: %s", sbtls_error(t->tls));
            lb_lost(t, "tls", now);
            return;
        }
        /* W500: bei einer Fortsetzung bleibt das Schluesselpaar (E geht in alle Paarschluessel ein, SECURITY.md §4.3) */
        if (!t->rs_susp && sbe2e_keypair(&t->kp, t->my_pub) != 0) {
            tl_logf(t, "LOCSEC: Schluesselpaar nicht erzeugbar - Online-Spiel aus");
            InterlockedExchange(&t->sec, SEC_SELFTEST);
            lb_close(t, "e2e", now);
            return;
        }
        InterlockedCompareExchange(&t->sec, SEC_OK, SEC_PIN);
        tl_logf(t, "LOCLOBBY: Treffpunkt verbunden (%s, Server-Pin bestaetigt)", sbtls_version(t->tls));
        lb_hello(t);
        t->lb_ping_last = now;
        if (t->lb_s == INVALID_SOCKET) return;
    }
    if (sbtls_flush(t->tls) == SBTLS_FAIL) {
        lb_lost(t, "tls-senden", now);
        return;
    }
    for (;;) {
        int r = sbtls_read(t->tls, t->lb_in + t->lb_inlen, (int)sizeof t->lb_in - 1 - t->lb_inlen);
        char *nl;
        if (r == SBTLS_CLOSED) {
            lb_lost(t, "eof", now);
            return;
        }
        if (r == SBTLS_FAIL) {
            tl_logf(t, "LOCSEC: %s", sbtls_error(t->tls));
            lb_lost(t, "tls", now);
            return;
        }
        if (r == SBTLS_WANT_IO) break;
        t->lb_inlen += r;
        t->lb_in[t->lb_inlen] = 0;
        while ((nl = (char *)memchr(t->lb_in, '\n', (size_t)t->lb_inlen)) != NULL) {
            int l = (int)(nl - t->lb_in);
            lb_on_line(t, t->lb_in, t->lb_in + l, now); /* W500: zaehlt die Zeilen nach dem welcome selbst */
            if (t->lb_s == INVALID_SOCKET) return;
            memmove(t->lb_in, nl + 1, (size_t)(t->lb_inlen - l - 1));
            t->lb_inlen -= l + 1;
            SecureZeroMemory(t->lb_in + t->lb_inlen, (size_t)l + 1); /* W446: verarbeitete Zeile (Chattext) loeschen */
        }
        if (t->lb_inlen >= (int)sizeof t->lb_in - 1) {
            lb_close(t, "zeile", now);
            return;
        }
    }
    if ((!t->my_cid || t->rs_susp) && now >= t->lb_next_try) {
        lb_lost(t, "welcome-timeout", now);
        return;
    }
    if (t->rs_susp) return; /* W500: Fortsetzung wartet auf ihr welcome */
    if (t->rs_cut_next > 0 && now >= t->rs_cut_next) { /* W500 TEST loc_lobby_cut */
        lb_cut_test(t, now);
        return;
    }
    if (now - t->lb_ping_last >= 15000.0) {
        if (t->rs_count) { /* W500: Bestaetigung der empfangenen Zeilen (der Dienst kuerzt seinen Nachsende-Ring) */
            char pb[64];
            _snprintf_s(pb, sizeof pb, _TRUNCATE, "{\"t\":\"ping\",\"rx\":%llu}", (unsigned long long)t->rs_rx);
            lb_send(t, pb);
        } else {
            lb_send(t, "{\"t\":\"ping\"}");
        }
        t->lb_ping_last = now;
        if (t->lb_s == INVALID_SOCKET) return;
    }
    if (t->my_cid && (now - t->lb_bind_last >= (t->bound ? 25000.0 : 300.0))) lb_bind(t, now);
    if (t->live_cap) pub_live_mode_tick(t, now); /* W501 ONLINE-BOTH: System Link <-> Xbox Live */
    if (t->pub && t->my_cid) pub_tick(t, now); /* W373 */
    lb_snchat_tick(t);                         /* W446 */
    if (t->lb_s == INVALID_SOCKET) return;
    if (t->my_cid && !t->pub && !t->lb_joined && t->lb_listed == 0 &&
        now - t->lb_list_last >= (t->c.code[0] ? 10000.0 : 1000.0) &&
        now - t->lb_welcome_at >= (double)t->c.join_delay_ms) { /* W446 TEST join_delay_ms (Standard 0) */
        lb_enter(t);
        t->lb_list_last = now;
    }
}

/* W395: Relay-Index einer Absenderadresse (-1 = keines). */
static int tl_relay_by_addr(loctun *t, const struct sockaddr_in *a)
{
    int i;
    for (i = 0; i < t->nrelays; i++) {
        if (t->relays[i].resolved && sa_eq(a, &t->relays[i].addr)) return i;
    }
    return -1;
}

/* W395: Soll die Verbindung an Relay ri gebunden sein? Mit eigener Wegwahl an allen Relays (dieselbe link_id, das
 * Ticket gilt auf jedem Relay mit dem gemeinsamen Schluessel), sonst nur am vom Treffpunkt gewaehlten. */
int tl_bind_want(loctun *t, const tlink *L, int ri)
{
    if (!L->used || L->blocked || !L->e2e.valid || !L->have_ticket || L->relay_link == 0) return 0;
    if (ri < 0 || ri >= t->nrelays || !t->relays[ri].resolved) return 0;
    if (L->want == 2 && L->want_ri == ri) return 1;
    if (!t->c.route_auto) return 0;
    /* Wartung (Treffpunkt nennt das Relay nicht mehr): keine neue Bindung, ein laufender Weg laeuft aus */
    return !t->relay_usable_set || (t->relay_usable & (1u << ri)) || (L->path == 2 && L->relay_idx == ri && L->rb[ri].ok);
}

/* W334: TBIND_CONFIRM mit Ticket und Rueckweg-Cookie (SECURITY.md §5.2). */
void tl_tbind_confirm(loctun *t, tlink *L, int ri)
{
    uint8_t p[5 + TICKET_LEN + 16];
    memcpy(p, "SBN1", 4);
    p[4] = SN_TBIND_CONFIRM;
    memcpy(p + 5, L->ticket, TICKET_LEN);
    memcpy(p + 5 + TICKET_LEN, L->rb[ri].cookie, 16);
    tl_sendto(t, &t->relays[ri].addr, p, sizeof p);
}

void lb_rx_sbn(loctun *t, const uint8_t *p, int len, const struct sockaddr_in *from, double now)
{
    int i;
    if (len < 5) return;
    switch (p[4]) {
    case SN_BIND_ACK:
        if (len == 11 && sa_eq(from, &t->srv_udp)) {
            if (!t->bound) tl_logf(t, "LOCLOBBY: UDP beim Treffpunkt gebunden");
            t->bound = 1;
        }
        break;
    case SN_PONG:
        if (len == 13) {
            for (i = 0; i < t->nrelays; i++) {
                trelay *r = &t->relays[i];
                int k;
                if (!sa_eq(from, &r->addr) || memcmp(r->nonce, p + 5, 7) != 0) continue;
                k = p[12];
                if (k < 5 && r->ping_at[k] > 0) {
                    if (r->nrtt < 5) r->rtt[r->nrtt++] = now - r->ping_at[k];
                    r->ping_at[k] = 0;
                }
            }
        }
        break;
    case SN_TBIND_COOKIE:
        /* Cookie kommt nur an die echte Absenderadresse (O2). Es traegt keinen Link-Bezug: fuer jede wartende
         * Bindung an diesem Relay versuchen (falsche Kombinationen verwirft das Relay). */
        if (len == 21) {
            int ri = tl_relay_by_addr(t, from);
            for (i = 0; i < TUN_MAX_LINKS && ri >= 0; i++) {
                tlink *L = &t->links[i];
                if (tl_bind_want(t, L, ri) && !L->rb[ri].ok) {
                    memcpy(L->rb[ri].cookie, p + 5, 16);
                    L->rb[ri].have_cookie = 1;
                    tl_tbind_confirm(t, L, ri);
                }
            }
        }
        break;
    case SN_TBIND_ACK:
        if (len == 9) {
            uint32_t lid = get32be(p + 5);
            int ri = tl_relay_by_addr(t, from);
            for (i = 0; i < TUN_MAX_LINKS && ri >= 0; i++) {
                tlink *L = &t->links[i];
                if (tl_bind_want(t, L, ri) && L->relay_link == lid && !L->rb[ri].ok) {
                    L->rb[ri].ok = 1;
                    L->rt_last_ok[1 + ri] = now;
                    tl_logf(t, "LOCTUN: Relay %s fuer c%u gebunden", t->relays[ri].name, L->cid);
                }
            }
        }
        break;
    case SN_RELAY_FROM:
        if (len > 9) {
            uint32_t lid = get32be(p + 5);
            int ri = tl_relay_by_addr(t, from);
            for (i = 0; i < TUN_MAX_LINKS && ri >= 0 && lid != 0; i++) {
                tlink *L = &t->links[i];
                /* W395: jedes Relay, an dem die link_id gebunden sein soll (Echtheit pruefte SBT2) */
                if (L->used && L->relay_link == lid && tl_bind_want(t, L, ri)) {
                    tl_rx_sbt(t, p + 9, len - 9, from, i, ri, now); /* Nutzlast = SBT2 (sonst verworfen) */
                    break;
                }
            }
        }
        break;
    default:
        break;
    }
}


/* W500 TEST: 32-Bit-Pruefsumme des Tokens fuer den Selbsttest (das Token selbst erscheint nie in einer Ausgabe). */
static unsigned w500_fnv(const char *s)
{
    unsigned h = 2166136261u;
    if (!s[0]) return 0;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

/* W500 TEST (loc_net_selftest): Fortsetzung ohne Netz durchspielen (loc_tun.h). */
int loctun_test_resume(loctun *t, int op, uint32_t arg, char *out, int cap)
{
    double now;
    int i, nl = 0, nv = 0;
    const tlink *L0 = NULL;
    if (!t || !out || cap < 1) return -1;
    out[0] = 0;
    EnterCriticalSection(&t->cs);
    now = tl_now(t);
    if (op != 4) {
        t->test_cap = out;
        t->test_cap_len = 0;
        t->test_cap_max = cap;
    }
    switch (op) {
    case 1:
        if (!t->kp.key && sbe2e_keypair(&t->kp, t->my_pub) != 0) break;
        t->acct = arg;
        t->acct_exp = 4102444800LL;
        break;
    case 2:
        lb_lost(t, "eof", now);
        break;
    case 3:
        lb_hello(t);
        break;
    case 4:
        for (i = 0; i < TUN_MAX_LINKS; i++) {
            if (!t->links[i].used || !t->links[i].cid) continue;
            nl++;
            nv += t->links[i].e2e.valid;
            if (!L0) L0 = &t->links[i];
        }
        _snprintf_s(out, (size_t)cap, _TRUNCATE,
                    "susp=%d st=%d cid=%u count=%d rx=%llu base=%llu qn=%d tok=%08x udp=%02x%02x%02x%02x bctr=%llu bound=%d "
                    "links=%d valid=%d sid=%08x ktx=%p pub=%02x%02x%02x%02x lobby=%d joined=%d dl=%d hint=%d relink=%d bye=%d "
                    "gh=%d",
                    t->rs_susp, t->lb_state, t->my_cid, t->rs_count, (unsigned long long)t->rs_rx,
                    (unsigned long long)t->rs_base, t->rs_qn, w500_fnv(t->rs_tok), t->udp_key[0], t->udp_key[1], t->udp_key[2],
                    t->udp_key[3], (unsigned long long)t->bctr, t->bound, nl, nv, L0 ? L0->e2e.sid : 0,
                    L0 ? L0->e2e.ktx : NULL, t->my_pub[0], t->my_pub[1], t->my_pub[2], t->my_pub[3],
                    loctun_lobby_state(t), t->lb_joined, t->rs_susp ? (int)(t->rs_deadline - t->rs_since) : 0, t->rs_hint,
                    loctun_relink_state(t), t->lb_bye, L0 ? L0->gone_hint : -1);
        break;
    case 5:
        t->rs_deadline = now - 1.0;
        lb_poll(t, now);
        break;
    case 6:
        lb_poll(t, now);
        break;
    case 7:
        lb_send(t, "{\"t\":\"x7\"}");
        break;
    case 8:
        t->c.resume = (int)arg;
        break;
    case 9: /* W517: Spiel endet geordnet */
        lb_bye(t);
        break;
    case 10: /* W517: Zeitsprung arg ms (Stille bzw. Dauer der Luecke), dann lb_poll */
        t->lb_rx_last -= (double)arg;
        t->rs_since -= (double)arg;
        lb_poll(t, now);
        break;
    case 11: /* W517: Abmeldung zuruecknehmen (naechster Selbsttest-Fall) */
        t->lb_bye = 0;
        break;
    case 12: /* W519: erste Verbindung gilt als aufgebaut mit vip arg (ohne echtes UDP), letztes Paket jetzt */
    case 13: /* W519: letztes Paket der ersten Verbindung arg ms zurueck */
        for (i = 0; i < TUN_MAX_LINKS; i++) {
            tlink *L = &t->links[i];
            if (!L->used || !L->cid) continue;
            if (op == 12) { /* arg 0 = wieder zuruecksetzen (vor dem Ende des Selbsttest-Falls) */
                L->up = arg ? 1 : 0;
                L->vip = arg;
                L->last_rx = now;
                L->last_tx = now + 3600000.0; /* keine Lebenszeichen ueber den (nicht vorhandenen) Weg */
            } else {
                L->last_rx -= (double)arg;
            }
            break;
        }
        break;
    default:
        break;
    }
    t->test_cap = NULL;
    LeaveCriticalSection(&t->cs);
    return 0;
}
