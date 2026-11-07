/* sb_userdata.c - W150 USER-DATA: zentrale Pfadfunktion (siehe sb_userdata.h). */
#include "sb_userdata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#ifndef SB_EDITION_COMMUNITY
#define SB_EDITION_COMMUNITY 0
#endif

static int g_init;
static sb_ud_mode_t g_mode;
static const char *g_src = "Edition";
static char g_root[MAX_PATH * 2];

static void exe_dir(char *out, size_t cap)
{
    char p[MAX_PATH * 2];
    char *s;
    DWORD n = GetModuleFileNameA(NULL, p, sizeof p);
    out[0] = '\0';
    if (n == 0 || n >= sizeof p) return;
    s = strrchr(p, '\\');
    if (s) *s = '\0';
    strcpy_s(out, cap, p);
}

static void mkdir_p(const char *path)
{
    char t[MAX_PATH * 2];
    char *s;
    strcpy_s(t, sizeof t, path);
    for (s = t; *s; s++) {
        if ((*s == '\\' || *s == '/') && s > t && s[-1] != ':') {
            char c = *s;
            *s = '\0';
            CreateDirectoryA(t, NULL);
            *s = c;
        }
    }
    CreateDirectoryA(t, NULL);
}

static void init(void)
{
    char ev[64], ed[MAX_PATH * 2], pt[MAX_PATH * 2 + 16];
    DWORD n;
    if (g_init) return;
    g_init = 1;
    g_root[0] = '\0';
    exe_dir(ed, sizeof ed);
    n = GetEnvironmentVariableA("SB_USERDATA", ev, sizeof ev);
    if (n > 0 && n < sizeof ev && ev[0]) {
        g_src = "SB_USERDATA";
        if (_stricmp(ev, "profile") == 0) g_mode = SB_UD_PROFILE;
        else if (_stricmp(ev, "portable") == 0) g_mode = SB_UD_PORTABLE;
        else g_mode = SB_UD_LEGACY;
    } else {
        _snprintf_s(pt, sizeof pt, _TRUNCATE, "%s\\portable.txt", ed);
        if (ed[0] && GetFileAttributesA(pt) != INVALID_FILE_ATTRIBUTES) {
            g_mode = SB_UD_PORTABLE;
            g_src = "portable.txt";
        } else {
            g_mode = SB_EDITION_COMMUNITY ? SB_UD_PROFILE : SB_UD_LEGACY;
            g_src = "Edition";
        }
    }
    if (g_mode == SB_UD_PORTABLE) {
        strcpy_s(g_root, sizeof g_root, ed);
    } else if (g_mode == SB_UD_PROFILE) {
        char base[MAX_PATH * 2];
        n = GetEnvironmentVariableA("SB_USERDATA_ROOT", base, sizeof base);
        if (n > 0 && n < sizeof base) {
            strcpy_s(g_root, sizeof g_root, base);
        } else {
            base[0] = '\0';
            n = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base);
            if (n == 0 || n >= sizeof base) base[0] = '\0';
            if (base[0]) {
                _snprintf_s(g_root, sizeof g_root, _TRUNCATE, "%s\\%s", base, SB_USERDATA_APPNAME);
            }
        }
    }
}

void sb_userdata_reset(void)
{
    g_init = 0;
}

sb_ud_mode_t sb_userdata_mode(void)
{
    init();
    return g_mode;
}

const char *sb_userdata_mode_name(sb_ud_mode_t m)
{
    return m == SB_UD_PROFILE ? "profile" : m == SB_UD_PORTABLE ? "portable" : "legacy";
}

const char *sb_userdata_mode_source(void)
{
    init();
    return g_src;
}

static const char *kind_name(sb_ud_kind_t k)
{
    switch (k) {
    case SB_UD_USERCFG: return "user.cfg";
    case SB_UD_HDD: return "hdd";
    case SB_UD_PACKS: return "packs";
    case SB_UD_SHADERCACHE: return "shadercache";
    case SB_UD_LOGS: return "logs";
    case SB_UD_CRASH: return "crash";
    case SB_UD_SCREENSHOTS: return "Screenshots"; /* W195 PHOTO-MODE */
    default: return "";
    }
}

static int path_impl(sb_ud_kind_t k, char *out, size_t cap, int create)
{
    const char *nm;
    init();
    if (out && cap) out[0] = '\0';
    if (g_mode == SB_UD_LEGACY) return 0;
    if (!g_root[0] || !out || !cap) return -1;
    nm = kind_name(k);
    if (nm[0]) _snprintf_s(out, cap, _TRUNCATE, "%s\\%s", g_root, nm);
    else strcpy_s(out, cap, g_root);
    if (create) {
        if (k == SB_UD_USERCFG) mkdir_p(g_root);
        else mkdir_p(out);
    }
    return 1;
}

int sb_userdata_path(sb_ud_kind_t k, char *out, size_t cap)
{
    return path_impl(k, out, cap, 1);
}

int sb_userdata_path_peek(sb_ud_kind_t k, char *out, size_t cap)
{
    return path_impl(k, out, cap, 0);
}

/* ---------------- Selbsttest ---------------- */
static void env_set(const char *k, const char *v) { SetEnvironmentVariableA(k, v); }

int sb_userdata_selftest(char *msg, size_t cap)
{
    char old_m[64], old_r[MAX_PATH * 2], tmp[MAX_PATH], root[MAX_PATH * 2], p[MAX_PATH * 2], ed[MAX_PATH * 2];
    DWORD nm = GetEnvironmentVariableA("SB_USERDATA", old_m, sizeof old_m);
    DWORD nr = GetEnvironmentVariableA("SB_USERDATA_ROOT", old_r, sizeof old_r);
    int fail = 0;
    GetTempPathA(sizeof tmp, tmp);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%ssb_userdata_selftest_%lu", tmp, GetCurrentProcessId());
    exe_dir(ed, sizeof ed);

    /* 1. legacy: 0, kein Pfad */
    env_set("SB_USERDATA", "legacy");
    sb_userdata_reset();
    if (sb_userdata_mode() != SB_UD_LEGACY || sb_userdata_path(SB_UD_HDD, p, sizeof p) != 0 || p[0]) fail = 1;
    /* 2. profile mit Test-Wurzel: Ordner werden angelegt */
    if (!fail) {
        env_set("SB_USERDATA", "profile");
        env_set("SB_USERDATA_ROOT", root);
        sb_userdata_reset();
        if (sb_userdata_mode() != SB_UD_PROFILE) fail = 2;
        else if (sb_userdata_path(SB_UD_HDD, p, sizeof p) != 1 || _stricmp(p + strlen(root), "\\hdd") != 0) fail = 3;
        else if (!(GetFileAttributesA(p) & FILE_ATTRIBUTE_DIRECTORY) || GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) fail = 4;
        else if (sb_userdata_path(SB_UD_USERCFG, p, sizeof p) != 1 || _stricmp(p + strlen(root), "\\user.cfg") != 0) fail = 5;
        else if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) fail = 6; /* user.cfg wird nie angelegt */
        else {
            static const sb_ud_kind_t ks[] = { SB_UD_PACKS, SB_UD_SHADERCACHE, SB_UD_LOGS, SB_UD_CRASH };
            unsigned i;
            for (i = 0; i < 4 && !fail; i++) {
                if (sb_userdata_path(ks[i], p, sizeof p) != 1 || GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) fail = 7;
            }
        }
    }
    /* 3. profile ohne Test-Wurzel: %LOCALAPPDATA%\SB-Preservation (nur peek, nichts anlegen) */
    if (!fail) {
        char la[MAX_PATH * 2], want[MAX_PATH * 3];
        env_set("SB_USERDATA_ROOT", NULL);
        sb_userdata_reset();
        if (GetEnvironmentVariableA("LOCALAPPDATA", la, sizeof la)) {
            _snprintf_s(want, sizeof want, _TRUNCATE, "%s\\%s", la, SB_USERDATA_APPNAME);
            if (sb_userdata_path_peek(SB_UD_ROOT, p, sizeof p) != 1 || _stricmp(p, want) != 0) fail = 8;
        }
    }
    /* 4. portable: Wurzel = Exe-Ordner (nur peek) */
    if (!fail) {
        env_set("SB_USERDATA", "portable");
        sb_userdata_reset();
        if (sb_userdata_mode() != SB_UD_PORTABLE || sb_userdata_path_peek(SB_UD_ROOT, p, sizeof p) != 1 || _stricmp(p, ed) != 0) fail = 9;
        else if (sb_userdata_path_peek(SB_UD_SHADERCACHE, p, sizeof p) != 1 || _stricmp(p + strlen(ed), "\\shadercache") != 0) fail = 10;
    }
    /* 5. ohne Schalter: Edition (private -> legacy, community -> profile),
     *    sofern kein portable.txt neben der Exe liegt */
    if (!fail) {
        char pt[MAX_PATH * 3];
        env_set("SB_USERDATA", NULL);
        sb_userdata_reset();
        _snprintf_s(pt, sizeof pt, _TRUNCATE, "%s\\portable.txt", ed);
        if (GetFileAttributesA(pt) != INVALID_FILE_ATTRIBUTES) {
            if (sb_userdata_mode() != SB_UD_PORTABLE) fail = 11;
        } else if (sb_userdata_mode() != (SB_EDITION_COMMUNITY ? SB_UD_PROFILE : SB_UD_LEGACY)) {
            fail = 12;
        }
    }
    /* aufraeumen (nur die eigene Test-Wurzel) */
    {
        static const char *sub[] = { "hdd", "packs", "shadercache", "logs", "crash" };
        unsigned i;
        for (i = 0; i < 5; i++) {
            _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\%s", root, sub[i]);
            RemoveDirectoryA(p);
        }
        RemoveDirectoryA(root);
    }
    env_set("SB_USERDATA", (nm > 0 && nm < sizeof old_m) ? old_m : NULL);
    env_set("SB_USERDATA_ROOT", (nr > 0 && nr < sizeof old_r) ? old_r : NULL);
    sb_userdata_reset();
    if (msg && cap) {
        _snprintf_s(msg, cap, _TRUNCATE, fail ? "sb_userdata Selbsttest FEHLER %d" :
                    "sb_userdata Selbsttest ok (legacy, profile, LOCALAPPDATA, portable, Edition)", fail);
    }
    return fail;
}
