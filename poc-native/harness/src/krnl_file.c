/* krnl_file.c - Kernel-HLE: Symlink-Tabelle, Xbox-Pfad -> Host-Pfad,
 * Nt*File/FATX-Datei-I/O, Symlink-Objekte, Seek-Cache (W22 FILMAV).
 * W68 SPLIT: 1:1 aus kernel_hle.c verschoben. */
#include "loc_rollback.h" /* W330: LOC_RB_PREWRITE */
#include "krnl_int.h"
#include "sb_profile.h" /* W229 SBA() */
#include "sb_remaster.h" /* W103 REM-FRAME */
#include "iso_vfs.h"     /* W150 ISO-DIRECT */
#include "sb_savecheck.h" /* W187 KIA-LOAD */
#include "sb_rauto.h"     /* W254 REPLAY-LAUNCHER */
#include "sb_diagcore.h"  /* W434: Ordner mit Platzhaltern */
#include "sb_lang.h"      /* W434: Meldung (de/en) */
#include "sb_origview.h"  /* W434: Einblendung */
#include <aclapi.h>       /* W434: Selbsttest (Ordner sperren) */
#include <sddl.h>
#include <ctype.h>
#include <intrin.h> /* W187: _AddressOfReturnAddress (FILE delete) */

/* ---------- W150 ISO-DIRECT: D: aus dem Spiel-Abbild (nur lesend) ----------
 * game_image=<Pfad> -> g_iso; alle D:-Zugriffe (NtCreateFile/NtOpenFile/
 * NtReadFile/NtQuery*File/NtClose, XAPI-ReadFile) laufen dann ueber iso_vfs.
 * Abbild-Handles: HT_FILE mit host=NULL, vfs=1, eigener Dateizeiger. Die
 * Pfadtabelle ist nach dem Start unveraenderlich, Lesen positionell
 * (ReadFile+OVERLAPPED-Offset) -> keine Sperre. Schreiben/Erzeugen auf D:
 * liefert ACCESS_DENIED wie auf der DVD. */
static iso_vfs_t *g_iso;

/* ---------- W423 LOC-XSB-LOOP (GitHub #71): Datei-Cache Z: im LoC-Start ----------
 * LoC kopiert beim Start in einem eigenen Thread (FUN_0001DA50) 33 Dateien aus
 * media/sndeff auf dem Spiel-Laufwerk (zuerst bank.xsb) per CopyFile nach Z: und prueft jede Kopie
 * durch Oeffnen; scheitert das, wiederholt es ohne Ende. Das Startbild wartet
 * auf diesen Thread (Dolby-Logo), die Soundbaenke liest das Spiel danach aus Z:.
 * Ist <hdd_root_loc>\ZCACHE nicht beschreibbar, oeffnet das Spiel also tausendfach
 * je Sekunde D:\...\bank.xsb (Log "FILE open ...bank.xsb -> xh=0x113") und haengt.
 * Hier: Schreibtest beim Start, Ersatzordner (loc_zcache_alt), Erkennung der
 * Schleife mit einer klaren Zeile, Drosselung gleicher Oeffnungen im Log,
 * Zustand fuer den F11-Bericht. Alles nur im LoC-Profil (g_zc_pre leer bei SB). */
static char g_zc_pre[640];          /* "<hdd_root>\ZCACHE\" (Backslashes), leer = nicht LoC */
static char g_zc_alt[640];          /* Ersatzordner fuer Z: (leer = keiner) */
static volatile LONG g_zc_state;    /* 0 = ungeprueft, 1 = ok, 2 = Ersatz aktiv, 3 = kein Ordner beschreibbar */
static volatile LONG g_zc_err;      /* letzter Win32-Fehler beim Schreiben nach Z: */
static volatile LONG g_zc_wfail;    /* Schreibfehler auf Z: gesamt */
static volatile LONG g_loc_copy_done; /* COPY_CHK.txt auf Z: erfolgreich geoeffnet = Start-Kopie fertig */
static volatile LONG g_loc_reopen_max; /* groesste Zahl gleicher Oeffnungen in Folge */
static char g_loc_reopen_name[64];  /* Dateiname dazu (ohne Ordner) */
static volatile LONG g_lbd_n;       /* DIAG-Zeilen bisher (Grenze) */
static ULONGLONG g_zc_t0;           /* Zeitpunkt des Schreibtests (Startphase) */

static int zc_starts(const char *s, const char *pre)
{
    size_t n = strlen(pre);
    return n && _strnicmp(s, pre, n) == 0;
}

static int zc_is_z(const char *host)
{
    return g_zc_pre[0] && (zc_starts(host, g_zc_pre) || (g_zc_alt[0] && zc_starts(host, g_zc_alt) &&
                                                          host[strlen(g_zc_alt)] == '\\'));
}

static int lbd_ok(void)
{
    return g_cfg.loc_boot_diag && InterlockedIncrement(&g_lbd_n) <= 600;
}

static int lbd_path(const char *p)
{
    return zc_is_z(p) || strstr(p, "\\sndeff\\") != NULL || strstr(p, "\\SNDEFF\\") != NULL;
}

/* Schreibtest: Ordner anlegen, Probedatei schreiben und loeschen. 1 = beschreibbar. */
static int zc_probe(const char *dir, DWORD *err)
{
    char p[700];
    HANDLE h;
    DWORD wr = 0;
    int ok;
    xb_mkdir_p(dir);
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\~sb_zprobe.tmp", dir);
    h = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        return 0;
    }
    ok = WriteFile(h, "Z", 1, &wr, NULL) && wr == 1;
    *err = ok ? 0 : GetLastError();
    CloseHandle(h);
    return ok;
}

/* Ersatzordner k (0 = %LOCALAPPDATA%\Steelbound\loc_zcache, 1 = %TEMP%\Steelbound_loc_zcache)
 * als ANSI-sicherer Kurzpfad. 1 = ok. */
static int zc_alt_dir(int k, char *out, size_t cap)
{
    wchar_t w[600], s[600];
    DWORD n;
    BOOL lossy = FALSE;
    if (k == 0) {
        n = GetEnvironmentVariableW(L"LOCALAPPDATA", w, 520);
        if (n == 0 || n >= 520) return 0;
        wcscat_s(w, 600, L"\\Steelbound");
        CreateDirectoryW(w, NULL);
        wcscat_s(w, 600, L"\\loc_zcache");
    } else {
        n = GetTempPathW(520, w);
        if (n == 0 || n >= 520) return 0;
        while (n && (w[n - 1] == L'\\' || w[n - 1] == L'/')) w[--n] = 0;
        wcscat_s(w, 600, L"\\Steelbound_loc_zcache");
    }
    CreateDirectoryW(w, NULL);
    n = GetShortPathNameW(w, s, 600);
    if (n == 0 || n >= 600) wcscpy_s(s, 600, w);
    n = (DWORD)WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, s, -1, out, (int)cap, NULL, &lossy);
    return n > 0 && !lossy && strlen(out) < 400;
}

/* Z: auf einen beschreibbaren Ersatzordner legen (einmal). */
static void zc_switch(const char *why)
{
    char d[640];
    DWORD e = 0;
    int k;
    if (!g_cfg.loc_zcache_alt) {
        HLOG_W("LOCZC: kein Ersatzordner fuer Z: (loc_zcache_alt=0, %s)", why);
        InterlockedExchange(&g_zc_state, 3);
        return;
    }
    if (g_zc_state >= 2) return;
    for (k = 0; k < 2; k++) {
        if (zc_alt_dir(k, d, sizeof d) && zc_probe(d, &e)) {
            char *s;
            for (s = d; *s; s++) {
                if (*s == '/') *s = '\\'; /* Host-Pfade vergleichen mit Backslashes (zc_is_z) */
            }
            strcpy_s(g_zc_alt, sizeof g_zc_alt, d);
            InterlockedExchange(&g_zc_state, 2);
            HLOG_W("LOCZC: Datei-Cache Z: weicht aus nach '%s' (%s, Win32 %ld) - Start laeuft weiter", d, why,
                   (long)g_zc_err);
            return;
        }
    }
    InterlockedExchange(&g_zc_state, 3);
    HLOG_E("LOCZC: auch kein Ersatzordner fuer Z: beschreibbar (%s, Win32 %lu) - LoC kann den Start nicht beenden", why,
           (unsigned long)e);
}

/* Aufruf aus dem Kernel-Start (nach dem Anlegen von ZCACHE), nur LoC-Profil. */
void krnl_loc_zcache_check(void)
{
    char d[640];
    char *s;
    DWORD e = 0;
    _snprintf_s(d, sizeof d, _TRUNCATE, "%s\\ZCACHE", g_cfg.hdd_root);
    for (s = d; *s; s++) {
        if (*s == '/') *s = '\\';
    }
    _snprintf_s(g_zc_pre, sizeof g_zc_pre, _TRUNCATE, "%s\\", d);
    g_zc_t0 = GetTickCount64();
    if (zc_probe(d, &e)) {
        InterlockedExchange(&g_zc_state, 1);
        if (g_cfg.loc_boot_diag) HLOG_I("LOCBOOTDIAG: Datei-Cache Z: beschreibbar (Pfadlaenge %u)", (unsigned)strlen(d));
        return;
    }
    InterlockedExchange(&g_zc_err, (LONG)e);
    HLOG_W("LOCZC: Datei-Cache Z: nicht beschreibbar ('%s', Win32 %lu, Pfadlaenge %u) - LoC kopiert beim Start "
           "media\\sndeff nach Z: und wuerde endlos wiederholen", d, (unsigned long)e, (unsigned)strlen(d));
    zc_switch("Schreibtest beim Start");
}

/* 1 = Start-Kopie fertig (COPY_CHK.txt gesehen) bzw. Startphase vorbei (Sicherheitsnetz 90 s, falls eine
 * andere Fassung COPY_CHK.txt nie oeffnet); nur fuer die Pause-Regel im LoC-Start. */
int krnl_loc_copy_done(void)
{
    return g_loc_copy_done != 0 || !g_zc_pre[0] || (g_zc_t0 && GetTickCount64() - g_zc_t0 > 90000u);
}

/* Kurzzustand fuer F11-Bericht (keine Pfade, keine Eingaben). */
int krnl_loc_boot_diag_line(char *o, int cap)
{
    static const char *zs[] = { "unchecked", "ok", "fallback", "unwritable" };
    LONG st = g_zc_state;
    if (!g_zc_pre[0]) return 0;
    _snprintf_s(o, (size_t)cap, _TRUNCATE, "zcache=%s win32=%ld write_fail=%ld start_copy=%s reopen_max=%ld (%s)",
                zs[st & 3], (long)g_zc_err, (long)g_zc_wfail, g_loc_copy_done ? "done" : "running",
                (long)g_loc_reopen_max, g_loc_reopen_name[0] ? g_loc_reopen_name : "-");
    return 1;
}

/* Erfolgreiches Oeffnen: gleiche Datei in Folge (ohne anderes erfolgreiches Oeffnen dazwischen)
 * nur beim 1.-10., 100., 1000., ... Mal protokollieren; ab 50 einmalig eine Schleifen-Meldung. */
static int loc_open_ok_log(const char *host)
{
    static char last[512];
    static unsigned long n;
    static LONG lk, said;
    int show;
    while (InterlockedExchange(&lk, 1)) {
        SwitchToThread();
    }
    if (strcmp(last, host) == 0) {
        n++;
    } else {
        strncpy_s(last, sizeof last, host, _TRUNCATE);
        n = 1;
    }
    if ((LONG)n > g_loc_reopen_max) {
        const char *b = strrchr(host, '\\');
        InterlockedExchange(&g_loc_reopen_max, (LONG)n);
        strncpy_s(g_loc_reopen_name, sizeof g_loc_reopen_name, b ? b + 1 : host, _TRUNCATE);
    }
    show = n <= 10 || n == 100 || n == 1000 || n % 10000 == 0;
    if (show && n >= 100) {
        HLOG_I("LOCFILE: '%s' erneut geoeffnet (%lu Mal in Folge)", host, n);
    }
    if (n == 50 && !said) {
        said = 1;
        HLOG_E("LOCBOOT: Endlosschleife erkannt - '%s' wird immer wieder geoeffnet (50 Mal in Folge; Z: %s, Win32 %ld, "
               "Schreibfehler Z: %ld). Bei media\\sndeff: LoC kann den Datei-Cache Z: nicht fuellen, Start haengt am "
               "Dolby-Logo", host, g_zc_state == 3 ? "nicht beschreibbar" : g_zc_state == 2 ? "Ersatzordner" : "ok",
               (long)g_zc_err, (long)g_zc_wfail);
    }
    InterlockedExchange(&lk, 0);
    return show;
}

/* Schreibfehler auf Z: (gleiche Datei in Folge zaehlen); ab dem 3. Ersatzordner. */
static void zc_write_failed(const char *host, DWORD e)
{
    static char last[512];
    static unsigned long n;
    static LONG lk;
    unsigned long k;
    while (InterlockedExchange(&lk, 1)) {
        SwitchToThread();
    }
    if (strcmp(last, host) == 0) {
        n++;
    } else {
        strncpy_s(last, sizeof last, host, _TRUNCATE);
        n = 1;
    }
    k = n;
    InterlockedExchange(&lk, 0);
    InterlockedIncrement(&g_zc_wfail);
    InterlockedExchange(&g_zc_err, (LONG)e);
    if (k == 3 && g_zc_state < 2) {
        HLOG_W("LOCZC: Schreiben nach Z: scheitert wiederholt ('%s', Win32 %lu)", host, (unsigned long)e);
        zc_switch("Kopierfehler");
    }
}

/* Ersatzordner aktiv: Pfad im Ersatzordner -> Pfad im urspruenglichen ZCACHE (Lesen als Rueckfall). */
static int zc_orig_path(const char *host, char *out, size_t cap)
{
    size_t n = strlen(g_zc_alt);
    if (!g_zc_alt[0] || !zc_starts(host, g_zc_alt) || host[n] != '\\') return 0;
    _snprintf_s(out, cap, _TRUNCATE, "%s%s", g_zc_pre, host + n + 1);
    return 1;
}

/* ---------- W449 LOC-BOOT-SEQ-PERF (GitHub #85): Lesedauer im LoC-Profil ----------
 * Befund: niedrige FPS und kein Ton in der VT-Startsequenz beim Tester von #71/#73. Nachgestellt (Datenordner per ACL
 * gesperrt, 0.3.3- und W434-Verhalten) bleiben Bildrate, Ton-Cues und Hash gleich - die Sperre allein erklaert es nicht.
 * Offen ist ein langsamer Datentraeger bzw. Virenschutz, der jedes Lesen bremst (Spiel-Thread wartet, Ton-Stream
 * verhungert). Deshalb nur Zaehler: Anzahl, langsame (>= 50 ms = ein Missionstick) und laengste Lesung je Quelle.
 * 0 = Disc (D:, Abbild/Ordner), 1 = Festplatte (T:/U:/Z:/E:), 2 = Ton-Stream (Wave-Banks, loc_audio.c). */
#define LIO_SLOW_US 50000
static volatile LONG g_lio_n[3], g_lio_slow[3], g_lio_max_us[3], g_lio_err[3];
static volatile LONG g_lio_logged_slow; /* Summe langsamer Lesungen bei der letzten LOCIO-Zeile */

LONGLONG krnl_loc_io_t0(void)
{
    LARGE_INTEGER q;
    if (!g_sba_loc) return 0;
    QueryPerformanceCounter(&q);
    return q.QuadPart ? q.QuadPart : 1;
}

void krnl_loc_io_note(int k, LONGLONG q0, int ok)
{
    static LONGLONG qpf;
    LARGE_INTEGER q;
    LONGLONG us;
    LONG cur;
    if (!q0 || k < 0 || k > 2) return;
    QueryPerformanceCounter(&q);
    if (!qpf) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpf = f.QuadPart ? f.QuadPart : 1;
    }
    us = (q.QuadPart - q0) * 1000000 / qpf;
    if (us < 0) us = 0;
    if (us > 0x7FFFFFFF) us = 0x7FFFFFFF;
    InterlockedIncrement(&g_lio_n[k]);
    if (!ok) InterlockedIncrement(&g_lio_err[k]);
    if (us >= LIO_SLOW_US) InterlockedIncrement(&g_lio_slow[k]);
    cur = g_lio_max_us[k];
    while ((LONG)us > cur) {
        LONG prev = InterlockedCompareExchange(&g_lio_max_us[k], (LONG)us, cur);
        if (prev == cur) break;
        cur = prev;
    }
}

/* F11 info.txt "loc io:" (nur Zaehler, keine Pfade). 0 = nicht LoC. */
int krnl_loc_io_diag_line(char *o, int cap)
{
    static const char *nm[3] = { "disc", "hdd", "sound stream" };
    int k, n = 0;
    if (!g_sba_loc || cap < 2) return 0;
    o[0] = '\0';
    for (k = 0; k < 3; k++) {
        n += _snprintf_s(o + n, (size_t)(cap - n), _TRUNCATE, "%s%s reads=%ld slow(>=50ms)=%ld max=%.1fms err=%ld",
                         k ? "; " : "", nm[k], (long)g_lio_n[k], (long)g_lio_slow[k], g_lio_max_us[k] / 1000.0,
                         (long)g_lio_err[k]);
        if (n < 0 || n >= cap - 1) break;
    }
    return 1;
}

/* Logzeile alle 30 s (loc_audio_tick), nur wenn seit der letzten Zeile neue langsame Lesungen dazukamen. */
void krnl_loc_io_log(int waited_s)
{
    char b[400];
    LONG s = g_lio_slow[0] + g_lio_slow[1] + g_lio_slow[2];
    if (!g_sba_loc || s == g_lio_logged_slow) return;
    g_lio_logged_slow = s;
    if (krnl_loc_io_diag_line(b, (int)sizeof b)) {
        HLOG_W("LOCIO: t=%ds langsames Lesen (Datentraeger/Virenschutz?) - %s", waited_s, b);
    }
}

/* ---------- W434 LOC-PILOT-LOST (GitHub #73): Spielstand-Ordner im LoC-Profil ----------
 * LoC speichert den Piloten auf T: in SYSTEMLINK/LINKUSER (<hdd_root_loc>/TDATA/43430009/...),
 * Einstellungen in CONFIG/CONFIG.DAT, Replays/Insignien auf U:. Ist der ganze Ordner
 * nicht beschreibbar, schlug bisher jedes Schreiben fehl (FILE open ... -> FEHLER), das Spiel
 * zeigte den Piloten trotzdem (nur im Speicher), nach dem Neustart war er weg. W423 hatte nur
 * Z: (Sound-Cache) umgelenkt. Hier: Schreibtest beim Start; bei Fehler laeuft das ganze
 * Laufwerk (T:/U:/E:/Z:) ueber den festen Ersatzordner %LOCALAPPDATA%\Steelbound\loc_save
 * (ANSI-sicherer Kurzpfad, nie %TEMP%). Vorhandene Staende des Originals werden einmal
 * uebernommen (nie ueberschrieben); die Merkdatei im Ersatzordner nennt das Original und haelt
 * den Ersatzordner aktiv, auch wenn das Original spaeter wieder beschreibbar ist (Lesen und
 * Schreiben bleiben am selben Ort). Geht auch das nicht: klare Meldung (Log, Einblendung, F11),
 * kein stilles Verwerfen. Nur LoC-Profil (g_ls_state 0 bei SB). */
#define LS_MARK "steelbound_loc_save.txt"
static volatile LONG g_ls_state;   /* 0 = nicht LoC, 1 = ok, 2 = Ersatzordner, 3 = nicht beschreibbar */
static volatile LONG g_ls_err;     /* Win32-Fehler des Schreibtests im Original */
static volatile LONG g_ls_wfail;   /* gescheiterte Schreib-Oeffnungen auf T:/U:/E: */
static volatile LONG g_ls_werr;    /* letzter Win32-Fehler dazu */
static int g_ls_sticky, g_ls_mig;  /* Ersatzordner per Merkdatei / einmal uebernommene Dateien */
static char g_ls_root[640];        /* aktiver Spielstand-Ordner (Backslashes, ohne Endtrenner) */
static char g_ls_show[400];        /* aktiver Ordner mit Platzhaltern (UTF-8) */
static char g_ls_oshow[400];       /* urspruenglicher Ordner mit Platzhaltern (UTF-8) */
static ULONGLONG g_ls_toast_t;

static void ls_norm(char *s)
{
    for (; *s; s++) {
        if (*s == '/') *s = '\\';
    }
}

static void ls_strip(char *s)
{
    size_t n = strlen(s);
    while (n > 3 && (s[n - 1] == '\\' || s[n - 1] == '/')) s[--n] = 0;
}

/* Grund in Klartext (englisch, fuer Einblendung/Log/F11). */
static const char *ls_reason(DWORD e)
{
    switch (e) {
    case 0: return "ok";
    case ERROR_ACCESS_DENIED: return "access denied - folder permissions, Controlled Folder Access or antivirus";
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: return "folder cannot be created";
    case ERROR_INVALID_NAME: return "invalid path - characters outside the system code page";
    case ERROR_WRITE_PROTECT: return "drive is write-protected";
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL: return "disk full";
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION: return "file in use by another program";
    case ERROR_FILENAME_EXCED_RANGE: return "path too long";
    default: return "Windows error";
    }
}

/* Ordner fuer Log/F11/Einblendung: Langform, %LOCALAPPDATA% usw., Client-Ordner -> <client>,
 * sonst <pfad>\name (sb_diag_sanitize, gleiche Regeln wie F11). Nie Benutzername. */
static void ls_show_path(const char *p, char *out, size_t cap)
{
    wchar_t w[700], l[700], ew[700];
    char exe[700], *s;
    sb_diag_root_t root[1];
    DWORD n;
    out[0] = 0;
    if (!MultiByteToWideChar(CP_ACP, 0, p, -1, w, 700)) return;
    n = GetLongPathNameW(w, l, 700);
    if (n == 0 || n >= 700) wcscpy_s(l, 700, w);
    for (n = 0; l[n]; n++) {
        if (l[n] == L'/') l[n] = L'\\';
    }
    if (!WideCharToMultiByte(CP_UTF8, 0, l, -1, out, (int)cap, NULL, NULL)) out[0] = 0;
    exe[0] = 0;
    n = GetModuleFileNameW(NULL, ew, 700);
    if (n && n < 700 && WideCharToMultiByte(CP_UTF8, 0, ew, -1, exe, sizeof exe, NULL, NULL)) {
        s = strrchr(exe, '\\');
        if (s) *s = 0;
    }
    root[0].path = exe;
    root[0].tag = "<client>";
    {
        char last[64] = "";
        const char *b = strrchr(out, '\\');
        if (b && strlen(b + 1) < sizeof last) strcpy_s(last, sizeof last, b + 1);
        sb_diag_sanitize(out, cap, root, exe[0] ? 1 : 0);
        if (strcmp(out, "<pfad>") == 0 && last[0] && sb_diag_clean_hits(last) == 0) { /* Ordnername bleibt lesbar */
            _snprintf_s(out, cap, _TRUNCATE, "<pfad>\\%s", last);
        }
    }
}

/* Ordner fuer die Einblendung (Spieler soll ihn finden): Langform, nur %LOCALAPPDATA%/%USERPROFILE%
 * ersetzt (kein Benutzername im Bild bzw. in einem F11-Bildschirmfoto). UTF-16. */
static void ls_ui_path(const char *p, wchar_t *out, int cap)
{
    static const wchar_t *const ev[][2] = { { L"LOCALAPPDATA", L"%LOCALAPPDATA%" }, { L"USERPROFILE", L"%USERPROFILE%" } };
    wchar_t w[700], l[700], e[700], el[700];
    DWORD n;
    int i;
    out[0] = 0;
    if (!MultiByteToWideChar(CP_ACP, 0, p, -1, w, 700)) return;
    n = GetLongPathNameW(w, l, 700);
    if (n == 0 || n >= 700) wcscpy_s(l, 700, w);
    for (n = 0; l[n]; n++) {
        if (l[n] == L'/') l[n] = L'\\';
    }
    for (i = 0; i < 2; i++) {
        size_t k;
        n = GetEnvironmentVariableW(ev[i][0], e, 700);
        if (n == 0 || n >= 700) continue;
        n = GetLongPathNameW(e, el, 700);
        if (n == 0 || n >= 700) wcscpy_s(el, 700, e);
        for (k = 0; el[k]; k++) {
            if (el[k] == L'/') el[k] = L'\\';
        }
        k = wcslen(el);
        while (k > 3 && el[k - 1] == L'\\') el[--k] = 0;
        if (k >= 3 && _wcsnicmp(l, el, k) == 0 && (l[k] == L'\\' || l[k] == 0)) {
            _snwprintf_s(out, cap, _TRUNCATE, L"%s%s", ev[i][1], l + k);
            return;
        }
    }
    wcsncpy_s(out, cap, l, _TRUNCATE);
}

/* Schreibtest eines Spiel-Laufwerks: TDATA\<id> und UDATA\<id> anlegen und je eine
 * Probedatei schreiben. 1 = beschreibbar. */
/* Ordner fehlt (Win32 2/3): eigentlicher Grund = Anlegen im tiefsten vorhandenen Elternordner. */
static DWORD ls_mkdir_err(const char *dir)
{
    char t[700], p[760];
    char *s;
    DWORD a;
    strcpy_s(t, sizeof t, dir);
    for (;;) {
        a = GetFileAttributesA(t);
        if (a != INVALID_FILE_ATTRIBUTES) break;
        s = strrchr(t, '\\');
        if (!s || s - t < 3) return 0;
        *s = 0;
    }
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_PATH_NOT_FOUND; /* Datei statt Ordner */
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\~sb_dprobe", t);
    if (CreateDirectoryA(p, NULL)) {
        RemoveDirectoryA(p);
        return 0;
    }
    a = GetLastError();
    return a == ERROR_ALREADY_EXISTS ? 0 : a;
}

static int ls_probe_dir(const char *d, DWORD *err)
{
    DWORD m;
    if (zc_probe(d, err)) return 1;
    if (*err == ERROR_PATH_NOT_FOUND || *err == ERROR_FILE_NOT_FOUND) {
        m = ls_mkdir_err(d);
        if (m) *err = m;
    }
    return 0;
}

static int ls_probe_drive(const char *root, DWORD *err)
{
    char d[700];
    *err = 0;
    _snprintf_s(d, sizeof d, _TRUNCATE, "%s\\TDATA\\%s", root, XB_TITLE_ID_STR);
    if (!ls_probe_dir(d, err)) return 0;
    _snprintf_s(d, sizeof d, _TRUNCATE, "%s\\UDATA\\%s", root, XB_TITLE_ID_STR);
    return ls_probe_dir(d, err);
}

/* Ersatzordner %LOCALAPPDATA%\Steelbound\loc_save als ANSI-sicherer Kurzpfad (wie W423). create = 0:
 * nur wenn er schon existiert (beschreibbarer Normalfall legt nichts an). 1 = ok. */
static int ls_alt_dir(char *out, size_t cap, int create)
{
    wchar_t w[600], s[600];
    DWORD n;
    BOOL lossy = FALSE;
    out[0] = 0;
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", w, 520);
    if (n == 0 || n >= 520) return 0;
    wcscat_s(w, 600, L"\\Steelbound");
    if (create) CreateDirectoryW(w, NULL);
    wcscat_s(w, 600, L"\\loc_save");
    if (create) CreateDirectoryW(w, NULL);
    else if (GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES) return 0;
    n = GetShortPathNameW(w, s, 600);
    if (n == 0 || n >= 600) wcscpy_s(s, 600, w);
    n = (DWORD)WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, s, -1, out, (int)cap, NULL, &lossy);
    if (n == 0 || lossy || strlen(out) >= 400) {
        out[0] = 0;
        return 0;
    }
    ls_norm(out);
    ls_strip(out);
    return 1;
}

/* Baum kopieren, vorhandene Ziele nie ueberschreiben. Rueckgabe: kopierte Dateien. */
static int ls_copy_tree(const char *src, const char *dst, int depth, int *budget)
{
    char m[800], a[800], b[800];
    WIN32_FIND_DATAA fd;
    HANDLE f;
    int n = 0;
    if (depth > 8 || *budget <= 0) return 0;
    _snprintf_s(m, sizeof m, _TRUNCATE, "%s\\*", src);
    f = FindFirstFileA(m, &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    xb_mkdir_p(dst);
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue; /* nie Verknuepfungen folgen */
        _snprintf_s(a, sizeof a, _TRUNCATE, "%s\\%s", src, fd.cFileName);
        _snprintf_s(b, sizeof b, _TRUNCATE, "%s\\%s", dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            n += ls_copy_tree(a, b, depth + 1, budget);
        } else if ((*budget)-- > 0 && CopyFileA(a, b, TRUE)) {
            n++;
        }
    } while (*budget > 0 && FindNextFileA(f, &fd));
    FindClose(f);
    return n;
}

/* Merkdatei lesen: 1 = vorhanden, orig = Inhalt (urspruenglicher Ordner). */
static int ls_mark_read(const char *alt, char *orig, size_t cap)
{
    char p[700];
    FILE *f;
    size_t n;
    orig[0] = 0;
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\" LS_MARK, alt);
    if (fopen_s(&f, p, "rb") != 0 || !f) return 0;
    n = fread(orig, 1, cap - 1, f);
    fclose(f);
    orig[n] = 0;
    while (n && (orig[n - 1] == '\r' || orig[n - 1] == '\n' || orig[n - 1] == ' ')) orig[--n] = 0;
    return 1;
}

static void ls_mark_write(const char *alt, const char *orig)
{
    char p[700];
    FILE *f;
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\" LS_MARK, alt);
    if (fopen_s(&f, p, "wb") == 0 && f) {
        fprintf(f, "%s\r\n", orig);
        fclose(f);
    }
}

/* Kern (auch Selbsttest): orig = Spielstand-Ordner laut Einstellung, alt = Ersatzordner ("" = keiner).
 * out = aktiver Ordner. Rueckgabe 1 ok, 2 Ersatzordner, 3 nicht beschreibbar. */
static int ls_resolve(const char *orig, const char *alt, char *out, size_t cap, DWORD *err, DWORD *aerr, int *mig,
                      int *sticky)
{
    char mk[700];
    int ok, has_mark, mine;
    DWORD ae = 0;
    *err = *aerr = 0;
    *mig = *sticky = 0;
    ok = ls_probe_drive(orig, err);
    has_mark = alt[0] && ls_mark_read(alt, mk, sizeof mk);
    mine = has_mark && _stricmp(mk, orig) == 0;
    if (mine && ls_probe_drive(alt, &ae)) { /* Ersatzordner bleibt aktiv (Staende liegen dort) */
        strcpy_s(out, cap, alt);
        *sticky = 1;
        return 2;
    }
    if (ok) {
        strcpy_s(out, cap, orig);
        return 1;
    }
    if (!alt[0] || !ls_probe_drive(alt, &ae)) {
        *aerr = alt[0] ? ae : ERROR_PATH_NOT_FOUND;
        strcpy_s(out, cap, orig);
        return 3;
    }
    if (!has_mark) { /* erstes Mal: vorhandene Staende einmal uebernehmen, nie ueberschreiben */
        char a[700], b[700];
        int budget = 4000;
        _snprintf_s(a, sizeof a, _TRUNCATE, "%s\\TDATA", orig);
        _snprintf_s(b, sizeof b, _TRUNCATE, "%s\\TDATA", alt);
        *mig = ls_copy_tree(a, b, 0, &budget);
        _snprintf_s(a, sizeof a, _TRUNCATE, "%s\\UDATA", orig);
        _snprintf_s(b, sizeof b, _TRUNCATE, "%s\\UDATA", alt);
        *mig += ls_copy_tree(a, b, 0, &budget);
        ls_mark_write(alt, orig);
    }
    strcpy_s(out, cap, alt);
    return 2;
}

static void ls_toast(int fail, const char *why)
{
    wchar_t t1[160], fmt[260], t2[600], pw[400], ww[200];
    char orig[640];
    strcpy_s(orig, sizeof orig, g_cfg.hdd_root);
    ls_ui_path(fail ? orig : g_ls_root, pw, 400); /* vor dem Umschalten = Original */
    if (!MultiByteToWideChar(CP_UTF8, 0, why, -1, ww, 200)) ww[0] = 0;
    {   /* Einblendung hat 200 Zeichen: Grund kurz ("access denied"), Einzelheiten im Log/F11 */
        wchar_t *c = wcsstr(ww, L" - ");
        if (c) *c = 0;
    }
    if (fail) {
        sb_lang_w("loc.save_fail_t", t1, 160);
        sb_lang_w("loc.save_fail_d", fmt, 260);
    } else {
        sb_lang_w("loc.save_alt_t", t1, 160);
        sb_lang_w("loc.save_alt_d", fmt, 260);
    }
    if (wcsstr(fmt, L"%s")) _snwprintf_s(t2, 600, _TRUNCATE, fmt, pw, ww);
    else wcsncpy_s(t2, 600, fmt, _TRUNCATE);
    sb_ov_toast_msg(t1, t2, fail ? 15000u : 12000u);
}

/* Aufruf aus dem Kernel-Start (nach dem Anlegen von TDATA/UDATA, vor Z:), nur LoC-Profil.
 * Setzt bei Bedarf g_cfg.hdd_root auf den Ersatzordner (dann folgen T:/U:/E:/Z:). */
void krnl_loc_save_check(void)
{
    char orig[640], alt[640] = "", act[640];
    DWORD e = 0, ae = 0;
    int st, mig = 0, sticky = 0;
    strcpy_s(orig, sizeof orig, g_cfg.hdd_root);
    ls_norm(orig);
    ls_strip(orig);
    ls_show_path(orig, g_ls_oshow, sizeof g_ls_oshow);
    if (g_cfg.loc_save_alt && !ls_alt_dir(alt, sizeof alt, 0) && !ls_probe_drive(orig, &e)) {
        ls_alt_dir(alt, sizeof alt, 1); /* erst bei Bedarf anlegen */
    }
    st = ls_resolve(orig, alt, act, sizeof act, &e, &ae, &mig, &sticky);
    g_ls_sticky = sticky;
    g_ls_mig = mig;
    InterlockedExchange(&g_ls_err, (LONG)e);
    strcpy_s(g_ls_root, sizeof g_ls_root, act);
    ls_show_path(act, g_ls_show, sizeof g_ls_show);
    if (st == 2) strcpy_s(g_cfg.hdd_root, sizeof g_cfg.hdd_root, act);
    InterlockedExchange(&g_ls_state, st);
    if (st == 1) {
        HLOG_I("LOCSAVE: save folder '%s' writable", g_ls_show);
    } else if (st == 2 && sticky) {
        HLOG_W("LOCSAVE: save folder '%s' (fallback, kept because it holds the saves since the original '%s' was not "
               "writable; original now %s) - reading and writing stay here; to go back, copy TDATA/UDATA back and "
               "delete " LS_MARK, g_ls_show, g_ls_oshow, e ? "still not writable" : "writable again");
    } else if (st == 2) {
        HLOG_W("LOCSAVE: save folder '%s' is not writable (%s, Win32 %lu) - Line of Contact saves to fallback '%s' "
               "instead (%d existing files copied once)", g_ls_oshow, ls_reason(e), (unsigned long)e, g_ls_show, mig);
        ls_toast(0, ls_reason(e));
    } else {
        HLOG_E("LOCSAVE: save folder '%s' is not writable (%s, Win32 %lu) and %s - pilots and settings will NOT be kept",
               g_ls_oshow, ls_reason(e), (unsigned long)e,
               !g_cfg.loc_save_alt ? "the fallback is off (loc_save_alt=0)" : "no fallback folder is writable either");
        ls_toast(1, ls_reason(e));
        g_ls_toast_t = GetTickCount64();
    }
}

/* Gescheiterte Schreib-Oeffnung auf dem Spielstand-Laufwerk (nicht Z:): melden statt still verwerfen. */
static void ls_write_failed(const char *host, DWORD e)
{
    size_t n = strlen(g_ls_root);
    LONG k;
    if (!g_ls_state || !n || _strnicmp(host, g_ls_root, n) != 0 || host[n] != '\\' || zc_is_z(host)) return;
    k = InterlockedIncrement(&g_ls_wfail);
    InterlockedExchange(&g_ls_werr, (LONG)e);
    if (k <= 20) {
        HLOG_E("LOCSAVE: could not write '%s' in the save folder (%s, Win32 %lu) - not saved", host + n + 1,
               ls_reason(e), (unsigned long)e);
    }
    if (GetTickCount64() - g_ls_toast_t > 20000u || !g_ls_toast_t) {
        g_ls_toast_t = GetTickCount64();
        ls_toast(1, ls_reason(e));
    }
}

/* Kurzzustand fuer F11-Bericht (Ordner mit Platzhaltern, keine Eingaben). */
int krnl_loc_save_diag_line(char *o, int cap)
{
    static const char *ss[] = { "-", "ok", "fallback", "unwritable" };
    LONG st = g_ls_state;
    if (!st) return 0;
    _snprintf_s(o, (size_t)cap, _TRUNCATE,
                "folder=%s writable=%s state=%s%s original=%s win32=%ld (%s) write_fail=%ld last_win32=%ld copied=%d",
                g_ls_show, st == 3 ? "no" : "yes", ss[st & 3], g_ls_sticky ? " (kept)" : "", g_ls_oshow,
                (long)g_ls_err, ls_reason((DWORD)g_ls_err), (long)g_ls_wfail, (long)g_ls_werr, g_ls_mig);
    return 1;
}

/* W213: A pending host entry survives a short caller buffer. */
typedef struct {
    HANDLE find;
    WIN32_FIND_DATAA data;
    char mask[260];
    u32 index;
    int pending;
} host_dir_t;

static void host_dir_close(ht_entry_t *e)
{
    host_dir_t *d = (host_dir_t *)(uintptr_t)e->dir_state;
    if (!d) return;
    if (d->find != INVALID_HANDLE_VALUE) FindClose(d->find);
    free(d);
    e->dir_state = 0;
}

/* Schreibende Zugriffsrechte (NT-Maske): GENERIC_WRITE/ALL, WRITE_DATA,
 * APPEND_DATA, WRITE_EA, WRITE_ATTRIBUTES, DELETE, WRITE_DAC/OWNER. */
#define VFS_WRITE_MASK (0x40000000u | 0x10000000u | 0x2u | 0x4u | 0x10u | 0x100u | \
                        0x10000u | 0x40000u | 0x80000u)

int krnl_vfs_open(const char *image, char *err, unsigned cap)
{
    if (!image || !image[0]) {
        return 0;
    }
    g_iso = iso_vfs_open(image, err, cap);
    if (!g_iso) {
        return -1;
    }
    HLOG_I("ISO: Abbild '%s' geoeffnet (Partition @0x%llX, %u Eintraege, %u Verzeichnisse) - D: liest nur aus dem Abbild",
           image, (unsigned long long)iso_vfs_part_base(g_iso), iso_vfs_count(g_iso),
           iso_vfs_dir_count(g_iso));
    return 0;
}

int krnl_vfs_active(void)
{
    return g_iso != NULL;
}

int krnl_vfs_handle(unsigned xh)
{
    ht_entry_t *e;
    if (!g_iso) {
        return 0;
    }
    e = ht_get(xh);
    return e && e->kind == HT_FILE && e->vfs;
}

int krnl_vfs_read_all(const char *rel, unsigned char **out, unsigned *len)
{
    uint32_t n = 0;
    int rc;
    if (!g_iso) {
        return -1;
    }
    rc = iso_vfs_read_all(g_iso, rel, out, &n);
    *len = n;
    return rc;
}

/* Lesen auf Abbild-Handle; off = NULL oder negativ -> eigener Zeiger
 * (NT: FILE_USE_FILE_POINTER_POSITION). Rueckgabe Win32-Fehler (0 = ok). */
static DWORD vfs_read_at(ht_entry_t *e, const int64_t *off, void *buf, u32 len, DWORD *got)
{
    uint64_t pos = (off && *off >= 0) ? (uint64_t)*off : e->vfs_pos;
    uint32_t g = 0;
    DWORD er = (DWORD)iso_vfs_read(g_iso, e->vfs_node, pos, buf, len, &g);
    *got = g;
    e->vfs_pos = pos + g;
    return er;
}

int krnl_vfs_read(unsigned xh, const long long *off, void *buf, unsigned len,
                  unsigned long *got, char *path, unsigned pathcap)
{
    ht_entry_t *e = ht_get(xh);
    DWORD g = 0;
    if (!e || e->kind != HT_FILE || !e->vfs || !g_iso) {
        return 0;
    }
    vfs_read_at(e, (const int64_t *)off, buf, len, &g);
    *got = g;
    if (path && pathcap) {
        strncpy_s(path, pathcap, e->path, _TRUNCATE);
    }
    return 1;
}

/* ---------- Symlink-Tabelle ---------- */
#define SYMLINK_MAX 64
static struct {
    char from[256];
    char to[256];
} g_symlinks[SYMLINK_MAX];

static void symlink_add(const char *from, const char *to)
{
    unsigned i;
    for (i = 0; i < SYMLINK_MAX; i++) {
        if (!g_symlinks[i].from[0]) {
            strncpy_s(g_symlinks[i].from, sizeof g_symlinks[i].from, from, _TRUNCATE);
            strncpy_s(g_symlinks[i].to, sizeof g_symlinks[i].to, to, _TRUNCATE);
            return;
        }
    }
}

static const char *symlink_find(const char *from)
{
    unsigned i;
    for (i = 0; i < SYMLINK_MAX; i++) {
        if (g_symlinks[i].from[0] && _stricmp(g_symlinks[i].from, from) == 0) {
            return g_symlinks[i].to;
        }
    }
    return NULL;
}

/* ---------- Xbox-Pfad -> Host-Pfad ---------- */
static void wide_to_narrow(const wchar_t *w, size_t nchars, char *out, size_t cap)
{
    size_t i;
    for (i = 0; i + 1 < cap && i < nchars && w[i]; i++) {
        wchar_t c = w[i];
        out[i] = (char)(c < 128 ? c : '?');
    }
    out[i] = '\0';
}


static void xb_skip_sep(const char **s)
{
    while (**s == '\\' || **s == '/') {
        (*s)++;
    }
}

/* ---------- W190 SAVE-LOCK: Spielstaende schuetzen ----------
 * Aktiv: jeder Host-Pfad <hdd>\TDATA\... bzw. <hdd>\UDATA\... (T:, U: und
 * E:\TDATA, E:\UDATA) wird auf <hdd>\_savelock\TDATA|UDATA\... umgebogen
 * (Wegwerf-Arbeitskopie, sb_savecheck.h). Einmal aktiv, bleibt der Schutz
 * bis zum Prozessende an (g_sl_hdd aendert sich danach nicht mehr). */
static volatile LONG g_sl_active;
static char g_sl_hdd[600];  /* hdd_root mit Backslashes, ohne Endtrenner */
static size_t g_sl_hddn;

static void sl_redirect(char *out, size_t cap)
{
    const char *r;
    char t[1024];
    if (!g_sl_hddn || _strnicmp(out, g_sl_hdd, g_sl_hddn) != 0 || out[g_sl_hddn] != '\\') {
        return;
    }
    r = out + g_sl_hddn + 1;
    if ((_strnicmp(r, "TDATA", 5) != 0 && _strnicmp(r, "UDATA", 5) != 0) ||
        (r[5] != '\\' && r[5] != '\0')) {
        return;
    }
    _snprintf_s(t, sizeof t, _TRUNCATE, "%s\\_savelock\\%s", g_sl_hdd, r);
    strcpy_s(out, cap, t);
}

/* W150: drel (optional) bekommt bei D:-Pfaden den Pfad relativ zur
 * Laufwerkswurzel, sonst "". Im Abbild-Modus ist der "Host-Pfad" nur ein
 * Anzeigename (<game_image>\<rel>, gleiche Dateinamen in den Logs). */
static int xb_path_to_host_ex(const char *xb, size_t nbytes, char *out, size_t cap,
                              char *drel, size_t drelcap, int *is_d)
{
    char tmp[1024];
    char base[640];
    const char *rel;
    size_t n;
    int d = 0;

    /* Xbox-Pfade sind ANSI (PSTRING), nicht UTF-16 */
    n = nbytes < sizeof tmp - 1 ? nbytes : sizeof tmp - 1;
    memcpy(tmp, xb, n);
    tmp[n] = '\0';

    /* T:/U: (Titel-Drives, beschreibbar) -> HDD/TDATA|UDATA/<titleid> */
    if (_strnicmp(tmp, "\\??\\T:", 6) == 0 ||
        ((tmp[0] == 'T' || tmp[0] == 't') && tmp[1] == ':')) {
        rel = tmp[1] == '?' ? tmp + 6 : tmp + 2;
        _snprintf_s(base, sizeof base, _TRUNCATE, "%s/TDATA/%s", g_cfg.hdd_root,
                    XB_TITLE_ID_STR);
    } else if (_strnicmp(tmp, "\\??\\U:", 6) == 0 ||
               ((tmp[0] == 'U' || tmp[0] == 'u') && tmp[1] == ':')) {
        rel = tmp[1] == '?' ? tmp + 6 : tmp + 2;
        _snprintf_s(base, sizeof base, _TRUNCATE, "%s/UDATA/%s", g_cfg.hdd_root,
                    XB_TITLE_ID_STR);
    } else if (_strnicmp(tmp, "\\??\\Z:", 6) == 0 ||
               ((tmp[0] == 'Z' || tmp[0] == 'z') && tmp[1] == ':')) {
        /* Z: (Utility/Cache, beschreibbar) -> hdd_root/ZCACHE */
        rel = tmp[1] == '?' ? tmp + 6 : tmp + 2;
        if (g_zc_alt[0]) { /* W423: Ersatzordner (LoC, ZCACHE nicht beschreibbar) */
            strcpy_s(base, sizeof base, g_zc_alt);
        } else {
            _snprintf_s(base, sizeof base, _TRUNCATE, "%s/ZCACHE", g_cfg.hdd_root);
        }
    } else if (_strnicmp(tmp, "\\Device\\Harddisk0\\Partition",
                         sizeof "\\Device\\Harddisk0\\Partition" - 1) == 0 ||
               _strnicmp(tmp, "Device\\Harddisk0\\partition",
                         sizeof "Device\\Harddisk0\\partition" - 1) == 0) {
        /* E: (Festplatte, beschreibbar) -> hdd_root (NICHT media_root!).
         * Alle Partitionen (0=Raw,1=E:,2=C:,...) -> hdd_root (M1). */
        if (tmp[0] == '\\') {
            rel = tmp + sizeof "\\Device\\Harddisk0\\Partition1" - 1;
        } else {
            rel = tmp + sizeof "Device\\Harddisk0\\partition0" - 1;
        }
        strcpy_s(base, sizeof base, g_cfg.hdd_root);
    } else {
        static const char *dpre[] = {
            "\\Device\\CdRom0", "\\??\\D:", "D:", NULL
        };
        const char **p;
        rel = tmp;
        for (p = dpre; *p; p++) {
            size_t pn = strlen(*p);
            if (_strnicmp(tmp, *p, pn) == 0) {
                rel = tmp + pn;
                break;
            }
        }
        strcpy_s(base, sizeof base, g_iso ? g_cfg.game_image : g_cfg.media_root);
        d = 1;
    }
    xb_skip_sep(&rel);
    if (drel && drelcap) {
        strncpy_s(drel, drelcap, d ? rel : "", _TRUNCATE);
    }
    if (is_d) {
        *is_d = d;
    }
    {
        int m = _snprintf_s(out, cap, _TRUNCATE, "%s/%s", base, rel);
        char *s;
        for (s = out; *s; s++) {
            if (*s == '/') {
                *s = '\\';
            }
        }
        if (g_sl_active && !d) { /* W190 SAVE-LOCK: TDATA/UDATA -> Arbeitskopie */
            sl_redirect(out, cap);
        }
        return m > 0 ? 0 : -1;
    }
}

static int xb_path_to_host(const char *xb, size_t nbytes, char *out, size_t cap)
{
    return xb_path_to_host_ex(xb, nbytes, out, cap, NULL, 0, NULL);
}

/* W254 REPLAY-LAUNCHER: Xbox-Pfad (z. B. "U:\<id>\st00_0.rep") wie das Spiel
 * aufloesen (Spielstand-Schutz, US-Ordner). 0 = ok. */
int krnl_host_path(const char *xb, char *out, size_t cap)
{
    return xb ? xb_path_to_host(xb, strlen(xb), out, cap) : -1;
}

/* W272 ALPHA-TEST: D:-Datei (rel zur Laufwerkswurzel, z. B. "Media\BIN\TEX05.BIN")
 * ganz lesen - aus dem Abbild (game_image) oder dem Ordner (media_root). Nur
 * lesend; *buf per malloc (Aufrufer gibt frei). 0 = ok. */
int krnl_read_d(const char *rel, unsigned char **buf, unsigned *len)
{
    char host[1024];
    HANDLE h;
    DWORD sz, got = 0;
    unsigned char *b;
    if (!rel || !buf || !len) return -1;
    *buf = NULL;
    *len = 0;
    if (g_iso) {
        uint8_t *p = NULL;
        uint32_t n = 0;
        if (iso_vfs_read_all(g_iso, rel, &p, &n) != 0) return -1;
        *buf = p;
        *len = n;
        return 0;
    }
    _snprintf_s(host, sizeof host, _TRUNCATE, "%s\\%s", g_cfg.media_root, rel);
    h = CreateFileA(host, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz > 64u * 1024u * 1024u) { CloseHandle(h); return -1; }
    b = (unsigned char *)malloc(sz ? sz : 1);
    if (!b) { CloseHandle(h); return -1; }
    if (!ReadFile(h, b, sz, &got, NULL) || got != sz) { free(b); CloseHandle(h); return -1; }
    CloseHandle(h);
    *buf = b;
    *len = sz;
    return 0;
}

/* W150: D:-Datei aus dem Abbild oeffnen. Log-Zeilen wie im Ordner-Modus
 * (FILE open '<pfad>' -> xh / -> FEHLER <win32>), damit Meilensteine und
 * logdiff gleich bleiben. */
static u32 winerr_to_status(DWORD e);

static u32 vfs_open(const char *host, const char *drel, u32 access, u32 disp,
                    u32 options, u32 *out_xh)
{
    int po = 0;
    int node = iso_vfs_lookup(g_iso, drel, &po);
    int create = disp == X_FILE_CREATE || disp == X_FILE_OVERWRITE ||
                 disp == X_FILE_OVERWRITE_IF || disp == X_FILE_SUPERSEDE ||
                 (disp == X_FILE_OPEN_IF && node < 0);
    extern void replay_note_file(const char *path);
    u32 xh;
    ht_entry_t *en;
    if ((access & VFS_WRITE_MASK) || create) {
        static unsigned nden;
        if (nden < 32) {
            nden++;
            HLOG_W("ISO: Schreibzugriff auf D: verweigert '%s' (access=0x%X disp=%u) - Abbild ist nur lesbar",
                   host, access, disp);
        }
        HLOG_I("FILE open '%s' (disp=%u opt=0x%X) -> FEHLER %lu", host, disp,
               options, (unsigned long)ERROR_ACCESS_DENIED);
        replay_note_file(host);
        return X_STATUS_ACCESS_DENIED;
    }
    if (node < 0) {
        DWORD e = po ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND;
        HLOG_I("FILE open '%s' (disp=%u opt=0x%X) -> FEHLER %lu", host, disp,
               options, e);
        replay_note_file(host);
        return winerr_to_status(e);
    }
    xh = ht_alloc(HT_FILE, NULL);
    if (!xh) {
        return X_STATUS_NO_MEMORY;
    }
    en = ht_get(xh);
    strncpy_s(en->path, sizeof en->path, host, _TRUNCATE);
    en->is_dir = (options & X_FILE_DIRECTORY_FILE) != 0; /* wie Ordner-Modus */
    en->vfs_node = node;
    en->vfs_pos = 0;
    en->vfs_dir_k = 0;
    en->vfs = 1;
    *out_xh = xh;
    if (!g_sba_loc || loc_open_ok_log(host)) { /* W423: LoC drosselt gleiche Oeffnungen in Folge */
        HLOG_I("FILE open '%s' -> xh=0x%X", host, xh);
    }
    if (g_cfg.loc_boot_diag && lbd_path(host) && lbd_ok()) {
        iso_vfs_stat_t vs;
        memset(&vs, 0, sizeof vs);
        iso_vfs_stat(g_iso, node, &vs);
        HLOG_I("LOCBOOTDIAG: open D: '%s' access=0x%X disp=%u opt=0x%X -> ok xh=0x%X groesse=%u attr=0x%X", drel, access,
               disp, options, xh, vs.size, vs.attr);
    }
    replay_note_file(host);
    return X_STATUS_SUCCESS;
}

/* mkdir -p (fuer TDATA/UDATA/Savegame-Verzeichnisse) */
void xb_mkdir_p(const char *path)
{
    char tmp[1024];
    char *s;
    strcpy_s(tmp, sizeof tmp, path);
    for (s = tmp + 1; *s; s++) {
        if (*s == '\\' || *s == '/') {
            char c = *s;
            *s = '\0';
            CreateDirectoryA(tmp, NULL);
            *s = c;
        }
    }
    CreateDirectoryA(tmp, NULL);
}

/* W187 KIA-LOAD: T:-Spielstaende vor dem Spielstart pruefen (save_check=1:
 * reparieren mit Sicherung unter <hdd_root>\_backup\, 2: nur melden).
 * Beschaedigte/fremde Staende (z. B. abgebrochenes Schreiben, Import von
 * einer echten Xbox) fuehren sonst zu "Unable to load KIA Record." usw. */
static void save_check_log(const char *line, void *ctx)
{
    (void)ctx;
    HLOG_W("%s", line);
}

void krnl_save_check(void)
{
    wchar_t w[640];
    sb_savecheck_t r;
    int rc;
    if (!MultiByteToWideChar(CP_ACP, 0, g_cfg.hdd_root, -1, w, 640)) {
        return;
    }
    rc = sb_savecheck_run(w, g_cfg.save_check == 1, &r, save_check_log, NULL);
    if (rc < 0) {
        return;
    }
    if (rc == 0) {
        HLOG_I("SAVECHECK: %d Spielstand-Dateien ok (davon %d Replays)", r.checked, r.replays);
    } else {
        char b[700];
        if (!WideCharToMultiByte(CP_ACP, 0, r.backup, -1, b, sizeof b, NULL, NULL)) {
            b[0] = '\0';
        }
        HLOG_W("SAVECHECK: %d Dateien (davon %d Replays), %d ok, %d repariert, %d verschoben, %d unveraendert%s%s",
               r.checked, r.replays, r.ok, r.repaired, r.moved, r.failed, b[0] ? ", Sicherung " : "", b);
    }
}

/* ---------- W190 SAVE-LOCK: Ein-/Ausschalten, Uebernehmen ----------
 * Sitzung = Prozess plus seine XLaunchNewImage-Neustarts (W186): Wer den
 * Schutz einschaltet, setzt SB_SAVELOCK_SESSION=1 in der eigenen Umgebung,
 * das Neustart-Kind erbt sie und behaelt die Arbeitskopie (BACK TO TITLE
 * verwirft also nichts). Ein neuer Start ohne die Variable verwirft die
 * Arbeitskopie (save_lock=0) bzw. legt sie frisch an (save_lock=1). Kein
 * Aufraeumen beim Beenden noetig: die echten Staende werden nie beruehrt. */
static int sl_hdd_w(wchar_t *w, int cap)
{
    return MultiByteToWideChar(CP_ACP, 0, g_cfg.hdd_root, -1, w, cap) > 0;
}

static void sl_set_root(void)
{
    char *s;
    strcpy_s(g_sl_hdd, sizeof g_sl_hdd, g_cfg.hdd_root);
    for (s = g_sl_hdd; *s; s++) {
        if (*s == '/') {
            *s = '\\';
        }
    }
    g_sl_hddn = strlen(g_sl_hdd);
    while (g_sl_hddn > 0 && g_sl_hdd[g_sl_hddn - 1] == '\\') {
        g_sl_hdd[--g_sl_hddn] = '\0';
    }
}

int krnl_savelock_active(void)
{
    return g_sl_active != 0;
}

int krnl_savelock_begin(const char *why)
{
    wchar_t w[640];
    char err[300];
    int n = 0;
    if (g_sl_active) {
        return 0;
    }
    if (!sl_hdd_w(w, 640) || sb_savelock_prepare(w, &n, err, sizeof err) != 0) {
        HLOG_W("SAVELOCK: Arbeitskopie nicht anlegbar (%s) - Schutz AUS, Spielstaende wie ohne save_lock",
               err[0] ? err : "hdd_root");
        return -1;
    }
    sl_set_root();
    InterlockedExchange(&g_sl_active, 1);
    SetEnvironmentVariableA("SB_SAVELOCK_SESSION", "1"); /* Neustart-Kind behaelt die Kopie */
    HLOG_I("SAVELOCK: aktiv (%s) - T:/U: in Arbeitskopie '%s\\_savelock' (%d Dateien kopiert), "
           "Aenderungen an Pilot/Spielstaenden werden verworfen", why, g_sl_hdd, n);
    return 0;
}

void krnl_savelock_init(void)
{
    wchar_t w[640];
    char b[8];
    int keep = GetEnvironmentVariableA("SB_SAVELOCK_SESSION", b, sizeof b) > 0 && b[0] == '1';
    int rc;
    if (!sl_hdd_w(w, 640)) {
        return;
    }
    if (keep && sb_savelock_present(w)) {
        sl_set_root();
        InterlockedExchange(&g_sl_active, 1);
        HLOG_I("SAVELOCK: aktiv (Neustart derselben Sitzung) - Arbeitskopie '%s\\_savelock' bleibt", g_sl_hdd);
        return;
    }
    if (g_cfg.save_lock || keep) {
        krnl_savelock_begin(keep ? "Neustart, Arbeitskopie fehlte" : "Start, save_lock=1");
        return;
    }
    rc = sb_savelock_discard(w); /* save_lock=0: Rest einer frueheren Sitzung */
    if (rc != 0) {
        HLOG_I("SAVELOCK: Arbeitskopie einer frueheren Sitzung %s ('%s\\_savelock')",
               rc > 0 ? "verworfen" : "nicht vollstaendig loeschbar", g_cfg.hdd_root);
    }
}

int krnl_savelock_commit(void)
{
    wchar_t w[640], bk[800];
    char err[300], b8[900], pre[640];
    unsigned i;
    size_t pn;
    int n = 0;
    if (!g_sl_active || !sl_hdd_w(w, 640)) {
        return -1;
    }
    /* Schreibt das Spiel gerade einen Spielstand? (offene Datei in der Kopie) */
    _snprintf_s(pre, sizeof pre, _TRUNCATE, "%s\\_savelock\\", g_sl_hdd);
    pn = strlen(pre);
    for (i = 0; i < XB_HANDLE_MAX; i++) {
        ht_entry_t *e = ht_get(XB_HANDLE_BASE + i);
        if (e && e->kind == HT_FILE && !e->vfs && !e->is_dir && _strnicmp(e->path, pre, pn) == 0) {
            HLOG_W("SAVELOCK: Uebernehmen verschoben - Spielstand offen '%s'", e->path);
            return 1;
        }
    }
    if (sb_savelock_commit(w, bk, 800, &n, err, sizeof err) != 0) {
        HLOG_W("SAVELOCK: Uebernehmen FEHLER (%s)", err);
        return -1;
    }
    if (!WideCharToMultiByte(CP_ACP, 0, bk, -1, b8, sizeof b8, NULL, NULL)) {
        b8[0] = '\0';
    }
    HLOG_I("SAVELOCK: geschuetzte Aenderungen uebernommen (%d Dateien), alter Stand gesichert '%s'", n,
           b8[0] ? b8 : "-");
    return 0;
}

/* ----- Dateien ----- */
static DWORD xb_disposition_to_win(u32 d)
{
    switch (d) {
    case X_FILE_OPEN: return OPEN_EXISTING;
    case X_FILE_CREATE: return CREATE_NEW;
    case X_FILE_OPEN_IF: return OPEN_ALWAYS;
    case X_FILE_OVERWRITE: return TRUNCATE_EXISTING;
    case X_FILE_OVERWRITE_IF:
    case X_FILE_SUPERSEDE: return CREATE_ALWAYS;
    default: return OPEN_EXISTING;
    }
}

static u32 winerr_to_status(DWORD e)
{
    switch (e) {
    case ERROR_SUCCESS: return X_STATUS_SUCCESS;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: return X_STATUS_OBJECT_NAME_NOT_FOUND;
    case ERROR_ACCESS_DENIED: return X_STATUS_ACCESS_DENIED;
    case ERROR_ALREADY_EXISTS: return X_STATUS_OBJECT_NAME_COLLISION;
    case ERROR_HANDLE_EOF: return X_STATUS_END_OF_FILE;
    default: return 0xC0000001u; /* UNSUCCESSFUL */
    }
}

/* W275 LOC-BOOT: LoC wiederholt einen fehlenden Disc-Zugriff (z. B.
 * TEXTURE.BIN) ohne Pause -> Logflut. Nur im LoC-Profil: dieselbe
 * fehlgeschlagene Datei direkt hintereinander nur beim 1., 10., 100., ...
 * Mal protokollieren (Zaehler in einer eigenen Zeile). SB unveraendert. */
static int loc_open_fail_log(const char *host)
{
    static char last[512];
    static unsigned long n;
    static LONG lk;
    int show;
    while (InterlockedExchange(&lk, 1)) {
        SwitchToThread();
    }
    if (strcmp(last, host) == 0) {
        n++;
    } else {
        strncpy_s(last, sizeof last, host, _TRUNCATE);
        n = 1;
    }
    show = n == 1 || n == 10 || n == 100 || n == 1000 || n % 10000 == 0;
    if (show && n > 1) {
        HLOG_I("LOCFILE: '%s' fehlt weiter (%lu Versuche in Folge)", host, n);
    }
    InterlockedExchange(&lk, 0);
    return show;
}

/* W433 REPLAY-DELETE: liegt p unterhalb von root (gleiche Schreibweise wie die
 * Host-Pfade, '/' und '\' gleich, ohne Endtrenner)? Die Wurzel selbst zaehlt nicht. */
static int sd_under(const char *root, const char *p)
{
    size_t n = 0, i;
    if (!root || !root[0] || !p) {
        return 0;
    }
    n = strlen(root);
    while (n > 1 && (root[n - 1] == '/' || root[n - 1] == '\\')) {
        n--;
    }
    for (i = 0; i < n; i++) {
        char a = root[i] == '/' ? '\\' : root[i], b = p[i] == '/' ? '\\' : p[i];
        if (!b || tolower((unsigned char)a) != tolower((unsigned char)b)) {
            return 0;
        }
    }
    if (p[n] != '\\' && p[n] != '/') {
        return 0;
    }
    for (i = n; p[i] == '\\' || p[i] == '/'; i++) {
    }
    return p[i] != 0;
}

/* W433: 1 = Pfad enthaelt eine "."- oder ".."-Komponente. */
static int sd_dotseg(const char *p)
{
    const char *s = p;
    while (*s) {
        const char *e = s;
        while (*e && *e != '\\' && *e != '/') {
            e++;
        }
        if ((e - s == 1 && s[0] == '.') || (e - s == 2 && s[0] == '.' && s[1] == '.')) {
            return 1;
        }
        s = *e ? e + 1 : e;
    }
    return 0;
}

/* W433: Loeschen nur im beschreibbaren Spiel-Laufwerk (hdd_root inkl. TDATA/UDATA/
 * ZCACHE/_savelock, bzw. W423-Ersatzordner fuer Z:), nie ausserhalb. */
static int sd_in_drive(const char *p)
{
    if (!p || !p[0] || sd_dotseg(p)) {
        return 0;
    }
    return sd_under(g_cfg.hdd_root, p) || (g_zc_alt[0] && sd_under(g_zc_alt, p));
}

/* W433: ObjectName relativ zu RootDirectory aufloesen (NT-Semantik). Die XAPI im
 * XBE (XDeleteSaveGame -> FUN_00193474/FUN_001932F7) oeffnet die Dateien eines
 * Spielstands per NtOpenFile(RootDirectory = Ordner-Handle, Name = "SaveMeta.xbx").
 * Nur echte Ordner-Handles auf der Festplatte (keine Pseudo-Handles wie
 * 0xFFFFFFFD, keine Abbild-Handles). Name: relativ, ohne Laufwerk, ohne "."/"..".
 * 1 = aufgeloest (out), 0 = kein solcher Fall (alter Weg), -1 = abgelehnt. */
static int sd_rel_path(u32 root, const char *name, size_t nbytes, char *out, size_t cap)
{
    ht_entry_t *re;
    char rel[512];
    size_t n, pn;
    char *s;
    if (!g_cfg.save_delete_fix || !root) {
        return 0;
    }
    re = ht_get(root);
    if (!re || re->kind != HT_FILE || re->vfs || !re->path[0]) {
        return 0;
    }
    n = nbytes < sizeof rel - 1 ? nbytes : sizeof rel - 1;
    if (n == 0 || nbytes >= sizeof rel) {
        return -1;
    }
    memcpy(rel, name, n);
    rel[n] = '\0';
    for (s = rel; *s; s++) {
        if (*s == '/') {
            *s = '\\';
        }
    }
    if (rel[0] == '\\' || strchr(rel, ':') || sd_dotseg(rel)) {
        return -1;
    }
    pn = strlen(re->path);
    while (pn > 0 && (re->path[pn - 1] == '\\' || re->path[pn - 1] == '/')) {
        pn--;
    }
    if (_snprintf_s(out, cap, _TRUNCATE, "%.*s\\%s", (int)pn, re->path, rel) <= 0) {
        return -1;
    }
    return 1;
}

static u32 open_by_attrs(u32 pAttr, u32 access, u32 share, u32 disp, u32 options,
                         u32 *out_xh)
{
    xb_object_attributes *oa;
    xb_ansi_string *as;
    char host[1024];
    DWORD want = 0, flags = FILE_ATTRIBUTE_NORMAL;
    HANDLE h;
    u32 xh;
    unsigned char *stale = NULL; /* W63 FATXSTALE */
    u32 stale_len = 0;

    if (!pAttr) {
        return X_STATUS_OBJECT_NAME_INVALID;
    }
    oa = (xb_object_attributes *)(uintptr_t)pAttr;
    as = oa->object_name;
    if (!as || !as->buffer) {
        return X_STATUS_OBJECT_NAME_INVALID;
    }
    {
        int rr = oa->root_directory ? sd_rel_path(oa->root_directory, as->buffer, as->length, host, sizeof host) : 0;
        if (rr < 0) { /* W433: relativer Name mit Laufwerk/"..": nie ausserhalb des Ordners */
            HLOG_W("FILE open relativ zu 0x%X abgelehnt '%.*s'", oa->root_directory, (int)(as->length < 200 ? as->length : 200),
                   as->buffer);
            return X_STATUS_OBJECT_NAME_INVALID;
        }
        if (rr > 0) {
            goto resolved;
        }
    }
    if (oa->root_directory) {
        static unsigned nrd;
        if (nrd < 4) { nrd++; HLOG_W("NtCreateFile: RootDirectory=0x%X wird ignoriert", oa->root_directory); }
    }
    {
        char drel[1024];
        int is_d = 0;
        if (xb_path_to_host_ex(as->buffer, as->length, host, sizeof host, drel,
                               sizeof drel, &is_d) != 0) {
            return X_STATUS_OBJECT_NAME_INVALID;
        }
        if (is_d && g_iso) { /* W150 ISO-DIRECT */
            return vfs_open(host, drel, access, disp, options, out_xh);
        }
        if (!is_d && g_cfg.replay_autoplay[0]) { /* W254 REPLAY-LAUNCHER: "Last Replay" lesend aufs gewaehlte Replay */
            char alt[1024];
            if (sb_rauto_open_hook(host, access, disp, alt, sizeof alt)) {
                strcpy_s(host, sizeof host, alt);
            }
        }
    }
resolved:
    if (access & 0x80000000u) {
        want |= GENERIC_READ;
    }
    if (access & 0x40000000u) {
        want |= GENERIC_WRITE;
    }
    if (access == 0x02000000u) { /* MAXIMUM_ALLOWED */
        want = GENERIC_READ | GENERIC_WRITE;
    }
    if (want == 0) {
        want = GENERIC_READ;
    }
    if (!(options & X_FILE_DIRECTORY_FILE)) { /* W424 SB-ALLIED-SQUADS: Replay-Vermerk (*.rep, nur SB) */
        extern void sb_ally_rep_open(const char *host, int write);
        sb_ally_rep_open(host, (want & GENERIC_WRITE) != 0);
    }
    if (options & X_FILE_DIRECTORY_FILE) {
        flags |= FILE_FLAG_BACKUP_SEMANTICS;
        /* Verzeichnis-Erstellung (TDATA/<title>, Saves): CreateFile kann
         * keine Dirs anlegen -> mkdir -p, dann oeffnen. */
        if (disp != X_FILE_OPEN && disp != X_FILE_OVERWRITE) {
            xb_mkdir_p(host);
            h = CreateFileA(host, want, share ? share : (DWORD)(FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE),
                            NULL, OPEN_EXISTING, flags, NULL);
            if (h == INVALID_HANDLE_VALUE) {
                DWORD e = GetLastError();
                HLOG_I("FILE mkdir+open '%s' -> FEHLER %lu", host, e);
                if (g_ls_state) ls_write_failed(host, e); /* W434: Spielstand-Ordner nicht anlegbar */
                return winerr_to_status(e);
            }
            goto opened;
        }
    } else {
        /* Geraete-/Laufwerkswurzeln (D:, T:) sind Verzeichnisse: ohne
         * BACKUP_SEMANTICS schlaegt CreateFile fehl (GLE 5). */
        DWORD attrs = GetFileAttributesA(host);
        if (attrs != INVALID_FILE_ATTRIBUTES &&
            (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            flags |= FILE_FLAG_BACKUP_SEMANTICS;
        }
    }
    /* W63 FATXSTALE: FATX (Xbox) fuellt beim Verlaengern einer Datei die
     * Luecke nicht mit Nullen - nach Ueberschreiben (Truncate) + Seek hinter
     * das Dateiende + Write stehen dort die alten Cluster-Daten. Das Spiel
     * verlaesst sich darauf: FUN_000C3A70 haengt die Spielstand-Signatur per
     * CreateFile(CREATE_ALWAYS) + SetFilePointer(0xAE8) + WriteFile(20 B) an
     * die gerade geschriebene Datei an (W63_c6: user01/user99/mslog00/config
     * komplett Nullen). Alten Inhalt vor dem Truncate sichern (<= 4 MB),
     * NtWriteFile fuellt Luecken hinter dem Dateiende daraus. */
    if (g_cfg.fatx_stale_extend && !(options & X_FILE_DIRECTORY_FILE) &&
        (disp == X_FILE_OVERWRITE || disp == X_FILE_OVERWRITE_IF ||
         disp == X_FILE_SUPERSEDE)) {
        HANDLE ho = CreateFileA(host, GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (ho != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz;
            if (GetFileSizeEx(ho, &sz) && sz.QuadPart > 0 && sz.QuadPart <= 0x400000) {
                stale = (unsigned char *)malloc((size_t)sz.QuadPart);
                if (stale) {
                    DWORD got = 0;
                    if (ReadFile(ho, stale, (DWORD)sz.QuadPart, &got, NULL) && got) {
                        stale_len = got;
                    } else {
                        free(stale);
                        stale = NULL;
                    }
                }
            }
            CloseHandle(ho);
        }
    }
    h = CreateFileA(host, want, share ? share : (DWORD)(FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE),
                    NULL, xb_disposition_to_win(disp), flags, NULL);
    if (h == INVALID_HANDLE_VALUE && g_zc_alt[0] && !(want & GENERIC_WRITE) && disp == X_FILE_OPEN) {
        /* W423: Ersatzordner fuer Z: aktiv - fehlt die Datei dort, lesend aus dem alten ZCACHE */
        DWORD e0 = GetLastError();
        char orig[1024];
        if ((e0 == ERROR_FILE_NOT_FOUND || e0 == ERROR_PATH_NOT_FOUND) && zc_orig_path(host, orig, sizeof orig)) {
            h = CreateFileA(orig, want, share ? share : (DWORD)(FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE),
                            NULL, OPEN_EXISTING, flags, NULL);
        }
        if (h == INVALID_HANDLE_VALUE) SetLastError(e0);
    }
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (g_zc_pre[0] && (want & GENERIC_WRITE) && zc_is_z(host)) { /* W423: Z: nicht beschreibbar? */
            zc_write_failed(host, e);
        }
        if (g_ls_state && ((want & GENERIC_WRITE) || disp != X_FILE_OPEN) &&
            !((disp == X_FILE_OPEN || disp == X_FILE_OVERWRITE) && e == ERROR_FILE_NOT_FOUND)) {
            ls_write_failed(host, e); /* W434: Spielstand nicht geschrieben - melden */
        }
        if (g_cfg.loc_boot_diag && lbd_path(host) && lbd_ok()) {
            HLOG_I("LOCBOOTDIAG: open '%s' access=0x%X disp=%u opt=0x%X -> FEHLER Win32 %lu", host, access, disp,
                   options, (unsigned long)e);
        }
        if (stale) { free(stale); stale = NULL; }
        if (!g_sba_loc || loc_open_fail_log(host)) { /* W275: LoC drosselt Wiederholungen */
            HLOG_I("FILE open '%s' (disp=%u opt=0x%X) -> FEHLER %lu", host, disp,
                   options, e);
        }
        {   /* A4: auch fehlgeschlagene Opens sind Spielzustand (mslog00.sav) */
            extern void replay_note_file(const char *path);
            replay_note_file(host);
        }
        return winerr_to_status(e);
    }
opened:
    xh = ht_alloc(HT_FILE, h);
    if (!xh) {
        CloseHandle(h);
        if (stale) { free(stale); }
        return X_STATUS_NO_MEMORY;
    }
    {
        ht_entry_t *en = ht_get(xh);
        strncpy_s(en->path, sizeof en->path, host, _TRUNCATE);
        en->is_dir = (options & X_FILE_DIRECTORY_FILE) != 0;
        en->stale = stale;
        en->stale_len = stale_len;
        if (stale) {
            static unsigned nst;
            if (nst < 32) {
                nst++;
                HLOG_I("FATXSTALE: '%s' disp=%u alter Inhalt %u B gesichert", host, disp, stale_len);
            }
        }
    }
    *out_xh = xh;
    if (!g_sba_loc || loc_open_ok_log(host)) { /* W423: LoC drosselt gleiche Oeffnungen in Folge */
        HLOG_I("FILE open '%s' -> xh=0x%X", host, xh);
    }
    if (g_zc_pre[0] && !g_loc_copy_done && zc_is_z(host)) {
        const char *b = strrchr(host, '\\');
        if (b && _stricmp(b + 1, "COPY_CHK.txt") == 0) { /* W423: letzte Datei der Start-Kopie liegt auf Z: */
            InterlockedExchange(&g_loc_copy_done, 1);
            HLOG_I("LOCBOOT: Start-Kopie nach Z: fertig (COPY_CHK.txt, Z: %s, Schreibfehler %ld)",
                   g_zc_state == 2 ? "Ersatzordner" : "ok", (long)g_zc_wfail);
        }
    }
    if (g_cfg.loc_boot_diag && lbd_path(host) && lbd_ok()) {
        HLOG_I("LOCBOOTDIAG: open '%s' access=0x%X disp=%u opt=0x%X -> ok xh=0x%X", host, access, disp, options, xh);
    }
    {   /* A4: Datei-Ereignis fuer zustandsgesteuertes Replay */
        extern void replay_note_file(const char *path);
        replay_note_file(host);
    }
    return X_STATUS_SUCCESS;
}

u32 __stdcall hle_NtCreateFile(u32 ph, u32 access, u32 attr, u32 iosb,
                                      u32 allocSize, u32 fileAttr, u32 share,
                                      u32 disp, u32 options)
{
    u32 xh = 0, st;
    (void)allocSize;
    (void)fileAttr;
    st = open_by_attrs(attr, access, share, disp, options, &xh);
    *(u32 *)(uintptr_t)ph = xh;
    if (iosb) {
        ((xb_io_status_block *)(uintptr_t)iosb)->status = st;
        ((xb_io_status_block *)(uintptr_t)iosb)->information = 1; /* OPENED */
    }
    return st;
}

u32 __stdcall hle_NtOpenFile(u32 ph, u32 access, u32 attr, u32 iosb, u32 share,
                                    u32 options)
{
    u32 xh = 0, st;
    st = open_by_attrs(attr, access, share, X_FILE_OPEN, options, &xh);
    *(u32 *)(uintptr_t)ph = xh;
    if (iosb) {
        ((xb_io_status_block *)(uintptr_t)iosb)->status = st;
        ((xb_io_status_block *)(uintptr_t)iosb)->information = 1;
    }
    return st;
}

/* W390 LOC-OWN-PREDICT: Dateipositionen, die der spekulative Vorlauf des Spiel-Threads bewegt, danach zuruecksetzen
 * (Spiel-Thread; nur waehrend loc_rb_spec()). */
#define KF_SPEC_MAX 16
static struct { u32 xh; int vfs; uint64_t vpos; LONGLONG hpos; } s_kf_spec[KF_SPEC_MAX];
static int s_kf_spec_n;
static unsigned s_kf_spec_full;
void krnl_file_spec_touch(u32 xh)
{
    ht_entry_t *e = ht_get(xh);
    int k;
    LARGE_INTEGER z, cur;
    if (!e || e->kind != HT_FILE) return;
    for (k = 0; k < s_kf_spec_n; k++)
        if (s_kf_spec[k].xh == xh) return;
    if (s_kf_spec_n >= KF_SPEC_MAX) { s_kf_spec_full++; return; }
    z.QuadPart = 0;
    cur.QuadPart = -1;
    if (!e->vfs) SetFilePointerEx(e->host, z, &cur, FILE_CURRENT);
    s_kf_spec[s_kf_spec_n].xh = xh;
    s_kf_spec[s_kf_spec_n].vfs = e->vfs != 0;
    s_kf_spec[s_kf_spec_n].vpos = e->vfs_pos;
    s_kf_spec[s_kf_spec_n].hpos = cur.QuadPart;
    s_kf_spec_n++;
}
int krnl_file_spec_restore(void)
{
    int k, n = s_kf_spec_n;
    for (k = 0; k < s_kf_spec_n; k++) {
        ht_entry_t *e = ht_get(s_kf_spec[k].xh);
        if (!e || e->kind != HT_FILE) continue;
        if (s_kf_spec[k].vfs) {
            e->vfs_pos = s_kf_spec[k].vpos;
        } else if (s_kf_spec[k].hpos >= 0) {
            LARGE_INTEGER li;
            li.QuadPart = s_kf_spec[k].hpos;
            SetFilePointerEx(e->host, li, NULL, FILE_BEGIN);
        }
        krnl_seek_invalidate(s_kf_spec[k].xh);
    }
    s_kf_spec_n = 0;
    return n;
}

u32 __stdcall hle_NtReadFile(u32 xh, u32 ev, u32 apc, u32 apcCtx, u32 iosb,
                                    u32 buf, u32 len, u32 pOff)
{
    ht_entry_t *e = ht_get(xh);
    DWORD got = 0;
    u32 st;
    LONGLONG lio0; /* W449: Lesedauer (nur LoC, sonst 0) */
    if (!e || e->kind != HT_FILE) {
        return X_STATUS_INVALID_HANDLE;
    }
    lio0 = krnl_loc_io_t0();
    LOC_RB_PREWRITE(buf, len); /* W330: Kernel schreibt in den Gast-Puffer (Rollback-Schreibschutz) */
    if (loc_rb_spec()) { /* W390: Vorlauf liest; die Dateiposition stellt krnl_file_spec_restore danach zurueck */
        krnl_file_spec_touch(xh);
        loc_rb_spec_note(1);
    }
    if (ev || apc) {
        static u32 warned;
        if (!warned) {
            HLOG_W("NtReadFile: async (Event/APC) -> synchron ausgefuehrt");
            warned = 1;
        }
        (void)apcCtx;
    }
    if (e->vfs) { /* W150 ISO-DIRECT: positionell aus dem Abbild */
        DWORD er = vfs_read_at(e, pOff ? (const int64_t *)(uintptr_t)pOff : NULL,
                               (void *)(uintptr_t)buf, len, &got);
        if (er) {
            st = winerr_to_status(er);
        } else {
            st = got ? X_STATUS_SUCCESS : X_STATUS_END_OF_FILE;
            if (g_cfg.tex_dump && got) { sb_rem_note_read(buf, got, e->path); } /* DIAG(REM-FRAME,W103) */
        }
        goto read_done;
    }
    if (pOff) {
        LARGE_INTEGER li;
        li.QuadPart = *(int64_t *)(uintptr_t)pOff;
        SetFilePointerEx(e->host, li, NULL, FILE_BEGIN);
    }
    if (!ReadFile(e->host, (void *)(uintptr_t)buf, len, &got, NULL)) {
        DWORD er = GetLastError();
        /* XapiSelectCachePartition liest 512B @0x800 von partition0 (Raw-
         * Platte). Bei uns ist das ein Verzeichnis (nicht lesbar) -> Fake:
         * NULLEN + ERFOLG (Funktion nimmt dann Default-Partition, M1). */
        if (len == 0x200 && pOff &&
            *(int64_t *)(uintptr_t)pOff == 0x800) {
            memset((void *)(uintptr_t)buf, 0, len);
            got = len;
            st = X_STATUS_SUCCESS;
            HLOG_T("NtReadFile: Partitions-Fake (512B @0x800 -> NULLEN)");
        } else {
            st = (er == ERROR_HANDLE_EOF)
                     ? X_STATUS_END_OF_FILE
                     : winerr_to_status(er);
        }
    } else {
        st = X_STATUS_SUCCESS;
        if (got == 0) {
            st = X_STATUS_END_OF_FILE;
        }
        if (g_cfg.tex_dump && got) { sb_rem_note_read(buf, got, e->path); } /* DIAG(REM-FRAME,W103): Textur-Quelle */
    }
read_done:
    krnl_loc_io_note(e->vfs ? 0 : 1, lio0, st == X_STATUS_SUCCESS || st == X_STATUS_END_OF_FILE); /* W449 */
    if (g_cfg.loc_boot_diag && g_sba_loc && !g_loc_copy_done && lbd_path(e->path) && lbd_ok()) { /* DIAG(W423) */
        HLOG_I("LOCBOOTDIAG: read xh=0x%X '%s' len=%u off=%lld -> st=0x%X got=%lu", xh, e->path, len,
               pOff ? (long long)*(int64_t *)(uintptr_t)pOff : -1LL, st, (unsigned long)got);
    }
    if (iosb) {
        ((xb_io_status_block *)(uintptr_t)iosb)->status = st;
        ((xb_io_status_block *)(uintptr_t)iosb)->information = got;
    }
    /* W22: Seek-Cache nachfuehren (pOff-Seek laeuft unbedingt, auch got=0!) */
    if (pOff)
        krnl_seek_note(xh, (uint64_t)*(int64_t *)(uintptr_t)pOff + got);
    else
        krnl_seek_add(xh, got);
    /* M2b: Async-Read (Missions-Laden!) -> Event signalisieren, sonst
     * wartet das Spiel ewig auf I/O-Abschluss. */
    if (ev) {
        ht_entry_t *ee = ht_get(ev);
        if (ee && ee->kind == HT_EVENT && ee->host) {
            SetEvent(ee->host);
        }
    }
    return st;
}

u32 __stdcall hle_NtWriteFile(u32 xh, u32 ev, u32 apc, u32 apcCtx, u32 iosb,
                                     u32 buf, u32 len, u32 pOff)
{
    ht_entry_t *e = ht_get(xh);
    DWORD wrote = 0;
    u32 st;
    (void)ev;
    (void)apc;
    (void)apcCtx;
    if (!e || e->kind != HT_FILE) {
        return X_STATUS_INVALID_HANDLE;
    }
    if (loc_rb_spec()) { /* W390: spekulativer Vorlauf schreibt nichts (keine Spielstaende/Aufzeichnung aus dem Vorlauf) */
        if (iosb) {
            ((xb_io_status_block *)(uintptr_t)iosb)->status = X_STATUS_SUCCESS;
            ((xb_io_status_block *)(uintptr_t)iosb)->information = len;
        }
        loc_rb_spec_note(2);
        return X_STATUS_SUCCESS;
    }
    if (e->vfs) { /* W150: Abbild ist nur lesbar (DVD) */
        HLOG_W("ISO: NtWriteFile auf D: verweigert '%s' (%u B)", e->path, len);
        if (iosb) {
            ((xb_io_status_block *)(uintptr_t)iosb)->status = X_STATUS_ACCESS_DENIED;
            ((xb_io_status_block *)(uintptr_t)iosb)->information = 0;
        }
        return X_STATUS_ACCESS_DENIED;
    }
    if (pOff) {
        LARGE_INTEGER li;
        li.QuadPart = *(int64_t *)(uintptr_t)pOff;
        SetFilePointerEx(e->host, li, NULL, FILE_BEGIN);
    }
    krnl_seek_invalidate(xh); /* W22: Write bewegt den Zeiger -> Cache weg */
    if (e->stale) {
        /* W63 FATXSTALE: Schreiben hinter das Dateiende -> Luecke mit dem
         * alten Inhalt fuellen (FATX nullt neue Cluster nicht). */
        LARGE_INTEGER zero, cur, sz;
        zero.QuadPart = 0;
        if (SetFilePointerEx(e->host, zero, &cur, FILE_CURRENT) &&
            GetFileSizeEx(e->host, &sz) && cur.QuadPart > sz.QuadPart &&
            sz.QuadPart < (LONGLONG)e->stale_len) {
            LONGLONG end = cur.QuadPart < (LONGLONG)e->stale_len ? cur.QuadPart : (LONGLONG)e->stale_len;
            DWORD w2 = 0;
            SetFilePointerEx(e->host, sz, NULL, FILE_BEGIN);
            WriteFile(e->host, e->stale + sz.QuadPart, (DWORD)(end - sz.QuadPart), &w2, NULL);
            SetFilePointerEx(e->host, cur, NULL, FILE_BEGIN);
            HLOG_I("FATXSTALE: '%s' Luecke %lld..%lld aus altem Inhalt gefuellt (%lu B)",
                   e->path, sz.QuadPart, end, w2);
        }
    }
    if (strstr(e->path, "TDATA") || strstr(e->path, "UDATA")) {
        /* W63: Spielstand-Schreibzugriffe belegen (Pfad, Offset, Laenge,
         * Anteil Nicht-Null-Bytes), gedeckelt. */
        static unsigned nsw;
        if (nsw < 64) {
            LARGE_INTEGER zero, cur;
            u32 k, nz = 0;
            zero.QuadPart = 0;
            cur.QuadPart = -1;
            SetFilePointerEx(e->host, zero, &cur, FILE_CURRENT);
            __try {
                for (k = 0; k < len; k++) {
                    if (((const unsigned char *)(uintptr_t)buf)[k]) nz++;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            nsw++;
            HLOG_I("FILE write '%s' off=%lld len=%u nichtnull=%u", e->path, cur.QuadPart, len, nz);
        }
    }
    if (!WriteFile(e->host, (void *)(uintptr_t)buf, len, &wrote, NULL)) {
        st = winerr_to_status(GetLastError());
    } else {
        st = X_STATUS_SUCCESS;
    }
    if (iosb) {
        ((xb_io_status_block *)(uintptr_t)iosb)->status = st;
        ((xb_io_status_block *)(uintptr_t)iosb)->information = wrote;
    }
    return st;
}

/* W433: gleicher Host-Pfad (Gross/Klein und Endtrenner egal). */
static int sd_same(const char *a, const char *b)
{
    size_t na = strlen(a), nb = strlen(b), i;
    while (na > 0 && (a[na - 1] == '\\' || a[na - 1] == '/')) {
        na--;
    }
    while (nb > 0 && (b[nb - 1] == '\\' || b[nb - 1] == '/')) {
        nb--;
    }
    if (na != nb) {
        return 0;
    }
    for (i = 0; i < na; i++) {
        char x = a[i] == '/' ? '\\' : a[i], y = b[i] == '/' ? '\\' : b[i];
        if (tolower((unsigned char)x) != tolower((unsigned char)y)) {
            return 0;
        }
    }
    return 1;
}

/* W433 REPLAY-DELETE: Loeschen beim Schliessen (FileDispositionInformation TRUE).
 * Ordner per RemoveDirectoryA - DeleteFileA loescht keine Ordner, der
 * Spielstand-Ordner eines geloeschten Replays blieb deshalb stehen. Haelt ein
 * anderer Handle denselben Pfad noch offen (Windows: Freigabe-Verletzung), wird
 * das Loeschen wie unter NT bis zum Schliessen des letzten Handles vertagt.
 * Nur innerhalb des Spiel-Laufwerks (sd_in_drive). */
static void krnl_delete_on_close(u32 xh, const ht_entry_t *e)
{
    DWORD a, err = 0;
    int dir, ok;
    unsigned i;
    if (!sd_in_drive(e->path)) {
        HLOG_W("FILE delete-close '%s' abgelehnt (ausserhalb des Spiel-Laufwerks)", e->path);
        return;
    }
    a = GetFileAttributesA(e->path);
    dir = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    ok = dir ? RemoveDirectoryA(e->path) : DeleteFileA(e->path);
    if (!ok) {
        err = GetLastError();
    }
    if (!ok && (err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED)) {
        for (i = 0; i < XB_HANDLE_MAX; i++) {
            u32 oh = XB_HANDLE_BASE + i;
            ht_entry_t *o = oh == xh ? NULL : ht_get(oh);
            if (o && o->kind == HT_FILE && !o->vfs && o->path[0] && sd_same(o->path, e->path)) {
                o->delete_on_close = 1;
                HLOG_I("FILE delete-close '%s' (%s) vertagt: Handle 0x%X noch offen (Win32 %lu)", e->path,
                       dir ? "Ordner" : "Datei", oh, (unsigned long)err);
                return;
            }
        }
    }
    HLOG_I("FILE delete-close '%s' (%s) -> %s %lu", e->path, dir ? "Ordner" : "Datei", ok ? "ok" : "FEHLER",
           (unsigned long)err);
}

u32 __stdcall hle_NtClose(u32 xh)
{
    ht_entry_t *e = ht_get(xh);
    if (e && loc_rb_spec()) { /* W390: die echte Zeitlinie haelt den Handle noch */
        loc_rb_spec_note(3);
        return X_STATUS_SUCCESS;
    }
    if (!e) {
        HLOG_T("NtClose(0x%X) -> INVALID_HANDLE", xh);
        return X_STATUS_INVALID_HANDLE;
    }
    HLOG_T("NtClose(0x%X kind=%d '%s')", xh, e->kind, e->path);
    if (e->kind == HT_FILE) {
        host_dir_close(e);
        if (!e->vfs) { /* W150: Abbild-Handles haben kein Host-Handle */
            extern void sb_ally_rep_close(const char *host);
            CloseHandle(e->host);
            sb_ally_rep_close(e->path); /* W424: Aufzeichnung geschrieben -> Vermerk */
        }
        if (e->stale) { /* W63 FATXSTALE */
            free(e->stale);
            e->stale = NULL;
            e->stale_len = 0;
        }
        if (e->delete_on_close && e->path[0] && !e->vfs && g_cfg.save_delete_fix) {
            krnl_delete_on_close(xh, e);
        } else if (e->delete_on_close && e->path[0]) {
            DeleteFileA(e->path);
        }
    } else if (e->host && e->host != GetCurrentThread() &&
               e->host != GetCurrentProcess()) {
        CloseHandle(e->host);
    }
    ht_free(xh);
    return X_STATUS_SUCCESS;
}

u32 __stdcall hle_NtQueryFullAttributesFile(u32 pAttr, u32 pInfo)
{
    xb_object_attributes *oa;
    xb_ansi_string *as;
    char host[1024];
    WIN32_FILE_ATTRIBUTE_DATA ad;
    xb_file_network_open_info *fi;

    if (!pAttr || !pInfo) {
        return X_STATUS_INVALID_PARAMETER;
    }
    oa = (xb_object_attributes *)(uintptr_t)pAttr;
    as = oa->object_name;
    {
        char drel[1024];
        int is_d = 0;
        if (!as || !as->buffer ||
            xb_path_to_host_ex(as->buffer, as->length, host, sizeof host, drel,
                               sizeof drel, &is_d) != 0) {
            return X_STATUS_OBJECT_NAME_INVALID;
        }
        if (is_d && g_iso) { /* W150 ISO-DIRECT */
            iso_vfs_stat_t vs;
            uint32_t tlo, thi;
            int node = iso_vfs_lookup(g_iso, drel, NULL);
            if (node < 0 || iso_vfs_stat(g_iso, node, &vs) != 0) {
                return X_STATUS_OBJECT_NAME_NOT_FOUND;
            }
            iso_vfs_volume_time(g_iso, &tlo, &thi);
            fi = (xb_file_network_open_info *)(uintptr_t)pInfo;
            fi->creation_time_lo = fi->last_access_lo = fi->last_write_lo = fi->change_lo = tlo;
            fi->creation_time_hi = fi->last_access_hi = fi->last_write_hi = fi->change_hi = thi;
            fi->alloc_lo = fi->eof_lo = vs.is_dir ? 0 : vs.size;
            fi->alloc_hi = fi->eof_hi = 0;
            fi->attributes = vs.attr ? vs.attr : FILE_ATTRIBUTE_NORMAL;
            HLOG_T("NtQueryFullAttributesFile '%s' ok (Abbild)", host);
            return X_STATUS_SUCCESS;
        }
    }
    {
        wchar_t w[1024];
        size_t i;
        for (i = 0; i + 1 < 1024 && host[i]; i++) {
            w[i] = (wchar_t)(unsigned char)host[i];
        }
        w[i] = 0;
        if (!GetFileAttributesExW(w, GetFileExInfoStandard, &ad)) {
            return X_STATUS_OBJECT_NAME_NOT_FOUND;
        }
    }
    fi = (xb_file_network_open_info *)(uintptr_t)pInfo;
    fi->creation_time_lo = ad.ftCreationTime.dwLowDateTime;
    fi->creation_time_hi = ad.ftCreationTime.dwHighDateTime;
    fi->last_access_lo = ad.ftLastAccessTime.dwLowDateTime;
    fi->last_access_hi = ad.ftLastAccessTime.dwHighDateTime;
    fi->last_write_lo = ad.ftLastWriteTime.dwLowDateTime;
    fi->last_write_hi = ad.ftLastWriteTime.dwHighDateTime;
    fi->change_lo = ad.ftLastWriteTime.dwLowDateTime;
    fi->change_hi = ad.ftLastWriteTime.dwHighDateTime;
    fi->alloc_lo = ad.nFileSizeLow;
    fi->alloc_hi = ad.nFileSizeHigh;
    fi->eof_lo = ad.nFileSizeLow;
    fi->eof_hi = ad.nFileSizeHigh;
    fi->attributes = ad.dwFileAttributes;
    HLOG_T("NtQueryFullAttributesFile '%s' ok", host);
    return X_STATUS_SUCCESS;
}

/* W150 ISO-DIRECT: NtQueryInformationFile fuer Abbild-Handles. Gleiche
 * Klassen/Groessen/Rueckgaben wie der Host-Pfad unten; Zeiten = Zeitstempel
 * des Volumes, Attribute aus dem Verzeichniseintrag. */
static u32 vfs_query_info(ht_entry_t *e, u32 iosb, u32 info, u32 len, u32 cls)
{
    iso_vfs_stat_t vs;
    uint32_t tlo, thi;
    u32 sz, attr;
    if (iso_vfs_stat(g_iso, e->vfs_node, &vs) != 0) {
        return X_STATUS_INVALID_HANDLE;
    }
    iso_vfs_volume_time(g_iso, &tlo, &thi);
    sz = vs.is_dir ? 0 : vs.size;
    attr = vs.attr ? vs.attr : FILE_ATTRIBUTE_NORMAL;
    switch (cls) {
    case X_FileBasicInformation: {
        u32 *b = (u32 *)(uintptr_t)info;
        if (len < 40) return X_STATUS_INFO_LENGTH_MISMATCH;
        b[0] = b[2] = b[4] = b[6] = tlo;
        b[1] = b[3] = b[5] = b[7] = thi;
        b[8] = attr;
        break;
    }
    case X_FileStandardInformation: {
        u32 *s = (u32 *)(uintptr_t)info;
        if (len < 24) return X_STATUS_INFO_LENGTH_MISMATCH;
        s[0] = sz; s[1] = 0; s[2] = sz; s[3] = 0;
        s[4] = 1; s[5] = 0; s[6] = e->is_dir ? 1 : 0;
        break;
    }
    case X_FilePositionInformation:
        if (len < 8) return X_STATUS_INFO_LENGTH_MISMATCH;
        *(uint64_t *)(uintptr_t)info = e->vfs_pos;
        break;
    case X_FileEndOfFileInformation:
        if (len < 8) return X_STATUS_INFO_LENGTH_MISMATCH;
        *(uint64_t *)(uintptr_t)info = sz;
        break;
    case X_FileNetworkOpenInformation: {
        u32 *n = (u32 *)(uintptr_t)info;
        if (len < 56) return X_STATUS_INFO_LENGTH_MISMATCH;
        n[0] = n[2] = n[4] = n[6] = tlo;
        n[1] = n[3] = n[5] = n[7] = thi;
        n[8] = sz; n[9] = 0; n[10] = sz; n[11] = 0;
        n[12] = attr;
        break;
    }
    default:
        HLOG_W("NtQueryInformationFile: Klasse %u -> NOT_SUPPORTED", cls);
        return X_STATUS_NOT_SUPPORTED;
    }
    if (iosb) {
        ((xb_io_status_block *)(uintptr_t)iosb)->status = X_STATUS_SUCCESS;
        ((xb_io_status_block *)(uintptr_t)iosb)->information = 0;
    }
    return X_STATUS_SUCCESS;
}

u32 __stdcall hle_NtQueryInformationFile(u32 xh, u32 iosb, u32 info, u32 len,
                                                u32 cls)
{
    ht_entry_t *e = ht_get(xh);
    BY_HANDLE_FILE_INFORMATION fi;
    u32 written = 0, st = X_STATUS_SUCCESS;

    if (!e || e->kind != HT_FILE) {
        return X_STATUS_INVALID_HANDLE;
    }
    if (e->vfs) { /* W150 ISO-DIRECT */
        return vfs_query_info(e, iosb, info, len, cls);
    }
    if (!GetFileInformationByHandle(e->host, &fi)) {
        return winerr_to_status(GetLastError());
    }
    switch (cls) {
    case X_FileBasicInformation: { /* 40 B */
        struct {
            u32 ct[2], at[2], wt[2], ch[2];
            u32 attr;
        } *b;
        if (len < 40) {
            return X_STATUS_INFO_LENGTH_MISMATCH;
        }
        b = (void *)(uintptr_t)info;
        b->ct[0] = fi.ftCreationTime.dwLowDateTime;
        b->ct[1] = fi.ftCreationTime.dwHighDateTime;
        b->at[0] = fi.ftLastAccessTime.dwLowDateTime;
        b->at[1] = fi.ftLastAccessTime.dwHighDateTime;
        b->wt[0] = fi.ftLastWriteTime.dwLowDateTime;
        b->wt[1] = fi.ftLastWriteTime.dwHighDateTime;
        b->ch[0] = b->wt[0];
        b->ch[1] = b->wt[1];
        b->attr = fi.dwFileAttributes;
        written = 40;
        break;
    }
    case X_FileStandardInformation: { /* 24 B */
        struct {
            u32 alloc[2], eof[2];
            u32 links, del, dir;
        } *s;
        LARGE_INTEGER sz;
        if (len < 24) {
            return X_STATUS_INFO_LENGTH_MISMATCH;
        }
        s = (void *)(uintptr_t)info;
        GetFileSizeEx(e->host, &sz);
        s->alloc[0] = sz.LowPart;
        s->alloc[1] = (u32)sz.HighPart;
        s->eof[0] = sz.LowPart;
        s->eof[1] = (u32)sz.HighPart;
        s->links = 1;
        s->del = 0;
        s->dir = e->is_dir ? 1 : 0;
        written = 24;
        break;
    }
    case X_FilePositionInformation: {
        LARGE_INTEGER zero, pos;
        if (len < 8) {
            return X_STATUS_INFO_LENGTH_MISMATCH;
        }
        zero.QuadPart = 0;
        SetFilePointerEx(e->host, zero, &pos, FILE_CURRENT);
        *(uint64_t *)(uintptr_t)info = (uint64_t)pos.QuadPart;
        written = 8;
        break;
    }
    case X_FileEndOfFileInformation: {
        LARGE_INTEGER sz;
        if (len < 8) {
            return X_STATUS_INFO_LENGTH_MISMATCH;
        }
        GetFileSizeEx(e->host, &sz);
        *(uint64_t *)(uintptr_t)info = (uint64_t)sz.QuadPart;
        written = 8;
        break;
    }
    case X_FileNetworkOpenInformation: { /* 56 B */
        struct {
            u32 ct[2], at[2], wt[2], ch[2];
            u32 alloc[2], eof[2];
            u32 attr;
        } *n;
        LARGE_INTEGER sz;
        if (len < 56) {
            return X_STATUS_INFO_LENGTH_MISMATCH;
        }
        n = (void *)(uintptr_t)info;
        GetFileSizeEx(e->host, &sz);
        n->ct[0] = fi.ftCreationTime.dwLowDateTime;
        n->ct[1] = fi.ftCreationTime.dwHighDateTime;
        n->at[0] = fi.ftLastAccessTime.dwLowDateTime;
        n->at[1] = fi.ftLastAccessTime.dwHighDateTime;
        n->wt[0] = fi.ftLastWriteTime.dwLowDateTime;
        n->wt[1] = fi.ftLastWriteTime.dwHighDateTime;
        n->ch[0] = n->wt[0];
        n->ch[1] = n->wt[1];
        n->alloc[0] = sz.LowPart;
        n->alloc[1] = (u32)sz.HighPart;
        n->eof[0] = sz.LowPart;
        n->eof[1] = (u32)sz.HighPart;
        n->attr = fi.dwFileAttributes;
        written = 56;
        break;
    }
    default:
        HLOG_W("NtQueryInformationFile: Klasse %u -> NOT_SUPPORTED", cls);
        return X_STATUS_NOT_SUPPORTED;
    }
    if (iosb) {
        /* NT: Query-Klassen melden Information=0 (nur Name/Dir-Queries
         * melden Laengen). Das Spiel prueft exakt darauf (TitleMeta). */
        ((xb_io_status_block *)(uintptr_t)iosb)->status = st;
        ((xb_io_status_block *)(uintptr_t)iosb)->information = 0;
    }
    (void)written;
    return st;
}

static u32 __stdcall hle_NtSetInformationFile(u32 xh, u32 iosb, u32 info, u32 len,
                                              u32 cls)
{
    ht_entry_t *e = ht_get(xh);
    if (!e || e->kind != HT_FILE) {
        return X_STATUS_INVALID_HANDLE;
    }
    if (loc_rb_spec()) { /* W390: Vorlauf darf nur die Position setzen (danach zurueck), nichts aendern */
        loc_rb_spec_note(4);
        if (cls == X_FilePositionInformation) {
            krnl_file_spec_touch(xh);
        } else {
            if (iosb) {
                ((xb_io_status_block *)(uintptr_t)iosb)->status = X_STATUS_SUCCESS;
                ((xb_io_status_block *)(uintptr_t)iosb)->information = 0;
            }
            return X_STATUS_SUCCESS;
        }
    }
    if (e->vfs) { /* W150 ISO-DIRECT: Zeiger setzen ja, Aendern nein */
        u32 st = X_STATUS_SUCCESS;
        if (cls == X_FilePositionInformation) {
            if (len < 8) {
                return X_STATUS_INFO_LENGTH_MISMATCH;
            }
            e->vfs_pos = *(uint64_t *)(uintptr_t)info;
        } else if (cls == X_FileEndOfFileInformation || cls == 13 /* Disposition */ ||
                   cls == 10 /* Rename */ || cls == 19 /* Allocation */) {
            HLOG_W("ISO: NtSetInformationFile(Klasse %u) auf D: verweigert '%s'", cls, e->path);
            st = X_STATUS_ACCESS_DENIED;
        } else {
            HLOG_W("NtSetInformationFile: Klasse %u -> SUCCESS (ignoriert)", cls);
        }
        if (iosb) {
            ((xb_io_status_block *)(uintptr_t)iosb)->status = st;
            ((xb_io_status_block *)(uintptr_t)iosb)->information = 0;
        }
        return st;
    }
    switch (cls) {
    case X_FilePositionInformation: {
        LARGE_INTEGER li;
        if (len < 8) {
            return X_STATUS_INFO_LENGTH_MISMATCH;
        }
        li.QuadPart = *(int64_t *)(uintptr_t)info;
        SetFilePointerEx(e->host, li, NULL, FILE_BEGIN);
        break;
    }
    case X_FileEndOfFileInformation: {
        LARGE_INTEGER li, cur;
        LARGE_INTEGER zero;
        if (len < 8) {
            return X_STATUS_INFO_LENGTH_MISMATCH;
        }
        li.QuadPart = *(int64_t *)(uintptr_t)info;
        zero.QuadPart = 0;
        SetFilePointerEx(e->host, zero, &cur, FILE_CURRENT);
        SetFilePointerEx(e->host, li, NULL, FILE_BEGIN);
        krnl_seek_invalidate(xh); /* W22: Truncate -> Cache weg */
        SetEndOfFile(e->host);
        SetFilePointerEx(e->host, cur, NULL, FILE_BEGIN);
        break;
    }
    case 13: { /* FileDispositionInformation */
        e->delete_on_close = (*(uint8_t *)(uintptr_t)info) ? 1 : 0;
        break;
    }
    default:
        HLOG_W("NtSetInformationFile: Klasse %u -> SUCCESS (ignoriert)", cls);
        break;
    }
    if (iosb) {
        ((xb_io_status_block *)(uintptr_t)iosb)->status = X_STATUS_SUCCESS;
        ((xb_io_status_block *)(uintptr_t)iosb)->information = 0;
    }
    return X_STATUS_SUCCESS;
}

/* W150: Platzhalter * und ? ohne Gross/Klein (Xbox-Masken wie "*.sav"). */
static int vfs_wild(const char *p, const char *s)
{
    while (*p) {
        if (*p == '*') {
            p++;
            if (!*p) return 1;
            for (; *s; s++) {
                if (vfs_wild(p, s)) return 1;
            }
            return 0;
        }
        if (!*s) return 0;
        if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s)) return 0;
        p++;
        s++;
    }
    return *s == 0;
}

/* W150 ISO-DIRECT: Verzeichnis im Abbild auflisten (je Aufruf ein Eintrag,
 * Xbox-FILE_DIRECTORY_INFORMATION mit ANSI-Namen, Klasse 1). Der
 * Host-Verzeichnisse verwenden denselben Xbox-Aufbau (unten). */
static u32 vfs_query_dir(ht_entry_t *e, u32 ev, u32 iosb, u32 buf, u32 len,
                         u32 cls, u32 pMask, u32 restart)
{
    static unsigned nlog;
    u32 st = X_STATUS_NO_MORE_FILES, info_n = 0;
    xb_ansi_string *m = (xb_ansi_string *)(uintptr_t)pMask;
    if (restart & 0xFFu) {
        e->vfs_dir_k = 0;
        e->vfs_mask[0] = 0;
    }
    if (!e->vfs_mask[0]) {
        if (m && m->buffer && m->length && m->length < sizeof e->vfs_mask) {
            memcpy(e->vfs_mask, m->buffer, m->length);
            e->vfs_mask[m->length] = 0;
        } else {
            strcpy_s(e->vfs_mask, sizeof e->vfs_mask, "*");
        }
    }
    if (cls != 1) {
        st = X_STATUS_INVALID_INFO_CLASS;
    } else {
        int c;
        while ((c = iso_vfs_child(g_iso, e->vfs_node, e->vfs_dir_k)) >= 0) {
            const char *nm = iso_vfs_name(g_iso, c);
            u32 nl = (u32)strlen(nm), need = 0x40 + nl;
            iso_vfs_stat_t vs;
            uint32_t tlo, thi;
            u32 *o = (u32 *)(uintptr_t)buf;
            if (!vfs_wild(e->vfs_mask, nm)) {
                e->vfs_dir_k++;
                continue;
            }
            if (!buf || len < need) {
                st = 0xC0000023u; /* BUFFER_TOO_SMALL, Eintrag bleibt */
                break;
            }
            iso_vfs_stat(g_iso, c, &vs);
            iso_vfs_volume_time(g_iso, &tlo, &thi);
            memset(o, 0, 0x40);
            o[1] = e->vfs_dir_k;                         /* FileIndex */
            o[2] = o[4] = o[6] = o[8] = tlo;             /* Zeiten */
            o[3] = o[5] = o[7] = o[9] = thi;
            o[10] = o[12] = vs.is_dir ? 0 : vs.size;     /* EOF, Allocation */
            o[14] = vs.attr ? vs.attr : FILE_ATTRIBUTE_NORMAL;
            o[15] = nl;
            memcpy((char *)(uintptr_t)buf + 0x40, nm, nl);
            info_n = need;
            e->vfs_dir_k++;
            st = X_STATUS_SUCCESS;
            break;
        }
    }
    if (nlog < 16) {
        nlog++;
        HLOG_I("ISO: NtQueryDirectoryFile '%s' Maske '%s' k=%u -> 0x%X", e->path,
               e->vfs_mask, e->vfs_dir_k, st);
    }
    if (iosb) {
        ((xb_io_status_block *)(uintptr_t)iosb)->status = st;
        ((xb_io_status_block *)(uintptr_t)iosb)->information = info_n;
    }
    if (ev) {
        ht_entry_t *ee = ht_get(ev);
        if (ee && ee->kind == HT_EVENT && ee->host) {
            SetEvent(ee->host);
        }
    }
    return st;
}

/* W213: XAPI FindFirst/NextFile (and XFindFirst/NextSaveGame inside the
 * original XBE) require ANSI FILE_DIRECTORY_INFORMATION, not Win32's
 * Unicode structure. Return one entry per call, in host enumeration order.
 * The already resolved handle path also covers save_lock's working copy. */
static u32 host_query_dir(ht_entry_t *e, u32 ev, u32 iosb, u32 buf, u32 len,
                         u32 cls, u32 pMask, u32 restart)
{
    u32 st = X_STATUS_NO_MORE_FILES, count = 0;
    host_dir_t *d = NULL;
    xb_ansi_string *m = (xb_ansi_string *)(uintptr_t)pMask;
    if (!e || e->kind != HT_FILE) {
        st = X_STATUS_INVALID_HANDLE;
        goto done;
    }
    if (!e->is_dir) { /* is_dir = mit FILE_DIRECTORY_FILE geoeffnet */
        DWORD a = GetFileAttributesA(e->path);
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            st = 0xC0000103u; /* NOT_A_DIRECTORY */
            goto done;
        }
    }
    if (cls != 1) {
        st = X_STATUS_INVALID_INFO_CLASS;
        goto done;
    }
    d = (host_dir_t *)(uintptr_t)e->dir_state;
    if (!d) {
        d = (host_dir_t *)calloc(1, sizeof *d);
        if (!d) {
            st = 0xC000009Au; /* INSUFFICIENT_RESOURCES */
            goto done;
        }
        d->find = INVALID_HANDLE_VALUE;
        strcpy_s(d->mask, sizeof d->mask, "*");
        if (m && m->length) {
            if (!m->buffer || m->length >= sizeof d->mask) {
                free(d);
                st = 0xC000000Du; /* INVALID_PARAMETER */
                goto done;
            }
            memcpy(d->mask, m->buffer, m->length);
            d->mask[m->length] = 0;
        }
        e->dir_state = (uint64_t)(uintptr_t)d;
        restart = 1;
        HLOG_I("FILE dir '%s' mask='%s'", e->path, d->mask);
    }
    if (restart & 0xFFu) {
        char pattern[sizeof e->path + 3];
        if (d->find != INVALID_HANDLE_VALUE) FindClose(d->find);
        d->index = 0;
        sprintf_s(pattern, sizeof pattern, "%s\\*", e->path);
        d->find = FindFirstFileA(pattern, &d->data);
        d->pending = d->find != INVALID_HANDLE_VALUE;
        if (!d->pending && GetLastError() != ERROR_FILE_NOT_FOUND) {
            st = winerr_to_status(GetLastError());
            goto done;
        }
    }
    while (d->find != INVALID_HANDLE_VALUE) {
        WIN32_FIND_DATAA *f = &d->data;
        u32 nl, *o;
        uint64_t size, alloc;
        if (!d->pending) {
            if (!FindNextFileA(d->find, f)) {
                DWORD err = GetLastError();
                FindClose(d->find);
                d->find = INVALID_HANDLE_VALUE;
                if (err != ERROR_NO_MORE_FILES) st = winerr_to_status(err);
                break;
            }
            d->pending = 1;
        }
        if (!strcmp(f->cFileName, ".") || !strcmp(f->cFileName, "..") ||
            !vfs_wild(d->mask, f->cFileName)) {
            d->pending = 0;
            continue;
        }
        if (g_cfg.replay_autoplay[0] && /* W254 REPLAY-LAUNCHER: nur der gewaehlte Replay-Ordner */
            sb_rauto_dir_skip(e->path, f->cFileName, (f->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)) {
            d->pending = 0;
            continue;
        }
        nl = (u32)strlen(f->cFileName);
        if (!buf || len < 0x40 + nl) {
            st = 0xC0000023u; /* BUFFER_TOO_SMALL; keep pending entry */
            break;
        }
        o = (u32 *)(uintptr_t)buf;
        memset(o, 0, 0x40);
        o[1] = d->index++;
        memcpy(o + 2, &f->ftCreationTime, 8);
        memcpy(o + 4, &f->ftLastAccessTime, 8);
        memcpy(o + 6, &f->ftLastWriteTime, 8);
        memcpy(o + 8, &f->ftLastWriteTime, 8);
        size = (f->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0 :
               ((uint64_t)f->nFileSizeHigh << 32) | f->nFileSizeLow;
        alloc = (size + 0x3FFFu) & ~0x3FFFull; /* FATX clusters */
        memcpy(o + 10, &size, 8);
        memcpy(o + 12, &alloc, 8);
        o[14] = f->dwFileAttributes & 0x37u; /* FATX attributes */
        if (!o[14]) o[14] = FILE_ATTRIBUTE_NORMAL;
        o[15] = nl;
        memcpy((char *)o + 0x40, f->cFileName, nl);
        count = 0x40 + nl;
        d->pending = 0;
        st = X_STATUS_SUCCESS;
        if (g_cfg.directory_selftest) {
            HLOG_I("FILE enum: name='%s' attr=0x%X size=%llu", f->cFileName,
                   o[14], (unsigned long long)size);
        }
        break;
    }
done:
    if (iosb) {
        ((xb_io_status_block *)(uintptr_t)iosb)->status = st;
        ((xb_io_status_block *)(uintptr_t)iosb)->information = count;
    }
    if (ev) {
        ht_entry_t *ee = ht_get(ev);
        if (ee && ee->kind == HT_EVENT && ee->host) SetEvent(ee->host);
    }
    return st;
}

u32 __stdcall hle_NtQueryDirectoryFile(u32 a0, u32 a1, u32 a2, u32 a3, u32 a4,
                                              u32 a5, u32 a6, u32 a7, u32 a8, u32 a9)
{
    if (g_iso) { /* W150 ISO-DIRECT */
        ht_entry_t *e = ht_get(a0);
        if (e && e->kind == HT_FILE && e->vfs) {
            return vfs_query_dir(e, a1, a4, a5, a6, a7, a8, a9);
        }
    }
    (void)a2; (void)a3;
    if (!g_cfg.host_dir_enum) { /* W241: Notschalter = altes Verhalten (M1) */
        HLOG_T("NtQueryDirectoryFile -> NO_MORE_FILES (M1)");
        return X_STATUS_NO_MORE_FILES;
    }
    return host_query_dir(ht_get(a0), a1, a4, a5, a6, a7, a8, a9);
}

u32 __stdcall hle_NtFlushBuffersFile(u32 xh, u32 iosb)
{
    ht_entry_t *e = ht_get(xh);
    if (!e || e->kind != HT_FILE) {
        return X_STATUS_INVALID_HANDLE;
    }
    if (!e->vfs) { /* W150: Abbild-Handles: nichts zu tun */
        FlushFileBuffers(e->host);
    }
    if (iosb) {
        ((xb_io_status_block *)(uintptr_t)iosb)->status = X_STATUS_SUCCESS;
        ((xb_io_status_block *)(uintptr_t)iosb)->information = 0;
    }
    return X_STATUS_SUCCESS;
}

u32 __stdcall hle_NtQueryVolumeInformationFile(u32 xh, u32 iosb, u32 buf, u32 len,
                                                      u32 cls)
{
    ht_entry_t *e = ht_get(xh);
    u32 st = X_STATUS_SUCCESS;
    if (cls == 3 && buf && len >= 24) {
        /* FILE_FS_SIZE_INFORMATION: Xbox meldet FATX-Geometrie (Cluster =
         * 0x4000 Bytes; XapiInitProcess prueft exakt das). Belegung kommt
         * vom Host (64-bit, ohne DWORD-Cap). */
        const char *dir = (e && e->path[0]) ? e->path : g_cfg.media_root;
        ULARGE_INTEGER freeAvail, totalBytes, totalFree;
        ULONGLONG unit = 0x4000u, totalU, freeU;
        u32 spc = 32, bps = 512;
        u32 *o = (u32 *)(uintptr_t)buf;
        if (GetDiskFreeSpaceExA(dir, &freeAvail, &totalBytes, &totalFree)) {
            totalU = totalBytes.QuadPart / unit;
            freeU = freeAvail.QuadPart / unit;
        } else {
            totalU = (8ull * 1024 * 1024 * 1024) / unit; /* 8 GB */
            freeU = totalU / 2;
        }
        if (totalU == 0) {
            totalU = (8ull * 1024 * 1024 * 1024) / unit;
            freeU = totalU / 2;
        }
        o[0] = (u32)(totalU & 0xFFFFFFFFu);
        o[1] = (u32)(totalU >> 32);
        o[2] = (u32)(freeU & 0xFFFFFFFFu);
        o[3] = (u32)(freeU >> 32);
        o[4] = spc;
        o[5] = bps;
        HLOG_T("NtQueryVolumeInformationFile(FsSize) total=%llu frei=%llu (xh=0x%X)",
               totalU, freeU, xh);
    } else {
        HLOG_W("NtQueryVolumeInformationFile(Klasse %u) -> zeros (M1)", cls);
        if (buf && len) {
            memset((void *)(uintptr_t)buf, 0, len < 64 ? len : 64);
        }
    }
    if (iosb) {
        ((xb_io_status_block *)(uintptr_t)iosb)->status = st;
        ((xb_io_status_block *)(uintptr_t)iosb)->information = 0;
    }
    return st;
}

u32 __stdcall hle_NtDeviceIoControlFile(u32 xh, u32 ev, u32 apc, u32 apcCtx,
                                               u32 iosb, u32 code, u32 inBuf,
                                               u32 inLen, u32 outBuf, u32 outLen)
{
    (void)xh; (void)ev; (void)apc; (void)apcCtx; (void)inBuf;
    /* IOCTL_DISK_GET_DRIVE_GEOMETRY (0x70000): 24B DISK_GEOMETRY.
     * Xbox-Platte (8GB, 512B-Sektoren, 63 S/T, 255 T/C, 1044 Zyl). */
    if (code == 0x70000 && outBuf && outLen >= 24) {
        uint8_t *o = (uint8_t *)(uintptr_t)outBuf;
        *(int64_t *)(o + 0) = 1044;   /* Cylinders */
        *(u32 *)(o + 8) = 12;         /* MediaType FixedMedia */
        *(u32 *)(o + 12) = 255;       /* TracksPerCylinder */
        *(u32 *)(o + 16) = 63;        /* SectorsPerTrack */
        *(u32 *)(o + 20) = 512;       /* BytesPerSector */
        if (iosb) {
            ((xb_io_status_block *)(uintptr_t)iosb)->status =
                X_STATUS_SUCCESS;
            ((xb_io_status_block *)(uintptr_t)iosb)->information = 24;
        }
        HLOG_T("NtDeviceIoControlFile(GET_DRIVE_GEOMETRY) -> 8GB/512B");
        return X_STATUS_SUCCESS;
    }
    /* IOCTL_DISK_GET_PARTITION_INFO (0x74004): 32B PARTITION_INFORMATION.
     * Partition 1 (E:/Cache, 4GB, Typ 0x83). */
    if (code == 0x74004 && outBuf && outLen >= 28) {
        uint8_t *o = (uint8_t *)(uintptr_t)outBuf;
        *(int64_t *)(o + 0) = 0;               /* StartingOffset */
        *(int64_t *)(o + 8) = 4294967296LL;    /* PartitionLength 4GB */
        *(u32 *)(o + 16) = 0;                  /* HiddenSectors */
        *(u32 *)(o + 20) = 1;                  /* PartitionNumber */
        o[24] = 0x83;                          /* PartitionType */
        o[25] = 0;                             /* BootIndicator */
        o[26] = 1;                             /* RecognizedPartition */
        o[27] = 0;                             /* RewritePartition */
        if (iosb) {
            ((xb_io_status_block *)(uintptr_t)iosb)->status =
                X_STATUS_SUCCESS;
            ((xb_io_status_block *)(uintptr_t)iosb)->information = 28;
        }
        HLOG_T("NtDeviceIoControlFile(GET_PARTITION_INFO) -> P1/4GB");
        return X_STATUS_SUCCESS;
    }
    HLOG_W("NtDeviceIoControlFile(code=0x%X in=%u out=%u) -> INVALID_DEVICE_REQUEST",
           code, inLen, outLen);
    if (iosb) {
        ((xb_io_status_block *)(uintptr_t)iosb)->status = X_STATUS_INVALID_DEVICE_REQUEST;
        ((xb_io_status_block *)(uintptr_t)iosb)->information = 0;
    }
    return X_STATUS_INVALID_DEVICE_REQUEST;
}

u32 __stdcall hle_NtFsControlFile(u32 a0, u32 a1, u32 a2, u32 a3, u32 iosb,
                                         u32 code, u32 a6, u32 a7, u32 a8, u32 a9)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a6; (void)a7; (void)a8; (void)a9;
    /* FSCTL_DISMOUNT_VOLUME (0x90020): Volume aushaengen (vor Reboot).
     * M1: ERFOLG (nichts zu tun, Host-FS bleibt gemountet). */
    if (code == 0x90020) {
        if (iosb) {
            ((xb_io_status_block *)(uintptr_t)iosb)->status =
                X_STATUS_SUCCESS;
            ((xb_io_status_block *)(uintptr_t)iosb)->information = 0;
        }
        HLOG_T("NtFsControlFile(DISMOUNT_VOLUME) -> SUCCESS");
        return X_STATUS_SUCCESS;
    }
    HLOG_W("NtFsControlFile(code=0x%X) -> INVALID_DEVICE_REQUEST", code);
    if (iosb) {
        ((xb_io_status_block *)(uintptr_t)iosb)->status = X_STATUS_INVALID_DEVICE_REQUEST;
        ((xb_io_status_block *)(uintptr_t)iosb)->information = 0;
    }
    return X_STATUS_INVALID_DEVICE_REQUEST;
}

/* ----- Objekte / Symlinks / Io ----- */
u32 __stdcall hle_IoCreateSymbolicLink(u32 pLink, u32 pTarget)
{
    /* PSTRING = ANSI_STRING auf Xbox */
    xb_ansi_string *l = (xb_ansi_string *)(uintptr_t)pLink;
    xb_ansi_string *t = (xb_ansi_string *)(uintptr_t)pTarget;
    char from[256], to[256];
    unsigned i;
    if (!l || !t || !l->buffer || !t->buffer) {
        return X_STATUS_OBJECT_NAME_INVALID;
    }
    for (i = 0; i < sizeof from - 1 && i < l->length; i++) {
        from[i] = l->buffer[i];
    }
    from[i] = '\0';
    for (i = 0; i < sizeof to - 1 && i < t->length; i++) {
        to[i] = t->buffer[i];
    }
    to[i] = '\0';
    symlink_add(from, to);
    HLOG_I("IoCreateSymbolicLink '%s' -> '%s'", from, to);
    return X_STATUS_SUCCESS;
}

/* Ord 203 (W162): Xbox-Konvention stdcall/2 (ph, pAttr) - einzige Aufrufstelle
 * @0x1923D3 in XLaunchNewImageA pusht 2 (lea/push x2, kein Caller-Cleanup).
 * Das fruehere stdcall/3 las pAttr aus [esp+12] = Ruecksprungadresse und
 * driftete ESP +4. Pfad derzeit tot: XLaunchNewImageA ist jmp-gehookt
 * (xdk_hooks.c patch_jmp -> hook_XLaunchNewImageA, loggt XLAUNCH). */
u32 __stdcall hle_NtOpenSymbolicLinkObject(u32 ph, u32 pAttr)
{
    xb_object_attributes *oa;
    xb_ansi_string *as;
    char name[256];
    size_t n;
    u32 xh;
    if (g_cfg.hle_arity_check) {
        InterlockedIncrement(&g_arity203_calls);
    }
    if (!pAttr) {
        return X_STATUS_OBJECT_NAME_INVALID;
    }
    oa = (xb_object_attributes *)(uintptr_t)pAttr;
    as = oa->object_name;
    if (!as || !as->buffer) {
        return X_STATUS_OBJECT_NAME_INVALID;
    }
    n = as->length < sizeof name - 1 ? as->length : sizeof name - 1;
    memcpy(name, as->buffer, n);
    name[n] = '\0';
    xh = ht_alloc(HT_SYMLINK, NULL);
    strncpy_s(ht_get(xh)->path, sizeof ht_get(xh)->path, name, _TRUNCATE);
    *(u32 *)(uintptr_t)ph = xh;
    return X_STATUS_SUCCESS;
}

u32 __stdcall hle_NtQuerySymbolicLinkObject(u32 xh, u32 pTarget, u32 pRetLen)
{
    ht_entry_t *e = ht_get(xh);
    const char *to;
    xb_unicode_string *t;
    unsigned i, n;
    if (!e || e->kind != HT_SYMLINK) {
        return X_STATUS_INVALID_HANDLE;
    }
    to = symlink_find(e->path);
    if (!to) {
        HLOG_W("NtQuerySymbolicLinkObject '%s': unbekannt", e->path);
        return X_STATUS_OBJECT_NAME_NOT_FOUND;
    }
    t = (xb_unicode_string *)(uintptr_t)pTarget;
    n = (unsigned)strlen(to);
    if (t->max_length < n * 2) {
        return 0xC0000023u; /* BUFFER_TOO_SMALL */
    }
    for (i = 0; i < n; i++) {
        t->buffer[i] = (wchar_t)(unsigned char)to[i];
    }
    t->length = (xb_u16)(n * 2);
    if (pRetLen) {
        *(u32 *)(uintptr_t)pRetLen = n * 2;
    }
    return X_STATUS_SUCCESS;
}

/* W22 FILMAV: Seek-Cache (t2-Spin: 350K redundante Seeks/s auf pausiertem BGM).
 * Nur Offsets < 4 GB (alle Spieldateien): u32 atomar, volatile gegen
 * Compiler-Caching (x86-TSO ordnet); Cross-Thread-gleiches-Handle war schon
 * immer racy (Host-Zeiger geteilt) - der Cache aendert die Klasse nicht. */
int krnl_seek_skip(u32 xh, uint64_t off)
{
    ht_entry_t *e;
    if (!g_cfg.seek_cache || off >= 0x100000000ull)
        return 0;
    e = ht_get(xh);
    if (!e || e->kind != HT_FILE || !e->seek_valid)
        return 0;
    return e->seek_pos == (u32)off;
}

void krnl_seek_note(u32 xh, uint64_t off)
{
    ht_entry_t *e;
    if (!g_cfg.seek_cache)
        return;
    e = ht_get(xh);
    if (!e || e->kind != HT_FILE)
        return;
    if (off >= 0x100000000ull) {
        e->seek_valid = 0;
        return;
    }
    e->seek_pos = (u32)off;
    e->seek_valid = 1;
}

void krnl_seek_add(u32 xh, uint64_t n)
{
    ht_entry_t *e;
    uint64_t np;
    if (!g_cfg.seek_cache || n == 0)
        return;
    e = ht_get(xh);
    if (!e || e->kind != HT_FILE || !e->seek_valid)
        return;
    np = (uint64_t)e->seek_pos + n;
    if (np >= 0x100000000ull) {
        e->seek_valid = 0;
        return;
    }
    e->seek_pos = (u32)np;
}

void krnl_seek_invalidate(u32 xh)
{
    ht_entry_t *e;
    if (!g_cfg.seek_cache)
        return;
    e = ht_get(xh);
    if (e && e->kind == HT_FILE)
        e->seek_valid = 0;
}

/* W22 FILMAV: Zaehler-Wrapper (t2-Seek-Spin: Datei/Ursache am Laufende). */
u32 __stdcall hle_NtSetInformationFile_counted(u32 xh, u32 iosb, u32 info,
                                                      u32 len, u32 cls)
{
    LONG n = InterlockedIncrement(&s_si_n);
    u32 st;
    if (n == 1 || (n % 2000000) == 0) {
        ht_entry_t *e = ht_get(xh);
        s_si_xh = xh;
        s_si_cls = cls;
        s_si_caller = (u32)_ReturnAddress();
        s_si_tid = GetCurrentThreadId(); /* W32 */
        s_si_off = (cls == X_FilePositionInformation && len >= 8)
                       ? *(uint64_t *)(uintptr_t)info
                       : 0;
        if (e && e->kind == HT_FILE) {
            strcpy_s(s_si_path, sizeof s_si_path, e->path);
        } else {
            s_si_path[0] = 0;
        }
    }
    if (cls == X_FilePositionInformation && len >= 8 &&
        krnl_seek_skip(xh, *(uint64_t *)(uintptr_t)info)) {
        InterlockedIncrement(&s_si_skip);
        w32_seek_note_skip(); /* W32: nur Zaehler, kein Verhalten */
        /* W22: redundanter Seek = kein Fortschritt (gleicher Offset, keine
         * Bytes dazwischen). Nur billiger machen laesst den Poll schneller
         * spinnen (c1: 10x Calls); throttlen gibt Main den Kern zurueck. */
        Sleep(1);
        if (iosb) {
            ((xb_io_status_block *)(uintptr_t)iosb)->status = X_STATUS_SUCCESS;
            ((xb_io_status_block *)(uintptr_t)iosb)->information = 0;
        }
        return X_STATUS_SUCCESS;
    }
    if (cls == 13 && info && *(uint8_t *)(uintptr_t)info) {
        /* W187: Loeschen per FileDispositionInformation(TRUE) protokollieren
         * (nur Log, Verhalten unveraendert). Kommt der Aufruf aus XAPI
         * DeleteFileA (FUN_0018E47D, Ruecksprung 0x18E4EA), steht dessen
         * Aufrufer 0x40 Byte ueber dem Ruecksprung (5 Args, esi, 0x20 Lokale,
         * ebp). Spielstand-Loescher: 0xCA17A (Missionsende, Pilot KIA/
         * degradiert), 0xD67AD (Degradierung -> sasen00.sav), 0x10A325/
         * 0x10A350 (Speichern gescheitert). */
        ht_entry_t *e = ht_get(xh);
        u32 *ra = (u32 *)_AddressOfReturnAddress();
        u32 caller = ra[0] == SBA(0x18E4EAu) ? ra[0x40 / 4] : 0;
        HLOG_I("FILE delete '%s' (Disposition, Spiel-Ret=0x%X, DeleteFileA-Aufrufer=0x%X)",
               e && e->kind == HT_FILE ? e->path : "?", ra[0], caller);
    }
    st = hle_NtSetInformationFile(xh, iosb, info, len, cls);
    if (st == X_STATUS_SUCCESS && cls == X_FilePositionInformation && len >= 8)
        krnl_seek_note(xh, *(uint64_t *)(uintptr_t)info);
    return st;
}

/* ---------- W150 ISO-DIRECT: Selbsttest (iso_selftest=1, vor dem Spiel) ----------
 * Ruft die HLE-Funktionen wie das Spiel (Xbox-Strukturen, D:-Pfade) und
 * prueft: Lesen (XBEH, Groesse, Dateizeiger, EOF), Gross/Klein, fehlende
 * Datei, Verzeichnisliste, und dass jeder Schreib-/Erzeugungsversuch auf D:
 * ACCESS_DENIED liefert. Aendert nichts (Abbild nur lesend geoeffnet). */
static u32 st_open(const char *p, u32 access, u32 disp, u32 opt, u32 *xh)
{
    xb_ansi_string as;
    xb_object_attributes oa;
    xb_io_status_block io;
    as.length = (xb_u16)strlen(p);
    as.max_length = as.length;
    as.buffer = (char *)p;
    oa.root_directory = 0;
    oa.object_name = &as;
    oa.attributes = 0;
    *xh = 0;
    return hle_NtCreateFile((u32)(uintptr_t)xh, access, (u32)(uintptr_t)&oa,
                            (u32)(uintptr_t)&io, 0, 0, 1, disp, opt);
}

/* W213: isolated synthetic fixture, never touches real save files. */
int krnl_directory_selftest(void)
{
    char path[700], file[720];
    HANDLE h, event;
    u32 xh = 0, other = 0, eh, st, n = 0, fail = 0;
    u32 out[100], first[100];
    DWORD wrote;
    xb_io_status_block io;
    xb_ansi_string mask = { 5, 5, "*.REP" };
    FILETIME stamp = { 0x12345678, 0x01DA0000 };
    ht_entry_t *e;
    sprintf_s(path, sizeof path, "%s\\W213_enum_%lu", g_cfg.hdd_root,
              GetCurrentProcessId());
    if (!CreateDirectoryA(path, NULL)) return 1;
    sprintf_s(file, sizeof file, "%s\\sample.rep", path);
    h = CreateFileA(file, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { RemoveDirectoryA(path); return 1; }
    WriteFile(h, "REP", 3, &wrote, NULL);
    SetFileTime(h, &stamp, &stamp, &stamp);
    CloseHandle(h);
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    xh = ht_alloc(HT_FILE, h);
    e = ht_get(xh);
    if (!e) { CloseHandle(h); DeleteFileA(file); RemoveDirectoryA(path); return 1; }
    strcpy_s(e->path, sizeof e->path, path);
    e->is_dir = 1;
    event = CreateEventA(NULL, TRUE, FALSE, NULL);
    eh = ht_alloc(HT_EVENT, event);
#define DQ(handle, cap, cl, pat, restart) hle_NtQueryDirectoryFile(handle, eh, 0, 0, (u32)(uintptr_t)&io, (u32)(uintptr_t)out, cap, cl, pat, restart)
#define DC(cond, label) do { n++; if (!(cond)) { fail++; HLOG_E("DIRECTORY-SELFTEST: FAIL %s", label); } } while (0)
    st = DQ(xh, 8, 1, (u32)(uintptr_t)&mask, 0);
    DC(st == 0xC0000023u && io.status == st && io.information == 0, "short buffer");
    DC(WaitForSingleObject(event, 0) == WAIT_OBJECT_0, "event signalled");
    st = DQ(xh, sizeof out, 1, 0, 0);
    DC(st == 0 && io.information == 74 && out[15] == 10 &&
       !memcmp((char *)out + 64, "sample.rep", 10), "ANSI name and retry");
    DC(out[10] == 3 && out[11] == 0 && out[12] == 0x4000 && out[13] == 0,
       "file and FATX allocation size");
    DC(!(out[14] & FILE_ATTRIBUTE_DIRECTORY) && out[2] == stamp.dwLowDateTime &&
       out[6] == stamp.dwLowDateTime && out[7] == stamp.dwHighDateTime, "attributes and times");
    memcpy(first, out, sizeof first);
    DC(DQ(xh, sizeof out, 1, 0, 0) == X_STATUS_NO_MORE_FILES && io.information == 0, "EOF");
    DC(DQ(xh, sizeof out, 1, 0, 0) == X_STATUS_NO_MORE_FILES, "stable EOF");
    DC(DQ(xh, sizeof out, 1, 0, 1) == 0 && !memcmp(first, out, 74), "restart");
    DC(DQ(xh, sizeof out, 2, 0, 0) == X_STATUS_INVALID_INFO_CLASS, "invalid class");
    DC(DQ(0, sizeof out, 1, 0, 0) == X_STATUS_INVALID_HANDLE, "invalid handle");
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    other = ht_alloc(HT_FILE, h);
    e = ht_get(other);
    if (e) {
        strcpy_s(e->path, sizeof e->path, path);
        e->is_dir = 1;
        DC(DQ(other, sizeof out, 1, 0, 0) == 0 && out[15] == 10, "independent handle and no dots");
        hle_NtClose(other);
    } else { CloseHandle(h); DC(0, "second handle"); }
    hle_NtClose(eh);
    hle_NtClose(xh); /* closes the still-active enumeration */
    DeleteFileA(file);
    DC(RemoveDirectoryA(path), "close and cleanup");
    HLOG_I("DIRECTORY-SELFTEST: %s (%u/%u)", fail ? "FAIL" : "OK", n - fail, n);
#undef DQ
#undef DC
    return (int)fail;
}

int krnl_vfs_selftest(void)
{
    static unsigned char buf[0x200];
    xb_io_status_block io;
    u32 xh = 0, st, n = 0, fail = 0;
    int64_t off;
    uint64_t pos = 0;
    if (!g_iso) {
        HLOG_W("ISO-SELFTEST: kein Abbild (game_image leer) - uebersprungen");
        return 0;
    }
#define ST_CHECK(cond, what) do { n++; if (!(cond)) { fail++; HLOG_E("ISO-SELFTEST: FEHLER %s", what); } } while (0)
    /* 1. Lesen */
    st = st_open("\\Device\\CdRom0\\DEFAULT.XBE", 0x80000000u, X_FILE_OPEN, 0x60, &xh);
    ST_CHECK(st == X_STATUS_SUCCESS && xh, "open default.xbe (Gross/Klein)");
    if (st == X_STATUS_SUCCESS) {
        off = 0;
        st = hle_NtReadFile(xh, 0, 0, 0, (u32)(uintptr_t)&io, (u32)(uintptr_t)buf, 4, (u32)(uintptr_t)&off);
        ST_CHECK(st == X_STATUS_SUCCESS && io.information == 4 && memcmp(buf, "XBEH", 4) == 0, "read XBEH");
        st = hle_NtReadFile(xh, 0, 0, 0, (u32)(uintptr_t)&io, (u32)(uintptr_t)buf, 4, 0);
        ST_CHECK(st == X_STATUS_SUCCESS && io.information == 4, "read ohne Offset (Dateizeiger)");
        st = hle_NtQueryInformationFile(xh, (u32)(uintptr_t)&io, (u32)(uintptr_t)&pos, 8, X_FilePositionInformation);
        ST_CHECK(st == X_STATUS_SUCCESS && pos == 8, "Position = 8");
        off = 0x7FFFFFF0;
        st = hle_NtReadFile(xh, 0, 0, 0, (u32)(uintptr_t)&io, (u32)(uintptr_t)buf, 16, (u32)(uintptr_t)&off);
        ST_CHECK(st == X_STATUS_END_OF_FILE && io.information == 0, "read hinter EOF -> END_OF_FILE");
        st = hle_NtWriteFile(xh, 0, 0, 0, (u32)(uintptr_t)&io, (u32)(uintptr_t)buf, 4, 0);
        ST_CHECK(st == X_STATUS_ACCESS_DENIED, "NtWriteFile auf Lese-Handle verweigert");
        pos = 1;
        st = hle_NtSetInformationFile(xh, (u32)(uintptr_t)&io, (u32)(uintptr_t)&pos, 8, X_FileEndOfFileInformation);
        ST_CHECK(st == X_STATUS_ACCESS_DENIED, "EndOfFile setzen verweigert");
        hle_NtClose(xh);
    }
    /* 2. Schreiben/Erzeugen auf D: */
    st = st_open("\\Device\\CdRom0\\default.xbe", 0x40000000u, X_FILE_OPEN, 0x60, &xh);
    ST_CHECK(st == X_STATUS_ACCESS_DENIED && !xh, "GENERIC_WRITE verweigert");
    st = st_open("\\??\\D:\\default.xbe", 0x80000000u, X_FILE_OVERWRITE_IF, 0x60, &xh);
    ST_CHECK(st == X_STATUS_ACCESS_DENIED && !xh, "OVERWRITE_IF verweigert");
    st = st_open("D:\\w150_neu.txt", 0x80000000u | 0x40000000u, X_FILE_CREATE, 0x60, &xh);
    ST_CHECK(st == X_STATUS_ACCESS_DENIED && !xh, "CREATE verweigert");
    st = st_open("\\Device\\CdRom0\\media\\w150_neu", 0x80000000u, X_FILE_CREATE, 0x61, &xh);
    ST_CHECK(st == X_STATUS_ACCESS_DENIED && !xh, "Verzeichnis anlegen verweigert");
    /* 3. Fehlende Datei, Verzeichnis */
    st = st_open("\\Device\\CdRom0\\media\\gibt_es_nicht.bin", 0x80000000u, X_FILE_OPEN, 0x60, &xh);
    ST_CHECK(st == X_STATUS_OBJECT_NAME_NOT_FOUND, "fehlende Datei -> NOT_FOUND");
    st = st_open("\\Device\\CdRom0", 0x80000000u, X_FILE_OPEN, 0x21, &xh);
    ST_CHECK(st == X_STATUS_SUCCESS && xh, "Wurzel als Verzeichnis oeffnen");
    if (st == X_STATUS_SUCCESS) {
        static unsigned char di[0x200];
        xb_ansi_string m;
        unsigned k = 0;
        m.buffer = "*";
        m.length = m.max_length = 1;
        while (hle_NtQueryDirectoryFile(xh, 0, 0, 0, (u32)(uintptr_t)&io, (u32)(uintptr_t)di,
                                        sizeof di, 1, (u32)(uintptr_t)&m, k == 0) == X_STATUS_SUCCESS && k < 64) {
            k++;
        }
        {
            unsigned want = 0;
            while (iso_vfs_child(g_iso, 0, want) >= 0) {
                want++;
            }
            ST_CHECK(k == want && k > 0, "Wurzel auflisten");
        }
        HLOG_I("ISO-SELFTEST: Wurzel hat %u Eintraege", k);
        hle_NtClose(xh);
    }
#undef ST_CHECK
    HLOG_I("ISO-SELFTEST: %s (%u/%u Pruefungen)", fail ? "FEHLER" : "ok", n - fail, n);
    return (int)fail;
}

/* ---------- W190 SAVE-LOCK: Selbsttest (savelock_selftest=1, vor dem Spiel) ----------
 * Spielt den Tod eines Piloten ueber dieselben HLE-Wege wie das Spiel nach:
 * XAPI DeleteFileA (NtOpenFile -> FileDispositionInformation(TRUE) ->
 * NtClose) auf T:\user00.sav, dann T:\death00.sav schreiben, dann die Sicht
 * des Spiels (T:) und die echten Ordner pruefen. save_lock=1: echte Staende
 * unveraendert, die Arbeitskopie zeigt den Tod (bleibt so, Beleg auf der
 * Platte). save_lock=0: Original-Verhalten (echter Pilot geloescht). */
int krnl_savelock_selftest(void)
{
    static const char kMark[16] = "W190 SAVE-LOCK!";
    char ru[700], rd[700];
    xb_io_status_block io;
    u32 xh = 0, st, n = 0, fail = 0;
    int act = g_sl_active != 0, had_d;
    unsigned char del = 1;
    _snprintf_s(ru, sizeof ru, _TRUNCATE, "%s\\TDATA\\%s\\user00.sav", g_cfg.hdd_root, XB_TITLE_ID_STR);
    _snprintf_s(rd, sizeof rd, _TRUNCATE, "%s\\TDATA\\%s\\death00.sav", g_cfg.hdd_root, XB_TITLE_ID_STR);
    if (GetFileAttributesA(ru) == INVALID_FILE_ATTRIBUTES) {
        HLOG_W("SAVELOCK-SELFTEST: kein Pilot '%s' - uebersprungen", ru);
        return 0;
    }
    had_d = GetFileAttributesA(rd) != INVALID_FILE_ATTRIBUTES;
#define ST_CHECK(cond, what) do { n++; if (!(cond)) { fail++; HLOG_E("SAVELOCK-SELFTEST: FEHLER %s", what); } } while (0)
    /* 1. Pilot loeschen wie DeleteFileA */
    st = st_open("T:\\user00.sav", 0x00010000u | 0x00100000u, X_FILE_OPEN, 0x40, &xh);
    ST_CHECK(st == X_STATUS_SUCCESS && xh, "T:\\user00.sav oeffnen (DELETE)");
    if (st == X_STATUS_SUCCESS) {
        hle_NtSetInformationFile_counted(xh, (u32)(uintptr_t)&io, (u32)(uintptr_t)&del, 1, 13);
        hle_NtClose(xh);
    }
    /* 2. KIA-Liste schreiben */
    st = st_open("T:\\death00.sav", 0x80000000u | 0x40000000u, X_FILE_OVERWRITE_IF, 0x40, &xh);
    ST_CHECK(st == X_STATUS_SUCCESS && xh, "T:\\death00.sav anlegen");
    if (st == X_STATUS_SUCCESS) {
        st = hle_NtWriteFile(xh, 0, 0, 0, (u32)(uintptr_t)&io, (u32)(uintptr_t)kMark, sizeof kMark, 0);
        ST_CHECK(st == X_STATUS_SUCCESS && io.information == sizeof kMark, "death00.sav schreiben");
        hle_NtClose(xh);
    }
    /* 3. Sicht des Spiels in der Sitzung: konsistent */
    st = st_open("T:\\user00.sav", 0x80000000u, X_FILE_OPEN, 0x40, &xh);
    ST_CHECK(st == X_STATUS_OBJECT_NAME_NOT_FOUND, "Spiel sieht den Piloten geloescht");
    if (st == X_STATUS_SUCCESS) {
        hle_NtClose(xh);
    }
    st = st_open("T:\\death00.sav", 0x80000000u, X_FILE_OPEN, 0x40, &xh);
    ST_CHECK(st == X_STATUS_SUCCESS, "Spiel sieht death00.sav");
    if (st == X_STATUS_SUCCESS) {
        hle_NtClose(xh);
    }
    /* 4. echte Ordner */
    if (act) {
        ST_CHECK(GetFileAttributesA(ru) != INVALID_FILE_ATTRIBUTES, "echter Pilot unveraendert da");
        ST_CHECK((GetFileAttributesA(rd) != INVALID_FILE_ATTRIBUTES) == had_d, "kein neues echtes death00.sav");
    } else {
        ST_CHECK(GetFileAttributesA(ru) == INVALID_FILE_ATTRIBUTES, "Original: echter Pilot geloescht");
        ST_CHECK(GetFileAttributesA(rd) != INVALID_FILE_ATTRIBUTES, "Original: echtes death00.sav");
    }
#undef ST_CHECK
    HLOG_I("SAVELOCK-SELFTEST: %s (%u/%u Pruefungen, Modus %s)", fail ? "FEHLER" : "ok", n - fail, n,
           act ? "geschuetzt" : "original");
    return (int)fail;
}

/* ---------- W433 REPLAY-DELETE: Selbsttest (save_delete_selftest=1, vor dem Spiel) ----------
 * Spielt XAPI XDeleteSaveGame aus dem XBE nach (FUN_00191CE4 -> FUN_00193474 ->
 * FUN_001932F7, Parameter wie im Original): Ordner U:\<id>\ oeffnen (0x110101,
 * Freigabe 3, 0x4021), Eintraege einzeln auflisten (Klasse 1, 0x146 Byte, ohne
 * Maske), jeden Eintrag RELATIV zum Ordner-Handle oeffnen (0x110100|Ordner,
 * Freigabe 7, 0x4040 bzw. 0x4001), Unterordner rekursiv, Disposition(TRUE),
 * schliessen; zuletzt Disposition auf dem Ordner selbst, schliessen.
 * Nur synthetische Ordner W433_sd_<pid>_* unter dem Lauf-hdd_root. */
static u32 sd_open(u32 root, const char *name, u32 access, u32 share, u32 opt, u32 *xh)
{
    xb_ansi_string as;
    xb_object_attributes oa;
    xb_io_status_block io;
    as.length = (xb_u16)strlen(name);
    as.max_length = as.length;
    as.buffer = (char *)name;
    oa.root_directory = root;
    oa.object_name = &as;
    oa.attributes = 0x40;
    *xh = 0;
    return hle_NtOpenFile((u32)(uintptr_t)xh, access, (u32)(uintptr_t)&oa, (u32)(uintptr_t)&io, share, opt);
}

static u32 sd_delete_tree(u32 dir, int depth)
{
    static unsigned char buf[8][0x146 + 2];
    unsigned char *b = buf[depth & 7];
    xb_io_status_block io;
    unsigned char del = 1;
    u32 st, restart = 1;
    for (;;) {
        char name[0x120];
        u32 nl, attr, ch = 0;
        int isdir;
        st = hle_NtQueryDirectoryFile(dir, 0, 0, 0, (u32)(uintptr_t)&io, (u32)(uintptr_t)b, 0x146, 1, 0, restart);
        restart = 0;
        if (st != X_STATUS_SUCCESS) {
            break;
        }
        nl = *(u32 *)(b + 0x3C);
        attr = *(u32 *)(b + 0x38);
        if (nl == 0 || nl >= sizeof name) {
            return 0xC0000001u;
        }
        memcpy(name, b + 0x40, nl);
        name[nl] = 0;
        isdir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
        st = sd_open(dir, name, 0x110100u | (isdir ? 1u : 0u), 7, isdir ? 0x4001u : 0x4040u, &ch);
        if (st != X_STATUS_SUCCESS) {
            return st; /* wie das Original: Abbruch beim ersten Fehler */
        }
        if (isdir && depth < 7) {
            st = sd_delete_tree(ch, depth + 1);
        } else {
            st = hle_NtSetInformationFile_counted(ch, (u32)(uintptr_t)&io, (u32)(uintptr_t)&del, 1, 13);
        }
        hle_NtClose(ch);
        if ((int)st < 0) {
            return st;
        }
    }
    if (st != X_STATUS_NO_MORE_FILES && st != 0xC000000Fu) {
        return st;
    }
    return hle_NtSetInformationFile_counted(dir, (u32)(uintptr_t)&io, (u32)(uintptr_t)&del, 1, 13);
}

/* Spielstand loeschen wie XDeleteSaveGame (Ordner U:\<id>\). */
static u32 sd_delete_save(const char *id)
{
    char p[300];
    u32 xh = 0, st;
    _snprintf_s(p, sizeof p, _TRUNCATE, "\\??\\U:\\%s\\", id);
    st = sd_open(0, p, 0x110101u, 3, 0x4021u, &xh);
    if (st != X_STATUS_SUCCESS) {
        return st;
    }
    st = sd_delete_tree(xh, 0);
    hle_NtClose(xh);
    return st;
}

/* Wie oft listet U:\ den Ordner id (XFindFirstSaveGame-Sicht)? */
static int sd_listed(const char *id)
{
    static unsigned char b[0x146 + 2];
    xb_io_status_block io;
    u32 xh = 0, restart = 1;
    int hits = 0, k;
    if (sd_open(0, "\\??\\U:\\", 0x100001u, 3, 0x4021u, &xh) != X_STATUS_SUCCESS) {
        return -1;
    }
    for (k = 0; k < 4096; k++) {
        u32 nl;
        if (hle_NtQueryDirectoryFile(xh, 0, 0, 0, (u32)(uintptr_t)&io, (u32)(uintptr_t)b, 0x146, 1, 0, restart) !=
            X_STATUS_SUCCESS) {
            break;
        }
        restart = 0;
        nl = *(u32 *)(b + 0x3C);
        if (nl == strlen(id) && _strnicmp((const char *)b + 0x40, id, nl) == 0) {
            hits++;
        }
    }
    hle_NtClose(xh);
    return hits;
}

static int sd_mkfile(const char *dir, const char *name, unsigned sz)
{
    char p[800];
    HANDLE h;
    DWORD w = 0;
    static unsigned char z[4096];
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\%s", dir, name);
    h = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    while (sz) {
        DWORD c = sz < sizeof z ? sz : (DWORD)sizeof z;
        WriteFile(h, z, c, &w, NULL);
        sz -= c;
    }
    CloseHandle(h);
    return 1;
}

/* Synthetischer Replay-Spielstand: SaveMeta.xbx, SaveImage.xbx, st00_0.rep, Unterordner. */
static int sd_mksave(const char *id, char *host, size_t cap)
{
    char sub[800];
    _snprintf_s(host, cap, _TRUNCATE, "%s\\UDATA\\%s\\%s", g_cfg.hdd_root, XB_TITLE_ID_STR, id);
    xb_mkdir_p(host);
    _snprintf_s(sub, sizeof sub, _TRUNCATE, "%s\\sub", host);
    CreateDirectoryA(sub, NULL);
    return sd_mkfile(host, "SaveMeta.xbx", 48) && sd_mkfile(host, "SaveImage.xbx", 64) &&
           sd_mkfile(host, "st00_0.rep", 4096) && sd_mkfile(sub, "x.bin", 16);
}

/* Aufraeumen nach Fehlern (nur der synthetische Ordner). */
static void sd_rmtree(const char *host)
{
    char p[800];
    WIN32_FIND_DATAA fd;
    HANDLE f;
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\*", host);
    f = FindFirstFileA(p, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\%s", host, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) sd_rmtree(p); else DeleteFileA(p);
        } while (FindNextFileA(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryA(host);
}

int krnl_savedelete_selftest(void)
{
    char id1[64], id2[64], h1[700], h2[700], p[800];
    u32 n = 0, fail = 0, st, xh = 0, other = 0;
    int fix = g_cfg.save_delete_fix;
    unsigned char del = 1;
    xb_io_status_block io;
#define SD_CHECK(cond, what) do { n++; if (!(cond)) { fail++; HLOG_E("SAVEDELETE-SELFTEST: FEHLER %s", what); } } while (0)
    sprintf_s(id1, sizeof id1, "W433_sd_%lu_a", GetCurrentProcessId());
    sprintf_s(id2, sizeof id2, "W433_sd_%lu_b", GetCurrentProcessId());
    if (!sd_mksave(id1, h1, sizeof h1) || !sd_mksave(id2, h2, sizeof h2)) {
        HLOG_E("SAVEDELETE-SELFTEST: Testordner nicht anlegbar (%s)", g_cfg.hdd_root);
        sd_rmtree(h1);
        sd_rmtree(h2);
        return 1;
    }
    /* 1. Replay loeschen entfernt Container und Listeneintrag */
    SD_CHECK(sd_listed(id1) == 1, "vorher gelistet");
    st = sd_delete_save(id1);
    SD_CHECK(st == X_STATUS_SUCCESS, "XDeleteSaveGame-Kette meldet Erfolg");
    SD_CHECK(GetFileAttributesA(h1) == INVALID_FILE_ATTRIBUTES, "Spielstand-Ordner entfernt");
    SD_CHECK(sd_listed(id1) == 0, "nicht mehr gelistet");
    SD_CHECK(sd_listed(id2) == 1, "anderer Spielstand unberuehrt");
    /* 2. Offener Handle (ohne Loesch-Freigabe, wie die Ordner-Oeffnung des Spiels)
     *    blockiert nicht dauerhaft: Loeschen vertagt bis zu dessen Schliessen. */
    _snprintf_s(p, sizeof p, _TRUNCATE, "\\??\\U:\\%s\\", id2);
    st = sd_open(0, p, 0x100001u, 3, 0x4021u, &other);
    SD_CHECK(st == X_STATUS_SUCCESS, "zweiter Ordner-Handle");
    st = sd_delete_save(id2);
    SD_CHECK(st == X_STATUS_SUCCESS, "Loeschen bei offenem Handle meldet Erfolg");
    if (other) {
        hle_NtClose(other);
    }
    SD_CHECK(GetFileAttributesA(h2) == INVALID_FILE_ATTRIBUTES, "nach dem Schliessen entfernt");
    SD_CHECK(sd_listed(id2) == 0, "nach dem Schliessen nicht mehr gelistet");
    /* 3. Pfadpruefung: relative Namen bleiben im Ordner, Loeschen nur im Laufwerk */
    sd_mksave(id1, h1, sizeof h1);
    _snprintf_s(p, sizeof p, _TRUNCATE, "\\??\\U:\\%s\\", id1);
    if (sd_open(0, p, 0x110101u, 3, 0x4021u, &xh) == X_STATUS_SUCCESS) {
        u32 t = 0;
        SD_CHECK(sd_open(xh, "..\\SaveMeta.xbx", 0x110100u, 7, 0x4040u, &t) == X_STATUS_OBJECT_NAME_INVALID && !t,
                 "relativ '..' abgelehnt");
        SD_CHECK(sd_open(xh, "C:\\Windows\\win.ini", 0x80000000u, 7, 0x4040u, &t) == X_STATUS_OBJECT_NAME_INVALID && !t,
                 "relativ mit Laufwerk abgelehnt");
        SD_CHECK(sd_open(xh, "\\SaveMeta.xbx", 0x80000000u, 7, 0x4040u, &t) == X_STATUS_OBJECT_NAME_INVALID && !t,
                 "relativ mit Wurzel abgelehnt");
        st = sd_open(xh, "sub\\x.bin", 0x80000000u, 7, 0x4040u, &t);
        SD_CHECK(st == X_STATUS_SUCCESS && t && ht_get(t), "relativ Unterpfad");
        if (st == X_STATUS_SUCCESS && ht_get(t)) {
            _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\sub\\x.bin", h1);
            SD_CHECK(sd_same(ht_get(t)->path, p), "relativ = Ordner + Name");
            hle_NtClose(t);
        }
        hle_NtClose(xh);
    } else {
        SD_CHECK(0, "Ordner fuer Pfadpruefung");
    }
    SD_CHECK(!sd_in_drive("C:\\Windows\\win.ini"), "ausserhalb des Laufwerks");
    SD_CHECK(!sd_in_drive(g_cfg.hdd_root), "Laufwerkswurzel selbst");
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\UDATA\\..\\..\\x", g_cfg.hdd_root);
    SD_CHECK(!sd_in_drive(p), "'..' im Pfad");
    SD_CHECK(sd_in_drive(h1), "Spielstand im Laufwerk");
    /* 4. Pilot loeschen (DeleteFileA-Weg: absoluter Pfad, Datei) weiter ok */
    _snprintf_s(p, sizeof p, _TRUNCATE, "\\??\\U:\\%s\\st00_0.rep", id1);
    st = sd_open(0, p, 0x00010000u | 0x00100000u, 7, 0x4040u, &xh);
    SD_CHECK(st == X_STATUS_SUCCESS, "Datei absolut oeffnen (DELETE)");
    if (st == X_STATUS_SUCCESS) {
        hle_NtSetInformationFile_counted(xh, (u32)(uintptr_t)&io, (u32)(uintptr_t)&del, 1, 13);
        hle_NtClose(xh);
    }
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\st00_0.rep", h1);
    SD_CHECK(GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES, "Datei geloescht (DeleteFileA-Weg)");
    sd_rmtree(h1);
    sd_rmtree(h2);
#undef SD_CHECK
    HLOG_I("SAVEDELETE-SELFTEST: %s (%u/%u Pruefungen, save_delete_fix=%d)", fail ? "FEHLER" : "ok", n - fail, n, fix);
    return (int)fail;
}

/* ---------- W434 LOC-PILOT-LOST: Selbsttest (loc_save_selftest=1, vor dem Spiel) ----------
 * Synthetische Ordner <hdd_root>\W434_ls_<pid>\..., nie der echte Datenordner. "Nicht
 * beschreibbar" per Verweigern-Eintrag (Jeder: Daten schreiben/anhaengen/Attribute) wie beim
 * Tester vermutet (Rechte, Ueberwachter Ordnerzugriff), bzw. Datei an Stelle eines Ordners. */
static int ls_st_acl(const char *dir, int deny)
{
    wchar_t w[700];
    PSECURITY_DESCRIPTOR sd = NULL;
    PACL dacl = NULL;
    BOOL present = FALSE, def = FALSE;
    DWORD rc;
    if (!MultiByteToWideChar(CP_ACP, 0, dir, -1, w, 700)) return 0;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            deny ? L"D:(D;OICI;0x116;;;WD)(A;OICI;FA;;;WD)" : L"D:(A;OICI;FA;;;WD)", SDDL_REVISION_1, &sd, NULL)) {
        return 0;
    }
    GetSecurityDescriptorDacl(sd, &present, &dacl, &def);
    rc = SetNamedSecurityInfoW(w, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION, NULL,
                               NULL, dacl, NULL);
    LocalFree(sd);
    return rc == ERROR_SUCCESS;
}

static int ls_st_put(const char *p, const char *txt)
{
    FILE *f;
    char d[800], *s;
    strcpy_s(d, sizeof d, p);
    s = strrchr(d, '\\');
    if (s) {
        *s = 0;
        xb_mkdir_p(d);
    }
    if (fopen_s(&f, p, "wb") != 0 || !f) return 0;
    fputs(txt, f);
    fclose(f);
    return 1;
}

static int ls_st_is(const char *p, const char *txt)
{
    FILE *f;
    char b[64];
    size_t n;
    if (fopen_s(&f, p, "rb") != 0 || !f) return 0;
    n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    return strcmp(b, txt) == 0;
}

int krnl_loc_save_selftest(void)
{
    char base[640], o1[700], a1[700], o2[700], a2[700], o4[700], a4[700], f4[700], out[700], p[800], q[800], la[700];
    u32 n = 0, fail = 0;
    DWORD e, ae;
    int st, mig, sticky;
#define LS_CHECK(cond, what) do { n++; if (!(cond)) { fail++; HLOG_E("LOCSAVE-SELFTEST: FEHLER %s", what); } } while (0)
    _snprintf_s(base, sizeof base, _TRUNCATE, "%s\\W434_ls_%lu", g_cfg.hdd_root, GetCurrentProcessId());
    ls_norm(base);
    xb_mkdir_p(base);
    if (!(GetFileAttributesA(base) & FILE_ATTRIBUTE_DIRECTORY) || GetFileAttributesA(base) == INVALID_FILE_ATTRIBUTES) {
        HLOG_E("LOCSAVE-SELFTEST: Testordner unter hdd_root nicht anlegbar - Selbsttest braucht einen beschreibbaren hdd_root");
        return 1;
    }
    _snprintf_s(o1, sizeof o1, _TRUNCATE, "%s\\ok_LOC", base);
    _snprintf_s(a1, sizeof a1, _TRUNCATE, "%s\\alt1", base);
    _snprintf_s(o2, sizeof o2, _TRUNCATE, "%s\\ro_LOC", base);
    _snprintf_s(a2, sizeof a2, _TRUNCATE, "%s\\alt2", base);
    _snprintf_s(f4, sizeof f4, _TRUNCATE, "%s\\file4", base);
    _snprintf_s(o4, sizeof o4, _TRUNCATE, "%s\\file4\\x_LOC", base);
    _snprintf_s(a4, sizeof a4, _TRUNCATE, "%s\\file4\\alt", base);
    xb_mkdir_p(a1);
    xb_mkdir_p(a2);

    /* 1. beschreibbarer Ordner: unveraendert, Ersatzordner unberuehrt */
    st = ls_resolve(o1, a1, out, sizeof out, &e, &ae, &mig, &sticky);
    LS_CHECK(st == 1 && sd_same(out, o1) && e == 0 && !mig && !sticky, "beschreibbar: Original bleibt aktiv");
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\TDATA", a1);
    _snprintf_s(q, sizeof q, _TRUNCATE, "%s\\" LS_MARK, a1);
    LS_CHECK(GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES && GetFileAttributesA(q) == INVALID_FILE_ATTRIBUTES,
             "beschreibbar: Ersatzordner unberuehrt");
    st = ls_resolve(o1, "", out, sizeof out, &e, &ae, &mig, &sticky);
    LS_CHECK(st == 1 && sd_same(out, o1), "beschreibbar ohne Ersatzordner (loc_save_alt=0) gleich");

    /* 2. nicht beschreibbar (Verweigern-Eintrag), vorhandener Pilot -> Ersatzordner mit Uebernahme */
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\TDATA\\%s\\SYSTEMLINK\\LINKUSER", o2, XB_TITLE_ID_STR);
    LS_CHECK(ls_st_put(p, "PILOT-A"), "Pilot im Original anlegen");
    LS_CHECK(ls_st_acl(o2, 1), "Original sperren");
    st = ls_resolve(o2, a2, out, sizeof out, &e, &ae, &mig, &sticky);
    LS_CHECK(st == 2 && sd_same(out, a2) && e == ERROR_ACCESS_DENIED && !sticky, "gesperrt: Ersatzordner aktiv (Win32 5)");
    LS_CHECK(mig == 1, "vorhandener Pilot einmal uebernommen");
    _snprintf_s(q, sizeof q, _TRUNCATE, "%s\\TDATA\\%s\\SYSTEMLINK\\LINKUSER", a2, XB_TITLE_ID_STR);
    LS_CHECK(ls_st_is(q, "PILOT-A"), "uebernommener Pilot gleich");
    /* Spiel speichert neuen Piloten (im aktiven Ordner), dann Neustart */
    _snprintf_s(q, sizeof q, _TRUNCATE, "%s\\TDATA\\%s\\SYSTEMLINK\\LINKUSER", out, XB_TITLE_ID_STR);
    LS_CHECK(ls_st_put(q, "PILOT-B"), "neuer Pilot im aktiven Ordner gespeichert");
    st = ls_resolve(o2, a2, out, sizeof out, &e, &ae, &mig, &sticky);
    LS_CHECK(st == 2 && sd_same(out, a2) && sticky && !mig, "Neustart: derselbe Ersatzordner, keine neue Uebernahme");
    _snprintf_s(q, sizeof q, _TRUNCATE, "%s\\TDATA\\%s\\SYSTEMLINK\\LINKUSER", out, XB_TITLE_ID_STR);
    LS_CHECK(ls_st_is(q, "PILOT-B"), "Pilot uebersteht Neustart");
    LS_CHECK(DeleteFileA(q), "Pilot im Ersatzordner loeschen");
    st = ls_resolve(o2, a2, out, sizeof out, &e, &ae, &mig, &sticky);
    LS_CHECK(st == 2 && GetFileAttributesA(q) == INVALID_FILE_ATTRIBUTES, "geloeschter Pilot kommt nicht aus dem Original zurueck");
    /* 3. Original wieder beschreibbar: Staende bleiben im Ersatzordner (Lesen = Schreiben) */
    LS_CHECK(ls_st_acl(o2, 0), "Original freigeben");
    st = ls_resolve(o2, a2, out, sizeof out, &e, &ae, &mig, &sticky);
    LS_CHECK(st == 2 && sd_same(out, a2) && sticky && e == 0, "Original wieder frei: Ersatzordner bleibt (Merkdatei)");
    /* andere Merkdatei (anderer Datenordner): beschreibbares Original gilt */
    st = ls_resolve(o1, a2, out, sizeof out, &e, &ae, &mig, &sticky);
    LS_CHECK(st == 1 && sd_same(out, o1), "fremde Merkdatei: beschreibbares Original");

    /* 4. Original und Ersatzordner nicht beschreibbar -> klare Meldung, kein Erfolg vorgetaeuscht */
    LS_CHECK(ls_st_put(f4, "x"), "Datei statt Ordner");
    st = ls_resolve(o4, a4, out, sizeof out, &e, &ae, &mig, &sticky);
    LS_CHECK(st == 3 && sd_same(out, o4) && e != 0 && ae != 0, "beide gesperrt: Zustand nicht beschreibbar");
    LS_CHECK(strstr(ls_reason(ERROR_ACCESS_DENIED), "Controlled Folder Access") != NULL && ls_reason(e)[0],
             "Grund im Klartext");
    st = ls_resolve(o4, "", out, sizeof out, &e, &ae, &mig, &sticky);
    LS_CHECK(st == 3 && sd_same(out, o4), "ohne Ersatzordner (loc_save_alt=0): nicht beschreibbar");
    /* leerer gesperrter Ordner (TDATA nicht anlegbar): Grund = Zugriff verweigert, nicht "fehlt" */
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\ro3_LOC", base);
    xb_mkdir_p(p);
    LS_CHECK(ls_st_acl(p, 1), "leeren Ordner sperren");
    st = ls_resolve(p, "", out, sizeof out, &e, &ae, &mig, &sticky);
    LS_CHECK(st == 3 && e == ERROR_ACCESS_DENIED, "leerer gesperrter Ordner: Win32 5");
    ls_st_acl(p, 0);

    /* 5. Anzeige mit Platzhaltern, ohne Benutzername */
    if (GetEnvironmentVariableA("LOCALAPPDATA", la, sizeof la) > 0) {
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\Steelbound\\loc\\hdd_LOC", la);
        ls_show_path(p, q, sizeof q);
        LS_CHECK(strcmp(q, "%LOCALAPPDATA%\\Steelbound\\loc\\hdd_LOC") == 0 && sb_diag_clean_hits(q) == 0,
                 "Anzeige %LOCALAPPDATA%\\Steelbound\\loc\\hdd_LOC");
        {
            wchar_t uw[400];
            _snprintf_s(p, sizeof p, _TRUNCATE, "%s/Steelbound/loc/hdd_LOC", la);
            ls_ui_path(p, uw, 400);
            LS_CHECK(wcscmp(uw, L"%LOCALAPPDATA%\\Steelbound\\loc\\hdd_LOC") == 0, "Einblendung %LOCALAPPDATA%\\...");
        }
    }
    ls_show_path(base, q, sizeof q);
    LS_CHECK(sb_diag_clean_hits(q) == 0, "Anzeige Testordner ohne Benutzername");

    ls_st_acl(o2, 0);
    sd_rmtree(base);
#undef LS_CHECK
    HLOG_I("LOCSAVE-SELFTEST: %s (%u/%u Pruefungen)", fail ? "FEHLER" : "ok", n - fail, n);
    return (int)fail;
}
