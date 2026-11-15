/* launcher_games.c - W276 LOC-LAUNCH: Spielauswahl Steel Battalion | Line of
 * Contact, LoC-Einstellungen in user.cfg, LoC-Abbild-Pruefung (Title-ID),
 * Startsperre "Coming soon". Reine Logik, keine Oberflaeche (die steht in
 * launcher_gui.c, Block "W276 LOCTAB"). ASCII-Quelltext. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "launcher_games.h"
#include "launcher_app.h"
#include "launcher_u8.h"
#include "sb_lang.h"
#include "sb_gamever.h"
#include "iso_vfs.h"

#define printf cli_printf

void lgame_defaults(lgame_state_t *st)
{
    memset(st, 0, sizeof *st);
    st->game = LGAME_SB;
}

const char *lgame_key(int game)
{
    return game == LGAME_LOC ? "loc" : "sb";
}

int lgame_from_key(const char *v)
{
    return (v && _stricmp(v, "loc") == 0) ? LGAME_LOC : LGAME_SB;
}

/* "key = value" -> Zeiger auf value (ohne Zeilenende-Bereinigung) oder NULL. */
static const char *key_line(const char *line, const char *key)
{
    size_t kl = strlen(key);
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    if (strncmp(line, key, kl) != 0) {
        return NULL;
    }
    line += kl;
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    return *line == '=' ? line + 1 : NULL;
}

static void trim_val(const char *v, char *out, size_t cap)
{
    size_t n;
    while (*v == ' ' || *v == '\t') {
        v++;
    }
    strncpy_s(out, cap, v, _TRUNCATE);
    n = strlen(out);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ' ||
                 out[n - 1] == '\t')) {
        out[--n] = '\0';
    }
}

static const char *const kKeys[3] = { LGAME_KEY_GAME, LGAME_KEY_LOC_IMAGE,
                                      LGAME_KEY_LOC_SERVER };

int lgame_cfg_load(const char *path, lgame_state_t *st)
{
    FILE *f = NULL;
    char line[1200], v[1100];
    lgame_defaults(st);
    if (!path || u8_fopen_s(&f, path, "r") != 0 || !f) {
        return 1;
    }
    while (fgets(line, sizeof line, f)) {
        const char *val;
        if ((val = key_line(line, LGAME_KEY_GAME)) != NULL) {
            trim_val(val, v, sizeof v);
            st->game = lgame_from_key(v);
        } else if ((val = key_line(line, LGAME_KEY_LOC_IMAGE)) != NULL) {
            trim_val(val, v, sizeof v);
            u8_from_acp(v, st->loc_image, sizeof st->loc_image); /* ANSI in der Datei */
        } else if ((val = key_line(line, LGAME_KEY_LOC_SERVER)) != NULL) {
            trim_val(val, v, sizeof v);
            strncpy_s(st->loc_server, sizeof st->loc_server, v, _TRUNCATE);
        }
    }
    fclose(f);
    return 0;
}

int lgame_cfg_save(const char *path, const lgame_state_t *st)
{
    FILE *f = NULL;
    char line[1200], vals[3][1100], *out;
    size_t n = 0, cap = 1024 * 1024;
    int done[3] = { 0, 0, 0 }, k;
    out = (char *)malloc(cap);
    if (!out || !path) {
        free(out);
        return -1;
    }
    strcpy_s(vals[0], sizeof vals[0], lgame_key(st->game));
    u8_to_acp(st->loc_image, vals[1], sizeof vals[1]); /* Kurzname, falls noetig */
    strncpy_s(vals[2], sizeof vals[2], st->loc_server, _TRUNCATE);
    for (k = 0; k < 3; k++) { /* keine Zeilenumbrueche in Werten */
        char *p;
        for (p = vals[k]; *p; p++) {
            if (*p == '\r' || *p == '\n') {
                *p = ' ';
            }
        }
    }
    if (u8_fopen_s(&f, path, "r") == 0 && f) {
        while (fgets(line, sizeof line, f)) {
            size_t l;
            int skip = 0;
            for (k = 0; k < 3; k++) {
                if (key_line(line, kKeys[k])) {
                    if (done[k]) {
                        skip = 1; /* doppelte Zeilen entfernen */
                    } else {
                        _snprintf_s(line, sizeof line, _TRUNCATE, "%s=%s\n", kKeys[k], vals[k]);
                        done[k] = 1;
                    }
                    break;
                }
            }
            if (skip) {
                continue;
            }
            l = strlen(line);
            if (n + l + 4096 >= cap) {
                fclose(f);
                free(out);
                return -1; /* user.cfg ist real < 8 KB */
            }
            memcpy(out + n, line, l);
            n += l;
        }
        fclose(f);
        if (n && out[n - 1] != '\n') {
            out[n++] = '\n';
        }
    }
    if (!done[0] || !done[1] || !done[2]) {
        n += (size_t)_snprintf_s(out + n, cap - n, _TRUNCATE,
                                 "# launcher: chosen game (sb = Steel Battalion, loc = Line of Contact)\n");
        for (k = 0; k < 3; k++) {
            if (!done[k]) {
                n += (size_t)_snprintf_s(out + n, cap - n, _TRUNCATE, "%s=%s\n", kKeys[k], vals[k]);
            }
        }
    }
    if (u8_fopen_s(&f, path, "w") != 0 || !f) {
        free(out);
        return -1;
    }
    k = fwrite(out, 1, n, f) == n;
    fclose(f);
    free(out);
    return k ? 0 : -1;
}

void lgame_loc_dir(const char *base, char *out, size_t cap)
{
    size_t n = base ? strlen(base) : 0;
    int sep = n && (base[n - 1] == '\\' || base[n - 1] == '/');
    _snprintf_s(out, cap, _TRUNCATE, "%s%sloc", base ? base : "", (n && !sep) ? "\\" : "");
}

int lgame_classify(int xbe_ok, uint32_t title_id)
{
    if (!xbe_ok) {
        return LGAME_IMG_UNREADABLE;
    }
    switch (title_id) {
    case LGAME_TID_LOC: return LGAME_IMG_LOC;
    case LGAME_TID_LOC_JP: return LGAME_IMG_LOC_JP;
    case LGAME_TID_SB: return LGAME_IMG_SB;
    default: return LGAME_IMG_OTHER;
    }
}

int lgame_loc_client_ready(void)
{
    char v[8];
    /* W276: Client startet LoC noch nicht (W275). Freischalten = hier 1. */
    if (u8_GetEnv("SB_TEST_LOC_READY", v, sizeof v) > 0 && v[0] == '1') {
        return 1;
    }
#if defined(SB_EDITION_COMMUNITY) && SB_EDITION_COMMUNITY
    /* Nutzer 15.11.: Community-Fassung zeigt LoC, Start bleibt grau -- auch
     * wenn die private Fassung LoC spaeter freischaltet (nur hier freigeben). */
    return 0;
#else
    return 0; /* W276: Client startet LoC noch nicht; Freischalten = hier 1 */
#endif
}

void lgame_check_xbe(const uint8_t *xbe, size_t n, lgame_img_t *out)
{
    sb_gamever_t gv;
    memset(out, 0, sizeof *out);
    memset(&gv, 0, sizeof gv);
    sb_gamever_check(xbe, n, &gv);
    out->kind = lgame_classify(gv.xbe_ok, gv.title_id);
    if (gv.xbe_ok) {
        out->title_id = gv.title_id;
        out->region = gv.region;
        out->version = gv.version;
    }
    strcpy_s(out->sha256, sizeof out->sha256, gv.sha256);
    out->startable = out->kind == LGAME_IMG_LOC && lgame_loc_client_ready();
}

static int read_file_u8(const char *path_u8, uint8_t **buf, size_t *n)
{
    FILE *f = NULL;
    long len;
    *buf = NULL;
    *n = 0;
    if (u8_fopen_s(&f, path_u8, "rb") != 0 || !f) {
        return 1;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) <= 0 || len > 256L * 1024 * 1024 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 1;
    }
    *buf = (uint8_t *)malloc((size_t)len);
    if (!*buf || fread(*buf, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(*buf);
        *buf = NULL;
        return 1;
    }
    fclose(f);
    *n = (size_t)len;
    return 0;
}

int lgame_loc_check(const char *path_u8, lgame_img_t *out)
{
    unsigned long a;
    uint8_t *xb = NULL;
    size_t xn = 0;
    size_t pl;
    memset(out, 0, sizeof *out);
    if (!path_u8 || !path_u8[0]) {
        out->kind = LGAME_IMG_NONE;
        return out->kind;
    }
    a = u8_GetFileAttributes(path_u8);
    if (a == INVALID_FILE_ATTRIBUTES) {
        out->kind = LGAME_IMG_MISSING;
        return out->kind;
    }
    pl = strlen(path_u8);
    if (a & FILE_ATTRIBUTE_DIRECTORY) { /* entpackter Ordner */
        char p[1100];
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s%sdefault.xbe", path_u8,
                    (path_u8[pl - 1] == '\\' || path_u8[pl - 1] == '/') ? "" : "\\");
        if (read_file_u8(p, &xb, &xn) != 0) {
            out->kind = LGAME_IMG_UNREADABLE;
            return out->kind;
        }
    } else if (pl > 4 && _stricmp(path_u8 + pl - 4, ".xbe") == 0) { /* Programmdatei */
        if (read_file_u8(path_u8, &xb, &xn) != 0) {
            out->kind = LGAME_IMG_UNREADABLE;
            return out->kind;
        }
    } else { /* Abbild (ISO/xiso, nur lesend) */
        char acp[1100], e[320];
        uint32_t n32 = 0;
        iso_vfs_t *v;
        u8_to_acp(path_u8, acp, sizeof acp);
        v = iso_vfs_open(acp, e, sizeof e);
        if (!v) {
            out->kind = LGAME_IMG_UNREADABLE;
            return out->kind;
        }
        if (iso_vfs_read_all(v, "default.xbe", &xb, &n32) != 0) {
            iso_vfs_close(v);
            out->kind = LGAME_IMG_UNREADABLE;
            return out->kind;
        }
        iso_vfs_close(v);
        xn = n32;
    }
    lgame_check_xbe(xb, xn, out);
    free(xb);
    return out->kind;
}

int lgame_img_msg(const lgame_img_t *r, const char *path_u8, char *msg, size_t cap)
{
    const char *p = path_u8 ? path_u8 : "";
    switch (r->kind) {
    case LGAME_IMG_NONE:
        strcpy_s(msg, cap, LT("gui.loc_img_none"));
        return 0;
    case LGAME_IMG_MISSING:
        _snprintf_s(msg, cap, _TRUNCATE, LT("gui.loc_img_missing"), p);
        return 2;
    case LGAME_IMG_UNREADABLE:
        _snprintf_s(msg, cap, _TRUNCATE, LT("gui.loc_img_unreadable"), p);
        return 2;
    case LGAME_IMG_LOC:
        _snprintf_s(msg, cap, _TRUNCATE,
                    LT(r->startable ? "gui.loc_img_ok_ready" : "gui.loc_img_ok"), r->title_id);
        return 1;
    case LGAME_IMG_LOC_JP:
        _snprintf_s(msg, cap, _TRUNCATE, LT("gui.loc_img_jp"), r->title_id);
        return 3;
    case LGAME_IMG_SB:
        strcpy_s(msg, cap, LT("gui.loc_img_sb"));
        return 3;
    default:
        _snprintf_s(msg, cap, _TRUNCATE, LT("gui.loc_img_other"), r->title_id);
        return 2;
    }
}

int lgame_loc_start_gate(const lgame_img_t *r, char *msg, size_t cap)
{
    if (!lgame_loc_client_ready()) {
        strcpy_s(msg, cap, LT("gui.loc_soon"));
        return 1;
    }
    if (!r || r->kind != LGAME_IMG_LOC) {
        strcpy_s(msg, cap, LT("gui.loc_start_noimg"));
        return 1;
    }
    msg[0] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* Selbsttest (reine Logik)                                            */
/* ------------------------------------------------------------------ */
static void st_xbe(uint8_t *xb, size_t n, uint32_t tid)
{
    uint32_t v;
    memset(xb, 0, n);
#define ST_PUT(off, val) do { v = (val); memcpy(xb + (off), &v, 4); } while (0)
    ST_PUT(0, 0x48454258u); /* "XBEH" */
    ST_PUT(0x104, 0x10000u);
    ST_PUT(0x118, 0x10200u);
    ST_PUT(0x208, tid);
    ST_PUT(0x2A0, 7u);
    ST_PUT(0x2AC, 1u);
#undef ST_PUT
}

static int st_write(const char *p, const void *b, size_t n)
{
    FILE *f = NULL;
    int ok;
    if (u8_fopen_s(&f, p, "wb") != 0 || !f) {
        return 0;
    }
    ok = fwrite(b, 1, n, f) == n;
    fclose(f);
    return ok;
}

static int st_read(const char *p, char *b, size_t cap)
{
    FILE *f = NULL;
    size_t n = 0;
    b[0] = '\0';
    if (u8_fopen_s(&f, p, "r") != 0 || !f) {
        return 0;
    }
    n = fread(b, 1, cap - 1, f);
    fclose(f);
    b[n] = '\0';
    return 1;
}

void lgame_selftest(int *pass, int *fail)
{
    char tmp[1024], dir[1100], cfg[1200], buf[8192], why[300] = "", keep[16];
    lgame_state_t a, b;
    lgame_img_t r;
    uint8_t xb[0x300];
    u8_GetTempPath(tmp, sizeof tmp);
    /* Ordner mit Umlaut (Pfade UTF-8 intern, ANSI in user.cfg) */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%sw276_l" U8_OE "c", tmp);
    u8_CreateDirectory(dir);
    _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%s\\user.cfg", dir);

    /* --- 1: user.cfg-Rundweg, fremde Zeilen bleiben, Doppelte weg --- */
    u8_DeleteFile(cfg);
    if (lgame_cfg_load(cfg, &a) != 1 || a.game != LGAME_SB || a.loc_image[0]) {
        strcpy_s(why, sizeof why, "fehlende Datei != Standard (sb)");
    }
    if (!why[0]) {
        static const char kIn[] = "[display]\nwidth=1280\n# mein Kommentar\nlauncher_game=loc\n"
                                  "launcher_game=sb\nhdd_root=HDD\n";
        st_write(cfg, kIn, sizeof kIn - 1);
        if (lgame_cfg_load(cfg, &a) != 0 || a.game != LGAME_SB) {
            strcpy_s(why, sizeof why, "letzte Zeile gewinnt nicht");
        }
    }
    if (!why[0]) {
        lgame_defaults(&a);
        a.game = LGAME_LOC;
        _snprintf_s(a.loc_image, sizeof a.loc_image, _TRUNCATE, "%s\\Line of Contact.iso", dir);
        strcpy_s(a.loc_server, sizeof a.loc_server, "play.example.net:3074");
        if (lgame_cfg_save(cfg, &a) != 0 || lgame_cfg_load(cfg, &b) != 0) {
            strcpy_s(why, sizeof why, "Speichern/Laden");
        } else if (b.game != LGAME_LOC || strcmp(a.loc_image, b.loc_image) != 0 ||
                   strcmp(a.loc_server, b.loc_server) != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Rundweg: %d '%s' '%s'", b.game, b.loc_image,
                        b.loc_server);
        }
    }
    if (!why[0]) {
        const char *g1;
        st_read(cfg, buf, sizeof buf);
        g1 = strstr(buf, "launcher_game=");
        if (!strstr(buf, "width=1280\n") || !strstr(buf, "# mein Kommentar\n") ||
            !strstr(buf, "hdd_root=HDD\n") || !g1 || strstr(g1 + 1, "launcher_game=") ||
            !strstr(buf, "launcher_game=loc\n") || !strstr(buf, "launcher_loc_server=play.example.net:3074\n")) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Inhalt: %.80s", buf); /* ohne Temp-Pfad */
        }
    }
    if (!why[0]) { /* Umschalten zurueck: Pfad bleibt (je Spiel eigener Abbild-Pfad) */
        a.game = LGAME_SB;
        if (lgame_cfg_save(cfg, &a) != 0 || lgame_cfg_load(cfg, &b) != 0 || b.game != LGAME_SB ||
            strcmp(b.loc_image, a.loc_image) != 0) {
            strcpy_s(why, sizeof why, "zurueck auf sb: LoC-Pfad verloren");
        }
    }
    if (!why[0]) {
        printf("PASS W276 loctab: user.cfg (Spielwahl merken, LoC-Abbild/Server eigen, fremde Zeilen bleiben, Doppelte weg, Umlaut)\n");
        (*pass)++;
    } else {
        printf("FAIL W276 loctab user.cfg: %s\n", why);
        (*fail)++;
    }

    /* --- 2: getrennte Datenordner + Erkennung (Ordner/xbe/ISO) --- */
    why[0] = '\0';
    {
        char o[1200];
        lgame_loc_dir("C:\\Data\\Steelbound", o, sizeof o);
        if (strcmp(o, "C:\\Data\\Steelbound\\loc") != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "loc_dir '%s'", o);
        }
        lgame_loc_dir("C:\\Data\\Steelbound\\", o, sizeof o);
        if (!why[0] && strcmp(o, "C:\\Data\\Steelbound\\loc") != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "loc_dir mit Trenner '%s'", o);
        }
    }
    if (!why[0]) {
        static const struct { uint32_t tid; int kind; } k[] = {
            { LGAME_TID_LOC, LGAME_IMG_LOC }, { LGAME_TID_LOC_JP, LGAME_IMG_LOC_JP },
            { LGAME_TID_SB, LGAME_IMG_SB }, { 0x4D530004u, LGAME_IMG_OTHER },
        };
        int i;
        for (i = 0; i < 4 && !why[0]; i++) {
            char p[1200], acp[1200];
            st_xbe(xb, sizeof xb, k[i].tid);
            /* Ordner mit default.xbe */
            _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\default.xbe", dir);
            st_write(p, xb, sizeof xb);
            if (lgame_loc_check(dir, &r) != k[i].kind || r.title_id != k[i].tid) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Ordner %08X -> %d", k[i].tid, r.kind);
            }
            /* direkte .xbe */
            if (!why[0] && (lgame_loc_check(p, &r) != k[i].kind || strlen(r.sha256) != 64)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "xbe %08X -> %d", k[i].tid, r.kind);
            }
            u8_DeleteFile(p);
            /* Abbild */
            _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\t.iso", dir);
            u8_to_acp(p, acp, sizeof acp);
            if (!why[0] && (iso_vfs_make_test_image(acp, xb, sizeof xb) != 0 ||
                            lgame_loc_check(p, &r) != k[i].kind || r.startable)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "ISO %08X -> %d", k[i].tid, r.kind);
            }
            u8_DeleteFile(p);
        }
        if (!why[0]) {
            char p[1200];
            _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\fehlt.iso", dir);
            if (lgame_loc_check(p, &r) != LGAME_IMG_MISSING || lgame_loc_check("", &r) != LGAME_IMG_NONE) {
                strcpy_s(why, sizeof why, "fehlend/leer");
            }
            _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\kaputt.iso", dir);
            st_write(p, "kein Abbild", 11);
            if (!why[0] && lgame_loc_check(p, &r) != LGAME_IMG_UNREADABLE) {
                strcpy_s(why, sizeof why, "kaputtes Abbild");
            }
            u8_DeleteFile(p);
            if (!why[0] && lgame_loc_check(dir, &r) != LGAME_IMG_UNREADABLE) {
                strcpy_s(why, sizeof why, "Ordner ohne default.xbe");
            }
        }
    }
    if (!why[0]) { /* optional: echte LoC-XBE (nur Erkennung, nichts gespeichert) */
        char real[1100];
        if (u8_GetEnv("SB_TEST_LOC_XBE", real, sizeof real) > 0) {
            lgame_loc_check(real, &r);
            printf("     (W276: echte LoC-XBE: Art %d, Title-ID %08X, Region %u)\n", r.kind,
                   r.title_id, r.region);
            if (r.kind != LGAME_IMG_LOC) {
                strcpy_s(why, sizeof why, "echte LoC-XBE nicht erkannt");
            }
        }
    }
    if (!why[0]) {
        printf("PASS W276 loctab: getrennte Datenordner (<Daten>\\loc) + Erkennung per Title-ID (LoC/LoC-JP/SB/fremd; Ordner, .xbe, ISO; fehlend, kaputt)\n");
        (*pass)++;
    } else {
        printf("FAIL W276 loctab Erkennung: %s\n", why);
        (*fail)++;
    }

    /* --- 3: Startsperre + Texte EN/DE --- */
    why[0] = '\0';
    strcpy_s(keep, sizeof keep, sb_lang_code());
    {
        int li;
        for (li = 0; li < 2 && !why[0]; li++) {
            char m[600];
            lgame_img_t ok;
            memset(&ok, 0, sizeof ok);
            ok.kind = LGAME_IMG_LOC;
            ok.title_id = LGAME_TID_LOC;
            sb_lang_set(li ? "de" : "en");
            SetEnvironmentVariableA("SB_TEST_LOC_READY", NULL);
            if (lgame_loc_start_gate(&ok, m, sizeof m) != 1 || strcmp(m, LT("gui.loc_soon")) != 0 ||
                !strstr(m, li ? "Demn" : "Coming soon")) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "%s: Sperre '%s'", li ? "de" : "en", m);
            } else if (lgame_loc_start_gate(NULL, m, sizeof m) != 1) {
                strcpy_s(why, sizeof why, "Sperre ohne Abbild");
            }
            if (!why[0] && (lgame_img_msg(&ok, "", m, sizeof m) != 1 || !strstr(m, "43430009"))) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Text ok '%s'", m);
            }
            ok.kind = LGAME_IMG_SB;
            if (!why[0] && (lgame_img_msg(&ok, "", m, sizeof m) != 3 || !strstr(m, "Steel Battalion"))) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Text SB '%s'", m);
            }
            ok.kind = LGAME_IMG_UNREADABLE;
            if (!why[0] && (lgame_img_msg(&ok, "X:\\a.iso", m, sizeof m) != 2 || !strstr(m, "X:\\a.iso"))) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Text kaputt '%s'", m);
            }
            /* spaeterer Weg: Client bereit -> LoC startbar, anderes Abbild nicht */
            SetEnvironmentVariableA("SB_TEST_LOC_READY", "1");
            ok.kind = LGAME_IMG_LOC;
            if (!why[0] && lgame_loc_start_gate(&ok, m, sizeof m) != 0) {
                strcpy_s(why, sizeof why, "bereit: LoC gesperrt");
            }
            ok.kind = LGAME_IMG_SB;
            if (!why[0] && (lgame_loc_start_gate(&ok, m, sizeof m) != 1 ||
                            strcmp(m, LT("gui.loc_start_noimg")) != 0)) {
                strcpy_s(why, sizeof why, "bereit: SB-Abbild nicht gesperrt");
            }
            SetEnvironmentVariableA("SB_TEST_LOC_READY", NULL);
        }
    }
    sb_lang_set(keep);
    if (!why[0]) {
        printf("PASS W276 loctab: LoC-Start gesperrt mit Meldung (EN/DE), spaeterer Freigabeweg, Texte je Erkennung\n");
        (*pass)++;
    } else {
        printf("FAIL W276 loctab Startsperre: %s\n", why);
        (*fail)++;
    }

    /* --- 4: W292 LOC-COMM: Community-Fassung -> LoC-Start grau (Nutzer 15.11.) ---
     * Der Client enthaelt die LoC-Netz-HLE in beiden Fassungen; gesperrt bleibt
     * nur der Launcher-Start. Ohne Test-Freigabe: nie startbar, Sperrmeldung,
     * erkanntes LoC-Abbild bleibt "nicht startbar"; Tab/Erkennung laufen weiter. */
    why[0] = '\0';
    {
        char m[600];
        lgame_img_t ok;

        char p[1200];
        SetEnvironmentVariableA("SB_TEST_LOC_READY", NULL);
        memset(&ok, 0, sizeof ok);
        ok.kind = LGAME_IMG_LOC;
        ok.title_id = LGAME_TID_LOC;
        if (lgame_loc_client_ready() != 0) {
            strcpy_s(why, sizeof why, "lgame_loc_client_ready() != 0");
        } else if (lgame_loc_start_gate(&ok, m, sizeof m) != 1 || strcmp(m, LT("gui.loc_soon")) != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Start nicht gesperrt '%s'", m);
        }
        if (!why[0]) { /* echtes (synthetisches) LoC-Abbild: erkannt, aber nicht startbar */
            st_xbe(xb, sizeof xb, LGAME_TID_LOC);
            _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\default.xbe", dir);
            st_write(p, xb, sizeof xb);
            if (lgame_loc_check(dir, &r) != LGAME_IMG_LOC || r.startable) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "LoC-Abbild Art %d startbar %d", r.kind, r.startable);
            }
            u8_DeleteFile(p);
        }
    }
    if (!why[0]) {
#if defined(SB_EDITION_COMMUNITY) && SB_EDITION_COMMUNITY
        printf("PASS W292 loctab: Community-Fassung - LoC-Start grau (gesperrt, Meldung, LoC-Abbild erkannt aber nicht startbar)\n");
#else
        printf("PASS W292 loctab: Private Fassung - LoC-Start heute ebenfalls gesperrt (Community-Sperre: eigener Zweig in lgame_loc_client_ready)\n");
#endif
        (*pass)++;
    } else {
        printf("FAIL W292 loctab Community-LoC-Start grau: %s\n", why);
        (*fail)++;
    }
    u8_DeleteFile(cfg);
    {
        wchar_t w[1200];
        u8_to_w(dir, w, 1200);
        RemoveDirectoryW(w);
    }
}
