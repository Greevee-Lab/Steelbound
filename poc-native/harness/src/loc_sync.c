/* loc_sync.c - W297 LOC-MISSION: Lockstep-Synchronitaet der LoC-Mission messen.
 *
 * Befund (Ghidra, LoC-VAs):
 *  - Waehrend eines Simulationsschritts summiert das Spiel Zustandsgroessen (VT-Positionen,
 *    Treffer-/Schadenswerte ...) als Float in den Akkumulator [0x6D0C58] (FUN_000C9420).
 *  - FUN_0001D010 baut je Tick den lokalen 0x34-Rahmen: CALL FUN_000C9400 @0x1D391 liest und
 *    loescht den Akkumulator, _ftol (FUN_0015004C) -> Sync-Byte [0x28DFD1], FUN_0004EC80 sendet.
 *  - FUN_0001D4C0 vergleicht die Sync-Bytes aller Spieler ([0x30708C+i*4]); weichen mindestens
 *    die Haelfte ab -> [0x306790] = 1 und eigene Mission endet (Desync).
 *  - Simulationstick [0x3070F4] (FUN_0002F100), Stau: [0x3069C0] = 1 solange Rahmen fehlen,
 *    [0x306C34] Stau-Frames gesamt, [0x306F48] Stau-Episoden, aktuelle Verzoegerung [0x306F04].
 *
 * Messung (nur mit loc_sync=1, nur LoC-Profil + Netz-HLE): Der CALL @0x1D391 laeuft ueber einen
 * Thunk, der den vollen 32-Bit-Wert des Akkumulators (nicht nur das Sync-Byte) in einen laufenden
 * 64-Bit-Hash (FNV-1a) einrechnet. Alle loc_sync_every Rahmen eine Logzeile LOCSYNC mit
 * Rahmenzaehler, Sim-Tick, Hash und Stauzaehlern. Zwei Clients vergleicht
 * scripts/loc_sync_compare.py (gleicher Rahmen -> gleicher Hash). Es werden keine Eingaben und
 * keine Paketinhalte protokolliert, nur ein Hash des lokalen Spielzustands. */
#include "loc_sync.h"
#include "hcfg.h"
#include "hlog.h"
#include "hooks_int.h"
#include "loc_net.h"
#include "loc_dodge.h"
#include "loc_state.h"
#include "loc_rollback.h"
#include "loc_lsarb_game.h"

#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;

#define LS_CALL_VA 0x1D391u  /* CALL FUN_000C9400 in FUN_0001D010 */
#define LS_ACC_FN 0xC9400u   /* Akkumulator lesen + loeschen */
#define LS_ACC 0x6D0C58u     /* Float-Akkumulator */
#define LS_TICK 0x3070F4u    /* Simulationstick */
#define LS_STALL 0x3069C0u   /* Stau-Flag */
#define LS_STALLF 0x306C34u  /* Stau-Frames gesamt */
#define LS_STALLE 0x306F48u  /* Stau-Episoden */
#define LS_DELAY 0x306F04u   /* Eingabeverzoegerung (Ticks) */
#define LS_DESYNC 0x306790u  /* Desync erkannt */
#define LS_NPL 0x3071A0u     /* Spielerzahl */
#define LS_ACTIVE 0x306830u  /* Aktiv-Maske */

static int s_on;
static u32 s_jmp_target = LS_ACC_FN;
static volatile LONG s_frames;
unsigned loc_sync_frames(void) { return (unsigned)s_frames; } /* W390 DIAG */
static unsigned long long s_hash = 1469598103934665603ull;
static u32 s_last_tick;
static u32 s_desync_logged;
static int s_ext; /* W338: Hash/LOCSYNC aus der Eingabeschicht (loc_rb_input) */

/* W297 TEST loc_replay_tick: "rahmen:vk,len;..." (len in Bildern), aufsteigend sortiert erwartet.
 * W336 LOCTP: "rahmen:vk,len@P" wiederholt den Eintrag alle P Lockstep-Rahmen ab "rahmen" (Testpartner:
 * Bewegen/Feuern bis Missionsende ohne lange Liste); Eintraege ohne @ wie bisher genau einmal. */
#define LS_RT_MAX 96
#define LS_RP_MAX 16
static struct { LONG at; int vk, len; } s_rt[LS_RT_MAX];
static int s_rt_n, s_rt_cur;
static struct { LONG next, period, at; int vk, len; } s_rp[LS_RP_MAX]; /* W375: at = Start (neue Mission) */
static u32 s_ls_missions; /* W375: Missionswechsel im selben Prozess (Zaehler/Hash/Testtasten neu) */
static int s_rp_n;

static void ls_rt_parse(const char *p)
{
    while (*p && s_rt_n < LS_RT_MAX) {
        long at, per = 0;
        int vk, len;
        while (*p == ' ' || *p == ';') p++;
        if (!*p) break;
        at = strtol(p, (char **)&p, 10);
        if (*p == ':') p++;
        vk = (int)strtol(p, (char **)&p, 10);
        len = 8;
        if (*p == ',') {
            p++;
            len = (int)strtol(p, (char **)&p, 10);
        }
        if (*p == '@') { /* W336: Wiederholung */
            p++;
            per = strtol(p, (char **)&p, 10);
        }
        while (*p && *p != ';') p++;
        if (per > 0) {
            if (s_rp_n < LS_RP_MAX) {
                s_rp[s_rp_n].next = at;
                s_rp[s_rp_n].at = at;
                s_rp[s_rp_n].period = per;
                s_rp[s_rp_n].vk = vk;
                s_rp[s_rp_n].len = len;
                s_rp_n++;
            }
            continue;
        }
        s_rt[s_rt_n].at = at;
        s_rt[s_rt_n].vk = vk;
        s_rt[s_rt_n].len = len;
        s_rt_n++;
    }
}

/* W297 TEST loc_steps: zustandsgesteuerte Testtasten fuer die Netzmenues (bildgetaktete
 * loc_replay-Folgen brechen, wenn ein Client langsamer laeuft). Schritte nacheinander:
 *   AUSLOESER[+N]>vk,len   mit AUSLOESER = acc>=K (angenommene TCP-Verbindungen),
 *   con>=K (eigene connects), rx:0xTT / tx:0xTT (Rahmentyp empfangen/gesendet gesehen),
 *   sync>=K (Lockstep-Rahmen), f (sofort), W344: scn=S[.T] (Szene[.Schritt]), mnK=V (Menuewort K).
 *   +N = N Bilder nach dem Ausloesen. vk 0 = Bild, vk 1 = Draw-Zensus (DIAG).
 *   W344: AUSLOESER/N=J[+M]>... = kommt der Ausloeser nicht binnen N Bildern, weiter bei Schritt J (1-basiert,
 *   ohne Taste); j=J = Sprung zu Schritt J (Wiederholung, z. B. erneut suchen). Hoechstens 1000 Spruenge
 *   (je Sprung mindestens ein Bild; Suchschleife 2 Spruenge je ~12 s = ~1,5 h Warten auf das Match). */
#define LS_ST_MAX 192 /* W375: 96 -> 192 (drei Runden); W366: 48 -> 96 (zweite Runde ohne Neustart: Nachbesprechung, SIGN OUT, neues Match) */
/* W344 LOC-TP-UX: Menue-Zustand als Ausloeser (statt Bildzaehler): scn=S bzw. scn=S.T (Szene u16 [0x727988],
 * Schritt u16 [0x72798A]), mnK=V (Wort K 0..5 des Menue-Unterzustands [[0x6DC678]], LOC_BOOT §15.4/§30). */
enum { LST_F, LST_ACC, LST_CON, LST_RX, LST_TX, LST_SYNC, LST_SCN, LST_MN, LST_J, LST_MSK, LST_STL };
/* W375: msk=1 / msk=0 = Lockstep-Aktiv-Maske [0x306830] gesetzt / leer (Missionsbeginn/-ende fuer Folgen ueber
 * mehrere Runden im selben Prozess). */
/* W375: stl=N = seit N Bildern kein neuer Lockstep-Rahmen (nach mindestens 100 Rahmen dieser Mission): Ende der
 * Mission (TIME UP, Ergebnis) unabhaengig von der Spielzeit. */
static long s_stl_frames = -1;
static unsigned s_stl_since;
#define LS_SCENE 0x727988u   /* Szene (u16), Schritt (u16) bei +2 */
#define LS_MENU 0x6DC678u    /* Zeiger auf den Menue-Unterzustand (Szene 5) */
#define LS_SCENE6 0x6DD3F8u  /* Zeiger auf den Szene-6-Unterzustand */
static struct { int kind; long arg, arg2; int delay, vk, len, to, to_j; } s_st[LS_ST_MAX];
static int s_st_n, s_st_cur, s_st_hit, s_st_jumps;
static unsigned s_st_hitf, s_st_since; /* W344: Bild, seit dem der aktuelle Schritt wartet */

static void ls_st_parse(const char *p)
{
    while (*p && s_st_n < LS_ST_MAX) {
        char tok[96];
        unsigned k = 0;
        const char *q;
        int kind = -1;
        long arg = 0, arg2 = -1;
        while (*p == ' ' || *p == ';') p++;
        if (!*p) break;
        while (*p && *p != ';' && k < sizeof tok - 1) tok[k++] = *p++;
        tok[k] = 0;
        if (strncmp(tok, "acc>=", 5) == 0) { kind = LST_ACC; arg = strtol(tok + 5, NULL, 0); }
        else if (strncmp(tok, "con>=", 5) == 0) { kind = LST_CON; arg = strtol(tok + 5, NULL, 0); }
        else if (strncmp(tok, "rx:", 3) == 0) { kind = LST_RX; arg = strtol(tok + 3, NULL, 0); }
        else if (strncmp(tok, "tx:", 3) == 0) { kind = LST_TX; arg = strtol(tok + 3, NULL, 0); }
        else if (strncmp(tok, "sync>=", 6) == 0) { kind = LST_SYNC; arg = strtol(tok + 6, NULL, 0); }
        else if (strncmp(tok, "msk=", 4) == 0) { kind = LST_MSK; arg = strtol(tok + 4, NULL, 0); } /* W375 */
        else if (strncmp(tok, "stl=", 4) == 0) { kind = LST_STL; arg = strtol(tok + 4, NULL, 0); } /* W375 */
        else if (strncmp(tok, "scn=", 4) == 0) { /* W344: Szene[.Schritt] */
            char *e = NULL;
            kind = LST_SCN;
            arg = strtol(tok + 4, &e, 0);
            arg2 = (e && *e == '.') ? strtol(e + 1, NULL, 0) : -1;
        } else if (tok[0] == 'm' && tok[1] == 'n' && tok[2] >= '0' && tok[2] <= '5' && tok[3] == '=') { /* W344 */
            kind = LST_MN;
            arg = tok[2] - '0';
            arg2 = (long)strtoul(tok + 4, NULL, 0);
        }
        else if (tok[0] == 'f') { kind = LST_F; }
        else if (tok[0] == 'j' && tok[1] == '=') { /* W344: Sprung */
            memset(&s_st[s_st_n], 0, sizeof s_st[s_st_n]);
            s_st[s_st_n].kind = LST_J;
            s_st[s_st_n].arg = atol(tok + 2);
            s_st_n++;
            continue;
        }
        if (kind < 0 || !strrchr(tok, '>')) {
            HLOG_W("LOCSTEPS: unbekannter Schritt %s", tok);
            continue;
        }
        /* Aktion nach dem LETZTEN '>' (acc>=/sync>= enthalten selbst ein '>') */
        q = strchr(tok, '+');
        s_st[s_st_n].delay = (q && q < strrchr(tok, '>')) ? atoi(q + 1) : 0;
        q = strrchr(tok, '>');
        s_st[s_st_n].vk = atoi(q + 1);
        q = strchr(q, ',');
        s_st[s_st_n].len = q ? atoi(q + 1) : 8;
        s_st[s_st_n].kind = kind;
        s_st[s_st_n].arg = arg;
        s_st[s_st_n].arg2 = arg2;
        q = strchr(tok, '/'); /* W344: Zeitgrenze /N=J */
        s_st[s_st_n].to = (q && q < strrchr(tok, '>')) ? atoi(q + 1) : 0;
        q = q ? strchr(q, '=') : NULL;
        s_st[s_st_n].to_j = (q && q < strrchr(tok, '>')) ? atoi(q + 1) : 0;
        s_st_n++;
    }
}

static void lp_swap(void); /* W320 LOC-PERF */

/* W344: Menue-Zustand lesen (0 = nicht lesbar). mn[6] = Woerter des Menue-Unterzustands (0 ohne Zeiger). */
static int ls_state(u32 *scene, u32 *step, u32 *mn, u32 *s6)
{
    int ok = 0;
    __try {
        const u32 *m = *(const u32 *const *)(uintptr_t)LS_MENU;
        const u32 *q = *(const u32 *const *)(uintptr_t)LS_SCENE6;
        int i;
        *scene = *(const uint16_t *)(uintptr_t)LS_SCENE;
        *step = *(const uint16_t *)(uintptr_t)(LS_SCENE + 2);
        for (i = 0; i < 6; i++) mn[i] = m ? m[i] : 0;
        for (i = 0; i < 4; i++) s6[i] = q ? q[i] : 0;
        ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return ok;
}

/* W344: Zustandswechsel loggen, solange eine Testfolge laeuft (Szene/Schritt immer; mit loc_net_diag auch
 * Menue-/Szene-6-Woerter, Zaehler +1 nicht). Nur Spielzustand, keine Eingaben. Hoechstens 600 Zeilen. */
static void ls_state_log(unsigned f)
{
    static u32 prev[12];
    static int n;
    u32 sc = 0, stp = 0, mn[6], s6[4], cur[12];
    int i, diff = 0;
    if (n >= 600 || !ls_state(&sc, &stp, mn, s6)) return;
    cur[0] = sc;
    cur[1] = stp;
    for (i = 0; i < 6; i++) cur[2 + i] = g_cfg.loc_net_diag ? mn[i] : 0;
    for (i = 0; i < 4; i++) cur[8 + i] = g_cfg.loc_net_diag ? s6[i] : 0;
    for (i = 0; i < 12; i++) diff |= cur[i] != prev[i] && cur[i] != prev[i] + 1; /* Zaehler (+1 je Bild) nicht */
    memcpy(prev, cur, sizeof prev);
    if (!diff && f > 1) return;
    n++;
    HLOG_I("LOCSTATE: Bild %u Szene=%u.%u Menue [%X %X %X %X %X %X] Szene6 [%X %X %X %X]", f, sc, stp, cur[2], cur[3],
           cur[4], cur[5], cur[6], cur[7], cur[8], cur[9], cur[10], cur[11]);
}

void loc_sync_frame(unsigned f)
{
    lp_swap(); /* W320: Swap-Zeitpunkt (nur loc_pace_diag) */
    if (s_st_n) ls_state_log(f); /* W344 */
    if (s_frames != s_stl_frames) { /* W375: Bild des letzten neuen Lockstep-Rahmens (stl=N) */
        s_stl_frames = s_frames;
        s_stl_since = f;
    }
    if (s_st_n) { /* W375 TEST: Aktiv-Maske (Missionsgrenze) bei Aenderung, hoechstens 200 Zeilen */
        static u32 pm = 0xFFFFFFFFu;
        static int nm;
        u32 m = 0;
        __try { m = *(volatile const u32 *)(uintptr_t)LS_ACTIVE; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (m != pm && nm < 200) {
            nm++;
            HLOG_I("LOCMASK: Bild %u Maske 0x%X (Lockstep-Rahmen %ld)", f, m, s_frames);
        }
        pm = m;
    }
    while (s_st_cur < s_st_n) {
        int hit = 0;
        long acc = 0, con = 0;
        if (!s_st_since) s_st_since = f;
        if (!s_st_hit && (s_st[s_st_cur].kind == LST_J ||
                          (s_st[s_st_cur].to > 0 && s_st[s_st_cur].to_j > 0 && f - s_st_since >= (unsigned)s_st[s_st_cur].to))) {
            /* W344: Sprung bzw. Zeitgrenze (ohne Taste) */
            int to = s_st[s_st_cur].kind == LST_J ? (int)s_st[s_st_cur].arg : s_st[s_st_cur].to_j;
            if (to < 1 || to > s_st_n || ++s_st_jumps > 1000) {
                HLOG_W("LOCSTEPS: Sprung von Schritt %d nach %d ungueltig bzw. zu oft - Testfolge endet", s_st_cur + 1, to);
                s_st_cur = s_st_n;
                return;
            }
            HLOG_I("LOCSTEPS: %s Schritt %d -> %d bei Bild %u", s_st[s_st_cur].kind == LST_J ? "Sprung" : "Zeitgrenze",
                   s_st_cur + 1, to, f);
            s_st_cur = to - 1;
            s_st_hitf = f;
            s_st_since = f;
            return; /* naechster Schritt ab dem naechsten Bild */
        }
        if (!s_st_hit) {
            switch (s_st[s_st_cur].kind) {
            case LST_F: hit = 1; break;
            case LST_ACC: hit = loc_net_test_state(&acc, &con, 0, 256) && acc >= s_st[s_st_cur].arg; break;
            case LST_CON: hit = loc_net_test_state(&acc, &con, 0, 256) && con >= s_st[s_st_cur].arg; break;
            case LST_RX: hit = loc_net_test_state(NULL, NULL, 0, (unsigned)s_st[s_st_cur].arg) == 2; break;
            case LST_TX: hit = loc_net_test_state(NULL, NULL, 1, (unsigned)s_st[s_st_cur].arg) == 2; break;
            case LST_SYNC: hit = s_frames >= s_st[s_st_cur].arg; break;
            case LST_STL: hit = s_frames >= 100 && f - s_stl_since >= (unsigned)s_st[s_st_cur].arg; break; /* W375 */
            case LST_MSK: { /* W375 */
                u32 m = 0;
                __try { m = *(volatile const u32 *)(uintptr_t)LS_ACTIVE; } __except (EXCEPTION_EXECUTE_HANDLER) {}
                hit = (m != 0u) == (s_st[s_st_cur].arg != 0);
                break;
            }
            case LST_SCN:
            case LST_MN: { /* W344 */
                u32 sc = 0, stp = 0, mn[6], s6[4];
                if (ls_state(&sc, &stp, mn, s6)) {
                    if (s_st[s_st_cur].kind == LST_SCN)
                        hit = (long)sc == s_st[s_st_cur].arg && (s_st[s_st_cur].arg2 < 0 || (long)stp == s_st[s_st_cur].arg2);
                    else
                        hit = mn[s_st[s_st_cur].arg] == (u32)s_st[s_st_cur].arg2;
                }
                break;
            }
            }
            if (!hit) return;
            s_st_hit = 1;
            s_st_hitf = f;
            if (s_st[s_st_cur].kind == LST_STL) { /* W375: Mission vorbei - sync>=K der naechsten Runde zaehlt neu */
                int i;
                HLOG_I("LOCSYNC: Missionsende erkannt (seit %u Bildern kein Rahmen, %ld Rahmen) - Rahmenzaehler, Hash und "
                       "Testtasten neu", f - s_stl_since, s_frames);
                s_ls_missions++;
                InterlockedExchange(&s_frames, 0);
                s_stl_frames = 0;
                s_hash = 1469598103934665603ull;
                s_rt_cur = 0;
                for (i = 0; i < s_rp_n; i++) s_rp[i].next = s_rp[i].at;
                s_desync_logged = 0;
                loc_net_test_rearm(); /* rx:/tx:/acc>= der naechsten Runde */
                replay_release_all(); /* gehaltene Testtasten (Kippschalter) los */
            }
        }
        if (f < s_st_hitf + (unsigned)s_st[s_st_cur].delay) return;
        if (s_st[s_st_cur].vk == 0) { /* vk 0 = Bild anfordern (naechstes Present) */
            extern unsigned g_shot_census;
            g_shot_census = 1;
        } else if (s_st[s_st_cur].vk == 1) { /* vk 1 = DIAG Draw-Zensus (braucht m3d_census + loc_census_from) */
            extern volatile int g_fx_census_req;
            g_fx_census_req = 1;
        } else {
            replay_inject(s_st[s_st_cur].vk, s_st[s_st_cur].len);
        }
        HLOG_I("LOCSTEPS: Schritt %d/%d bei Bild %u (Lockstep-Rahmen %ld)", s_st_cur + 1, s_st_n, f, s_frames);
        s_st_cur++;
        s_st_hit = 0;
        s_st_hitf = f; /* f+N relativ zum vorigen Schritt */
        s_st_since = f; /* W344 */
    }
}

static unsigned long long fnv_u32(unsigned long long h, u32 v)
{
    int i;
    for (i = 0; i < 4; i++) {
        h ^= (v >> (8 * i)) & 0xFFu;
        h *= 1099511628211ull;
    }
    return h;
}

/* W322 LOC-NETLAT: Abstand der Lockstep-Rahmen (= neue Spielbilder, Soll 50 ms) als Histogramm in
 * 1-ms-Klassen, dazu Spanne der Eingabeverzoegerung. Nur Ganzzahlen (QPC), alle 10 s LOCLAT. */
#define NL_H 4001
static LONG s_nl_hist[NL_H];
static LONG s_nl_prev[NL_H];
static long long s_nl_last, s_nl_qpf;
static volatile LONG s_nl_max, s_nl_vmin = 0x7FFFFFFF, s_nl_vmax;
static u32 s_nl_stallf0;

static void nl_record(u32 delay)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    if (!s_nl_qpf) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        s_nl_qpf = f.QuadPart;
    }
    if (s_nl_last) {
        long long ms = (q.QuadPart - s_nl_last) * 1000 / s_nl_qpf;
        if (ms > s_nl_max) s_nl_max = (LONG)ms;
        if (ms >= NL_H) ms = NL_H - 1;
        InterlockedIncrement(&s_nl_hist[ms]);
    }
    s_nl_last = q.QuadPart;
    if ((LONG)delay < s_nl_vmin) s_nl_vmin = (LONG)delay;
    if ((LONG)delay > s_nl_vmax) s_nl_vmax = (LONG)delay;
}

static int nl_pct(const LONG *h, long long tot, int pct)
{
    long long acc = 0, need = (tot * pct + 99) / 100;
    int i;
    if (need < 1) need = 1;
    for (i = 0; i < NL_H; i++) {
        acc += h[i];
        if (acc >= need) return i;
    }
    return NL_H - 1;
}

unsigned loc_sync_hangs(void)
{
    unsigned n = 0;
    int i;
    for (i = 101; i < NL_H; i++) n += (unsigned)s_nl_hist[i];
    return n;
}

static void nl_log(int waited_s)
{
    static LONG win[NL_H];
    long long nw = 0, na = 0, wait = 0;
    int i, over = 0, overa = 0, over100 = 0;
    u32 stallf = 0;
    LONG mx = InterlockedExchange(&s_nl_max, 0), vmin = InterlockedExchange(&s_nl_vmin, 0x7FFFFFFF),
         vmax = InterlockedExchange(&s_nl_vmax, 0);
    for (i = 0; i < NL_H; i++) {
        LONG c = s_nl_hist[i];
        win[i] = c - s_nl_prev[i];
        s_nl_prev[i] = c;
        nw += win[i];
        na += c;
        if (i > 75) { over += win[i]; overa += c; }
        if (i > 100) over100 += win[i]; /* W324: Haenger > 100 ms */
        if (i > 50) wait += (long long)win[i] * (i - 50);
    }
    __try { stallf = *(const u32 *)(uintptr_t)LS_STALLF; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!nw) {
        return;
    }
    HLOG_I("LOCLAT: t=%ds Rahmen=%lld Abstand p50=%d p95=%d p99=%d max=%ld ms >75ms=%d Warten=%lld ms Verz=%ld..%ld "
           "Stauframes=+%u | gesamt Rahmen=%lld p50=%d p99=%d >75ms=%d | >100ms=%d",
           waited_s, nw, nl_pct(win, nw, 50), nl_pct(win, nw, 95), nl_pct(win, nw, 99), mx, over, wait,
           vmin == 0x7FFFFFFF ? 0 : vmin, vmax, stallf - s_nl_stallf0, na, nl_pct(s_nl_hist, na, 50),
           nl_pct(s_nl_hist, na, 99), overa, over100);
    s_nl_stallf0 = stallf;
}

/* cdecl, aus dem Thunk (alle Register gesichert). Nur Ganzzahlarithmetik: der x87-Stapel des
 * Spiels bleibt unberuehrt. */
/* W382 LOC-ISSUES-B: Spieltakt (Stuetzpunkte je Sekunde aus ls_record) fuer die LoC-Zustandszeile im Feedback. */
static loc_tickrate_t s_tr;
static SRWLOCK s_tr_lk = SRWLOCK_INIT;

/* W396: wirksame Eingabeverzoegerung fuer LOCSYNC "Wirk=" (Testpartner-Ergebnis): im Rollback die Verzoegerung
 * der F3-Zeile, im Lockstep der Vorlauf L (eigener Empfangsring, LOC_BOOT §69), sonst [0x306F04] wie bisher. */
static u32 ls_eff_delay(u32 reg)
{
    int l = 0, d = loc_rb_eff_delay();
    if (d > 0) return (u32)d;
    if (loc_lsarb_hud(NULL, &l, NULL) && l > 0) return (u32)l;
    return reg;
}

static void __cdecl ls_record(void)
{
    u32 acc = 0, tick = 0, stall = 0, stallf = 0, stalle = 0, delay = 0, desync = 0, npl = 0, act = 0;
    LONG n;
    __try {
        acc = *(const u32 *)(uintptr_t)LS_ACC;
        tick = *(const u32 *)(uintptr_t)LS_TICK;
        stall = *(const u32 *)(uintptr_t)LS_STALL;
        stallf = *(const u32 *)(uintptr_t)LS_STALLF;
        stalle = *(const u32 *)(uintptr_t)LS_STALLE;
        delay = *(const u32 *)(uintptr_t)LS_DELAY;
        desync = *(const u32 *)(uintptr_t)LS_DESYNC;
        npl = *(const u32 *)(uintptr_t)LS_NPL;
        act = *(const u32 *)(uintptr_t)LS_ACTIVE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (s_frames > 0 && tick + 100u < s_last_tick) { /* W375: neue Mission im selben Prozess (Tick wieder klein) */
        int i;
        s_ls_missions++;
        HLOG_I("LOCSYNC: neue Mission %u (Tick %u nach %u, %ld Rahmen) - Rahmenzaehler, Hash und Testtasten neu",
               s_ls_missions + 1, tick, s_last_tick, s_frames);
        InterlockedExchange(&s_frames, 0);
        s_hash = 1469598103934665603ull;
        s_rt_cur = 0;
        for (i = 0; i < s_rp_n; i++) s_rp[i].next = s_rp[i].at;
        s_desync_logged = 0;
    }
    n = InterlockedIncrement(&s_frames);
    nl_record(delay); /* W322 */
    loc_rb_hitch_frame(stallf); /* W469 */
    if (!loc_rb_resim()) { /* W382: Spieltakt fuer das Feedback-Paket (nur Ganzzahlen, kein x87 im Thunk) */
        LARGE_INTEGER q, f;
        QueryPerformanceCounter(&q);
        QueryPerformanceFrequency(&f);
        AcquireSRWLockExclusive(&s_tr_lk);
        loc_tickrate_push(&s_tr, q.QuadPart, tick, f.QuadPart);
        ReleaseSRWLockExclusive(&s_tr_lk);
    }
    if (!s_ext) s_hash = fnv_u32(fnv_u32(s_hash, acc), tick);
    s_last_tick = tick;
    while (s_rt_cur < s_rt_n && s_rt[s_rt_cur].at <= n) { /* Testtasten am Lockstep-Rahmen */
        replay_inject(s_rt[s_rt_cur].vk, s_rt[s_rt_cur].len);
        s_rt_cur++;
    }
    {
        int i;
        for (i = 0; i < s_rp_n; i++) { /* W336: wiederholte Testtasten */
            if (s_rp[i].next <= n) {
                replay_inject(s_rp[i].vk, s_rp[i].len);
                s_rp[i].next += s_rp[i].period * (1 + (n - s_rp[i].next) / s_rp[i].period);
            }
        }
    }
    if (g_cfg.loc_census_ticks[0]) { /* DIAG(W448): Draw-Zensus an Lockstep-Rahmen */
        static int s_ct_done;
        const char *p = g_cfg.loc_census_ticks;
        int idx = 0;
        while (*p) {
            long at = strtol(p, (char **)&p, 10);
            if (!(s_ct_done & (1 << idx)) && at > 0 && n >= at && idx < 30) {
                extern volatile int g_fx_census_req;
                s_ct_done |= 1 << idx;
                g_fx_census_req = 1;
                HLOG_W("LOCCENSUS: Zensus angefordert bei Rahmen %ld (Ziel %ld)", n, at);
                break;
            }
            while (*p == ',' || *p == ' ') { p++; }
            if (++idx >= 30) { break; }
        }
    }
    loc_dodge_frame(); /* W380 DIAG (ohne loc_dodge_diag ohne Wirkung) */
    if (desync && !s_desync_logged) {
        s_desync_logged = 1;
        HLOG_W("LOCSYNC: DESYNC vom Spiel erkannt bei Rahmen %ld Tick %u", n, tick);
    }
    if (!s_ext && g_cfg.loc_sync_every > 0 && n % g_cfg.loc_sync_every == 0) {
        HLOG_I("LOCSYNC: Rahmen=%ld Tick=%u Hash=%016llX Stau=%u Stauframes=%u Stauepisoden=%u Verz=%u "
               "Spieler=%u Maske=0x%X f=%u Wirk=%u",
               n, tick, s_hash, stall, stallf, stalle, delay, npl, act, hooks_frame_count(), ls_eff_delay(delay));
    }
}

void loc_sync_ext_on(int on) { s_ext = on; }

/* W338: endgueltiger Akkumulator von Slot 'slot' (Slot-Reihenfolge, Spiel-Thread am Ruhepunkt). */
void loc_sync_ext_frame(unsigned slot, unsigned acc, unsigned tick)
{
    u32 stall = 0, stallf = 0, stalle = 0, delay = 0, npl = 0, act = 0;
    long n = (long)slot + 1;
    if (!s_on || !s_ext) return;
    s_hash = fnv_u32(fnv_u32(s_hash, acc), tick);
    if (g_cfg.loc_sync_every > 0 && n % g_cfg.loc_sync_every == 0) {
        __try {
            stall = *(const u32 *)(uintptr_t)LS_STALL;
            stallf = *(const u32 *)(uintptr_t)LS_STALLF;
            stalle = *(const u32 *)(uintptr_t)LS_STALLE;
            delay = *(const u32 *)(uintptr_t)LS_DELAY;
            npl = *(const u32 *)(uintptr_t)LS_NPL;
            act = *(const u32 *)(uintptr_t)LS_ACTIVE;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        HLOG_I("LOCSYNC: Rahmen=%ld Tick=%u Hash=%016llX Stau=%u Stauframes=%u Stauepisoden=%u Verz=%u "
               "Spieler=%u Maske=0x%X f=%u Wirk=%u",
               n, tick, s_hash, stall, stallf, stalle, delay, npl, act, hooks_frame_count(), ls_eff_delay(delay));
    }
}

static __declspec(naked) void ls_thunk(void)
{
    __asm {
        pushad
        pushfd
        call ls_record
        popfd
        popad
        jmp dword ptr [s_jmp_target]
    }
}

int loc_sync_install(void)
{
    /* E8 rel32 -> 0xC9400, danach E8 rel32 -> 0x15004C (_ftol) */
    static const uint8_t expect[10] = {0xE8, 0x6A, 0xC0, 0x0A, 0x00, 0xE8, 0xB1, 0x2C, 0x13, 0x00};
    uint8_t *p = (uint8_t *)(uintptr_t)LS_CALL_VA;
    intptr_t rel;
    DWORD old;
    if (!g_cfg.loc_sync) {
        return 0;
    }
    if (memcmp(p, expect, sizeof expect) != 0) {
        HLOG_E("LOCBYTES LOCSYNC @0x%X: Byte-Mismatch - ABBRUCH (exit 7)", LS_CALL_VA);
        hlog_close();
        ExitProcess(7);
    }
    rel = (uint8_t *)(void *)ls_thunk - (p + 5);
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    s_on = 1;
    if (g_cfg.loc_steps[0]) {
        ls_st_parse(g_cfg.loc_steps);
        HLOG_I("LOCSTEPS: zustandsgesteuerte Testfolge, %d Schritte", s_st_n);
    }
    if (g_cfg.loc_replay_tick[0]) {
        ls_rt_parse(g_cfg.loc_replay_tick);
        HLOG_I("LOCSYNC: Test-Tastenfolge am Lockstep-Rahmen (loc_replay_tick, %d Slots)", s_rt_n);
        if (s_rp_n) {
            HLOG_I("LOCSYNC: davon %d wiederholte Testtasten (@P, W336)", s_rp_n);
        }
    }
    HLOG_I("LOCSYNC: aktiv (Zustandsakkumulator je Lockstep-Rahmen, Log alle %d Rahmen, nur Hash)",
           g_cfg.loc_sync_every);
    return 1;
}

static void lp_log(void); /* W320 LOC-PERF */

void loc_sync_tick(int waited_s)
{
    lp_log(); /* W320: LOCPACE alle 10 s (nur loc_pace_diag) */
    if (s_on) {
        nl_log(waited_s); /* W322: LOCLAT alle 10 s */
    }
    if (s_on && waited_s % 30 == 0) {
        HLOG_I("LOCSYNC: t=%ds Rahmen=%ld Tick=%u Hash=%016llX", waited_s, s_frames, s_last_tick, s_hash);
    }
    if (waited_s > 0 && waited_s % 60 == 0) { /* W382: Zustandszeile wie im Feedback-Paket (Spieltakt, Bild-Weg) */
        char st[240];
        loc_sync_state_line(st, sizeof st);
        HLOG_I("LOCGAME: t=%ds %s", waited_s, st);
    }
}

/* ---- W320 LOC-PERF: Bildtakt der Mission messen (loc_pace_diag) / Gegenprobe (loc_fields) ----
 *
 * Befund (LoC-VAs, nur gelesen): Die Missionsschleife (FUN_0002F100) endet je Bild mit
 * "push 3; call FUN_00033050" @0x2F2F7. FUN_00033050(N) pollt GetDisplayFieldStatus und kehrt erst
 * zurueck, wenn seit dem zuletzt gesehenen Feld N VBlank-Felder vergangen sind (dazwischen
 * SwitchToThread, je Feldwechsel FUN_00087A10/FUN_0006A730). Mission = 3 Felder = 20 Bilder/s bei
 * 60 Hz; Menue/Lobby (@0x3C193) = 2 Felder = 30 Bilder/s. Die Simulation rechnet einen Schritt je
 * Bild -> 20 Hz ist der Original-Takt der LoC-Mission, kein Leistungsengpass.
 *
 * loc_pace_diag=1 (DIAG, Standard 0): Der CALL @0x2F2F9 laeuft ueber einen Thunk, der Eintritt und
 * Ruecksprung per QPC stempelt (nur Ganzzahlen, x87 des Spiels unberuehrt). Alle 10 s eine Zeile
 * LOCPACE mit Warten/Arbeit/Periode/Feldphase/Swap-Abstand (p50/p99/max in ms).
 * loc_fields=N (TEST, Standard 0 = Original 3): Der Thunk uebergibt N statt 3. Das aendert die
 * Spielgeschwindigkeit (ein Tick je Bild), nur zur Messung; beide Clients brauchen denselben Wert. */
#define LP_CALL_VA 0x2F2F7u /* push 3; call FUN_00033050 */
#define LP_WAIT_FN 0x33050u
#define LP_N 4096
typedef struct {
    unsigned n, ns, nr;
    u32 wait[LP_N], work[LP_N], per[LP_N], phase[LP_N], soff[LP_N], sper[LP_N], pick[LP_N], rdur[LP_N];
} lp_win_t;
static lp_win_t s_lp[2];
static volatile LONG s_lp_cur;
static int s_lp_on;
static u32 s_lp_fn = LP_WAIT_FN;
static long long s_lp_qpf = 1, s_lp_enter, s_lp_leave, s_lp_swap;
static u32 s_lp_arg;
#define LP_RCALL_VA 0x2C59Bu /* Render-Thread (Start 0x2C580), Zustand [0x306EEC]=1: call FUN_00029320 */
static u32 s_lp_rfn = 0x29320u;
static long long s_lp_renter;

static long long lp_now(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}

static u32 lp_us(long long d)
{
    return d <= 0 ? 0u : (u32)(d * 1000000 / s_lp_qpf);
}

static void __cdecl lp_enter(void)
{
    lp_win_t *w = &s_lp[s_lp_cur & 1];
    s_lp_enter = lp_now();
    if (s_lp_on && s_lp_leave && w->n < LP_N) { w->work[w->n] = lp_us(s_lp_enter - s_lp_leave); }
}

static void __cdecl lp_leave(void)
{
    lp_win_t *w = &s_lp[s_lp_cur & 1];
    long long t = lp_now();
    unsigned hz = g_cfg.vblank_hz > 0 ? (unsigned)g_cfg.vblank_hz : 60u;
    if (s_lp_on && s_lp_leave && w->n < LP_N) {
        unsigned long long qh = (unsigned long long)t * hz;
        w->wait[w->n] = lp_us(t - s_lp_enter);
        w->per[w->n] = lp_us(t - s_lp_leave);
        w->phase[w->n] = (u32)((qh % (unsigned long long)s_lp_qpf) * 1000000ull / ((unsigned long long)s_lp_qpf * hz));
        w->n++;
    }
    s_lp_leave = t;
}

static void lp_swap(void)
{
    lp_win_t *w;
    long long t;
    if (!s_lp_on) return;
    w = &s_lp[s_lp_cur & 1];
    t = lp_now();
    if (s_lp_leave && s_lp_swap && w->ns < LP_N) {
        w->soff[w->ns] = lp_us(t - s_lp_leave); /* Swap nach Ende des Feld-Wartens */
        w->sper[w->ns] = lp_us(t - s_lp_swap);
        w->ns++;
    }
    s_lp_swap = t;
}

static void __cdecl lp_renter(void)
{
    lp_win_t *w = &s_lp[s_lp_cur & 1];
    s_lp_renter = lp_now();
    if (s_lp_leave && w->nr < LP_N) { w->pick[w->nr] = lp_us(s_lp_renter - s_lp_leave); } /* Abholung nach Takt-Ende */
}

static void __cdecl lp_rleave(void)
{
    lp_win_t *w = &s_lp[s_lp_cur & 1];
    if (s_lp_leave && w->nr < LP_N) { w->rdur[w->nr] = lp_us(lp_now() - s_lp_renter); w->nr++; }
}

static __declspec(naked) void lp_rthunk(void)
{
    __asm {
        pushad
        pushfd
        call lp_renter
        popfd
        popad
        call dword ptr [s_lp_rfn]
        pushad
        pushfd
        call lp_rleave
        popfd
        popad
        ret
    }
}

static __declspec(naked) void lp_thunk(void)
{
    __asm {
        pushad
        pushfd
        call lp_enter
        popfd
        popad
        mov eax, [esp + 4]
        cmp dword ptr [s_lp_arg], 0
        je lp_k
        mov eax, dword ptr [s_lp_arg]
    lp_k:
        push eax
        call dword ptr [s_lp_fn]
        add esp, 4
        pushad
        pushfd
        call lp_leave
        popfd
        popad
        ret
    }
}

int loc_pace_install(void)
{
    static const uint8_t expect[7] = {0x6A, 0x03, 0xE8, 0x52, 0x3D, 0x00, 0x00}; /* push 3; call 0x33050 */
    uint8_t *p = (uint8_t *)(uintptr_t)LP_CALL_VA;
    intptr_t rel;
    DWORD old;
    LARGE_INTEGER f;
    if (!g_cfg.loc_pace_diag && g_cfg.loc_fields <= 0) {
        return 0;
    }
    if (memcmp(p, expect, sizeof expect) != 0) {
        HLOG_E("LOCBYTES LOCPACE @0x%X: Byte-Mismatch - ABBRUCH (exit 7)", LP_CALL_VA);
        hlog_close();
        ExitProcess(7);
    }
    QueryPerformanceFrequency(&f);
    s_lp_qpf = f.QuadPart ? f.QuadPart : 1;
    s_lp_arg = (g_cfg.loc_fields > 0 && g_cfg.loc_fields <= 6) ? (u32)g_cfg.loc_fields : 0u;
    rel = (uint8_t *)(void *)lp_thunk - (p + 7);
    VirtualProtect(p, 7, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p + 3, &rel, 4);
    VirtualProtect(p, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 7);
    s_lp_on = g_cfg.loc_pace_diag ? 1 : 0;
    if (s_lp_on) {
        static const uint8_t rexp[5] = {0xE8, 0x80, 0xCD, 0xFF, 0xFF}; /* call 0x29320 */
        uint8_t *r = (uint8_t *)(uintptr_t)LP_RCALL_VA;
        if (memcmp(r, rexp, sizeof rexp) != 0) {
            HLOG_E("LOCBYTES LOCPACE @0x%X: Byte-Mismatch - ABBRUCH (exit 7)", LP_RCALL_VA);
            hlog_close();
            ExitProcess(7);
        }
        rel = (uint8_t *)(void *)lp_rthunk - (r + 5);
        VirtualProtect(r, 5, PAGE_EXECUTE_READWRITE, &old);
        memcpy(r + 1, &rel, 4);
        VirtualProtect(r, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), r, 5);
    }
    HLOG_I("LOCPACE: Missionstakt FUN_00033050 @0x%X (Diag=%d), Felder je Bild %s%u", LP_CALL_VA, s_lp_on,
           s_lp_arg ? "TEST loc_fields=" : "Original ", s_lp_arg ? s_lp_arg : 3u);
    return 1;
}

static int lp_cmp(const void *a, const void *b)
{
    u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return x < y ? -1 : x > y;
}

static void lp_q(u32 *v, unsigned n, double *o)
{
    if (!n) { o[0] = o[1] = o[2] = 0; return; }
    qsort(v, n, sizeof *v, lp_cmp);
    o[0] = v[n / 2] / 1000.0;
    o[1] = v[(n * 99u) / 100u] / 1000.0;
    o[2] = v[n - 1] / 1000.0;
}

static void lp_log(void)
{
    lp_win_t *w;
    double a[24];
    unsigned n, ns, nr, i, j55 = 0;
    if (!s_lp_on) return;
    w = &s_lp[s_lp_cur & 1];
    InterlockedExchange(&s_lp_cur, (s_lp_cur + 1) & 1); /* Spiel schreibt ab jetzt ins andere Fenster */
    Sleep(5);
    n = w->n;
    ns = w->ns;
    nr = w->nr;
    if (n) {
        for (i = 0; i < ns; i++) { if (w->sper[i] > 55000u) j55++; }
        lp_q(w->per, n, a + 0);
        lp_q(w->wait, n, a + 3);
        lp_q(w->work, n, a + 6);
        lp_q(w->phase, n, a + 9);
        lp_q(w->soff, ns, a + 12);
        lp_q(w->sper, ns, a + 15);
        HLOG_I("LOCPACE: n=%u Felder=%u | Takt-Periode p50/p99/max=%.2f/%.2f/%.2f ms | Warten=%.2f/%.2f/%.2f | "
               "Arbeit=%.2f/%.2f/%.2f | Feldphase=%.2f/%.2f/%.2f | Swap nach Warten=%.2f/%.2f/%.2f | "
               "Swap-Periode=%.2f/%.2f/%.2f (>55ms: %u von %u)",
               n, s_lp_arg ? s_lp_arg : 3u, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11],
               a[12], a[13], a[14], a[15], a[16], a[17], j55, ns);
        lp_q(w->pick, nr, a + 18);
        lp_q(w->rdur, nr, a + 21);
        HLOG_I("LOCPACE: Render-Thread n=%u | Abholung nach Takt-Ende p50/p99/max=%.2f/%.2f/%.2f ms | Renderdauer=%.2f/%.2f/%.2f",
               nr, a[18], a[19], a[20], a[21], a[22], a[23]);
    }
    w->n = 0;
    w->ns = 0;
    w->nr = 0;
}

/* W382 LOC-ISSUES-B: Zustandszeile fuer Feedback-/Issue-Pakete (beliebiger Thread). */
int loc_sync_state_line(char *out, size_t cap)
{
    loc_state_t st;
    memset(&st, 0, sizeof st);
    __try {
        st.lockstep = *(const u32 *)(uintptr_t)LS_ACTIVE != 0u;
        st.tick = *(const u32 *)(uintptr_t)LS_TICK;
        st.scene = *(const uint16_t *)(uintptr_t)LS_SCENE;
        st.step = *(const uint16_t *)(uintptr_t)(LS_SCENE + 2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    st.frame = s_frames;
    AcquireSRWLockShared(&s_tr_lk);
    st.hz = loc_tickrate_hz(&s_tr, 60.0, &st.span_s);
    ReleaseSRWLockShared(&s_tr_lk);
    st.interp_fps = g_cfg.interp_fps;
    st.render_thread = g_cfg.render_thread;
    st.vblank_hz = g_cfg.vblank_hz;
    return loc_state_line(out, cap, &st);
}
