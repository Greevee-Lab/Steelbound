/* loc_tun_live.c - W482 LIVE-MATCH: Xbox-Live-Sessions ueber steelnet im Transport (nur loctun_cfg.live = 1).
 *
 * Die XOnline-HLE (loc_live_hle.c, Spiel-Thread) reiht Anfragen ein (loctun_live_send); dieser Teil sendet sie im
 * Transport-Thread ueber den TLS-Steuerkanal mit ref "lv<n>", ordnet die Antworten zu und legt sie fuer
 * loctun_live_poll ab (nie Blockade des Spiel-Threads). Suchergebnisse werden geprueft (vip wie W373) und als
 * Geister angelegt: XNetXnAddrToInAddr kennt die Host-vip dann, und die Beitrittsanfrage des Spiels (Typ 3 an die
 * vip) fuehrt ueber die W373-Mechanik zu live_join (statt lobby_join). Eigene Session: Keepalive alle 60 s
 * (live_update ohne Felder), Lobby-Start beim ersten Lockstep. Keine Attributwerte, Schluessel oder Namen im Log.
 * Protokoll: net/steelnet/PROTOKOLL.md "W482", Sicherheit: net/SECURITY.md §40. */
#include "loc_tun_int.h"

#include "loc_camp.h" /* W491 CAMP-CLIENT */

#include <errno.h>
#include <stdlib.h>
#include <time.h>

#define LV_KEEP_MS 60000.0

static void lv_slot_clear(loctun *t, int i)
{
    if (t->lv_req[i].sess) free(t->lv_req[i].sess);
    SecureZeroMemory(&t->lv_req[i], sizeof t->lv_req[i]);
}

/* ---------- W491 CAMP-CLIENT: Kampagnen-Ticket (Steuerkanal, ref "ct<n>") ----------
 * Angemeldet mit dem Spielschluessel (Challenge-Response des Treffpunkts); das Ticket bleibt hier im Transport und
 * geht nur an die Kampagnen-Bruecke (loc_camp_bridge.c), nie ins Spiel und nie ins Log. */
#define CT_TIMEOUT_MS 15000.0
#define CT_RETRY_MS 10000.0

static int ct_ref_ours(loctun *t, const char *m, const char *e)
{
    const char *v, *ve;
    char ref[24] = "";
    v = js_get(m, e, "ref", &ve);
    if (!js_str(v, ve, ref, sizeof ref) || ref[0] != 'c' || ref[1] != 't') return -1;
    return t->ct_pending && atoi(ref + 2) == t->ct_id ? 1 : 0;
}

static void ct_fail(loctun *t, const char *code, double now, double retry_ms)
{
    t->ct_pending = 0;
    t->ct_retry_at = now + retry_ms;
    strcpy_s(t->ct_err, sizeof t->ct_err, code && code[0] ? code : "error");
    if (t->ct_len <= 0 || (long long)time(NULL) >= t->ct_exp) { /* kein gueltiges Ticket mehr */
        t->ct_state = -1;
        t->ct_len = 0;
        SecureZeroMemory(t->ct_ticket, sizeof t->ct_ticket);
    }
    tl_logf(t, "LOCCAMP: Kampagnen-Ticket nicht erhalten (%s)", t->ct_err);
}

/* Antwort campaign_ticket bzw. error mit ref ct<n>: 1 = verarbeitet. */
static int ct_on_line(loctun *t, const char *typ, const char *m, const char *e, double now)
{
    const char *v, *ve;
    int own = ct_ref_ours(t, m, e);
    if (own < 0) return 0;
    if (!own) return 1; /* unsere Form, aber veraltet */
    if (strcmp(typ, "campaign_ticket") == 0) {
        char b64[420] = "";
        uint8_t tk[300];
        long long exp = 0;
        int n;
        v = js_get(m, e, "ticket", &ve);
        if (!v || ve - v >= (int)sizeof b64 || !js_str(v, ve, b64, sizeof b64)) b64[0] = 0;
        n = b64[0] ? loccamp_b64url_dec(b64, tk, sizeof tk) : -1;
        SecureZeroMemory(b64, sizeof b64);
        if (n <= 0 || !loccamp_ticket_ok(tk, n, t->acct, &exp) || exp <= (long long)time(NULL)) {
            SecureZeroMemory(tk, sizeof tk);
            ct_fail(t, "bad_ticket", now, CT_RETRY_MS);
            return 1;
        }
        memcpy(t->ct_ticket, tk, (size_t)n);
        SecureZeroMemory(tk, sizeof tk);
        t->ct_len = n;
        t->ct_exp = exp;
        t->ct_state = 1;
        t->ct_pending = 0;
        t->ct_err[0] = 0;
        tl_logf(t, "LOCCAMP: Kampagnen-Ticket erhalten (%s, gueltig noch %lld s)", t->ct_camp,
                exp - (long long)time(NULL));
        return 1;
    }
    if (strcmp(typ, "error") == 0) {
        char code[24] = "";
        double ra = 0;
        v = js_get(m, e, "code", &ve);
        js_str(v, ve, code, sizeof code);
        v = js_get(m, e, "retry_after", &ve);
        if (!v || !js_num(v, ve, &ra) || ra < 0) ra = 0;
        if (ra > 600) ra = 600;
        ct_fail(t, code, now, ra > 0 ? ra * 1000.0 : CT_RETRY_MS);
        return 1;
    }
    return 0;
}

/* Transport-Thread (unter cs): Anfrage senden, Zeitlimit, Erneuerung vor Ablauf. */
static void ct_tick(loctun *t, double now)
{
    if (!t->ct_camp[0]) return;
    if (t->ct_pending) {
        if (now - t->ct_sent_at > CT_TIMEOUT_MS) ct_fail(t, t->my_cid ? "timeout" : "offline", now, CT_RETRY_MS);
        return;
    }
    if (t->ct_state == 1 && loccamp_renew_due((long long)time(NULL), t->ct_exp, t->ct_len > 0)) t->ct_want = 1;
    if (!t->ct_want || now < t->ct_retry_at || !t->my_cid || t->acct == 0 ||
        !(t->lb_state == LB_HELLO || t->lb_state == LB_LOBBY))
        return;
    {
        char b[128];
        if (++t->ct_id <= 0) t->ct_id = 1;
        _snprintf_s(b, sizeof b, _TRUNCATE, "{\"t\":\"campaign_ticket\",\"ref\":\"ct%d\",\"campaign\":\"%s\"}", t->ct_id,
                    t->ct_camp);
        t->ct_want = 0;
        t->ct_pending = 1;
        t->ct_sent_at = now;
        lb_send(t, b);
    }
}

void loctun_camp_want(loctun *t, const char *campaign, int force)
{
    size_t i, l;
    if (!t || !campaign) return;
    l = strlen(campaign);
    if (l == 0 || l >= sizeof t->ct_camp) return;
    for (i = 0; i < l; i++) { /* Alphabet wie steelnet: [a-z0-9.-] */
        char c = campaign[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return;
    }
    EnterCriticalSection(&t->cs);
    if (strcmp(t->ct_camp, campaign) != 0 || force) {
        if (force && t->ct_len > 0) tl_logf(t, "LOCCAMP: Kampagnen-Ticket verworfen (neues angefordert)");
        t->ct_len = 0;
        t->ct_exp = 0;
        t->ct_state = 0;
        SecureZeroMemory(t->ct_ticket, sizeof t->ct_ticket);
        if (force) t->ct_retry_at = 0;
    }
    strcpy_s(t->ct_camp, sizeof t->ct_camp, campaign);
    if (t->ct_len <= 0 && !t->ct_pending) {
        t->ct_want = 1;
        if (t->ct_state < 0) t->ct_state = 0;
    }
    LeaveCriticalSection(&t->cs);
    SetEvent(t->ev_app);
}

int loctun_camp_ticket(loctun *t, uint8_t *out, int cap, long long *exp, char *err, int errcap)
{
    int r;
    if (exp) *exp = 0;
    if (err && errcap > 0) err[0] = 0;
    if (!t) return -1;
    EnterCriticalSection(&t->cs);
    if (t->ct_len > 0 && (long long)time(NULL) < t->ct_exp) {
        r = t->ct_len <= cap ? t->ct_len : -1;
        if (r > 0 && out) memcpy(out, t->ct_ticket, (size_t)r);
        if (exp) *exp = t->ct_exp;
    } else if (t->ct_state < 0) {
        r = -1;
        if (err && errcap > 0) strcpy_s(err, (size_t)errcap, t->ct_err);
    } else {
        r = 0;
        if (t->ct_camp[0] && !t->ct_pending) t->ct_want = 1; /* abgelaufen: neues holen */
    }
    LeaveCriticalSection(&t->cs);
    return r;
}

/* Alle offenen Anfragen scheitern (Trennung vom Treffpunkt), eigene Session vergessen. */
void lv_reset(loctun *t, const char *why)
{
    int i, n = 0;
    for (i = 0; i < TUN_LIVE_REQ; i++) {
        if (t->lv_req[i].id && t->lv_req[i].state == 0) {
            t->lv_req[i].state = -1;
            strcpy_s(t->lv_req[i].err, sizeof t->lv_req[i].err, "offline");
            t->lv_req[i].queued = 0;
            n++;
        }
    }
    if (t->lv_own || n) tl_logf(t, "LOCLIVE: Live-Sessions zurueckgesetzt (%s, %d Anfragen offen)", why, n);
    if (t->ct_pending) ct_fail(t, "offline", tl_now(t), CT_RETRY_MS); /* W491: Ticket spaeter neu (gueltiges bleibt) */
    t->lv_own = 0;
    SecureZeroMemory(t->lv_own_sid, sizeof t->lv_own_sid);
}

static int lv_slot_of_ref(loctun *t, const char *m, const char *e)
{
    const char *v, *ve;
    char ref[24] = "";
    int id, i;
    v = js_get(m, e, "ref", &ve);
    if (!js_str(v, ve, ref, sizeof ref) || ref[0] != 'l' || ref[1] != 'v') return -1;
    id = atoi(ref + 2);
    for (i = 0; i < TUN_LIVE_REQ && id > 0; i++) {
        if (t->lv_req[i].id == id && t->lv_req[i].state == 0) return i;
    }
    return -2; /* unsere Form, aber unbekannt/abgelaufen */
}

/* Ganzzahl (int64) aus dem rohen JSON-Text (js_num ist double: verliert Stellen). */
static int lv_int(const char *v, const char *ve, int64_t lo, int64_t hi, int64_t *out)
{
    char b[24];
    size_t l, k;
    char *end = NULL;
    long long x;
    if (!v || v >= ve) return 0;
    l = (size_t)(ve - v);
    if (l == 0 || l >= sizeof b) return 0;
    memcpy(b, v, l);
    b[l] = 0;
    for (k = (b[0] == '-'); k < l; k++) {
        if (b[k] < '0' || b[k] > '9') return 0;
    }
    if (l == (size_t)(b[0] == '-')) return 0;
    errno = 0;
    x = _strtoi64(b, &end, 10);
    if (errno || !end || *end || x < lo || x > hi) return 0;
    *out = x;
    return 1;
}

/* JSON-String -> WCHAR (BMP, ohne Surrogate/Steuerzeichen, <= 15 Zeichen; wie steelnet live.py). 1 = gueltig. */
static int lv_wstr(const char *v, const char *ve, uint16_t *w, int cap)
{
    int n = 0;
    const unsigned char *p = (const unsigned char *)v, *e = (const unsigned char *)ve;
    if (!v || v >= ve || *p != '"' || e - p < 2 || e[-1] != '"') return 0;
    memset(w, 0, (size_t)cap * 2);
    for (p++, e--; p < e;) {
        unsigned c = *p++;
        if (c == '\\') {
            if (p >= e) return 0;
            c = *p++;
            if (c == 'u') {
                unsigned k, x = 0;
                if (e - p < 4) return 0;
                for (k = 0; k < 4; k++) {
                    unsigned d = p[k];
                    x <<= 4;
                    if (d >= '0' && d <= '9') x |= d - '0';
                    else if (d >= 'a' && d <= 'f') x |= d - 'a' + 10;
                    else if (d >= 'A' && d <= 'F') x |= d - 'A' + 10;
                    else return 0;
                }
                p += 4;
                c = x;
            } else if (c == 'n' || c == 'r' || c == 't' || c == 'b' || c == 'f') {
                return 0; /* Steuerzeichen */
            } else if (c != '"' && c != '\\' && c != '/') {
                return 0;
            }
        } else if (c >= 0x80) { /* UTF-8 (BMP) */
            if ((c & 0xE0) == 0xC0 && p < e && (p[0] & 0xC0) == 0x80) {
                c = ((c & 0x1F) << 6) | (p[0] & 0x3F);
                p += 1;
                if (c < 0x80) return 0;
            } else if ((c & 0xF0) == 0xE0 && e - p >= 2 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) {
                c = ((c & 0x0F) << 12) | ((unsigned)(p[0] & 0x3F) << 6) | (p[1] & 0x3F);
                p += 2;
                if (c < 0x800) return 0;
            } else {
                return 0;
            }
        }
        if (c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0) || (c >= 0xD800 && c <= 0xDFFF) || n >= cap - 1)
            return 0;
        w[n++] = (uint16_t)c;
    }
    return 1;
}

/* vip eines gelisteten Hosts pruefen (wie pub_on_list): 127/8, nicht .0/.255, nicht eigene/verbundene/andere Geister. */
static int lv_vip_ok(loctun *t, uint32_t vip, int lid)
{
    uint32_t h = ntohl(vip);
    int j, clash = (h >> 24) != 127u || h == 0x7F000001u || (h & 0xFFu) == 0u || (h & 0xFFu) == 255u ||
                   (vip & 0xFFFFFF00u) == 0 || ((vip ^ t->n->self_ip) & htonl(0x00FFFFFFu)) == 0;
    for (j = 0; j < TUN_MAX_LINKS && !clash; j++)
        clash = t->links[j].used && t->links[j].vip == vip;
    for (j = 0; j < TUN_MAX_GHOST && !clash; j++)
        clash = t->gh[j].used && t->gh[j].lid != lid && ((t->gh[j].vip ^ vip) & htonl(0x00FFFFFFu)) == 0;
    return !clash;
}

/* Ein Eintrag aus live_results. 1 = gueltig (out gefuellt). */
static int lv_parse_sess(loctun *t, const char *el, const char *ee, loclive_sess *out)
{
    const char *x, *xe, *a, *ae, *it;
    char sid[24] = "";
    int64_t lid = 0, vip = 0, sl;
    int k = 0;
    memset(out, 0, sizeof *out);
    x = js_get(el, ee, "sid", &xe);
    if (!js_str(x, xe, sid, sizeof sid) || strlen(sid) != 16 || !unhex(sid, out->sid, 8)) return 0;
    x = js_get(el, ee, "lobby_id", &xe);
    if (!lv_int(x, xe, 1, 2147483647, &lid)) return 0;
    x = js_get(el, ee, "vip", &xe);
    if (!lv_int(x, xe, 1, 4294967295LL, &vip)) return 0;
    out->lobby_id = (uint32_t)lid;
    out->vip = htonl((uint32_t)vip);
    if (!lv_vip_ok(t, out->vip, (int)lid)) return 0;
    x = js_get(el, ee, "slots", &xe);
    for (it = js_arr_first(x, xe); it && k < 4; k++) {
        const char *ie = js_skip(it, xe, 0);
        if (!ie || !lv_int(it, ie, 0, 10, &sl)) return 0;
        out->slots[k] = (uint32_t)sl;
        it = js_arr_next(it, xe, NULL);
    }
    if (k != 4) return 0;
    a = js_get(el, ee, "attrs", &ae);
    if (!a || *a != '[') return 0;
    for (it = js_arr_first(a, ae); it; it = js_arr_next(it, ae, NULL)) {
        const char *ie = js_skip(it, ae, 0), *p0, *p0e, *p1, *p1e;
        int64_t id;
        loclive_attr *at;
        if (!ie || *it != '[' || out->nattr >= LOCLIVE_MAX_ATTR) return 0;
        p0 = js_arr_first(it, ie);
        if (!p0) return 0;
        p0e = js_skip(p0, ie, 0);
        p1 = js_arr_next(p0, ie, NULL);
        if (!p0e || !p1) return 0;
        p1e = js_skip(p1, ie, 0);
        if (!p1e || js_arr_next(p1, ie, NULL)) return 0; /* genau zwei Elemente */
        if (!lv_int(p0, p0e, 0, 4294967295LL, &id)) return 0;
        at = &out->attr[out->nattr];
        at->id = (uint32_t)id;
        if ((at->id & LOCLIVE_ATTR_TYPEMASK) == LOCLIVE_ATTR_STRING) {
            at->is_str = 1;
            if (!lv_wstr(p1, p1e, at->w, LOCLIVE_STR_MAX + 1)) return 0;
        } else if ((at->id & LOCLIVE_ATTR_TYPEMASK) == 0) {
            if (!lv_int(p1, p1e, INT64_MIN, INT64_MAX, &at->v)) return 0;
        } else {
            return 0;
        }
        out->nattr++;
    }
    return 1;
}

/* Geist fuer einen gefundenen Host (Beitritt per live_join). */
static void lv_ghost(loctun *t, const loclive_sess *s, double now)
{
    tghost *g = NULL;
    int j;
    for (j = 0; j < TUN_MAX_GHOST; j++) {
        if (t->gh[j].used && t->gh[j].lid == (int)s->lobby_id) g = &t->gh[j];
    }
    if (g && g->vip != s->vip) {
        if (t->pub_join == (int)(g - t->gh)) return;
        pub_ghosts_close_one(g);
        g = NULL;
    }
    if (!g) {
        for (j = 0; j < TUN_MAX_GHOST && !g; j++) {
            if (!t->gh[j].used) g = &t->gh[j];
        }
        if (!g) return;
        memset(g, 0, sizeof *g);
        g->used = 1;
        g->lid = (int)s->lobby_id;
        g->vip = s->vip;
        g->s[0] = g->s[1] = INVALID_SOCKET;
        g->est = -1;
        g->mode = LAS_SIM;
        g->ans = t->pub_seq; /* beantwortet keine System-Link-Suche */
        strcpy_s(g->name, sizeof g->name, "Live");
    }
    g->live = 1;
    memcpy(g->live_sid, s->sid, 8);
    g->seen = now;
}

static void lv_on_results(loctun *t, int si, const char *m, const char *e, double now)
{
    const char *v, *ve, *el;
    int n = 0, bad = 0, i, proc_find;
    double proc = 0;
    loclive_sess *ss = (loclive_sess *)calloc(LOCLIVE_MAX_RESULTS, sizeof(loclive_sess));
    v = js_get(m, e, "proc", &ve);
    js_num(v, ve, &proc);
    proc_find = proc == 0;
    if (!ss) {
        t->lv_req[si].state = -1;
        strcpy_s(t->lv_req[si].err, sizeof t->lv_req[si].err, "memory");
        return;
    }
    v = js_get(m, e, "sessions", &ve);
    for (el = js_arr_first(v, ve); el && n < LOCLIVE_MAX_RESULTS; el = js_arr_next(el, ve, NULL)) {
        const char *ee = js_skip(el, ve, 0);
        if (!ee) break;
        if (lv_parse_sess(t, el, ee, &ss[n])) n++;
        else bad++;
    }
    if (!proc_find) { /* neue Suche: Geister frueherer Ergebnisse, die nicht mehr gelistet sind, verwerfen */
        for (i = 0; i < TUN_MAX_GHOST; i++) {
            int keep = 0, k;
            if (!t->gh[i].used || !t->gh[i].live || i == t->pub_join) continue;
            for (k = 0; k < n; k++) keep |= t->gh[i].lid == (int)ss[k].lobby_id;
            if (!keep) pub_ghosts_close_one(&t->gh[i]);
        }
    }
    for (i = 0; i < n; i++) lv_ghost(t, &ss[i], now);
    t->lv_req[si].sess = ss;
    t->lv_req[si].nsess = n;
    t->lv_req[si].state = 1;
    tl_logf(t, "LOCLIVE: %s -> %d Sessions%s", proc_find ? "Find" : "Suche", n, bad ? " (ungueltige verworfen)" : "");
}

/* Antworten des Dienstes auf live_* (Typ beginnt mit "live_"). */
void lv_on_line(loctun *t, const char *typ, const char *m, const char *e, double now)
{
    const char *v, *ve;
    int si;
    if (ct_on_line(t, typ, m, e, now)) return; /* W491 CAMP-CLIENT */
    si = lv_slot_of_ref(t, m, e);
    if (strcmp(typ, "live_deleted") == 0) { /* Antwort oder Ablauf (ohne ref) */
        char sid[24] = "";
        uint8_t b[8];
        v = js_get(m, e, "sid", &ve);
        if (js_str(v, ve, sid, sizeof sid) && unhex(sid, b, 8) && t->lv_own && memcmp(b, t->lv_own_sid, 8) == 0) {
            t->lv_own = 0;
            SecureZeroMemory(t->lv_own_sid, sizeof t->lv_own_sid);
        }
        v = js_get(m, e, "left", &ve);
        if (v && ve - v == 4 && memcmp(v, "true", 4) == 0 && t->lb_joined) { /* allein: Lobby ist zu */
            t->lb_joined = 0;
            t->lb_listed = 1;
            if (t->lb_state == LB_LOBBY) t->lb_state = LB_HELLO;
            t->pub_host = t->pub_started = 0;
            lb_play_mode_set(t, 0, "Live-Session geloescht");
        }
        if (si >= 0) t->lv_req[si].state = 1;
        if (si < 0) tl_logf(t, "LOCLIVE: eigene Session vom Dienst entfernt (abgelaufen)");
        return;
    }
    if (si < 0) return; /* keine offene Anfrage dazu */
    if (strcmp(typ, "live_session") == 0) {
        char sid[24] = "", key[40] = "";
        v = js_get(m, e, "sid", &ve);
        js_str(v, ve, sid, sizeof sid);
        v = js_get(m, e, "key", &ve);
        js_str(v, ve, key, sizeof key);
        if (strlen(sid) != 16 || strlen(key) != 32 || !unhex(sid, t->lv_req[si].sid, 8) ||
            !unhex(key, t->lv_req[si].key, 16)) {
            t->lv_req[si].state = -1;
            strcpy_s(t->lv_req[si].err, sizeof t->lv_req[si].err, "bad_answer");
        } else {
            memcpy(t->lv_own_sid, t->lv_req[si].sid, 8);
            t->lv_own = 1;
            t->lv_keep_at = now;
            t->pub_host = 1;
            t->pub_started = 0;
            t->lv_req[si].state = 1;
            tl_logf(t, "LOCLIVE: Session angelegt (Lobby %d, Schluessel vom Dienst)", t->lb_lobby_id);
        }
        SecureZeroMemory(key, sizeof key);
    } else if (strcmp(typ, "live_ok") == 0) {
        t->lv_req[si].state = 1;
    } else if (strcmp(typ, "live_results") == 0) {
        lv_on_results(t, si, m, e, now);
    }
}

/* error mit ref "lv<n>": 1 = verarbeitet (nicht weiter als Lobby-Fehler deuten). */
int lv_on_error(loctun *t, const char *m, const char *e)
{
    const char *v, *ve;
    char code[24] = "";
    int si;
    if (ct_on_line(t, "error", m, e, tl_now(t))) return 1; /* W491 CAMP-CLIENT */
    si = lv_slot_of_ref(t, m, e);
    if (si == -1) return 0;
    if (si >= 0) {
        v = js_get(m, e, "code", &ve);
        js_str(v, ve, code, sizeof code);
        t->lv_req[si].state = -1;
        strcpy_s(t->lv_req[si].err, sizeof t->lv_req[si].err, code[0] ? code : "error");
        tl_logf(t, "LOCLIVE: Dienst lehnt Anfrage ab: %s", t->lv_req[si].err);
    }
    return 1;
}

/* Transport-Thread (unter cs, mit Treffpunkt): eingereihte Anfragen senden, Zeitlimits, Keepalive, Lobby-Start. */
void lv_tick(loctun *t, double now)
{
    int i;
    if (!t->live) return;
    ct_tick(t, now); /* W491 CAMP-CLIENT */
    if (!t->live) return;
    for (i = 0; i < TUN_LIVE_REQ; i++) {
        lvreq *r = &t->lv_req[i];
        if (!r->id || r->state != 0) continue;
        if (r->queued && t->my_cid && (t->lb_state == LB_HELLO || t->lb_state == LB_LOBBY)) {
            char b[TUN_LIVE_JSON + 64];
            const char *rest = r->json + 1; /* hinter '{' */
            if (r->with_vip)
                _snprintf_s(b, sizeof b, _TRUNCATE, "{\"ref\":\"lv%d\",\"vip\":%u,%s", r->id,
                            (unsigned)ntohl(t->n->self_ip), rest);
            else
                _snprintf_s(b, sizeof b, _TRUNCATE, "{\"ref\":\"lv%d\",%s", r->id, rest);
            r->queued = 0;
            r->sent_at = now;
            lb_send(t, b);
            SecureZeroMemory(b, sizeof b);
            if (!t->live) return;
        } else if (now - (r->queued ? r->q_at : r->sent_at) > LOCLIVE_REQ_TIMEOUT_MS) {
            r->state = -1;
            r->queued = 0;
            strcpy_s(r->err, sizeof r->err, t->my_cid ? "timeout" : "offline");
            tl_logf(t, "LOCLIVE: Anfrage ohne Antwort (%s)", r->err);
        }
    }
    if (t->lv_own && (!t->pub_host || !t->lb_joined)) { /* eigene Lobby verlassen: Session ist beim Dienst weg */
        t->lv_own = 0;
        SecureZeroMemory(t->lv_own_sid, sizeof t->lv_own_sid);
    }
    if (t->lv_own && t->my_cid && now - t->lv_keep_at >= LV_KEEP_MS) {
        char b[96], sid[17];
        tohex(t->lv_own_sid, 8, sid);
        _snprintf_s(b, sizeof b, _TRUNCATE, "{\"t\":\"live_update\",\"lv\":1,\"sid\":\"%s\"}", sid);
        t->lv_keep_at = now;
        lb_send(t, b);
    }
    if (t->pub_host && t->lb_joined && !t->pub_started) {
        LONG l = t->n->st.last34_ms;
        if (l && (LONG)(GetTickCount() - (DWORD)l) < 2000) { /* Mission laeuft: nicht mehr beitretbar */
            lb_send(t, "{\"t\":\"lobby_start\"}");
            t->pub_started = 1;
            tl_logf(t, "LOCLIVE: Match beginnt - Lobby %d nicht mehr beitretbar", t->lb_lobby_id);
        }
    }
}

/* ---------- API (Spiel-Thread) ---------- */
void loctun_live_mode(loctun *t, int on) /* W501 ONLINE-BOTH */
{
    if (!t || !t->live_cap) return;
    if (InterlockedExchange(&t->live_want, on ? 1 : 0) != (on ? 1 : 0)) SetEvent(t->ev_app);
}

int loctun_live_send(loctun *t, const char *json, int with_vip)
{
    int i, id = 0;
    size_t l;
    if (!t || !json || json[0] != '{' || !t->live) return 0;
    l = strlen(json);
    if (l < 2 || l >= TUN_LIVE_JSON) return 0;
    EnterCriticalSection(&t->cs);
    if (t->my_cid) {
        for (i = 0; i < TUN_LIVE_REQ; i++) {
            if (!t->lv_req[i].id) {
                lvreq *r = &t->lv_req[i];
                if (++t->lv_next <= 0) t->lv_next = 1;
                r->id = id = t->lv_next;
                memcpy(r->json, json, l + 1);
                r->with_vip = with_vip ? 1 : 0;
                r->queued = 1;
                r->q_at = tl_now(t);
                break;
            }
        }
    }
    LeaveCriticalSection(&t->cs);
    if (id) SetEvent(t->ev_app);
    return id;
}

int loctun_live_poll(loctun *t, int id, char *err, int errcap, uint8_t sid[8], uint8_t key[16], loclive_sess *sess,
                     int cap, int *nsess)
{
    int i, r = -2;
    if (nsess) *nsess = 0;
    if (err && errcap > 0) err[0] = 0;
    if (!t || id <= 0) return -2;
    EnterCriticalSection(&t->cs);
    for (i = 0; i < TUN_LIVE_REQ; i++) {
        lvreq *q = &t->lv_req[i];
        if (q->id != id) continue;
        r = q->state;
        if (r == 0) break;
        if (err && errcap > 0) strcpy_s(err, (size_t)errcap, q->err);
        if (sid) memcpy(sid, q->sid, 8);
        if (key) memcpy(key, q->key, 16);
        if (q->sess && sess && cap > 0 && nsess) {
            int n = q->nsess < cap ? q->nsess : cap;
            memcpy(sess, q->sess, (size_t)n * sizeof *sess);
            *nsess = n;
        }
        lv_slot_clear(t, i);
        break;
    }
    LeaveCriticalSection(&t->cs);
    return r;
}

void loctun_xnaddr(loctun *t, uint32_t vip, uint8_t out[36])
{
    if (!t || !t->n) {
        memset(out, 0, 36);
        return;
    }
    locnet_xnaddr(t->n, vip, out);
}

/* ---------- W485 LIVE-QOS: Messwerte fuer XNetQosLookup im Live-Modus ---------- */
/* RTT zum Dienst: Relay des Treffpunkts (gleiche UDP-Adresse), sonst das schnellste gemessene Relay; ms < 0 = nichts. */
static double lv_service_ms(loctun *t)
{
    double best = -1;
    int i;
    for (i = 0; i < t->nrelays; i++) {
        const trelay *r = &t->relays[i];
        if (!t->relays_done || r->ms <= 0 || !r->resolved) continue;
        if (r->addr.sin_addr.s_addr == t->srv_udp.sin_addr.s_addr && r->addr.sin_port == t->srv_udp.sin_port)
            return r->ms;
        if (best < 0 || r->ms < best) best = r->ms;
    }
    return best;
}

int loctun_qos_service(loctun *t, loclive_qos_val *v)
{
    double ms;
    if (!t || !v) return 0;
    EnterCriticalSection(&t->cs);
    ms = lv_service_ms(t);
    LeaveCriticalSection(&t->cs);
    v->src = LOCLIVE_QOS_SRC_SERVICE;
    if (ms <= 0) {
        v->src = LOCLIVE_QOS_SRC_NONE;
        v->rtt_min = v->rtt_med = LOCLIVE_QOS_RTT_DEFAULT;
        return 0;
    }
    v->rtt_min = v->rtt_med = (int)(ms + 0.5);
    return 1;
}

int loctun_qos_target(loctun *t, uint32_t vip, loclive_qos_val *v)
{
    int i, r = 0;
    if (!t || !v) return 0;
    v->src = LOCLIVE_QOS_SRC_NONE;
    v->rtt_min = v->rtt_med = LOCLIVE_QOS_RTT_DEFAULT;
    if (!vip) return 0;
    EnterCriticalSection(&t->cs);
    for (i = 0; i < TUN_MAX_LINKS && !r; i++) { /* verbundener Mitspieler: gleiche Formel wie tl_qos_update */
        const tlink *L = &t->links[i];
        if (!L->used || L->vip != vip || !L->up || L->rtt_n == 0) continue;
        v->src = L->path == 2 ? LOCLIVE_QOS_SRC_RELAY : LOCLIVE_QOS_SRC_DIRECT;
        v->rtt_med = (int)(L->srtt + 4.0 * L->rttvar + (double)t->c.qos_reserve_ms + 0.5);
        v->rtt_min = (int)((L->rtt_min > 0 ? L->rtt_min : L->srtt) + 0.5);
        r = 1;
    }
    for (i = 0; i < TUN_MAX_GHOST && !r; i++) { /* gefundener Host ohne Verbindung: Schaetzung */
        const tghost *g = &t->gh[i];
        double est;
        if (!g->used || g->vip != vip) continue;
        est = g->est;
        if (est < 0) { /* Live-Host: Weg ueber das Relay des Treffpunkts ~ 2 x eigene RTT zum Dienst */
            double s = lv_service_ms(t);
            est = s > 0 ? 2.0 * s : -1;
        }
        if (est > 0) {
            v->src = LOCLIVE_QOS_SRC_EST;
            v->rtt_min = (int)(est + 0.5);
            v->rtt_med = (int)(est + (double)t->c.qos_reserve_ms + 0.5);
            r = 2;
        }
    }
    LeaveCriticalSection(&t->cs);
    return r;
}
