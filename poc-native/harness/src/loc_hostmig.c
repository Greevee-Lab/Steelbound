/* loc_hostmig.c - W520 HOSTMIG (GitHub #123, LOC_BOOT §96): Host-Wechsel der LoC-Lobby im Spiel.
 *
 * Der Dienst (steelnet) waehlt bei Weggang des Hosts den stabilsten Spieler als neuen Host und behaelt die Lobby
 * (Raumcode, Sichtbarkeit, Modus, Mitspieler-Verbindungen). Das Spiel selbst kennt keine Host-Migration: verliert ein
 * Client in der Lobby den Host, geht der Lobby-Automat in den Zustand 0x31 („expelled“, LOC_RE1 §6.3, FUN_000511E0).
 * Dieses Modul baut die Spiel-Session danach selbst neu auf, ohne Neustart und ohne Eingabe des Spielers:
 *   - neuer Host: Meldung bestaetigen, Create Match, Mission, CREATE (wie die Testfolge W344 „Ersteller“),
 *   - uebrige: Meldung bestaetigen, Search Match, Eintrag waehlen, Seite, beitreten (wie „Mitspieler“), mit
 *     Wiederholung, bis der neue Host sein Match anbietet.
 * Tasten gehen nur an das virtuelle Menue-Pad (loc_pad.c, unabhaengig von der Tastenbelegung), echte Eingaben bleiben
 * aktiv. Im Match (Zustand 0x4E) passiert nichts: der Host hat dort keine Spielrolle (BERICHT_M2 „W520“ §Stufe 2); die
 * Mission laeuft fuer die uebrigen weiter, der Neuaufbau folgt nach dem Match.
 * Zusaetzlich: Hinweis „<Name> left – <Name> is now the host“ (de/en) und host_gone an den Dienst, sobald das Spiel den
 * Host verloren hat (beschleunigt die Wahl bei Absturz/Spielende ohne Abmeldung). Keine Eingaben im Log.
 * W523 HOSTMIG-SCOPE (0.6.3, Nutzerentscheid 26.11.): Neuaufbau ueber die Menues und Auto-Host nur noch mit dem
 * internen Schalter loc_hostmig_rebuild=1 (TEST, Grundlage fuer 0.7.0 HOSTMIG-MEM). Standard: Wechsel im Match ->
 * Mission laeuft weiter, nach der Mission Hinweis auf den neuen Host; Wechsel in der Lobby -> Hinweis, das Spiel zeigt
 * seine Meldung (0x31), Match von Hand neu (Lobby, Raumcode und Verbindungen bleiben bei steelnet). */
#include "loc_hostmig.h"
#include "loc_hostmig_prog.h" /* Ablaufe + Zerleger (rein, auch im Selbsttest) */
#include "loc_net.h"
#include "hcfg.h"
#include "hlog.h"
#include "sb_lang.h"
#include "sb_origview.h"
#include "loc_sync.h" /* TEST: Fortsetzung der Testfolge */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define HM_MAX 48
#define HM_LS_EXPELLED 0x31 /* Client hat den Host verloren (Meldung, wartet auf A) */
#define HM_LS_INGAME 0x4E

static hm_step s_st[HM_MAX];
static int s_n, s_cur, s_hit, s_jumps, s_run;
static unsigned s_hitf, s_since, s_t0;
static volatile LONG s_bits;
static unsigned s_rel_at; /* Bild, ab dem die Tasten wieder los sind */
static int s_pend;        /* 1 = Ereignis erhalten, Neuaufbau wartet auf Zustand 0x31 */
static unsigned s_pend_at;
static int s_pend_new;    /* dieser Client ist der neue Host */
static int s_prev_ls = -1;
static unsigned s_ho_since; /* Auto-Host: Bild, seit dem alle Mitglieder im Match des Hosts stehen */
static int s_ho_sent;
static int s_hint_pend;     /* W523: HM_ACT_HINT_AFTER/_31 - Hinweis „von Hand weiter“ nach der Mission bzw. zur Meldung 0x31 */
static unsigned s_hint_at;
static int s_hint_new;
static char s_hint_host[24];
static int s_watch;         /* W523: Lobby-Zustaende nach einem Wechsel protokollieren (Diagnose, keine Eingaben) */
static int s_gone_skip;     /* W523: naechstes 0x31 folgt aus dem gemeldeten Wechsel - kein host_gone gegen den neuen Host */

static int hm_lstate(void)
{
    int v = -1;
    __try {
        const unsigned char *g = *(const unsigned char *const *)(uintptr_t)0x361708u;
        if (g) v = g[0x15];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v = -1;
    }
    return v;
}

static int hm_menu0(unsigned *w)
{
    int ok = 0;
    __try {
        const unsigned *m = *(const unsigned *const *)(uintptr_t)0x6DC678u; /* LOC_BOOT §30.1 */
        *w = m ? m[0] : 0xFFFFFFFFu;
        ok = m != NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return ok;
}

/* Vorlage mit bis zu zwei %s (Namen) fuellen. */
static void hm_fill(const wchar_t *tpl, const char *a, const char *b, wchar_t *out, int cap)
{
    const char *nm[2] = {a, b};
    int k = 0, o = 0, i;
    for (i = 0; tpl[i] && o < cap - 1; i++) {
        if (tpl[i] == L'%' && tpl[i + 1] == L's' && k < 2) {
            const char *s = nm[k++] ? nm[k - 1] : "";
            while (*s && o < cap - 1) out[o++] = (wchar_t)(unsigned char)*s++;
            i++;
            continue;
        }
        out[o++] = tpl[i];
    }
    out[o] = 0;
}

static void hm_toast(const char *kt, const char *kd, const char *a, const char *b, unsigned ms)
{
    wchar_t t1[160], t2[240], f1[200];
    sb_lang_w(kt, t1, 160);
    sb_lang_w(kd, t2, 240);
    hm_fill(t1, a, b, f1, 200);
    sb_ov_toast_msg(f1, t2, ms);
}

/* host: 1 = neuer Host, 0 = Mitspieler, 3 = abgebender Host (Auto-Host) */
static void hm_start(int host, unsigned f)
{
    const char *p = host == 3 ? HM_PROG_LEAVE : host ? HM_PROG_HOST : HM_PROG_JOIN;
    if (host == 1 && g_cfg.loc_hostmig_prog_host[0]) p = g_cfg.loc_hostmig_prog_host; /* TEST: anderer Ablauf */
    if (!host && g_cfg.loc_hostmig_prog_join[0]) p = g_cfg.loc_hostmig_prog_join;
    s_n = hm_parse(p, s_st, HM_MAX);
    if (s_n <= 0) {
        HLOG_W("LOCHOSTMIG: Ablauf ungueltig - kein Neuaufbau");
        s_n = 0;
        return;
    }
    s_cur = s_hit = s_jumps = 0;
    s_since = s_hitf = s_t0 = f;
    s_run = host == 3 ? 3 : host ? 1 : 2;
    loc_net_test_rearm(); /* rx:/tx: ab jetzt (alte Suche zaehlt nicht) */
    HLOG_I("LOCHOSTMIG: Neuaufbau der Spiel-Session gestartet (%s, %d Schritte, Bild %u)",
           host == 3 ? "Auto-Host: eigenes Match verlassen, beim neuen Host beitreten" :
           host ? "neuer Host: Match erstellen" : "Mitspieler: neuen Host suchen und beitreten", s_n, f);
}

static void hm_end(int ok, unsigned f, const char *why)
{
    int role = s_run;
    HLOG_I("LOCHOSTMIG: Neuaufbau %s nach %u Bildern (%s)", ok ? "fertig" : "gescheitert", f - s_t0, why);
    s_run = 0;
    s_n = 0;
    InterlockedExchange(&s_bits, 0);
    if (!ok) hm_toast("loc.hostmig_fail_t", "loc.hostmig_fail_d", NULL, NULL, 12000u);
    else hm_toast("loc.hostmig_ok_t", "loc.hostmig_ok_d", NULL, NULL, 6000u);
    if (ok && g_cfg.loc_hostmig_test) { /* TEST: Testfolge fortsetzen (naechstes Match bis zum Debriefing) */
        const char *c = role == 1 ? g_cfg.loc_hostmig_cont_host : g_cfg.loc_hostmig_cont_join;
        if (c[0]) loc_sync_steps_replace(c);
    }
}

static void hm_step_run(unsigned f, int ls)
{
    if (s_rel_at && f >= s_rel_at) {
        InterlockedExchange(&s_bits, 0);
        s_rel_at = 0;
    }
    while (s_run && s_cur < s_n) {
        hm_step *st = &s_st[s_cur];
        int hit = 0;
        if (!s_hit && (st->kind == HM_K_J || (st->to > 0 && st->to_j > 0 && f - s_since >= (unsigned)st->to))) {
            int to = st->kind == HM_K_J ? st->arg : st->to_j;
            if (to == 99 || to < 1 || to > s_n || ++s_jumps > 40) {
                hm_end(0, f, to == 99 ? "Zeitgrenze" : "zu viele Wiederholungen");
                return;
            }
            s_cur = to - 1;
            s_since = s_hitf = f;
            return;
        }
        if (!s_hit) {
            long acc = 0, con = 0;
            switch (st->kind) {
            case HM_K_F: hit = 1; break;
            case HM_K_LS: hit = ls == st->arg; break;
            case HM_K_LSN: hit = ls >= 0 && ls != st->arg; break;
            case HM_K_RX: hit = loc_net_test_state(&acc, &con, 0, (unsigned)st->arg) == 2; break;
            case HM_K_TX: hit = loc_net_test_state(&acc, &con, 1, (unsigned)st->arg) == 2; break;
            case HM_K_MN0: {
                unsigned w = 0;
                hit = hm_menu0(&w) && w == (unsigned)st->arg;
                break;
            }
            }
            if (!hit) return;
            s_hit = 1;
            s_hitf = f;
        }
        if (f < s_hitf + (unsigned)st->delay) return;
        if (st->btn == HM_B_SHOT) {
            extern unsigned g_shot_census;
            if (g_cfg.loc_hostmig_test) g_shot_census = 1;
        } else if (st->btn) {
            InterlockedExchange(&s_bits, (LONG)st->btn);
            s_rel_at = f + (unsigned)st->len;
        }
        if (g_cfg.loc_net_diag) HLOG_I("LOCHOSTMIG: Schritt %d/%d bei Bild %u (Zustand 0x%02X)", s_cur + 1, s_n, f, ls & 0xFF);
        s_cur++;
        s_hit = 0;
        s_hitf = s_since = f;
    }
    if (s_run && s_cur >= s_n) hm_end(1, f, s_run == 1 ? "Match angeboten" : "beim neuen Host in der Lobby");
}

void loc_hostmig_frame(unsigned f)
{
    loc_hostmig_ev ev;
    int ls;
    if (!loc_net_online()) return;
    ls = hm_lstate();
    if (ls != s_prev_ls) {
        /* Dienst entscheidet; zaehlt nur, wenn auch er den Host verlor. Nicht nach einem schon gemeldeten Wechsel (dann
         * ist der Host des Transports bereits der neue). */
        if (ls == HM_LS_EXPELLED && !s_pend && !s_run && !s_gone_skip) loc_net_host_gone();
        /* W523: 0x31 als Folge eines schon gemeldeten Wechsels zaehlt nicht gegen den neuen Host (einmal); eine neue
         * Session (Host bietet an / beigetreten) beendet die Ausnahme */
        if (ls == HM_LS_EXPELLED || ls == 0x22 || ls == 0x23 || ls == 0x2E) s_gone_skip = 0;
        if (s_watch > 0) {
            s_watch--;
            HLOG_I("LOCHOSTMIG: Lobby-Zustand 0x%02X nach dem Host-Wechsel (Bild %u)", ls & 0xFF, f);
        }
        s_prev_ls = ls;
    }
    /* W523 HOSTMIG-SCOPE (0.6.3): ohne loc_hostmig_rebuild kein Neuaufbau ueber die Menues. Wechsel im Match: die Mission
     * laeuft weiter; verlaesst das Spiel danach die Mission, nennt ein Hinweis den neuen Host (Match von Hand erstellen bzw.
     * suchen, wie zwischen zwei Runden ueblich; Lobby, Raumcode und Verbindungen bleiben bei steelnet). */
    if (s_hint_pend == HM_ACT_HINT_31 && f - s_hint_at > 60u * 60u) s_hint_pend = 0; /* kein 0x31 binnen 60 s */
    if ((s_hint_pend == HM_ACT_HINT_AFTER && ls >= 0 && ls < HM_LS_INGAME) || (s_hint_pend == HM_ACT_HINT_31 && ls == HM_LS_EXPELLED)) {
        hm_toast("loc.hostmig_now_t", s_hint_new ? "loc.hostmig_hand_new_d" : "loc.hostmig_hand_d", s_hint_host, NULL, 15000u);
        HLOG_I("LOCHOSTMIG: %s (Lobby-Zustand 0x%02X) - Hinweis: Match von Hand %s (kein Neuaufbau, Bild %u)",
               s_hint_pend == HM_ACT_HINT_31 ? "Meldung des Spiels" : "Mission verlassen", ls & 0xFF,
               s_hint_new ? "erstellen" : "suchen", f);
        s_hint_pend = 0;
    }
    if (loc_net_hostmig_poll(&ev)) {
        const char *kt = ev.reason == 3 ? "loc.hostmig_auto_t" : ev.reason == 2 ? "loc.hostmig_lost_t" : "loc.hostmig_left_t";
        int rb = g_cfg.loc_hostmig_rebuild;
        const char *kd = "";
        int act = hm_decide(rb, ls, ev.me_new, ev.me_old, ev.reason, s_run, &kd); /* W523 (loc_hostmig_prog.h) */
        if (ev.reason == 3) hm_toast(kt, kd, ev.host, NULL, 9000u);
        else hm_toast(kt, kd, ev.old, ev.host, rb || ls >= HM_LS_INGAME ? 9000u : 15000u);
        HLOG_I("LOCHOSTMIG: Host-Wechsel erhalten (%s, %s, Lobby-Zustand 0x%02X)%s",
               ev.reason == 3 ? "Auto-Host" : ev.reason == 2 ? "Host verloren" : "Host gegangen",
               ev.me_new ? "neuer Host" : ev.me_old ? "bisheriger Host" : "Mitspieler", ls & 0xFF,
               rb ? "" : ls >= HM_LS_INGAME ? " - Mission laeuft weiter, kein Neuaufbau" : " - kein Neuaufbau (von Hand weiter)");
        s_ho_sent = 0;
        s_watch = 24;
        s_hint_pend = 0;
        s_gone_skip = ls != HM_LS_EXPELLED;
        if (act == HM_ACT_HINT_AFTER || act == HM_ACT_HINT_31) { /* W523: Hinweis nach der Mission bzw. zur Meldung 0x31 */
            s_hint_pend = act;
            s_hint_at = f;
            s_hint_new = ev.me_new;
            strcpy_s(s_hint_host, sizeof s_hint_host, ev.host);
        } else if (act == HM_ACT_LEAVE) {
            hm_start(3, f); /* Auto-Host: eigenes Match verlassen, die anderen folgen ueber 0x31 */
        } else if (act == HM_ACT_PEND) {
            s_pend = 1;
            s_pend_at = f;
            s_pend_new = ev.me_new;
        }
    }
    if (s_pend && !s_run) {
        if (ls == HM_LS_EXPELLED) {
            s_pend = 0;
            hm_start(s_pend_new, f);
        } else if (ls >= HM_LS_INGAME) {
            s_pend_at = f; /* im Match bzw. danach (0x4F/0x50 Ergebnis, Garage): Neuaufbau, sobald die Lobby fehlt */
        } else if (f - s_pend_at > 60u * 60u) {
            s_pend = 0; /* 60 s ohne Host-Verlust im Spiel: Session lief weiter (z. B. Host war im Spiel schon weg) */
            HLOG_I("LOCHOSTMIG: kein Host-Verlust im Spiel binnen 60 s - kein Neuaufbau noetig");
        }
    }
    /* W520 Auto-Host: als Host einer Auto-Host-Lobby, Match angeboten (0x22/0x23) und alle Mitglieder der steelnet-Lobby
     * stehen im Match (Spieler je Seite G+0xA4/A5) seit 5 s -> Uebergabe anfragen (der Dienst waehlt und erlaubt das nur
     * einmal je Lobby; bleibt der Host der stabilste, aendert sich nichts). */
    if (g_cfg.loc_hostmig_rebuild && /* W523: Auto-Host nur TEST/0.7.0 */
        !s_run && !s_pend && !s_ho_sent && (ls == 0x22 || ls == 0x23) && (f & 15u) == 0) { /* Lobby-Einstellung zaehlt */
        int mig = 0, autoh = 0, sme = 0, amh = 0, nmem = 0, pl = 0;
        if (loc_net_hostmig_info(&mig, &autoh, &sme, &amh) && autoh && amh) {
            nmem = loc_net_hostmig_members();
            __try {
                const unsigned char *g = *(const unsigned char *const *)(uintptr_t)0x361708u;
                if (g) pl = (int)(signed char)g[0xA4] + (int)(signed char)g[0xA5];
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                pl = 0;
            }
        }
        if (nmem >= 2 && pl >= nmem) {
            if (!s_ho_since) s_ho_since = f;
            else if (f - s_ho_since >= 300u) {
                s_ho_sent = 1;
                loc_net_handover();
                HLOG_I("LOCHOSTMIG: Auto-Host - alle %d Spieler im Match, Uebergabe angefragt (Bild %u)", nmem, f);
            }
        } else {
            s_ho_since = 0;
        }
    }
    if (s_run) hm_step_run(f, ls);
}

unsigned loc_hostmig_pad_bits(void)
{
    return (unsigned)InterlockedCompareExchange(&s_bits, 0, 0);
}
