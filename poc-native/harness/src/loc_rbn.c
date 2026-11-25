/* loc_rbn.c - W346 LOC-ROLLBACK-R4: Aushandlung der Eingabeschicht je Mission. Beschreibung in loc_rbn.h.
 * Reines Modul (keine Sockets, kein Log, kein Spielspeicher).
 *
 * Paket "SBRH" (little endian, Laenge exakt RBN_PKT_HEAD + nent * RBN_PKT_ENT):
 *   0  "SBRH"            4  ver = 1            5  Spielerindex (0..9, 0xFF unbekannt)
 *   6  Flags (b0 Wunsch, b1 in Mission, b2 Host-Entscheidung gueltig, b3 Auto gewuenscht (W447), Rest 0)  7  nent (0..8)
 *   8  Kennung (!= 0)    12 Uhrzeit ms         16 Entscheidung: Folge   20 Alter der Entscheidung ms
 *   24 Entscheidung: Modus (0/1 LS/2 R2)   25 Verzoegerung (0 oder 1..8)   26 angewandter Modus (0/1/2)
 *   27 Lockstep-Grund RBN_WHY_* (W370; nur bei Lockstep-Entscheidung des Hosts), bei R2-Entscheidung W447 Profil
 *      RBN_PROF_* (0 fest, 1 Auto-Direkt, 2 Auto-Rollback), sonst 0
 *   28 angewandte Folge  32 Alter der eigenen Mission ms (0 = keine)
 *   je Eintrag: Kennung des Mitspielers, Echo seiner Uhrzeit, Haltezeit ms (u16), Flags (b0 Wunsch gesehen),
 *   W494: eigene Umlaufzeit zu diesem Mitspieler (Mittel + 2 Schwankung) in 8-ms-Schritten aufgerundet (1..255,
 *   0 = noch nicht gemessen). Damit kennt jeder (v. a. der Host) die schlechteste Strecke im ganzen Netz, auch
 *   zwischen zwei anderen Mitspielern (Lockstep braucht jede Eingabe bei jedem). */
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
    int want_auto;            /* W447: Hallo b3 */
    u32 ping_t, ping_rx;      /* letzte Uhrzeit des Mitspielers + eigene Empfangszeit (fuer das Echo) */
    double srtt, rttvar;
    int rtt_ok;
    int dec_valid, dec_mode, dec_delay, dec_why; /* W370: dec_why = Lockstep-Grund des Hosts (Byte 27) */
    int dec_prof;             /* W447: Profil einer R2-Entscheidung (Byte 27) */
    int far_eff;              /* W494: groesste gemeldete Umlaufzeit des Mitspielers zu Dritten (ms, -1 = keine) */
    u32 dec_seq, dec_at;      /* Zeitpunkt der Entscheidung in eigener Uhr (Empfang - Alter) */
    int app_mode;
    u32 app_seq, mis_at;      /* Missionsbeginn des Mitspielers in eigener Uhr (0 = keine) */
    int confirmed;            /* Host: R2 fuer die laufende Mission bestaetigt */
    u32 pre_ack_seq, pre_ack_t; /* W475 Host: letzte gueltige Bestaetigung des Vorschlags (Folge, Empfangszeit) */
} rbn_peer;

struct rbn {
    int want, dmin, dmax, dfix;
    int want_auto, direct_rtt, direct_jit, direct_start1; /* W447 */
    int dec_prof, prof;       /* W447: Host-Entscheidung (Profil) bzw. Profil der laufenden Mission */
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
    /* W475 LOC-RB-PREAGREE */
    u32 dec_seq;              /* Host: Folge der Entscheidung der laufenden/letzten Mission (Hallo), getrennt von seq */
    int role;                 /* RBN_ROLE_* (Lobby) */
    u32 host_ip, ctx;         /* Client: Beitrittsziel; Host: Lobby-Kontext */
    /* Host: Vorschlag */
    u32 pre_seq, pre_digest, pre_ctx, pre_chg_t, pre_wd_seq, pre_wd_until;
    int pre_d, pre_prof, pre_wauto, pre_npart;
    u32 pre_part[RBN_PRE_MAXPART];
    uint64_t pre_psrc[RBN_PRE_MAXPART];
    int conn_n;               /* Host: Zahl der verbundenen Adressen fuer den Missionsbeginn (-1 = unbekannt) */
    u32 conn_ip[RBN_PRE_MAXPART + 2];
    /* Client: Bindung */
    int bnd, bnd_d, bnd_prof;
    u32 bnd_hn, bnd_seq, bnd_digest, bnd_rx_t, pre_seen, pre_seen_hn;
    uint64_t bnd_src;
    /* Mission */
    int pre_used, pre_proofed, pre_miss;
    u32 pre_c, pre_hn;
    uint64_t pre_src;
    rbn_pre_stats_t pst;
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
    n->direct_rtt = 80; /* W447 (Messreihe: D = 1 rechtzeitig bis ~80 ms gemessen) */
    n->direct_jit = 15;
    n->st_.rtt_min = 0xFFFFFFFFu;
    n->conn_n = -1;
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
    for (i = 0; i < RBN_MAXPEER; i++) { /* W494: Strecken zwischen anderen Mitspielern (von ihnen gemeldet) */
        const rbn_peer *P = &n->p[i];
        if (fresh(P, now) && P->far_eff > mx) mx = P->far_eff;
    }
    return mx;
}

int rbn_tx(rbn_t *n, uint32_t now, int self, uint8_t *b, int cap)
{
    int i, ne = 0, fl = 0;
    /* W475: ein gebundener Client (bzw. in einer Mission aus der Vorab-Einigung) sendet auch mit Wunsch "aus" weiter
     * (Flags b0 = 0), damit der Host die Aenderung sieht und den Vorschlag zuruecknimmt */
    if (!n || (!n->want && !n->bnd && n->st != RBN_ST_PRE && !(n->st == RBN_ST_R2 && n->pre_used)) || cap < RBN_PKT_MAX)
        return 0;
    memset(b, 0, RBN_PKT_MAX);
    memcpy(b, "SBRH", 4);
    b[4] = 1;
    b[5] = (uint8_t)((self >= 0 && self < 10) ? self : 0xFF);
    fl = n->want ? 1 : 0;
    if (n->st != RBN_ST_IDLE) fl |= 2;
    if (n->self == 0 && n->dec_mode != RBN_MODE_NONE && n->dec_seq) fl |= 4;
    if (n->want_auto) fl |= 8; /* W447 */
    b[6] = (uint8_t)fl;
    wr32(b + 8, n->nonce);
    wr32(b + 12, now);
    if (fl & 4) {
        u32 age = now - n->dec_at;
        wr32(b + 16, n->dec_seq); /* W475: Folge der Entscheidung (seq zaehlt auch Vorschlaege) */
        wr32(b + 20, age > AGE_MAX ? AGE_MAX : age);
        b[24] = (uint8_t)n->dec_mode;
        b[25] = (uint8_t)(n->dec_mode == RBN_MODE_R2 ? n->dec_delay : 0);
        if (n->dec_mode == RBN_MODE_LS && n->dec_why > 0 && n->dec_why < RBN_WHY_N) b[27] = (uint8_t)n->dec_why;
        if (n->dec_mode == RBN_MODE_R2 && n->dec_prof > 0 && n->dec_prof <= RBN_PROF_ROLLBACK) b[27] = (uint8_t)n->dec_prof; /* W447 */
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
        if (P->rtt_ok) { /* W494: eigene Umlaufzeit zu diesem Mitspieler, 8-ms-Schritte aufgerundet, 1..255 */
            double eff = P->srtt + 2.0 * P->rttvar;
            int q8 = (int)((eff + 7.999) / 8.0);
            e[11] = (uint8_t)(q8 < 1 ? 1 : (q8 > 255 ? 255 : q8));
        } else {
            e[11] = 0;
        }
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
    if ((pidx >= 10 && pidx != 0xFF) || (fl & ~15) || ne > RBN_PKT_MAXENT || len != RBN_PKT_HEAD + ne * RBN_PKT_ENT) goto bad;
    nonce = rd32(b + 8);
    dage = rd32(b + 20);
    mage = rd32(b + 32);
    dm = b[24];
    dd = b[25];
    am = b[26];
    if (!nonce || dm > 2 || dd > 8 || am > 2 || dage > AGE_MAX || mage > AGE_MAX) goto bad;
    /* W370: Byte 27 = Lockstep-Grund, nur bei einer Lockstep-Entscheidung des Hosts; W447: bei einer R2-Entscheidung
     * das Profil (1..2); sonst 0 */
    if (b[27] && (!(fl & 4) || (dm == RBN_MODE_LS && b[27] >= RBN_WHY_N) || (dm == RBN_MODE_R2 && b[27] > RBN_PROF_ROLLBACK) ||
                  (dm != RBN_MODE_LS && dm != RBN_MODE_R2)))
        goto bad;
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
        if ((e[10] & ~1) || rd16(e + 8) > HOLD_MAX || !rd32(e)) goto bad; /* W494: Byte 11 = RTT zum Eintrag */
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
    P->want_auto = (fl & 8) ? 1 : 0; /* W447 */
    P->pidx = pidx == 0xFF ? -1 : pidx;
    P->in_mission = (fl & 2) ? 1 : 0;
    P->ping_t = rd32(b + 12);
    P->ping_rx = now;
    P->ack_me = 0;
    P->far_eff = -1;
    for (i = 0; i < ne; i++) {
        const uint8_t *e = b + RBN_PKT_HEAD + i * RBN_PKT_ENT;
        if (rd32(e) != n->nonce) {
            if (e[11] && 8 * (int)e[11] > P->far_eff) P->far_eff = 8 * (int)e[11]; /* W494: Strecke zu einem Dritten */
            continue;
        }
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
        P->dec_why = dm == RBN_MODE_LS ? b[27] : 0;
        P->dec_prof = dm == RBN_MODE_R2 ? b[27] : 0; /* W447 */
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
    n->dec_prof = 0; /* W447 */
    n->prof = 0;
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
    n->seq++; /* W475: liegt immer ueber jedem Vorschlag (Vorschlaege zaehlen seq mit) -> andere Epoche als c */
    n->dec_seq = n->seq;
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
    /* W447: Profil - Auto nur, wenn Host und alle Mitspieler Auto wollen; dann nach Umlaufzeit/Schwankung */
    n->dec_prof = rbn_auto_agreed(n, now) ? rbn_pick_profile(rbn_rtt_max(n, now), rbn_rttvar_max(n, now), n->direct_rtt,
                                                            n->direct_jit)
                                          : RBN_PROF_FIXED;
    n->prof = n->dec_prof;
    if (n->dec_prof == RBN_PROF_DIRECT && n->direct_start1 > 0 && !n->dfix && rbn_rtt_max(n, now) >= 0 &&
        rbn_rtt_max(n, now) <= n->direct_start1) { /* W447: DIREKT mit kleiner RTT beginnt mit 1 Tick */
        n->dec_delay = 1;
        n->delay = 1;
    }
    n->st_.decisions_r2++;
    n->st = RBN_ST_TENT;
    n->why = RBN_WHY_NONE;
    n->app_mode = RBN_MODE_R2; /* angeboten */
    return n->st;
}

/* ---- W475 LOC-RB-PREAGREE (Beschreibung loc_rbn.h) ---- */
static void pre_unbind(rbn_t *n)
{
    if (n->bnd) n->pst.unbind++;
    n->bnd = 0;
}

static void pre_withdraw(rbn_t *n, u32 now)
{
    if (!n->pre_seq) return;
    n->pre_wd_seq = n->pre_seq;
    n->pre_wd_until = (now + 2000u) | 1u; /* Ruecknahme 2 s lang mitsenden (Verlust) */
    n->pre_seq = 0;
    n->pre_chg_t = 0;
}

/* Missionsbeginn ohne Nutzung: Vorschlag/Bindung sind verbraucht (gelten nur fuer genau diesen Start). */
static void pre_consume(rbn_t *n)
{
    if (n->pre_seq) n->pst.miss_host++;
    n->pre_seq = 0;
    n->pre_chg_t = 0;
    n->pre_wd_until = 0;
    if (n->bnd) {
        n->bnd = 0; /* verbraucht, nicht "geloest" */
    }
}

static rbn_peer *peer_by_src(rbn_t *n, uint64_t src)
{
    int i;
    for (i = 0; i < RBN_MAXPEER; i++)
        if (n->p[i].used && n->p[i].src == src) return &n->p[i];
    return NULL;
}

static int pre_acked(const rbn_t *n, u32 now, int k)
{
    int i;
    for (i = 0; i < RBN_MAXPEER; i++) {
        const rbn_peer *P = &n->p[i];
        if (!P->used || P->src != n->pre_psrc[k] || P->nonce != n->pre_part[k]) continue;
        return fresh(P, now) && P->want && P->pre_ack_seq == n->pre_seq && (u32)(now - P->pre_ack_t) <= RBN_PRE_ACKAGE_MS;
    }
    return 0;
}

int rbn_pre_ready(const rbn_t *n, uint32_t now)
{
    int k;
    if (!n || !n->pre_seq || n->pre_npart < 1) return 0;
    for (k = 0; k < n->pre_npart; k++)
        if (!pre_acked(n, now, k)) return 0;
    return 1;
}

/* Host, Missionsbeginn: Fall a (alle gebunden und bestaetigt, nichts geaendert) -> sofort R2 mit c. */
static int pre_host_try(rbn_t *n, u32 now)
{
    int k, j;
    n->pre_miss = !n->pre_seq ? 1 : n->pre_seq != n->seq ? 2 : (!n->want || n->want_auto != n->pre_wauto) ? 3
                : n->role != RBN_ROLE_HOST ? 4 : n->npeers != n->pre_npart ? 5 : !rbn_pre_ready(n, now) ? 6 : 0;
    if (n->pre_miss) return 0;
    if (n->conn_n >= 0) { /* gleiche Adressen wie die Teilnehmer (Mehrfachmengen-Vergleich) */
        int used[RBN_PRE_MAXPART] = {0};
        n->pre_miss = 7;
        if (n->conn_n != n->pre_npart) return 0;
        for (j = 0; j < n->conn_n; j++) {
            for (k = 0; k < n->pre_npart; k++)
                if (!used[k] && (u32)(n->pre_psrc[k] >> 16) == n->conn_ip[j]) break;
            if (k >= n->pre_npart) return 0;
            used[k] = 1;
        }
        n->pre_miss = 0;
    }
    n->dec_seq = n->seq;
    n->dec_at = now;
    n->app_seq = n->seq;
    n->epoch = (n->nonce & 0xFFFF0000u) ^ (n->seq & 0xFFFFu);
    n->hwait = 0;
    n->dec_mode = RBN_MODE_R2;
    n->dec_why = 0;
    n->dec_delay = n->delay = n->pre_d;
    n->dec_prof = n->prof = n->pre_prof;
    for (k = 0; k < n->pre_npart; k++) {
        rbn_peer *P = peer_by_src(n, n->pre_psrc[k]);
        if (P) P->confirmed = 1;
    }
    n->st_.decisions_r2++;
    n->pst.used_host++;
    n->pre_used = 1;
    n->pre_c = n->seq;
    n->pre_hn = n->nonce;
    n->pre_seq = 0; /* verbraucht */
    n->pre_wd_until = 0;
    set_r2(n);
    return 1;
}

/* Client, Missionsbeginn: gebunden -> PRE (Epoche/D von c, Entnahme erst nach dem Beweis). */
static int pre_client_try(rbn_t *n, u32 now)
{
    rbn_peer *H;
    if (!n->bnd || n->role != RBN_ROLE_JOIN) return 0;
    H = peer_by_src(n, n->bnd_src);
    if (!H || H->nonce != n->bnd_hn) return 0;
    if ((u32)(now - n->bnd_rx_t) > RBN_PRE_HOLD_MS && !H->in_mission) return 0;
    n->pre_c = n->bnd_seq;
    n->pre_hn = n->bnd_hn;
    n->pre_src = n->bnd_src;
    n->pre_proofed = 0;
    n->epoch = (n->bnd_hn & 0xFFFF0000u) ^ (n->bnd_seq & 0xFFFFu);
    n->delay = n->bnd_d;
    n->prof = n->bnd_prof;
    n->bnd = 0; /* verbraucht */
    n->st = RBN_ST_PRE;
    n->why = RBN_WHY_NONE;
    n->app_mode = RBN_MODE_NONE;
    return 1;
}

static void pre_to_r2(rbn_t *n)
{
    n->last_applied = n->pre_c;
    n->app_seq = n->pre_c;
    n->pre_used = 1;
    n->pst.used_client++;
    set_r2(n);
}

static int pre_client_poll(rbn_t *n, u32 now)
{
    rbn_peer *H = peer_by_src(n, n->pre_src);
    if (n->pre_proofed) {
        n->pst.pre_r2_frame++;
        pre_to_r2(n);
        return n->st;
    }
    if (H && H->nonce == n->pre_hn && H->dec_valid && H->dec_seq >= n->pre_c && H->dec_seq > n->last_applied &&
        same_mission(n, H->dec_at | 1u)) {
        if (H->dec_seq == n->pre_c && H->dec_mode == RBN_MODE_R2 && H->dec_delay == n->delay &&
            H->dec_prof == n->prof) {
            n->pst.pre_r2_hello++;
            pre_to_r2(n);
            return n->st;
        }
        /* Host hat anders entschieden (bisheriger Weg): folgen wie PEND - noch nichts entnommen */
        n->pst.pre_diverge++;
        n->last_applied = H->dec_seq;
        n->app_seq = H->dec_seq;
        if (H->dec_mode == RBN_MODE_R2) {
            n->epoch = (H->nonce & 0xFFFF0000u) ^ (H->dec_seq & 0xFFFFu);
            n->delay = H->dec_delay;
            n->prof = H->dec_prof;
            set_r2(n);
        } else {
            n->pst.pre_ls++;
            n->host_why = H->dec_why;
            set_ls(n, RBN_WHY_HOST_LS);
        }
        return n->st;
    }
    if ((u32)(now - n->mis_start) > RBN_WAIT_MS) {
        n->pst.pre_timeout++;
        set_ls(n, RBN_WHY_TIMEOUT);
    }
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
    n->prof = 0; /* W447 */
    n->pre_used = n->pre_proofed = 0; /* W475 */
    n->pre_c = n->pre_hn = 0;
    n->pre_src = 0;
    for (i = 0; i < RBN_MAXPEER; i++) n->p[i].confirmed = 0;
    if (self == 0 && pre_host_try(n, now)) return n->st;  /* W475: Vorab-Einigung, sofort R2 */
    if (self > 0 && self < 10 && pre_client_try(n, now)) return n->st; /* W475: gebunden -> PRE (vor dem Wunsch!) */
    pre_consume(n);
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
    if (n->st == RBN_ST_PRE) return pre_client_poll(n, now); /* W475 */
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
            if (P->app_mode == RBN_MODE_R2 && P->app_seq == n->dec_seq) P->confirmed = 1;
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
                n->prof = H->dec_prof; /* W447 */
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

/* ---- W447 LOC-NET-AUTO (Beschreibung in loc_rbn.h) ---- */
void rbn_set_want(rbn_t *n, int want, int want_auto)
{
    if (!n) return;
    n->want = want ? 1 : 0;
    n->want_auto = (want && want_auto) ? 1 : 0;
}
int rbn_want(const rbn_t *n) { return n ? n->want : 0; }
int rbn_want_auto(const rbn_t *n) { return n ? n->want_auto : 0; }

int rbn_pick_profile(int rtt_eff, int rttvar, int direct_rtt, int direct_jit)
{
    if (rtt_eff < 0 || rttvar < 0) return RBN_PROF_ROLLBACK;
    return (rtt_eff <= direct_rtt && rttvar <= direct_jit) ? RBN_PROF_DIRECT : RBN_PROF_ROLLBACK;
}

int rbn_rttvar_max(const rbn_t *n, uint32_t now)
{
    int i, mx = -1;
    if (!n) return -1;
    for (i = 0; i < RBN_MAXPEER; i++) {
        const rbn_peer *P = &n->p[i];
        int v;
        if (!fresh(P, now) || !P->rtt_ok) continue;
        v = (int)(P->rttvar + 0.5);
        if (v > mx) mx = v;
    }
    return mx;
}

void rbn_set_direct(rbn_t *n, int direct_rtt, int direct_jit)
{
    if (!n) return;
    if (direct_rtt >= 0 && direct_rtt <= 1000) n->direct_rtt = direct_rtt;
    if (direct_jit >= 0 && direct_jit <= 500) n->direct_jit = direct_jit;
}

int rbn_profile(const rbn_t *n) { return (n && n->st == RBN_ST_R2) ? n->prof : 0; }
void rbn_set_direct_start(rbn_t *n, int start1)
{
    if (n) n->direct_start1 = (start1 > 0 && start1 <= 1000) ? start1 : 0;
}

int rbn_auto_agreed(const rbn_t *n, uint32_t now)
{
    int i, k = 0;
    if (!n || !n->want || !n->want_auto) return 0;
    for (i = 0; i < RBN_MAXPEER; i++) {
        const rbn_peer *P = &n->p[i];
        if (!fresh(P, now) || !P->want) continue;
        if (!P->want_auto) return 0;
        k++;
    }
    return k > 0;
}

int rbn_peer_list(const rbn_t *n, uint32_t now, int *pidx, int *rtt, int *wauto, int cap)
{
    int i, k = 0;
    if (!n) return 0;
    for (i = 0; i < RBN_MAXPEER && k < cap; i++) {
        const rbn_peer *P = &n->p[i];
        if (!fresh(P, now)) continue;
        if (pidx) pidx[k] = P->pidx;
        if (rtt) rtt[k] = P->rtt_ok ? (int)(P->srtt + 0.5) : -1;
        if (wauto) wauto[k] = P->want ? (P->want_auto ? 2 : 1) : 0;
        k++;
    }
    return k;
}
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
    int startlow;             /* W447: Mission beginnt niedrig (Profil DIREKT): Ring fehlt anfangs, das ist kein Grund zurueck */
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
            if (!r->ready) { if (!l->startlow) { sev = 1; why = RBL_WHY_NOTREADY; } } /* W447: Start niedrig */
            else if ((int)r->rp > l->c.hrp) { sev = 1; why = RBL_WHY_REPLAY; }
            else if ((int)r->hang >= 2 * l->c.hhang) { sev = 1; why = RBL_WHY_HANG; }
            else if ((int)r->dep >= l->c.hdep + 2) { sev = 1; why = RBL_WHY_DEPTH; }
            else if ((int)r->cpm > l->c.hcpm) { bad = 1; if (!why) why = RBL_WHY_CPM; }
            else if ((int)r->dep >= l->c.hdep) { bad = 1; if (!why) why = RBL_WHY_DEPTH; }
            else if ((int)r->hang >= l->c.hhang) { bad = 1; if (!why) why = RBL_WHY_HANG; }
            else if ((int)r->jump > l->c.hjump) { bad = 1; if (!why) why = RBL_WHY_JUMP; }
        }
        if (!all && !sev) { bad = 1; if (!why) why = RBL_WHY_NOTREADY; } /* Mitspieler meldet sich nicht mehr */
        if (l->startlow && all) { /* W447: Startphase vorbei, sobald alle Seiten den Ring melden */
            int rd = 1;
            for (k = 0; k < n; k++) rd &= R[k]->ready ? 1 : 0;
            if (rd) l->startlow = 0;
        }
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

void rbl_start_low(rbl_t *l)
{
    if (!l || !l->active || l->d_low >= l->d_std) return;
    l->target = l->d_low;
    l->startlow = 1;
    l->good = l->bad = 0;
}
int rbl_startlow(const rbl_t *l) { return l ? l->startlow : 0; }

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

/* ================= W475 LOC-RB-PREAGREE: Paket "SBRP" (Vorab-Einigung in der Lobby) =================
 * little endian, Laenge exakt RBN_PRE_HEAD + 4 * npart:
 *   0 "SBRP"   4 ver = 1   5 kind (1 Vorschlag, 2 Bestaetigung, 3 Ruecknahme)   6 Flags (kind 1: b0 alle bestaetigt)
 *   7 npart (kind 1: 1..8, sonst 0)   8 Kennung des Absenders (!= 0)   12 Kennung des Hosts (kind 1/3 = Absender)
 *   16 Folge c (!= 0)   20 Pruefsumme (kind 1: rbn_pre_digest des Vorschlags, kind 2: bestaetigte, kind 3: 0)
 *   24 Modus (kind 1: 2 = R2, sonst 0)   25 Verzoegerung (kind 1: 1..8)   26 Profil (kind 1: 0..2)   27 0
 *   28 Lobby-Kontext (kind 1, beliebig; sonst 0)   32 kind 1: Teilnehmer-Kennungen (je != 0, streng aufsteigend) */
static u32 fnv_u32(u32 h, u32 v)
{
    int k;
    for (k = 0; k < 4; k++) {
        h ^= (v >> (8 * k)) & 0xFFu;
        h *= 16777619u;
    }
    return h;
}

uint32_t rbn_pre_digest(uint32_t hnonce, uint32_t seq, int mode, int d, int prof, uint32_t ctx, const uint32_t *part, int npart)
{
    u32 h = 2166136261u;
    int k;
    h = fnv_u32(h, 0x50524253u); /* "SBRP" */
    h = fnv_u32(h, hnonce);
    h = fnv_u32(h, seq);
    h = fnv_u32(h, (u32)mode | ((u32)d << 8) | ((u32)prof << 16));
    h = fnv_u32(h, ctx);
    h = fnv_u32(h, (u32)npart);
    for (k = 0; k < npart; k++) h = fnv_u32(h, part[k]);
    return h;
}

int rbn_is_pre(const uint8_t *b, int len) { return b && len >= 4 && memcmp(b, "SBRP", 4) == 0; }

int rbn_pre_check(const uint8_t *b, int len)
{
    int kind, np, k;
    u32 snd, hn, seq;
    if (!b || len < RBN_PRE_HEAD || len > RBN_PRE_MAX || memcmp(b, "SBRP", 4) != 0 || b[4] != 1 || b[27]) return 0;
    kind = b[5];
    np = b[7];
    snd = rd32(b + 8);
    hn = rd32(b + 12);
    seq = rd32(b + 16);
    if (!snd || !hn || !seq) return 0;
    if (kind == RBN_PRE_PROP) {
        u32 part[RBN_PRE_MAXPART];
        if ((b[6] & ~1) || np < 1 || np > RBN_PRE_MAXPART || len != RBN_PRE_HEAD + 4 * np || hn != snd) return 0;
        if (b[24] != RBN_MODE_R2 || b[25] < 1 || b[25] > 8 || b[26] > RBN_PROF_ROLLBACK) return 0;
        for (k = 0; k < np; k++) {
            part[k] = rd32(b + RBN_PRE_HEAD + 4 * k);
            if (!part[k] || part[k] == hn || (k && part[k] <= part[k - 1])) return 0;
        }
        return rd32(b + 20) == rbn_pre_digest(hn, seq, b[24], b[25], b[26], rd32(b + 28), part, np);
    }
    if (kind != RBN_PRE_ACK && kind != RBN_PRE_WD) return 0;
    if (b[6] || np || len != RBN_PRE_HEAD || b[24] || b[25] || b[26] || rd32(b + 28)) return 0;
    if (kind == RBN_PRE_ACK) return hn != snd;
    return hn == snd && rd32(b + 20) == 0;
}

void rbn_set_lobby(rbn_t *n, int role, uint32_t host_ip, uint32_t ctx)
{
    if (!n) return;
    if (role < RBN_ROLE_NONE || role > RBN_ROLE_JOIN) role = RBN_ROLE_NONE;
    if (n->bnd && (role != RBN_ROLE_JOIN || host_ip != n->host_ip)) pre_unbind(n);
    n->role = role;
    n->host_ip = role == RBN_ROLE_JOIN ? host_ip : 0;
    n->ctx = role == RBN_ROLE_HOST ? ctx : 0;
}

void rbn_pre_set_conn(rbn_t *n, int nip, const uint32_t *ips)
{
    int k;
    if (!n) return;
    if (nip < 0 || !ips || nip > RBN_PRE_MAXPART + 2) {
        n->conn_n = -1;
        return;
    }
    n->conn_n = nip;
    for (k = 0; k < nip; k++) n->conn_ip[k] = ips[k];
}

static void sort_part(u32 *v, uint64_t *s, int np)
{
    int a, b;
    for (a = 1; a < np; a++)
        for (b = a; b > 0 && v[b - 1] > v[b]; b--) {
            u32 t = v[b];
            uint64_t ts = s[b];
            v[b] = v[b - 1];
            v[b - 1] = t;
            s[b] = s[b - 1];
            s[b - 1] = ts;
        }
}

void rbn_pre_tick(rbn_t *n, uint32_t now)
{
    u32 part[RBN_MAXPEER];
    uint64_t psrc[RBN_MAXPEER];
    int i, np = 0, nowant = 0, cd, cprof, rtt, same;
    if (!n) return;
    if (n->bnd && (u32)(now - n->bnd_rx_t) > RBN_PRE_HOLD_MS) { /* Client: Bindung ohne Auffrischung abgelaufen */
        rbn_peer *H = peer_by_src(n, n->bnd_src);
        if (!H || !H->in_mission || !fresh(H, now)) pre_unbind(n); /* Host schon in der Mission: gilt fuer diesen Start */
    }
    if (n->role != RBN_ROLE_HOST || !n->want || n->st != RBN_ST_IDLE) {
        if (n->st == RBN_ST_IDLE) pre_withdraw(n, now);
        return;
    }
    for (i = 0; i < RBN_MAXPEER; i++) {
        rbn_peer *P = &n->p[i];
        if (!fresh(P, now)) continue;
        if (!P->want || !P->ack_me) {
            nowant++;
            continue;
        }
        if (np < RBN_MAXPEER) {
            part[np] = P->nonce;
            psrc[np] = P->src;
            np++;
        }
    }
    if (nowant || np < 1 || np > RBN_PRE_MAXPART) { /* nicht alle koennen/wollen: kein Vorschlag (Missionsbeginn wie bisher) */
        pre_withdraw(n, now);
        return;
    }
    sort_part(part, psrc, np);
    rtt = rbn_rtt_max(n, now);
    cd = n->dfix ? n->dfix : rbn_pick_delay(rtt, n->dmin, n->dmax);
    cprof = rbn_auto_agreed(n, now) ? rbn_pick_profile(rtt, rbn_rttvar_max(n, now), n->direct_rtt, n->direct_jit)
                                    : RBN_PROF_FIXED;
    if (cprof == RBN_PROF_DIRECT && n->direct_start1 > 0 && !n->dfix && rtt >= 0 && rtt <= n->direct_start1) cd = 1;
    same = n->pre_seq && np == n->pre_npart && n->ctx == n->pre_ctx && n->want_auto == n->pre_wauto;
    for (i = 0; same && i < np; i++)
        if (part[i] != n->pre_part[i] || psrc[i] != n->pre_psrc[i]) same = 0;
    if (same) {
        if (cd == n->pre_d && cprof == n->pre_prof) {
            n->pre_chg_t = 0;
            return;
        }
        if (!n->pre_chg_t) {
            n->pre_chg_t = now | 1u;
            return;
        }
        if ((u32)(now - n->pre_chg_t) < RBN_PRE_HYST_MS) return;
    }
    /* neuer Vorschlag (neue Folge; der alte ist damit fuer jeden Client zurueckgenommen) */
    n->seq++;
    n->pre_seq = n->seq;
    n->pre_d = cd;
    n->pre_prof = cprof;
    n->pre_ctx = n->ctx;
    n->pre_wauto = n->want_auto;
    n->pre_npart = np;
    n->pre_chg_t = 0;
    n->pre_wd_until = 0;
    for (i = 0; i < np; i++) {
        n->pre_part[i] = part[i];
        n->pre_psrc[i] = psrc[i];
    }
    n->pre_digest = rbn_pre_digest(n->nonce, n->pre_seq, RBN_MODE_R2, cd, cprof, n->pre_ctx, part, np);
    n->pst.prop++;
}

int rbn_pre_tx(rbn_t *n, uint32_t now, uint8_t *b, int cap)
{
    int k;
    if (!n || !b || cap < RBN_PRE_MAX) return 0;
    memset(b, 0, RBN_PRE_HEAD);
    memcpy(b, "SBRP", 4);
    b[4] = 1;
    wr32(b + 8, n->nonce);
    if (n->role == RBN_ROLE_HOST && n->pre_seq && n->st == RBN_ST_IDLE) {
        b[5] = RBN_PRE_PROP;
        b[6] = (uint8_t)(rbn_pre_ready(n, now) ? 1 : 0);
        b[7] = (uint8_t)n->pre_npart;
        wr32(b + 12, n->nonce);
        wr32(b + 16, n->pre_seq);
        wr32(b + 20, n->pre_digest);
        b[24] = RBN_MODE_R2;
        b[25] = (uint8_t)n->pre_d;
        b[26] = (uint8_t)n->pre_prof;
        wr32(b + 28, n->pre_ctx);
        for (k = 0; k < n->pre_npart; k++) wr32(b + RBN_PRE_HEAD + 4 * k, n->pre_part[k]);
        n->pst.prop_tx++;
        return RBN_PRE_HEAD + 4 * n->pre_npart;
    }
    if (n->role == RBN_ROLE_HOST && n->pre_wd_until && (int32_t)(n->pre_wd_until - now) > 0 && n->pre_wd_seq) {
        b[5] = RBN_PRE_WD;
        wr32(b + 12, n->nonce);
        wr32(b + 16, n->pre_wd_seq);
        n->pst.wd_tx++;
        return RBN_PRE_HEAD;
    }
    if (n->bnd && n->st == RBN_ST_IDLE && (u32)(now - n->bnd_rx_t) <= RBN_PRE_ACKWIN_MS) {
        b[5] = RBN_PRE_ACK;
        wr32(b + 12, n->bnd_hn);
        wr32(b + 16, n->bnd_seq);
        wr32(b + 20, n->bnd_digest);
        n->pst.ack_tx++;
        return RBN_PRE_HEAD;
    }
    return 0;
}

int rbn_pre_rx(rbn_t *n, uint32_t now, const uint8_t *b, int len, uint64_t src)
{
    int kind, k, np, mine = 0;
    u32 snd, hn, seq, dg;
    rbn_peer *P;
    if (!n) return 0;
    if (!rbn_pre_check(b, len)) {
        n->pst.rx_bad++;
        return -1;
    }
    kind = b[5];
    np = b[7];
    snd = rd32(b + 8);
    hn = rd32(b + 12);
    seq = rd32(b + 16);
    dg = rd32(b + 20);
    if (snd == n->nonce) return 0; /* eigenes Paket */
    /* Bindung an das Hallo: derselbe Absender muss mit derselben Kennung frisch bekannt sein */
    P = peer_by_src(n, src);
    if (!P || P->nonce != snd || !fresh(P, now)) {
        n->pst.rx_ign++;
        return 0;
    }
    if (kind == RBN_PRE_ACK) { /* Host: Bestaetigung genau des laufenden Vorschlags von einem Teilnehmer */
        if (n->role != RBN_ROLE_HOST || !n->pre_seq || hn != n->nonce || seq != n->pre_seq || dg != n->pre_digest ||
            n->st != RBN_ST_IDLE) {
            n->pst.rx_ign++;
            return 0;
        }
        for (k = 0; k < n->pre_npart; k++)
            if (n->pre_part[k] == snd && n->pre_psrc[k] == src) break;
        if (k >= n->pre_npart) {
            n->pst.rx_ign++;
            return 0;
        }
        P->pre_ack_seq = seq;
        P->pre_ack_t = now;
        n->pst.ack_rx++;
        return 1;
    }
    /* Vorschlag/Ruecknahme: nur vom Beitrittsziel (Client) */
    if (n->role != RBN_ROLE_JOIN || !n->host_ip || (u32)(src >> 16) != n->host_ip) {
        n->pst.rx_ign++;
        return 0;
    }
    if (n->pre_seen_hn != hn) { /* anderer Host-Prozess: Folgen neu */
        n->pre_seen_hn = hn;
        n->pre_seen = 0;
    }
    if (kind == RBN_PRE_WD) {
        if (seq > n->pre_seen) n->pre_seen = seq;
        if (n->bnd && n->bnd_hn == hn && seq >= n->bnd_seq) pre_unbind(n);
        n->pst.wd_rx++;
        return 1;
    }
    if (n->st != RBN_ST_IDLE || seq <= n->last_applied) { /* nur in der Lobby, nie fuer eine schon entschiedene Folge */
        n->pst.rx_ign++;
        return 0;
    }
    if (n->bnd && n->bnd_hn == hn && seq == n->bnd_seq) { /* Auffrischung desselben Vorschlags */
        if (dg != n->bnd_digest) {
            n->pst.rx_bad++;
            return -1;
        }
        n->bnd_rx_t = now;
        return 1;
    }
    if (seq <= n->pre_seen) { /* aelter oder Wiederholung einer zurueckgenommenen Folge */
        n->pst.rx_ign++;
        return 0;
    }
    n->pre_seen = seq;
    for (k = 0; k < np; k++)
        if (rd32(b + RBN_PRE_HEAD + 4 * k) == n->nonce) mine = 1;
    if (!mine || !n->want) { /* nicht Teilnehmer bzw. kein Rollback gewuenscht: nicht binden (alte Bindung ist ueberholt) */
        pre_unbind(n);
        n->pst.rx_ign++;
        return 0;
    }
    n->bnd = 1;
    n->bnd_hn = hn;
    n->bnd_seq = seq;
    n->bnd_digest = dg;
    n->bnd_d = b[25];
    n->bnd_prof = b[26];
    n->bnd_src = src;
    n->bnd_rx_t = now;
    n->pst.bind++;
    return 1;
}

void rbn_pre_proof(rbn_t *n, uint64_t src)
{
    if (n && n->st == RBN_ST_PRE && src == n->pre_src) n->pre_proofed = 1;
}

int rbn_pre_bound(const rbn_t *n, uint32_t *seq)
{
    if (!n || !n->bnd) return 0;
    if (seq) *seq = n->bnd_seq;
    return 1;
}

int rbn_pre_used(const rbn_t *n) { return n ? n->pre_used : 0; }
uint64_t rbn_pre_host_src(const rbn_t *n) { return n ? (n->st == RBN_ST_PRE ? n->pre_src : (n->bnd ? n->bnd_src : 0)) : 0; }
const rbn_pre_stats_t *rbn_pre_stats(const rbn_t *n) { return &n->pst; }
int rbn_pre_miss(const rbn_t *n) { return n ? n->pre_miss : 0; }

/* W475 TEST (loc_rb_opts pretest=1): Verzoegerungswechsel genau beim Missionsbeginn - neuer Vorschlag mit anderer D, noch
 * ohne Bestaetigung (Rueckfallweg im echten Spiel pruefen). */
void rbn_pre_test_bump(rbn_t *n)
{
    if (!n || !n->pre_seq) return;
    n->seq++;
    n->pre_seq = n->seq;
    n->pre_d = n->pre_d == 2 ? 3 : 2;
    n->pre_digest = rbn_pre_digest(n->nonce, n->pre_seq, RBN_MODE_R2, n->pre_d, n->pre_prof, n->pre_ctx, n->pre_part, n->pre_npart);
}
