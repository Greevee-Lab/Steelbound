/* sb_crash.c - W279 DIAG-CRASH: Absturz-/Haenger-Berichte mit Minidump.
 * Beschreibung s. sb_crash.h. Wird nur bei Absturz, Haenger oder Test-
 * Schalter aktiv; im normalen Spiel nur die festen Angaben beim Start. */
#include "render_int.h"
#include <dbghelp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "hlog.h"
#include "hcfg.h"
#include "sb_crash.h"
#include "sb_diagcore.h"
#include "sb_edition.h"
#include "sb_fpsrate.h" /* W309: monitor_hz */
#include "sb_texhealth.h" /* W315 */
#include "sb_pack.h"
#include "sb_pause.h"
#include "sb_profile.h"
#include "sb_state.h"
#include "sb_userdata.h"
#include "sb_watermark.h"
#include "kernel_hle.h"
#include "xdk_hooks.h"

#pragma comment(lib, "dbghelp.lib")

extern void wd_sym_init(void); /* emu_main.c (SymInitialize einmal je Prozess) */

/* ---- cfg ---- */
static int g_dump_on = 1, g_hang_on = 1, g_hang_secs = 20, g_crash_test, g_hang_test;

int sb_crash_cfg_key(const char *k, const char *v)
{
    if (strcmp(k, "crash_dump") == 0) { g_dump_on = atoi(v); }
    else if (strcmp(k, "hang_report") == 0) { g_hang_on = atoi(v); }
    else if (strcmp(k, "hang_secs") == 0) { g_hang_secs = atoi(v) < 10 ? 10 : atoi(v); }
    else if (strcmp(k, "crash_test") == 0) { g_crash_test = atoi(v); } /* TEST(W279) */
    else if (strcmp(k, "hang_test") == 0) { g_hang_test = atoi(v); }   /* TEST(W279) */
    else { return 0; }
    return 1;
}

/* ---- Zustand ---- */
static char g_root[600];          /* <crash> (ACP) */
static char g_root_data[1100];    /* Datenordner (UTF-8) fuer <data> */
static char g_root_client[1100];  /* Client-Ordner (UTF-8) fuer <client> */
static sb_diag_root_t g_roots[2];
static int g_nroots;
static char g_fixed[16384];       /* feste Angaben (bereinigt) */
static char g_packs[4096];        /* Packs (bereinigt) */
static char g_cfgdiff[8192];      /* cfg-Abweichungen (bereinigt) */
static char g_crash_dir_cur[700]; /* Ordner dieser Sitzung (Absturz) */
static int g_fatal_written;
static volatile LONG g_fc_pending; /* erste Chance geschrieben, noch nicht erholt */
static unsigned g_fc_frames;
static char g_hang_dir[700];
static int g_hang_n, g_hang_open;
static char g_buf[65536 * 4];     /* Textpuffer (kein malloc im Absturz) */
static char g_tail[262144];
static char g_cmdset[8192];

/* Hilfs-Thread fuer den Minidump */
static HANDLE g_dreq, g_ddone, g_dthr;
static DWORD g_dthr_id;
static struct {
    EXCEPTION_POINTERS *ep;
    DWORD tid;
    char path[760];
    int ok;
    unsigned kb;
    DWORD err;
} g_dj;

static void acp_to_u8(const char *a, char *o, size_t cap)
{
    wchar_t w[1100];
    o[0] = '\0';
    if (MultiByteToWideChar(CP_ACP, 0, a, -1, w, 1100)) {
        WideCharToMultiByte(CP_UTF8, 0, w, -1, o, (int)cap, NULL, NULL);
    }
}

static void mkdirs(const char *path)
{
    char t[700];
    size_t i;
    strcpy_s(t, sizeof t, path);
    for (i = 3; t[i]; i++) {
        if (t[i] == '\\' || t[i] == '/') {
            char c = t[i];
            t[i] = '\0';
            CreateDirectoryA(t, NULL);
            t[i] = c;
        }
    }
    CreateDirectoryA(t, NULL);
}

static int wfile(const char *dir, const char *name, const char *txt)
{
    char p[800];
    HANDLE f;
    DWORD w = 0;
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\%s", dir, name);
    f = CreateFileA(p, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { return 0; }
    WriteFile(f, txt, (DWORD)strlen(txt), &w, NULL);
    FlushFileBuffers(f);
    CloseHandle(f);
    return 1;
}

static int san(char *s, size_t cap)
{
    return sb_diag_sanitize(s, cap, g_roots, g_nroots);
}

/* ------------------------------------------------------------------ */
/* Start                                                               */
/* ------------------------------------------------------------------ */
void sb_crash_init(const char *dir_acp, const char *cmdline_set)
{
    char t[1100], *s;
    if (dir_acp && dir_acp[0]) {
        strcpy_s(g_root, sizeof g_root, dir_acp);
    } else {
        const char *o = hcfg_out_dir();
        _snprintf_s(g_root, sizeof g_root, _TRUNCATE, "%s\\crash", o && o[0] ? o : ".");
    }
    g_nroots = 0;
    if (sb_userdata_path_peek(SB_UD_ROOT, t, sizeof t) == 1) {
        acp_to_u8(t, g_root_data, sizeof g_root_data);
        g_roots[g_nroots].path = g_root_data;
        g_roots[g_nroots++].tag = "<data>";
    }
    if (GetModuleFileNameA(NULL, t, sizeof t) && (s = strrchr(t, '\\')) != NULL) {
        *s = '\0';
        acp_to_u8(t, g_root_client, sizeof g_root_client);
        /* Datenordner im Client-Ordner (portable): laengerer Pfad zuerst */
        g_roots[g_nroots].path = g_root_client;
        g_roots[g_nroots++].tag = "<client>";
    }
    strcpy_s(g_cmdset, sizeof g_cmdset, cmdline_set ? cmdline_set : "");
}

static void pdb_id(HMODULE m, char *o, size_t cap)
{
    const IMAGE_DOS_HEADER *dh = (const IMAGE_DOS_HEADER *)m;
    const IMAGE_NT_HEADERS *nt;
    const IMAGE_DATA_DIRECTORY *dd;
    unsigned i, n;
    strcpy_s(o, cap, "none");
    if (!m) { return; }
    __try {
        nt = (const IMAGE_NT_HEADERS *)((const char *)m + dh->e_lfanew);
        dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
        n = dd->Size / sizeof(IMAGE_DEBUG_DIRECTORY);
        for (i = 0; i < n; i++) {
            const IMAGE_DEBUG_DIRECTORY *d =
                (const IMAGE_DEBUG_DIRECTORY *)((const char *)m + dd->VirtualAddress) + i;
            if (d->Type == IMAGE_DEBUG_TYPE_CODEVIEW && d->AddressOfRawData) {
                const unsigned char *cv = (const unsigned char *)m + d->AddressOfRawData;
                if (memcmp(cv, "RSDS", 4) == 0) {
                    const GUID *g = (const GUID *)(cv + 4);
                    unsigned age = *(const unsigned *)(cv + 20);
                    const char *pn = (const char *)(cv + 24), *b = strrchr(pn, '\\');
                    _snprintf_s(o, cap, _TRUNCATE, "%s %08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%X",
                                b ? b + 1 : pn, g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1],
                                g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7], age);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strcpy_s(o, cap, "?");
    }
}

static void pe_date(HMODULE m, char *o, size_t cap)
{
    strcpy_s(o, cap, "?");
    if (m) {
        const IMAGE_DOS_HEADER *dh = (const IMAGE_DOS_HEADER *)m;
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)m + dh->e_lfanew);
        time_t tt = (time_t)nt->FileHeader.TimeDateStamp;
        struct tm g;
        if (gmtime_s(&g, &tt) == 0) {
            _snprintf_s(o, cap, _TRUNCATE, "%04d-%02d-%02d %02d:%02d UTC (0x%08lX)", g.tm_year + 1900, g.tm_mon + 1,
                        g.tm_mday, g.tm_hour, g.tm_min, nt->FileHeader.TimeDateStamp);
        }
    }
}

static void gpu_desc(char *o, size_t cap)
{
    IDXGIDevice *dx = NULL;
    IDXGIAdapter *ad = NULL;
    DXGI_ADAPTER_DESC d;
    strcpy_s(o, cap, "?");
    if (!g_dev || FAILED(ID3D11Device_QueryInterface(g_dev, &IID_IDXGIDevice, (void **)&dx)) || !dx) { return; }
    if (SUCCEEDED(IDXGIDevice_GetAdapter(dx, &ad)) && ad) {
        if (SUCCEEDED(IDXGIAdapter_GetDesc(ad, &d))) {
            char nm[160], drv[48] = "?";
            LARGE_INTEGER umd;
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, nm, sizeof nm, NULL, NULL);
            if (SUCCEEDED(IDXGIAdapter_CheckInterfaceSupport(ad, &IID_IDXGIDevice, &umd))) {
                _snprintf_s(drv, sizeof drv, _TRUNCATE, "%u.%u.%u.%u", (unsigned)(umd.HighPart >> 16),
                            (unsigned)(umd.HighPart & 0xFFFF), (unsigned)(umd.LowPart >> 16),
                            (unsigned)(umd.LowPart & 0xFFFF));
            }
            /* VRAM nicht: DedicatedVideoMemory ist im 32-Bit-Prozess auf 4 GB gekappt (falsche Zahl) */
            _snprintf_s(o, cap, _TRUNCATE, "%s (vendor 0x%04X device 0x%04X, driver %s)", nm, d.VendorId, d.DeviceId,
                        drv);
        }
        IDXGIAdapter_Release(ad);
    }
    IDXGIDevice_Release(dx);
}

HWND sb_render_hwnd(void); /* d3d11_render.c */

/* W309 FPS-MONITOR: Rate des Spielmonitors (Fenster, sonst Hauptmonitor; 0 = unbekannt).
 * vblank_hz ist die Xbox-Zeitbasis, nicht der Monitor. */
static int crash_monitor_hz(void)
{
    HWND h = sb_render_hwnd();
    return sb_fps_monitor_hz(h ? MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY) : NULL);
}

void sb_crash_note_game(const sb_gamever_t *gv)
{
    char build[64], pdb_emu[200], pdb_ldr[200], gpu[300], game[300];
    const char *fl = "EU";
    size_t used;
    pe_date(GetModuleHandleA("sb_emu.dll"), build, sizeof build);
    pdb_id(GetModuleHandleA("sb_emu.dll"), pdb_emu, sizeof pdb_emu);
    pdb_id(GetModuleHandleA(NULL), pdb_ldr, sizeof pdb_ldr);
    gpu_desc(gpu, sizeof gpu);
    if (gv) {
        fl = gv->loc ? "LoC" : gv->ntsc_u ? "US" : gv->supported ? "EU" : "unknown";
        _snprintf_s(game, sizeof game, _TRUNCATE, "%s [%s] title=0x%08X region=%u version=0x%X supported=%d sha256=%.16s",
                    gv->label[0] ? gv->label : "?", fl, gv->title_id, gv->region, gv->version, gv->supported,
                    gv->sha256);
    } else {
        strcpy_s(game, sizeof game, "?");
    }
    _snprintf_s(g_fixed, sizeof g_fixed, _TRUNCATE,
                "client: Steelbound %s edition, version %s, sb_emu.dll built %s\r\n"
                "build-id: %s | loader %s\r\n"
                "game: %s\r\n"
                "gpu: %s\r\n"
                "settings: %ux%u window_mode=%d interp_fps=%d vblank_hz=%d monitor_hz=%d shading=%d monitor_fx=%d edge_mode=%d "
                "aa_mode=%d remaster=%d hd_textures=%d vr_enable=%d lang=%s input_profile=%s\r\n"
                "data-location: %s (%s)\r\n",
                SB_EDITION_NAME, sb_version_str()[0] ? sb_version_str() : "dev", build, pdb_emu, pdb_ldr, game, gpu,
                g_cfg.width, g_cfg.height, g_cfg.window_mode, g_cfg.interp_fps, g_cfg.vblank_hz, crash_monitor_hz(), g_cfg.shading,
                g_cfg.monitor_fx, g_cfg.edge_mode, g_cfg.aa_mode, g_cfg.remaster, g_cfg.hd_textures, g_cfg.vr_enable,
                g_cfg.lang[0] ? g_cfg.lang : "en", g_cfg.input_profile[0] ? g_cfg.input_profile : "(legacy)",
                sb_userdata_mode_name(sb_userdata_mode()), sb_userdata_mode_source());
    san(g_fixed, sizeof g_fixed);
    /* cfg-Abweichungen: user.cfg (sofern geladen) + Start-Kette + SB_CFG_SET */
    {
        char base[1100] = "sb_native.cfg", chain[17000], env[8192];
        const char *uc = getenv("SB_NO_USERCFG") ? NULL : hcfg_user_cfg_path();
        DWORD n;
        int nl;
        /* Reihenfolge wie hcfg: user.cfg -> SB_CFG_SET -> Start-Parameter --set */
        chain[0] = '\0';
        n = GetEnvironmentVariableA("SB_CFG_SET", env, sizeof env);
        if (n > 0 && n < sizeof env) { strcpy_s(chain, sizeof chain, env); }
        if (g_cmdset[0]) {
            if (chain[0]) { strcat_s(chain, sizeof chain, ";"); }
            strcat_s(chain, sizeof chain, g_cmdset);
        }
        {
            char exe[1100], *s;
            if (GetModuleFileNameA(NULL, exe, sizeof exe) && (s = strrchr(exe, '\\')) != NULL) {
                *s = '\0';
                _snprintf_s(base, sizeof base, _TRUNCATE, "%s\\sb_native.cfg", exe);
            }
        }
        {
            char b8[1100], u8[1100];
            acp_to_u8(base, b8, sizeof b8);
            if (uc) { acp_to_u8(uc, u8, sizeof u8); }
            nl = sb_diag_cfg_diff(b8, uc ? u8 : NULL, chain, g_cfgdiff, sizeof g_cfgdiff);
        }
        used = strlen(g_cfgdiff);
        if (nl == 0) { strcpy_s(g_cfgdiff, sizeof g_cfgdiff, "(none - all values as in sb_native.cfg)\r\n"); }
        (void)used;
        san(g_cfgdiff, sizeof g_cfgdiff);
        sb_diag_drop_input_lines(g_cfgdiff); /* keine Eingabe-Folgen (z. B. Replay-Schritte) */
    }
    HLOG_I("CRASH: Berichte nach %s (Minidump %s, Haenger-Bericht %s nach %d s)", g_root, g_dump_on ? "an" : "aus",
           g_hang_on ? "an" : "aus", g_hang_secs);
}

/* W279: von sb_remaster.c beim Laden der Packs (Pfad der packs.json). */
void sb_crash_note_packs(const char *packs_json)
{
    static sb_pack_scan_t sc[24];
    char dir[1100], *s;
    int n, i;
    size_t used = 0;
    g_packs[0] = '\0';
    if (!packs_json || !packs_json[0]) {
        strcpy_s(g_packs, sizeof g_packs, "(none)\r\n");
        return;
    }
    n = sb_pack_scan(packs_json, sc, 24);
    strcpy_s(dir, sizeof dir, packs_json);
    s = strrchr(dir, '\\');
    if (!s || (strrchr(dir, '/') && strrchr(dir, '/') > s)) { s = strrchr(dir, '/'); }
    if (s) { *s = '\0'; } else { strcpy_s(dir, sizeof dir, "."); }
    for (i = 0; i < n && i < 24; i++) {
        char full[1300], fp[80] = "?";
        int r;
        if (!sc[i].enabled) { continue; }
        if ((strlen(sc[i].src) > 1 && sc[i].src[1] == ':') || sc[i].src[0] == '\\' || sc[i].src[0] == '/') {
            strcpy_s(full, sizeof full, sc[i].src);
        } else {
            _snprintf_s(full, sizeof full, _TRUNCATE, "%s\\%s", dir, sc[i].src);
        }
        r = sb_diag_pack_fp(full, fp, sizeof fp);
        _snprintf_s(g_packs + used, sizeof g_packs - used, _TRUNCATE, "%s %s \"%s\" type=%s %s=%.16s\r\n", sc[i].id,
                    sc[i].version, sc[i].name, sc[i].type, r == 2 ? "manifest-sha256" : "cd-sha256",
                    r ? fp : "unreadable");
        used = strlen(g_packs);
    }
    if (!used) { strcpy_s(g_packs, sizeof g_packs, n < 0 ? "(packs.json unreadable)\r\n" : "(none active)\r\n"); }
    san(g_packs, sizeof g_packs);
}

/* ------------------------------------------------------------------ */
/* Minidump                                                            */
/* ------------------------------------------------------------------ */
static void dump_now(void)
{
    HANDLE f;
    MINIDUMP_EXCEPTION_INFORMATION mei;
    LARGE_INTEGER sz;
    g_dj.ok = 0;
    g_dj.kb = 0;
    f = CreateFileA(g_dj.path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        g_dj.err = GetLastError();
        return;
    }
    mei.ThreadId = g_dj.tid;
    mei.ExceptionPointers = g_dj.ep;
    mei.ClientPointers = FALSE;
    g_dj.ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                                (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory),
                                g_dj.ep ? &mei : NULL, NULL, NULL) ? 1 : 0;
    g_dj.err = g_dj.ok ? 0 : GetLastError();
    if (GetFileSizeEx(f, &sz)) { g_dj.kb = (unsigned)(sz.QuadPart / 1024); }
    CloseHandle(f);
}

static DWORD WINAPI dump_thread(LPVOID p)
{
    (void)p;
    for (;;) {
        if (WaitForSingleObject(g_dreq, INFINITE) != WAIT_OBJECT_0) { return 0; }
        dump_now();
        SetEvent(g_ddone);
    }
}

void sb_crash_start(void)
{
    if (!g_dump_on || g_dthr) { return; }
    g_dreq = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_ddone = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!g_dreq || !g_ddone) { return; }
    g_dthr = (HANDLE)krnl_CreateThread((void *)dump_thread, NULL, 0, &g_dthr_id);
}

/* Minidump schreiben (Hilfs-Thread, sonst direkt) und bereinigen. */
static void write_dump(const char *path, EXCEPTION_POINTERS *ep, char *note, size_t ncap)
{
    char u8[1100];
    int scrub;
    strcpy_s(g_dj.path, sizeof g_dj.path, path);
    g_dj.ep = ep;
    g_dj.tid = GetCurrentThreadId();
    if (g_dthr && GetCurrentThreadId() != g_dthr_id) {
        ResetEvent(g_ddone);
        SetEvent(g_dreq);
        if (WaitForSingleObject(g_ddone, 30000) != WAIT_OBJECT_0) {
            _snprintf_s(note, ncap, _TRUNCATE, "minidump: timeout");
            return;
        }
    } else {
        dump_now();
    }
    acp_to_u8(path, u8, sizeof u8);
    scrub = g_dj.ok ? sb_diag_dump_scrub(u8) : -1;
    _snprintf_s(note, ncap, _TRUNCATE,
                "minidump: %s, %u KB (MiniDumpWithIndirectlyReferencedMemory|MiniDumpScanMemory, no full memory), "
                "scrubbed %d (module paths -> file names, user/computer name)",
                g_dj.ok ? "ok" : "FAILED", g_dj.kb, scrub);
    if (!g_dj.ok) {
        _snprintf_s(note + strlen(note), ncap - strlen(note), _TRUNCATE, " err=%lu", g_dj.err);
        DeleteFileA(path);
    }
}

/* ------------------------------------------------------------------ */
/* Bausteine                                                           */
/* ------------------------------------------------------------------ */
static unsigned rd32(unsigned a)
{
    unsigned v = 0;
    __try { v = *(volatile unsigned *)(uintptr_t)a; }
    __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
    return v;
}

static void state_line(char *o, size_t cap)
{
    if (g_sba_loc) {
        _snprintf_s(o, cap, _TRUNCATE, "state: Line of Contact profile (no SB state), frames=%u present=%u",
                    hooks_frame_count(), g_present_no);
        return;
    }
    _snprintf_s(o, cap, _TRUNCATE,
                "state: scene=0x%X loadcount=%u in_mission=%d mission=%u vt=%u countdown=%u frames=%u present=%u "
                "paused=%ld",
                rd32(SBA(0x6B96DCu)), sb_state_loadcount(), sb_in_mission(), rd32(SBA(0x3B89A0u)) & 0xFFFFu,
                rd32(SBA(0x3B8AA8u)) & 0xFFFFu, rd32(SBA(0x3563F0u)), hooks_frame_count(), g_present_no,
                g_sb_paused);
}

/* Log-Ende ohne Eingabe-Zeilen, bereinigt -> log_tail.txt. */
static void write_tail(const char *dir)
{
    const char *lp = hlog_path();
    HANDLE h;
    LARGE_INTEGER sz, off;
    DWORD got = 0, want;
    int dropped, lines = 0;
    char *st, head[200];
    g_tail[0] = '\0';
    if (lp && lp[0]) {
        h = CreateFileA(lp, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            if (GetFileSizeEx(h, &sz)) {
                want = (DWORD)(sz.QuadPart > 131072 ? 131072 : sz.QuadPart);
                off.QuadPart = sz.QuadPart - want;
                if (SetFilePointerEx(h, off, NULL, FILE_BEGIN) && ReadFile(h, g_tail, want, &got, NULL)) {
                    g_tail[got] = '\0';
                    if (off.QuadPart > 0 && (st = strchr(g_tail, 10)) != NULL) {
                        memmove(g_tail, st + 1, strlen(st + 1) + 1);
                    }
                }
            }
            CloseHandle(h);
        }
    }
    dropped = sb_diag_drop_input_lines(g_tail);
    san(g_tail, sizeof g_tail);
    for (st = g_tail; *st; st++) { lines += *st == 10; }
    _snprintf_s(head, sizeof head, _TRUNCATE,
                "# end of the client log (%d lines; %d input lines removed; user/computer name and paths replaced)\r\n",
                lines, dropped);
    {
        char p[800];
        HANDLE f;
        DWORD w;
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\log_tail.txt", dir);
        f = CreateFileA(p, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            WriteFile(f, head, (DWORD)strlen(head), &w, NULL);
            WriteFile(f, g_tail, (DWORD)strlen(g_tail), &w, NULL);
            CloseHandle(f);
        }
    }
}

/* Neuer Ordner <root>\<stamp>[-nnn]-<kind>; name = Ordnername. */
static int new_dir(const char *kind, char *out, size_t cap)
{
    SYSTEMTIME lt;
    char stamp[32];
    unsigned k;
    GetLocalTime(&lt);
    _snprintf_s(stamp, sizeof stamp, _TRUNCATE, "%04u%02u%02u-%02u%02u%02u", lt.wYear, lt.wMonth, lt.wDay, lt.wHour,
                lt.wMinute, lt.wSecond);
    mkdirs(g_root);
    for (k = 0; k < 100; k++) {
        if (k == 0) {
            _snprintf_s(out, cap, _TRUNCATE, "%s\\%s-%s", g_root, stamp, kind);
        } else {
            _snprintf_s(out, cap, _TRUNCATE, "%s\\%s-%03u-%s", g_root, stamp, k, kind);
        }
        if (CreateDirectoryA(out, NULL)) { return 1; }
        if (GetLastError() != ERROR_ALREADY_EXISTS) { return 0; }
    }
    return 0;
}

static void write_info(const char *dir, const char *kind, const char *status, const char *reason, const char *exc,
                       const char *dumpnote)
{
    SYSTEMTIME lt;
    char st[400];
    static char th[4096]; /* W315: Textur-Gesundheit (letzter Stand, keine DXGI-Abfrage im Absturz); W360: 3072 -> 4096 */
    GetLocalTime(&lt);
    state_line(st, sizeof st);
    __try { sb_th_format(th, sizeof th, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(th, sizeof th, "[texture health] (unavailable)\r\n"); }
    _snprintf_s(g_buf, sizeof g_buf, _TRUNCATE,
                "Steelbound %s report (W279)\r\n"
                "status: %s\r\n"
                "created: %04u-%02u-%02u %02u:%02u:%02u (local time)\r\n"
                "reason: %s\r\n"
                "%s%s"
                "%s\r\n"
                "%s"
                "%s%s"
                "\r\n%s"
                "\r\n[active packs]\r\n%s"
                "\r\n[settings changed from sb_native.cfg]\r\n%s"
                "\r\n[files]\r\n"
                "%s = this file; log_tail.txt = end of the client log (input lines removed); status.txt;\r\n"
                "%s\r\n"
                "Nothing has been uploaded. User name, computer name and paths are replaced; the minidump holds\r\n"
                "only thread stacks and the memory they point to (no full memory, no game files, no saves).\r\n",
                kind, status, lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond, reason,
                exc && exc[0] ? exc : "", exc && exc[0] ? "\r\n" : "", st,
                g_fixed[0] ? g_fixed : "client: (start not finished - no fixed details yet)\r\n",
                dumpnote && dumpnote[0] ? dumpnote : "", dumpnote && dumpnote[0] ? "\r\n" : "", th,
                g_packs[0] ? g_packs : "(none loaded)\r\n", g_cfgdiff[0] ? g_cfgdiff : "(not known yet)\r\n",
                "info.txt",
                strcmp(kind, "hang") == 0 ? "hang.txt = stacks of all game threads; hang.dmp = minidump"
                                          : "crash_N.txt = exception, registers, stack; crash_N.dmp = minidump");
    san(g_buf, sizeof g_buf);
    wfile(dir, "info.txt", g_buf);
    wfile(dir, "status.txt", status);
}

/* ------------------------------------------------------------------ */
/* Absturz                                                             */
/* ------------------------------------------------------------------ */
int sb_crash_report(EXCEPTION_POINTERS *ep, const char *why, const char *text, int k, int fatal, char *out_path,
                    size_t cap)
{
    char nm[64], dumpnote[400] = "", exc[400];
    EXCEPTION_RECORD *er = ep ? ep->ExceptionRecord : NULL;
    if (out_path && cap) { out_path[0] = '\0'; }
    if (!g_root[0]) { sb_crash_init(NULL, NULL); }
    if (!g_crash_dir_cur[0] && !new_dir("crash", g_crash_dir_cur, sizeof g_crash_dir_cur)) {
        g_crash_dir_cur[0] = '\0';
        return 0;
    }
    /* Text (bereinigt, ohne Eingabe-Zeilen aus den letzten Logzeilen) */
    strcpy_s(g_buf, sizeof g_buf, text ? text : "");
    sb_diag_drop_input_lines(g_buf);
    san(g_buf, sizeof g_buf);
    _snprintf_s(nm, sizeof nm, _TRUNCATE, "crash_%d.txt", k);
    wfile(g_crash_dir_cur, nm, g_buf);
    if (out_path && cap) { _snprintf_s(out_path, cap, _TRUNCATE, "%s\\%s", g_crash_dir_cur, nm); }
    if (g_dump_on && k <= 2) {
        char dp[800];
        _snprintf_s(dp, sizeof dp, _TRUNCATE, "%s\\crash_%d.dmp", g_crash_dir_cur, k);
        write_dump(dp, ep, dumpnote, sizeof dumpnote);
    }
    {
        uintptr_t rip = er ? (uintptr_t)er->ExceptionAddress : 0;
        HMODULE hm = NULL;
        char mod[MAX_PATH] = "?";
        const char *mb = mod;
        if (rip && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      (LPCSTR)rip, &hm) && hm && GetModuleFileNameA(hm, mod, sizeof mod)) {
            const char *s = strrchr(mod, '\\');
            mb = s ? s + 1 : mod;
        }
        _snprintf_s(exc, sizeof exc, _TRUNCATE, "exception: code=0x%08lX at 0x%08X (%s+0x%X)%s thread=T%04lx",
                    er ? er->ExceptionCode : 0, (unsigned)rip, hm ? mb : "no module",
                    (unsigned)(rip - (uintptr_t)hm), (rip >= 0x10000u && rip < 0x2810000u) ? " game code" : "",
                    GetCurrentThreadId());
    }
    if (fatal) { g_fatal_written = 1; }
    write_info(g_crash_dir_cur, "crash", g_fatal_written ? "fatal" : "first-chance", why, exc, dumpnote);
    write_tail(g_crash_dir_cur);
    if (!g_fatal_written) {
        g_fc_frames = hooks_frame_count();
        InterlockedExchange(&g_fc_pending, 1);
    } else {
        InterlockedExchange(&g_fc_pending, 0);
        {
            char rp[800];
            _snprintf_s(rp, sizeof rp, _TRUNCATE, "%s\\recovered.txt", g_crash_dir_cur);
            DeleteFileA(rp);
        }
    }
    HLOG_E("CRASH: Bericht %s (%s, %s)", g_crash_dir_cur, g_fatal_written ? "fatal" : "erste Chance",
           dumpnote[0] ? dumpnote : "ohne Minidump");
    return 1;
}

/* ------------------------------------------------------------------ */
/* Haenger                                                             */
/* ------------------------------------------------------------------ */
static void hang_stacks(char *o, size_t cap)
{
    extern int krnl_xb_thread_count(void);
    int i, n = krnl_xb_thread_count();
    size_t used = strlen(o);
    wd_sym_init();
    for (i = 0; i < n && used + 256 < cap; i++) {
        void *h = NULL;
        unsigned rt = 0;
        CONTEXT cx;
        STACKFRAME64 sf;
        int di;
        if (!krnl_xb_thread_get(i, &h, &rt) || !h) { continue; }
        if (SuspendThread((HANDLE)h) == (DWORD)-1) { continue; }
        memset(&cx, 0, sizeof cx);
        cx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext((HANDLE)h, &cx)) {
            _snprintf_s(o + used, cap - used, _TRUNCATE, "thread %d (start 0x%X): EIP=0x%08lX ESP=0x%08lX EBP=0x%08lX\r\n",
                        i, rt, cx.Eip, cx.Esp, cx.Ebp);
            used = strlen(o);
            memset(&sf, 0, sizeof sf);
            sf.AddrPC.Offset = cx.Eip;
            sf.AddrPC.Mode = AddrModeFlat;
            sf.AddrFrame.Offset = cx.Ebp;
            sf.AddrFrame.Mode = AddrModeFlat;
            sf.AddrStack.Offset = cx.Esp;
            sf.AddrStack.Mode = AddrModeFlat;
            for (di = 0; di < 24 && used + 200 < cap; di++) {
                BYTE sbuf[sizeof(SYMBOL_INFO) + 96];
                SYMBOL_INFO *si = (SYMBOL_INFO *)sbuf;
                DWORD64 disp = 0;
                MEMORY_BASIC_INFORMATION mbi;
                char fm[MAX_PATH] = "";
                const char *bn = "?";
                if (!StackWalk64(IMAGE_FILE_MACHINE_I386, GetCurrentProcess(), (HANDLE)h, &sf, &cx, NULL,
                                 SymFunctionTableAccess64, SymGetModuleBase64, NULL) ||
                    !sf.AddrPC.Offset) {
                    break;
                }
                if (sf.AddrPC.Offset >= 0x10000u && sf.AddrPC.Offset < 0x2810000u) {
                    bn = "game"; /* Xbox-VA (liegt im Abbild von sb_ldr.exe), Namen: symbolize.py */
                } else if (VirtualQuery((void *)(uintptr_t)sf.AddrPC.Offset, &mbi, sizeof mbi) &&
                           GetModuleFileNameA((HMODULE)mbi.AllocationBase, fm, sizeof fm)) {
                    bn = strrchr(fm, '\\');
                    bn = bn ? bn + 1 : fm;
                }
                si->SizeOfStruct = sizeof(SYMBOL_INFO);
                si->MaxNameLen = 96;
                if (SymFromAddr(GetCurrentProcess(), sf.AddrPC.Offset, &disp, si)) {
                    _snprintf_s(o + used, cap - used, _TRUNCATE, "  #%02d 0x%08X %s!%s+0x%X\r\n", di,
                                (unsigned)sf.AddrPC.Offset, bn, si->Name, (unsigned)disp);
                } else {
                    _snprintf_s(o + used, cap - used, _TRUNCATE, "  #%02d 0x%08X %s\r\n", di,
                                (unsigned)sf.AddrPC.Offset, bn);
                }
                used = strlen(o);
            }
        }
        ResumeThread((HANDLE)h);
    }
}

static void hang_report(int still_s)
{
    char dumpnote[400] = "", why[160], dp[800];
    if (!new_dir("hang", g_hang_dir, sizeof g_hang_dir)) {
        HLOG_W("CRASH: Haenger-Ordner nicht anlegbar");
        g_hang_dir[0] = '\0';
        return;
    }
    _snprintf_s(why, sizeof why, _TRUNCATE, "no new game frame for %d s (not paused)", still_s);
    _snprintf_s(g_buf, sizeof g_buf, _TRUNCATE, "SB-HANG: %s\r\n", why);
    hang_stacks(g_buf, sizeof g_buf);
    san(g_buf, sizeof g_buf);
    wfile(g_hang_dir, "hang.txt", g_buf);
    if (g_dump_on) {
        _snprintf_s(dp, sizeof dp, _TRUNCATE, "%s\\hang.dmp", g_hang_dir);
        write_dump(dp, NULL, dumpnote, sizeof dumpnote);
    }
    write_info(g_hang_dir, "hang", "hang", why, "", dumpnote);
    write_tail(g_hang_dir);
    HLOG_W("CRASH: Haenger-Bericht %s (%s)", g_hang_dir, dumpnote[0] ? dumpnote : "ohne Minidump");
}

static void mark_recovered(const char *dir, const char *txt)
{
    if (dir[0]) {
        wfile(dir, "recovered.txt", txt);
        HLOG_I("CRASH: %s - Spiel laeuft weiter (recovered.txt)", dir);
    }
}

void sb_crash_tick(int waited)
{
    static unsigned last_f;
    static int still_s, crash_done, hang_done;
    unsigned f = hooks_frame_count();
    /* TEST(W279): Zugriffsverletzung im Watchdog-Thread -> last_chance */
    if (g_crash_test > 0 && waited >= g_crash_test && !crash_done) {
        crash_done = 1;
        HLOG_W("CRASH-TEST: crash_test=%d -> Zugriffsverletzung (Schreiben an 0x8)", g_crash_test);
        hlog_flush();
        *(volatile int *)(uintptr_t)8 = 0x279;
    }
    /* TEST(W279): alle Spiel-Threads anhalten -> echter Haenger */
    if (g_hang_test > 0 && waited >= g_hang_test && !hang_done) {
        extern int krnl_xb_thread_count(void);
        int i, n = krnl_xb_thread_count(), ns = 0;
        hang_done = 1;
        for (i = 0; i < n; i++) {
            void *h = NULL;
            unsigned rt = 0;
            if (krnl_xb_thread_get(i, &h, &rt) && h && SuspendThread((HANDLE)h) != (DWORD)-1) { ns++; }
        }
        HLOG_W("CRASH-TEST: hang_test=%d -> %d Spiel-Threads angehalten", g_hang_test, ns);
    }
    /* erste Chance gefangen? Bilder laufen 10 s+ weiter -> erholt */
    if (g_fc_pending && f != g_fc_frames && f - g_fc_frames > 300) {
        InterlockedExchange(&g_fc_pending, 0);
        mark_recovered(g_crash_dir_cur, "The game kept running after this first-chance exception.\r\n");
    }
    /* Haenger: keine neuen Bilder, nicht pausiert (LoC-Profil ausgenommen) */
    if (f == last_f && waited > 10 && !g_sb_paused && !g_sba_loc) {
        still_s += 5;
        if (g_hang_on && still_s >= g_hang_secs && !g_hang_open && g_hang_n < 2) {
            g_hang_open = 1;
            g_hang_n++;
            hang_report(still_s);
        }
    } else {
        if (g_hang_open && f != last_f) {
            g_hang_open = 0;
            mark_recovered(g_hang_dir, "New game frames appeared again after this hang.\r\n");
        }
        still_s = 0;
    }
    last_f = f;
}
