/* sb_cchain.c - W303 CAMPAIGN-CHAIN: Anbindung der Kampagnen-Kette an
 * Spielspeicher, Eingabe und Missionsende-Testhook (reiner Test-Schalter).
 * Ablauf: sb_cchain.h, reine Logik: sb_cchain_core.c.
 * Ohne [input] campaign_chain (Standard 0) und campaign_chain_load (0) ist
 * g_cc.phase == CC_OFF und jede Funktion kehrt sofort zurueck.
 *
 * Logzeilen (von scripts/campaign_chain.py ausgewertet, nicht umformulieren):
 *   CCHAIN: Mission laeuft #k mno=.. f=.. versorgung=.. kampf=.. rang=.. wahl=.. fortschritt=.. timer=.. schaden=.. schwierigkeit=..
 *   CCHAIN: Sieg ausgeloest mno=.. f=.. timer=.. schaden=.. versorgung=..
 *   CCHAIN: Szene 0x.. f=.. mno=.. phase=.. ergebnis=.. grund=0x.. versorgung=.. kampf=.. rang=.. wahl=.. fortschritt=.. dialog=.. cursor=..
 *   CCHAIN: Abzug <ordner> (n Dateien)
 *   CCHAIN: Ende ok|FEHLER ... */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sb_cchain.h"
#include "hcfg.h"
#include "hlog.h"
#include "hooks_int.h" /* g_frames, sbc_in_t */
#include "sb_profile.h"
#include "sb_state.h"
#include "sb_overlay.h"

#define CC_MNO     0x3B89A0u /* u16 Missionsnummer */
#define CC_PHASE   0x66ABF4u /* u16 Missionsphase (5 laeuft, 0 Sieg, W238) */
#define CC_RESULT  0x356DD0u /* Ergebnis 1 Sieg / 2 Fehlschlag */
#define CC_REASON  0x35236Cu /* Endgrund 0xD5/0xD6 */
#define CC_F30     0x356F30u /* float Einblendung */
#define CC_DIALOG  0x686D44u /* Dialog/Auswahl offen */
#define CC_BCUR    0x6B9C50u /* Cursor der Zweig-Auswahl nach mno 15 (FUN_000E0140) */
#define CC_SUPPLY  0x69D5D8u /* Versorgungspunkte (userNN.sav +0x30) */
#define CC_COMBAT  0x69D5D4u /* Kampfpunkte (+0x2C) */
#define CC_RANK    0x69D5D0u /* Rang (+0x28) */
#define CC_PROG    0x69D5DCu /* zuletzt gespielte mno (FUN_000C9D40) */
#define CC_SEL     0x69D5E0u /* u16 Missionswahl = Nachfolger (+0x38) */
#define CC_TIMER   0x3563F0u /* Countdown (Frames) */
#define CC_DAMAGE  0x6A37D8u /* Schaden in der Mission (0 = Schadensfrei-Praemie) */
#define CC_DIFF    0x69CA88u /* Schwierigkeit */
/* Training (0x4A880): Story-Film nach dem Sieg */
#define CC_M1_3516D0 0x3516D0u
#define CC_M1_FILM   0x356F88u
#define CC_M1_AFTER  0x35226Cu
/* Sieg-Aufrufe der Missionsskripte (MISSIONEN §1.2, SPIELZUSTAENDE §26.1) */
#define CC_FN_PHASE  0x0002C280u /* FUN_0002C280(phase): nur bei Phase 5 */
#define CC_FN_END    0x0002C250u /* FUN_0002C250(grund, blende): nur wenn [0x356FC8]==0 */
#define CC_BLEND_001 0x3C23D70Au /* float 0,01 wie push 0x3c23d70a in 0x4A93A */

static cc_t g_cc;
static int g_cc_last_phase = -1;
static unsigned g_cc_last_scene = 0xFFFFFFFFu;
static int g_cc_end_logged;
static ULONGLONG g_cc_t0;
static int g_cc_snap_n;
static int g_cc_last_dialog;
static int g_cc_last_key;
static int g_cc_dlg_logs;
static ULONGLONG g_cc_end_t;
extern void emu_run_seconds_end_now(void); /* emu_main.c */
extern unsigned g_shot_census; /* render_diag.c: Bild beim naechsten Present */

int sb_cchain_active(void)
{
    return g_cc.phase != CC_OFF && g_cc.phase != CC_DONE && g_cc.phase != CC_FAIL;
}

static unsigned cc_rd(unsigned eu)
{
    __try { return *(volatile unsigned *)(uintptr_t)SBA(eu); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFFu; }
}

static unsigned cc_rd16(unsigned eu)
{
    __try { return *(volatile unsigned short *)(uintptr_t)SBA(eu); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFu; }
}

static float cc_rdf(unsigned eu)
{
    __try { return *(volatile float *)(uintptr_t)SBA(eu); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1.0f; }
}

static void cc_wr(unsigned eu, unsigned v)
{
    __try { *(volatile unsigned *)(uintptr_t)SBA(eu) = v; }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

static const char *cc_err_name(int e)
{
    switch (e) {
    case CC_ERR_WAIT: return "keine Mission (Zeitgrenze Laden)";
    case CC_ERR_WIN: return "Sieg nicht uebernommen";
    case CC_ERR_STUCK: return "Szene haengt";
    case CC_ERR_NAV: return "Ladetest-Menue";
    case CC_ERR_LOST: return "Pilot gefallen/GAME OVER";
    case CC_ERR_GATE: return "Briefing-Tor (COMPLETE abgelehnt)";
    default: return "-";
    }
}

void sb_cchain_disable(void)
{
    if (g_cc.phase != CC_OFF) {
        HLOG_I("CCHAIN: im LoC-Profil abgeschaltet");
    }
    memset(&g_cc, 0, sizeof g_cc);
    g_cc.phase = CC_OFF;
}

void sb_cchain_init(void)
{
    if (g_cfg.campaign_chain <= 0 && !g_cfg.campaign_chain_load) {
        return;
    }
    cc_init(&g_cc, g_cfg.campaign_chain, g_cfg.campaign_chain_branch,
            (unsigned)(g_cfg.campaign_chain_hold > 0 ? g_cfg.campaign_chain_hold : 1), g_cfg.campaign_chain_load);
    g_cc.chapter = g_cfg.campaign_chain_chapter ? 1 : 0;
    g_cfg.alpha_welcome = 0;
    if (g_cfg.replay_steps[0] || g_cfg.replay[0]) { /* Testketten wuerden mitdruecken */
        g_cfg.replay_steps[0] = 0;
        g_cfg.replay[0] = 0;
        HLOG_I("CCHAIN: replay_steps/replay abgeschaltet");
    }
    HLOG_I("CCHAIN: aktiv siege=%d zweig=%d hold=%d ladetest=%d (Testwerkzeug W303, Missionsende-Testhook)",
           g_cfg.campaign_chain, g_cfg.campaign_chain_branch, g_cfg.campaign_chain_hold, g_cfg.campaign_chain_load);
}

static void cc_log_state(const char *what, unsigned scene)
{
    HLOG_I("CCHAIN: %s 0x%X f=%u mno=%u phase=%u ergebnis=%u grund=0x%X versorgung=%d kampf=%d rang=%u wahl=%u "
           "fortschritt=%u dialog=%u cursor=%u",
           what, scene, g_frames, cc_rd16(CC_MNO), cc_rd16(CC_PHASE), cc_rd(CC_RESULT), cc_rd(CC_REASON),
           (int)cc_rd(CC_SUPPLY), (int)cc_rd(CC_COMBAT), cc_rd(CC_RANK), cc_rd16(CC_SEL), cc_rd(CC_PROG),
           cc_rd(CC_DIALOG), cc_rd(CC_BCUR));
}

/* Spielstaende <hdd_root>\TDATA\43430002\* nach <SB_OUT_DIR>\cchain\NN_mMM kopieren
 * (nur in Testlaeufen mit SB_OUT_DIR; Ladetest und Auswertung lesen sie). */
static void cc_snapshot(void)
{
    char out[600], src[1100], dst[1200], pat[1200], a[1300], b[1300];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0;
    DWORD e = GetEnvironmentVariableA("SB_OUT_DIR", out, sizeof out);
    if (e == 0 || e >= sizeof out) {
        return;
    }
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\TDATA\\43430002", g_cfg.hdd_root);
    _snprintf_s(dst, sizeof dst, _TRUNCATE, "%s\\cchain", hcfg_out_dir());
    CreateDirectoryA(dst, NULL);
    _snprintf_s(dst, sizeof dst, _TRUNCATE, "%s\\cchain\\%02d_m%02u", hcfg_out_dir(), g_cc_snap_n++, cc_rd16(CC_MNO));
    CreateDirectoryA(dst, NULL);
    _snprintf_s(pat, sizeof pat, _TRUNCATE, "%s\\*", src);
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            _snprintf_s(a, sizeof a, _TRUNCATE, "%s\\%s", src, fd.cFileName);
            _snprintf_s(b, sizeof b, _TRUNCATE, "%s\\%s", dst, fd.cFileName);
            if (CopyFileA(a, b, FALSE)) n++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    HLOG_I("CCHAIN: Abzug %s (%d Dateien)", dst, n);
}

/* Missionsende-Testhook: dieselben Aufrufe wie das Missionsskript beim Sieg,
 * im Spiel-Thread (Swap-Hook liegt zwischen zwei Spiel-Frames). */
static void cc_win(void)
{
    typedef void (__cdecl *fn_phase_t)(unsigned);
    typedef void (__cdecl *fn_end_t)(unsigned, unsigned);
    fn_phase_t fphase = (fn_phase_t)(uintptr_t)SBA(CC_FN_PHASE);
    fn_end_t fend = (fn_end_t)(uintptr_t)SBA(CC_FN_END);
    unsigned mno = cc_rd16(CC_MNO);
    HLOG_I("CCHAIN: Sieg ausgeloest mno=%u f=%u timer=%u schaden=%u versorgung=%d", mno, g_frames,
           cc_rd(CC_TIMER), cc_rd(CC_DAMAGE), (int)cc_rd(CC_SUPPLY));
    __try {
        if (mno == 0) { /* Training 0x4A880: Endblende, Story-Film opj.bik, dann Phase */
            fend(0xD6u, CC_BLEND_001);
            cc_wr(CC_M1_3516D0, 0);
            cc_wr(CC_M1_FILM, 1);
            cc_wr(CC_M1_AFTER, 1);
            fphase(0);
        } else { /* alle anderen Missionen (z. B. 0x4A920): Phase 0, dann Endblende */
            fphase(0);
            fend(0xD6u, CC_BLEND_001);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        HLOG_W("CCHAIN: Testhook-Aufruf mit Ausnahme 0x%08lX", GetExceptionCode());
    }
}

void sb_cchain_tick(void)
{
    ULONGLONG now;
    unsigned dt = 0;
    cc_in_t in;
    if (g_cc.phase == CC_OFF) {
        return;
    }
    if (g_cc.phase == CC_DONE || g_cc.phase == CC_FAIL) {
        if (g_cc_end_logged && g_cfg.campaign_chain_exit > 0 && g_cc_end_t &&
            GetTickCount64() - g_cc_end_t >= (ULONGLONG)g_cfg.campaign_chain_exit * 1000u) {
            g_cc_end_t = 0;
            HLOG_I("CCHAIN: Testlauf endet (campaign_chain_exit=%d)", g_cfg.campaign_chain_exit);
            emu_run_seconds_end_now(); /* nur mit --run-seconds (Testlauf) */
        }
        if (!g_cc_end_logged) {
            g_cc_end_logged = 1;
            g_cc_end_t = GetTickCount64();
            if (g_cc.phase == CC_DONE) {
                HLOG_I("CCHAIN: Ende ok siege=%d folgemissionen=%d titel=%d mno=%u f=%u szene=0x%X", g_cc.won, g_cc.chained,
                       g_cc.title_end, cc_rd16(CC_MNO), g_frames, cc_rd(SB_ST_SCENE));
            } else {
                HLOG_W("CCHAIN: Ende FEHLER %d (%s) phase_vorher=%s siege=%d folgemissionen=%d szene=0x%X mno=%u f=%u",
                       g_cc.err, cc_err_name(g_cc.err), g_cc_last_phase < 0 ? "-" : cc_phase_name(g_cc_last_phase),
                       g_cc.won, g_cc.chained, cc_rd(SB_ST_SCENE), cc_rd16(CC_MNO), g_frames);
            }
        }
        return;
    }
    now = GetTickCount64();
    if (g_cc_t0) dt = (unsigned)(now - g_cc_t0);
    g_cc_t0 = now;
    if (g_cfg.overlay && sb_ovl_open()) {
        return; /* ESC-Menue offen: Spiel pausiert */
    }
    if (dt > 1000u) dt = 1000u;
    in.scene = cc_rd(SB_ST_SCENE);
    in.mloop = cc_rd(SB_ST_MLOOP);
    in.phase = cc_rd16(CC_PHASE);
    in.mno = cc_rd16(CC_MNO);
    in.demo = cc_rd(SB_ST_DEMO) == 1u;
    in.film = cc_rdf(CC_F30) >= 0.999f;
    in.dialog = cc_rd(CC_DIALOG) != 0u;
    in.cursor = (int)cc_rd(CC_BCUR);
    cc_step(&g_cc, g_frames, &in, dt);
    if (g_cc.act == CC_ACT_WIN) {
        cc_win();
    } else if (g_cc.act == CC_ACT_SNAP) {
        cc_snapshot();
    }
    if (in.dialog != g_cc_last_dialog && g_cc.phase == CC_POST) { /* Zweig-Auswahl nach mno 15 */
        g_cc_last_dialog = in.dialog;
        g_cc_dlg_logs = 0;
        cc_log_state(in.dialog ? "Dialog auf in Szene" : "Dialog zu in Szene", in.scene);
    }
    {
        int k = cc_key(&g_cc, g_frames);
        if (g_cfg.campaign_chain_shots && k != g_cc_last_key && k != CC_KEY_NONE &&
            (g_cc.phase == CC_POST || g_cc.phase == CC_NAV)) {
            g_shot_census = 1; /* DIAG: Bild bei jeder Menuetaste */
        }
        if (k != g_cc_last_key && k != CC_KEY_NONE && in.dialog && g_cc.phase == CC_POST && g_cc_dlg_logs++ < 8) {
            HLOG_I("CCHAIN: Auswahl Taste %d cursor=%d zweig=%d mno=%u f=%u", k, in.cursor, g_cc.branch, in.mno, g_frames);
        }
        g_cc_last_key = k;
    }
    if (in.scene != g_cc_last_scene) {
        g_cc_last_scene = in.scene;
        if (g_cc.phase == CC_POST || g_cc.phase == CC_NAV || g_cc.phase == CC_END) {
            cc_log_state("Szene", in.scene);
            if (g_cfg.campaign_chain_shots) g_shot_census = 1; /* DIAG: Bild je Szenenwechsel */
        }
    }
    if (g_cc.phase != g_cc_last_phase) {
        if (g_cc.phase == CC_HOLD || (g_cc.phase == CC_DONE && g_cc_last_phase == CC_WAIT)) {
            HLOG_I("CCHAIN: Mission laeuft #%d mno=%u f=%u versorgung=%d kampf=%d rang=%u wahl=%u fortschritt=%u "
                   "timer=%u schaden=%u schwierigkeit=%u",
                   g_cc.won + 1, in.mno, g_frames, (int)cc_rd(CC_SUPPLY), (int)cc_rd(CC_COMBAT), cc_rd(CC_RANK),
                   cc_rd16(CC_SEL), cc_rd(CC_PROG), cc_rd(CC_TIMER), cc_rd(CC_DAMAGE), cc_rd(CC_DIFF) & 0xFFu);
        }
        HLOG_I("CCHAIN: Phase %s (f=%u szene=0x%X mno=%u)", cc_phase_name(g_cc.phase), g_frames, in.scene, in.mno);
        if (g_cc.phase != CC_FAIL) g_cc_last_phase = g_cc.phase;
    }
}

void sb_cchain_apply(void *blk)
{
    sbc_in_t *s = (sbc_in_t *)blk;
    int k;
    if (g_cc.phase == CC_OFF || !s) {
        return;
    }
    k = cc_key(&g_cc, g_frames);
    if (!cc_navigating(&g_cc) && k == CC_KEY_NONE) {
        return;
    }
    if (g_cfg.overlay && sb_ovl_open()) {
        return;
    }
    if (g_cc.phase == CC_NAV) { /* Ladetest-Menue: echte Tasten/Hebel nicht ans Spiel (POST laesst
                                  * die Film-Ueberspringen-Taste von movie_skip durch) */
        s->buttons[0] = s->buttons[1] = s->buttons[2] = 0;
        s->sight_x = 0;
        s->sight_y = 0;
    }
    if (k == CC_KEY_OK) s->buttons[0] |= 1u << 0;        /* bi0 Hauptwaffe */
    else if (k == CC_KEY_BACK) s->buttons[0] |= 1u << 2; /* bi2 Lock-On */
    else if (k == CC_KEY_UP) s->sight_y = -127;
    else if (k == CC_KEY_DOWN) s->sight_y = 127;
    else if (k == CC_KEY_LEFT) s->sight_x = -127;
    else if (k == CC_KEY_RIGHT) s->sight_x = 127;
}
