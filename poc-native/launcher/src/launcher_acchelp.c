/* launcher_acchelp.c - W537 LAUNCHER-SLIM: Aufruf des Konto-Hilfsprogramms sb_account.exe (s. launcher_acchelp.h).
 * Nur im Launcher. Quelltext ASCII. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "launcher_acchelp.h"
#include "launcher_account_cfg.h"
#include "launcher_app.h"
#include "launcher_u8.h"

#define printf cli_printf

/* Test-Ueberschreibungen wie im Discord-Ablauf (W534): nur Entwickler- und Test-Launcher */
#if (defined(SB_TEST_HOOKS) && SB_TEST_HOOKS) || (defined(SB_LAUNCHER_SELFTEST) && SB_LAUNCHER_SELFTEST)
#define LACH_TEST_ENV 1
#else
#define LACH_TEST_ENV 0
#endif

void lach_exe_path(char *out, size_t cap)
{
    char d[1024];
    if (!u8_ExeDir(d, sizeof d)) d[0] = '\0';
    _snprintf_s(out, cap, _TRUNCATE, "%s%s%s", d, d[0] ? "\\" : "", LACH_EXE);
}

static void set_err(char *err, size_t ecap, const char *code)
{
    if (err && ecap) strcpy_s(err, ecap, code);
}

/* Nur die erwartete Discord-Autorisierung oeffnen (das Hilfsprogramm baut die URL; Pruefung hier nochmals). */
static int url_allowed(const char *u)
{
    size_t n = strlen(SB_DISCORD_AUTHORIZE_URL);
    if (strncmp(u, SB_DISCORD_AUTHORIZE_URL, n) == 0 && u[n] == '?') return 1;
#if LACH_TEST_ENV
    if (strncmp(u, "http://127.0.0.1:", 17) == 0 || strncmp(u, "http://localhost:", 17) == 0) return 1;
#endif
    return 0;
}

static int open_url(const char *u)
{
    wchar_t *w;
    INT_PTR r;
    if (!url_allowed(u) || strpbrk(u, "\"' \r\n\t")) return 1;
#if LACH_TEST_ENV
    {
        char v[8];
        if (u8_GetEnv("SB_TEST_NO_BROWSER", v, sizeof v) > 0 && v[0] == '1') {
            printf("[discord-url] %s\n", u);
            return 0;
        }
    }
#endif
    w = u8_wdup(u);
    if (!w) return 1;
    r = (INT_PTR)ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    free(w);
    return r <= 32;
}

static void copy_in(char *dst, size_t cap, const char *src)
{
    if (src) strncpy_s(dst, cap, src, _TRUNCATE);
    else dst[0] = '\0';
}

int lach_run(const char *op, const char *dir, const char *a, const char *b, const char *c, int k, int timeout_ms,
             volatile long *cancel, lach_res_t *res, char *err, size_t ecap)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE map = NULL, ev_cancel = NULL, ev_url = NULL, parent = NULL, inh[4], waits[2];
    lach_shm_t *m = NULL;
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    LPPROC_THREAD_ATTRIBUTE_LIST al = NULL;
    SIZE_T als = 0;
    char exe[1100], cwd[1024];
    wchar_t wexe[1100], wcmd[1300], wcwd[1024];
    ULONGLONG t0 = GetTickCount64(), cancel_at = 0;
    int rc = 1, started = 0, sent_cancel = 0, timed_out = 0, browser_fail = 0, killed = 0;
    memset(&pi, 0, sizeof pi);
    if (res) memset(res, 0, sizeof *res);
    set_err(err, ecap, "helper");
    if (timeout_ms <= 0) timeout_ms = 60000;
    lach_exe_path(exe, sizeof exe);
    if (u8_GetFileAttributes(exe) == INVALID_FILE_ATTRIBUTES) {
        printf("[acc] %s fehlt\n", LACH_EXE);
        return 1;
    }
    /* Block + Ereignisse (unbenannt, vererbbar), Launcher-Prozess als vererbbares SYNCHRONIZE-Handle */
    map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, (DWORD)sizeof(lach_shm_t), NULL);
    ev_cancel = CreateEventW(&sa, TRUE, FALSE, NULL);
    ev_url = CreateEventW(&sa, FALSE, FALSE, NULL);
    if (!map || !ev_cancel || !ev_url ||
        !DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parent, SYNCHRONIZE, TRUE, 0))
        goto done;
    m = (lach_shm_t *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(lach_shm_t));
    if (!m) goto done;
    memset(m, 0, sizeof *m);
    memcpy(m->magic, LACH_MAGIC, sizeof LACH_MAGIC);
    m->size = (uint32_t)sizeof *m;
    m->res_size = (uint32_t)sizeof(lach_res_t);
    m->ev_cancel = (uint32_t)(UINT_PTR)ev_cancel;
    m->ev_url = (uint32_t)(UINT_PTR)ev_url;
    m->h_parent = (uint32_t)(UINT_PTR)parent;
    copy_in(m->op, sizeof m->op, op);
    copy_in(m->dir, sizeof m->dir, dir);
    copy_in(m->a, sizeof m->a, a);
    copy_in(m->b, sizeof m->b, b);
    copy_in(m->c, sizeof m->c, c);
    m->k = k;
    m->timeout_ms = timeout_ms;
    /* Nur diese vier Handles vererben (parallele Auftraege sehen einander nicht) */
    InitializeProcThreadAttributeList(NULL, 1, 0, &als);
    al = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, als);
    if (!al || !InitializeProcThreadAttributeList(al, 1, 0, &als)) {
        if (al) HeapFree(GetProcessHeap(), 0, al);
        al = NULL;
        goto done;
    }
    inh[0] = map;
    inh[1] = ev_cancel;
    inh[2] = ev_url;
    inh[3] = parent;
    if (!UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inh, sizeof inh, NULL, NULL)) goto done;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.lpAttributeList = al;
    u8_to_w(exe, wexe, 1100);
    _snwprintf_s(wcmd, 1300, _TRUNCATE, L"\"%s\" --job %lu", wexe, (unsigned long)(UINT_PTR)map);
    if (!u8_ExeDir(cwd, sizeof cwd)) cwd[0] = '\0';
    u8_to_w(cwd, wcwd, 1024);
    if (!CreateProcessW(wexe, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, NULL,
                        cwd[0] ? wcwd : NULL, &si.StartupInfo, &pi)) {
        printf("[acc] %s startet nicht (%lu)\n", LACH_EXE, GetLastError());
        goto done;
    }
    started = 1;
    waits[0] = pi.hProcess;
    waits[1] = ev_url;
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, 50);
        ULONGLONG now = GetTickCount64();
        if (w == WAIT_OBJECT_0) break; /* Hilfsprogramm beendet */
        if (w == WAIT_OBJECT_0 + 1) { /* Discord: Browser oeffnen (aus dem Launcher, wie bisher) */
            char url[sizeof m->url];
            memcpy(url, m->url, sizeof url);
            url[sizeof url - 1] = '\0';
            if (open_url(url) != 0) {
                browser_fail = 1;
                SetEvent(ev_cancel);
                if (!sent_cancel) {
                    sent_cancel = 1;
                    cancel_at = now;
                }
            }
            continue;
        }
        if (!sent_cancel && ((cancel && *cancel) || now - t0 > (ULONGLONG)timeout_ms)) {
            timed_out = !(cancel && *cancel);
            SetEvent(ev_cancel);
            sent_cancel = 1;
            cancel_at = now;
        }
        if (sent_cancel && now - cancel_at > 5000) { /* Nachfrist vorbei: nur dieses Kind (eigener Handle) beenden */
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            killed = 1;
            break;
        }
    }
    if (killed || !m->done || memcmp(m->magic, LACH_MAGIC, sizeof LACH_MAGIC) != 0 || m->size != sizeof *m ||
        m->res_size != sizeof(lach_res_t)) {
        if (browser_fail) set_err(err, ecap, "browser");
        else if (timed_out) set_err(err, ecap, "timeout");
        else if (sent_cancel) set_err(err, ecap, "cancel");
        else printf("[acc] %s: kein Ergebnis (Fassung?)\n", LACH_EXE);
        rc = 1;
        goto done;
    }
    m->err[sizeof m->err - 1] = '\0';
    rc = m->rc ? 1 : 0;
    if (res) *res = m->res;
    if (rc && browser_fail) set_err(err, ecap, "browser");
    else if (rc && timed_out) set_err(err, ecap, "timeout"); /* Frist des Launchers, Hilfsprogramm brach ab */
    else set_err(err, ecap, rc ? (m->err[0] ? m->err : "bad") : "");
done:
    if (pi.hProcess) {
        if (WaitForSingleObject(pi.hProcess, 3000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
    }
    if (pi.hThread) CloseHandle(pi.hThread);
    if (m) {
        SecureZeroMemory(m, sizeof *m); /* Passwort, Ergebnis */
        UnmapViewOfFile(m);
    }
    if (map) CloseHandle(map);
    if (ev_cancel) CloseHandle(ev_cancel);
    if (ev_url) CloseHandle(ev_url);
    if (parent) CloseHandle(parent);
    if (al) {
        DeleteProcThreadAttributeList(al);
        HeapFree(GetProcessHeap(), 0, al);
    }
    return started ? rc : 1;
}

#if SB_LAUNCHER_SELFTEST
typedef struct {
    volatile long *flag;
    DWORD ms;
} lach_tcancel_t;

static DWORD WINAPI lach_tcancel(LPVOID v)
{
    lach_tcancel_t *t = (lach_tcancel_t *)v;
    Sleep(t->ms);
    InterlockedExchange(t->flag, 1);
    return 0;
}

/* Laeuft noch ein sb_account.exe, das dieser Launcher gestartet hat? (Kindprozesse per Snapshot, nur zaehlen) */
#include <tlhelp32.h>
static int lach_children(void)
{
    HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe;
    DWORD me = GetCurrentProcessId();
    int n = 0;
    if (sn == INVALID_HANDLE_VALUE) return -1;
    pe.dwSize = sizeof pe;
    if (Process32FirstW(sn, &pe)) {
        do {
            if (pe.th32ParentProcessID == me && _wcsicmp(pe.szExeFile, L"" LACH_EXE) == 0) n++;
        } while (Process32NextW(sn, &pe));
    }
    CloseHandle(sn);
    return n;
}

void lach_selftest(int *pass, int *fail_n)
{
    char why[200] = "", e[48];
    lach_res_t *res = (lach_res_t *)calloc(1, sizeof *res);
    int rc, wait_ok = 1;
    /* 1) nur die Discord-Autorisierung (Test-Launcher zusaetzlich Loopback) wird geoeffnet */
    if (!url_allowed(SB_DISCORD_AUTHORIZE_URL "?response_type=code") ||
        url_allowed("https://discord.com.evil.example/oauth2/authorize?x") ||
        url_allowed(SB_DISCORD_AUTHORIZE_URL "X?y") || url_allowed("file:///C:/x") ||
        url_allowed("https://example.com/?" SB_DISCORD_AUTHORIZE_URL))
        strcpy_s(why, sizeof why, "URL-Pruefung");
    if (!why[0] && (open_url("https://example.com/") == 0 || open_url(SB_DISCORD_AUTHORIZE_URL "?a b") == 0))
        strcpy_s(why, sizeof why, "fremde URL wuerde geoeffnet");
    /* 2) echte Aufrufe: ping, unbekannter Auftrag (mit Zeilenumbruch im Wert) */
    if (!why[0]) {
        rc = lach_run("ping", NULL, NULL, NULL, NULL, 0, 10000, NULL, res, e, sizeof e);
        if (rc != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "ping rc %d err %s (sb_account.exe neben dem Launcher?)", rc, e);
    }
    if (!why[0]) {
        rc = lach_run("gibt-es-nicht", "gibt_es_nicht_w537", "a\nb", "x", NULL, 0, 10000, NULL, res, e, sizeof e);
        if (rc != 1 || strcmp(e, "bad") != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "unbekannter Auftrag rc %d err %s", rc, e);
    }
    /* 3) Abbruch und Frist (Auftrag "wait" gibt es nur im Entwickler-/Test-Hilfsprogramm; ausgeliefertes -> "bad") */
    if (!why[0]) {
        volatile long flag = 0;
        lach_tcancel_t t = { &flag, 300 };
        HANDLE th = CreateThread(NULL, 0, lach_tcancel, &t, 0, NULL);
        ULONGLONG t0 = GetTickCount64();
        rc = lach_run("wait", NULL, NULL, NULL, NULL, 0, 20000, &flag, res, e, sizeof e);
        if (th) {
            WaitForSingleObject(th, 5000);
            CloseHandle(th);
        }
        if (rc == 1 && strcmp(e, "bad") == 0) {
            wait_ok = 0; /* ausgeliefertes Hilfsprogramm neben dem Test-Launcher */
        } else if (rc != 1 || strcmp(e, "cancel") != 0 || GetTickCount64() - t0 > 4000) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Abbruch rc %d err %s nach %llu ms", rc, e, GetTickCount64() - t0);
        }
    }
    if (!why[0] && wait_ok) {
        ULONGLONG t0 = GetTickCount64();
        rc = lach_run("wait", NULL, NULL, NULL, NULL, 0, 1200, NULL, res, e, sizeof e);
        if (rc != 1 || strcmp(e, "timeout") != 0 || GetTickCount64() - t0 > 6000)
            _snprintf_s(why, sizeof why, _TRUNCATE, "Frist rc %d err %s nach %llu ms", rc, e, GetTickCount64() - t0);
    }
    if (!why[0] && lach_children() > 0) strcpy_s(why, sizeof why, "sb_account.exe laeuft nach dem Auftrag weiter");
    free(res);
    if (!why[0]) {
        printf("PASS W537 LAUNCHER-SLIM sb_account.exe: URL-Pruefung (nur Discord-Autorisierung), ping, unbekannter "
               "Auftrag, %s, kein Restprozess\n",
               wait_ok ? "Abbruch + Frist" : "Abbruch/Frist uebersprungen (Release-Hilfsprogramm)");
        (*pass)++;
    } else {
        printf("FAIL W537 LAUNCHER-SLIM sb_account.exe: %s\n", why);
        (*fail_n)++;
    }
}
#endif /* SB_LAUNCHER_SELFTEST */
