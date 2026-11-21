/* launcher_diag.c - W279 DIAG-CRASH: Absturzberichte im Launcher und das
 * Diagnosepaket. Beschreibung s. launcher_diag.h. Kein Netz: der Browser
 * oeffnet die Issue-Seite nur auf Knopfdruck (launcher_diag.inc). */
#define COBJMACROS
#include <windows.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "launcher_diag.h"
#include "launcher_u8.h"
#include "launcher_cfg.h"
#include "launcher_ctrl.h"
#include "launcher_alpha.h"
#include "launcher_update.h"
#include "sb_userdata.h"
#include "sb_pack.h"
#include "sb_sha256.h"
#include "sb_feedback.h" /* SB_FEEDBACK_REPO_DEFAULT */

#ifndef SB_EDITION_COMMUNITY
#define SB_EDITION_COMMUNITY 0
#endif

#define LD_P 1100
#define LD_LOG_TAIL (2u * 1024 * 1024)  /* je Log hoechstens die letzten 2 MB */
#define LD_FILE_MAX (48u * 1024 * 1024) /* Einzeldatei im Paket */
#define LD_ZMAX 96

static char g_t_crash[LD_P], g_t_logs[LD_P], g_t_data[LD_P];

void ld_test_dirs(const char *crash, const char *logs, const char *data)
{
    strcpy_s(g_t_crash, sizeof g_t_crash, crash ? crash : "");
    strcpy_s(g_t_logs, sizeof g_t_logs, logs ? logs : "");
    strcpy_s(g_t_data, sizeof g_t_data, data ? data : "");
}

static void strip_sep(char *p)
{
    size_t n = strlen(p);
    while (n > 3 && (p[n - 1] == '\\' || p[n - 1] == '/')) { p[--n] = '\0'; }
}

void ld_dirs(const char *client_dir, char *crash, char *logs, char *data, size_t cap)
{
    char acp[1024], d[LD_P];
    int legacy = sb_userdata_mode() == SB_UD_LEGACY;
    la_data_root(client_dir, d, sizeof d);
    if (data) { strcpy_s(data, cap, g_t_data[0] ? g_t_data : d); }
    if (crash) {
        if (g_t_crash[0]) {
            strcpy_s(crash, cap, g_t_crash);
        } else if (!legacy && sb_userdata_path_peek(SB_UD_CRASH, acp, sizeof acp) == 1) {
            u8_from_acp(acp, crash, cap);
        } else {
            _snprintf_s(crash, cap, _TRUNCATE, "%s\\out\\crash", d);
        }
        strip_sep(crash);
    }
    if (logs) {
        if (g_t_logs[0]) {
            strcpy_s(logs, cap, g_t_logs);
        } else if (!legacy && sb_userdata_path_peek(SB_UD_LOGS, acp, sizeof acp) == 1) {
            u8_from_acp(acp, logs, cap);
        } else {
            _snprintf_s(logs, cap, _TRUNCATE, "%s\\out", d);
        }
        strip_sep(logs);
    }
}

/* ------------------------------------------------------------------ */
/* Dateien                                                             */
/* ------------------------------------------------------------------ */
/* Datei lesen (max = hoechstens die letzten max Bytes, 0 = ganz). Extra
 * Platz fuer Ersetzungen: room. Rueckgabe malloc'd (NUL-terminiert). */
static char *rd_file(const char *path, unsigned max, unsigned room, unsigned *len, int *cut)
{
    wchar_t w[LD_P];
    HANDLE h;
    LARGE_INTEGER sz, off;
    DWORD want, got = 0;
    char *b;
    if (len) { *len = 0; }
    if (cut) { *cut = 0; }
    if (!u8_to_w(path, w, LD_P)) { return NULL; }
    h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { return NULL; }
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 1024ll * 1024 * 1024) {
        CloseHandle(h);
        return NULL;
    }
    want = (DWORD)sz.QuadPart;
    if (max && want > max) {
        want = max;
        if (cut) { *cut = 1; }
    }
    b = (char *)malloc((size_t)want + room + 1);
    if (!b) {
        CloseHandle(h);
        return NULL;
    }
    off.QuadPart = sz.QuadPart - want;
    if (!SetFilePointerEx(h, off, NULL, FILE_BEGIN) || !ReadFile(h, b, want, &got, NULL)) { got = 0; }
    CloseHandle(h);
    b[got] = '\0';
    if (len) { *len = got; }
    return b;
}

static int wr_file(const char *path, const void *d, size_t n)
{
    FILE *f = NULL;
    int ok;
    if (u8_fopen_s(&f, path, "wb") != 0 || !f) { return 0; }
    ok = fwrite(d, 1, n, f) == n;
    if (fclose(f)) { ok = 0; }
    return ok;
}

static int is_dir(const char *p)
{
    unsigned long a = u8_GetFileAttributes(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* ------------------------------------------------------------------ */
/* ZIP (gespeichert, ohne Kompression)                                  */
/* ------------------------------------------------------------------ */
typedef struct {
    char name[200];
    unsigned char *data;
    unsigned size;
} zent_t;
typedef struct {
    zent_t e[LD_ZMAX];
    int n;
    unsigned long long bytes;
} zip_t;

static unsigned crc32b(const unsigned char *d, unsigned n)
{
    static unsigned t[256];
    static int init;
    unsigned c = 0xFFFFFFFFu, i;
    if (!init) {
        for (i = 0; i < 256; i++) {
            unsigned k, v = i;
            for (k = 0; k < 8; k++) { v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1; }
            t[i] = v;
        }
        init = 1;
    }
    for (i = 0; i < n; i++) { c = t[(c ^ d[i]) & 255] ^ (c >> 8); }
    return c ^ 0xFFFFFFFFu;
}

/* Eintrag uebernehmen (data gehoert danach dem ZIP). */
static int z_add(zip_t *z, const char *name, void *data, unsigned size)
{
    size_t i;
    if (z->n >= LD_ZMAX || !data) {
        free(data);
        return 0;
    }
    strcpy_s(z->e[z->n].name, sizeof z->e[z->n].name, name);
    for (i = 0; z->e[z->n].name[i]; i++) {
        if (z->e[z->n].name[i] == '\\') { z->e[z->n].name[i] = '/'; }
    }
    z->e[z->n].data = (unsigned char *)data;
    z->e[z->n].size = size;
    z->bytes += size;
    z->n++;
    return 1;
}

static int z_add_text(zip_t *z, const char *name, const char *txt)
{
    size_t n = strlen(txt);
    char *c = (char *)malloc(n + 1);
    if (!c) { return 0; }
    memcpy(c, txt, n + 1);
    return z_add(z, name, c, (unsigned)n);
}

static void z_free(zip_t *z)
{
    int i;
    for (i = 0; i < z->n; i++) { free(z->e[i].data); }
    z->n = 0;
}

static void p16(FILE *f, unsigned v)
{
    fputc(v & 255, f);
    fputc((v >> 8) & 255, f);
}
static void p32(FILE *f, unsigned v)
{
    p16(f, v & 0xFFFF);
    p16(f, v >> 16);
}

static int z_write(const zip_t *z, const char *path)
{
    FILE *f = NULL;
    unsigned off[LD_ZMAX], cs[LD_ZMAX], start, end;
    int i, ok;
    SYSTEMTIME lt;
    unsigned dt, tm;
    GetLocalTime(&lt);
    dt = ((unsigned)(lt.wYear - 1980) << 9) | ((unsigned)lt.wMonth << 5) | lt.wDay;
    tm = ((unsigned)lt.wHour << 11) | ((unsigned)lt.wMinute << 5) | (lt.wSecond / 2u);
    if (u8_fopen_s(&f, path, "wb") != 0 || !f) { return 0; }
    for (i = 0; i < z->n; i++) {
        unsigned nl = (unsigned)strlen(z->e[i].name);
        off[i] = (unsigned)ftell(f);
        cs[i] = crc32b(z->e[i].data, z->e[i].size);
        p32(f, 0x04034b50u);
        p16(f, 20);
        p16(f, 0x0800); /* UTF-8-Namen */
        p16(f, 0);
        p16(f, tm);
        p16(f, dt);
        p32(f, cs[i]);
        p32(f, z->e[i].size);
        p32(f, z->e[i].size);
        p16(f, nl);
        p16(f, 0);
        fwrite(z->e[i].name, 1, nl, f);
        fwrite(z->e[i].data, 1, z->e[i].size, f);
    }
    start = (unsigned)ftell(f);
    for (i = 0; i < z->n; i++) {
        unsigned nl = (unsigned)strlen(z->e[i].name);
        p32(f, 0x02014b50u);
        p16(f, 20);
        p16(f, 20);
        p16(f, 0x0800);
        p16(f, 0);
        p16(f, tm);
        p16(f, dt);
        p32(f, cs[i]);
        p32(f, z->e[i].size);
        p32(f, z->e[i].size);
        p16(f, nl);
        p16(f, 0);
        p16(f, 0);
        p16(f, 0);
        p16(f, 0);
        p32(f, 0);
        p32(f, off[i]);
        fwrite(z->e[i].name, 1, nl, f);
    }
    end = (unsigned)ftell(f);
    p32(f, 0x06054b50u);
    p16(f, 0);
    p16(f, 0);
    p16(f, (unsigned)z->n);
    p16(f, (unsigned)z->n);
    p32(f, end - start);
    p32(f, start);
    p16(f, 0);
    ok = !ferror(f);
    if (fclose(f)) { ok = 0; }
    if (!ok) { u8_DeleteFile(path); }
    return ok;
}

/* ------------------------------------------------------------------ */
/* Bereinigen mit Launcher-Wurzeln                                      */
/* ------------------------------------------------------------------ */
typedef struct {
    char data[LD_P], client[LD_P];
    sb_diag_root_t r[2];
    int n;
} roots_t;

static void roots_init(roots_t *rt, const char *client_dir, const char *data)
{
    wchar_t a[LD_P], b[LD_P];
    memset(rt, 0, sizeof *rt);
    strcpy_s(rt->data, sizeof rt->data, data ? data : "");
    strip_sep(rt->data);
    if (client_dir && u8_to_w(client_dir, a, LD_P) && GetFullPathNameW(a, LD_P, b, NULL) > 0) {
        w_to_u8(b, rt->client, (int)sizeof rt->client);
    } else {
        strcpy_s(rt->client, sizeof rt->client, client_dir ? client_dir : "");
    }
    strip_sep(rt->client);
    /* laengerer Pfad zuerst (portabel: Datenordner = Client-Ordner) */
    if (strlen(rt->data) >= strlen(rt->client)) {
        if (rt->data[0]) { rt->r[rt->n].path = rt->data; rt->r[rt->n++].tag = "<data>"; }
        if (rt->client[0] && _stricmp(rt->client, rt->data) != 0) {
            rt->r[rt->n].path = rt->client;
            rt->r[rt->n++].tag = "<client>";
        }
    } else {
        rt->r[rt->n].path = rt->client;
        rt->r[rt->n++].tag = "<client>";
        if (rt->data[0]) { rt->r[rt->n].path = rt->data; rt->r[rt->n++].tag = "<data>"; }
    }
}

/* Text bereinigen und ins ZIP (buf muss cap Bytes haben). */
static void z_add_clean(zip_t *z, ld_pkg_t *r, const roots_t *rt, const char *name, char *buf, size_t cap,
                        int drop_input)
{
    if (drop_input) { r->dropped += sb_diag_drop_input_lines(buf); }
    r->repl += sb_diag_sanitize(buf, cap, rt->r, rt->n);
    r->clean_hits += sb_diag_clean_hits(buf);
    z_add_text(z, name, buf);
}

/* ------------------------------------------------------------------ */
/* PE-Angaben aus der Datei (Build-Datum, PDB-Kennung)                  */
/* ------------------------------------------------------------------ */
static void pe_info(const char *path, char *out, size_t cap)
{
    unsigned len = 0;
    unsigned char *b = (unsigned char *)rd_file(path, 0, 0, &len, NULL);
    unsigned char dg[32];
    char hex[65] = "?", pdb[160] = "none", when[40] = "?";
    _snprintf_s(out, cap, _TRUNCATE, "missing");
    if (!b) { return; }
    sb_sha256(b, len, dg);
    sb_sha256_hex(dg, hex);
    if (len > 0x400 && b[0] == 'M' && b[1] == 'Z') {
        unsigned pe = *(unsigned *)(b + 0x3C);
        if (pe + 0x18 + 0xE0 < len && memcmp(b + pe, "PE\0\0", 4) == 0) {
            const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(b + pe);
            const IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
            unsigned ns = nt->FileHeader.NumberOfSections, dva, dsz, i, k;
            time_t tt = (time_t)nt->FileHeader.TimeDateStamp;
            struct tm g;
            if (gmtime_s(&g, &tt) == 0) {
                _snprintf_s(when, sizeof when, _TRUNCATE, "%04d-%02d-%02d %02d:%02d UTC", g.tm_year + 1900, g.tm_mon + 1,
                            g.tm_mday, g.tm_hour, g.tm_min);
            }
            if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
                (const unsigned char *)(sh + ns) <= b + len) {
                dva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].VirtualAddress;
                dsz = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].Size;
                for (i = 0; i < ns; i++) {
                    if (dva >= sh[i].VirtualAddress && dva < sh[i].VirtualAddress + sh[i].SizeOfRawData) {
                        unsigned fo = dva - sh[i].VirtualAddress + sh[i].PointerToRawData;
                        for (k = 0; k + sizeof(IMAGE_DEBUG_DIRECTORY) <= dsz && fo + k + sizeof(IMAGE_DEBUG_DIRECTORY) <= len;
                             k += sizeof(IMAGE_DEBUG_DIRECTORY)) {
                            const IMAGE_DEBUG_DIRECTORY *d = (const IMAGE_DEBUG_DIRECTORY *)(b + fo + k);
                            if (d->Type == IMAGE_DEBUG_TYPE_CODEVIEW && d->PointerToRawData + 25 < len &&
                                memcmp(b + d->PointerToRawData, "RSDS", 4) == 0) {
                                const unsigned char *cv = b + d->PointerToRawData;
                                const GUID *gu = (const GUID *)(cv + 4);
                                const char *pn = (const char *)(cv + 24), *bs = strrchr(pn, '\\');
                                _snprintf_s(pdb, sizeof pdb, _TRUNCATE,
                                            "%.40s %08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%X", bs ? bs + 1 : pn,
                                            gu->Data1, gu->Data2, gu->Data3, gu->Data4[0], gu->Data4[1], gu->Data4[2],
                                            gu->Data4[3], gu->Data4[4], gu->Data4[5], gu->Data4[6], gu->Data4[7],
                                            *(const unsigned *)(cv + 20));
                            }
                        }
                        break;
                    }
                }
            }
        }
    }
    _snprintf_s(out, cap, _TRUNCATE, "built %s, %u bytes, sha256 %s, pdb %s", when, len, hex, pdb);
    free(b);
}

/* ------------------------------------------------------------------ */
/* Systeminfo                                                          */
/* ------------------------------------------------------------------ */
static void reg_str(HKEY root, const wchar_t *key, const wchar_t *val, char *o, size_t cap)
{
    wchar_t w[256];
    DWORD sz = sizeof w, ty = 0;
    o[0] = '\0';
    if (RegGetValueW(root, key, val, RRF_RT_REG_SZ, &ty, w, &sz) == ERROR_SUCCESS) {
        w_to_u8(w, o, (int)cap);
    }
}

static const GUID kIID_IDXGIFactory1 = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
static const GUID kIID_IDXGIDevice = { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };

void ld_sysinfo(char *out, size_t cap)
{
    char t[512], cpu[256], dv[64], ubr[32];
    size_t used = 0;
    out[0] = '\0';
#define ADD(...)                                                                \
    do {                                                                        \
        _snprintf_s(out + used, cap - used, _TRUNCATE, __VA_ARGS__);           \
        used = strlen(out);                                                     \
    } while (0)
    { /* OS */
        typedef LONG(WINAPI * rgv_t)(OSVERSIONINFOW *);
        OSVERSIONINFOW v;
        rgv_t f = (rgv_t)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
        DWORD u = 0, us = sizeof u;
        memset(&v, 0, sizeof v);
        v.dwOSVersionInfoSize = sizeof v;
        if (f) { f(&v); }
        reg_str(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion", dv, sizeof dv);
        ubr[0] = '\0';
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD,
                         NULL, &u, &us) == ERROR_SUCCESS) {
            _snprintf_s(ubr, sizeof ubr, _TRUNCATE, ".%lu", u);
        }
        reg_str(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName", t, sizeof t);
        if (v.dwBuildNumber >= 22000 && strncmp(t, "Windows 10", 10) == 0) { /* ProductName bleibt bei 11 "Windows 10" */
            t[9] = '1';
        }
        ADD("os: %s %s (%lu.%lu.%lu%s)\r\n", t[0] ? t : "Windows", dv, v.dwMajorVersion, v.dwMinorVersion,
            v.dwBuildNumber, ubr);
        {
            BOOL wow = FALSE;
            IsWow64Process(GetCurrentProcess(), &wow);
            ADD("os-bits: %s\r\n", wow ? "64-bit Windows (launcher 32-bit)" : "32-bit Windows or native");
        }
    }
    { /* CPU + RAM */
        MEMORYSTATUSEX m;
        reg_str(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString", cpu,
                sizeof cpu);
        ADD("cpu: %s, %lu logical processors\r\n", cpu[0] ? cpu : "?", GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
        m.dwLength = sizeof m;
        if (GlobalMemoryStatusEx(&m)) {
            ADD("ram: %llu MB total, %llu MB free\r\n", m.ullTotalPhys >> 20, m.ullAvailPhys >> 20);
        }
    }
    { /* GPUs (DXGI, Treiberversion = UMD-Version) */
        typedef HRESULT(WINAPI * cdf_t)(REFIID, void **);
        HMODULE dx = LoadLibraryW(L"dxgi.dll");
        cdf_t cf = dx ? (cdf_t)(void *)GetProcAddress(dx, "CreateDXGIFactory1") : NULL;
        IDXGIFactory1 *fa = NULL;
        if (cf && SUCCEEDED(cf(&kIID_IDXGIFactory1, (void **)&fa)) && fa) {
            IDXGIAdapter1 *ad = NULL;
            UINT i;
            for (i = 0; i < 8 && IDXGIFactory1_EnumAdapters1(fa, i, &ad) != DXGI_ERROR_NOT_FOUND; i++) {
                DXGI_ADAPTER_DESC1 d;
                LARGE_INTEGER umd;
                char nm[200], drv[48] = "?";
                if (ad && SUCCEEDED(IDXGIAdapter1_GetDesc1(ad, &d))) {
                    w_to_u8(d.Description, nm, (int)sizeof nm);
                    if (SUCCEEDED(IDXGIAdapter1_CheckInterfaceSupport(ad, &kIID_IDXGIDevice, &umd))) {
                        _snprintf_s(drv, sizeof drv, _TRUNCATE, "%u.%u.%u.%u", (unsigned)(umd.HighPart >> 16),
                                    (unsigned)(umd.HighPart & 0xFFFF), (unsigned)(umd.LowPart >> 16),
                                    (unsigned)(umd.LowPart & 0xFFFF));
                    }
                    /* VRAM nicht: DedicatedVideoMemory ist im 32-Bit-Prozess auf 4 GB gekappt */
                    ADD("gpu%u: %s (vendor 0x%04X device 0x%04X, driver %s%s)\r\n", i, nm, d.VendorId, d.DeviceId, drv,
                        (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? ", software" : "");
                }
                if (ad) { IDXGIAdapter1_Release(ad); }
                ad = NULL;
            }
            IDXGIFactory1_Release(fa);
        } else {
            ADD("gpu: DXGI not available\r\n");
        }
        if (dx) { FreeLibrary(dx); }
    }
    { /* Monitore */
        DISPLAY_DEVICEW dd;
        DWORD i;
        int nmon = 0;
        for (i = 0; i < 16; i++) {
            DEVMODEW dm;
            DISPLAY_DEVICEW mon;
            char an[160], mn[160] = "";
            memset(&dd, 0, sizeof dd);
            dd.cb = sizeof dd;
            if (!EnumDisplayDevicesW(NULL, i, &dd, 0)) { break; }
            if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) { continue; }
            memset(&dm, 0, sizeof dm);
            dm.dmSize = sizeof dm;
            if (!EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) { continue; }
            memset(&mon, 0, sizeof mon);
            mon.cb = sizeof mon;
            if (EnumDisplayDevicesW(dd.DeviceName, 0, &mon, 0)) { w_to_u8(mon.DeviceString, mn, (int)sizeof mn); }
            w_to_u8(dd.DeviceString, an, (int)sizeof an);
            ADD("monitor%d: %lux%lu @ %lu Hz, %lu bit, position %ld,%ld%s (%s on %s)\r\n", nmon++, dm.dmPelsWidth,
                dm.dmPelsHeight, dm.dmDisplayFrequency, dm.dmBitsPerPel, dm.dmPosition.x, dm.dmPosition.y,
                (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? ", primary" : "", mn[0] ? mn : "monitor", an);
        }
        {
            HDC dc = GetDC(NULL);
            if (dc) {
                ADD("desktop-dpi: %d\r\n", GetDeviceCaps(dc, LOGPIXELSX));
                ReleaseDC(NULL, dc);
            }
        }
    }
#undef ADD
}

/* ------------------------------------------------------------------ */
/* Absturzberichte                                                     */
/* ------------------------------------------------------------------ */
static void seen_path(const char *crash_dir, char *o, size_t cap)
{
    _snprintf_s(o, cap, _TRUNCATE, "%s\\last_seen.txt", crash_dir);
}

int ld_crash_new(const char *crash_dir, sb_crash_ent_t *e)
{
    static sb_crash_ent_t l[64];
    char sp[LD_P], seen[64] = "";
    int n, i;
    unsigned len = 0;
    char *b;
    if (!crash_dir || !crash_dir[0]) { return 0; }
    n = sb_diag_crash_list(crash_dir, l, 64);
    if (n <= 0) { return 0; }
    seen_path(crash_dir, sp, sizeof sp);
    b = rd_file(sp, 256, 0, &len, NULL);
    if (b) {
        size_t k = strcspn(b, "\r\n \t");
        if (k < sizeof seen) {
            memcpy(seen, b, k);
            seen[k] = '\0';
        }
        free(b);
    }
    for (i = 0; i < n; i++) { /* neueste zuerst */
        if (seen[0] && strcmp(l[i].name, seen) <= 0) { break; }
        if (sb_diag_crash_counts(&l[i])) {
            if (e) { *e = l[i]; }
            return 1;
        }
    }
    return 0;
}

int ld_crash_mark_seen(const char *crash_dir, const char *name)
{
    char sp[LD_P], t[100];
    if (!crash_dir || !name || !sb_diag_crash_name_ok(name) || !is_dir(crash_dir)) { return 0; }
    seen_path(crash_dir, sp, sizeof sp);
    _snprintf_s(t, sizeof t, _TRUNCATE, "%s\r\n", name);
    return wr_file(sp, t, strlen(t));
}

/* Wert einer "key: wert"-Zeile aus info.txt. */
static void info_val(const char *info, const char *key, char *o, size_t cap)
{
    const char *p = info;
    size_t kl = strlen(key);
    o[0] = '\0';
    while (p && *p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == ':') {
            const char *v = p + kl + 1;
            size_t l;
            while (*v == ' ') { v++; }
            l = strcspn(v, "\r\n");
            if (l >= cap) { l = cap - 1; }
            memcpy(o, v, l);
            o[l] = '\0';
            return;
        }
        p = strchr(p, '\n');
        if (p) { p++; }
    }
}

static char *crash_info(const char *crash_dir, const char *name)
{
    char p[LD_P];
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\%s\\info.txt", crash_dir, name);
    return rd_file(p, 256 * 1024, 0, NULL, NULL);
}

void ld_crash_summary(const char *crash_dir, const char *name, char *out, size_t cap)
{
    char *in = crash_info(crash_dir, name), rs[300] = "", ex[300] = "";
    out[0] = '\0';
    if (!in) { return; }
    info_val(in, "reason", rs, sizeof rs);
    info_val(in, "exception", ex, sizeof ex);
    _snprintf_s(out, cap, _TRUNCATE, "%s%s%s", ex[0] ? ex : rs, ex[0] && rs[0] ? " - " : "", ex[0] ? rs : "");
    free(in);
}

int ld_crash_zip(const char *crash_dir, const char *name, char *zip, size_t cap)
{
    char sub[LD_P], pat[LD_P];
    u8_find_t fd;
    zip_t *z;
    int ok;
    if (!sb_diag_crash_name_ok(name)) { return 0; }
    _snprintf_s(sub, sizeof sub, _TRUNCATE, "%s\\%s", crash_dir, name);
    _snprintf_s(pat, sizeof pat, _TRUNCATE, "%s\\*", sub);
    z = (zip_t *)calloc(1, sizeof *z);
    if (!z) { return 0; }
    if (u8_find_first(pat, &fd)) {
        do {
            char fp[LD_P], zn[260];
            unsigned len = 0;
            char *b;
            if (fd.attr & FILE_ATTRIBUTE_DIRECTORY) { continue; }
            _snprintf_s(fp, sizeof fp, _TRUNCATE, "%s\\%s", sub, fd.name);
            b = rd_file(fp, 0, 0, &len, NULL);
            if (b && len <= LD_FILE_MAX) {
                _snprintf_s(zn, sizeof zn, _TRUNCATE, "%s/%s", name, fd.name);
                z_add(z, zn, b, len);
            } else {
                free(b);
            }
        } while (u8_find_next(&fd));
        u8_find_close(&fd);
    }
    _snprintf_s(zip, cap, _TRUNCATE, "%s\\%s.zip", crash_dir, name);
    ok = z->n > 0 && z_write(z, zip);
    z_free(z);
    free(z);
    return ok;
}

static void urlenc(char *o, size_t cap, const char *s)
{
    static const char hx[] = "0123456789ABCDEF";
    size_t n = 0;
    for (; *s && n + 4 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            o[n++] = (char)c;
        } else {
            o[n++] = '%';
            o[n++] = hx[c >> 4];
            o[n++] = hx[c & 15];
        }
    }
    o[n] = '\0';
}

int ld_url_ok(const char *u)
{
    static const char pre[] = "https://github.com/" SB_FEEDBACK_REPO_DEFAULT "/issues/new?";
    const char *p;
    if (!u || strncmp(u, pre, sizeof pre - 1) != 0 || strlen(u) > 7000) { return 0; }
    for (p = u + sizeof pre - 1; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
              c == '.' || c == '~' || c == '%' || c == '=' || c == '&')) {
            return 0;
        }
    }
    return 1;
}

int ld_issue_url(const char *crash_dir, const char *name, const char *zipname, char *url, size_t cap)
{
    char *in = crash_info(crash_dir, name);
    char ver[300] = "?", game[300] = "?", gpu[300] = "?", st[300] = "?", ex[300] = "", rs[300] = "", status[40] = "";
    char title[200], what[1400];
    char e1[700], e2[700], e3[700], e4[700], e5[700], e6[3200], e7[64];
    if (!sb_diag_crash_name_ok(name)) {
        free(in);
        return 0;
    }
    if (in) {
        info_val(in, "client", ver, sizeof ver);
        info_val(in, "game", game, sizeof game);
        info_val(in, "gpu", gpu, sizeof gpu);
        info_val(in, "state", st, sizeof st);
        info_val(in, "exception", ex, sizeof ex);
        info_val(in, "reason", rs, sizeof rs);
        info_val(in, "status", status, sizeof status);
        free(in);
    }
    _snprintf_s(title, sizeof title, _TRUNCATE, "%s %s", strcmp(status, "hang") == 0 ? "[Hang]" : "[Crash]",
                ex[0] ? ex : rs[0] ? rs : "(short description)");
    if (strlen(title) > 100) { title[100] = '\0'; }
    _snprintf_s(what, sizeof what, _TRUNCATE,
                "The game %s (report folder %s, status %s).\n\nWhat I was doing:\n...\n\n"
                "Please attach %s from the folder the launcher opened (drag it into the Attachments field). "
                "The launcher removed user name, computer name and paths; nothing was uploaded automatically.",
                strcmp(status, "hang") == 0 ? "froze" : "crashed", name, status[0] ? status : "?",
                zipname && zipname[0] ? zipname : "the report ZIP");
    urlenc(e1, sizeof e1, title);
    urlenc(e2, sizeof e2, ver);
    urlenc(e3, sizeof e3, game);
    urlenc(e4, sizeof e4, gpu);
    urlenc(e5, sizeof e5, st);
    urlenc(e6, sizeof e6, what);
    /* 19.11.: Dropdown "Which game?" (Feld-ID which) -> GitHub-Workflow setzt Label game: SB/LoC */
    urlenc(e7, sizeof e7, (game && strstr(game, "Line of Contact")) ? "Line of Contact" : "Steel Battalion");
    _snprintf_s(url, cap, _TRUNCATE,
                "https://github.com/" SB_FEEDBACK_REPO_DEFAULT
                "/issues/new?template=bug_report.yml&title=%s&which=%s&what=%s&version=%s&game=%s&gpu=%s&state=%s",
                e1, e7, e6, e2, e3, e4, e5);
    return ld_url_ok(url);
}

/* ------------------------------------------------------------------ */
/* Diagnosepaket                                                       */
/* ------------------------------------------------------------------ */
static void cfg_key(const char *path, const char *key, char *o, size_t cap)
{
    char *b = rd_file(path, 1024 * 1024, 0, NULL, NULL), *p;
    size_t kl = strlen(key);
    o[0] = '\0';
    if (!b) { return; }
    for (p = b; p && *p;) {
        char *l = p + strspn(p, " \t");
        if (strncmp(l, key, kl) == 0) {
            char *v = l + kl;
            while (*v == ' ' || *v == '\t') { v++; }
            if (*v == '=') {
                size_t n;
                v++;
                while (*v == ' ' || *v == '\t') { v++; }
                n = strcspn(v, "\r\n");
                while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\t')) { n--; }
                if (n >= cap) { n = cap - 1; }
                memcpy(o, v, n);
                o[n] = '\0'; /* letzter Treffer gewinnt (wie der Client) */
            }
        }
        p = strchr(p, '\n');
        if (p) { p++; }
    }
    free(b);
}

static const char *g_ld_devices; /* W312 */
void ld_pkg_set_devices(const char *txt)
{
    g_ld_devices = txt;
}

int ld_pkg_build(const char *client_dir, const char *packs_json, ld_pkg_t *r)
{
    char crash[LD_P], logs[LD_P], data[LD_P], uc[LD_P], base[LD_P], t[LD_P], stamp[32];
    roots_t rt;
    zip_t *z;
    char *big;
    size_t bigcap = 64 * 1024;
    SYSTEMTIME lt;
    int ok, i;
    memset(r, 0, sizeof *r);
    z = (zip_t *)calloc(1, sizeof *z);
    big = (char *)malloc(bigcap);
    if (!z || !big) {
        free(z);
        free(big);
        return 0;
    }
    ld_dirs(client_dir, crash, logs, data, LD_P);
    roots_init(&rt, client_dir, data);
    lcfg_user_cfg_path(client_dir, uc, sizeof uc);
    _snprintf_s(base, sizeof base, _TRUNCATE, "%s\\sb_native.cfg", rt.client);
    GetLocalTime(&lt);
    _snprintf_s(stamp, sizeof stamp, _TRUNCATE, "%04u%02u%02u-%02u%02u%02u", lt.wYear, lt.wMonth, lt.wDay, lt.wHour,
                lt.wMinute, lt.wSecond);

    /* system.txt: Systeminfo + Versionen */
    {
        char si[8192], e1[600], e2[600];
        const char *v = lupd_version();
        ld_sysinfo(si, sizeof si);
        _snprintf_s(t, sizeof t, _TRUNCATE, "%s\\sb_emu.dll", rt.client);
        pe_info(t, e1, sizeof e1);
        _snprintf_s(t, sizeof t, _TRUNCATE, "%s\\sb_ldr.exe", rt.client);
        pe_info(t, e2, sizeof e2);
        _snprintf_s(big, bigcap, _TRUNCATE,
                    "Steelbound diagnostic package (W279)\r\ncreated: %04u-%02u-%02u %02u:%02u:%02u (local time)\r\n\r\n"
                    "[system]\r\n%s\r\n[versions]\r\nlauncher: %s edition, version %s\r\nclient sb_emu.dll: %s\r\n"
                    "client sb_ldr.exe: %s\r\ndata-location: %s (%s)\r\n",
                    lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond, si,
                    SB_EDITION_COMMUNITY ? "community" : "private", v && v[0] ? v : "dev", e1, e2,
                    sb_userdata_mode_name(sb_userdata_mode()), sb_userdata_mode_source());
        z_add_clean(z, r, &rt, "system.txt", big, bigcap, 0);
    }
    if (g_ld_devices && g_ld_devices[0]) { /* W312: Geraetezeilen ohne Werte */
        _snprintf_s(big, bigcap, _TRUNCATE, "%s", g_ld_devices);
        z_add_clean(z, r, &rt, "devices.txt", big, bigcap, 0);
    }
    /* settings_changed.txt */
    {
        char *d = (char *)malloc(65536);
        if (d) {
            int nl = sb_diag_cfg_diff(base, uc, NULL, d, 65536 - 1024);
            _snprintf_s(big, bigcap, _TRUNCATE,
                        "# settings that differ from sb_native.cfg (the shipped defaults); user.cfg %s\r\n%s",
                        u8_GetFileAttributes(uc) == INVALID_FILE_ATTRIBUTES ? "not found" : "found",
                        nl > 0 ? d : nl == 0 ? "(none)\r\n" : "(sb_native.cfg not readable)\r\n");
            free(d);
            z_add_clean(z, r, &rt, "settings_changed.txt", big, bigcap, 1);
        }
    }
    /* packs.txt */
    {
        static sb_pack_scan_t sc[32];
        char acp[LD_P], dir[LD_P], *s;
        int n = -1;
        size_t used;
        _snprintf_s(big, bigcap, _TRUNCATE, "# packs in the pack list (cd-sha256 = SHA-256 over size + ZIP central "
                                            "directory with the CRC32 of every entry)\r\n");
        if (packs_json && packs_json[0] && u8_to_acp(packs_json, acp, sizeof acp) == 0) {
            n = sb_pack_scan(acp, sc, 32);
        }
        strcpy_s(dir, sizeof dir, packs_json ? packs_json : "");
        s = strrchr(dir, '\\');
        if (!s || (strrchr(dir, '/') && strrchr(dir, '/') > s)) { s = strrchr(dir, '/'); }
        if (s) { *s = '\0'; }
        used = strlen(big);
        if (n <= 0) {
            _snprintf_s(big + used, bigcap - used, _TRUNCATE, "%s\r\n", n == 0 ? "(no packs)" : "(no pack list)");
        }
        for (i = 0; i < n && i < 32; i++) {
            char full[LD_P], fa[LD_P], fp[80] = "";
            int k = 0;
            if ((strlen(sc[i].src) > 1 && sc[i].src[1] == ':') || sc[i].src[0] == '\\' || sc[i].src[0] == '/') {
                strcpy_s(full, sizeof full, sc[i].src);
            } else {
                _snprintf_s(full, sizeof full, _TRUNCATE, "%s\\%s", dir, sc[i].src);
            }
            if (u8_to_acp(full, fa, sizeof fa) == 0) { k = sb_diag_pack_fp(fa, fp, sizeof fp); }
            _snprintf_s(big + used, bigcap - used, _TRUNCATE, "%s %s %s \"%s\" type=%s %s=%s\r\n",
                        sc[i].enabled ? "[on] " : "[off]", sc[i].id, sc[i].version, sc[i].name, sc[i].type,
                        k == 2 ? "manifest-sha256" : "cd-sha256", k ? fp : "unreadable");
            used = strlen(big);
        }
        z_add_clean(z, r, &rt, "packs.txt", big, bigcap, 0);
    }
    /* Belegungsprofil (Konfiguration, keine Eingaben) */
    {
        char prof[96], pp[LD_P];
        cfg_key(uc, "input_profile", prof, sizeof prof);
        if (!prof[0]) { cfg_key(base, "input_profile", prof, sizeof prof); }
        if (prof[0] && lctrl_input_profile_path(rt.client, prof, pp, sizeof pp) == 0) {
            unsigned len = 0;
            char *b = rd_file(pp, 256 * 1024, 64 * 1024, &len, NULL);
            if (b) {
                _snprintf_s(t, sizeof t, _TRUNCATE, "bindings/%s.ini", prof);
                r->repl += sb_diag_sanitize(b, (size_t)len + 64 * 1024, rt.r, rt.n);
                r->clean_hits += sb_diag_clean_hits(b);
                z_add(z, t, b, (unsigned)strlen(b));
                r->profile = 1;
            }
        }
        if (!r->profile) {
            _snprintf_s(big, bigcap, _TRUNCATE, "input_profile=%s\r\n%s\r\n", prof,
                        prof[0] ? "(profile file not found)" : "(built-in default bindings, no profile file)");
            z_add_clean(z, r, &rt, "bindings/active.txt", big, bigcap, 0);
        }
    }
    /* die letzten 3 Client-Logs (ohne Eingabe-Zeilen) */
    {
        static const char *const ln[] = { "sb_harness.log", "sb_harness.prev.log", "sb_harness.prev2.log" };
        for (i = 0; i < 3; i++) {
            char lp[LD_P], zn[80];
            unsigned len = 0;
            int cut = 0;
            char *b;
            size_t cap;
            _snprintf_s(lp, sizeof lp, _TRUNCATE, "%s\\%s", logs, ln[i]);
            cap = 0;
            b = rd_file(lp, LD_LOG_TAIL, LD_LOG_TAIL / 2, &len, &cut);
            if (!b) { continue; }
            cap = (size_t)len + LD_LOG_TAIL / 2 + 1;
            if (cut) { /* angeschnittene erste Zeile weg */
                char *nl = strchr(b, '\n');
                if (nl) { memmove(b, nl + 1, strlen(nl + 1) + 1); }
            }
            r->dropped += sb_diag_drop_input_lines(b);
            r->repl += sb_diag_sanitize(b, cap, rt.r, rt.n);
            r->clean_hits += sb_diag_clean_hits(b);
            _snprintf_s(zn, sizeof zn, _TRUNCATE, "logs/%s", ln[i]);
            z_add(z, zn, b, (unsigned)strlen(b));
            r->logs++;
        }
    }
    /* die letzten 3 Absturzberichte */
    {
        sb_crash_ent_t l[3];
        int n = sb_diag_crash_list(crash, l, 3), c;
        for (c = 0; c < n; c++) {
            char sub[LD_P], pat[LD_P];
            u8_find_t fd;
            _snprintf_s(sub, sizeof sub, _TRUNCATE, "%s\\%s", crash, l[c].name);
            _snprintf_s(pat, sizeof pat, _TRUNCATE, "%s\\*", sub);
            if (!u8_find_first(pat, &fd)) { continue; }
            do {
                char fp[LD_P], zn[300];
                unsigned len = 0;
                size_t nl = strlen(fd.name);
                int txt = nl > 4 && _stricmp(fd.name + nl - 4, ".txt") == 0;
                char *b;
                if (fd.attr & FILE_ATTRIBUTE_DIRECTORY) { continue; }
                _snprintf_s(fp, sizeof fp, _TRUNCATE, "%s\\%s", sub, fd.name);
                b = rd_file(fp, 0, txt ? 64 * 1024 : 0, &len, NULL);
                if (!b || len > LD_FILE_MAX) {
                    free(b);
                    continue;
                }
                if (txt) { /* vom Client schon bereinigt; hier mit Launcher-Wurzeln nachziehen */
                    r->dropped += sb_diag_drop_input_lines(b);
                    r->repl += sb_diag_sanitize(b, (size_t)len + 64 * 1024, rt.r, rt.n);
                    r->clean_hits += sb_diag_clean_hits(b);
                    len = (unsigned)strlen(b);
                }
                _snprintf_s(zn, sizeof zn, _TRUNCATE, "crash/%s/%s", l[c].name, fd.name);
                z_add(z, zn, b, len);
            } while (u8_find_next(&fd));
            u8_find_close(&fd);
            r->crashes++;
        }
    }
    /* README.txt */
    _snprintf_s(big, bigcap, _TRUNCATE,
                "Steelbound diagnostic package\r\n\r\n"
                "Created by the launcher on your request. Nothing has been uploaded - you decide what to share.\r\n\r\n"
                "system.txt            Windows version, CPU, RAM, graphics card + driver, monitors, versions\r\n"
                "settings_changed.txt  settings that differ from the defaults\r\n"
                "packs.txt             packs in your pack list with checksums\r\n"
                "bindings/             your active key/controller binding profile (configuration only)\r\n"
                "logs/                 the last %d client logs (lines that record single inputs removed)\r\n"
                "crash/                the last %d crash/hang reports (text, minidump, info)\r\n\r\n"
                "Not included: save games, replays, game files, recorded inputs, screenshots.\r\n"
                "User name, computer name and paths were replaced (<data> = data folder, <client> = program\r\n"
                "folder, <user>, <pc>, <pfad> = other folder). Please have a quick look before you share it.\r\n",
                r->logs, r->crashes);
    z_add_text(z, "README.txt", big);
    /* schreiben: <Datenordner>\diagnostics\steelbound-diag-<zeit>.zip */
    _snprintf_s(r->dir, sizeof r->dir, _TRUNCATE, "%s\\diagnostics", data);
    lcfg_mkdir_p(r->dir);
    _snprintf_s(r->zip, sizeof r->zip, _TRUNCATE, "%s\\steelbound-diag-%s.zip", r->dir, stamp);
    r->files = z->n;
    r->bytes = z->bytes;
    ok = z_write(z, r->zip);
    z_free(z);
    free(z);
    free(big);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Selbsttests                                                         */
/* ------------------------------------------------------------------ */
static int mk_crash(const char *root, const char *name, const char *status, int recovered)
{
    char d[LD_P], p[LD_P], info[600];
    _snprintf_s(d, sizeof d, _TRUNCATE, "%s\\%s", root, name);
    lcfg_mkdir_p(d);
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\status.txt", d);
    wr_file(p, status, strlen(status));
    _snprintf_s(info, sizeof info, _TRUNCATE,
                "Steelbound crash report (W279)\r\nstatus: %s\r\nreason: last_chance: test\r\n"
                "exception: code=0xC0000005 at 0x30012345 (sb_emu.dll+0x12345) thread=T0001\r\n"
                "state: scene=0x2C loadcount=3 in_mission=1\r\nclient: Steelbound private edition, version dev\r\n"
                "game: Steel Battalion (Europe, PAL) [EU]\r\ngpu: Test GPU (vendor 0x10DE)\r\n",
                status);
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\info.txt", d);
    wr_file(p, info, strlen(info));
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\crash_1.dmp", d);
    wr_file(p, "MDMP-test", 9);
    if (recovered) {
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\recovered.txt", d);
        wr_file(p, "ok", 2);
    }
    return 1;
}

static void rm_tree(const char *dir)
{
    char pat[LD_P];
    u8_find_t fd;
    wchar_t w[LD_P];
    _snprintf_s(pat, sizeof pat, _TRUNCATE, "%s\\*", dir);
    if (u8_find_first(pat, &fd)) {
        do {
            char p[LD_P];
            if (strcmp(fd.name, ".") == 0 || strcmp(fd.name, "..") == 0) { continue; }
            _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\%s", dir, fd.name);
            if ((fd.attr & FILE_ATTRIBUTE_DIRECTORY) && !(fd.attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
                rm_tree(p);
            } else {
                u8_DeleteFile(p);
            }
        } while (u8_find_next(&fd));
        u8_find_close(&fd);
    }
    if (u8_to_w(dir, w, LD_P)) { RemoveDirectoryW(w); }
}

/* Namen der ZIP-Eintraege (Zentralverzeichnis) als "\n"-Liste. */
static int zip_names(const char *zip, char *out, size_t cap, unsigned char **all, unsigned *alen)
{
    unsigned len = 0, i, n, off, k;
    unsigned char *b = (unsigned char *)rd_file(zip, 0, 0, &len, NULL);
    out[0] = '\0';
    if (!b || len < 22) {
        free(b);
        return -1;
    }
    for (i = len - 22 + 1; i-- > 0;) {
        if (*(unsigned *)(b + i) == 0x06054b50u) { break; }
    }
    if (i == (unsigned)-1 || i + 22 > len) {
        free(b);
        return -1;
    }
    n = *(unsigned short *)(b + i + 10);
    off = *(unsigned *)(b + i + 16);
    for (k = 0; k < n && off + 46 <= len; k++) {
        unsigned nl = *(unsigned short *)(b + off + 28), xl = *(unsigned short *)(b + off + 30),
                 cl = *(unsigned short *)(b + off + 32);
        size_t used = strlen(out);
        if (off + 46 + nl > len || used + nl + 2 >= cap) { break; }
        memcpy(out + used, b + off + 46, nl);
        out[used + nl] = '\n';
        out[used + nl + 1] = '\0';
        off += 46 + nl + xl + cl;
    }
    if (all) {
        *all = b;
        *alen = len;
    } else {
        free(b);
    }
    return (int)n;
}

static int mem_has(const unsigned char *b, unsigned n, const char *s)
{
    size_t l = strlen(s), i;
    for (i = 0; l && i + l <= n; i++) {
        if (memcmp(b + i, s, l) == 0) { return 1; }
    }
    return 0;
}

void ld_selftest(int *pass, int *fail)
{
    char tmp[LD_P], root[LD_P], crash[LD_P], logs[LD_P], client[LD_P], why[400] = "", p[LD_P];
    sb_crash_ent_t e;
    int ok = 1;
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%sw279_diag_%lu", tmp, GetCurrentProcessId());
    rm_tree(root);
    _snprintf_s(crash, sizeof crash, _TRUNCATE, "%s\\data\\crash", root);
    _snprintf_s(logs, sizeof logs, _TRUNCATE, "%s\\data\\logs", root);
    _snprintf_s(client, sizeof client, _TRUNCATE, "%s\\client", root);
    lcfg_mkdir_p(crash);
    lcfg_mkdir_p(logs);
    lcfg_mkdir_p(client);

    /* (a) Erkennung + Hinweis einmalig */
    mk_crash(crash, "20261115-060000-hang", "hang", 0);
    mk_crash(crash, "20261115-070000-crash", "fatal", 0);
    mk_crash(crash, "20261115-080000-crash", "first-chance", 1); /* gefangen -> zaehlt nicht */
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\not-a-report", crash);
    lcfg_mkdir_p(p);
    if (!ld_crash_new(crash, &e) || strcmp(e.name, "20261115-070000-crash") != 0 || !e.fatal || !e.has_dump) {
        ok = 0;
        _snprintf_s(why, sizeof why, _TRUNCATE, "neuester zaehlender Bericht '%s'", e.name);
    }
    if (ok && (!ld_crash_mark_seen(crash, e.name) || ld_crash_new(crash, &e))) {
        ok = 0;
        strcpy_s(why, sizeof why, "nach dem Hinweis erneut gemeldet");
    }
    if (ok) {
        mk_crash(crash, "20261115-090000-hang", "hang", 0); /* neuer Haenger ohne Erholung */
        if (!ld_crash_new(crash, &e) || strcmp(e.name, "20261115-090000-hang") != 0 || e.kind != SB_CRASH_KIND_HANG) {
            ok = 0;
            strcpy_s(why, sizeof why, "neuer Haenger nicht erkannt");
        }
        ld_crash_mark_seen(crash, "20261115-090000-hang");
        if (ok && ld_crash_new(crash, &e)) {
            ok = 0;
            strcpy_s(why, sizeof why, "Haenger zweimal gemeldet");
        }
    }
    if (ok && (sb_diag_crash_name_ok("not-a-report") || !sb_diag_crash_name_ok("20261115-090000-001-crash") ||
               sb_diag_crash_name_ok("20261115-090000-crash.zip"))) {
        ok = 0;
        strcpy_s(why, sizeof why, "Ordnernamen-Pruefung");
    }
    if (ok) {
        printf("PASS W279 crash notice (newest fatal/hang report, recovered first-chance ignored, once per report)\n");
        (*pass)++;
    } else {
        printf("FAIL W279 crash notice: %s\n", why);
        (*fail)++;
    }

    /* (b) Issue-Link + Bericht-ZIP */
    ok = 1;
    {
        char url[8192], zip[LD_P], names[2048];
        if (!ld_crash_zip(crash, "20261115-070000-crash", zip, sizeof zip) ||
            zip_names(zip, names, sizeof names, NULL, NULL) != 3 || !strstr(names, "20261115-070000-crash/info.txt") ||
            !strstr(names, "20261115-070000-crash/crash_1.dmp")) {
            ok = 0;
            strcpy_s(why, sizeof why, "Bericht-ZIP");
        }
        if (ok && (!ld_issue_url(crash, "20261115-070000-crash", "20261115-070000-crash.zip", url, sizeof url) ||
                   strncmp(url, "https://github.com/" SB_FEEDBACK_REPO_DEFAULT "/issues/new?template=bug_report.yml&title=%5BCrash%5D",
                           strlen("https://github.com/" SB_FEEDBACK_REPO_DEFAULT
                                  "/issues/new?template=bug_report.yml&title=%5BCrash%5D")) != 0 ||
                   !strstr(url, "20261115-070000-crash.zip") || !strstr(url, "&version=") || strstr(url, "&files="))) {
            ok = 0;
            _snprintf_s(why, sizeof why, _TRUNCATE, "Issue-Link '%.150s'", url);
        }
        if (ok && (ld_url_ok("https://github.com/evil/x/issues/new?a=b") ||
                   ld_url_ok("https://github.com/" SB_FEEDBACK_REPO_DEFAULT "/issues/new?a=b c") ||
                   ld_url_ok("http://github.com/" SB_FEEDBACK_REPO_DEFAULT "/issues/new?a=b") ||
                   ld_url_ok("https://github.com/" SB_FEEDBACK_REPO_DEFAULT "/issues/new?a=<b>"))) {
            ok = 0;
            strcpy_s(why, sizeof why, "fremde/verbogene URL angenommen");
        }
        if (ok && ld_issue_url(crash, "..\\..\\x", "", url, sizeof url)) {
            ok = 0;
            strcpy_s(why, sizeof why, "Ordnername mit Pfad angenommen");
        }
    }
    if (ok) {
        printf("PASS W279 report on GitHub (report ZIP, prefilled bug_report.yml link without attachments, URL check)\n");
        (*pass)++;
    } else {
        printf("FAIL W279 report on GitHub: %s\n", why);
        (*fail)++;
    }

    /* (c) Bereinigung (Testpfade mit Benutzername) + Eingabe-Zeilen */
    ok = 1;
    {
        wchar_t old_u[260] = L"", old_p[600] = L"";
        DWORD hu = GetEnvironmentVariableW(L"USERNAME", old_u, 260), hp = GetEnvironmentVariableW(L"USERPROFILE", old_p, 600);
        char s[2048], raw[2048];
        sb_diag_root_t rr[1];
        int n;
        SetEnvironmentVariableW(L"USERNAME", L"W279TestUser");
        SetEnvironmentVariableW(L"USERPROFILE", L"C:\\Users\\W279TestUser");
        rr[0].path = "C:\\Users\\W279TestUser\\AppData\\Local\\Steelbound";
        rr[0].tag = "<data>";
        strcpy_s(s, sizeof s,
                 "[1.0] [INFO ] [T1] cfg: C:\\Users\\W279TestUser\\AppData\\Local\\Steelbound\\user.cfg geladen\n"
                 "[1.1] [INFO ] [T1] INPUT: Taste W gedrueckt\n"
                 "[1.2] [INFO ] [T1] ISO: D:/Games/W279TestUser/Steel Battalion.iso offen von w279testuser\n"
                 "[1.3] [INFO ] [T1] MOUSE: dx=3 dy=-1\n"
                 "[1.4] [INFO ] [T1] watchdog: 5s, Frames=150\n");
        strcpy_s(raw, sizeof raw, s);
        n = sb_diag_drop_input_lines(s);
        sb_diag_sanitize(s, sizeof s, rr, 1);
        if (n != 2 || strstr(s, "INPUT:") || strstr(s, "MOUSE:") || !strstr(s, "watchdog: 5s") ||
            !strstr(s, "<data>\\user.cfg") || strstr(s, "W279TestUser") || strstr(s, "w279testuser") ||
            sb_diag_clean_hits(s) != 0) {
            ok = 0;
            _snprintf_s(why, sizeof why, _TRUNCATE, "Text n=%d '%.200s'", n, s);
        }
        /* (d) Diagnosepaket mit Testdaten */
        if (ok) {
            ld_pkg_t r;
            char names[8192], q[LD_P];
            unsigned char *all = NULL;
            unsigned alen = 0;
            static const char *const want[] = { "README.txt", "system.txt", "settings_changed.txt", "packs.txt",
                                                "bindings/w279test.ini", "logs/sb_harness.log", "logs/sb_harness.prev.log",
                                                "logs/sb_harness.prev2.log" };
            size_t w;
            _snprintf_s(q, sizeof q, _TRUNCATE, "%s\\sb_native.cfg", client);
            wr_file(q, "width=640\nlang=en\n", 18);
            _snprintf_s(q, sizeof q, _TRUNCATE, "%s\\user.cfg", client);
            {
                const char *u = "width=1280\nlang=en\ninput_profile=w279test\n"
                                "game_image=C:\\Users\\W279TestUser\\Games\\Steel Battalion (Europe).iso\n"
                                "replay_steps=10:a;20:b\n";
                wr_file(q, u, strlen(u));
            }
            _snprintf_s(q, sizeof q, _TRUNCATE, "%s\\profiles", client);
            lcfg_mkdir_p(q);
            _snprintf_s(q, sizeof q, _TRUNCATE, "%s\\profiles\\w279test.ini", client);
            wr_file(q, "[keys]\nfire=MOUSE1\n", 19);
            _snprintf_s(q, sizeof q, _TRUNCATE, "%s\\HDD\\TDATA\\43430002", client); /* Spielstand: nie im Paket */
            lcfg_mkdir_p(q);
            _snprintf_s(q, sizeof q, _TRUNCATE, "%s\\HDD\\TDATA\\43430002\\user00.sav", client);
            wr_file(q, "SAVE", 4);
            _snprintf_s(q, sizeof q, _TRUNCATE, "%s\\sb_harness.log", logs);
            wr_file(q, raw, strlen(raw));
            {
                const char *l2 = "[2.0] [INFO ] [T1] KEY: F11\n[2.1] [INFO ] [T1] out C:\\Users\\W279TestUser\\x.txt\n";
                _snprintf_s(q, sizeof q, _TRUNCATE, "%s\\sb_harness.prev.log", logs);
                wr_file(q, l2, strlen(l2));
                _snprintf_s(q, sizeof q, _TRUNCATE, "%s\\sb_harness.prev2.log", logs);
                wr_file(q, l2, strlen(l2));
            }
            _snprintf_s(q, sizeof q, _TRUNCATE, "%s\\data", root);
            ld_test_dirs(crash, logs, q);
            {
                char uc[LD_P];
                lcfg_user_cfg_path(client, uc, sizeof uc); /* Selbsttest: legacy -> <client>/user.cfg */
            }
            if (!ld_pkg_build(client, NULL, &r)) {
                ok = 0;
                strcpy_s(why, sizeof why, "Paket nicht geschrieben");
            } else if (zip_names(r.zip, names, sizeof names, &all, &alen) < 10) {
                ok = 0;
                _snprintf_s(why, sizeof why, _TRUNCATE, "zu wenige Eintraege '%.200s'", names);
            } else {
                for (w = 0; w < sizeof want / sizeof want[0] && ok; w++) {
                    if (!strstr(names, want[w])) {
                        ok = 0;
                        _snprintf_s(why, sizeof why, _TRUNCATE, "fehlt %s", want[w]);
                    }
                }
                /* die 3 neuesten Berichte (von 4), keine Spielstaende */
                if (ok && (!strstr(names, "crash/20261115-090000-hang/") || !strstr(names, "crash/20261115-080000-crash/") ||
                           !strstr(names, "crash/20261115-070000-crash/") || strstr(names, "20261115-060000-hang") ||
                           strstr(names, ".sav") || strstr(names, "TDATA") || r.crashes != 3 || r.logs != 3 ||
                           !r.profile)) {
                    ok = 0;
                    _snprintf_s(why, sizeof why, _TRUNCATE, "Auswahl Berichte/Spielstaende '%.300s'", names);
                }
                if (ok && (mem_has(all, alen, "W279TestUser") || mem_has(all, alen, "INPUT:") ||
                           mem_has(all, alen, "KEY: F11") || mem_has(all, alen, "MOUSE:") || mem_has(all, alen, "10:a;20:b") ||
                           !mem_has(all, alen, "width=1280") || !mem_has(all, alen, "Games\\Steel Battalion (Europe).iso") ||
                           !mem_has(all, alen, "fire=MOUSE1") || r.clean_hits != 0 || r.dropped < 4)) {
                    ok = 0;
                    _snprintf_s(why, sizeof why, _TRUNCATE, "Inhalt: Name %d Eingaben %d/%d width %d iso %d hits %d dropped %d",
                                mem_has(all, alen, "W279TestUser"), mem_has(all, alen, "INPUT:"),
                                mem_has(all, alen, "KEY: F11"), mem_has(all, alen, "width=1280"),
                                mem_has(all, alen, "Games\\Steel Battalion (Europe).iso"), r.clean_hits, r.dropped);
                }
                if (ok && !mem_has(all, alen, "cpu: ")) {
                    ok = 0;
                    strcpy_s(why, sizeof why, "system.txt ohne CPU");
                }
                printf("     (W279 d: Paket %d Dateien, %llu Bytes, %d Ersetzungen, %d Eingabe-Zeilen entfernt)\n",
                       r.files, r.bytes, r.repl, r.dropped);
            }
            ld_test_dirs(NULL, NULL, NULL);
            /* echter Benutzername ebenfalls nicht im Paket (Umgebung zurueck) */
            if (hu) { SetEnvironmentVariableW(L"USERNAME", old_u); } else { SetEnvironmentVariableW(L"USERNAME", NULL); }
            if (hp) { SetEnvironmentVariableW(L"USERPROFILE", old_p); } else { SetEnvironmentVariableW(L"USERPROFILE", NULL); }
            if (ok && all) {
                char *zt = (char *)malloc((size_t)alen + 1);
                if (zt) {
                    unsigned i;
                    for (i = 0; i < alen; i++) { zt[i] = all[i] ? (char)all[i] : ' '; }
                    zt[alen] = '\0';
                    if (sb_diag_clean_hits(zt) != 0) {
                        ok = 0;
                        strcpy_s(why, sizeof why, "echter Benutzername/Profilpfad im Paket");
                    }
                    free(zt);
                }
            }
            free(all);
        }
        if (hu) { SetEnvironmentVariableW(L"USERNAME", old_u); }
        if (hp) { SetEnvironmentVariableW(L"USERPROFILE", old_p); }
    }
    if (ok) {
        printf("PASS W279 diagnostic package (system/versions/settings/packs/bindings, 3 logs, 3 newest reports, no saves, "
               "test paths with user name replaced, input lines removed)\n");
        (*pass)++;
    } else {
        printf("FAIL W279 diagnostic package: %s\n", why);
        (*fail)++;
    }
    rm_tree(root);
}
