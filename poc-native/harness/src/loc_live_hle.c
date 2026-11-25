/* loc_live_hle.c - W479 LIVE-LOGON: XOnline-HLE fuer den Xbox-Live-Modus von Line of Contact.
 *
 * Nur mit loc_live=1 (Standard 0): die 39 XOnline-Thunks des Spiels ("mov ecx,[0x73AE78]; jmp CXo::...",
 * LOC_HOOKS.tsv, Status loc-hle) werden per 5-Byte-JMP auf eigene stdcall-Handler umgelenkt (8 Originalbytes
 * je Einstieg verglichen, sonst exit 7). Ohne loc_live bleiben sie unberuehrt (System Link bitgleich).
 * steelnet antwortet anstelle von Xbox Live: Logon = steelnet-Kontoanmeldung des eigenen Transports
 * (loc_tun, Launcher-Spielschluessel), Gamertag = Pilotname, XUID = Pseudo-XUID aus der Konto-ID (loc_live.c).
 * Argumentzahlen = "ret N"/4 der CXo-Methoden bzw. Thunks (W479-Analyse). Keine Inhalte (Namen, Feedback,
 * Presence-Daten) im Log, nur Zustaende und Zahlen. Doku: docs/loc/LOC_LIVE.md.
 * W482 LIVE-MATCH: Sessions/Suche/Find ueber steelnet (Transport loc_tun_live.c), XNKID/XNKEY vom Dienst. */
#include "loc_live.h"
#include "loc_live_hle.h"
#include "loc_tun.h"
#include "loc_camp_bridge.h" /* W491 CAMP-CLIENT */
#include "loc_net_core.h"
#include "hcfg.h"
#include "hlog.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>

#include <stdio.h>

#define TUN_LIVE_JSON_HLE 3800 /* W482: Anfrage-JSON (Transport nimmt < 3900, steelnet-Zeile <= 4096) */

typedef uint32_t u32;

static loctun *s_tun;
static int s_on;
static volatile LONG s_flow;  /* W501 ONLINE-BOTH: Spiel ist im Xbox-Live-Fluss (Kontenauswahl/Logon bis Logon-Task zu) */
static volatile LONG s_camp_tried; /* W501: Kampagnen-Bruecke erst beim ersten Betreten von Xbox Live */
static volatile LONG s_tcont[LLT_KINDS], s_tclose[LLT_KINDS]; /* W501 DIAG: TaskContinue/TaskClose je Task-Art (Zahlen) */
static LONG s_tlog[2 * LLT_KINDS];
static void ll_flow_enter(const char *via);
static void ll_flow_leave(const char *why);
static volatile LONG s_first[48];
static loclive_task s_task[64];
static uint8_t s_logon[LOCLIVE_MAX_LOGON * LOCLIVE_USER_SIZE]; /* XOnlineGetLogonUsers */
static uint8_t s_nores[0x60];                                 /* leere Suchergebnisliste */
static int s_pad = -1;                                        /* angemeldeter Controller-Platz */
static char s_tag[16];                                        /* fester Gamertag dieser Sitzung */

static void ll_first(int id, const char *name, u32 ra)
{
    if (id >= 0 && id < 48 && InterlockedExchange(&s_first[id], 1) == 0) {
        HLOG_I("LOCLIVE: %s erster Aufruf (Aufrufer 0x%X)", name, ra);
    }
}
#define FIRST(id, nm) ll_first(id, nm, (u32)(uintptr_t)_ReturnAddress())

static int ll_put(u32 dst, const void *src, unsigned n)
{
    if (!dst || !src || !n) return 0;
    __try {
        memcpy((void *)(uintptr_t)dst, src, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 1;
}
static int ll_get(void *dst, u32 src, unsigned n)
{
    if (!dst || !src || !n) return 0;
    __try {
        memcpy(dst, (const void *)(uintptr_t)src, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 1;
}

static void ll_net_poll(loclive_task *t); /* W482 */

/* ---------- Tasks ---------- */
static loclive_task *ll_task_new(int kind)
{
    int i;
    for (i = 0; i < (int)(sizeof s_task / sizeof s_task[0]); i++) {
        if (s_task[i].magic != LOCLIVE_TASK_MAGIC) {
            memset(&s_task[i], 0, sizeof s_task[i]);
            s_task[i].magic = LOCLIVE_TASK_MAGIC;
            s_task[i].kind = kind;
            s_task[i].start_ms = GetTickCount();
            return &s_task[i];
        }
    }
    HLOG_W("LOCLIVE: keine freie Task (64 offen)");
    return NULL;
}
static loclive_task *ll_task_of(u32 h)
{
    uintptr_t p = (uintptr_t)h, b = (uintptr_t)&s_task[0];
    if (p < b || p >= b + sizeof s_task || (p - b) % sizeof(loclive_task)) return NULL;
    return ((loclive_task *)p)->magic == LOCLIVE_TASK_MAGIC ? (loclive_task *)p : NULL;
}
/* neue Task anlegen und den Griff nach *ph schreiben; Rueckgabe HRESULT */
static u32 ll_task_out(int kind, u32 ph)
{
    loclive_task *t;
    u32 h;
    if (!ph) return LOCLIVE_E_INVALIDARG;
    t = ll_task_new(kind);
    if (!t) return LOCLIVE_E_FAIL;
    h = (u32)(uintptr_t)t;
    if (!ll_put(ph, &h, 4)) {
        t->magic = 0;
        return LOCLIVE_E_INVALIDARG;
    }
    return LOCLIVE_S_OK;
}

/* ---------- Identitaet ---------- */
static void ll_pin_path(char *p, size_t cap)
{
    size_t n;
    strcpy_s(p, cap, g_cfg.hdd_root);
    n = strlen(p);
    while (n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\')) p[--n] = 0;
    strcat_s(p, cap, "/steelbound_live.txt");
}
/* steelnet-Anmeldung: 2 angenommen, 1 laeuft, 0 kein Konto (Gast/kein Treffpunkt), -1 abgelehnt */
static int ll_auth(uint32_t *acct)
{
    uint32_t a = s_tun ? loctun_account(s_tun) : 0;
    int st = s_tun ? loctun_auth_state(s_tun) : 0;
    if (acct) *acct = a;
    if (!a && st != -1) st = 0;
    return st;
}
/* W487 LIVE-GAMERTAG: Gamertag des Kontos aus dem welcome (steelcore); 1 = vorhanden und gueltig */
static int ll_gamertag(char out[16])
{
    char g[24] = "";
    out[0] = 0;
    if (!s_tun || !loctun_my_gamertag(s_tun, g, (int)sizeof g) || !loclive_tag_valid(g)) return 0;
    strcpy_s(out, 16, g);
    return 1;
}

/* Ordner T:\<from>CAMP/FREE -> T:\<to>... (Spielpfad ueber krnl_host_path: Ersatzordner, Spielstand-Schutz) */
extern int krnl_host_path(const char *xb, char *out, size_t cap); /* krnl_file.c */
static int ll_move_folders(const char *from, const char *to)
{
    char tdir[700];
    if (krnl_host_path("T:\\", tdir, sizeof tdir) != 0) return -1;
    return loclive_move_folders(tdir, from, to);
}

/* Gamertag fuer die Kontenliste (final=0, ohne Dateizugriff ausser Lesen) bzw. beim Logon (final=1): Gamertag
 * aus dem Konto; bei einem Wechsel ziehen die Pilotordner einmalig auf den neuen Tag um (LOC_LIVE.md §4.3).
 * Rueckgabe 1 = Gamertag da, 0 = keiner (Logon wird abgelehnt). */
static int ll_tag(uint32_t acct, int auth, int final, char out[16])
{
    /* statisch: nur der Spiel-Thread ruft (Kontenliste, Logon-Abschluss); Spielthread-Stack klein halten */
    static char orig[LOCLIVE_PIN_MAX + 1], cur[LOCLIVE_PIN_MAX + 64], nxt[LOCLIVE_PIN_MAX + 64];
    char path[600], g[16], probe[700];
    loclive_tag_plan_t pl;
    FILE *f = NULL;
    size_t n = 0;
    int r, keep_old = 0, lock;
    ll_pin_path(path, sizeof path);
    if (auth != 2 || !ll_gamertag(g)) {
        /* noch nicht angemeldet bzw. kein Gamertag: Anzeige mit dem zuletzt genutzten Tag, sonst Platzhalter */
        if (!acct || !loclive_pin_lookup(path, acct, out) || !loclive_tag_valid(out)) strcpy_s(out, 16, "Pilot");
        return 0;
    }
    strcpy_s(out, 16, g);
    if (!final || !acct) return 1;
    if (fopen_s(&f, path, "rb") == 0 && f) {
        n = fread(orig, 1, LOCLIVE_PIN_MAX, f);
        fclose(f);
    }
    orig[n] = 0;
    if (loclive_tag_plan(orig, acct, g, &pl) < 0) return 1;
    strcpy_s(cur, sizeof cur, orig);
    if (pl.park_acct) {
        r = ll_move_folders(pl.park_from, pl.park_to);
        HLOG_I("LOCLIVE: veralteter Eintrag eines anderen Kontos mit diesem Gamertag - Pilotordner parken: %s",
               r == 1 ? "ok" : r == 0 ? "Ziel belegt" : "Fehler");
        if (r == 1 && loclive_pin_put(cur, pl.park_acct, pl.park_to, nxt, sizeof nxt) == 0) strcpy_s(cur, sizeof cur, nxt);
    }
    if (pl.own_from[0]) {
        r = ll_move_folders(pl.own_from, pl.own_to);
        keep_old = r != 1;
        HLOG_I("LOCLIVE: Gamertag gewechselt - Pilotordner %s",
               r == 1 ? "umgezogen (Piloten bleiben)"
                      : r == 0 ? "NICHT umgezogen (Zielordner existiert), alter Ordner bleibt"
                               : "NICHT umgezogen (Fehler), alter Ordner bleibt");
        if (keep_old) strcpy_s(out, 16, pl.own_from); /* Piloten nicht verlieren: alter Ordner bleibt sichtbar */
    }
    if (!keep_old && loclive_pin_put(cur, acct, g, nxt, sizeof nxt) == 0) strcpy_s(cur, sizeof cur, nxt);
    /* Spielstand-Schutz aktiv (Ordner liegen in der Wegwerf-Kopie): Merkdatei nicht aendern, damit der naechste
     * Start ohne Schutz die echten Ordner genauso umzieht */
    lock = krnl_host_path("T:\\", probe, sizeof probe) == 0 && strstr(probe, "_savelock") != NULL;
    if (strcmp(cur, orig) != 0) {
        f = NULL;
        if (lock) {
            HLOG_I("LOCLIVE: Spielstand-Schutz aktiv - Merkdatei unveraendert");
        } else if (fopen_s(&f, path, "wb") == 0 && f) {
            fwrite(cur, 1, strlen(cur), f);
            fclose(f);
        } else {
            HLOG_W("LOCLIVE: Merkdatei nicht beschreibbar - Zuordnung gilt nur fuer diese Sitzung");
        }
    }
    HLOG_I("LOCLIVE: Gamertag aus dem Konto (%d Zeichen%s)", (int)strlen(out), pl.own_from[0] ? ", Wechsel" : "");
    return 1;
}

/* ---------- Logon ---------- */
static u32 __stdcall ll_XOnlineGetUsers(u32 pusers, u32 pcount)
{
    uint8_t u[LOCLIVE_USER_SIZE];
    uint32_t acct = 0, n = 1;
    int auth = ll_auth(&acct);
    char tag[16];
    FIRST(0, "XOnlineGetUsers (1 Konto)");
    ll_flow_enter("Kontenauswahl"); /* W501 */
    ll_tag(acct, auth, 0, tag);
    loclive_fill_user(u, loclive_xuid(acct), tag, LOCLIVE_S_OK);
    if (!pusers || !pcount || !ll_put(pusers, u, sizeof u) || !ll_put(pcount, &n, 4)) return LOCLIVE_E_INVALIDARG;
    return LOCLIVE_S_OK;
}
static u32 __stdcall ll_XOnlineLogon(u32 pusers, u32 psvc, u32 nsvc, u32 hev, u32 ph)
{
    uint8_t us[LOCLIVE_MAX_LOGON * LOCLIVE_USER_SIZE];
    u32 svc[8];
    int i;
    (void)hev;
    ll_flow_enter("Logon"); /* W501 */
    memset(us, 0, sizeof us);
    if (!ll_get(us, pusers, sizeof us)) return LOCLIVE_E_INVALIDARG;
    s_pad = -1;
    for (i = 0; i < LOCLIVE_MAX_LOGON; i++) {
        uint64_t x;
        memcpy(&x, us + i * LOCLIVE_USER_SIZE + LOCLIVE_USER_OFF_XUID, 8);
        if (x && s_pad < 0) s_pad = i;
    }
    memset(svc, 0, sizeof svc);
    if (nsvc > 8) nsvc = 8;
    ll_get(svc, psvc, nsvc * 4);
    HLOG_I("LOCLIVE: XOnlineLogon (Platz %d, %u Dienste: 0x%X 0x%X 0x%X 0x%X)", s_pad, nsvc, svc[0], svc[1], svc[2],
           svc[3]);
    memset(s_logon, 0, sizeof s_logon);
    return ll_task_out(LLT_LOGON, ph);
}
static void ll_logon_done(loclive_task *t, int auth)
{
    uint32_t acct = 0;
    ll_auth(&acct);
    memset(s_logon, 0, sizeof s_logon);
    if (t->result == LOCLIVE_LOGON_OK) {
        ll_tag(acct, auth, 1, s_tag);
        if (s_pad >= 0)
            loclive_fill_user(s_logon + s_pad * LOCLIVE_USER_SIZE, loclive_xuid(acct), s_tag, LOCLIVE_S_OK);
        HLOG_I("LOCLIVE: Logon angenommen nach %u ms (steelnet-Konto, Pseudo-XUID-Bereich 0x%08X, Dienste 6/8/9 ok, "
               "Kampagne 0x%08X - %s)", GetTickCount() - t->start_ms, LOCLIVE_XUID_PREFIX,
               loccamp_service_hr(loccamp_bridge_state()), loccamp_state_name(loccamp_bridge_state()));
    } else {
        HLOG_W("LOCLIVE: Logon abgelehnt -> 0x%08X (steelnet-Anmeldung: %s)", t->result,
               auth == -2 ? "kein Gamertag gesetzt - im Launcher unter steelnet > Manage account waehlen"
               : auth == -1 ? "Konto abgelehnt" : auth == 1 ? "Zeit abgelaufen"
                                              : !s_tun ? "kein Treffpunkt/Transport" : "kein Konto (Gast)");
    }
}
static u32 __stdcall ll_XOnlineTaskContinue(u32 h)
{
    loclive_task *t = ll_task_of(h);
    u32 r, before;
    int auth;
    if (!t) {
        static volatile LONG warned;
        if (InterlockedIncrement(&warned) <= 4) HLOG_W("LOCLIVE: XOnlineTaskContinue mit unbekanntem Griff");
        return LOCLIVE_E_INVALIDARG;
    }
    before = t->result;
    if ((unsigned)t->kind < LLT_KINDS) InterlockedIncrement(&s_tcont[t->kind]); /* W501 DIAG */
    if (t->kind == LLT_FRIENDS || t->kind == LLT_MUTE) {
        /* W501: Abmelden ("Signing out..."): Der Abmelde-Ablauf FUN_0011bb80 -> FUN_000d7090 pumpt Friends
         * (FUN_000d8bf0) und Mutelist (FUN_000d8f70) und schliesst die Startup-Tasks erst, wenn sie 0x001500F2
         * (XONLINETASK_S_RUNNING_IDLE) melden; danach folgt der Logon-Task (XOnlineTaskClose). Gleiches Muster im
         * Abbau FUN_000d6fc0 (FUN_000d8250/FUN_000d8e30). Diese Pumpen laufen nur beim Abmelden -> dort "idle" melden
         * (sonst hing das Abmelden fuer immer); im normalen Betrieb bleibt es bei "laeuft". */
        u32 ra = (u32)(uintptr_t)_ReturnAddress();
        if ((t->kind == LLT_FRIENDS && ((ra >= 0xD8BF0u && ra < 0xD8C5Du) || (ra >= 0xD8250u && ra < 0xD830Au))) ||
            (t->kind == LLT_MUTE && ((ra >= 0xD8F70u && ra < 0xD8FE8u) || (ra >= 0xD8E30u && ra < 0xD8E9Du)))) {
            static volatile LONG s_down_logged;
            if (InterlockedExchange(&s_down_logged, 1) == 0)
                HLOG_I("LOCLIVE: Abmelden - Friends/Mutelist melden idle (Abmelde-Ablauf FUN_000d7090)");
            return 0x001500F2u; /* XONLINETASK_S_RUNNING_IDLE */
        }
    }
    if (t->kind == LLT_NET) ll_net_poll(t); /* W482: Antwort von steelnet abholen */
    auth = t->kind == LLT_LOGON ? ll_auth(NULL) : 0;
    if (auth == 2) { /* W487: Xbox Live nur mit Gamertag des Kontos (im Launcher gewaehlt) */
        char g[16];
        if (!ll_gamertag(g)) auth = -2;
    }
    if (t->kind == LLT_LOGON && auth == 2 && !t->result &&
        !loccamp_logon_ready(loccamp_bridge_state(), GetTickCount() - t->start_ms, LOCCAMP_LOGON_WAIT_MS))
        return LOCLIVE_TASK_RUNNING; /* W491: Logon erst, wenn die Kampagne entschieden ist (Dienstliste danach) */
    r = loclive_task_step(t, auth, GetTickCount(), LOCLIVE_LOGON_TIMEOUT_MS);
    if (t->kind == LLT_LOGON && !before && t->result) ll_logon_done(t, auth);
    return r;
}
static u32 __stdcall ll_XOnlineTaskClose(u32 h)
{
    loclive_task *t = ll_task_of(h);
    if (t) {
        if ((unsigned)t->kind < LLT_KINDS) InterlockedIncrement(&s_tclose[t->kind]); /* W501 DIAG */
        if (t->kind == LLT_LOGON) ll_flow_leave("Logon-Task geschlossen"); /* W501: XDK-Abmeldung = Logon-Task schliessen */
        if (t->res_mem) free(t->res_mem); /* W482: Suchergebnisse */
        t->res_mem = NULL;
        t->magic = 0;
    }
    return LOCLIVE_S_OK;
}
static u32 __stdcall ll_XOnlineLogonTaskGetResults(u32 h)
{
    loclive_task *t = ll_task_of(h);
    if (!t || t->kind != LLT_LOGON) return LOCLIVE_E_INVALIDARG;
    return t->result ? t->result : LOCLIVE_TASK_RUNNING;
}
static u32 __stdcall ll_XOnlineGetLogonUsers(void)
{
    return (u32)(uintptr_t)s_logon;
}
static u32 __stdcall ll_XOnlineGetServiceInfo(u32 svc, u32 pinfo)
{
    u32 hr = loclive_service_hr(svc);
    static volatile LONG seen[4], camp_last = -99;
    int k = svc == 6 ? 0 : svc == 8 ? 1 : svc == 9 ? 2 : 3;
    if (svc == LOCLIVE_SVC_CAMPAIGN) {
        /* W491 CAMP-CLIENT: virtuelle Adresse + Port, solange Ticket und Zugang bereit sind; sonst "temporarily
         * unavailable" (Campaign grau; auch Notfall "Kampagne aus" = Zugang bzw. Ticketausgabe abgeschaltet). */
        int st = loccamp_bridge_state();
        hr = loccamp_service_hr(st);
        if (InterlockedExchange(&camp_last, st) != st)
            HLOG_I("LOCCAMP: XOnlineGetServiceInfo(0x%X) -> 0x%08X (Kampagne %s)", svc, hr, loccamp_state_name(st));
        if (hr == LOCLIVE_S_OK && pinfo) {
            uint8_t info[12];
            loccamp_service_info(info);
            ll_put(pinfo, info, sizeof info);
        }
        return hr;
    }
    if (InterlockedExchange(&seen[k], 1) == 0) HLOG_I("LOCLIVE: XOnlineGetServiceInfo(0x%X) -> 0x%08X", svc, hr);
    if (hr == LOCLIVE_S_OK && pinfo) {
        u32 info[3] = {svc, 0, 0}; /* dwServiceID, IN_ADDR, Port - QoS laeuft ueber das XNet-HLE */
        ll_put(pinfo, info, sizeof info);
    }
    return hr;
}
static u32 __stdcall ll_XOnlineTitleUpdate(u32 title)
{
    loc_live_count(LLC_TITLE_UPDATE);
    HLOG_W("LOCLIVE: XOnlineTitleUpdate(0x%X) - nie vorgesehen (kein \"Update noetig\") -> E_FAIL", title);
    return LOCLIVE_E_FAIL;
}

/* ---------- W482 LIVE-MATCH: Matchmaking ueber steelnet ----------
 * Create/Update/Delete/FindFromID/Search reihen eine Anfrage beim Transport ein (Netz-Thread) und geben sofort eine
 * Task zurueck; XOnlineTaskContinue pollt die Antwort (laeuft -> SUCCESS bzw. E_FAIL nach Fehler/15 s). XNKID/XNKEY
 * kommen vom Dienst; Suchergebnisse werden hier als XMATCH_SEARCHRESULT (0x54 B + Attribute) gebaut, Host-XNADDR aus
 * der gelisteten vip (der Transport hat sie geprueft und einen Geist fuer den Beitritt angelegt). Keine
 * Attributwerte, Namen oder Schluessel im Log. */
static loclive_sess s_sessbuf[LOCLIVE_MAX_RESULTS]; /* nur Spiel-Thread (TaskContinue) */
static uint8_t s_own_sid[8];
static int s_own;

static const char *ll_op_name(int op)
{
    return op == LLO_CREATE ? "MatchSessionCreate" : op == LLO_UPDATE ? "MatchSessionUpdate"
         : op == LLO_DELETE ? "MatchSessionDelete" : op == LLO_SEARCH ? "MatchSearch" : "MatchSessionFindFromID";
}

/* WCHAR-String aus dem Spielspeicher (<= 15 Zeichen; Steuerzeichen/Surrogate -> '?'). 0 = Zeiger ungueltig. */
static int ll_wread(u32 p, uint16_t w[LOCLIVE_STR_MAX + 1])
{
    int i;
    memset(w, 0, (LOCLIVE_STR_MAX + 1) * 2);
    if (!p) return 0;
    for (i = 0; i < LOCLIVE_STR_MAX; i++) {
        uint16_t c;
        if (!ll_get(&c, p + 2u * (u32)i, 2)) return 0;
        if (!c) break;
        if (c < 0x20 || c == 0x7F || (c >= 0xD800 && c <= 0xDFFF)) c = '?';
        w[i] = c;
    }
    return 1;
}

/* XONLINE_ATTRIBUTE-Liste lesen. changed_only: nur fChanged (Update). Rueckgabe Anzahl, -1 = Speicher ungueltig. */
static int ll_read_attrs(u32 at, u32 n, int changed_only, loclive_attr *out)
{
    u32 i;
    int k = 0;
    if (n > LOCLIVE_MAX_ATTR) n = LOCLIVE_MAX_ATTR;
    for (i = 0; i < n; i++) {
        uint8_t e[LOCLIVE_ATTR_SIZE];
        u32 id, chg, p;
        loclive_attr *a = &out[k];
        if (!ll_get(e, at + i * LOCLIVE_ATTR_SIZE, sizeof e)) return -1;
        memcpy(&id, e, 4);
        memcpy(&chg, e + 4, 4);
        if (changed_only && !chg) continue;
        if ((id & LOCLIVE_ATTR_TYPEMASK) != 0 && (id & LOCLIVE_ATTR_TYPEMASK) != LOCLIVE_ATTR_STRING) continue;
        memset(a, 0, sizeof *a);
        a->id = id;
        if (id & LOCLIVE_ATTR_STRING) {
            memcpy(&p, e + 8, 4);
            a->is_str = 1;
            if (p && !ll_wread(p, a->w)) return -1;
        } else {
            memcpy(&a->v, e + 8, 8);
        }
        k++;
    }
    return k;
}

/* Suchparameter: dwAttributeID = Typ (0 Integer, 0x00100000 String, 0x00F00000 NULL); Parameter i -> Attribut i. */
static int ll_read_params(u32 proc, u32 at, u32 n, loclive_attr *out)
{
    const uint32_t *ids;
    int nids = loclive_proc_ids(proc, &ids), k;
    if (!nids) return -1;
    if (n > (u32)nids) n = (u32)nids;
    for (k = 0; k < (int)n; k++) {
        uint8_t e[LOCLIVE_ATTR_SIZE];
        u32 typ, p;
        loclive_attr *a = &out[k];
        if (!ll_get(e, at + (u32)k * LOCLIVE_ATTR_SIZE, sizeof e)) return -1;
        memcpy(&typ, e, 4);
        memset(a, 0, sizeof *a);
        a->id = ids[k];
        a->is_str = (ids[k] & LOCLIVE_ATTR_STRING) != 0;
        if ((typ & LOCLIVE_ATTR_TYPEMASK) == LOCLIVE_ATTR_NULL) {
            a->is_null = 1;
        } else if (a->is_str) {
            memcpy(&p, e + 8, 4);
            if ((typ & LOCLIVE_ATTR_TYPEMASK) != LOCLIVE_ATTR_STRING || !p || !ll_wread(p, a->w) || !a->w[0])
                a->is_null = 1; /* leerer Name = egal (wie das Spiel bei der optionalen Suche) */
        } else {
            memcpy(&a->v, e + 8, 8);
            if ((typ & LOCLIVE_ATTR_TYPEMASK) != 0 || a->v == 0x7FFFFFFFFFFFFFFFll) a->is_null = 1;
        }
    }
    return (int)n;
}

static u32 ll_clamp_slot(u32 v)
{
    return v > 10 ? 10 : v;
}

/* Anfrage einreihen und Task anlegen. Ohne Verbindung scheitert die Task beim ersten Pollen (Spiel: Live-Init neu). */
static u32 ll_net_task(int op, const char *json, int with_vip, u32 ph)
{
    loclive_task *t;
    u32 h;
    if (!ph) return LOCLIVE_E_INVALIDARG;
    t = ll_task_new(LLT_NET);
    if (!t) return LOCLIVE_E_FAIL;
    t->op = op;
    t->req = s_tun && json ? loctun_live_send(s_tun, json, with_vip) : 0;
    h = (u32)(uintptr_t)t;
    if (!ll_put(ph, &h, 4)) {
        t->magic = 0;
        return LOCLIVE_E_INVALIDARG;
    }
    if (!t->req) HLOG_W("LOCLIVE: %s - steelnet nicht verbunden -> Task scheitert", ll_op_name(op));
    return LOCLIVE_S_OK;
}

static void ll_sid_hex(u32 k0, u32 k1, char out[17], uint8_t sid[8])
{
    static const char hx[] = "0123456789abcdef";
    int i;
    memcpy(sid, &k0, 4);
    memcpy(sid + 4, &k1, 4);
    for (i = 0; i < 8; i++) {
        out[2 * i] = hx[sid[i] >> 4];
        out[2 * i + 1] = hx[sid[i] & 15];
    }
    out[16] = 0;
}

/* Suchergebnisse -> ein Block: Zeigerliste (n * 4 B) + XMATCH_SEARCHRESULTs (je 4-B-ausgerichtet). */
static void ll_build_results(loclive_task *t, int n)
{
    uint32_t total = (uint32_t)n * 4u, off;
    uint8_t *mem;
    int i;
    for (i = 0; i < n; i++) total += (loclive_result_size(&s_sessbuf[i]) + 3u) & ~3u;
    mem = (uint8_t *)calloc(1, total ? total : 4);
    if (!mem) {
        t->result = LOCLIVE_E_FAIL;
        return;
    }
    off = (uint32_t)n * 4u;
    for (i = 0; i < n; i++) {
        uint8_t xn[LOCLIVE_XNADDR_SIZE], key[16];
        u32 va = (u32)(uintptr_t)(mem + off);
        loctun_xnaddr(s_tun, s_sessbuf[i].vip, xn);
        loclive_key_placeholder(s_sessbuf[i].sid, key);
        loclive_result_build(mem + off, va, &s_sessbuf[i], xn, key);
        memcpy(mem + 4u * (uint32_t)i, &va, 4);
        off += (loclive_result_size(&s_sessbuf[i]) + 3u) & ~3u;
    }
    t->res_mem = mem;
    t->res_len = total;
    t->nres = (uint32_t)n;
}

/* XOnlineTaskContinue fuer Netz-Tasks: Antwort des Transports abholen. */
static void ll_net_poll(loclive_task *t)
{
    char err[24] = "";
    uint8_t sid[8], key[16];
    int n = 0, r, want = t->op == LLO_SEARCH || t->op == LLO_FIND;
    if (t->result) return;
    if (!t->req || !s_tun) {
        t->result = LOCLIVE_E_FAIL;
        return;
    }
    r = loctun_live_poll(s_tun, t->req, err, (int)sizeof err, sid, key, want ? s_sessbuf : NULL,
                         want ? LOCLIVE_MAX_RESULTS : 0, &n);
    if (r == 0) {
        if (GetTickCount() - t->start_ms > LOCLIVE_REQ_TIMEOUT_MS + 5000u) {
            t->result = LOCLIVE_E_FAIL;
            HLOG_W("LOCLIVE: %s - keine Antwort vom Transport -> E_FAIL", ll_op_name(t->op));
        }
        return;
    }
    t->req = 0;
    if (r != 1) {
        t->result = LOCLIVE_E_FAIL;
        HLOG_W("LOCLIVE: %s abgelehnt (%s) nach %u ms -> E_FAIL", ll_op_name(t->op), err[0] ? err : "?",
               GetTickCount() - t->start_ms);
        return;
    }
    t->result = LOCLIVE_TASK_SUCCESS;
    if (t->op == LLO_CREATE) {
        memcpy(t->sid, sid, 8);
        memcpy(t->key, key, 16);
        memcpy(s_own_sid, sid, 8);
        s_own = 1;
        SecureZeroMemory(key, sizeof key);
    } else if (want) {
        ll_build_results(t, n);
    }
    HLOG_I("LOCLIVE: %s fertig nach %u ms%s", ll_op_name(t->op), GetTickCount() - t->start_ms,
           want ? (n ? " (Treffer)" : " (0 Treffer)") : "");
    if (want) HLOG_I("LOCLIVE: %s: %d Treffer", ll_op_name(t->op), n);
}

static u32 __stdcall ll_XOnlineMatchSessionCreate(u32 a, u32 b, u32 c, u32 d, u32 n, u32 at, u32 hev, u32 ph)
{
    loclive_attr at_[LOCLIVE_MAX_ATTR];
    char js[TUN_LIVE_JSON_HLE], attrs[TUN_LIVE_JSON_HLE - 200];
    int na, kind;
    (void)hev;
    na = ll_read_attrs(at, n, 0, at_);
    if (na < 0 || loclive_attrs_json(at_, na, attrs, (int)sizeof attrs) < 0) {
        HLOG_W("LOCLIVE: XOnlineMatchSessionCreate - Attribute nicht lesbar -> E_INVALIDARG");
        return LOCLIVE_E_INVALIDARG;
    }
    kind = loclive_kind(at_, na);
    _snprintf_s(js, sizeof js, _TRUNCATE,
                "{\"t\":\"live_create\",\"lv\":1,\"kind\":\"%s\",\"slots\":[%u,%u,%u,%u],\"attrs\":%s}",
                kind == 2 ? "chat" : "match", ll_clamp_slot(b), ll_clamp_slot(a), ll_clamp_slot(d), ll_clamp_slot(c),
                attrs);
    HLOG_I("LOCLIVE: XOnlineMatchSessionCreate (%s, %d Attribute, Plaetze %u/%u offen) -> steelnet",
           kind == 2 ? "Chatraum" : "Match", na, b, d);
    return ll_net_task(LLO_CREATE, js, 1, ph);
}
static u32 __stdcall ll_XOnlineMatchSessionUpdate(u32 k0, u32 k1, u32 a, u32 b, u32 c, u32 d, u32 n, u32 at, u32 hev,
                                                  u32 ph)
{
    loclive_attr at_[LOCLIVE_MAX_ATTR];
    char js[TUN_LIVE_JSON_HLE], attrs[TUN_LIVE_JSON_HLE - 200], hex[17];
    uint8_t sid[8];
    int na;
    (void)hev;
    na = ll_read_attrs(at, n, 1, at_);
    if (na < 0 || loclive_attrs_json(at_, na, attrs, (int)sizeof attrs) < 0) return LOCLIVE_E_INVALIDARG;
    ll_sid_hex(k0, k1, hex, sid);
    _snprintf_s(js, sizeof js, _TRUNCATE,
                "{\"t\":\"live_update\",\"lv\":1,\"sid\":\"%s\",\"slots\":[%u,%u,%u,%u],\"attrs\":%s}", hex,
                ll_clamp_slot(b), ll_clamp_slot(a), ll_clamp_slot(d), ll_clamp_slot(c), attrs);
    FIRST(11, "XOnlineMatchSessionUpdate");
    HLOG_I("LOCLIVE: XOnlineMatchSessionUpdate (%d geaenderte Attribute, Plaetze %u/%u offen)", na, b, d);
    return ll_net_task(LLO_UPDATE, js, 0, ph);
}
static u32 __stdcall ll_XOnlineMatchSessionGetInfo(u32 h, u32 pkid, u32 pkey)
{
    loclive_task *t = ll_task_of(h);
    FIRST(12, "XOnlineMatchSessionGetInfo");
    if (!t || t->kind != LLT_NET || t->op != LLO_CREATE || t->result != LOCLIVE_TASK_SUCCESS) {
        HLOG_W("LOCLIVE: XOnlineMatchSessionGetInfo ohne fertige Create-Task -> E_FAIL");
        return LOCLIVE_E_FAIL;
    }
    if (!ll_put(pkid, t->sid, 8) || !ll_put(pkey, t->key, 16)) return LOCLIVE_E_INVALIDARG;
    return LOCLIVE_S_OK;
}
static u32 __stdcall ll_XOnlineMatchSessionDelete(u32 k0, u32 k1, u32 hev, u32 ph)
{
    char js[96], hex[17];
    uint8_t sid[8];
    (void)hev;
    ll_sid_hex(k0, k1, hex, sid);
    if (s_own && memcmp(sid, s_own_sid, 8) == 0) s_own = 0;
    _snprintf_s(js, sizeof js, _TRUNCATE, "{\"t\":\"live_delete\",\"lv\":1,\"sid\":\"%s\"}", hex);
    HLOG_I("LOCLIVE: XOnlineMatchSessionDelete -> steelnet");
    return ll_net_task(LLO_DELETE, js, 0, ph);
}
static u32 __stdcall ll_XOnlineMatchSessionFindFromID(u32 k0, u32 k1, u32 hev, u32 ph)
{
    char js[96], hex[17];
    uint8_t sid[8];
    (void)hev;
    ll_sid_hex(k0, k1, hex, sid);
    _snprintf_s(js, sizeof js, _TRUNCATE, "{\"t\":\"live_find\",\"lv\":1,\"sid\":\"%s\"}", hex);
    HLOG_I("LOCLIVE: XOnlineMatchSessionFindFromID -> steelnet");
    return ll_net_task(LLO_FIND, js, 0, ph);
}
static u32 __stdcall ll_XOnlineMatchSearch(u32 proc, u32 nres, u32 nattr, u32 at, u32 len, u32 hev, u32 ph)
{
    loclive_attr pa[LOCLIVE_MAX_ATTR];
    char js[TUN_LIVE_JSON_HLE], params[TUN_LIVE_JSON_HLE - 200];
    int np, i, nn = 0;
    (void)len; (void)hev;
    np = ll_read_params(proc, at, nattr, pa);
    if (np < 0 || loclive_attrs_json(pa, np, params, (int)sizeof params) < 0) {
        HLOG_W("LOCLIVE: XOnlineMatchSearch(Prozedur %u) - unbekannte Prozedur/Parameter -> E_INVALIDARG", proc);
        return LOCLIVE_E_INVALIDARG;
    }
    for (i = 0; i < np; i++) nn += pa[i].is_null;
    if (nres < 1) nres = 1;
    if (nres > LOCLIVE_MAX_RESULTS) nres = LOCLIVE_MAX_RESULTS;
    _snprintf_s(js, sizeof js, _TRUNCATE, "{\"t\":\"live_search\",\"lv\":1,\"proc\":%u,\"max\":%u,\"params\":%s}", proc,
                nres, params);
    HLOG_I("LOCLIVE: XOnlineMatchSearch(Prozedur %u, %d Parameter, %d egal, max %u) -> steelnet", proc, np, nn, nres);
    return ll_net_task(LLO_SEARCH, js, 0, ph);
}
static u32 __stdcall ll_XOnlineMatchSearchGetResults(u32 h, u32 pprg, u32 pn)
{
    loclive_task *t = ll_task_of(h);
    u32 p = (u32)(uintptr_t)s_nores, z = 0;
    if (t && t->kind == LLT_NET && (t->op == LLO_SEARCH || t->op == LLO_FIND) && t->result == LOCLIVE_TASK_SUCCESS &&
        t->res_mem) {
        p = (u32)(uintptr_t)t->res_mem;
        z = t->nres;
    }
    if (!ll_put(pprg, &p, 4) || !ll_put(pn, &z, 4)) return LOCLIVE_E_INVALIDARG;
    return LOCLIVE_S_OK;
}
static u32 __stdcall ll_XOnlineMatchSearchParse(u32 r, u32 n, u32 spec, u32 out)
{
    uint32_t sp[2 * LOCLIVE_MAX_ATTR], used = 0, hr;
    static uint8_t buf[LOCLIVE_MAX_ATTR * 256];
    int i;
    FIRST(16, "XOnlineMatchSearchParse");
    if (n > LOCLIVE_MAX_ATTR || !ll_get(sp, spec, n * 8u)) return LOCLIVE_E_INVALIDARG;
    for (i = 0; i < (int)(sizeof s_task / sizeof s_task[0]); i++) {
        loclive_task *t = &s_task[i];
        uintptr_t b = (uintptr_t)t->res_mem;
        if (t->magic != LOCLIVE_TASK_MAGIC || !t->res_mem) continue;
        if ((uintptr_t)r < b + 4u * t->nres || (uintptr_t)r >= b + t->res_len) continue;
        hr = loclive_parse((const uint8_t *)(uintptr_t)r, r, (u32)(b + t->res_len - (uintptr_t)r), n, sp, buf,
                           (u32)sizeof buf, &used);
        if (hr != LOCLIVE_S_OK) {
            HLOG_W("LOCLIVE: XOnlineMatchSearchParse: Specs passen nicht zum Ergebnis -> E_FAIL");
            return hr;
        }
        return ll_put(out, buf, used) ? LOCLIVE_S_OK : LOCLIVE_E_INVALIDARG;
    }
    HLOG_W("LOCLIVE: XOnlineMatchSearchParse mit unbekanntem Ergebnis -> E_FAIL");
    return LOCLIVE_E_FAIL;
}
static u32 __stdcall ll_XOnlineMatchSearchResultsLen(u32 nres, u32 nattr, u32 spec)
{
    (void)spec;
    if (nres > 50) nres = 50;
    if (nattr > 32) nattr = 32;
    return nres * (0x54u + nattr * 0x48u) + 0x10u; /* Kopf 0x54 + Attribute (ID, Typ, <= 32 B Wert) */
}

/* ---------- Presence / Friends / Mutelist / Feedback / Namenspruefung: leer bzw. angenommen ---------- */
static u32 __stdcall ll_XOnlineTitleIdIsSameTitle(u32 title)
{
    (void)title;
    loc_live_count(LLC_SAME_TITLE);
    return 0;
}
static u32 __stdcall ll_XOnlineNotificationSetState(u32 user, u32 flags, u32 k0, u32 k1, u32 cb, u32 pb)
{
    (void)user; (void)k0; (void)k1; (void)cb; (void)pb; /* Titeldaten werden nicht gelesen */
    loc_live_count(LLC_PRESENCE_SET);
    if (InterlockedExchange(&s_first[20], 1) == 0)
        HLOG_I("LOCLIVE: XOnlineNotificationSetState (Flags 0x%X) -> S_OK", flags);
    return LOCLIVE_S_OK;
}
static u32 __stdcall ll_XOnlineGetNotification(u32 user, u32 type)
{
    (void)user; (void)type;
    FIRST(21, "XOnlineGetNotification (keine)");
    loc_live_count(LLC_NOTIFICATION_GET);
    return 0;
}
static u32 __stdcall ll_XOnlineFriendsStartup(u32 hev, u32 ph)
{
    (void)hev;
    FIRST(22, "XOnlineFriendsStartup");
    loc_live_count(LLC_FRIENDS_STARTUP);
    return ll_task_out(LLT_FRIENDS, ph);
}
static u32 __stdcall ll_XOnlineFriendsEnumerate(u32 user, u32 hev, u32 ph)
{
    (void)user; (void)hev;
    FIRST(23, "XOnlineFriendsEnumerate (leer)");
    loc_live_count(LLC_FRIENDS_ENUM);
    return ll_task_out(LLT_FRIENDS_ENUM, ph);
}
static u32 __stdcall ll_XOnlineFriendsEnumerateFinish(u32 h)
{
    loclive_task *t = ll_task_of(h);
    loc_live_count(LLC_FRIENDS_ENUM_FINISH);
    if (t) t->finished = 1;
    return LOCLIVE_S_OK;
}
static u32 __stdcall ll_XOnlineFriendsGetLatest(u32 user, u32 cnt, u32 buf)
{
    (void)user; (void)cnt; (void)buf;
    loc_live_count(LLC_FRIENDS_LATEST);
    return 0;
}
static u32 __stdcall ll_XOnlineFriendsGetTitleName(u32 t, u32 l, u32 c, u32 p)
{
    (void)t; (void)l; (void)c; (void)p;
    loc_live_count(LLC_FRIENDS_TITLENAME);
    return LOCLIVE_E_FAIL;
}
static u32 __stdcall ll_XOnlineFriendsRemove(u32 a, u32 b)
{
    (void)a; (void)b;
    loc_live_count(LLC_FRIENDS_REMOVE);
    return LOCLIVE_S_OK;
}
static u32 __stdcall ll_XOnlineFriendsRequest(u32 a, u32 b, u32 c, u32 d)
{
    (void)a; (void)b; (void)c; (void)d;
    loc_live_count(LLC_FRIENDS_REQUEST);
    return LOCLIVE_S_OK;
}
static u32 __stdcall ll_XOnlineFriendsGameInvite(u32 a, u32 b, u32 c, u32 d, u32 e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    loc_live_count(LLC_FRIENDS_INVITE);
    return LOCLIVE_S_OK;
}
static u32 __stdcall ll_XOnlineFriendsRevokeGameInvite(u32 a, u32 b, u32 c, u32 d, u32 e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    loc_live_count(LLC_FRIENDS_REVOKE);
    return LOCLIVE_S_OK;
}
static u32 __stdcall ll_XOnlineFriendsAnswerRequest(u32 a, u32 b, u32 c)
{
    (void)a; (void)b; (void)c;
    loc_live_count(LLC_FRIENDS_ANSWER_REQ);
    return LOCLIVE_S_OK;
}
static u32 __stdcall ll_XOnlineFriendsAnswerGameInvite(u32 a, u32 b, u32 c)
{
    (void)a; (void)b; (void)c;
    loc_live_count(LLC_FRIENDS_ANSWER_INV);
    return LOCLIVE_S_OK;
}
static u32 __stdcall ll_XOnlineFriendsJoinGame(u32 a, u32 b)
{
    (void)a; (void)b;
    loc_live_count(LLC_FRIENDS_JOIN);
    return LOCLIVE_S_OK;
}
static u32 __stdcall ll_XOnlineFriendsGetAcceptedGameInvite(u32 p)
{
    (void)p;
    FIRST(24, "XOnlineFriendsGetAcceptedGameInvite (keine)");
    loc_live_count(LLC_INVITE_GET);
    return LOCLIVE_S_FALSE;
}
static u32 __stdcall ll_XOnlineMutelistStartup(u32 hev, u32 ph)
{
    (void)hev;
    FIRST(25, "XOnlineMutelistStartup");
    loc_live_count(LLC_MUTE_STARTUP);
    return ll_task_out(LLT_MUTE, ph);
}
/* W482: XDK-Reihenfolge (dwUserIndex, dwMaxUsers, hWorkEvent, phTask, pMutelistUsers, pdwNumMutelistUsers) - Spiel
 * FUN_000D8D.. ruft (user, 0xFA, 0, M+4, M+0xC, M+8) mit M = [0x277FD8]. W479 schrieb den Task-Griff nach M+8 (Zahl
 * der Eintraege); FUN_000D8FF0 (Stimmen-/Stumm-Status je Mitspieler, nur Live mit XUIDs) lief dann ueber das Ende
 * der Liste hinaus -> Absturz beim ersten Beitritt (0xD9010). */
static u32 __stdcall ll_XOnlineMutelistGet(u32 user, u32 cnt, u32 hev, u32 ph, u32 pusers, u32 pn)
{
    u32 z = 0;
    (void)user; (void)cnt; (void)hev; (void)pusers;
    FIRST(26, "XOnlineMutelistGet (leer)");
    loc_live_count(LLC_MUTE_GET);
    ll_put(pn, &z, 4);
    return ll_task_out(LLT_DONE, ph);
}
static u32 __stdcall ll_XOnlineMutelistAdd(u32 a, u32 b, u32 c, u32 d)
{
    (void)a; (void)b; (void)c; (void)d;
    loc_live_count(LLC_MUTE_ADD);
    return LOCLIVE_S_OK;
}
static u32 __stdcall ll_XOnlineMutelistRemove(u32 a, u32 b, u32 c, u32 d)
{
    (void)a; (void)b; (void)c; (void)d;
    loc_live_count(LLC_MUTE_REMOVE);
    return LOCLIVE_S_OK;
}
static u32 __stdcall ll_XOnlineFeedbackSend(u32 user, u32 x0, u32 x1, u32 x2, u32 type, u32 nick, u32 hev, u32 ph)
{
    (void)user; (void)x0; (void)x1; (void)x2; (void)nick; (void)hev; /* angenommen und verworfen, Inhalt nie geloggt */
    loc_live_count(LLC_FEEDBACK);
    if (InterlockedExchange(&s_first[27], 1) == 0)
        HLOG_I("LOCLIVE: XOnlineFeedbackSend (Typ %u) angenommen und verworfen", type);
    return ll_task_out(LLT_DONE, ph);
}
static u32 __stdcall ll_XOnlineVerifyNickname(u32 name, u32 hev, u32 ph)
{
    (void)name; (void)hev; /* immer gueltig; der Wortfilter laeuft lokal im Spiel (LOC_RE1 §3.3) */
    FIRST(28, "XOnlineVerifyNickname (-> gueltig)");
    loc_live_count(LLC_VERIFY_NICK);
    return ll_task_out(LLT_DONE, ph);
}

/* ---------- Installation ---------- */
typedef struct ll_hook {
    u32 va;
    uint8_t expect[8];
    void *handler;
    const char *name;
} ll_hook;

static const ll_hook k_live[] = {
    {0x157A20u, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0xE9, 0xEF}, (void *)ll_XOnlineTaskContinue, "XOnlineTaskContinue"},
    {0x157A2Bu, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0xE9, 0xF9}, (void *)ll_XOnlineTaskClose, "XOnlineTaskClose"},
    {0x157A36u, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0xE9, 0x05}, (void *)ll_XOnlineGetUsers, "XOnlineGetUsers"},
    {0x157A41u, {0x55, 0x8B, 0xEC, 0x8B, 0x0D, 0x78, 0xAE, 0x73}, (void *)ll_XOnlineLogon, "XOnlineLogon"},
    {0x157A50u, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0xE9, 0x8B}, (void *)ll_XOnlineLogonTaskGetResults, "XOnlineLogonTaskGetResults"},
    {0x157A5Bu, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0xE9, 0x95}, (void *)ll_XOnlineGetLogonUsers, "XOnlineGetLogonUsers"},
    {0x157A66u, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0xE9, 0x23}, (void *)ll_XOnlineGetServiceInfo, "XOnlineGetServiceInfo"},
    {0x157A71u, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0xE9, 0x32}, (void *)ll_XOnlineTitleUpdate, "XOnlineTitleUpdate"},
    {0x157A7Cu, {0x55, 0x8B, 0xEC, 0x8B, 0x0D, 0x78, 0xAE, 0x73}, (void *)ll_XOnlineMatchSessionCreate, "XOnlineMatchSessionCreate"},
    {0x157A8Bu, {0x55, 0x8B, 0xEC, 0xFF, 0x75, 0x2C, 0x8B, 0x0D}, (void *)ll_XOnlineMatchSessionUpdate, "XOnlineMatchSessionUpdate"},
    {0x157ABBu, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0xE9, 0xBD}, (void *)ll_XOnlineMatchSessionGetInfo, "XOnlineMatchSessionGetInfo"},
    {0x157AC6u, {0xFF, 0x74, 0x24, 0x10, 0x8B, 0x0D, 0x78, 0xAE}, (void *)ll_XOnlineMatchSessionDelete, "XOnlineMatchSessionDelete"},
    {0x157AE4u, {0xFF, 0x74, 0x24, 0x10, 0x8B, 0x0D, 0x78, 0xAE}, (void *)ll_XOnlineMatchSessionFindFromID, "XOnlineMatchSessionFindFromID"},
    {0x157B02u, {0x55, 0x8B, 0xEC, 0x8B, 0x0D, 0x78, 0xAE, 0x73}, (void *)ll_XOnlineMatchSearch, "XOnlineMatchSearch"},
    {0x157B11u, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0xE9, 0x99}, (void *)ll_XOnlineMatchSearchGetResults, "XOnlineMatchSearchGetResults"},
    {0x157B1Cu, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0xE9, 0x08}, (void *)ll_XOnlineMatchSearchParse, "XOnlineMatchSearchParse"},
    {0x157B27u, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0xE9, 0xFE}, (void *)ll_XOnlineMatchSearchResultsLen, "XOnlineMatchSearchResultsLen"},
    {0x157B32u, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0xE9, 0x90}, (void *)ll_XOnlineTitleIdIsSameTitle, "XOnlineTitleIdIsSameTitle"},
    {0x157B3Du, {0x55, 0x8B, 0xEC, 0xFF, 0x75, 0x1C, 0x8B, 0x0D}, (void *)ll_XOnlineNotificationSetState, "XOnlineNotificationSetState"},
    {0x157B61u, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0xE9, 0x70}, (void *)ll_XOnlineGetNotification, "XOnlineGetNotification"},
    {0x157B6Cu, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0xE9, 0xE3}, (void *)ll_XOnlineFriendsStartup, "XOnlineFriendsStartup"},
    {0x157B77u, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0xE9, 0x0D}, (void *)ll_XOnlineFriendsEnumerate, "XOnlineFriendsEnumerate"},
    {0x157B82u, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0xE9, 0x2B}, (void *)ll_XOnlineFriendsEnumerateFinish, "XOnlineFriendsEnumerateFinish"},
    {0x157B8Du, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0xE9, 0x4E}, (void *)ll_XOnlineFriendsGetLatest, "XOnlineFriendsGetLatest"},
    {0x157B98u, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0xE9, 0x38}, (void *)ll_XOnlineFriendsGetTitleName, "XOnlineFriendsGetTitleName"},
    {0x157BA3u, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0xE9, 0x79}, (void *)ll_XOnlineFriendsRemove, "XOnlineFriendsRemove"},
    {0x157BAEu, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0x56, 0x57}, (void *)ll_XOnlineFriendsRequest, "XOnlineFriendsRequest"},
    {0x157BD0u, {0x55, 0x8B, 0xEC, 0xFF, 0x75, 0x18, 0x8B, 0x0D}, (void *)ll_XOnlineFriendsGameInvite, "XOnlineFriendsGameInvite"},
    {0x157BF1u, {0x55, 0x8B, 0xEC, 0xFF, 0x75, 0x18, 0x8B, 0x0D}, (void *)ll_XOnlineFriendsRevokeGameInvite, "XOnlineFriendsRevokeGameInvite"},
    {0x157C12u, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0xE9, 0xDF}, (void *)ll_XOnlineFriendsAnswerRequest, "XOnlineFriendsAnswerRequest"},
    {0x157C1Du, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0xE9, 0xC0}, (void *)ll_XOnlineFriendsAnswerGameInvite, "XOnlineFriendsAnswerGameInvite"},
    {0x157C28u, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0xE9, 0xEF}, (void *)ll_XOnlineFriendsJoinGame, "XOnlineFriendsJoinGame"},
    {0x157C33u, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0xE9, 0xAE}, (void *)ll_XOnlineFriendsGetAcceptedGameInvite, "XOnlineFriendsGetAcceptedGameInvite"},
    {0x157C3Eu, {0x55, 0x8B, 0xEC, 0x8B, 0x0D, 0x78, 0xAE, 0x73}, (void *)ll_XOnlineMutelistGet, "XOnlineMutelistGet"},
    {0x157C4Du, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0xE9, 0xF0}, (void *)ll_XOnlineMutelistStartup, "XOnlineMutelistStartup"},
    {0x157C58u, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0x56, 0x57}, (void *)ll_XOnlineMutelistAdd, "XOnlineMutelistAdd"},
    {0x157C7Au, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0x56, 0x57}, (void *)ll_XOnlineMutelistRemove, "XOnlineMutelistRemove"},
    {0x157C9Cu, {0x55, 0x8B, 0xEC, 0x56, 0x57, 0xFF, 0x75, 0x24}, (void *)ll_XOnlineFeedbackSend, "XOnlineFeedbackSend"},
    {0x157CCCu, {0x8B, 0x0D, 0x78, 0xAE, 0x73, 0x00, 0xE9, 0x4E}, (void *)ll_XOnlineVerifyNickname, "XOnlineVerifyNickname"},
};

static void ll_log_line(const char *m) /* W491: Logzeilen der Kampagnen-Bruecke (Netz-Thread) */
{
    HLOG_I("%s", m);
}

int loc_live_install(loctun *tun)
{
    unsigned i, n = (unsigned)(sizeof k_live / sizeof k_live[0]);
    if (!g_cfg.loc_live) return 0;
    for (i = 0; i < n; i++) {
        if (memcmp((const void *)(uintptr_t)k_live[i].va, k_live[i].expect, 8) != 0) {
            HLOG_E("LOCBYTES LOCLIVE %s @0x%X: Byte-Mismatch - ABBRUCH (exit 7)", k_live[i].name, k_live[i].va);
            hlog_close();
            ExitProcess(7);
        }
    }
    s_tun = tun;
    for (i = 0; i < n; i++) {
        uint8_t *p = (uint8_t *)(uintptr_t)k_live[i].va;
        intptr_t rel = (uint8_t *)k_live[i].handler - (p + 5);
        DWORD old;
        VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
        p[0] = 0xE9;
        memcpy(p + 1, &rel, 4);
        VirtualProtect(p, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 5);
    }
    s_on = 1;
    HLOG_I("LOCLIVE: Xbox Live bereit - %u XOnline-Einstiege umgelenkt (steelnet als Live-Dienst, %s); System Link "
           "unveraendert, die Live-HLE greift erst, wenn im Spiel Xbox Live gewaehlt wird (W501)", n,
           !tun ? "OHNE Transport: Logon schlaegt fehl"
                : loctun_account(tun) ? "mit Konto" : "ohne Konto: Logon schlaegt fehl");
    return 1;
}

/* W501 ONLINE-BOTH: Kampagnen-Bruecke (W491) erst, wenn das Spiel Xbox Live betritt - im System-Link-Betrieb keine
 * Verbindung zum Kampagnen-Zugang und kein Ticket. Einmal je Prozess (die Bruecke bleibt danach bestehen). */
static void ll_camp_start(void)
{
    loctun *tun = s_tun;
    if (InterlockedExchange(&s_camp_tried, 1) != 0) return;
    if (g_cfg.loc_camp && tun && loctun_account(tun)) { /* W491 CAMP-CLIENT */
        loccamp_cfg cc;
        memset(&cc, 0, sizeof cc);
        strcpy_s(cc.campaign, sizeof cc.campaign, loccamp_campaign_id(g_cfg.loc_camp_id, &cc.port));
        strcpy_s(cc.host, sizeof cc.host, g_cfg.loc_camp_host);
        if (g_cfg.loc_camp_port) cc.port = (uint16_t)g_cfg.loc_camp_port;
        strcpy_s(cc.spki, sizeof cc.spki, g_cfg.loc_camp_spki);
        if (loccamp_bridge_start(tun, &cc, ll_log_line)) locnet_set_camp_redirect(loccamp_bridge_redirect);
        else HLOG_W("LOCCAMP: Bruecke nicht gestartet - Campaign bleibt grau");
    } else {
        HLOG_I("LOCCAMP: Kampagne aus (%s) - Campaign bleibt grau",
               !g_cfg.loc_camp ? "loc_camp=0" : !tun ? "kein Transport" : "ohne Konto");
    }
}

static void ll_flow_enter(const char *via)
{
    if (!s_on || InterlockedExchange(&s_flow, 1) != 0) return;
    if (s_tun) loctun_live_mode(s_tun, 1);
    HLOG_I("LOCLIVE: Spiel betritt Xbox Live (%s) - Live-HLE aktiv, System Link ruht", via);
    ll_camp_start();
}

static void ll_flow_leave(const char *why)
{
    if (InterlockedExchange(&s_flow, 0) == 0) return;
    memset(s_logon, 0, sizeof s_logon);
    s_pad = -1;
    s_own = 0;
    SecureZeroMemory(s_own_sid, sizeof s_own_sid);
    if (s_tun) loctun_live_mode(s_tun, 0);
    HLOG_I("LOCLIVE: Xbox Live verlassen (%s) - zurueck zu System Link", why);
}

void loc_live_syslink_seen(void) /* W501 */
{
    ll_flow_leave("System Link gewaehlt (Port 1201 gebunden)");
}

int loc_live_active(void)
{
    return s_on && s_flow; /* W501: nur im Xbox-Live-Fluss (QoS-Werte des Live-Wegs; System Link unveraendert) */
}

/* ---------- W485 LIVE-QOS: Aufrufzaehler (nur Zahlen) und QoS-Log ---------- */
static volatile LONG s_cnt[LLC_N];
static LONG s_cnt_logged[LLC_N];
static const char *const k_cnt_name[LLC_N] = {
    "NotificationSetState", "GetNotification", "FriendsGetAcceptedGameInvite", "FriendsStartup", "FriendsEnumerate",
    "FriendsEnumerateFinish", "FriendsGetLatest", "FriendsGetTitleName", "FriendsRemove", "FriendsRequest",
    "FriendsGameInvite", "FriendsRevokeGameInvite", "FriendsAnswerRequest", "FriendsAnswerGameInvite",
    "FriendsJoinGame", "MutelistStartup", "MutelistGet", "MutelistAdd", "MutelistRemove", "FeedbackSend",
    "VerifyNickname", "TitleUpdate", "TitleIdIsSameTitle", "QosLookup", "QosListen", "QosRelease"};

void loc_live_count(int which)
{
    if (s_on && which >= 0 && which < LLC_N) InterlockedIncrement(&s_cnt[which]);
}

long loc_live_count_get(int which)
{
    return which >= 0 && which < LLC_N ? (long)s_cnt[which] : -1;
}

void loc_live_log_counts(int final)
{
    char b[900];
    int i, o, changed = 0, any = 0;
    if (!s_on) return;
    o = _snprintf_s(b, sizeof b, _TRUNCATE, "LOCLIVE: Aufrufe%s:", final ? " (Ende)" : "");
    for (i = 0; i < LLC_N && o > 0 && o < (int)sizeof b - 48; i++) {
        LONG v = s_cnt[i];
        if (v != s_cnt_logged[i]) changed = 1;
        s_cnt_logged[i] = v;
        if (v == 0) continue;
        any = 1;
        o += _snprintf_s(b + o, sizeof b - (size_t)o, _TRUNCATE, " %s=%ld", k_cnt_name[i], (long)v);
    }
    if (!any) strcat_s(b, sizeof b, " keine");
    if (changed || final) HLOG_I("%s", b);
    { /* W501 DIAG: Tasks je Art (1 Logon, 2 Friends, 3 FriendsEnum, 4 Mutelist, 5 sofort, 6 Netz) - nur Zahlen */
        int k, ch = 0;
        for (k = 0; k < LLT_KINDS; k++) {
            if (s_tlog[k] != s_tcont[k] || s_tlog[LLT_KINDS + k] != s_tclose[k]) ch = 1;
            s_tlog[k] = s_tcont[k];
            s_tlog[LLT_KINDS + k] = s_tclose[k];
        }
        if (ch || final)
            HLOG_I("LOCLIVE: Tasks Continue L%ld F%ld E%ld M%ld D%ld N%ld / Close L%ld F%ld E%ld M%ld D%ld N%ld",
                   s_tcont[1], s_tcont[2], s_tcont[3], s_tcont[4], s_tcont[5], s_tcont[6], s_tclose[1], s_tclose[2],
                   s_tclose[3], s_tclose[4], s_tclose[5], s_tclose[6]);
    }
}

static const char *ll_qos_src(int src)
{
    switch (src) {
    case LOCLIVE_QOS_SRC_DIRECT: return "direkt";
    case LOCLIVE_QOS_SRC_RELAY: return "Relay";
    case LOCLIVE_QOS_SRC_EST: return "Schaetzung";
    case LOCLIVE_QOS_SRC_SERVICE: return "Dienst";
    default: return "Vorgabe";
    }
}

void loc_live_qos_listen(uint32_t flags, uint32_t bps)
{
    static volatile LONG n;
    LONG k = InterlockedIncrement(&n);
    if (k <= 4 || k % 50 == 0)
        HLOG_I("LOCLIVE-QOS: XNetQosListen #%ld Flags 0x%X (%s%s%s) %u bit/s", (long)k, flags,
               (flags & 1u) ? "an " : "", (flags & 2u) ? "aus " : "", (flags & 0x10u) ? "frei" : "", bps);
}

void loc_live_qos_note(uint32_t cxna, uint32_t cina, uint32_t probes, const loclive_qos_val *v, unsigned n, long seq)
{
    char b[600];
    unsigned i;
    int o;
    if (!(seq <= 6 || seq % 50 == 0) || !v) return;
    o = _snprintf_s(b, sizeof b, _TRUNCATE, "LOCLIVE-QOS: Lookup #%ld (%u Hosts, %u Dienste, %u Proben):", seq, cxna,
                    cina, probes);
    for (i = 0; i < n && i < 10 && o > 0 && o < (int)sizeof b - 80; i++) {
        uint8_t e[LOCLIVE_QOS_INFO_SIZE];
        loclive_qos_entry(e, probes, &v[i]);
        o += _snprintf_s(b + o, sizeof b - (size_t)o, _TRUNCATE, " [%s %s RTT %d/%d ms, %u bit/s, Klasse %d]",
                         i < cxna ? "Host" : "Dienst", ll_qos_src(v[i].src), v[i].rtt_min, v[i].rtt_med,
                         loclive_qos_bps(v[i].src), loclive_qos_class(e));
    }
    HLOG_I("%s", b);
}
