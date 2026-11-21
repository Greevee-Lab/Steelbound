/* loc_rbn.c - W346 LOC-ROLLBACK-R4: Aushandlung der Eingabeschicht je Mission. Beschreibung in loc_rbn.h.
 * Reines Modul (keine Sockets, kein Log, kein Spielspeicher).
 *
 * Paket "SBRH" (little endian, Laenge exakt RBN_PKT_HEAD + nent * RBN_PKT_ENT):
 *   0  "SBRH"            4  ver = 1            5  Spielerindex (0..9, 0xFF unbekannt)
 *   6  Flags (b0 Wunsch, b1 in Mission, b2 Host-Entscheidung gueltig, Rest 0)       7  nent (0..4)
 *   8  Kennung (!= 0)    12 Uhrzeit ms         16 Entscheidung: Folge   20 Alter der Entscheidung ms
 *   24 Entscheidung: Modus (0/1 LS/2 R2)   25 Verzoegerung (0 oder 1..8)   26 angewandter Modus (0/1/2)
 *   27 Lockstep-Grund RBN_WHY_* (W370; nur bei Lockstep-Entscheidung des Hosts, sonst 0)
 *   28 angewandte Folge  32 Alter der eigenen Mission ms (0 = keine)
 *   je Eintrag: Kennung des Mitspielers, Echo seiner Uhrzeit, Haltezeit ms (u16), Flags (b0 Wunsch gesehen), 0 */
#include "loc_rbn.h"

#include <stdio.h>
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
    int dec_valid, dec_mode, dec_delay, dec_why; /* W370: dec_why = Lockstep-Grund des Hosts (Byte 27) */
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
    int dec_why;              /* W370 Host: Grund einer Lockstep-Entscheidung (im Hallo, Byte 27) */
    int hwait;                /* W370 Host: Nachfrist, Entscheidung steht noch aus */
    int host_why;             /* W370 Client: Grund des Hosts bei RBN_WHY_HOST_LS */
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
        if (n->dec_mode == RBN_MODE_LS && n->dec_why > 0 && n->dec_why < RBN_WHY_N) b[27] = (uint8_t)n->dec_why;
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
    if (!b || len < RBN_PKT_HEAD || len > RBN_PKT_MAX || memcmp(b, "SBRH", 4) != 0 || b[4] != 1) goto bad;
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
    /* W370: Byte 27 = Lockstep-Grund, nur bei einer Lockstep-Entscheidung des Hosts, sonst 0 */
    if (b[27] && (!(fl & 4) || dm != RBN_MODE_LS || b[27] >= RBN_WHY_N)) goto bad;
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
        P->dec_why = b[27];
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

/* W370: Host zaehlt Mitspieler (ohne Host-Index 0): ok = frisch, mit Wunsch und Bestaetigung des eigenen Wunsches;
 * known = irgendwann mit Wunsch gesehen (Eintrag belegt). */
static void host_count(const rbn_t *n, u32 now, int *ok, int *known)
{
    int i;
    *ok = *known = 0;
    for (i = 0; i < RBN_MAXPEER; i++) {
        const rbn_peer *P = &n->p[i];
        if (!P->used) continue;
        if (P->want) (*known)++; /* auch Index 0: veraltet von einer frueheren Runde als Host (wechselt beim Missionsbeginn) */
        if (P->pidx != 0 && fresh(P, now) && P->want && P->ack_me) (*ok)++; /* Index in der Lobby oft noch -1 */
    }
}

/* W370: Client: R2-faehiger Mitspieler (der Host) bekannt? need_fresh = 1: jetzt frisch und bestaetigt, 2: jetzt frisch
 * (Hallo kommt an, Bestaetigung kann fehlen), 0: je gesehen. */
static int client_host_known(const rbn_t *n, u32 now, int need_fresh)
{
    int i;
    for (i = 0; i < RBN_MAXPEER; i++) {
        const rbn_peer *P = &n->p[i];
        if (!P->used || !P->want || P->pidx == n->self) continue;
        if (!need_fresh || (fresh(P, now) && (need_fresh == 2 || P->ack_me))) return 1;
    }
    return 0;
}

static void host_set_ls(rbn_t *n, int why)
{
    n->hwait = 0;
    n->dec_mode = RBN_MODE_LS; /* Entscheidung fuer alle sichtbar auf Lockstep (mit Grund, W370) */
    n->dec_delay = 0;
    n->dec_why = why;
    set_ls(n, why);
}

/* Host entscheidet (Missionsbeginn bzw. Ende der Nachfrist). final = 1: keine weitere Nachfrist. */
static int host_decide(rbn_t *n, u32 now, int final)
{
    int ok, known;
    host_count(n, now, &ok, &known);
    if (n->npeers >= 1 && ok < n->npeers && !final && known >= n->npeers) { /* W370: Nachfrist */
        n->hwait = 1;
        n->st = RBN_ST_TENT;
        n->why = RBN_WHY_NONE;
        n->dec_mode = RBN_MODE_NONE; /* noch keine Entscheidung im Hallo */
        n->app_mode = RBN_MODE_NONE;
        return n->st;
    }
    n->seq++;
    n->dec_at = now;
    n->app_seq = n->seq;
    n->epoch = (n->nonce & 0xFFFF0000u) ^ (n->seq & 0xFFFFu);
    if (n->npeers < 1 || ok < n->npeers) {
        n->st_.decisions_ls++;
        host_set_ls(n, RBN_WHY_PEER_NO);
        return n->st;
    }
    n->hwait = 0;
    n->dec_mode = RBN_MODE_R2;
    n->dec_why = 0;
    n->dec_delay = n->dfix ? n->dfix : rbn_pick_delay(rbn_rtt_max(n, now), n->dmin, n->dmax);
    n->delay = n->dec_delay;
    n->st_.decisions_r2++;
    n->st = RBN_ST_TENT;
    n->why = RBN_WHY_NONE;
    n->app_mode = RBN_MODE_R2; /* angeboten */
    return n->st;
}

int rbn_mission_start(rbn_t *n, uint32_t now, int self, int npeers)
{
    int i;
    if (!n) return RBN_ST_LS;
    n->self = self;
    n->npeers = npeers;
    n->mis_start = now;
    n->disagree = 0;
    n->epoch = 0;
    n->delay = 0;
    n->app_seq = 0;
    n->app_mode = RBN_MODE_NONE;
    n->hwait = 0;
    n->host_why = 0;
    for (i = 0; i < RBN_MAXPEER; i++) n->p[i].confirmed = 0;
    if (!n->want) {
        set_ls(n, RBN_WHY_SELF_OFF);
        return n->st;
    }
    if (self == 0) return host_decide(n, now, 0); /* Host entscheidet */
    /* Client: ein R2-faehiger Mitspieler (der Host) muss bekannt sein; W370: kurz fehlendes Hallo = Nachfrist */
    if (!client_host_known(n, now, 1)) {
        if (!client_host_known(n, now, 0)) {
            set_ls(n, RBN_WHY_HOST_NO);
            return n->st;
        }
        n->hwait = 1;
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
    if (n->st == RBN_ST_TENT && n->hwait) return host_decide(n, now, (u32)(now - n->mis_start) > RBN_GRACE_MS); /* W370 */
    if (n->st == RBN_ST_TENT) {
        int conf = 0;
        for (i = 0; i < RBN_MAXPEER; i++) {
            rbn_peer *P = &n->p[i];
            if (!P->used || P->pidx <= 0) continue;
            if (P->app_mode == RBN_MODE_LS && P->in_mission && same_mission(n, P->mis_at)) { /* Client: Lockstep */
                host_set_ls(n, RBN_WHY_PEER_LS);
                return n->st;
            }
            if (P->app_mode == RBN_MODE_R2 && P->app_seq == n->seq) P->confirmed = 1;
            if (P->confirmed) conf++;
        }
        if (conf >= n->npeers) {
            set_r2(n);
        } else if ((u32)(now - n->mis_start) > RBN_WAIT_MS) {
            host_set_ls(n, RBN_WHY_TIMEOUT);
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
                n->host_why = H->dec_why;
                set_ls(n, RBN_WHY_HOST_LS);
            }
        } else if (n->hwait && !client_host_known(n, now, 2) && (u32)(now - n->mis_start) > RBN_GRACE_MS) {
            set_ls(n, RBN_WHY_HOST_NO); /* W370: Nachfrist ohne frisches Hallo des Hosts abgelaufen (sonst auf ihn warten) */
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
int rbn_host_why(const rbn_t *n) { return n ? n->host_why : 0; }
int rbn_host_waiting(const rbn_t *n) { return n && n->st == RBN_ST_TENT && n->hwait; }

int rbn_peers_desc(const rbn_t *n, uint32_t now, char *buf, int cap)
{
    int i, len = 0, k = 0;
    if (!buf || cap <= 0) return 0;
    buf[0] = 0;
    if (!n) return 0;
    for (i = 0; i < RBN_MAXPEER && len < cap - 1; i++) {
        const rbn_peer *P = &n->p[i];
        int w;
        u32 age;
        if (!P->used) continue;
        age = now - P->last_rx;
        if (age > 999999u) age = 999999u;
        w = snprintf(buf + len, (size_t)(cap - len), "%sPlatz %d Wunsch %d bestaetigt %d Alter %u ms RTT %d ms%s",
                     k ? "; " : "", P->pidx, P->want, P->ack_me, age, P->rtt_ok ? (int)(P->srtt + 0.5) : -1,
                     P->in_mission ? " in Mission" : "");
        if (w < 0) break;
        len += w;
        if (len >= cap) len = cap - 1;
        k++;
    }
    if (!k) snprintf(buf, (size_t)cap, "keine");
    return k;
}
int rbn_peer_disagrees(const rbn_t *n) { return n ? n->disagree : 0; }
const rbn_stats_t *rbn_stats(const rbn_t *n) { return &n->st_; }

/* ================= W386 LOC-LATENCY: niedrige Verzoegerung (Beschreibung in loc_rbn.h) =================
 * Paket "SBRL" (little endian, Laenge exakt RBL_LEN = 32):
 *   0  "SBRL"   4 ver = 1   5 Flags (b0 Ring bereit, b1 Entscheidung gueltig - nur Spieler 0; Rest 0)
 *   6  Spielerindex (0..9)  7 angewandtes D (1..8)   8 Epoche (wie SBR2)
 *   12 Fenster (u16, >= 1)  14 Korrekturen/min (u16)  16 geschaetzt bei D-1 /min (u16)  18 Sprung (u16)
 *   20 Tiefe (u8)  21 geschaetzte Tiefe (u8)  22 Neurechnen p95 0,1 ms (u8)  23 Haenger (u8)
 *   24 Folge (u16; Host: seine Entscheidung, sonst die zuletzt angewandte)   26 Entscheidung D (1..8 nur mit b1, sonst 0)
 *   27 0   28 Entscheidung ab Slot (u32, <= 0x40000000, nur mit b1, sonst 0) */
#define RBL_SLOT_MAX 0x40000000u

typedef struct {
    int have;
    rbl_rep_t r;
    u32 t;          /* Empfangszeit (ms) */
    unsigned used;  /* zuletzt ausgewertete Fensternummer */
    unsigned ack;   /* angewandte Folge */
} rbl_peer;

struct rbl {
    rbl_cfg_t c;
    int active, self, npeers, d_std, d_low;
    u32 epoch;
    int target;               /* Host: aktuelles Ziel */
    int good, bad;            /* Fenster in Folge */
    u32 cool_until;
    int cool_set, ups;
    unsigned seq;             /* Host: Folge der letzten Entscheidung; Client: zuletzt gesehene Folge des Hosts */
    int dec_d;
    u32 dec_slot;
    unsigned applied;         /* zuletzt angewandte Folge */
    int dec_new;
    rbl_peer p[RBL_MAXP];
    uint8_t hh[6];            /* W386: Haenger der letzten Fenster in der niedrigen Stufe (groessere Seite) */
    int hn;
    rbl_stats_t st;
};

static void wr16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static unsigned clampu(unsigned v, unsigned mx) { return v > mx ? mx : v; }

void rbl_cfg_default(rbl_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->on = 1;
    c->lowmax = 300; /* gemessen wie rbn_rtt_max (Mittel + 2 Schwankung): eu-jp (250 ms nominal) misst ~270-280 */
    c->lcpm = 300;  /* 5/s geschaetzt: W385 K4 (D 1, 150 ms, Maussturm, Trend) 147/min */
    c->hcpm = 600;  /* 10/s */
    c->ldep = 2;
    c->hdep = 4;
    c->lrp = 30;    /* 3 ms */
    c->hrp = 50;    /* 5 ms (W385 LOC_BOOT 59.4: Rueckfall bei Neurechnen > 5 ms) */
    c->lhang = 1;
    c->hhang = 3;   /* D 2 hat ausserhalb des Missionsstarts ~0,1-0,25 Haenger je 10 s (W385 K2) */
    c->hjump = 500; /* 50 cm; W385: Ausweichen bei D 1 hoechstens 110 (geglaettet) */
    c->hang6 = 6;   /* 1 min; nachgerechnet auf den Laeufen: N150/N100b nie, WLAN (Nwlan/Nwlan2) und N250 nach 3-12 Fenstern */
    c->good_n = 2;
    c->cool_s = 60;
    c->max_up = 3;
    c->lead = 10;   /* 0,5 s */
}

int rbn_pick_low(int rtt_ms, int d_std, int lowmax)
{
    if (rtt_ms < 0 || rtt_ms > lowmax || d_std <= 1) return d_std;
    return d_std - 1;
}

rbl_t *rbl_new(void) { return (rbl_t *)calloc(1, sizeof(rbl_t)); }
void rbl_free(rbl_t *l) { free(l); }

void rbl_begin(rbl_t *l, const rbl_cfg_t *c, uint32_t epoch, int self, int npeers, int d_std, int d_low)
{
    rbl_stats_t keep;
    if (!l) return;
    keep = l->st;
    memset(l, 0, sizeof *l);
    l->st = keep;
    l->c = *c;
    if (d_std < 1) d_std = 1;
    if (d_std > 8) d_std = 8;
    if (d_low < 1 || d_low > d_std) d_low = d_std;
    l->self = self;
    l->npeers = npeers;
    l->d_std = d_std;
    l->d_low = d_low;
    l->target = d_std;
    l->epoch = epoch;
    l->active = 1;
}
void rbl_end(rbl_t *l)
{
    if (l) l->active = 0;
}
int rbl_active(const rbl_t *l) { return l && l->active; }
int rbl_target(const rbl_t *l) { return l ? (l->self == 0 ? l->target : l->dec_d) : 0; }
int rbl_dstd(const rbl_t *l) { return l ? l->d_std : 0; }
int rbl_dlow(const rbl_t *l) { return l ? l->d_low : 0; }
const rbl_stats_t *rbl_stats(const rbl_t *l) { return &l->st; }

int rbl_is_pkt(const uint8_t *b, int len) { return b && len >= 4 && memcmp(b, "SBRL", 4) == 0; }
int rbl_peek_pidx(const uint8_t *b, int len) { return (rbl_is_pkt(b, len) && len == RBL_LEN && b[6] < RBL_MAXP) ? b[6] : -1; }

int rbl_tx(rbl_t *l, const rbl_rep_t *o, uint8_t *b, int cap)
{
    int host;
    if (!l || !l->active || !o || cap < RBL_LEN || l->self < 0 || l->self >= RBL_MAXP || o->dcur < 1 || o->dcur > 8)
        return 0;
    host = l->self == 0;
    memset(b, 0, RBL_LEN);
    memcpy(b, "SBRL", 4);
    b[4] = 1;
    b[5] = (uint8_t)((o->ready ? 1 : 0) | (host && l->seq ? 2 : 0));
    b[6] = (uint8_t)l->self;
    b[7] = (uint8_t)o->dcur;
    wr32(b + 8, l->epoch);
    wr16(b + 12, o->win ? clampu(o->win, 0xFFFFu) : 1u);
    wr16(b + 14, clampu(o->cpm, 0xFFFFu));
    wr16(b + 16, clampu(o->est_cpm, 0xFFFFu));
    wr16(b + 18, clampu(o->jump, 0xFFFFu));
    b[20] = (uint8_t)clampu(o->dep, 255u);
    b[21] = (uint8_t)clampu(o->est_dep, 255u);
    b[22] = (uint8_t)clampu(o->rp, 255u);
    b[23] = (uint8_t)clampu(o->hang, 255u);
    if (host && l->seq) {
        wr16(b + 24, l->seq & 0xFFFFu);
        b[26] = (uint8_t)l->dec_d;
        wr32(b + 28, l->dec_slot);
    } else {
        wr16(b + 24, l->applied & 0xFFFFu);
    }
    l->st.tx++;
    return RBL_LEN;
}

int rbl_rx(rbl_t *l, uint32_t now, const uint8_t *b, int len)
{
    int fl, pidx, dc, dd;
    unsigned win, seq;
    u32 slot;
    rbl_peer *P;
    if (!l) return 0;
    /* Pass 1: alles pruefen */
    if (!b || len != RBL_LEN || memcmp(b, "SBRL", 4) != 0 || b[4] != 1) goto bad;
    fl = b[5];
    pidx = b[6];
    dc = b[7];
    win = rd16(b + 12);
    seq = rd16(b + 24);
    dd = b[26];
    slot = rd32(b + 28);
    if ((fl & ~3) || pidx >= RBL_MAXP || dc < 1 || dc > 8 || !win || b[27]) goto bad;
    if (fl & 2) {
        if (pidx != 0 || !seq || dd < 1 || dd > 8 || slot > RBL_SLOT_MAX) goto bad;
    } else if (dd || slot) {
        goto bad;
    }
    if (!l->active || rd32(b + 8) != l->epoch) {
        l->st.rx_epoch++;
        return 0;
    }
    if (pidx == l->self) goto bad; /* eigener Index von fremder Adresse (die Bindung prueft die Anbindung) */
    /* Pass 2: uebernehmen */
    P = &l->p[pidx];
    if (!P->have) l->st.peers_seen++;
    P->have = 1;
    P->t = now;
    P->r.ready = fl & 1;
    P->r.dcur = dc;
    P->r.win = win;
    P->r.cpm = rd16(b + 14);
    P->r.est_cpm = rd16(b + 16);
    P->r.jump = rd16(b + 18);
    P->r.dep = b[20];
    P->r.est_dep = b[21];
    P->r.rp = b[22];
    P->r.hang = b[23];
    if (fl & 2) { /* Entscheidung des Hosts (Client) */
        if (l->self != 0 && seq != l->seq) {
            l->seq = seq;
            l->dec_d = dd;
            l->dec_slot = slot;
            l->dec_new = 1;
        }
    } else {
        P->ack = seq;
    }
    l->st.rx_ok++;
    return 1;
bad:
    l->st.rx_bad++;
    return -1;
}

static void rbl_decide(rbl_t *l, int d, int why, u32 next_slot, int rtt_ms)
{
    int lead = l->c.lead + (rtt_ms > 0 ? (rtt_ms + 49) / 50 : 4);
    if (lead > 60) lead = 60;
    l->target = d;
    l->seq = (l->seq + 1u) & 0xFFFFu;
    if (!l->seq) l->seq = 1;
    l->dec_d = d;
    l->dec_slot = next_slot + (u32)lead;
    if (l->dec_slot > RBL_SLOT_MAX) l->dec_slot = RBL_SLOT_MAX;
    l->dec_new = 1;
    l->good = l->bad = 0;
    l->hn = 0;
    l->st.last_why = (u32)why;
    if (d < l->d_std) l->st.down++;
    else l->st.up++;
}

int rbl_host_step(rbl_t *l, uint32_t now, const rbl_rep_t *own, uint32_t next_slot, int rtt_ms)
{
    const rbl_rep_t *R[RBL_MAXP + 1];
    int n = 0, i, k, all, newwin = 1;
    if (!l || !l->active || l->self != 0 || !own || l->d_low >= l->d_std) return 0; /* nichts zu regeln */
    R[n++] = own;
    for (i = 0; i < RBL_MAXP; i++) {
        rbl_peer *P = &l->p[i];
        if (i == l->self || !P->have || (u32)(now - P->t) > 2u * RBL_WIN_MS) continue;
        if (P->r.win == P->used) newwin = 0; /* kein neues Fenster dieses Mitspielers */
        R[n++] = &P->r;
    }
    all = n - 1 >= l->npeers && l->npeers >= 1;
    for (k = 0; k < n; k++)
        if (R[k]->dcur != l->target || !newwin) { /* Wechsel laeuft noch bzw. kein neues Fenster: nicht bewerten */
            l->st.skipped++;
            return 0;
        }
    l->st.evals++;
    for (i = 0; i < RBL_MAXP; i++)
        if (l->p[i].have) l->p[i].used = l->p[i].r.win;
    if (l->target > l->d_low) { /* hoch: senken? */
        int good = all && l->c.on;
        for (k = 0; k < n && good; k++) {
            const rbl_rep_t *r = R[k];
            if (!r->ready || (int)r->est_cpm > l->c.lcpm || (int)r->est_dep > l->c.ldep || (int)r->rp > l->c.lrp ||
                (int)r->hang > l->c.lhang)
                good = 0;
        }
        l->good = good ? l->good + 1 : 0;
        if (l->good >= l->c.good_n && (!l->cool_set || (int32_t)(now - l->cool_until) >= 0) && l->ups < l->c.max_up) {
            rbl_decide(l, l->d_low, RBL_WHY_GOOD, next_slot, rtt_ms);
            return 1;
        }
        return 0;
    }
    { /* niedrig: zurueck? */
        int sev = 0, bad = 0, why = RBL_WHY_NONE;
        for (k = 0; k < n; k++) {
            const rbl_rep_t *r = R[k];
            if (!r->ready) { sev = 1; why = RBL_WHY_NOTREADY; }
            else if ((int)r->rp > l->c.hrp) { sev = 1; why = RBL_WHY_REPLAY; }
            else if ((int)r->hang >= 2 * l->c.hhang) { sev = 1; why = RBL_WHY_HANG; }
            else if ((int)r->dep >= l->c.hdep + 2) { sev = 1; why = RBL_WHY_DEPTH; }
            else if ((int)r->cpm > l->c.hcpm) { bad = 1; if (!why) why = RBL_WHY_CPM; }
            else if ((int)r->dep >= l->c.hdep) { bad = 1; if (!why) why = RBL_WHY_DEPTH; }
            else if ((int)r->hang >= l->c.hhang) { bad = 1; if (!why) why = RBL_WHY_HANG; }
            else if ((int)r->jump > l->c.hjump) { bad = 1; if (!why) why = RBL_WHY_JUMP; }
        }
        if (!all && !sev) { bad = 1; if (!why) why = RBL_WHY_NOTREADY; } /* Mitspieler meldet sich nicht mehr */
        { /* W386: Haenger ueber 6 Fenster (einzelne Fenster sind verrauscht, die Summe trennt WLAN von sauberen Wegen) */
            unsigned wm = 0, sum = 0;
            int j;
            for (k = 0; k < n; k++)
                if (R[k]->hang > wm) wm = R[k]->hang;
            l->hh[l->hn % 6] = (uint8_t)(wm > 255u ? 255u : wm);
            l->hn++;
            for (j = 0; j < 6 && j < l->hn; j++) sum += l->hh[j];
            if (l->c.hang6 > 0 && (int)sum >= l->c.hang6 && !sev) {
                sev = 1;
                why = RBL_WHY_HANG;
            }
        }
        l->bad = (bad || sev) ? l->bad + 1 : 0;
        if (sev || l->bad >= 2) {
            int f = l->ups < 2 ? l->ups : 2;
            l->ups++;
            l->cool_until = now + (u32)l->c.cool_s * 1000u * (1u << f);
            l->cool_set = 1;
            rbl_decide(l, l->d_std, why, next_slot, rtt_ms);
            return 1;
        }
    }
    return 0;
}

int rbl_force(rbl_t *l, int d, uint32_t next_slot, int rtt_ms)
{
    if (!l || !l->active || l->self != 0 || d < 1 || d > 8 || d == l->target) return 0;
    rbl_decide(l, d, RBL_WHY_NONE, next_slot, rtt_ms);
    return 1;
}

int rbl_take_decision(rbl_t *l, int *d, uint32_t *slot)
{
    if (!l || !l->active || !l->dec_new) return 0;
    l->dec_new = 0;
    l->applied = l->seq;
    if (d) *d = l->dec_d;
    if (slot) *slot = l->dec_slot;
    return 1;
}
