/* sb_feedback.c - W188 GAME-MENU: Feedback Stufe 1. Beschreibung s. sb_feedback.h. */
#include "render_int.h"
#include "render_qa.h"   /* rq_in_mission */
#include "kernel_hle.h"  /* krnl_CreateThread */
#include "sb_edition.h"
#include "sb_feedback.h"
#include "sb_pause.h"
#include "sb_userdata.h"
#include <shellapi.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

static sb_gamever_t g_fb_gv;
static int g_fb_gv_set;

void sb_feedback_note_gamever(const sb_gamever_t *gv)
{
    if (gv) {
        g_fb_gv = *gv;
        g_fb_gv_set = 1;
    }
}

/* ------------------------------------------------------------------ */
/* Bereinigen                                                          */
/* ------------------------------------------------------------------ */
static int fb_ieq(char a, char b)
{
    if (a == '/') { a = '\\'; }
    if (b == '/') { b = '\\'; }
    if (a >= 'A' && a <= 'Z') { a = (char)(a + 32); }
    if (b >= 'A' && b <= 'Z') { b = (char)(b + 32); }
    return a == b;
}

static int fb_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/* Alle Vorkommen von needle (ohne Gross/Klein, / == \) durch rep ersetzen.
 * word=1: nur ohne Buchstabe/Ziffer davor/danach. */
static int fb_replace(char *s, size_t cap, const char *needle, const char *rep, int word)
{
    size_t nl = needle ? strlen(needle) : 0, rl = strlen(rep), i = 0, sl = strlen(s);
    int n = 0;
    if (nl < 2) { return 0; }
    while (i + nl <= sl) {
        size_t k;
        for (k = 0; k < nl && fb_ieq(s[i + k], needle[k]); k++) {}
        if (k == nl && (!word || ((i == 0 || !fb_alnum(s[i - 1])) && !fb_alnum(s[i + nl])))) {
            if (sl - nl + rl + 1 > cap) { break; }
            memmove(s + i + rl, s + i + nl, sl - i - nl + 1);
            memcpy(s + i, rep, rl);
            sl = sl - nl + rl;
            i += rl;
            n++;
        } else {
            i++;
        }
    }
    return n;
}

static int fb_count(const char *s, const char *needle)
{
    size_t nl = needle ? strlen(needle) : 0, i, k;
    int n = 0;
    if (nl < 3) { return 0; }
    for (i = 0; s[i]; i++) {
        for (k = 0; k < nl && s[i + k] && fb_ieq(s[i + k], needle[k]); k++) {}
        if (k == nl) { n++; }
    }
    return n;
}

static int fb_pathch(char c)
{
    return c && c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '"' && c != '\'' && c != '<' && c != '>' &&
           c != '|' && c != ',' && c != ';' && c != '(' && c != ')' && c != '[' && c != ']';
}

/* Absolute Pfade (X:\..., X:/..., \\host\...) -> <pfad>\<datei> bzw. <pfad>. */
static int fb_abs_paths(char *s, size_t cap)
{
    size_t i = 0, sl = strlen(s);
    int n = 0;
    while (s[i]) {
        size_t st = i, en, b;
        int abs = 0;
        if (((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z')) && s[i + 1] == ':' &&
            (s[i + 2] == '\\' || s[i + 2] == '/') && (i == 0 || !fb_alnum(s[i - 1]))) {
            abs = 1;
        } else if (s[i] == '\\' && s[i + 1] == '\\' && fb_alnum(s[i + 2]) && (i == 0 || s[i - 1] != '\\')) {
            abs = 1;
        }
        if (!abs) { i++; continue; }
        if (st > 0 && (s[st - 1] == '\'' || s[st - 1] == '"')) { /* in Anfuehrung: bis zum Ende (Leerzeichen im Pfad) */
            char q = s[st - 1];
            for (en = st + 2; s[en] && s[en] != q && s[en] != '\n' && s[en] != '\r'; en++) {}
        } else {
            for (en = st + 2; fb_pathch(s[en]); en++) {}
        }
        /* Dateiname = letzter Teil mit Punkt (Endung); Ordner -> nur <pfad> */
        for (b = en; b > st && s[b - 1] != '\\' && s[b - 1] != '/'; b--) {}
        {
            char rep[96];
            size_t bl = en - b, rl;
            if (bl > 0 && bl < 48 && memchr(s + b, '.', bl) && b > st + 3) {
                _snprintf_s(rep, sizeof rep, _TRUNCATE, "<pfad>\\%.*s", (int)bl, s + b);
            } else {
                strcpy_s(rep, sizeof rep, "<pfad>");
            }
            rl = strlen(rep);
            if (sl - (en - st) + rl + 1 > cap) { break; }
            memmove(s + st + rl, s + en, sl - en + 1);
            memcpy(s + st, rep, rl);
            sl = sl - (en - st) + rl;
            i = st + rl;
            n++;
        }
    }
    return n;
}

static void fb_env(const char *name, char *o, size_t cap, UINT cp)
{
    wchar_t wn[64], value[600];
    DWORD r;
    o[0] = '\0';
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wn, 64)) { return; }
    r = GetEnvironmentVariableW(wn, value, 600);
    if (r && r < 600) {
        if (cp == CP_ACP) {
            DWORD i;
            for (i = 0; i < r && value[i] < 128; ++i) {}
            if (i == r || GetACP() == CP_UTF8) { return; } /* schon im UTF-8-Durchgang bereinigt */
        }
        WideCharToMultiByte(cp, 0, value, -1, o, (int)cap, NULL, NULL);
    }
}

int sb_feedback_sanitize(char *s, size_t cap)
{
    static const char *const envp[][2] = {
        { "LOCALAPPDATA", "%LOCALAPPDATA%" }, { "APPDATA", "%APPDATA%" }, { "TEMP", "%TEMP%" },
        { "TMP", "%TEMP%" }, { "OneDrive", "%OneDrive%" }, { "USERPROFILE", "%USERPROFILE%" },
        { "PUBLIC", "%PUBLIC%" },
    };
    char v[600];
    size_t i;
    unsigned pass;
    unsigned passes;
    int n = 0;
    if (!s || !cap) { return 0; }
    /* UTF-8-Eingabe und alte ANSI-Logzeilen beide bereinigen. */
    passes = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0) ? 1u : 2u;
    for (pass = 0; pass < passes; ++pass) {
        UINT cp = pass ? CP_ACP : CP_UTF8;
        for (i = 0; i < sizeof envp / sizeof envp[0]; i++) {
            fb_env(envp[i][0], v, sizeof v, cp);
            if (strlen(v) >= 4) { n += fb_replace(s, cap, v, envp[i][1], 0); }
        }
        fb_env("USERNAME", v, sizeof v, cp);
        if (strlen(v) >= 2) { n += fb_replace(s, cap, v, "<user>", strlen(v) < 4); }
        fb_env("COMPUTERNAME", v, sizeof v, cp);
        if (strlen(v) >= 3) { n += fb_replace(s, cap, v, "<pc>", strlen(v) < 5); }
        fb_env("USERDOMAIN", v, sizeof v, cp);
        if (strlen(v) >= 3) { n += fb_replace(s, cap, v, "<pc>", strlen(v) < 5); }
    }
    n += fb_abs_paths(s, cap);
    return n;
}

/* Nachpruefung: Benutzername bzw. Profilpfad noch enthalten? */
static int fb_clean_hits(const char *s)
{
    char v[600];
    int n = 0;
    unsigned pass;
    unsigned passes = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0) ? 1u : 2u;
    for (pass = 0; pass < passes; ++pass) {
        UINT cp = pass ? CP_ACP : CP_UTF8;
        fb_env("USERNAME", v, sizeof v, cp);
        if (strlen(v) >= 4) { n += fb_count(s, v); }
        fb_env("USERPROFILE", v, sizeof v, cp);
        if (strlen(v) >= 4) { n += fb_count(s, v); }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Paket                                                               */
/* ------------------------------------------------------------------ */
static void fb_mkdirs(const char *path)
{
    char t[600];
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

int sb_feedback_begin(sb_fb_pkg_t *p, int kind)
{
    char root[600];
    SYSTEMTIME lt;
    DWORD a;
    memset(p, 0, sizeof *p);
    p->kind = kind ? 1 : 0;
    GetLocalTime(&lt);
    _snprintf_s(p->stamp, sizeof p->stamp, _TRUNCATE, "%04u%02u%02u-%02u%02u%02u", lt.wYear, lt.wMonth, lt.wDay,
                lt.wHour, lt.wMinute, lt.wSecond);
    if (sb_userdata_path(SB_UD_ROOT, root, sizeof root) != 1) { /* legacy (Private/Tests): Ausgabeordner */
        const char *o = hcfg_out_dir();
        strcpy_s(root, sizeof root, o && o[0] ? o : ".");
    }
    /* W211: rapid hotkey sessions must never overwrite an earlier package. */
    {
        unsigned suffix = 0;
        char base[600];
        _snprintf_s(base, sizeof base, _TRUNCATE, "%s\\feedback", root);
        fb_mkdirs(base);
        do {
            _snprintf_s(p->dir, sizeof p->dir, _TRUNCATE, "%s\\%s-%03u", base, p->stamp, suffix++);
            if (CreateDirectoryA(p->dir, NULL)) { break; }
            if (GetLastError() != ERROR_ALREADY_EXISTS || suffix == 1000) { return 0; }
        } while (1);
    }
    a = GetFileAttributesA(p->dir);
    strcpy_s(p->dir_show, sizeof p->dir_show, p->dir);
    sb_feedback_sanitize(p->dir_show, sizeof p->dir_show);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        HLOG_W("FEEDBACK: Ordner nicht anlegbar (%s)", p->dir_show);
        return 0;
    }
    HLOG_I("FEEDBACK: Paket %s (%s)", p->dir_show, p->kind ? "Idee" : "Bug");
    return 1;
}

static int fb_write(const char *dir, const char *name, const char *txt)
{
    char path[700];
    FILE *f = NULL;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", dir, name);
    if (fopen_s(&f, path, "wb") != 0 || !f) { return 0; }
    fwrite(txt, 1, strlen(txt), f);
    fclose(f);
    return 1;
}

/* Log-Ende (letzte maxl Zeilen der letzten 192 KB) in buf (cap). Zeilen. */
static int fb_log_tail(char *buf, size_t cap, int maxl)
{
    const char *lp = hlog_path();
    HANDLE h;
    LARGE_INTEGER sz;
    DWORD got = 0, want;
    char *st;
    int n = 0;
    buf[0] = '\0';
    if (!lp || !lp[0]) { return 0; }
    hlog_flush();
    h = CreateFileA(lp, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { return 0; }
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return 0; }
    want = (DWORD)(sz.QuadPart > 196608 ? 196608 : sz.QuadPart);
    if (want >= cap) { want = (DWORD)cap - 1; }
    {
        LARGE_INTEGER off;
        off.QuadPart = sz.QuadPart - want;
        SetFilePointerEx(h, off, NULL, FILE_BEGIN);
    }
    if (!ReadFile(h, buf, want, &got, NULL)) { got = 0; }
    CloseHandle(h);
    buf[got] = '\0';
    /* von hinten maxl Zeilen */
    st = buf + got;
    while (st > buf) {
        st--;
        if (*st == '\n' && st + 1 < buf + got) {
            if (++n >= maxl) { st++; break; }
        }
    }
    if (st != buf) { memmove(buf, st, strlen(st) + 1); }
    if (sz.QuadPart > want && n < maxl) { /* erste (angeschnittene) Zeile weg */
        char *nl = strchr(buf, '\n');
        if (nl) { memmove(buf, nl + 1, strlen(nl + 1) + 1); }
    }
    for (n = 0, st = buf; *st; st++) { n += *st == '\n'; }
    return n;
}

static void fb_gpu(char *o, size_t cap)
{
    IDXGIDevice *dx = NULL;
    IDXGIAdapter *ad = NULL;
    DXGI_ADAPTER_DESC d;
    strcpy_s(o, cap, "?");
    if (!g_dev || FAILED(ID3D11Device_QueryInterface(g_dev, &IID_IDXGIDevice, (void **)&dx)) || !dx) { return; }
    if (SUCCEEDED(IDXGIDevice_GetAdapter(dx, &ad)) && ad) {
        if (SUCCEEDED(IDXGIAdapter_GetDesc(ad, &d))) {
            char nm[160];
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, nm, sizeof nm, NULL, NULL);
            _snprintf_s(o, cap, _TRUNCATE, "%s (vendor 0x%04X)", nm, d.VendorId);
        }
        IDXGIAdapter_Release(ad);
    }
    IDXGIDevice_Release(dx);
}

static void fb_build(char *o, size_t cap)
{
    HMODULE m = GetModuleHandleA("sb_emu.dll");
    strcpy_s(o, cap, "?");
    if (m) {
        const IMAGE_DOS_HEADER *dh = (const IMAGE_DOS_HEADER *)m;
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)m + dh->e_lfanew);
        time_t t = (time_t)nt->FileHeader.TimeDateStamp;
        struct tm g;
        if (gmtime_s(&g, &t) == 0) {
            _snprintf_s(o, cap, _TRUNCATE, "%04d-%02d-%02d %02d:%02d UTC", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
                        g.tm_hour, g.tm_min);
        }
    }
}

static unsigned fb_rd(unsigned addr)
{
    unsigned v = 0;
    __try { v = *(volatile unsigned *)(uintptr_t)addr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
    return v;
}

/* URL-Kodierung (UTF-8, RFC 3986 unreserviert) */
static void fb_urlenc(char *o, size_t cap, const char *s)
{
    static const char hx[] = "0123456789ABCDEF";
    size_t n = 0;
    for (; *s && n + 4 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (fb_alnum((char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            o[n++] = (char)c;
        } else {
            o[n++] = '%';
            o[n++] = hx[c >> 4];
            o[n++] = hx[c & 15];
        }
    }
    o[n] = '\0';
}

/* Repo "Owner/Name" nur aus [A-Za-z0-9._-] mit genau einem '/'; sonst Standard. */
static const char *fb_repo(void)
{
    const char *r = g_cfg.feedback_repo;
    int slash = 0, ok = r[0] != '\0';
    const char *q;
    for (q = r; *q && ok; q++) {
        if (*q == '/') { slash++; }
        else if (!fb_alnum(*q) && *q != '.' && *q != '_' && *q != '-') { ok = 0; }
    }
    if (!ok || slash != 1 || r[0] == '/' || r[strlen(r) - 1] == '/') {
        HLOG_W("FEEDBACK: repo '%s' ungueltig -> %s", r, SB_FEEDBACK_REPO_DEFAULT);
        return SB_FEEDBACK_REPO_DEFAULT;
    }
    return r;
}

int sb_feedback_finish(sb_fb_pkg_t *p, const wchar_t *text)
{
    char gpu[200], build[40], txt8[1024], title[160], info[4096], game[200], state[160], ver[120];
    char e1[400], e2[400], e3[400], e4[400], e5[400];
    char *logb;
    size_t logcap = 196608 * 2 + 4096;
    unsigned mode = fb_rd(0x2EEA34u), scene = fb_rd(0x6B96DCu), tmr = fb_rd(0x3563F0u);
    int inm = rq_in_mission();
    SYSTEMTIME lt;
    const char *kn = p->kind ? "idea" : "bug";
    txt8[0] = '\0';
    if (text && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, txt8, sizeof txt8, NULL, NULL)) {
        HLOG_W("FEEDBACK: Text nicht als UTF-8 darstellbar (ungueltiges UTF-16 oder zu lang)");
        return 0;
    }
    p->repl += sb_feedback_sanitize(txt8, sizeof txt8);
    fb_gpu(gpu, sizeof gpu);
    fb_build(build, sizeof build);
    GetLocalTime(&lt);
    if (g_fb_gv_set) {
        _snprintf_s(game, sizeof game, _TRUNCATE, "%s title=0x%08X region=%u version=0x%X supported=%d sha256=%.16s",
                    g_fb_gv.label[0] ? g_fb_gv.label : "?", g_fb_gv.title_id, g_fb_gv.region, g_fb_gv.version,
                    g_fb_gv.supported, g_fb_gv.sha256);
    } else {
        strcpy_s(game, sizeof game, "?");
    }
    _snprintf_s(state, sizeof state, _TRUNCATE, "mode=%u scene=0x%X in_mission=%d countdown=%u", mode, scene, inm,
                tmr);
    _snprintf_s(ver, sizeof ver, _TRUNCATE, "%s edition, build %s", SB_EDITION_NAME, build);
    _snprintf_s(info, sizeof info, _TRUNCATE,
                "Steelbound feedback package (W188, stage 1)\r\n"
                "kind: %s\r\n"
                "created: %04u-%02u-%02u %02u:%02u:%02u (local time)\r\n"
                "client: %s\r\n"
                "game_image: %s\r\n"
                "gpu: %s\r\n"
                "display: %ux%u window_mode=%d interp_fps=%d vblank_hz=%d\r\n"
                "state: %s present=%u pauses=%u\r\n"
                "settings: shading=%d monitor_fx=%d edge_mode=%d aa_mode=%d ssao=%d remaster=%d hd_textures=%d "
                "hd_scale=%d vr_enable=%d lang=%s input_profile=%s menu_pause=%d\r\n"
                "contents: screenshot.jpg (game image only, never the desktop), log.txt (end of the client log, "
                "user name/computer name/paths replaced), info.txt, text.txt\r\n",
                kn, lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond, ver, game, gpu, g_bb_w, g_bb_h,
                g_cfg.window_mode, g_cfg.interp_fps, g_cfg.vblank_hz, state, g_present_no, sb_pause_count(),
                g_cfg.shading, g_cfg.monitor_fx, g_cfg.edge_mode, g_cfg.aa_mode, g_cfg.ssao, g_cfg.remaster,
                g_cfg.hd_textures, g_cfg.hd_scale, g_cfg.vr_enable, g_cfg.lang[0] ? g_cfg.lang : "en",
                g_cfg.input_profile[0] ? g_cfg.input_profile : "(legacy)", g_cfg.menu_pause);
    {
        size_t used = strlen(info);
        _snprintf_s(info + used, sizeof info - used, _TRUNCATE,
                    "clip: %s, %u bytes, %ux%u, %s; client image only, no audio\r\n",
                    p->clip == 1 ? "clip.mp4 (H264)" : p->clip == 2 ? "clip_000.jpg..clip_049.jpg (MP4 unavailable)" :
                    p->clip < 0 ? "failed" : "none", p->clip_bytes, p->clip_w, p->clip_h,
                    p->clip > 0 ? "5 seconds; see clip.txt" : "not attached");
    }
    p->repl += sb_feedback_sanitize(info, sizeof info);
    fb_write(p->dir, "info.txt", info);
    {
        char tt[1200];
        _snprintf_s(tt, sizeof tt, _TRUNCATE, "kind: %s\r\ntext: %s\r\n", kn, txt8);
        fb_write(p->dir, "text.txt", tt);
        p->clean_hits += fb_clean_hits(tt);
    }
    logb = (char *)malloc(logcap);
    if (logb) {
        p->log_lines = fb_log_tail(logb, logcap / 2, 400);
        p->repl += sb_feedback_sanitize(logb, logcap);
        fb_write(p->dir, "log.txt", logb);
        p->clean_hits += fb_clean_hits(logb);
        free(logb);
    }
    p->clean_hits += fb_clean_hits(info);
    /* URL: Titel + Kurzinfos fuer die Issue-Vorlage (Feld-IDs in editions/community/github/ISSUE_TEMPLATE) */
    {
        size_t k = 0;
        /* Titel: Kurztext, hoechstens ~80 Bytes an einer Zeichengrenze */
        _snprintf_s(title, sizeof title, _TRUNCATE, "%s %s", p->kind ? "[Idea]" : "[Bug]",
                    txt8[0] ? txt8 : "(short description)");
        k = strlen(title);
        if (k > 90) {
            k = 90;
            while (k > 0 && ((unsigned char)title[k] & 0xC0u) == 0x80u) { k--; }
            title[k] = '\0';
        }
        fb_urlenc(e1, sizeof e1, title);
        fb_urlenc(e2, sizeof e2, ver);
        fb_urlenc(e3, sizeof e3, g_fb_gv_set && g_fb_gv.label[0] ? g_fb_gv.label : "?");
        fb_urlenc(e4, sizeof e4, gpu);
        fb_urlenc(e5, sizeof e5, state);
        _snprintf_s(p->url, sizeof p->url, _TRUNCATE,
                    "https://github.com/%s/issues/new?template=%s&title=%s&version=%s&game=%s&gpu=%s&state=%s",
                    fb_repo(), p->kind ? "idea.yml" : "bug_report.yml", e1, e2, e3, e4, e5);
        /* Nachpruefung ohne den Repo-Namen (oeffentlicher GitHub-Owner kann den Nutzernamen enthalten) */
        p->clean_hits += fb_clean_hits(strstr(p->url, "/issues/new") ? strstr(p->url, "/issues/new") : p->url);
    }
    HLOG_I("FEEDBACK: Paket fertig (%s): screenshot=%d (%ux%u) log=%d Zeilen, %d Ersetzungen, Nachpruefung "
           "Benutzername/Profilpfad: %d Funde, URL %u Zeichen", p->dir_show, p->shot_ok, p->shot_w, p->shot_h,
           p->log_lines, p->repl, p->clean_hits, (unsigned)strlen(p->url));
    return 1;
}

/* ------------------------------------------------------------------ */
/* Oeffnen (nur Nutzeraktion; eigener STA-Thread fuer ShellExecute)      */
/* ------------------------------------------------------------------ */
typedef struct { wchar_t url[2048]; wchar_t dir[600]; } fb_open_t;

static DWORD WINAPI fb_open_thread(LPVOID a)
{
    fb_open_t *o = (fb_open_t *)a;
    HRESULT hc = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (o->url[0]) { ShellExecuteW(NULL, L"open", o->url, NULL, NULL, SW_SHOWNORMAL); }
    if (o->dir[0]) { ShellExecuteW(NULL, L"open", o->dir, NULL, NULL, SW_SHOWNORMAL); }
    if (SUCCEEDED(hc)) { CoUninitialize(); }
    free(o);
    return 0;
}

void sb_feedback_open(const sb_fb_pkg_t *p, HWND hwnd, int which, int test)
{
    fb_open_t *o;
    void *th;
    if (!p || !p->dir[0]) { return; }
    if (test) {
        HLOG_I("FEEDBACK: Test - %s nicht geoeffnet (Ordner %s)", which == 2 ? "Explorer" : "Browser/Explorer",
               p->dir_show);
        if (which != 2) { HLOG_I("FEEDBACK: URL (%u Zeichen): %s", (unsigned)strlen(p->url), p->url); }
        return;
    }
    if (hwnd && g_cfg.window_mode != 0) { /* Vollbild: minimieren, damit Browser/Explorer sichtbar sind */
        ShowWindowAsync(hwnd, SW_MINIMIZE);
        HLOG_I("FEEDBACK: Vollbild (window_mode=%d) minimiert", g_cfg.window_mode);
    }
    o = (fb_open_t *)calloc(1, sizeof *o);
    if (!o) { return; }
    if (which != 2) { MultiByteToWideChar(CP_UTF8, 0, p->url, -1, o->url, 2048); }
    MultiByteToWideChar(CP_ACP, 0, p->dir, -1, o->dir, 600);
    th = krnl_CreateThread((void *)fb_open_thread, o, 0, NULL);
    if (th) {
        CloseHandle((HANDLE)th);
        HLOG_I("FEEDBACK: %s geoeffnet (Nutzeraktion, Ordner %s)", which == 2 ? "Explorer" : "Browser + Explorer",
               p->dir_show);
    } else {
        free(o);
        HLOG_W("FEEDBACK: Thread zum Oeffnen nicht erstellbar (%lu)", GetLastError());
    }
}
