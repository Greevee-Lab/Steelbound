/* loc_tun_pub.c - W464 NET-CORE: aus loc_tun.c verschoben (W373 LOC-PUBLIC-LOBBY u. a.), unveraendert.
 * Oeffentliche Lobbys: Geister (lokale Vertreter fremder Hosts) mit gepruefter Host-Info, Liste/Beitritt ueber
 * lobby_id, Veroeffentlichen und Lebenszyklus der eigenen Lobby. */
#include "loc_tun_int.h"

/* ====================================================================== W373 LOC-PUBLIC-LOBBY
 * Oeffentliche Lobbys (loc_lobby_vis=public, ohne Raumcode; LOC_BOOT §49, net/SECURITY.md §23):
 *  - Host: Sobald das Spiel ein Match anbietet (Lobby-Zustand G+0x15 = 0x22/0x23, "wartet auf Beitritte"), legt der
 *    Transport eine oeffentliche Lobby an. rules = eigene vip + Sitze/Lager/Mission/Zaehler aus dem Spielspeicher
 *    (nur Werte, geprueft wie beim Empfaenger) + gerundete Relay-Pings (Ping-Schaetzung). Aenderungen -> lobby_update,
 *    Missionsbeginn -> lobby_start (unsichtbar), Host verlaesst das Match ohne Mitspieler -> lobby_leave.
 *  - Suchender: Die Suche des Spiels (Typ 1, einmal je Suche, Antwortfenster ~3 s) holt die Liste offener Lobbys;
 *    je Lobby beantwortet ein "Geist" (Sockets auf vip:Stellvertreter+201/+202) die Suche lokal mit einer Host-Info
 *    (Typ 2, Felder nur aus geprueften Zahlen gebaut, Gastfilter vor dem Senden). Waehlt das Spiel den Eintrag
 *    (Typ 3 an die vip), tritt der Transport der Lobby bei; der Client wartet ohne Antwort beliebig lange (B), die
 *    Beitrittsanfrage geht ueber die Verbindung, sobald sie steht. Fehlschlag -> Typ 7 (6 B) vom Geist.
 * Keine Adressen in Liste und Log; Namen nur druckbares ASCII, gekuerzt. */
static int pub_lockstep(loctun *t)
{
    LONG l = t->n->st.last34_ms;
    return l && (LONG)(GetTickCount() - (DWORD)l) < 2000;
}

/* Host-Zustand aus dem Spiel (loc_net.c schreibt pub_w): 1 = gueltig. */
static int pub_snap(loctun *t, uint8_t s[8], uint8_t c[2], int *state)
{
    LONG w0 = t->n->pub_w[0], w1 = t->n->pub_w[1], w2 = t->n->pub_w[2];
    if (!(w2 & 0x1000000)) return 0;
    put32(s, (uint32_t)w0);
    put32(s + 4, (uint32_t)w1);
    *state = (int)(w2 & 0xFF);
    c[0] = (uint8_t)(w2 >> 8);
    c[1] = (uint8_t)(w2 >> 16);
    return 1;
}

/* Werte wie loc_guard hostinfo_ok + Wertebereiche des Originals (LOC_BOOT §49.2). */
static int pub_fields_ok(const uint8_t s[8], const uint8_t c[2])
{
    int a = (int8_t)s[2], b = (int8_t)s[3];
    return s[0] <= 3 && s[1] <= 3 && a >= 1 && b >= 0 && a + b <= 10 && s[4] <= 2 && s[5] < 0x1B && s[5] != 0x18 &&
           s[5] != 0x1A && s[6] <= 2 && s[7] <= 1 && (int8_t)c[0] >= 0 && (int8_t)c[0] <= a && (int8_t)c[1] >= 0 &&
           (int8_t)c[1] <= b;
}

static void pub_ghost_close(tghost *g)
{
    if (g->s[0] != INVALID_SOCKET) closesocket(g->s[0]);
    if (g->s[1] != INVALID_SOCKET) closesocket(g->s[1]);
    g->s[0] = g->s[1] = INVALID_SOCKET;
    g->used = 0;
}

void pub_ghosts_close_one(tghost *g) /* W482 */
{
    pub_ghost_close(g);
}

void pub_ghosts_clear(loctun *t)
{
    int i;
    for (i = 0; i < TUN_MAX_GHOST; i++) {
        if (t->gh[i].used) pub_ghost_close(&t->gh[i]);
    }
    t->pub_join = -1;
}

tghost *pub_ghost_by_vip(loctun *t, uint32_t vip)
{
    int i;
    for (i = 0; i < TUN_MAX_GHOST; i++) {
        if (t->gh[i].used && t->gh[i].vip == vip) return &t->gh[i];
    }
    return NULL;
}

/* Socket des Geists fuer Spielport gp (1201/1202) auf vip:Stellvertreter-Basis+(gp-1000), wie ein Mitspieler. */
static SOCKET pub_ghost_sock(loctun *t, tghost *g, int k)
{
    struct sockaddr_in a;
    BOOL off = FALSE;
    DWORD got = 0;
    SOCKET s;
    if (g->s[k] != INVALID_SOCKET) return g->s[k];
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;
    WSAIoctl(s, 0x9800000Cu, &off, sizeof off, NULL, 0, &got, NULL, NULL);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = g->vip;
    a.sin_port = htons((uint16_t)(t->c.proxy_base + (k ? 202 : 201)));
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    g->s[k] = s;
    return s;
}

static void pub_ghost_send(loctun *t, tghost *g, uint16_t gp, const uint8_t *d, int len)
{
    struct sockaddr_in a;
    SOCKET s;
    uint16_t hp = locnet_port_out(t->n, t->n->self_ip, gp);
    /* W373: auch selbst gebaute Rahmen durch den Gastfilter (Verteidigung in der Tiefe) und nur an gebundene Ports */
    if (!lguard_udp_ok(gp, d, len) || !locnet_udp_bound(t->n, hp)) return;
    s = pub_ghost_sock(t, g, gp == 1202);
    if (s == INVALID_SOCKET) return;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = t->n->self_ip;
    a.sin_port = htons(hp);
    sendto(s, (const char *)d, len, 0, (struct sockaddr *)&a, sizeof a);
}

/* Host-Info Typ 2 (100 B) eines Geists fuer die laufende Suche (Nonce-Echo). Layout LOC_BOOT §49.2. */
static void pub_hostinfo(loctun *t, const tghost *g, uint8_t d[100])
{
    memset(d, 0, 100);
    d[0] = 2;
    memcpy(d + 4, t->pub_nonce, 8);
    d[0x0C] = 0x00; /* XNKID System-Link-Typ (oberes Halbbyte 0), eindeutig je Lobby */
    memcpy(d + 0x0D, "SBP", 3);
    put32(d + 0x10, (uint32_t)g->lid);
    memcpy(d + 0x14, "SBPUBLIC", 8); /* XNKEY: die HLE prueft keine Schluessel */
    put32(d + 0x1C, (uint32_t)g->lid);
    locnet_xnaddr(t->n, g->vip, d + 0x24);
    memcpy(d + 0x48, g->sf, 8);
    memcpy(d + 0x50, g->name, strlen(g->name)); /* <= 15 Zeichen, Rest 0 */
    d[0x60] = g->cf[0];
    d[0x61] = g->cf[1];
}

static void pub_answer(loctun *t, double now)
{
    int i;
    uint8_t d[100];
    if (t->pub_t1_at <= 0 || now - t->pub_t1_at > 2500.0) return; /* nur auf eine Suche, im Antwortfenster (~3 s) */
    for (i = 0; i < TUN_MAX_GHOST; i++) {
        tghost *g = &t->gh[i];
        if (!g->used || g->ans == t->pub_seq) continue;
        g->ans = t->pub_seq;
        pub_hostinfo(t, g, d);
        pub_ghost_send(t, g, 1201, d, 100);
        t->pub_answered++;
    }
}

static void pub_list_req(loctun *t, double now)
{
    t->pub_list_at = now;
    lb_send(t, "{\"t\":\"lobby_list\",\"game\":\"loc\",\"open\":true}");
}

/* Lobby verlassen (eigener Abgang): Verbindungen abbauen wie bei lobby_closed. */
static void pub_leave(loctun *t, const char *why, double now)
{
    int i;
    lb_send(t, "{\"t\":\"lobby_leave\"}");
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        if (!L->used || L->cid == 0) continue;
        tl_link_drop(t, L, "Lobby verlassen");
        L->used = 0;
    }
    tl_logf(t, "LOCPUB: Lobby %d verlassen (%s)", t->lb_lobby_id, why);
    lb_play_mode_set(t, 0, "Lobby verlassen"); /* W440 */
    t->lb_joined = 0;
    t->lb_listed = 1;
    if (t->lb_state == LB_LOBBY) t->lb_state = LB_HELLO;
    t->pub_host = t->pub_started = 0;
    t->pub_host_cid = 0;
    t->pub_host_vip = 0;
    t->pub_nmem = 0;
    t->pub_idle_at = 0;
    t->pub_off_at = 0;
    t->pub_create_at = now + 2000.0;
}

/* rules der eigenen Lobby: vip, Spielfelder, gerundete Relay-Pings (rt_<Relay>, 10-ms-Stufen). */
static int pub_rules(loctun *t, const uint8_t s[8], const uint8_t c[2], char *out, int cap)
{
    int i, n, nr = 0;
    n = _snprintf_s(out, (size_t)cap, _TRUNCATE,
                    "{\"vip\":%u,\"fa\":%u,\"fb\":%u,\"ma\":%d,\"mb\":%d,\"tm\":%u,\"ms\":%u,\"cf\":%u,\"op\":%u,"
                    "\"ca\":%d,\"cb\":%d",
                    (unsigned)ntohl(t->n->self_ip), s[0], s[1], (int8_t)s[2], (int8_t)s[3], s[4], s[5], s[6], s[7],
                    (int8_t)c[0], (int8_t)c[1]);
    for (i = 0; i < t->nrelays && nr < 4 && n > 0 && n < cap - 64; i++) {
        trelay *r = &t->relays[i];
        size_t k, l = strlen(r->name);
        int ok = l > 0 && l <= 24;
        for (k = 0; k < l && ok; k++) ok = isalnum((unsigned char)r->name[k]) || r->name[k] == '_' || r->name[k] == '-';
        if (!ok || !t->relays_done || r->ms <= 0) continue;
        n += _snprintf_s(out + n, (size_t)(cap - n), _TRUNCATE, ",\"rt_%s\":%d", r->name,
                         ((int)(r->ms + 9.999) / 10) * 10 > 990 ? 990 : ((int)(r->ms + 9.999) / 10) * 10);
        nr++;
    }
    if (n <= 0 || n >= cap - 2) return 0;
    out[n++] = '}';
    out[n] = 0;
    return 1;
}

static void pub_publish(loctun *t, const uint8_t s[8], const uint8_t c[2], int create, double now)
{
    char rules[400], b[700], nm[64];
    if (!pub_rules(t, s, c, rules, sizeof rules)) return;
    memcpy(t->pub_sig, s, 8);
    memcpy(t->pub_sig + 8, c, 2);
    t->pub_upd_at = now;
    if (create) {
        int maxp = (int8_t)s[2] + (int8_t)s[3];
        tl_js_esc(t->my_pilot[0] ? t->my_pilot : "Pilot", nm, sizeof nm);
        _snprintf_s(b, sizeof b, _TRUNCATE,
                    "{\"t\":\"lobby_create\",\"name\":\"%s\",\"game\":\"loc\",\"mode\":\"syslink\",\"map\":\"m%02u\","
                    "\"max_players\":%d,\"rules\":%s,\"visibility\":\"public\",\"play_mode\":\"%s\"}",
                    nm, s[5], maxp < 2 ? 2 : (maxp > 10 ? 10 : maxp), rules, lb_my_play_mode(t)); /* W440 */
        t->pub_creating = 1;
        tl_logf(t, "LOCPUB: Match angeboten - oeffentliche Lobby anlegen (Mission %u, Sitze %d/%d)", s[5],
                (int8_t)s[2], (int8_t)s[3]);
    } else {
        _snprintf_s(b, sizeof b, _TRUNCATE, "{\"t\":\"lobby_update\",\"rules\":%s}", rules);
    }
    lb_send(t, b);
}

/* Anzeigename "Pilot 120ms" (<= 15 Zeichen + NUL, nur druckbares ASCII ohne Steuerzeichen). */
static void pub_name(const char *pilot, int est, char out[16])
{
    char suf[12] = "";
    int i, o = 0, room;
    if (est > 0) _snprintf_s(suf, sizeof suf, _TRUNCATE, " %dms", est > 999 ? 999 : est);
    room = 15 - (int)strlen(suf);
    if (strncmp(pilot, "Guest-", 6) == 0 && pilot[6]) { /* Gast: "~Name" (Platz; bleibt von Kontonamen unterscheidbar) */
        out[o++] = '~';
        pilot += 6;
    }
    for (i = 0; pilot[i] && o < room; i++) {
        char ch = pilot[i];
        out[o++] = (ch >= 0x20 && ch <= 0x7E) ? ch : '?';
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    if (o == 0) out[o++] = '?';
    out[o] = 0;
    strcat_s(out, 16, suf);
}

/* Liste offener oeffentlicher Lobbys -> Geister (jede Zahl vor der Verwendung geprueft). */
void pub_on_list(loctun *t, const char *m, const char *e, double now)
{
    const char *v, *ve, *el, *more, *moree;
    int i, seen[TUN_MAX_GHOST], bad = 0, n = 0;
    memset(seen, 0, sizeof seen);
    v = js_get(m, e, "lobbies", &ve);
    more = js_get(m, e, "more", &moree);
    t->pub_list_rx = now > 0 ? now : 1.0; /* W460: Widget - Liste ist frisch */
    for (el = js_arr_first(v, ve); el; el = js_arr_next(el, ve, NULL)) {
        const char *ee = js_skip(el, ve, 0), *x, *xe, *ru, *rue;
        char pilot[24] = "", state[12] = "", key[40], pm[12] = "";
        double id = 0, f[11], rt;
        static const char *const k[11] = {"vip", "fa", "fb", "ma", "mb", "tm", "ms", "cf", "op", "ca", "cb"};
        uint8_t s8[8], c2[2];
        uint32_t vip;
        int j, est = -1, ok = 1, pmode;
        tghost *g;
        if (!ee) break;
        if (++n > 64) break; /* Obergrenze je Liste */
        x = js_get(el, ee, "id", &xe);
        js_num(x, xe, &id);
        x = js_get(el, ee, "state", &xe);
        js_str(x, xe, state, sizeof state);
        x = js_get(el, ee, "host_pilot", &xe);
        js_str(x, xe, pilot, sizeof pilot);
        /* W443 LOC-LOBBY-LIST: Modus je Lobby wie W440 streng (nur "sim"/"arcade"; fehlt = aelterer Dienst = Sim) */
        x = js_get(el, ee, "play_mode", &xe);
        pmode = (x && js_str(x, xe, pm, sizeof pm) && las_parse(pm)) ? las_parse(pm) : LAS_SIM;
        ru = js_get(el, ee, "rules", &rue);
        for (j = 0; j < 11 && ok; j++) {
            x = ru ? js_get(ru, rue, k[j], &xe) : NULL;
            ok = js_num(x, xe, &f[j]) && f[j] >= (j == 0 ? 0.0 : -128.0) && f[j] <= (j == 0 ? 4294967295.0 : 127.0) &&
                 f[j] == floor(f[j]);
        }
        if (!ok || !(id >= 1 && id <= 2147483647.0) || strcmp(state, "open") != 0 || !pilot[0]) {
            bad += ok ? 0 : 1;
            continue;
        }
        if (t->lb_joined && (int)id == t->lb_lobby_id) continue; /* eigene Lobby: der echte Host antwortet */
        for (j = 0; j < 8; j++) s8[j] = (uint8_t)(int)f[j + 1];
        c2[0] = (uint8_t)(int)f[9];
        c2[1] = (uint8_t)(int)f[10];
        vip = htonl((uint32_t)f[0]);
        if (!pub_fields_ok(s8, c2) || (int8_t)c2[0] + (int8_t)c2[1] >= (int8_t)s8[2] + (int8_t)s8[3]) {
            bad++;
            continue; /* ungueltig bzw. voll */
        }
        {
            uint32_t h = ntohl(vip);
            int clash = (h >> 24) != 127u || h == 0x7F000001u || (h & 0xFFu) == 0u || (h & 0xFFu) == 255u ||
                        (vip & 0xFFFFFF00u) == 0 || ((vip ^ t->n->self_ip) & htonl(0x00FFFFFFu)) == 0;
            for (j = 0; j < TUN_MAX_LINKS && !clash; j++)
                clash = t->links[j].used && t->links[j].vip == vip;
            for (j = 0; j < TUN_MAX_GHOST && !clash; j++) /* abEnet = letzte 3 Adressbytes: eindeutig */
                clash = t->gh[j].used && t->gh[j].lid != (int)id && ((t->gh[j].vip ^ vip) & htonl(0x00FFFFFFu)) == 0;
            if (clash) {
                bad++;
                continue;
            }
        }
        for (j = 0; j < t->nrelays; j++) { /* Ping-Schaetzung ueber das gemeinsame Relay mit der kleinsten Summe */
            trelay *r = &t->relays[j];
            if (!t->relays_done || r->ms <= 0 || strlen(r->name) > 24) continue;
            _snprintf_s(key, sizeof key, _TRUNCATE, "rt_%s", r->name);
            x = ru ? js_get(ru, rue, key, &xe) : NULL;
            if (js_num(x, xe, &rt) && rt >= 0 && rt <= 990 && (est < 0 || rt + r->ms < est)) est = (int)(rt + r->ms + 0.5);
        }
        g = NULL;
        for (j = 0; j < TUN_MAX_GHOST; j++) {
            if (t->gh[j].used && t->gh[j].lid == (int)id) g = &t->gh[j];
        }
        if (g && g->vip != vip) { /* andere vip unter derselben Lobby: neu anlegen */
            if (t->pub_join == (int)(g - t->gh)) continue;
            pub_ghost_close(g);
            g = NULL;
        }
        if (!g) {
            for (j = 0; j < TUN_MAX_GHOST && !g; j++) {
                if (!t->gh[j].used) g = &t->gh[j];
            }
            if (!g) break;
            memset(g, 0, sizeof *g);
            g->used = 1;
            g->lid = (int)id;
            g->vip = vip;
            g->s[0] = g->s[1] = INVALID_SOCKET;
            g->ans = t->pub_seq - 1;
        }
        memcpy(g->sf, s8, 8);
        memcpy(g->cf, c2, 2);
        g->est = est;
        g->mode = pmode; /* W443 */
        pub_name(pilot, est, g->name);
        for (j = 0; pilot[j]; j++) /* W460: Anzeige im Widget, Steuerzeichen raus */
            g->pilot[j] = (unsigned char)pilot[j] < 0x20 || pilot[j] == 0x7F ? '?' : pilot[j];
        g->pilot[j] = 0;
        g->seen = now;
        seen[g - t->gh] = 1;
    }
    for (i = 0; i < TUN_MAX_GHOST; i++) { /* nicht mehr gelistet (voll, gestartet, geschlossen) -> weg */
        if (t->gh[i].used && !seen[i] && i != t->pub_join && (!more || now - t->gh[i].seen > 30000.0))
            pub_ghost_close(&t->gh[i]);
    }
    if (bad && !t->pub_bad_logged) {
        t->pub_bad_logged = 1;
        tl_logf(t, "LOCPUB: %d Listeneintraege ungueltig/voll - nicht angezeigt", bad);
    }
    n = 0;
    for (i = 0; i < TUN_MAX_GHOST; i++) n += t->gh[i].used;
    if (n != t->pub_ngames) tl_logf(t, "LOCPUB: %d offene oeffentliche Spiele", n);
    t->pub_ngames = n;
    pub_answer(t, now);
}

/* lobby_state im oeffentlichen Modus: Host und Mitgliederzahl merken. */
void pub_on_state(loctun *t, const char *m, const char *e)
{
    const char *lb, *lbe = NULL, *v, *ve = NULL, *el;   /* /sdl C4703 */
    int nm = 0;
    uint32_t host = 0;
    lb = js_get(m, e, "lobby", &lbe);
    v = lb ? js_get(lb, lbe, "members", &ve) : NULL;
    for (el = js_arr_first(v, ve); el; el = js_arr_next(el, ve, NULL)) {
        const char *ee = js_skip(el, ve, 0), *x, *xe;
        double cid = 0;
        if (!ee) break;
        nm++;
        x = js_get(el, ee, "client_id", &xe);
        js_num(x, xe, &cid);
        x = js_get(el, ee, "host", &xe);
        if (x && xe - x >= 4 && strncmp(x, "true", 4) == 0 && cid > 0 && cid <= 4294967295.0) host = (uint32_t)cid;
    }
    t->pub_nmem = nm;
    if (host) t->pub_host_cid = host;
    if (t->pub_creating && host == t->my_cid) {
        t->pub_creating = 0;
        t->pub_host = 1;
        t->pub_started = 0;
        tl_logf(t, "LOCPUB: oeffentliche Lobby %d angelegt", t->lb_lobby_id);
    }
}

/* Synthetische Ablehnung (Typ 7, 6 B, Grund 0) vom Geist an das Spiel (B: Client verlaesst den Wartezustand). */
void pub_reject(loctun *t, int gi, const char *why, double now)
{
    static const uint8_t rej[6] = {7, 0, 0, 0, 0, 0};
    if (gi >= 0 && gi < TUN_MAX_GHOST && t->gh[gi].used) {
        pub_ghost_send(t, &t->gh[gi], 1202, rej, 6);
        pub_ghost_close(&t->gh[gi]);
    }
    tl_logf(t, "LOCPUB: Beitritt abgelehnt (%s)", why);
    t->pub_join = -1;
    t->pub_joining = 0;
    t->pub_t3_len = 0;
    if (t->lb_joined && !t->pub_host) pub_leave(t, why, now);
}

/* Verbindung steht: Geist der Lobby schliessen (Stellvertreter-Ports frei), gemerkte Beitrittsanfrage nachreichen. */
void pub_on_link_up(loctun *t, tlink *L)
{
    tghost *g = pub_ghost_by_vip(t, L->vip);
    if (g) {
        int gi = (int)(g - t->gh);
        pub_ghost_close(g);
        if (gi == t->pub_join) t->pub_join = -1;
    }
    if (t->pub_joining && L->cid == t->pub_host_cid) {
        if (L->vip != t->pub_join_vip) {
            t->pub_join_bad = 1; /* Host meldet eine andere vip als gelistet -> Ablehnung im naechsten pub_tick */
            return;
        }
        t->pub_joining = 0;
        t->pub_host_vip = L->vip;
        t->pub_idle_at = tl_now(t);
        if (t->pub_t3_len > 0) {
            int k, slot = -1;
            for (k = 0; k < TUN_DQ; k++) {
                if (!L->dq[k].used) { slot = k; break; }
            }
            if (slot >= 0) {
                L->dq[slot].used = 1;
                L->dq[slot].id = L->dg_next++;
                L->dq[slot].src = t->pub_t3_src;
                L->dq[slot].dst = 1202;
                L->dq[slot].len = (uint16_t)t->pub_t3_len;
                L->dq[slot].tx = 0;
                memcpy(L->dq[slot].data, t->pub_t3, (size_t)t->pub_t3_len);
                InterlockedIncrement(&t->st.dg_tx);
            }
            t->pub_t3_len = 0;
        }
        tl_logf(t, "LOCPUB: Verbindung zum Host c%u steht - Beitrittsanfrage weitergereicht", L->cid);
    }
}

static int pub_host_stream(loctun *t)
{
    int i;
    for (i = 0; i < TUN_MAX_STREAMS; i++) {
        const tstream *st = &t->streams[i];
        if (st->used && st->link >= 0 && st->link < TUN_MAX_LINKS && t->links[st->link].used &&
            t->links[st->link].vip == t->pub_host_vip)
            return 1;
    }
    return 0;
}

static void pub_join_tick(loctun *t, double now);

/* W501 ONLINE-BOTH: System Link <-> Xbox Live (Netz-Thread, unter cs). Der Transport startet wie ohne loc_live im
 * System-Link-Zustand (oeffentlich bzw. Code-Lobby). Betritt das Spiel den Xbox-Live-Fluss (XOnline-HLE), ruht System
 * Link: offene System-Link-Lobby verlassen, Geister weg, Live-Modus wie W482 (pub = 1, keine Start-Lobby). Verlaesst es
 * Xbox Live (Logon-Task geschlossen), enden die Live-Anfragen und die Live-Lobby; System Link wie beim Start (Code-
 * Lobby wird neu betreten; ohne Raumcode = neue eigene Code-Lobby). */
void pub_live_mode_tick(loctun *t, double now)
{
    int want = t->live_cap && t->live_want;
    if (want == t->live) return;
    if (t->pub_joining) pub_reject(t, t->pub_join, want ? "Xbox Live gewaehlt" : "Xbox Live verlassen", now);
    if (!want) lv_reset(t, "Xbox Live verlassen");
    if (t->lb_joined) pub_leave(t, want ? "Xbox Live gewaehlt" : "Xbox Live verlassen", now);
    pub_ghosts_clear(t);
    t->pub_host = t->pub_started = t->pub_creating = 0;
    t->pub_host_vip = t->pub_host_cid = 0;
    t->pub_t1_new = 0;
    t->pub_t3_len = 0;
    t->live = want;
    t->pub = want ? 1 : t->pub0;
    t->lb_listed = t->pub ? 1 : 0; /* System Link ohne pub: lb_enter wie beim Start (lb_poll) */
    t->pub_list_at = -1e9;
    t->pub_create_at = now + 1000.0;
    tl_logf(t, want ? "LOCLIVE: Spiel im Xbox-Live-Menue - Transport im Live-Modus (System Link ruht)"
                    : "LOCLIVE: Xbox Live verlassen - Transport wieder System Link (%s)",
            t->pub ? "oeffentlich" : t->c.code[0] ? "Raumcode" : "eigene Code-Lobby");
}

void pub_tick(loctun *t, double now)
{
    uint8_t s[8], c[2];
    int state = -1, snap, open_host;
    if (t->live) { /* W482: Host per live_create (keine oeffentliche Lobby), keine System-Link-Suche; Beitritt wie W373 */
        lv_tick(t, now);
        pub_join_tick(t, now); /* waehlt das Spiel einen anderen Host, verlaesst es die eigene Lobby (pub_leave) */
        return;
    }
    snap = pub_snap(t, s, c, &state);
    open_host = snap && (state == 0x22 || state == 0x23) && pub_fields_ok(s, c);
    /* --- Host --- */
    if (!t->lb_joined) {
        if (open_host && !t->pub_creating && t->pub_join < 0 && !t->pub_joining && now >= t->pub_create_at) {
            pub_publish(t, s, c, 1, now);
            t->pub_create_at = now + 3000.0;
        }
        if (t->pub_creating && now >= t->pub_create_at) t->pub_creating = 0; /* keine Antwort: spaeter erneut */
    } else if (t->pub_host) {
        if (!t->pub_started) {
            if (pub_lockstep(t) || (!open_host && t->pub_nmem > 1 && t->pub_off_at && now - t->pub_off_at >= 2000.0)) {
                lb_send(t, "{\"t\":\"lobby_start\"}");
                t->pub_started = 1;
                tl_logf(t, "LOCPUB: Match beginnt - Lobby %d nicht mehr gelistet", t->lb_lobby_id);
            } else if (!open_host && t->pub_nmem <= 1 && t->pub_off_at && now - t->pub_off_at >= 3000.0) {
                pub_leave(t, "Host hat das Match verlassen", now);
                return;
            } else if (open_host && memcmp(t->pub_sig, s, 8) == 0 && memcmp(t->pub_sig + 8, c, 2) == 0) {
                t->pub_chg_at = 0;
            } else if (open_host) {
                if (!t->pub_chg_at) t->pub_chg_at = now; /* erst nach 1 s Ruhe (keine Zwischenstaende) */
                if (now - t->pub_chg_at >= 1000.0 && now - t->pub_upd_at >= 1500.0) {
                    pub_publish(t, s, c, 0, now);
                    t->pub_chg_at = 0;
                }
            }
        } else if (open_host && t->pub_nmem <= 1 && !pub_lockstep(t)) {
            pub_leave(t, "Match zu Ende, Host allein", now); /* danach neue offene Lobby */
            return;
        }
        if (open_host) t->pub_off_at = 0;
        else if (!t->pub_off_at) t->pub_off_at = now;
        return;
    }
    /* --- Suchender --- */
    if (t->pub_t1_new) {
        t->pub_t1_new = 0;
        t->pub_seq++;
        pub_answer(t, now);                                    /* sofort aus dem Zwischenspeicher */
        if (now - t->pub_list_at >= 1500.0) pub_list_req(t, now); /* und frisch (Antwort im Fenster) */
    } else if (!t->lb_joined && t->pub_t1_at > 0 && now - t->pub_t1_at < 60000.0 && now - t->pub_list_at >= 10000.0) {
        pub_list_req(t, now); /* Suchbildschirm offen: Liste warm halten */
    } else if (!t->lb_joined && t->pub_watch && now - t->pub_list_at >= TUN_WATCH_MS) {
        pub_list_req(t, now); /* W460 LOC-LOBBY-WIDGET: Hauptmenue/MatchMaking sichtbar (hoechstens alle 12 s) */
    }
    pub_join_tick(t, now);
}

/* Beitritt zu einem gewaehlten Geist, Zeitlimit, Platz freigeben ohne Spielverbindung (W373; W482 auch Live). */
static void pub_join_tick(loctun *t, double now)
{
    if (t->pub_join >= 0 && !t->pub_joining && t->gh[t->pub_join].used) {
        char b[96];
        if (t->lb_joined) pub_leave(t, "anderes Spiel gewaehlt", now);
        t->pub_joining = 1;
        t->pub_join_at = now;
        t->pub_join_vip = t->gh[t->pub_join].vip;
        t->pub_join_bad = 0;
        t->pub_host_cid = 0;
        if (t->gh[t->pub_join].live) { /* W482: Live-Session - XNKEY kommt mit lobby_state nur an Mitglieder */
            char sid[17];
            tohex(t->gh[t->pub_join].live_sid, 8, sid);
            _snprintf_s(b, sizeof b, _TRUNCATE, "{\"t\":\"live_join\",\"lv\":1,\"sid\":\"%s\"}", sid);
        } else {
            _snprintf_s(b, sizeof b, _TRUNCATE, "{\"t\":\"lobby_join\",\"lobby_id\":%d}", t->gh[t->pub_join].lid);
        }
        lb_send(t, b);
        tl_logf(t, "LOCPUB: Spiel gewaehlt - trete Lobby %d bei", t->gh[t->pub_join].lid);
    }
    if (t->pub_joining && (t->pub_join_bad || now - t->pub_join_at > 25000.0)) {
        pub_reject(t, t->pub_join, t->pub_join_bad ? "Host-Adresse passt nicht" : "keine Verbindung in 25 s", now);
        return;
    }
    if (t->lb_joined && !t->pub_joining && t->pub_host_vip) { /* Mitglied ohne Spielverbindung: Platz freigeben */
        if (pub_host_stream(t)) t->pub_idle_at = now;
        else if (now - t->pub_idle_at > 20000.0) pub_leave(t, "keine Spielverbindung zum Host", now);
    }
}

