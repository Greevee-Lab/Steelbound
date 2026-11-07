/* iso_vfs_check.c - W150 ISO-DIRECT: Pruefwerkzeug fuer iso_vfs.c.
 *
 *   iso_vfs_check --selftest              Selbsttest ohne Spieldaten
 *   iso_vfs_check --userdata-selftest     Selbsttest Datenorte (sb_userdata)
 *   iso_vfs_check <abbild> [--list]       Abbild oeffnen, Kennzahlen (+ Liste)
 *   iso_vfs_check <abbild> --compare <ordner>
 *        jede Datei des Abbilds byteweise gegen den entpackten Ordner
 *        (und umgekehrt: Ordner-Dateien, die im Abbild fehlen)
 *   iso_vfs_check <abbild> --version      Spielversion (sb_gamever)
 *   iso_vfs_check <abbild> --bench        alle Dateien einmal lesen (MB/s)
 *   iso_vfs_check <abbild> --bench-dir <ordner>  dieselben Dateien aus dem Ordner lesen (MB/s)
 * Exit 0 = ok. Liest nur, schreibt nichts ausser Temp-Datei im Selbsttest. */
#include "iso_vfs.h"
#include "sb_gamever.h"
#include "sb_userdata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static iso_vfs_t *g_v;
static unsigned g_files, g_diff, g_missing;
static unsigned long long g_bytes;
static const char *g_bench_dir; /* --bench-dir: gleiche Dateien aus dem Ordner lesen */

static void walk(int dir, const char *rel, const char *cmp_root, int list, int bench)
{
    unsigned k;
    int c;
    for (k = 0; (c = iso_vfs_child(g_v, dir, k)) >= 0; k++) {
        char r[1024];
        iso_vfs_stat_t st;
        iso_vfs_stat(g_v, c, &st);
        if (rel[0]) _snprintf_s(r, sizeof r, _TRUNCATE, "%s\\%s", rel, iso_vfs_name(g_v, c));
        else strcpy_s(r, sizeof r, iso_vfs_name(g_v, c));
        if (list) printf("%s%s  %u  attr=0x%02X\n", r, st.is_dir ? "\\" : "", st.size, st.attr);
        if (st.is_dir) {
            walk(c, r, cmp_root, list, bench);
            continue;
        }
        g_files++;
        g_bytes += st.size;
        if (g_bench_dir) { /* Ordner-Gegenstueck: gleiche Datei vom Host lesen */
            static uint8_t hb[1 << 20];
            char hp[1400];
            HANDLE fh;
            DWORD got;
            _snprintf_s(hp, sizeof hp, _TRUNCATE, "%s\\%s", g_bench_dir, r);
            fh = CreateFileA(hp, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            if (fh == INVALID_HANDLE_VALUE) { g_missing++; continue; }
            while (ReadFile(fh, hb, sizeof hb, &got, NULL) && got) {}
            CloseHandle(fh);
            continue;
        }
        if (cmp_root || bench) {
            static uint8_t a[1 << 20], b[1 << 20];
            FILE *f = NULL;
            uint64_t pos = 0;
            if (cmp_root) {
                char hp[1400];
                _snprintf_s(hp, sizeof hp, _TRUNCATE, "%s\\%s", cmp_root, r);
                if (fopen_s(&f, hp, "rb") != 0 || !f) {
                    printf("FEHLT im Ordner: %s\n", r);
                    g_missing++;
                    continue;
                }
            }
            for (;;) {
                uint32_t got = 0;
                size_t gb;
                if (iso_vfs_read(g_v, c, pos, a, sizeof a, &got) != 0) { printf("LESEFEHLER %s\n", r); g_diff++; break; }
                if (f) {
                    gb = fread(b, 1, sizeof b, f);
                    if (gb != got || memcmp(a, b, got) != 0) { printf("DIFF %s @%llu\n", r, (unsigned long long)pos); g_diff++; break; }
                }
                if (got == 0) break;
                pos += got;
            }
            if (f) fclose(f);
        }
    }
}

/* Ordner-Dateien, die im Abbild fehlen */
static void walk_host(const char *root, const char *rel)
{
    char pat[1400];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    _snprintf_s(pat, sizeof pat, _TRUNCATE, "%s\\%s%s*", root, rel, rel[0] ? "\\" : "");
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char r[1024];
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (rel[0]) _snprintf_s(r, sizeof r, _TRUNCATE, "%s\\%s", rel, fd.cFileName);
        else strcpy_s(r, sizeof r, fd.cFileName);
        if (iso_vfs_lookup(g_v, r, NULL) < 0) {
            printf("NUR im Ordner: %s\n", r);
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) walk_host(root, r);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

int main(int argc, char **argv)
{
    char err[512];
    int i, list = 0, bench = 0, ver = 0;
    const char *cmp = NULL;
    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0) {
        int rc = iso_vfs_selftest(err, sizeof err);
        printf("%s\n", err);
        return rc ? 1 : 0;
    }
    if (argc >= 4 && strcmp(argv[1], "--make-foreign") == 0) {
        /* Synthetisches Abbild einer "anderen Fassung": nur ein erfundener
         * XBE-Kopf (XBEH, Zertifikat mit Title-ID/Region), keine Spielbytes. */
        static uint8_t x[0x1000];
        uint32_t v;
        memset(x, 0, sizeof x);
        memcpy(x, "XBEH", 4);
        v = 0x10000; memcpy(x + 0x104, &v, 4);
        v = 0x1000; memcpy(x + 0x108, &v, 4);
        v = 0x1000; memcpy(x + 0x10C, &v, 4);
        v = 0x10200; memcpy(x + 0x118, &v, 4);
        v = (uint32_t)strtoul(argv[3], NULL, 16); memcpy(x + 0x200 + 0x08, &v, 4);
        v = argc >= 5 ? (uint32_t)strtoul(argv[4], NULL, 0) : 1u; memcpy(x + 0x200 + 0xA0, &v, 4);
        if (iso_vfs_make_test_image(argv[2], x, sizeof x) != 0) {
            printf("FEHLER: %s nicht schreibbar\n", argv[2]);
            return 1;
        }
        printf("Test-Abbild %s (Title-ID %s, Region %u)\n", argv[2], argv[3], v);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "--userdata-selftest") == 0) {
        int rc = sb_userdata_selftest(err, sizeof err);
        char p[600];
        printf("%s\n", err);
        printf("Modus jetzt: %s (Quelle %s)\n", sb_userdata_mode_name(sb_userdata_mode()), sb_userdata_mode_source());
        if (sb_userdata_path_peek(SB_UD_ROOT, p, sizeof p) == 1) printf("Wurzel: %s\n", p);
        return rc ? 1 : 0;
    }
    if (argc < 2) {
        printf("Aufruf: iso_vfs_check --selftest | <abbild> [--list] [--compare <ordner>] [--version] [--bench]\n");
        return 2;
    }
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--bench")) bench = 1;
        else if (!strcmp(argv[i], "--version")) ver = 1;
        else if (!strcmp(argv[i], "--compare") && i + 1 < argc) cmp = argv[++i];
        else if (!strcmp(argv[i], "--bench-dir") && i + 1 < argc) { g_bench_dir = argv[++i]; bench = 1; }
    }
    {
        LARGE_INTEGER f0, t0, t1;
        QueryPerformanceFrequency(&f0);
        QueryPerformanceCounter(&t0);
        g_v = iso_vfs_open(argv[1], err, sizeof err);
        QueryPerformanceCounter(&t1);
        if (!g_v) {
            printf("FEHLER: %s\n", err);
            return 1;
        }
        printf("Abbild: %s\nPartition @0x%llX, %u Eintraege, %u Verzeichnisse, Tabelle in %.1f ms\n",
               argv[1], (unsigned long long)iso_vfs_part_base(g_v), iso_vfs_count(g_v),
               iso_vfs_dir_count(g_v), (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f0.QuadPart);
    }
    if (ver) {
        uint8_t *x = NULL;
        uint32_t n = 0;
        sb_gamever_t gv;
        if (iso_vfs_read_all(g_v, "default.xbe", &x, &n) != 0) {
            printf("FEHLER: default.xbe fehlt im Abbild\n");
            return 1;
        }
        sb_gamever_check(x, n, &gv);
        printf("%s\n", gv.text);
        free(x);
        if (!gv.supported) return 3;
    }
    {
        LARGE_INTEGER f0, t0, t1;
        double s;
        QueryPerformanceFrequency(&f0);
        QueryPerformanceCounter(&t0);
        walk(0, "", cmp, list, bench);
        QueryPerformanceCounter(&t1);
        s = (double)(t1.QuadPart - t0.QuadPart) / (double)f0.QuadPart;
        printf("%u Dateien, %.1f MB", g_files, (double)g_bytes / 1048576.0);
        if (cmp || bench) printf(", gelesen in %.2f s (%.0f MB/s)", s, (double)g_bytes / 1048576.0 / (s > 0 ? s : 1));
        printf("\n");
    }
    if (cmp) {
        walk_host(cmp, "");
        printf("Vergleich: %u DIFF, %u fehlen im Ordner\n", g_diff, g_missing);
        iso_vfs_close(g_v);
        return (g_diff || g_missing) ? 1 : 0;
    }
    iso_vfs_close(g_v);
    return g_diff ? 1 : 0;
}
