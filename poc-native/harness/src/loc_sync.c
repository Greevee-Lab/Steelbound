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
static unsigned long long s_hash = 1469598103934665603ull;
static u32 s_last_tick;
static u32 s_desync_logged;

/* W297 TEST loc_replay_tick: "rahmen:vk,len;..." (len in Bildern), aufsteigend sortiert erwartet */
#define LS_RT_MAX 96
static struct { LONG at; int vk, len; } s_rt[LS_RT_MAX];
static int s_rt_n, s_rt_cur;

static void ls_rt_parse(const char *p)
{
    while (*p && s_rt_n < LS_RT_MAX) {
        long at;
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
        while (*p && *p != ';') p++;
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
 *   sync>=K (Lockstep-Rahmen), f (sofort). +N = N Bilder nach dem Ausloesen. vk 0 = Bild,
 *   vk 1 = Draw-Zensus (DIAG). */
#define LS_ST_MAX 48
enum { LST_F, LST_ACC, LST_CON, LST_RX, LST_TX, LST_SYNC };
static struct { int kind; long arg; int delay, vk, len; } s_st[LS_ST_MAX];
static int s_st_n, s_st_cur, s_st_hit;
static unsigned s_st_hitf;

static void ls_st_parse(const char *p)
{
    while (*p && s_st_n < LS_ST_MAX) {
        char tok[96];
        unsigned k = 0;
        const char *q;
        int kind = -1;
        long arg = 0;
        while (*p == ' ' || *p == ';') p++;
        if (!*p) break;
        while (*p && *p != ';' && k < sizeof tok - 1) tok[k++] = *p++;
        tok[k] = 0;
        if (strncmp(tok, "acc>=", 5) == 0) { kind = LST_ACC; arg = strtol(tok + 5, NULL, 0); }
        else if (strncmp(tok, "con>=", 5) == 0) { kind = LST_CON; arg = strtol(tok + 5, NULL, 0); }
        else if (strncmp(tok, "rx:", 3) == 0) { kind = LST_RX; arg = strtol(tok + 3, NULL, 0); }
        else if (strncmp(tok, "tx:", 3) == 0) { kind = LST_TX; arg = strtol(tok + 3, NULL, 0); }
        else if (strncmp(tok, "sync>=", 6) == 0) { kind = LST_SYNC; arg = strtol(tok + 6, NULL, 0); }
        else if (tok[0] == 'f') { kind = LST_F; }
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
        s_st_n++;
    }
}

void loc_sync_frame(unsigned f)
{
    while (s_st_cur < s_st_n) {
        int hit = 0;
        long acc = 0, con = 0;
        if (!s_st_hit) {
            switch (s_st[s_st_cur].kind) {
            case LST_F: hit = 1; break;
            case LST_ACC: hit = loc_net_test_state(&acc, &con, 0, 256) && acc >= s_st[s_st_cur].arg; break;
            case LST_CON: hit = loc_net_test_state(&acc, &con, 0, 256) && con >= s_st[s_st_cur].arg; break;
            case LST_RX: hit = loc_net_test_state(NULL, NULL, 0, (unsigned)s_st[s_st_cur].arg) == 2; break;
            case LST_TX: hit = loc_net_test_state(NULL, NULL, 1, (unsigned)s_st[s_st_cur].arg) == 2; break;
            case LST_SYNC: hit = s_frames >= s_st[s_st_cur].arg; break;
            }
            if (!hit) return;
            s_st_hit = 1;
            s_st_hitf = f;
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

/* cdecl, aus dem Thunk (alle Register gesichert). Nur Ganzzahlarithmetik: der x87-Stapel des
 * Spiels bleibt unberuehrt. */
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
    n = InterlockedIncrement(&s_frames);
    s_hash = fnv_u32(fnv_u32(s_hash, acc), tick);
    s_last_tick = tick;
    while (s_rt_cur < s_rt_n && s_rt[s_rt_cur].at <= n) { /* Testtasten am Lockstep-Rahmen */
        replay_inject(s_rt[s_rt_cur].vk, s_rt[s_rt_cur].len);
        s_rt_cur++;
    }
    if (desync && !s_desync_logged) {
        s_desync_logged = 1;
        HLOG_W("LOCSYNC: DESYNC vom Spiel erkannt bei Rahmen %ld Tick %u", n, tick);
    }
    if (g_cfg.loc_sync_every > 0 && n % g_cfg.loc_sync_every == 0) {
        HLOG_I("LOCSYNC: Rahmen=%ld Tick=%u Hash=%016llX Stau=%u Stauframes=%u Stauepisoden=%u Verz=%u "
               "Spieler=%u Maske=0x%X f=%u",
               n, tick, s_hash, stall, stallf, stalle, delay, npl, act, hooks_frame_count());
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
    }
    HLOG_I("LOCSYNC: aktiv (Zustandsakkumulator je Lockstep-Rahmen, Log alle %d Rahmen, nur Hash)",
           g_cfg.loc_sync_every);
    return 1;
}

void loc_sync_tick(int waited_s)
{
    if (s_on && waited_s % 30 == 0) {
        HLOG_I("LOCSYNC: t=%ds Rahmen=%ld Tick=%u Hash=%016llX", waited_s, s_frames, s_last_tick, s_hash);
    }
}
