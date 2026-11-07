/* watch_dr.c - WATCHDR (W41): Daten-Watchpoints per x86-Debug-Register.
 *
 * Bis zu 4 Adressen (DR0-DR3) auf Schreiben (:w, Default) oder Lesen/
 * Schreiben (:rw) ueberwachen. Der VEH (emu_main.c) faengt
 * EXCEPTION_SINGLE_STEP (WOW64: 0x4000001E) und loggt pro Treffer EIP,
 * alten/neuen Wert, Ruecksprungkette (EBP-Walk + Stack-Scan, je max 5)
 * und Frame. Deckel: jeder EIP einmal pro Slot (max 64 EIPs), Zaehler
 * weiter; Summenzeile alle 500 Treffer.
 *
 * Bewaffnung laeuft IMMER remote (Suspend/SetThreadContext/Resume) ueber
 * einen eigenen Service-Thread: Selbst-Bewaffnung (SetThreadContext auf
 * den laufenden eigenen Thread) meldet Erfolg, wirkt aber unter WOW64
 * nicht (Beleg: W41_mission, Timer-Treffer nur von remote-bewaffneten
 * Threads). Der Service-Thread ist nie selbst Ziel. suspend/resume
 * balanciert (wie Watchdog-Sampling), zwischen Suspend und Resume keine
 * Sperren (nur Set/GetThreadContext) -> kein neuer SUSPSAFE-Deadlock
 * (W10). Eigene Sonden-Faults (:rw-Shadow-Reads im Handler) konsumiert
 * der VEH-Einsprung still (s. emu_main.c). watch_dr ist der einzige
 * DR-Schreiber (M2-DR-Probes sind entfernt, M4).
 */
#include "watch_dr.h"
#include "hlog.h"
#include "hcfg.h"
#include "kernel_hle.h"
#include "xdk_hooks.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define WDR_MAX 4
#define WDR_EIPS 64
#define WDR_TAB_ADDR 0x351828u /* u32, Entity-Tabellen-Zeiger (Heap) */
#define WDR_STRIDE 0x5Cu
#define WDR_XBE_LO 0x12000u /* XBE-Bereich f. Stack-Scan (wie symbolize.py) */
#define WDR_XBE_HI 0x700000u

typedef struct {
    int used;          /* Slot aus cfg belegt */
    int armed;         /* Adresse aufgeloest + in DR programmiert */
    int is_ent;        /* relativ: Entity-Feld */
    unsigned idx, off; /* ent: IDX+OFF */
    unsigned addr;     /* aufgeloeste Adresse (absolut: aus cfg) */
    unsigned len;      /* 1/2/4 (effektiv, nach Alignment-Fix) */
    int is_rw;         /* 1 = lesen+schreiben, 0 = nur schreiben */
    unsigned shadow;   /* letzter gesehener Wert (len Bytes) */
    int have_shadow;
    volatile LONG hits;
    volatile LONG sum_logged; /* letzter per SUM gemeldeter Stand */
    unsigned eips[WDR_EIPS];
    volatile LONG eip_n[WDR_EIPS];
    int neip;
    volatile LONG eip_over; /* Treffer jenseits der 64 EIPs */
    char spec[64];     /* Original-Token fuer Logs */
} wdr_slot_t;

static wdr_slot_t g_slots[WDR_MAX];
static int g_active;   /* >=1 Slot aus cfg */
static int g_has_ent;  /* >=1 ent:-Slot (braucht Tick) */
static unsigned g_ent_base = 0xFFFFFFFFu; /* zuletzt bewaffnete Basis */
static volatile LONG g_arm_seq;
static HANDLE g_evt;   /* Service-Thread-Wecker (Auto-Reset) */

static unsigned wdr_read(unsigned addr)
{
    unsigned v = 0xFFFFFFFFu;
    __try {
        v = *(volatile unsigned *)(uintptr_t)addr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v = 0xFFFFFFFEu;
    }
    return v;
}

static unsigned wdr_read_val(unsigned addr, unsigned len)
{
    unsigned v = 0xEEEEEEEEu;
    __try {
        if (len == 1) {
            v = *(volatile unsigned char *)(uintptr_t)addr;
        } else if (len == 2) {
            v = *(volatile unsigned short *)(uintptr_t)addr;
        } else {
            v = *(volatile unsigned *)(uintptr_t)addr;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v = 0xEEEEEEEEu;
    }
    return v;
}

/* DR-LEN-Code aus (addr,len): Adresse muss len-ausgerichtet sein
 * (x86-Vorgabe), sonst len schrittweise auf 2/1 kuerzen. */
static void wdr_fix_align(wdr_slot_t *s)
{
    if (s->len >= 4 && (s->addr & 3u) == 0) {
        s->len = 4;
    } else if (s->len >= 2 && (s->addr & 1u) == 0) {
        s->len = 2;
    } else {
        s->len = 1;
    }
}

static unsigned wdr_len_code(unsigned len)
{
    if (len >= 4) return 3u; /* 11 = 4 Bytes */
    if (len == 2) return 1u; /* 01 = 2 Bytes */
    return 0u;               /* 00 = 1 Byte */
}

static int wdr_parse_token(const char *tok, wdr_slot_t *s)
{
    /* Teile an ':' : [ent|ADDR] [IDX+OFF|len|mode] [len|mode] [mode] */
    char b[96], *parts[4];
    int np = 0, i;
    size_t k;
    memset(s, 0, sizeof *s);
    if (strlen(tok) + 1 > sizeof b) return 0;
    strcpy_s(b, sizeof b, tok);
    /* Spaces trimmen */
    for (k = 0; b[k]; k++) {
        if (b[k] == ' ' || b[k] == '\t') {
            memmove(b + k, b + k + 1, strlen(b + k));
            k--;
        }
    }
    if (!b[0]) return 0;
    parts[0] = b;
    np = 1;
    for (k = 0; b[k] && np < 4; k++) {
        if (b[k] == ':') {
            b[k] = '\0';
            parts[np++] = b + k + 1;
        }
    }
    s->len = 4;
    s->is_rw = 0;
    if (_stricmp(parts[0], "ent") == 0) {
        char *plus;
        if (np < 2 || !parts[1][0]) return 0;
        plus = strchr(parts[1], '+');
        s->is_ent = 1;
        s->idx = (unsigned)strtoul(parts[1], NULL, 0);
        s->off = plus ? (unsigned)strtoul(plus + 1, NULL, 0) : 0;
        s->len = 2; /* Entity-Worte (HP +0x0, Akku +0x32) */
        i = 2;
    } else {
        char *end = NULL;
        s->addr = (unsigned)strtoul(parts[0], &end, 0);
        if (!end || end == parts[0]) return 0;
        i = 1;
    }
    for (; i < np; i++) {
        if (_stricmp(parts[i], "rw") == 0 || _stricmp(parts[i], "r") == 0) {
            s->is_rw = 1;
        } else if (_stricmp(parts[i], "w") == 0) {
            s->is_rw = 0;
        } else if (parts[i][0]) {
            unsigned l = (unsigned)strtoul(parts[i], NULL, 0);
            s->len = (l <= 1) ? 1 : (l == 2) ? 2 : 4;
        }
    }
    if (!s->is_ent) wdr_fix_align(s);
    _snprintf_s(s->spec, sizeof s->spec, _TRUNCATE, "%s", tok);
    s->used = 1;
    return 1;
}

/* DR7 aus allen bewaffneten Slots bauen (Ln + RWn + LENn, LE=1). */
static unsigned wdr_build_dr7(void)
{
    unsigned dr7 = (1u << 8); /* LE: exakte Ueberpruefung */
    int i;
    for (i = 0; i < WDR_MAX; i++) {
        if (g_slots[i].used && g_slots[i].armed) {
            dr7 |= (1u << (i * 2)); /* Ln */
            dr7 |= (g_slots[i].is_rw ? 3u : 1u) << (16 + i * 4);
            dr7 |= wdr_len_code(g_slots[i].len) << (18 + i * 4);
        }
    }
    return dr7;
}

/* Einen Thread remote bewaffnen. Rueckgabe: 1 = ok (+Ruecklesung stimmt),
 * 0 = Suspend/lebend-Fehlschlag, -GLE = Set-Fehlschlag, -2000 = DR7-Drift
 * (gesetzt, aber Ruecklesung weicht ab). Lockfrei zwischen Suspend/Resume;
 * Aufrufer loggt. */
static int wdr_arm_remote(void *h)
{
    CONTEXT cx, rb;
    DWORD ec = 0;
    int ok;
    if (!GetExitCodeThread((HANDLE)h, &ec) || ec != STILL_ACTIVE) {
        return 0; /* tot: still ueberspringen (Registry haelt Duplikate) */
    }
    if (SuspendThread((HANDLE)h) == (DWORD)-1) return 0;
    memset(&cx, 0, sizeof cx);
    cx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    cx.Dr0 = g_slots[0].armed ? g_slots[0].addr : 0;
    cx.Dr1 = g_slots[1].armed ? g_slots[1].addr : 0;
    cx.Dr2 = g_slots[2].armed ? g_slots[2].addr : 0;
    cx.Dr3 = g_slots[3].armed ? g_slots[3].addr : 0;
    cx.Dr6 = 0;
    cx.Dr7 = wdr_build_dr7();
    ok = SetThreadContext((HANDLE)h, &cx);
    memset(&rb, 0, sizeof rb);
    rb.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (ok) ok = GetThreadContext((HANDLE)h, &rb);
    ResumeThread((HANDLE)h);
    if (!ok) return -(int)GetLastError();
    if (rb.Dr0 != cx.Dr0 || rb.Dr1 != cx.Dr1 ||
        rb.Dr2 != cx.Dr2 || rb.Dr3 != cx.Dr3 || rb.Dr7 != cx.Dr7) {
        return -2000; /* gesetzt, aber Ruecklesung driftet */
    }
    return 1;
}

/* Alle Ziel-Threads (Host-Main + lebende Xbox-Threads) remote bewaffnen.
 * Laeuft auf dem Service-Thread (nie selbst Ziel) oder auf einem
 * erzeugenden Thread (Ziel = der Neue, immer remote). */
static void wdr_arm_all(const char *why)
{
    int i, n = 0, fail = 0, nfail_log = 0;
    void *mh;
    LONG seq;
    if (!g_active) return;
    seq = InterlockedIncrement(&g_arm_seq);
    mh = krnl_main_thread();
    if (mh) {
        int r = wdr_arm_remote(mh);
        n++;
        if (r != 1) {
            fail++;
            HLOG_W("WATCHDR ARM#%ld: Host-Main r=%d", seq, r);
        }
    }
    for (i = 0; i < krnl_xb_thread_count(); i++) {
        void *h = NULL;
        unsigned rt = 0;
        int r;
        if (!krnl_xb_thread_get(i, &h, &rt) || !h) continue;
        r = wdr_arm_remote(h);
        if (r == 1) {
            n++;
        } else {
            DWORD ec = 0;
            /* Tote still schlucken (nur lebende Fehlschlaege zaehlen). */
            if (GetExitCodeThread((HANDLE)h, &ec) && ec == STILL_ACTIVE) {
                fail++;
                if (nfail_log < 3) {
                    nfail_log++;
                    HLOG_W("WATCHDR ARM#%ld: Thread %d (rt=0x%X) r=%d",
                           seq, i, rt, r);
                }
            }
        }
    }
    for (i = 0; i < WDR_MAX; i++) {
        if (g_slots[i].used) {
            HLOG_I("WATCHDR ARM#%ld: s=%d spec=%s addr=0x%X len=%u mode=%s "
                   "thr=%d fail=%d (%s)", seq, i, g_slots[i].spec,
                   g_slots[i].armed ? g_slots[i].addr : 0, g_slots[i].len,
                   g_slots[i].is_rw ? "rw" : "w", n, fail, why);
        }
    }
}

static DWORD WINAPI wdr_service(LPVOID unused)
{
    (void)unused;
    for (;;) {
        WaitForSingleObject(g_evt, INFINITE);
        wdr_arm_all("service");
    }
    return 0;
}

static void wdr_signal(void)
{
    if (g_evt) SetEvent(g_evt);
}

/* ent:-Adressen aus Tabellenbasis neu berechnen (1 = geaendert). */
static int wdr_resolve_ent(unsigned base)
{
    int i, changed = 0;
    for (i = 0; i < WDR_MAX; i++) {
        wdr_slot_t *s = &g_slots[i];
        unsigned a;
        if (!s->used || !s->is_ent) continue;
        a = base + s->idx * WDR_STRIDE + s->off;
        if (a != s->addr || !s->armed) {
            s->addr = a;
            wdr_fix_align(s);
            s->armed = 1;
            s->shadow = wdr_read_val(s->addr, s->len);
            s->have_shadow = (s->shadow != 0xEEEEEEEEu);
            changed = 1;
        }
    }
    return changed;
}

static void wdr_disarm_ent(void)
{
    int i;
    for (i = 0; i < WDR_MAX; i++) {
        if (g_slots[i].used && g_slots[i].is_ent) {
            g_slots[i].armed = 0;
            g_slots[i].addr = 0;
        }
    }
}

void watch_dr_init(void)
{
    char b[256];
    char *tok, *ctx = NULL;
    int n = 0;
    if (!g_cfg.watch[0]) return; /* aus (Default) */
    strcpy_s(b, sizeof b, g_cfg.watch);
    for (tok = strtok_s(b, ",", &ctx); tok && n < WDR_MAX;
         tok = strtok_s(NULL, ",", &ctx)) {
        if (wdr_parse_token(tok, &g_slots[n])) {
            if (g_slots[n].is_ent) {
                g_has_ent = 1;
            } else {
                g_slots[n].armed = 1;
                g_slots[n].shadow =
                    wdr_read_val(g_slots[n].addr, g_slots[n].len);
                g_slots[n].have_shadow =
                    (g_slots[n].shadow != 0xEEEEEEEEu);
            }
            n++;
        } else {
            HLOG_W("WATCHDR: ignoriere ungueltiges Token '%s'", tok);
        }
    }
    if (n == 0) return;
    g_active = 1;
    HLOG_I("WATCHDR CFG: %d Slot(s) aus watch=%s", n, g_cfg.watch);
    g_evt = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (g_evt) {
        void *th = krnl_CreateThread(wdr_service, NULL, 0, NULL);
        if (th) {
            CloseHandle(th);
            wdr_signal(); /* Erstbewaffnung (remote, via Service) */
        } else {
            HLOG_W("WATCHDR: Service-Thread nicht erstellbar (%lu)",
                   GetLastError());
        }
    }
}

void watch_dr_on_thread(void *h, unsigned long tid)
{
    static volatile LONG nlog;
    int r;
    if (!g_active || !h) return;
    if (tid == GetCurrentThreadId()) {
        wdr_signal(); /* Paranoia: nie selbst (Service macht remote) */
        return;
    }
    r = wdr_arm_remote(h);
    if (InterlockedIncrement(&nlog) <= 12) {
        HLOG_I("WATCHDR THREAD: tid=%lu r=%d", tid, r);
    }
}

void watch_dr_tick(unsigned frame)
{
    unsigned base;
    (void)frame;
    if (!g_active || !g_has_ent) return;
    base = wdr_read(WDR_TAB_ADDR);
    if (base == g_ent_base) return;
    g_ent_base = base;
    if (base < 0x10000u || base >= 0x80000000u ||
        base == 0xFFFFFFFEu) {
        /* Tabelle weg (Missionswechsel): ent:-Slots entwaffnen. */
        wdr_disarm_ent();
        wdr_signal();
        return;
    }
    if (wdr_resolve_ent(base)) {
        wdr_signal();
    }
}

/* Ruecksprungkette per EBP-Walk (SEH-gepanzert, max Tiefe 5). */
static int wdr_retchain(EXCEPTION_POINTERS *ep, unsigned *out, int cap)
{
    unsigned ebp = (unsigned)ep->ContextRecord->Ebp;
    int n = 0, guard = 0;
    __try {
        while (n < cap && guard++ < 8 && ebp && (ebp & 3u) == 0) {
            unsigned ret = *(unsigned *)(uintptr_t)(ebp + 4);
            unsigned neb = *(unsigned *)(uintptr_t)ebp;
            if (!ret) break;
            out[n++] = ret;
            if (!neb || neb == ebp) break;
            ebp = neb;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

/* Stack-Scan: bis zu 5 Werte im XBE-Bereich ab ESP (FPO-Code hat keine
 * EBP-Kette; rohe Ruecksprung-Kandidaten, ggf. mit symbolize.py deuten). */
static int wdr_stackscan(EXCEPTION_POINTERS *ep, unsigned *out, int cap)
{
    unsigned esp = (unsigned)ep->ContextRecord->Esp;
    int n = 0, k;
    __try {
        for (k = 0; k < 64 && n < cap; k++) {
            unsigned v =
                *(volatile unsigned *)(uintptr_t)(esp + (unsigned)k * 4);
            if (v >= WDR_XBE_LO && v < WDR_XBE_HI) {
                out[n++] = v;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

int watch_dr_veh(EXCEPTION_POINTERS *ep)
{
    DWORD code, dr6;
    int i, hit = -1;
    unsigned eip, oldv, newv, frame, chain[5], stkret[5];
    int cn, sn, k, found = -1;
    LONG cnt;
    wdr_slot_t *s;
    if (!g_active) return 0;
    code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_SINGLE_STEP && code != 0x4000001Eu) return 0;
    dr6 = (DWORD)ep->ContextRecord->Dr6;
    for (i = 0; i < WDR_MAX; i++) {
        if (g_slots[i].used && g_slots[i].armed && (dr6 & (1u << i))) {
            hit = i;
            break;
        }
    }
    if (hit < 0) return 0; /* nicht unserer: Legacy-Pfade */
    s = &g_slots[hit];
    eip = (unsigned)ep->ContextRecord->Eip;
    newv = wdr_read_val(s->addr, s->len);
    oldv = s->have_shadow ? s->shadow : 0xEEEEEEEEu;
    s->shadow = newv; /* benignes Rennen (letzter gewinnt) */
    s->have_shadow = 1;
    cnt = InterlockedIncrement(&s->hits);
    ep->ContextRecord->Dr6 = 0; /* sticky Bits loeschen */
    /* Pro EIP einmal loggen (max 64), Zaehler immer. */
    for (k = 0; k < s->neip; k++) {
        if (s->eips[k] == eip) {
            found = k;
            break;
        }
    }
    if (found >= 0) {
        InterlockedIncrement(&s->eip_n[found]);
    } else if (s->neip < WDR_EIPS) {
        s->eips[s->neip] = eip;
        s->eip_n[s->neip] = 1;
        s->neip++;
    } else {
        InterlockedIncrement(&s->eip_over);
        found = -2; /* still zaehlen */
    }
    if (found == -1) {
        /* Neuer EIP: Ketten erst jetzt lesen (teuer). */
        frame = hooks_frame_count();
        cn = wdr_retchain(ep, chain, 5);
        sn = wdr_stackscan(ep, stkret, 5);
        HLOG_W("WATCHDR HIT s=%d spec=%s f=%u EIP=0x%X old=0x%X new=0x%X "
               "cnt=%ld ret=0x%X,0x%X,0x%X,0x%X,0x%X "
               "stkret=0x%X,0x%X,0x%X,0x%X,0x%X",
               hit, s->spec, frame, eip, oldv, newv, cnt,
               cn > 0 ? chain[0] : 0, cn > 1 ? chain[1] : 0,
               cn > 2 ? chain[2] : 0, cn > 3 ? chain[3] : 0,
               cn > 4 ? chain[4] : 0,
               sn > 0 ? stkret[0] : 0, sn > 1 ? stkret[1] : 0,
               sn > 2 ? stkret[2] : 0, sn > 3 ? stkret[3] : 0,
               sn > 4 ? stkret[4] : 0);
    }
    if (cnt - s->sum_logged >= 500) {
        s->sum_logged = cnt;
        HLOG_I("WATCHDR SUM s=%d spec=%s hits=%ld eips=%d over=%ld "
               "lastEIP=0x%X lastOld=0x%X lastNew=0x%X", hit, s->spec,
               cnt, s->neip, s->eip_over, eip, oldv, newv);
    }
    return 1;
}
