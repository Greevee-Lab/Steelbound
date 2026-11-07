/* sb_relaunch.c - W186 RESET-TITLE: "Zurueck zum Titel" per Client-Neustart.
 *
 * Befund (Disasm der eigenen XBE, FUN_000E0140 = Dialog-Automat der Menues):
 * CONFIGURATION (Szene 6, Hauptmenue) bzw. CONFIG (Szene 0x1F, Briefing-Menue)
 * -> BACK TO TITLE -> "Reset the game and return to the title screen?" -> YES
 * haelt einen Spiel-Thread an, faehrt Subsysteme herunter (0x10A630 ...
 * 0x14A70) und ruft @0xE066D XLaunchNewImageA("D:\default.xbe", &LD) mit
 * genulltem LAUNCH_DATA. Auf der Xbox kehrt der Aufruf nie zurueck (Reboot
 * in das eigene Image). Der alte Hook meldete Erfolg und ignorierte ihn ->
 * das Spiel lief mit heruntergefahrenen Subsystemen weiter ("YES tut nichts").
 *
 * Umsetzung: sauberer Neustart des ganzen Clients (robuster als ein interner
 * Reset: das XBE liegt fest ab 0x10000 im Prozess, Spiel-Threads, Kernel-HLE-
 * Objekte, D3D/Ton/VR haben Prozesszustand). Ablauf:
 *  Eltern: Log-Zeilen, Zustand an das Kind (Umgebung), Kind starten (gleiche
 *          Kommandozeile, gleicher Arbeitsordner, Umgebung geerbt, nur die
 *          Std-Handles + ein SYNCHRONIZE-Handle auf sich selbst werden
 *          vererbt), PID nach <out>/relaunch.pid (run_once.ps1 folgt ihr),
 *          Exklusiv-Vollbild verlassen, geordnetes Ende wie --run-seconds,
 *          ExitProcess(0).
 *  Kind:   wartet ganz am Anfang auf das Ende des Elternprozesses (dann sind
 *          Ton, D3D/Swapchain, VR-Session, WinUSB-Handle, Logdatei frei),
 *          haengt an sb_harness.log an, wendet SB_RELAUNCH_SET an (Standard:
 *          replay_steps=/replay= leer, Fenster-Modus des Elternprozesses),
 *          erbt bei Testlaeufen die Rest-Laufzeit (--run-seconds).
 * Fokus: Das Kind erbt window_hidden/SB_CFG_SET -> Testlaeufe bleiben
 * unsichtbar ohne Aktivierung; nur im Spielerbetrieb mit eigenem Fenster im
 * Vordergrund darf das Kind den Vordergrund uebernehmen. Ohne Konsolenfenster
 * startet das Kind mit CREATE_NO_WINDOW (nie ein neues Konsolenfenster). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>
#include "hlog.h"
#include "hcfg.h"
#include "sb_relaunch.h"

/* emu_main.c / render_winmode.c */
int emu_run_seconds_left(void);
void emu_shutdown_for_exit(void);
int sb_winmode_current(void);
void sb_winmode_on_close(void);
HWND sb_render_hwnd(void);

static ULONGLONG g_t0;           /* Prozessstart (sb_emulate) */
static int g_child;              /* N-ter Neustart (0 = Erststart) */
static int g_child_run = -1;     /* Rest-Laufzeit aus dem Elternprozess */
static DWORD g_wait_ms;          /* Wartezeit auf den Elternprozess */
static int g_wait_res = -1;      /* 0 = beendet, 1 = Zeitablauf, 2 = kein Handle */
static int g_have_pos;
static int g_pos_x, g_pos_y;
static wchar_t g_cwd[MAX_PATH + 1];
static char g_set[512];          /* SB_RELAUNCH_SET (Kind) */
static char g_from[260];         /* SB_RELAUNCH_FROM (Kind, nur Log) */

#define RL_WAIT_MAX_MS 30000u    /* Elternprozess-Ende abwarten (hoechstens) */
#define RL_MIN_LEFT_S 20         /* Testlauf: Rest darunter -> sauber beenden */
#define RL_MIN_UPTIME_MS 5000u   /* Neustart-Schleifen-Schutz */

static int env_int(const char *name, int def)
{
    char b[32];
    DWORD n = GetEnvironmentVariableA(name, b, sizeof b);
    if (n == 0 || n >= sizeof b) {
        return def;
    }
    return atoi(b);
}

void sb_relaunch_boot(void)
{
    char b[64];
    DWORD n;
    g_t0 = GetTickCount64();
    if (!GetCurrentDirectoryW(MAX_PATH, g_cwd)) {
        g_cwd[0] = 0;
    }
    g_child = env_int("SB_RELAUNCH_N", 0);
    if (g_child <= 0) {
        g_child = 0;
        return;
    }
    g_child_run = env_int("SB_RELAUNCH_RUN_SECONDS", -1);
    n = GetEnvironmentVariableA("SB_RELAUNCH_WAIT", b, sizeof b);
    if (n > 0 && n < sizeof b) {
        HANDLE h = (HANDLE)(ULONG_PTR)strtoul(b, NULL, 10);
        ULONGLONG w0 = GetTickCount64();
        DWORD r = WaitForSingleObject(h, RL_WAIT_MAX_MS);
        g_wait_ms = (DWORD)(GetTickCount64() - w0);
        g_wait_res = (r == WAIT_OBJECT_0) ? 0 : 1;
        CloseHandle(h);
    } else {
        g_wait_res = 2;
    }
    /* Werte gelten nur fuer diesen Prozess (Enkel bekommt sie neu). */
    SetEnvironmentVariableA("SB_RELAUNCH_WAIT", NULL);
    n = GetEnvironmentVariableA("SB_RELAUNCH_SET", g_set, sizeof g_set);
    if (n == 0 || n >= sizeof g_set) {
        g_set[0] = 0;
    }
    n = GetEnvironmentVariableA("SB_RELAUNCH_FROM", g_from, sizeof g_from);
    if (n == 0 || n >= sizeof g_from) {
        g_from[0] = 0;
    }
    n = GetEnvironmentVariableA("SB_RELAUNCH_WINPOS", b, sizeof b);
    if (n > 0 && n < sizeof b && sscanf_s(b, "%d,%d", &g_pos_x, &g_pos_y) == 2) {
        g_have_pos = 1;
    }
}

int sb_relaunch_child(void)
{
    return g_child;
}

int sb_relaunch_run_seconds(void)
{
    return g_child ? g_child_run : -1;
}

void sb_relaunch_after_cfg(void)
{
    if (!g_child) {
        return;
    }
    HLOG_I("RELAUNCH: Neustart #%d nach XLaunchNewImage('%s') - Elternprozess %s nach %lu ms; "
           "run-seconds=%d, Ersatz-Set '%s'",
           g_child, g_from[0] ? g_from : "?",
           g_wait_res == 0 ? "beendet" : (g_wait_res == 1 ? "NICHT beendet (Zeitablauf)" : "unbekannt (kein Handle)"),
           (unsigned long)g_wait_ms, g_child_run, g_set);
    if (g_set[0]) {
        hcfg_apply_set(g_set, "relaunch");
    }
}

int sb_relaunch_winpos(int *x, int *y)
{
    POINT p;
    if (!g_child || !g_have_pos || g_cfg.window_hidden) {
        return 0;
    }
    p.x = g_pos_x + 8;
    p.y = g_pos_y + 8;
    if (!MonitorFromPoint(p, MONITOR_DEFAULTTONULL)) {
        return 0; /* Monitor inzwischen weg -> Standardposition */
    }
    *x = g_pos_x;
    *y = g_pos_y;
    return 1;
}

/* Vererbbare Kopie eines Handles (NULL bei ungueltigem Handle). */
static HANDLE dup_inherit(HANDLE h)
{
    HANDLE d = NULL;
    if (!h || h == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, 0, TRUE,
                         DUPLICATE_SAME_ACCESS)) {
        return NULL;
    }
    return d;
}

static void rl_exit(const char *why)
{
    HLOG_I("RELAUNCH: %s -> Exit(0)", why);
    sb_winmode_on_close();
    emu_shutdown_for_exit();
    ExitProcess(0);
}

/* Zustand fuer das Kind in die eigene Umgebung (CreateProcess erbt sie). */
static void rl_env(const char *path, int left)
{
    char b[600];
    HWND hw = sb_render_hwnd();
    int wm = g_cfg.window_hidden ? -1 : sb_winmode_current();
    _snprintf_s(b, sizeof b, _TRUNCATE, "%d", g_child + 1);
    SetEnvironmentVariableA("SB_RELAUNCH_N", b);
    SetEnvironmentVariableA("SB_RELAUNCH_FROM", path);
    if (left >= 0) {
        _snprintf_s(b, sizeof b, _TRUNCATE, "%d", left);
        SetEnvironmentVariableA("SB_RELAUNCH_RUN_SECONDS", b);
    } else {
        SetEnvironmentVariableA("SB_RELAUNCH_RUN_SECONDS", NULL);
    }
    b[0] = 0;
    if (!g_cfg.xlaunch_replay) {
        /* Die Replay-Kette ist das Drehbuch DIESES Prozesses: das Kind spielt
         * sie nicht erneut ab (sonst Endlosschleife Menue -> Neustart).
         * movie_skip/pad_replay_gate bleiben -> Eingabe bleibt gesperrt. */
        strcpy_s(b, sizeof b, "replay_steps=;replay=");
    }
    if (wm >= 0 && wm != g_cfg.window_mode) {
        size_t k = strlen(b);
        _snprintf_s(b + k, sizeof b - k, _TRUNCATE, "%swindow_mode=%d", k ? ";" : "", wm);
    }
    SetEnvironmentVariableA("SB_RELAUNCH_SET", b[0] ? b : NULL);
    SetEnvironmentVariableA("SB_RELAUNCH_WINPOS", NULL);
    if (hw && !g_cfg.window_hidden && !IsIconic(hw)) {
        RECT r;
        int ok = 0;
        if (wm == 0 && GetWindowRect(hw, &r)) {
            ok = 1;
        } else {
            MONITORINFO mi;
            mi.cbSize = sizeof mi;
            if (GetMonitorInfoA(MonitorFromWindow(hw, MONITOR_DEFAULTTONEAREST), &mi)) {
                r.left = mi.rcWork.left + 40; /* Vollbild: Fenster auf demselben Monitor */
                r.top = mi.rcWork.top + 40;
                ok = 1;
            }
        }
        if (ok) {
            _snprintf_s(b, sizeof b, _TRUNCATE, "%ld,%ld", (long)r.left, (long)r.top);
            SetEnvironmentVariableA("SB_RELAUNCH_WINPOS", b);
        }
    }
}

void sb_relaunch_exec(const char *path)
{
    STARTUPINFOEXW si;
    STARTUPINFOW psi;
    PROCESS_INFORMATION pi;
    LPPROC_THREAD_ATTRIBUTE_LIST al = NULL;
    SIZE_T alsz = 0;
    HANDLE inh[4], self = NULL, hin, hout, herr;
    int ninh = 0, left, ok, i;
    DWORD flags = EXTENDED_STARTUPINFO_PRESENT, gle = 0;
    wchar_t exe[MAX_PATH + 1], *cmd = NULL;
    char b[64];
    HWND hw;
    ULONGLONG up = GetTickCount64() - g_t0;

    if (g_cfg.xlaunch_self == 2) {
        rl_exit("xlaunch_self=2 (sauberes Ende statt Neustart)");
    }
    if (g_cfg.xlaunch_self != 1) {
        return;
    }
    left = emu_run_seconds_left();
    if (left >= 0 && left < RL_MIN_LEFT_S) {
        /* Testlauf fast vorbei: kein Kind mehr starten, sauber enden. */
        rl_exit("Testlauf: Rest-Laufzeit zu kurz fuer einen Neustart");
    }
    if (g_child && up < RL_MIN_UPTIME_MS) {
        HLOG_W("RELAUNCH: Neustart #%d nach nur %llu ms erneut verlangt -> abgelehnt (Schleifen-Schutz)",
               g_child, up);
        return;
    }
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) {
        HLOG_E("RELAUNCH: GetModuleFileName fehlgeschlagen (GLE=%lu)", GetLastError());
        return;
    }
    {
        const wchar_t *c = GetCommandLineW();
        size_t n = wcslen(c) + 1;
        cmd = (wchar_t *)malloc(n * sizeof(wchar_t)); /* CreateProcessW schreibt hinein */
        if (!cmd) {
            return;
        }
        wcscpy_s(cmd, n, c);
    }
    /* SYNCHRONIZE-Handle auf uns selbst: das Kind wartet darauf. */
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &self,
                         SYNCHRONIZE, TRUE, 0)) {
        self = NULL;
    }
    if (self) {
        inh[ninh++] = self;
        _snprintf_s(b, sizeof b, _TRUNCATE, "%lu", (unsigned long)(ULONG_PTR)self);
        SetEnvironmentVariableA("SB_RELAUNCH_WAIT", b);
    } else {
        SetEnvironmentVariableA("SB_RELAUNCH_WAIT", NULL);
    }
    rl_env(path, left);

    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    GetStartupInfoW(&psi);
    if (psi.dwFlags & STARTF_USESHOWWINDOW) { /* wie der eigene Start */
        si.StartupInfo.dwFlags |= STARTF_USESHOWWINDOW;
        si.StartupInfo.wShowWindow = psi.wShowWindow;
    }
    /* Std-Handles (boot.log/boot.err bei run_once) gezielt vererben. */
    hin = dup_inherit(GetStdHandle(STD_INPUT_HANDLE));
    hout = dup_inherit(GetStdHandle(STD_OUTPUT_HANDLE));
    herr = dup_inherit(GetStdHandle(STD_ERROR_HANDLE));
    if (hin) { inh[ninh++] = hin; }
    if (hout) { inh[ninh++] = hout; }
    if (herr) { inh[ninh++] = herr; }
    if (hout || herr) {
        si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = hin;
        si.StartupInfo.hStdOutput = hout;
        si.StartupInfo.hStdError = herr;
    }
    if (ninh > 0) {
        InitializeProcThreadAttributeList(NULL, 1, 0, &alsz);
        al = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(alsz);
        if (al && InitializeProcThreadAttributeList(al, 1, 0, &alsz) &&
            UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inh,
                                      (SIZE_T)ninh * sizeof(HANDLE), NULL, NULL)) {
            si.lpAttributeList = al;
        } else {
            free(al);
            al = NULL;
        }
    }
    if (!GetConsoleWindow()) {
        flags |= CREATE_NO_WINDOW; /* nie ein neues Konsolenfenster */
    }
    HLOG_W("RELAUNCH: XLaunchNewImage('%s') -> Client-Neustart #%d (Uptime %llu s, run-seconds-Rest %d, "
           "Ersatz-Set '%s', Fenster-Modus %d, Konsole %s)",
           path, g_child + 1, up / 1000u, left,
           GetEnvironmentVariableA("SB_RELAUNCH_SET", b, sizeof b) ? b : "",
           g_cfg.window_hidden ? -1 : sb_winmode_current(), GetConsoleWindow() ? "geerbt" : "keine");
    hlog_flush();
    memset(&pi, 0, sizeof pi);
    ok = CreateProcessW(exe, cmd, NULL, NULL, al ? TRUE : FALSE, flags, NULL,
                        g_cwd[0] ? g_cwd : NULL, &si.StartupInfo, &pi);
    if (!ok) {
        gle = GetLastError();
    }
    if (al) {
        DeleteProcThreadAttributeList(al);
        free(al);
    }
    for (i = 0; i < ninh; i++) {
        CloseHandle(inh[i]);
    }
    free(cmd);
    if (!ok) {
        HLOG_E("RELAUNCH: CreateProcess fehlgeschlagen (GLE=%lu) -> wie bisher ignoriert", gle);
        SetEnvironmentVariableA("SB_RELAUNCH_WAIT", NULL);
        return;
    }
    HLOG_W("RELAUNCH: Kind PID %lu gestartet, alter Prozess endet", (unsigned long)pi.dwProcessId);
    { /* run_once.ps1 folgt dieser Datei (Testlauf haengt nicht am alten PID) */
        char pf[700];
        FILE *f = NULL;
        _snprintf_s(pf, sizeof pf, _TRUNCATE, "%s/relaunch.pid", hcfg_out_dir());
        if (fopen_s(&f, pf, "w") == 0 && f) {
            fprintf(f, "%lu\n", (unsigned long)pi.dwProcessId);
            fclose(f);
        }
    }
    hw = sb_render_hwnd();
    if (!g_cfg.window_hidden && emu_run_seconds_left() < 0 && hw && GetForegroundWindow() == hw) {
        /* Spielerbetrieb, unser Fenster hat den Fokus: Kind darf ihn uebernehmen. */
        AllowSetForegroundWindow(pi.dwProcessId);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    rl_exit("Neustart uebergeben");
}
