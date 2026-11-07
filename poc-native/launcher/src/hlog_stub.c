/* hlog_stub.c - W148 PACK-LAUNCHER: HLOG-Ersatz fuer den Launcher.
 * sb_packscan.c (aus dem Harness einkompiliert) loggt per HLOG_*; der
 * Launcher hat keinen Harness-Logger. Meldungen gehen in die Debug-Ausgabe
 * (nur im Debugger sichtbar), sonst verfallen sie. Kein Verhalten im Spiel. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include "hlog.h"

void hlog_init(const char *path, int level)
{
    (void)path;
    (void)level;
}

void hlog_close(void)
{
}

int hlog_level(void)
{
    return HLOG_INFO;
}

void hlog_init_append(const char *path, int level)
{
    (void)path;
    (void)level;
}

long hlog_dropped(void)
{
    return 0;
}

void hlog_msg(int level, const char *fmt, ...)
{
    char b[1024];
    va_list ap;
    (void)level;
    va_start(ap, fmt);
    _vsnprintf_s(b, sizeof b, _TRUNCATE, fmt, ap);
    va_end(ap);
    OutputDebugStringA("sb_pack: ");
    OutputDebugStringA(b);
    OutputDebugStringA("\n");
}

int hlog_throttle(uint32_t *counter)
{
    (*counter)++;
    return *counter <= 3 || (*counter % 1000) == 0;
}
