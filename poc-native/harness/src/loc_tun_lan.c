/* loc_tun_lan.c - W454 LOC-LAN-MODE: LAN-Modus des Transports ohne steelnet (klassischer System Link im lokalen Netz).
 *
 * Entdeckung per gerichtetem Broadcast in die eigenen privaten IPv4-Subnetze (TTL 1, UDP LLAN_PORT, Paket SBL1 aus
 * loc_lan.c), danach Paarbildung wie online ueber tl_link_pair (SBT2, Schluessel aus X25519 + Transkript, zusaetzlich an
 * den Raumschluessel gebunden). Kein Treffpunkt, kein Konto, keine Relays, kein steelnet-Chat. Alle Mitspieler desselben
 * Raums verbinden sich untereinander (wie ein echtes LAN); die Spiele erscheinen in der Suchliste des Spiels ueber die
 * normale System-Link-Suche durch den Transport. Sicherheit: net/SECURITY.md §36. Keine Inhalte, keine Adressen im Log
 * ausser im TEST-Pfad (Loopback). */
#include "loc_tun_int.h"
#include "loc_lan.h"
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")

#define LAN_TX_MS 1000.0      /* Abstand der Broadcast-Pakete */
#define LAN_IF_MS 10000.0     /* Schnittstellen neu lesen (Kabel/WLAN spaeter verbunden) */
#define LAN_GONE_MS 20000.0   /* kein Paket mehr: Mitspieler gilt als weg (Eintrag frei) */
#define LAN_RX_RATE 200.0     /* Entdeckungspakete je Sekunde insgesamt (vor der Pruefung), Burst 400 */
#define LAN_REPLY_RATE 20.0   /* sofortige Antworten (Unicast) je Sekunde, Burst 40 */
#define LAN_PAIR_MS 15000.0   /* Paar ohne Verbindung (z. B. anderes Passwort): Platz frei, Kennung bis zum Abgang uebergangen */

static void lan_ifaces(loctun *t)
{
    ULONG sz = 16384;
    IP_ADAPTER_ADDRESSES *aa = NULL, *a;
    int tries, n = 0, k;
    uint32_t bc[8];
    for (tries = 0; tries < 3; tries++) {
        ULONG r;
        aa = (IP_ADAPTER_ADDRESSES *)malloc(sz);
        if (!aa) return;
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 NULL, aa, &sz);
        if (r == ERROR_SUCCESS) break;
        free(aa);
        aa = NULL;
        if (r != ERROR_BUFFER_OVERFLOW) return;
    }
    if (!aa) return;
    for (a = aa; a && n < 8; a = a->Next) {
        IP_ADAPTER_UNICAST_ADDRESS *u;
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (u = a->FirstUnicastAddress; u && n < 8; u = u->Next) {
            struct sockaddr_in *sa = (struct sockaddr_in *)u->Address.lpSockaddr;
            uint32_t ip, mask, b;
            int dup = 0;
            if (!sa || sa->sin_family != AF_INET) continue;
            if (u->OnLinkPrefixLength < 8 || u->OnLinkPrefixLength > 30) continue;
            if (!locnet_ip_allowed(sa->sin_addr.s_addr) || (ntohl(sa->sin_addr.s_addr) >> 24) == 127u) continue;
            ip = ntohl(sa->sin_addr.s_addr);
            mask = 0xFFFFFFFFu << (32 - u->OnLinkPrefixLength);
            b = htonl(ip | ~mask);
            for (k = 0; k < n; k++) dup |= bc[k] == b;
            if (!dup) bc[n++] = b;
        }
    }
    free(aa);
    if (n != t->lan_nbc || memcmp(bc, t->lan_bc, sizeof bc[0] * (size_t)n) != 0) {
        memcpy(t->lan_bc, bc, sizeof bc[0] * (size_t)n);
        t->lan_nbc = n;
        tl_logf(t, "LOCLAN: %d lokale Netze fuer die Entdeckung", n);
    }
}

static int lan_beacon(loctun *t, uint8_t *b)
{
    llan_beacon o;
    memset(&o, 0, sizeof o);
    o.tport = t->uport;
    o.cid = t->my_cid;
    o.room_id = t->lan_room_id;
    memcpy(o.nonce, t->lan_nonce, 16);
    memcpy(o.pub, t->my_pub, 32);
    strcpy_s(o.room, sizeof o.room, t->lan_room);
    strcpy_s(o.pilot, sizeof o.pilot, t->my_pilot);
    strncpy_s(o.ver, sizeof o.ver, tl_myver(t), _TRUNCATE);
    o.mode = _stricmp(t->c.play_mode, "arcade") == 0 ? 2 : 1;
    return llan_beacon_build(b, LLAN_BEACON_LEN, &o);
}

static void lan_send_all(loctun *t)
{
    uint8_t b[LLAN_BEACON_LEN];
    int i;
    if (t->netcut || lan_beacon(t, b) != LLAN_BEACON_LEN) return;
    for (i = 0; i < t->lan_nbc; i++) {
        struct sockaddr_in to;
        memset(&to, 0, sizeof to);
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = t->lan_bc[i];
        to.sin_port = htons(t->lan_port);
        sendto(t->lan_s, (const char *)b, LLAN_BEACON_LEN, 0, (const struct sockaddr *)&to, sizeof to);
        InterlockedIncrement(&t->lan_tx);
    }
    for (i = 0; i < t->lan_ntg; i++) {
        sendto(t->lan_s, (const char *)b, LLAN_BEACON_LEN, 0, (const struct sockaddr *)&t->lan_tg[i], sizeof t->lan_tg[i]);
        InterlockedIncrement(&t->lan_tx);
    }
}

static int lan_peer_find(loctun *t, uint32_t cid)
{
    int i;
    for (i = 0; i < LAN_MAX_PEERS; i++) {
        if (t->lan_p[i].cid == cid) return i;
    }
    return -1;
}

static void lan_peer_free(loctun *t, int i, const char *why)
{
    tlink *L = tl_link_by_cid(t, t->lan_p[i].cid, 0);
    if (L) {
        if (L->up || L->e2e.valid || L->lsn != INVALID_SOCKET) tl_link_drop(t, L, why);
        L->used = 0;
    }
    memset(&t->lan_p[i], 0, sizeof t->lan_p[i]);
}

/* gueltiges SBL1 eines anderen Clients (Absender privat, Rate geprueft) */
static void lan_on_beacon(loctun *t, const llan_beacon *b, const struct sockaddr_in *from, double now)
{
    int i = lan_peer_find(t, b->cid), k;
    tlink *L;
    struct sockaddr_in cand[TUN_MAX_CAND];
    uint8_t pn[16];
    if (strcmp(b->room, t->lan_room) != 0) {
        InterlockedIncrement(&t->lan_ign); /* anderer Raum: still uebergehen */
        return;
    }
    if (i >= 0) {
        t->lan_p[i].seen = now;
        if (memcmp(t->lan_p[i].pub, b->pub, 32) != 0 || memcmp(t->lan_p[i].nonce, b->nonce, 16) != 0) {
            /* gleiche Kennung, anderer Schluessel: nie still neu ableiten (wie §14 online) */
            if (!(t->lan_p[i].logged & 1)) {
                t->lan_p[i].logged |= 1;
                tl_logf(t, "LOCSEC: LAN-Paket fuer c%u mit anderem Schluessel - uebergangen", b->cid);
            }
            return;
        }
        if (t->lan_p[i].state == 1 && t->lan_p[i].ip != from->sin_addr.s_addr) {
            L = tl_link_by_cid(t, b->cid, 0); /* neue Adresse (z. B. DHCP): nur die Durchstich-Adresse auffrischen */
            t->lan_p[i].ip = from->sin_addr.s_addr;
            if (L && !L->blocked) {
                L->cand[0] = *from;
                L->cand[0].sin_port = htons(b->tport);
                L->ncand = 1;
                if (!L->direct_ok && !L->punching) {
                    L->punching = 1;
                    L->punch_start = now;
                    L->punch_last = 0;
                }
            }
        }
        return;
    }
    /* neuer Mitspieler */
    for (i = 0; i < LAN_MAX_PEERS && t->lan_p[i].cid; i++) {
    }
    if (i >= LAN_MAX_PEERS) {
        if (!(t->lan_full_logged)) {
            t->lan_full_logged = 1;
            tl_logf(t, "LOCLAN: Tabelle voll (%d) - weitere Mitspieler uebergangen", LAN_MAX_PEERS);
        }
        return;
    }
    t->lan_p[i].cid = b->cid;
    t->lan_p[i].ip = from->sin_addr.s_addr;
    t->lan_p[i].seen = now;
    memcpy(t->lan_p[i].pub, b->pub, 32);
    memcpy(t->lan_p[i].nonce, b->nonce, 16);
    if (strcmp(b->ver, tl_myver(t)) != 0) {
        /* W454: gleiche Version direkt zwischen den Clients (wie W420 online) - schon vor dem Schluesseltausch */
        t->lan_p[i].state = 2;
        strcpy_s(t->peer_ver, sizeof t->peer_ver, b->ver);
        strcpy_s(t->need_ver, sizeof t->need_ver, sb_netver_cmp(b->ver, tl_myver(t)) > 0 ? b->ver : tl_myver(t));
        InterlockedExchange(&t->ver_state, 3);
        tl_logf(t, "LOCVER: LAN-Mitspieler c%u nutzt Client-Version %s, eigene %s - nicht verbunden (alle brauchen "
                "dieselbe Version)", b->cid, b->ver, tl_myver(t));
        return;
    }
    L = tl_link_by_cid(t, b->cid, 1);
    if (!L) {
        t->lan_p[i].state = 2;
        if (!(t->lan_full_logged)) {
            t->lan_full_logged = 1;
            tl_logf(t, "LOCLAN: keine freie Verbindung (%d) - Mitspieler c%u uebergangen", TUN_MAX_LINKS, b->cid);
        }
        return;
    }
    if (b->cid < t->my_cid) llan_pair_nonce(b->nonce, t->lan_nonce, pn);
    else llan_pair_nonce(t->lan_nonce, b->nonce, pn);
    memset(cand, 0, sizeof cand);
    cand[0] = *from;
    cand[0].sin_port = htons(b->tport);
    k = tl_link_pair(t, L, b->cid, t->lan_room_id, pn, b->pub, 0, b->pilot, cand, 1);
    SecureZeroMemory(pn, sizeof pn);
    if (k != TL_PAIR_OK) {
        t->lan_p[i].state = 2;
        L->used = 0;
        return;
    }
    t->lan_p[i].state = 1;
    t->lan_p[i].paired = now;
    tl_logf(t, "LOCLAN: Mitspieler c%u (%s) im Raum gefunden, Ende-zu-Ende-Schluessel bereit%s", b->cid, b->pilot,
            t->lan_loop ? " (TEST Loopback)" : "");
    /* sofort antworten, damit der andere nicht auf das naechste Broadcast-Paket wartet */
    if (lguard_bucket_take(&t->lan_breply, now, LAN_REPLY_RATE, 2.0 * LAN_REPLY_RATE)) {
        uint8_t ob[LLAN_BEACON_LEN];
        struct sockaddr_in to = *from;
        if (lan_beacon(t, ob) == LLAN_BEACON_LEN) {
            sendto(t->lan_s, (const char *)ob, LLAN_BEACON_LEN, 0, (const struct sockaddr *)&to, sizeof to);
            InterlockedIncrement(&t->lan_tx);
        }
    }
}

static void lan_rx(loctun *t, double now)
{
    uint8_t b[512];
    for (;;) {
        struct sockaddr_in from;
        int fl = sizeof from;
        llan_beacon o;
        int r = recvfrom(t->lan_s, (char *)b, sizeof b, 0, (struct sockaddr *)&from, &fl);
        if (r < 0) {
            if (WSAGetLastError() == WSAECONNRESET || WSAGetLastError() == WSAEMSGSIZE) continue;
            break;
        }
        if (t->netcut) continue; /* W366 TEST */
        /* Grenze: nur private/Loopback-Absender (kein Lauschen ausserhalb des LANs), Rate vor jeder Pruefung */
        if (fl < (int)sizeof from || from.sin_family != AF_INET || !locnet_ip_allowed(from.sin_addr.s_addr) ||
            ntohs(from.sin_port) == 0) {
            InterlockedIncrement(&t->lan_bad);
            continue;
        }
        if (!lguard_bucket_take(&t->lan_brx, now, LAN_RX_RATE, 2.0 * LAN_RX_RATE)) {
            InterlockedIncrement(&t->lan_bad);
            continue;
        }
        if (!llan_beacon_parse(b, r, &o)) {
            InterlockedIncrement(&t->lan_bad);
            continue;
        }
        InterlockedIncrement(&t->lan_rx);
        if (o.cid == t->my_cid) continue; /* eigenes Paket (Broadcast kommt zurueck) */
        lan_on_beacon(t, &o, &from, now);
    }
}

void lan_poll(loctun *t, double now)
{
    int i;
    if (!t->lan || t->lan_s == INVALID_SOCKET) return;
    lan_rx(t, now);
    if (!t->lan_loop && now - t->lan_if_at >= LAN_IF_MS) {
        t->lan_if_at = now;
        lan_ifaces(t);
    }
    if (now - t->lan_tx_at >= LAN_TX_MS) {
        t->lan_tx_at = now;
        lan_send_all(t);
    }
    for (i = 0; i < LAN_MAX_PEERS; i++) { /* Paar kam nicht zustande (anderes Passwort, Stoerer): Verbindungsplatz frei */
        tlink *L;
        if (!t->lan_p[i].cid || t->lan_p[i].state != 1 || now - t->lan_p[i].paired < LAN_PAIR_MS) continue;
        L = tl_link_by_cid(t, t->lan_p[i].cid, 0);
        if (L && (L->up || L->got_hello)) continue;
        if (L) {
            if (L->e2e.valid || L->lsn != INVALID_SOCKET) tl_link_drop(t, L, "keine Verbindung im LAN");
            L->used = 0;
        }
        t->lan_p[i].state = 2;
        tl_logf(t, "LOCLAN: Mitspieler c%u nach %d s nicht verbunden (anderes Passwort?) - uebergangen", t->lan_p[i].cid,
                (int)(LAN_PAIR_MS / 1000.0));
    }
    for (i = 0; i < LAN_MAX_PEERS; i++) { /* Abgang: lange kein Entdeckungspaket mehr und Verbindung still bzw. zu */
        tlink *L;
        if (!t->lan_p[i].cid || now - t->lan_p[i].seen < LAN_GONE_MS) continue;
        L = tl_link_by_cid(t, t->lan_p[i].cid, 0);
        if (L && L->up && now - L->last_rx < LAN_GONE_MS) continue;
        tl_logf(t, "LOCLAN: Mitspieler c%u nicht mehr im Netz - Eintrag frei", t->lan_p[i].cid);
        lan_peer_free(t, i, "nicht mehr im LAN");
    }
}

/* nach dem Binden des Transport-Sockets (uport bekannt), vor dem Thread. 0 = ok, -1 = LAN-Modus nicht moeglich. */
int lan_start(loctun *t)
{
    struct sockaddr_in a;
    BOOL on = TRUE;
    int ttl = 1;
    u_long nb = 1;
    char pass[LLAN_PASS_MAX + 1];
    t->lan_s = INVALID_SOCKET;
    if (!llan_room_norm(t->c.lan_room, t->lan_room, (int)sizeof t->lan_room)) {
        tl_logf(t, "LOCLAN: Raumname ungueltig (nur ASCII, 1..16 Zeichen) - LAN aus");
        return -1;
    }
    llan_pilot_norm(t->c.pilot, t->my_pilot, (int)sizeof t->my_pilot);
    t->lan_room_id = llan_room_id(t->lan_room);
    t->lan_port = t->c.lan_port ? t->c.lan_port : (uint16_t)LLAN_PORT;
    /* Raumpasswort aus cfg bzw. Umgebung SB_LAN_PASS (danach geloescht, wie SB_LOC_AUTH) */
    strncpy_s(pass, sizeof pass, t->c.lan_pass, _TRUNCATE);
    if (!pass[0]) {
        DWORD gl = GetEnvironmentVariableA("SB_LAN_PASS", pass, sizeof pass);
        if (gl == 0 || gl >= sizeof pass) pass[0] = 0;
    }
    SetEnvironmentVariableA("SB_LAN_PASS", NULL);
    SecureZeroMemory(t->c.lan_pass, sizeof t->c.lan_pass);
    t->lan_has_pass = pass[0] != 0;
    llan_psk(t->lan_room, pass, 0, t->lan_psk);
    SecureZeroMemory(pass, sizeof pass);
    if (sbe2e_keypair(&t->kp, t->my_pub) != 0 || sbe2e_random(t->lan_nonce, 16) != 0 ||
        sbe2e_random(&t->my_cid, 4) != 0) {
        tl_logf(t, "LOCSEC: Zufall/Schluessel fuer LAN nicht verfuegbar - LAN aus");
        return -1;
    }
    t->my_cid &= 0x7FFFFFFFu; /* nie 0, nie die TEST-Kennungen des festen Testwegs (Bit 31) */
    if (!t->my_cid) t->my_cid = 1;
    t->lan_s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (t->lan_s == INVALID_SOCKET) return -1;
    setsockopt(t->lan_s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on); /* mehrere Clients je Rechner */
    if (!t->lan_loop) setsockopt(t->lan_s, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof on);
    setsockopt(t->lan_s, IPPROTO_IP, IP_TTL, (const char *)&ttl, sizeof ttl); /* nie ueber einen Router */
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = t->bind_ip;
    a.sin_port = htons(t->lan_port);
    if (bind(t->lan_s, (struct sockaddr *)&a, sizeof a) != 0) {
        tl_logf(t, "LOCLAN: Entdeckungs-Port %u nicht verfuegbar (%d) - LAN aus", t->lan_port, WSAGetLastError());
        closesocket(t->lan_s);
        t->lan_s = INVALID_SOCKET;
        return -1;
    }
    {
        BOOL off = FALSE;
        DWORD got = 0;
        WSAIoctl(t->lan_s, 0x9800000Cu /* SIO_UDP_CONNRESET */, &off, sizeof off, NULL, 0, &got, NULL, NULL);
    }
    ioctlsocket(t->lan_s, FIONBIO, &nb);
    if (!t->lan_loop) lan_ifaces(t);
    t->lan_if_at = tl_now(t);
    t->lan_tx_at = -1e9; /* erstes Paket sofort */
    t->lan = 1;
    tl_logf(t, "LOCLAN: LAN-Modus aktiv (ohne steelnet), Entdeckung UDP %u%s, Raum %s, %s", t->lan_port,
            t->lan_loop ? " nur Loopback (TEST)" : " (Broadcast, TTL 1, nur private Netze)",
            strcmp(t->lan_room, "LAN") == 0 ? "Standard" : "eigener Name", t->lan_has_pass ? "mit Passwort" : "ohne Passwort");
    return 0;
}

/* TEST-Ziele "ip:port,..." (nur Loopback/privat) - vor lan_start; lan_loop = alle Ziele Loopback (dann kein Broadcast,
 * Bindung Loopback). Rueckgabe Zahl der Ziele. */
int lan_targets(loctun *t)
{
    char tmp[128], *ctx = NULL, *tok;
    int loop = 1;
    t->lan_ntg = 0;
    strncpy_s(tmp, sizeof tmp, t->c.lan_targets, _TRUNCATE);
    for (tok = strtok_s(tmp, ",; ", &ctx); tok && t->lan_ntg < LAN_MAX_TG; tok = strtok_s(NULL, ",; ", &ctx)) {
        char host[64];
        uint16_t port;
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        if (!tl_split_hostport(tok, host, sizeof host, &port) || inet_pton(AF_INET, host, &a.sin_addr) != 1 ||
            !locnet_ip_allowed(a.sin_addr.s_addr)) {
            tl_logf(t, "LOCLAN: loc_lan_targets: Eintrag ist kein privates ip:port - uebergangen");
            continue;
        }
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        if ((ntohl(a.sin_addr.s_addr) >> 24) != 127u) loop = 0;
        t->lan_tg[t->lan_ntg++] = a;
    }
    t->lan_loop = t->lan_ntg > 0 && loop;
    return t->lan_ntg;
}

void lan_stop(loctun *t)
{
    int i;
    if (t->lan_s != INVALID_SOCKET) {
        closesocket(t->lan_s);
        t->lan_s = INVALID_SOCKET;
    }
    for (i = 0; i < LAN_MAX_PEERS; i++) memset(&t->lan_p[i], 0, sizeof t->lan_p[i]);
    SecureZeroMemory(t->lan_psk, sizeof t->lan_psk);
}

int lan_peers(const loctun *t, int *up)
{
    int i, n = 0, u = 0;
    for (i = 0; i < LAN_MAX_PEERS; i++) {
        if (t->lan_p[i].cid && t->lan_p[i].state == 1) {
            n++;
            for (int j = 0; j < TUN_MAX_LINKS; j++) {
                if (t->links[j].used && t->links[j].cid == t->lan_p[i].cid && t->links[j].up) u++;
            }
        }
    }
    if (up) *up = u;
    return n;
}
