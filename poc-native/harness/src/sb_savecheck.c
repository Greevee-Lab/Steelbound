/* sb_savecheck.c - W187 KIA-LOAD: Spielstaende pruefen/reparieren/zuruecksetzen.
 * Beschreibung und Format: sb_savecheck.h. Nur Win32 (W-API), keine
 * Harness-Abhaengigkeiten (Client, Launcher und Selbsttests binden es ein). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "sb_savecheck.h"

#define SIG_LEN 20

static const struct {
    const wchar_t *prefix; /* Dateiname beginnt so (Gross/Klein egal) */
    unsigned n;            /* Datengroesse */
} kSaves[] = {
    { L"user", 0xAE8 },
    { L"mslog", 0xB40 },
    { L"death", 0x15D00 },
    { L"sasen", 0x15D00 },
    { L"config", 0x2C },
};

unsigned sb_save_data_size(const wchar_t *name)
{
    size_t i, ln = wcslen(name);
    if (ln < 5 || _wcsicmp(name + ln - 4, L".sav") != 0) {
        return 0;
    }
    for (i = 0; i < sizeof kSaves / sizeof kSaves[0]; i++) {
        size_t pl = wcslen(kSaves[i].prefix);
        if (_wcsnicmp(name, kSaves[i].prefix, pl) != 0) {
            continue;
        }
        if (i == 4) { /* config.sav (ohne Nummer) */
            return ln == pl + 4 ? kSaves[i].n : 0;
        }
        /* <prefix>NN.sav, NN = zwei Ziffern ("%s%s%02d%s") */
        if (ln == pl + 6 && iswdigit(name[pl]) && iswdigit(name[pl + 1])) {
            return kSaves[i].n;
        }
    }
    return 0;
}

/* Signatur, die der Client fuer n Datenbytes erzeugt (XcSHA-Stub: 0). */
static void sb_sig_expected(unsigned char sig[SIG_LEN])
{
    memset(sig, 0, SIG_LEN);
}

static void logf_u8(sb_savecheck_log_fn log, void *ctx, const char *fmt, ...)
{
    char line[1400];
    va_list ap;
    if (!log) {
        return;
    }
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    log(line, ctx);
}

static void w2u8(const wchar_t *w, char *out, size_t cap)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL)) {
        out[0] = '\0';
    }
}

static int is_dir_w(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void mkdir_p_w(const wchar_t *path)
{
    wchar_t t[1024];
    size_t i;
    wcsncpy_s(t, 1024, path, _TRUNCATE);
    for (i = 0; t[i]; i++) {
        if ((t[i] == L'\\' || t[i] == L'/') && i > 0 && t[i - 1] != L':') {
            wchar_t c = t[i];
            t[i] = 0;
            CreateDirectoryW(t, NULL);
            t[i] = c;
        }
    }
    CreateDirectoryW(t, NULL);
}

static void stamp(wchar_t *out, size_t cap)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snwprintf_s(out, cap, _TRUNCATE, L"%04u%02u%02u-%02u%02u%02u", st.wYear,
                 st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

/* Neuer Sicherungsordner <hdd>\_backup\<kind>-<zeit>[-k] (angelegt). */
static int make_backup_dir(const wchar_t *hdd, const wchar_t *kind, wchar_t *out,
                           size_t cap)
{
    wchar_t ts[32];
    int k;
    stamp(ts, 32);
    for (k = 1; k < 100; k++) {
        if (k == 1) {
            _snwprintf_s(out, cap, _TRUNCATE, L"%s\\_backup\\%s-%s", hdd, kind, ts);
        } else {
            _snwprintf_s(out, cap, _TRUNCATE, L"%s\\_backup\\%s-%s-%d", hdd, kind, ts, k);
        }
        if (!is_dir_w(out) && GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES) {
            mkdir_p_w(out);
            return is_dir_w(out) ? 0 : -1;
        }
    }
    return -1;
}

static sb_save_state_t classify(const wchar_t *path, unsigned n, unsigned long long *size_out)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    unsigned long long sz;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
        return SB_SAVE_UNREAD;
    }
    sz = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    *size_out = sz;
    if (sz < (unsigned long long)n) {
        return SB_SAVE_SHORT; /* ReadFile liefert keine n Bytes */
    }
    if (sz != (unsigned long long)n + SIG_LEN) {
        return SB_SAVE_OK; /* Spiel liest die Signatur nicht -> laedt */
    }
    {
        unsigned char sig[SIG_LEN], want[SIG_LEN];
        DWORD got = 0;
        LARGE_INTEGER off;
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            return SB_SAVE_UNREAD;
        }
        off.QuadPart = n;
        if (!SetFilePointerEx(h, off, NULL, FILE_BEGIN) ||
            !ReadFile(h, sig, SIG_LEN, &got, NULL) || got != SIG_LEN) {
            CloseHandle(h);
            return SB_SAVE_UNREAD;
        }
        CloseHandle(h);
        sb_sig_expected(want);
        return memcmp(sig, want, SIG_LEN) == 0 ? SB_SAVE_OK : SB_SAVE_FOREIGN;
    }
}

/* Signatur an Offset n schreiben (haengt bei Groesse n an). */
static int write_sig(const wchar_t *path, unsigned n)
{
    unsigned char sig[SIG_LEN];
    DWORD w = 0;
    LARGE_INTEGER off;
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    sb_sig_expected(sig);
    off.QuadPart = n;
    if (!SetFilePointerEx(h, off, NULL, FILE_BEGIN) ||
        !WriteFile(h, sig, SIG_LEN, &w, NULL) || w != SIG_LEN ||
        !SetEndOfFile(h)) {
        CloseHandle(h);
        return -1;
    }
    CloseHandle(h);
    return 0;
}

static const char *state_txt(sb_save_state_t s)
{
    switch (s) {
    case SB_SAVE_FOREIGN: return "fremde Signatur";
    case SB_SAVE_SHORT: return "zu kurz";
    case SB_SAVE_UNREAD: return "nicht lesbar";
    default: return "ok";
    }
}

int sb_savecheck_run(const wchar_t *hdd_root, int apply, sb_savecheck_t *res,
                     sb_savecheck_log_fn log, void *ctx)
{
    wchar_t dir[800], pat[820], bdir[800];
    WIN32_FIND_DATAW fd;
    HANDLE fh;
    int found = 0;
    sb_savecheck_t r;
    memset(&r, 0, sizeof r);
    bdir[0] = 0;
    _snwprintf_s(dir, 800, _TRUNCATE, L"%s\\TDATA\\%s", hdd_root, SB_SAVE_TITLE_W);
    if (!is_dir_w(dir)) {
        if (res) {
            *res = r;
        }
        return -1;
    }
    _snwprintf_s(pat, 820, _TRUNCATE, L"%s\\*.sav", dir);
    fh = FindFirstFileW(pat, &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            wchar_t path[1100], dst[1100];
            char nm[300];
            unsigned n;
            unsigned long long sz = 0;
            sb_save_state_t s;
            int rc = -1;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                continue;
            }
            n = sb_save_data_size(fd.cFileName);
            if (!n) {
                continue;
            }
            r.checked++;
            _snwprintf_s(path, 1100, _TRUNCATE, L"%s\\%s", dir, fd.cFileName);
            s = classify(path, n, &sz);
            if (s == SB_SAVE_OK) {
                r.ok++;
                continue;
            }
            found++;
            w2u8(fd.cFileName, nm, sizeof nm);
            if (!apply || s == SB_SAVE_UNREAD) {
                r.failed++;
                logf_u8(log, ctx, "SAVECHECK: %s %s (%llu B, erwartet %u B)%s", nm,
                        state_txt(s), sz, n + SIG_LEN, apply ? " - unveraendert" : "");
                continue;
            }
            if (!bdir[0]) {
                wchar_t sub[900];
                if (make_backup_dir(hdd_root, L"repair", bdir, 800) != 0) {
                    bdir[0] = 0;
                    r.failed++;
                    logf_u8(log, ctx, "SAVECHECK: %s %s - Sicherungsordner nicht anlegbar, unveraendert",
                            nm, state_txt(s));
                    continue;
                }
                _snwprintf_s(sub, 900, _TRUNCATE, L"%s\\TDATA\\%s", bdir, SB_SAVE_TITLE_W);
                wcsncpy_s(bdir, 800, sub, _TRUNCATE);
                mkdir_p_w(bdir);
            }
            _snwprintf_s(dst, 1100, _TRUNCATE, L"%s\\%s", bdir, fd.cFileName);
            if (s == SB_SAVE_SHORT) {
                rc = MoveFileExW(path, dst, MOVEFILE_COPY_ALLOWED) ? 0 : -1;
                if (rc == 0) {
                    r.moved++;
                }
            } else if (CopyFileW(path, dst, FALSE)) {
                rc = write_sig(path, n);
                if (rc == 0) {
                    r.repaired++;
                }
            }
            if (rc != 0) {
                r.failed++;
            }
            logf_u8(log, ctx, "SAVECHECK: %s %s (%llu B, erwartet %u B) -> %s", nm,
                    state_txt(s), sz, n + SIG_LEN,
                    rc != 0 ? "FEHLER (unveraendert)"
                    : s == SB_SAVE_SHORT ? "in Sicherung verschoben (Spiel legt den Eintrag neu an)"
                                         : "Signatur ersetzt (Daten unveraendert)");
        } while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }
    if (bdir[0]) {
        /* Anzeige: Sicherungswurzel (ohne TDATA\<title>) */
        wchar_t *p = wcsstr(bdir, L"\\TDATA\\");
        if (p) {
            *p = 0;
        }
        wcsncpy_s(r.backup, 600, bdir, _TRUNCATE);
    }
    if (res) {
        *res = r;
    }
    return found;
}

static int has_files(const wchar_t *dir, int depth)
{
    wchar_t pat[1100];
    WIN32_FIND_DATAW fd;
    HANDLE fh;
    int any = 0;
    _snwprintf_s(pat, 1100, _TRUNCATE, L"%s\\*", dir);
    fh = FindFirstFileW(pat, &fd);
    if (fh == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            wchar_t sub[1100];
            _snwprintf_s(sub, 1100, _TRUNCATE, L"%s\\%s", dir, fd.cFileName);
            if (depth < 8 && has_files(sub, depth + 1)) {
                any = 1;
            }
        } else {
            any = 1;
        }
    } while (!any && FindNextFileW(fh, &fd));
    FindClose(fh);
    return any;
}

/* TDATA+UDATA nach <hdd>\_backup\<kind>-<zeit>\ verschieben und leer neu
 * anlegen (gemeinsam fuer Zuruecksetzen und W190 SAVE-LOCK "Uebernehmen"). */
static int saves_move_backup(const wchar_t *hdd_root, const wchar_t *kind, wchar_t *backup_out,
                             size_t cap, char *err, size_t errcap)
{
    static const wchar_t *const kDirs[] = { L"TDATA", L"UDATA" };
    wchar_t src[2][900], bdir[800];
    int has[2], i, moved[2] = { 0, 0 };
    if (backup_out && cap) {
        backup_out[0] = 0;
    }
    if (err && errcap) {
        err[0] = '\0';
    }
    for (i = 0; i < 2; i++) {
        _snwprintf_s(src[i], 900, _TRUNCATE, L"%s\\%s", hdd_root, kDirs[i]);
        has[i] = is_dir_w(src[i]) && has_files(src[i], 0);
    }
    if (!has[0] && !has[1]) {
        return 1;
    }
    if (make_backup_dir(hdd_root, kind, bdir, 800) != 0) {
        if (err) {
            _snprintf_s(err, errcap, _TRUNCATE, "_backup (Windows %lu)", GetLastError());
        }
        return -1;
    }
    for (i = 0; i < 2; i++) {
        wchar_t dst[1000];
        if (!has[i]) {
            continue;
        }
        _snwprintf_s(dst, 1000, _TRUNCATE, L"%s\\%s", bdir, kDirs[i]);
        if (!MoveFileExW(src[i], dst, 0)) {
            DWORD e = GetLastError();
            int k;
            for (k = 0; k < i; k++) { /* zuruecknehmen */
                if (moved[k]) {
                    wchar_t back[1000];
                    _snwprintf_s(back, 1000, _TRUNCATE, L"%s\\%s", bdir, kDirs[k]);
                    MoveFileExW(back, src[k], 0);
                }
            }
            RemoveDirectoryW(bdir);
            if (err) {
                _snprintf_s(err, errcap, _TRUNCATE, "%ls (Windows %lu)", kDirs[i], e);
            }
            return -1;
        }
        moved[i] = 1;
    }
    for (i = 0; i < 2; i++) { /* leer neu anlegen (wie krnl_init) */
        wchar_t d[1000];
        _snwprintf_s(d, 1000, _TRUNCATE, L"%s\\%s", src[i], SB_SAVE_TITLE_W);
        mkdir_p_w(d);
    }
    if (backup_out && cap) {
        wcsncpy_s(backup_out, cap, bdir, _TRUNCATE);
    }
    return 0;
}

int sb_saves_reset(const wchar_t *hdd_root, wchar_t *backup_out, size_t cap,
                   char *err, size_t errcap)
{
    return saves_move_backup(hdd_root, L"saves", backup_out, cap, err, errcap);
}

/* ------------------------------------------------------------------ */
/* W190 SAVE-LOCK: Wegwerf-Arbeitskopie (Beschreibung sb_savecheck.h)    */
/* ------------------------------------------------------------------ */
static int rmtree_w(const wchar_t *dir, int depth)
{
    wchar_t pat[1100];
    WIN32_FIND_DATAW fd;
    HANDLE fh;
    int bad = 0;
    if (depth > 12) {
        return -1;
    }
    _snwprintf_s(pat, 1100, _TRUNCATE, L"%s\\*", dir);
    fh = FindFirstFileW(pat, &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            wchar_t p[1100];
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
                continue;
            }
            _snwprintf_s(p, 1100, _TRUNCATE, L"%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) {
                SetFileAttributesW(p, fd.dwFileAttributes & ~(DWORD)FILE_ATTRIBUTE_READONLY);
            }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (rmtree_w(p, depth + 1) != 0) {
                    bad = 1;
                }
            } else if (!DeleteFileW(p)) {
                bad = 1;
            }
        } while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }
    if (!RemoveDirectoryW(dir) && GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES) {
        bad = 1;
    }
    return bad ? -1 : 0;
}

/* Baum src -> dst kopieren (dst wird angelegt, vorhandene Dateien werden
 * ueberschrieben). Rueckgabe: Anzahl Dateien, -1 = Fehler. */
static int copytree_w(const wchar_t *src, const wchar_t *dst, int depth)
{
    wchar_t pat[1100];
    WIN32_FIND_DATAW fd;
    HANDLE fh;
    int n = 0;
    if (depth > 12) {
        return -1;
    }
    mkdir_p_w(dst);
    if (!is_dir_w(dst)) {
        return -1;
    }
    _snwprintf_s(pat, 1100, _TRUNCATE, L"%s\\*", src);
    fh = FindFirstFileW(pat, &fd);
    if (fh == INVALID_HANDLE_VALUE) {
        return is_dir_w(src) ? 0 : -1;
    }
    do {
        wchar_t s[1100], d[1100];
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }
        _snwprintf_s(s, 1100, _TRUNCATE, L"%s\\%s", src, fd.cFileName);
        _snwprintf_s(d, 1100, _TRUNCATE, L"%s\\%s", dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            int k = copytree_w(s, d, depth + 1);
            if (k < 0) {
                n = -1;
                break;
            }
            n += k;
        } else {
            if (!CopyFileW(s, d, FALSE)) {
                n = -1;
                break;
            }
            /* Arbeitskopie bleibt beschreibbar (das Spiel ueberschreibt Staende) */
            SetFileAttributesW(d, fd.dwFileAttributes & ~(DWORD)FILE_ATTRIBUTE_READONLY);
            n++;
        }
    } while (FindNextFileW(fh, &fd));
    FindClose(fh);
    return n;
}

static void savelock_root(const wchar_t *hdd_root, wchar_t *out, size_t cap)
{
    _snwprintf_s(out, cap, _TRUNCATE, L"%s\\%s", hdd_root, SB_SAVELOCK_DIR_W);
}

int sb_savelock_present(const wchar_t *hdd_root)
{
    wchar_t r[900];
    savelock_root(hdd_root, r, 900);
    return is_dir_w(r);
}

int sb_savelock_discard(const wchar_t *hdd_root)
{
    wchar_t r[900];
    savelock_root(hdd_root, r, 900);
    if (GetFileAttributesW(r) == INVALID_FILE_ATTRIBUTES) {
        return 0;
    }
    return rmtree_w(r, 0) == 0 ? 1 : -1;
}

int sb_savelock_prepare(const wchar_t *hdd_root, int *files, char *err, size_t errcap)
{
    static const wchar_t *const kDirs[] = { L"TDATA", L"UDATA" };
    static const char kTxt[] =
        "Steelbound - Spielstaende schuetzen / Protect saves (save_lock=1)\r\n"
        "Arbeitskopie einer Sitzung. Wird beim naechsten Start verworfen; die echten\r\n"
        "Spielstaende in TDATA/UDATA bleiben unveraendert.\r\n"
        "Working copy of one session. Discarded on the next start; your real saves\r\n"
        "in TDATA/UDATA stay unchanged.\r\n";
    wchar_t r[900], p[1000];
    int i, n = 0;
    HANDLE h;
    if (files) {
        *files = 0;
    }
    if (err && errcap) {
        err[0] = '\0';
    }
    if (sb_savelock_discard(hdd_root) < 0) {
        if (err) {
            _snprintf_s(err, errcap, _TRUNCATE, "alte Arbeitskopie nicht loeschbar (Windows %lu)",
                        GetLastError());
        }
        return -1;
    }
    savelock_root(hdd_root, r, 900);
    for (i = 0; i < 2; i++) {
        wchar_t s[1000], d[1000], t[1100];
        int k;
        _snwprintf_s(s, 1000, _TRUNCATE, L"%s\\%s", hdd_root, kDirs[i]);
        _snwprintf_s(d, 1000, _TRUNCATE, L"%s\\%s", r, kDirs[i]);
        k = is_dir_w(s) ? copytree_w(s, d, 0) : 0;
        if (k < 0) {
            DWORD e = GetLastError();
            rmtree_w(r, 0);
            if (err) {
                _snprintf_s(err, errcap, _TRUNCATE, "%ls kopieren (Windows %lu)", kDirs[i], e);
            }
            return -1;
        }
        n += k;
        _snwprintf_s(t, 1100, _TRUNCATE, L"%s\\%s", d, SB_SAVE_TITLE_W);
        mkdir_p_w(t); /* wie krnl_init: Titel-Ordner existieren immer */
    }
    /* Hinweis fuer Spieler, die den Ordner finden (liegt neben TDATA/UDATA) */
    _snwprintf_s(p, 1000, _TRUNCATE, L"%s\\README.txt", r);
    h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, kTxt, (DWORD)(sizeof kTxt - 1), &w, NULL);
        CloseHandle(h);
    }
    if (files) {
        *files = n;
    }
    return 0;
}

int sb_savelock_commit(const wchar_t *hdd_root, wchar_t *backup_out, size_t cap, int *files,
                       char *err, size_t errcap)
{
    static const wchar_t *const kDirs[] = { L"TDATA", L"UDATA" };
    wchar_t r[900];
    int i, n = 0;
    if (files) {
        *files = 0;
    }
    if (backup_out && cap) {
        backup_out[0] = 0;
    }
    if (err && errcap) {
        err[0] = '\0';
    }
    savelock_root(hdd_root, r, 900);
    if (!is_dir_w(r)) {
        if (err) {
            _snprintf_s(err, errcap, _TRUNCATE, "keine Arbeitskopie");
        }
        return -1;
    }
    /* 1. echte Staende sichern (gesperrte Datei -> nichts verschoben, Fehler) */
    if (saves_move_backup(hdd_root, L"savelock", backup_out, cap, err, errcap) < 0) {
        return -1;
    }
    /* 2. Arbeitskopie hineinkopieren (die Arbeitskopie bleibt bestehen) */
    for (i = 0; i < 2; i++) {
        wchar_t s[1000], d[1000];
        int k;
        _snwprintf_s(s, 1000, _TRUNCATE, L"%s\\%s", r, kDirs[i]);
        _snwprintf_s(d, 1000, _TRUNCATE, L"%s\\%s", hdd_root, kDirs[i]);
        k = is_dir_w(s) ? copytree_w(s, d, 0) : 0;
        if (k < 0) {
            if (err) {
                _snprintf_s(err, errcap, _TRUNCATE, "%ls zurueckkopieren (Windows %lu)", kDirs[i],
                            GetLastError());
            }
            return -1;
        }
        n += k;
    }
    if (files) {
        *files = n;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Selbsttest                                                           */
/* ------------------------------------------------------------------ */
static int st_write(const wchar_t *dir, const wchar_t *name, unsigned len,
                    unsigned char fill, unsigned siglen, unsigned char sigfill)
{
    wchar_t p[1100];
    unsigned char *b = (unsigned char *)malloc(len + siglen + 1);
    DWORD w = 0;
    HANDLE h;
    unsigned i;
    if (!b) {
        return -1;
    }
    for (i = 0; i < len; i++) {
        b[i] = (unsigned char)(fill + (i % 7));
    }
    memset(b + len, sigfill, siglen);
    _snwprintf_s(p, 1100, _TRUNCATE, L"%s\\%s", dir, name);
    h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        free(b);
        return -1;
    }
    WriteFile(h, b, len + siglen, &w, NULL);
    CloseHandle(h);
    free(b);
    return w == len + siglen ? 0 : -1;
}

/* 1 = Datei hat Groesse len+20, Daten = Muster fill, Signatur = 0 */
static int st_good(const wchar_t *dir, const wchar_t *name, unsigned len, unsigned char fill)
{
    wchar_t p[1100];
    unsigned char *b;
    DWORD got = 0;
    HANDLE h;
    unsigned i;
    int ok = 1;
    _snwprintf_s(p, 1100, _TRUNCATE, L"%s\\%s", dir, name);
    h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    b = (unsigned char *)malloc(len + SIG_LEN + 8);
    if (!b) {
        CloseHandle(h);
        return 0;
    }
    ReadFile(h, b, len + SIG_LEN + 8, &got, NULL);
    CloseHandle(h);
    if (got != len + SIG_LEN) {
        ok = 0;
    }
    for (i = 0; ok && i < len; i++) {
        ok = b[i] == (unsigned char)(fill + (i % 7));
    }
    for (i = 0; ok && i < SIG_LEN; i++) {
        ok = b[len + i] == 0;
    }
    free(b);
    return ok;
}

static void st_rmtree(const wchar_t *dir)
{
    wchar_t pat[1100];
    WIN32_FIND_DATAW fd;
    HANDLE fh;
    _snwprintf_s(pat, 1100, _TRUNCATE, L"%s\\*", dir);
    fh = FindFirstFileW(pat, &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            wchar_t p[1100];
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
                continue;
            }
            _snwprintf_s(p, 1100, _TRUNCATE, L"%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                st_rmtree(p);
            } else {
                DeleteFileW(p);
            }
        } while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }
    RemoveDirectoryW(dir);
}

static void st_count(const char *line, void *ctx)
{
    (void)line;
    (*(int *)ctx)++;
}

int sb_savecheck_selftest(char *msg, size_t cap)
{
    wchar_t tmp[MAX_PATH], root[900], td[1000], ud[1000], bk[800], p[1100];
    sb_savecheck_t r;
    int nlog = 0, rc, step = 0;
    const char *why = NULL;
    GetTempPathW(MAX_PATH, tmp);
    _snwprintf_s(root, 900, _TRUNCATE, L"%ssb_savecheck_%lu_%lu", tmp,
                 GetCurrentProcessId(), GetTickCount());
    _snwprintf_s(td, 1000, _TRUNCATE, L"%s\\TDATA\\%s", root, SB_SAVE_TITLE_W);
    _snwprintf_s(ud, 1000, _TRUNCATE, L"%s\\UDATA\\%s", root, SB_SAVE_TITLE_W);

#define ST(c, w)            \
    do {                    \
        step++;             \
        if (!(c)) {         \
            why = (w);      \
            goto done;      \
        }                   \
    } while (0)

    /* Namen -> Datengroesse */
    ST(sb_save_data_size(L"death00.sav") == 0x15D00 && sb_save_data_size(L"USER99.SAV") == 0xAE8 &&
       sb_save_data_size(L"config.sav") == 0x2C && sb_save_data_size(L"sasen00.sav") == 0x15D00 &&
       sb_save_data_size(L"mslog00.sav") == 0xB40 && sb_save_data_size(L"user1.sav") == 0 &&
       sb_save_data_size(L"config00.sav") == 0 && sb_save_data_size(L"death00.bak") == 0 &&
       sb_save_data_size(L"SaveMeta.xbx") == 0,
       "Namensregeln");
    /* frische Installation: kein TDATA -> -1, nichts angelegt */
    ST(sb_savecheck_run(root, 1, &r, NULL, NULL) == -1 && !is_dir_w(root), "frisch");
    mkdir_p_w(td);
    mkdir_p_w(ud);
    ST(st_write(td, L"user99.sav", 0xAE8, 1, SIG_LEN, 0) == 0 &&
       st_write(td, L"user01.sav", 0xAE8, 2, SIG_LEN, 0) == 0 &&
       st_write(td, L"death00.sav", 0x15D00, 3, 0, 0) == 0 &&        /* ohne Signatur: laedt */
       st_write(td, L"user02.sav", 0xAE8 + 30, 9, 0, 0) == 0 &&      /* zu lang: laedt */
       st_write(td, L"mslog00.sav", 0xB40, 4, SIG_LEN, 0xA5) == 0 && /* fremde Signatur */
       st_write(td, L"config.sav", 10, 5, 0, 0) == 0 &&              /* abgeschnitten */
       st_write(td, L"sasen00.sav", 0, 0, 0, 0) == 0 &&              /* 0 Byte */
       st_write(td, L"notes.txt", 5, 6, 0, 0) == 0 &&
       st_write(ud, L"TitleMeta.xbx", 56, 7, 0, 0) == 0,
       "Testdateien");
    /* nur pruefen: 4 Befunde, nichts veraendert, keine Sicherung */
    rc = sb_savecheck_run(root, 0, &r, st_count, &nlog);
    ST(rc == 3 && r.checked == 7 && r.ok == 4 && r.failed == 3 && nlog == 3 && !r.backup[0],
       "Pruefen");
    _snwprintf_s(p, 1100, _TRUNCATE, L"%s\\_backup", root);
    ST(!is_dir_w(p), "Pruefen legt keine Sicherung an");
    /* reparieren */
    nlog = 0;
    rc = sb_savecheck_run(root, 1, &r, st_count, &nlog);
    ST(rc == 3 && r.repaired == 1 && r.moved == 2 && r.failed == 0 && nlog == 3 && r.backup[0],
       "Reparieren");
    {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        _snwprintf_s(p, 1100, _TRUNCATE, L"%s\\death00.sav", td);
        ST(GetFileAttributesExW(p, GetFileExInfoStandard, &fa) && fa.nFileSizeLow == 0x15D00,
           "death00.sav ohne Signatur unveraendert");
        _snwprintf_s(p, 1100, _TRUNCATE, L"%s\\user02.sav", td);
        ST(GetFileAttributesExW(p, GetFileExInfoStandard, &fa) && fa.nFileSizeLow == 0xAE8 + 30,
           "user02.sav (zu lang) unveraendert");
    }
    ST(st_good(td, L"mslog00.sav", 0xB40, 4) &&
       st_good(td, L"user99.sav", 0xAE8, 1) && st_good(td, L"user01.sav", 0xAE8, 2),
       "Daten erhalten + Signatur 0");
    _snwprintf_s(p, 1100, _TRUNCATE, L"%s\\config.sav", td);
    ST(GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES, "config.sav verschoben");
    _snwprintf_s(p, 1100, _TRUNCATE, L"%s\\TDATA\\%s\\config.sav", r.backup, SB_SAVE_TITLE_W);
    ST(GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES, "config.sav in Sicherung");
    _snwprintf_s(p, 1100, _TRUNCATE, L"%s\\TDATA\\%s\\mslog00.sav", r.backup, SB_SAVE_TITLE_W);
    {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        ST(GetFileAttributesExW(p, GetFileExInfoStandard, &fa) && fa.nFileSizeLow == 0xB40 + SIG_LEN,
           "Original mslog00.sav gesichert");
    }
    /* zweiter Lauf: alles ok */
    ST(sb_savecheck_run(root, 1, &r, NULL, NULL) == 0 && r.checked == 5 && r.ok == 5, "Wiederholung");
    /* Zuruecksetzen */
    {
        char e[200];
        rc = sb_saves_reset(root, bk, 800, e, sizeof e);
        ST(rc == 0 && bk[0], "Zuruecksetzen");
        _snwprintf_s(p, 1100, _TRUNCATE, L"%s\\TDATA\\%s\\user99.sav", bk, SB_SAVE_TITLE_W);
        ST(GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES, "TDATA gesichert");
        _snwprintf_s(p, 1100, _TRUNCATE, L"%s\\UDATA\\%s\\TitleMeta.xbx", bk, SB_SAVE_TITLE_W);
        ST(GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES, "UDATA gesichert");
        ST(is_dir_w(td) && is_dir_w(ud) && !has_files(td, 0) && !has_files(ud, 0),
           "leer neu angelegt");
        ST(sb_saves_reset(root, bk, 800, e, sizeof e) == 1, "zweimal: nichts zu sichern");
        /* Spiel offen: gesperrte Datei -> Fehler, nichts verschoben */
        ST(st_write(td, L"user00.sav", 0xAE8, 8, SIG_LEN, 0) == 0, "Sperrtest-Datei");
        _snwprintf_s(p, 1100, _TRUNCATE, L"%s\\user00.sav", td);
        {
            HANDLE h = CreateFileW(p, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
            rc = sb_saves_reset(root, bk, 800, e, sizeof e);
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
            }
            ST(h != INVALID_HANDLE_VALUE && rc == -1 && e[0] && st_good(td, L"user00.sav", 0xAE8, 8),
               "gesperrt -> Fehler, nichts verschoben");
        }
    }
done:
#undef ST
    st_rmtree(root);
    if (why) {
        _snprintf_s(msg, cap, _TRUNCATE, "savecheck: Schritt %d '%s' FEHLER", step, why);
        return 1;
    }
    _snprintf_s(msg, cap, _TRUNCATE, "savecheck: ok (%d/%d)", step, step);
    return 0;
}

/* W190 SAVE-LOCK: Arbeitskopie anlegen, darin "sterben" (Pilot loeschen,
 * death00 schreiben), verwerfen bzw. uebernehmen. */
int sb_savelock_selftest(char *msg, size_t cap)
{
    wchar_t tmp[MAX_PATH], root[900], td[1000], wtd[1000], wud[1000], bk[800], p[1100];
    int rc, n = 0, step = 0;
    const char *why = NULL;
    char e[200];
    GetTempPathW(MAX_PATH, tmp);
    _snwprintf_s(root, 900, _TRUNCATE, L"%ssb_savelock_%lu_%lu", tmp, GetCurrentProcessId(),
                 GetTickCount());
    _snwprintf_s(td, 1000, _TRUNCATE, L"%s\\TDATA\\%s", root, SB_SAVE_TITLE_W);
    _snwprintf_s(wtd, 1000, _TRUNCATE, L"%s\\%s\\TDATA\\%s", root, SB_SAVELOCK_DIR_W, SB_SAVE_TITLE_W);
    _snwprintf_s(wud, 1000, _TRUNCATE, L"%s\\%s\\UDATA\\%s", root, SB_SAVELOCK_DIR_W, SB_SAVE_TITLE_W);

#define ST(c, w)            \
    do {                    \
        step++;             \
        if (!(c)) {         \
            why = (w);      \
            goto done;      \
        }                   \
    } while (0)

    /* frische Installation: leere Arbeitskopie mit Titel-Ordnern */
    mkdir_p_w(root);
    ST(sb_savelock_prepare(root, &n, e, sizeof e) == 0 && n == 0 && is_dir_w(wtd) && is_dir_w(wud),
       "frisch: leere Arbeitskopie");
    ST(sb_savelock_present(root) && sb_savelock_discard(root) == 1 && !sb_savelock_present(root) &&
       sb_savelock_discard(root) == 0, "verwerfen");
    /* Pilot + Liste */
    mkdir_p_w(td);
    ST(st_write(td, L"user01.sav", 0xAE8, 2, SIG_LEN, 0) == 0 &&
       st_write(td, L"user99.sav", 0xAE8, 1, SIG_LEN, 0) == 0, "Testdateien");
    ST(sb_savelock_prepare(root, &n, e, sizeof e) == 0 && n == 2 && st_good(wtd, L"user01.sav", 0xAE8, 2),
       "Arbeitskopie mit Pilot");
    /* Tod in der Arbeitskopie: Pilot geloescht, death00 geschrieben */
    _snwprintf_s(p, 1100, _TRUNCATE, L"%s\\user01.sav", wtd);
    ST(DeleteFileW(p) && st_write(wtd, L"death00.sav", 0x15D00, 3, SIG_LEN, 0) == 0, "Tod in Arbeitskopie");
    ST(st_good(td, L"user01.sav", 0xAE8, 2), "echter Pilot unveraendert");
    _snwprintf_s(p, 1100, _TRUNCATE, L"%s\\death00.sav", td);
    ST(GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES, "kein echtes death00.sav");
    /* Neustart = neue Arbeitskopie: Pilot wieder da, death00 weg */
    ST(sb_savelock_prepare(root, &n, e, sizeof e) == 0 && n == 2 && st_good(wtd, L"user01.sav", 0xAE8, 2),
       "neue Sitzung: Pilot wieder da");
    _snwprintf_s(p, 1100, _TRUNCATE, L"%s\\death00.sav", wtd);
    ST(GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES, "neue Sitzung: death00 verworfen");
    /* Uebernehmen: Arbeitskopie -> echt, Sicherung savelock-* */
    ST(st_write(wtd, L"mslog00.sav", 0xB40, 4, SIG_LEN, 0) == 0, "Aenderung in Arbeitskopie");
    _snwprintf_s(p, 1100, _TRUNCATE, L"%s\\user99.sav", wtd);
    ST(DeleteFileW(p), "Loeschen in Arbeitskopie");
    rc = sb_savelock_commit(root, bk, 800, &n, e, sizeof e);
    ST(rc == 0 && bk[0] && n == 2 && wcsstr(bk, L"savelock-") != NULL, "uebernehmen");
    ST(st_good(td, L"mslog00.sav", 0xB40, 4) && st_good(td, L"user01.sav", 0xAE8, 2), "uebernommen");
    _snwprintf_s(p, 1100, _TRUNCATE, L"%s\\user99.sav", td);
    ST(GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES, "Loeschung uebernommen");
    _snwprintf_s(p, 1100, _TRUNCATE, L"%s\\TDATA\\%s\\user99.sav", bk, SB_SAVE_TITLE_W);
    ST(GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES, "alter Stand gesichert");
    ST(sb_savelock_present(root), "Arbeitskopie bleibt nach dem Uebernehmen");
    sb_savelock_discard(root);
    ST(sb_savelock_commit(root, bk, 800, &n, e, sizeof e) == -1 && e[0], "ohne Arbeitskopie -> Fehler");
done:
#undef ST
    st_rmtree(root);
    if (why) {
        _snprintf_s(msg, cap, _TRUNCATE, "savelock: Schritt %d '%s' FEHLER", step, why);
        return 1;
    }
    _snprintf_s(msg, cap, _TRUNCATE, "savelock: ok (%d/%d)", step, step);
    return 0;
}
