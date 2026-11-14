/* diag_states.c - DIAG(STATES): Spielmodus/Szene pro Swap ueberwachen (A3).
 * Ladezaehler u32 0x2EEA34 (W232: kein Spielmodus, +1 je Ladebildschirm;
 * Logfeld heisst weiter "mode=" fuer golden.py/milestones.py)
 * + Szene u32 [0x6B96DC] + Missions-Schleifenflag
 * u32 0x356F58 (1 = 45f60-Schleife laeuft). Loggt bei JEDER Aenderung und
 * alle 600 Frames einen Heartbeat (logdrop-robust fuer milestones.py).
 * Schalter: [log] diag_states=1. Aufruf: eine Zeile im Swap-Hook. */
#include "diag_states.h"
#include "sb_profile.h" /* W229 SBA() */
#include "hlog.h"
#include "hcfg.h"
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <windows.h>

typedef uint32_t u32;

#define ST_MODE_ADDR SBA(0x2EEA34u) /* W232: Ladezaehler (Logfeld "mode=") */
#define ST_SCENE_ADDR SBA(0x6B96DCu)
#define ST_MLOOP_ADDR SBA(0x356F58u)
#define ST_HEARTBEAT 600u

static u32 st_last_mode = 0xFFFFFFFFu;
static u32 st_last_scene = 0xFFFFFFFFu;
static u32 st_last_mloop = 0xFFFFFFFFu;

/* DIAG(STATE) (A11c): Missions-Zustandsadressen, s. docs/SPIELZUSTAENDE.md.
 * Bekannte Felder (W7/W8); Positions-/HP-/Munitions-Adressen folgen, sobald
 * der Memscan sie belegt (§10). Alle Reads SEH-geschuetzt. */
#define STA_F30_ADDR SBA(0x356F30u)   /* float, Missionsende-Bedingung ==1.0 */
#define STA_FC8_ADDR SBA(0x356FC8u)   /* u32, Missionsende-Bedingung ==1 */
#define STA_B894_ADDR SBA(0x3B8994u)  /* u32, Missionsende-Bedingung ==0 */
#define STA_LMOD_ADDR SBA(0x346FC0u)  /* u32, Loader-Modus (0/1/2) */
#define STA_ACT_ADDR SBA(0x6B96E4u)   /* Schleifen-Aktiv-Flag (Wort, u32 gelesen) */
#define STA_GATE_ADDR SBA(0x669E24u)  /* u32, Folge-Gate (==0x65 offen) */
#define STA_SUB_ADDR SBA(0x66ABF4u)   /* u32, Folge-Sub-State */
#define STA_B89A0_ADDR SBA(0x3B89A0u) /* u32, Fall-Weiche/Input-Lock */
#define STA_PORT_ADDR SBA(0x346FB0u)  /* Portmaske (Wort, u32 gelesen) */
#define STA_LOCK_ADDR SBA(0x686D44u)  /* u32, Input-Lock (!=0 sperrt) */
#define STA_E690_ADDR SBA(0x2E6F90u)  /* u32, U5/Load-Unterzustand */
#define STA_AA8_ADDR SBA(0x3B8AA8u)   /* Render-Modus (Wort, u32 gelesen; 8 = Mission) */
#define STA_DEMO_ADDR SBA(0x3B8CDCu)  /* W239: u32 1 = Titel-Demo/Replay (wie sb_state.h SB_ST_DEMO) */
/* DIAG(STARTUP) (W21-R2): Start-Minispiel (8AA10), s. SPIELZUSTAENDE.md */
#define STU_BAR0_ADDR SBA(0x55D19Cu)   /* float[5], Balken (Stride 4) */
#define STU_LINE_ADDR SBA(0x55D244u)   /* float, Stabil-Linie (0.7) */
#define STU_STAGE_ADDR SBA(0x51D8B4u)  /* Byte, Stufe (3=Schalter, 0=fertig) */
#define STU_CNT_ADDR SBA(0x51D92Cu)    /* u32, Schalter-Zaehler (0..5) */
#define STU_S168_ADDR SBA(0x55D168u)   /* Byte, Minispiel-Status (2=Balken, 9=nach START) */
#define STU_ENG_ADDR SBA(0x3516DCu)    /* u16, Engine-Flag (37290-Toggle) */
#define STU_OK_ADDR SBA(0x55D3F4u)     /* u32, START-Erfolg (8AA10: alle > Linie) */
#define STU_FAIL_ADDR SBA(0x55D3F0u)   /* u32, START-Fehlschlag */
/* DIAG(COMBAT) (W35): Entity-Tabelle [0x351828], Stride 0x5C, HP = Wort +0x0,
 * Schadens-Akku = Wort +0x32 (s. FUN_0002F250, SPIELZUSTAENDE.md). */
#define CMB_N_ADDR SBA(0x352288u)    /* u32, Entity-Anzahl (Missionsdaten) */
#define CMB_TAB_ADDR SBA(0x351828u)  /* u32, Tabellen-Zeiger (Heap) */
#define CMB_OURS_ADDR SBA(0x356E2Cu) /* u16, unser VT-Index */
#define CMB_DMG_ADDR SBA(0x356F10u)   /* u32, Schadensbetrag (laufend) */
#define CMB_PEND_ADDR SBA(0x354F24u)  /* u32, Schaden-anstehend-Flag */
#define CMB_SUM_ADDR SBA(0x6A37D8u)   /* u32, Schadenssumme (Master: +400/~250f) */
#define CMB_CNT_ADDR SBA(0x354FFCu)   /* u32, HP-0-Zaehler (bis 90) */
#define CMB_RSN_ADDR SBA(0x35236Cu)   /* u32, Missionsende-Grund (0xD5 = VT tot) */
#define CMB_B24_ADDR SBA(0x355024u)   /* u16, Gruppenstart B */
#define CMB_B26_ADDR SBA(0x355026u)   /* u16, Gruppenz. B */
#define CMB_B28_ADDR SBA(0x355028u)   /* u16, Gruppenstart C (= unser Index) */
#define CMB_B2A_ADDR SBA(0x35502Au)   /* u16, Gruppenz. C */
#define CMB_STRIDE 0x5Cu
#define CMB_MAX 64u
/* DIAG(COMBATX) (W37): Entity-Rohdaten + Eingabe-Spiegel im COMBAT-Takt.
 * COMBATX: volles 0x5C-Struct (23 u32-Worte) je Kampf-Eintrag (uns + alle
 *   mit 0<HP<0x7FFF, max 8) -> Positionsfelder empirisch finden (H4/H3).
 * COMBATI: Eingabe-Spiegel (decIdx, History-Zeile 14 B, Out-Lane-0-Kopf 16 B
 *   + 0x2A8, Gang-Latch) -> Kette Eingabe->Wirkung pro Tick (H1/H4).
 * Kein eigener Schalter: laeuft unter [log] combat_diag=1 mit. */
#define CMX_HIST_ADDR SBA(0x3B8D6Cu)   /* Decoder-Historie, 14 B/Frame (47730) */
#define CMX_DEC_ADDR SBA(0x4AEEF0u)    /* u32, Missions-Frame-Zaehler (decIdx) */
#define CMX_OUT_ADDR SBA(0x4AF480u)    /* Out-Lane 0 (Mapper-Ziel, Feuer=+3) */
#define CMX_GEAR_ADDR SBA(0x351518u)   /* u32, Mapper-Gang-Latch (W17: 8/9) */
#define CMX_MAXENT 48u /* W39: +Projektil-Pool (200-211) */
/* DIAG(COMBATH) (W40): Hitpfad-Zustand im COMBAT-Takt (1 Zeile/Tick).
 * gate=[0x356E7C] (20BD0-Eingang, muss !=0), vic=[0x346F80] (Opfer-Index),
 * rayvic=[0x4C9B38] (523F0-Treffer), st304=[0x352304] (4A9F0-Pfad),
 * o30=unser +0x30 (Selbst-Index per Init 0x200BD, KEIN Lock, W57),
 * e67/e71: HP+Akku der Gegner.
 * W57 AIMFIX: + aim (Out Aim-Bytes s8), from/off (Strahlengeometrie),
 * tur (unsere Turm-Worte +0x40/+0x44), pr0 (erstes lebendes Projektil
 * Typ 0x3A8/HP 120 aus Pool 200-211: Pos + Worte +0x18/+0x1C/+0x20).
 * Kein eigener Schalter: laeuft unter [log] combat_diag=1 mit. */
#define CMH_AIMX_ADDR SBA(0x4AF48Cu)   /* s8, Out+0x0C Aim-X (Mapper-Ziel, W57) */
#define CMH_AIMY_ADDR SBA(0x4AF48Du)   /* s8, Out+0x0D Aim-Y (W57) */
#define CMH_FROMX_ADDR SBA(0x358400u)  /* float[3], Case-1-Strahl Von-Punkt (W57) */
#define CMH_OFF_ADDR SBA(0x352148u)    /* float, Case-1-Strahl Offset (W42/W57) */
#define CMH_GATE_ADDR SBA(0x356E7Cu)   /* u16, 20BD0-Gate (!=0 offen) */
#define CMH_VIC_ADDR SBA(0x346F80u)    /* u32, Opfer-Index (Case 3 setzt) */
#define CMH_RAY_ADDR SBA(0x4C9B38u)    /* u16, 523F0-Treffer-Index */
#define CMH_304_ADDR SBA(0x352304u)    /* u32, 4A9F0-Schadenspfad-Marker */
#define CMH_AD0_ADDR SBA(0x4C9AD0u)    /* u16, Case-4-Opfer (523F0-Slot 2) */
#define CMH_MZX_ADDR SBA(0x3579B4u)    /* float, Muendung/Referenzpunkt X */
#define CMH_MZY_ADDR SBA(0x3579B8u)    /* float, Muendung/Referenzpunkt Y */
#define CMH_MZZ_ADDR SBA(0x3579BCu)    /* float, Muendung/Referenzpunkt Z */
#define CMH_DMQ_ADDR SBA(0x6CA870u)    /* u32, Schadens-Queue-Zaehler (<0x100) */
#define CMH_M61_ADDR SBA(0x35161Cu)    /* u32, Case-0-Hit-Marker */
#define CMH_B2C_ADDR SBA(0x35502Cu)    /* u16, Drain-Untergrenze 1608C0 (W40-offen-4, W42) */
#define STA_TMR_ADDR SBA(0x3563F0u)   /* u32, Missions-Frame-Countdown (W17-scan: 71998->0, -1/Frame) */
#define STA_MA_ADDR SBA(0x6C8510u)    /* u32, Missionswert A (3000->2589->112, W17-Kandidat) */
#define STA_MB_ADDR SBA(0x6C851Cu)    /* u32, Missionswert B (325->12, W17-Kandidat) */
#define STA_MC_ADDR SBA(0x347174u)    /* u32, Missionswert C (1565->1337, W17-Kandidat) */

static u32 st_read(u32 addr)
{
    u32 v = 0xFFFFFFFFu;
    __try {
        v = *(volatile u32 *)(uintptr_t)addr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v = 0xFFFFFFFEu; /* (noch) nicht lesbar */
    }
    return v;
}

static u32 st_read16(u32 addr)
{
    u32 v = 0xFFFFFFFFu;
    __try {
        v = *(volatile uint16_t *)(uintptr_t)addr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v = 0xFFFFFFFEu;
    }
    return v;
}

void diag_states_tick(unsigned frame)
{
    u32 mode, scene, mloop;
    if (!g_cfg.diag_states) {
        return;
    }
    mode = st_read(ST_MODE_ADDR);
    scene = st_read(ST_SCENE_ADDR);
    mloop = st_read(ST_MLOOP_ADDR);
    if (mode != st_last_mode || scene != st_last_scene ||
        mloop != st_last_mloop) {
        st_last_mode = mode;
        st_last_scene = scene;
        st_last_mloop = mloop;
        HLOG_I("STATES: f=%u mode=%u scene=0x%X mloop=%u (chg)", frame, mode,
               scene, mloop);
    } else if ((frame % ST_HEARTBEAT) == 0) {
        HLOG_I("STATES: f=%u mode=%u scene=0x%X mloop=%u", frame, mode,
               scene, mloop);
    }
}

/* DIAG(STATE) (A11c): kompakte Missions-Zustandszeile, 1x/s (wall-clock,
 * fps-unabhaengig). Schalter [log] state_diag=1, Intervall [log]
 * state_every (ms). Format: key=value, parst scripts/state_check.py. */
/* TEST(PAL50,W234) film_diag=1: Filmdauer gegen die Wanduhr (QPC). Liest
 * den Bink-Zeiger des Film-Players wie sb_pause_movie_frame (W192):
 * Width/Height/Frames/FrameNum/LastFrameNum/FrameRate/FrameRateDiv.
 * Je Film eine start- und eine end-Zeile (Zeiger genullt bzw. gewechselt). */
static void diag_film_tick(unsigned frame)
{
    static const volatile unsigned *cur;
    static LARGE_INTEGER q0, qf;
    static unsigned last_fn, frames, rate, div, swaps;
    const volatile unsigned *b = NULL;
    LARGE_INTEGER q;
    __try {
        b = *(const volatile unsigned *volatile *)(uintptr_t)SBA(0x3B8AD0u);
        if (!(b && b[0] && b[1] && b[3] <= b[2])) { b = NULL; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { b = NULL; }
    if (!qf.QuadPart) { QueryPerformanceFrequency(&qf); }
    QueryPerformanceCounter(&q);
    if (cur && b != cur) {
        HLOG_I("FILMDIAG: end f=%u bink=%u/%u rate=%u/%u wall_s=%.3f swaps=%u vblank_hz=%d game_tick=%d", frame,
               last_fn, frames, rate, div, (double)(q.QuadPart - q0.QuadPart) / (double)qf.QuadPart, swaps,
               g_cfg.vblank_hz, g_cfg.game_tick);
        cur = NULL;
    }
    if (b && !cur) {
        cur = b;
        q0 = q;
        swaps = 0;
        __try { frames = b[2]; rate = b[5]; div = b[6]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        HLOG_I("FILMDIAG: start f=%u frames=%u rate=%u/%u vblank_hz=%d game_tick=%d", frame, frames, rate, div,
               g_cfg.vblank_hz, g_cfg.game_tick);
    }
    if (cur) {
        swaps++;
        __try { last_fn = b[3]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

void diag_state_tick(unsigned frame)
{
    static DWORD sta_last_ms, sta_t0;
    static int sta_have_t0;
    DWORD now, iv;
    u32 mode, scene, mloop, f30raw, fc8, b894, lmod, act, gate, sub;
    u32 b89a0, port, lock, e690, aa8;
    u32 tmr, ma, mb, mc, demo;
    float f30;
    if (g_cfg.film_diag) { diag_film_tick(frame); } /* TEST(PAL50,W234) */
    if (!g_cfg.state_diag) {
        return;
    }
    now = GetTickCount();
    if (!sta_have_t0) {
        sta_have_t0 = 1;
        sta_t0 = now;
    }
    iv = g_cfg.state_every > 0 ? (DWORD)g_cfg.state_every : 1000u;
    if (sta_last_ms && now - sta_last_ms < iv) {
        return;
    }
    sta_last_ms = now;
    mode = st_read(ST_MODE_ADDR);
    scene = st_read(ST_SCENE_ADDR);
    mloop = st_read(ST_MLOOP_ADDR);
    f30raw = st_read(STA_F30_ADDR);
    fc8 = st_read(STA_FC8_ADDR);
    b894 = st_read(STA_B894_ADDR);
    lmod = st_read(STA_LMOD_ADDR);
    act = st_read(STA_ACT_ADDR);
    gate = st_read(STA_GATE_ADDR);
    sub = st_read(STA_SUB_ADDR);
    b89a0 = st_read(STA_B89A0_ADDR);
    port = st_read(STA_PORT_ADDR);
    lock = st_read(STA_LOCK_ADDR);
    e690 = st_read(STA_E690_ADDR);
    aa8 = st_read(STA_AA8_ADDR);
    tmr = st_read(STA_TMR_ADDR);
    ma = st_read(STA_MA_ADDR);
    mb = st_read(STA_MB_ADDR);
    mc = st_read(STA_MC_ADDR);
    demo = st_read(STA_DEMO_ADDR);
    memcpy(&f30, &f30raw, 4);
    HLOG_I("STATE: f=%u t=%.1f mode=%u scene=0x%X mloop=%u f30=%.3f "
           "fc8=%u b894=%u lmod=%u act=%u gate=0x%X sub=%u b89a0=0x%X "
           "port=%u lock=%u e690=%u aa8=%u tmr=%u ma=%u mb=%u mc=%u demo=%u",
           frame,
           (double)(now - sta_t0) / 1000.0, mode, scene, mloop, f30,
           fc8, b894, lmod, act, gate, sub, b89a0, port, lock, e690,
           aa8, tmr, ma, mb, mc, demo);
}

/* DIAG(STARTUP) (W21-R2): Start-Minispiel-Zeile im STATE-Takt (wall-clock).
 * Schalter [log] startup_diag=1, Intervall [log] state_every (ms).
 * Alle Reads SEH-geschuetzt (st_read). */
void diag_startup_tick(unsigned frame)
{
    static DWORD stu_last_ms, stu_t0;
    static int stu_have_t0;
    DWORD now, iv;
    float b[5], line;
    u32 raw, stage, cnt, s168, eng, ok, fail;
    int i;
    if (!g_cfg.startup_diag) {
        return;
    }
    now = GetTickCount();
    if (!stu_have_t0) {
        stu_have_t0 = 1;
        stu_t0 = now;
    }
    iv = g_cfg.state_every > 0 ? (DWORD)g_cfg.state_every : 1000u;
    if (stu_last_ms && now - stu_last_ms < iv) {
        return;
    }
    stu_last_ms = now;
    for (i = 0; i < 5; i++) {
        raw = st_read(STU_BAR0_ADDR + (u32)(i * 4));
        memcpy(&b[i], &raw, 4);
    }
    raw = st_read(STU_LINE_ADDR);
    memcpy(&line, &raw, 4);
    stage = st_read(STU_STAGE_ADDR) & 0xFFu;
    cnt = st_read(STU_CNT_ADDR);
    s168 = st_read(STU_S168_ADDR) & 0xFFu;
    eng = st_read(STU_ENG_ADDR) & 0xFFFFu;
    ok = st_read(STU_OK_ADDR);
    fail = st_read(STU_FAIL_ADDR);
    HLOG_I("STARTUP: f=%u t=%.1f b0=%.4f b1=%.4f b2=%.4f b3=%.4f b4=%.4f "
           "line=%.4f stage=%u cnt=%u s168=%u eng=%u ok=%u fail=%u",
           frame, (double)(now - stu_t0) / 1000.0,
           b[0], b[1], b[2], b[3], b[4], line,
           stage, cnt, s168, eng, ok, fail);
    /* Draw-Drop-Zensus (drawdrop_diag): Cockpit-/Flach-Entscheidung FUN_000459b0
     * (0x45E3D): flach (0x297C0), wenn [0x346FB0]==0x80 oder (eng!=0 und
     * ckp=[0x3520DC]==0); ckp = [0x356EEC], auf 0 erzwungen bei [0x356E7C]==0,
     * [0x356C44] > 2.0 oder [0x35468C]!=0 (0x45A9F..0x45AF6). */
    if (g_cfg.drawdrop_diag) {
        float c44;
        raw = st_read(SBA(0x356C44u));
        memcpy(&c44, &raw, 4);
        HLOG_I("CKP: f=%u eng=%u ckp=%u eec=%u e7c=%u c44=%.3f s68c=%u fb0=0x%X",
               frame, eng, st_read(SBA(0x3520DCu)), st_read(SBA(0x356EECu)),
               st_read(SBA(0x356E7Cu)) & 0xFFFFu, c44, st_read(SBA(0x35468Cu)),
               st_read(SBA(0x346FB0u)) & 0xFFFFu);
    }
}
/* DIAG(GEAR) (W141): Gang-Kette Block -> Out+0xB -> Mapper-Latch +
 * eigene Position, im STATE-Takt (wall-clock). Schalter [log] gear_diag=1,
 * Intervall [log] state_every (ms). Belegt die Gang-Kodierung des echten
 * Controllers (254/255/1-5, Nutzer 6.11.): blk = pState-Gang des
 * Mapper-Ports, out = Out+0xB (Fahr-/Lampen-Byte), latch = [0x351518]
 * (Mapper-intern, -1/0 bei 254/255), x/z = eigene Entity-Position
 * (Fahrtrichtung). Alle Reads SEH-geschuetzt. */
#define GR_MP_ADDR SBA(0x352274u)   /* u32, Mapper-Port (Mission: 3) */
#define GR_PST_ADDR SBA(0x4AEEF8u)  /* pState-Basis, Stride 0x1C, Gang bei +27 */
#define GR_OUTP_ADDR SBA(0x4AF9D0u) /* u32, Out-Lane-Zeiger des Mappers */
#define GR_LATCH_ADDR SBA(0x351518u) /* s32, Mapper-Gang-Latch */
void diag_gear_tick(unsigned frame)
{
    static DWORD gr_last_ms, gr_t0;
    static int gr_have_t0;
    DWORD now, iv;
    u32 mp, blk, outp, outb, latch, tab, n, ours;
    u32 xr, zr;
    float xf = 0.0f, zf = 0.0f;
    if (!g_cfg.gear_diag) {
        return;
    }
    now = GetTickCount();
    if (!gr_have_t0) {
        gr_have_t0 = 1;
        gr_t0 = now;
    }
    iv = g_cfg.state_every > 0 ? (DWORD)g_cfg.state_every : 1000u;
    if (gr_last_ms && now - gr_last_ms < iv) {
        return;
    }
    gr_last_ms = now;
    mp = st_read(GR_MP_ADDR);
    blk = 0xEEu;
    if (mp < 4u) {
        __try {
            blk = *(volatile uint8_t *)(uintptr_t)(GR_PST_ADDR + mp * 0x1Cu + 27u);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            blk = 0xEEu;
        }
    }
    outp = st_read(GR_OUTP_ADDR);
    outb = 0xEEu;
    if (outp != 0 && outp != 0xFFFFFFFFu && outp != 0xFFFFFFFEu) {
        __try {
            outb = *(volatile uint8_t *)(uintptr_t)(outp + 0xBu);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            outb = 0xEEu;
        }
    }
    latch = st_read(GR_LATCH_ADDR);
    tab = st_read(CMB_TAB_ADDR);
    n = st_read(CMB_N_ADDR);
    ours = st_read16(CMB_OURS_ADDR) & 0xFFFFu;
    if (tab != 0 && tab != 0xFFFFFFFFu && tab != 0xFFFFFFFEu &&
        n < 100000u && ours < n) {
        xr = st_read(tab + ours * CMB_STRIDE + 0x0Cu);
        zr = st_read(tab + ours * CMB_STRIDE + 0x14u);
        memcpy(&xf, &xr, 4);
        memcpy(&zf, &zr, 4);
    }
    HLOG_I("GEAR: f=%u mp=%u blk=%u out=%02X latch=%d b8cc0=%u dec=%u x=%.1f z=%.1f",
           frame, mp, blk & 0xFFu, outb & 0xFFu, (int32_t)latch,
           st_read(SBA(0x3B8CC0u)), st_read(SBA(0x4AEEF0u)), xf, zf);
}
/* DIAG(COMBAT) (W35): Entity-Tabelle im STATE-Takt (wall-clock).
 * Schalter [log] combat_diag=1, Intervall [log] state_every (ms).
 * Zeile 1: Kopf (Anzahl, Zeiger, unser Index/HP, Schaden, Grund).
 * Zeile 2: pro Eintrag (max CMB_MAX) hp/accu/flags. Alle Reads
 * SEH-geschuetzt (st_read/st_read16). */
/* W35: HP-Vorwerte je Tabelleneintrag (Delta-Erkennung, Heap des Hosts). */
#include <stdlib.h>
static uint16_t *cmb_prev_hp = 0;
static u32 cmb_prev_n = 0, cmb_prev_tab = 0;

void diag_combat_tick(unsigned frame)
{
    static DWORD cmb_last_ms, cmb_t0;
    static int cmb_have_t0;
    DWORD now, iv;
    u32 n, tab, ours, dmg, pend, sum, cnt, rsn;
    u32 b24, b26, b28, b2a, ohp, oac;
    u32 i, lim;
    char hp[1024], ac[1024], fl[1024];
    size_t hp_n = 0, ac_n = 0, fl_n = 0;
    int w;
    if (!g_cfg.combat_diag) {
        return;
    }
    now = GetTickCount();
    if (!cmb_have_t0) {
        cmb_have_t0 = 1;
        cmb_t0 = now;
    }
    iv = g_cfg.state_every > 0 ? (DWORD)g_cfg.state_every : 1000u;
    if (cmb_last_ms && now - cmb_last_ms < iv) {
        return;
    }
    cmb_last_ms = now;
    n = st_read(CMB_N_ADDR);
    tab = st_read(CMB_TAB_ADDR);
    ours = st_read16(CMB_OURS_ADDR) & 0xFFFFu;
    dmg = st_read(CMB_DMG_ADDR);
    pend = st_read(CMB_PEND_ADDR);
    sum = st_read(CMB_SUM_ADDR);
    cnt = st_read(CMB_CNT_ADDR);
    rsn = st_read(CMB_RSN_ADDR);
    b24 = st_read16(CMB_B24_ADDR) & 0xFFFFu;
    b26 = st_read16(CMB_B26_ADDR) & 0xFFFFu;
    b28 = st_read16(CMB_B28_ADDR) & 0xFFFFu;
    b2a = st_read16(CMB_B2A_ADDR) & 0xFFFFu;
    ohp = 0xFFFFFFFFu; oac = 0xFFFFFFFFu;
    hp[0] = ac[0] = fl[0] = 0;
    lim = n < CMB_MAX ? n : CMB_MAX;
    if (tab != 0 && tab != 0xFFFFFFFFu && tab != 0xFFFFFFFEu && n < 100000u) {
        for (i = 0; i < lim; i++) {
            u32 base = tab + i * CMB_STRIDE;
            u32 h = st_read16(base) & 0xFFFFu;
            u32 a = st_read16(base + 0x32u) & 0xFFFFu;
            u32 f = st_read(base + 0x28u);
            if (i == ours) { ohp = h; oac = a; }
            w = _snprintf_s(hp + hp_n, sizeof(hp) - hp_n, _TRUNCATE,
                          "%s%u", i ? "," : "", h);
            if (w > 0) hp_n += (size_t)w;
            w = _snprintf_s(ac + ac_n, sizeof(ac) - ac_n, _TRUNCATE,
                          "%s%u", i ? "," : "", a);
            if (w > 0) ac_n += (size_t)w;
            w = _snprintf_s(fl + fl_n, sizeof(fl) - fl_n, _TRUNCATE,
                          "%s%X", i ? "," : "", f);
            if (w > 0) fl_n += (size_t)w;
            if (hp_n + 24 >= sizeof(hp) || ac_n + 24 >= sizeof(ac) ||
                fl_n + 24 >= sizeof(fl)) { lim = i + 1; break; }
        }
    }
    /* Vollscan: Deltas gegen Vorwerte + Kennziffern (nur bei Aenderung). */
    if (tab != 0 && tab != 0xFFFFFFFFu && tab != 0xFFFFFFFEu && n > 0 && n < 100000u) {
        if (n != cmb_prev_n || tab != cmb_prev_tab) {
            char sp[2048]; size_t sp_n = 0; u32 c0 = 0, cM = 0, cS = 0;
            uint16_t *nb = (uint16_t *)realloc(cmb_prev_hp, n * sizeof(uint16_t));
            if (nb) { cmb_prev_hp = nb; cmb_prev_n = n; cmb_prev_tab = tab; }
            sp[0] = 0;
            for (i = 0; i < n; i++) {
                u32 base = tab + i * CMB_STRIDE;
                u32 h = st_read16(base) & 0xFFFFu;
                if (h == 0) { c0++; }
                else if (h == 0x7FFFu) { cM++; }
                else {
                    u32 tp = st_read16(base + 2u) & 0xFFFFu;
                    u32 mx = st_read16(base + 0x3Au) & 0xFFFFu;
                    cS++;
                    if (cS <= 80) {
                        w = _snprintf_s(sp + sp_n, sizeof(sp) - sp_n, _TRUNCATE,
                                      "%s%u:%u/%X/%u", sp_n ? "," : "",
                                      i, h, tp, mx);
                        if (w > 0) sp_n += (size_t)w;
                    }
                }
                if (nb) nb[i] = (uint16_t)h;
            }
            HLOG_I("COMBATN: f=%u n=%u tab=0x%X ours=%u ohp=%u zero=%u max=%u spec=%u %s",
                   frame, n, tab, ours, ohp, c0, cM, cS, sp);
        } else if (cmb_prev_hp) {
            char dg[2048]; size_t dg_n = 0; u32 nd = 0;
            dg[0] = 0;
            for (i = 0; i < n; i++) {
                u32 base = tab + i * CMB_STRIDE;
                u32 h = st_read16(base) & 0xFFFFu;
                if (h != cmb_prev_hp[i]) {
                    nd++;
                    if (nd <= 48) {
                        w = _snprintf_s(dg + dg_n, sizeof(dg) - dg_n, _TRUNCATE,
                                      "%s%u:%u>%u", dg_n ? "," : "",
                                      i, cmb_prev_hp[i], h);
                        if (w > 0) dg_n += (size_t)w;
                    }
                    cmb_prev_hp[i] = (uint16_t)h;
                }
            }
            if (nd) {
                HLOG_I("COMBATD: f=%u nd=%u%s%s", frame, nd,
                       nd > 48 ? " (gek.)" : "", dg);
            }
        }
    }
    HLOG_I("COMBAT: f=%u t=%.1f n=%u tab=0x%X ours=%u ohp=%u oac=%u "
           "dmg=%u pend=%u sum=%u cnt=%u rsn=0x%X "
           "b24=%u b26=%u b28=%u b2a=%u",
           frame, (double)(now - cmb_t0) / 1000.0, n, tab, ours, ohp, oac,
           dmg, pend, sum, cnt, rsn, b24, b26, b28, b2a);
    if (hp[0]) {
        HLOG_I("COMBATE: f=%u hp=%s", frame, hp);
        HLOG_I("COMBATA: f=%u ac=%s", frame, ac);
        HLOG_I("COMBATF: f=%u fl=%s", frame, fl);
    }
    /* COMBATX (W37): Entity-Rohdaten + Eingabe-Spiegel (s. Defines oben). */
    if (tab != 0 && tab != 0xFFFFFFFFu && tab != 0xFFFFFFFEu && n > 0 && n < 100000u) {
        u32 shown = 0;
        /* Eintrag ours zuerst, dann Kampf-Eintraege in Index-Reihenfolge. */
        for (i = 0; i < n && shown < CMX_MAXENT; i++) {
            u32 idx = (shown == 0) ? ours : i;
            u32 base, h, k;
            char xb[512];
            size_t xb_n = 0;
            if (shown > 0 && i == ours) {
                continue;
            }
            if (idx >= n) {
                break;
            }
            base = tab + idx * CMB_STRIDE;
            h = st_read16(base) & 0xFFFFu;
            if (shown > 0 && (h == 0 || h == 0x7FFFu)) {
                continue;
            }
            xb[0] = 0;
            for (k = 0; k < CMB_STRIDE / 4; k++) {
                u32 wv = st_read(base + k * 4);
                w = _snprintf_s(xb + xb_n, sizeof(xb) - xb_n, _TRUNCATE,
                                "%s%08X", k ? "," : "", wv);
                if (w > 0) {
                    xb_n += (size_t)w;
                } else {
                    break;
                }
            }
            HLOG_I("COMBATX: f=%u idx=%u hp=%u w=%s", frame, idx, h, xb);
            shown++;
        }
    }
    {
        u32 dec = st_read(CMX_DEC_ADDR);
        char hb[64], ob[64];
        size_t hb_n = 0, ob_n = 0;
        u32 k, gear, o2a8;
        /* Decoder (47730) schreibt Zeile dec und inkrementiert danach: die
         * frisch geschriebene Zeile ist dec-1 (W37-Fix, war dec = leer). */
        u32 hist = dec > 0 ? dec - 1 : 0;
        hb[0] = ob[0] = 0;
        if (hist < 0x20000u) {
            for (k = 0; k < 14; k++) {
                u32 bv = 0xEEu;
                __try {
                    bv = *(volatile uint8_t *)(uintptr_t)(CMX_HIST_ADDR + hist * 14u + k);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    bv = 0xEEu;
                }
                w = _snprintf_s(hb + hb_n, sizeof(hb) - hb_n, _TRUNCATE, "%02X", bv & 0xFFu);
                if (w > 0) {
                    hb_n += (size_t)w;
                }
            }
        }
        for (k = 0; k < 16; k++) {
            u32 bv = 0xEEu;
            __try {
                bv = *(volatile uint8_t *)(uintptr_t)(CMX_OUT_ADDR + k);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                bv = 0xEEu;
            }
            w = _snprintf_s(ob + ob_n, sizeof(ob) - ob_n, _TRUNCATE, "%02X", bv & 0xFFu);
            if (w > 0) {
                ob_n += (size_t)w;
            }
        }
        gear = st_read(CMX_GEAR_ADDR);
        o2a8 = 0xEEu;
        __try {
            o2a8 = *(volatile uint8_t *)(uintptr_t)(CMX_OUT_ADDR + 0x2A8u);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            o2a8 = 0xEEu;
        }
        HLOG_I("COMBATI: f=%u dec=%u hist=%s out=%s o2a8=%02X gear=%u",
               frame, dec, hb, ob, o2a8 & 0xFFu, gear);
    }
    /* COMBATH (W40): Hitpfad-Zustand, 1 Zeile/Tick (s. Defines oben). */
    {
        u32 gate = st_read16(CMH_GATE_ADDR) & 0xFFFFu;
        u32 vic = st_read(CMH_VIC_ADDR);
        u32 ray = st_read16(CMH_RAY_ADDR) & 0xFFFFu;
        u32 s304 = st_read(CMH_304_ADDR);
        u32 ad0 = st_read16(CMH_AD0_ADDR) & 0xFFFFu;
        u32 dmq = st_read(CMH_DMQ_ADDR);
        u32 m61 = st_read(CMH_M61_ADDR);
        u32 mzr[3]; float mzf[3]; int mi;
        for (mi = 0; mi < 3; mi++) {
            mzr[mi] = st_read(CMH_MZX_ADDR + (u32)(mi * 4));
            memcpy(&mzf[mi], &mzr[mi], 4);
        }
        u32 o30 = 0xFFFFFFFFu, e67h = 0xFFFFFFFFu, e67a = 0xFFFFFFFFu;
        u32 e71h = 0xFFFFFFFFu, e71a = 0xFFFFFFFFu;
        u32 vhp = 0xFFFFFFFFu, vtp = 0xFFFFFFFFu, vfl = 0xFFFFFFFFu;
        float vpf[3] = { 0.0f, 0.0f, 0.0f };
        u32 b2c = st_read16(CMH_B2C_ADDR) & 0xFFFFu;
        if (tab != 0 && tab != 0xFFFFFFFFu && tab != 0xFFFFFFFEu && n > 71u &&
            n < 100000u) {
            if (ours < n) {
                o30 = st_read16(tab + ours * CMB_STRIDE + 0x30u) & 0xFFFFu;
            }
            e67h = st_read16(tab + 67u * CMB_STRIDE) & 0xFFFFu;
            e67a = st_read16(tab + 67u * CMB_STRIDE + 0x32u) & 0xFFFFu;
            e71h = st_read16(tab + 71u * CMB_STRIDE) & 0xFFFFu;
            e71a = st_read16(tab + 71u * CMB_STRIDE + 0x32u) & 0xFFFFu;
        }
        /* W42 AIM1521: Opfer-Entity identifizieren (Typ/HP/Flags/Position
         * von vic = [0x346F80], z. B. 1521) + Drain-Grenze [0x35502C]. */
        if (tab != 0 && tab != 0xFFFFFFFFu && tab != 0xFFFFFFFEu &&
            n < 100000u && vic < n) {
            u32 vb = tab + vic * CMB_STRIDE;
            u32 vr;
            int vi;
            vhp = st_read16(vb) & 0xFFFFu;
            vtp = st_read16(vb + 2u) & 0xFFFFu;
            vfl = st_read(vb + 0x28u);
            for (vi = 0; vi < 3; vi++) {
                vr = st_read(vb + 0x0Cu + (u32)(vi * 4));
                memcpy(&vpf[vi], &vr, 4);
            }
        }
        /* W57 AIMFIX: Aim/Turm/Projektil/Muendungsrichtung im COMBAT-Takt. */
        {
            int ax = 0, ay = 0;
            u32 fr[3]; float frf[3]; int fi;
            u32 offr; float offf = 0.0f;
            u32 t16r = 0, t17r = 0; float t16f = 0.0f, t17f = 0.0f;
            int pr0 = -1;
            float prp[3] = { 0.0f, 0.0f, 0.0f };
            float prv[3] = { 0.0f, 0.0f, 0.0f };
            __try {
                ax = *(volatile int8_t *)(uintptr_t)CMH_AIMX_ADDR;
                ay = *(volatile int8_t *)(uintptr_t)CMH_AIMY_ADDR;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ax = ay = -128;
            }
            for (fi = 0; fi < 3; fi++) {
                fr[fi] = st_read(CMH_FROMX_ADDR + (u32)(fi * 4));
                memcpy(&frf[fi], &fr[fi], 4);
            }
            offr = st_read(CMH_OFF_ADDR);
            memcpy(&offf, &offr, 4);
            if (tab != 0 && tab != 0xFFFFFFFFu && tab != 0xFFFFFFFEu &&
                n < 100000u && ours < n) {
                u32 ob = tab + ours * CMB_STRIDE;
                t16r = st_read(ob + 0x40u);
                t17r = st_read(ob + 0x44u);
                memcpy(&t16f, &t16r, 4);
                memcpy(&t17f, &t17r, 4);
            }
            if (tab != 0 && tab != 0xFFFFFFFFu && tab != 0xFFFFFFFEu &&
                n > 211u && n < 100000u) {
                u32 pi;
                for (pi = 200u; pi <= 211u; pi++) {
                    u32 pb = tab + pi * CMB_STRIDE;
                    u32 ph = st_read16(pb) & 0xFFFFu;
                    u32 pt = st_read16(pb + 2u) & 0xFFFFu;
                    if (ph == 120u && pt == 0x3A8u) {
                        u32 pr; int pj;
                        pr0 = (int)pi;
                        for (pj = 0; pj < 3; pj++) {
                            pr = st_read(pb + 0x0Cu + (u32)(pj * 4));
                            memcpy(&prp[pj], &pr, 4);
                            pr = st_read(pb + 0x18u + (u32)(pj * 4));
                            memcpy(&prv[pj], &pr, 4);
                        }
                        break;
                    }
                }
            }
        HLOG_I("COMBATH: f=%u gate=%u vic=%u ray=%u ad0=%u s304=%u "
               "m61=%u dmq=%u mz=%.1f,%.1f,%.1f o30=%u e67=%u/%u e71=%u/%u "
               "v=%u/%X/%X@%.1f,%.1f,%.1f b2c=%u "
               "aim=%d,%d from=%.1f,%.1f,%.1f off=%.2f tur=%.1f,%.1f "
               "pr0=%d@%.1f,%.1f,%.1f/%.2f,%.2f,%.2f",
               frame, gate, vic, ray, ad0, s304, m61, dmq,
               mzf[0], mzf[1], mzf[2], o30, e67h, e67a, e71h, e71a,
               vhp, vtp, vfl, vpf[0], vpf[1], vpf[2], b2c,
               ax, ay, frf[0], frf[1], frf[2], offf, t16f, t17f,
               pr0, prp[0], prp[1], prp[2], prv[0], prv[1], prv[2]);
        }
    }
    /* COMBATW (W59): echter Missions-Waffen-/Ziel-Pfad (FUN_00037290 liest
     * Aim per 48CF0(1,0,18/19) -> [0x356E1C]/[0x356D38], FUN_00034BE0 glaettet
     * -> [0x356D08]/[0x3521BC], Lock-Kandidat 2E350 -> [0x357834], Lock-Ziel
     * [0x34700C] (Flanke 48CF0(1,6,1)), Fadenkreuz-Schirmpos [0x358560/64],
     * Zielpunkt [0x35838C], Feuer 48CF0(1,0,9) -> 177090 -> Geschoss-Pool
     * 0x6D2FD8 (60 x 0x58, Flag +4 Bit0, Typ +0xC, Tick +0x10, +0x18 Start,
     * +0x24 Ziel). Gates [0x354F00] (0/4), [0x351548]&1, [0x3520DC]. */
    {
        u32 r; float aimx = 0, aimy = 0, smx = 0, smy = 0, chx = 0, chy = 0;
        float tp[3] = { 0, 0, 0 }, b0[6] = { 0, 0, 0, 0, 0, 0 };
        u32 cand = st_read(SBA(0x357834u)), lck = st_read(SBA(0x34700Cu));
        u32 g4f00 = st_read(SBA(0x354F00u)), g1548 = st_read(SBA(0x351548u));
        u32 g20dc = st_read(SBA(0x3520DCu)), wp = st_read(SBA(0x355000u));
        u32 wid = 0xFFFFu, am0 = 0xFFFFu, rl0 = st_read16(SBA(0x6E449Cu));
        u32 rl1 = st_read16(SBA(0x6E44A6u)), nb = 0, b0i = 0xFFu, b0t = 0, b0k = 0;
        u32 bi; int k;
        r = st_read(SBA(0x356E1Cu)); memcpy(&aimx, &r, 4);
        r = st_read(SBA(0x356D38u)); memcpy(&aimy, &r, 4);
        r = st_read(SBA(0x356D08u)); memcpy(&smx, &r, 4);
        r = st_read(SBA(0x3521BCu)); memcpy(&smy, &r, 4);
        r = st_read(SBA(0x358560u)); memcpy(&chx, &r, 4);
        r = st_read(SBA(0x358564u)); memcpy(&chy, &r, 4);
        for (k = 0; k < 3; k++) {
            r = st_read(SBA(0x35838Cu) + (u32)(k * 4));
            memcpy(&tp[k], &r, 4);
        }
        if (wp > 0x10000u && wp < 0x8000000u) {
            wid = st_read16(wp + 0x0Cu) & 0xFFFFu;
            am0 = st_read16(wp + 0x12u) & 0xFFFFu;
        }
        for (bi = 0; bi < 60u; bi++) {
            u32 bb = SBA(0x6D2FD8u) + bi * 0x58u;
            if (st_read(bb + 4u) & 1u) {
                nb++;
                if (b0i == 0xFFu) {
                    b0i = bi;
                    b0t = st_read16(bb + 0x0Cu) & 0xFFFFu;
                    b0k = st_read16(bb + 0x10u) & 0xFFFFu;
                    for (k = 0; k < 6; k++) {
                        r = st_read(bb + 0x18u + (u32)(k * 4));
                        memcpy(&b0[k], &r, 4);
                    }
                }
            }
        }
        /* Peilung zu den Gegnern 67/71 relativ zur Rumpf-Gierung [0x3521C0]
         * (Zielpunkt-Gierung = hy + sm_x, Richtung (sin, cos) in x/z, s. 0x34C94). */
        {
            float hy = 0, mz[3] = { 0, 0, 0 }, ed[2] = { -1, -1 }, eb[2] = { 0, 0 };
            int ei;
            r = st_read(SBA(0x3521C0u)); memcpy(&hy, &r, 4);
            for (k = 0; k < 3; k++) {
                r = st_read(SBA(0x3579B4u) + (u32)(k * 4));
                memcpy(&mz[k], &r, 4);
            }
            if (tab != 0 && tab != 0xFFFFFFFFu && tab != 0xFFFFFFFEu && n > 71u &&
                n < 100000u) {
                for (ei = 0; ei < 2; ei++) {
                    u32 eb0 = tab + (ei ? 71u : 67u) * CMB_STRIDE;
                    float ep[3];
                    for (k = 0; k < 3; k++) {
                        r = st_read(eb0 + 0x0Cu + (u32)(k * 4));
                        memcpy(&ep[k], &r, 4);
                    }
                    {
                        float dx = ep[0] - mz[0], dz = ep[2] - mz[2];
                        double a = atan2((double)dx, (double)dz) - (double)hy;
                        while (a > 3.14159265) a -= 6.2831853;
                        while (a < -3.14159265) a += 6.2831853;
                        ed[ei] = (float)sqrt((double)dx * dx + (double)dz * dz);
                        eb[ei] = (float)a;
                    }
                }
            }
            HLOG_I("COMBATW: f=%u aim=%.3f,%.3f sm=%.3f,%.3f ch=%.1f,%.1f cand=%u "
                   "lock=%u tp=%.1f,%.1f,%.1f g=%u/%X/%u wid=%u am=%u rl=%u/%u "
                   "nb=%u b0=%u/t%u/k%u@%.1f,%.1f,%.1f>%.1f,%.1f,%.1f hy=%.3f "
                   "e67=%.1f@%.3f e71=%.1f@%.3f",
                   frame, aimx, aimy, smx, smy, chx, chy, cand, lck, tp[0], tp[1],
                   tp[2], g4f00, g1548, g20dc, wid, am0, rl0, rl1, nb, b0i, b0t,
                   b0k, b0[0], b0[1], b0[2], b0[3], b0[4], b0[5], hy, ed[0],
                   eb[0], ed[1], eb[1]);
        }
    }
}

