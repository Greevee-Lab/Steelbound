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
#include "launcher_cfg.h" /* W369: lcfg_image_check im Selbsttest */
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

#define LGAME_NKEYS 5 /* W402: + launcher_pergame (Markierung der Uebernahme) */
static const char *const kKeys[LGAME_NKEYS] = { LGAME_KEY_GAME, LGAME_KEY_LOC_IMAGE,
                                                LGAME_KEY_LOC_SERVER, LGAME_KEY_LOC_WIDE,
                                                LGAME_KEY_PERGAME };

/* W326 LOC-WIDE: Bildformat (Index der Auswahl = Wert) */
int lgame_wide_count(void)
{
    return LOC_WIDE_720P ? 3 : 2;
}

int lgame_wide_clamp(int v)
{
    return (v >= 0 && v < lgame_wide_count()) ? v : LGAME_WIDE_43;
}

int lgame_wide_experimental(int v)
{
    return lgame_wide_clamp(v) != LGAME_WIDE_43;
}

const char *lgame_wide_label_key(int v)
{
    switch (lgame_wide_clamp(v)) {
    case LGAME_WIDE_169: return "gui.loc_wide_169";
    case LGAME_WIDE_720P: return "gui.loc_wide_720p";
    default: return "gui.loc_wide_43";
    }
}

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
        } else if ((val = key_line(line, LGAME_KEY_LOC_WIDE)) != NULL) {
            trim_val(val, v, sizeof v);
            st->loc_widescreen = lgame_wide_clamp(atoi(v)); /* wie hcfg (atoi), ungueltig -> 0 */
        }
    }
    fclose(f);
    return 0;
}

int lgame_cfg_save(const char *path, const lgame_state_t *st)
{
    return lgame_cfg_save_ex(path, st, LGAME_K_GAME | LGAME_K_IMAGE | LGAME_K_SERVER | LGAME_K_WIDE, 0);
}

/* W402 PERGAME: nur die Schluessel aus "mask" schreiben, die aus "drop" entfernen (Rest bleibt). */
int lgame_cfg_save_ex(const char *path, const lgame_state_t *st, unsigned mask, unsigned drop)
{
    FILE *f = NULL;
    char line[1200], vals[LGAME_NKEYS][1100], *out;
    size_t n = 0, cap = 1024 * 1024;
    int done[LGAME_NKEYS] = { 0, 0, 0, 0, 0 }, k;
    out = (char *)malloc(cap);
    if (!out || !path) {
        free(out);
        return -1;
    }
    strcpy_s(vals[0], sizeof vals[0], lgame_key(st->game));
    u8_to_acp(st->loc_image, vals[1], sizeof vals[1]); /* Kurzname, falls noetig */
    strncpy_s(vals[2], sizeof vals[2], st->loc_server, _TRUNCATE);
    _snprintf_s(vals[3], sizeof vals[3], _TRUNCATE, "%d", lgame_wide_clamp(st->loc_widescreen));
    strcpy_s(vals[4], sizeof vals[4], "1");
    for (k = 0; k < LGAME_NKEYS; k++) {
        if (!(mask & (1u << k))) {
            done[k] = 1; /* nicht schreiben: vorhandene Zeile bleibt (oder faellt per drop weg) */
        }
    }
    for (k = 0; k < LGAME_NKEYS; k++) { /* keine Zeilenumbrueche in Werten */
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
            for (k = 0; k < LGAME_NKEYS; k++) {
                if (key_line(line, kKeys[k])) {
                    if (drop & (1u << k)) {
                        skip = 1; /* W402: gehoert nicht in diese Datei */
                    } else if (!(mask & (1u << k))) {
                        /* fremder Schluessel ausserhalb der Maske: unveraendert lassen */
                    } else if (done[k]) {
                        skip = 1; /* doppelte Zeilen entfernen */
                    } else {
                        _snprintf_s(line, sizeof line, _TRUNCATE, "%s=%s\n", kKeys[k], vals[k]);
                        done[k] = 1;
                    }
                    break;
                }
            }
            if (!skip && line[0] == '#' && /* W402: Kommentarzeile des entfernten Schluessels mit entfernen */
                (((drop & LGAME_K_WIDE) && strncmp(line, "# Line of Contact picture format", 32) == 0) ||
                 ((drop & LGAME_K_GAME) && strncmp(line, "# launcher: chosen game", 23) == 0))) {
                skip = 1;
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
    if (!done[0] || !done[1] || !done[2] || !done[4]) {
        n += (size_t)_snprintf_s(out + n, cap - n, _TRUNCATE,
                                 "# launcher: chosen game (sb = Steel Battalion, loc = Line of Contact)\n");
    }
    if (!done[3]) { /* W326: eigene Kommentarzeile (auch an aeltere user.cfg angehaengt) */
        n += (size_t)_snprintf_s(out + n, cap - n, _TRUNCATE,
                                 "# Line of Contact picture format (0 = 4:3 original, 1 = 16:9, 2 = 720p)\n");
    }
    for (k = 0; k < LGAME_NKEYS; k++) {
        if (!done[k]) {
            n += (size_t)_snprintf_s(out + n, cap - n, _TRUNCATE, "%s=%s\n", kKeys[k], vals[k]);
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

/* ------------------------------------------------------------------ */
/* W402 LAUNCHER-PERGAME: eigene Einstellungen je Spiel                 */
/* ------------------------------------------------------------------ */
void lgame_cfg_path(const char *usercfg, int game, char *out, size_t cap)
{
    const char *a, *b, *sep;
    size_t dl;
    if (game != LGAME_LOC || !usercfg) {
        strcpy_s(out, cap, usercfg ? usercfg : "");
        return;
    }
    a = usercfg ? strrchr(usercfg, '\\') : NULL;
    b = usercfg ? strrchr(usercfg, '/') : NULL;
    sep = (a && b) ? (a > b ? a : b) : (a ? a : b);
    dl = sep ? (size_t)(sep - usercfg) + 1 : 0;
    if (_stricmp(usercfg + dl, "user.cfg") == 0 || !usercfg[dl]) {
        _snprintf_s(out, cap, _TRUNCATE, "%.*s%s", (int)dl, usercfg, LGAME_LOC_CFG_NAME);
    } else { /* andere Namen (Selbsttest, SB_USER_CFG): <name>_loc.cfg daneben */
        const char *dot = strrchr(usercfg + dl, '.');
        size_t bl = dot ? (size_t)(dot - usercfg) : strlen(usercfg);
        _snprintf_s(out, cap, _TRUNCATE, "%.*s_loc%s", (int)bl, usercfg, dot ? dot : ".cfg");
    }
}

static int file_exists_u8(const char *p)
{
    FILE *f = NULL;
    if (!p || !p[0] || u8_fopen_s(&f, p, "r") != 0 || !f) {
        return 0;
    }
    fclose(f);
    return 1;
}

int lgame_pergame_marked(const char *usercfg)
{
    FILE *f = NULL;
    char line[1200], v[64];
    int on = 0;
    if (!usercfg || u8_fopen_s(&f, usercfg, "r") != 0 || !f) {
        return 0;
    }
    while (fgets(line, sizeof line, f)) {
        const char *val = key_line(line, LGAME_KEY_PERGAME);
        if (val) {
            trim_val(val, v, sizeof v);
            on = atoi(v) != 0;
        }
    }
    fclose(f);
    return on;
}

/* Gehoert eine user.cfg-Zeile nur dem Launcher bzw. zu Steel Battalion (nicht in die LoC-Datei)? */
static int pergame_sb_only_line(const char *line)
{
    static const char *const kSbOnly[] = { "xbe_path", "media_root", "hdd_root", "game_image", "update_check" };
    const char *p = line;
    size_t i;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (strncmp(p, "launcher_", 9) == 0 || strncmp(p, "# launcher:", 11) == 0 ||
        strncmp(p, "# user.cfg", 10) == 0) {
        return 1;
    }
    for (i = 0; i < sizeof kSbOnly / sizeof kSbOnly[0]; i++) {
        if (key_line(p, kSbOnly[i])) {
            return 1;
        }
    }
    return 0;
}

int lgame_pergame_migrate(const char *usercfg, const char *loccfg)
{
    FILE *f = NULL, *o = NULL;
    char line[4096];
    lgame_state_t st;
    int ok = 1;
    if (!usercfg || !loccfg || !file_exists_u8(usercfg) || lgame_pergame_marked(usercfg)) {
        return 0; /* neu (noch keine Werte) oder schon uebernommen */
    }
    lgame_defaults(&st);
    if (!file_exists_u8(loccfg)) { /* nur kopieren, wenn es noch keine LoC-Datei gibt */
        if (u8_fopen_s(&f, usercfg, "r") != 0 || !f) {
            return -1;
        }
        if (u8_fopen_s(&o, loccfg, "w") != 0 || !o) {
            fclose(f);
            return -1;
        }
        fputs("# " LGAME_LOC_CFG_NAME " - Line of Contact settings (launcher). Steel Battalion uses user.cfg.\n", o);
        while (fgets(line, sizeof line, f)) {
            if (!pergame_sb_only_line(line)) {
                ok &= fputs(line, o) >= 0;
            }
        }
        fclose(f);
        ok &= fclose(o) == 0;
        if (!ok) {
            return -1;
        }
    }
    /* Markierung in user.cfg; loc_widescreen gehoert ab jetzt nur in die LoC-Datei */
    return lgame_cfg_save_ex(usercfg, &st, LGAME_K_PERGAME, LGAME_K_WIDE) == 0 ? 1 : -1;
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

/* W353 LOC-DURCHSTICH: Freigabe des LoC-Starts je Ausgabe. Interne Ausgabe: frei.
 * Community: mit CMake SB_LOC_PLAY (W389 RELEASE-030: seit 0.3.0 Standard an, LoC fuer alle, Nutzerentscheid
 * 20.11.); -DSB_LOC_PLAY=OFF = Notschalter, LoC-Start grau wie bis 0.2.x. Hinweis: Die Sperre ist Bedienung, keine Sicherheitsgrenze (der
 * Client startet LoC mit loc_boot=1 in jeder Ausgabe). */
#if defined(SB_EDITION_COMMUNITY) && SB_EDITION_COMMUNITY
#if defined(SB_LOC_PLAY) && SB_LOC_PLAY
#define LGAME_LOC_BUILD_READY 1
#else
#define LGAME_LOC_BUILD_READY 0
#endif
#else
#define LGAME_LOC_BUILD_READY 1
#endif

int lgame_loc_build_ready(void)
{
    return LGAME_LOC_BUILD_READY;
}

int lgame_loc_client_ready(void)
{
    char v[8];
    /* Test-Haken (W276): SB_TEST_LOC_READY=1 frei, W353: =0 gesperrt (Sperrtexte in jeder Ausgabe pruefbar) */
    if (u8_GetEnv("SB_TEST_LOC_READY", v, sizeof v) > 0) {
        if (v[0] == '1') {
            return 1;
        }
        if (v[0] == '0') {
            return 0;
        }
    }
    return LGAME_LOC_BUILD_READY;
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
    /* W384 (GH #52): Client startet nur bekannte LoC-Fassungen (sb_gamever_loc_variant) */
    out->ver_unknown = out->kind == LGAME_IMG_LOC && gv.xbe_ok && !sb_gamever_loc_variant(gv.sha256);
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
        strncpy_s(msg, cap, LT("gui.loc_img_none"), _TRUNCATE);
        return 0;
    case LGAME_IMG_MISSING:
        _snprintf_s(msg, cap, _TRUNCATE, LT("gui.loc_img_missing"), p);
        return 2;
    case LGAME_IMG_UNREADABLE:
        _snprintf_s(msg, cap, _TRUNCATE, LT("gui.loc_img_unreadable"), p);
        return 2;
    case LGAME_IMG_LOC:
        if (r->ver_unknown && r->startable) { /* W384: z. B. Titel-Update eingespielt */
            _snprintf_s(msg, cap, _TRUNCATE, LT("gui.loc_img_ver"), r->title_id);
            return 3;
        }
        _snprintf_s(msg, cap, _TRUNCATE,
                    LT(r->startable ? "gui.loc_img_ok_ready" : "gui.loc_img_ok"), r->title_id);
        return 1;
    case LGAME_IMG_LOC_JP:
        _snprintf_s(msg, cap, _TRUNCATE, LT("gui.loc_img_jp"), r->title_id);
        return 3;
    case LGAME_IMG_SB:
        strncpy_s(msg, cap, LT("gui.loc_img_sb"), _TRUNCATE);
        return 3;
    default:
        _snprintf_s(msg, cap, _TRUNCATE, LT("gui.loc_img_other"), r->title_id);
        return 2;
    }
}

/* ';' im Pfad fuer hcfg_apply_set maskieren (ACP-Text anhaengen). */
static void loc_set_cat_esc(char *set, size_t cap, const char *v)
{
    size_t n = strlen(set);
    for (; *v && n + 3 < cap; v++) {
        if (*v == ';') {
            set[n++] = '\\';
        }
        set[n++] = *v;
    }
    set[n] = '\0';
}

void lgame_loc_image_set(const char *img_u8, char *set, size_t cap, char *xbe_u8, size_t xcap)
{
    char dir[1024], xbe[1100], acp[1200];
    strcpy_s(set, cap, "loc_boot=1;game_image=");
    if (img_u8 && img_u8[0] && lcfg_image_as_folder(img_u8, dir, sizeof dir, xbe, sizeof xbe)) {
        /* Ordner-Modus: game_image leer (ueberschreibt auch das SB-Abbild aus user.cfg) */
        strcat_s(set, cap, ";xbe_path=");
        u8_to_acp(xbe, acp, sizeof acp);
        loc_set_cat_esc(set, cap, acp);
        strcat_s(set, cap, ";media_root=");
        u8_to_acp(dir, acp, sizeof acp);
        loc_set_cat_esc(set, cap, acp);
        strcpy_s(xbe_u8, xcap, xbe);
        return;
    }
    u8_to_acp(img_u8 ? img_u8 : "", acp, sizeof acp);
    loc_set_cat_esc(set, cap, acp);
    strcpy_s(xbe_u8, xcap, img_u8 ? img_u8 : "");
}

int lgame_loc_start_gate(const lgame_img_t *r, char *msg, size_t cap)
{
    if (!lgame_loc_client_ready()) {
        strncpy_s(msg, cap, LT("gui.loc_soon"), _TRUNCATE);
        return 1;
    }
    if (!r || r->kind != LGAME_IMG_LOC) {
        strncpy_s(msg, cap, LT("gui.loc_start_noimg"), _TRUNCATE);
        return 1;
    }
    if (r->ver_unknown) { /* W384 (GH #52): Client wuerde mit "nicht unterstuetzt" abbrechen */
        _snprintf_s(msg, cap, _TRUNCATE, LT("gui.loc_img_ver"), r->title_id);
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
                            lgame_loc_check(p, &r) != k[i].kind ||
                            r.startable != (k[i].kind == LGAME_IMG_LOC && lgame_loc_client_ready()))) { /* W353 */
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

    /* --- 2a: W378 LOC-IMAGE-VARIANT: Client-Erkennung beider 1.0-XBEs (Media-Patch
     * und unveraendert) per SHA-256; optional echte XBEs (SB_TEST_LOC_XBES, Dateien mit ";" getrennt) --- */
    why[0] = '\0';
    {
        static const char k_up[] = "B75FAAF36947F967C33435EC64FA792F9F99B9BC40254CF91484FA13347E4117";
        char sh[66];
        if (sb_gamever_loc_variant(SB_GAMEVER_LOC_SHA) != 1 ||
            sb_gamever_loc_variant(SB_GAMEVER_LOC_SHA_STOCK) != 2 || sb_gamever_loc_variant(k_up) != 2) {
            strcpy_s(why, sizeof why, "bekannte LoC-SHA nicht zugeordnet");
        }
        strcpy_s(sh, sizeof sh, SB_GAMEVER_LOC_SHA_STOCK);
        sh[63] = sh[63] == '7' ? '8' : '7'; /* ein Zeichen anders */
        if (!why[0] && (sb_gamever_loc_variant(sh) != 0 || sb_gamever_loc_variant("") != 0 ||
                        sb_gamever_loc_variant(NULL) != 0 ||
                        sb_gamever_loc_variant("a8285ca55dee8e770ef180f22665981101b3818e7e00858e60daa766b8b2e8f7") != 0)) {
            strcpy_s(why, sizeof why, "fremde SHA als LoC erkannt");
        }
        strcpy_s(sh, sizeof sh, SB_GAMEVER_LOC_SHA);
        strcat_s(sh, sizeof sh, "0"); /* 65 Zeichen */
        if (!why[0] && sb_gamever_loc_variant(sh) != 0) {
            strcpy_s(why, sizeof why, "zu lange SHA als LoC erkannt");
        }
    }
    if (!why[0]) {
        char list[2200], *ctx = NULL, *tok;
        if (u8_GetEnv("SB_TEST_LOC_XBES", list, sizeof list) > 0) {
            for (tok = strtok_s(list, ";", &ctx); tok && !why[0]; tok = strtok_s(NULL, ";", &ctx)) {
                uint8_t *xb2 = NULL;
                size_t n2 = 0;
                sb_gamever_t gv;
                memset(&gv, 0, sizeof gv);
                if (read_file_u8(tok, &xb2, &n2) != 0) {
                    continue; /* Ordner/ISO prueft W276 oben */
                }
                sb_gamever_check(xb2, n2, &gv);
                free(xb2);
                printf("     (W378: echte LoC-XBE: loc=%d, Version %u, sha256 %.16s...)\n", gv.loc, gv.version,
                       gv.sha256);
                if (!gv.loc || gv.title_id != SB_GAMEVER_LOC_TITLE) {
                    strcpy_s(why, sizeof why, "echte LoC-XBE vom Client nicht erkannt");
                }
            }
        }
    }
    if (!why[0]) {
        printf("PASS W378 locvariant: Client erkennt LoC 1.0 mit Media-Patch (loc=1) und unveraendert (loc=2), fremde/kaputte SHA nicht\n");
        (*pass)++;
    } else {
        printf("FAIL W378 locvariant: %s\n", why);
        (*fail)++;
    }

    /* --- 2a2: W384 (GH #52): LoC-Start mit .xbe/Ordner = Ordner-Modus (game_image leer, sonst
     * liest der Client die XBE als ISO bzw. nimmt das SB-Abbild aus user.cfg); ISO bleibt game_image;
     * ';' maskiert; unbekannte LoC-Fassung (z. B. Titel-Update) -> klare Meldung + Start gesperrt --- */
    why[0] = '\0';
    {
        char p[1200], d2[1200], iso[1200], pacp[1200], dacp[1200], set[4096], xu[1200], want[4096], m[600];
        lgame_img_t f;
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\default.xbe", dir);
        _snprintf_s(d2, sizeof d2, _TRUNCATE, "%s\\", dir);
        _snprintf_s(iso, sizeof iso, _TRUNCATE, "%s\\L;oc.iso", dir);
        st_xbe(xb, sizeof xb, LGAME_TID_LOC);
        st_write(p, xb, sizeof xb);
        u8_to_acp(p, pacp, sizeof pacp);
        u8_to_acp(dir, dacp, sizeof dacp);
        _snprintf_s(want, sizeof want, _TRUNCATE, "loc_boot=1;game_image=;xbe_path=%s;media_root=%s", pacp, dacp);
        lgame_loc_image_set(p, set, sizeof set, xu, sizeof xu);
        if (strcmp(set, want) != 0 || strcmp(xu, p) != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, ".xbe -> '%.200s'", set);
        }
        lgame_loc_image_set(dir, set, sizeof set, xu, sizeof xu);
        if (!why[0] && (strcmp(set, want) != 0 || strcmp(xu, p) != 0)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Ordner -> '%.200s'", set);
        }
        lgame_loc_image_set(d2, set, sizeof set, xu, sizeof xu);
        if (!why[0] && strcmp(set, want) != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Ordner mit Trenner -> '%.200s'", set);
        }
        lgame_loc_image_set(iso, set, sizeof set, xu, sizeof xu); /* ISO (auch fehlend): game_image */
        u8_to_acp(iso, pacp, sizeof pacp);
        if (!why[0] && (strncmp(set, "loc_boot=1;game_image=", 22) != 0 || strstr(set, "xbe_path") ||
                        !strstr(set, "L\\;oc.iso") || strcmp(xu, iso) != 0)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "ISO -> '%.200s'", set);
        }
        /* Fassung: synthetische LoC-XBE (unbekannte SHA) = ver_unknown, Start gesperrt mit Meldung */
        if (!why[0] && (lgame_loc_check(p, &f) != LGAME_IMG_LOC || !f.ver_unknown)) {
            strcpy_s(why, sizeof why, "unbekannte LoC-Fassung nicht markiert");
        }
        if (!why[0] && lgame_loc_client_ready() &&
            (lgame_loc_start_gate(&f, m, sizeof m) != 1 || !strstr(m, "4343000"))) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Startsperre '%.200s'", m);
        }
        if (!why[0] && lgame_loc_client_ready() && (lgame_img_msg(&f, p, m, sizeof m) != 3 || !strstr(m, "4343000"))) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Meldung '%.200s'", m);
        }
        memset(&f, 0, sizeof f); /* bekannte Fassung: ver_unknown 0 -> Start frei */
        f.kind = LGAME_IMG_LOC;
        f.startable = lgame_loc_client_ready();
        if (!why[0] && lgame_loc_client_ready() && lgame_loc_start_gate(&f, m, sizeof m) != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "bekannte Fassung gesperrt '%.200s'", m);
        }
        u8_DeleteFile(p);
    }
    if (!why[0]) {
        printf("PASS W384 loc-folder: LoC-Start mit .xbe/Ordner im Ordner-Modus (game_image leer), ISO als game_image, ';' maskiert, unbekannte LoC-Fassung gesperrt mit Meldung\n");
        (*pass)++;
    } else {
        printf("FAIL W384 loc-folder: %s\n", why);
        (*fail)++;
    }

    /* --- 2b: W369 LAUNCHER-XBE-PICK: .xbe/Spielordner in der SB-Karte =
     * Ordner-Modus; falsches Spiel -> Hinweis auf den anderen Tab (EN/DE) --- */
    why[0] = '\0';
    strcpy_s(keep, sizeof keep, sb_lang_code());
    {
        char p[1200], d2[1200], iso[1200], acp[1200], m[1200], m2[1200];
        launcher_settings_t s;
        int li, rc;
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\default.xbe", dir);
        _snprintf_s(d2, sizeof d2, _TRUNCATE, "%s\\", dir);
        _snprintf_s(iso, sizeof iso, _TRUNCATE, "%s\\w369.iso", dir);
        for (li = 0; li < 2 && !why[0]; li++) {
            sb_lang_set(li ? "de" : "en");
            /* SB-xbe (Datei, Ordner, Ordner mit Trenner): angenommen, Fassung geprueft */
            st_xbe(xb, sizeof xb, LGAME_TID_SB);
            st_write(p, xb, sizeof xb);
            rc = lcfg_image_check(p, m, sizeof m);
            if ((rc != 0 && rc != 4) || !strstr(m, "43430002")) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "SB-xbe rc %d '%.120s'", rc, m);
            } else if (lcfg_image_check(dir, m2, sizeof m2) != rc || strcmp(m, m2) != 0 ||
                       lcfg_image_check(d2, m2, sizeof m2) != rc) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "SB-Ordner '%.120s'", m2);
            }
            lcfg_defaults(&s);
            strcpy_s(s.game_image, sizeof s.game_image, p);
            if (!why[0] && (!lcfg_image_folder_mode(&s) || s.game_image[0] ||
                            strcmp(s.media_root, dir) != 0 || strcmp(s.xbe_path, p) != 0)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Ordner-Modus aus xbe: '%.60s'", s.media_root);
            }
            lcfg_defaults(&s);
            strcpy_s(s.game_image, sizeof s.game_image, d2);
            if (!why[0] && (!lcfg_image_folder_mode(&s) || strcmp(s.media_root, dir) != 0 ||
                            strcmp(s.xbe_path, p) != 0)) {
                strcpy_s(why, sizeof why, "Ordner-Modus aus Ordner");
            }
            lcfg_defaults(&s);
            strcpy_s(s.game_image, sizeof s.game_image, p);
            if (!why[0] && (lcfg_validate(&s, m2, sizeof m2) == 0 ? rc != 0 : strcmp(m, m2) != 0)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "validate xbe '%.120s'", m2);
            }
            /* SB-xbe in der LoC-Karte -> Hinweis auf den SB-Tab */
            if (!why[0] && (lgame_loc_check(p, &r) != LGAME_IMG_SB || lgame_img_msg(&r, p, m, sizeof m) != 3 ||
                            strcmp(m, LT("gui.loc_img_sb")) != 0 || !strstr(m, "Steel Battalion") ||
                            !strstr(m, li ? "Tab" : "tab"))) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "SB in LoC-Karte '%.120s'", m);
            }
            /* LoC-xbe (Datei, Ordner, ISO, Erweitert) in der SB-Karte -> Hinweis auf den LoC-Tab */
            st_xbe(xb, sizeof xb, LGAME_TID_LOC);
            st_write(p, xb, sizeof xb);
            if (!why[0] && (lcfg_image_check(p, m, sizeof m) != 5 || strcmp(m, LT("cfg.image_is_loc")) != 0 ||
                            !strstr(m, "Line of Contact") || !strstr(m, li ? "Tab" : "tab") ||
                            lcfg_image_check(dir, m2, sizeof m2) != 5)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "LoC in SB-Karte '%.120s'", m);
            }
            u8_to_acp(iso, acp, sizeof acp);
            if (!why[0] && (iso_vfs_make_test_image(acp, xb, sizeof xb) != 0 ||
                            lcfg_image_check(iso, m2, sizeof m2) != 5 || strcmp(m, m2) != 0)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "LoC-ISO in SB-Karte '%.120s'", m2);
            }
            u8_DeleteFile(iso);
            lcfg_defaults(&s);
            s.game_image[0] = '\0';
            strcpy_s(s.media_root, sizeof s.media_root, dir);
            strcpy_s(s.xbe_path, sizeof s.xbe_path, p);
            if (!why[0] && (lcfg_validate(&s, m2, sizeof m2) == 0 || strcmp(m, m2) != 0)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "LoC Erweitert '%.120s'", m2);
            }
            /* kaputte .xbe: eigener Text statt "kein Xbox-Abbild (XDVDFS)" */
            st_write(p, "kein XBE", 8);
            if (!why[0] && (lcfg_image_check(p, m, sizeof m) != 2 || !strstr(m, "default.xbe") ||
                            strstr(m, "XDVDFS"))) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "kaputte xbe '%.120s'", m);
            }
            u8_DeleteFile(p);
            /* Ordner ohne default.xbe: unveraendert Ordner-Hinweis, kein Ordner-Modus */
            lcfg_defaults(&s);
            strcpy_s(s.game_image, sizeof s.game_image, dir);
            if (!why[0] && (lcfg_image_check(dir, m, sizeof m) != 1 || lcfg_image_folder_mode(&s) ||
                            strcmp(s.game_image, dir) != 0)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Ordner ohne xbe '%.120s'", m);
            }
        }
    }
    sb_lang_set(keep);
    if (!why[0]) {
        printf("PASS W369 xbe-pick: .xbe/Spielordner in der SB-Karte = Ordner-Modus (Datei, Ordner, Trenner, validate); LoC in SB-Karte (xbe/Ordner/ISO/Erweitert) und SB in LoC-Karte -> Tab-Hinweis EN/DE; kaputte xbe, Ordner ohne xbe\n");
        (*pass)++;
    } else {
        printf("FAIL W369 xbe-pick: %s\n", why);
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
            SetEnvironmentVariableA("SB_TEST_LOC_READY", "0"); /* W353: Sperre erzwingen (intern sonst frei) */
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
        /* W353/W389: Community mit SB_LOC_PLAY (Standard seit 0.3.0) und intern frei; nur Notschalter OFF grau */
#if defined(SB_EDITION_COMMUNITY) && SB_EDITION_COMMUNITY && !(defined(SB_LOC_PLAY) && SB_LOC_PLAY)
        const int want = 0;
#else
        const int want = 1;
#endif
        SetEnvironmentVariableA("SB_TEST_LOC_READY", NULL);
        memset(&ok, 0, sizeof ok);
        ok.kind = LGAME_IMG_LOC;
        ok.title_id = LGAME_TID_LOC;
        if (lgame_loc_build_ready() != want || lgame_loc_client_ready() != want) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Freigabe der Ausgabe != %d", want);
        } else if (lgame_loc_start_gate(&ok, m, sizeof m) != !want ||
                   (!want && strcmp(m, LT("gui.loc_soon")) != 0)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Startsperre falsch '%s'", m);
        }
        if (!why[0]) { /* echtes (synthetisches) LoC-Abbild: erkannt, startbar nur mit Freigabe */
            st_xbe(xb, sizeof xb, LGAME_TID_LOC);
            _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\default.xbe", dir);
            st_write(p, xb, sizeof xb);
            if (lgame_loc_check(dir, &r) != LGAME_IMG_LOC || r.startable != want) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "LoC-Abbild Art %d startbar %d", r.kind, r.startable);
            }
            u8_DeleteFile(p);
        }
    }
    if (!why[0]) {
#if defined(SB_EDITION_COMMUNITY) && SB_EDITION_COMMUNITY && !(defined(SB_LOC_PLAY) && SB_LOC_PLAY)
        printf("PASS W292 loctab: Community-Fassung (Notschalter SB_LOC_PLAY=OFF) - LoC-Start grau (gesperrt, Meldung, LoC-Abbild erkannt aber nicht startbar)\n");
#elif defined(SB_EDITION_COMMUNITY) && SB_EDITION_COMMUNITY
        printf("PASS W389 loctab: Community-Fassung (SB_LOC_PLAY, Standard seit 0.3.0) - LoC-Start frei, LoC-Abbild startbar\n");
#else
        printf("PASS W353 loctab: interne Ausgabe - LoC-Start frei, LoC-Abbild startbar\n");
#endif
        (*pass)++;
    } else {
        printf("FAIL W292 loctab Community-LoC-Start grau: %s\n", why);
        (*fail)++;
    }
    /* --- W402 LAUNCHER-PERGAME: einmalige Uebernahme der gemeinsamen Werte in beide Spiele --- */
    {
        char lcf[1200], lb[8192];
        why[0] = '\0';
        lgame_cfg_path(cfg, LGAME_LOC, lcf, sizeof lcf);
        u8_DeleteFile(cfg);
        u8_DeleteFile(lcf);
        if (strcmp(lcf + strlen(lcf) - 12, "user_loc.cfg") != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Pfad '%s'", lcf);
        } else if (lgame_pergame_migrate(cfg, lcf) != 0 || st_read(lcf, lb, sizeof lb)) {
            strcpy_s(why, sizeof why, "neu (ohne user.cfg): etwas uebernommen/angelegt");
        }
        if (!why[0]) {
            static const char kOld[] = "# user.cfg - vom Launcher geschrieben\n[xbe]\nxbe_path=C:\\sb\\default.xbe\n"
                                       "[game]\ngame_image=D:\\sb.iso\nhdd_root=HDD\n[display]\nwidth=2560\nheight=1080\n"
                                       "[game]\ngame_tick=30\n[audio]\nvolume_music=30\n[render]\nshading=1\nlight_world=0.8\n"
                                       "update_check=1\n# launcher: chosen game (sb = Steel Battalion, loc = Line of Contact)\n"
                                       "launcher_game=loc\nlauncher_loc_image=D:\\loc.iso\nlauncher_loc_online=1\n"
                                       "# Line of Contact picture format (0 = 4:3 original, 1 = 16:9, 2 = 720p)\nloc_widescreen=1\n";
            st_write(cfg, kOld, sizeof kOld - 1);
            if (lgame_pergame_migrate(cfg, lcf) != 1) {
                strcpy_s(why, sizeof why, "alte user.cfg nicht uebernommen");
            } else if (!st_read(lcf, lb, sizeof lb) || !strstr(lb, "width=2560\n") || !strstr(lb, "volume_music=30\n") ||
                       !strstr(lb, "light_world=0.8\n") || !strstr(lb, "loc_widescreen=1\n")) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "LoC-Kopie unvollstaendig: %.120s", lb);
            } else if (strstr(lb, "xbe_path") || strstr(lb, "game_image") || strstr(lb, "hdd_root") ||
                       strstr(lb, "launcher_") || strstr(lb, "update_check") || strstr(lb, "# launcher:")) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "LoC-Kopie mit SB-/Launcher-Zeilen: %.160s", lb);
            } else if (!st_read(cfg, buf, sizeof buf) || !strstr(buf, "launcher_pergame=1\n") ||
                       !strstr(buf, "width=2560\n") || !strstr(buf, "xbe_path=") || !strstr(buf, "launcher_loc_online=1\n") ||
                       strstr(buf, "loc_widescreen") || strstr(buf, "# Line of Contact picture")) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "user.cfg nach Uebernahme: %.160s", buf);
            }
        }
        if (!why[0]) { /* idempotent: zweiter Lauf aendert nichts, auch nicht nach Aenderung in LoC */
            st_write(lcf, "width=1111\n", 11);
            if (lgame_pergame_migrate(cfg, lcf) != 0 || !st_read(lcf, lb, sizeof lb) || strcmp(lb, "width=1111\n") != 0) {
                strcpy_s(why, sizeof why, "zweiter Lauf hat erneut kopiert");
            }
            u8_DeleteFile(lcf); /* geloescht (Nutzer) -> Markierung gilt, keine neue Kopie */
            if (!why[0] && (lgame_pergame_migrate(cfg, lcf) != 0 || st_read(lcf, lb, sizeof lb))) {
                strcpy_s(why, sizeof why, "markiert, aber erneut kopiert");
            }
        }
        if (!why[0]) { /* vorhandene LoC-Datei ohne Markierung: nur markieren, nichts ueberschreiben */
            st_write(cfg, "width=800\n", 10);
            st_write(lcf, "width=1234\n", 11);
            if (lgame_pergame_migrate(cfg, lcf) != 1 || !st_read(lcf, lb, sizeof lb) || strcmp(lb, "width=1234\n") != 0 ||
                !lgame_pergame_marked(cfg)) {
                strcpy_s(why, sizeof why, "vorhandene user_loc.cfg ueberschrieben bzw. nicht markiert");
            }
        }
        if (!why[0]) {
            char o2[300];
            lgame_cfg_path("C:\\x\\w402_t.cfg", LGAME_LOC, o2, sizeof o2);
            if (strcmp(o2, "C:\\x\\w402_t_loc.cfg") != 0) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Testname '%s'", o2);
            }
            lgame_cfg_path("C:\\x\\user.cfg", LGAME_SB, o2, sizeof o2);
            if (!why[0] && strcmp(o2, "C:\\x\\user.cfg") != 0) {
                strcpy_s(why, sizeof why, "SB-Pfad veraendert");
            }
        }
        if (!why[0]) {
            printf("PASS W402 pergame: Uebernahme alter gemeinsamer Werte einmalig in beide Spiele (Kopie ohne SB-Pfade/launcher_*/"
                   "Update, Markierung launcher_pergame=1, Bildformat nur LoC), idempotent, vorhandene LoC-Datei bleibt, "
                   "neu ohne Kopie\n");
            (*pass)++;
        } else {
            printf("FAIL W402 pergame: %s\n", why);
            (*fail)++;
        }
        u8_DeleteFile(lcf);
    }
    u8_DeleteFile(cfg);
    {
        wchar_t w[1200];
        u8_to_w(dir, w, 1200);
        RemoveDirectoryW(w);
    }
}
