/* sb_ally.c - W424 SB-ALLIED-SQUADS: verbuendete Trupps kaempfen auch fern vom
 * Spieler (nur Steel Battalion; Logik sb_ally_core.h, Belege BERICHT_M2 "W424").
 *
 * Idee: oeffentliche "SB1 Patch Dev Notes" eines Community-Patches (LGPL 2.1).
 * Eigene Umsetzung, kein fremder Code.
 *
 * Spielstelle (EU-Adressen, US per Bytevergleich, s. kSite):
 *   0x5CC90  Einheitenschleife je Spielbild (Zaehler [0x4C9E28]++ am Ende,
 *            Missionsanfang 0x5D4C0 setzt ihn auf 0)
 *   0x5CED9  einziger Aufruf  call 0x5BF80(Einheit) -> AL 1 = simulieren,
 *            sonst Slot-Flag 0x20 (+0x48) = eingefroren
 *   0x5BF80  Slot [Einheit+0x48]: +0x28 & 0x40000 = Trupp-Mitglied (folgt dem
 *            Anfuehrer), sonst Zelle (+0x3D4 x, +0x3D6 z) im Rechteck
 *            [0x4C9E24..26] x / [0x4C9E20..22] z (Spielerzelle +-10)
 *   Slots [0x351828] (je 0x5C, Anzahl [0x356E2E] ab Spieler [0x356E2C]),
 *   Einheiten [0x4CA034] (je 0x4E0, Index Slot+0x34), verbuendet = Slot+0x28
 *   & 0x80000, tot/leer = Einheit+0x38 & 2 bzw. Slot-Wort 0 == 0.
 *
 * Der Thunk ersetzt nur das call-Ziel (5 Bytes), ruft das Original zuerst und
 * entscheidet nur bei "eingefroren" zusaetzlich. W426: Verbuendete erst, wenn
 * das Original sie einmal aktiv geschaltet hat ("gesehen"), danach fuer den
 * Rest der Mission; keine globale Freigabe mehr. Ohne Option bleibt der
 * Spielcode unveraendert (kein Patch). Die Einstellung wird je Mission beim
 * ersten Spielbild festgehalten; in Replays gilt die beim Aufzeichnen
 * vermerkte Einstellung (Begleitdatei, s. Replay-Teil unten). */
#include "hooks_int.h"
#include "sb_ally.h"
#include "sb_ally_core.h"

#define RD32(a) (*(volatile u32 *)(uintptr_t)(a))
#define RD16S(a) (*(volatile short *)(uintptr_t)(a))
#define RD16(a) (*(volatile u16 *)(uintptr_t)(a))
#define RD8(a) (*(volatile uint8_t *)(uintptr_t)(a))
#define RDF(a) (*(volatile float *)(uintptr_t)(a))

/* EU-Datenadressen (US: SBA = +0x740, im Code der US-Fassung nachgeprueft). */
#define A_SLOTS 0x351828u
#define A_PIDX 0x356E2Cu
#define A_COUNT 0x356E2Eu
#define A_UNITS 0x4CA034u
#define A_FRAME 0x4C9E28u
#define A_RECT 0x4C9E20u
#define A_DEMO 0x3B8CDCu  /* u32 1 = Titel-Demo oder Replay-Wiedergabe */
#define A_TDEMO 0x670E9Cu /* u32 1 = Titel-Demo */

typedef struct {
    int prof;
    u32 call;   /* Adresse des call-Befehls (E8) */
    u32 target; /* Original 0x5BF80 */
} ally_site_t;

static const ally_site_t kSite[] = {
    { SB_PROFILE_EU, 0x5CED9u, 0x5BF80u },
    { SB_PROFILE_US, 0x5D489u, 0x5C530u }, /* US: Block 0x5B0 weiter (gleiche Bytes) */
};

/* Erste 34 Bytes des Originals (ohne Adressen) nur als FNV-1a-32 (keine
 * Spielbytes im Quelltext, clean_check); EU und US gleich. */
#define ALLY_TGT_LEN 34u
#define ALLY_TGT_FNV 0x16C43BE4u
/* Umgebung des Aufrufs: push esi; call rel32; add esp,4; cmp al,1; je +6; or [edi+0x48],bx */
static const uint8_t kCallPre = 0x56;
static const uint8_t kCallPost[12] = { 0x83, 0xC4, 0x04, 0x3C, 0x01, 0x74, 0x06, 0x66, 0x09, 0x5F, 0x48, 0xEB };

typedef unsigned char(__cdecl *ally_fn_t)(u32 unit);

static ally_fn_t g_orig;
static const ally_site_t *g_site;
static int g_installed, g_checked, g_bad;
static u32 a_slots, a_pidx, a_count, a_units, a_frame, a_demo, a_tdemo;

/* Missionszustand (nur Spiel-Thread). */
static int g_have;         /* Missionsanfang gesehen */
static u32 g_frame;        /* letzter Bildzaehler */
static int g_on;           /* Zusatzregel in dieser Mission */
static sb_ally_pt_t g_al[SB_ALLY_MAXN]; /* lebende, gesehene Verbuendete (Stand Bildanfang) */
static int g_aln, g_alive;
/* W426: Merker "gesehen" je Slot-Index (Schluessel sb_ally_key, 0 = nie vom
 * Original aktiv geschaltet). Folgt deterministisch aus dem Spielzustand seit
 * dem Missionsanfang (Original-Entscheidung je Einheit und Bild), daher in
 * Replays identisch. Wird auch mit Zusatzregel aus gefuehrt (nur Zaehler). */
static unsigned g_seen[SB_ALLY_SLOTS];
static unsigned g_seen_new; /* neu gemerkt seit Missionsanfang (DIAG) */
/* Zaehler je Bild (DIAG SBALLY) */
static unsigned g_n, g_orig1, g_plus_a, g_plus_e;
static unsigned g_last_n, g_last_orig1, g_last_plus_a, g_last_plus_e;

/* Replays: Einstellung der zuletzt live gespielten Mission (Aufzeichnung) und
 * die beim Oeffnen eines Replays gefundene Einstellung (Wiedergabe). */
static volatile LONG g_rec_flag;      /* 0/1 */
static volatile LONG g_rec_pending;   /* 1 = naechstes geschriebenes *.rep ist die Aufzeichnung der Live-Mission */
static volatile LONG g_play_flag = -1; /* -1 = kein Replay geoeffnet */
static char g_rep_wpath[1024];         /* Replay mit Schreib-Handle (Kernel-Thread) */
static CRITICAL_SECTION g_rep_cs;
static volatile LONG g_rep_cs_init;

int sb_ally_mission_on(void) { return g_on; }

static int ally_check_site(const ally_site_t *s, char *why, size_t cap)
{
    const uint8_t *c = (const uint8_t *)(uintptr_t)s->call;
    const uint8_t *t = (const uint8_t *)(uintptr_t)s->target;
    int32_t rel;
    u32 d = s->call - 0x5CED9u; /* Versatz der Einheitenschleife gegenueber EU */
    static const struct { u32 eu_op; u32 eu_data; } kOps[] = {
        { 0x5CC97u, A_COUNT }, { 0x5CCA0u, A_PIDX }, { 0x5CCBCu, A_SLOTS }, { 0x5CCDAu, A_UNITS }, { 0x5CF55u, A_FRAME },
    };
    static const unsigned kRectOp[4] = { 0x54u, 0x49u, 0x3Eu, 0x2Cu }; /* RECT+0/+2/+4/+6 im Original */
    unsigned i;
    __try {
        memcpy(&rel, c + 1, 4);
        if (c[-1] != kCallPre || c[0] != 0xE8 || memcmp(c + 5, kCallPost, sizeof kCallPost) != 0 ||
            (u32)(s->call + 5u + (u32)rel) != s->target) {
            _snprintf_s(why, cap, _TRUNCATE, "Aufrufstelle 0x%X passt nicht", s->call);
            return 0;
        }
        uint32_t hf = 2166136261u;
        for (i = 0; i < ALLY_TGT_LEN; i++) {
            hf = (hf ^ t[i]) * 16777619u;
        }
        if (hf != ALLY_TGT_FNV) {
            _snprintf_s(why, cap, _TRUNCATE, "Erstbytes 0x%X passen nicht", s->target);
            return 0;
        }
        for (i = 0; i < 4; i++) {
            u32 op;
            memcpy(&op, t + kRectOp[i], 4);
            if (op != SBA(A_RECT + 2u * i)) {
                _snprintf_s(why, cap, _TRUNCATE, "Rechteck-Adresse @0x%X = 0x%X statt 0x%X", s->target + kRectOp[i], op,
                            SBA(A_RECT + 2u * i));
                return 0;
            }
        }
        for (i = 0; i < sizeof kOps / sizeof kOps[0]; i++) {
            u32 op;
            memcpy(&op, (const uint8_t *)(uintptr_t)(kOps[i].eu_op + d), 4);
            if (op != SBA(kOps[i].eu_data)) {
                _snprintf_s(why, cap, _TRUNCATE, "Datenadresse @0x%X = 0x%X statt 0x%X", kOps[i].eu_op + d, op,
                            SBA(kOps[i].eu_data));
                return 0;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        _snprintf_s(why, cap, _TRUNCATE, "Lesefehler bei 0x%X", s->call);
        return 0;
    }
    return 1;
}

static void ally_new_mission(u32 frame, int mid)
{
    u32 pidx = RD16(a_pidx);
    int demo = RD32(a_demo) == 1, tdemo = RD32(a_tdemo) == 1;
    LONG play = InterlockedExchange(&g_play_flag, -1);
    const char *src;
    if (mid) {
        g_on = 0;
        src = "spaet (ab naechster Mission)";
    } else if (demo && tdemo) {
        g_on = 0;
        InterlockedExchange(&g_rec_pending, 0);
        src = "Titel-Demo (Original)";
    } else if (demo) {
        g_on = play == 1;
        InterlockedExchange(&g_rec_pending, 0);
        src = play < 0 ? "Replay ohne Vermerk (Original)" : "Replay-Vermerk";
    } else {
        g_on = g_cfg.sb_allied_squads ? 1 : 0;
        InterlockedExchange(&g_rec_flag, g_on);
        InterlockedExchange(&g_rec_pending, 1);
        src = "Einstellung";
    }
    g_have = 1;
    memset(g_seen, 0, sizeof g_seen);
    g_seen_new = 0;
    HLOG_I("SBALLY Mission: Zusatzregel %s (%s, f=%u, Spieler-Slot %u)", g_on ? "an" : "aus", src, frame, pidx);
}

/* DIAG SBALLY (nur Lesen, aendert keine Entscheidung): Zaehler des letzten
 * Bilds und Lage der Verbuendeten gegenueber dem Missionsanfang (bewegt =
 * Zelle anders als im ersten Bild, Weg = mittlere Chebyshev-Distanz in Zellen,
 * fern = ausserhalb des Spieler-Rechtecks). Hz = Spielbilder je Host-Sekunde
 * (nur Messzeile, nie Entscheidung). */
#define ALLY_ST_MAX 512
static short g_st_x[ALLY_ST_MAX], g_st_z[ALLY_ST_MAX];
static uint8_t g_st_v[ALLY_ST_MAX];
static int g_st_ok;
static LARGE_INTEGER g_diag_t;
/* Spanne erster..letzter Aufruf je Spielbild ~ Einheitenschleife ohne die
 * Simulation der letzten Einheit (nur mit sb_ally_diag gemessen). */
static LONGLONG g_sp_first, g_sp_last, g_sp_sum, g_sp_max;
static unsigned g_sp_n;
static u32 g_diag_f;

static void ally_diag(u32 frame)
{
    u32 slots = RD32(a_slots), units = RD32(a_units), pidx = RD16(a_pidx), i;
    int cnt = RD16S(a_count), alive = 0, moved = 0, nfar = 0, walk = 0, seen = 0, far_seen = 0, moved_unseen = 0;
    short zlo = RD16S(SBA(A_RECT)), zhi = RD16S(SBA(A_RECT + 2u)), xlo = RD16S(SBA(A_RECT + 4u)),
          xhi = RD16S(SBA(A_RECT + 6u));
    LARGE_INTEGER t, fq;
    double hz = 0.0;
    char lst[400];
    size_t lo = 0;
    lst[0] = 0;
    if (!slots || !units || cnt <= 0) { return; }
    for (i = 1; (int)i < cnt && i < ALLY_ST_MAX; i++) {
        u32 s = slots + (pidx + i) * 0x5Cu, u;
        short x, z;
        int isfar, sn;
        if (RD16(s) == 0 || !(RD32(s + 0x28u) & 0x80000u)) { continue; }
        u = units + RD16(s + 0x34u) * 0x4E0u;
        if (RD8(u + 0x38u) & 2u) { continue; }
        x = RD16S(u + 0x3D4u);
        z = RD16S(u + 0x3D6u);
        isfar = x < xlo || x > xhi || z < zlo || z > zhi;
        sn = sb_ally_seen(g_seen, SB_ALLY_SLOTS, (int)(pidx + i), sb_ally_key(RD16(s + 0x34u)));
        if (!g_st_ok) {
            g_st_x[i] = x; g_st_z[i] = z; g_st_v[i] = 1;
            if (lo + 24 < sizeof lst) { /* W426: Slot@Zelle (T = Trupp-Mitglied, F = fern) fuer Fahrziele */
                int k = _snprintf_s(lst + lo, sizeof lst - lo, _TRUNCATE, " %u@%d,%d%s%s", pidx + i, x, z,
                                    (RD32(s + 0x28u) & 0x40000u) ? "T" : "", isfar ? "F" : "");
                lo = k > 0 ? lo + (size_t)k : sizeof lst;
            }
        }
        alive++;
        if (isfar) { nfar++; }
        if (sn) { seen++; if (isfar) { far_seen++; } }
        if (g_st_v[i] && (x != g_st_x[i] || z != g_st_z[i])) {
            int dx = abs(x - g_st_x[i]), dz = abs(z - g_st_z[i]);
            moved++;
            walk += dx > dz ? dx : dz;
            if (!sn) { moved_unseen++; }
        }
    }
    if (!g_st_ok) {
        g_st_ok = 1;
        HLOG_I("SBALLY Start f=%u: Verbuendete=%d, davon fern=%d (Spieler-Rechteck x %d..%d z %d..%d)", frame, alive, nfar,
               xlo, xhi, zlo, zhi);
        HLOG_I("SBALLY Verbuendete (Slot@x,z):%s", lst);
        QueryPerformanceCounter(&g_diag_t);
        g_diag_f = frame;
        return;
    }
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&fq);
    if (t.QuadPart > g_diag_t.QuadPart && frame > g_diag_f) {
        hz = (double)(frame - g_diag_f) * (double)fq.QuadPart / (double)(t.QuadPart - g_diag_t.QuadPart);
    }
    g_diag_t = t;
    g_diag_f = frame;
    HLOG_I("SBALLY f=%u an=%d Einheiten=%u orig_aktiv=%u +verb=%u +feind=%u | Verbuendete=%d fern=%d gesehen=%d "
           "(fern %d, gemerkt %u) bewegt=%d (ungesehen %d) Weg=%.1f Zellen | %.1f Spielbilder/s | Schleife %.3f ms "
           "(max %.3f)",
           frame, g_on, g_last_n, g_last_orig1, g_last_plus_a, g_last_plus_e, alive, nfar, seen, far_seen, g_seen_new,
           moved, moved_unseen, moved ? (double)walk / moved : 0.0, hz,
           g_sp_n ? (double)g_sp_sum * 1000.0 / g_sp_n / fq.QuadPart : 0.0, (double)g_sp_max * 1000.0 / fq.QuadPart);
    g_sp_sum = g_sp_max = 0;
    g_sp_n = 0;
}

/* Erstes Thunk-Aufruf eines Spielbilds: Merker "gesehen" abgleichen (Tod,
 * leerer oder neu belegter Slot -> geloescht) und Liste der lebenden,
 * gesehenen Verbuendeten (Stand vor der Simulation dieses Bilds). */
static void ally_new_frame(u32 frame)
{
    u32 slots, units, pidx, i;
    int cnt;
    if (!g_have || frame < g_frame || frame == 0) {
        ally_new_mission(frame, g_have == 0 && frame > 1);
        g_st_ok = 0;
    }
    g_frame = frame;
    g_last_n = g_n; g_last_orig1 = g_orig1; g_last_plus_a = g_plus_a; g_last_plus_e = g_plus_e;
    g_n = g_orig1 = g_plus_a = g_plus_e = 0;
    if (g_cfg.sb_ally_diag && (!g_st_ok || (frame % 300u) == 0)) {
        ally_diag(frame);
    }
    g_aln = 0;
    g_alive = 0;
    slots = RD32(a_slots);
    units = RD32(a_units);
    pidx = RD16(a_pidx);
    cnt = RD16S(a_count);
    if (!slots || !units) { return; }
    for (i = 1; (int)i < cnt; i++) {
        u32 s = slots + (pidx + i) * 0x5Cu, u;
        int idx = (int)(pidx + i), alive;
        unsigned key;
        if (idx >= SB_ALLY_SLOTS) { break; }
        if (!(RD32(s + 0x28u) & 0x80000u)) { /* nicht (mehr) verbuendet */
            g_seen[idx] = 0;
            continue;
        }
        key = sb_ally_key(RD16(s + 0x34u));
        u = units + RD16(s + 0x34u) * 0x4E0u;
        alive = RD16(s) != 0 && !(RD8(u + 0x38u) & 2u);
        if (!sb_ally_keep(g_seen, SB_ALLY_SLOTS, idx, key, alive)) { continue; }
        g_alive++;
        if (g_aln < SB_ALLY_MAXN) {
            g_al[g_aln].x = RD16S(u + 0x3D4u);
            g_al[g_aln].z = RD16S(u + 0x3D6u);
            g_aln++;
        }
    }
}

/* Slot-Index (ab Tabellenanfang) eines Slot-Zeigers, -1 = ausserhalb. */
static int ally_slot_idx(u32 slot)
{
    u32 b = RD32(a_slots), d;
    if (!b || slot < b) { return -1; }
    d = slot - b;
    if (d % 0x5Cu) { return -1; }
    d /= 0x5Cu;
    return d < SB_ALLY_SLOTS ? (int)d : -1;
}

static unsigned char __cdecl ally_thunk(u32 unit)
{
    unsigned char r = g_orig(unit);
    u32 frame = RD32(a_frame), slot;
    LARGE_INTEGER t;
    if (g_cfg.sb_ally_diag) {
        QueryPerformanceCounter(&t);
    }
    if (!g_have || frame != g_frame) {
        if (g_cfg.sb_ally_diag) {
            if (g_sp_first && g_sp_last >= g_sp_first) {
                LONGLONG d = g_sp_last - g_sp_first;
                g_sp_sum += d;
                g_sp_n++;
                if (d > g_sp_max) { g_sp_max = d; }
            }
            g_sp_first = t.QuadPart;
        }
        ally_new_frame(frame);
    }
    if (g_cfg.sb_ally_diag) {
        g_sp_last = t.QuadPart;
    }
    g_n++;
    slot = RD32(unit + 0x48u);
    if (r == 1) {
        g_orig1++;
        /* W426: Original schaltet eine verbuendete Einheit aktiv (Rechteck um den
         * Spieler bzw. Trupp-Mitglied eines aktiven Anfuehrers) -> gesehen. */
        if (slot && (RD32(slot + 0x28u) & 0x80000u) && ally_slot_idx(slot) != (int)RD16(a_pidx) && /* Spieler nicht */
            sb_ally_mark(g_seen, SB_ALLY_SLOTS, ally_slot_idx(slot), sb_ally_key(RD16(slot + 0x34u)), frame)) {
            g_seen_new++;
            if (g_cfg.sb_ally_diag && g_seen_new <= 64u) {
                HLOG_I("SBALLY gesehen: Slot %d (Einheit %u, Zelle %d,%d%s) f=%u", ally_slot_idx(slot),
                       (unsigned)RD16(slot + 0x34u), RD16S(unit + 0x3D4u), RD16S(unit + 0x3D6u),
                       (RD32(slot + 0x28u) & 0x40000u) ? ", Trupp-Mitglied" : "", frame);
            }
        }
        return r;
    }
    if (!g_on || !slot) { return r; }
    {
        u32 fl = RD32(slot + 0x28u);
        int member = (fl & 0x40000u) != 0, ally = (fl & 0x80000u) != 0;
        int seen = ally && sb_ally_seen(g_seen, SB_ALLY_SLOTS, ally_slot_idx(slot), sb_ally_key(RD16(slot + 0x34u)));
        if (sb_ally_extra(member, ally, seen, RD16S(unit + 0x3D4u), RD16S(unit + 0x3D6u), g_al, g_aln,
                          SB_ALLY_RANGE)) {
            if (ally) { g_plus_a++; } else { g_plus_e++; }
            return 1;
        }
    }
    return r;
}

static void ally_do_install(const char *when)
{
    char why[160];
    unsigned i;
    uint8_t *p;
    intptr_t rel;
    DWORD old;
    if (g_installed || g_bad || g_sba_loc) { return; }
    if (!g_checked) {
        g_checked = 1;
        g_site = NULL;
        why[0] = 0;
        for (i = 0; i < sizeof kSite / sizeof kSite[0]; i++) {
            if (kSite[i].prof == sb_profile_id()) {
                g_site = &kSite[i];
                break;
            }
        }
        if (!g_site) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Fassung %s nicht unterstuetzt", sb_profile_name());
        } else if (!ally_check_site(g_site, why, sizeof why)) {
            g_site = NULL;
        }
        if (!g_site) {
            g_bad = 1;
            HLOG_W("SBALLY: unbekannte default.xbe (%s) - kein Eingriff, Originalverhalten", why);
            return;
        }
    }
    a_slots = SBA(A_SLOTS); a_pidx = SBA(A_PIDX); a_count = SBA(A_COUNT); a_units = SBA(A_UNITS);
    a_frame = SBA(A_FRAME); a_demo = SBA(A_DEMO); a_tdemo = SBA(A_TDEMO);
    g_orig = (ally_fn_t)(uintptr_t)g_site->target;
    p = (uint8_t *)(uintptr_t)g_site->call;
    rel = (uint8_t *)ally_thunk - (p + 5);
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    g_installed = 1;
    HLOG_I("SBALLY: Thunk @0x%X (Ziel 0x%X, %s, %s)", g_site->call, g_site->target, sb_profile_name(), when);
}

void sb_ally_install(void)
{
    if (!g_rep_cs_init) {
        InitializeCriticalSection(&g_rep_cs);
        g_rep_cs_init = 1;
    }
    if (g_sba_loc) { return; }
    if (g_cfg.sb_allied_squads) {
        ally_do_install("beim Start");
    } else if (g_cfg.sb_ally_diag) { /* DIAG: nur messen, Entscheidungen bleiben Original (Zusatzregel aus) */
        ally_do_install("nur Messung (sb_ally_diag, Zusatzregel aus)");
    } else {
        HLOG_I("SBALLY: aus (sb_allied_squads=0) - Spielcode unveraendert");
    }
}

void sb_ally_live(void)
{
    if (g_installed || g_bad || g_sba_loc || !g_cfg.sb_allied_squads) { return; }
    ally_do_install("im Spiel eingeschaltet, wirkt ab naechster Mission");
}

/* ---- Replays: Begleitdatei <hdd_root>\steelbound_replays.txt ----
 * Zeile "<fnv64 der Replay-Daten ohne Signatur> <0|1>". Eintrag beim Schliessen
 * des ersten nach einem Live-Missionsanfang geschriebenen *.rep (die Aufzeichnung,
 * ZCACHE\st00_0.rep "Last Replay") mit der Einstellung dieser Mission. Spaetere
 * Schreibvorgaenge tragen nichts ein: Die Titel-Demo kopiert ihr Replay von der
 * Disc ebenfalls nach ZCACHE, gespeicherte Replays (UDATA) haben dieselben Daten
 * wie die Aufzeichnung und finden deren Eintrag.
 * Beim lesenden Oeffnen: gefunden -> Wert, sonst Original (aus). Keine
 * Eingaben, nur dieses Bit. */
static int ally_rep_hash(const char *host, unsigned long long *h, unsigned long long *h_all)
{
    HANDLE f = CreateFileA(host, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER sz;
    unsigned char *b;
    DWORD got = 0;
    int ok = 0;
    if (f == INVALID_HANDLE_VALUE) { return 0; }
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 20 && sz.QuadPart <= 0x400000) {
        b = (unsigned char *)malloc((size_t)sz.QuadPart);
        if (b) {
            if (ReadFile(f, b, (DWORD)sz.QuadPart, &got, NULL) && got == (DWORD)sz.QuadPart) {
                *h = sb_ally_fnv64(b, got - 20u);
                if (h_all) { *h_all = sb_ally_fnv64(b, got); }
                ok = 1;
            }
            free(b);
        }
    }
    CloseHandle(f);
    return ok;
}

static void ally_reg_path(char *out, size_t cap)
{
    _snprintf_s(out, cap, _TRUNCATE, "%s\\steelbound_replays.txt", g_cfg.hdd_root);
}

static int ally_reg_find(unsigned long long h)
{
    char path[700], line[64];
    FILE *f;
    int v = -1;
    ally_reg_path(path, sizeof path);
    if (fopen_s(&f, path, "r") != 0 || !f) { return -1; }
    while (fgets(line, sizeof line, f)) {
        unsigned long long k = 0;
        int x = 0;
        if (sscanf_s(line, "%llx %d", &k, &x) == 2 && k == h) { v = x ? 1 : 0; }
    }
    fclose(f);
    return v;
}

static int ends_rep(const char *p)
{
    size_t n = p ? strlen(p) : 0;
    return n > 4 && _stricmp(p + n - 4, ".rep") == 0;
}

void sb_ally_rep_open(const char *host, int write)
{
    unsigned long long h;
    if (g_sba_loc || !g_rep_cs_init || !ends_rep(host)) { return; }
    EnterCriticalSection(&g_rep_cs);
    if (write) {
        strcpy_s(g_rep_wpath, sizeof g_rep_wpath, host);
        if (g_cfg.sb_ally_diag) {
            HLOG_I("SBALLY Replay-Schreibhandle (Aufzeichnung offen=%d, f=%u)", (int)g_rec_pending, g_frame);
        }
    } else if (ally_rep_hash(host, &h, NULL)) {
        int v = ally_reg_find(h);
        InterlockedExchange(&g_play_flag, v < 0 ? -1 : v);
        HLOG_I("SBALLY Replay geoeffnet: Vermerk %s", v < 0 ? "keiner (Original)" : v ? "an" : "aus");
    }
    LeaveCriticalSection(&g_rep_cs);
}

/* W426: Das Spiel schreibt die Aufzeichnung in zwei Durchgaengen: zuerst die
 * Daten (Datei noch ohne Signatur, W426_b2: 1 008 108 B), dann mit einem
 * zweiten Schreib-Handle die 20-Byte-Signatur. Beim ersten Schliessen sind
 * deshalb die ganzen Dateibytes die Replay-Daten; W424 hasht dort n-20 Bytes,
 * der Vermerk passte beim Abspielen nie (Wiedergabe lief als Original).
 * Eingetragen werden jetzt beide Lesarten (ganze Datei = Daten ohne Signatur,
 * n-20 = Datei mit Signatur), so passt der Vermerk in beiden Faellen. */
void sb_ally_rep_close(const char *host)
{
    unsigned long long h, h_all;
    if (g_sba_loc || !g_rep_cs_init || !ends_rep(host)) { return; }
    EnterCriticalSection(&g_rep_cs);
    if (g_rep_wpath[0] && _stricmp(g_rep_wpath, host) == 0) {
        g_rep_wpath[0] = 0;
        if (InterlockedExchange(&g_rec_pending, 0) && ally_rep_hash(host, &h, &h_all)) {
            char path[700];
            FILE *f;
            int v = (int)g_rec_flag, need_all = ally_reg_find(h_all) < 0, need_sig = ally_reg_find(h) < 0;
            ally_reg_path(path, sizeof path);
            if ((need_all || need_sig) && fopen_s(&f, path, "a") == 0 && f) {
                if (need_all) { fprintf(f, "%016llx %d\n", h_all, v); }
                if (need_sig) { fprintf(f, "%016llx %d\n", h, v); }
                fclose(f);
                HLOG_I("SBALLY Replay geschrieben: Vermerk %s", v ? "an" : "aus");
            }
        }
    }
    LeaveCriticalSection(&g_rep_cs);
}
