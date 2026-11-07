/* hlog.c - Thread-sicherer, niemals blockierender Logger.
 *
 * HINTERGRUND (boot314): Jede Zeile ging per fputs/fflush auf zwei CRT-FILEs
 * (stdout + Logdatei). Die CRT sperrt FILEs intern; das eigene TryEnter
 * schuetzte nur das eigene Schloss, NICHT die CRT-Locks. Suspendiert das
 * Spiel (NtSuspendThread-Sturm beim Voice-Streaming) einen Thread mitten in
 * fflush, blockiert JEDE weitere Zeile fuer immer -> totale Stille bei
 * lebendigem Prozess (Main+Loader+Watchdog+Sampler stecken alle in HLOG).
 * FIX: rohe HANDLEs + WriteFile (kein CRT-Lock) + TryEnter-Spin; wer das
 * Schloss nicht bekommt, verwirft die Zeile und zaehlt (g_dropped).
 * Logging darf einen Prozess niemals aufhaengen. */
#include "hlog.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h> /* W188 */
#include <windows.h>

static HANDLE g_h = INVALID_HANDLE_VALUE; /* Logdatei (roh, ohne CRT-Lock). */
static HANDLE g_out = INVALID_HANDLE_VALUE; /* stdout (roh, zur Laufzeit). */
static int g_level = HLOG_INFO;
static CRITICAL_SECTION g_cs;
static int g_cs_init;
static uint64_t g_qpf;
static volatile LONG g_dropped;

/* PERF(PERF-TICK,W168) hlog_async: Zeilen in einen Doppelpuffer (Kopie unter kurzem Schloss), ein
 * Schreib-Thread schreibt sie per WriteFile. Befund W168_t1m1: ein WriteFile der Zeile PERFTHR stand
 * 49 ms im Spiel-Thread (Tick-Last 62,9 ms); synchrones Schreiben haelt ausserdem das Log-Schloss, an dem
 * andere Threads warten. Reihenfolge bleibt erhalten, voller Puffer verwirft (g_dropped wie bisher).
 * ERROR-Zeilen und hlog_close/hlog_flush warten bis zu 200 ms auf das Leeren. */
#define HLOG_ABUF (1u << 20)
static char g_abuf[2][HLOG_ABUF];
static size_t g_alen[2];
static int g_acur;
static volatile LONG g_async;
static HANDLE g_aev, g_athr;
static volatile LONG g_apend; /* Bytes im Schreib-Thread noch nicht geschrieben */

static void hlog_write_raw(const char *p, DWORD n)
{
    DWORD w;
    if (g_out != INVALID_HANDLE_VALUE && g_out != NULL) {
        WriteFile(g_out, p, n, &w, NULL);
    }
    {
        HANDLE h = g_h; /* Einmal lesen (Shutdown nullt per XCHG). */
        if (h != INVALID_HANDLE_VALUE && h != NULL) {
            WriteFile(h, p, n, &w, NULL);
        }
    }
}

static DWORD WINAPI hlog_writer(LPVOID arg)
{
    (void)arg;
    for (;;) {
        int k;
        size_t n;
        WaitForSingleObject(g_aev, 20);
        EnterCriticalSection(&g_cs);
        k = g_acur;
        n = g_alen[k];
        if (n) { g_acur ^= 1; g_alen[g_acur] = 0; }
        LeaveCriticalSection(&g_cs);
        if (n) {
            hlog_write_raw(g_abuf[k], (DWORD)n);
            InterlockedExchangeAdd(&g_apend, -(LONG)n);
        }
    }
}

static void hlog_flush_ms(int ms)
{
    int i;
    if (!g_async) { return; }
    for (i = 0; i < ms && g_apend > 0; i++) {
        SetEvent(g_aev);
        Sleep(1);
    }
}

void hlog_flush(void)
{
    hlog_flush_ms(200);
}

void hlog_async_start(void)
{
    if (g_async || !g_cs_init) { return; }
    g_aev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!g_aev) { return; }
    g_athr = CreateThread(NULL, 0, hlog_writer, NULL, 0, NULL);
    if (!g_athr) { CloseHandle(g_aev); g_aev = NULL; return; }
    SetThreadPriority(g_athr, THREAD_PRIORITY_BELOW_NORMAL);
    InterlockedExchange(&g_async, 1);
}

static char g_hlog_path[600]; /* W188 GAME-MENU: Feedback liest das Log-Ende */
const char *hlog_path(void)
{
    return g_hlog_path;
}

void hlog_init(const char *path, int level)
{
    if (path) { strncpy_s(g_hlog_path, sizeof g_hlog_path, path, _TRUNCATE); }
    g_level = level;
    if (!g_cs_init) {
        InitializeCriticalSection(&g_cs);
        g_cs_init = 1;
    }
    QueryPerformanceFrequency((LARGE_INTEGER *)&g_qpf);
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    /* CREATE_ALWAYS: frisches Log pro Boot (wie bisher fopen "w"). */
    g_h = CreateFileA(path, GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "WARN: %s nicht schreibbar (%lu)\n", path,
                GetLastError());
    }
}

void hlog_init_append(const char *path, int level)
{
    if (path) { strncpy_s(g_hlog_path, sizeof g_hlog_path, path, _TRUNCATE); }
    g_level = level;
    if (!g_cs_init) {
        InitializeCriticalSection(&g_cs);
        g_cs_init = 1;
    }
    QueryPerformanceFrequency((LARGE_INTEGER *)&g_qpf);
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    /* OPEN_ALWAYS + ans Ende: behaelt die bisherigen Zeilen (A6 hlog_keep).
     * Der Aufrufer hat die Datei zuvor per hlog_close geschlossen. */
    g_h = CreateFileA(path, GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "WARN: %s nicht schreibbar (%lu)\n", path,
                GetLastError());
    } else {
        SetFilePointer(g_h, 0, NULL, FILE_END);
    }
}

void hlog_close(void)
{
    hlog_flush(); /* W168 hlog_async: Rest schreiben (bis 200 ms) */
    /* KEIN CloseHandle: Shutdown (Watchdog/Main/last_chance) rast mit noch
     * loggenden Threads; use-after-close korrumpiert -> FAIL_FAST
     * (boot302, Dump 222844). WriteFile ist ungepuffert (jede Zeile sofort
     * auf Platte); ExitProcess gibt OS-Ressourcen frei. */
    InterlockedExchangePointer((void **)&g_h, (void *)INVALID_HANDLE_VALUE);
}

int hlog_level(void)
{
    return g_level;
}

long hlog_dropped(void)
{
    return (long)g_dropped;
}

static const char *level_name(int level)
{
    switch (level) {
    case HLOG_ERROR: return "ERROR";
    case HLOG_WARN: return "WARN";
    case HLOG_INFO: return "INFO";
    default: return "TRACE";
    }
}

void hlog_msg(int level, const char *fmt, ...)
{
    uint64_t now = 0;
    double sec = 0.0;
    va_list ap;
    char line[2048];
    int n, m;
    DWORD w;
    int i, got = 0;

    if (level > g_level) {
        return;
    }
    QueryPerformanceCounter((LARGE_INTEGER *)&now);
    if (g_qpf) {
        sec = (double)now / (double)g_qpf;
    }
    /* Formatieren AUSSERHALB des Schlosses (kostet nichts, blockiert nie). */
    n = _snprintf_s(line, sizeof line, _TRUNCATE, "[%9.3f] [%-5s] [T%04x] ",
                    sec, level_name(level), GetCurrentThreadId());
    if (n < 0) {
        n = 0;
    }
    va_start(ap, fmt);
    m = vsnprintf_s(line + n, sizeof line - (size_t)n - 2u, _TRUNCATE, fmt,
                    ap);
    va_end(ap);
    if (m < 0) {
        m = 0;
    }
    /* "\r\n" selbst anhaengen (rohes WriteFile uebersetzt kein \n). */
    line[n + m] = '\r';
    line[n + m + 1] = '\n';
    /* Kurz spinnen (langsamer Halter), dann verwerfen (suspendierter
     * Halter gibt nie frei -> kein ignorant langes Warten). */
    for (i = 0; i < 2000; i++) {
        if (TryEnterCriticalSection(&g_cs)) {
            got = 1;
            break;
        }
        YieldProcessor();
    }
    if (!got) {
        InterlockedIncrement(&g_dropped);
        return;
    }
    if (g_async) { /* PERF(PERF-TICK,W168): nur kopieren, Schreib-Thread schreibt */
        size_t ln = (size_t)(n + m + 2);
        int ok = 0;
        if (g_alen[g_acur] + ln <= HLOG_ABUF) {
            memcpy(g_abuf[g_acur] + g_alen[g_acur], line, ln);
            g_alen[g_acur] += ln;
            InterlockedExchangeAdd(&g_apend, (LONG)ln);
            ok = 1;
        }
        LeaveCriticalSection(&g_cs);
        if (!ok) { InterlockedIncrement(&g_dropped); }
        SetEvent(g_aev);
        if (level == HLOG_ERROR) { hlog_flush_ms(20); }
        return;
    }
    hlog_write_raw(line, (DWORD)(n + m + 2));
    (void)w;
    LeaveCriticalSection(&g_cs);
}

int hlog_throttle(uint32_t *counter)
{
    uint32_t c = *counter;
    *counter = c + 1;
    return c < HLOG_CALL_MAX_FIRST || (c % 1000) == 0;
}
