/* sb_mauto.c - W272 ALPHA-TEST: Anbindung des Teststarts (Mission + VT direkt)
 * an Spielspeicher, Eingabe, Test-ZCACHE und Overlay.
 * Ablauf und Regeln: sb_mauto.h, reine Logik: sb_mauto_core.c.
 * Ohne [input] mission_autostart (Standard -1) ist g_ma.phase == MA_OFF und
 * jede Funktion kehrt sofort zurueck (Standardpfad unveraendert). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sb_mauto.h"
#include "hcfg.h"
#include "hlog.h"
#include "hooks_int.h" /* g_frames, sbc_in_t */
#include "sb_profile.h"
#include "sb_state.h"
#include "sb_overlay.h"
#include "sb_lang.h"

#define MA_MNO   0x3B89A0u /* u16 Missionsnummer (0-basiert), SPIELZUSTAENDE §11.1 */
#define MA_VT    0x3B8AA8u /* u16 Spieler-VT-Modell (ENGDATnn), W243 */
#define MA_DIFFV 0x69CA88u /* Schwierigkeit 0..5 (ROOKIE..ARMAGEDDON), W238 */
#define MA_F30   0x356F30u /* float 1,0 Laden/Film, 0,0 ab Missionsbeginn (§11.4) */
#define MA_CKP   0x354F3Cu /* Cockpit-Modus aus ENGDAT +0xBC (W243) */

extern unsigned g_shot_census; /* render_diag.c: Bild beim naechsten Present */
extern int krnl_read_d(const char *rel, unsigned char **buf, unsigned *len); /* krnl_file.c */

static ma_t g_ma;
static int g_ma_last_phase = -1;
static int g_ma_noticed;
static int g_ma_staged;
static int g_ma_ckp_want = -1;   /* Cockpit-Modus laut ENGDAT des gewaehlten VT */
static unsigned g_ma_pokes;
static int g_ma_done_logged;
static int g_ma_last_key;
static ULONGLONG g_ma_t0;

int sb_mauto_active(void)
{
    return g_ma.phase != MA_OFF && g_ma.phase != MA_DONE && g_ma.phase != MA_FAIL;
}

static unsigned ma_rd(unsigned eu)
{
    __try { return *(volatile unsigned *)(uintptr_t)SBA(eu); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFFu; }
}

static unsigned short ma_rd16(unsigned eu)
{
    __try { return *(volatile unsigned short *)(uintptr_t)SBA(eu); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFu; }
}

static float ma_rdf(unsigned eu)
{
    __try { return *(volatile float *)(uintptr_t)SBA(eu); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1.0f; }
}

static void ma_wr16(unsigned eu, unsigned short v)
{
    __try { *(volatile unsigned short *)(uintptr_t)SBA(eu) = v; }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

static void ma_wr8(unsigned eu, unsigned char v)
{
    __try { *(volatile unsigned char *)(uintptr_t)SBA(eu) = v; }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

static const char *ma_diff_name(int d)
{
    static const char *const nm[] = { "ROOKIE", "NORMAL", "VETERAN", "DESPERADO", "MAGNUM FORCE", "ARMAGEDDON" };
    return (d >= 0 && d <= 5) ? nm[d] : "Spiel";
}

static void ma_notice(int err)
{
    const char *l[5] = { 0 };
    const char *k1 = "ovl.ma_fail";
    switch (err) {
    case MA_ERR_ARGS: k1 = "ovl.ma_args"; break;
    case MA_ERR_LOAD: k1 = "ovl.ma_load"; break;
    default: break;
    }
    l[0] = LT(k1);
    l[1] = LT("ovl.ma_fail2");
    l[2] = "";
    l[3] = LT("ovl.ma_cont");
    sb_ovl_notice_ex("ovl.ma_title", "ovl.ma_tag", l, 4);
    if (g_cfg.mission_autostart_shots) g_shot_census = 1;
}

/* W277 LOC-DISC: im LoC-Profil nie aktiv (SB-Szenen/Menues). */
void sb_mauto_disable(void)
{
    if (g_ma.phase != MA_OFF) {
        HLOG_I("MISSION-AUTO: im LoC-Profil abgeschaltet");
    }
    memset(&g_ma, 0, sizeof g_ma);
    g_ma.phase = MA_OFF;
}

void sb_mauto_init(void)
{
    if (g_cfg.mission_autostart < 0) {
        return;
    }
    if (ma_args_valid(g_cfg.mission_autostart, g_cfg.mission_vt, g_cfg.mission_difficulty) != MA_ERR_NONE) {
        memset(&g_ma, 0, sizeof g_ma);
        g_ma.phase = MA_FAIL;
        g_ma.err = MA_ERR_ARGS;
        HLOG_W("MISSION-AUTO: ungueltige Werte (mission=%d vt=%d schwierigkeit=%d; erlaubt mission 0..23, "
               "vt -1/1..22 ohne 12, schwierigkeit -1/0..5) - normaler Start",
               g_cfg.mission_autostart, g_cfg.mission_vt, g_cfg.mission_difficulty);
        return;
    }
    ma_init(&g_ma, g_cfg.mission_skip_intro);
    if (g_cfg.mission_nav_name[0] && ma_seq_parse(g_cfg.mission_nav_name, &g_ma.name_seq) != 0) {
        HLOG_W("MISSION-AUTO: mission_nav_name '%s' ungueltig - Standardfolge", g_cfg.mission_nav_name);
        ma_init(&g_ma, g_cfg.mission_skip_intro);
    }
    g_cfg.alpha_welcome = 0; /* kein Willkommens-Hinweis (pausiert) vor dem Teststart */
    if ((g_cfg.replay_steps[0] || g_cfg.replay[0]) && !g_cfg.mission_autostart_steps) { /* Testketten wuerden mitdruecken (W315: TEST-Schalter laesst sie an) */
        g_cfg.replay_steps[0] = 0;
        g_cfg.replay[0] = 0;
        HLOG_I("MISSION-AUTO: replay_steps/replay fuer den Teststart abgeschaltet");
    }
    HLOG_I("MISSION-AUTO: aktiv mission=%d vt=%d schwierigkeit=%d (%s) film_skip=%d (Alpha-Testwerkzeug)",
           g_cfg.mission_autostart, g_cfg.mission_vt, g_cfg.mission_difficulty,
           ma_diff_name(g_cfg.mission_difficulty), g_cfg.mission_skip_intro);
}

/* Einmal nach dem Start (hdd_root ist dann endgueltig, auch der US-Ordner):
 * TEXnn.BIN in den Test-ZCACHE stellen (mno >= 1), Cockpit-Modus des VT aus
 * ENGDATnn.ENG (+0xBC) fuer die Kontrolle lesen. */
static void ma_stage(void)
{
    char zdir[1100], dst[1200], rel[64];
    unsigned char *buf = NULL;
    unsigned len = 0;
    int mno = g_cfg.mission_autostart;
    g_ma_staged = 1;
    if (g_cfg.mission_vt > 0) {
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "Media\\EngData\\ENGDAT%02d.ENG", g_cfg.mission_vt);
        if (krnl_read_d(rel, &buf, &len) == 0 && len >= 0xC0u) {
            g_ma_ckp_want = (int)(buf[0xBC] | (buf[0xBD] << 8));
            HLOG_I("MISSION-AUTO: VT-Daten %s (%u Byte, Cockpit-Modus %d)", rel, len, g_ma_ckp_want);
        } else {
            HLOG_W("MISSION-AUTO: VT-Daten %s fehlen - Spiel laedt sie nicht, VT bleibt Missionsstandard", rel);
        }
        free(buf);
        buf = NULL;
    }
    if (mno < 1) {
        return;
    }
    _snprintf_s(zdir, sizeof zdir, _TRUNCATE, "%s\\ZCACHE", g_cfg.hdd_root);
    CreateDirectoryA(g_cfg.hdd_root, NULL);
    CreateDirectoryA(zdir, NULL);
    _snprintf_s(dst, sizeof dst, _TRUNCATE, "%s\\TEX%02d.BIN", zdir, mno);
    if (GetFileAttributesA(dst) != INVALID_FILE_ATTRIBUTES) {
        HLOG_I("MISSION-AUTO: TEX%02d.BIN liegt schon im ZCACHE", mno);
        return;
    }
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "Media\\BIN\\TEX%02d.BIN", mno);
    if (krnl_read_d(rel, &buf, &len) != 0) {
        HLOG_W("MISSION-AUTO: %s nicht lesbar - Mission %d kann nicht laden", rel, mno);
        return;
    }
    {
        HANDLE h = CreateFileA(dst, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD wr = 0;
        BOOL ok = FALSE;
        if (h != INVALID_HANDLE_VALUE) {
            ok = WriteFile(h, buf, len, &wr, NULL) && wr == len;
            CloseHandle(h);
            if (!ok) DeleteFileA(dst);
        }
        HLOG_I("MISSION-AUTO: %s -> Test-ZCACHE (%u Byte) %s", rel, len, ok ? "ok" : "FEHLER");
    }
    free(buf);
}

/* Missionswerte im Ladefenster schreiben (alle Schreiber des Menues sind dann
 * durch: DFC70 in Szene 0xF setzt mno 0/1 und VT 8, W27/W243). */
static void ma_poke(void)
{
    unsigned short mno = (unsigned short)g_cfg.mission_autostart;
    if (ma_rd16(MA_MNO) != mno) ma_wr16(MA_MNO, mno);
    if (g_cfg.mission_vt > 0 && ma_rd16(MA_VT) != (unsigned short)g_cfg.mission_vt) {
        ma_wr16(MA_VT, (unsigned short)g_cfg.mission_vt);
    }
    if (g_cfg.mission_difficulty >= 0) {
        unsigned d = ma_rd(MA_DIFFV);
        if ((d & 0xFFu) != (unsigned)g_cfg.mission_difficulty) ma_wr8(MA_DIFFV, (unsigned char)g_cfg.mission_difficulty);
    }
    if (g_ma_pokes++ == 0) {
        HLOG_I("MISSION-AUTO: Missionswerte gesetzt (f=%u szene=0x%X mno=%u vt=%u schwierigkeit=%u)",
               g_frames, ma_rd(SB_ST_SCENE), ma_rd16(MA_MNO), ma_rd16(MA_VT), ma_rd(MA_DIFFV) & 0xFFu);
    }
}

void sb_mauto_tick(void)
{
    ULONGLONG now;
    unsigned dt = 0, scene, mloop;
    int film;
    if (g_ma.phase == MA_OFF) {
        return;
    }
    if (g_ma.phase == MA_FAIL || g_ma.phase == MA_DONE) {
        if (g_ma.phase == MA_FAIL && !g_ma_noticed && ma_key(&g_ma, g_frames) == MA_KEY_NONE) {
            g_ma_noticed = 1;
            HLOG_W("MISSION-AUTO: abgebrochen (Fehler %d, Phase vorher %s, f=%u szene=0x%X) - Spiel laeuft normal weiter",
                   g_ma.err, g_ma_last_phase < 0 ? "Start" : ma_phase_name(g_ma_last_phase), g_frames, ma_rd(SB_ST_SCENE));
            ma_notice(g_ma.err);
        }
        if (g_ma.phase == MA_DONE && !g_ma_done_logged) {
            g_ma_done_logged = 1;
            HLOG_I("MISSION-AUTO: Mission laeuft (f=%u mno=%u vt=%u cockpit=%u erwartet=%d schwierigkeit=%u) - "
                   "Steuerung beim Spieler", g_frames, ma_rd16(MA_MNO), ma_rd16(MA_VT), ma_rd(MA_CKP),
                   g_ma_ckp_want, ma_rd(MA_DIFFV) & 0xFFu);
        }
        return;
    }
    if (!g_ma_staged && g_frames > 2u) {
        ma_stage();
    }
    now = GetTickCount64();
    if (g_ma_t0) {
        dt = (unsigned)(now - g_ma_t0);
    }
    g_ma_t0 = now;
    if (g_cfg.overlay && sb_ovl_open()) {
        return; /* Menue offen: Spiel pausiert, Zeit laeuft nicht */
    }
    if (dt > 1000u) dt = 1000u;
    scene = ma_rd(SB_ST_SCENE);
    mloop = ma_rd(SB_ST_MLOOP);
    film = ma_rdf(MA_F30) >= 0.999f;
    ma_step(&g_ma, g_frames, scene, mloop, film, dt);
    if (ma_poking(&g_ma, scene)) {
        ma_poke();
    }
    if (g_cfg.mission_autostart_shots) { /* DIAG: Bild vor jeder Taste */
        int k = ma_key(&g_ma, g_frames);
        if (k != MA_KEY_NONE && k != g_ma_last_key) g_shot_census = 1;
        g_ma_last_key = k;
    }
    if (g_ma.phase != g_ma_last_phase) {
        HLOG_I("MISSION-AUTO: Phase %s (f=%u szene=0x%X mloop=%u mno=%u vt=%u)",
               ma_phase_name(g_ma.phase), g_frames, scene, mloop, ma_rd16(MA_MNO), ma_rd16(MA_VT));
        if (g_cfg.mission_autostart_shots) g_shot_census = 1;
        if (g_ma.phase != MA_FAIL) g_ma_last_phase = g_ma.phase;
    }
}

void sb_mauto_apply(void *blk)
{
    sbc_in_t *s = (sbc_in_t *)blk;
    int k;
    if (g_ma.phase == MA_OFF || !s) {
        return;
    }
    k = ma_key(&g_ma, g_frames);
    if (!ma_navigating(&g_ma) && k == MA_KEY_NONE) {
        return; /* Film/Mission: echte Eingabe */
    }
    if (g_cfg.overlay && sb_ovl_open()) {
        return;
    }
    if (ma_navigating(&g_ma)) { /* Menue-Fuehrung: echte Tasten/Hebel nicht ans Spiel */
        s->buttons[0] = s->buttons[1] = s->buttons[2] = 0;
        s->sight_x = 0;
        s->sight_y = 0;
    }
    if (k == MA_KEY_OK) s->buttons[0] |= 1u << 0;        /* bi0 Hauptwaffe */
    else if (k == MA_KEY_BACK) s->buttons[0] |= 1u << 2; /* bi2 Lock-On = zurueck / Film-Skip */
    else if (k == MA_KEY_UP) s->sight_y = -127;
    else if (k == MA_KEY_DOWN) s->sight_y = 127;
    else if (k == MA_KEY_LEFT) s->sight_x = -127;
    else if (k == MA_KEY_RIGHT) s->sight_x = 127;
    else if (k >= MA_KEY_BTN && k < MA_KEY_BTN + 48) {
        int bi = k - MA_KEY_BTN;
        s->buttons[bi >> 4] |= 1u << (bi & 15);
    }
}
