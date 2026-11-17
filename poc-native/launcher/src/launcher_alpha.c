/* launcher_alpha.c - W272 ALPHA-TEST: Testwerkzeuge der Alpha (siehe
 * launcher_alpha.h). Rein bzw. nur Dateisystem; die Seite selbst steht in
 * launcher_alpha.inc (in launcher_gui.c eingebunden). */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <winioctl.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "launcher_alpha.h"
#include "launcher_app.h"
#include "launcher_cfg.h"
#include "launcher_u8.h"
#include "sb_lang.h"
#include "sb_userdata.h"
#include "sb_sha256.h"
#include "iso_vfs.h"

#define printf cli_printf
#define LA_P 1100 /* Pfadpuffer (Zeichen) */

/* ------------------------------------------------------------------ */
/* VT-Tabelle                                                          */
/* ------------------------------------------------------------------ */
static const char *const kVtName[LA_VT_MAX + 1] = {
    "",              "Vitzh",         "M-Vitzh",          "Vortex",        "Juggernaut",
    "D3EYE",         "Falchion",      "Blade",            "Decider",       "Scare Face",
    "Scare Face A1", "Prominence M1", "Prominence M2",    "Prominence M1-WA",
    "Scare Face II", "Rapier",        "Quasar",           "Jaralaccs C",   "Jaralaccs NS",
    "Jaralaccs N",   "Regal Dress N", "Regal Dress A",    "Behemoth",
};
/* Gemessene EU-Werte (Feld 0xBC; -1 = Datei fehlt im Spiel). */
static const signed char kVtGen[LA_VT_MAX + 1] = {
    -1, 0, 0, 0, 1, 1, 0, 1, 0, 0, 0, 1, -1, 0, 1, 1, 2, 1, 1, 1, 2, 2, 2,
};
static const char *const kDiffName[LA_DIFF_MAX + 1] = {
    "Rookie", "Normal", "Veteran", "Desperado", "Magnum Force", "Armageddon",
};

const char *la_vt_name(int model)
{
    return (model >= 1 && model <= LA_VT_MAX) ? kVtName[model] : "";
}

const char *la_diff_name(int diff)
{
    return (diff >= 0 && diff <= LA_DIFF_MAX) ? kDiffName[diff] : NULL;
}

static void vt_count(la_vtlist_t *l)
{
    int m;
    l->n = 0;
    l->m[0].present = 0; /* Platzhalter, nie angeboten */
    for (m = 1; m <= LA_VT_MAX; m++) {
        l->n += l->m[m].present ? 1 : 0;
    }
}

void la_vt_fallback(la_vtlist_t *l)
{
    int m;
    memset(l, 0, sizeof *l);
    for (m = 1; m <= LA_VT_MAX; m++) {
        l->m[m].present = kVtGen[m] >= 0;
        l->m[m].gen = kVtGen[m];
        l->m[m].size = (m == 13 || m == 18) ? 240u : LA_VT_FULL_SIZE;
    }
    l->src = LA_SRC_FALLBACK;
    vt_count(l);
}

void la_vt_parse(const uint8_t *data, uint32_t n, la_vt_t *out)
{
    memset(out, 0, sizeof *out);
    out->present = 1;
    out->size = n;
    out->gen = -1;
    if (data && n >= 0xC0) {
        out->gen = (int)((uint32_t)data[0xBC] | ((uint32_t)data[0xBD] << 8) |
                         ((uint32_t)data[0xBE] << 16) | ((uint32_t)data[0xBF] << 24));
    }
}

int la_vt_scan_folder(const char *game_root_u8, la_vtlist_t *l)
{
    int m;
    memset(l, 0, sizeof *l);
    l->src = LA_SRC_FOLDER;
    if (!game_root_u8 || !game_root_u8[0]) {
        return 0;
    }
    for (m = 1; m <= LA_VT_MAX; m++) {
        char p[LA_P];
        FILE *f = NULL;
        uint8_t b[0x100];
        size_t got;
        long sz;
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\Media\\EngData\\ENGDAT%02d.ENG", game_root_u8, m);
        if (u8_fopen_s(&f, p, "rb") != 0 || !f) {
            continue;
        }
        got = fread(b, 1, sizeof b, f);
        fseek(f, 0, SEEK_END);
        sz = ftell(f);
        fclose(f);
        la_vt_parse(b, (uint32_t)got, &l->m[m]);
        l->m[m].size = sz > 0 ? (unsigned)sz : (unsigned)got;
    }
    vt_count(l);
    return l->n > 0;
}

int la_vt_scan_image(const char *image_u8, la_vtlist_t *l)
{
    char acp[LA_P], e[320];
    iso_vfs_t *v;
    int m;
    DWORD a;
    memset(l, 0, sizeof *l);
    l->src = LA_SRC_IMAGE;
    if (!image_u8 || !image_u8[0]) {
        return 0;
    }
    a = u8_GetFileAttributes(image_u8);
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) {
        return 0;
    }
    if (u8_to_acp(image_u8, acp, sizeof acp) == 2) {
        return 0;
    }
    v = iso_vfs_open(acp, e, sizeof e);
    if (!v) {
        return 0;
    }
    for (m = 1; m <= LA_VT_MAX; m++) {
        char rel[64];
        uint8_t b[0x100];
        uint32_t got = 0;
        iso_vfs_stat_t st;
        int node;
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "Media\\EngData\\ENGDAT%02d.ENG", m);
        node = iso_vfs_lookup(v, rel, NULL);
        if (node < 0 || iso_vfs_stat(v, node, &st) != 0 || st.is_dir) {
            continue;
        }
        if (iso_vfs_read(v, node, 0, b, sizeof b, &got) != 0) {
            continue;
        }
        la_vt_parse(b, got, &l->m[m]);
        l->m[m].size = st.size;
    }
    iso_vfs_close(v);
    vt_count(l);
    return l->n > 0;
}

void la_vt_scan(const launcher_settings_t *s, la_vtlist_t *l)
{
    if (s && s->game_image[0]) {
        if (la_vt_scan_image(s->game_image, l)) {
            return;
        }
    } else if (s && s->media_root[0]) {
        if (la_vt_scan_folder(s->media_root, l)) {
            return;
        }
    }
    la_vt_fallback(l);
}

/* ------------------------------------------------------------------ */
/* Pruefung                                                            */
/* ------------------------------------------------------------------ */
int la_check(const la_sel_t *sel, const la_vtlist_t *l, la_check_t *res)
{
    int v = sel->vt, m;
    memset(res, 0, sizeof *res);
    res->suggest = *sel;
    res->suggest.skip_intro = sel->skip_intro ? 1 : 0;
    if (sel->mission < 0 || sel->mission > LA_MISSION_MAX) {
        res->bad_mission = 1;
        res->suggest.mission = sel->mission < 0 ? 0 : LA_MISSION_MAX;
        res->key = "gui.alpha_err_mission";
    }
    if (v != -1) {
        int lo = 0, hi = 0;
        if (v < 1 || v > LA_VT_MAX) {
            res->bad_vt = 2;
            if (!res->key) {
                res->key = "gui.alpha_err_vtrange";
            }
            if (v > LA_VT_MAX) {
                for (m = LA_VT_MAX; m >= 1 && !lo; m--) {
                    lo = l->m[m].present ? m : 0;
                }
            } else if (v == 0) { /* Platzhalter: naechstes echtes Modell */
                for (m = 1; m <= LA_VT_MAX && !hi; m++) {
                    hi = l->m[m].present ? m : 0;
                }
            }
            res->suggest.vt = lo ? lo : hi ? hi : -1;
        } else if (!l->m[v].present) {
            res->bad_vt = 1;
            if (!res->key) {
                res->key = "gui.alpha_err_vt";
            }
            for (m = v - 1; m >= 1 && !lo; m--) {
                lo = l->m[m].present ? m : 0;
            }
            for (m = v + 1; m <= LA_VT_MAX && !hi; m++) {
                hi = l->m[m].present ? m : 0;
            }
            res->suggest.vt = lo ? lo : hi ? hi : -1;
            res->alt_vt = (lo && hi) ? hi : 0;
        }
    }
    if (sel->diff < -1 || sel->diff > LA_DIFF_MAX) {
        res->bad_diff = 1;
        res->suggest.diff = sel->diff < -1 ? -1 : LA_DIFF_MAX;
        if (!res->key) {
            res->key = "gui.alpha_err_diff";
        }
    }
    return res->key != NULL;
}

static void mis_label(int m, char *out, size_t cap)
{
    if (m == 0) {
        strncpy_s(out, cap, LT("gui.alpha_mis_training"), _TRUNCATE);
    } else {
        _snprintf_s(out, cap, _TRUNCATE, LT("gui.alpha_mis_n"), m);
    }
}

static void vt_label(int v, char *out, size_t cap)
{
    if (v < 1) {
        strncpy_s(out, cap, LT("gui.alpha_vt_default"), _TRUNCATE);
    } else {
        _snprintf_s(out, cap, _TRUNCATE, "%d %s", v, la_vt_name(v));
    }
}

static void diff_label(int d, char *out, size_t cap)
{
    if (d < 0) {
        strncpy_s(out, cap, LT("gui.alpha_diff_default"), _TRUNCATE);
    } else {
        _snprintf_s(out, cap, _TRUNCATE, "%d %s", d, la_diff_name(d));
    }
}

void la_check_text(const la_sel_t *sel, const la_check_t *res, char *out, size_t cap)
{
    char a[160], b[160], c[400], d[160], line[600];
    strncpy_s(out, cap, LT("gui.alpha_inv_head"), _TRUNCATE);
    if (res->bad_mission) {
        _snprintf_s(line, sizeof line, _TRUNCATE, LT("gui.alpha_err_mission"), sel->mission);
        strcat_s(out, cap, "\n");
        strcat_s(out, cap, line);
    }
    if (res->bad_vt == 1) {
        _snprintf_s(line, sizeof line, _TRUNCATE, LT("gui.alpha_err_vt"), sel->vt, la_vt_name(sel->vt));
        strcat_s(out, cap, "\n");
        strcat_s(out, cap, line);
    } else if (res->bad_vt == 2) {
        _snprintf_s(line, sizeof line, _TRUNCATE, LT("gui.alpha_err_vtrange"), sel->vt);
        strcat_s(out, cap, "\n");
        strcat_s(out, cap, line);
    }
    if (res->bad_diff) {
        _snprintf_s(line, sizeof line, _TRUNCATE, LT("gui.alpha_err_diff"), sel->diff);
        strcat_s(out, cap, "\n");
        strcat_s(out, cap, line);
    }
    mis_label(res->suggest.mission, a, sizeof a);
    vt_label(res->suggest.vt, b, sizeof b);
    if (res->alt_vt) {
        vt_label(res->alt_vt, d, sizeof d);
        _snprintf_s(c, sizeof c, _TRUNCATE, LT("gui.alpha_suggest_or"), b, d);
    } else {
        strcpy_s(c, sizeof c, b);
    }
    diff_label(res->suggest.diff, d, sizeof d);
    _snprintf_s(line, sizeof line, _TRUNCATE, LT("gui.alpha_suggest"), a, c, d);
    strcat_s(out, cap, "\n");
    strcat_s(out, cap, line);
}

void la_suggest_cli(const la_check_t *res, char *out, size_t cap)
{
    _snprintf_s(out, cap, _TRUNCATE, "--alpha-mission %d --alpha-vt %d --alpha-diff %d%s",
                res->suggest.mission, res->suggest.vt, res->suggest.diff,
                res->suggest.skip_intro ? " --alpha-skip-intro" : "");
}

/* ------------------------------------------------------------------ */
/* Start-Kette                                                         */
/* ------------------------------------------------------------------ */
/* Pfad fuer die --set-Kette: muss in der ANSI-Codepage darstellbar sein
 * (Client liest per GetCommandLineA), kein '"', ';' -> "\;". */
static int chain_path(const char *u8, char *out, size_t cap)
{
    char acp[LA_P], back[LA_P * 2];
    size_t i, j = 0;
    if (!u8 || !u8[0] || strchr(u8, '"')) {
        return 1;
    }
    if (u8_to_acp(u8, acp, sizeof acp) == 2) {
        return 1;
    }
    u8_from_acp(acp, back, sizeof back);
    for (i = 0; back[i]; i++) {
        if (j + 3 >= cap) {
            return 2;
        }
        if (back[i] == ';') {
            out[j++] = '\\';
        }
        out[j++] = back[i];
    }
    out[j] = '\0';
    return 0;
}

int la_extra_args(const la_sel_t *sel, const char *hdd_eu_u8, const char *hdd_us_u8,
                  char *out, size_t cap)
{
    char eu[LA_P * 2], us[LA_P * 2];
    int rc, n;
    out[0] = '\0';
    if ((rc = chain_path(hdd_eu_u8, eu, sizeof eu)) != 0 ||
        (rc = chain_path(hdd_us_u8, us, sizeof us)) != 0) {
        return rc;
    }
    n = _snprintf_s(out, cap, _TRUNCATE,
                    "mission_autostart=%d;mission_vt=%d;mission_difficulty=%d;"
                    "mission_skip_intro=%d;hdd_root=%s;hdd_root_us=%s",
                    sel->mission, sel->vt, sel->diff, sel->skip_intro ? 1 : 0, eu, us);
    if (n < 0) {
        out[0] = '\0';
        return 2;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Testordner                                                          */
/* ------------------------------------------------------------------ */
static void strip_sep(char *p)
{
    size_t n = strlen(p);
    while (n > 3 && (p[n - 1] == '\\' || p[n - 1] == '/')) {
        p[--n] = '\0';
    }
}

void la_data_root(const char *client_dir, char *out, size_t cap)
{
    char p[1024];
    if (sb_userdata_path_peek(SB_UD_ROOT, p, sizeof p) == 1) {
        u8_from_acp(p, out, cap);
    } else {
        wchar_t a[LA_P], b[LA_P];
        u8_to_w(client_dir ? client_dir : ".", a, LA_P);
        if (GetFullPathNameW(a, LA_P, b, NULL) > 0) {
            w_to_u8(b, out, (int)cap);
        } else {
            strcpy_s(out, cap, client_dir ? client_dir : ".");
        }
    }
    strip_sep(out);
}

void la_test_dirs(const char *data_root, char *root, char *eu, char *us, size_t cap)
{
    char r[LA_P];
    strcpy_s(r, sizeof r, data_root ? data_root : "");
    strip_sep(r);
    if (root) {
        _snprintf_s(root, cap, _TRUNCATE, "%s\\alpha_test", r);
    }
    if (eu) {
        _snprintf_s(eu, cap, _TRUNCATE, "%s\\alpha_test\\HDD", r);
    }
    if (us) {
        _snprintf_s(us, cap, _TRUNCATE, "%s\\alpha_test\\HDD_US", r);
    }
}

static int norm_full(const char *u8, wchar_t *out, DWORD cap)
{
    wchar_t w[LA_P];
    DWORD n;
    size_t L;
    if (!u8 || !u8[0]) {
        return 0;
    }
    u8_to_w(u8, w, LA_P);
    n = GetFullPathNameW(w, cap, out, NULL);
    if (n == 0 || n >= cap) {
        return 0;
    }
    L = wcslen(out);
    while (L > 3 && out[L - 1] == L'\\') {
        out[--L] = L'\0';
    }
    return 1;
}

/* Echter Zielpfad (Verknuepfungen aufgeloest), nur fuer vorhandene Ordner.
 * Oeffnet nur den Ordner selbst (kein Zugriff auf Inhalte). */
static int final_path(const wchar_t *p, wchar_t *out, DWORD cap)
{
    HANDLE h = CreateFileW(p, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    DWORD n;
    size_t L;
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    n = GetFinalPathNameByHandleW(h, out, cap, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (n == 0 || n >= cap) {
        return 0;
    }
    if (wcsncmp(out, L"\\\\?\\UNC\\", 8) == 0) {
        memmove(out + 1, out + 7, (wcslen(out + 7) + 1) * sizeof(wchar_t));
        out[0] = L'\\';
    } else if (wcsncmp(out, L"\\\\?\\", 4) == 0) {
        memmove(out, out + 4, (wcslen(out + 4) + 1) * sizeof(wchar_t));
    }
    L = wcslen(out);
    while (L > 3 && out[L - 1] == L'\\') {
        out[--L] = L'\0';
    }
    return 1;
}

static int rel_w(const wchar_t *a, const wchar_t *b)
{
    size_t la = wcslen(a), lb = wcslen(b);
    if (!la || !lb) {
        return 0;
    }
    if (la == lb && _wcsicmp(a, b) == 0) {
        return 1;
    }
    if (la > lb && _wcsnicmp(a, b, lb) == 0 && (a[lb] == L'\\' || b[lb - 1] == L'\\')) {
        return 2;
    }
    if (lb > la && _wcsnicmp(a, b, la) == 0 && (b[la] == L'\\' || a[la - 1] == L'\\')) {
        return 3;
    }
    return 0;
}

int la_path_rel(const char *a_u8, const char *b_u8)
{
    wchar_t a[LA_P], b[LA_P], fa[LA_P], fb[LA_P];
    int r;
    if (!norm_full(a_u8, a, LA_P) || !norm_full(b_u8, b, LA_P)) {
        return 0;
    }
    r = rel_w(a, b);
    if (r) {
        return r;
    }
    if (final_path(a, fa, LA_P) && final_path(b, fb, LA_P)) {
        return rel_w(fa, fb);
    }
    return 0;
}

/* Inhalt eines Ordners loeschen, ohne Reparse-Points zu folgen
 * (Verknuepfung selbst wird entfernt, ihr Ziel bleibt). 0 = ok. */
static int del_contents(const wchar_t *dir, int depth)
{
    wchar_t pat[LA_P + 4], full[LA_P + 270];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int bad = 0;
    if (depth > 32) {
        return 1;
    }
    _snwprintf_s(pat, LA_P + 4, _TRUNCATE, L"%s\\*", dir);
    h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (h == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : 1;
    }
    do {
        DWORD a = fd.dwFileAttributes;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }
        if (_snwprintf_s(full, LA_P + 270, _TRUNCATE, L"%s\\%s", dir, fd.cFileName) < 0) {
            bad = 1;
            continue;
        }
        if ((a & FILE_ATTRIBUTE_READONLY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
            SetFileAttributesW(full, a & ~FILE_ATTRIBUTE_READONLY);
        }
        if (a & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
                bad |= del_contents(full, depth + 1);
            }
            if (!RemoveDirectoryW(full)) {
                bad = 1;
            }
        } else if (!DeleteFileW(full)) {
            bad = 1;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return bad;
}

static int guard_hit(const char *test_root, const char *const *real, int nreal)
{
    int i;
    for (i = 0; i < nreal; i++) {
        if (real[i] && real[i][0] && la_path_rel(test_root, real[i]) != 0) {
            return 1;
        }
    }
    return 0;
}

int la_prepare(const char *test_root_u8, const char *const *real_u8, int nreal, int dry,
               const char **err_key)
{
    wchar_t wr[LA_P], we[LA_P + 16], wu[LA_P + 16];
    DWORD a;
    *err_key = NULL;
    if (!norm_full(test_root_u8, wr, LA_P)) {
        *err_key = "gui.alpha_err_mkdir";
        return 1;
    }
    if (guard_hit(test_root_u8, real_u8, nreal)) {
        *err_key = "gui.alpha_err_guard";
        return 1;
    }
    if (dry) {
        return 0;
    }
    a = GetFileAttributesW(wr);
    if (a != INVALID_FILE_ATTRIBUTES) {
        if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
            *err_key = "gui.alpha_err_clean";
            return 1;
        }
        if (a & FILE_ATTRIBUTE_REPARSE_POINT) { /* nur die Verknuepfung entfernen */
            if (!RemoveDirectoryW(wr)) {
                *err_key = "gui.alpha_err_clean";
                return 1;
            }
        } else if (del_contents(wr, 0) != 0) {
            *err_key = "gui.alpha_err_clean";
            return 1;
        }
    }
    {
        char r8[LA_P * 3];
        w_to_u8(wr, r8, (int)sizeof r8);
        lcfg_mkdir_p(r8);
    }
    _snwprintf_s(we, LA_P + 16, _TRUNCATE, L"%s\\HDD", wr);
    _snwprintf_s(wu, LA_P + 16, _TRUNCATE, L"%s\\HDD_US", wr);
    CreateDirectoryW(we, NULL);
    CreateDirectoryW(wu, NULL);
    a = GetFileAttributesW(we);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY) ||
        (a = GetFileAttributesW(wu)) == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        *err_key = "gui.alpha_err_mkdir";
        return 1;
    }
    /* Zweite Pruefung mit echten Zielpfaden (Ordner existiert jetzt):
     * liegt der Datenordner hinter einer Verknuepfung im echten HDD,
     * die eben angelegten leeren Ordner wieder entfernen. */
    if (guard_hit(test_root_u8, real_u8, nreal)) {
        RemoveDirectoryW(wu);
        RemoveDirectoryW(we);
        RemoveDirectoryW(wr);
        *err_key = "gui.alpha_err_guard";
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* A/B-Schalter (user.cfg)                                             */
/* ------------------------------------------------------------------ */
static int line_key(const char *ln, const char *key, const char **val)
{
    size_t kl = strlen(key);
    while (*ln == ' ' || *ln == '\t') {
        ln++;
    }
    if (strncmp(ln, key, kl) != 0) {
        return 0;
    }
    ln += kl;
    while (*ln == ' ' || *ln == '\t') {
        ln++;
    }
    if (*ln != '=') {
        return 0;
    }
    if (val) {
        *val = ln + 1;
    }
    return 1;
}

static int cfg_last_int(const char *path, const char *key, int *out)
{
    FILE *f = NULL;
    char ln[1200];
    int found = 0;
    if (!path || !path[0] || u8_fopen_s(&f, path, "r") != 0 || !f) {
        return 0;
    }
    while (fgets(ln, sizeof ln, f)) {
        const char *v;
        if (line_key(ln, key, &v)) {
            *out = atoi(v);
            found = 1;
        }
    }
    fclose(f);
    return found;
}

int la_cfg_get_int(const char *user_cfg, const char *native_cfg, const char *key, int dflt)
{
    int v = dflt;
    if (cfg_last_int(user_cfg, key, &v)) {
        return v;
    }
    if (cfg_last_int(native_cfg, key, &v)) {
        return v;
    }
    return dflt;
}

int la_cfg_set(const char *path, const char *key, const char *val)
{
    FILE *f = NULL;
    char *out, ln[4096];
    size_t cap = 64 * 1024, len = 0;
    int done = 0;
    if (u8_fopen_s(&f, path, "r") == 0 && f) {
        fseek(f, 0, SEEK_END);
        cap = (size_t)ftell(f) + 4096;
        fseek(f, 0, SEEK_SET);
        if (cap > 1024 * 1024 + 4096) {
            fclose(f);
            return 1;
        }
    } else {
        f = NULL;
    }
    out = (char *)malloc(cap);
    if (!out) {
        if (f) {
            fclose(f);
        }
        return 1;
    }
    out[0] = '\0';
    if (f) {
        while (fgets(ln, sizeof ln, f)) {
            char *nl = ln + strcspn(ln, "\r\n");
            *nl = '\0';
            if (line_key(ln, key, NULL)) {
                if (!done) {
                    len += (size_t)_snprintf_s(out + len, cap - len, _TRUNCATE, "%s=%s\n", key, val);
                    done = 1;
                }
                continue; /* Dubletten fallen weg */
            }
            len += (size_t)_snprintf_s(out + len, cap - len, _TRUNCATE, "%s\n", ln);
        }
        fclose(f);
    }
    if (!done) {
        len += (size_t)_snprintf_s(out + len, cap - len, _TRUNCATE, "[render]\n%s=%s\n", key, val);
    }
    if (u8_fopen_s(&f, path, "w") != 0 || !f) {
        free(out);
        return 1;
    }
    fwrite(out, 1, len, f);
    fclose(f);
    free(out);
    return 0;
}

int la_client_knows_key(const char *client_dir, const char *key)
{
    char p[LA_P];
    FILE *f = NULL;
    size_t kl = strlen(key);
    int found = 0;
    if (!client_dir || !client_dir[0] || kl == 0 || kl > 60) {
        return 0;
    }
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\sb_native.cfg", client_dir);
    if (u8_fopen_s(&f, p, "r") == 0 && f) {
        char ln[1200];
        while (!found && fgets(ln, sizeof ln, f)) {
            found = line_key(ln, key, NULL);
        }
        fclose(f);
    }
    if (found) {
        return 1;
    }
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\sb_emu.dll", client_dir);
    if (u8_fopen_s(&f, p, "rb") == 0 && f) {
        /* Literal "\0key\0" in Bloecken mit Ueberlappung suchen */
        enum { BLK = 1 << 20 };
        unsigned char *b = (unsigned char *)malloc(BLK + 64);
        size_t keep = 0;
        if (b) {
            for (;;) {
                size_t got = fread(b + keep, 1, BLK, f), n = keep + got, i;
                for (i = 1; i + kl < n && !found; i++) {
                    if (b[i - 1] == 0 && b[i + kl] == 0 && memcmp(b + i, key, kl) == 0) {
                        found = 1;
                    }
                }
                if (found || got == 0) {
                    break;
                }
                keep = n < kl + 2 ? n : kl + 2;
                memmove(b, b + n - keep, keep);
            }
            free(b);
        }
        fclose(f);
    }
    return found;
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */
static int la_client_running(void)
{
    PROCESSENTRY32W pe;
    int found = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) {
        return 0;
    }
    pe.dwSize = sizeof pe;
    if (Process32FirstW(s, &pe)) {
        do {
            found = _wcsicmp(pe.szExeFile, L"sb_ldr.exe") == 0;
        } while (!found && Process32NextW(s, &pe));
    }
    CloseHandle(s);
    return found;
}

static int cli_check_list(const la_vtlist_t *l, const la_sel_t *sel)
{
    la_check_t r;
    char msg[2000];
    if (!la_check(sel, l, &r)) {
        return 0;
    }
    la_check_text(sel, &r, msg, sizeof msg);
    printf("FEHLER: %s\n", msg);
    la_suggest_cli(&r, msg, sizeof msg);
    printf("Vorschlag: %s\n", msg);
    return 3;
}

int la_cli_check(const launcher_settings_t *s, const la_sel_t *sel)
{
    la_vtlist_t l;
    la_vt_scan(s, &l);
    return cli_check_list(&l, sel);
}

int la_cli_apply(const char *client_dir, const launcher_settings_t *s, const la_sel_t *sel,
                 int dry, char *set_args, size_t cap)
{
    la_vtlist_t l;
    char msg[2000], data[LA_P], root[LA_P], eu[LA_P], us[LA_P], base[LA_P], rus[LA_P + 8];
    char extra[1600], a[160], b[160], d[160];
    const char *reals[2];
    const char *ek = NULL;
    la_vt_scan(s, &l);
    if (cli_check_list(&l, sel) != 0) {
        return 3;
    }
    mis_label(sel->mission, a, sizeof a);
    vt_label(sel->vt, b, sizeof b);
    diff_label(sel->diff, d, sizeof d);
    printf("alpha: %s, VT %s, %s, skip_intro=%d (VT-Liste: %s, %d Modelle)\n", a, b, d,
           sel->skip_intro ? 1 : 0,
           l.src == LA_SRC_IMAGE ? "Abbild" : l.src == LA_SRC_FOLDER ? "Spielordner" : "feste Tabelle",
           l.n);
    if (sel->vt >= 1 && l.m[sel->vt].size && l.m[sel->vt].size < LA_VT_FULL_SIZE) {
        _snprintf_s(msg, sizeof msg, _TRUNCATE, LT("gui.alpha_vt_reduced"), l.m[sel->vt].size);
        printf("alpha: %s\n", msg);
    }
    if (!dry && la_client_running()) {
        printf("FEHLER: %s\n", LT("gui.alpha_running"));
        return 1;
    }
    la_data_root(client_dir, data, sizeof data);
    la_test_dirs(data, root, eu, us, sizeof root);
    app_hdd_base(client_dir, s->hdd_root, base, sizeof base);
    lcfg_hdd_for_edition(base, LCFG_ED_US, rus, sizeof rus);
    reals[0] = base;
    reals[1] = rus;
    if (la_prepare(root, reals, 2, dry, &ek) != 0) {
        printf("FEHLER: %s\n", LT(ek));
        return 1;
    }
    printf(dry ? "alpha: Testordner %s (dry-run: nicht angelegt/geleert)\n"
               : "alpha: Testordner %s frisch angelegt (HDD, HDD_US leer)\n", root);
    if (la_extra_args(sel, eu, us, extra, sizeof extra) != 0) {
        printf("FEHLER: %s\n", LT("gui.alpha_err_path"));
        return 1;
    }
    if (strlen(set_args) + 1 + strlen(extra) + 1 > cap) {
        printf("FEHLER: --set-Kette zu lang\n");
        return 1;
    }
    if (set_args[0]) {
        strcat_s(set_args, cap, ";");
    }
    strcat_s(set_args, cap, extra);
    printf("alpha: %s (nur dieser Start, user.cfg bleibt)\n", extra);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Selbsttest                                                          */
/* ------------------------------------------------------------------ */
#define LT_OK(name)                       \
    do {                                  \
        printf("PASS %s\n", name);        \
        (*pass)++;                        \
    } while (0)
#define LT_FAIL(name, fmt, ...)                          \
    do {                                                 \
        printf("FAIL %s: " fmt "\n", name, __VA_ARGS__); \
        (*fail)++;                                       \
    } while (0)

static void st_write(const char *path, const void *data, size_t n)
{
    FILE *f = NULL;
    if (u8_fopen_s(&f, path, "wb") == 0 && f) {
        fwrite(data, 1, n, f);
        fclose(f);
    }
}

static void st_wstr(const char *path, const char *text)
{
    st_write(path, text, strlen(text));
}

static void st_engdat(const char *dir, int model, unsigned size, uint32_t gen)
{
    uint8_t b[300];
    char p[LA_P];
    memset(b, 0x11 * (model & 7), sizeof b);
    b[0xBC] = (uint8_t)gen;
    b[0xBD] = (uint8_t)(gen >> 8);
    b[0xBE] = (uint8_t)(gen >> 16);
    b[0xBF] = (uint8_t)(gen >> 24);
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\ENGDAT%02d.ENG", dir, model);
    st_write(p, b, size);
}

static int st_hash(const char *path, char hex[65])
{
    FILE *f = NULL;
    uint8_t buf[4096], h[32];
    sb_sha256_t c;
    size_t n;
    hex[0] = '\0';
    if (u8_fopen_s(&f, path, "rb") != 0 || !f) {
        return 0;
    }
    sb_sha256_init(&c);
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        sb_sha256_update(&c, buf, n);
    }
    fclose(f);
    sb_sha256_final(&c, h);
    sb_sha256_hex(h, hex);
    return 1;
}

static int st_exists(const char *u8)
{
    return u8_GetFileAttributes(u8) != INVALID_FILE_ATTRIBUTES;
}

static int st_dir_empty(const char *u8)
{
    char pat[LA_P];
    u8_find_t fd;
    int empty = 1;
    _snprintf_s(pat, sizeof pat, _TRUNCATE, "%s\\*", u8);
    if (u8_find_first(pat, &fd)) {
        do {
            if (strcmp(fd.name, ".") != 0 && strcmp(fd.name, "..") != 0) {
                empty = 0;
            }
        } while (empty && u8_find_next(&fd));
        u8_find_close(&fd);
    }
    return empty && st_exists(u8);
}

/* Verzeichnis-Verknuepfung (Junction) link -> target; 1 = angelegt. */
typedef struct {
    DWORD tag;
    WORD len, res;
    WORD sub_off, sub_len, prn_off, prn_len;
    WCHAR buf[1];
} st_mp_t;
static int st_junction(const char *link_u8, const char *target_u8)
{
    wchar_t link[LA_P], tgt[LA_P], sub[LA_P + 8];
    BYTE raw[4096];
    st_mp_t *r = (st_mp_t *)raw;
    HANDLE h;
    DWORD ret = 0;
    size_t sl, pl;
    BOOL ok;
    if (!norm_full(link_u8, link, LA_P) || !norm_full(target_u8, tgt, LA_P)) {
        return 0;
    }
    if (!CreateDirectoryW(link, NULL)) {
        return 0;
    }
    h = CreateFileW(link, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(link);
        return 0;
    }
    _snwprintf_s(sub, LA_P + 8, _TRUNCATE, L"\\??\\%s", tgt);
    sl = wcslen(sub) * sizeof(WCHAR);
    pl = wcslen(tgt) * sizeof(WCHAR);
    memset(raw, 0, sizeof raw);
    r->tag = IO_REPARSE_TAG_MOUNT_POINT;
    r->sub_off = 0;
    r->sub_len = (WORD)sl;
    r->prn_off = (WORD)(sl + sizeof(WCHAR));
    r->prn_len = (WORD)pl;
    memcpy(r->buf, sub, sl + sizeof(WCHAR));
    memcpy((BYTE *)r->buf + sl + sizeof(WCHAR), tgt, pl + sizeof(WCHAR));
    r->len = (WORD)(8 + sl + sizeof(WCHAR) + pl + sizeof(WCHAR));
    ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, raw, (DWORD)r->len + 8, NULL, 0, &ret, NULL);
    CloseHandle(h);
    if (!ok) {
        RemoveDirectoryW(link);
    }
    return ok ? 1 : 0;
}

static void st_rmtree(const char *u8)
{
    wchar_t w[LA_P];
    if (!norm_full(u8, w, LA_P)) {
        return;
    }
    if (GetFileAttributesW(w) & FILE_ATTRIBUTE_REPARSE_POINT) {
        RemoveDirectoryW(w);
        return;
    }
    del_contents(w, 0);
    RemoveDirectoryW(w);
}

void la_selftest(int *pass, int *fail)
{
    char tmp[LA_P], base[LA_P], p[LA_P], q[LA_P], why[300];
    char keep[16];
    unsigned long pid = GetCurrentProcessId();
    u8_GetTempPath(tmp, sizeof tmp);
    strip_sep(tmp);
    _snprintf_s(base, sizeof base, _TRUNCATE, "%s\\w272_alpha_" U8_AE "_%lu", tmp, pid);
    st_rmtree(base);
    lcfg_mkdir_p(base);
    strcpy_s(keep, sizeof keep, sb_lang_code());
    sb_lang_set("en");

    /* (e) VT-Liste aus synthetischen ENGDAT-Dateien (Feld 0xBC) */
    {
        la_vtlist_t l, f;
        launcher_settings_t s;
        int m, ok = 1;
        why[0] = '\0';
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\game\\Media\\EngData", base);
        lcfg_mkdir_p(p);
        st_engdat(p, 0, 240, 0);  /* Platzhalter, nie angeboten */
        st_engdat(p, 1, 268, 0);
        st_engdat(p, 4, 268, 1);
        st_engdat(p, 13, 240, 0); /* reduzierte Daten */
        st_engdat(p, 16, 268, 2);
        st_engdat(p, 22, 100, 2); /* zu kurz: Feld fehlt -> -1 */
        _snprintf_s(q, sizeof q, _TRUNCATE, "%s\\game", base);
        if (!la_vt_scan_folder(q, &l) || l.n != 5 || l.src != LA_SRC_FOLDER) {
            ok = 0;
            _snprintf_s(why, sizeof why, _TRUNCATE, "Ordner: n=%d", l.n);
        } else if (l.m[0].present || l.m[12].present || l.m[2].present ||
                   l.m[1].gen != 0 || l.m[4].gen != 1 || l.m[16].gen != 2 || l.m[13].gen != 0 ||
                   l.m[13].size != 240 || l.m[1].size != 268 || l.m[22].gen != -1 ||
                   l.m[22].size != 100) {
            ok = 0;
            strcpy_s(why, sizeof why, "Ordner: Werte");
        }
        lcfg_defaults(&s);
        s.game_image[0] = '\0';
        s.xbe_path[0] = '\0';
        strcpy_s(s.media_root, sizeof s.media_root, q);
        la_vt_scan(&s, &l);
        if (ok && (l.src != LA_SRC_FOLDER || l.n != 5)) {
            ok = 0;
            strcpy_s(why, sizeof why, "la_vt_scan Ordner");
        }
        _snprintf_s(s.game_image, sizeof s.game_image, _TRUNCATE, "%s\\fehlt.iso", base);
        la_vt_scan(&s, &l);
        la_vt_fallback(&f);
        if (ok && (l.src != LA_SRC_FALLBACK || l.n != 21 || memcmp(&l, &f, sizeof l) != 0)) {
            ok = 0;
            strcpy_s(why, sizeof why, "Abbild fehlt -> feste Tabelle");
        }
        /* feste Tabelle = gemessene EU-Werte */
        for (m = 1; m <= LA_VT_MAX && ok; m++) {
            static const int g0[] = { 1, 2, 3, 6, 8, 9, 10, 13 }, g1[] = { 4, 5, 7, 11, 14, 15, 17, 18, 19 },
                             g2[] = { 16, 20, 21, 22 };
            int want = -1, k;
            for (k = 0; k < 8; k++) { want = g0[k] == m ? 0 : want; }
            for (k = 0; k < 9; k++) { want = g1[k] == m ? 1 : want; }
            for (k = 0; k < 4; k++) { want = g2[k] == m ? 2 : want; }
            if (f.m[m].gen != want || f.m[m].present != (want >= 0) ||
                (f.m[m].present && f.m[m].size != ((m == 13 || m == 18) ? 240u : 268u))) {
                ok = 0;
                _snprintf_s(why, sizeof why, _TRUNCATE, "Tabelle Modell %d", m);
            }
        }
        if (ok && (strcmp(la_vt_name(16), "Quasar") != 0 || strcmp(la_vt_name(12), "Prominence M2") != 0 ||
                   la_vt_name(0)[0] || la_vt_name(23)[0])) {
            ok = 0;
            strcpy_s(why, sizeof why, "Namen");
        }
        if (ok) {
            LT_OK("W272 alpha VT list (synthetic ENGDAT, gen field 0xBC, size, model 0/12, fallback table)");
        } else {
            LT_FAIL("W272 alpha VT list", "%s", why);
        }
    }

    /* (d) ungueltige Kombination -> Meldung + Vorschlag, kein Start */
    {
        la_vtlist_t l, syn;
        la_check_t r;
        la_sel_t s;
        char t[2000];
        int ok = 1;
        why[0] = '\0';
        la_vt_fallback(&l);
        s.mission = 5; s.vt = 16; s.diff = -1; s.skip_intro = 1;
        if (la_check(&s, &l, &r) != 0) {
            ok = 0;
            strcpy_s(why, sizeof why, "gueltig abgelehnt");
        }
        s.vt = 12;
        if (ok && (la_check(&s, &l, &r) != 1 || strcmp(r.key, "gui.alpha_err_vt") != 0 ||
                   r.suggest.vt != 11 || r.alt_vt != 13 || r.suggest.mission != 5)) {
            ok = 0;
            strcpy_s(why, sizeof why, "VT 12");
        }
        la_check_text(&s, &r, t, sizeof t);
        if (ok && (!strstr(t, "12 (Prominence M2)") || !strstr(t, "11 Prominence M1 or 13 Prominence M1-WA"))) {
            ok = 0;
            _snprintf_s(why, sizeof why, _TRUNCATE, "Text VT 12: %.120s", t);
        }
        s.vt = 16; s.mission = 24;
        if (ok && (la_check(&s, &l, &r) != 1 || strcmp(r.key, "gui.alpha_err_mission") != 0 ||
                   r.suggest.mission != 23)) {
            ok = 0;
            strcpy_s(why, sizeof why, "Mission 24");
        }
        s.mission = 5; s.diff = 7;
        if (ok && (la_check(&s, &l, &r) != 1 || strcmp(r.key, "gui.alpha_err_diff") != 0 ||
                   r.suggest.diff != 5)) {
            ok = 0;
            strcpy_s(why, sizeof why, "Schwierigkeit 7");
        }
        s.mission = 24; s.vt = 12; s.diff = 7; s.skip_intro = 0;
        if (ok && (la_check(&s, &l, &r) != 1 || strcmp(r.key, "gui.alpha_err_mission") != 0 ||
                   r.suggest.mission != 23 || r.suggest.vt != 11 || r.suggest.diff != 5)) {
            ok = 0;
            strcpy_s(why, sizeof why, "alle drei");
        }
        la_check_text(&s, &r, t, sizeof t);
        if (ok && (!strstr(t, "Mission 24") || !strstr(t, "VT model 12") || !strstr(t, "Difficulty 7") ||
                   !strstr(t, "Mission 23, VT 11 Prominence M1 or 13 Prominence M1-WA, difficulty 5 Armageddon"))) {
            ok = 0;
            _snprintf_s(why, sizeof why, _TRUNCATE, "Text alle: %.160s", t);
        }
        la_suggest_cli(&r, t, sizeof t);
        if (ok && strcmp(t, "--alpha-mission 23 --alpha-vt 11 --alpha-diff 5") != 0) {
            ok = 0;
            _snprintf_s(why, sizeof why, _TRUNCATE, "CLI-Vorschlag '%s'", t);
        }
        /* Randfaelle */
        s.mission = -1; s.vt = 0; s.diff = -2;
        if (ok && (la_check(&s, &l, &r) != 1 || r.suggest.mission != 0 || r.suggest.vt != 1 ||
                   r.suggest.diff != -1 || r.bad_vt != 2)) {
            ok = 0;
            strcpy_s(why, sizeof why, "Rand -1/0/-2");
        }
        s.mission = 0; s.vt = 23; s.diff = 5;
        if (ok && (la_check(&s, &l, &r) != 1 || strcmp(r.key, "gui.alpha_err_vtrange") != 0 ||
                   r.suggest.vt != 22)) {
            ok = 0;
            strcpy_s(why, sizeof why, "VT 23");
        }
        s.vt = -1;
        if (ok && la_check(&s, &l, &r) != 0) {
            ok = 0;
            strcpy_s(why, sizeof why, "Training + Vorgabe");
        }
        /* Vorschlag folgt den echten Spieldaten (nur 1, 4, 13, 16) */
        memset(&syn, 0, sizeof syn);
        syn.m[1].present = syn.m[4].present = syn.m[13].present = syn.m[16].present = 1;
        s.mission = 3; s.vt = 12; s.diff = 2;
        if (ok && (la_check(&s, &syn, &r) != 1 || r.suggest.vt != 4 || r.alt_vt != 13)) {
            ok = 0;
            strcpy_s(why, sizeof why, "Vorschlag aus Spieldaten");
        }
        /* CLI: ungueltig -> Exit 3, Kette unveraendert (kein Start) */
        if (ok) {
            launcher_settings_t ls;
            char set[400] = "vr_enable=0";
            lcfg_defaults(&ls);
            _snprintf_s(ls.game_image, sizeof ls.game_image, _TRUNCATE, "%s\\fehlt.iso", base);
            s.mission = 24; s.vt = 12; s.diff = 7;
            if (la_cli_apply(base, &ls, &s, 0, set, sizeof set) != 3 || strcmp(set, "vr_enable=0") != 0) {
                ok = 0;
                strcpy_s(why, sizeof why, "CLI startet trotz Fehler");
            }
        }
        if (ok) {
            LT_OK("W272 alpha invalid combination (VT 12, mission 24, difficulty 7 -> message + suggestion, no start)");
        } else {
            LT_FAIL("W272 alpha invalid combination", "%s", why);
        }
    }

    /* (b) Teststart-Argumente (genaue Kette, US-Fall mit hdd_root_us davor) */
    {
        la_sel_t s = { 5, 16, -1, 1 };
        lcfg_gameinfo_t gi;
        char x[1600], set[2000];
        const char *last;
        int ok = 1;
        why[0] = '\0';
        if (la_extra_args(&s, "C:\\Data\\alpha_test\\HDD", "C:\\Data\\alpha_test\\HDD_US", x, sizeof x) != 0 ||
            strcmp(x, "mission_autostart=5;mission_vt=16;mission_difficulty=-1;mission_skip_intro=1;"
                      "hdd_root=C:\\Data\\alpha_test\\HDD;hdd_root_us=C:\\Data\\alpha_test\\HDD_US") != 0) {
            ok = 0;
            _snprintf_s(why, sizeof why, _TRUNCATE, "EU '%.150s'", x);
        }
        /* US: Startweg wie do_start_ex (Bildschirm + Fassung + Zusatz dahinter) */
        memset(&gi, 0, sizeof gi);
        gi.edition = LCFG_ED_US;
        gi.startable = 1;
        gi.needs_allow = 1;
        app_vr_set_args(0, "", set, sizeof set);
        if (ok && lcfg_edition_set_args(&gi, "C:\\Data\\hdd\\us", set, sizeof set) != 0) {
            ok = 0;
            strcpy_s(why, sizeof why, "Fassung");
        }
        strcat_s(set, sizeof set, ";");
        strcat_s(set, sizeof set, x);
        last = strstr(set, "hdd_root_us=");
        while (last && strstr(last + 1, "hdd_root_us=")) {
            last = strstr(last + 1, "hdd_root_us=");
        }
        if (ok && (strcmp(set, "vr_enable=0;allow_ntsc_u=1;hdd_root_us=C:\\Data\\hdd\\us;"
                               "mission_autostart=5;mission_vt=16;mission_difficulty=-1;mission_skip_intro=1;"
                               "hdd_root=C:\\Data\\alpha_test\\HDD;hdd_root_us=C:\\Data\\alpha_test\\HDD_US") != 0 ||
                   !last || strcmp(last, "hdd_root_us=C:\\Data\\alpha_test\\HDD_US") != 0)) {
            ok = 0;
            _snprintf_s(why, sizeof why, _TRUNCATE, "US '%.200s'", set);
        }
        s.mission = 0; s.vt = -1; s.diff = 3; s.skip_intro = 0;
        if (ok && (la_extra_args(&s, "C:\\a;b\\alpha_test\\HDD", "C:\\a;b\\alpha_test\\HDD_US", x, sizeof x) != 0 ||
                   strcmp(x, "mission_autostart=0;mission_vt=-1;mission_difficulty=3;mission_skip_intro=0;"
                             "hdd_root=C:\\a\\;b\\alpha_test\\HDD;hdd_root_us=C:\\a\\;b\\alpha_test\\HDD_US") != 0)) {
            ok = 0;
            _snprintf_s(why, sizeof why, _TRUNCATE, "';' maskiert '%.150s'", x);
        }
        if (ok && la_extra_args(&s, "C:\\a\"b\\HDD", "C:\\x\\HDD_US", x, sizeof x) != 1) {
            ok = 0;
            strcpy_s(why, sizeof why, "'\"' nicht abgelehnt");
        }
        if (ok && la_extra_args(&s, "C:\\\xE6\x97\xA5\\HDD", "C:\\x\\HDD_US", x, sizeof x) != 1) {
            ok = 0;
            strcpy_s(why, sizeof why, "Zeichen ausserhalb der ANSI-Codepage nicht abgelehnt");
        }
        /* CLI-Trockenlauf: Kette hinter vorhandener Kette, nichts angelegt */
        if (ok) {
            launcher_settings_t ls;
            char data[LA_P], root[LA_P], hd[LA_P];
            char c[2400] = "vr_enable=0;allow_ntsc_u=1;hdd_root_us=C:\\Data\\hdd\\us";
            int existed;
            la_sel_t cs = { 5, 16, -1, 0 };
            lcfg_defaults(&ls);
            _snprintf_s(ls.game_image, sizeof ls.game_image, _TRUNCATE, "%s\\fehlt.iso", base);
            _snprintf_s(hd, sizeof hd, _TRUNCATE, "%s\\realhdd", base);
            strcpy_s(ls.hdd_root, sizeof ls.hdd_root, hd);
            la_data_root(base, data, sizeof data);
            la_test_dirs(data, root, NULL, NULL, sizeof root);
            existed = st_exists(root);
            static const char pre[] = "vr_enable=0;allow_ntsc_u=1;hdd_root_us=C:\\Data\\hdd\\us;mission_autostart=5;"
                                      "mission_vt=16;mission_difficulty=-1;mission_skip_intro=0;hdd_root=";
            static const char suf[] = "\\alpha_test\\HDD_US";
            if (la_cli_apply(base, &ls, &cs, 1, c, sizeof c) != 0 || strncmp(c, pre, sizeof pre - 1) != 0 ||
                strlen(c) < sizeof suf || strcmp(c + strlen(c) - (sizeof suf - 1), suf) != 0 ||
                (!existed && st_exists(root))) {
                ok = 0;
                _snprintf_s(why, sizeof why, _TRUNCATE, "CLI dry '%.160s'", c);
            }
        }
        if (ok) {
            LT_OK("W272 alpha start args (exact set chain, US after edition hdd_root_us, ';' masked, '\"'/non-ANSI rejected, CLI dry-run)");
        } else {
            LT_FAIL("W272 alpha start args", "%s", why);
        }
    }

    /* (c) Testordner isoliert: echter HDD-Ordner unberuehrt (Hash), Testordner
     * frisch, Verknuepfungen nicht verfolgt, Schutz gleich/innen/enthaelt */
    {
        char real[LA_P], rus[LA_P], rf[LA_P], rf2[LA_P], data[LA_P], root[LA_P], eu[LA_P], us[LA_P];
        char h1[65], h2[65], h1b[65], h2b[65];
        const char *reals[2];
        const char *ek = NULL;
        int ok = 1, junc = 0, rc;
        why[0] = '\0';
        _snprintf_s(real, sizeof real, _TRUNCATE, "%s\\real\\HDD", base);
        _snprintf_s(rus, sizeof rus, _TRUNCATE, "%s\\real\\HDD\\us", base);
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\TDATA\\43430002", real);
        lcfg_mkdir_p(p);
        lcfg_mkdir_p(rus);
        _snprintf_s(rf, sizeof rf, _TRUNCATE, "%s\\user00.sav", p);
        st_wstr(rf, "ECHTER SPIELSTAND 1234567890");
        _snprintf_s(rf2, sizeof rf2, _TRUNCATE, "%s\\user00.sav", rus);
        st_wstr(rf2, "ECHTER US-SPIELSTAND");
        st_hash(rf, h1);
        st_hash(rf2, h2);
        reals[0] = real;
        reals[1] = rus;
        _snprintf_s(data, sizeof data, _TRUNCATE, "%s\\daten", base);
        la_test_dirs(data, root, eu, us, sizeof root);
        /* alter Testordner: Dateien, Unterordner, schreibgeschuetzt, Verknuepfung */
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\TDATA\\43430002", eu);
        lcfg_mkdir_p(p);
        _snprintf_s(q, sizeof q, _TRUNCATE, "%s\\alt.sav", p);
        st_wstr(q, "alt");
        _snprintf_s(q, sizeof q, _TRUNCATE, "%s\\alt.txt", root);
        st_wstr(q, "alt");
        _snprintf_s(q, sizeof q, _TRUNCATE, "%s\\nurlesen.txt", root);
        st_wstr(q, "ro");
        {
            wchar_t wq[LA_P];
            u8_to_w(q, wq, LA_P);
            SetFileAttributesW(wq, FILE_ATTRIBUTE_READONLY);
        }
        _snprintf_s(q, sizeof q, _TRUNCATE, "%s\\link_zum_echten_hdd", root);
        junc = st_junction(q, real);
        /* Trockenlauf aendert nichts */
        rc = la_prepare(root, reals, 2, 1, &ek);
        _snprintf_s(q, sizeof q, _TRUNCATE, "%s\\alt.txt", root);
        if (rc != 0 || !st_exists(q)) {
            ok = 0;
            strcpy_s(why, sizeof why, "Trockenlauf");
        }
        rc = la_prepare(root, reals, 2, 0, &ek);
        if (ok && (rc != 0 || st_exists(q) || !st_dir_empty(eu) || !st_dir_empty(us))) {
            ok = 0;
            _snprintf_s(why, sizeof why, _TRUNCATE, "frisch: rc=%d %s", rc, ek ? ek : "");
        }
        _snprintf_s(q, sizeof q, _TRUNCATE, "%s\\link_zum_echten_hdd", root);
        if (ok && st_exists(q)) {
            ok = 0;
            strcpy_s(why, sizeof why, "Verknuepfung nicht entfernt");
        }
        st_hash(rf, h1b);
        st_hash(rf2, h2b);
        if (ok && (!h1[0] || strcmp(h1, h1b) != 0 || strcmp(h2, h2b) != 0)) {
            ok = 0;
            strcpy_s(why, sizeof why, "echter Spielstand veraendert");
        }
        /* Schutz: gleich, innen, enthaelt (auch andere Schreibweise) */
        if (ok) {
            char t1[LA_P], t2[LA_P], t3[LA_P], t4[LA_P];
            const char *e1 = NULL, *e2 = NULL, *e3 = NULL, *e4 = NULL;
            _snprintf_s(t1, sizeof t1, _TRUNCATE, "%s\\REAL/hdd/", base);
            _snprintf_s(t2, sizeof t2, _TRUNCATE, "%s\\alpha_test", real);
            _snprintf_s(t3, sizeof t3, _TRUNCATE, "%s\\real", base);
            _snprintf_s(t4, sizeof t4, _TRUNCATE, "%s\\daten\\alpha_test", base);
            if (la_prepare(t1, reals, 2, 0, &e1) != 1 || la_prepare(t2, reals, 2, 0, &e2) != 1 ||
                la_prepare(t3, reals, 2, 0, &e3) != 1 || !e1 || strcmp(e1, "gui.alpha_err_guard") != 0 ||
                !e2 || strcmp(e2, "gui.alpha_err_guard") != 0 || !e3 || strcmp(e3, "gui.alpha_err_guard") != 0 ||
                st_exists(t2)) {
                ok = 0;
                strcpy_s(why, sizeof why, "Schutz gleich/innen/enthaelt");
            }
            if (ok && (la_path_rel(t1, real) != 1 || la_path_rel(t2, real) != 2 || la_path_rel(t3, real) != 3 ||
                       la_path_rel(t4, real) != 0)) {
                ok = 0;
                strcpy_s(why, sizeof why, "la_path_rel");
            }
            /* Testordner selbst ist eine Verknuepfung ins echte HDD -> Schutz greift ueber
             * den echten Zielpfad, nichts geloescht */
            _snprintf_s(t4, sizeof t4, _TRUNCATE, "%s\\daten2\\alpha_test", base);
            _snprintf_s(q, sizeof q, _TRUNCATE, "%s\\daten2", base);
            lcfg_mkdir_p(q);
            if (ok && st_junction(t4, real)) {
                if (la_prepare(t4, reals, 2, 0, &e4) != 1 || !e4 || strcmp(e4, "gui.alpha_err_guard") != 0) {
                    ok = 0;
                    strcpy_s(why, sizeof why, "Testordner als Verknuepfung ins HDD");
                }
                st_rmtree(t4);
            }
            st_hash(rf, h1b);
            st_hash(rf2, h2b);
            if (ok && (strcmp(h1, h1b) != 0 || strcmp(h2, h2b) != 0)) {
                ok = 0;
                strcpy_s(why, sizeof why, "echter Spielstand nach Schutzfaellen veraendert");
            }
        }
        printf("     (W272 c: Verknuepfung im Testordner %s)\n", junc ? "angelegt + nur entfernt" : "nicht anlegbar (uebersprungen)");
        if (ok) {
            LT_OK("W272 alpha test folder isolated (real HDD hash unchanged, folder fresh, links not followed, guard equal/inside/contains)");
        } else {
            LT_FAIL("W272 alpha test folder", "%s", why);
        }
    }

    /* A/B-Schalter: user.cfg gewinnt vor sb_native.cfg, Setzen ersetzt/haengt an,
     * Schluessel-Erkennung ueber sb_native.cfg bzw. Literal in sb_emu.dll */
    {
        char uc[LA_P], nc[LA_P], buf[600];
        FILE *f = NULL;
        size_t n = 0;
        int ok = 1;
        static const char dll[] = "MZ\0\0interp_guardx\0interp_zcons\0\0xinterp_guard\0";
        why[0] = '\0';
        _snprintf_s(uc, sizeof uc, _TRUNCATE, "%s\\user.cfg", base);
        _snprintf_s(nc, sizeof nc, _TRUNCATE, "%s\\sb_native.cfg", base);
        st_wstr(nc, "[render]\ninterp_hlerp=2\n# interp_guard=1\n");
        st_wstr(uc, "[display]\nwidth=800\n[render]\ninterp_zcons = 0\ninterp_zcons=2\n");
        if (la_cfg_get_int(uc, nc, "interp_zcons", 1) != 2 || la_cfg_get_int(uc, nc, "interp_hlerp", 0) != 2 ||
            la_cfg_get_int(uc, nc, "interp_guard", 7) != 7) {
            ok = 0;
            strcpy_s(why, sizeof why, "lesen");
        }
        if (ok && (la_cfg_set(uc, "interp_zcons", "1") != 0 || la_cfg_set(uc, "interp_hlerp", "0") != 0)) {
            ok = 0;
            strcpy_s(why, sizeof why, "schreiben");
        }
        if (ok && u8_fopen_s(&f, uc, "r") == 0 && f) {
            n = fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
        }
        buf[n] = '\0';
        if (ok && strcmp(buf, "[display]\nwidth=800\n[render]\ninterp_zcons=1\n[render]\ninterp_hlerp=0\n") != 0) {
            ok = 0;
            _snprintf_s(why, sizeof why, _TRUNCATE, "Datei '%.100s'", buf);
        }
        if (ok && (la_cfg_get_int(uc, nc, "interp_zcons", 1) != 1 || la_cfg_get_int(uc, nc, "interp_hlerp", 1) != 0)) {
            ok = 0;
            strcpy_s(why, sizeof why, "Rundweg");
        }
        _snprintf_s(q, sizeof q, _TRUNCATE, "%s\\sb_emu.dll", base);
        st_write(q, dll, sizeof dll);
        if (ok && (la_client_knows_key(base, "interp_guard") || !la_client_knows_key(base, "interp_zcons"))) {
            ok = 0;
            strcpy_s(why, sizeof why, "DLL-Literal");
        }
        st_wstr(nc, "interp_guard=1\n");
        if (ok && !la_client_knows_key(base, "interp_guard")) {
            ok = 0;
            strcpy_s(why, sizeof why, "sb_native.cfg-Zeile");
        }
        if (ok) {
            LT_OK("W272 alpha A/B switches (user.cfg over sb_native.cfg, set/replace, key detection cfg/DLL literal)");
        } else {
            LT_FAIL("W272 alpha A/B switches", "%s", why);
        }
    }
    sb_lang_set(keep);
    st_rmtree(base);
}
