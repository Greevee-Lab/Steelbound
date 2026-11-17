/* loc_netsim.c - W322 LOC-NETLAT: Verzoegerungs-/Jitter-Stufe im LoC-Netz-HLE (siehe loc_netsim.h). */
#include "loc_netsim.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NS_MSS 1460
#define NS_HDR 40
#define NS_QCAP (256 * 1024)
#define NS_MAXSOCK 64
#define NS_RTO_MIN 200.0
#define NS_HIST 512 /* 10-ms-Klassen bis 5,12 s */

typedef struct ns_ent {
    struct ns_ent *next;
    SOCKET s;
    int tcp, kind, flags, len, has_to;
    double t_in, due;
    struct sockaddr_in to;
    char data[1];
} ns_ent;

typedef struct ns_sock {
    SOCKET s;
    int used, err, pending;
    double last_due, seg_depart, ack_at;
} ns_sock;

struct locnetsim {
    locnetsim_cfg c;
    locnetsim_stats st;
    LONG hist[NS_HIST];
    locnetsim_log_fn log;
    CRITICAL_SECTION cs;
    HANDLE ev, timer, th;
    volatile LONG stop;
    ns_ent *head, *tail;
    ns_sock socks[NS_MAXSOCK];
    double busy_until;
    int ge_bad;
    unsigned long long rng;
    double qpf_ms;
    volatile LONG start_tick;
};

/* ---------- Profile (Spiegel von poc-native/net/netsim/profiles.json, W247) ---------- */
typedef struct ns_prof {
    const char *name;
    double rtt, jitter, loss, p_gb, p_bg, loss_bad, bw;
    int reorder;
} ns_prof;

static const ns_prof k_prof[] = {
    {"lan", 1, 0.1, 0.0, 0, 0, 0, 0, 0},
    {"eu-eu", 30, 2, 0.1, 0, 0, 0, 0, 0},
    {"eu-useast", 90, 3, 0.2, 0, 0, 0, 0, 0},
    {"useast-uswest", 65, 3, 0.1, 0, 0, 0, 0, 0},
    {"eu-uswest", 150, 5, 0.3, 0, 0, 0, 0, 0},
    {"uswest-jp", 120, 5, 0.3, 0, 0, 0, 0, 0},
    {"useast-jp", 170, 6, 0.3, 0, 0, 0, 0, 0},
    {"eu-jp", 250, 8, 0.5, 0, 0, 0, 0, 0},
    {"eu-jp-bad", 250, 30, 0.0, 0.007692, 0.3, 0.8, 0, 1},
    {"mobile-bad", 80, 40, 0.0, 0.016667, 0.25, 0.8, 1000, 1}, /* bw = up (1000 kbit/s) */
};
static const struct { const char *alias, *name; } k_alias[] = {
    {"dsl", "eu-eu"}, {"dsl-fern", "eu-useast"}, {"uebersee", "eu-jp"}, {"wlan", "mobile-bad"},
};

int locnetsim_profile_count(void) { return (int)(sizeof k_prof / sizeof k_prof[0]); }
const char *locnetsim_profile_name(int i)
{
    return (i >= 0 && i < locnetsim_profile_count()) ? k_prof[i].name : NULL;
}

int locnetsim_profile(const char *name, locnetsim_cfg *out)
{
    int i;
    const char *real = name;
    for (i = 0; i < (int)(sizeof k_alias / sizeof k_alias[0]); i++) {
        if (_stricmp(name, k_alias[i].alias) == 0) {
            real = k_alias[i].name;
        }
    }
    for (i = 0; i < locnetsim_profile_count(); i++) {
        const ns_prof *p = &k_prof[i];
        if (_stricmp(real, p->name) == 0) {
            strncpy_s(out->name, sizeof out->name, name, _TRUNCATE);
            out->rtt_ms = p->rtt;
            out->jitter_ms = p->jitter;
            out->loss_pct = p->loss;
            out->ge_p_gb = p->p_gb;
            out->ge_p_bg = p->p_bg;
            out->ge_loss_bad = p->loss_bad;
            out->bw_kbps = p->bw;
            out->reorder = p->reorder;
            return 1;
        }
    }
    return 0;
}

int locnetsim_parse(const char *spec, locnetsim_cfg *out, char *err, int errlen)
{
    char tmp[256], *ctx = NULL, *tok;
    int any = 0;
    memset(out, 0, sizeof *out);
    out->nagle = 1;
    strcpy_s(out->name, sizeof out->name, "eigen");
    if (err && errlen > 0) {
        err[0] = 0;
    }
    if (!spec || !spec[0] || strcmp(spec, "0") == 0 || _stricmp(spec, "aus") == 0 || _stricmp(spec, "off") == 0) {
        return 0;
    }
    strncpy_s(tmp, sizeof tmp, spec, _TRUNCATE);
    for (tok = strtok_s(tmp, ", ", &ctx); tok; tok = strtok_s(NULL, ", ", &ctx)) {
        char *c = strchr(tok, ':');
        if (!c) {
            locnetsim_cfg keep = *out;
            if (!locnetsim_profile(tok, out)) {
                if (err) _snprintf_s(err, errlen, _TRUNCATE, "Profil \"%s\" unbekannt", tok);
                return 0;
            }
            out->nagle = keep.nagle;
            out->nagle_set = keep.nagle_set;
            out->seed = keep.seed;
            out->uloss_pct = keep.uloss_pct;
            out->uloss_set = keep.uloss_set;
            out->ramp_n = keep.ramp_n;
            memcpy(out->ramp_s, keep.ramp_s, sizeof out->ramp_s);
            memcpy(out->ramp_rtt, keep.ramp_rtt, sizeof out->ramp_rtt);
            any = 1;
            continue;
        }
        *c++ = 0;
        if (_stricmp(tok, "rtt") == 0) {
            out->rtt_ms = atof(c);
        } else if (_stricmp(tok, "jitter") == 0) {
            out->jitter_ms = atof(c);
        } else if (_stricmp(tok, "loss") == 0) {
            out->loss_pct = atof(c);
        } else if (_stricmp(tok, "uloss") == 0) {
            out->uloss_pct = atof(c);
            out->uloss_set = 1;
        } else if (_stricmp(tok, "bw") == 0) {
            out->bw_kbps = atof(c);
        } else if (_stricmp(tok, "nagle") == 0) {
            out->nagle = atoi(c) ? 1 : 0;
            out->nagle_set = 1;
        } else if (_stricmp(tok, "reorder") == 0) {
            out->reorder = atoi(c) ? 1 : 0;
        } else if (_stricmp(tok, "seed") == 0) {
            out->seed = (unsigned)strtoul(c, NULL, 10);
        } else if (_stricmp(tok, "ramp") == 0) {
            char *p = c;
            out->ramp_n = 0;
            while (*p && out->ramp_n < LOCNETSIM_RAMP_MAX) {
                char *e;
                long s = strtol(p, &e, 10), r;
                if (e == p || *e != '>') {
                    if (err) _snprintf_s(err, errlen, _TRUNCATE, "ramp: \"s>rtt/...\" erwartet");
                    return 0;
                }
                p = e + 1;
                r = strtol(p, &e, 10);
                if (e == p) {
                    if (err) _snprintf_s(err, errlen, _TRUNCATE, "ramp: RTT fehlt");
                    return 0;
                }
                out->ramp_s[out->ramp_n] = (int)s;
                out->ramp_rtt[out->ramp_n] = (int)r;
                out->ramp_n++;
                p = e;
                if (*p == '/') p++;
            }
        } else {
            if (err) _snprintf_s(err, errlen, _TRUNCATE, "Schluessel \"%s\" unbekannt", tok);
            return 0;
        }
        any = 1;
    }
    if (out->rtt_ms < 0 || out->rtt_ms > 20000 || out->jitter_ms < 0 || out->jitter_ms > 5000 ||
        out->loss_pct < 0 || out->loss_pct > 50 || out->uloss_pct < 0 || out->uloss_pct > 100 || out->bw_kbps < 0) {
        if (err) _snprintf_s(err, errlen, _TRUNCATE, "Wert ausserhalb (rtt 0..20000, jitter 0..5000, loss 0..50)");
        return 0;
    }
    return any;
}

/* ---------- Zufall (wiederholbar) ---------- */
static double ns_u01(locnetsim *s)
{
    unsigned long long x = s->rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    s->rng = x;
    return (double)((x * 2685821657736338717ull) >> 11) * (1.0 / 9007199254740992.0);
}

static double ns_gauss(locnetsim *s)
{
    double u1 = ns_u01(s), u2 = ns_u01(s);
    if (u1 < 1e-12) u1 = 1e-12;
    return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

static int ns_lost(locnetsim *s, int is_tcp)
{
    if (!is_tcp && s->c.uloss_set) {
        return s->c.uloss_pct > 0 && ns_u01(s) * 100.0 < s->c.uloss_pct;
    }
    if (s->c.ge_p_gb > 0 && s->c.ge_loss_bad > 0) {
        if (s->ge_bad) {
            if (ns_u01(s) < s->c.ge_p_bg) s->ge_bad = 0;
        } else if (ns_u01(s) < s->c.ge_p_gb) {
            s->ge_bad = 1;
        }
        if (s->ge_bad && ns_u01(s) < s->c.ge_loss_bad) return 1;
    }
    return s->c.loss_pct > 0 && ns_u01(s) * 100.0 < s->c.loss_pct;
}

/* Neuuebertragung (>= 200 ms spaeter): Bursts (GE, ~1/p_bg Pakete) sind dann vorbei -> mittlere
 * Verlustrate statt des Burst-Zustands (sonst wiederholt verlorene Segmente ueber Sekunden). */
static int ns_lost_retx(locnetsim *s)
{
    double p = s->c.loss_pct / 100.0;
    if (s->c.ge_p_gb > 0 && s->c.ge_loss_bad > 0) {
        p += s->c.ge_p_gb / (s->c.ge_p_gb + s->c.ge_p_bg) * s->c.ge_loss_bad;
    }
    return p > 0 && ns_u01(s) < p;
}

static double ns_now(const locnetsim *s)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return (double)q.QuadPart * s->qpf_ms;
}

double locnetsim_rtt_now(locnetsim *s)
{
    double r = s->c.rtt_ms;
    LONG t0 = s->start_tick;
    int i;
    if (s->c.ramp_n > 0 && t0) {
        DWORD el = (GetTickCount() - (DWORD)t0) / 1000u;
        for (i = 0; i < s->c.ramp_n; i++) {
            if (el >= (DWORD)s->c.ramp_s[i]) r = s->c.ramp_rtt[i];
        }
    }
    return r;
}

void locnetsim_mark_start(locnetsim *s, DWORD tick_ms)
{
    if (s) InterlockedCompareExchange(&s->start_tick, (LONG)(tick_ms | 1u), 0);
}

static ns_sock *ns_sock_get(locnetsim *s, SOCKET sk, int create)
{
    int i, fr = -1;
    for (i = 0; i < NS_MAXSOCK; i++) {
        if (s->socks[i].used && s->socks[i].s == sk) return &s->socks[i];
        if (!s->socks[i].used && fr < 0) fr = i;
    }
    if (!create || fr < 0) return NULL;
    memset(&s->socks[fr], 0, sizeof s->socks[fr]);
    s->socks[fr].used = 1;
    s->socks[fr].s = sk;
    return &s->socks[fr];
}

static void ns_append(locnetsim *s, ns_ent *e)
{
    e->next = NULL;
    if (s->tail) s->tail->next = e; else s->head = e;
    s->tail = e;
}

int locnetsim_send(locnetsim *s, SOCKET sk, int is_tcp, const char *buf, int len, int flags,
                   const struct sockaddr_in *to)
{
    ns_ent *e;
    ns_sock *so;
    double t, rtt, one, depart, extra = 0.0, due;
    int waited = 0, joined = 0;
    if (len <= 0) {
        return len;
    }
    for (;;) { /* volle Warteschlange: blockieren wie ein voller Sendepuffer (TCP-Sockets des Spiels blockieren) */
        if (s->st.q_bytes + len <= NS_QCAP || waited >= 5000) break;
        if (waited == 0) InterlockedIncrement(&s->st.overflow);
        Sleep(1);
        waited++;
    }
    e = (ns_ent *)malloc(sizeof *e + (size_t)len);
    if (!e) {
        WSASetLastError(WSAENOBUFS);
        return SOCKET_ERROR;
    }
    memset(e, 0, sizeof *e);
    memcpy(e->data, buf, (size_t)len);
    e->s = sk;
    e->tcp = is_tcp;
    e->len = len;
    e->flags = flags;
    if (to) {
        e->to = *to;
        e->has_to = 1;
    }
    EnterCriticalSection(&s->cs);
    so = ns_sock_get(s, sk, 1);
    if (so && so->err) {
        int er = so->err;
        LeaveCriticalSection(&s->cs);
        free(e);
        WSASetLastError(er);
        return SOCKET_ERROR;
    }
    t = ns_now(s);
    rtt = locnetsim_rtt_now(s);
    one = rtt * 0.5;
    depart = t;
    if (is_tcp && s->c.nagle && so) {
        if (so->seg_depart > t) { /* kleines Segment wartet schon: anhaengen */
            depart = so->seg_depart;
            joined = 1; /* teilt das Schicksal des wartenden Segments */
            InterlockedIncrement(&s->st.nagle_held);
        } else if (so->ack_at > t && len < NS_MSS) { /* unbestaetigte Daten unterwegs: auf ACK warten */
            depart = so->ack_at;
            so->seg_depart = depart;
            so->ack_at = depart + rtt;
            InterlockedIncrement(&s->st.nagle_held);
        } else {
            so->seg_depart = t;
            so->ack_at = t + rtt;
        }
    }
    if (!joined && ns_lost(s, is_tcp)) {
        if (!is_tcp) {
            InterlockedIncrement(&s->st.udp_lost);
            LeaveCriticalSection(&s->cs);
            free(e);
            return len; /* UDP: verloren */
        } else {
            double rto = rtt + 4.0 * s->c.jitter_ms;
            if (rto < NS_RTO_MIN) rto = NS_RTO_MIN;
            do { /* TCP: Neuuebertragung nach RTO, verdoppelt je weiterem Verlust */
                extra += rto;
                rto *= 2.0;
                InterlockedIncrement(&s->st.tcp_retx);
            } while (ns_lost_retx(s) && extra < 60000.0);
        }
    }
    if (s->c.bw_kbps > 0) {
        double st0 = depart > s->busy_until ? depart : s->busy_until;
        s->busy_until = st0 + (double)(len + NS_HDR) * 8.0 / s->c.bw_kbps;
        depart = s->busy_until;
    }
    due = depart + one + extra + (s->c.jitter_ms > 0 ? fabs(ns_gauss(s)) * s->c.jitter_ms : 0.0);
    if (so && (is_tcp || !s->c.reorder)) {
        if (due < so->last_due) due = so->last_due; /* Reihenfolge bleibt (TCP immer) */
        so->last_due = due;
    }
    if (so) so->pending++;
    e->t_in = t;
    e->due = due;
    ns_append(s, e);
    {
        LONG qb = InterlockedExchangeAdd(&s->st.q_bytes, len) + len;
        if (qb > s->st.q_bytes_max) s->st.q_bytes_max = qb;
    }
    InterlockedIncrement(&s->st.queued);
    LeaveCriticalSection(&s->cs);
    SetEvent(s->ev);
    return len;
}

int locnetsim_close(locnetsim *s, SOCKET sk)
{
    ns_sock *so;
    ns_ent *e;
    int deferred = 0;
    if (!s) return 0;
    EnterCriticalSection(&s->cs);
    so = ns_sock_get(s, sk, 0);
    if (so && so->pending > 0 && (e = (ns_ent *)calloc(1, sizeof *e)) != NULL) {
        e->s = sk;
        e->kind = 1;
        e->tcp = 1;
        e->t_in = ns_now(s);
        e->due = so->last_due > e->t_in ? so->last_due : e->t_in;
        so->pending++;
        ns_append(s, e);
        deferred = 1;
    } else if (so) {
        so->used = 0;
    }
    LeaveCriticalSection(&s->cs);
    if (deferred) {
        InterlockedIncrement(&s->st.closes);
        SetEvent(s->ev);
    }
    return deferred;
}

static void ns_deliver(locnetsim *s, ns_ent *e)
{
    int err = 0;
    double t;
    if (e->kind == 1) {
        closesocket(e->s);
    } else if (e->tcp && !e->has_to) {
        int off = 0, spins = 0;
        while (off < e->len) {
            int r = send(e->s, e->data + off, e->len - off, e->flags);
            if (r > 0) {
                off += r;
                continue;
            }
            err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK && spins++ < 2000) {
                err = 0;
                Sleep(1);
                continue;
            }
            break;
        }
    } else {
        int r = e->has_to ? sendto(e->s, e->data, e->len, e->flags, (const struct sockaddr *)&e->to, sizeof e->to)
                          : send(e->s, e->data, e->len, e->flags);
        if (r < 0) err = WSAGetLastError();
    }
    t = ns_now(s);
    EnterCriticalSection(&s->cs);
    {
        ns_sock *so = ns_sock_get(s, e->s, 0);
        if (so) {
            if (err && e->tcp && !so->err) so->err = err;
            if (--so->pending <= 0 && e->kind == 1) so->used = 0;
        }
    }
    if (e->kind == 0) {
        int ms = (int)(t - e->t_in + 0.5), k = ms / 10;
        if (k >= NS_HIST) k = NS_HIST - 1;
        s->hist[k]++;
        if (ms > s->st.lat_max_ms) s->st.lat_max_ms = ms;
        InterlockedExchangeAdd(&s->st.q_bytes, -e->len);
        InterlockedIncrement(&s->st.sent);
        if (err) InterlockedIncrement(&s->st.send_err);
    }
    LeaveCriticalSection(&s->cs);
}

/* Erste zustellbare Sendung entnehmen: je Socket nur die vorderste (Reihenfolge), UDP mit
 * Umordnung frei. *next_due = fruehester Termin der Wartenden. */
static ns_ent *ns_take(locnetsim *s, double now, double *next_due, int force)
{
    SOCKET seen[NS_MAXSOCK];
    int nseen = 0, i;
    ns_ent *e, *prev = NULL;
    *next_due = 1e300;
    for (e = s->head; e; prev = e, e = e->next) {
        int ordered = e->tcp || !s->c.reorder || e->kind == 1, blocked = 0;
        if (ordered) {
            for (i = 0; i < nseen; i++) {
                if (seen[i] == e->s) { blocked = 1; break; }
            }
            if (blocked) continue;
        }
        if (force || e->due <= now) {
            if (prev) prev->next = e->next; else s->head = e->next;
            if (s->tail == e) s->tail = prev;
            return e;
        }
        if (e->due < *next_due) *next_due = e->due;
        if (ordered && nseen < NS_MAXSOCK) seen[nseen++] = e->s;
    }
    return NULL;
}

static DWORD WINAPI ns_thread(LPVOID p)
{
    locnetsim *s = (locnetsim *)p;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (;;) {
        double nd, now;
        ns_ent *e;
        int stop = s->stop != 0;
        EnterCriticalSection(&s->cs);
        now = ns_now(s);
        e = ns_take(s, now, &nd, stop);
        LeaveCriticalSection(&s->cs);
        if (e) {
            ns_deliver(s, e);
            free(e);
            continue;
        }
        if (stop) break;
        if (nd < 1e299) {
            double w = nd - now;
            if (w <= 0.0) continue;
            if (s->timer) {
                LARGE_INTEGER due;
                HANDLE hs[2];
                due.QuadPart = -(LONGLONG)(w * 10000.0); /* relativ, 100 ns */
                SetWaitableTimer(s->timer, &due, 0, NULL, NULL, FALSE);
                hs[0] = s->ev;
                hs[1] = s->timer;
                WaitForMultipleObjects(2, hs, FALSE, (DWORD)(w + 50.0));
            } else {
                WaitForSingleObject(s->ev, w >= 1.0 ? (DWORD)w : 0);
            }
        } else {
            WaitForSingleObject(s->ev, 100);
        }
    }
    return 0;
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

locnetsim *locnetsim_start(const locnetsim_cfg *c, locnetsim_log_fn log)
{
    LARGE_INTEGER f;
    locnetsim *s = (locnetsim *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->c = *c;
    s->log = log;
    QueryPerformanceFrequency(&f);
    s->qpf_ms = 1000.0 / (double)f.QuadPart;
    s->rng = 0x9E3779B97F4A7C15ull ^ ((unsigned long long)(c->seed ? c->seed : 1u) * 0xBF58476D1CE4E5B9ull);
    InitializeCriticalSection(&s->cs);
    s->ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    s->timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!s->timer) {
        s->timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    s->th = CreateThread(NULL, 0, ns_thread, s, 0, NULL);
    if (!s->ev || !s->th) {
        if (s->th) { s->stop = 1; SetEvent(s->ev); WaitForSingleObject(s->th, 2000); CloseHandle(s->th); }
        if (s->ev) CloseHandle(s->ev);
        if (s->timer) CloseHandle(s->timer);
        DeleteCriticalSection(&s->cs);
        free(s);
        return NULL;
    }
    if (log) {
        char b[256];
        _snprintf_s(b, sizeof b, _TRUNCATE,
                    "LOCNETSIM: aktiv, Profil %s: RTT %.0f ms (je Richtung %.1f), Jitter %.1f ms, Verlust %.2f %%%s%s, "
                    "Bandbreite %s, Nagle %s, Umordnung UDP %s, Stufen %d",
                    c->name, c->rtt_ms, c->rtt_ms * 0.5, c->jitter_ms, c->loss_pct,
                    c->ge_p_gb > 0 ? " + Bursts" : "", c->uloss_set ? (c->uloss_pct > 0 ? " (UDP eigen)" : " (UDP ohne)") : "", c->bw_kbps > 0 ? "begrenzt" : "unbegrenzt",
                    c->nagle ? "an" : "aus", c->reorder ? "an" : "aus", c->ramp_n);
        log(b);
    }
    return s;
}

void locnetsim_stop(locnetsim *s)
{
    if (!s) return;
    s->stop = 1;
    SetEvent(s->ev);
    WaitForSingleObject(s->th, 5000);
    CloseHandle(s->th);
    CloseHandle(s->ev);
    if (s->timer) CloseHandle(s->timer);
    DeleteCriticalSection(&s->cs);
    free(s);
}

const locnetsim_cfg *locnetsim_config(const locnetsim *s) { return &s->c; }
const locnetsim_stats *locnetsim_get_stats(const locnetsim *s) { return &s->st; }

int locnetsim_lat_pct(const locnetsim *s, int pct)
{
    long long tot = 0, acc = 0, need;
    int i;
    for (i = 0; i < NS_HIST; i++) tot += s->hist[i];
    if (!tot) return 0;
    need = (tot * pct + 99) / 100;
    if (need < 1) need = 1;
    for (i = 0; i < NS_HIST; i++) {
        acc += s->hist[i];
        if (acc >= need) return (i + 1) * 10 < s->st.lat_max_ms ? (i + 1) * 10 : (int)s->st.lat_max_ms;
    }
    return NS_HIST * 10;
}

int locnetsim_pending(locnetsim *s)
{
    int n = 0;
    ns_ent *e;
    EnterCriticalSection(&s->cs);
    for (e = s->head; e; e = e->next) n++;
    LeaveCriticalSection(&s->cs);
    return n;
}
