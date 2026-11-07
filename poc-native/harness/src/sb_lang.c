/* sb_lang.c - W142 L10N-EN: Sprachdatei-Lader (Launcher + Client).
 * Format lang/<code>.lang: UTF-8, je Zeile schluessel=Text, '#' am
 * Zeilenanfang = Kommentar, '\\n'/'\\t'/'\\r'/'\\\\' = Escape. Der Wert
 * beginnt direkt nach dem ersten '=' (fuehrende Leerzeichen bleiben:
 * eingerueckte Overlay-Zeilen); nur CR/LF am Ende wird entfernt.
 * Englisch ist eingebaut (sb_lang_en.inc, aus en.lang erzeugt). */
#include "sb_lang.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "sb_lang_en.inc" /* kSbLangEn[], kSbLangEnN (sortiert) */

/* ---- Fremdsprache (de.lang) ---- */
#define LANG_FILE_MAX (192 * 1024)
#define LANG_ENT_MAX 2048
typedef struct {
    const char *key;
    const char *val;
} lang_ent_t;
static char g_file[LANG_FILE_MAX];
static lang_ent_t g_ent[LANG_ENT_MAX];
static const lang_ent_t *g_idx[LANG_ENT_MAX];
static unsigned g_ent_n;
static char g_code[8] = "en";
static char g_dir[1024];
static void (*g_warn)(const char *key);

/* Einmal-Warnung: Ring der letzten Schluessel-Hashes. */
#define WARN_RING 64
static unsigned g_warned[WARN_RING];
static unsigned g_warned_n;

static unsigned hash_key(const char *s)
{
    unsigned h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h ? h : 1;
}

static void warn_once(const char *key)
{
    unsigned h = hash_key(key), i;
    for (i = 0; i < g_warned_n && i < WARN_RING; i++) {
        if (g_warned[i] == h) {
            return;
        }
    }
    g_warned[g_warned_n % WARN_RING] = h;
    g_warned_n++;
    if (g_warn) {
        g_warn(key);
    }
}

static int ent_cmp(const void *a, const void *b)
{
    const lang_ent_t *ea = *(const lang_ent_t *const *)a;
    const lang_ent_t *eb = *(const lang_ent_t *const *)b;
    return strcmp(ea->key, eb->key);
}

static const char *search_en(const char *key)
{
    /* kSbLangEn ist sortiert (lang_gen.py): binaer suchen. */
    unsigned lo = 0, hi = kSbLangEnN;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        int c = strcmp(key, kSbLangEn[mid].key);
        if (c == 0) {
            return kSbLangEn[mid].val;
        }
        if (c < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return NULL;
}

static const char *search_de(const char *key)
{
    unsigned lo = 0, hi = g_ent_n;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        int c = strcmp(key, g_idx[mid]->key);
        if (c == 0) {
            return g_idx[mid]->val;
        }
        if (c < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return NULL;
}

/* Escapes in-place aufloesen (wird kuerzer, nie laenger). */
static void unescape(char *s)
{
    char *w = s;
    while (*s) {
        if (s[0] == '\\' && (s[1] == 'n' || s[1] == 't' || s[1] == 'r' || s[1] == '\\')) {
            *w++ = s[1] == 'n' ? '\n' : s[1] == 't' ? '\t' : s[1] == 'r' ? '\r' : '\\';
            s += 2;
        } else {
            *w++ = *s++;
        }
    }
    *w = '\0';
}

static void parse_file(void)
{
    char *p = g_file, *end;
    unsigned i;
    g_ent_n = 0;
    end = g_file + strlen(g_file);
    while (p < end && g_ent_n < LANG_ENT_MAX) {
        char *nl = strchr(p, '\n');
        char *key, *val, *t;
        if (nl) {
            *nl = '\0';
        }
        /* CR am Ende weg, fuehrende Leer zur Kommentar-/Leer-Erkennung weg. */
        t = p + strlen(p);
        while (t > p && (t[-1] == '\r' || t[-1] == '\n')) {
            *--t = '\0';
        }
        key = p;
        while (*key == ' ' || *key == '\t') {
            key++;
        }
        if (*key && *key != '#' && (val = strchr(key, '=')) != NULL) {
            *val++ = '\0';
            /* Schluessel rechts trimmen. */
            t = val - 1;
            while (t > key && (t[-1] == ' ' || t[-1] == '\t')) {
                *--t = '\0';
            }
            if (*key) {
                unescape(val);
                g_ent[g_ent_n].key = key;
                g_ent[g_ent_n].val = val;
                g_ent_n++;
            }
        }
        p = nl ? nl + 1 : end;
    }
    for (i = 0; i < g_ent_n; i++) {
        g_idx[i] = &g_ent[i];
    }
    qsort((void *)g_idx, g_ent_n, sizeof g_idx[0], ent_cmp);
}

static void load_code(const char *code)
{
    char path[1150];
    FILE *f = NULL;
    size_t n = 0;
    g_ent_n = 0;
    if (!code || !*code || strcmp(code, "en") == 0) {
        return; /* Englisch ist eingebaut, keine Datei noetig. */
    }
    if (!g_dir[0]) {
        return;
    }
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s/%s.lang", g_dir, code);
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        return; /* fehlt: still Englisch (Fallback je Schluessel). */
    }
    n = fread(g_file, 1, sizeof g_file - 1, f);
    fclose(f);
    g_file[n] = '\0';
    /* BOM weg. */
    if (n >= 3 && (unsigned char)g_file[0] == 0xEF && (unsigned char)g_file[1] == 0xBB &&
        (unsigned char)g_file[2] == 0xBF) {
        memmove(g_file, g_file + 3, n - 2);
    }
    parse_file();
}

void sb_lang_init(const char *lang_dir, const char *code)
{
    if (lang_dir && *lang_dir) {
        strncpy_s(g_dir, sizeof g_dir, lang_dir, _TRUNCATE);
    } else {
        g_dir[0] = '\0';
    }
    sb_lang_set(code);
}

void sb_lang_set(const char *code)
{
    if (!code || !*code || (strcmp(code, "en") != 0 && strcmp(code, "de") != 0)) {
        code = "en";
    }
    if (strcmp(code, g_code) == 0 && (g_ent_n > 0 || strcmp(code, "en") == 0)) {
        return;
    }
    strcpy_s(g_code, sizeof g_code, code);
    load_code(code);
}

const char *sb_lang_code(void)
{
    return g_code;
}

const char *sb_lang(const char *key)
{
    const char *v;
    if (!key || !*key) {
        return "";
    }
    if (strcmp(g_code, "en") != 0) {
        v = search_de(key);
        if (v) {
            return v;
        }
    }
    v = search_en(key);
    if (v) {
        return v;
    }
    warn_once(key);
    return key;
}

int sb_lang_w(const char *key, wchar_t *out, int cap)
{
    const char *u8 = sb_lang(key);
    int n;
    if (!out || cap <= 0) {
        return 1;
    }
    out[0] = 0;
    n = MultiByteToWideChar(CP_UTF8, 0, u8, -1, out, cap);
    if (n <= 0) { /* nie leer zurueck: ASCII-Anteil */
        size_t i;
        for (i = 0; u8[i] && i + 1 < (size_t)cap; i++) {
            out[i] = (wchar_t)(unsigned char)u8[i];
        }
        out[i] = 0;
        return 1;
    }
    return 0;
}

void sb_lang_set_warn(void (*cb)(const char *key))
{
    g_warn = cb;
}
