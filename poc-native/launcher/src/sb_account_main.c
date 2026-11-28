/* sb_account_main.c - W537 LAUNCHER-SLIM: Konto-Hilfsprogramm sb_account.exe (s. launcher_acchelp.h).
 *
 * Wird nur vom Steelbound Launcher gestartet ("--job <Handle>", unsichtbar) und beendet sich nach genau einem
 * Auftrag. Auftrag und Ergebnis stehen im vom Launcher vererbten, unbenannten Speicherblock. Ohne gueltigen Block tut
 * es nichts (Exit 2). Kein Fenster, keine Konsole, keine Pipes, kein Autostart, keine Dateien ausser der eigenen
 * Schluesseldatei im Ordner, den der Launcher nennt.
 * Quelltext ASCII. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "launcher_acchelp.h"
#include "launcher_account.h"
#include "launcher_camp.h"
#include "launcher_app.h"

static lach_shm_t *g_m;
static volatile long g_cancel;

/* Die Kontologik gibt im Hilfsprogramm nichts aus (CLI und Testausgaben sind nur im Launcher). */
int cli_printf(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}

/* Discord: URL in den Block, Launcher wecken (er oeffnet den Browser) */
static int url_to_launcher(const char *url)
{
    if (strlen(url) >= sizeof g_m->url) return 1;
    strcpy_s(g_m->url, sizeof g_m->url, url);
    return SetEvent((HANDLE)(UINT_PTR)g_m->ev_url) ? 0 : 1;
}

/* Abbruch: Ereignis des Launchers oder Launcher beendet */
static DWORD WINAPI cancel_watch(LPVOID p)
{
    HANDLE w[2];
    (void)p;
    w[0] = (HANDLE)(UINT_PTR)g_m->ev_cancel;
    w[1] = (HANDLE)(UINT_PTR)g_m->h_parent;
    WaitForMultipleObjects(2, w, FALSE, INFINITE);
    InterlockedExchange(&g_cancel, 1);
    return 0;
}

static void fail(char *err, size_t ecap, const char *code)
{
    strcpy_s(err, ecap, code);
}

/* Auftrag ausfuehren. Rueckgabe rc (0 ok), err gesetzt bei Fehler. */
static int run(const lach_shm_t *q, lach_res_t *res, char *err, size_t ecap)
{
    lacc_srv_t srv;
    lacc_store_t st;
    int rc = 1;
    const char *dir = q->dir;
    memset(&st, 0, sizeof st);
    err[0] = '\0';
    if (strcmp(q->op, "ping") == 0) return 0;
#if (defined(SB_TEST_HOOKS) && SB_TEST_HOOKS) || (defined(SB_LAUNCHER_SELFTEST) && SB_LAUNCHER_SELFTEST)
    if (strcmp(q->op, "wait") == 0) { /* Selbsttest (lach_selftest): bis Abbruch warten, hoechstens 30 s */
        int i;
        for (i = 0; i < 3000 && !g_cancel; i++) Sleep(10);
        fail(err, ecap, g_cancel ? "cancel" : "timeout");
        return 1;
    }
#endif
    if (strcmp(q->op, "camp") == 0) { /* Kampagnen-Erreichbarkeit (reines TCP, W534) */
        int to = q->timeout_ms > 2500 ? q->timeout_ms - 2000 : 500; /* Launcher-Frist minus Start-Reserve */
        if (to > 15000) to = 15000;
        return lcamp_probe(q->k, to, err, ecap);
    }
    if (strcmp(q->op, "info") == 0) { /* Kopf-Chip: GET /v1/info */
        rc = lacc_probe(&res->probe);
        if (rc != 0) strcpy_s(err, ecap, res->probe.error[0] ? res->probe.error : "net");
        return rc;
    }
    if (!dir[0]) {
        fail(err, ecap, "bad");
        return 1;
    }
    if (lacc_store_load(dir, &st) != 1) memset(&st, 0, sizeof st);
    if (strcmp(q->op, "load") == 0) { /* oeffentliche Felder (Ergebnis st, s. WinMain) */
        SecureZeroMemory(&st, sizeof st);
        return 0;
    }
    if (strcmp(q->op, "recovery-show") == 0 || strcmp(q->op, "game-auth") == 0) {
        if (!st.have) {
            fail(err, ecap, "no_account");
        } else if (q->op[0] == 'r') {
            lacc_recovery_code(st.account, st.key, res->code, sizeof res->code);
            rc = 0;
        } else {
            lacc_game_auth(&st, (long long)time(NULL), res->auth, sizeof res->auth);
            rc = 0;
        }
        SecureZeroMemory(&st, sizeof st);
        return rc;
    }
    if (strcmp(q->op, "lobbies") == 0) {
        rc = lacc_lobbies(st.have ? &st : NULL, &res->lob);
        if (rc != 0) strcpy_s(err, ecap, res->lob.error[0] ? res->lob.error : "bad");
        SecureZeroMemory(&st, sizeof st);
        return rc;
    }
    lacc_server(&srv);
    if (strcmp(q->op, "status") == 0) rc = lacc_op_status(&srv, &st, &res->status, err, ecap);
    else if (strcmp(q->op, "check") == 0) rc = lacc_op_check(&srv, q->a, &res->avail, err, ecap);
    else if (strcmp(q->op, "create") == 0) rc = lacc_op_create_gt(&srv, dir, q->a, q->c, &st, &res->gts, err, ecap);
    else if (strcmp(q->op, "gtcheck") == 0)
        rc = lacc_op_gamertag_check(&srv, q->a[0] ? q->a : NULL, q->c[0] ? q->c : NULL, &res->gtc, err, ecap);
    else if (strcmp(q->op, "gtset") == 0) rc = lacc_op_gamertag_set(&srv, &st, q->a, &res->gts, err, ecap);
    else if (strcmp(q->op, "discord-link") == 0 || strcmp(q->op, "discord-login") == 0) {
        int to = q->timeout_ms > 7000 ? q->timeout_ms - 5000 : 180000; /* Launcher-Frist minus Reserve */
        g_lacc_open_url = url_to_launcher;
        rc = lacc_op_discord(&srv, dir, strcmp(q->op, "discord-login") == 0, &st, to, &g_cancel, err, ecap);
    } else if (strcmp(q->op, "login-set") == 0) rc = lacc_op_login_set(&srv, &st, q->a, q->b, err, ecap);
    else if (strcmp(q->op, "login") == 0) rc = lacc_op_login(&srv, dir, q->a, q->b, &st, err, ecap);
    else if (strcmp(q->op, "transfer") == 0) rc = lacc_op_transfer(&srv, dir, q->a, &st, err, ecap);
    else if (strcmp(q->op, "recovery") == 0) rc = lacc_op_recovery(&srv, dir, q->a, &st, err, ecap);
    else if (strcmp(q->op, "delete") == 0) rc = lacc_op_delete(&srv, dir, &st, err, ecap);
    else fail(err, ecap, "bad");
    SecureZeroMemory(&st, sizeof st);
    if (rc != 0 && !err[0]) fail(err, ecap, "bad");
    return rc;
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    static lach_res_t res;
    lach_shm_t *q;
    const char *p;
    char err[48] = "";
    unsigned long hv;
    HANDLE map, th;
    int rc;
    (void)hi;
    (void)hp;
    (void)show;
    p = cmd ? strstr(cmd, "--job ") : NULL;
    if (!p) return 2;
    hv = strtoul(p + 6, NULL, 10);
    map = (HANDLE)(UINT_PTR)hv;
    if (!hv) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    g_m = (lach_shm_t *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(lach_shm_t));
    if (!g_m) return 2;
    if (memcmp(g_m->magic, LACH_MAGIC, sizeof LACH_MAGIC) != 0 || g_m->size != sizeof *g_m ||
        g_m->res_size != sizeof(lach_res_t)) {
        UnmapViewOfFile(g_m);
        return 3; /* andere Fassung: der Launcher meldet "helper" */
    }
    /* Auftrag lokal kopieren (Block kann waehrenddessen nur url/Ergebnis bekommen) */
    q = (lach_shm_t *)calloc(1, sizeof *q);
    if (!q) return 4;
    memcpy(q, g_m, sizeof *q);
    q->op[sizeof q->op - 1] = q->dir[sizeof q->dir - 1] = '\0';
    q->a[sizeof q->a - 1] = q->b[sizeof q->b - 1] = q->c[sizeof q->c - 1] = '\0';
    SecureZeroMemory(g_m->b, sizeof g_m->b); /* Passwort nur noch in der lokalen Kopie */
    th = CreateThread(NULL, 0, cancel_watch, NULL, 0, NULL);
    if (th) CloseHandle(th);
    memset(&res, 0, sizeof res);
    rc = run(q, &res, err, sizeof err);
    if (q->dir[0]) { /* Schluesseldatei nach dem Auftrag: nur oeffentliche Felder an den Launcher */
        res.st_load = lacc_store_load(q->dir, &res.st);
        SecureZeroMemory(res.st.key, sizeof res.st.key);
    }
    SecureZeroMemory(q, sizeof *q);
    free(q);
    g_m->rc = rc;
    strcpy_s(g_m->err, sizeof g_m->err, rc ? err : "");
    res.filled = 1;
    g_m->res = res;
    SecureZeroMemory(&res, sizeof res);
    MemoryBarrier();
    g_m->done = 1;
    UnmapViewOfFile(g_m);
    return 0;
}
