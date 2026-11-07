/* launcher_u8.c - UTF-8-Schicht des Launchers (W82 LAUNCHER-UI2).
 * ASCII-Quelltext. s. launcher_u8.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "launcher_u8.h"

int u8_to_w(const char *s, wchar_t *out, int cap)
{
    int n;
    if (!out || cap <= 0) {
        return 0;
    }
    out[0] = 0;
    if (!s || !*s) {
        return 0;
    }
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap);
    if (n <= 0) {
        /* zu klein: gekuerzt neu (ohne NUL), dann terminieren */
        n = MultiByteToWideChar(CP_UTF8, 0, s, (int)strlen(s), out, cap - 1);
        if (n < 0) {
            n = 0;
        }
        out[n] = 0;
        return n;
    }
    return n - 1;
}

int w_to_u8(const wchar_t *s, char *out, int cap)
{
    int n;
    if (!out || cap <= 0) {
        return 0;
    }
    out[0] = 0;
    if (!s || !*s) {
        return 0;
    }
    n = WideCharToMultiByte(CP_UTF8, 0, s, -1, out, cap, NULL, NULL);
    if (n <= 0) {
        /* zu klein: zeichenweise kuerzen (keine halben Sequenzen) */
        int len = (int)wcslen(s);
        while (len > 0) {
            n = WideCharToMultiByte(CP_UTF8, 0, s, len, out, cap - 1, NULL,
                                    NULL);
            if (n > 0) {
                out[n] = 0;
                return n;
            }
            len--;
        }
        out[0] = 0;
        return 0;
    }
    return n - 1;
}

wchar_t *u8_wdup(const char *s)
{
    int n;
    wchar_t *w;
    if (!s) {
        s = "";
    }
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) {
        n = 1;
    }
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
        w[0] = 0;
    }
    return w;
}

int u8_is_ascii(const char *s)
{
    for (; s && *s; s++) {
        if ((unsigned char)*s >= 0x80) {
            return 0;
        }
    }
    return 1;
}

void u8_from_acp(const char *acp, char *out, size_t cap)
{
    wchar_t w[2048];
    if (!out || cap == 0) {
        return;
    }
    if (u8_is_ascii(acp)) {
        strcpy_s(out, cap, acp ? acp : "");
        return;
    }
    if (MultiByteToWideChar(CP_ACP, 0, acp, -1, w, 2048) <= 0) {
        strcpy_s(out, cap, acp);
        return;
    }
    w_to_u8(w, out, (int)cap);
}

static int w_to_acp_exact(const wchar_t *w, char *out, size_t cap)
{
    BOOL used = FALSE;
    int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w, -1, out,
                                (int)cap, NULL, &used);
    if (n <= 0) {
        out[0] = 0;
        return 0;
    }
    return used ? 0 : 1;
}

int u8_to_acp(const char *u8, char *out, size_t cap)
{
    wchar_t w[2048], sh[2048];
    if (!out || cap == 0) {
        return 2;
    }
    if (u8_is_ascii(u8)) {
        strcpy_s(out, cap, u8 ? u8 : "");
        return 0;
    }
    u8_to_w(u8, w, 2048);
    if (w_to_acp_exact(w, out, cap)) {
        return 0;
    }
    /* Nicht in der ANSI-Codepage: 8.3-Kurzname (nur existierende Pfade,
     * nur wenn das Laufwerk Kurznamen fuehrt). */
    if (GetShortPathNameW(w, sh, 2048) > 0 && w_to_acp_exact(sh, out, cap)) {
        return 1;
    }
    /* Ersatzzeichen (Client kann den Pfad nicht oeffnen). */
    WideCharToMultiByte(CP_ACP, 0, w, -1, out, (int)cap, NULL, NULL);
    return 2;
}

int u8_fopen_s(FILE **f, const char *path, const char *mode)
{
    wchar_t wp[2048], wm[16];
    *f = NULL;
    if (!path || !*path) {
        return 1;
    }
    u8_to_w(path, wp, 2048);
    u8_to_w(mode, wm, 16);
    return _wfopen_s(f, wp, wm);
}

unsigned long u8_GetFileAttributes(const char *path)
{
    wchar_t wp[2048];
    if (!path || !*path) {
        return INVALID_FILE_ATTRIBUTES;
    }
    u8_to_w(path, wp, 2048);
    return GetFileAttributesW(wp);
}

int u8_DeleteFile(const char *path)
{
    wchar_t wp[2048];
    u8_to_w(path, wp, 2048);
    return DeleteFileW(wp) ? 1 : 0;
}

int u8_CreateDirectory(const char *path)
{
    wchar_t wp[2048];
    u8_to_w(path, wp, 2048);
    return CreateDirectoryW(wp, NULL) ? 1 : 0;
}

unsigned long u8_GetEnv(const char *name, char *out, size_t cap)
{
    wchar_t wn[128], wv[2048];
    DWORD n;
    if (out && cap) {
        out[0] = 0;
    }
    u8_to_w(name, wn, 128);
    n = GetEnvironmentVariableW(wn, wv, 2048);
    if (n == 0 || n >= 2048) {
        return 0;
    }
    return (unsigned long)w_to_u8(wv, out, (int)cap);
}

void u8_GetTempPath(char *out, size_t cap)
{
    wchar_t w[MAX_PATH + 2];
    DWORD n = GetTempPathW(MAX_PATH + 1, w);
    if (n == 0 || n > MAX_PATH) {
        strcpy_s(out, cap, ".\\");
        return;
    }
    w_to_u8(w, out, (int)cap);
}

int u8_ExeDir(char *out, size_t cap)
{
    wchar_t w[2048];
    wchar_t *sep;
    DWORD n = GetModuleFileNameW(NULL, w, 2048);
    if (n == 0 || n >= 2048) {
        return 0;
    }
    sep = wcsrchr(w, L'\\');
    if (!sep) {
        return 0;
    }
    *sep = 0;
    w_to_u8(w, out, (int)cap);
    return 1;
}

int u8_find_first(const char *pattern, u8_find_t *fd)
{
    wchar_t wp[2048];
    WIN32_FIND_DATAW d;
    HANDLE h;
    u8_to_w(pattern, wp, 2048);
    fd->h = NULL;
    h = FindFirstFileW(wp, &d);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    fd->h = h;
    fd->attr = d.dwFileAttributes;
    w_to_u8(d.cFileName, fd->name, (int)sizeof fd->name);
    return 1;
}

int u8_find_next(u8_find_t *fd)
{
    WIN32_FIND_DATAW d;
    if (!fd->h) {
        return 0;
    }
    if (!FindNextFileW((HANDLE)fd->h, &d)) {
        return 0;
    }
    fd->attr = d.dwFileAttributes;
    w_to_u8(d.cFileName, fd->name, (int)sizeof fd->name);
    return 1;
}

void u8_find_close(u8_find_t *fd)
{
    if (fd->h) {
        FindClose((HANDLE)fd->h);
        fd->h = NULL;
    }
}

int u8_CreateProcess(const char *cmdline, const char *cwd, unsigned long flags,
                     int inherit, void *si_stdout, int hide,
                     unsigned long *out_pid, void **out_proc)
{
    wchar_t *wc = u8_wdup(cmdline);
    wchar_t wd[2048];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    BOOL ok;
    if (!wc) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    u8_to_w(cwd, wd, 2048);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (hide) {
        si.dwFlags |= STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
    }
    if (si_stdout) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = (HANDLE)si_stdout;
        si.hStdInput = NULL;
        si.hStdError = NULL;
    }
    memset(&pi, 0, sizeof pi);
    ok = CreateProcessW(NULL, wc, NULL, NULL, inherit ? TRUE : FALSE, flags,
                        NULL, (cwd && *cwd) ? wd : NULL, &si, &pi);
    free(wc);
    if (!ok) {
        return 0;
    }
    CloseHandle(pi.hThread);
    if (out_pid) {
        *out_pid = pi.dwProcessId;
    }
    if (out_proc) {
        *out_proc = pi.hProcess;
    } else {
        CloseHandle(pi.hProcess);
    }
    return 1;
}
