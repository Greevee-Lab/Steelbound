/* sb_rauto.c - W254 REPLAY-LAUNCHER (experimentell): Anbindung des Replay-
 * Starts an Spielspeicher, Eingabe, Kernel-Dateizugriffe und Overlay.
 * Ablauf und Regeln: sb_rauto.h, reine Logik: sb_rauto_core.c.
 * Ohne [input] replay_autoplay ist g_ra.phase == RA_OFF und jede Funktion
 * kehrt sofort zurueck (Standardpfad unveraendert). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "sb_rauto.h"
#include "hcfg.h"
#include "hlog.h"
#include "hooks_int.h" /* g_frames, sbc_in_t */
#include "sb_profile.h"
#include "sb_state.h"
#include "sb_overlay.h"
#include "sb_lang.h"

#define RA_REPLAY_ON  0x3B8CC0u /* u32 1 = Wiedergabe (Replay/Titel-Demo), SPIELZUSTAENDE §24.2 */
#define RA_REPLAY_CAM 0x3B8CF4u /* u32 Ansicht der Wiedergabe (FUN_00037290 switch) */

extern unsigned g_shot_census; /* render_diag.c: Bild beim naechsten Present */

static ra_t g_ra;                 /* Spiel-Thread (Swap/Eingabe) */
static volatile LONG g_ra_rep;    /* Kernel-Thread: 1 = gewaehltes .rep geoeffnet, 2 = anderes */
static int g_ra_last_phase = -1;
static int g_ra_noticed;
static ULONGLONG g_ra_t0;
static char g_ra_path[1024];      /* Host-Pfad des gewaehlten st00_0.rep (Kernel-Thread fuellt) */
static volatile LONG g_ra_path_state; /* 0 offen, 1 vorhanden, 2 fehlt */
static volatile LONG g_ra_filtered, g_ra_seen, g_ra_redir;
static int g_ra_cam_logged;

extern int krnl_host_path(const char *xb, char *out, size_t cap); /* krnl_file.c */

int sb_rauto_active(void)
{
    return g_ra.phase != RA_OFF && g_ra.phase != RA_DONE && g_ra.phase != RA_FAIL;
}

/* W255 VIDEO-EXPORT: aktuelle Phase (RA_*) und Fehler (RA_ERR_*) */
int sb_rauto_phase(int *err)
{
    if (err) *err = g_ra.err;
    return g_ra.phase;
}

static unsigned ra_rd(unsigned eu)
{
    __try { return *(volatile unsigned *)(uintptr_t)SBA(eu); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFFu; }
}

static int ends_with_i(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && _stricmp(s + a - b, suf) == 0;
}

/* Host-Pfad des gewaehlten Replays (gleiche Aufloesung wie das Spiel: U:,
 * Spielstand-Schutz-Kopie, US-Ordner). Einmalig, aus jedem Thread. */
static int ra_path(void)
{
    if (g_ra_path_state == 0) {
        char xb[96], p[1024];
        _snprintf_s(xb, sizeof xb, _TRUNCATE, "U:\\%s\\st00_0.rep", g_cfg.replay_autoplay);
        if (krnl_host_path(xb, p, sizeof p) == 0) {
            DWORD a = GetFileAttributesA(p);
            if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
                strcpy_s(g_ra_path, sizeof g_ra_path, p);
                InterlockedExchange(&g_ra_path_state, 1);
            } else {
                InterlockedExchange(&g_ra_path_state, 2);
            }
        } else {
            InterlockedExchange(&g_ra_path_state, 2);
        }
    }
    return g_ra_path_state == 1;
}

void sb_rauto_init(void)
{
    if (!g_cfg.replay_autoplay[0]) {
        return;
    }
    if (!ra_id_valid(g_cfg.replay_autoplay)) {
        memset(&g_ra, 0, sizeof g_ra);
        g_ra.phase = RA_FAIL;
        g_ra.err = RA_ERR_ID;
        HLOG_W("REPLAY-AUTO: ungueltige Replay-ID (1..32 Hex-Zeichen erwartet) - normaler Start");
        return;
    }
    ra_init(&g_ra);
    g_cfg.alpha_welcome = 0; /* kein Willkommens-Hinweis (pausiert) vor dem Replay */
    if (g_cfg.replay_camera > 6) {
        g_cfg.replay_camera = -1;
    }
    HLOG_I("REPLAY-AUTO: aktiv id=%s kamera=%d (experimentell)", g_cfg.replay_autoplay, g_cfg.replay_camera);
}

/* Kernel: UDATA\43430002 nur mit dem gewaehlten Ordner auflisten. */
int sb_rauto_dir_skip(const char *dir_host, const char *name, int is_dir)
{
    size_t n;
    char d[1024];
    if (!sb_rauto_active() || !dir_host || !name) {
        return 0;
    }
    strcpy_s(d, sizeof d, dir_host);
    n = strlen(d);
    while (n && (d[n - 1] == '\\' || d[n - 1] == '/')) d[--n] = 0;
    if (!ends_with_i(d, "\\UDATA\\43430002")) {
        return 0;
    }
    if (_stricmp(name, g_cfg.replay_autoplay) == 0) {
        InterlockedIncrement(&g_ra_seen);
        HLOG_I("REPLAY-AUTO: Replay-Liste: nur '%s' (andere Ordner ausgeblendet)", name);
        return 0;
    }
    if (!is_dir) {
        return 0; /* TitleMeta.xbx, SaveImage.xbx ... */
    }
    InterlockedIncrement(&g_ra_filtered);
    return 1;
}

/* Kernel: .rep-Oeffnungen beobachten; "Last Replay" lesend aufs gewaehlte. */
int sb_rauto_open_hook(const char *host, uint32_t access, uint32_t disp, char *alt, unsigned cap)
{
    const uint32_t wr = 0x40000000u | 0x10000000u | 0x02000000u | 0x2u | 0x4u | 0x10000u; /* GENERIC_WRITE/ALL, MAXIMUM_ALLOWED, WRITE/APPEND_DATA, DELETE */
    if (!sb_rauto_active() || !host || !ends_with_i(host, "\\st00_0.rep")) {
        return 0;
    }
    if (ends_with_i(host, "\\ZCACHE\\st00_0.rep")) {
        if (disp != 1u || (access & wr) || !ra_path()) { /* nur reines Lesen umleiten */
            HLOG_I("REPLAY-AUTO: Last Replay unveraendert (disp=%u access=0x%X)", disp, access);
            return 0;
        }
        strcpy_s(alt, cap, g_ra_path);
        InterlockedIncrement(&g_ra_redir);
        if (g_ra.phase == RA_CONFIRM) InterlockedExchange(&g_ra_rep, 1);
        HLOG_I("REPLAY-AUTO: Last Replay (lesen) -> Replay %s", g_cfg.replay_autoplay);
        return 1;
    }
    if (g_ra.phase == RA_CONFIRM) {
        char seg[48];
        int mine;
        _snprintf_s(seg, sizeof seg, _TRUNCATE, "\\%s\\st00_0.rep", g_cfg.replay_autoplay);
        mine = ends_with_i(host, seg);
        InterlockedExchange(&g_ra_rep, mine ? 1 : 2);
        HLOG_I("REPLAY-AUTO: Spiel laedt %s Replay", mine ? "das gewaehlte" : "ein ANDERES");
    }
    return 0;
}

static void ra_notice(int err)
{
    const char *l[6] = { 0 };
    const char *k1 = "ovl.ra_start";
    const char *k2 = "ovl.ra_start2";
    switch (err) {
    case RA_ERR_ID: k1 = "ovl.ra_id"; k2 = NULL; break;
    case RA_ERR_MISSING: k1 = "ovl.ra_missing"; k2 = "ovl.ra_missing2"; break;
    case RA_ERR_TITLE:
    case RA_ERR_MENU: k1 = "ovl.ra_menu"; k2 = NULL; break;
    case RA_ERR_WRONG: k1 = "ovl.ra_wrong"; k2 = NULL; break;
    default: break;
    }
    l[0] = LT(k1);
    l[1] = k2 ? LT(k2) : "";
    l[2] = "";
    if (err == RA_ERR_ID || err == RA_ERR_MISSING) { /* nichts zum Selbst-Waehlen */
        l[3] = LT("ovl.ra_cont");
        l[4] = "";
    } else {
        l[3] = LT("ovl.ra_manual");
        l[4] = LT("ovl.ra_cont");
    }
    sb_ovl_notice(l, 5);
    if (g_cfg.replay_autoplay_shots) g_shot_census = 1; /* DIAG: Hinweis erscheint im naechsten Bild */
}

void sb_rauto_tick(void)
{
    ULONGLONG now;
    unsigned dt = 0, scene, mloop;
    if (g_ra.phase == RA_OFF) {
        return;
    }
    if (g_ra.phase == RA_FAIL || g_ra.phase == RA_DONE) {
        if (g_ra.phase == RA_FAIL && !g_ra_noticed && ra_key(&g_ra, g_frames) == RA_KEY_NONE) {
            g_ra_noticed = 1;
            HLOG_W("REPLAY-AUTO: abgebrochen (Fehler %d, Phase vorher %s) - Spiel laeuft normal weiter",
                   g_ra.err, ra_phase_name(g_ra_last_phase));
            ra_notice(g_ra.err);
        }
        return;
    }
    if (g_ra.phase == RA_TITLE && g_ra_path_state == 0 && g_frames > 2u && !ra_path()) {
        /* Datei fehlt (vom Launcher geloescht/verschoben?): nicht navigieren */
        g_ra.phase = RA_FAIL;
        g_ra.err = RA_ERR_MISSING;
        HLOG_W("REPLAY-AUTO: Replay %s nicht gefunden", g_cfg.replay_autoplay);
        return;
    }
    now = GetTickCount64();
    if (g_ra_t0) {
        dt = (unsigned)(now - g_ra_t0);
    }
    g_ra_t0 = now;
    if (g_cfg.overlay && sb_ovl_open()) {
        return; /* Menue offen: Spiel pausiert, Zeit laeuft nicht */
    }
    if (dt > 1000u) dt = 1000u; /* Ladepausen/Haenger nicht als Wartezeit zaehlen */
    scene = ra_rd(SB_ST_SCENE);
    mloop = ra_rd(SB_ST_MLOOP);
    g_ra.rep_open = (int)g_ra_rep;
    ra_step(&g_ra, g_frames, scene, mloop, dt);
    if (g_ra.phase != g_ra_last_phase) {
        HLOG_I("REPLAY-AUTO: Phase %s (f=%u szene=0x%X mloop=%u, gefiltert=%ld, umgeleitet=%ld)",
               ra_phase_name(g_ra.phase), g_frames, scene, mloop, (long)g_ra_filtered, (long)g_ra_redir);
        if (g_ra.phase == RA_LIST || g_ra.phase == RA_CONFIRM) InterlockedExchange(&g_ra_rep, 0);
        if (g_cfg.replay_autoplay_shots) g_shot_census = 1; /* DIAG: Bild zum Phasenwechsel */
        if (g_ra.phase == RA_PLAY) {
            HLOG_I("REPLAY-AUTO: Wiedergabe laeuft (Replay %s)", g_cfg.replay_autoplay);
        } else if (g_ra.phase == RA_DONE) {
            HLOG_I("REPLAY-AUTO: Wiedergabe beendet - normales Spiel");
        }
        if (g_ra.phase != RA_FAIL) g_ra_last_phase = g_ra.phase;
    }
    if (g_ra.phase == RA_PLAY && g_cfg.replay_camera >= 0 && ra_rd(RA_REPLAY_ON) == 1u) {
        __try {
            *(volatile unsigned *)(uintptr_t)SBA(RA_REPLAY_CAM) = (unsigned)g_cfg.replay_camera;
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
        if (!g_ra_cam_logged) {
            g_ra_cam_logged = 1;
            HLOG_I("REPLAY-AUTO: Kamera fest auf Ansicht %d", g_cfg.replay_camera);
        }
    }
}

void sb_rauto_apply(void *blk)
{
    sbc_in_t *s = (sbc_in_t *)blk;
    int k;
    if (g_ra.phase == RA_OFF || !s) {
        return;
    }
    k = ra_key(&g_ra, g_frames);
    if (!ra_navigating(&g_ra) && k == RA_KEY_NONE) {
        return; /* Wiedergabe/danach: echte Eingabe */
    }
    if (g_cfg.overlay && sb_ovl_open()) {
        return; /* Overlay sperrt echte Eingaben ohnehin */
    }
    /* Menue-Fuehrung: echte Tasten/Hebel nicht ans Spiel, nur die eigene Taste */
    s->buttons[0] = s->buttons[1] = s->buttons[2] = 0;
    s->sight_x = 0;
    s->sight_y = 0;
    if (k == RA_KEY_OK) s->buttons[0] |= 1u << 0;        /* bi0 Hauptwaffe */
    else if (k == RA_KEY_BACK) s->buttons[0] |= 1u << 2; /* bi2 Lock-On = zurueck */
    else if (k == RA_KEY_UP) s->sight_y = -127;
    else if (k == RA_KEY_DOWN) s->sight_y = 127;
}
