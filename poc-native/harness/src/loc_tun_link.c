/* loc_tun_link.c - W464 NET-CORE: aus loc_tun.c verschoben (W324 LOC-NETPLAY u. a.), unveraendert.
 * Hilfen (Log, Zeit, HMAC, Hex, Adressen, Wege-Simulation), Senden an Socket/Weg, Verbindungstabelle, RTT/RTO,
 * Stroeme (Spiel-TCP ueber Stellvertreter, Gastfilter je Rahmen) und Datagramme (Spiel-UDP, Doppel-Erkennung). */
#include "loc_tun_int.h"
#include <iphlpapi.h> /* W521: GetAdaptersAddresses (eigene globale IPv6-Adresse) */
#pragma comment(lib, "iphlpapi.lib")

static void tl_natf_note(loctun *t, const struct sockaddr_in *to); /* W521 TEST */

/* ====================================================================== Hilfen */
void tl_logf(loctun *t, const char *fmt, ...)
{
    char b[600];
    va_list ap;
    DWORD now;
    if (!t->log) return;
    /* W332: Viele Zeilen haengen an Paketen von Mitspielern bzw. Zeilen des Treffpunkts. Ohne Obergrenze kann eine
     * Gegenstelle das Log (und die Platte) in Paketrate fuellen: hoechstens 200 Zeilen je 10 s, Rest gezaehlt. */
    now = GetTickCount();
    if (now - t->log_t0 >= 10000u) {
        if (t->log_drop) {
            _snprintf_s(b, sizeof b, _TRUNCATE, "LOCTUN: %u Logzeilen unterdrueckt (hoechstens 200 je 10 s)", t->log_drop);
            t->log(b);
        }
        t->log_t0 = now;
        t->log_n = 0;
        t->log_drop = 0;
    }
    if (t->log_n >= 200u) {
        t->log_drop++;
        return;
    }
    t->log_n++;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    t->log(b);
}

double tl_now(const loctun *t)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return (double)q.QuadPart * t->qpf_ms - t->t0;
}


static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* W328: HMAC-SHA256 (RFC 2104) ueber sb_sha256; Schluessel <= 64 Byte. */
void tl_hmac(const uint8_t *key, size_t klen, const char *msg, size_t mlen, uint8_t out[32])
{
    uint8_t k[64], pad[64], inner[32];
    sb_sha256_t c;
    int i;
    memset(k, 0, sizeof k);
    memcpy(k, key, klen > 64 ? 64 : klen);
    for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sb_sha256_init(&c);
    sb_sha256_update(&c, pad, 64);
    sb_sha256_update(&c, msg, mlen);
    sb_sha256_final(&c, inner);
    for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sb_sha256_init(&c);
    sb_sha256_update(&c, pad, 64);
    sb_sha256_update(&c, inner, 32);
    sb_sha256_final(&c, out);
    SecureZeroMemory(k, sizeof k);
    SecureZeroMemory(pad, sizeof pad);
}

int unhex(const char *s, uint8_t *out, int n)
{
    int i;
    if ((int)strlen(s) != 2 * n) return 0;
    for (i = 0; i < n; i++) {
        int a = hexval(s[2 * i]), b = hexval(s[2 * i + 1]);
        if (a < 0 || b < 0) return 0;
        out[i] = (uint8_t)(a * 16 + b);
    }
    return 1;
}

void tohex(const uint8_t *p, int n, char *out)
{
    static const char hx[] = "0123456789abcdef";
    int i;
    for (i = 0; i < n; i++) {
        out[2 * i] = hx[p[i] >> 4];
        out[2 * i + 1] = hx[p[i] & 15];
    }
    out[2 * n] = 0;
}

int sa_eq(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

int tl_dest_ok(loctun *t, uint32_t ip)
{
    if ((ntohl(ip) & 0xFFFF0000u) == TUN_V6_NET) { /* W521: IPv6-Ersatzadresse - nur ein belegter Tabelleneintrag */
        uint32_t i = ntohl(ip) & 0xFFFFu;
        return t->us6 != INVALID_SOCKET && i < TUN_V6T && t->v6t[i].used;
    }
    if (ip == 0 || ip == INADDR_BROADCAST) return 0;
    return locnet_ip_allowed(ip) || t->c.allow_public;
}

int tl_resolve(const char *host, uint16_t port, struct sockaddr_in *out)
{
    struct addrinfo hints, *res = NULL;
    char ps[8];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    _snprintf_s(ps, sizeof ps, _TRUNCATE, "%u", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return 0;
    memcpy(out, res->ai_addr, sizeof *out);
    freeaddrinfo(res);
    return 1;
}

int tl_split_hostport(const char *s, char *host, int hl, uint16_t *port)
{
    const char *c = strrchr(s, ':');
    int p;
    if (!c || c == s) return 0;
    p = atoi(c + 1);
    if (p <= 0 || p > 65535) return 0;
    if ((int)(c - s) >= hl) return 0;
    memcpy(host, s, (size_t)(c - s));
    host[c - s] = 0;
    *port = (uint16_t)p;
    return 1;
}

/* W395 TEST loc_route_sim: Eintrag fuer den Weg zu/von einer Adresse (Relayname, sonst "direct"), -1 = keiner. */
int tl_rsim_find(loctun *t, const struct sockaddr_in *a)
{
    const char *name = "direct";
    int i;
    for (i = 0; i < t->nrelays; i++) {
        if (t->relays[i].resolved && sa_eq(a, &t->relays[i].addr)) {
            name = t->relays[i].name;
            break;
        }
    }
    if (i == t->nrelays && sa_eq(a, &t->srv_udp)) return -1; /* Treffpunkt-UDP ohne gleichnamiges Relay */
    for (i = 0; i < t->nrsim; i++) {
        if (strcmp(t->rsim[i].name, name) == 0) return i;
    }
    return -1;
}

/* W494 TEST loc_route_sim "@PREFIX:SPEC": Strecke je Mitspieler (Pilotname beginnt mit PREFIX, z. B. @AU / @EU), damit
 * mehrere Clients auf einer Maschine Regionenpaare nachbilden (jede Seite verzoegert ihre Sendungen um die halbe RTT).
 * Gilt fuer alle Pakete der Verbindung (direkt und ueber Relays), -1 = keiner. */
int tl_rsim_find_link(loctun *t, const tlink *L)
{
    int i;
    if (!L || !L->pilot[0]) return -1;
    for (i = 0; i < t->nrsim; i++) {
        const char *n = t->rsim[i].name;
        size_t k;
        if (n[0] != '@') continue;
        k = strlen(n + 1);
        if (!k) continue;
        if (_strnicmp(L->pilot, n + 1, k) == 0) return i;
        { /* Gastkonten heissen "Guest-<Pilot>" (devserver): Praefix auch nach einem '-' */
            const char *d = strchr(L->pilot, '-');
            if (d && _strnicmp(d + 1, n + 1, k) == 0) return i;
        }
    }
    return -1;
}

/* Versiegeltes Paket einer Verbindung senden (W494: ueber die Strecke des Mitspielers, falls TEST gesetzt). */
static void tl_sendto_link(loctun *t, tlink *L, const struct sockaddr_in *to, const uint8_t *buf, int len)
{
    if (t->nrsim && !tl_is_v6(to)) {
        int k = tl_rsim_find_link(t, L);
        if (k >= 0) {
            if (tl_rsim_cut(t, k)) return;
            if (t->rsim[k].sim && tl_dest_ok(t, to->sin_addr.s_addr) && !t->netcut) {
                locnetsim_send(t->rsim[k].sim, t->us, 0, (const char *)buf, len, 0, to);
                return;
            }
        }
    }
    tl_sendto(t, to, buf, len);
}

/* 1 = Weg k ist gerade im Ausfallfenster (beide Richtungen verwerfen). */
int tl_rsim_cut(loctun *t, int k)
{
    double s;
    if (k < 0 || t->rsim[k].cut_a < 0) return 0;
    s = (tl_now(t) - 1000.0) / 1000.0; /* t0 liegt 1 s vor dem Start */
    if (s >= t->rsim[k].cut_a && (t->rsim[k].cut_b < 0 || s < t->rsim[k].cut_b)) {
        if (!(t->rsim[k].cut_logged & 1)) {
            t->rsim[k].cut_logged |= 1;
            tl_logf(t, "LOCROUTE: TEST Weg %s ausgefallen (t=%.1f s)", t->rsim[k].name, s);
        }
        return 1;
    }
    if ((t->rsim[k].cut_logged & 3) == 1) {
        t->rsim[k].cut_logged |= 2;
        tl_logf(t, "LOCROUTE: TEST Weg %s wieder da (t=%.1f s)", t->rsim[k].name, s);
    }
    return 0;
}

/* loc_route_sim zerlegen: "name:SPEC/name:SPEC", SPEC = loc_netsim-Teile plus cut:A-B. */
void tl_rsim_setup(loctun *t)
{
    char buf[256], *ctx = NULL, *part;
    strcpy_s(buf, sizeof buf, t->c.route_sim);
    for (part = strtok_s(buf, "/", &ctx); part && t->nrsim < TUN_ROUTE_SIMS; part = strtok_s(NULL, "/", &ctx)) {
        char *c = strchr(part, ':'), spec[200] = "", err[96] = "", *q, *ctx2 = NULL;
        locnetsim_cfg sc;
        int k = t->nrsim;
        if (!c) continue;
        *c++ = 0;
        memset(&t->rsim[k], 0, sizeof t->rsim[k]);
        strncpy_s(t->rsim[k].name, sizeof t->rsim[k].name, part, _TRUNCATE);
        t->rsim[k].cut_a = t->rsim[k].cut_b = -1;
        for (q = strtok_s(c, ",", &ctx2); q; q = strtok_s(NULL, ",", &ctx2)) {
            if (_strnicmp(q, "cut:", 4) == 0) {
                const char *dash = strchr(q + 4, '-');
                t->rsim[k].cut_a = atof(q + 4);
                t->rsim[k].cut_b = (dash && dash[1]) ? atof(dash + 1) : -1;
            } else {
                if (spec[0]) strcat_s(spec, sizeof spec, ",");
                strncat_s(spec, sizeof spec, q, _TRUNCATE);
            }
        }
        if (spec[0]) {
            if (locnetsim_parse(spec, &sc, err, sizeof err)) {
                t->rsim[k].sim = locnetsim_start(&sc, NULL);
                if (t->rsim[k].sim) locnetsim_mark_start(t->rsim[k].sim, GetTickCount()); /* ramp ab Transportstart */
            } else {
                tl_logf(t, "LOCROUTE: TEST loc_route_sim %s ungueltig (%s)", t->rsim[k].name, err);
            }
        }
        tl_logf(t, "LOCROUTE: TEST Weg %s: %s%s, Ausfall %.0f-%.0f s", t->rsim[k].name, spec[0] ? spec : "ohne Verzoegerung",
                t->rsim[k].sim ? "" : (spec[0] ? " (aus)" : ""), t->rsim[k].cut_a, t->rsim[k].cut_b);
        t->nrsim++;
    }
}

/* Rohes Senden ueber den Transport-Socket (ueber die Verzoegerungsstufe, falls aktiv). */
void tl_sendto(loctun *t, const struct sockaddr_in *to, const uint8_t *buf, int len)
{
    if (!tl_dest_ok(t, to->sin_addr.s_addr)) {
        InterlockedIncrement(&t->st.bad);
        return;
    }
    if (t->netcut) return; /* W366 TEST: Netz weg */
    if (t->c.nat_filter) tl_natf_note(t, to); /* W521 TEST: Ziel merken (Filter der NAT-Nachbildung) */
    if (tl_is_v6(to)) { /* W521: ueber den IPv6-Socket an die echte Adresse */
        struct sockaddr_in6 a6;
        uint32_t i = ntohl(to->sin_addr.s_addr) & 0xFFFFu;
        a6 = t->v6t[i].a;
        a6.sin6_port = to->sin_port;
        sendto(t->us6, (const char *)buf, len, 0, (const struct sockaddr *)&a6, sizeof a6);
        return;
    }
    if (t->nrsim) { /* W395 TEST: Weg mit eigener Verzoegerung bzw. im Ausfallfenster */
        int k = tl_rsim_find(t, to);
        if (k >= 0 && tl_rsim_cut(t, k)) return;
        if (k >= 0 && t->rsim[k].sim) {
            locnetsim_send(t->rsim[k].sim, t->us, 0, (const char *)buf, len, 0, to);
            return;
        }
    }
    if (t->sim) {
        locnetsim_send(t->sim, t->us, 0, (const char *)buf, len, 0, to);
    } else {
        sendto(t->us, (const char *)buf, len, 0, (const struct sockaddr *)to, sizeof *to);
    }
}

void tl_hdr(uint8_t *p, int type)
{
    p[0] = (uint8_t)type; /* W334: innerer Typ; Magic/Sitzung stehen im SBT2-Kopf */
}


/* W334: Klartext versiegeln (SBT2). 0 = kein Paket (keine Schluessel, Grenze erreicht). */
static int tl_seal(loctun *t, tlink *L, const uint8_t *pt, int len, uint8_t *out)
{
    int n;
    if (!L->e2e.valid || L->blocked) return 0;
    n = sbe2e_seal(&L->e2e, pt, len, out, TUN_PKT);
    if (n <= 0) {
        InterlockedIncrement(&t->st.bad);
        return 0;
    }
    return n;
}

/* Nach dem Senden: harte ctr-Grenze 2^32 erreicht -> Verbindung beenden (kein Schluesselwechsel, §4.5). */
static void tl_after_seal(loctun *t, tlink *L)
{
    (void)t;
    if (sbe2e_exhausted(&L->e2e)) L->drop_pending = 1; /* Abbau im naechsten Zeitgeber-Durchlauf */
}

/* Versiegeltes Paket an eine Adresse (Durchstich, Antworten auf dem Ankunftsweg). */
void tl_send_to(loctun *t, tlink *L, const struct sockaddr_in *to, const uint8_t *pt, int len)
{
    uint8_t pkt[TUN_PKT];
    int n = tl_seal(t, L, pt, len, pkt);
    if (n <= 0) return;
    tl_sendto_link(t, L, to, pkt, n);
    tl_after_seal(t, L);
}

/* W395: Paket ueber einen bestimmten Weg-Slot (0 = direkt, 1+i = Relay i; SBT2 unveraendert als Relay-Nutzlast). */
void tl_send_slot(loctun *t, tlink *L, int slot, const uint8_t *pt, int len)
{
    uint8_t b[TUN_PKT + 16];
    int n;
    if (slot == 0) {
        if (!L->direct_ok) return;
    } else if (slot < 1 || slot > t->nrelays || !t->relays[slot - 1].resolved || L->relay_link == 0) {
        return;
    }
    n = tl_seal(t, L, pt, len, b + 9);
    if (n <= 0) return;
    if (slot == 0) {
        tl_sendto_link(t, L, &L->direct, b + 9, n);
    } else {
        memcpy(b, "SBN1", 4);
        b[4] = SN_RELAY;
        put32be(b + 5, L->relay_link);
        tl_sendto_link(t, L, &t->relays[slot - 1].addr, b, n + 9);
    }
    L->last_tx = tl_now(t);
    InterlockedIncrement(&L->s_tx);
    InterlockedIncrement(&t->st.pkt_tx);
    tl_after_seal(t, L);
}

/* Paket an den Mitspieler ueber den gewaehlten Weg (path 1 direkt, 2 Relay relay_idx). W395: auf dem aktiven Weg
 * waehrend eines Wechsels bzw. bei verdaechtigem Weg zusaetzlich ueber den zweiten Weg (der Empfaenger entdoppelt). */
void tl_send_link(loctun *t, tlink *L, int path, const uint8_t *pt, int len)
{
    int slot;
    if (path == 1) slot = 0;
    else if (path == 2 && L->relay_idx >= 0) slot = 1 + L->relay_idx;
    else return;
    tl_send_slot(t, L, slot, pt, len);
    if (L->rt_on && path == L->path) {
        int d = locroute_dual(&L->rt, tl_now(t));
        if (d >= 0 && d != slot) tl_send_slot(t, L, d, pt, len);
    }
}

tlink *tl_link_by_cid(loctun *t, uint32_t cid, int create)
{
    int i, fr = -1;
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        if (t->links[i].used && t->links[i].cid == cid) return &t->links[i];
        if (!t->links[i].used && fr < 0) fr = i;
    }
    if (!create || fr < 0) return NULL;
    memset(&t->links[fr], 0, sizeof t->links[fr]);
    t->links[fr].used = 1;
    t->links[fr].cid = cid;
    t->links[fr].relay_idx = -1;
    t->links[fr].want_ri = -1;
    locroute_init(&t->links[fr].rt); /* W395 */
    t->links[fr].lsn = INVALID_SOCKET;
    t->links[fr].srtt = 0;
    t->links[fr].red = t->c.red_fixed > 0 ? t->c.red_fixed : 3;
    return &t->links[fr];
}

tlink *tl_link_by_vip(loctun *t, uint32_t vip)
{
    int i;
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        if (t->links[i].used && t->links[i].up && t->links[i].vip == vip) return &t->links[i];
    }
    return NULL;
}

double tl_rto(const tlink *L)
{
    double r;
    if (L->rtt_n == 0) return 200.0;
    r = L->srtt + 4.0 * L->rttvar;
    if (r < 30.0) r = 30.0;
    if (r > 1000.0) r = 1000.0;
    return r;
}

void tl_rtt_sample(tlink *L, double s)
{
    if (s < 0) s = 0;
    if (s > 30000) return;
    if (L->rtt_n == 0) {
        L->srtt = s;
        L->rttvar = s / 2;
        L->rtt_min = s;
    } else {
        L->rttvar = 0.75 * L->rttvar + 0.25 * fabs(L->srtt - s);
        L->srtt = 0.875 * L->srtt + 0.125 * s;
        if (s < L->rtt_min) L->rtt_min = s;
    }
    L->rtt_n++;
}

int tl_red(loctun *t, const tlink *L)
{
    int n;
    if (t->c.red_fixed > 0) return t->c.red_fixed;
    /* Kopien kosten bei den kleinen Lockstep-Rahmen kaum etwas (0x34: 26 B) und gehen nur hinaus, solange
     * die Nachricht unbestaetigt ist (LAN: Bestaetigung vor der ersten Kopie). */
    if (L->loss_peer < 5) n = 3;
    else if (L->loss_peer < 20) n = 4;
    else if (L->loss_peer < 50) n = 6;
    else n = 8;
    if (n > t->c.red_max) n = t->c.red_max;
    if (n < 1) n = 1;
    return n;
}

/* ====================================================================== Stroeme (Spiel-TCP) */
void tl_sock_events(loctun *t, SOCKET s, long ev)
{
    WSAEventSelect(s, t->ev_net, ev);
}

void tl_nodelay(SOCKET s)
{
    BOOL on = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);
}

int tl_queue_msg(loctun *t, tlink *L, int sub, uint16_t stream, const uint8_t *data, int len)
{
    tmsg *m;
    if (L->snd_next - L->snd_una >= TUN_SQ) return 0;
    m = (tmsg *)malloc(sizeof *m + (size_t)(len > 0 ? len : 1));
    if (!m) return 0;
    memset(m, 0, sizeof *m);
    m->seq = L->snd_next;
    m->sub = (uint8_t)sub;
    m->stream = stream;
    m->len = (uint16_t)len;
    if (len > 0) memcpy(m->data, data, (size_t)len);
    L->sq[L->snd_next % TUN_SQ] = m;
    L->snd_next++;
    L->sq_bytes += len;
    InterlockedIncrement(&L->s_new);
    InterlockedIncrement(&t->st.msg_new);
    (void)t;
    return 1;
}

tstream *tl_stream_new(loctun *t, int link, uint16_t id, SOCKET s)
{
    int i;
    for (i = 0; i < TUN_MAX_STREAMS; i++) {
        if (!t->streams[i].used) {
            tstream *st = &t->streams[i];
            lguard_tcp *g = (lguard_tcp *)malloc(sizeof *g); /* W334: jeder Strom zum Spiel durch den Rahmenfilter */
            if (!g) return NULL;
            lguard_tcp_init(g);
            memset(st, 0, sizeof *st);
            st->g = g;
            st->used = 1;
            st->link = link;
            st->id = id;
            st->s = s;
            InterlockedIncrement(&t->st.streams);
            return st;
        }
    }
    return NULL;
}

static tstream *tl_stream_find(loctun *t, int link, uint16_t id)
{
    int i;
    for (i = 0; i < TUN_MAX_STREAMS; i++) {
        if (t->streams[i].used && t->streams[i].link == link && t->streams[i].id == id) return &t->streams[i];
    }
    return NULL;
}

void tl_stream_free(loctun *t, tstream *st)
{
    if (st->s != INVALID_SOCKET) {
        WSAEventSelect(st->s, NULL, 0);
        closesocket(st->s);
    }
    free(st->ob);
    if (st->g) {
        SecureZeroMemory(st->g, sizeof *st->g); /* unzugestellte Bytes des Mitspielers */
        free(st->g);
    }
    memset(st, 0, sizeof *st);
    st->s = INVALID_SOCKET;
    (void)t;
}

void tl_stream_flush(loctun *t, tstream *st)
{
    while (st->olen > 0 && !st->connecting && st->s != INVALID_SOCKET) {
        int r = send(st->s, (const char *)st->ob, st->olen, 0);
        if (r > 0) {
            memmove(st->ob, st->ob + r, (size_t)(st->olen - r));
            st->olen -= r;
            continue;
        }
        if (WSAGetLastError() == WSAEWOULDBLOCK) return;
        {
            /* W409: Verbindung zum Spiel weg - einmal je Mitspieler sichtbar machen und dem Mitspieler das Ende melden
             * (vorher blieb sein Strom halb offen: kein S_FIN, Eintrag nie frei) */
            tlink *L = &t->links[st->link];
            int e = WSAGetLastError();
            if (!(L->bad_logged & 256)) {
                L->bad_logged |= 256;
                tl_logf(t, "LOCTUN: Strom %u (c%u): Senden an das eigene Spiel gescheitert (Fehler %d) - Strom beendet",
                        st->id, L->cid, e);
            }
            if (!st->fin_sent) {
                tl_queue_msg(t, L, S_FIN, st->id, NULL, 0);
                st->fin_sent = 1;
            }
            st->wr_shut = 1;
        }
        st->olen = 0; /* Verbindung zum Spiel weg */
        st->rd_closed = 1;
        break;
    }
    if (st->fin_rx && st->olen == 0 && !st->wr_shut && !st->connecting && st->s != INVALID_SOCKET) {
        shutdown(st->s, SD_SEND);
        st->wr_shut = 1;
    }
    if (st->wr_shut && st->rd_closed) {
        tl_stream_free(t, st);
    }
}

/* W329: Obergrenze des Puffers zum eigenen Spiel. Liest das Spiel nicht, darf ein Mitspieler den Speicher
 * nicht unbegrenzt fuellen (Spielrahmen sind <= 0x100C Byte; 1 MiB ist reichlich). 0 = voll/kein Speicher. */
#define TUN_OB_MAX (1024 * 1024)
static int tl_stream_append(tstream *st, const uint8_t *d, int len)
{
    if (len < 0 || st->olen + len > TUN_OB_MAX) return 0;
    if (st->olen + len > st->ocap) {
        int nc = st->ocap ? st->ocap * 2 : 8192;
        uint8_t *nb;
        while (nc < st->olen + len) nc *= 2;
        nb = (uint8_t *)realloc(st->ob, (size_t)nc);
        if (!nb) return 0;
        st->ob = nb;
        st->ocap = nc;
    }
    memcpy(st->ob + st->olen, d, (size_t)len);
    st->olen += len;
    return 1;
}

/* W334: Verbindung zu einem Mitspieler abbauen (Stroeme zu, Stellvertreter weg, Schluessel geloescht). Der Eintrag
 * bleibt als "gesperrt" stehen, bis ein neues peer_info (neues pair_nonce) kommt bzw. die Lobby ihn entfernt. */
void tl_link_drop(loctun *t, tlink *L, const char *why)
{
    int i, li = (int)(L - t->links);
    uint32_t s;
    if (!L->used) return;
    nr_on_link_drop(t, L, why); /* W521 (#118): Grund im Netzbericht (nur im Match, nur Zaehler) */
    if (L->e2e.valid && !L->blocked && L->path) {
        uint8_t bye[1];
        bye[0] = T_BYE;
        tl_send_link(t, L, L->path, bye, 1); /* BYE (authentisch), dann Schluessel weg */
    }
    for (i = 0; i < TUN_MAX_STREAMS; i++) {
        if (t->streams[i].used && t->streams[i].link == li) tl_stream_free(t, &t->streams[i]);
    }
    if (L->lsn != INVALID_SOCKET) {
        WSAEventSelect(L->lsn, NULL, 0);
        closesocket(L->lsn);
        L->lsn = INVALID_SOCKET;
    }
    for (i = 0; i < L->ndpx; i++) closesocket(L->dpx[i].s);
    L->ndpx = 0;
    for (s = 0; s < TUN_SQ; s++) {
        free(L->sq[s]);
        L->sq[s] = NULL;
    }
    for (s = 0; s < TUN_RQ; s++) {
        free(L->rq[s]);
        L->rq[s] = NULL;
    }
    sbe2e_link_free(&L->e2e);
    L->up = 0;
    L->path = 0;
    L->want = 0;
    L->punching = 0;
    L->direct_ok = 0;
    L->seen_ok = 0;
    L->repunch_at = 0; /* W521 */
    L->repunch_n = 0;
    L->d4_pend_ok = 0;
    L->punch_failed_once = 0;
    L->late_direct = 0;
    memset(L->rb, 0, sizeof L->rb); /* W395 */
    L->rt_on = 0;
    locroute_init(&L->rt);
    L->blocked = 1;
    L->drop_pending = 0;
    tl_logf(t, "LOCTUN: Verbindung zu c%u beendet (%s)", L->cid, why);
}

/* W464 NET-CORE: aus lb_on_peer_info ausgeschnitten (unveraendert). Paar mit einem Mitspieler bilden, sobald dessen
 * Identitaet bekannt ist: Schluessel je Paar aus X25519 + Transkript (SECURITY.md §4.3), sid-Kollision mit einer
 * anderen eigenen Verbindung, Probe-ID, Adressen fuer den Durchstich. Online liefert der Treffpunkt die Felder
 * (peer_info); Einhaengepunkt fuer einen LAN-Weg ohne Treffpunkt (W454), dessen Entdeckung dieselben Felder liefert.
 * Meldet nichts an den Treffpunkt (link_report macht der Aufrufer). Rueckgabe TL_PAIR_* (bei Fehler L->blocked). */
int tl_link_pair(loctun *t, tlink *L, uint32_t peer, uint32_t lobby, const uint8_t pn[16], const uint8_t pub[32],
                 uint32_t acct, const char *pilot, const struct sockaddr_in *cand, int ncand)
{
    uint8_t th[32];
    sbe2e_side me, pa;
    int i;
    memset(&me, 0, sizeof me);
    memset(&pa, 0, sizeof pa);
    me.cid = t->my_cid;
    me.account = t->acct;
    strcpy_s(me.pilot, sizeof me.pilot, t->my_pilot);
    memcpy(me.pub, t->my_pub, 32);
    pa.cid = peer;
    pa.account = acct;
    strcpy_s(pa.pilot, sizeof pa.pilot, pilot);
    memcpy(pa.pub, pub, 32);
    memcpy(L->pair_nonce, pn, 16);
    memcpy(L->peer_pub, pub, 32);
    L->account = acct;
    L->info_at = tl_now(t); /* W353 */
    strcpy_s(L->pilot, sizeof L->pilot, pilot);
    i = sbe2e_transcript(th, lobby, pn, &me, &pa);
    if (i == 0 && t->lan) llan_th_mix(th, t->lan_psk); /* W454: LAN - Transkript an den Raumschluessel gebunden (§36) */
    if (i != 0 ||
        sbe2e_derive(&t->kp, pub, th, t->my_cid, peer, &L->e2e) != 0) {
        L->blocked = 1;
        tl_logf(t, "LOCSEC: Schluessel fuer c%u abgelehnt (ungueltiger Schluessel/Identitaet) - keine Verbindung", L->cid);
        SecureZeroMemory(th, sizeof th);
        return TL_PAIR_KEY;
    }
    SecureZeroMemory(th, sizeof th);
    for (i = 0; i < TUN_MAX_LINKS; i++) { /* sid-Kollision mit einer anderen eigenen Verbindung (~1e-8) */
        tlink *o = &t->links[i];
        if (o != L && o->used && o->e2e.valid && o->e2e.sid == L->e2e.sid) {
            sbe2e_link_free(&L->e2e);
            L->blocked = 1;
            tl_logf(t, "LOCSEC: Kennung fuer c%u doppelt - Verbindung abgelehnt", L->cid);
            return TL_PAIR_SID;
        }
    }
    if (sbe2e_random(&L->probe_id, 8) != 0 || L->probe_id == 0) {
        sbe2e_link_free(&L->e2e);
        L->blocked = 1;
        return TL_PAIR_RAND;
    }
    memcpy(L->cand, cand, sizeof L->cand[0] * TUN_MAX_CAND);
    L->ncand = ncand;
    /* W521: Budget und Zustand des Durchstichs gelten je Paarung */
    memset(L->probes_tx, 0, sizeof L->probes_tx);
    L->refl_n = 0;
    L->seen_any = 0;
    L->repunch_at = 0;
    L->repunch_n = 0;
    L->d4_pend_ok = 0;
    L->punch_failed_once = 0;
    L->late_direct = 0;
    L->punch_hold_until = 0;
    if (!L->direct_ok && !t->c.nodirect) {
        L->direct_failed = 0;
        tl_punch_start(t, L, 1, tl_now(t));
        if (t->c.punch_delay_ms > 0) { /* TEST: Durchstich beginnt spaeter (Zeitplan zaehlt ab dann) */
            L->punch_hold_until = L->punch_start + t->c.punch_delay_ms;
            L->punch_start = L->punch_hold_until;
        }
    } else if (t->c.nodirect) {
        L->direct_failed = 1;
        L->rep_dirty = 1;
    }
    return TL_PAIR_OK;
}

/* W334: Gastfilter-Verstoss (SECURITY.md §6.1): einmal je Grund protokollieren (ohne Inhalte), trennen. */
static void tl_guard_kill(loctun *t, tlink *L, int why)
{
    InterlockedIncrement(&t->st.guard);
    if (!L->used || L->blocked) return;
    tl_logf(t, "LOCGUARD: c%u Verstoss %s - Verbindung getrennt", L->cid, lguard_reason(why));
    tl_link_drop(t, L, "Gastfilter");
}

/* W409 LOC-MESH-KICK: Strom vom Mitspieler aufgeben - S_FIN an ihn, Eintrag frei. */
void tl_stream_give_up(loctun *t, tstream *st)
{
    tlink *L = &t->links[st->link];
    if (!st->fin_sent) {
        tl_queue_msg(t, L, S_FIN, st->id, NULL, 0);
        st->fin_sent = 1;
    }
    tl_stream_free(t, st);
}

/* W409: Verbindung zum eigenen Spiel (neu) aufbauen, Absender = vip des Mitspielers. 1 = laeuft bzw. steht,
 * 0 = abgelehnt (*err, z. B. WSAECONNREFUSED: Spiel lauscht nicht), -1 = kein Socket (*err). */
int tl_stream_connect(loctun *t, tstream *st, int *err)
{
    tlink *L = &t->links[st->link];
    struct sockaddr_in a;
    char ipb[16];
    u_long nb = 1;
    SOCKET s;
    *err = 0;
    if (t->c.test_refuse_ms > 0 && tl_now(t) - st->t_open < (double)t->c.test_refuse_ms) {
        *err = WSAECONNREFUSED; /* W409 TEST: wie ein Spiel, das TCP 1000 (noch) nicht lauscht */
        return 0;
    }
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        *err = WSAGetLastError();
        return -1;
    }
    tl_nodelay(s);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = L->vip;
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 && !(L->bad_logged & 8192)) {
        L->bad_logged |= 8192; /* W409: einmal je Mitspieler (Text unveraendert) */
        tl_logf(t, "LOCTUN: Strom %u: bind %s fehlgeschlagen (%d)", st->id, locnet_ip_str(L->vip, ipb), WSAGetLastError());
    }
    ioctlsocket(s, FIONBIO, &nb);
    tl_sock_events(t, s, FD_READ | FD_WRITE | FD_CONNECT | FD_CLOSE);
    a.sin_addr.s_addr = t->n->self_ip;
    a.sin_port = htons(locnet_port_out(t->n, t->n->self_ip, 1000));
    st->s = s;
    st->connecting = 1;
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
        st->connecting = 0;
    } else if (WSAGetLastError() != WSAEWOULDBLOCK) {
        *err = WSAGetLastError();
        if (!(L->bad_logged & 32)) {
            L->bad_logged |= 32; /* W409: einmal je Mitspieler (Text unveraendert) */
            tl_logf(t, "LOCTUN: Strom %u: connect an das eigene Spiel fehlgeschlagen (%d)", st->id, *err);
        }
        WSAEventSelect(s, NULL, 0);
        closesocket(s);
        st->s = INVALID_SOCKET;
        st->connecting = 0;
        return 0;
    }
    return 1;
}

/* W409: Zustand des eigenen Spiels fuer die Diagnosezeilen (keine Adressen): lauscht es auf TCP 1000, Lobby-Zustand
 * G+0x15 (vom Spiel-Thread alle 8 Bilder gemeldet, W373; "?" = unbekannt). */
const char *tl_game_info(loctun *t, char *b, size_t cap)
{
    LONG w = t->n->pub_w[2];
    if (w & 0x1000000)
        _snprintf_s(b, cap, _TRUNCATE, "Spiel lauscht %s, Lobby-Zustand 0x%02X", t->n->tcp_listen ? "ja" : "nein",
                    (unsigned)(w & 0xFF));
    else
        _snprintf_s(b, cap, _TRUNCATE, "Spiel lauscht %s, Lobby-Zustand ?", t->n->tcp_listen ? "ja" : "nein");
    return b;
}

/* W409: Das eigene Spiel nahm den Strom des Mitspielers nicht an (connect abgelehnt: lauscht nicht auf TCP 1000).
 * Das Original wiederholt einen abgelehnten connect (FUN_0004FD80: select meldet Ausnahme -> Socket zu,
 * FUN_000562C0 verbindet neu); beim Mitspieler steht die Verbindung zum Stellvertreter aber schon, ein S_FIN wuerde
 * ihm als Verbindungsreset gemeldet (W350) und sein Spiel entfernt uns. Daher den Strom kurz halten und neu
 * verbinden (alle 200 ms, hoechstens open_hold_ms ab OPEN), danach S_FIN wie bisher. Puffer bleibt begrenzt
 * (TUN_OB_MAX, Gastfilter je Rahmen), Strom-Tabelle ebenso (TUN_MAX_STREAMS). */
void tl_stream_refused(loctun *t, tstream *st, int err)
{
    tlink *L = &t->links[st->link];
    double now = tl_now(t), age = now - st->t_open;
    char gi[64];
    if (st->s != INVALID_SOCKET) {
        WSAEventSelect(st->s, NULL, 0);
        closesocket(st->s);
        st->s = INVALID_SOCKET;
    }
    st->connecting = 0;
    if (st->peer && !st->tx_any && !st->fin_rx && !L->blocked && L->up && age + 200.0 <= (double)t->c.open_hold_ms) {
        st->held = 1;
        st->tries++;
        st->retry_at = now + 200.0;
        if (!(L->bad_logged & 64)) {
            L->bad_logged |= 64;
            tl_logf(t, "LOCTUN: Strom %u von c%u: eigenes Spiel nimmt TCP 1000 nicht an (Fehler %d, %s) - Strom gehalten, "
                       "neuer Versuch bis %d ms",
                    st->id, L->cid, err, tl_game_info(t, gi, sizeof gi), t->c.open_hold_ms);
        }
        return;
    }
    if (!(L->bad_logged & 4096)) {
        L->bad_logged |= 4096;
        tl_logf(t, "LOCTUN: Strom %u von c%u: eigenes Spiel nahm TCP 1000 nicht an (Fehler %d, %d Versuche, %.0f ms, %s) - "
                   "Strom geschlossen (S_FIN)",
                st->id, L->cid, err, st->tries + 1, age, tl_game_info(t, gi, sizeof gi));
    }
    tl_stream_give_up(t, st);
}

/* OPEN vom Mitspieler: Verbindung zum eigenen Spiel aufbauen, Absender = vip des Mitspielers. */
static void tl_stream_open_remote(loctun *t, int li, uint16_t id, uint16_t dst_gp)
{
    tlink *L = &t->links[li];
    tstream *st;
    int r, err;
    if (tl_stream_find(t, li, id)) return;
    if (dst_gp != 1000) {
        /* W329: Der Mitspieler bestimmt den Zielport. Das Spiel nutzt nur TCP 1000 (tl_accept); jeder andere Port
         * wuerde eine TCP-Verbindung zu beliebigen lokalen Diensten (0.0.0.0 ist ueber die 127.x-Adresse
         * erreichbar) tunneln. Ablehnen und den Strom beim Mitspieler schliessen. */
        InterlockedIncrement(&t->st.bad);
        if (!(L->bad_logged & 1)) {
            L->bad_logged |= 1;
            tl_logf(t, "LOCTUN: Strom %u von c%u an Port %u abgelehnt (nur Spiel-TCP 1000)", id, L->cid, dst_gp);
        }
        tl_queue_msg(t, L, S_FIN, id, NULL, 0);
        return;
    }
    st = tl_stream_new(t, li, id, INVALID_SOCKET);
    if (!st) {
        /* W409: vorher still verworfen (der Strom des Mitspielers blieb offen) - jetzt S_FIN und einmal sichtbar */
        InterlockedIncrement(&t->st.bad);
        if (!(L->bad_logged & 16)) {
            L->bad_logged |= 16;
            tl_logf(t, "LOCTUN: Strom %u von c%u abgelehnt (Strom-Tabelle voll, %d) - S_FIN", id, L->cid, TUN_MAX_STREAMS);
        }
        tl_queue_msg(t, L, S_FIN, id, NULL, 0);
        return;
    }
    st->peer = 1;
    st->t_open = tl_now(t);
    tl_logf(t, "LOCTUN: Strom %u von c%u geoeffnet (Spielport %u)", id, L->cid, dst_gp);
    r = tl_stream_connect(t, st, &err);
    if (r < 0) {
        if (!(L->bad_logged & 16)) {
            L->bad_logged |= 16;
            tl_logf(t, "LOCTUN: Strom %u von c%u abgelehnt (kein Socket, Fehler %d) - S_FIN", id, L->cid, err);
        }
        tl_stream_give_up(t, st);
    } else if (r == 0) {
        tl_stream_refused(t, st, err);
    }
}

void tl_deliver_msg(loctun *t, int li, tmsg *m)
{
    tstream *st;
    if (t->links[li].blocked) return;
    if (m->sub == S_OPEN) {
        tl_stream_open_remote(t, li, m->stream, m->len >= 2 ? get16(m->data) : 1000);
        return;
    }
    st = tl_stream_find(t, li, m->stream);
    if (!st) {
        /* W409: Nachricht fuer einen Strom, den es hier nicht (mehr) gibt - einmal je Mitspieler sichtbar */
        tlink *L = &t->links[li];
        if (!(L->bad_logged & 1024)) {
            L->bad_logged |= 1024;
            tl_logf(t, "LOCTUN: Strom %u (c%u): %s fuer unbekannten Strom verworfen", m->stream, L->cid,
                    m->sub == S_FIN ? "Ende" : "Daten");
        }
        return;
    }
    if (m->sub == S_DATA) {
        st->rx_any = 1;
        /* W334: Gastfilter TCP (SECURITY.md §6.2) - nur ganze, gueltige Spielrahmen; Verstoss = Mitspieler trennen */
        static uint8_t gout[LGUARD_TCP_MAX + TUN_MSG];
        int gn = st->g ? lguard_tcp_feed(st->g, m->data, m->len, gout, (int)sizeof gout) : -1;
        if (gn < 0) {
            tl_guard_kill(t, &t->links[li], st->g ? st->g->bad : LGUARD_V_STATE);
            return;
        }
        if (gn > 0 && !tl_stream_append(st, gout, gn)) {
            tlink *L = &t->links[li];
            InterlockedIncrement(&t->st.bad);
            if (!(L->bad_logged & 4)) {
                L->bad_logged |= 4;
                tl_logf(t, "LOCTUN: Strom %u von c%u: Puffer zum Spiel voll (%d Byte) - Strom geschlossen", m->stream,
                        L->cid, st->olen);
            }
            if (!st->fin_sent) tl_queue_msg(t, L, S_FIN, st->id, NULL, 0);
            tl_stream_free(t, st);
            return;
        }
    } else if (m->sub == S_FIN) {
        st->fin_rx = 1;
        if (!st->peer && !st->rx_any) {
            /* W409: eigener Strom (Spiel verband sich zum Mitspieler), Gegenseite schloss ihn vor jedem Byte - das
             * Spiel sieht gleich einen Verbindungsreset (W350). Grund steht im Log der Gegenseite (LOCTUN: Strom ...). */
            tlink *L = &t->links[li];
            if (!(L->bad_logged & 2048)) {
                L->bad_logged |= 2048;
                tl_logf(t, "LOCTUN: Strom %u zu c%u: Gegenseite schloss ihn sofort (nach %.0f ms, vor Daten)", st->id,
                        L->cid, tl_now(t) - st->t_open);
            }
        }
    }
    tl_stream_flush(t, st);
}

/* ====================================================================== Datagramme (Spiel-UDP) */
static SOCKET tl_dpx(loctun *t, tlink *L, uint16_t src_gp)
{
    struct sockaddr_in a;
    int i, l = sizeof a;
    BOOL off = FALSE;
    DWORD got = 0;
    SOCKET s;
    for (i = 0; i < L->ndpx; i++) {
        if (L->dpx[i].gp == src_gp) return L->dpx[i].s;
    }
    if (L->ndpx >= 8) return INVALID_SOCKET;
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;
    WSAIoctl(s, 0x9800000Cu, &off, sizeof off, NULL, 0, &got, NULL, NULL);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = L->vip;
    a.sin_port = (src_gp >= 1000 && src_gp <= 1999) ? htons((uint16_t)(t->c.proxy_base + (src_gp - 1000))) : 0;
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        char ipb[16];
        tl_logf(t, "LOCTUN: Stellvertreter-UDP %s:%u belegt (%d)", locnet_ip_str(L->vip, ipb), ntohs(a.sin_port),
                WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }
    getsockname(s, (struct sockaddr *)&a, &l);
    L->dpx[L->ndpx].gp = src_gp;
    L->dpx[L->ndpx].local = ntohs(a.sin_port);
    L->dpx[L->ndpx].s = s;
    L->ndpx++;
    return s;
}

int tl_seen_dg(tlink *L, uint16_t id)
{
    int i;
    for (i = 0; i < L->nseen; i++) {
        if (L->seen[i] == id) return 1;
    }
    L->seen[L->seen_pos] = id;
    L->seen_pos = (L->seen_pos + 1) % TUN_SEEN;
    if (L->nseen < TUN_SEEN) L->nseen++;
    return 0;
}

void tl_deliver_dg(loctun *t, tlink *L, uint16_t src, uint16_t dst, const uint8_t *d, int len)
{
    struct sockaddr_in a;
    uint16_t hp = (dst >= 1000 && dst <= 1999) ? locnet_port_out(t->n, t->n->self_ip, dst) : dst;
    SOCKET s;
    if (L->blocked) return;
    if (t->app_fn && t->app_gp && dst == t->app_gp && src == t->app_gp) { /* W338: R2-Kanal, nie an den Gast */
        loctun_app_fn fn = t->app_fn;
        if (!lguard_bucket_take(&L->b_app, tl_now(t), TUN_DG_RATE, 2.0 * TUN_DG_RATE)) {
            InterlockedIncrement(&t->st.dg_drop);
            return;
        }
        if (fn) fn(L->vip, d, len);
        return;
    }
    if (t->app2_fn && t->app2_gp && dst == t->app2_gp && src == t->app2_gp) { /* W420: Regelkanal, nie an den Gast */
        loctun_app_fn fn = t->app2_fn;
        if (!lguard_bucket_take(&L->b_app2, tl_now(t), 20.0, 40.0)) { /* 5/s erwartet */
            InterlockedIncrement(&t->st.dg_drop);
            return;
        }
        if (fn) fn(L->vip, d, len);
        return;
    }
    if (t->chat_fn && t->chat_gp && dst == t->chat_gp && src == t->chat_gp) { /* W437: Chatkanal, nie an den Gast */
        loctun_chat_fn fn = t->chat_fn;
        t->chat_hit = 1;
        if (!lguard_bucket_take(&L->b_app3, tl_now(t), 4.0, 8.0)) { /* Chat selbst: 3 je 5 s */
            InterlockedIncrement(&t->st.dg_drop);
            return;
        }
        if (fn) fn(L->vip, L->pilot, d, len);
        return;
    }
    /* W329: Zielport bestimmt der Mitspieler. Nur Ports, die das eigene Spiel gebunden hat - sonst koennte er
     * beliebige lokale UDP-Dienste (alles auf 0.0.0.0 ist auch ueber die 127.x-Adresse erreichbar) ansprechen. */
    if (hp == 0 || !locnet_udp_bound(t->n, hp)) {
        InterlockedIncrement(&t->st.bad);
        return;
    }
    /* W334: Ratenbremse je Mitspieler und Port (60/s, Burst 120, darueber verwerfen - kein Verstoss, §6.4), dann
     * UDP-Positivliste (§6.3): Verstoss = Mitspieler trennen. */
    if (!lguard_bucket_take(&L->b_dg[dst == 1201 ? 0 : 1], tl_now(t), TUN_DG_RATE, 2.0 * TUN_DG_RATE)) {
        InterlockedIncrement(&t->st.dg_drop);
        return;
    }
    {
        int why = 0;
        if (!lguard_udp_check_live(dst, d, len, &why, t->live)) { /* W514: Chatraum-Handel nur im Live-Betrieb */
            tl_guard_kill(t, L, why);
            return;
        }
    }
    s = tl_dpx(t, L, src);
    if (s == INVALID_SOCKET) return;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = t->n->self_ip;
    a.sin_port = htons(hp);
    sendto(s, (const char *)d, len, 0, (struct sockaddr *)&a, sizeof a);
}

/* ====================================================================== W521 NETPATHS (#119)
 * Direkter Weg oefter: (1) IPv6 zuerst - jeder Client meldet seine eigene globale IPv6-Adresse ueber den bestehenden
 * IPv4-TLS-Kanal (hello "eps6"), die Partner stanzen direkt untereinander (der Treffpunkt braucht kein IPv6);
 * (2) Rueck-Stanzen: kommt ein echtes PROBE von einer unbekannten Adresse (symmetrisches NAT, andere Quelladresse),
 * stanzen wir dorthin zurueck (vorher: nur Antwort, eigener Weg blieb zu -> "mixed"); (3) Zeitplan: 3 s schnell wie
 * bisher, dann Relay anfordern, aber weiter stanzen (bis 5 s alle 100 ms, bis 15 s jede Sekunde), danach seltene
 * Hintergrund-Stoesse (20 s, 40 s, ... hoechstens 10 min) - gelingt der Weg spaeter, wechselt die Wegwahl (W395) im
 * laufenden Spiel von Relay auf direkt. Sicherheit (SECURITY.md §53): Proben sind authentisch (SBT2), nicht groesser
 * als die Antwort, je Kandidat und Paarung hart begrenzt; IPv6-Adressen nie ins Log.
 * IPv6 im sockaddr_in-Pfad: jede bekannte IPv6-Adresse bekommt eine Ersatzadresse 0.254.0.i (Port = echter Port),
 * tl_sendto schickt sie ueber t->us6. 0.0.0.0/8 ist als Ziel nie gueltig, daher kollisionsfrei. */
#define TL_PUNCH_FAIL_MS 3000.0   /* wie vor W521: danach Relay anfordern (der Beitritt wartet nicht laenger) */
#define TL_PUNCH_FAST_MS 5000.0   /* bis hier alle 100 ms */
#define TL_PUNCH_END_MS 15000.0   /* bis hier jede Sekunde */
#define TL_BURST_MS 3000.0        /* Hintergrund-Stoss: 3 s alle 250 ms */
#define TL_REPUNCH_FIRST_MS 20000.0
#define TL_REPUNCH_MAX_MS 600000.0
#define TL_REPUNCH_MAX 8          /* 20, 40, 80, 160, 320, 600, 600, 600 s */
#define TL_PROBE_CAP 200          /* Proben je Kandidat und Paarung (schnell 50 + langsam 10 + 8 Stoesse je 12 = 156) */
#define TL_V6_PREF_MS 1000.0      /* eine IPv4-Bestaetigung wartet so lange auf eine IPv6-Bestaetigung */
#define TL_REFL_MAX 8             /* neue gesehene Absenderadressen je Paarung */

int tl_is_v6(const struct sockaddr_in *a)
{
    return (ntohl(a->sin_addr.s_addr) & 0xFFFF0000u) == TUN_V6_NET;
}

static int tl_v6_eq(const struct sockaddr_in6 *a, const struct sockaddr_in6 *b)
{
    return a->sin6_port == b->sin6_port && memcmp(&a->sin6_addr, &b->sin6_addr, sizeof a->sin6_addr) == 0;
}

static int tl_v6_find(loctun *t, const struct sockaddr_in6 *a6)
{
    int i;
    for (i = 1; i < TUN_V6T; i++) {
        if (t->v6t[i].used && tl_v6_eq(&t->v6t[i].a, a6)) return i;
    }
    return -1;
}

static void tl_v6_syn(int i, uint16_t port_n, struct sockaddr_in *out)
{
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    out->sin_addr.s_addr = htonl(TUN_V6_NET | (uint32_t)i);
    out->sin_port = port_n;
}

/* 1 = Ersatzadresse (Tabellenplatz i) wird von einer Verbindung benutzt */
static int tl_v6_ref(loctun *t, int i)
{
    struct sockaddr_in s;
    int li, k;
    tl_v6_syn(i, 0, &s);
    for (li = 0; li < TUN_MAX_LINKS; li++) {
        const tlink *L = &t->links[li];
        if (!L->used) continue;
        if ((L->direct_ok && L->direct.sin_addr.s_addr == s.sin_addr.s_addr) ||
            (L->seen_ok && L->seen_from.sin_addr.s_addr == s.sin_addr.s_addr))
            return 1;
        for (k = 0; k < L->ncand; k++) {
            if (L->cand[k].sin_addr.s_addr == s.sin_addr.s_addr) return 1;
        }
    }
    return 0;
}

int tl_v6_intern(loctun *t, const struct sockaddr_in6 *a6, struct sockaddr_in *out)
{
    int i = tl_v6_find(t, a6), k, old = -1;
    double now = tl_now(t);
    if (i < 0) {
        for (k = 1; k < TUN_V6T && i < 0; k++) {
            if (!t->v6t[k].used) i = k;
        }
        for (k = 1; k < TUN_V6T && i < 0; k++) { /* voll: aeltesten unbenutzten Eintrag ersetzen */
            if (!tl_v6_ref(t, k) && (old < 0 || t->v6t[k].last < t->v6t[old].last)) old = k;
        }
        if (i < 0) i = old;
        if (i < 0) return 0;
        memset(&t->v6t[i].a, 0, sizeof t->v6t[i].a);
        t->v6t[i].a.sin6_family = AF_INET6;
        t->v6t[i].a.sin6_addr = a6->sin6_addr;
        t->v6t[i].a.sin6_port = a6->sin6_port;
        t->v6t[i].a.sin6_scope_id = a6->sin6_scope_id;
        t->v6t[i].used = 1;
    }
    t->v6t[i].last = now;
    tl_v6_syn(i, a6->sin6_port, out);
    return 1;
}

/* Taugt eine IPv6-Adresse als Kandidat? Global unicast (2000::/3) ohne Teredo, 6to4 und Doku-Bereich; ULA, Link-Local,
 * Loopback und Multicast liegen ausserhalb von 2000::/3. TEST v6_loop: nur ::1. */
int tl_v6_cand_ok(loctun *t, const struct in6_addr *a)
{
    const uint8_t *b = a->s6_addr;
    static const uint8_t loop[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    if (t->c.v6_loop) return memcmp(b, loop, 16) == 0;
    if ((b[0] & 0xE0) != 0x20) return 0;
    if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x00 && b[3] == 0x00) return 0; /* 2001::/32 Teredo */
    if (b[0] == 0x20 && b[1] == 0x02) return 0;                                 /* 2002::/16 6to4 */
    if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8) return 0; /* 2001:db8::/32 Doku */
    return 1;
}

void tl_v6_close(loctun *t)
{
    if (t->us6 != INVALID_SOCKET) {
        WSAEventSelect(t->us6, NULL, 0);
        closesocket(t->us6);
    }
    t->us6 = INVALID_SOCKET;
    t->have6 = 0;
    SecureZeroMemory(&t->my6, sizeof t->my6);
}

/* Beste eigene globale IPv6-Adresse: Schnittstelle aktiv, kein Tunnel (Teredo/ISATAP), Adresse "bevorzugt";
 * stabile Adressen (DNS-geeignet) vor temporaeren. 1 = gefunden. */
static int tl_v6_pick(loctun *t, struct in6_addr *out, int *stable)
{
    ULONG sz = 16384, r;
    IP_ADAPTER_ADDRESSES *aa = NULL, *ad;
    int best = 0, k;
    for (k = 0; k < 3; k++) {
        aa = (IP_ADAPTER_ADDRESSES *)malloc(sz);
        if (!aa) return 0;
        r = GetAdaptersAddresses(AF_INET6, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER |
                                 GAA_FLAG_SKIP_FRIENDLY_NAME, NULL, aa, &sz);
        if (r == NO_ERROR) break;
        free(aa);
        aa = NULL;
        if (r != ERROR_BUFFER_OVERFLOW) return 0;
    }
    if (!aa) return 0;
    for (ad = aa; ad; ad = ad->Next) {
        IP_ADAPTER_UNICAST_ADDRESS *u;
        if (ad->OperStatus != IfOperStatusUp || ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK || ad->IfType == IF_TYPE_TUNNEL)
            continue;
        for (u = ad->FirstUnicastAddress; u; u = u->Next) {
            const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)u->Address.lpSockaddr;
            int score;
            if (!s6 || s6->sin6_family != AF_INET6 || u->DadState != IpDadStatePreferred) continue;
            if (!tl_v6_cand_ok(t, &s6->sin6_addr)) continue;
            score = (u->Flags & IP_ADAPTER_ADDRESS_DNS_ELIGIBLE) ? 2 : 1; /* temporaere Adressen sind nicht DNS-geeignet */
            if (score > best) {
                best = score;
                *out = s6->sin6_addr;
            }
        }
    }
    free(aa);
    *stable = best == 2;
    return best > 0;
}

/* Bei jedem hello: eigene IPv6-Adresse pruefen und den Socket genau an sie binden (die Quelladresse der Proben muss
 * die gemeldete sein, sonst oeffnet die Firewall des Partners bzw. unsere eigene das falsche Loch). */
void tl_v6_refresh(loctun *t, double now)
{
    struct in6_addr a;
    struct sockaddr_in6 b;
    int stable = 1, l = sizeof t->my6, rb = 1 << 20;
    BOOL off = FALSE;
    DWORD got = 0, v6only = 1;
    u_long nb = 1;
    t->v6_chk = now;
    if (!t->c.ipv6 || t->c.punch_legacy || t->c.lan || !t->c.lobby[0] || t->c.nodirect) {
        if (t->us6 != INVALID_SOCKET) tl_v6_close(t);
        return;
    }
    if ((t->nrsim > 0 || t->sim) && !t->c.v6_loop) {
        /* TEST loc_route_sim/Netzsimulator verzoegern nur den IPv4-Socket - mit IPv6 liefe der direkte Weg unverzoegert
         * an der Simulation vorbei (Labor W494/W521 auf einem Rechner mit globalem IPv6) */
        if (!t->v6_logged) tl_logf(t, "LOCNET6: TEST Wegsimulation aktiv - IPv6 aus (Simulation nur ueber IPv4)");
        t->v6_logged = 1;
        if (t->us6 != INVALID_SOCKET) tl_v6_close(t);
        return;
    }
    memset(&a, 0, sizeof a);
    if (t->c.v6_loop) {
        a.s6_addr[15] = 1;
    } else if (!tl_v6_pick(t, &a, &stable)) {
        if (t->us6 != INVALID_SOCKET) {
            tl_v6_close(t);
            tl_logf(t, "LOCNET6: keine globale IPv6-Adresse mehr - nur IPv4");
        } else if (!t->v6_logged) {
            tl_logf(t, "LOCNET6: keine globale IPv6-Adresse - Durchstich nur ueber IPv4");
        }
        t->v6_logged = 1;
        return;
    }
    if (t->us6 != INVALID_SOCKET && memcmp(&t->my6.sin6_addr, &a, sizeof a) == 0) return; /* unveraendert */
    tl_v6_close(t);
    t->us6 = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (t->us6 == INVALID_SOCKET) return;
    setsockopt(t->us6, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&v6only, sizeof v6only);
    WSAIoctl(t->us6, 0x9800000Cu, &off, sizeof off, NULL, 0, &got, NULL, NULL); /* SIO_UDP_CONNRESET aus */
    setsockopt(t->us6, SOL_SOCKET, SO_RCVBUF, (const char *)&rb, sizeof rb);
    memset(&b, 0, sizeof b);
    b.sin6_family = AF_INET6;
    b.sin6_addr = a;
    b.sin6_port = htons(t->uport); /* gleiche Portnummer wie IPv4 (Firewall-Regeln), sonst frei */
    if (bind(t->us6, (struct sockaddr *)&b, sizeof b) != 0) {
        b.sin6_port = 0;
        if (bind(t->us6, (struct sockaddr *)&b, sizeof b) != 0) {
            tl_logf(t, "LOCNET6: IPv6-Socket nicht bindbar (%d) - nur IPv4", WSAGetLastError());
            tl_v6_close(t);
            return;
        }
    }
    getsockname(t->us6, (struct sockaddr *)&t->my6, &l);
    ioctlsocket(t->us6, FIONBIO, &nb);
    WSAEventSelect(t->us6, t->ev_net, FD_READ);
    t->have6 = 1;
    t->v6_logged = 0;
    /* W521 / SECURITY.md §53: die Adresse selbst nie ins Log (Logs gehen in Fehlerberichte) */
    tl_logf(t, "LOCNET6: eigene IPv6-Adresse als Kandidat (%s), Port %u",
            t->c.v6_loop ? "TEST Loopback" : stable ? "global, stabil" : "global, temporaer", ntohs(t->my6.sin6_port));
}

int tl_v6_hello_field(loctun *t, char *out, size_t cap)
{
    char ip[64];
    if (!t->have6 || t->us6 == INVALID_SOCKET) return 0;
    if (!inet_ntop(AF_INET6, &t->my6.sin6_addr, ip, sizeof ip)) return 0;
    return _snprintf_s(out, cap, _TRUNCATE, ",\"eps6\":[[\"%s\",%u]]", ip, ntohs(t->my6.sin6_port)) > 0;
}

/* Empfang auf dem IPv6-Socket: nur SBT2 (der Treffpunkt spricht nie IPv6). Unbekannte Absender laufen ueber den
 * Tabellenplatz 0; erst wenn das Paket authentisch war und eine Verbindung die Adresse uebernimmt (PROBE: gesehen
 * bzw. Rueck-Stanzen), bekommt sie einen festen Platz - unechter Verkehr fuellt die Tabelle nicht. */
void tl_rx_udp6(loctun *t, double now)
{
    uint8_t b[2048];
    if (t->us6 == INVALID_SOCKET) return;
    for (;;) {
        struct sockaddr_in6 from6;
        struct sockaddr_in syn;
        int fl = sizeof from6, r, i, li, k;
        r = recvfrom(t->us6, (char *)b, sizeof b, 0, (struct sockaddr *)&from6, &fl);
        if (r < 0) {
            if (WSAGetLastError() == WSAECONNRESET || WSAGetLastError() == WSAEMSGSIZE) continue;
            break;
        }
        if (r < 5 || t->netcut || fl < (int)sizeof from6 || from6.sin6_family != AF_INET6) continue;
        if (memcmp(b, "SBT2", 4) != 0) {
            InterlockedIncrement(&t->st.e2e_bad);
            continue;
        }
        i = tl_v6_find(t, &from6);
        if (i < 0) {
            i = 0;
            t->v6t[0].a = from6;
            t->v6t[0].used = 1;
        }
        tl_v6_syn(i, from6.sin6_port, &syn);
        if (t->c.nat_filter && !tl_natf_pass(t, &syn, now)) {
            t->v6t[0].used = 0;
            continue;
        }
        tl_rx_sbt(t, b, r, &syn, -1, -1, now);
        if (i == 0) { /* Platz 0 uebernommen? -> festen Platz vergeben und die Verweise umschreiben */
            struct sockaddr_in fix;
            int need = 0;
            for (li = 0; li < TUN_MAX_LINKS && !need; li++) {
                const tlink *L = &t->links[li];
                if (!L->used) continue;
                need = (L->seen_ok && sa_eq(&L->seen_from, &syn)) || (L->direct_ok && sa_eq(&L->direct, &syn));
                for (k = 0; k < L->ncand && !need; k++) need = sa_eq(&L->cand[k], &syn);
            }
            if (need && tl_v6_intern(t, &from6, &fix)) {
                for (li = 0; li < TUN_MAX_LINKS; li++) {
                    tlink *L = &t->links[li];
                    if (!L->used) continue;
                    if (sa_eq(&L->seen_from, &syn)) L->seen_from = fix;
                    if (sa_eq(&L->direct, &syn)) L->direct = fix;
                    for (k = 0; k < L->ncand; k++) {
                        if (sa_eq(&L->cand[k], &syn)) L->cand[k] = fix;
                    }
                }
            } else if (need) { /* kein Platz frei: Verweise verwerfen */
                for (li = 0; li < TUN_MAX_LINKS; li++) {
                    tlink *L = &t->links[li];
                    if (L->used && L->seen_ok && sa_eq(&L->seen_from, &syn)) L->seen_ok = 0;
                    if (L->used && L->direct_ok && sa_eq(&L->direct, &syn)) L->direct_ok = 0;
                }
            }
            t->v6t[0].used = 0;
        }
    }
}

/* ---------------------------------------------------------------- TEST: NAT-Nachbildung (Labor) */
static void tl_natf_note(loctun *t, const struct sockaddr_in *to)
{
    int i;
    double now = tl_now(t);
    for (i = 0; i < TUN_NATF; i++) {
        if (t->natf[i].at > 0 && sa_eq(&t->natf[i].a, to)) {
            t->natf[i].at = now;
            return;
        }
    }
    t->natf[t->natf_pos].a = *to;
    t->natf[t->natf_pos].at = now;
    t->natf_pos = (t->natf_pos + 1) % TUN_NATF;
}

/* nat_filter: nur Absender, an die wir in den letzten 60 s gesendet haben (port-restricted bzw. IPv6-Firewall) */
int tl_natf_pass(loctun *t, const struct sockaddr_in *from, double now)
{
    int i;
    if (!t->c.nat_filter) return 1;
    for (i = 0; i < TUN_NATF; i++) {
        if (t->natf[i].at > 0 && now - t->natf[i].at < 60000.0 && sa_eq(&t->natf[i].a, from)) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- Durchstich-Zeitplan */
static int tl_has_v6_cand(const tlink *L)
{
    int k;
    for (k = 0; k < L->ncand; k++) {
        if (tl_is_v6(&L->cand[k])) return 1;
    }
    return 0;
}

void tl_punch_start(loctun *t, tlink *L, int phase, double now)
{
    if (t->c.nodirect || L->direct_ok) return;
    L->punching = 1;
    L->punch_phase = phase;
    L->punch_start = now;
    L->punch_last = 0;
}

static void tl_direct_set(loctun *t, tlink *L, const struct sockaddr_in *a)
{
    char ipb[16];
    int v6 = tl_is_v6(a);
    L->direct = *a;
    L->direct_ok = 1;
    L->direct_failed = 0;
    L->punching = 0;
    L->d4_pend_ok = 0;
    L->repunch_at = 0;
    if (L->punch_failed_once) L->late_direct = 1;
    /* W339 (W335 I3): oeffentliche Adresse des Mitspielers nie ins Log (Logs gehen in Fehlerberichte);
     * Loopback/privat/link-local bleiben lesbar (Fehlersuche im LAN). Zeilenanfang unveraendert. W521: IPv6 nie. */
    tl_logf(t, "LOCTUN: direkter Weg zu c%u offen (%s:%u)%s%s", L->cid,
            (!v6 && locnet_ip_allowed(a->sin_addr.s_addr)) ? locnet_ip_str(a->sin_addr.s_addr, ipb) : "oeffentlich",
            ntohs(a->sin_port), v6 ? " IPv6" : "", L->late_direct ? " - spaet, Wechsel von Relay auf direkt" : "");
}

static void tl_punch_fail_note(loctun *t, tlink *L)
{
    if (L->punch_failed_once) return;
    L->punch_failed_once = 1;
    L->direct_failed = 1;
    L->rep_dirty = 1;
    if (t->c.punch_legacy) {
        tl_logf(t, "LOCTUN: direkter Weg zu c%u gescheitert (3 s) - Relay noetig", L->cid);
    } else {
        tl_logf(t, "LOCTUN: direkter Weg zu c%u gescheitert (3 s) - Relay noetig, Durchstich laeuft weiter (%s)", L->cid,
                L->seen_any ? "Proben des Partners kommen an" : "noch nichts vom Partner");
    }
}

void tl_punch_tick(loctun *t, tlink *L, double now)
{
    double age, iv, end;
    int k;
    if (!L->punching) {
        /* Hintergrund-Versuch, solange nur ein Relay steht */
        if (!t->c.punch_legacy && !t->c.nodirect && !L->direct_ok && L->punch_failed_once && L->repunch_at > 0 &&
            now >= L->repunch_at && L->ncand > 0)
            tl_punch_start(t, L, 3, now);
        if (!L->punching) return;
    }
    if (now < L->punch_hold_until) return; /* TEST punch_delay_ms */
    age = now - L->punch_start;
    if (t->c.punch_legacy) { /* Gegenprobe: wie vor W521 */
        if (age >= TL_PUNCH_FAIL_MS) {
            L->punching = 0;
            tl_punch_fail_note(t, L);
            return;
        }
        iv = 100.0;
    } else {
        if (L->d4_pend_ok && (age >= TL_V6_PREF_MS || !tl_has_v6_cand(L))) { /* IPv6 kam nicht: IPv4 nehmen */
            tl_direct_set(t, L, &L->d4_pend);
            return;
        }
        if (L->punch_phase == 1 && age >= TL_PUNCH_FAIL_MS) tl_punch_fail_note(t, L);
        end = L->punch_phase == 3 ? TL_BURST_MS : TL_PUNCH_END_MS;
        if (age >= end) {
            double d = TL_REPUNCH_FIRST_MS;
            L->punching = 0;
            tl_punch_fail_note(t, L);
            for (k = 0; k < L->repunch_n && d < TL_REPUNCH_MAX_MS; k++) d *= 2.0;
            if (d > TL_REPUNCH_MAX_MS) d = TL_REPUNCH_MAX_MS;
            if (L->repunch_n < TL_REPUNCH_MAX) {
                L->repunch_at = now + d;
                L->repunch_n++;
            } else {
                L->repunch_at = 0;
            }
            if (L->punch_phase != 3) {
                tl_logf(t, "LOCTUN: direkter Weg zu c%u nach 15 s nicht offen (%s) - bleibt Relay, neuer Versuch in %.0f s",
                        L->cid, L->seen_any ? "nur eine Richtung" : "keine Antwort", d / 1000.0);
            }
            return;
        }
        iv = L->punch_phase == 3 ? 250.0 : (age < TL_PUNCH_FAST_MS ? 100.0 : 1000.0);
    }
    if (now - L->punch_last >= iv) {
        uint8_t p[HDR + 8]; /* W334: PROBE als SBT2 (jede Sendung neuer ctr, nicht faelschbar) */
        tl_hdr(p, T_PROBE);
        memcpy(p + HDR, &L->probe_id, 8);
        for (k = 0; k < L->ncand && L->e2e.valid; k++) { /* IPv6-Kandidaten stehen vorn (lb_on_peer_info) */
            if (L->probes_tx[k] >= TL_PROBE_CAP) continue;
            L->probes_tx[k]++;
            tl_send_to(t, L, &L->cand[k], p, sizeof p);
        }
        L->punch_last = now;
    }
}

/* Authentisches PROBE vom Partner (die Antwort ist schon gesendet): Rueck-Stanzen an eine unbekannte Absenderadresse
 * (peer-reflexiv) - deren NAT hat fuer uns eine andere Zuordnung als die vom Treffpunkt gesehene. */
void tl_probe_seen(loctun *t, tlink *L, const struct sockaddr_in *from, double now)
{
    int k, known = -1;
    uint8_t p[HDR + 8];
    L->seen_any = 1;
    if (t->c.punch_legacy || L->direct_ok || t->c.nodirect || !L->e2e.valid) return;
    for (k = 0; k < L->ncand; k++) {
        if (sa_eq(&L->cand[k], from)) known = k;
    }
    if (known < 0) {
        if (L->refl_n >= TL_REFL_MAX) return;
        L->refl_n++;
        known = L->ncand < TUN_MAX_CAND ? L->ncand++ : TUN_MAX_CAND - 1; /* voll: letzten (meist LAN) ersetzen */
        L->cand[known] = *from;
        L->probes_tx[known] = 0;
        tl_logf(t, "LOCTUN: c%u erreicht uns ueber eine andere Adresse%s (NAT) - stanze dorthin zurueck", L->cid,
                tl_is_v6(from) ? " (IPv6)" : "");
    } else if (L->punching) {
        return; /* bekannte Adresse, Durchstich laeuft: der Zeitplan deckt sie ab (keine doppelte Rate) */
    }
    if (L->probes_tx[known] < TL_PROBE_CAP) {
        tl_hdr(p, T_PROBE);
        memcpy(p + HDR, &L->probe_id, 8);
        L->probes_tx[known]++;
        tl_send_to(t, L, from, p, sizeof p);
    }
    if (!L->punching) tl_punch_start(t, L, 3, now);
}

/* PROBE_ACK mit unserer probe_id (Pruefungen beim Aufrufer): Weg zu dieser Adresse bestaetigt. IPv6 hat Vorrang -
 * eine IPv4-Bestaetigung wartet bis TL_V6_PREF_MS nach Beginn auf IPv6, falls der Partner IPv6 gemeldet hat. */
void tl_probe_ack(loctun *t, tlink *L, const struct sockaddr_in *from, double now)
{
    if (t->c.punch_legacy || tl_is_v6(from) || !L->punching || !tl_has_v6_cand(L) ||
        now - L->punch_start >= TL_V6_PREF_MS) {
        tl_direct_set(t, L, from);
        return;
    }
    if (!L->d4_pend_ok) {
        L->d4_pend = *from;
        L->d4_pend_ok = 1;
    }
}
