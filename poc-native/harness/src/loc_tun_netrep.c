/* loc_tun_netrep.c - W496 NET-TELEMETRY Stufe 2: Netzbericht an steelnet (nur mit Zustimmung des Spielers).
 *
 * Ohne Zustimmung (loctun_cfg.net_report = 0) gibt es keinen Zustand, keine Datei und keine Zeile: nr_start loescht
 * eine alte Warteschlange und die Laufmarke (Zustimmung zurueckgezogen) und laesst t->nr = NULL.
 *
 * Inhalt (steelnet PROTOKOLL.md "W496"): feste Codes und Zahlen - Version, Fassung (community/private), GPU-Hersteller
 * (nur die PCI-ID), Modus (sim/arcade), Netz (syslink/live), Spielerzahl, Dauer, je Mitspieler Ping-Klasse Median/p95
 * und Verlust in %, Rollbacks/s, Desyncs, Neuverbindungen, Trennungsgrund (Client-Sicht), Weg (direkt/Relay), Relayname
 * (der Dienst macht daraus die Region und speichert den Namen nicht), Grund des Endes. Nie: Namen, Adressen, Ports,
 * Konto, Raumcode, Eingaben, Chat.
 *
 * Senden: ueber den angemeldeten Steuerkanal ({"t":"net_report","r":{...}}), nur wenn welcome "netlog":true meldet.
 * Was nicht gesendet werden kann (getrennt, Prozessende), liegt in einer kleinen Warteschlange (hoechstens NR_Q Zeilen,
 * nur Codes) in netrep_dir und geht bei der naechsten Anmeldung raus; danach wird die Datei geloescht. Laufmarke:
 * existiert sie beim Start noch, endete der vorige Lauf ohne geordnetes Ende -> nur der Code "crash" (keine Daten).
 *
 * Alles im Transport-Thread bzw. unter t->cs, Takt 1 s (nr_tick); kein Einfluss auf Spiel-Thread oder Netz-Timing. */
#include "loc_tun_int.h"
#include "sb_edition.h"

#define NR_Q 4
#define NR_LINE 700
#define NR_PC 6
#define NR_PEERS 9
#define NR_TICK_MS 1000.0

enum { NR_H_NONE = 0, NR_H_HOST, NR_H_DISC };

static const char *const k_pc[NR_PC] = {"lt100", "100_200", "200_300", "300_400", "400_500", "gt500"};

typedef struct tnetrep {
    int srv;            /* welcome: "netlog":true (Dienst nimmt Berichte) */
    int welcomes;       /* Anmeldungen in diesem Lauf (Neuverbindungen = welcomes - 1) */
    int online;         /* angemeldet */
    double on_at;
    int hint;           /* NR_H_*: Grund fuer das naechste Lobby-/Match-Ende */
    char why[12];       /* letzter Trennungsgrund (Client-Sicht) */
    int mode;           /* zuletzt gesehener Lobby-Modus (1 sim, 2 arcade) */
    /* Lobby */
    int lobby, lobby_peak, lobby_id; /* lobby_id: Treffpunkt-Nummer, nur zum Erkennen eines Lobbywechsels (nie gesendet) */
    double lobby_at;
    /* Match */
    int match, np, up_max, peer_lost, path_mask, relay_i;
    int match_done;            /* Match wegen "allein" beendet: neues erst nach dem Missionsende des Spiels */
    double match_at, alone_at; /* alone_at: seit wann kein Mitspieler mehr verbunden ist (0 = verbunden) */
    unsigned rb0, ds0;
    uint32_t cid[TUN_MAX_LINKS];
    unsigned hist[TUN_MAX_LINKS][NR_PC], loss_sum[TUN_MAX_LINKS], loss_n[TUN_MAX_LINKS];
    /* Warteschlange */
    char q[NR_Q][NR_LINE];
    int nq;
    char qpath[300], mpath[300];
    double tick_at;
    int exited;
    /* TEST */
    int test_only;      /* loctun_netrep_test aktiv: der Transport-Thread taktet nicht mehr (kein Wettlauf) */
    int test_gi_on;
    loctun_game_info test_gi;
    double test_dt;
} tnetrep;

static int nr_pclass(double ms)
{
    if (!(ms >= 0.0)) return -1;
    if (ms < 100.0) return 0;
    if (ms < 200.0) return 1;
    if (ms < 300.0) return 2;
    if (ms < 400.0) return 3;
    if (ms < 500.0) return 4;
    return 5;
}

/* Klasse, bei der der kumulierte Anteil pct erreicht (Median 50, p95 95); -1 = keine Probe */
static int nr_hist_q(const unsigned *h, int pct)
{
    unsigned tot = 0, run = 0;
    int i;
    for (i = 0; i < NR_PC; i++) tot += h[i];
    if (!tot) return -1;
    for (i = 0; i < NR_PC; i++) {
        run += h[i];
        if ((double)run * 100.0 >= (double)tot * pct) return i;
    }
    return NR_PC - 1;
}

/* nur [0-9A-Za-z.+-], 1..16 Zeichen (Version) bzw. [A-Za-z0-9_.:-] bis 23 (Relayname); sonst leer */
static void nr_clean(const char *in, char *out, int cap, int relay)
{
    int i, n = 0;
    out[0] = 0;
    for (i = 0; in && in[i]; i++) {
        char ch = in[i];
        int ok = isalnum((unsigned char)ch) || ch == '.' || ch == '-' || (relay ? (ch == '_' || ch == ':') : ch == '+');
        if (!ok || n >= cap - 1) {
            out[0] = 0;
            return;
        }
        out[n++] = ch;
    }
    out[n] = 0;
}

static void nr_save(loctun *t)
{
    tnetrep *r = t->nr;
    FILE *f = NULL;
    int i;
    if (!r->qpath[0]) return;
    if (!r->nq) {
        DeleteFileA(r->qpath);
        return;
    }
    if (fopen_s(&f, r->qpath, "wb") != 0 || !f) return;
    for (i = 0; i < r->nq; i++) fprintf(f, "%s\n", r->q[i]);
    fclose(f);
}

static void nr_load(loctun *t)
{
    tnetrep *r = t->nr;
    FILE *f = NULL;
    char line[NR_LINE + 8];
    if (!r->qpath[0] || fopen_s(&f, r->qpath, "rb") != 0 || !f) return;
    while (r->nq < NR_Q && fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        /* nur eigene Zeilen (Form pruefen, nichts Fremdes weitersenden) */
        static const char pre[] = "{\"t\":\"net_report\",\"r\":{\"v\":1,";
        if (l > 0 && l < NR_LINE && strncmp(line, pre, sizeof pre - 1) == 0 &&
            line[l - 1] == '}' && !strchr(line, '\\'))
            strcpy_s(r->q[r->nq++], NR_LINE, line);
    }
    fclose(f);
}

static void nr_push(loctun *t, const char *line)
{
    tnetrep *r = t->nr;
    if (r->nq == NR_Q) { /* aelteste verwerfen */
        memmove(r->q[0], r->q[1], sizeof r->q[0] * (NR_Q - 1));
        r->nq--;
    }
    strcpy_s(r->q[r->nq++], NR_LINE, line);
    nr_save(t);
}

static int nr_links(loctun *t, int *up)
{
    int i, n = 0, u = 0;
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        if (!L->used || L->cid == 0) continue;
        n++;
        if (L->up) u++;
    }
    if (up) *up = u;
    return n;
}

/* Bericht bauen und einreihen. kind: lobby|match|disconnect|crash; end/why: Codes (NULL = Feld weglassen) */
static void nr_report(loctun *t, const char *kind, const char *end, const char *why, int np, double dur_ms, int match)
{
    tnetrep *r = t->nr;
    char b[NR_LINE], ver[20], rel[24] = "";
    int o, i, np_out = 0, have = 0;
    unsigned gpu = t->c.gpu_vendor;
    loctun_game_info gi;
    memset(&gi, 0, sizeof gi);
    if (r->test_gi_on) {
        gi = r->test_gi;
        have = 1;
    } else if (t->game_fn) {
        have = t->game_fn(&gi);
    }
    if (have && gi.gpu_vendor) gpu = gi.gpu_vendor;
    nr_clean(tl_myver(t), ver, 17, 0);
    o = _snprintf_s(b, sizeof b, _TRUNCATE, "{\"t\":\"net_report\",\"r\":{\"v\":1,\"kind\":\"%s\"", kind);
    if (ver[0]) o += _snprintf_s(b + o, sizeof b - o, _TRUNCATE, ",\"ver\":\"%s\"", ver);
    o += _snprintf_s(b + o, sizeof b - o, _TRUNCATE, ",\"ed\":\"%s\",\"gpu\":%u", SB_EDITION_COMMUNITY ? "community" : "private",
                     gpu & 0xFFFFu);
    if (strcmp(kind, "crash") != 0) {
        o += _snprintf_s(b + o, sizeof b - o, _TRUNCATE, ",\"net\":\"%s\",\"recon\":%d", t->live ? "live" : "syslink",
                         r->welcomes > 1 ? (r->welcomes - 1 > 100 ? 100 : r->welcomes - 1) : 0);
        if (r->mode) o += _snprintf_s(b + o, sizeof b - o, _TRUNCATE, ",\"mode\":\"%s\"", r->mode == 2 ? "arcade" : "sim");
        if (np > 0) o += _snprintf_s(b + o, sizeof b - o, _TRUNCATE, ",\"np\":%d", np > 10 ? 10 : np);
        if (dur_ms >= 0.0) {
            double s = dur_ms / 1000.0;
            o += _snprintf_s(b + o, sizeof b - o, _TRUNCATE, ",\"dur\":%d", s > 86400.0 ? 86400 : (int)(s + 0.5));
        }
    }
    if (end) o += _snprintf_s(b + o, sizeof b - o, _TRUNCATE, ",\"end\":\"%s\"", end);
    if (why) o += _snprintf_s(b + o, sizeof b - o, _TRUNCATE, ",\"why\":\"%s\"", why);
    if (match) {
        o += _snprintf_s(b + o, sizeof b - o, _TRUNCATE, ",\"peers\":[");
        for (i = 0; i < TUN_MAX_LINKS && np_out < NR_PEERS; i++) {
            int p50 = nr_hist_q(r->hist[i], 50), p95 = nr_hist_q(r->hist[i], 95);
            unsigned loss = r->loss_n[i] ? (r->loss_sum[i] / r->loss_n[i] + 5) / 10 : 0; /* Promille -> % */
            if (p50 < 0 || p95 < 0) continue;
            o += _snprintf_s(b + o, sizeof b - o, _TRUNCATE, "%s[\"%s\",\"%s\",%u]", np_out ? "," : "", k_pc[p50], k_pc[p95],
                             loss > 100 ? 100 : loss);
            np_out++;
        }
        o += _snprintf_s(b + o, sizeof b - o, _TRUNCATE, "]");
        if (have && dur_ms > 1000.0) {
            double rbs = (double)(gi.rb_total - r->rb0) / (dur_ms / 1000.0);
            unsigned ds = gi.desyncs - r->ds0;
            o += _snprintf_s(b + o, sizeof b - o, _TRUNCATE, ",\"rbs\":%.1f,\"desync\":%u", rbs > 1000.0 ? 1000.0 : rbs,
                             ds > 100 ? 100 : ds);
        }
        o += _snprintf_s(b + o, sizeof b - o, _TRUNCATE, ",\"path\":\"%s\"",
                         r->path_mask == 3 ? "mixed" : r->path_mask == 2 ? "relay" : r->path_mask == 1 ? "direct" : "none");
        if ((r->path_mask & 2) && r->relay_i >= 0 && r->relay_i < t->nrelays) nr_clean(t->relays[r->relay_i].name, rel, 24, 1);
        if (rel[0]) o += _snprintf_s(b + o, sizeof b - o, _TRUNCATE, ",\"relay\":\"%s\"", rel);
    }
    if (o <= 0 || o >= (int)sizeof b - 3) return; /* abgeschnitten: lieber nichts als ein kaputter Bericht */
    strcat_s(b, sizeof b, "}}");
    nr_push(t, b);
    tl_logf(t, "LOCNETREP: Netzbericht \"%s\" eingereiht (%d in der Warteschlange)", kind, r->nq);
}

const char *tl_drop_code(const char *why)
{
    if (!why) return "error";
    if (strcmp(why, "eof") == 0) return "closed";
    if (strstr(why, "timeout")) return "timeout";
    if (strncmp(why, "tls", 3) == 0) return "tls";
    if (strcmp(why, "connect") == 0) return "refused";
    if (strcmp(why, "upgrade_required") == 0 || strcmp(why, "version_required") == 0 || strcmp(why, "e2e_required") == 0)
        return "version";
    return "error";
}

void nr_start(loctun *t)
{
    tnetrep *r;
    char qp[300] = "", mp[300] = "";
    if (t->c.netrep_dir[0]) {
        _snprintf_s(qp, sizeof qp, _TRUNCATE, "%s\\net_report_q.txt", t->c.netrep_dir);
        _snprintf_s(mp, sizeof mp, _TRUNCATE, "%s\\net_report_run.txt", t->c.netrep_dir);
    }
    if (!t->c.net_report) { /* keine Zustimmung (bzw. zurueckgezogen): nichts behalten, nichts senden */
        if (qp[0] && DeleteFileA(qp)) tl_logf(t, "LOCNETREP: Zustimmung aus - alte Warteschlange geloescht");
        if (mp[0]) DeleteFileA(mp);
        return;
    }
    if (!t->c.lobby[0]) return; /* nur steelnet (LAN/Test ohne Treffpunkt: kein Bericht) */
    r = (tnetrep *)calloc(1, sizeof *r);
    if (!r) return;
    t->nr = r;
    r->relay_i = -1;
    strcpy_s(r->qpath, sizeof r->qpath, qp);
    strcpy_s(r->mpath, sizeof r->mpath, mp);
    nr_load(t);
    if (mp[0]) {
        FILE *f = NULL;
        if (GetFileAttributesA(mp) != INVALID_FILE_ATTRIBUTES) nr_report(t, "crash", NULL, NULL, 0, -1.0, 0);
        if (fopen_s(&f, mp, "wb") == 0 && f) {
            fputs("1\n", f);
            fclose(f);
        }
    }
    tl_logf(t, "LOCNETREP: Netzbericht an (Zustimmung), %d Eintraege in der Warteschlange", r->nq);
}

void nr_free(loctun *t)
{
    if (t->nr) {
        SecureZeroMemory(t->nr, sizeof *t->nr);
        free(t->nr);
        t->nr = NULL;
    }
}

void nr_on_welcome(loctun *t, const char *m, const char *e, double now)
{
    tnetrep *r = t->nr;
    const char *v, *ve;
    if (!r) return;
    v = js_get(m, e, "netlog", &ve);
    r->srv = v && ve - v == 4 && memcmp(v, "true", 4) == 0;
    r->welcomes++;
    r->online = 1;
    r->on_at = now;
}

void nr_on_drop(loctun *t, const char *why, double now)
{
    tnetrep *r = t->nr;
    if (!r) return;
    strcpy_s(r->why, sizeof r->why, tl_drop_code(why));
    if (r->online) { /* nur eine stehende Anmeldung zaehlt (keine Berichte fuer jeden gescheiterten Aufbau) */
        r->online = 0;
        r->hint = NR_H_DISC;
        nr_report(t, "disconnect", NULL, r->why, 0, now - r->on_at, 0);
    }
}

void nr_on_host_left(loctun *t)
{
    if (t->nr && t->nr->hint == NR_H_NONE) t->nr->hint = NR_H_HOST;
}

static void nr_match_begin(loctun *t, const loctun_game_info *gi, double now)
{
    tnetrep *r = t->nr;
    int up = 0, n = nr_links(t, &up);
    r->match = 1;
    r->match_at = now;
    r->rb0 = gi->rb_total;
    r->ds0 = gi->desyncs;
    r->np = gi->npeers >= 0 ? gi->npeers + 1 : n + 1;
    r->up_max = up;
    r->peer_lost = 0;
    r->alone_at = 0.0;
    r->path_mask = 0;
    r->relay_i = -1;
    memset(r->cid, 0, sizeof r->cid);
    memset(r->hist, 0, sizeof r->hist);
    memset(r->loss_sum, 0, sizeof r->loss_sum);
    memset(r->loss_n, 0, sizeof r->loss_n);
    if (r->hint == NR_H_HOST) r->hint = NR_H_NONE;
}

static void nr_match_sample(loctun *t)
{
    tnetrep *r = t->nr;
    int i, up = 0;
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        int k, slot = -1, pc;
        if (!L->used || L->cid == 0 || !L->up) continue;
        up++;
        for (k = 0; k < TUN_MAX_LINKS; k++) {
            if (r->cid[k] == L->cid) {
                slot = k;
                break;
            }
            if (slot < 0 && r->cid[k] == 0) slot = k;
        }
        if (slot < 0) continue;
        r->cid[slot] = L->cid; /* nur die Steuer-Nummer dieser Verbindung, verlaesst den Prozess nie */
        pc = nr_pclass(L->srtt);
        if (pc >= 0) r->hist[slot][pc]++;
        r->loss_sum[slot] += (unsigned)(L->loss_in < 0 ? 0 : L->loss_in);
        r->loss_n[slot]++;
        if (L->path == 1) r->path_mask |= 1;
        if (L->path == 2) {
            r->path_mask |= 2;
            r->relay_i = L->relay_idx;
        }
    }
    if (up < r->up_max) r->peer_lost = 1;
    if (up > r->up_max) r->up_max = up;
    if (up == 0 && r->up_max > 0) {
        if (r->alone_at == 0.0) r->alone_at = r->tick_at;
    } else {
        r->alone_at = 0.0;
    }
}

static void nr_match_end(loctun *t, const loctun_game_info *gi, double now, const char *force_end)
{
    tnetrep *r = t->nr;
    const char *end = force_end;
    if (!end) {
        if (gi && gi->desyncs != r->ds0) end = "desync";
        else if (r->hint == NR_H_DISC) end = "disconnect";
        else if (r->peer_lost) end = "peer_lost";
        else if (r->hint == NR_H_HOST) end = "host_left";
        else end = "end";
    }
    nr_report(t, "match", end, NULL, r->np, now - r->match_at, 1);
    r->match = 0;
}

static void nr_tick_in(loctun *t, double now)
{
    tnetrep *r = t->nr;
    loctun_game_info gi;
    int have = 0, joined, up = 0, n;
    if (!r || r->exited) return;
    now += r->test_dt;
    if (now - r->tick_at < NR_TICK_MS) return;
    r->tick_at = now;
    if (t->play_mode) r->mode = (int)t->play_mode;
    /* Lobby */
    joined = t->lb_joined;
    n = nr_links(t, &up);
    if (joined && r->lobby && t->lb_lobby_id != r->lobby_id && !r->match) { /* neue Lobby ohne Pause dazwischen */
        nr_report(t, "lobby", r->hint == NR_H_DISC ? "disconnect" : r->hint == NR_H_HOST ? "host_left" : "left", NULL,
                  r->lobby_peak, now - r->lobby_at, 0);
        r->lobby = 0;
        if (r->hint == NR_H_HOST) r->hint = NR_H_NONE;
    }
    if (joined && !r->lobby) {
        r->lobby_id = t->lb_lobby_id;
        r->lobby = 1;
        r->lobby_at = now;
        r->lobby_peak = n + 1;
        if (r->hint != NR_H_DISC) r->hint = NR_H_NONE;
    } else if (r->lobby && joined && n + 1 > r->lobby_peak) {
        r->lobby_peak = n + 1;
    }
    /* Match (Spielzustand) */
    memset(&gi, 0, sizeof gi);
    if (r->test_gi_on) {
        gi = r->test_gi;
        have = 1;
    } else if (t->game_fn) {
        have = t->game_fn(&gi);
    }
    if (have && !gi.in_mission) r->match_done = 0;
    if (have && gi.in_mission && !r->match && !r->match_done) nr_match_begin(t, &gi, now);
    if (r->match) nr_match_sample(t);
    /* alle Mitspieler seit 5 s weg (Lobby geschlossen, Abbruch): fuer das Netz ist das Match vorbei, auch wenn das
     * Spiel allein weiterlaeuft - Bericht jetzt (die Anmeldung steht meist noch), nicht erst beim Prozessende */
    if (r->match && r->alone_at > 0.0 && now - r->alone_at >= 5000.0) {
        nr_match_end(t, have ? &gi : NULL, now, NULL);
        r->match_done = 1;
    }
    if (r->match && have && !gi.in_mission) nr_match_end(t, &gi, now, NULL);
    if (r->lobby && !joined && !r->match) {
        nr_report(t, "lobby", r->hint == NR_H_DISC ? "disconnect" : r->hint == NR_H_HOST ? "host_left" : "left", NULL,
                  r->lobby_peak, now - r->lobby_at, 0);
        r->lobby = 0;
        if (r->hint == NR_H_HOST) r->hint = NR_H_NONE;
    }
    if (r->hint == NR_H_DISC && r->online && !r->lobby && !r->match) r->hint = NR_H_NONE;
    /* Senden: angemeldet, Dienst nimmt Berichte */
    if (r->nq && r->online && r->srv && t->my_cid && (t->lb_state == LB_HELLO || t->lb_state == LB_LOBBY)) {
        int i, k = r->nq;
        for (i = 0; i < k; i++) {
            lb_send(t, r->q[i]);
            SecureZeroMemory(r->q[i], sizeof r->q[i]);
        }
        r->nq = 0;
        nr_save(t); /* gesendet -> Datei weg */
        tl_logf(t, "LOCNETREP: %d Netzbericht(e) gesendet", k);
    }
}

void nr_tick(loctun *t, double now)
{
    if (t->nr && !t->nr->test_only) nr_tick_in(t, now);
}

void nr_exit(loctun *t)
{
    tnetrep *r = t->nr;
    double now;
    if (!r || r->exited) return;
    now = tl_now(t) + r->test_dt;
    if (r->match) nr_match_end(t, NULL, now, "quit");
    else if (r->lobby) nr_report(t, "lobby", "quit", NULL, r->lobby_peak, now - r->lobby_at, 0);
    r->lobby = r->match = 0;
    nr_save(t);
    if (r->mpath[0]) DeleteFileA(r->mpath); /* geordnetes Ende: keine "crash"-Meldung beim naechsten Start */
    r->exited = 1;
}

void loctun_set_game_fn(loctun *t, loctun_game_fn fn)
{
    if (t) t->game_fn = fn;
}

int loctun_netrep_test(loctun *t, int op, int a, int b, int c, const char *s, char *out, int cap)
{
    tnetrep *r;
    int ret = 0;
    if (out && cap > 0) out[0] = 0;
    if (!t) return -1;
    EnterCriticalSection(&t->cs);
    r = t->nr;
    if (!r) {
        LeaveCriticalSection(&t->cs);
        return -1;
    }
    t->test_cap = out;
    t->test_cap_len = 0;
    t->test_cap_max = cap;
    r->test_only = 1;
    switch (op) {
    case 1:
        r->test_gi_on = 1;
        r->test_gi.in_mission = a;
        r->test_gi.rb_total = (unsigned)b;
        r->test_gi.desyncs = (unsigned)c;
        r->test_gi.npeers = -1;
        break;
    case 2:
        r->test_dt += (double)a;
        nr_tick_in(t, tl_now(t));
        ret = r->nq;
        break;
    case 3: {
        char w[40];
        strcpy_s(w, sizeof w, a ? "{\"netlog\":true}" : "{\"netlog\":false}");
        if (!t->my_cid) t->my_cid = 7;
        if (t->lb_state == LB_OFF || t->lb_state == LB_CONNECTING) t->lb_state = LB_HELLO;
        nr_on_welcome(t, w, w + strlen(w), tl_now(t) + r->test_dt);
        break;
    }
    case 4:
        nr_on_drop(t, s, tl_now(t) + r->test_dt);
        t->lb_state = LB_OFF;
        t->lb_joined = 0;
        break;
    case 5:
        t->lb_joined = a;
        if (a && t->lb_state == LB_HELLO) t->lb_state = LB_LOBBY;
        if (!a && t->lb_state == LB_LOBBY) t->lb_state = LB_HELLO;
        break;
    case 6:
        ret = r->nq;
        t->test_cap = NULL;
        if (r->nq && out && cap > 0) strcpy_s(out, (size_t)cap, r->q[0]);
        break;
    case 7:
        nr_exit(t);
        ret = r->nq;
        break;
    default:
        ret = -2;
    }
    t->test_cap = NULL;
    LeaveCriticalSection(&t->cs);
    return ret;
}
