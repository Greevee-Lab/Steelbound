/* W199 SAVE-IMPORT. All input is staged in memory before any HDD mutation.
 * Only the game's title directories are replaced; cache/backup/savelock stay out.
 * ZIP32 stored/deflate, bounded sizes and CRC; no shell or external runtime. */
#include "launcher_saves.h"
#include "sb_savecheck.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#define LIM (128u << 20)
#define MAXFILES 4096
#define PATHN 1024
extern int sb_save_inflate(const unsigned char *, size_t, unsigned char **, size_t);
typedef struct
{
    wchar_t name[240];
    unsigned char *data;
    unsigned size;
} item;
struct ls_plan
{
    wchar_t hdd[PATHN];
    item *v;
    int n;
    unsigned total;
    ls_counts c;
    const char *reason;
};
static const char *bad = "gui.si_invalid";
static int join(wchar_t *out, const wchar_t *a, const wchar_t *b)
{
    return _snwprintf_s(out, PATHN, _TRUNCATE, L"%s\\%s", a, b) >= 0;
}
static int exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
static int mkdirs(const wchar_t *p)
{
    wchar_t b[PATHN];
    size_t i;
    if (wcslen(p) < 3 || wcslen(p) >= PATHN)
        return 0;
    wcscpy_s(b, PATHN, p);
    for (i = 3; b[i]; ++i)
        if (b[i] == '\\' || b[i] == '/')
        {
            wchar_t c = b[i];
            b[i] = 0;
            if (!CreateDirectoryW(b, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
                return 0;
            b[i] = c;
        }
    return CreateDirectoryW(b, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}
static int safe_tree(const wchar_t *p)
{
    wchar_t b[PATHN];
    size_t i;
    DWORD a;
    if (wcslen(p) < 3 || wcslen(p) >= PATHN)
        return 0;
    wcscpy_s(b, PATHN, p);
    for (i = 3;; i++)
        if (!b[i] || b[i] == '\\' || b[i] == '/')
        {
            wchar_t c = b[i];
            b[i] = 0;
            a = GetFileAttributesW(b);
            b[i] = c;
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT))
                return 0;
            if (!c)
                break;
        }
    return 1;
}
static int writebytes(const wchar_t *p, const void *d, unsigned n)
{
    wchar_t parent[PATHN], *s;
    HANDLE h;
    DWORD w;
    int ok;
    wcscpy_s(parent, PATHN, p);
    s = wcsrchr(parent, '\\');
    if (s)
    {
        *s = 0;
        if (!mkdirs(parent))
            return 0;
    }
    h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    ok = WriteFile(h, d, n, &w, NULL) && w == n && FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}
static unsigned char *readbytes(const wchar_t *p, unsigned *n)
{
    HANDLE h;
    LARGE_INTEGER sz;
    DWORD r;
    unsigned char *d = NULL;
    h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart >= 0 && sz.QuadPart <= LIM)
    {
        *n = (unsigned)sz.QuadPart;
        d = malloc(*n + 1);
        if (d && (!ReadFile(h, d, *n, &r, NULL) || r != *n))
        {
            free(d);
            d = NULL;
        }
    }
    CloseHandle(h);
    return d;
}
static void remove_tree(const wchar_t *p)
{
    WIN32_FIND_DATAW f;
    HANDLE h;
    wchar_t q[PATHN], child[PATHN];
    if (!safe_tree(p))
        return;
    join(q, p, L"*");
    h = FindFirstFileW(q, &f);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L".."))
                continue;
            join(child, p, f.cFileName);
            if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                remove_tree(child);
            else
                DeleteFileW(child);
        } while (FindNextFileW(h, &f));
        FindClose(h);
    }
    RemoveDirectoryW(p);
}
/* Return 1 mapped, 0 unrelated file, -1 unsafe/foreign title. Xbox names are ASCII. */
static int mapname(const wchar_t *input, wchar_t *out)
{
    wchar_t buf[512], *parts[32], *ctx = NULL, *s;
    int n = 0, i, t = -1, title = -1, base;
    const wchar_t *drive = NULL;
    if (!*input || wcslen(input) >= 500 || input[0] == '/' || input[0] == '\\')
        return -1;
    wcscpy_s(buf, 512, input);
    for (i = 0; buf[i]; i++)
    {
        if (buf[i] == '\\')
            buf[i] = '/';
        if (buf[i] < 32 || wcschr(L":*?\"<>|", buf[i]))
            return -1;
    }
    for (s = wcstok_s(buf, L"/", &ctx); s; s = wcstok_s(NULL, L"/", &ctx))
    {
        size_t len = wcslen(s);
        int hex = 1;
        if (n == 32 || !wcscmp(s, L".") || !wcscmp(s, L"..") || s[len - 1] == '.' ||
            s[len - 1] == ' ')
            return -1;
        {
            size_t stem = wcscspn(s, L".");
            if ((stem == 3 && (!_wcsnicmp(s, L"CON", 3) || !_wcsnicmp(s, L"NUL", 3) ||
                               !_wcsnicmp(s, L"AUX", 3) || !_wcsnicmp(s, L"PRN", 3))) ||
                (stem == 4 && s[3] >= '1' && s[3] <= '9' &&
                 (!_wcsnicmp(s, L"COM", 3) || !_wcsnicmp(s, L"LPT", 3))))
                return -1;
        }
        parts[n] = s;
        if (!_wcsicmp(s, L"TDATA") || !_wcsicmp(s, L"UDATA"))
        {
            if (t >= 0)
                return -1;
            t = n;
            drive = (!_wcsicmp(s, L"TDATA")) ? L"TDATA" : L"UDATA";
        }
        for (i = 0; s[i]; i++)
            if (!iswxdigit(s[i]))
                hex = 0;
        if (len == 8 && hex)
        {
            if (_wcsicmp(s, SB_SAVE_TITLE_W))
                return -2;
            title = n;
        }
        n++;
    }
    if (!n)
        return -1;
    base = t >= 0 ? t + 1 : (title >= 0 ? title + 1 : n - 1);
    if (base < n && !_wcsicmp(parts[base], SB_SAVE_TITLE_W))
        base++;
    if (base >= n)
        return 0;
    if (!drive)
        drive = sb_save_data_size(parts[n - 1]) ? L"TDATA" : L"UDATA";
    if (!wcscmp(drive, L"TDATA"))
    {
        if (base != n - 1 || !sb_save_data_size(parts[n - 1]))
            return 0;
    }
    else
    {
        const wchar_t *ext = wcsrchr(parts[n - 1], '.');
        if (title < 0 && t < 0)
            return 0;
        if (!ext || (_wcsicmp(ext, L".rep") && _wcsicmp(ext, L".xbx")))
            return 0;
    }
    for (i = base; i < n; i++)
    {
        const wchar_t *q = parts[i];
        while (*q)
            if (*q++ > 126)
                return -1;
    }
    _snwprintf_s(out, 240, _TRUNCATE, L"%s\\43430002", drive);
    for (i = base; i < n; i++)
        if (wcslen(out) + wcslen(parts[i]) + 2 >= 240)
            return -1;
        else
        {
            wcscat_s(out, 240, L"\\");
            wcscat_s(out, 240, parts[i]);
        }
    return 1;
}
static int add(ls_plan *p, const wchar_t *raw, const unsigned char *d, unsigned n)
{
    wchar_t name[240], dest[PATHN];
    int m = mapname(raw, name), i;
    const wchar_t *fn, *ext;
    item *v;
    if (m == -2)
        p->reason = "gui.si_foreign";
    if (m <= 0)
        return m == 0;
    if (!wcsncmp(name, L"TDATA", 5))
    {
        unsigned expected = sb_save_data_size(wcsrchr(name, '\\') + 1);
        if (n != expected && n != expected + 20)
            return 0;
    }
    if (p->n >= MAXFILES || n > LIM - p->total)
        return 0;
    for (i = 0; i < p->n; i++)
        if (!_wcsicmp(p->v[i].name, name))
            return 0;
    v = &p->v[p->n];
    v->data = malloc(n + 1);
    if (!v->data)
        return 0;
    memcpy(v->data, d, n);
    v->size = n;
    wcscpy_s(v->name, 240, name);
    p->n++;
    p->total += n;
    fn = wcsrchr(name, '\\') + 1;
    ext = wcsrchr(fn, '.');
    p->c.files++;
    if (!wcsncmp(name, L"TDATA", 5))
    {
        p->c.saves++;
        if (!_wcsnicmp(fn, L"user", 4) && _wcsicmp(fn, L"user99.sav"))
            p->c.pilots++;
    }
    if (ext && !_wcsicmp(ext, L".rep"))
        p->c.replays++;
    if (join(dest, p->hdd, name) && exists(dest))
        p->c.conflicts++;
    return 1;
}
static int walk(ls_plan *p, const wchar_t *root, const wchar_t *rel, int depth)
{
    wchar_t dir[PATHN], pat[PATHN], path[PATHN], r[PATHN];
    WIN32_FIND_DATAW f;
    HANDLE h;
    int ok = 1;
    if (depth > 16 || !join(dir, root, rel) || !safe_tree(dir) || !join(pat, dir, L"*"))
        return 0;
    h = FindFirstFileW(pat, &f);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    do
    {
        unsigned n;
        unsigned char *d;
        wchar_t mapped[240];
        if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L".."))
            continue;
        if (f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        {
            ok = 0;
            break;
        }
        if (*rel)
            join(r, rel, f.cFileName);
        else
            wcscpy_s(r, PATHN, f.cFileName);
        if (!join(path, root, r))
        {
            ok = 0;
            break;
        }
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (!walk(p, root, r, depth + 1))
            {
                ok = 0;
                break;
            }
        }
        else
        {
            int m = mapname(r, mapped);
            if (m < 0)
            {
                if (m == -2)
                    p->reason = "gui.si_foreign";
                ok = 0;
                break;
            }
            if (!m)
                continue;
            d = readbytes(path, &n);
            if (!d)
            {
                ok = 0;
                break;
            }
            ok = add(p, r, d, n);
            free(d);
            if (!ok)
                break;
        }
    } while (FindNextFileW(h, &f));
    FindClose(h);
    return ok;
}
static unsigned u16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned u32(const unsigned char *p) { return u16(p) | (u16(p + 2) << 16); }
static unsigned crc(const unsigned char *p, unsigned n)
{
    unsigned c = ~0u, i;
    while (n--)
    {
        c ^= *p++;
        for (i = 0; i < 8; i++)
            c = (c >> 1) ^ ((0 - (c & 1)) & 0xedb88320u);
    }
    return ~c;
}
static int zipread(ls_plan *p, const wchar_t *path)
{
    unsigned n = 0, off, count, i, end;
    unsigned char *d = readbytes(path, &n);
    int ok = 0;
    long e;
    if (!d || n < 22)
        goto done;
    for (e = (long)n - 22; e >= 0 && e >= (long)n - 65557; e--)
        if (u32(d + e) == 0x06054b50 && e + 22 + u16(d + e + 20) == n)
            break;
    if (e < 0 || e < (long)n - 65557 || u16(d + e + 4) || u16(d + e + 6))
        goto done;
    count = u16(d + e + 10);
    off = u32(d + e + 16);
    end = off + u32(d + e + 12);
    if (!count || count > MAXFILES || u16(d + e + 8) != count || end < off || end != (unsigned)e)
        goto done;
    for (i = 0; i < count; i++)
    {
        unsigned fn, ex, co, cs, us, lo, start, method, flags, j;
        wchar_t name[512], mapped[240];
        unsigned char *bytes = NULL;
        int m;
        if (off > end || end - off < 46 || u32(d + off) != 0x02014b50)
            goto done;
        fn = u16(d + off + 28);
        ex = u16(d + off + 30);
        co = u16(d + off + 32);
        cs = u32(d + off + 20);
        us = u32(d + off + 24);
        lo = u32(d + off + 42);
        method = u16(d + off + 10);
        flags = u16(d + off + 8);
        if (!fn || fn >= 500 || 46 + fn + ex + co > end - off || (flags & 1) ||
            us > LIM - p->total || cs > LIM || lo > n || n - lo < 30 || u32(d + lo) != 0x04034b50)
            goto done;
        for (j = 0; j < fn; j++)
            if (!d[off + 46 + j])
                goto done;
        j = MultiByteToWideChar((flags & 0x800) ? CP_UTF8 : 437,
                                (flags & 0x800) ? MB_ERR_INVALID_CHARS : 0,
                                (const char *)d + off + 46, fn, name, 511);
        if (!j)
            goto done;
        name[j] = 0;
        m = mapname(name, mapped);
        if (m < 0)
        {
            if (m == -2)
                p->reason = "gui.si_foreign";
            goto done;
        }
        start = lo + 30 + u16(d + lo + 26) + u16(d + lo + 28);
        if (start < lo || start > off || cs > off - start || u16(d + lo + 8) != method)
            goto done;
        if (name[j - 1] != '/' && name[j - 1] != '\\' && m)
        {
            if (method == 0)
            {
                if (cs != us)
                    goto done;
                bytes = malloc(us + 1);
                if (bytes)
                    memcpy(bytes, d + start, us);
            }
            else if (method == 8)
            {
                if (!sb_save_inflate(d + start, cs, &bytes, us))
                    goto done;
            }
            else
                goto done;
            if (!bytes)
                goto done;
            if (crc(bytes, us) != u32(d + off + 16) || !add(p, name, bytes, us))
            {
                free(bytes);
                goto done;
            }
            free(bytes);
        }
        off += 46 + fn + ex + co;
    }
    ok = off == end;
done:
    free(d);
    return ok;
}
ls_plan *ls_open(const wchar_t *source, const wchar_t *hdd, ls_counts *counts, const char **error)
{
    ls_plan *p = calloc(1, sizeof *p);
    DWORD a;
    int ok = 0;
    wchar_t root[PATHN], *s;
    const wchar_t *ext;
    *error = bad;
    if (!p)
        return NULL;
    p->v = calloc(MAXFILES, sizeof(item));
    if (!p->v || wcslen(hdd) >= PATHN || wcslen(source) >= PATHN || !safe_tree(source) ||
        !safe_tree(hdd))
        goto fail;
    /* A selected file/TDATA subfolder must not hide a foreign parent title. */
    {
        wchar_t ancestry[PATHN], *ctx, *part;
        wcscpy_s(ancestry, PATHN, source);
        for (part = wcstok_s(ancestry, L"/\\", &ctx); part; part = wcstok_s(NULL, L"/\\", &ctx))
        {
            size_t k;
            int hex = 1;
            for (k = 0; part[k]; k++)
                if (!iswxdigit(part[k]))
                    hex = 0;
            if (hex && k == 8 && _wcsicmp(part, SB_SAVE_TITLE_W))
            {
                p->reason = "gui.si_foreign";
                goto fail;
            }
        }
    }
    wcscpy_s(p->hdd, PATHN, hdd);
    a = GetFileAttributesW(source);
    if (a == INVALID_FILE_ATTRIBUTES)
        goto fail;
    if (a & FILE_ATTRIBUTE_DIRECTORY)
    {
        /* Include the selected basename so a foreign title root is rejected. */
        wcscpy_s(root, PATHN, source);
        while (wcslen(root) > 3 &&
               (root[wcslen(root) - 1] == '\\' || root[wcslen(root) - 1] == '/'))
            root[wcslen(root) - 1] = 0;
        s = wcsrchr(root, '\\');
        {
            wchar_t *t = wcsrchr(root, '/');
            if (t && (!s || t > s))
                s = t;
        }
        if (s)
        {
            wchar_t leaf[PATHN];
            size_t k;
            int title = 1;
            wcscpy_s(leaf, PATHN, s + 1);
            for (k = 0; leaf[k]; k++)
                if (!iswxdigit(leaf[k]))
                    title = 0;
            if ((title && k == 8) || !_wcsicmp(leaf, L"TDATA") || !_wcsicmp(leaf, L"UDATA"))
            {
                *s = 0;
                ok = walk(p, root, leaf, 0);
            }
            else
                ok = walk(p, source, L"", 0);
        }
        else
            ok = walk(p, source, L"", 0);
    }
    else
    {
        ext = wcsrchr(source, '.');
        if (ext && !_wcsicmp(ext, L".zip"))
            ok = zipread(p, source);
        else
        {
            unsigned n;
            unsigned char *d = readbytes(source, &n);
            s = wcsrchr(source, '\\');
            {
                wchar_t *t = wcsrchr(source, '/');
                if (t && (!s || t > s))
                    s = t;
            }
            if (d)
            {
                ok = add(p, s ? s + 1 : source, d, n);
                free(d);
            }
        }
    }
    if (!ok || !p->n)
        goto fail;
    *counts = p->c;
    *error = NULL;
    return p;
fail:
    if (p->reason)
        *error = p->reason;
    ls_close(p);
    return NULL;
}
void ls_close(ls_plan *p)
{
    int i;
    if (!p)
        return;
    for (i = 0; i < p->n; i++)
        free(p->v[i].data);
    free(p->v);
    free(p);
}
void ls_conflict_names(const ls_plan *p, wchar_t *out, size_t cap)
{
    int i, shown = 0;
    wchar_t dest[PATHN];
    if (!cap)
        return;
    out[0] = 0;
    for (i = 0; i < p->n && shown < 8; i++)
    {
        if (join(dest, p->hdd, p->v[i].name) && exists(dest))
        {
            if (wcslen(out) + wcslen(p->v[i].name) + 2 >= cap)
                break;
            wcscat_s(out, cap, p->v[i].name);
            wcscat_s(out, cap, L"\n");
            shown++;
        }
    }
}
static int copytree(const wchar_t *src, const wchar_t *dst, int depth)
{
    wchar_t pat[PATHN], s[PATHN], d[PATHN];
    WIN32_FIND_DATAW f;
    HANDLE h;
    int ok = 1;
    if (depth > 16 || !safe_tree(src) || !safe_tree(dst) || !mkdirs(dst))
        return 0;
    if (!exists(src))
        return 1;
    join(pat, src, L"*");
    h = FindFirstFileW(pat, &f);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    do
    {
        if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L".."))
            continue;
        if (f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        {
            ok = 0;
            break;
        }
        if (!join(s, src, f.cFileName) || !join(d, dst, f.cFileName))
        {
            ok = 0;
            break;
        }
        ok = (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? copytree(s, d, depth + 1)
                                                             : CopyFileW(s, d, FALSE);
        if (!ok)
            break;
    } while (FindNextFileW(h, &f));
    FindClose(h);
    return ok;
}
int ls_import(ls_plan *p, int mode, const char **error)
{
    wchar_t backup[PATHN], stage[PATHN], s[PATHN], d[PATHN], old[2][PATHN], cur[2][PATHN],
        fresh[2][PATHN], name[100];
    SYSTEMTIME tm;
    static LONG serial;
    int i, moved[2] = {0}, installed[2] = {0}, ok = 0;
    *error = "gui.si_io";
    if (!mode)
    {
        *error = NULL;
        return 0;
    }
    if (mode != 1 && mode != 2)
        return -1;
    if (!safe_tree(p->hdd) || !mkdirs(p->hdd))
        return -1;
    GetLocalTime(&tm);
    _snwprintf_s(name, 100, _TRUNCATE, L"_backup\\import-%04u%02u%02u-%02u%02u%02u-%lu-%llu",
                 tm.wYear, tm.wMonth, tm.wDay, tm.wHour, tm.wMinute, tm.wSecond,
                 GetCurrentProcessId(),
                 GetTickCount64() * 1000 + (unsigned)InterlockedIncrement(&serial));
    if (!join(backup, p->hdd, name) || exists(backup) || !safe_tree(backup) || !mkdirs(backup) ||
        !join(stage, backup, L"_stage") || !mkdirs(stage))
        return -1;
    for (i = 0; i < 2; i++)
    {
        const wchar_t *rel = i ? L"UDATA\\43430002" : L"TDATA\\43430002";
        join(cur[i], p->hdd, rel);
        join(old[i], backup, rel);
        join(fresh[i], stage, rel);
        if (!copytree(cur[i], fresh[i], 0))
            goto done;
    }
    for (i = 0; i < p->n; i++)
    {
        if (!join(d, stage, p->v[i].name))
            goto done;
        if (mode == 2 && exists(d))
            continue;
        if (!writebytes(d, p->v[i].data, p->v[i].size))
            goto done;
    }
    /* Prepare both trees completely, then rename. Roll back any failed rename. */
    for (i = 0; i < 2; i++)
    {
        join(s, backup, i ? L"UDATA" : L"TDATA");
        join(d, p->hdd, i ? L"UDATA" : L"TDATA");
        if (!mkdirs(s) || !mkdirs(d))
            goto rollback;
        if (exists(cur[i]))
        {
            if (!MoveFileW(cur[i], old[i]))
                goto rollback;
            moved[i] = 1;
        }
        if (!MoveFileW(fresh[i], cur[i]))
            goto rollback;
        installed[i] = 1;
    }
    ok = 1;
    goto done;
rollback:
    for (i = 1; i >= 0; i--)
    {
        if (installed[i])
            remove_tree(cur[i]);
        if (moved[i] && !MoveFileW(old[i], cur[i]))
            *error = "gui.si_recovery";
    }
done:
    remove_tree(stage);
    if (ok)
        *error = NULL;
    return ok ? 1 : -1;
}
static void put16(FILE *f, unsigned v)
{
    fputc(v & 255, f);
    fputc((v >> 8) & 255, f);
}
static void put32(FILE *f, unsigned v)
{
    put16(f, v);
    put16(f, v >> 16);
}
static int zipwrite(ls_plan *p, const wchar_t *path)
{
    FILE *f = NULL;
    unsigned offsets[MAXFILES], cs[MAXFILES], start, end;
    int i;
    char name[240];
    if (_wfopen_s(&f, path, L"wbx") || !f)
        return 0;
    for (i = 0; i < p->n; i++)
    {
        unsigned j, n = (unsigned)wcslen(p->v[i].name);
        for (j = 0; j < n; j++)
            name[j] = p->v[i].name[j] == '\\' ? '/' : (char)p->v[i].name[j];
        offsets[i] = (unsigned)ftell(f);
        cs[i] = crc(p->v[i].data, p->v[i].size);
        put32(f, 0x04034b50);
        put16(f, 20);
        put16(f, 0);
        put16(f, 0);
        put16(f, 0);
        put16(f, 33);
        put32(f, cs[i]);
        put32(f, p->v[i].size);
        put32(f, p->v[i].size);
        put16(f, n);
        put16(f, 0);
        fwrite(name, 1, n, f);
        fwrite(p->v[i].data, 1, p->v[i].size, f);
    }
    start = (unsigned)ftell(f);
    for (i = 0; i < p->n; i++)
    {
        unsigned j, n = (unsigned)wcslen(p->v[i].name);
        for (j = 0; j < n; j++)
            name[j] = p->v[i].name[j] == '\\' ? '/' : (char)p->v[i].name[j];
        put32(f, 0x02014b50);
        put16(f, 20);
        put16(f, 20);
        put16(f, 0);
        put16(f, 0);
        put16(f, 0);
        put16(f, 33);
        put32(f, cs[i]);
        put32(f, p->v[i].size);
        put32(f, p->v[i].size);
        put16(f, n);
        put16(f, 0);
        put16(f, 0);
        put16(f, 0);
        put16(f, 0);
        put32(f, 0);
        put32(f, offsets[i]);
        fwrite(name, 1, n, f);
    }
    end = (unsigned)ftell(f);
    put32(f, 0x06054b50);
    put16(f, 0);
    put16(f, 0);
    put16(f, p->n);
    put16(f, p->n);
    put32(f, end - start);
    put32(f, start);
    put16(f, 0);
    {
        int ok = !ferror(f);
        if (fclose(f))
            ok = 0;
        if (!ok)
            DeleteFileW(path);
        return ok;
    }
}
int ls_export(const wchar_t *hdd, const wchar_t *zip, const char **error)
{
    ls_plan *p = calloc(1, sizeof *p);
    int ok = 0;
    *error = "gui.si_io";
    if (!p)
        return -1;
    p->v = calloc(MAXFILES, sizeof(item));
    if (!p->v)
        goto done;
    if (!safe_tree(hdd) || !safe_tree(zip))
        goto done;
    /* Explicit roots exclude backups, _savelock and other titles. */
    if (!walk(p, hdd, L"TDATA\\43430002", 0) || !walk(p, hdd, L"UDATA\\43430002", 0))
        goto done;
    if (!p->n)
    {
        *error = "gui.si_empty";
        goto done;
    }
    ok = zipwrite(p, zip);
    if (ok)
        *error = NULL;
done:
    ls_close(p);
    return ok ? 1 : -1;
}
int ls_selftest(void)
{
    wchar_t tmp[PATHN], root[PATHN], src[PATHN], dst[PATHN], file[PATHN], zip[PATHN], dest2[PATHN];
    const wchar_t *layouts[] = {L"TDATA\\43430002\\user01.sav", L"43430002\\user01.sav",
                                L"43430002\\TDATA\\user01.sav", L"user01.sav"};
    unsigned char data[0xAE8 + 20] = {0};
    const char *err;
    ls_counts c;
    ls_plan *p = NULL;
    int ok = 1, i;
    unsigned n;
    unsigned char *read;
#define CHECK(x)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        if (!(x))                                                                                  \
        {                                                                                          \
            printf("save-import: check %d failed\n", __LINE__);                                    \
            ok = 0;                                                                                \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
    GetTempPathW(PATHN, tmp);
    _snwprintf_s(root, PATHN, _TRUNCATE, L"%sSB-W199-%lu-%llu", tmp, GetCurrentProcessId(),
                 GetTickCount64());
    CHECK(mkdirs(root));
    join(src, root, L"source_\u00fc");
    join(dst, root, L"hdd_\u00fc");
    join(zip, root, L"roundtrip.zip");
    join(dest2, root, L"roundtrip");
    for (i = 0; i < 4; i++)
    {
        join(file, src, layouts[i]);
        data[0] = (unsigned char)(i + 1);
        CHECK(writebytes(file, data, sizeof data));
        p = ls_open(i == 3 ? file : src, dst, &c, &err);
        CHECK(p && c.pilots == 1 && c.saves == 1);
        CHECK(ls_import(p, 0, &err) == 0);
        CHECK(!exists(dst));
        CHECK(ls_import(p, 1, &err) == 1);
        ls_close(p);
        p = NULL;
        join(file, dst, L"TDATA\\43430002\\user01.sav");
        read = readbytes(file, &n);
        CHECK(read && n == sizeof data && read[0] == i + 1);
        free(read);
        remove_tree(src);
        remove_tree(dst);
    }
    join(file, src, L"43430002\\TDATA\\user01.sav");
    CHECK(writebytes(file, data, sizeof data));
    join(file, src, L"43430002\\UDATA\\43430002\\ABCDEF123456\\st00_0.rep");
    CHECK(writebytes(file, "replay", 6));
    p = ls_open(src, dst, &c, &err);
    CHECK(p && c.replays == 1 && c.files == 2);
    CHECK(ls_import(p, 1, &err) == 1);
    ls_close(p);
    p = NULL;
    join(file, src, L"43430002\\TDATA\\user01.sav");
    data[0] = 99;
    CHECK(writebytes(file, data, sizeof data));
    p = ls_open(src, dst, &c, &err);
    CHECK(p && c.conflicts == 2);
    CHECK(ls_import(p, 2, &err) == 1);
    join(file, dst, L"TDATA\\43430002\\user01.sav");
    read = readbytes(file, &n);
    CHECK(read && read[0] == 4);
    free(read);
    CHECK(ls_import(p, 1, &err) == 1);
    ls_close(p);
    p = NULL;
    read = readbytes(file, &n);
    CHECK(read && read[0] == 99);
    free(read);
    {
        WIN32_FIND_DATAW fd;
        HANDLE h;
        wchar_t pat[PATHN], b[PATHN];
        int found = 0;
        join(pat, dst, L"_backup\\import-*");
        h = FindFirstFileW(pat, &fd);
        CHECK(h != INVALID_HANDLE_VALUE);
        do
        {
            join(b, dst, L"_backup");
            join(pat, b, fd.cFileName);
            join(b, pat, L"TDATA\\43430002\\user01.sav");
            read = readbytes(b, &n);
            if (read)
            {
                if (read[0] == 4)
                    found = 1;
                free(read);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        CHECK(found);
    }
    /* Neither another title nor cache/backup/protected working copies belong in exports. */
    join(file, dst, L"_savelock\\TDATA\\43430002\\user02.sav");
    CHECK(writebytes(file, data, sizeof data));
    join(file, dst, L"TDATA\\12345678\\user02.sav");
    CHECK(writebytes(file, data, sizeof data));
    join(file, dst, L"ZCACHE\\user02.sav");
    CHECK(writebytes(file, data, sizeof data));
    CHECK(ls_export(dst, zip, &err) == 1);
    p = ls_open(zip, dest2, &c, &err);
    CHECK(p && c.pilots == 1 && c.replays == 1 && c.files == 2);
    CHECK(ls_import(p, 1, &err) == 1);
    ls_close(p);
    p = NULL;
    join(file, dest2, L"TDATA\\43430002\\user01.sav");
    read = readbytes(file, &n);
    CHECK(read && n == sizeof data && !memcmp(read, data, n));
    free(read);
    remove_tree(src);
    join(file, src, L"TDATA\\12345678\\user01.sav");
    CHECK(writebytes(file, data, sizeof data));
    p = ls_open(src, dst, &c, &err);
    CHECK(!p);
    join(file, src, L"TDATA\\12345678");
    p = ls_open(file, dst, &c, &err);
    CHECK(!p);
    {
        wchar_t m[240];
        CHECK(mapname(L"../TDATA/43430002/user01.sav", m) < 0);
        CHECK(mapname(L"C:/user01.sav", m) < 0);
        CHECK(mapname(L"TDATA/43430002/user01.sav:stream", m) < 0);
    }

    /* Short files cannot be confused with another game's loose .sav files. */
    remove_tree(src);
    join(file, src, L"user01.sav");
    CHECK(writebytes(file, "bad", 3));
    p = ls_open(file, dst, &c, &err);
    CHECK(!p);
    /* All known record types, real-size synthetic fixtures, preserved signatures. */
    {
        const wchar_t *names[] = {L"config.sav", L"death00.sav", L"mslog00.sav", L"sasen00.sav",
                                  L"user99.sav"};
        remove_tree(src);
        for (i = 0; i < 5; i++)
        {
            unsigned size = sb_save_data_size(names[i]) + 20;
            unsigned char *b = calloc(1, size);
            CHECK(b);
            b[size - 1] = 0x73;
            join(file, src, names[i]);
            CHECK(writebytes(file, b, size));
            free(b);
        }
        p = ls_open(src, dst, &c, &err);
        CHECK(p && c.saves == 5 && c.pilots == 0);
        CHECK(ls_import(p, 1, &err) == 1);
        ls_close(p);
        p = NULL;
        join(file, dst, L"TDATA\\43430002\\config.sav");
        read = readbytes(file, &n);
        CHECK(read && read[n - 1] == 0x73);
        free(read);
    }
    /* Locked existing file: no partial mutation, no loss of old bytes. */
    p = ls_open(src, dst, &c, &err);
    CHECK(p);
    {
        HANDLE lock = CreateFileW(file, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
        CHECK(lock != INVALID_HANDLE_VALUE);
        i = ls_import(p, 1, &err);
        CloseHandle(lock);
        CHECK(i == -1);
        read = readbytes(file, &n);
        CHECK(read && read[n - 1] == 0x73);
        free(read);
    }
    ls_close(p);
    p = NULL;
    /* Force the second rename to fail: the already replaced TDATA must roll back. */
    p = ls_open(src, dst, &c, &err);
    CHECK(p);
    {
        wchar_t udir[PATHN];
        HANDLE lock;
        join(udir, dst, L"UDATA\\43430002");
        lock = CreateFileW(udir, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        CHECK(lock != INVALID_HANDLE_VALUE);
        for (i = 0; i < p->n; i++)
            p->v[i].data[0] = 77;
        i = ls_import(p, 1, &err);
        CloseHandle(lock);
        CHECK(i == -1);
        join(file, dst, L"TDATA\\43430002\\config.sav");
        read = readbytes(file, &n);
        CHECK(read && read[0] == 0 && read[n - 1] == 0x73);
        free(read);
    }
    ls_close(p);
    p = NULL;
    /* Ambiguous duplicate destinations reject the entire selection. */
    join(file, src, L"TDATA\\43430002\\config.sav");
    CHECK(writebytes(file, data, 0x2c + 20));
    p = ls_open(src, dst, &c, &err);
    CHECK(!p);
    /* Existing export destination is never truncated. */
    CHECK(ls_export(dst, zip, &err) == -1);
    /* Independently generated deflate fixture (Python zlib), no game data. */
    {
        static const unsigned char deflated[] = {
            80,  75,  3,   4,   20,  0,   0,   0,  8,   0,  198, 185, 103, 93,  156, 36,  22,
            188, 19,  0,   0,   0,   252, 10,  0,  0,   25, 0,   0,   0,   84,  68,  65,  84,
            65,  47,  52,  51,  52,  51,  48,  48, 48,  50, 47,  117, 115, 101, 114, 48,  49,
            46,  115, 97,  118, 237, 193, 1,   1,  0,   0,  0,   130, 32,  255, 175, 110, 72,
            64,  1,   0,   192, 135, 1,   80,  75, 1,   2,  20,  0,   20,  0,   0,   0,   8,
            0,   198, 185, 103, 93,  156, 36,  22, 188, 19, 0,   0,   0,   252, 10,  0,   0,
            25,  0,   0,   0,   0,   0,   0,   0,  0,   0,  0,   0,   128, 1,   0,   0,   0,
            0,   84,  68,  65,  84,  65,  47,  52, 51,  52, 51,  48,  48,  48,  50,  47,  117,
            115, 101, 114, 48,  49,  46,  115, 97, 118, 80, 75,  5,   6,   0,   0,   0,   0,
            1,   0,   1,   0,   71,  0,   0,   0,  74,  0,  0,   0,   0,   0};
        wchar_t z[PATHN];
        join(z, root, L"deflate.zip");
        CHECK(writebytes(z, deflated, sizeof deflated));
        p = ls_open(z, dest2, &c, &err);
        CHECK(p && c.pilots == 1);
        ls_close(p);
        p = NULL;
    }
    /* Malicious ZIP names and duplicate normalized names never reach disk. */
    {
        wchar_t z[PATHN];
        ls_plan *evil = calloc(1, sizeof *evil);
        CHECK(evil);
        evil->v = calloc(MAXFILES, sizeof(item));
        CHECK(evil->v);
        CHECK(add(evil, L"user01.sav", data, sizeof data));
        join(z, root, L"unsafe.zip");
        wcscpy_s(evil->v[0].name, 240, L"..\\TDATA\\43430002\\user01.sav");
        CHECK(zipwrite(evil, z));
        p = ls_open(z, dst, &c, &err);
        CHECK(!p);
        DeleteFileW(z);
        wcscpy_s(evil->v[0].name, 240, L"TDATA\\12345678\\user01.sav");
        CHECK(zipwrite(evil, z));
        p = ls_open(z, dst, &c, &err);
        CHECK(!p);
        DeleteFileW(z);
        wcscpy_s(evil->v[0].name, 240, L"user01.sav");
        CHECK(add(evil, L"user01.sav", data, sizeof data));
        CHECK(zipwrite(evil, z));
        p = ls_open(z, dst, &c, &err);
        CHECK(!p);
        ls_close(evil);
    }
    /* CRC corruption must fail before creating a destination. */
    read = readbytes(zip, &n);
    CHECK(read && n > 60);
    read[60] ^= 1;
    DeleteFileW(zip);
    CHECK(writebytes(zip, read, n));
    free(read);
    p = ls_open(zip, dest2, &c, &err);
    CHECK(!p);
done:
    ls_close(p);
    remove_tree(root);
    return ok ? 0 : 1;
#undef CHECK
}
