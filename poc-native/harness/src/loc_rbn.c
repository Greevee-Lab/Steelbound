/* loc_rbn.c - W346 LOC-ROLLBACK-R4: Aushandlung der Eingabeschicht je Mission. Beschreibung in loc_rbn.h.
 * Reines Modul (keine Sockets, kein Log, kein Spielspeicher).
 *
 * Paket "SBRH" (little endian, Laenge exakt RBN_PKT_HEAD + nent * RBN_PKT_ENT):
 *   0  "SBRH"            4  ver = 1            5  Spielerindex (0..9, 0xFF unbekannt)
 *   6  Flags (b0 Wunsch, b1 in Mission, b2 Host-Entscheidung gueltig, Rest 0)       7  nent (0..4)
 *   8  Kennung (!= 0)    12 Uhrzeit ms         16 Entscheidung: Folge   20 Alter der Entscheidung ms
 *   24 Entscheidung: Modus (0/1 LS/2 R2)   25 Verzoegerung (0 oder 1..8)   26 angewandter Modus (0/1/2)   27 0
 *   28 angewandte Folge  32 Alter der eigenen Mission ms (0 = keine)
 *   je Eintrag: Kennung des Mitspielers, Echo seiner Uhrzeit, Haltezeit ms (u16), Flags (b0 Wunsch gesehen), 0 */
#include "loc_rbn.h"

#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;

#define AGE_MAX 3600000u
#define HOLD_MAX 60000u

typedef struct {
    int used;
    uint64_t src;
    u32 nonce;
    u32 last_rx;
    int want, pidx, in_mission, ack_me;
    u32 ping_t, ping_rx;      /* letzte Uhrzeit des Mitspielers + eigene Empfangszeit (fuer das Echo) */
    double srtt, rttvar;
    int rtt_ok;
    int dec_valid, dec_mode, dec_delay;
    u32 dec_seq, dec_at;      /* Zeitpunkt der Entscheidung in eigener Uhr (Empfang - Alter) */
    int app_mode;
    u32 app_seq, mis_at;      /* Missionsbeginn des Mitspielers in eigener Uhr (0 = keine) */
    int confirmed;            /* Host: R2 fuer die laufende Mission bestaetigt */
} rbn_peer;

struct rbn {
    int want, dmin, dmax, dfix;
    u32 nonce;
    rbn_peer p[RBN_MAXPEER];
    /* eigene Mission */
    int st, why, self, npeers;
    u32 mis_start;
    u32 seq;                  /* Host: Folge der letzten eigenen Entscheidung */
    int dec_mode, dec_delay;  /* Host: Entscheidung der laufenden Mission */
    u32 dec_at;
    u32 epoch;
    int delay;
    u32 app_seq;              /* angewandte Folge (Client: vom Host, Host: eigene) */
    int app_mode;
    u32 last_applied;         /* Client: zuletzt angewandte Folge des Hosts */
    int disagree;
    rbn_stats_t st_;
};

static void wr32(uint8_t *p, u32 v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static u32 rd32(const uint8_t *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u32 rd16(const uint8_t *p) { return (u32)p[0] | ((u32)p[1] << 8); }

rbn_t *rbn_new(int want, uint32_t nonce, int dmin, int dmax, int dfix)
{
    rbn_t *n = (rbn_t *)calloc(1, sizeof(rbn_t));
    if (!n) return NULL;
    if (dmin < 1) dmin = 1;
    if (dmax > 8) dmax = 8;
    if (dmax < dmin) dmax = dmin;
    if (dfix < 0 || dfix > 8) dfix = 0;
    n->want = want ? 1 : 0;
    n->nonce = nonce ? nonce : 0x5EEDu;
    n->dmin = dmin;
    n->dmax = dmax;
    n->dfix = dfix;
    n->self = -1;
    n->st_.rtt_min = 0xFFFFFFFFu;
    return n;
}
void rbn_free(rbn_t *n) { free(n); }

int rbn_is_hello(const uint8_t *b, int len) { return b && len >= 4 && memcmp(b, "SBRH", 4) == 0; }

static int fresh(const rbn_peer *P, u32 now) { return P->used && (u32)(now - P->last_rx) <= RBN_FRESH_MS; }

int rbn_pick_delay(int rtt_ms, int dmin, int dmax)
{
    int d;
    if (rtt_ms < 0) d = 2;
    else d = (rtt_ms / 2 + 35 + 49) / 50; /* W346 M_fern: 90 ms -> 2 (W338 belegt), schwankend/weit -> 3 */
    if (d < dmin) d = dmin;
    if (d > dmax) d = dmax;
    return d;
}

int rbn_rtt_max(const rbn_t *n, uint32_t now)
{
    int i, mx = -1;
    for (i = 0; i < RBN_MAXPEER; i++) {
        const rbn_peer *P = &n->p[i];
        int eff;
        if (!fresh(P, now) || !P->rtt_ok) continue;
        eff = (int)(P->srtt + 2.0 * P->rttvar + 0.5); /* geglaettet + Schwankung (wie TCP) */
        if (eff > mx) mx = eff;
    }
    return mx;
}

int rbn_tx(rbn_t *n, uint32_t now, int self, uint8_t *b, int cap)
{
    int i, ne = 0, fl = 0;
    if (!n || !n->want || cap < RBN_PKT_MAX) return 0;
    memset(b, 0, RBN_PKT_MAX);
    memcpy(b, "SBRH", 4);
    b[4] = 1;
    b[5] = (uint8_t)((self >= 0 && self < 10) ? self : 0xFF);
    fl = 1;
    if (n->st != RBN_ST_IDLE) fl |= 2;
    if (n->self == 0 && n->dec_mode != RBN_MODE_NONE && n->seq) fl |= 4;
    b[6] = (uint8_t)fl;
    wr32(b + 8, n->nonce);
    wr32(b + 12, now);
    if (fl & 4) {
        u32 age = now - n->dec_at;
        wr32(b + 16, n->seq);
        wr32(b + 20, age > AGE_MAX ? AGE_MAX : age);
        b[24] = (uint8_t)n->dec_mode;
        b[25] = (uint8_t)(n->dec_mode == RBN_MODE_R2 ? n->dec_delay : 0);
    }
    b[26] = (uint8_t)n->app_mode;
    wr32(b + 28, n->app_seq);
    if (n->st != RBN_ST_IDLE) {
        u32 age = now - n->mis_start;
        wr32(b + 32, age == 0 ? 1u : (age > AGE_MAX ? AGE_MAX : age));
    }
    for (i = 0; i < RBN_MAXPEER && ne < RBN_PKT_MAXENT; i++) {
        rbn_peer *P = &n->p[i];
        uint8_t *e = b + RBN_PKT_HEAD + ne * RBN_PKT_ENT;
        u32 held;
        if (!fresh(P, now)) continue;
        held = now - P->ping_rx;
        if (held > HOLD_MAX) held = HOLD_MAX;
        wr32(e, P->nonce);
        wr32(e + 4, P->ping_t);
        e[8] = (uint8_t)held;
        e[9] = (uint8_t)(held >> 8);
        e[10] = (uint8_t)(P->want ? 1 : 0);
        e[11] = 0;
        ne++;
    }
    b[7] = (uint8_t)ne;
    n->st_.tx++;
    return RBN_PKT_HEAD + ne * RBN_PKT_ENT;
}

int rbn_rx(rbn_t *n, uint32_t now, const uint8_t *b, int len, uint64_t src)
{
    int i, ne, fl, pidx, slot = -1, dm, dd, am;
    u32 nonce, dage, mage;
    rbn_peer *P;
    if (!n) return 0;
    /* Pass 1: alles pruefen, bevor etwas uebernommen wird */
    if (!b || len < RBN_PKT_HEAD || len > RBN_PKT_MAX || memcmp(b, "SBRH", 4) != 0 || b[4] != 1 || b[27] != 0) goto bad;
    pidx = b[5];
    fl = b[6];
    ne = b[7];
    if ((pidx >= 10 && pidx != 0xFF) || (fl & ~7) || ne > RBN_PKT_MAXENT || len != RBN_PKT_HEAD + ne * RBN_PKT_ENT) goto bad;
    nonce = rd32(b + 8);
    dage = rd32(b + 20);
    mage = rd32(b + 32);
    dm = b[24];
    dd = b[25];
    am = b[26];
    if (!nonce || dm > 2 || dd > 8 || am > 2 || dage > AGE_MAX || mage > AGE_MAX) goto bad;
    if (fl & 4) {
        if (!rd32(b + 16) || dm == RBN_MODE_NONE || (dm == RBN_MODE_R2 && dd < 1) || (dm != RBN_MODE_R2 && dd) ||
            pidx != 0)
            goto bad;
    } else if (rd32(b + 16) || dage || dm || dd) {
        goto bad;
    }
    if (!(fl & 2) && mage) goto bad;
    if ((fl & 2) && !mage) goto bad;
    for (i = 0; i < ne; i++) {
        const uint8_t *e = b + RBN_PKT_HEAD + i * RBN_PKT_ENT;
        if (e[11] || (e[10] & ~1) || rd16(e + 8) > HOLD_MAX || !rd32(e)) goto bad;
    }
    if (nonce == n->nonce) { /* eigenes Paket (Rundruf) */
        n->st_.rx_self++;
        return 0;
    }
    /* Bindung: eine Absenderkennung = ein Eintrag; dieselbe Kennung von anderer Adresse = verworfen */
    for (i = 0; i < RBN_MAXPEER; i++) {
        if (!n->p[i].used) continue;
        if (n->p[i].src == src) slot = i;
        else if (n->p[i].nonce == nonce && fresh(&n->p[i], now)) goto bad;
    }
    if (slot < 0) {
        for (i = 0; i < RBN_MAXPEER; i++)
            if (!n->p[i].used || !fresh(&n->p[i], now)) { slot = i; break; }
        if (slot < 0) {
            n->st_.rx_full++;
            return -1;
        }
        memset(&n->p[slot], 0, sizeof n->p[slot]);
    }
    P = &n->p[slot];
    if (P->used && P->nonce != nonce) memset(P, 0, sizeof *P); /* Neustart des Mitspielers */
    /* Pass 2: uebernehmen */
    P->used = 1;
    P->src = src;
    P->nonce = nonce;
    P->last_rx = now;
    P->want = fl & 1;
    P->pidx = pidx == 0xFF ? -1 : pidx;
    P->in_mission = (fl & 2) ? 1 : 0;
    P->ping_t = rd32(b + 12);
    P->ping_rx = now;
    P->ack_me = 0;
    for (i = 0; i < ne; i++) {
        const uint8_t *e = b + RBN_PKT_HEAD + i * RBN_PKT_ENT;
        if (rd32(e) != n->nonce) continue;
        P->ack_me = e[10] & 1;
        { /* Umlaufzeit: jetzt - Echo - Haltezeit beim Mitspieler */
            u32 rtt = now - rd32(e + 4) - rd16(e + 8);
            if (rtt < 10000u) {
                if (!P->rtt_ok) {
                    P->srtt = rtt;
                    P->rttvar = rtt / 2.0;
                    P->rtt_ok = 1;
                } else {
                    double d = (double)rtt - P->srtt;
                    P->rttvar = 0.75 * P->rttvar + 0.25 * (d < 0 ? -d : d);
                    P->srtt = 0.875 * P->srtt + 0.125 * rtt;
                }
                n->st_.rtt_last = rtt;
                if (rtt < n->st_.rtt_min) n->st_.rtt_min = rtt;
                if (rtt > n->st_.rtt_max) n->st_.rtt_max = rtt;
                n->st_.rtt_n++;
            }
        }
    }
    P->dec_valid = (fl & 4) ? 1 : 0;
    if (P->dec_valid) {
        P->dec_seq = rd32(b + 16);
        P->dec_mode = dm;
        P->dec_delay = dd;
        P->dec_at = now - dage;
    }
    P->app_mode = am;
    P->app_seq = rd32(b + 28);
    P->mis_at = (fl & 2) ? (now - mage) | 1u : 0;
    n->st_.rx_ok++;
    return 1;
bad:
    n->st_.rx_bad++;
    return -1;
}

static void set_ls(rbn_t *n, int why)
{
    n->st = RBN_ST_LS;
    n->why = why;
    n->app_mode = RBN_MODE_LS;
    if (why > 0 && why < RBN_WHY_N) n->st_.fallback[why]++;
}

static void set_r2(rbn_t *n)
{
    n->st = RBN_ST_R2;
    n->why = RBN_WHY_NONE;
    n->app_mode = RBN_MODE_R2;
}

/* Gilt eine Meldung des Mitspielers fuer die laufende Mission? (Missionsbeginne liegen innerhalb der Frist) */
static int same_mission(const rbn_t *n, u32 at)
{
    u32 d = at > n->mis_start ? at - n->mis_start : n->mis_start - at;
    return at != 0 && d <= RBN_WAIT_MS;
}

static rbn_peer *find_host(rbn_t *n, u32 now)
{
    int i;
    for (i = 0; i < RBN_MAXPEER; i++)
        if (fresh(&n->p[i], now) && n->p[i].pidx == 0) return &n->p[i];
    return NULL;
}

int rbn_mission_start(rbn_t *n, uint32_t now, int self, int npeers)
{
    int i, ok = 0;
    if (!n) return RBN_ST_LS;
    n->self = self;
    n->npeers = npeers;
    n->mis_start = now;
    n->disagree = 0;
    n->epoch = 0;
    n->delay = 0;
    n->app_seq = 0;
    n->app_mode = RBN_MODE_NONE;
    for (i = 0; i < RBN_MAXPEER; i++) n->p[i].confirmed = 0;
    if (!n->want) {
        set_ls(n, RBN_WHY_SELF_OFF);
        return n->st;
    }
    if (self == 0) { /* Host entscheidet */
        for (i = 0; i < RBN_MAXPEER; i++) {
            rbn_peer *P = &n->p[i];
            if (fresh(P, now) && P->want && P->ack_me && P->pidx != 0) ok++; /* Index steht in der Lobby oft noch nicht fest */
        }
        n->seq++;
        n->dec_at = now;
        n->app_seq = n->seq;
        n->epoch = (n->nonce & 0xFFFF0000u) ^ (n->seq & 0xFFFFu);
        if (npeers < 1 || ok < npeers) {
            n->dec_mode = RBN_MODE_LS;
            n->dec_delay = 0;
            n->st_.decisions_ls++;
            set_ls(n, RBN_WHY_PEER_NO);
            return n->st;
        }
        n->dec_mode = RBN_MODE_R2;
        n->dec_delay = n->dfix ? n->dfix : rbn_pick_delay(rbn_rtt_max(n, now), n->dmin, n->dmax);
        n->delay = n->dec_delay;
        n->st_.decisions_r2++;
        n->st = RBN_ST_TENT;
        n->why = RBN_WHY_NONE;
        n->app_mode = RBN_MODE_R2; /* angeboten */
        return n->st;
    }
    for (i = 0; i < RBN_MAXPEER; i++) /* Client: ein R2-faehiger Mitspieler (der Host) muss bekannt sein */
        if (fresh(&n->p[i], now) && n->p[i].want && n->p[i].ack_me && n->p[i].pidx != n->self) ok++;
    if (!ok) {
        set_ls(n, RBN_WHY_HOST_NO);
        return n->st;
    }
    n->st = RBN_ST_PEND;
    n->why = RBN_WHY_NONE;
    return n->st;
}

void rbn_confirm_peer(rbn_t *n, uint64_t src)
{
    int i;
    if (!n || n->st != RBN_ST_TENT) return;
    for (i = 0; i < RBN_MAXPEER; i++)
        if (n->p[i].used && n->p[i].src == src) n->p[i].confirmed = 1;
}

int rbn_poll(rbn_t *n, uint32_t now)
{
    int i;
    if (!n) return RBN_ST_LS;
    if (n->st == RBN_ST_TENT) {
        int conf = 0;
        for (i = 0; i < RBN_MAXPEER; i++) {
            rbn_peer *P = &n->p[i];
            if (!P->used || P->pidx <= 0) continue;
            if (P->app_mode == RBN_MODE_LS && P->in_mission && same_mission(n, P->mis_at)) { /* Client: Lockstep */
                n->dec_mode = RBN_MODE_LS;
                set_ls(n, RBN_WHY_PEER_LS);
                return n->st;
            }
            if (P->app_mode == RBN_MODE_R2 && P->app_seq == n->seq) P->confirmed = 1;
            if (P->confirmed) conf++;
        }
        if (conf >= n->npeers) {
            set_r2(n);
        } else if ((u32)(now - n->mis_start) > RBN_WAIT_MS) {
            n->dec_mode = RBN_MODE_LS; /* Entscheidung fuer alle sichtbar auf Lockstep */
            set_ls(n, RBN_WHY_TIMEOUT);
        }
        return n->st;
    }
    if (n->st == RBN_ST_PEND) {
        rbn_peer *H = find_host(n, now);
        if (H && H->dec_valid && H->dec_seq > n->last_applied && same_mission(n, H->dec_at | 1u)) {
            n->last_applied = H->dec_seq;
            n->app_seq = H->dec_seq;
            if (H->dec_mode == RBN_MODE_R2) {
                n->epoch = (H->nonce & 0xFFFF0000u) ^ (H->dec_seq & 0xFFFFu);
                n->delay = H->dec_delay;
                set_r2(n);
            } else {
                set_ls(n, RBN_WHY_HOST_LS);
            }
        } else if ((u32)(now - n->mis_start) > RBN_WAIT_MS) {
            set_ls(n, RBN_WHY_TIMEOUT);
        }
        return n->st;
    }
    if (n->st == RBN_ST_R2) { /* Uneinigkeit erkennen (sollte nicht vorkommen): Mitspieler meldet Lockstep */
        for (i = 0; i < RBN_MAXPEER; i++) {
            rbn_peer *P = &n->p[i];
            if (!fresh(P, now) || P->pidx < 0) continue;
            if (n->self == 0 && P->pidx > 0 && P->app_mode == RBN_MODE_LS && P->in_mission && same_mission(n, P->mis_at))
                n->disagree = 1;
            if (n->self != 0 && P->pidx == 0 && P->dec_valid && P->dec_seq == n->app_seq && P->dec_mode == RBN_MODE_LS)
                n->disagree = 1;
        }
    }
    return n->st;
}

void rbn_mission_end(rbn_t *n)
{
    if (!n) return;
    n->st = RBN_ST_IDLE;
    n->app_mode = RBN_MODE_NONE;
    n->app_seq = 0;
    n->disagree = 0;
}

int rbn_state(const rbn_t *n) { return n ? n->st : RBN_ST_LS; }
uint32_t rbn_epoch(const rbn_t *n) { return n ? n->epoch : 0; }
int rbn_delay(const rbn_t *n) { return n ? n->delay : 0; }
int rbn_why(const rbn_t *n) { return n ? n->why : 0; }
int rbn_peer_disagrees(const rbn_t *n) { return n ? n->disagree : 0; }
const rbn_stats_t *rbn_stats(const rbn_t *n) { return &n->st_; }
