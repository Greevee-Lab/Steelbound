/* launcher_packtest.c - W148 PACK-LAUNCHER: Selbsttest Packs-Seite.
 * Fixture-Pakete (Ordner) im Temp-Verzeichnis, keine Python-Abhaengigkeit:
 * packs.json lesen/schreiben (Reihenfolge, An/Aus, Stufe, Grenzen),
 * Scan (Meta + Status), Pruefen (gruen/gelb/rot wie tools/pack/tests),
 * Verdeckung, SHA-256-Vektoren, Render-Skalierung-Keys (Roundtrip).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sb_pack.h"
#include "sb_sha256.h"
#include "launcher_cfg.h"

#define P_OK(name) do { printf("PASS %s\n", name); fflush(stdout); \
    (*pass)++; } while (0)
#define P_FAIL(name, ...) do { printf("FAIL %s: ", name); \
    printf(__VA_ARGS__); printf("\n"); fflush(stdout); (*fail)++; } while (0)

static char pk_tmp[1024];

static void pk_rmdir(const char *dir)
{
    char pat[1200];
    WIN32_FIND_DATAA fd;
    HANDLE fh;
    _snprintf_s(pat, sizeof pat, _TRUNCATE, "%s/*", dir);
    fh = FindFirstFileA(pat, &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            char fp[1200];
            if (strcmp(fd.cFileName, ".") == 0 ||
                strcmp(fd.cFileName, "..") == 0) {
                continue;
            }
            _snprintf_s(fp, sizeof fp, _TRUNCATE, "%s/%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                pk_rmdir(fp);
            } else {
                DeleteFileA(fp);
            }
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
    RemoveDirectoryA(dir);
}

static int pk_setup(void)
{
    char tmp[900];
    DWORD n = GetTempPathA((DWORD)sizeof tmp, tmp);
    if (!n || n >= sizeof tmp) {
        return 0;
    }
    _snprintf_s(pk_tmp, sizeof pk_tmp, _TRUNCATE, "%ssb_w148_pk%lu",
                tmp, GetCurrentProcessId());
    pk_rmdir(pk_tmp);
    CreateDirectoryA(pk_tmp, NULL);
    CreateDirectoryA(pk_tmp, NULL);
    return 1;
}

/* Minimales gueltiges DDS (BC1/BC3/DXT5, mip 0, keine Mips). */
static int pk_dds(const char *path, int w, int h, const char *fcc)
{
    unsigned char hd[128];
    unsigned char px[64];
    FILE *f = NULL;
    int i, nb;
    memset(hd, 0, sizeof hd);
    memcpy(hd, "DDS ", 4);
    hd[4] = 124;
    hd[8] = 0x07;
    hd[9] = 0x10;
    hd[12] = (unsigned char)h;
    hd[13] = (unsigned char)(h >> 8);
    hd[16] = (unsigned char)w;
    hd[17] = (unsigned char)(w >> 8);
    hd[20] = (unsigned char)((w / 4) * 8);
    hd[76] = 32;
    hd[80] = 4;
    memcpy(hd + 84, fcc, 4);
    hd[108] = 0x10;
    if (fopen_s(&f, path, "wb") != 0 || !f) {
        return 0;
    }
    fwrite(hd, 1, 128, f);
    nb = (w / 4) * (h / 4) * (memcmp(fcc, "DXT1", 4) == 0 ? 8 : 16);
    for (i = 0; i < nb && i < 64; i++) {
        px[i] = (unsigned char)(i * 7 + 1);
    }
    fwrite(px, 1, nb, f);
    fclose(f);
    return 1;
}

static int pk_sha_of(const char *path, char hex[65])
{
    uint8_t dg[32];
    if (!sb_sha256_file(path, dg)) {
        return 0;
    }
    sb_sha256_hex(dg, hex);
    return 1;
}

static int pk_write(const char *path, const char *text)
{
    FILE *f = NULL;
    if (fopen_s(&f, path, "wb") != 0 || !f) {
        return 0;
    }
    fputs(text, f);
    fclose(f);
    return 1;
}

/* Ein-Eintrags-Manifest schreiben (Variante x2 aus fpath, Rest frei). */
static int pk_manifest(const char *dir, const char *id, const char *hash,
                       const char *vf, int mw, int mh, const char *mfmt,
                       int mmips, const char *msha, int ow, int oh,
                       const char *cat, const char *xbe, const char *extra)
{
    char path[1200], t[4096];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s/manifest.json", dir);
    _snprintf_s(t, sizeof t, _TRUNCATE,
        "{ \"format\": \"sbpack\", \"format_version\": 1,\n"
        "  \"id\": \"%s\", \"name\": \"T %s\", \"version\": \"1.2.3\",\n"
        "  \"author\": \"A\", \"license\": \"CC0\", \"type\": \"texture\",\n"
        "  \"hash_algo\": \"sbt1-fnv64\", \"game\": { \"xbe_sha256\": [\"%s\"] },\n"
        "  \"entries\": [ { \"hash\": \"%s\",%s \"category\": \"%s\",\n"
        "    \"orig\": { \"w\": %d, \"h\": %d },\n"
        "    \"variants\": { \"x2\": { \"file\": \"%s\", \"w\": %d, \"h\": %d,\n"
        "      \"fmt\": \"%s\", \"mips\": %d, \"sha256\": \"%s\" } } } ] }%s",
        id, id, xbe, hash, extra ? extra : "", cat, ow, oh, vf, mw, mh,
        mfmt, mmips, msha, "");
    return pk_write(path, t);
}

/* Gruenes Standard-Paket anlegen. Rueckgabe: 1 = ok. */
static int pk_green(const char *name, const char *id, const char *hash,
                    const char *xbe)
{
    char dir[1100], fp[1200], sha[65];
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s/%s", pk_tmp, name);
    CreateDirectoryA(dir, NULL);
    _snprintf_s(fp, sizeof fp, _TRUNCATE, "%s/t.dds", dir);
    if (!pk_dds(fp, 4, 4, "DXT1")) {
        return 0;
    }
    if (!pk_sha_of(fp, sha)) {
        return 0;
    }
    return pk_manifest(dir, id, hash, "t.dds", 4, 4, "BC1", 1, sha, 2, 2,
                       "menu", xbe, NULL);
}

/* Effect-Manifest schreiben (type effect, entries frei). */
static int pk_fxmanifest(const char *dir, const char *id, const char *xbe,
                         const char *entries_json)
{
    char path[1200], m[4096];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s/manifest.json", dir);
    _snprintf_s(m, sizeof m, _TRUNCATE,
        "{ \"format\": \"sbpack\", \"format_version\": 1,\n"
        "  \"id\": \"%s\", \"name\": \"FX %s\", \"version\": \"1.0.0\",\n"
        "  \"author\": \"A\", \"license\": \"CC0\", \"type\": \"effect\",\n"
        "  \"hash_algo\": \"sbt1-fnv64\", \"game\": { \"xbe_sha256\": [\"%s\"] },\n"
        "  \"entries\": %s }",
        id, id, xbe, entries_json ? entries_json : "[]");
    return pk_write(path, m);
}

static void t_sha256(int *pass, int *fail)
{
    static const char *const in[3] = { "", "abc",
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq" };
    static const char *const want[3] = {
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" };
    int i, ok = 1;
    for (i = 0; i < 3; i++) {
        uint8_t dg[32];
        char hex[65];
        sb_sha256(in[i], strlen(in[i]), dg);
        sb_sha256_hex(dg, hex);
        if (strcmp(hex, want[i]) != 0) {
            ok = 0;
        }
    }
    if (ok) {
        P_OK("30 sha256");
    } else {
        P_FAIL("30 sha256", "%s", "Vektor falsch");
    }
}

static void t_cfg(int *pass, int *fail)
{
    char pj[1100];
    sb_pack_order_t o[8];
    sb_pack_cap_t c[8];
    sb_pack_over_t v[8];
    char level[8];
    int n, ok = 1;
    _snprintf_s(pj, sizeof pj, _TRUNCATE, "%s/packs.json", pk_tmp);
    if (!pk_write(pj,
        "{ \"version\": 1,\n"
        "  \"order\": [\n"
        "    { \"id\": \"t.a\", \"file\": \"a\", \"enabled\": true },\n"
        "    { \"id\": \"t.b\", \"file\": \"b.sbpack\", \"enabled\": false },\n"
        "    { \"id\": \"t.c\", \"file\": \"c\" } ],\n"
        "  \"level\": \"x2\",\n"
        "  \"category_level\": { \"menu\": \"orig\" },\n"
        "  \"overrides\": { \"0011223344556677\": \"x2\" } }\n")) {
        ok = 0;
    }
    memset(o, 0, sizeof o);
    memset(c, 0, sizeof c);
    memset(v, 0, sizeof v);
    n = sb_pack_read_cfg(pj, o, 8, level, sizeof level, c, 8, v, 8);
    if (n != 3 || strcmp(o[0].id, "t.a") != 0 || o[1].enabled != 0 ||
        o[2].enabled != 1 || strcmp(level, "x2") != 0 ||
        strcmp(c[0].name, "menu") != 0 || c[0].cap != 0 ||
        strcmp(v[0].hash, "0011223344556677") != 0 || v[0].cap != 2) {
        ok = 0;
    }
    /* Reihenfolge tauschen, An/Aus kippen, Stufe setzen. */
    if (ok) {
        sb_pack_order_t t = o[0];
        o[0] = o[2];
        o[2] = t;
        o[1].enabled = 1;
        strcpy_s(level, sizeof level, "x4");
        c[0].cap = 2;
        if (!sb_pack_write_cfg(pj, o, 3, level, c, 1, v, 1)) {
            ok = 0;
        }
    }
    if (ok) {
        memset(o, 0, sizeof o);
        memset(c, 0, sizeof c);
        memset(v, 0, sizeof v);
        n = sb_pack_read_cfg(pj, o, 8, level, sizeof level, c, 8, v, 8);
        if (n != 3 || strcmp(o[0].id, "t.c") != 0 || o[1].enabled != 1 ||
            strcmp(level, "x4") != 0 || c[0].cap != 2 ||
            strcmp(v[0].hash, "0011223344556677") != 0) {
            ok = 0;
        }
    }
    if (ok) {
        P_OK("31 packs-cfg");
    } else {
        P_FAIL("31 packs-cfg", "%s", "lesen/schreiben falsch");
    }
}

static void t_scan(int *pass, int *fail)
{
    char pj[1100], xbe[65];
    sb_pack_scan_t s[4];
    int n, ok = 1;
    memset(xbe, 'a', 64);
    xbe[64] = '\0';
    if (!pk_green("ga", "t.ga", "0011223344556677", xbe)) {
        ok = 0;
    }
    _snprintf_s(pj, sizeof pj, _TRUNCATE, "%s/scan.json", pk_tmp);
    if (!pk_write(pj,
        "{ \"version\": 1, \"order\": [\n"
        "  { \"id\": \"t.ga\", \"file\": \"ga\" },\n"
        "  { \"id\": \"t.miss\", \"file\": \"fehlt\", \"enabled\": false } ] }")) {
        ok = 0;
    }
    memset(s, 0, sizeof s);
    n = sb_pack_scan(pj, s, 4);
    if (n != 2 || strcmp(s[0].id, "t.ga") != 0 ||
        strcmp(s[0].version, "1.2.3") != 0 ||
        strcmp(s[0].author, "A") != 0 ||
        strcmp(s[0].license, "CC0") != 0 ||
        strcmp(s[0].type, "texture") != 0 || !s[0].enabled ||
        s[0].is_zip != 0 || s[0].nentries != 1 || s[0].status != 2) {
        ok = 0;
    }
    if (s[1].enabled != 0 || s[1].is_zip != -1 || s[1].status != 0) {
        ok = 0;
    }
    if (ok) {
        P_OK("32 packs-scan");
    } else {
        P_FAIL("32 packs-scan", "%s", "Meta/Status falsch");
    }
}

/* Minimales stored-ZIP schreiben (kein CRC, kein Kommentar). */
static int pk_zip(const char *zpath, const char files[][2][1100], int n)
{
    FILE *f = NULL;
    long offs[16];
    long cd_off;
    unsigned cd_size = 0;
    int i;
    unsigned char eocd[22];
    if (n > 16 || fopen_s(&f, zpath, "wb") != 0 || !f) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        FILE *g = NULL;
        long start = ftell(f);
        unsigned char lh[30];
        size_t fnlen = strlen(files[i][0]);
        size_t size = 0;
        unsigned char buf[65536];
        size_t r;
        offs[i] = start;
        memset(lh, 0, sizeof lh);
        lh[0] = 'P';
        lh[1] = 'K';
        lh[2] = 3;
        lh[3] = 4;
        lh[4] = 20;
        lh[26] = (unsigned char)fnlen;
        if (fopen_s(&g, files[i][1], "rb") == 0 && g) {
            fseek(g, 0, SEEK_END);
            size = (size_t)ftell(g);
            fseek(g, 0, SEEK_SET);
        }
        lh[18] = (unsigned char)size;
        lh[19] = (unsigned char)(size >> 8);
        lh[20] = (unsigned char)(size >> 16);
        lh[21] = (unsigned char)(size >> 24);
        lh[22] = lh[18];
        lh[23] = lh[19];
        lh[24] = lh[20];
        lh[25] = lh[21];
        fwrite(lh, 1, 30, f);
        fwrite(files[i][0], 1, fnlen, f);
        if (g) {
            while ((r = fread(buf, 1, sizeof buf, g)) > 0) {
                fwrite(buf, 1, r, f);
            }
            fclose(g);
        }
    }
    cd_off = ftell(f);
    for (i = 0; i < n; i++) {
        FILE *g = NULL;
        unsigned char ch[46];
        size_t fnlen = strlen(files[i][0]);
        size_t size = 0;
        memset(ch, 0, sizeof ch);
        ch[0] = 'P';
        ch[1] = 'K';
        ch[2] = 1;
        ch[3] = 2;
        ch[4] = 20;
        ch[6] = 20;
        if (fopen_s(&g, files[i][1], "rb") == 0 && g) {
            fseek(g, 0, SEEK_END);
            size = (size_t)ftell(g);
            fclose(g);
        }
        ch[20] = (unsigned char)size;
        ch[21] = (unsigned char)(size >> 8);
        ch[22] = (unsigned char)(size >> 16);
        ch[23] = (unsigned char)(size >> 24);
        ch[24] = ch[20];
        ch[25] = ch[21];
        ch[26] = ch[22];
        ch[27] = ch[23];
        ch[28] = (unsigned char)fnlen;
        ch[42] = (unsigned char)offs[i];
        ch[43] = (unsigned char)(offs[i] >> 8);
        ch[44] = (unsigned char)(offs[i] >> 16);
        ch[45] = (unsigned char)(offs[i] >> 24);
        fwrite(ch, 1, 46, f);
        fwrite(files[i][0], 1, fnlen, f);
        cd_size += 46 + (unsigned)fnlen;
    }
    memset(eocd, 0, sizeof eocd);
    eocd[0] = 'P';
    eocd[1] = 'K';
    eocd[2] = 5;
    eocd[3] = 6;
    eocd[8] = (unsigned char)n;
    eocd[10] = (unsigned char)n;
    eocd[12] = (unsigned char)cd_size;
    eocd[13] = (unsigned char)(cd_size >> 8);
    eocd[14] = (unsigned char)(cd_size >> 16);
    eocd[15] = (unsigned char)(cd_size >> 24);
    eocd[16] = (unsigned char)cd_off;
    eocd[17] = (unsigned char)(cd_off >> 8);
    eocd[18] = (unsigned char)(cd_off >> 16);
    eocd[19] = (unsigned char)(cd_off >> 24);
    fwrite(eocd, 1, 22, f);
    fclose(f);
    return 1;
}

static void t_check_green(int *pass, int *fail)
{
    char dir[1100], xbe[65], reason[64], detail[300];
    int st, ok = 1;
    memset(xbe, 'b', 64);
    xbe[64] = '\0';
    if (!pk_green("gb", "t.gb", "8899aabbccddeeff", xbe)) {
        ok = 0;
    }
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s/gb", pk_tmp);
    st = sb_pack_check(dir, xbe, reason, sizeof reason, detail,
                       sizeof detail);
    if (st != 2 || reason[0] != '\0') {
        ok = 0;
    }
    /* Gleiches Paket als .sbpack (ZIP-Pfad: stored lesen). */
    if (ok) {
        char zpath[1100], mf[1200], df[1200];
        char files[2][2][1100];
        _snprintf_s(zpath, sizeof zpath, _TRUNCATE, "%s/gb.sbpack", pk_tmp);
        _snprintf_s(mf, sizeof mf, _TRUNCATE, "%s/manifest.json", dir);
        _snprintf_s(df, sizeof df, _TRUNCATE, "%s/t.dds", dir);
        strcpy_s(files[0][0], 1100, "manifest.json");
        strcpy_s(files[0][1], 1100, mf);
        strcpy_s(files[1][0], 1100, "t.dds");
        strcpy_s(files[1][1], 1100, df);
        if (!pk_zip(zpath, files, 2)) {
            ok = 0;
        } else {
            st = sb_pack_check(zpath, xbe, reason, sizeof reason, detail,
                               sizeof detail);
            if (st != 2 || reason[0] != '\0') {
                ok = 0;
            }
        }
    }
    if (ok) {
        P_OK("33 packs-check-gruen");
    } else {
        P_FAIL("33 packs-check-gruen", "%s %s", reason, detail);
    }
}

/* Rot-Faelle wie tools/pack/tests (C-Gegenstuecke). */
static void t_check_red(int *pass, int *fail)
{
    char dir[1100], evil[1200], fp[1200], sha[65], xbe[65];
    char reason[64], detail[300];
    int st[5], ok = 1;
    memset(xbe, 'c', 64);
    xbe[64] = '\0';
    /* 1) deny_suffix: Fremddatei im Paket. */
    pk_green("r1", "t.r1", "1111111111111111", xbe);
    _snprintf_s(evil, sizeof evil, _TRUNCATE, "%s/r1/evil.exe", pk_tmp);
    pk_write(evil, "MZ");
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s/r1", pk_tmp);
    st[0] = sb_pack_check(dir, NULL, reason, sizeof reason, detail,
                          sizeof detail);
    if (st[0] != 0 || strcmp(reason, "deny_suffix") != 0) {
        ok = 0;
    }
    /* 2) bad_variant_path: ".." in der Variante. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s/r2", pk_tmp);
    CreateDirectoryA(dir, NULL);
    _snprintf_s(fp, sizeof fp, _TRUNCATE, "%s/t.dds", dir);
    pk_dds(fp, 4, 4, "DXT1");
    pk_sha_of(fp, sha);
    pk_manifest(dir, "t.r2", "2222222222222222", "../x.dds", 4, 4, "BC1", 1,
                sha, 2, 2, "menu", xbe, NULL);
    st[1] = sb_pack_check(dir, NULL, reason, sizeof reason, detail,
                          sizeof detail);
    if (st[1] != 0 || strcmp(reason, "bad_variant_path") != 0) {
        ok = 0;
    }
    /* 3) aspect_mismatch: DDS 8x4 bei Orig 2x2. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s/r3", pk_tmp);
    CreateDirectoryA(dir, NULL);
    _snprintf_s(fp, sizeof fp, _TRUNCATE, "%s/t.dds", dir);
    pk_dds(fp, 8, 4, "DXT1");
    pk_sha_of(fp, sha);
    pk_manifest(dir, "t.r3", "3333333333333333", "t.dds", 8, 4, "BC1", 1,
                sha, 2, 2, "menu", xbe, NULL);
    st[2] = sb_pack_check(dir, NULL, reason, sizeof reason, detail,
                          sizeof detail);
    if (st[2] != 0 || strcmp(reason, "aspect_mismatch") != 0) {
        ok = 0;
    }
    /* 4) game_mismatch: fremder XBE-Hash. */
    pk_green("r4", "t.r4", "4444444444444444",
             "0000000000000000000000000000000000000000000000000000000000000000");
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s/r4", pk_tmp);
    st[3] = sb_pack_check(dir,
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        reason, sizeof reason, detail, sizeof detail);
    if (st[3] != 0 || strcmp(reason, "game_mismatch") != 0) {
        ok = 0;
    }
    /* 5) dup_hash: zwei Eintraege, ein Hash. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s/r5", pk_tmp);
    CreateDirectoryA(dir, NULL);
    _snprintf_s(fp, sizeof fp, _TRUNCATE, "%s/t.dds", dir);
    pk_dds(fp, 4, 4, "DXT1");
    pk_sha_of(fp, sha);
    {
        char mp[1200], t[4096];
        _snprintf_s(mp, sizeof mp, _TRUNCATE, "%s/manifest.json", dir);
        _snprintf_s(t, sizeof t, _TRUNCATE,
            "{ \"format\": \"sbpack\", \"format_version\": 1,\n"
            "  \"id\": \"t.r5\", \"name\": \"T\", \"version\": \"1.0.0\",\n"
            "  \"author\": \"A\", \"license\": \"CC0\", \"type\": \"texture\",\n"
            "  \"hash_algo\": \"sbt1-fnv64\",\n"
            "  \"game\": { \"xbe_sha256\": [\"%s\"] },\n"
            "  \"entries\": [\n"
            "    { \"hash\": \"5555555555555555\",\n"
            "      \"variants\": { \"x2\": { \"file\": \"t.dds\", \"w\": 4,\n"
            "        \"h\": 4, \"fmt\": \"BC1\", \"mips\": 1,\n"
            "        \"sha256\": \"%s\" } } },\n"
            "    { \"hash\": \"5555555555555555\",\n"
            "      \"variants\": { \"x2\": { \"file\": \"t.dds\", \"w\": 4,\n"
            "        \"h\": 4, \"fmt\": \"BC1\", \"mips\": 1,\n"
            "        \"sha256\": \"%s\" } } } ] }", xbe, sha, sha);
        pk_write(mp, t);
    }
    st[4] = sb_pack_check(dir, NULL, reason, sizeof reason, detail,
                          sizeof detail);
    if (st[4] != 0 || strcmp(reason, "dup_hash") != 0) {
        ok = 0;
    }
    if (ok) {
        P_OK("34 packs-check-rot");
    } else {
        P_FAIL("34 packs-check-rot", "%d%d%d%d%d %s", st[0], st[1], st[2],
               st[3], st[4], reason);
    }
}

/* Rot-Faelle Schema (id/version). */
static void t_check_red2(int *pass, int *fail)
{
    char dir[1100], fp[1200], sha[65], xbe[65], mp[1200], t[4096];
    char reason[64], detail[300];
    int st, ok = 1;
    memset(xbe, 'f', 64);
    xbe[64] = '\0';
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s/r6", pk_tmp);
    CreateDirectoryA(dir, NULL);
    _snprintf_s(fp, sizeof fp, _TRUNCATE, "%s/t.dds", dir);
    pk_dds(fp, 4, 4, "DXT1");
    pk_sha_of(fp, sha);
    _snprintf_s(mp, sizeof mp, _TRUNCATE, "%s/manifest.json", dir);
    _snprintf_s(t, sizeof t, _TRUNCATE,
        "{ \"format\": \"sbpack\", \"format_version\": 1,\n"
        "  \"id\": \"NoDot\", \"name\": \"T\", \"version\": \"1.0\",\n"
        "  \"author\": \"A\", \"license\": \"CC0\", \"type\": \"texture\",\n"
        "  \"hash_algo\": \"sbt1-fnv64\",\n"
        "  \"game\": { \"xbe_sha256\": [\"%s\"] },\n"
        "  \"entries\": [ { \"hash\": \"7777777777777777\",\n"
        "    \"variants\": { \"x2\": { \"file\": \"t.dds\", \"w\": 4, \"h\": 4,\n"
        "      \"fmt\": \"BC1\", \"mips\": 1, \"sha256\": \"%s\" } } } ] }",
        xbe, sha);
    pk_write(mp, t);
    st = sb_pack_check(dir, NULL, reason, sizeof reason, detail,
                       sizeof detail);
    /* Erste rote Regel gewinnt (bad_id vor bad_version). */
    if (st != 0 || strcmp(reason, "bad_id") != 0) {
        ok = 0;
    }
    if (ok) {
        P_OK("34b packs-check-schema");
    } else {
        P_FAIL("34b packs-check-schema", "%d %s", st, reason);
    }
}

static void t_check_yellow(int *pass, int *fail)
{
    char dir[1100], fp[1200], sha[65], xbe[65];
    char reason[64], detail[300];
    int st, ok = 1;
    memset(xbe, 'd', 64);
    xbe[64] = '\0';
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s/ye", pk_tmp);
    CreateDirectoryA(dir, NULL);
    _snprintf_s(fp, sizeof fp, _TRUNCATE, "%s/t.dds", dir);
    pk_dds(fp, 4, 4, "DXT1");
    pk_sha_of(fp, sha);
    pk_manifest(dir, "t.ye", "6666666666666666", "t.dds", 4, 4, "BC1", 1,
                sha, 2, 2, "phantasia", xbe, NULL);
    st = sb_pack_check(dir, NULL, reason, sizeof reason, detail,
                       sizeof detail);
    if (st != 1 || strcmp(reason, "unknown_category") != 0) {
        ok = 0;
    }
    if (ok) {
        P_OK("35 packs-check-gelb");
    } else {
        P_FAIL("35 packs-check-gelb", "%d %s", st, reason);
    }
}

static void t_shadow(int *pass, int *fail)
{
    char da[1100], db[1100], fa[1200], fb1[1200], fb2[1200], pj[1100];
    char sha[65], xbe[65], ma[4096], mb[4096], mpa[1200], mpb[1200];
    int ne[4], sh[4], n, ok = 1;
    memset(xbe, 'e', 64);
    xbe[64] = '\0';
    _snprintf_s(da, sizeof da, _TRUNCATE, "%s/sa", pk_tmp);
    _snprintf_s(db, sizeof db, _TRUNCATE, "%s/sb", pk_tmp);
    CreateDirectoryA(da, NULL);
    CreateDirectoryA(db, NULL);
    _snprintf_s(fa, sizeof fa, _TRUNCATE, "%s/a.dds", da);
    _snprintf_s(fb1, sizeof fb1, _TRUNCATE, "%s/b1.dds", db);
    _snprintf_s(fb2, sizeof fb2, _TRUNCATE, "%s/b2.dds", db);
    pk_dds(fa, 4, 4, "DXT1");
    pk_dds(fb1, 4, 4, "DXT1");
    pk_dds(fb2, 4, 4, "DXT1");
    pk_sha_of(fa, sha);
    _snprintf_s(mpa, sizeof mpa, _TRUNCATE, "%s/manifest.json", da);
    _snprintf_s(ma, sizeof ma, _TRUNCATE,
        "{ \"format\": \"sbpack\", \"format_version\": 1,\n"
        "  \"id\": \"t.sa\", \"name\": \"A\", \"version\": \"1.0.0\",\n"
        "  \"author\": \"A\", \"license\": \"CC0\", \"type\": \"texture\",\n"
        "  \"hash_algo\": \"sbt1-fnv64\",\n"
        "  \"game\": { \"xbe_sha256\": [\"%s\"] },\n"
        "  \"entries\": [ { \"hash\": \"aaaaaaaaaaaaaaaa\",\n"
        "    \"variants\": { \"x2\": { \"file\": \"a.dds\", \"w\": 4, \"h\": 4,\n"
        "      \"fmt\": \"BC1\", \"mips\": 1, \"sha256\": \"%s\" } } } ] }",
        xbe, sha);
    pk_write(mpa, ma);
    pk_sha_of(fb1, sha);
    _snprintf_s(mpb, sizeof mpb, _TRUNCATE, "%s/manifest.json", db);
    _snprintf_s(mb, sizeof mb, _TRUNCATE,
        "{ \"format\": \"sbpack\", \"format_version\": 1,\n"
        "  \"id\": \"t.sb\", \"name\": \"B\", \"version\": \"1.0.0\",\n"
        "  \"author\": \"A\", \"license\": \"CC0\", \"type\": \"texture\",\n"
        "  \"hash_algo\": \"sbt1-fnv64\",\n"
        "  \"game\": { \"xbe_sha256\": [\"%s\"] },\n"
        "  \"entries\": [\n"
        "    { \"hash\": \"aaaaaaaaaaaaaaaa\",\n"
        "      \"variants\": { \"x2\": { \"file\": \"b1.dds\", \"w\": 4,\n"
        "        \"h\": 4, \"fmt\": \"BC1\", \"mips\": 1,\n"
        "        \"sha256\": \"%s\" } } },\n"
        "    { \"hash\": \"bbbbbbbbbbbbbbbb\",\n"
        "      \"variants\": { \"x2\": { \"file\": \"b2.dds\", \"w\": 4,\n"
        "        \"h\": 4, \"fmt\": \"BC1\", \"mips\": 1,\n"
        "        \"sha256\": \"%s\" } } } ] }", xbe, sha, sha);
    pk_write(mpb, mb);
    _snprintf_s(pj, sizeof pj, _TRUNCATE, "%s/sh.json", pk_tmp);
    pk_write(pj,
        "{ \"version\": 1, \"order\": [\n"
        "  { \"id\": \"t.sa\", \"file\": \"sa\" },\n"
        "  { \"id\": \"t.sb\", \"file\": \"sb\" } ] }");
    n = sb_pack_shadow(pj, ne, sh, 4);
    /* A gewinnt "aa" (zuerst), B behaelt "bb": A 1/0, B 2/1. */
    if (n != 2 || ne[0] != 1 || sh[0] != 0 || ne[1] != 2 || sh[1] != 1) {
        ok = 0;
    }
    if (ok) {
        P_OK("36 packs-shadow");
    } else {
        P_FAIL("36 packs-shadow", "%d %d/%d %d/%d", n, ne[0], sh[0], ne[1],
               sh[1]);
    }
}

static void t_scan_fx(int *pass, int *fail)
{
    char dir[1100], dir2[1100], pj[1100], pp[1200], xbe[65];
    sb_pack_scan_t s[2];
    int out[SB_FXN], hit[SB_FXN], n, ok = 1;
    static char src[SB_FXN][128];
    memset(xbe, 'f', 64);
    xbe[64] = '\0';
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s/fx1", pk_tmp);
    CreateDirectoryA(dir, NULL);
    if (!pk_fxmanifest(dir, "t.fx1", xbe, "[]")) {
        ok = 0;
    }
    _snprintf_s(pp, sizeof pp, _TRUNCATE, "%s/params.json", dir);
    if (!pk_write(pp, "{ \"params\": { \"fx_soft\": 60, \"color_sat\": 110, \"nope\": 1 } }")) {
        ok = 0;
    }
    /* Zweites, tieferes Paket: ueberschneidender Key + klemmender Wert. */
    _snprintf_s(dir2, sizeof dir2, _TRUNCATE, "%s/fx2", pk_tmp);
    CreateDirectoryA(dir2, NULL);
    pk_fxmanifest(dir2, "t.fx2", xbe, "[]");
    _snprintf_s(pp, sizeof pp, _TRUNCATE, "%s/params.json", dir2);
    pk_write(pp, "{ \"params\": { \"fx_soft\": 10, \"color_temp\": 999 } }");
    _snprintf_s(pj, sizeof pj, _TRUNCATE, "%s/fxscan.json", pk_tmp);
    if (!pk_write(pj, "{ \"version\": 1, \"order\": [\n  { \"id\": \"t.fx1\", \"file\": \"fx1\" },\n  { \"id\": \"t.fx2\", \"file\": \"fx2\" } ] }")) {
        ok = 0;
    }
    memset(s, 0, sizeof s);
    n = sb_pack_scan(pj, s, 2);
    if (n != 2 || strcmp(s[0].type, "effect") != 0 || s[0].status != 2 ||
        s[0].nparams != 2 || !strstr(s[0].paramsum, "fx_soft=60") ||
        !strstr(s[0].paramsum, "color_sat=110") || strstr(s[0].paramsum, "nope")) {
        ok = 0;
    }
    /* Aufloesung: je Parameter hoechstes Paket, unbekannt ignoriert,
     * ausserhalb geklemmt (999 -> 100). Keys 0/6/7 = soft/sat/temp. */
    memset(out, 0, sizeof out);
    memset(hit, 0, sizeof hit);
    memset(src, 0, sizeof src);
    if (!sb_pack_fxparams(pj, out, hit, src)) {
        ok = 0;
    }
    if (strcmp(kSbFxParams[0].key, "fx_soft") != 0 ||
        strcmp(kSbFxParams[6].key, "color_sat") != 0 ||
        strcmp(kSbFxParams[7].key, "color_temp") != 0 ||
        !hit[0] || out[0] != 60 || strcmp(src[0], "t.fx1") != 0 ||
        !hit[6] || out[6] != 110 || !hit[7] || out[7] != 100 ||
        strcmp(src[7], "t.fx2") != 0 || hit[1] || hit[2] || hit[3] ||
        hit[4] || hit[5] || hit[8] || hit[9] || hit[10]) {
        ok = 0;
    }
    if (ok) {
        P_OK("38 packs-scan-fx");
    } else {
        P_FAIL("38 packs-scan-fx", "n=%d type=%s status=%d nparams=%d sum=%s",
               n, s[0].type, s[0].status, s[0].nparams, s[0].paramsum);
    }
}

static void t_check_fx(int *pass, int *fail)
{
    char dg[1100], dy[1100], dr[1100], dt[1100], pg[1200], py[1200], pr[1200];
    char fp[1200], sha[65], xbe[65], reason[64], detail[300];
    int st, ok = 1;
    memset(xbe, 'a', 64);
    xbe[64] = '\0';
    /* gruen: nur bekannte Keys im Bereich */
    _snprintf_s(dg, sizeof dg, _TRUNCATE, "%s/fxg", pk_tmp);
    CreateDirectoryA(dg, NULL);
    pk_fxmanifest(dg, "t.fxg", xbe, "[]");
    _snprintf_s(pg, sizeof pg, _TRUNCATE, "%s/params.json", dg);
    pk_write(pg, "{ \"params\": { \"fx_soft\": 60, \"fx_interp\": 1 } }");
    st = sb_pack_check(dg, NULL, reason, sizeof reason, detail, sizeof detail);
    if (st != 2) {
        ok = 0;
    }
    /* gelb: unbekannter Key (V13) */
    _snprintf_s(dy, sizeof dy, _TRUNCATE, "%s/fxy", pk_tmp);
    CreateDirectoryA(dy, NULL);
    pk_fxmanifest(dy, "t.fxy", xbe, "[]");
    _snprintf_s(py, sizeof py, _TRUNCATE, "%s/params.json", dy);
    pk_write(py, "{ \"params\": { \"fx_soft\": 60, \"mega_bloom\": 1 } }");
    st = sb_pack_check(dy, NULL, reason, sizeof reason, detail, sizeof detail);
    if (st != 1 || strcmp(reason, "unknown_param") != 0) {
        ok = 0;
    }
    /* gelb: Wert ausserhalb (V14, wird geklemmt) */
    pk_write(py, "{ \"params\": { \"fx_soft\": 999 } }");
    st = sb_pack_check(dy, NULL, reason, sizeof reason, detail, sizeof detail);
    if (st != 1 || strcmp(reason, "param_range") != 0) {
        ok = 0;
    }
    /* rot: params.json kein JSON */
    _snprintf_s(dr, sizeof dr, _TRUNCATE, "%s/fxr", pk_tmp);
    CreateDirectoryA(dr, NULL);
    pk_fxmanifest(dr, "t.fxr", xbe, "[]");
    _snprintf_s(pr, sizeof pr, _TRUNCATE, "%s/params.json", dr);
    pk_write(pr, "{ kein json");
    st = sb_pack_check(dr, NULL, reason, sizeof reason, detail, sizeof detail);
    if (st != 0 || strcmp(reason, "params_not_json") != 0) {
        ok = 0;
    }
    /* gruen: Effekt-Textur (Kategorie effect, kein V12) */
    _snprintf_s(dt, sizeof dt, _TRUNCATE, "%s/fxt", pk_tmp);
    CreateDirectoryA(dt, NULL);
    _snprintf_s(fp, sizeof fp, _TRUNCATE, "%s/e.dds", dt);
    pk_dds(fp, 4, 4, "DXT1");
    pk_sha_of(fp, sha);
    {
        char ent[1024];
        _snprintf_s(ent, sizeof ent, _TRUNCATE,
            "[ { \"hash\": \"7777777777777777\", \"category\": \"effect\",\n"
            "    \"orig\": { \"w\": 2, \"h\": 2 },\n"
            "    \"variants\": { \"x2\": { \"file\": \"e.dds\", \"w\": 4, \"h\": 4,\n"
            "      \"fmt\": \"BC1\", \"mips\": 1, \"sha256\": \"%s\" } } } ]",
            sha);
        pk_fxmanifest(dt, "t.fxt", xbe, ent);
    }
    st = sb_pack_check(dt, NULL, reason, sizeof reason, detail, sizeof detail);
    if (st != 2) {
        ok = 0;
    }
    /* gelb: params.json im Textur-Paket (wird ignoriert, wohl type falsch) */
    if (!pk_green("fxt2", "t.fxt2", "8888888888888888", xbe)) {
        ok = 0;
    } else {
        char pj2[1200];
        _snprintf_s(pj2, sizeof pj2, _TRUNCATE, "%s/fxt2/params.json", pk_tmp);
        pk_write(pj2, "{ \"params\": { \"fx_soft\": 60 } }");
        _snprintf_s(dt, sizeof dt, _TRUNCATE, "%s/fxt2", pk_tmp);
        st = sb_pack_check(dt, NULL, reason, sizeof reason, detail,
                           sizeof detail);
        if (st != 1 || strcmp(reason, "params_ignored") != 0) {
            ok = 0;
        }
    }
    if (ok) {
        P_OK("39 packs-check-fx");
    } else {
        P_FAIL("39 packs-check-fx", "%d %s %s", st, reason, detail);
    }
}

static void t_rtcfg(int *pass, int *fail)
{
    launcher_settings_t s, r;
    int ok = 1;
    char uc[1100], buf[65536];
    FILE *f = NULL;
    size_t nr = 0;
    lcfg_defaults(&s);
    if (s.rt_scale != 0 || s.rt_scale_dyn != 0) /* Steuer 6.11.: Standard aus */ {
        ok = 0;
    }
    lcfg_apply_set(&s, "rt_scale=3;rt_scale_dyn=0");
    if (s.rt_scale != 3 || s.rt_scale_dyn != 0) {
        ok = 0;
    }
    lcfg_apply_set(&s, "rt_scale=auto");
    if (s.rt_scale != 0) {
        ok = 0;
    }
    lcfg_apply_set(&s, "rt_scale=9");
    if (s.rt_scale != 4) {
        ok = 0;
    }
    /* Datei-Roundtrip: "auto"-Schreibung + verwaltete Keys. */
    _snprintf_s(uc, sizeof uc, _TRUNCATE, "%s/rt.cfg", pk_tmp);
    lcfg_defaults(&r);
    lcfg_apply_set(&r, "rt_scale=auto;rt_scale_dyn=1");
    if (lcfg_save_user(uc, &r) != 0) {
        ok = 0;
    }
    if (fopen_s(&f, uc, "rb") == 0 && f) {
        nr = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[nr] = '\0';
        if (!strstr(buf, "rt_scale=auto") || !strstr(buf, "rt_scale_dyn=1")) {
            ok = 0;
        }
    } else {
        ok = 0;
    }
    memset(&r, 0, sizeof r);
    if (lcfg_load_file(uc, &r) == 0 &&
        (r.rt_scale != 0 || r.rt_scale_dyn != 1)) {
        ok = 0;
    }
    if (ok) {
        P_OK("37 rt-cfg");
    } else {
        P_FAIL("37 rt-cfg", "%s", "Defaults/Roundtrip falsch");
    }
}

/* 47 (W172 LAUNCHER-P5): Abdeckung je Kategorie + paarweise Verdeckung
 * ("verdeckt durch"), Reihenfolge entscheidet, aus = nicht geladen. */
static void pk_cov_pack(const char *name, const char *id, const char *ents)
{
    char dir[1100], p[1200], m[4096];
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s/%s", pk_tmp, name);
    CreateDirectoryA(dir, NULL);
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s/a.dds", dir);
    pk_dds(p, 4, 4, "DXT1");
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s/b.dds", dir);
    pk_dds(p, 8, 8, "DXT1");
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s/manifest.json", dir);
    _snprintf_s(m, sizeof m, _TRUNCATE,
        "{ \"format\": \"sbpack\", \"format_version\": 1,\n"
        "  \"id\": \"%s\", \"name\": \"%s\", \"version\": \"1.0.0\",\n"
        "  \"author\": \"A\", \"license\": \"CC0\", \"type\": \"texture\",\n"
        "  \"hash_algo\": \"sbt1-fnv64\",\n"
        "  \"game\": { \"xbe_sha256\": [\"%064d\"] },\n"
        "  \"entries\": [ %s ] }", id, id, 0, ents);
    pk_write(p, m);
}

#define PK_V2 "\"x2\": { \"file\": \"a.dds\", \"w\": 4, \"h\": 4, \"fmt\": \"BC1\", \"mips\": 1, \"sha256\": \"0\" }"
#define PK_V4 "\"x4\": { \"file\": \"b.dds\", \"w\": 8, \"h\": 8, \"fmt\": \"BC1\", \"mips\": 1, \"sha256\": \"0\" }"

static void t_coverage(int *pass, int *fail)
{
    char pj[1100];
    sb_pack_cov_t cov[4];
    int cb[16], n, ok = 1;
    char why[160] = "";
    pk_cov_pack("ca", "t.ca",
        "{ \"hash\": \"1111111111111111\", \"category\": \"font_hud\", \"orig\": { \"w\": 2, \"h\": 2 },\n"
        "  \"variants\": { " PK_V2 ", " PK_V4 " } },\n"
        "{ \"hash\": \"2222222222222222\", \"category\": \"cockpit\", \"orig\": { \"w\": 2, \"h\": 2 },\n"
        "  \"variants\": { " PK_V2 " } }");
    pk_cov_pack("cb", "t.cb",
        "{ \"hash\": \"1111111111111111\", \"category\": \"font_hud\", \"orig\": { \"w\": 2, \"h\": 2 },\n"
        "  \"variants\": { " PK_V2 " } },\n"
        "{ \"hash\": \"3333333333333333\", \"category\": \"effect\", \"orig\": { \"w\": 2, \"h\": 2 },\n"
        "  \"variants\": { " PK_V2 " } },\n"
        "{ \"hash\": \"4444444444444444\", \"orig\": { \"w\": 2, \"h\": 2 },\n"
        "  \"variants\": { " PK_V2 " } }");
    _snprintf_s(pj, sizeof pj, _TRUNCATE, "%s/cov.json", pk_tmp);
    pk_write(pj, "{ \"version\": 1, \"order\": [\n"
                 "  { \"id\": \"t.ca\", \"file\": \"ca\" },\n"
                 "  { \"id\": \"t.cb\", \"file\": \"cb\" } ] }");
    memset(cov, 0, sizeof cov);
    n = sb_pack_coverage(pj, cov, cb, 4);
    /* ca oben: ca 1 HUD (x2+x4) + 1 Cockpit; cb 1 HUD + 1 Effekte ("effect")
     * + 1 Sonstige; cb verliert h1 an ca. */
    if (n != 2 || cov[0].cat[0] != 1 || cov[0].cat4[0] != 1 ||
        cov[0].cat2[0] != 1 || cov[0].cat[1] != 1 || cov[0].cat4[1] != 0 ||
        cov[1].cat[0] != 1 || cov[1].cat[5] != 1 || cov[1].cat[8] != 1 ||
        cb[1 * 4 + 0] != 1 || cb[0 * 4 + 1] != 0 ||
        strcmp(kSbPackCat[5], "effects") != 0) {
        ok = 0;
        _snprintf_s(why, sizeof why, _TRUNCATE,
                    "oben ca: n=%d ca=%d/%d/%d cb=%d/%d/%d covby=%d/%d", n,
                    cov[0].cat[0], cov[0].cat4[0], cov[0].cat[1], cov[1].cat[0],
                    cov[1].cat[5], cov[1].cat[8], cb[4], cb[1]);
    }
    /* Reihenfolge getauscht: jetzt verliert ca h1 an cb. */
    pk_write(pj, "{ \"version\": 1, \"order\": [\n"
                 "  { \"id\": \"t.cb\", \"file\": \"cb\" },\n"
                 "  { \"id\": \"t.ca\", \"file\": \"ca\" } ] }");
    n = sb_pack_coverage(pj, cov, cb, 4);
    if (ok && (n != 2 || cb[1 * 4 + 0] != 1 || cb[0 * 4 + 1] != 0)) {
        ok = 0;
        _snprintf_s(why, sizeof why, _TRUNCATE, "getauscht: covby=%d/%d", cb[4], cb[1]);
    }
    /* cb aus: nicht geladen (-1), ca verliert nichts mehr. */
    pk_write(pj, "{ \"version\": 1, \"order\": [\n"
                 "  { \"id\": \"t.cb\", \"file\": \"cb\", \"enabled\": false },\n"
                 "  { \"id\": \"t.ca\", \"file\": \"ca\" } ] }");
    n = sb_pack_coverage(pj, cov, cb, 4);
    if (ok && (n != 2 || cov[0].cat[0] != -1 || cb[1 * 4 + 0] != 0 ||
               cov[1].cat[0] != 1)) {
        ok = 0;
        _snprintf_s(why, sizeof why, _TRUNCATE, "aus: cat=%d covby=%d", cov[0].cat[0], cb[4]);
    }
    /* Grenze cap: nur so viele Zeilen wie Platz */
    if (ok && sb_pack_coverage(pj, cov, NULL, 1) != 2) {
        ok = 0;
        strcpy_s(why, sizeof why, "cap 1");
    }
    if (ok) {
        P_OK("47 packs-coverage (Kategorien, verdeckt durch, Reihenfolge)");
    } else {
        P_FAIL("47 packs-coverage", "%s", why);
    }
}

void packs_selftest(int *pass, int *fail)
{
    if (!pk_setup()) {
        P_FAIL("30 sha256", "%s", "kein Temp-Verzeichnis");
        return;
    }
    t_sha256(pass, fail);
    t_cfg(pass, fail);
    t_scan(pass, fail);
    t_check_green(pass, fail);
    t_check_red(pass, fail);
    t_check_red2(pass, fail);
    t_check_yellow(pass, fail);
    t_shadow(pass, fail);
    t_rtcfg(pass, fail);
    t_scan_fx(pass, fail);
    t_check_fx(pass, fail);
    t_coverage(pass, fail);
    pk_rmdir(pk_tmp);
}
