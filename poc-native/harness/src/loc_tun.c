/* loc_tun.c - W324 LOC-NETPLAY: UDP-Transport mit Redundanz + steelnet-Anbindung (siehe loc_tun.h).
 *
 * Aufbau (ein Thread, alle Sockets mit EINEM Netz-Ereignis, dazu ein Ereignis fuer Spiel-UDP):
 *   Spiel --TCP--> Stellvertreter vip:proxy_base (Loopback) --+
 *   Spiel --UDP--> loctun_dgram (aus locnet_sendto)          --+--> Verbindung je Mitspieler --UDP--> direkt/Relay
 *   Spiel <-TCP--- Verbindung des Stellvertreters (bind vip)  <-+
 *   Spiel <-UDP--- Stellvertreter-UDP (bind vip:proxy_base+x) <-+
 * Paket "SBT1" (Little Endian): Kopf 10 B (Magic, Typ, Flags, Sitzung), DATA zusaetzlich Paketnummer,
 * kumulative Bestaetigung, Zeitstempel + Echo (RTT), gemessener Eingangsverlust, Eintraege:
 *   Strom-Nachricht: Art 1, seq u32, sub u8 (1 OPEN, 2 DATA, 3 FIN), Strom u16, Kopie u8, Laenge u16, Daten
 *   Datagramm:       Art 2, id u16, Quellport u16, Zielport u16, Kopie u8, Laenge u16, Daten
 * Jede Nachricht geht in bis zu N Paketen hinaus (Abstand >= gap_ms), danach nur noch nach RTO.
 *
 * W334 LOC-SEC-CLIENT (Protokoll v2, net/SECURITY.md): Steuerkanal TLS mit SPKI-Pin (sb_tls), jedes
 * Transportpaket als SBT2 (AES-256-GCM, Schluessel je Paar aus X25519 + Transkript, sb_e2e) - auch Durchstich
 * (PROBE/PROBE_ACK) und Relay-Nutzlast; die alten Klartextformate SBT1/SBP1 werden ohne Ausnahme verworfen.
 * Der Klartext eines SBT2-Pakets ist [u8 Typ][Rumpf wie v1 nach dem alten 10-Byte-Kopf] (Little Endian).
 * Vor jeder Uebergabe ans Spiel: Gastfilter (loc_guard) - Verstoss = Mitspieler getrennt.
 *
 * W464 NET-CORE: aufgeteilt - hier Thread-Zyklus, Zeitgeber, Sende-Fuzzer (TEST) und die oeffentlichen Funktionen;
 * gemeinsamer Zustand in loc_tun_int.h, uebrige Teile in loc_tun_link/_wire/_lobby/_pub.c. */
#include "loc_tun_int.h"

/* ====================================================================== Thread-Zyklus */
static void tl_rx_udp(loctun *t, double now)
{
    uint8_t b[2048];
    for (;;) {
        struct sockaddr_in from;
        int fl = sizeof from;
        int r = recvfrom(t->us, (char *)b, sizeof b, 0, (struct sockaddr *)&from, &fl);
        if (r < 0) {
            if (WSAGetLastError() == WSAECONNRESET || WSAGetLastError() == WSAEMSGSIZE) continue; /* W329: Uebergrosses verwerfen, weiterlesen */
            break;
        }
        if (r < 5 || t->netcut) continue; /* W366 TEST: Netz weg - Empfang verwerfen */
        if (t->nrsim && tl_rsim_cut(t, tl_rsim_find(t, &from))) continue; /* W395 TEST: Weg ausgefallen */
        if (t->c.nat_filter && !tl_natf_pass(t, &from, now)) continue;      /* W521 TEST: NAT-Nachbildung */
        if (t->lan && !locnet_ip_allowed(from.sin_addr.s_addr)) { /* W454: LAN - nur private Absender (SECURITY §36) */
            InterlockedIncrement(&t->st.bad);
            continue;
        }
        if (memcmp(b, "SBT2", 4) == 0) {
            tl_rx_sbt(t, b, r, &from, -1, -1, now);
        } else if (memcmp(b, "SBN1", 4) == 0) {
            lb_rx_sbn(t, b, r, &from, now);
        } else {
            InterlockedIncrement(&t->st.e2e_bad); /* W334: SBT1/SBP1 und alles andere ohne Ausnahme verworfen */
        }
    }
}

static void tl_accept(loctun *t, int li)
{
    tlink *L = &t->links[li];
    for (;;) {
        struct sockaddr_in a;
        int l = sizeof a;
        u_long nb = 1;
        SOCKET c = accept(L->lsn, (struct sockaddr *)&a, &l);
        tstream *st;
        uint16_t id;
        uint8_t od[4];
        if (c == INVALID_SOCKET) break;
        if (l < (int)sizeof a || a.sin_addr.s_addr != t->n->self_ip) {
            /* W334 (SECURITY.md §6.5): der Stellvertreter nimmt nur Verbindungen des eigenen Spiels an */
            closesocket(c);
            InterlockedIncrement(&t->st.bad);
            continue;
        }
        tl_nodelay(c);
        ioctlsocket(c, FIONBIO, &nb);
        id = (uint16_t)((++L->stream_ctr << 1) | (ntohl(t->n->self_ip) < ntohl(L->vip) ? 0 : 1));
        st = tl_stream_new(t, li, id, c);
        if (!st) {
            closesocket(c);
            continue;
        }
        st->t_open = tl_now(t); /* W409: Diagnose "Gegenseite schloss sofort" */
        tl_sock_events(t, c, FD_READ | FD_WRITE | FD_CLOSE);
        put16(od, 1000);
        put16(od + 2, 0);
        tl_queue_msg(t, L, S_OPEN, id, od, 4);
        tl_logf(t, "LOCTUN: Strom %u zu c%u geoeffnet (Spiel-TCP 1000)", id, L->cid);
    }
}

/* ---------- W335 NET-AUDIT: Sende-Fuzzer fuer das Spiel-Fuzzing (SECURITY.md §6.6 Punkt 2) ----------
 * TEST: verfaelscht Felder in den Rahmen, die das EIGENE Spiel an den Mitspieler sendet - Typ (Bytes 0/1), Laenge und
 * die Anzahl von 0x34 (Byte 4) bleiben, damit der Gastfilter des Empfaengers die Rahmen durchlaesst. Nur aktiv mit
 * SB_LOC_FUZZ_TX und dem Testweg ohne Treffpunkt (loc_transport_peers); im Online-Pfad nie. Log nur Typ + Feldoffset. */
static uint32_t fz_rand(loctun *t)
{
    uint32_t x = t->fz_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return t->fz_rng = x ? x : 0x9E3779B9u;
}

static uint8_t fz_byte(loctun *t, uint8_t v)
{
    static const uint8_t k[] = {0x00, 0xFF, 0x7F, 0x80, 0x0A, 0x10, 0x41, 0xFE, 0x01, 0x40};
    switch (fz_rand(t) % 5) {
    case 0: return (uint8_t)fz_rand(t);
    case 1: return (uint8_t)(v + 1);
    case 2: return (uint8_t)(v - 1);
    case 3: return v ? v : 0x41; /* Null-Ende von Zeichenketten entfernen */
    default: return k[fz_rand(t) % sizeof k];
    }
}

/* Typen mit Feldern, die der Spielcode als Index/Anzahl/Laenge/Text nutzen kann (SECURITY.md §6.6) */
static int fz_target(unsigned typ)
{
    switch (typ) {
    case 0x01: case 0x02: case 0x03: case 0x04: case 0x06: case 0x0A: case 0x0B: case 0x0D: case 0x13: case 0x1B:
    case 0x20: case 0x22: case 0x27: case 0x0E: case 0x30:
        return 1;
    default:
        return 0;
    }
}

/* W339 TEST (SB_LOC_FUZZ_RULE=1): gezielt die Felder der Inhaltsregeln Stufe 2 verletzen (Erwartung beim Empfaenger:
 * Verstoss, Mitspieler getrennt). Rueckgabe: 1 = Byte an Offset ofs ersetzen durch *v. */
static int fz_rule_byte(unsigned typ, int ofs, uint8_t *v, int udp)
{
    if (udp) {
        if ((typ == 0x03 || typ == 0x04) && ofs >= 0x10 && ofs < 0x20) return *v = 'G', 1; /* Gamertag ohne NUL */
        if (typ == 0x06 && ofs >= 0x3C && ofs < 0x4C) return *v = 'G', 1;
        return 0;
    }
    switch (typ) {
    case 0x0B: case 0x0D: if (ofs >= 0x3C && ofs < 0x4C) return *v = 'G', 1; break;
    case 0x27: if (ofs >= 0x2C && ofs < 0x2C + 23) return *v = 'N', 1; break;
    case 0x0E: case 0x30: if (ofs == 6 || ofs == 7) return *v = 0xFF, 1; break;
    case 0x2A: case 0x2B: if (ofs == 4 || ofs == 5) return *v = 0xFF, 1; break;
    case 0x20: case 0x2E: case 0x22: if (ofs >= 4 && ofs < 8) return *v = 0xFF, 1; break;
    case 0x25: if (ofs == 4) return *v = 0x7F, 1; break;
    default: break;
    }
    return 0;
}

static void fz_note(loctun *t, unsigned typ, int ofs)
{
    if (InterlockedIncrement(&t->fz_bytes) <= 4000) tl_logf(t, "LOCFUZZ: Typ 0x%02X Feld +0x%X", typ, ofs);
}

static void tl_fuzz_stream(loctun *t, tstream *st, uint8_t *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (st->fz.rem <= 0) {
            int fl;
            st->fz.h[st->fz.hl++] = b[i];
            fl = lguard_frame_len(st->fz.h, st->fz.hl);
            if (fl < 0 && st->fz.hl < 5) continue;
            if (fl <= 0) { /* eigener Strom nicht zerlegbar: Fuzzer fuer diesen Strom aus */
                st->fz.hl = 0;
                st->fz.rem = 0x7FFFFFFF;
                st->fz.mut = 0;
                continue;
            }
            st->fz.typ = (unsigned)st->fz.h[0] | ((unsigned)st->fz.h[1] << 8);
            st->fz.fofs = st->fz.hl;
            st->fz.rem = fl - st->fz.hl;
            st->fz.hl = 0;
            /* grosse Rahmen mit Index-/Anzahl-/Textfeldern (SECURITY.md §6.6) oft und dichter, Lockstep selten */
            st->fz.mut = fz_target(st->fz.typ) ? (fz_rand(t) % 2) == 0 : (fz_rand(t) % 12) == 0;
            if (t->fz_rule == 2) st->fz.mut = 0; /* W339 TEST: nur Regel-Felder */
            st->fz.pb = (fz_target(st->fz.typ) ? 12.0 : 3.0) / (double)fl;
            {
                uint8_t dummy;
                int has = 0, o;
                for (o = 2; o < fl && o < 0x60 && !has; o++) has = fz_rule_byte(st->fz.typ, o, &dummy, 0);
                st->fz.rule = t->fz_rule && has && (fz_rand(t) % 3) == 0; /* W339: jeder 3. Rahmen mit Regel-Feld */
                if (st->fz.rule) tl_logf(t, "LOCFUZZ: Regel-Treffer Typ 0x%02X", st->fz.typ);
            }
            if (st->fz.mut) InterlockedIncrement(&t->fz_frames);
            continue;
        }
        if (st->fz.rule) { /* W339 TEST: Regel-Feld verletzen */
            uint8_t v;
            if (fz_rule_byte(st->fz.typ, st->fz.fofs, &v, 0)) b[i] = v;
        }
        if (st->fz.mut && (fz_rand(t) % 1000000u) < (uint32_t)(st->fz.pb * 1000000.0)) {
            b[i] = fz_byte(t, b[i]);
            fz_note(t, st->fz.typ, st->fz.fofs);
        }
        st->fz.fofs++;
        st->fz.rem--;
    }
}

static void tl_fuzz_dgram(loctun *t, uint8_t *d, int len)
{
    int k, m;
    if (t->fz_rule && len >= 0x4C && (fz_rand(t) % 3) == 0) { /* W339 TEST: Regel-Feld im Datagramm verletzen */
        unsigned typ = (unsigned)d[0] | ((unsigned)d[1] << 8);
        int hit = 0;
        for (k = 2; k < len && k < 0x60; k++) {
            uint8_t v;
            if (fz_rule_byte(typ, k, &v, 1)) {
                d[k] = v;
                hit = 1;
            }
        }
        if (hit) tl_logf(t, "LOCFUZZ: Regel-Treffer UDP Typ 0x%02X", typ);
    }
    if (t->fz_rule == 2) return; /* W339 TEST: nur Regel-Felder */
    if (len < 3 || (fz_rand(t) % (fz_target((unsigned)d[0] | ((unsigned)d[1] << 8)) ? 2u : 8u)) != 0) return;
    InterlockedIncrement(&t->fz_dg);
    m = 1 + (int)(fz_rand(t) % (len > 64 ? 12u : 3u));
    for (k = 0; k < m; k++) {
        int pos = 2 + (int)(fz_rand(t) % (uint32_t)(len - 2));
        d[pos] = fz_byte(t, d[pos]);
        fz_note(t, (unsigned)d[0] | ((unsigned)d[1] << 8), pos);
    }
}

static void tl_stream_read(loctun *t, tstream *st)
{
    tlink *L = &t->links[st->link];
    uint8_t b[TUN_MSG];
    if (st->held) { /* W409: OPEN gehalten - Mitspieler schloss inzwischen bzw. Zeit fuer den naechsten Versuch */
        int r, err;
        if (st->fin_rx) {
            tl_stream_give_up(t, st);
            return;
        }
        if (tl_now(t) < st->retry_at) return;
        st->held = 0;
        r = tl_stream_connect(t, st, &err);
        if (r < 0) {
            tl_stream_give_up(t, st);
            return;
        }
        if (r == 0) {
            tl_stream_refused(t, st, err);
            return;
        }
    }
    if (st->connecting) {
        fd_set w, x;
        struct timeval tv = {0, 0};
        FD_ZERO(&w);
        FD_ZERO(&x);
        FD_SET(st->s, &w);
        FD_SET(st->s, &x);
        if (select(0, NULL, &w, &x, &tv) > 0) {
            if (FD_ISSET(st->s, &x)) {
                /* W409: connect ans eigene Spiel abgelehnt (vorher still S_FIN) - halten/neu versuchen bzw. sichtbar
                 * aufgeben (tl_stream_refused) */
                int err = 0, el = sizeof err;
                if (getsockopt(st->s, SOL_SOCKET, SO_ERROR, (char *)&err, &el) != 0 || err == 0) err = WSAECONNREFUSED;
                tl_stream_refused(t, st, err);
                return;
            }
            st->connecting = 0;
            if (st->tries && !(L->bad_logged & 16384)) {
                L->bad_logged |= 16384;
                tl_logf(t, "LOCTUN: Strom %u von c%u: eigenes Spiel nahm TCP 1000 nach %d Versuchen an (%.0f ms)", st->id,
                        L->cid, st->tries + 1, tl_now(t) - st->t_open);
            }
        } else {
            return;
        }
    }
    while (!st->rd_closed) {
        int r;
        if (L->snd_next - L->snd_una >= TUN_SQ - 4 || L->sq_bytes >= TUN_FLOW_BYTES) break; /* Flusskontrolle */
        r = recv(st->s, (char *)b, sizeof b, 0);
        if (r > 0) {
            st->tx_any = 1;
            if (t->fz_on) tl_fuzz_stream(t, st, b, r); /* W335 TEST */
            tl_queue_msg(t, L, S_DATA, st->id, b, r);
            continue;
        }
        if (r < 0 && WSAGetLastError() == WSAEWOULDBLOCK) break;
        if (st->peer && !st->tx_any && !(L->bad_logged & 128)) {
            /* W409: das eigene Spiel nahm den Strom an und schloss ihn vor jedem Byte (z. B. Netz-HLE verwarf die
             * Verbindung: "kein verbundener Mitspieler", bzw. das Spiel schloss seinen Lauschsocket) */
            char gi[64];
            int e = r == 0 ? 0 : WSAGetLastError();
            L->bad_logged |= 128;
            tl_logf(t, "LOCTUN: Strom %u von c%u: eigenes Spiel schloss die Verbindung sofort (nach %.0f ms, %s %d, %s)",
                    st->id, L->cid, tl_now(t) - st->t_open, r == 0 ? "Ende" : "Fehler", e, tl_game_info(t, gi, sizeof gi));
        }
        st->rd_closed = 1;
        if (!st->fin_sent) {
            tl_queue_msg(t, L, S_FIN, st->id, NULL, 0);
            st->fin_sent = 1;
        }
    }
    if (st->used) tl_stream_flush(t, st);
}

static void tl_relay_pings(loctun *t, double now)
{
    int i, all = 1;
    if (t->relays_done || t->nrelays == 0 || !t->my_cid) {
        if (t->nrelays == 0 && t->my_cid && !t->relays_done) t->relays_done = 1;
        return;
    }
    for (i = 0; i < t->nrelays; i++) {
        trelay *r = &t->relays[i];
        if (!r->resolved) continue;
        if (r->pings < 5 && now - t->relays_start >= r->pings * 100.0) {
            uint8_t p[13];
            if (r->pings == 0) locnet_random(r->nonce, 7);
            memcpy(p, "SBN1", 4);
            p[4] = SN_PING;
            memcpy(p + 5, r->nonce, 7);
            p[12] = (uint8_t)r->pings;
            r->ping_at[r->pings] = now;
            tl_sendto(t, &r->addr, p, 13);
            r->pings++;
        }
        if (r->pings < 5 || (r->nrtt < 5 && now - t->relays_start < 1500.0)) all = 0;
    }
    if (all) {
        for (i = 0; i < t->nrelays; i++) {
            trelay *r = &t->relays[i];
            double v[5], tmp;
            int a, b2;
            if (r->nrtt == 0) {
                r->ms = -1;
                continue;
            }
            memcpy(v, r->rtt, sizeof v);
            for (a = 0; a < r->nrtt; a++)
                for (b2 = a + 1; b2 < r->nrtt; b2++)
                    if (v[b2] < v[a]) { tmp = v[a]; v[a] = v[b2]; v[b2] = tmp; }
            r->ms = v[r->nrtt / 2];
            tl_logf(t, "LOCROUTE: Ping Relay %s = %.1f ms (%d/5 Antworten)", r->name, r->ms, r->nrtt);
        }
        t->relays_done = 1;
        lb_report(t);
    }
}

/* W395: Name eines Weg-Slots (Log/Overlay). */
static const char *tl_slot_name(loctun *t, int s)
{
    if (s == 0) return "direkt";
    if (s >= 1 && s <= t->nrelays) return t->relays[s - 1].name;
    return "keiner";
}

/* W395: Messwerte aller nutzbaren Wege einer Verbindung als eine Logzeile (nur Namen und Zahlen). */
static void tl_route_summary(loctun *t, tlink *L, double now, char *b, int cap)
{
    int s, o = 0;
    b[0] = 0;
    for (s = 0; s < LOCROUTE_MAXP && o >= 0 && o < cap; s++) {
        const locroute_path *p = &L->rt.p[s];
        int r;
        if (!p->avail) continue;
        if (p->n) {
            r = _snprintf_s(b + o, (size_t)(cap - o), _TRUNCATE, "%s%s %.1f/%.1f/%.0f%%%s%s", o ? ", " : "",
                            tl_slot_name(t, s), p->srtt, p->jit, 100.0 * locroute_loss(p, now),
                            locroute_alive(p, now) == 0 ? " aus" : "", s == L->rt.active ? "*" : "");
        } else {
            r = _snprintf_s(b + o, (size_t)(cap - o), _TRUNCATE, "%s%s -%s", o ? ", " : "", tl_slot_name(t, s),
                            s == L->rt.active ? "*" : "");
        }
        if (r < 0) break;
        o += r;
    }
}

/* Durchstich, Pings, HELLO, Relay-Bindung, Wegwahl, Lebenszeichen. */
static void tl_link_timers(loctun *t, tlink *L, double now)
{
    int k;
    if (L->drop_pending) tl_link_drop(t, L, "Paketgrenze 2^32 erreicht");
    if (L->blocked || !L->e2e.valid) return; /* W334: ohne Schluessel kein Paket */
    tl_punch_tick(t, L, now); /* W521: Zeitplan, IPv6 zuerst, Hintergrund-Wiederholung (loc_tun_link.c) */
    /* Ping des direkten Weges (5 Proben, 100 ms Abstand; ohne Antwort nach 1 s erneut) */
    if (L->direct_ok && L->direct_rtt <= 0) {
        if (L->dpings < 5 && now >= L->dping_next) {
            uint8_t p[HDR + 4];
            tl_hdr(p, T_PING);
            put32(p + HDR, (uint32_t)L->dpings);
            L->dping_at[L->dpings] = now;
            tl_send_to(t, L, &L->direct, p, sizeof p);
            L->dpings++;
            L->dping_next = now + 100.0;
        } else if (L->dpings >= 5 && (L->ndrtt >= 5 || now - L->dping_next > 1000.0)) {
            if (L->ndrtt == 0) {
                L->dpings = 0; /* alle verloren: neue Runde */
            } else {
                double v[5], tmp;
                int a, b2;
                memcpy(v, L->drtt, sizeof v);
                for (a = 0; a < L->ndrtt; a++)
                    for (b2 = a + 1; b2 < L->ndrtt; b2++)
                        if (v[b2] < v[a]) { tmp = v[a]; v[a] = v[b2]; v[b2] = tmp; }
                L->direct_rtt = v[L->ndrtt / 2] > 0.01 ? v[L->ndrtt / 2] : 0.01;
                L->rep_dirty = 1;
                tl_logf(t, "LOCROUTE: Ping direkt c%u = %.1f ms (%d/5 Antworten)", L->cid, L->direct_rtt, L->ndrtt);
            }
        }
    }
    /* Relay-Bindungen (W395: mit eigener Wegwahl an allen Relays, sonst am vom Treffpunkt gewaehlten) */
    for (k = 0; k < t->nrelays; k++) {
        if (!tl_bind_want(t, L, k)) continue;
        if (L->rb[k].ok && L->rt_on && 1 + k < LOCROUTE_MAXP && now - L->rt_last_ok[1 + k] > 3000.0) {
            /* gebunden, aber seit 3 s keine Antwort (Relay neu gestartet bzw. NAT-Zuordnung weg): neu binden */
            L->rb[k].ok = 0;
            L->rb[k].have_cookie = 0;
            L->rb[k].tries = 10;
            tl_logf(t, "LOCROUTE: Relay %s fuer c%u antwortet nicht - neu binden", t->relays[k].name, L->cid);
        }
        if (!L->rb[k].ok && now - L->rb[k].bind_last >= (L->rb[k].tries < 10 ? 200.0 : 1000.0)) {
            /* W334: TBIND bindet nicht mehr - das Relay antwortet mit einem Cookie an unsere echte Adresse, erst
             * TBIND_CONFIRM (Ticket + Cookie) bindet (SECURITY.md §5.2). */
            if (L->rb[k].have_cookie) {
                tl_tbind_confirm(t, L, k);
            } else {
                uint8_t p[5 + TICKET_LEN];
                memcpy(p, "SBN1", 4);
                p[4] = SN_TBIND;
                memcpy(p + 5, L->ticket, TICKET_LEN);
                tl_sendto(t, &t->relays[k].addr, p, sizeof p);
            }
            L->rb[k].bind_last = now;
            L->rb[k].tries++;
        }
    }
    /* Wegwahl umsetzen */
    {
        int np = L->path, nri = L->relay_idx;
        if (!L->rt_on && t->c.route_auto && L->peer_route && t->c.lobby[0] && L->path != 0) {
            /* W395: beide Seiten waehlen selbst - ab jetzt alle Wege messen, Start = bisheriger Weg */
            L->rt_on = 1;
            locroute_init(&L->rt);
            tl_logf(t, "LOCROUTE: c%u Wegwahl im Client (direkt + %d Relays), Start %s", L->cid, t->nrelays,
                    L->path == 1 ? "direkt" : (L->relay_idx >= 0 ? t->relays[L->relay_idx].name : "Relay"));
        }
        if (L->rt_on) {
            int s, why = 0, ready, prev;
            locroute_set_avail(&L->rt, 0, L->direct_ok && !t->c.nodirect, now);
            for (s = 0; s < t->nrelays && 1 + s < LOCROUTE_MAXP; s++) locroute_set_avail(&L->rt, 1 + s, L->rb[s].ok && tl_bind_want(t, L, s), now);
            if (L->rt.active < 0 && L->path != 0) L->rt.active = L->path == 1 ? 0 : 1 + L->relay_idx;
            for (s = 0; s < LOCROUTE_MAXP; s++) {
                unsigned short seq;
                if (locroute_probe_due(&L->rt, s, now, &seq)) {
                    uint8_t p[HDR + RP_LEN];
                    tl_hdr(p, T_PING);
                    p[HDR] = 'R';
                    p[HDR + 1] = 'T';
                    p[HDR + 2] = (uint8_t)s;
                    p[HDR + 3] = 0;
                    put16(p + HDR + 4, seq);
                    put16(p + HDR + 6, 0);
                    tl_send_slot(t, L, s, p, sizeof p);
                }
            }
            ready = L->route_seen || now - L->info_at > 6000.0; /* Treffpunkt hat die Wege (Ticket) genannt */
            prev = L->rt.active;
            s = locroute_step(&L->rt, now, ready, &why);
            if (L->rt.kick) { /* Verdacht/Ausfall: unbestaetigte Nachrichten jetzt neu (aktiv + zweiter Weg) */
                uint32_t q;
                L->rt.kick = 0;
                for (q = L->snd_una; q != L->snd_next; q++) {
                    tmsg *m = L->sq[q % TUN_SQ];
                    if (m && m->tx > 0) m->last = -1e9;
                }
            }
            if (s >= 0) {
                np = s == 0 ? 1 : 2;
                nri = s == 0 ? L->relay_idx : s - 1;
            }
            if (why) {
                char sum[256];
                tl_route_summary(t, L, now, sum, sizeof sum);
                if (why == 3) {
                    tl_logf(t, "LOCROUTE: c%u Ausfall %s -> %s (%.0f ms ohne Antwort) | %s", L->cid, tl_slot_name(t, prev),
                            tl_slot_name(t, s), L->rt.switch_ms, sum);
                } else {
                    tl_logf(t, "LOCROUTE: c%u %s %s -> %s | %s", L->cid,
                            why == 1 ? "Wahl nach Messung" : why == 2 ? "Wechsel (dauerhaft besser)" : "vorlaeufig",
                            tl_slot_name(t, prev), tl_slot_name(t, s), sum);
                }
            }
        } else if (L->want == 2) {
            if (L->want_ri >= 0 && L->rb[L->want_ri].ok) {
                np = 2;
                nri = L->want_ri;
            }
        } else if (L->want == 1 || L->cid == 0 || !t->c.lobby[0]) {
            if (L->direct_ok) {
                np = 1;
            } else if (L->want == 1 && L->direct_failed && !t->c.punch_legacy) {
                /* W521 (#119): der Treffpunkt sagt "direkt" (der Partner erreicht uns), unser eigener Durchstich ist aber
                 * gescheitert - statt ohne Weg zu bleiben (Verbindung kam nie zustande) ein gebundenes Relay nehmen */
                int r2;
                for (r2 = 0; r2 < t->nrelays && np == 0; r2++) {
                    if (L->rb[r2].ok) {
                        np = 2;
                        nri = r2;
                    }
                }
            }
        } else if (L->direct_ok && L->path == 0) {
            np = 1; /* vorlaeufig direkt, bis der Treffpunkt entscheidet */
        }
        if (np != L->path || (np == 2 && nri != L->relay_idx)) {
            L->rep_link = 1;
            if (L->path != 0 && np != L->path) InterlockedIncrement(&t->path_sw); /* W521: Wechsel direkt <-> Relay */
            if (L->path != 0 && !L->rt_on) {
                tl_logf(t, "LOCROUTE: c%u Weg %s -> %s", L->cid, L->path == 1 ? "direkt" : "Relay",
                        np == 1 ? "direkt" : "Relay");
            }
            L->path = np;
            L->relay_idx = nri;
        }
    }
    if (L->rep_dirty && t->lb_state != LB_OFF && t->relays_done && t->my_cid) {
        L->rep_dirty = 0;
        lb_report(t);
    }
    /* Weg dem Treffpunkt melden (Statistik), sobald die Verbindung steht und die RTT gemessen ist */
    if (L->rep_link && L->up && L->cid && L->rtt_n >= 4 && t->lb_s != INVALID_SOCKET && t->my_cid) {
        char b[192];
        /* W521: Zusatzfelder (aeltere Dienste ignorieren sie) - direkt: Adressfamilie, "late" = erst nach Relay;
         * Relay nach gescheitertem Durchstich: Art (oneway = Proben des Partners kamen an, silent = nichts) */
        _snprintf_s(b, sizeof b, _TRUNCATE, "{\"t\":\"link_report\",\"peer\":%u,\"kind\":\"%s\",\"rtt_ms\":%.1f%s%s%s}",
                    L->cid, L->path == 2 ? "relay" : "direct", L->srtt,
                    L->path == 1 ? (tl_is_v6(&L->direct) ? ",\"fam\":\"v6\"" : ",\"fam\":\"v4\"") : "",
                    L->path == 1 && L->late_direct ? ",\"late\":1" : "",
                    L->path == 2 && L->punch_failed_once && !L->direct_ok ? (L->seen_any ? ",\"pf\":\"oneway\"" : ",\"pf\":\"silent\"") : "");
        lb_send(t, b);
        L->rep_link = 0;
    }
    if (L->got_hello && !L->up && L->path) tl_link_up(t, L); /* W334: HELLO kam vor dem eigenen Weg */
    /* HELLO bis der Mitspieler uns kennt */
    if (L->path != 0 && (!L->got_hello || !L->peer_got) && now - L->hello_last >= 200.0) tl_hello(t, L, now);
    /* Lebenszeichen und Stille */
    if (L->up) {
        if (now - L->last_tx >= 200.0) tl_build_send(t, L, now, 1);
        if (now - L->last_rx >= 15000.0 && !L->down_logged) {
            tl_logf(t, "LOCTUN: c%u seit 15 s still", L->cid);
            L->down_logged = 1;
        } else if (now - L->last_rx < 15000.0) {
            L->down_logged = 0;
        }
    }
}

static double tl_next_wake(loctun *t, double now)
{
    double nx = now + 50.0, gap = (double)t->c.gap_ms;
    int i, j;
    if (t->c.lobby[0] || t->nrelays) nx = now + 20.0;
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        uint32_t s;
        double rto;
        if (!L->used) continue;
        if (L->punching || (L->path && !L->peer_got)) {
            if (now + 20.0 < nx) nx = now + 20.0;
        }
        if (!L->up) continue;
        rto = tl_rto(L);
        if (L->ack_pending && L->ack_due < nx) nx = L->ack_due;
        if (L->last_tx + 200.0 < nx) nx = L->last_tx + 200.0;
        for (s = L->snd_una, j = 0; s != L->snd_next && j < 64; s++, j++) {
            tmsg *m = L->sq[s % TUN_SQ];
            double d;
            if (!m) continue;
            d = m->tx == 0 ? now : (m->tx < L->red ? m->last + gap : m->last + rto);
            if (d < nx) nx = d;
        }
        for (j = 0; j < TUN_DQ; j++) {
            if (L->dq[j].used) {
                double d = L->dq[j].tx == 0 ? now : L->dq[j].last + gap;
                if (d < nx) nx = d;
            }
        }
        if (t->c.hold_ms > 0) {
            tmsg *m = L->rq[L->rcv_next % TUN_RQ];
            if (m && m->arr + t->c.hold_ms < nx) nx = m->arr + t->c.hold_ms;
        }
    }
    return nx;
}

int loctun_qos_game(int rtt_ms, int online)
{
    return (online && rtt_ms > TUN_QOS_ONLINE_MAX) ? TUN_QOS_ONLINE_MAX : rtt_ms;
}

static void tl_qos_update(loctun *t)
{
    double worst = 0;
    int i, any = 0;
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        double v;
        if (!L->used || !L->up || L->rtt_n == 0) continue;
        v = L->srtt + 4.0 * L->rttvar + (double)t->c.qos_reserve_ms;
        if (v > worst) worst = v;
        any = 1;
    }
    if (any) {
        int r = (int)(worst + 0.5);
        if (r > 4999) r = 4999;
        if (r < 1) r = 1;
        /* W494 LOC-FARPING (#103 "issues connecting to players over 300 ping"): das Spiel bildet aus RttMed/2 die
         * QoS-Klasse (FUN_00052390: < 300 ms Ping -> 2, sonst 1). Ab Klasse 1 bricht der Beitritt in Zustand 0x2D ab
         * (kein 0x14 nach 0x13, 0x2D -> 0x31 nach ~5 s; Lauf F5: gemeldet 657-713 ms, belegt), obwohl der Transport
         * die Strecke traegt. Online (Treffpunkt) daher hoechstens TUN_QOS_ONLINE_MAX melden. Die Startverzoegerung
         * des Original-Lockstep (clamp(Ping,100,5000)/50) bleibt damit bei 5 Ticks, die Regelung des Spiels geht bis
         * 10; mit eigener Eingabeschicht (R2, Standard) waehlt rbn_pick_delay nach der echten RTT. LAN unveraendert. */
        if (loctun_qos_game(r, t->c.lobby[0] != 0) != r) {
            if (!t->qos_capped) tl_logf(t, "LOCTUN: QoS fuer das Spiel auf %d ms begrenzt (gemessen %d ms; Original "
                                           "Klasse 1 ab 600 ms = Ping 300 bricht den Beitritt ab, W494)",
                                        TUN_QOS_ONLINE_MAX, r);
            t->qos_capped = 1;
            r = TUN_QOS_ONLINE_MAX;
        }
        t->n->rtt_ms = r; /* XNetQosLookup meldet das (Startverzoegerung des Spiels) */
    }
}

static void tl_write_status(loctun *t, double now)
{
    FILE *f;
    char tmp[280];
    int i, up = 0, relay = 0, stuck = 0;
    double worst = 0;
    if (!t->c.status_path[0] || now - t->status_last < 2000.0) return;
    t->status_last = now;
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        /* W353: Mitspieler bekannt, aber nach 20 s weder direkt noch ueber ein Relay erreichbar */
        if (L->used && !L->up && L->cid && !L->blocked && L->info_at > 0 && now - L->info_at > 20000.0) stuck++;
        if (!L->used || !L->up) continue;
        up++;
        if (L->path == 2) relay++;
        if (L->srtt > worst) worst = L->srtt;
    }
    _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "%s.tmp", t->c.status_path);
    if (fopen_s(&f, tmp, "w") != 0 || !f) return;
    fprintf(f, "transport=udp\nlobby=%s\nroute=%s\nroute_mode=%s\npeers=%d\nrelay_links=%d\nping_ms=%.0f\n",
            (t->lb_state == LB_LOBBY || (t->rs_susp && t->lb_joined)) ? "lobby" /* W500: Fortsetzung laeuft */
                : (t->lb_state == LB_OFF && !t->rs_susp ? (t->c.lobby[0] ? "getrennt" : "aus") : "verbinde"),
            up == 0 ? "keiner" : (relay == 0 ? "direkt" : (relay == up ? "relay" : "gemischt")),
            t->route_mode[0] ? t->route_mode : "-", up, relay, worst);
    /* W334: Sicherheitszustand und Raumcode fuer den Launcher */
    fprintf(f, "sec=%s\nlobby_code=%s\n",
            tl_sec_name(t), t->lobby_code);
    if (t->ver_state) /* W420: Versionsablehnung (Launcher-Meldung mit Link zur Release-Seite) */
        fprintf(f, "ver=%s\nneed_ver=%s\npeer_ver=%s\nown_ver=%s\n", t->ver_state == 1 ? "server" : (t->ver_state == 3 ? "lan" : "peer"), t->need_ver,
                t->peer_ver[0] ? t->peer_ver : "-", SB_NET_VERSION);
    /* W353: Fehlergrund (dns, connect, lost, code, full, rate, path; leer = keiner) - nur Zustaende, keine Adressen */
    fprintf(f, "err=%s\n", t->lb_err[0] ? t->lb_err : (t->lb_joined && stuck ? "path" : ""));
    if (t->lan) { /* W454: LAN-Modus - nur Zahlen (keine Adressen, kein Raumname) */
        int lu = 0, ln = lan_peers(t, &lu);
        fprintf(f, "mode=lan\nlan_peers=%d\nlan_up=%d\nlan_pass=%d\n", ln, lu, t->lan_has_pass);
    }
    if (t->pub) /* W373: nur Zustand und Zahlen */
        fprintf(f, "vis=public\npub=%s\npub_games=%d\npub_answered=%ld\n",
                t->pub_host ? (t->pub_started ? "host_started" : "host") : (t->lb_joined ? "joined" : "browse"),
                t->pub_ngames, t->pub_answered);
    fclose(f);
    MoveFileExA(tmp, t->c.status_path, MOVEFILE_REPLACE_EXISTING);
}

static DWORD WINAPI tl_thread(LPVOID arg)
{
    loctun *t = (loctun *)arg;
    HANDLE hs[3];
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    hs[0] = t->ev_net;
    hs[1] = t->ev_app;
    hs[2] = t->timer;
    while (!t->stop) {
        double now, nx;
        int i;
        DWORD w;
        EnterCriticalSection(&t->cs);
        now = tl_now(t);
        if (t->c.netcut_file[0] && now - t->netcut_chk >= 100.0) { /* W366 TEST: Netz kurz weg (Marker-Datei) */
            LONG cut = GetFileAttributesA(t->c.netcut_file) != INVALID_FILE_ATTRIBUTES;
            t->netcut_chk = now;
            if (cut != t->netcut) {
                t->netcut = cut;
                tl_logf(t, "LOCTUN: TEST Netz %s", cut ? "unterbrochen (alle UDP-Pakete verloren)" : "wieder da");
            }
        }
        tl_rx_udp(t, now);
        tl_rx_udp6(t, now); /* W521 */
        lb_poll(t, now);
        nr_tick(t, now); /* W496 NET-TELEMETRY (nur mit Zustimmung) */
        lan_poll(t, now); /* W454 */
        tl_relay_pings(t, now);
        for (i = 0; i < TUN_MAX_LINKS; i++) {
            if (t->links[i].used && t->links[i].lsn != INVALID_SOCKET) tl_accept(t, i);
        }
        for (i = 0; i < TUN_MAX_STREAMS; i++) {
            if (t->streams[i].used) tl_stream_read(t, &t->streams[i]);
        }
        now = tl_now(t);
        for (i = 0; i < TUN_MAX_LINKS; i++) {
            tlink *L = &t->links[i];
            int k;
            if (!L->used) continue;
            tl_link_timers(t, L, now);
            for (k = 0; k < 16 && tl_build_send(t, L, now, 0) == 2; k++) {
            }
        }
        tl_qos_update(t);
        tl_write_status(t, now);
        nx = tl_next_wake(t, tl_now(t));
        now = tl_now(t);
        LeaveCriticalSection(&t->cs);
        if (nx <= now) continue;
        if (nx - now > 100.0) nx = now + 100.0;
        if (t->timer) {
            /* Hochaufloesender Wartezeitgeber: timeBeginPeriod wirkt unter Windows 11 fuer verborgene Prozesse
             * nicht (sonst 15,6-ms-Stufen bei den Kopien und Bestaetigungen). */
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((nx - now) * 10000.0);
            SetWaitableTimer(t->timer, &due, 0, NULL, NULL, FALSE);
            w = (DWORD)(nx - now) + 20;
            WaitForMultipleObjects(3, hs, FALSE, w);
        } else {
            w = (DWORD)(nx - now);
            WaitForMultipleObjects(2, hs, FALSE, w ? w : 1);
        }
    }
    return 0;
}

/* ====================================================================== oeffentliche Funktionen */
void loctun_cfg_default(loctun_cfg *c)
{
    memset(c, 0, sizeof *c);
    c->proxy_base = 21000;
    c->red_max = 8;
    c->gap_ms = 15;
    c->qos_reserve_ms = 100;
    strcpy_s(c->room, sizeof c->room, "loc");
    strcpy_s(c->pilot, sizeof c->pilot, "Pilot");
    strcpy_s(c->role, sizeof c->role, "auto");
    c->route_auto = 1; /* W395 */
    c->open_hold_ms = 3000; /* W409 */
    c->resume = 1;          /* W500 */
    c->ipv6 = 1;            /* W521 */
}

int loctun_available(void) { return 1; }

static void tl_add_test_peers(loctun *t)
{
    char tmp[256], *ctx = NULL, *tok;
    uint32_t idx = 0;
    strncpy_s(tmp, sizeof tmp, t->c.peers, _TRUNCATE);
    for (tok = strtok_s(tmp, ",; ", &ctx); tok; tok = strtok_s(NULL, ",; ", &ctx)) {
        char host[64];
        uint16_t port;
        struct sockaddr_in a;
        tlink *L;
        if (!tl_split_hostport(tok, host, sizeof host, &port) || inet_pton(AF_INET, host, &a.sin_addr) != 1) {
            tl_logf(t, "LOCTUN: loc_transport_peers: \"%s\" ist kein ip:port - uebergangen", tok);
            continue;
        }
        if (!locnet_ip_allowed(a.sin_addr.s_addr)) {
            tl_logf(t, "LOCTUN: loc_transport_peers: nur Loopback/privat ohne Treffpunkt - uebergangen");
            continue;
        }
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        if (idx > 0) {
            /* W334: der Testweg hat EIN festes Geheimnis (SECURITY.md §4.7) -> gleiche sid; nur ein fester Mitspieler */
            tl_logf(t, "LOCTUN: loc_transport_peers: nur ein fester Mitspieler mit SBT2 - weitere uebergangen");
            break;
        }
        L = tl_link_by_cid(t, 0x80000000u | ++idx, 1);
        if (!L) break;
        L->cid = 0; /* TEST-Verbindung ohne Treffpunkt */
        {
            /* lo/hi: kleinerer Transport-Port (bei Gleichstand kleinere Adresse) ist "lo" */
            uint32_t mine = ntohl(t->bind_ip), theirs = ntohl(a.sin_addr.s_addr);
            int lo = t->uport < port || (t->uport == port && mine < theirs);
            if ((t->uport == port && mine == theirs) || sbe2e_link_test(lo, &L->e2e) != 0 ||
                sbe2e_random(&L->probe_id, 8) != 0) {
                tl_logf(t, "LOCTUN: loc_transport_peers: Testschluessel nicht moeglich - uebergangen");
                L->used = 0;
                break;
            }
        }
        L->cand[0] = a;
        L->ncand = 1;
        if (t->c.nodirect) {
            tl_logf(t, "LOCTUN: loc_transport_nodirect ohne Treffpunkt: kein Weg");
        } else {
            L->punching = 1;
            L->punch_start = tl_now(t);
        }
    }
}

loctun *loctun_start(const loctun_cfg *c, locnet *n, locnetsim *sim, loctun_log_fn log)
{
    LARGE_INTEGER f;
    struct sockaddr_in a;
    int l = sizeof a;
    BOOL off = FALSE;
    DWORD got = 0;
    u_long nb = 1;
    int rb = 1 << 20;
    char ipb[16];
    loctun *t = (loctun *)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->c = *c;
    t->n = n;
    t->sim = sim;
    t->log = log;
    t->lb_s = INVALID_SOCKET;
    t->rs_old_s = INVALID_SOCKET; /* W500 */
    t->lan_s = INVALID_SOCKET; /* W454 */
    t->us6 = INVALID_SOCKET;   /* W521 */
    QueryPerformanceFrequency(&f);
    t->qpf_ms = 1000.0 / (double)f.QuadPart;
    t->t0 = 0;
    t->t0 = tl_now(t) - 1000.0; /* Zeitstempel > 0 */
    if (t->c.route_sim[0]) tl_rsim_setup(t); /* W395 TEST */
    {
        /* W334: Start-Selbsttest der Krypto-Bausteine (SECURITY.md §4.8). Abweichung (z. B. falsche Bytereihenfolge,
         * abweichendes CNG) darf nie zu "ungeschuetzt, aber laeuft" fuehren -> Transport aus. */
        char why[96];
        if (sbe2e_selftest(why, sizeof why) != 0) {
            tl_logf(t, "LOCSEC: Krypto-Selbsttest fehlgeschlagen (%s) - Online-Spiel aus", why);
            if (t->c.status_path[0]) {
                FILE *sf;
                if (fopen_s(&sf, t->c.status_path, "w") == 0 && sf) {
                    fprintf(sf, "transport=udp\nlobby=aus\nsec=selftest_failed\n");
                    fclose(sf);
                }
            }
            free(t);
            return NULL;
        }
    }
    t->c.code[sizeof t->c.code - 1] = 0;
    t->c.spki[sizeof t->c.spki - 1] = 0;
    if (t->c.gap_ms < 1) t->c.gap_ms = 15;
    if (t->c.red_max < 1) t->c.red_max = 8;
    if (t->c.red_max > 16) t->c.red_max = 16;
    if (t->c.proxy_base < 2000 || t->c.proxy_base > 64000) t->c.proxy_base = 21000;
    n->tun_base = t->c.proxy_base; /* W373: Absender ohne Eintrag (Geister) wie Mitspieler abbilden */
    t->c.vis[sizeof t->c.vis - 1] = 0;
    t->pub = t->c.lobby[0] && !t->c.code[0] && _stricmp(t->c.vis, "public") == 0; /* W373: Code geht immer privat */
    /* W482: Live-Modus nutzt die Geister/Beitrittsmechanik von W373. W501 ONLINE-BOTH: Start immer System Link wie
     * ohne loc_live; Xbox Live erst, wenn das Spiel den Live-Fluss betritt (loctun_live_mode, pub_live_mode_tick). */
    t->live_cap = t->c.live && t->c.lobby[0];
    t->live = 0;
    t->pub0 = t->pub;
    t->pub_list_at = -1e9; /* W373: erste Suche holt die Liste sofort */
    t->pub_join = -1;
    if (t->c.lobby[0]) {
        if (!tl_split_hostport(t->c.lobby, t->lb_host, sizeof t->lb_host, &t->lb_port)) {
            tl_logf(t, "LOCLOBBY: loc_lobby=\"%s\" ist kein host:port - Treffpunkt aus", t->c.lobby);
            t->c.lobby[0] = 0;
        }
    }
    if (t->c.lobby[0]) { /* W328: Konto aus cfg.auth bzw. Umgebung SB_LOC_AUTH (danach geloescht) */
        char a[100];
        unsigned acct = 0;
        long long exp = 0;
        int off2 = 0;
        if (t->c.auth[0]) {
            strcpy_s(a, sizeof a, t->c.auth);
        } else {
            DWORD gl = GetEnvironmentVariableA("SB_LOC_AUTH", a, sizeof a);
            if (gl == 0 || gl >= sizeof a) a[0] = 0; /* W329: zu lang = Puffer ungefuellt (nicht terminiert) */
        }
        SetEnvironmentVariableA("SB_LOC_AUTH", NULL);
        SecureZeroMemory(t->c.auth, sizeof t->c.auth);
        if (a[0]) {
            if (sscanf_s(a, "%u:%lld:%n", &acct, &exp, &off2) == 2 && off2 > 0 && acct > 0 &&
                strlen(a + off2) == 64 && unhex(a + off2, t->gkey, 32)) {
                t->acct = acct;
                t->acct_exp = exp;
                t->auth_state = 1;
                tl_logf(t, "LOCLOBBY: Anmeldung mit Konto a%u", acct);
            } else {
                tl_logf(t, "LOCLOBBY: SB_LOC_AUTH ungueltig - Gast");
            }
            SecureZeroMemory(a, sizeof a);
        }
    }
    if (t->c.lobby[0] && t->c.relays[0]) { /* zusaetzliche Relays "name=host:port,..." */
        char tmp[256], *ctx = NULL, *tok;
        strncpy_s(tmp, sizeof tmp, t->c.relays, _TRUNCATE);
        for (tok = strtok_s(tmp, ", ", &ctx); tok; tok = strtok_s(NULL, ", ", &ctx)) {
            char *eq = strchr(tok, '='), host[96];
            uint16_t port;
            if (!eq || eq == tok || !tl_split_hostport(eq + 1, host, sizeof host, &port)) {
                tl_logf(t, "LOCLOBBY: loc_relays: \"%s\" ist kein name=host:port - uebergangen", tok);
                continue;
            }
            *eq = 0;
            lb_relay_set(t, tok, host, port);
        }
    }
    /* Bindung: Loopback, solange Treffpunkt und Mitspieler lokal sind, sonst alle Schnittstellen */
    t->bind_ip = htonl(INADDR_LOOPBACK);
    if (t->c.lobby[0]) {
        struct sockaddr_in la;
        if (tl_resolve(t->lb_host, t->lb_port, &la) && !locnet_ip_allowed(la.sin_addr.s_addr)) {
            t->bind_ip = INADDR_ANY;
        } else if (tl_resolve(t->lb_host, t->lb_port, &la) && (ntohl(la.sin_addr.s_addr) >> 24) != 127) {
            t->bind_ip = INADDR_ANY; /* LAN-Treffpunkt */
        }
    } else if (t->c.peers[0] && strncmp(t->c.peers, "127.", 4) != 0) {
        t->bind_ip = INADDR_ANY;
    }
    if (t->c.lan && (t->c.lobby[0] || t->c.peers[0])) { /* W454: LAN nur ohne Treffpunkt und ohne festen Testweg */
        tl_logf(t, "LOCLAN: loc_lan mit Treffpunkt bzw. loc_transport_peers nicht moeglich - LAN aus");
        t->c.lan = 0;
    }
    if (t->c.lan) { /* W454: LAN-Modus - alle Schnittstellen (Filter: nur private Absender), TEST nur Loopback */
        lan_targets(t);
        t->bind_ip = t->lan_loop ? htonl(INADDR_LOOPBACK) : INADDR_ANY;
        t->c.allow_public = 0;
        t->c.route_auto = 0;
    }
    t->us = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (t->us == INVALID_SOCKET) {
        free(t);
        return NULL;
    }
    WSAIoctl(t->us, 0x9800000Cu, &off, sizeof off, NULL, 0, &got, NULL, NULL);
    setsockopt(t->us, SOL_SOCKET, SO_RCVBUF, (const char *)&rb, sizeof rb);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = t->bind_ip;
    a.sin_port = htons(t->c.port);
    if (bind(t->us, (struct sockaddr *)&a, sizeof a) != 0) {
        tl_logf(t, "LOCTUN: UDP-Port %u nicht verfuegbar (%d) - Transport aus", t->c.port, WSAGetLastError());
        closesocket(t->us);
        free(t);
        return NULL;
    }
    getsockname(t->us, (struct sockaddr *)&a, &l);
    t->uport = ntohs(a.sin_port);
    ioctlsocket(t->us, FIONBIO, &nb);
    InitializeCriticalSection(&t->cs);
    t->ev_net = CreateEventW(NULL, FALSE, FALSE, NULL);
    t->ev_app = CreateEventW(NULL, FALSE, FALSE, NULL);
    t->timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (!t->timer) t->timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    WSAEventSelect(t->us, t->ev_net, FD_READ);
    if (t->c.lan) { /* W454: Entdeckung (Socket meldet ueber dasselbe Ereignis) */
        if (lan_start(t) == 0) {
            WSAEventSelect(t->lan_s, t->ev_net, FD_READ);
        } else {
            t->lan = 0;
            strcpy_s(t->lb_err, sizeof t->lb_err, "lan"); /* Launcher: err=lan (Port belegt bzw. Raumname) */
        }
    }
    if (t->c.peers[0] && t->c.lobby[0]) {
        /* W335 (NET-AUDIT N1, SECURITY.md §4.7): der Testweg hat ein festes, oeffentlich bekanntes Geheimnis - mit
         * Treffpunkt (Release-Pfad, Bindung auf alle Schnittstellen) nie aktiv */
        tl_logf(t, "LOCSEC: loc_transport_peers (Testweg) mit Treffpunkt nicht erlaubt - uebergangen");
    } else if (t->c.peers[0]) {
        char fz[24];
        DWORD fl = GetEnvironmentVariableA("SB_LOC_FUZZ_TX", fz, sizeof fz);
        tl_add_test_peers(t);
        if (fl > 0 && fl < sizeof fz && atoi(fz) > 0) { /* W335 TEST: Sende-Fuzzer nur auf dem Testweg */
            t->fz_rng = (uint32_t)atoi(fz) * 2654435761u + (uint32_t)GetCurrentProcessId();
            t->fz_on = 1;
            t->fz_rule = GetEnvironmentVariableA("SB_LOC_FUZZ_RULE", fz, sizeof fz) > 0 ? atoi(fz) : 0; /* W339 TEST: 1 = mit, 2 = nur Regel-Felder */
            tl_logf(t, "LOCFUZZ: Sende-Fuzzer aktiv (TEST, Testweg ohne Treffpunkt)%s",
                    t->fz_rule ? ", Regel-Felder gezielt (W339)" : "");
        }
    }
    nr_start(t); /* W496 NET-TELEMETRY: ohne Zustimmung kein Zustand, keine Datei */
    timeBeginPeriod(1); /* Abstaende der Kopien (gap_ms) brauchen 1-ms-Wartezeiten */
    t->th = CreateThread(NULL, 0, tl_thread, t, 0, NULL);
    if (!t->th) {
        closesocket(t->us);
        CloseHandle(t->ev_net);
        CloseHandle(t->ev_app);
        DeleteCriticalSection(&t->cs);
        free(t);
        return NULL;
    }
    tl_logf(t, "LOCTUN: aktiv, UDP %s:%u, Stellvertreter-Basis %u, Redundanz %s (max %d, Abstand %d ms), Haltezeit %d ms, "
            "QoS-Reserve %d ms%s%s%s",
            locnet_ip_str(t->bind_ip, ipb), t->uport, t->c.proxy_base, t->c.red_fixed ? "fest" : "adaptiv",
            t->c.red_fixed ? t->c.red_fixed : t->c.red_max, t->c.gap_ms, t->c.hold_ms, t->c.qos_reserve_ms,
            t->c.lobby[0] ? ", Treffpunkt gesetzt" : "", t->c.peers[0] ? ", feste Mitspieler (TEST)" : "",
            t->c.nodirect ? ", direkt aus (TEST)" : "");
    return t;
}

void loctun_stop(loctun *t)
{
    int i, k;
    if (!t) return;
    t->stop = 1;
    SetEvent(t->ev_app);
    WaitForSingleObject(t->th, 5000);
    CloseHandle(t->th);
    timeEndPeriod(1);
    nr_exit(t); /* W496 */
    nr_free(t);
    for (i = 0; i < t->nrsim; i++) {
        if (t->rsim[i].sim) locnetsim_stop(t->rsim[i].sim); /* W395 TEST */
    }
    for (i = 0; i < TUN_MAX_STREAMS; i++) {
        if (t->streams[i].used) tl_stream_free(t, &t->streams[i]);
    }
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        uint32_t s;
        if (L->lsn != INVALID_SOCKET && L->used) closesocket(L->lsn);
        for (k = 0; k < L->ndpx; k++) closesocket(L->dpx[k].s);
        for (s = 0; s < TUN_SQ; s++) free(L->sq[s]);
        for (s = 0; s < TUN_RQ; s++) free(L->rq[s]);
    }
    for (i = 0; i < TUN_MAX_LINKS; i++) sbe2e_link_free(&t->links[i].e2e); /* W334 */
    lb_bye(t); /* W517: geordnetes Ende -> "bye" an den Treffpunkt (Thread steht schon) */
    if (t->tls) sbtls_free(t->tls);
    if (t->lb_s != INVALID_SOCKET) closesocket(t->lb_s);
    lb_rs_reset(t); /* W500: Token, Nachsende-Schlange, liegengelassene TEST-Verbindung */
    lan_stop(t); /* W454 */
    tl_v6_close(t); /* W521 */
    sbe2e_kp_free(&t->kp);
    SecureZeroMemory(t->udp_key, sizeof t->udp_key);
    SecureZeroMemory(t->gkey, sizeof t->gkey); /* W328 */
    closesocket(t->us);
    CloseHandle(t->ev_net);
    CloseHandle(t->ev_app);
    if (t->timer) CloseHandle(t->timer);
    DeleteCriticalSection(&t->cs);
    free(t);
}

static void tl_bye(loctun *t, int crash)
{
    int i, k, n = 0;
    DWORD t0 = GetTickCount();
    if (!t) return;
    while (!TryEnterCriticalSection(&t->cs)) { /* nie haengen: der Prozess endet ohnehin */
        if (GetTickCount() - t0 > 200u) return;
        Sleep(1);
    }
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        uint8_t bye[1];
        if (!L->used || !L->e2e.valid || L->blocked || !L->path) continue;
        for (k = 0; k < 2; k++) {
            bye[0] = T_BYE;
            tl_send_link(t, L, L->path, bye, 1);
        }
        n++;
    }
    if (n) tl_logf(t, "LOCTUN: Client endet - BYE an %d Mitspieler", n);
    if (!crash) nr_exit(t); /* W496: offener Bericht in die Warteschlange, Laufmarke weg (W519: Absturz behaelt sie) */
    lb_bye(t); /* W517: auch beim Treffpunkt abmelden (keine gehaltene Sitzung, kein "Host verloren") */
    LeaveCriticalSection(&t->cs);
    if (n && t->sim) Sleep(150); /* Test-Netzmodell (loc_netsim) stellt verzoegert zu - vor dem Prozessende abwarten */
}

int loctun_is_live(const loctun *t)
{
    return t && *(volatile const int *)&t->live ? 1 : 0;
}

void loctun_bye_all(loctun *t) { tl_bye(t, 0); }

/* W519 STALLHOLD: Absturz im Netzspiel (LOCSEC "Absturz = Ende") - BYE an die Mitspieler und "bye" an den Treffpunkt,
 * damit sie den Abgang sofort sehen statt einen Stillstand bis zur Obergrenze zu halten; Laufmarke bleibt ("crash"). */
void loctun_bye_crash(loctun *t) { tl_bye(t, 1); }

int loctun_dgram(loctun *t, uint32_t dst_vip, uint16_t src_gp, uint16_t dst_gp, const char *buf, int len)
{
    int i, sent = 0;
    if (!t || len <= 0 || len > TUN_PT - DHDR - 10) {
        if (t) InterlockedIncrement(&t->st.dg_drop);
        return -1;
    }
    EnterCriticalSection(&t->cs);
    if (t->pub) { /* W373: Suche (Typ 1) beantworten die Geister; Beitritt (Typ 3) an einen Geist -> Lobby beitreten */
        unsigned typ = len >= 2 ? (unsigned)(uint8_t)buf[0] | ((unsigned)(uint8_t)buf[1] << 8) : 0;
        if (dst_vip == INADDR_BROADCAST && dst_gp == 1201 && typ == 1 && len == 0x14) {
            memcpy(t->pub_nonce, buf + 4, 8);
            t->pub_t1_at = tl_now(t);
            t->pub_t1_new = 1;
            SetEvent(t->ev_app);
        } else if (dst_vip != INADDR_BROADCAST && dst_gp == 1202 && (typ == 3 || typ == 4) && len == 0x2C4 &&
                   !tl_link_by_vip(t, dst_vip)) {
            /* W514 #112: Typ 4 = Beitrittsanfrage Chatraum (FUN_00054030; Typ 3 = Match, FUN_00053F60). Chatraeume gibt
             * es nur ueber Xbox Live -> Typ 4 nur an Live-Geister (System Link unveraendert). */
            tghost *g = pub_ghost_by_vip(t, dst_vip);
            if (g && typ == 4 && !g->live) g = NULL;
            if (g) {
                if (t->pub_join < 0 || t->pub_join == (int)(g - t->gh)) {
                    t->pub_join = (int)(g - t->gh);
                    memcpy(t->pub_t3, buf, (size_t)len);
                    t->pub_t3_len = len;
                    t->pub_t3_src = src_gp;
                }
                LeaveCriticalSection(&t->cs);
                SetEvent(t->ev_app);
                return len;
            }
        }
    }
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        int k, slot = -1;
        uint16_t dst = dst_gp;
        if (!L->used || !L->up) continue;
        if (dst_vip != INADDR_BROADCAST && L->vip != dst_vip) continue;
        if (!(dst >= 1000 && dst <= 1999)) { /* Antwort an einen rohen Stellvertreter-Port: zurueckuebersetzen */
            for (k = 0; k < L->ndpx; k++) {
                if (L->dpx[k].local == dst) dst = L->dpx[k].gp;
            }
        }
        for (k = 0; k < TUN_DQ; k++) {
            if (!L->dq[k].used) { slot = k; break; }
        }
        if (slot < 0) {
            InterlockedIncrement(&t->st.dg_drop);
            continue;
        }
        L->dq[slot].used = 1;
        L->dq[slot].id = L->dg_next++;
        L->dq[slot].src = src_gp;
        L->dq[slot].dst = dst;
        L->dq[slot].len = (uint16_t)len;
        L->dq[slot].tx = 0;
        memcpy(L->dq[slot].data, buf, (size_t)len);
        if (t->fz_on) tl_fuzz_dgram(t, L->dq[slot].data, len); /* W335 TEST */
        InterlockedIncrement(&L->s_dgtx);
        InterlockedIncrement(&t->st.dg_tx);
        sent++;
    }
    LeaveCriticalSection(&t->cs);
    if (sent) SetEvent(t->ev_app);
    return (sent || dst_vip == INADDR_BROADCAST) ? len : -1;
}

void loctun_set_app(loctun *t, uint16_t gp, loctun_app_fn fn)
{
    if (!t) return;
    t->app_fn = NULL;
    t->app_gp = gp;
    t->app_fn = fn;
}

int loctun_lan_state(loctun *t, int *peers, int *up) /* W454 */
{
    int n = 0, u = 0, on;
    if (peers) *peers = 0;
    if (up) *up = 0;
    if (!t) return 0;
    EnterCriticalSection(&t->cs);
    on = t->lan;
    if (on) n = lan_peers(t, &u);
    LeaveCriticalSection(&t->cs);
    if (peers) *peers = n;
    if (up) *up = u;
    return on;
}

int loctun_version_state(loctun *t, char *need, int ncap, char *peer, int pcap)
{
    int st;
    if (!t) return 0;
    EnterCriticalSection(&t->cs);
    st = (int)t->ver_state;
    if (need && ncap > 0) strncpy_s(need, (size_t)ncap, t->need_ver, _TRUNCATE);
    if (peer && pcap > 0) strncpy_s(peer, (size_t)pcap, t->peer_ver, _TRUNCATE);
    LeaveCriticalSection(&t->cs);
    return st;
}

void loctun_set_app2(loctun *t, uint16_t gp, loctun_app_fn fn)
{
    if (!t) return;
    t->app2_fn = NULL;
    t->app2_gp = gp;
    t->app2_fn = fn;
}

void loctun_set_chat(loctun *t, uint16_t gp, loctun_chat_fn fn) /* W437 */
{
    if (!t) return;
    t->chat_fn = NULL;
    t->chat_gp = gp;
    t->chat_fn = fn;
}

void loctun_set_snchat(loctun *t, loctun_snchat_fn fn, int want) /* W446 */
{
    if (!t) return;
    t->sn_fn = fn;
    InterlockedExchange(&t->sn_want, fn && want ? 1 : 0);
    SetEvent(t->ev_app);
}

int loctun_snchat_state(loctun *t) /* W446 */
{
    return t ? (int)t->sn_state : 0;
}

int loctun_snchat_send(loctun *t, const char *json, int len) /* W446 */
{
    int ok = 0;
    if (!t || !json || len <= 0 || len >= LOCTUN_SN_LINE) return 0;
    EnterCriticalSection(&t->cs);
    if (t->sn_state == 2 && t->sn_out_n < 4) {
        memcpy(t->sn_out[t->sn_out_n], json, (size_t)len);
        t->sn_out[t->sn_out_n][len] = 0;
        t->sn_out_len[t->sn_out_n] = len;
        t->sn_out_n++;
        ok = 1;
    }
    LeaveCriticalSection(&t->cs);
    if (ok) SetEvent(t->ev_app);
    return ok;
}

int loctun_my_pilot(loctun *t, char *buf, int cap) /* W437 */
{
    int r;
    if (!buf || cap <= 0) return 0;
    buf[0] = 0;
    if (!t || (!t->c.lobby[0] && !t->lan)) return 0; /* W454: auch im LAN-Modus */
    EnterCriticalSection(&t->cs);
    strncpy_s(buf, (size_t)cap, t->my_pilot, _TRUNCATE);
    LeaveCriticalSection(&t->cs);
    r = buf[0] != 0;
    return r;
}

int loctun_my_gamertag(loctun *t, char *buf, int cap) /* W487 LIVE-GAMERTAG */
{
    if (!buf || cap <= 0) return 0;
    buf[0] = 0;
    if (!t || !t->c.lobby[0]) return 0;
    EnterCriticalSection(&t->cs);
    strncpy_s(buf, (size_t)cap, t->my_gamertag, _TRUNCATE);
    LeaveCriticalSection(&t->cs);
    return buf[0] != 0;
}

int loctun_peer_pilot(loctun *t, uint32_t vip, char *buf, int cap) /* W437 */
{
    tlink *L;
    int r = 0;
    if (!buf || cap <= 0) return 0;
    buf[0] = 0;
    if (!t) return 0;
    EnterCriticalSection(&t->cs);
    L = tl_link_by_vip(t, vip);
    if (L && L->pilot[0]) {
        strncpy_s(buf, (size_t)cap, L->pilot, _TRUNCATE);
        r = 1;
    }
    LeaveCriticalSection(&t->cs);
    return r;
}

int loctun_is_peer(loctun *t, uint32_t vip)
{
    int r;
    if (!t) return 0;
    EnterCriticalSection(&t->cs);
    r = tl_link_by_vip(t, vip) != NULL || (t->pub && pub_ghost_by_vip(t, vip) != NULL); /* W373: Geister */
    LeaveCriticalSection(&t->cs);
    return r;
}

int loctun_peer_stall(loctun *t, uint32_t vip, int *rx_age_ms) /* W519 */
{
    tlink *L;
    int r = 0;
    double age = 0.0;
    if (rx_age_ms) *rx_age_ms = -1;
    if (!t) return 0;
    EnterCriticalSection(&t->cs);
    L = tl_link_by_vip(t, vip);
    if (L && L->used && L->up && !L->blocked && !L->drop_pending) {
        age = tl_now(t) - L->last_rx;
        if (age < 0.0) age = 0.0;
        r = L->gone_hint ? 3 : age < 3000.0 ? 1 : 2;
    }
    LeaveCriticalSection(&t->cs);
    if (rx_age_ms && r) *rx_age_ms = age > 3600000.0 ? 3600000 : (int)age;
    return r;
}

int loctun_links_up(loctun *t)
{
    int i, n = 0;
    if (!t) return 0;
    EnterCriticalSection(&t->cs);
    for (i = 0; i < TUN_MAX_LINKS; i++) n += t->links[i].used && t->links[i].up;
    LeaveCriticalSection(&t->cs);
    return n;
}

int loctun_link_info(loctun *t, int idx, int *path, double *srtt_ms, int *red, uint32_t *vip, char *relay, int rlen)
{
    int i, k = 0, r = 0;
    EnterCriticalSection(&t->cs);
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        if (!L->used) continue;
        if (k++ != idx) continue;
        if (path) *path = L->up ? L->path : 0;
        if (srtt_ms) *srtt_ms = L->srtt;
        if (red) *red = L->red;
        if (vip) *vip = L->vip;
        if (relay && rlen > 0) {
            strncpy_s(relay, (size_t)rlen, L->relay_idx >= 0 ? t->relays[L->relay_idx].name : "", _TRUNCATE);
        }
        r = 1;
        break;
    }
    LeaveCriticalSection(&t->cs);
    return r;
}

int loctun_link_direct(loctun *t, int idx, int *direct_ok, int *v6, int *late, int *failed, int *seen)
{
    int i, k = 0, r = 0;
    if (!t) return 0;
    EnterCriticalSection(&t->cs);
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        if (!L->used) continue;
        if (k++ != idx) continue;
        if (direct_ok) *direct_ok = L->direct_ok;
        if (v6) *v6 = L->direct_ok && tl_is_v6(&L->direct);
        if (late) *late = L->late_direct;
        if (failed) *failed = L->punch_failed_once;
        if (seen) *seen = L->seen_any;
        r = 1;
        break;
    }
    LeaveCriticalSection(&t->cs);
    return r;
}

int loctun_has_v6(loctun *t)
{
    int r;
    if (!t) return 0;
    EnterCriticalSection(&t->cs);
    r = t->have6;
    LeaveCriticalSection(&t->cs);
    return r;
}

int loctun_peer_hud_get(loctun *t, int idx, loctun_peer_hud *o) /* W374 F3-Overlay */
{
    int i, k = 0, r = 0;
    if (!t || !o) return 0;
    EnterCriticalSection(&t->cs);
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        if (!L->used) continue;
        if (k++ != idx) continue;
        o->up = L->up ? 1 : 0;
        o->path = L->up ? L->path : 0;
        o->srtt = L->srtt;
        o->rtt_min = L->rtt_min;
        o->rttvar = L->rttvar;
        o->loss_in = L->loss_in;
        o->loss_peer = L->loss_peer;
        strncpy_s(o->pilot, sizeof o->pilot, L->pilot, _TRUNCATE);
        /* W395: aktiver Weg und Alternativen */
        o->route[0] = 0;
        if (L->up && L->path == 1) strcpy_s(o->route, sizeof o->route, "direct");
        else if (L->up && L->path == 2 && L->relay_idx >= 0) {
            strncpy_s(o->route, sizeof o->route, t->relays[L->relay_idx].name, _TRUNCATE);
        }
        o->nalt = 0;
        o->nswitch = L->rt.nswitch;
        o->nfail = L->rt.nfail;
        o->rt_on = L->rt_on;
        o->decided = L->rt.decided;
        if (L->rt_on) {
            double now = tl_now(t);
            int s;
            for (s = 0; s < LOCROUTE_MAXP && o->nalt < LOCTUN_HUD_ALT; s++) {
                const locroute_path *p = &L->rt.p[s];
                if (!p->avail) continue;
                strncpy_s(o->alt[o->nalt].name, sizeof o->alt[0].name, s == 0 ? "direct" : t->relays[s - 1].name,
                          _TRUNCATE);
                o->alt[o->nalt].rtt = p->n ? p->srtt : -1.0;
                o->alt[o->nalt].state = locroute_alive(p, now);
                o->alt[o->nalt].active = s == L->rt.active;
                o->nalt++;
            }
        }
        r = 1;
        break;
    }
    LeaveCriticalSection(&t->cs);
    return r;
}

const loctun_stats *loctun_get_stats(loctun *t) { return &t->st; }
uint16_t loctun_port(loctun *t) { return t->uport; }
int loctun_auth_state(loctun *t)
{
    return t ? (int)t->auth_state : 0;
}

uint32_t loctun_account(loctun *t) /* W479 */
{
    return t ? t->acct : 0;
}

int loctun_play_mode(loctun *t) /* W440 */
{
    if (!t || !t->c.lobby[0]) return 0;
    return (int)InterlockedCompareExchange(&t->play_mode, 0, 0);
}

int loctun_lobby_state(loctun *t)
{
    if (t && !t->c.lobby[0] && t->lan) return 3; /* W454: LAN-Raum gilt als Lobby (Lobby-Chat an alle im Raum) */
    if (!t || !t->c.lobby[0]) return 0;
    if (t->rs_susp) return t->lb_joined ? 3 : 2; /* W500: Fortsetzung laeuft - fuer das Spiel unveraendert */
    return t->lb_state == LB_OFF || t->lb_state == LB_CONNECTING ? 1 : (t->lb_joined ? 3 : 2);
}

int loctun_relink_state(loctun *t)
{
    return t ? (int)InterlockedCompareExchange(&t->rs_hint_pub, 0, 0) : 0; /* W517 */
}

void loctun_log_stats(loctun *t, int waited_s)
{
    int i;
    if (!t) return;
    EnterCriticalSection(&t->cs);
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        if (!L->used) continue;
        tl_logf(t,
                "LOCTUN: t=%ds c%u %s Weg=%s%s RTT=%.1f (min %.1f, var %.1f) ms Verlust ein=%d.%d%% Partner=%d.%d%% "
                "N=%d Pakete tx=%ld rx=%ld Nachrichten neu=%ld Kopien=%ld RTO=%ld gerettet=%ld doppelt=%ld "
                "Datagramme tx=%ld rx=%ld offen=%u",
                waited_s, L->cid, L->up ? "verbunden" : "getrennt",
                L->path == 1 ? "direkt" : (L->path == 2 ? "relay:" : "keiner"),
                L->path == 2 && L->relay_idx >= 0 ? t->relays[L->relay_idx].name : "", L->srtt, L->rtt_min,
                L->rttvar, L->loss_in / 10, L->loss_in % 10, L->loss_peer / 10, L->loss_peer % 10, L->red, L->s_tx,
                L->s_rx, L->s_new, L->s_copy, L->s_rto, L->s_saved, L->s_dup, L->s_dgtx, L->s_dgrx,
                L->snd_next - L->snd_una);
        if (L->rt_on) { /* W395: alle Wege (RTT/Schwankung/Verlust, * = aktiv) */
            char sum[256];
            tl_route_summary(t, L, tl_now(t), sum, sizeof sum);
            tl_logf(t, "LOCROUTE: t=%ds c%u Wege %s | Wechsel=%d Ausfall=%d", waited_s, L->cid, sum, L->rt.nswitch,
                    L->rt.nfail);
        }
    }
    if (t->fz_on) /* W335 TEST */
        tl_logf(t, "LOCFUZZ: t=%ds Rahmen verfaelscht=%ld Bytes=%ld Datagramme=%ld Gastfilter-Verstoesse=%ld", waited_s,
                t->fz_frames, t->fz_bytes, t->fz_dg, t->st.guard);
    LeaveCriticalSection(&t->cs);
}

int loctun_lobby_code(loctun *t, char *buf, int cap)
{
    if (!buf || cap <= 0) return 0;
    buf[0] = 0;
    if (!t) return 0;
    EnterCriticalSection(&t->cs);
    strncpy_s(buf, (size_t)cap, t->lobby_code, _TRUNCATE);
    LeaveCriticalSection(&t->cs);
    return buf[0] != 0;
}

/* W373 TEST: Steuerzeile ohne Netz einspeisen, dann einen Schritt der oeffentlichen Logik rechnen. */
int loctun_test_line(loctun *t, uint32_t my_cid, const char *line, char *out, int cap)
{
    double now;
    if (!t || !out || cap < 1) return -1;
    out[0] = 0;
    EnterCriticalSection(&t->cs);
    now = tl_now(t);
    t->test_cap = out;
    t->test_cap_len = 0;
    t->test_cap_max = cap;
    if (my_cid) {
        t->my_cid = my_cid;
        if (t->lb_state == LB_OFF) t->lb_state = LB_HELLO;
        if (!t->my_pilot[0]) strcpy_s(t->my_pilot, sizeof t->my_pilot, t->c.pilot[0] ? t->c.pilot : "Pilot");
    }
    if (line) lb_on_line(t, line, line + strlen(line), now);
    if (t->live_cap) pub_live_mode_tick(t, now); /* W501: wie lb_poll */
    if (t->pub && t->my_cid) pub_tick(t, now);
    if (t->my_cid) lb_snchat_tick(t); /* W446: Abonnement/Senden des steelnet-Chats */
    t->test_cap = NULL;
    LeaveCriticalSection(&t->cs);
    return t->pub;
}

int loctun_pub_games(loctun *t)
{
    int i, n = 0;
    if (!t) return 0;
    EnterCriticalSection(&t->cs);
    for (i = 0; i < TUN_MAX_GHOST; i++) n += t->gh[i].used;
    LeaveCriticalSection(&t->cs);
    return n;
}

int loctun_pub_lookup(loctun *t, const int *lid, int n, int *mode, int *est) /* W443 */
{
    int i, j, found = 0;
    if (!t || !lid || !mode || !est || n <= 0) return 0;
    EnterCriticalSection(&t->cs);
    for (i = 0; i < n; i++) {
        mode[i] = 0;
        est[i] = -1;
        for (j = 0; j < TUN_MAX_GHOST && lid[i] > 0; j++) {
            if (t->gh[j].used && t->gh[j].lid == lid[i]) {
                mode[i] = t->gh[j].mode == LAS_ARCADE ? LAS_ARCADE : LAS_SIM;
                est[i] = t->gh[j].est;
                found++;
                break;
            }
        }
    }
    LeaveCriticalSection(&t->cs);
    return found;
}

void loctun_pub_watch(loctun *t, int on) /* W460 */
{
    if (t) InterlockedExchange(&t->pub_watch, on ? 1 : 0);
}

int loctun_pub_list(loctun *t, loctun_pub_entry *out, int cap, int *total) /* W460 */
{
    int i, j, n = 0, all = 0, r;
    if (total) *total = 0;
    if (!t || !out || cap <= 0) return LOCTUN_PL_OFF;
    if (!t->pub || t->live) return LOCTUN_PL_OFF; /* W482: Live-Hosts nicht im System-Link-Widget */
    EnterCriticalSection(&t->cs);
    if (!t->my_cid || t->pub_list_rx <= 0 || tl_now(t) - t->pub_list_rx > 40000.0) {
        LeaveCriticalSection(&t->cs);
        return LOCTUN_PL_WAIT;
    }
    for (i = 0; i < TUN_MAX_GHOST; i++) {
        const tghost *g = &t->gh[i];
        loctun_pub_entry e;
        if (!g->used) continue;
        all++;
        memset(&e, 0, sizeof e);
        e.lid = g->lid;
        e.mode = g->mode == LAS_ARCADE ? LAS_ARCADE : LAS_SIM;
        e.est = g->est;
        e.cur = (int8_t)g->cf[0] + (int8_t)g->cf[1];
        e.max = (int8_t)g->sf[2] + (int8_t)g->sf[3];
        strcpy_s(e.pilot, sizeof e.pilot, g->pilot[0] ? g->pilot : "?");
        /* sortiert einfuegen (Lobby-Id aufsteigend = aelteste zuerst), nur die ersten cap behalten */
        for (j = n; j > 0 && out[j - 1].lid > e.lid; j--)
            if (j < cap) out[j] = out[j - 1];
        if (j < cap) {
            out[j] = e;
            if (n < cap) n++;
        }
    }
    LeaveCriticalSection(&t->cs);
    r = n;
    if (total) *total = all;
    return r;
}

int loctun_sec_state(loctun *t)
{
    return t ? (int)t->sec : 0;
}

int loctun_diag(loctun *t, char *buf, int cap)
{
    int i, up = 0, relay = 0, known = 0, lin = 0;
    double worst = 0;
    char rn[24] = "";
    if (!buf || cap <= 0) return 0;
    buf[0] = 0;
    if (!t) return 0;
    EnterCriticalSection(&t->cs);
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        if (!L->used) continue;
        known++;
        if (!L->up) continue;
        up++;
        if (L->path == 2) {
            relay++;
            if (L->relay_idx >= 0) strncpy_s(rn, sizeof rn, t->relays[L->relay_idx].name, _TRUNCATE);
        }
        if (L->srtt > worst) worst = L->srtt;
        if (L->loss_in > lin) lin = L->loss_in;
    }
    _snprintf_s(buf, (size_t)cap, _TRUNCATE,
                "meeting_point=%s lobby=%s route=%s%s%s peers=%d/%d ping_ms=%.0f loss_in=%d.%d%% sec=%s err=%s",
                t->c.lobby[0] ? "yes" : "no",
                t->lb_joined ? "joined" : (t->lb_state == LB_OFF ? "off" : "connecting"),
                up == 0 ? "none" : (relay == 0 ? "direct" : (relay == up ? "relay" : "mixed")), relay ? ":" : "",
                relay ? rn : "", up, known, worst, lin / 10, lin % 10,
                tl_sec_name(t), t->lb_err[0] ? t->lb_err : "-");
    LeaveCriticalSection(&t->cs);
    return 1;
}
