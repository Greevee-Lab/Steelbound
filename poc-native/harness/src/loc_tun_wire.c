/* loc_tun_wire.c - W464 NET-CORE: aus loc_tun.c verschoben (W324/W334/W395/W420), unveraendert.
 * Pakete bauen/senden (SBT2-Klartext, Redundanz), HELLO mit Flags/Version (Handshake), Verbindung steht,
 * Empfang: SBT2 oeffnen (Fenster/Replay-Schutz in sb_e2e), Raten je Verbindung (Entschluesseln, PROBE_ACK, Wegproben),
 * DATA zerlegen mit Laengenpruefungen -> Stroeme/Datagramme. Sicherheitsrelevant: net/SECURITY.md §4, §6. */
#include "loc_tun_int.h"

/* ====================================================================== Pakete bauen/senden */
int tl_build_send(loctun *t, tlink *L, double now, int force)
{
    uint8_t p[TUN_PT];
    int off = DHDR, nitems = 0, i, red = L->red, path = L->path;
    double rto = tl_rto(L), gap = (double)t->c.gap_ms;
    uint32_t s;
    if (!L->up || path == 0) return 0;
    /* Strom-Nachrichten, aelteste zuerst */
    for (s = L->snd_una; s != L->snd_next; s++) {
        tmsg *m = L->sq[s % TUN_SQ];
        int need;
        if (!m) continue;
        if (!(m->tx == 0 || (m->tx < red && now - m->last >= gap) || now - m->last >= rto)) continue;
        need = 11 + m->len;
        if (off + need > TUN_PT) {
            if (nitems == 0) continue;
            break;
        }
        p[off] = K_STREAM;
        put32(p + off + 1, m->seq);
        p[off + 5] = m->sub;
        put16(p + off + 6, m->stream);
        p[off + 8] = m->tx > 254 ? 254 : m->tx;
        put16(p + off + 9, m->len);
        memcpy(p + off + 11, m->data, m->len);
        off += need;
        nitems++;
        if (m->tx == 0) {
        } else if (m->tx < red && now - m->last < rto) {
            InterlockedIncrement(&L->s_copy);
            InterlockedIncrement(&t->st.msg_copy);
        } else {
            InterlockedIncrement(&L->s_rto);
            InterlockedIncrement(&t->st.msg_rto);
        }
        if (m->tx < 255) m->tx++;
        m->last = now;
    }
    /* Datagramme */
    for (i = 0; i < TUN_DQ; i++) {
        tdg *d = &L->dq[i];
        int need;
        if (!d->used) continue;
        if (!(d->tx == 0 || now - d->last >= gap)) continue;
        need = 10 + d->len;
        if (off + need > TUN_PT) continue;
        p[off] = K_DGRAM;
        put16(p + off + 1, d->id);
        put16(p + off + 3, d->src);
        put16(p + off + 5, d->dst);
        p[off + 7] = d->tx;
        put16(p + off + 8, d->len);
        memcpy(p + off + 10, d->data, d->len);
        off += need;
        nitems++;
        d->tx++;
        d->last = now;
        if (d->tx >= (red > 2 ? red : 2)) d->used = 0;
        if (nitems >= 250) break;
    }
    if (nitems == 0 && !force && !(L->ack_pending && now >= L->ack_due)) return 0;
    tl_hdr(p, T_DATA);
    put32(p + HDR, ++L->pkt_seq);
    put32(p + HDR + 4, L->rcv_next);
    put32(p + HDR + 8, (uint32_t)(now < 1 ? 1 : now));
    put32(p + HDR + 12, L->echo_ts);
    put16(p + HDR + 16, (uint16_t)(L->echo_ts ? (now - L->echo_at > 60000 ? 60000 : now - L->echo_at) : 0));
    put16(p + HDR + 18, (uint16_t)L->loss_in);
    p[HDR + 20] = (uint8_t)nitems;
    tl_send_link(t, L, path, p, off);
    L->ack_pending = 0;
    return nitems > 0 ? 2 : 1;
}

/* W420: Version nur aus [0-9A-Za-z.+-] (hoechstens 15 Zeichen), sonst leer */
void tl_ver_clean(const char *in, int n, char *out, int cap)
{
    int i, o = 0;
    out[0] = 0;
    for (i = 0; i < n && in[i]; i++) {
        char c = in[i];
        if (!(isalnum((unsigned char)c) || c == '.' || c == '+' || c == '-') || o + 1 >= cap || o >= 15) {
            out[0] = 0;
            return;
        }
        out[o++] = c;
    }
    out[o] = 0;
}
/* W420: numerischer Vergleich "0.3.10" > "0.3.2" (Nicht-Ziffern beenden den Teil); > 0 = a neuer.
 * W421: gemeinsame Hilfsfunktion mit dem Launcher (sb_version.h, LoC-Startsperre aus /v1/info) */
static int tl_ver_cmp(const char *a, const char *b)
{
    return sb_netver_cmp(a, b);
}
/* W420: eigene Version (TEST: ver_test) und ob die Pruefung gilt (steelnet bzw. TEST ver_check) */
static int tl_ver_required(const loctun *t) { return t->c.lobby[0] || t->c.ver_check || t->lan; } /* W454: auch LAN */
const char *tl_sec_name(const loctun *t)
{
    return t->sec == SEC_OK ? "ok" : (t->sec == SEC_PIN ? "pin_failed" : (t->sec == SEC_NOPIN ? "no_pin" :
           (t->sec == SEC_UPGRADE ? "upgrade_required" : (t->sec == SEC_VERSION ? "version_required" :
           "selftest_failed"))));
}

static void tl_hello_fill(loctun *t, tlink *L, uint8_t *p)
{
    tl_hdr(p, T_HELLO);
    put32(p + HDR, t->n->self_ip);
    p[HDR + 4] = (uint8_t)L->got_hello;
    p[HDR + 5] = 2; /* Version (W334: SBT2) */
    memset(p + HDR + 8, 0, HELLO_VLEN);
    if (strcmp(tl_myver(t), "-") == 0) { /* TEST: wie ein aelterer Client (Feld fehlt, Flag aus) */
        put16(p + HDR + 6, (uint16_t)(t->c.route_auto ? HELLO_F_ROUTE : 0));
    } else {
        size_t vl = strlen(tl_myver(t));
        put16(p + HDR + 6, (uint16_t)((t->c.route_auto ? HELLO_F_ROUTE : 0) | HELLO_F_VER)); /* W395: Flags, W420 */
        memcpy(p + HDR + 8, tl_myver(t), vl < HELLO_VLEN ? vl : HELLO_VLEN - 1);
    }
}

void tl_hello(loctun *t, tlink *L, double now)
{
    uint8_t p[HELLO_LEN];
    tl_hello_fill(t, L, p);
    tl_send_link(t, L, L->path, p, sizeof p);
    L->hello_last = now;
}

/* HELLO auf einem bestimmten Weg: slot >= 1 = Relay slot-1 (W395), sonst direkt an "to". */
static void tl_hello_on(loctun *t, tlink *L, int slot, const struct sockaddr_in *to, double now)
{
    uint8_t p[HELLO_LEN];
    tl_hello_fill(t, L, p);
    if (slot >= 1) {
        tl_send_slot(t, L, slot, p, sizeof p);
    } else {
        tl_send_to(t, L, to, p, sizeof p);
    }
    L->hello_last = now;
}

/* ====================================================================== Empfang */
void tl_link_up(loctun *t, tlink *L)
{
    struct sockaddr_in a;
    char ipb[16], ipc[16];
    u_long nb = 1;
    if (L->up || L->ver_bad) return; /* W420 */
    if (L->vip == t->n->self_ip) {
        if (!(L->bad_logged & 8)) { /* W332: je HELLO eine Zeile = Mitspieler fuellt das Log unbegrenzt */
            L->bad_logged |= 8;
            tl_logf(t, "LOCTUN: c%u hat dieselbe virtuelle Adresse %s - Verbindung abgelehnt (loc_net_addr=random)",
                    L->cid, locnet_ip_str(L->vip, ipb));
        }
        return;
    }
    L->up = 1;
    L->last_rx = tl_now(t);
    if (t->pub) pub_on_link_up(t, L); /* W373: Geister-Sockets vor den Stellvertretern schliessen */
    /* Stellvertreter fuer TCP 1000 */
    L->lsn = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (L->lsn != INVALID_SOCKET) {
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = L->vip;
        a.sin_port = htons(t->c.proxy_base);
        if (bind(L->lsn, (struct sockaddr *)&a, sizeof a) != 0 || listen(L->lsn, 8) != 0) {
            tl_logf(t, "LOCTUN: Stellvertreter %s:%u nicht moeglich (%d)", locnet_ip_str(L->vip, ipb), t->c.proxy_base,
                    WSAGetLastError());
            closesocket(L->lsn);
            L->lsn = INVALID_SOCKET;
        } else {
            ioctlsocket(L->lsn, FIONBIO, &nb);
            tl_sock_events(t, L->lsn, FD_ACCEPT);
        }
    }
    locnet_peer_add(t->n, L->vip, t->c.proxy_base);
    tl_logf(t, "LOCTUN: Verbindung zu c%u steht: virtuelle Adresse %s (eigene %s), Weg %s, RTT %.1f ms", L->cid,
            locnet_ip_str(L->vip, ipb), locnet_ip_str(t->n->self_ip, ipc),
            L->path == 1 ? "direkt" : (L->path == 2 ? "Relay" : "keiner"),
            L->rtt_n ? L->srtt : L->direct_rtt);
}

static void tl_rx_data(loctun *t, int li, const uint8_t *p, int len, double now)
{
    tlink *L = &t->links[li];
    uint32_t pseq, ack, ts, echo;
    int hold, nitems, off = DHDR, i, newdata = 0, fresh;
    if (len < DHDR) return;
    pseq = get32(p + HDR);
    ack = get32(p + HDR + 4);
    ts = get32(p + HDR + 8);
    echo = get32(p + HDR + 12);
    hold = get16(p + HDR + 16);
    L->loss_peer = get16(p + HDR + 18);
    nitems = p[HDR + 20];
    L->peer_got = 1;
    /* Verlust (Eingang) */
    L->rx_cnt++;
    fresh = (int32_t)(pseq - L->rx_hi) > 0 || L->rx_hi == 0; /* W395: Kopie ueber den zweiten Weg bzw. Nachzuegler */
    if ((int32_t)(pseq - L->rx_hi) > 0) L->rx_hi = pseq;
    if (now - L->loss_t >= 1000.0) {
        uint32_t exp = L->rx_hi - L->rx_hi0, got = L->rx_cnt - L->rx_cnt0;
        if (L->loss_t > 0 && exp > 0) {
            int pm = got >= exp ? 0 : (int)((exp - got) * 1000 / exp);
            L->loss_in = (L->loss_in * 7 + pm * 3 + 5) / 10;
            if (pm > L->loss_in) L->loss_in = pm; /* schnell hoch, langsam runter */
        }
        L->rx_hi0 = L->rx_hi;
        L->rx_cnt0 = L->rx_cnt;
        L->loss_t = now;
    }
    /* RTT (W395: nur aus dem neuesten Paket - eine spaetere Kopie desselben Pakets verfaelschte die Messung) */
    if (ts && fresh) {
        L->echo_ts = ts;
        L->echo_at = now;
    }
    if (echo && fresh) {
        tl_rtt_sample(L, now - (double)echo - (double)hold);
    }
    /* Bestaetigung */
    while ((int32_t)(ack - L->snd_una) > 0 && L->snd_una != L->snd_next) {
        tmsg *m = L->sq[L->snd_una % TUN_SQ];
        if (m) {
            L->sq_bytes -= m->len;
            free(m);
            L->sq[L->snd_una % TUN_SQ] = NULL;
        }
        L->snd_una++;
    }
    L->red = tl_red(t, L);
    /* Eintraege */
    for (i = 0; i < nitems; i++) {
        if (off + 1 > len) break;
        if (p[off] == K_STREAM) {
            uint32_t seq;
            uint16_t ml;
            uint8_t cp;
            if (off + 11 > len) break;
            seq = get32(p + off + 1);
            cp = p[off + 8];
            ml = get16(p + off + 9);
            if (off + 11 + ml > len || ml > TUN_MSG) break;
            newdata = 1;
            if ((int32_t)(seq - L->rcv_next) < 0 || (int32_t)(seq - L->rcv_next) >= TUN_RQ ||
                L->rq[seq % TUN_RQ]) {
                InterlockedIncrement(&L->s_dup);
                InterlockedIncrement(&t->st.msg_dup);
            } else {
                tmsg *m = (tmsg *)malloc(sizeof *m + (ml ? ml : 1));
                if (m) {
                    memset(m, 0, sizeof *m);
                    m->seq = seq;
                    m->sub = p[off + 5];
                    m->stream = get16(p + off + 6);
                    m->len = ml;
                    m->arr = now;
                    memcpy(m->data, p + off + 11, ml);
                    L->rq[seq % TUN_RQ] = m;
                    InterlockedIncrement(&t->st.msg_rx);
                    if (cp > 0) {
                        InterlockedIncrement(&L->s_saved);
                        InterlockedIncrement(&t->st.msg_saved);
                    }
                }
            }
            off += 11 + ml;
        } else if (p[off] == K_DGRAM) {
            uint16_t id, src, dst, dl;
            if (off + 10 > len) break;
            id = get16(p + off + 1);
            src = get16(p + off + 3);
            dst = get16(p + off + 5);
            dl = get16(p + off + 8);
            if (off + 10 + dl > len) break;
            newdata = 1;
            if (tl_seen_dg(L, id)) {
                InterlockedIncrement(&t->st.dg_dup);
            } else {
                InterlockedIncrement(&L->s_dgrx);
                InterlockedIncrement(&t->st.dg_rx);
                tl_deliver_dg(t, L, src, dst, p + off + 10, dl);
            }
            off += 10 + dl;
        } else {
            break;
        }
    }
    /* geordnet zustellen (mit optionaler Haltezeit = Jitter-Puffer) */
    for (;;) {
        tmsg *m = L->rq[L->rcv_next % TUN_RQ];
        if (!m || m->seq != L->rcv_next) break;
        if (t->c.hold_ms > 0 && now - m->arr < (double)t->c.hold_ms) break;
        L->rq[L->rcv_next % TUN_RQ] = NULL;
        L->rcv_next++;
        tl_deliver_msg(t, li, m);
        free(m);
    }
    if (newdata && !L->ack_pending) {
        L->ack_pending = 1;
        L->ack_due = now + 2.0;
    }
}

/* W329: virtuelle Adresse aus dem HELLO des Mitspielers. Auf ihr bindet der Stellvertreter (TCP proxy_base,
 * UDP) - sie muss daher eine eigene Loopback-Adresse sein: 127/8 ausser 127.0.0.1 und .0/.255,
 * nicht die eines anderen Mitspielers (sonst Verkehr fuer ihn umgeleitet), nach dem Verbindungsaufbau fest.
 * Ohne Pruefung: 0.0.0.0 bzw. die eigene LAN-Adresse = Stellvertreter-Port offen fuers Netz. */
static int tl_vip_ok(loctun *t, const tlink *L, uint32_t vip)
{
    uint32_t h = ntohl(vip);
    int i;
    if ((h >> 24) != 127u || h == 0x7F000001u || (h & 0xFFu) == 0u || (h & 0xFFu) == 255u) return 0;
    /* vip == eigene Adresse: tl_link_up lehnt mit eigener Meldung ab (loc_net_addr=random) */
    if (L->up && vip != L->vip) return 0;
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        const tlink *o = &t->links[i];
        if (o != L && o->used && o->vip == vip) return 0;
    }
    return 1;
}

/* W334: Empfang eines SBT2-Pakets (SECURITY.md §4.5, Reihenfolge genau so):
 * 1. Form (Laenge, Magic, sid bekannt, ctr-Bereich) ohne Krypto, 2. Ratenbremse vor der Krypto, 3.-5. Fenster
 * pruefen, GCM oeffnen, erst dann Fenster setzen (sbe2e_open), 6. authentische Pakete von jeder Adresse - der
 * Sendeweg wechselt nur per PROBE/PROBE_ACK oder route, 7. Transportlogik, Gastfilter vor dem Spiel. */
/* via_relay_link/via_ri: Verbindung und Relay einer Relay-Nutzlast (-1 = direkt empfangen). */
void tl_rx_sbt(loctun *t, const uint8_t *pkt, int plen, const struct sockaddr_in *from, int via_relay_link,
                      int via_ri, double now)
{
    uint8_t p[TUN_PKT];
    uint32_t sid;
    int li, len, slot;
    tlink *L = NULL;
    if (!sbe2e_peek_sid(pkt, plen, &sid)) {
        InterlockedIncrement(&t->st.e2e_bad);
        return;
    }
    for (li = 0; li < TUN_MAX_LINKS; li++) {
        tlink *c = &t->links[li];
        if (!c->used || c->blocked || !c->e2e.valid || c->e2e.sid != sid) continue;
        if (via_relay_link >= 0 && li != via_relay_link) continue;
        L = c;
        break;
    }
    if (!L) {
        InterlockedIncrement(&t->st.e2e_bad); /* unbekannte sid: verwerfen ohne Rechenaufwand */
        return;
    }
    if (L->cid == 0 && !locnet_ip_allowed(from->sin_addr.s_addr)) {
        /* W335 (NET-AUDIT N1): Testverbindung (loc_transport_peers, Schluessel oeffentlich bekannt) nur von
         * Loopback/privat - sonst koennte jeder, der den UDP-Port erreicht, als "Mitspieler" Rahmen einspeisen */
        InterlockedIncrement(&t->st.e2e_bad);
        return;
    }
    if (!lguard_bucket_take(&L->b_dec, now, TUN_DEC_RATE, TUN_DEC_RATE)) {
        InterlockedIncrement(&t->st.e2e_bad);
        return;
    }
    len = sbe2e_open(&L->e2e, pkt, plen, p, sizeof p);
    if (len < 1) {
        InterlockedIncrement(&t->st.e2e_bad);
        return;
    }
    L->last_rx = now;
    InterlockedIncrement(&L->s_rx);
    InterlockedIncrement(&t->st.pkt_rx);
    /* W395: Ankunftsweg (Relay i -> 1+i; direkt nur von einer per Durchstich bestaetigten Adresse) */
    if (via_relay_link >= 0) slot = via_ri >= 0 ? 1 + via_ri : -1;
    else slot = ((L->direct_ok && sa_eq(&L->direct, from)) || (L->seen_ok && sa_eq(&L->seen_from, from))) ? 0 : -1;
    switch (p[0]) {
    case T_HELLO:
        if (len >= HDR + 6) {
            uint32_t vip = get32(p + HDR);
            if (!tl_vip_ok(t, L, vip)) {
                char ipb[16];
                InterlockedIncrement(&t->st.bad);
                if (!(L->bad_logged & 2)) {
                    L->bad_logged |= 2;
                    tl_logf(t, "LOCTUN: c%u meldet unzulaessige virtuelle Adresse %s - ignoriert", L->cid,
                            locnet_ip_str(vip, ipb));
                }
                break;
            }
            if (tl_ver_required(t)) { /* W420: steelnet nur mit gleicher Client-Version (fehlt = aelterer Client) */
                char pv[16] = "";
                int has = len >= HELLO_LEN && (get16(p + HDR + 6) & HELLO_F_VER);
                if (has) tl_ver_clean((const char *)p + HDR + 8, HELLO_VLEN, pv, (int)sizeof pv);
                if (!has || strcmp(pv, tl_myver(t)) != 0) {
                    if (!L->ver_bad) {
                        L->ver_bad = 1;
                        strcpy_s(t->peer_ver, sizeof t->peer_ver, pv);
                        strcpy_s(t->need_ver, sizeof t->need_ver,
                                 (pv[0] && tl_ver_cmp(pv, tl_myver(t)) > 0) ? pv : tl_myver(t));
                        InterlockedExchange(&t->ver_state, t->lan ? 3 : 2); /* W454: 3 = LAN */
                        tl_logf(t, "LOCVER: c%u nutzt Client-Version %s, eigene %s - Verbindung abgelehnt (steelnet nur mit "
                                "gleicher Version)", L->cid, pv[0] ? pv : "(fehlt, aelterer Client)", tl_myver(t));
                        if (slot >= 0) tl_hello_on(t, L, slot, from, now); /* damit ein neuer Client es auch sieht */
                        tl_link_drop(t, L, "andere Client-Version");
                    }
                    break;
                }
            }
            L->vip = vip;
            if (p[HDR + 4]) L->peer_got = 1;
            if (len >= HDR + 8 && (get16(p + HDR + 6) & HELLO_F_ROUTE)) L->peer_route = 1; /* W395 */
            if (!L->got_hello) {
                L->got_hello = 1;
                /* Antwort auf dem Ankunftsweg - direkt nur an eine per Durchstich bestaetigte Adresse */
                if (slot >= 0) tl_hello_on(t, L, slot, from, now);
            }
            /* "steht" erst mit einem Weg zum Partner (Durchstich bestaetigt bzw. Relay); sonst holt der Zeitgeber
             * das nach, sobald der Weg da ist */
            if (L->path || via_relay_link >= 0) tl_link_up(t, L);
        }
        break;
    case T_DATA:
        if (L->up) tl_rx_data(t, li, p, len, now);
        if (t->chat_hit) { /* W437: Klartext mit Chat nicht im Puffer liegen lassen (Minidump) */
            t->chat_hit = 0;
            SecureZeroMemory(p, sizeof p);
        }
        break;
    case T_PING:
        if (len == HDR + RP_LEN && p[HDR] == 'R' && p[HDR + 1] == 'T') {
            /* W395: Wegprobe - gleich gross auf dem Ankunftsweg zurueck (keine Verstaerkung, nur authentisch) */
            uint8_t q[HDR + RP_LEN];
            if (slot < 0 || !lguard_bucket_take(&L->b_rp, now, TUN_RP_RATE, TUN_RP_RATE)) break;
            tl_hdr(q, T_PONG);
            memcpy(q + HDR, p + HDR, RP_LEN);
            if (slot >= 1) tl_send_slot(t, L, slot, q, sizeof q);
            else tl_send_to(t, L, from, q, sizeof q);
            break;
        }
        if (len >= HDR + 4) {
            uint8_t q[HDR + 4];
            tl_hdr(q, T_PONG);
            memcpy(q + HDR, p + HDR, 4);
            if (slot >= 1) {
                tl_send_slot(t, L, slot, q, sizeof q);
            } else if (slot == 0) {
                tl_send_to(t, L, from, q, sizeof q);
            }
        }
        break;
    case T_PONG:
        if (len == HDR + RP_LEN && p[HDR] == 'R' && p[HDR + 1] == 'T') {
            /* W395: nur auf dem Weg, ueber den die Probe ging (Slot im Echo = Ankunftsweg) */
            if (slot >= 0 && p[HDR + 2] == (uint8_t)slot && p[HDR + 3] == 0 &&
                locroute_probe_ack(&L->rt, slot, get16(p + HDR + 4), now)) {
                L->rt_last_ok[slot] = now;
            }
            break;
        }
        if (len >= HDR + 4) {
            uint32_t k = get32(p + HDR);
            if (k < 5 && L->dping_at[k] > 0 && slot == 0) {
                double smp = now - L->dping_at[k];
                if (L->ndrtt < 5) L->drtt[L->ndrtt++] = smp;
                L->dping_at[k] = 0;
                tl_rtt_sample(L, smp);
            }
        }
        break;
    case T_PROBE: /* Durchstich: authentisch -> Weg vom Partner zu uns offen; ACK gleich gross an die Absenderadresse */
        if (len == HDR + 8 && via_relay_link < 0 && !t->c.nodirect && tl_dest_ok(t, from->sin_addr.s_addr) &&
            lguard_bucket_take(&L->b_ack, now, TUN_ACK_RATE, TUN_ACK_RATE)) {
            uint8_t q[HDR + 8];
            L->seen_from = *from;
            L->seen_ok = 1;
            tl_hdr(q, T_PROBE_ACK);
            memcpy(q + HDR, p + HDR, 8);
            tl_send_to(t, L, from, q, sizeof q);
            tl_probe_seen(t, L, from, now); /* W521: an eine unbekannte Absenderadresse zurueckstanzen (peer-reflexiv) */
        }
        break;
    case T_PROBE_ACK: /* nur mit der offenen probe_id: direkter Weg zu dieser Adresse bestaetigt */
        if (len == HDR + 8 && via_relay_link < 0 && !t->c.nodirect && L->probe_id != 0 && !L->direct_ok &&
            memcmp(p + HDR, &L->probe_id, 8) == 0 && tl_dest_ok(t, from->sin_addr.s_addr)) {
            tl_probe_ack(t, L, from, now); /* W521: IPv6 vor IPv4, Log ohne oeffentliche Adresse (loc_tun_link.c) */
        }
        break;
    case T_BYE:
        if (L->up || L->path) tl_link_drop(t, L, "Partner hat beendet");
        break;
    default:
        break;
    }
    SecureZeroMemory(p, (size_t)len);
}

