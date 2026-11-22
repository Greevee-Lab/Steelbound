/* loc_rule.c - W420 LOC-CLIPDUMP: Match-Regeln des Hosts, reiner Kern. Beschreibung in loc_rule.h.
 *
 * Paket "SBRU" (little endian, Laenge exakt 16):
 *   0 "SBRU"   4 ver = 1   5 Regelsatz (>= 1)   6 Spielerindex (0..9, 0xFF unbekannt)   7 Flags (b0 in der Mission)
 *   8 Kennung (!= 0, zufaellig je Prozess)   12 Host-Einstellung (Regel-Bits)   13 angewandte Regel (nur in der Mission,
 *   sonst 0)   14 u16 0
 * Regelsatz gleich dem eigenen: nur bekannte Bits. Neuerer Regelsatz: unbekannte Bits erlaubt (Aufrufer lehnt ab). */
#include "loc_rule.h"

#include <string.h>

typedef uint32_t u32;

static void wr32(uint8_t *p, u32 v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static u32 rd32(const uint8_t *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

void lru_init(lru_t *r, uint32_t nonce)
{
    memset(r, 0, sizeof *r);
    r->nonce = nonce ? nonce : 0x5EEDu;
}

int lru_build(uint8_t *b, int cap, uint32_t nonce, int pidx, unsigned pref, int in_mission, unsigned applied)
{
    if (!b || cap < LRU_PKT_LEN || !nonce) return 0;
    memset(b, 0, LRU_PKT_LEN);
    memcpy(b, "SBRU", 4);
    b[4] = 1;
    b[5] = LRU_RULESET;
    b[6] = (uint8_t)((pidx >= 0 && pidx < 10) ? pidx : 0xFF);
    b[7] = (uint8_t)(in_mission ? 1 : 0);
    wr32(b + 8, nonce);
    b[12] = (uint8_t)(pref & LRU_KNOWN);
    b[13] = (uint8_t)(in_mission ? (applied & LRU_KNOWN) : 0);
    return LRU_PKT_LEN;
}

int lru_parse(const uint8_t *b, int len, lru_pkt *o)
{
    unsigned known;
    if (!b || len != LRU_PKT_LEN || memcmp(b, "SBRU", 4) != 0 || b[4] != 1) return 0;
    if (b[5] < 1 || (b[6] >= 10 && b[6] != 0xFF) || (b[7] & ~1u) || !rd32(b + 8) || b[14] || b[15]) return 0;
    if (!(b[7] & 1u) && b[13]) return 0; /* angewandte Regel nur in der Mission */
    known = b[5] <= LRU_RULESET ? LRU_KNOWN : 0xFFu; /* neuerer Regelsatz: Bits unbekannt, aber zulaessig */
    if ((b[12] & ~known) || (b[13] & ~known)) return 0;
    if (o) {
        o->ver = b[4];
        o->ruleset = b[5];
        o->pidx = b[6] == 0xFF ? -1 : b[6];
        o->in_mission = b[7] & 1;
        o->nonce = rd32(b + 8);
        o->pref = b[12];
        o->applied = b[13];
    }
    return 1;
}

int lru_fresh(const lru_peer *P, uint32_t now) { return P && P->used && (u32)(now - P->last_ms) <= LRU_FRESH_MS; }

int lru_rx(lru_t *r, uint32_t now, uint32_t ip, const uint8_t *b, int len)
{
    lru_pkt k;
    int i, slot = -1;
    lru_peer *P;
    if (!r) return 0;
    if (!lru_parse(b, len, &k) || !ip) { /* Pass 1: alles pruefen */
        r->rx_bad++;
        return -1;
    }
    if (k.nonce == r->nonce) {
        r->rx_self++;
        return 0;
    }
    for (i = 0; i < LRU_MAXPEER; i++) { /* Bindung: eine Adresse = ein Eintrag; fremde Kennung einer frischen Adresse */
        if (!r->p[i].used) continue;
        if (r->p[i].ip == ip) slot = i;
        else if (r->p[i].nonce == k.nonce && lru_fresh(&r->p[i], now)) {
            r->rx_bad++;
            return -1;
        }
    }
    if (slot < 0) {
        for (i = 0; i < LRU_MAXPEER; i++)
            if (!r->p[i].used || !lru_fresh(&r->p[i], now)) { slot = i; break; }
        if (slot < 0) {
            r->rx_full++;
            return -1;
        }
        memset(&r->p[slot], 0, sizeof r->p[slot]);
    }
    P = &r->p[slot];
    if (!P->used || P->nonce != k.nonce) { /* neu bzw. Neustart des Mitspielers */
        memset(P, 0, sizeof *P);
        P->first_ms = now;
    }
    P->used = 1; /* Pass 2: uebernehmen */
    P->ip = ip;
    P->nonce = k.nonce;
    P->last_ms = now;
    P->ruleset = k.ruleset;
    P->pidx = k.pidx;
    P->in_mission = k.in_mission;
    P->pref = k.pref;
    P->applied = k.applied;
    r->rx_ok++;
    return 1;
}

const lru_peer *lru_find(const lru_t *r, uint32_t ip)
{
    int i;
    if (!r || !ip) return NULL;
    for (i = 0; i < LRU_MAXPEER; i++)
        if (r->p[i].used && r->p[i].ip == ip) return &r->p[i];
    return NULL;
}

unsigned lru_decide(const lru_t *r, int role, uint32_t host_ip, unsigned local_pref, int *src)
{
    const lru_peer *P;
    int s;
    unsigned v;
    if (role == LRU_ROLE_HOST) {
        s = LRU_SRC_HOST_SELF;
        v = local_pref & LRU_KNOWN;
    } else if (role == LRU_ROLE_JOIN) {
        P = lru_find(r, host_ip);
        if (!P) { /* Host ohne Regel-Feld (aelterer Client) = Original */
            s = LRU_SRC_HOST_OLD;
            v = 0;
        } else if (P->ruleset > LRU_RULESET && (P->pref & ~LRU_KNOWN)) {
            s = LRU_SRC_HOST_NEWER; /* unbekannte Regel aktiv: nicht nachbildbar -> ablehnen */
            v = 0;
        } else {
            s = LRU_SRC_HOST;
            v = P->pref & LRU_KNOWN;
        }
    } else {
        s = LRU_SRC_LOCAL;
        v = local_pref & LRU_KNOWN;
    }
    if (src) *src = s;
    return v;
}

int lru_admit(const lru_t *r, uint32_t ip, unsigned host_rule, uint32_t now, uint32_t connected_ms)
{
    if (!(host_rule & LRU_KNOWN)) return 1;  /* Original: auch aeltere Clients spielen gleich */
    if (lru_find(r, ip)) return 1;           /* kennt Regel-Feld, uebernimmt die Regel des Hosts */
    return (u32)(now - connected_ms) < LRU_ADMIT_MS; /* Frist fuer das erste SBRU, danach aelterer Client */
}

int lru_mismatch(const lru_peer *P, unsigned applied, uint32_t now)
{
    return lru_fresh(P, now) && P->in_mission && (P->applied & LRU_KNOWN) != (applied & LRU_KNOWN);
}

const char *lru_src_name(int src)
{
    static const char *const k[LRU_SRC_N] = {"lokal", "host", "host", "host-alt", "host-neuer"};
    return (src >= 0 && src < LRU_SRC_N) ? k[src] : "?";
}

int lru_phase_lobby(lru_phase *p, uint32_t host_mark, uint32_t join_mark)
{
    if (!p->frozen || (host_mark == p->host_mark && join_mark == p->join_mark)) return 0;
    p->frozen = 0;
    return 1;
}

int lru_phase_mission(lru_phase *p, int mission_ev, uint32_t host_mark, uint32_t join_mark)
{
    if (p->frozen || !mission_ev) return 0;
    p->frozen = 1;
    p->host_mark = host_mark;
    p->join_mark = join_mark;
    return 1;
}
