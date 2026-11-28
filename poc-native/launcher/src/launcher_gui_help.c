/* launcher_gui_help.c - Hilfeseiten: Tastenuebersicht HTML (W179), Controller-SVG (W182), Zadig-Anleitung (W185).
 * W462 LAUNCHER-SPLIT: aus launcher_gui.c verschoben (unveraendert), gemeinsamer Zustand
 * in launcher_gui_int.h. */
#include "launcher_gui_int.h"
/* W179 KEYS-HTML: Tastenuebersicht als lokale HTML-Datei              */
/* ------------------------------------------------------------------ */
/* Gleiche Quelle wie update_ctrl_overview (keine doppelte Pflege):
 * Profilname aus der UI, Profiltext aus profiles/<name>.ini, sonst das
 * Memo LT("gui.keys_memo"); Maus-Modus/Empfindlichkeit, Gamepad- und
 * Controller-Status live. Eine Datei mit eingebettetem CSS, keine
 * externen Verweise, Sprache = Launcher-Sprache, druckbar (hell).
 * Quelltext ASCII (U8_*), Ausgabe UTF-8. */

/* Abschnitte des Memos (feste Reihenfolge im HTML). */
enum { KH_MENUS, KH_START, KH_COMBAT, KH_MOVE, KH_COCKPIT, KH_MOUSEX,
       KH_N };

typedef struct {
    int sec;
    char key[96];
    char act[320];
} kh_row_t;

#define KH_MAXROWS 96
static kh_row_t kh_rows[KH_MAXROWS];
static int kh_n;
static char kh_footer[2048];

void keyshtml_escape(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    if (!s) {
        s = "";
    }
    if (!out || cap < 1) {
        return;
    }
    while (*s && o + 1 < cap) {
        const char *rep = NULL;
        size_t rl = 0;
        switch (*s) {
        case '&': rep = "&amp;"; rl = 5; break;
        case '<': rep = "&lt;"; rl = 4; break;
        case '>': rep = "&gt;"; rl = 4; break;
        case '"': rep = "&quot;"; rl = 6; break;
        default: break;
        }
        if (rep) {
            if (o + rl >= cap) {
                break;
            }
            memcpy(out + o, rep, rl);
            o += rl;
        } else {
            out[o++] = *s;
        }
        s++;
    }
    out[o] = '\0';
}

/* Anhaengen (still gekuerzt bei Platzmangel). */
static void keyshtml_add(char *out, size_t cap, size_t *o, const char *s)
{
    size_t n;
    if (!out || !o || !s || *o + 1 >= cap) {
        return;
    }
    n = strlen(s);
    if (*o + n >= cap) {
        n = cap - *o - 1;
    }
    memcpy(out + *o, s, n);
    *o += n;
    out[*o] = '\0';
}

static void keyshtml_addf(char *out, size_t cap, size_t *o, const char *fmt,
                          int v)
{
    char b[64];
    _snprintf_s(b, sizeof b, _TRUNCATE, fmt, v);
    keyshtml_add(out, cap, o, b);
}

/* Primaerfeuer-Zeile? (Memo: "primary fire" / "Primaerfeuer"; die
 * Suffixe sind ASCII, daher ohne UTF-8-Fallunterscheidung.) */
static int keyshtml_is_primary(const char *act)
{
    return act && (strstr(act, "mary fire") || strstr(act, "rfeuer")) != 0;
}

/* Cockpit-Zeile im Kampf-Block? (Chaff/Override, Nacht/FSS, Zoom, Mode;
 * Woerter in beiden Sprachen gleich.) */
static int keyshtml_is_cockpit(const char *act)
{
    return act && (strstr(act, "Chaff") || strstr(act, "FSS") ||
                   strstr(act, "Zoom") || strstr(act, "Mode")) != 0;
}

static char *keyshtml_trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) {
        *--e = '\0';
    }
    return s;
}

/* Ein Keycap (Inhalt schon escapen). */
static void keyshtml_cap(char *out, size_t cap, size_t *o, const char *t)
{
    char e[256];
    keyshtml_escape(t, e, sizeof e);
    keyshtml_add(out, cap, o, "<kbd>");
    keyshtml_add(out, cap, o, e);
    keyshtml_add(out, cap, o, "</kbd>");
}

/* Nur Einzelzeichen, je durch genau ein Leerzeichen getrennt?
 * ("1 2 3" ja, "Mouse left" nein.) */
static int keyshtml_all_single(const char *t)
{
    int n = 0;
    const char *p = t ? t : "";
    if (!*p) {
        return 0;
    }
    for (;;) {
        const char *sp = strchr(p, ' ');
        size_t len = sp ? (size_t)(sp - p) : strlen(p);
        if (len != 1) {
            return 0;
        }
        n++;
        if (!sp) {
            break;
        }
        p = sp + 1;
        if (!*p) {
            return 0;
        }
    }
    return n >= 2;
}

/* Taste(n) als Keycaps: "A / B" -> je ein Cap, "1 2 3" (nur
 * Einzelzeichen) -> je Zeichen ein Cap, sonst ein Cap. */
static void keyshtml_keys(char *out, size_t cap, size_t *o, const char *key)
{
    const char *p = key ? key : "";
    int first = 1;
    while (*p) {
        const char *sep = strstr(p, " / ");
        char part[128];
        size_t n = sep ? (size_t)(sep - p) : strlen(p);
        char *t, *q;
        int single = 1;
        if (n >= sizeof part) {
            n = sizeof part - 1;
        }
        memcpy(part, p, n);
        part[n] = '\0';
        t = keyshtml_trim(part);
        single = keyshtml_all_single(t);
        if (single) {
            /* "1 2 3 4 5": jedes Zeichen ein Cap */
            for (q = t; *q; q++) {
                if (*q != ' ') {
                    char c[2];
                    if (!first) {
                        keyshtml_add(out, cap, o, " ");
                    }
                    first = 0;
                    c[0] = *q;
                    c[1] = '\0';
                    keyshtml_cap(out, cap, o, c);
                }
            }
        } else if (*t) {
            char *plus;
            if (!first) {
                keyshtml_add(out, cap, o, " / ");
            }
            first = 0;
            /* "Shift+Arrows": je Summand ein Cap */
            while ((plus = strchr(t, '+')) != NULL) {
                *plus = '\0';
                keyshtml_cap(out, cap, o, keyshtml_trim(t));
                keyshtml_add(out, cap, o, "+");
                t = keyshtml_trim(plus + 1);
            }
            keyshtml_cap(out, cap, o, t);
        }
        if (!sep) {
            break;
        }
        p = sep + 3;
    }
}

static void keyshtml_row(char *out, size_t cap, size_t *o, const char *key,
                         const char *act)
{
    char ea[1600];
    if ((!key || !*key) && (!act || !*act)) {
        return;
    }
    keyshtml_add(out, cap, o,
                 keyshtml_is_primary(act) ? "<tr class=\"prim\">" : "<tr>");
    keyshtml_add(out, cap, o, "<td class=\"k\">");
    keyshtml_keys(out, cap, o, key);
    keyshtml_add(out, cap, o, "</td><td>");
    keyshtml_escape(act ? act : "", ea, sizeof ea);
    keyshtml_add(out, cap, o, ea);
    keyshtml_add(out, cap, o, "</td></tr>\n");
}

/* Memo in kh_rows/kh_footer zerlegen (gleiche Quelle wie das Memo). */
static void keyshtml_parse(const char *memo)
{
    static char work[16384];
    char *p;
    int sec = -1, blank = 0;
    kh_n = 0;
    kh_footer[0] = '\0';
    if (!memo) {
        return;
    }
    strncpy_s(work, sizeof work, memo, _TRUNCATE); /* W332 SEC: ueberlanges Memo kuerzen statt Abbruch */
    p = work;
    while (*p) {
        char *line = p, *tab;
        while (*p && *p != '\n' && *p != '\r') {
            p++;
        }
        if (*p == '\r') {
            *p++ = '\0';
            if (*p == '\n') {
                p++;
            }
        } else if (*p == '\n') {
            *p++ = '\0';
        }
        tab = strchr(line, '\t');
        if (!*keyshtml_trim(line)) {
            blank = 1;
            continue;
        }
        if (!tab) {
            /* Kopf oder Fussnote (Fuss: lang oder mit EJECT). */
            if (strstr(line, "EJECT") || strlen(line) > 100) {
                size_t fl = strlen(kh_footer);
                if (fl + strlen(line) + 2 < sizeof kh_footer) {
                    if (fl) {
                        strcpy_s(kh_footer + fl, sizeof kh_footer - fl, " ");
                        fl++;
                    }
                    strcpy_s(kh_footer + fl, sizeof kh_footer - fl, line);
                }
            } else if (strncmp(line, "MEN", 3) == 0) {
                sec = KH_MENUS;
            } else if (strstr(line, "MECH")) {
                sec = KH_START;
            } else if (strncmp(line, "COMBAT", 6) == 0 ||
                       strncmp(line, "KAMPF", 5) == 0) {
                sec = KH_COMBAT;
            } else if (strncmp(line, "DRIVING", 7) == 0 ||
                       strncmp(line, "FAHREN", 6) == 0) {
                sec = KH_MOVE;
            } else {
                size_t fl = strlen(kh_footer);
                if (fl + strlen(line) + 2 < sizeof kh_footer) {
                    if (fl) {
                        strcpy_s(kh_footer + fl, sizeof kh_footer - fl, " ");
                        fl++;
                    }
                    strcpy_s(kh_footer + fl, sizeof kh_footer - fl, line);
                }
            }
            blank = 0;
            continue;
        }
        *tab = '\0';
        /* W332 SEC: Tasten-/Aktionstext laenger als das Feld (Sprachdatei) -> Zeile verwerfen statt
         * strcpy_s-Abbruch (Fuzz-Fund keyshtml). */
        if (kh_n < KH_MAXROWS && strlen(keyshtml_trim(line)) < sizeof kh_rows[0].key &&
            strlen(keyshtml_trim(tab + 1)) < sizeof kh_rows[0].act) {
            int s = sec;
            if (blank) {
                s = KH_MOUSEX; /* kopflose Zeile (F10) -> Maus */
            } else if (s == KH_COMBAT &&
                       keyshtml_is_cockpit(keyshtml_trim(tab + 1))) {
                s = KH_COCKPIT;
            }
            if (s < 0) {
                s = KH_MOUSEX;
            }
            kh_rows[kh_n].sec = s;
            strcpy_s(kh_rows[kh_n].key, sizeof kh_rows[kh_n].key,
                     keyshtml_trim(line));
            strcpy_s(kh_rows[kh_n].act, sizeof kh_rows[kh_n].act,
                     keyshtml_trim(tab + 1));
            kh_n++;
        }
        blank = 0;
    }
}

/* Abschnitt mit Tabelle (nichts bei null Zeilen). */
static void keyshtml_table(char *out, size_t cap, size_t *o, const char *h,
                           int sec)
{
    int i, n = 0;
    for (i = 0; i < kh_n; i++) {
        n += kh_rows[i].sec == sec;
    }
    if (!n) {
        return;
    }
    keyshtml_add(out, cap, o, "<section><h2>");
    keyshtml_add(out, cap, o, h);
    keyshtml_add(out, cap, o, "</h2><table>\n");
    for (i = 0; i < kh_n; i++) {
        if (kh_rows[i].sec == sec) {
            keyshtml_row(out, cap, o, kh_rows[i].key, kh_rows[i].act);
        }
    }
    keyshtml_add(out, cap, o, "</table></section>\n");
}

/* Eingebettetes CSS (Launcher-Farbwelt, Keycaps, zwei Spalten,
 * Druck hell). Keine externen Verweise. */
static void keyshtml_css(char *out, size_t cap, size_t *o)
{
    static const char *const css =
        "*{box-sizing:border-box}\n"
        "body{margin:0;background:#0b0d11;color:#eaedf2;"
        "font-family:system-ui,\"Segoe UI\",SegoeUI,Roboto,Arial,sans-serif;"
        "line-height:1.5}\n"
        ".wrap{max-width:1240px;margin:0 auto;padding:24px 20px 40px}\n"
        "h1{font-size:26px;margin:0 0 4px;letter-spacing:.5px}\n"
        ".sub{color:#98a0ac;margin:0 0 16px;font-size:14px}\n"
        ".prim{background:#2a1f10;border:1px solid #e2963a;"
        "border-left-width:6px;border-radius:8px;padding:10px 14px;"
        "margin:0 0 20px}\n"
        ".prim-h{color:#f2a84e;font-size:12px;letter-spacing:1.5px;"
        "font-weight:700}\n"
        ".prim-k{font-size:17px;font-weight:600}\n"
        ".grid{display:grid;gap:14px;grid-template-columns:1fr}\n"
        "@media(min-width:900px){.grid{grid-template-columns:1fr 1fr}}\n"
        "section{background:#15181e;border:1px solid #2a2f3a;"
        "border-radius:10px;padding:12px 14px;break-inside:avoid}\n"
        "section.full{grid-column:1/-1}\n"
        "h2{font-size:15px;margin:0 0 8px;color:#f2a84e;letter-spacing:.4px}\n"
        "p{margin:0 0 8px;font-size:14px;color:#c9cfd9}\n"
        "table{width:100%;border-collapse:collapse;font-size:13.5px}\n"
        "td{padding:4px 6px;vertical-align:top;border-top:1px solid #242932}\n"
        "tr:first-child td{border-top:0}\n"
        "td.k{white-space:nowrap;width:38%}\n"
        "tr.prim td{background:#2a1f10}\n"
        "kbd{display:inline-block;min-width:22px;text-align:center;"
        "background:#1e222a;border:1px solid #3e4552;border-bottom-width:2px;"
        "border-radius:6px;padding:1px 7px;margin:1px 1px;"
        "font-family:Consolas,\"Courier New\",monospace;font-size:12.5px;"
        "color:#fff}\n"
        "pre{background:#0e1116;border:1px solid #2a2f3a;border-radius:8px;"
        "padding:10px 12px;font-size:12.5px;overflow-x:auto;"
        "white-space:pre-wrap;word-break:break-word;color:#d6dbe3}\n"
        "footer{margin-top:18px;color:#6c7480;font-size:12.5px}\n"
        "@media print{\n"
        "body{background:#fff;color:#111}\n"
        ".wrap{max-width:none}\n"
        ".prim{background:#fff7ec;border-color:#b26a1b}\n"
        ".prim-h{color:#8a4d00}\n"
        "section{background:#fff;border-color:#bbb}\n"
        "h2{color:#8a4d00}\n"
        "p{color:#333}\n"
        "td{border-top-color:#ddd}\n"
        "tr.prim td{background:#fff3e2}\n"
        "kbd{background:#f2f2f2;border-color:#999;color:#000}\n"
        "pre{background:#f7f7f7;border-color:#bbb;color:#222}\n"
        "footer{color:#555}\n"
        "}\n";
    keyshtml_add(out, cap, o, "<style>\n");
    keyshtml_add(out, cap, o, css);
    keyshtml_add(out, cap, o, "</style>\n");
}

/* ------------------------------------------------------------------ */
/* W182 KEYS-SVG: Original-Controller als schematische SVG-Zeichnung    */
/* ------------------------------------------------------------------ */
/* Eigene Zeichnung (keine Fotos, keine fremden Grafiken, keine Logos),
 * inline im HTML, ohne Verweise. Elemente/Bezeichnungen nach
 * CONTROLLER.md S8 (Labels bind.*). Belegung je Element aus derselben
 * Quelle wie W179: aktives Profil (Profiltext, lctrl_bind-Parser wie
 * der Belegungs-Dialog); Standardbelegung = eingebautes keyboard_default
 * (bitgenau Legacy; Selbsttest 54 vergleicht mit
 * harness/profiles/keyboard_default.ini). Tasten 1-5 rasten die fuenf
 * Kippschalter immer (Launcher-Start key_toggles=1, imap_toggles_apply).
 * Hover: CSS (:hover/:has), kein Skript. Quelltext ASCII, Ausgabe UTF-8. */

#define KS_LARR "\xE2\x86\x90"  /* Pfeil links */
#define KS_UARR "\xE2\x86\x91"  /* Pfeil hoch */
#define KS_RARR "\xE2\x86\x92"  /* Pfeil rechts */
#define KS_DARR "\xE2\x86\x93"  /* Pfeil runter */
#define KS_HARR "\xE2\x86\x94"  /* links-rechts */
#define KS_VARR "\xE2\x86\x95"  /* hoch-runter */
#define KS_TRI_U "\xE2\x96\xB2" /* Dreieck hoch */
#define KS_TRI_D "\xE2\x96\xBC" /* Dreieck runter */
#define KS_TRI_L "\xE2\x97\x80" /* Dreieck links */
#define KS_TRI_R "\xE2\x96\xB6" /* Dreieck rechts */
#define KS_EMDASH "\xE2\x80\x94" /* Geviertstrich = nicht belegt */


/* Formen der Zeichnung. KS_LEDR/G/B = Taste mit roter/gruener/blauer
 * Leuchtleiste (3x3-Raster im Mittelblock). */
/* Baugruppen (Reihenfolge der Tabelle). */


/* W184 KEYS-LAYOUT: physische Anordnung des Original-Controllers (eigene
 * Zeichnung; Lage der Bedienelemente nach oeffentlichen Beschreibungen
 * und einem Belegungsplan als Lagehinweis, nichts davon uebernommen).
 * Linker Block: Schaltkulisse 5..R mit Gang-Leuchten, Drehhebel in
 * waagrechter Kulisse mit Sicht-Stick (Daumen, druckbar) oben drauf,
 * Feld KIPPSCHALTER (2 + 3 schraeg versetzt). Mittelblock: COM1-5 oben
 * links, Tuner-Rad darunter, 3x3-Raster rechts (Leuchtleiste rot/gruen/
 * blau), unten Feld FUNKTION (FSS/Manipulator/Linienfarbe) und WAFFENWAHL
 * (Haupt/Sub/Magazin). Rechter Block: Zielhebel (Daumentaste Hauptwaffe +
 * Lock-On oben, Abzug vorn), Eject unter Schutzklappe oben rechts, Feld
 * MULTI-MONITOR (2x3, untere Reihe Hauptmonitor-Zoom), senkrechte Reihe
 * Luke/Zuendung/START. Pedale: Slide-Step, Bremse, Gas.
 * Zellen (lw = 0): Hover-Flaeche x-3,y-3,w+6,h+40; Name auf der Kappe,
 * Belegung darunter. Selbsttest 55 prueft Bloecke + Ueberlappung. */
#define KS_VIEWBOX "0 0 1320 964"
#define KS_CO_H 62 /* Hoehe Callout-Kasten */
/* Mittelblock: COM-Reihe (5 x 40, Schritt 46) und 3x3-Raster (72/78). */

#define KS_N ((int)(sizeof ks_elems / sizeof ks_elems[0]))

static const char *const ks_grp_key[KS_G_N] = {
    "gui.svg_left", "gui.svg_center", "gui.svg_right", "gui.svg_pedals",
};

/* Baugruppen-Rahmen x, y, w, h (Reihenfolge wie KS_G_*); Beschriftung
 * oben mittig. */
static const short ks_blk[KS_G_N][4] = {
    { 12, 30, 392, 610 }, { 414, 30, 492, 610 }, { 916, 30, 392, 610 },
    { 390, 660, 540, 292 },
};

/* Felder innerhalb der Bloecke: Rahmen (w = 0: nur Beschriftung) und
 * Beschriftung (lkey NULL: keine) an tx/ty. */
typedef struct {
    short x, y, w, h;
    const char *lkey;
    short tx, ty;
} ks_field_t;

static const ks_field_t ks_fields[] = {
    { 152, 330, 246, 296, "gui.svg_f_tog", 162, 348 },
    { 424, 426, 472, 194, "gui.svg_f_func", 436, 448 },
    { 0, 0, 0, 0, "gui.svg_f_wpn", 436, 532 },
    { 926, 356, 246, 262, "gui.svg_f_mm", 936, 374 },
    { 0, 0, 0, 0, "gui.svg_f_zoom", 936, 537 },
    { 1182, 356, 120, 262, NULL, 0, 0 },
};
#define KS_NF ((int)(sizeof ks_fields / sizeof ks_fields[0]))

/* Quellen je Callout-Zeile/Tabellenspalte. SBC-Quellen (W159) laufen
 * als "SBC:<Name>" in der Tastatur-Zeile mit. */
#define KS_TOKN 24

typedef struct {
    char t[KS_SRC_N][KS_TOKN][64];
    int n[KS_SRC_N];
} ks_toks_t;

static void ks_printf(char *out, size_t cap, size_t *o, const char *fmt, ...)
{
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(b, sizeof b, _TRUNCATE, fmt, ap);
    va_end(ap);
    keyshtml_add(out, cap, o, b);
}

/* Zeichen (Codepoints) eines UTF-8-Textes. */
static int ks_cps(const char *s)
{
    int n = 0;
    for (; s && *s; s++) {
        n += ((unsigned char)*s & 0xC0) != 0x80;
    }
    return n;
}

static void ks_tok_add(ks_toks_t *k, int src, const char *s)
{
    int i;
    if (src < 0 || src >= KS_SRC_N || !s || !*s) {
        return;
    }
    for (i = 0; i < k->n[src]; i++) {
        if (strcmp(k->t[src][i], s) == 0) {
            return; /* doppelt (z. B. Pfeile fuer X und Sicht) */
        }
    }
    if (k->n[src] < KS_TOKN) {
        strncpy_s(k->t[src][k->n[src]], sizeof k->t[src][0], s, _TRUNCATE);
        k->n[src]++;
    }
}

/* "LS <->" + "LS ^v" -> "LS" (beide Achsen derselben Quelle). */
static void ks_tok_merge(ks_toks_t *k)
{
    static const char hs[] = " " KS_HARR, vs[] = " " KS_VARR;
    int s, i, j;
    for (s = 0; s < KS_SRC_N; s++) {
        for (i = 0; i < k->n[s]; i++) {
            size_t n = strlen(k->t[s][i]);
            if (n <= sizeof hs - 1 ||
                strcmp(k->t[s][i] + n - (sizeof hs - 1), hs) != 0) {
                continue;
            }
            for (j = 0; j < k->n[s]; j++) {
                size_t m = strlen(k->t[s][j]);
                if (j == i || m != n ||
                    strncmp(k->t[s][j], k->t[s][i], n - (sizeof hs - 1)) ||
                    strcmp(k->t[s][j] + m - (sizeof vs - 1), vs) != 0) {
                    continue;
                }
                k->t[s][i][n - (sizeof hs - 1)] = '\0';
                memmove(k->t[s][j], k->t[s][j + 1],
                        (size_t)(k->n[s] - j - 1) * sizeof k->t[s][0]);
                k->n[s]--;
                if (j < i) {
                    i--;
                }
                break;
            }
        }
    }
}

/* Funktions-Praefix je Profil-Element (mehrteilige Elemente). */
static const char *ks_prefix(const char *key)
{
    static const struct { const char *k, *p; } t[] = {
        { "gear.up", KS_TRI_U }, { "gear.down", KS_TRI_D },
        { "gear.r", "R:" }, { "gear.n", "N:" }, { "gear.1", "1:" },
        { "gear.2", "2:" }, { "gear.3", "3:" }, { "gear.4", "4:" },
        { "gear.5", "5:" }, { "tuner.up", KS_TRI_U },
        { "tuner.down", KS_TRI_D }, { "rot.neg", KS_TRI_L },
        { "rot.pos", KS_TRI_R },
    };
    int i;
    for (i = 0; i < (int)(sizeof t / sizeof t[0]); i++) {
        if (strcmp(key, t[i].k) == 0) {
            return t[i].p;
        }
    }
    return "";
}

/* Eine Quelle ("KEY:LEFT:cond=!SHIFT", "PAD:LX:dz=0.15") lesbar machen;
 * *src = Gruppe. Parameter fallen weg (cond=SHIFT -> "Shift+"). */
void ks_atom(const char *a, char *out, size_t cap, int *src)
{
    static const struct { const char *n, *t; } keys[] = {
        { "LEFT", KS_LARR }, { "RIGHT", KS_RARR }, { "UP", KS_UARR },
        { "DOWN", KS_DARR }, { "LCTRL", "LCtrl" }, { "RCTRL", "RCtrl" },
        { "CTRL", "Ctrl" }, { "SHIFT", "Shift" }, { "ALT", "Alt" },
        { "ESC", "Esc" },
    }, pads[] = {
        { "LX", "LS " KS_HARR }, { "LY", "LS " KS_VARR },
        { "RX", "RS " KS_HARR }, { "RY", "RS " KS_VARR },
        { "DU", "D" KS_UARR }, { "DUP", "D" KS_UARR },
        { "DD", "D" KS_DARR }, { "DDOWN", "D" KS_DARR },
        { "DL", "D" KS_LARR }, { "DLEFT", "D" KS_LARR },
        { "DR", "D" KS_RARR }, { "DRIGHT", "D" KS_RARR },
    };
    char ty[16], nm[64];
    const char *c1, *c2, *par = "";
    size_t n;
    int i;
    *src = KS_SRC_KEY;
    out[0] = '\0';
    c1 = strchr(a, ':');
    if (!c1 || (size_t)(c1 - a) >= sizeof ty) {
        strncpy_s(out, cap, a, _TRUNCATE); /* unbekannte Form: roh */
        return;
    }
    memcpy(ty, a, (size_t)(c1 - a));
    ty[c1 - a] = '\0';
    c2 = strchr(c1 + 1, ':');
    n = c2 ? (size_t)(c2 - c1 - 1) : strlen(c1 + 1);
    if (n >= sizeof nm) {
        n = sizeof nm - 1;
    }
    memcpy(nm, c1 + 1, n);
    nm[n] = '\0';
    if (c2) {
        par = c2;
    }
    if (_stricmp(ty, "MOUSE") == 0) {
        static const struct { const char *n, *k; } ms[] = {
            { "LEFT", "gui.svg_m_left" }, { "RIGHT", "gui.svg_m_right" },
            { "MIDDLE", "gui.svg_m_middle" }, { "X1", "gui.svg_m_x1" },
            { "X2", "gui.svg_m_x2" },
            { "WHEEL_UP", "gui.svg_m_wup" }, { "WHEEL_DOWN", "gui.svg_m_wdown" }, /* W260 */
            { "WHEEL_LEFT", "gui.svg_m_wleft" }, { "WHEEL_RIGHT", "gui.svg_m_wright" },
        };
        *src = KS_SRC_MOUSE;
        if (_stricmp(nm, "X") == 0 || _stricmp(nm, "Y") == 0) {
            _snprintf_s(out, cap, _TRUNCATE, "%s %s", LT("gui.svg_mouse"),
                        _stricmp(nm, "X") == 0 ? KS_HARR : KS_VARR);
            return;
        }
        for (i = 0; i < (int)(sizeof ms / sizeof ms[0]); i++) {
            if (_stricmp(nm, ms[i].n) == 0) {
                strncpy_s(out, cap, LT(ms[i].k), _TRUNCATE);
                return;
            }
        }
        _snprintf_s(out, cap, _TRUNCATE, "%s %s", LT("gui.svg_mouse"), nm);
        return;
    }
    if (_stricmp(ty, "PAD") == 0) {
        *src = KS_SRC_PAD;
        for (i = 0; i < (int)(sizeof pads / sizeof pads[0]); i++) {
            if (_stricmp(nm, pads[i].n) == 0) {
                strncpy_s(out, cap, pads[i].t, _TRUNCATE);
                return;
            }
        }
        strncpy_s(out, cap, nm, _TRUNCATE);
        return;
    }
    if (_stricmp(ty, "KEY") == 0) {
        const char *t = nm;
        char kd[48];
        for (i = 0; i < (int)(sizeof keys / sizeof keys[0]); i++) {
            if (_stricmp(nm, keys[i].n) == 0) {
                t = keys[i].t;
                break;
            }
        }
        if (t == nm && nm[0] >= '0' && nm[0] <= '9' && nm[1]) {
            /* W365 BIND-KEYNAMES: numerisch gespeicherte Taste ("20") mit Namen ("Caps Lock"). */
            int vk = lctrl_key_vk(nm);
            if (vk >= 0) {
                lctrl_key_disp(vk, kd, sizeof kd);
                t = kd;
            }
        }
        _snprintf_s(out, cap, _TRUNCATE, "%s%s",
                    (strstr(par, "cond=SHIFT") || strstr(par, "cond=shift"))
                        ? "Shift+" : "", t);
        return;
    }
    if (_stricmp(ty, "SBC") == 0) {
        _snprintf_s(out, cap, _TRUNCATE, "SBC:%s", nm);
        return;
    }
    if (lctrl_joy_atom_text(a, out, cap)) { /* W221: "Joystick 1 Taste 3" */
        *src = KS_SRC_PAD;
        return;
    }
    strncpy_s(out, cap, a, _TRUNCATE);
}

/* Alternative (Akkord "A+B" erlaubt) -> Text, Gruppe des ersten Atoms. */
static void ks_alt(const char *alt, char *out, size_t cap, int *src)
{
    char work[128], *p, *plus;
    size_t o = 0;
    int first = 1;
    out[0] = '\0';
    *src = KS_SRC_KEY;
    strncpy_s(work, sizeof work, alt, _TRUNCATE);
    p = work;
    for (;;) {
        char t[64];
        int s;
        plus = (char *)lctrl_chord_plus(p); /* W226: "JOY0:Z+" */
        if (plus) {
            *plus = '\0';
        }
        ks_atom(keyshtml_trim(p), t, sizeof t, &s);
        if (first) {
            *src = s;
        } else {
            keyshtml_add(out, cap, &o, "+");
        }
        first = 0;
        keyshtml_add(out, cap, &o, t);
        if (!plus) {
            break;
        }
        p = plus + 1;
    }
}

/* Belegung eines gezeichneten Elements einsammeln. */
static void ks_collect(const lctrl_bind_t *b, const ks_elem_t *e,
                       ks_toks_t *k)
{
    char names[160], *nm, *ctx = NULL;
    memset(k, 0, sizeof *k);
    strncpy_s(names, sizeof names, e->elems, _TRUNCATE);
    for (nm = strtok_s(names, " ", &ctx); nm; nm = strtok_s(NULL, " ", &ctx)) {
        int idx = lctrl_bind_elem_index(nm), a;
        const char *pre = ks_prefix(nm);
        if (idx < 0) {
            continue;
        }
        for (a = 0; a < b->row[idx].n_alt; a++) {
            char t[64], u[80];
            int s;
            ks_alt(b->row[idx].alt[a], t, sizeof t, &s);
            _snprintf_s(u, sizeof u, _TRUNCATE, "%s%s", pre, t);
            ks_tok_add(k, s, u);
        }
    }
    /* Tasten 1-5 rasten die Kippschalter immer (key_toggles=1). */
    if (strncmp(e->id, "bi", 2) == 0) {
        int bi = atoi(e->id + 2);
        if (bi >= 34 && bi <= 38) {
            char t[4];
            t[0] = (char)('1' + bi - 34);
            t[1] = '\0';
            ks_tok_add(k, KS_SRC_KEY, t);
        }
    }
    ks_tok_merge(k);
}

static const char *ks_name(const ks_elem_t *e, int shortname)
{
    return LT(shortname && e->skey ? e->skey : e->lkey);
}

/* Eine Callout-Zeile (Quellen-Maske; leer = Geviertstrich). Zu lang ->
 * textLength (Schrift staucht, Kasten bleibt). */
static void ks_svg_line(char *out, size_t cap, size_t *o, int x, int y,
                        int mid, int avail, const ks_toks_t *k, int mask)
{
    static const char *const cls[KS_SRC_N] = { "tk", "tm", "tp" };
    char buf[8192];
    size_t bo = 0;
    int cps = 0, first = 1, s, i;
    buf[0] = '\0';
    for (s = 0; s < KS_SRC_N; s++) {
        if (!(mask & (1 << s))) {
            continue;
        }
        for (i = 0; i < k->n[s]; i++) {
            char e[256];
            if (!first) {
                keyshtml_add(buf, sizeof buf, &bo,
                             "<tspan class=\"sep\"> " U8_DOT " </tspan>");
                cps += 3;
            }
            first = 0;
            keyshtml_escape(k->t[s][i], e, sizeof e);
            ks_printf(buf, sizeof buf, &bo, "<tspan class=\"%s\">%s</tspan>",
                      cls[s], e);
            cps += ks_cps(k->t[s][i]);
        }
    }
    if (first) {
        keyshtml_add(buf, sizeof buf, &bo,
                     "<tspan class=\"un\">" KS_EMDASH "</tspan>");
        cps = 1;
    }
    ks_printf(out, cap, o, "<text class=\"cl\" x=\"%d\" y=\"%d\"%s", x, y,
              mid ? " text-anchor=\"middle\"" : "");
    if (cps * 70 > avail * 10) { /* Halbfett 12: ~7 je Zeichen */
        ks_printf(out, cap, o,
                  " textLength=\"%d\" lengthAdjust=\"spacingAndGlyphs\"",
                  avail);
    }
    keyshtml_add(out, cap, o, ">");
    keyshtml_add(out, cap, o, buf);
    keyshtml_add(out, cap, o, "</text>");
}

/* Name (proportional, ~6.6 je Zeichen bei 11.5). */
static void ks_svg_name(char *out, size_t cap, size_t *o, int x, int y,
                        int mid, int avail, const char *name)
{
    char e[512];
    keyshtml_escape(name, e, sizeof e);
    ks_printf(out, cap, o, "<text class=\"nm\" x=\"%d\" y=\"%d\"%s", x, y,
              mid ? " text-anchor=\"middle\"" : "");
    if (ks_cps(name) * 66 > avail * 10) {
        ks_printf(out, cap, o,
                  " textLength=\"%d\" lengthAdjust=\"spacingAndGlyphs\"",
                  avail);
    }
    keyshtml_add(out, cap, o, ">");
    keyshtml_add(out, cap, o, e);
    keyshtml_add(out, cap, o, "</text>");
}

/* Form zeichnen (Klassen: sh = hervorhebbare Flaeche). */
static void ks_svg_shape(char *out, size_t cap, size_t *o, const ks_elem_t *e)
{
    int x = e->x, y = e->y, w = e->w, h = e->h, i;
    switch (e->shape) {
    case KS_TOG:
        ks_printf(out, cap, o,
                  "<rect class=\"sh\" x=\"%d\" y=\"%d\" width=\"%d\" "
                  "height=\"%d\" rx=\"5\"/>"
                  "<rect class=\"ic\" x=\"%d\" y=\"%d\" width=\"9\" "
                  "height=\"16\" rx=\"3\"/>"
                  "<line class=\"ic\" x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\"/>",
                  x, y, w, h, x + 6, y + 4, x + 10, y + 12, x + 14, y + 3);
        break;
    case KS_LEDR: /* Taste mit Leuchtleiste (rot/gruen/blau) */
    case KS_LEDG:
    case KS_LEDB:
        ks_printf(out, cap, o,
                  "<rect class=\"sh\" x=\"%d\" y=\"%d\" width=\"%d\" "
                  "height=\"%d\" rx=\"5\"/>"
                  "<rect class=\"%s\" x=\"%d\" y=\"%d\" width=\"%d\" "
                  "height=\"3\" rx=\"1.5\"/>",
                  x, y, w, h,
                  e->shape == KS_LEDR ? "ldr"
                  : e->shape == KS_LEDG ? "ldg" : "ldb",
                  x + 8, y + 4, w - 16);
        break;
    case KS_EJECT: /* Schutzklappe mit Warnstreifen, Scharnier oben, Kappe */
        ks_printf(out, cap, o,
                  "<rect class=\"hzf\" x=\"%d\" y=\"%d\" width=\"%d\" "
                  "height=\"%d\" rx=\"6\"/>"
                  "<line class=\"cvh\" x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\"/>"
                  "<rect class=\"sh ej\" x=\"%d\" y=\"%d\" width=\"%d\" "
                  "height=\"%d\" rx=\"5\"/>",
                  x - 12, y - 12, w + 24, h + 24, x - 4, y - 12, x + w + 4,
                  y - 12, x, y, w, h);
        break;
    case KS_DIAL: { /* Tuner-Rad (Mitte x/y, Radius w): 13 Rasten von 9 Uhr
                     * im Uhrzeigersinn bis 6 Uhr (CONTROLLER.md S3.4) */
        double r1 = w + 5, r2 = w + 13;
        ks_printf(out, cap, o,
                  "<circle class=\"bs\" cx=\"%d\" cy=\"%d\" r=\"%d\"/>"
                  "<circle class=\"sh\" cx=\"%d\" cy=\"%d\" r=\"%d\"/>"
                  "<circle class=\"ic\" cx=\"%d\" cy=\"%d\" r=\"%d\"/>",
                  x, y, w + 16, x, y, w, x, y, w * 62 / 100);
        for (i = 0; i <= 12; i++) {
            double a = i * 3.14159265358979 / 8.0;
            double dx = -cos(a), dy = -sin(a);
            ks_printf(out, cap, o,
                      "<line class=\"ic\" x1=\"%d\" y1=\"%d\" x2=\"%d\" "
                      "y2=\"%d\"/>",
                      x + (int)lround(dx * r1), y + (int)lround(dy * r1),
                      x + (int)lround(dx * r2), y + (int)lround(dy * r2));
        }
        ks_printf(out, cap, o,
                  "<line class=\"ic\" x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\"/>",
                  x, y - w * 30 / 100, x, y - w * 90 / 100);
        break;
    }
    case KS_GEAR: { /* Kulisse senkrecht (oben 5 ... unten R), rechts davon
                     * die Gang-Leuchten mit Ziffern, Knauf auf N */
        static const char *const pos[7] = { "5", "4", "3", "2", "1", "N",
                                            "R" };
        int sx = x + 18, st = y + 14, sb = y + h - 14;
        ks_printf(out, cap, o,
                  "<rect class=\"sh\" x=\"%d\" y=\"%d\" width=\"%d\" "
                  "height=\"%d\" rx=\"12\"/>"
                  "<rect class=\"bs\" x=\"%d\" y=\"%d\" width=\"22\" "
                  "height=\"%d\" rx=\"11\"/>",
                  x, y, w, h, sx, st, sb - st);
        for (i = 0; i < 7; i++) {
            int py = st + 16 + i * (sb - st - 32) / 6;
            ks_printf(out, cap, o,
                      "<line class=\"ic\" x1=\"%d\" y1=\"%d\" x2=\"%d\" "
                      "y2=\"%d\"/>"
                      "<rect class=\"ic\" x=\"%d\" y=\"%d\" width=\"20\" "
                      "height=\"12\" rx=\"2\"/>"
                      "<text class=\"gl\" x=\"%d\" y=\"%d\">%s</text>",
                      sx + 26, py, sx + 34, py, x + 58, py - 6, x + 96,
                      py + 4, pos[i]);
            if (i == 5) {
                ks_printf(out, cap, o,
                          "<circle class=\"sh kn\" cx=\"%d\" cy=\"%d\" "
                          "r=\"12\"/>",
                          sx + 11, py);
            }
        }
        break;
    }
    case KS_ROT: { /* Drehhebel von oben: waagrechte Kulisse, Griffkopf
                    * in der Mitte (traegt den Sicht-Stick) */
        int cx = x + w / 2, cy = y + h / 2;
        ks_printf(out, cap, o,
                  "<rect class=\"bs\" x=\"%d\" y=\"%d\" width=\"%d\" "
                  "height=\"%d\" rx=\"%d\"/>"
                  "<text class=\"gl\" x=\"%d\" y=\"%d\">" KS_TRI_L "</text>"
                  "<text class=\"gl\" x=\"%d\" y=\"%d\">" KS_TRI_R "</text>"
                  "<rect class=\"sh\" x=\"%d\" y=\"%d\" width=\"64\" "
                  "height=\"46\" rx=\"14\"/>",
                  x, y, w, h, h / 2, x + 14, cy + 4, x + w - 14, cy + 4,
                  cx - 32, cy - 23);
        break;
    }
    case KS_LEVER: { /* Griff, Schaft, Sockel (Seitenansicht) */
        int cx = x + w / 2, gh = h * 55 / 100, sh = h * 36 / 100;
        ks_printf(out, cap, o,
                  "<ellipse class=\"bs\" cx=\"%d\" cy=\"%d\" rx=\"%d\" "
                  "ry=\"13\"/>"
                  "<rect class=\"bs\" x=\"%d\" y=\"%d\" width=\"16\" "
                  "height=\"%d\" rx=\"4\"/>"
                  "<rect class=\"sh\" x=\"%d\" y=\"%d\" width=\"%d\" "
                  "height=\"%d\" rx=\"%d\"/>",
                  cx, y + gh + sh + 6, w * 78 / 100, cx - 8, y + gh, sh,
                  x, y, w, gh, w * 38 / 100);
        break;
    }
    case KS_TRIG:
        ks_printf(out, cap, o,
                  "<rect class=\"sh\" x=\"%d\" y=\"%d\" width=\"%d\" "
                  "height=\"%d\" rx=\"%d\"/>",
                  x, y, w, h, w < h ? 6 : 4);
        break;
    case KS_KNOB:
    case KS_SBTN:
        ks_printf(out, cap, o,
                  "<circle class=\"sh\" cx=\"%d\" cy=\"%d\" r=\"%d\"/>", x, y,
                  w);
        break;
    case KS_SIGHT: /* Sicht-Stick von oben: Kopf mit vier Richtungsmarken */
        ks_printf(out, cap, o,
                  "<circle class=\"sh\" cx=\"%d\" cy=\"%d\" r=\"%d\"/>"
                  "<path class=\"ic\" d=\"M%d %dv4M%d %dv-4M%d %dh4M%d %dh-4\"/>",
                  x, y, w, x, y - w + 2, x, y + w - 2, x - w + 2, y,
                  x + w - 2, y);
        break;
    case KS_PEDAL:
        ks_printf(out, cap, o,
                  "<rect class=\"sh\" x=\"%d\" y=\"%d\" width=\"%d\" "
                  "height=\"%d\" rx=\"10\"/>",
                  x, y, w, h);
        for (i = 1; i <= 4; i++) {
            ks_printf(out, cap, o,
                      "<line class=\"ic\" x1=\"%d\" y1=\"%d\" x2=\"%d\" "
                      "y2=\"%d\"/>",
                      x + 10, y + i * h / 5, x + w - 10, y + i * h / 5);
        }
        break;
    default: /* KS_BTN */
        ks_printf(out, cap, o,
                  "<rect class=\"sh\" x=\"%d\" y=\"%d\" width=\"%d\" "
                  "height=\"%d\" rx=\"5\"/>",
                  x, y, w, h);
        break;
    }
}

/* Masken der belegten Zeilen (kompakt, leer = eine Strich-Zeile). */
static int ks_masks(const ks_toks_t *k, int cell, int m[3])
{
    int n = 0;
    if (cell) {
        if (k->n[KS_SRC_KEY] || k->n[KS_SRC_MOUSE]) {
            m[n++] = (1 << KS_SRC_KEY) | (1 << KS_SRC_MOUSE);
        }
        if (k->n[KS_SRC_PAD]) {
            m[n++] = 1 << KS_SRC_PAD;
        }
    } else {
        int s;
        for (s = 0; s < KS_SRC_N; s++) {
            if (k->n[s]) {
                m[n++] = 1 << s;
            }
        }
    }
    if (!n) {
        m[n++] = 0;
    }
    return n;
}

/* Hover-Kopplung Element <-> Callout <-> Tabellenzeile (nur CSS). */
static void ks_css(char *out, size_t cap, size_t *o)
{
    static const char *const css =
        ".ctl{margin:0 0 20px}\n"
        ".ctl svg{display:block;width:100%;height:auto;margin:6px 0 4px;"
        "font-family:system-ui,\"Segoe UI\",Roboto,Arial,sans-serif}\n"
        ".ctl .bd{fill:#15181e;stroke:#343a46;stroke-width:2}\n"
        ".ctl .bdc{fill:#6c7480;font-size:12px;font-weight:700;"
        "letter-spacing:2px;text-anchor:middle}\n"
        ".ctl .sh{fill:#1e222a;stroke:#4a5260;stroke-width:1.5}\n"
        ".ctl .bs{fill:#191c22;stroke:#3a404c;stroke-width:1.2}\n"
        ".ctl .ic{fill:none;stroke:#98a0ac;stroke-width:1.5;"
        "stroke-linecap:round}\n"
        ".ctl .sl{fill:none;stroke:#5a6270;stroke-width:5;"
        "stroke-linecap:round}\n"
        ".ctl .kn{fill:#2b303a}\n"
        ".ctl .gl{fill:#98a0ac;font-size:12px;font-weight:700;"
        "text-anchor:middle}\n"
        ".ctl .ej{fill:#4a1a1a;stroke:#d9534f}\n"
        ".ctl .cv{fill:none;stroke:#d9534f;stroke-width:1.2;"
        "stroke-dasharray:4 3}\n"
        ".ctl .cvh{stroke:#d9534f;stroke-width:3;stroke-linecap:round}\n"
        ".ctl .hzf{fill:url(#ks-hz);stroke:#d9534f;stroke-width:1.2}\n"
        ".ctl .hz{fill:#7a6418}\n.ctl .hzb{fill:#1a1c20}\n"
        ".ctl .ldr{fill:#e0524c}\n.ctl .ldg{fill:#5cc15a}\n"
        ".ctl .ldb{fill:#3d9be0}\n"
        ".ctl .fd{fill:none;stroke:#3a414d;stroke-width:1.2}\n"
        ".ctl .fdc{fill:#7d8592;font-size:10px;font-weight:700;"
        "letter-spacing:1.5px}\n"
        ".ctl .lb{fill:#11141a;stroke:#2f3541;stroke-width:1}\n"
        ".ctl .lbc{fill:transparent;stroke:none}\n"
        ".ctl .ld{fill:none;stroke:#6c7480;stroke-width:1;"
        "stroke-dasharray:3 3}\n"
        ".ctl .nm{fill:#eaedf2;font-size:11.5px;font-weight:600}\n"
        ".ctl .cl{font-size:12px;font-weight:600}\n"
        ".ctl svg .tk{fill:#ffffff}\n.ctl svg .tm{fill:#7cc4ff}\n"
        ".ctl svg .tp{fill:#8fdc8a}\n"
        ".ctl svg .un,.ctl svg .sep{fill:#5c6470}\n"
        ".ctl .pf .sh{fill:#3a2810;stroke:#e2963a;stroke-width:2}\n"
        ".ctl .pf .lb{stroke:#e2963a}\n"
        ".ctl .el:hover .sh{stroke:#f2a84e;stroke-width:2.6}\n"
        ".ctl .el:hover .lb{stroke:#f2a84e;fill:#2a1f10}\n"
        ".lg{display:flex;flex-wrap:wrap;gap:6px 18px;margin:0 0 6px;"
        "font-size:13px;color:#c9cfd9}\n"
        ".lg .sw{display:inline-block;width:12px;height:12px;"
        "border-radius:3px;margin-right:6px;vertical-align:-1px;"
        "border:1px solid #3e4552}\n"
        ".sw.kb{background:#fff}.sw.ms{background:#7cc4ff}"
        ".sw.pd{background:#8fdc8a}.sw.pf{background:#e2963a}"
        ".sw.un{background:#5c6470}\n"
        ".note{font-size:12.5px;color:#98a0ac}\n"
        "table.et th{text-align:left;font-size:12px;color:#98a0ac;"
        "padding:4px 6px;font-weight:600}\n"
        "table.et tr.grp th{color:#f2a84e;padding-top:10px;"
        "letter-spacing:.4px}\n"
        "table.et td:first-child{width:30%}\n"
        "table.et .tm,table.et .tp{display:inline-block;margin:1px 2px;"
        "padding:0 6px;border-radius:6px;font-size:12.5px;"
        "font-family:Consolas,\"Courier New\",monospace}\n"
        "table.et .tm{color:#7cc4ff;border:1px solid #2c4a66}\n"
        "table.et .tp{color:#8fdc8a;border:1px solid #2f5230}\n"
        "table.et .un{color:#5c6470}\n"
        "table.et tr:hover td{background:#1f2430}\n"
        "@media print{\n"
        ".ctl svg{break-inside:avoid}\n"
        ".ctl .bd{fill:#f6f6f6;stroke:#999}\n.ctl .bdc{fill:#555}\n"
        ".ctl .sh{fill:#fff;stroke:#444}\n.ctl .bs{fill:#eee;stroke:#888}\n"
        ".ctl .ic{stroke:#555}\n.ctl .sl{stroke:#888}\n.ctl .kn{fill:#ddd}\n"
        ".ctl .gl{fill:#333}\n"
        ".ctl .ej{fill:#fde8e8;stroke:#b22}\n.ctl .cv,.ctl .cvh{stroke:#b22}\n"
        ".ctl .hzf{stroke:#b22}\n.ctl .hz{fill:#e8c440}\n"
        ".ctl .hzb{fill:#fff}\n"
        ".ctl .ldr{fill:#c62828}\n.ctl .ldg{fill:#2e7d32}\n"
        ".ctl .ldb{fill:#1565c0}\n"
        ".ctl .fd{stroke:#aaa}\n.ctl .fdc{fill:#555}\n"
        ".ctl .lb{fill:#fff;stroke:#bbb}\n.ctl .lbc{fill:none;stroke:none}\n"
        ".ctl .ld{stroke:#999}\n.ctl .nm{fill:#111}\n"
        ".ctl svg .tk{fill:#000}\n.ctl svg .tm{fill:#0a58a8}\n"
        ".ctl svg .tp{fill:#1d7a1d}\n"
        ".ctl svg .un,.ctl svg .sep{fill:#888}\n"
        ".ctl .pf .sh{fill:#fff3e2;stroke:#b26a1b}\n"
        ".ctl .pf .lb{stroke:#b26a1b}\n"
        ".lg{color:#333}\n.note{color:#555}\n"
        "table.et .tm{color:#0a58a8;border-color:#9bb8d6}\n"
        "table.et .tp{color:#1d7a1d;border-color:#a8cfa6}\n"
        "table.et tr.grp th{color:#8a4d00}\n"
        "}\n";
    int i, r;
    keyshtml_add(out, cap, o, "<style>\n");
    keyshtml_add(out, cap, o, css);
    /* Kopplung per :has (Element/Zeile ueberfahren -> beide markiert). */
    for (r = 0; r < 3; r++) {
        for (i = 0; i < KS_N; i++) {
            ks_printf(out, cap, o, "%sbody:has(.e-%s:hover) .e-%s%s",
                      i ? "," : "", ks_elems[i].id, ks_elems[i].id,
                      r == 0 ? " .sh" : r == 1 ? " .lb" : " td");
        }
        keyshtml_add(out, cap, o,
                     r == 0 ? "{stroke:#f2a84e;stroke-width:2.6}\n"
                     : r == 1 ? "{stroke:#f2a84e;fill:#2a1f10}\n"
                              : "{background:#2a1f10}\n");
    }
    keyshtml_add(out, cap, o, "</style>\n");
}

/* Tokens einer Quelle als Tabellenzelle. */
static void ks_td(char *out, size_t cap, size_t *o, const ks_toks_t *k,
                  int s)
{
    int i;
    keyshtml_add(out, cap, o, "<td>");
    if (!k->n[s]) {
        keyshtml_add(out, cap, o, "<span class=\"un\">" KS_EMDASH "</span>");
    }
    for (i = 0; i < k->n[s]; i++) {
        if (s == KS_SRC_KEY) {
            if (i) {
                keyshtml_add(out, cap, o, " ");
            }
            keyshtml_cap(out, cap, o, k->t[s][i]);
        } else {
            char e[256];
            keyshtml_escape(k->t[s][i], e, sizeof e);
            ks_printf(out, cap, o, "<span class=\"%s\">%s</span>",
                      s == KS_SRC_MOUSE ? "tm" : "tp", e);
        }
    }
    keyshtml_add(out, cap, o, "</td>");
}

/* Abschnitt "Controller und Belegung": Legende, SVG, Hinweis, Tabelle.
 * prof_text NULL bei Standardbelegung (has_prof = 0) oder fehlendem
 * Profil (dann keyboard_default eingebaut, sonst alles unbelegt). */
static void keysvg_build(const char *profile, const char *prof_text,
                         char *out, size_t cap, size_t *o)
{
    /* W535 AVVT: Arbeitsspeicher (~250 KB) vom Heap statt statisch (.bss des Release-Launchers verkleinert) */
    lctrl_bind_t *b = (lctrl_bind_t *)calloc(1, sizeof *b);
    ks_toks_t *tok = (ks_toks_t *)calloc((size_t)KS_N, sizeof *tok);
    int i, g, has_prof = profile && profile[0];
    if (!b || !tok) {
        free(b);
        free(tok);
        return; /* ohne Speicher: Abschnitt fehlt, Rest der Seite bleibt */
    }
    if (!has_prof || (!prof_text && strcmp(profile, "keyboard_default") == 0)) {
        lctrl_bind_load_text(ks_legacy_text, b);
    } else {
        lctrl_bind_load_text(prof_text ? prof_text : "", b);
    }
    for (i = 0; i < KS_N; i++) {
        ks_collect(b, &ks_elems[i], &tok[i]);
    }
    keyshtml_add(out, cap, o, "<section class=\"ctl\"><h2>");
    keyshtml_add(out, cap, o, LT("gui.svg_h"));
    keyshtml_add(out, cap, o, "</h2>\n<div class=\"lg\">");
    ks_printf(out, cap, o, "<span><span class=\"sw kb\"></span>%s</span>",
              LT("gui.svg_kbd"));
    ks_printf(out, cap, o, "<span><span class=\"sw ms\"></span>%s</span>",
              LT("gui.svg_mouse"));
    ks_printf(out, cap, o, "<span><span class=\"sw pd\"></span>%s</span>",
              LT("gui.svg_pad"));
    ks_printf(out, cap, o, "<span><span class=\"sw pf\"></span>%s</span>",
              LT("gui.ctl_prim"));
    ks_printf(out, cap, o,
              "<span><span class=\"sw un\"></span>" KS_EMDASH " %s</span>",
              LT("gui.svg_unset"));
    keyshtml_add(out, cap, o, "</div>\n");
    /* Zeichnung (kein xmlns noetig: inline-SVG in HTML) */
    keyshtml_add(out, cap, o,
                 "<svg viewBox=\"" KS_VIEWBOX "\" role=\"img\" aria-label=\"");
    {
        char e[256];
        keyshtml_escape(LT("gui.svg_h"), e, sizeof e);
        keyshtml_add(out, cap, o, e);
    }
    /* Warnstreifen der Eject-Klappe (lokales Muster, kein Verweis) */
    keyshtml_add(out, cap, o, "\">\n<defs><pattern id=\"ks-hz\" "
                 "width=\"10\" height=\"10\" patternUnits=\"userSpaceOnUse\" "
                 "patternTransform=\"rotate(45)\"><rect class=\"hzb\" "
                 "width=\"10\" height=\"10\"/><rect class=\"hz\" width=\"5\" "
                 "height=\"10\"/></pattern></defs>\n");
    for (g = 0; g < KS_G_N; g++) {
        char e[256];
        ks_printf(out, cap, o,
                  "<rect class=\"bd\" x=\"%d\" y=\"%d\" width=\"%d\" "
                  "height=\"%d\" rx=\"%d\"/>\n",
                  ks_blk[g][0], ks_blk[g][1], ks_blk[g][2], ks_blk[g][3],
                  g == KS_G_CENTER ? 18 : g == KS_G_PEDALS ? 20 : 26);
        keyshtml_escape(LT(ks_grp_key[g]), e, sizeof e);
        ks_printf(out, cap, o,
                  "<text class=\"bdc\" x=\"%d\" y=\"%d\">%s</text>\n",
                  ks_blk[g][0] + ks_blk[g][2] / 2, ks_blk[g][1] + 22, e);
    }
    /* Felder (Rahmen + Beschriftung) und Sockelplatte des Zielhebels */
    for (i = 0; i < KS_NF; i++) {
        const ks_field_t *f = &ks_fields[i];
        if (f->w) {
            ks_printf(out, cap, o,
                      "<rect class=\"fd\" x=\"%d\" y=\"%d\" width=\"%d\" "
                      "height=\"%d\" rx=\"8\"/>",
                      f->x, f->y, f->w, f->h);
        }
        if (f->lkey) {
            char e[256];
            keyshtml_escape(LT(f->lkey), e, sizeof e);
            ks_printf(out, cap, o,
                      "<text class=\"fdc\" x=\"%d\" y=\"%d\">%s</text>",
                      f->tx, f->ty, e);
        }
        keyshtml_add(out, cap, o, "\n");
    }
    keyshtml_add(out, cap, o,
                 "<rect class=\"bs\" x=\"1009\" y=\"288\" width=\"140\" "
                 "height=\"44\" rx=\"8\"/>\n");
    for (i = 0; i < KS_N; i++) {
        const ks_elem_t *e = &ks_elems[i];
        const ks_toks_t *k = &tok[i];
        int cell = e->lw == 0, m[3], nl, l, s, j, any = 0;
        char t[512];
        nl = ks_masks(k, cell, m);
        ks_printf(out, cap, o, "<g class=\"el e-%s%s\" id=\"el-%s\"><title>",
                  e->id, strcmp(e->id, "bi0") == 0 ? " pf" : "", e->id);
        keyshtml_escape(ks_name(e, 0), t, sizeof t);
        keyshtml_add(out, cap, o, t);
        if (e->shape == KS_EJECT) {
            keyshtml_add(out, cap, o, " (");
            keyshtml_escape(LT("gui.svg_cover"), t, sizeof t);
            keyshtml_add(out, cap, o, t);
            keyshtml_add(out, cap, o, ")");
        }
        keyshtml_add(out, cap, o, ": ");
        for (s = 0; s < KS_SRC_N; s++) {
            for (j = 0; j < k->n[s]; j++) {
                keyshtml_escape(k->t[s][j], t, sizeof t);
                keyshtml_add(out, cap, o, any ? ", " : "");
                keyshtml_add(out, cap, o, t);
                any = 1;
            }
        }
        if (!any) {
            keyshtml_add(out, cap, o, KS_EMDASH);
        }
        keyshtml_add(out, cap, o, "</title>");
        if (cell) {
            /* Hintergrund (Hover-Flaeche), Kappe mit Name, Zeilen darunter */
            int nx = e->x + e->w / 2, off = e->shape == KS_TOG ? 9 : 0;
            ks_printf(out, cap, o,
                      "<rect class=\"lb lbc\" x=\"%d\" y=\"%d\" width=\"%d\" "
                      "height=\"%d\" rx=\"6\"/>",
                      e->x - 3, e->y - 3, e->w + 6, e->h + 40);
            ks_svg_shape(out, cap, o, e);
            ks_printf(out, cap, o, "<g class=\"co\" id=\"co-%s\">", e->id);
            ks_svg_name(out, cap, o, nx + off,
                        e->y + e->h / 2 + 4 + (e->shape >= KS_LEDR ? 2 : 0),
                        1, e->w - 12 - 2 * off, ks_name(e, 1));
            for (l = 0; l < nl; l++) {
                ks_svg_line(out, cap, o, nx, e->y + e->h + 14 + l * 14, 1,
                            e->w, k, m[l]);
            }
            keyshtml_add(out, cap, o, "</g>");
        } else {
            ks_svg_shape(out, cap, o, e);
            if (e->ax) {
                int px = e->ax < e->lx ? e->lx
                         : e->ax > e->lx + e->lw ? e->lx + e->lw : e->ax;
                int py = e->ay < e->ly ? e->ly
                         : e->ay > e->ly + KS_CO_H ? e->ly + KS_CO_H : e->ay;
                ks_printf(out, cap, o,
                          "<line class=\"ld\" x1=\"%d\" y1=\"%d\" x2=\"%d\" "
                          "y2=\"%d\"/>",
                          e->ax, e->ay, px, py);
            }
            ks_printf(out, cap, o,
                      "<g class=\"co\" id=\"co-%s\"><rect class=\"lb\" "
                      "x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" rx=\"7\"/>",
                      e->id, e->lx, e->ly, e->lw, KS_CO_H);
            ks_svg_name(out, cap, o, e->lx + 6, e->ly + 15, 0, e->lw - 12,
                        ks_name(e, 1));
            for (l = 0; l < nl; l++) {
                ks_svg_line(out, cap, o, e->lx + 6, e->ly + 30 + l * 13, 0,
                            e->lw - 12, k, m[l]);
            }
            keyshtml_add(out, cap, o, "</g>");
        }
        keyshtml_add(out, cap, o, "</g>\n");
    }
    keyshtml_add(out, cap, o, "</svg>\n<p class=\"note\">");
    keyshtml_add(out, cap, o, LT("gui.svg_note"));
    keyshtml_add(out, cap, o, " ");
    keyshtml_add(out, cap, o, LT("gui.svg_tog"));
    keyshtml_add(out, cap, o, "</p>\n");
    /* Tabelle je Element (gleiche Reihenfolge, Hover gekoppelt) */
    ks_printf(out, cap, o,
              "<table class=\"et\"><tr><th>%s</th><th>%s</th><th>%s</th>"
              "<th>%s</th></tr>\n",
              LT("gui.svg_elem"), LT("gui.svg_kbd"), LT("gui.svg_mouse"),
              LT("gui.svg_pad"));
    for (g = 0; g < KS_G_N; g++) {
        char e[256];
        keyshtml_escape(LT(ks_grp_key[g]), e, sizeof e);
        ks_printf(out, cap, o,
                  "<tr class=\"grp\"><th colspan=\"4\">%s</th></tr>\n", e);
        for (i = 0; i < KS_N; i++) {
            if (ks_elems[i].grp != g) {
                continue;
            }
            ks_printf(out, cap, o, "<tr class=\"e-%s%s\"><td>", ks_elems[i].id,
                      strcmp(ks_elems[i].id, "bi0") == 0 ? " prim" : "");
            keyshtml_escape(ks_name(&ks_elems[i], 0), e, sizeof e);
            keyshtml_add(out, cap, o, e);
            keyshtml_add(out, cap, o, "</td>");
            ks_td(out, cap, o, &tok[i], KS_SRC_KEY);
            ks_td(out, cap, o, &tok[i], KS_SRC_MOUSE);
            ks_td(out, cap, o, &tok[i], KS_SRC_PAD);
            keyshtml_add(out, cap, o, "</tr>\n");
        }
    }
    keyshtml_add(out, cap, o, "</table></section>\n");
    free(tok);
    free(b);
}

/* W182: Fixture-Profil (eigener Text, keine Spieldaten) fuer Selbsttest
 * 54 und die Beispiel-HTML (Tastatur + Maus + Gamepad gemischt). */

/* W182 Selbsttest: Callout-Gruppe co-<id> (bis zum naechsten </g>). */
static int ks_co(const char *html, const char *id, char *buf, size_t cap)
{
    char pat[64];
    const char *a, *e;
    size_t n;
    buf[0] = '\0';
    _snprintf_s(pat, sizeof pat, _TRUNCATE, "id=\"co-%s\"", id);
    a = strstr(html, pat);
    if (!a) {
        return 0;
    }
    e = strstr(a, "</g>");
    if (!e) {
        return 0;
    }
    n = (size_t)(e - a);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(buf, a, n);
    buf[n] = '\0';
    return 1;
}

/* W182 Selbsttest: Struktur pruefen (prof = Profil- statt Standard-
 * Variante). 1 = ok, sonst Grund in why. */
int ks_selfcheck(const char *html, int prof, char *why, size_t cap)
{
    static char co[8192];
    const char *sv = strstr(html, "<svg viewBox=\"" KS_VIEWBOX "\"");
    const char *se = sv ? strstr(sv, "</svg>") : NULL;
    size_t hl = strlen(html);
    int i;
    if (!sv || !se) {
        strcpy_s(why, cap, "SVG fehlt");
        return 0;
    }
    if (hl < 8 || strcmp(html + hl - 8, "</html>\n") != 0) {
        strcpy_s(why, cap, "HTML abgeschnitten (Puffer)");
        return 0;
    }
    if (strstr(html, "http") || strstr(html, "HTTP") ||
        strstr(html, "<script") || strstr(html, "src=")) {
        strcpy_s(why, cap, "Verweis/Skript enthalten");
        return 0;
    }
    for (i = 0; i < KS_N; i++) {
        char p1[64], p2[64], p3[64];
        const char *a, *b, *c;
        _snprintf_s(p1, sizeof p1, _TRUNCATE, "id=\"el-%s\"", ks_elems[i].id);
        _snprintf_s(p2, sizeof p2, _TRUNCATE, "id=\"co-%s\"", ks_elems[i].id);
        _snprintf_s(p3, sizeof p3, _TRUNCATE, "<tr class=\"e-%s",
                    ks_elems[i].id);
        a = strstr(sv, p1);
        b = strstr(sv, p2);
        c = strstr(se, p3);
        if (!a || a > se || !b || b > se || !c ||
            !ks_co(html, ks_elems[i].id, co, sizeof co) ||
            !strstr(co, "<text class=\"cl\"")) {
            _snprintf_s(why, cap, _TRUNCATE, "Element/Callout/Zeile fehlt: %s",
                        ks_elems[i].id);
            return 0;
        }
    }
    if (!strstr(sv, "class=\"el e-bi0 pf\"") ||
        !strstr(se, "<tr class=\"e-bi0 prim\"")) {
        strcpy_s(why, cap, "Primaerfeuer nicht hervorgehoben");
        return 0;
    }
    if (!strstr(html, "body:has(.e-pedal_r:hover) .e-pedal_r td")) {
        strcpy_s(why, cap, "Hover-Kopplung fehlt");
        return 0;
    }
    if (!strstr(html, LT("gui.svg_kbd")) || !strstr(html, LT("gui.svg_pad")) ||
        !strstr(html, LT("gui.svg_unset"))) {
        strcpy_s(why, cap, "Legende fehlt");
        return 0;
    }
    ks_co(html, "bi34", co, sizeof co);
    if (!strstr(co, ">1<")) {
        strcpy_s(why, cap, "Kippschalter-Taste 1 fehlt");
        return 0;
    }
    ks_co(html, "bi19", co, sizeof co);
    if (!strstr(co, "class=\"un\">" KS_EMDASH)) {
        strcpy_s(why, cap, "Unbelegt-Strich fehlt (bi19)");
        return 0;
    }
    ks_co(html, "bi0", co, sizeof co);
    if (!strstr(co, ">SPACE<") || !strstr(co, LT("gui.svg_m_left")) ||
        (prof ? !strstr(co, ">RB<") : strstr(co, ">RB<") != NULL)) {
        strcpy_s(why, cap, "Belegung bi0 falsch");
        return 0;
    }
    if (prof) {
        ks_co(html, "aim", co, sizeof co);
        if (!strstr(co, ">LS<") || strstr(co, "LS " KS_HARR) ||
            !strstr(co, ">" KS_LARR "<")) {
            strcpy_s(why, cap, "Achsen-Zusammenfassung falsch (aim)");
            return 0;
        }
        ks_co(html, "bi34", co, sizeof co);
        if (!strstr(co, ">BACK+D" KS_LARR "<")) {
            strcpy_s(why, cap, "Akkord falsch (bi34)");
            return 0;
        }
        ks_co(html, "gear", co, sizeof co);
        if (!strstr(co, ">" KS_TRI_U "BACK+RB<") || !strstr(co, ">N:0<")) {
            strcpy_s(why, cap, "Gang-Callout falsch");
            return 0;
        }
        ks_co(html, "sight", co, sizeof co);
        if (!strstr(co, ">Shift+" KS_LARR "<") || !strstr(co, ">RS " KS_VARR
                                                          "<")) {
            strcpy_s(why, cap, "Sicht-Callout falsch");
            return 0;
        }
    } else {
        if (strstr(sv, "class=\"tp\"")) {
            strcpy_s(why, cap, "Standard-Variante mit Gamepad");
            return 0;
        }
        ks_co(html, "bi3", co, sizeof co);
        if (strstr(co, KS_EMDASH) || !strstr(co, "Shift")) { /* W403: Eject = Umschalt+Entf (frueher unbelegt) */
            _snprintf_s(why, cap, _TRUNCATE, "Eject nicht Umschalt+Entf (Standard): %.80s", co);
            return 0;
        }
        ks_co(html, "pedal_r", co, sizeof co);
        if (!strstr(co, ">W<")) {
            strcpy_s(why, cap, "Gaspedal falsch (Standard)");
            return 0;
        }
        ks_co(html, "gear", co, sizeof co);
        if (!strstr(co, ">" KS_TRI_U "E<") || !strstr(co, ">" KS_TRI_D "Q<")) {
            strcpy_s(why, cap, "Gang falsch (Standard)");
            return 0;
        }
    }
    return 1;
}

/* W184 Selbsttest: Kasten eines Elements (Callout bzw. Zelle). */
static void ks_box(const ks_elem_t *e, int r[4])
{
    if (e->lw) {
        r[0] = e->lx, r[1] = e->ly, r[2] = e->lw, r[3] = KS_CO_H;
    } else {
        r[0] = e->x - 3, r[1] = e->y - 3, r[2] = e->w + 6, r[3] = e->h + 40;
    }
}

static int ks_in(const int r[4], const short b[4])
{
    return r[0] >= b[0] && r[1] >= b[1] && r[0] + r[2] <= b[0] + b[2] &&
           r[1] + r[3] <= b[1] + b[3];
}

/* W184 Selbsttest: physische Anordnung. Jedes Element liegt mit Form-
 * Anker, Leader-Start und Kasten in seinem Block, kein Kasten ueberdeckt
 * einen anderen, Felder liegen in einem Block, und die Bloecke stimmen
 * mit der Zuordnung des Original-Controllers ueberein (Regressionsschutz:
 * Kippschalter + Sicht-Stick links, Luke/Zuendung/START/Eject/MultiMon
 * rechts, Magazin + Waffenwahl + COM + Tuner in der Mitte). 1 = ok. */
int ks_layoutcheck(char *why, size_t cap)
{
    static const struct { const char *id; int g; } want[] = {
        { "gear", KS_G_LEFT }, { "rot", KS_G_LEFT }, { "sight", KS_G_LEFT },
        { "bi33", KS_G_LEFT }, { "bi34", KS_G_LEFT }, { "bi38", KS_G_LEFT },
        { "bi28", KS_G_CENTER }, { "bi32", KS_G_CENTER },
        { "tuner", KS_G_CENTER }, { "bi16", KS_G_CENTER },
        { "bi24", KS_G_CENTER }, { "bi13", KS_G_CENTER },
        { "bi25", KS_G_CENTER }, { "bi27", KS_G_CENTER },
        { "aim", KS_G_RIGHT }, { "bi0", KS_G_RIGHT }, { "bi3", KS_G_RIGHT },
        { "bi4", KS_G_RIGHT }, { "bi6", KS_G_RIGHT }, { "bi7", KS_G_RIGHT },
        { "bi12", KS_G_RIGHT }, { "pedal_m", KS_G_PEDALS },
    };
    int i, j;
    for (i = 0; i < KS_N; i++) {
        const ks_elem_t *e = &ks_elems[i];
        const short *b = ks_blk[e->grp];
        int r[4], pt[4];
        ks_box(e, r);
        pt[0] = e->x, pt[1] = e->y, pt[2] = 0, pt[3] = 0;
        if (!ks_in(r, b) || !ks_in(pt, b)) {
            _snprintf_s(why, cap, _TRUNCATE, "%s ausserhalb seines Blocks",
                        e->id);
            return 0;
        }
        pt[0] = e->ax, pt[1] = e->ay;
        if (e->ax && !ks_in(pt, b)) {
            _snprintf_s(why, cap, _TRUNCATE, "Leader %s ausserhalb", e->id);
            return 0;
        }
        for (j = 0; j < i; j++) {
            int q[4];
            ks_box(&ks_elems[j], q);
            if (r[0] < q[0] + q[2] && q[0] < r[0] + r[2] &&
                r[1] < q[1] + q[3] && q[1] < r[1] + r[3]) {
                _snprintf_s(why, cap, _TRUNCATE, "Kasten %s ueberdeckt %s",
                            e->id, ks_elems[j].id);
                return 0;
            }
        }
    }
    for (i = 0; i < KS_NF; i++) {
        const ks_field_t *f = &ks_fields[i];
        int r[4], hit = 0, g;
        r[0] = f->w ? f->x : f->tx, r[1] = f->w ? f->y : f->ty;
        r[2] = f->w, r[3] = f->h;
        for (g = 0; g < KS_G_N; g++) {
            hit |= ks_in(r, ks_blk[g]);
        }
        if (!hit) {
            _snprintf_s(why, cap, _TRUNCATE, "Feld %d ausserhalb", i);
            return 0;
        }
    }
    for (i = 0; i < (int)(sizeof want / sizeof want[0]); i++) {
        for (j = 0; j < KS_N; j++) {
            if (strcmp(ks_elems[j].id, want[i].id) == 0) {
                break;
            }
        }
        if (j == KS_N || ks_elems[j].grp != want[i].g) {
            _snprintf_s(why, cap, _TRUNCATE, "%s im falschen Block",
                        want[i].id);
            return 0;
        }
    }
    return 1;
}

/* W182 Selbsttest: jedes Element der Mapping-Liste (kCtrlElems ohne
 * Client-Aktionen) ist einem gezeichneten Element zugeordnet. */
const char *ks_uncovered(void)
{
    int i, j;
    for (i = 0; i < LCTRL_ELEM_N; i++) {
        int hit = 0;
        if (kCtrlElems[i].kind == LCTRL_ACT) {
            continue;
        }
        for (j = 0; j < KS_N && !hit; j++) {
            char names[160], *nm, *ctx = NULL;
            strcpy_s(names, sizeof names, ks_elems[j].elems);
            for (nm = strtok_s(names, " ", &ctx); nm;
                 nm = strtok_s(NULL, " ", &ctx)) {
                if (strcmp(nm, kCtrlElems[i].key) == 0) {
                    hit = 1;
                    break;
                }
            }
        }
        if (!hit) {
            return kCtrlElems[i].key;
        }
    }
    return NULL;
}

/* HTML bauen (rein bis auf LT; ohne Fenster pruefbar). profile = ""
 * heisst Standardbelegung (kein Gamepad-Abschnitt). prof_text = UTF-8
 * oder NULL (mit prof_missing als Hinweis). */
void keyshtml_build(const char *memo, const char *profile,
                           const char *prof_text, const char *prof_missing,
                           const char *mouse_lbl, int mouse_sens,
                           const char *pad_txt, const char *pad_live,
                           const char *sbc_txt, char *out, size_t cap)
{
    size_t o = 0;
    const char *brand = SB_EDITION_COMMUNITY ? "Steelbound" : "Steel Battalion";
    char e[4096];
    int i, has_prof = profile && profile[0];
    keyshtml_parse(memo);
    { /* W211: memo rows use actual profile bindings, including disabled keys. */
        lctrl_bind_t bindings;
        lctrl_bind_init(&bindings);
        lctrl_bind_load_text(has_prof ? (prof_text ? prof_text : "") : ks_legacy_text, &bindings);
        for (i = 0; i < kh_n; ++i) {
            const char *action = !strcmp(kh_rows[i].act, LT("bind.feedback_shot")) ? "feedback_shot" :
                                 !strcmp(kh_rows[i].act, LT("bind.feedback_clip")) ? "feedback_clip" :
                                 !strcmp(kh_rows[i].act, LT("bind.orig_view")) ? "orig_view" : /* W283 */
                                 !strcmp(kh_rows[i].act, LT("bind.photo")) ? "photo" :
                                 !strcmp(kh_rows[i].act, LT("bind.quick_shot")) ? "quick_shot" : /* W290 */
                                 !strcmp(kh_rows[i].act, LT("bind.replay_exit")) ? "replay_exit" : /* W299 */
                                 !strcmp(kh_rows[i].act, LT("bind.net_hud")) ? "net_hud" : NULL; /* W374 */
            if (action) {
                lctrl_bind_row_text(&bindings, lctrl_bind_elem_index(action), kh_rows[i].key, sizeof kh_rows[i].key);
            }
        }
    }
    keyshtml_add(out, cap, &o, "<!DOCTYPE html>\n<html lang=\"");
    keyshtml_add(out, cap, &o, sb_lang_code());
    keyshtml_add(out, cap, &o, "\">\n<head>\n<meta charset=\"utf-8\">\n"
                 "<meta name=\"viewport\" content=\"width=device-width,"
                 "initial-scale=1\">\n<title>");
    keyshtml_add(out, cap, &o, brand);
    keyshtml_add(out, cap, &o, " " U8_DASH " ");
    keyshtml_add(out, cap, &o, LT("gui.page_controls"));
    keyshtml_add(out, cap, &o, "</title>\n");
    keyshtml_css(out, cap, &o);
    ks_css(out, cap, &o); /* W182 KEYS-SVG */
    keyshtml_add(out, cap, &o, "</head>\n<body><div class=\"wrap\">\n<h1>");
    keyshtml_add(out, cap, &o, brand);
    keyshtml_add(out, cap, &o, " " U8_DASH " ");
    keyshtml_add(out, cap, &o, LT("gui.page_controls"));
    keyshtml_add(out, cap, &o, "</h1>\n<p class=\"sub\">");
    if (has_prof) {
        keyshtml_add(out, cap, &o, LT("gui.ctl_profile"));
        keyshtml_add(out, cap, &o, ": ");
        keyshtml_escape(profile, e, sizeof e);
        keyshtml_add(out, cap, &o, e);
    } else {
        keyshtml_escape(LT("gui.legacy_label"), e, sizeof e);
        keyshtml_add(out, cap, &o, e);
    }
    keyshtml_add(out, cap, &o, "</p>\n<div class=\"prim\">"
                 "<div class=\"prim-h\">");
    keyshtml_add(out, cap, &o, LT("gui.ctl_prim"));
    keyshtml_add(out, cap, &o, "</div><div class=\"prim-k\">");
    keyshtml_add(out, cap, &o, LT("gui.ctl_prim_keys"));
    keyshtml_add(out, cap, &o, "</div></div>\n");
    keysvg_build(profile, prof_text, out, cap, &o); /* W182 KEYS-SVG */
    keyshtml_add(out, cap, &o, "<div class=\"grid\">\n");
    if (has_prof) {
        keyshtml_add(out, cap, &o, "<section class=\"full\"><h2>");
        keyshtml_add(out, cap, &o, LT("gui.ctl_profile"));
        keyshtml_add(out, cap, &o, ": ");
        keyshtml_escape(profile, e, sizeof e);
        keyshtml_add(out, cap, &o, e);
        keyshtml_add(out, cap, &o, "</h2>");
        if (prof_text && prof_text[0]) {
            enum { PRE_CAP = 65536 };
            char *pre = (char *)malloc(PRE_CAP); /* W535: Heap statt statisch */
            if (pre) {
                keyshtml_escape(prof_text, pre, PRE_CAP);
                keyshtml_add(out, cap, &o, "<pre>");
                keyshtml_add(out, cap, &o, pre);
                keyshtml_add(out, cap, &o, "</pre>");
                free(pre);
            }
        } else if (prof_missing && prof_missing[0]) {
            keyshtml_escape(prof_missing, e, sizeof e);
            keyshtml_add(out, cap, &o, "<p>");
            keyshtml_add(out, cap, &o, e);
            keyshtml_add(out, cap, &o, "</p>");
        }
        keyshtml_add(out, cap, &o, "</section>\n");
    }
    keyshtml_table(out, cap, &o, LT("gui.keys_h_menus"), KH_MENUS);
    { /* Start: Reihenfolge als Einleitung (gleiche Quelle: Startseite) */
        int n = 0;
        for (i = 0; i < kh_n; i++) {
            n += kh_rows[i].sec == KH_START;
        }
        if (n) {
            keyshtml_add(out, cap, &o, "<section><h2>");
            keyshtml_add(out, cap, &o, LT("gui.start_mech"));
            keyshtml_add(out, cap, &o, "</h2><p>");
            keyshtml_add(out, cap, &o, LT("gui.start_seq"));
            keyshtml_add(out, cap, &o, "</p><table>\n");
            for (i = 0; i < kh_n; i++) {
                if (kh_rows[i].sec == KH_START) {
                    keyshtml_row(out, cap, &o, kh_rows[i].key, kh_rows[i].act);
                }
            }
            keyshtml_add(out, cap, &o, "</table></section>\n");
        }
    }
    keyshtml_table(out, cap, &o, LT("gui.keys_h_combat"), KH_COMBAT);
    keyshtml_table(out, cap, &o, LT("gui.keys_h_move"), KH_MOVE);
    keyshtml_table(out, cap, &o, LT("gui.keys_h_cockpit"), KH_COCKPIT);
    { /* Maus: Modus/Empfindlichkeit live + F10 aus dem Memo */
        int n = 0;
        keyshtml_add(out, cap, &o, "<section><h2>");
        keyshtml_add(out, cap, &o, LT("gui.ctl_mouse"));
        keyshtml_add(out, cap, &o, "</h2><p>");
        keyshtml_escape(mouse_lbl ? mouse_lbl : "", e, sizeof e);
        keyshtml_add(out, cap, &o, e);
        keyshtml_add(out, cap, &o, " " U8_DOT " ");
        keyshtml_add(out, cap, &o, LT("gui.ctl_sens"));
        keyshtml_add(out, cap, &o, " ");
        keyshtml_addf(out, cap, &o, "%d", mouse_sens);
        keyshtml_add(out, cap, &o, "</p>");
        for (i = 0; i < kh_n; i++) {
            n += kh_rows[i].sec == KH_MOUSEX;
        }
        if (n) {
            keyshtml_add(out, cap, &o, "<table>\n");
            for (i = 0; i < kh_n; i++) {
                if (kh_rows[i].sec == KH_MOUSEX) {
                    keyshtml_row(out, cap, &o, kh_rows[i].key, kh_rows[i].act);
                }
            }
            keyshtml_add(out, cap, &o, "</table>");
        }
        keyshtml_add(out, cap, &o, "</section>\n");
    }
    if (has_prof) {
        /* Gamepad: nur bei aktivem Profil (PAD-Zeilen daraus). */
        keyshtml_add(out, cap, &o, "<section><h2>");
        keyshtml_add(out, cap, &o, LT("gui.ctl_pad"));
        keyshtml_add(out, cap, &o, "</h2><p>");
        keyshtml_escape(pad_txt ? pad_txt : "", e, sizeof e);
        keyshtml_add(out, cap, &o, e);
        if (pad_live && pad_live[0]) {
            keyshtml_add(out, cap, &o, "<br>");
            keyshtml_escape(pad_live, e, sizeof e);
            keyshtml_add(out, cap, &o, e);
        }
        keyshtml_add(out, cap, &o, "</p>");
        if (prof_text && strstr(prof_text, "PAD:")) {
            static char plines[16384];
            char *p;
            strcpy_s(plines, sizeof plines, prof_text);
            keyshtml_add(out, cap, &o, "<table>\n");
            p = plines;
            while (*p) {
                char *line = p, *eq;
                while (*p && *p != '\n' && *p != '\r') {
                    p++;
                }
                if (*p == '\r') {
                    *p++ = '\0';
                    if (*p == '\n') {
                        p++;
                    }
                } else if (*p == '\n') {
                    *p++ = '\0';
                }
                if (!strstr(line, "PAD:")) {
                    continue;
                }
                eq = strchr(line, '=');
                if (!eq) {
                    continue;
                }
                *eq = '\0';
                keyshtml_row(out, cap, &o, keyshtml_trim(line),
                             keyshtml_trim(eq + 1));
            }
            keyshtml_add(out, cap, &o, "</table>");
        }
        keyshtml_add(out, cap, &o, "</section>\n");
    }
    { /* Original-Controller: Kurzinfo */
        keyshtml_add(out, cap, &o, "<section><h2>");
        keyshtml_add(out, cap, &o, LT("gui.ctl_device"));
        keyshtml_add(out, cap, &o, "</h2><p>");
        keyshtml_escape(sbc_txt ? sbc_txt : "", e, sizeof e);
        keyshtml_add(out, cap, &o, e);
        keyshtml_add(out, cap, &o, "</p><p>");
        keyshtml_add(out, cap, &o, LT("gui.keys_sbc_info"));
        keyshtml_add(out, cap, &o, "</p></section>\n");
    }
    keyshtml_add(out, cap, &o, "</div>\n<footer>");
    if (kh_footer[0]) {
        keyshtml_escape(kh_footer, e, sizeof e);
        keyshtml_add(out, cap, &o, e);
        keyshtml_add(out, cap, &o, "<br>");
    }
    keyshtml_add(out, cap, &o, LT("gui.keys_local"));
    keyshtml_add(out, cap, &o, "</footer></div></body>\n</html>\n");
}

/* W179: Zielpfad controls.html (W150-Datenorte, sonst Client-Ordner). */
static void keyshtml_path(char *out, size_t cap)
{
    char acp[1024];
    if (sb_userdata_path(SB_UD_ROOT, acp, sizeof acp) == 1) {
        char u8[1024];
        u8_from_acp(acp, u8, sizeof u8);
        _snprintf_s(out, cap, _TRUNCATE, "%s/controls.html", u8);
        return;
    }
    _snprintf_s(out, cap, _TRUNCATE, "%s/controls.html", g_client_dir);
}

/* W179: Knopf "Im Browser oeffnen": HTML schreiben + oeffnen. Fehler
 * als Klartext in die Statuszeile. Test/Shot: nur schreiben, nie
 * oeffnen (stiehlt nie den Fokus). */
void keyshtml_open(void)
{
    char prof[64], miss[512], path[1200];
    enum { HTML_CAP = 393216 };
    static char praw[12288], pu8[24576];
    char *html; /* W535 AVVT: 384 KB vom Heap statt statisch (.bss des Release-Launchers verkleinert) */
    const char *prof_text = NULL, *mouse_lbl;
    int r = 1, sens, msel, ssel;
    FILE *f = NULL;
    size_t hlen;
    miss[0] = '\0';
    ctrl_profile_from_ui(prof, sizeof prof);
    if (prof[0]) {
        size_t n = 0;
        r = ctrl_profile_load(prof, praw, sizeof praw, &n);
        if (r > 0) {
            u8_from_acp(praw, pu8, sizeof pu8);
            prof_text = pu8;
        } else {
            _snprintf_s(miss, sizeof miss, _TRUNCATE,
                        LT(r == 0 ? "gui.ctl_notname" : "gui.ctl_notfound"),
                        prof);
        }
    }
    msel = ComboBox_GetCurSel(ctl(ID_MOUSE_MODE));
    mouse_lbl = LT(msel == 1 ? "gui.mouse_rel" : "gui.mouse_abs");
    ssel = ComboBox_GetCurSel(ctl(ID_MOUSE_SENS));
    sens = (ssel >= 0 && ssel < 16) ? ssel + 1 : 4;
    keyshtml_path(path, sizeof path);
    html = (char *)calloc(1, HTML_CAP);
    if (!html) {
        char t[1500];
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.keys_write_err"), path);
        set_status_k(t, 2);
        return;
    }
    keyshtml_build(LT("gui.keys_memo"), prof, prof_text,
                   miss[0] ? miss : NULL, mouse_lbl, sens, g_pad_txt,
                   g_padlive_txt, g_sbc_txt, html, HTML_CAP);
    if (u8_fopen_s(&f, path, "wb") != 0 || !f) {
        char t[1500];
        free(html);
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.keys_write_err"), path);
        set_status_k(t, 2);
        return;
    }
    hlen = strlen(html);
    if (hlen && fwrite(html, 1, hlen, f) != hlen) {
        char t[1500];
        fclose(f);
        free(html);
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.keys_write_err"), path);
        set_status_k(t, 2);
        return;
    }
    fclose(f);
    free(html);
    if (g_o.shot != 0) {
        printf("[open] %s\n", path);
        set_status(LT("gui.keys_open_ok"));
        return;
    }
    {
        wchar_t w[1200];
        HINSTANCE rc;
        u8_to_w(path, w, 1200);
        rc = ShellExecuteW(g_hwnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)rc <= 32) {
            char t[1500];
            _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.keys_open_err"), path);
            set_status_k(t, 2);
        } else {
            set_status(LT("gui.keys_open_ok"));
        }
    }
}

/* ------------------------------------------------------------------ */
/* W185 ZADIG-HELP: Anleitung "Original-Controller anschliessen"       */
/* ------------------------------------------------------------------ */
/* Lokale HTML-Seite wie W179 (controls.html): Erzeugen im Benutzer-
 * daten-Ordner, im Browser oeffnen, eingebettetes CSS im Launcher-
 * Look (keyshtml_css), druckbar, Sprache = Launcher-Sprache, kein
 * Netzwerk durch den Launcher. Inhalt aus lang-Schluesseln
 * (gui.zadig_*), genau ein externer Verweis (Zadig-Seite als <a href>,
 * der Nutzer laedt selbst), keine Skripte. Quelltext ASCII (U8_*),
 * Ausgabe UTF-8. */

/* Zusatz-CSS: Listen + die eine Link-Farbe (Schirm, Druck). */
static void zadig_css(char *out, size_t cap, size_t *o)
{
    keyshtml_add(out, cap, o,
                 "<style>\n"
                 "ol,ul{margin:0 0 8px;padding-left:22px;font-size:14px;"
                 "color:#c9cfd9}\n"
                 "li{margin:0 0 4px}\n"
                 "a{color:#f2a84e}\n"
                 "@media print{\n"
                 "ol,ul{color:#333}\n"
                 "a{color:#8a4d00}\n"
                 "}\n"
                 "</style>\n");
}

/* Eine Anleitungszeile als <li> (escapen, Zadig-Adresse verlinken). */
static void zadig_li(char *out, size_t cap, size_t *o, const char *line)
{
    static char e[4096];
    char *p, *q;
    keyshtml_escape(line ? line : "", e, sizeof e);
    keyshtml_add(out, cap, o, "<li>");
    p = e;
    for (;;) {
        char sv;
        q = strstr(p, ZADIG_URL);
        if (!q) {
            keyshtml_add(out, cap, o, p);
            break;
        }
        sv = *q;
        *q = '\0';
        keyshtml_add(out, cap, o, p);
        *q = sv;
        keyshtml_add(out, cap, o, "<a href=\"" ZADIG_URL "\">" ZADIG_URL "</a>");
        p = q + sizeof ZADIG_URL - 1;
    }
    keyshtml_add(out, cap, o, "</li>\n");
}

/* Abschnitt oeffnen: <section><h2>Kopf</h2><ol/ul> Zeilen </..>.
 * zadig_section_end schliesst (dazwischen passt Hinweis-HTML, s.
 * Warnung in Schritt 2, Zentrier-Hinweis in Schritt 4). */
static void zadig_section_open(char *out, size_t cap, size_t *o,
                               const char *head, const char *body, int ordered)
{
    static char b[4096], e[256];
    char *p;
    keyshtml_add(out, cap, o, "<section><h2>");
    keyshtml_escape(head ? head : "", e, sizeof e);
    keyshtml_add(out, cap, o, e);
    keyshtml_add(out, cap, o, ordered ? "</h2>\n<ol>\n" : "</h2>\n<ul>\n");
    strcpy_s(b, sizeof b, body ? body : "");
    p = b;
    while (*p) {
        char *line = p, *nl = strchr(p, '\n');
        if (nl) {
            *nl = '\0';
        }
        if (*keyshtml_trim(line)) {
            zadig_li(out, cap, o, line);
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    keyshtml_add(out, cap, o, ordered ? "</ol>\n" : "</ul>\n");
}

static void zadig_section_end(char *out, size_t cap, size_t *o)
{
    keyshtml_add(out, cap, o, "</section>\n");
}

/* Hinweis-Absatz in einem Abschnitt. */
static void zadig_p(char *out, size_t cap, size_t *o, const char *text)
{
    static char e[2048];
    keyshtml_add(out, cap, o, "<p>");
    keyshtml_escape(text ? text : "", e, sizeof e);
    keyshtml_add(out, cap, o, e);
    keyshtml_add(out, cap, o, "</p>\n");
}

/* Warnung in einem Abschnitt (Warnkasten wie Primaerfeuer). */
static void zadig_warn(char *out, size_t cap, size_t *o, const char *text)
{
    static char e[2048];
    keyshtml_add(out, cap, o, "<div class=\"prim\"><div class=\"prim-k\">");
    keyshtml_escape(text ? text : "", e, sizeof e);
    keyshtml_add(out, cap, o, e);
    keyshtml_add(out, cap, o, "</div></div>\n");
}

void zadig_build(char *out, size_t cap)
{
    size_t o = 0;
    const char *brand = SB_EDITION_COMMUNITY ? "Steelbound" : "Steel Battalion";
    keyshtml_add(out, cap, &o, "<!DOCTYPE html>\n<html lang=\"");
    keyshtml_add(out, cap, &o, sb_lang_code());
    keyshtml_add(out, cap, &o, "\">\n<head>\n<meta charset=\"utf-8\">\n"
                 "<meta name=\"viewport\" content=\"width=device-width,"
                 "initial-scale=1\">\n<title>");
    keyshtml_add(out, cap, &o, brand);
    keyshtml_add(out, cap, &o, " " U8_DASH " ");
    keyshtml_add(out, cap, &o, LT("gui.zadig_title"));
    keyshtml_add(out, cap, &o, "</title>\n");
    keyshtml_css(out, cap, &o);
    zadig_css(out, cap, &o);
    keyshtml_add(out, cap, &o, "</head>\n<body><div class=\"wrap\">\n<h1>");
    keyshtml_add(out, cap, &o, brand);
    keyshtml_add(out, cap, &o, " " U8_DASH " ");
    keyshtml_add(out, cap, &o, LT("gui.zadig_title"));
    keyshtml_add(out, cap, &o, "</h1>\n<div class=\"grid\">\n");
    zadig_section_open(out, cap, &o, LT("gui.zadig_need_h"),
                       LT("gui.zadig_need"), 0);
    zadig_section_end(out, cap, &o);
    zadig_section_open(out, cap, &o, LT("gui.zadig_driver_h"),
                       LT("gui.zadig_driver"), 1);
    zadig_warn(out, cap, &o, LT("gui.zadig_warn"));
    zadig_p(out, cap, &o, LT("gui.zadig_admin"));
    zadig_section_end(out, cap, &o);
    zadig_section_open(out, cap, &o, LT("gui.zadig_launch_h"),
                       LT("gui.zadig_launch"), 1);
    zadig_section_end(out, cap, &o);
    zadig_section_open(out, cap, &o, LT("gui.zadig_assist_h"),
                       LT("gui.zadig_assist"), 1);
    zadig_p(out, cap, &o, LT("gui.zadig_center"));
    zadig_section_end(out, cap, &o);
    zadig_section_open(out, cap, &o, LT("gui.zadig_start_h"),
                       LT("gui.zadig_start"), 1);
    zadig_section_end(out, cap, &o);
    zadig_section_open(out, cap, &o, LT("gui.zadig_fix_h"),
                       LT("gui.zadig_fix"), 0);
    zadig_section_end(out, cap, &o);
    keyshtml_add(out, cap, &o, "</div>\n<footer>");
    keyshtml_add(out, cap, &o, LT("gui.keys_local"));
    keyshtml_add(out, cap, &o, "</footer></div></body>\n</html>\n");
}

/* W185: Zielpfad zadig_guide.html (W150-Datenorte, sonst Client-Ordner). */
static void zadig_path(char *out, size_t cap)
{
    char acp[1024];
    if (sb_userdata_path(SB_UD_ROOT, acp, sizeof acp) == 1) {
        char u8[1024];
        u8_from_acp(acp, u8, sizeof u8);
        _snprintf_s(out, cap, _TRUNCATE, "%s/zadig_guide.html", u8);
        return;
    }
    _snprintf_s(out, cap, _TRUNCATE, "%s/zadig_guide.html", g_client_dir);
}

/* W185: Knopf "Setup guide": HTML schreiben + oeffnen. Fehler als
 * Klartext in die Statuszeile. Test/Shot: nur schreiben, nie oeffnen
 * (stiehlt nie den Fokus). */
void zadig_open(void)
{
    char path[1200];
    enum { ZHTML_CAP = 65536 };
    char *html = (char *)calloc(1, ZHTML_CAP); /* W535: Heap statt statisch */
    FILE *f = NULL;
    size_t hlen;
    zadig_path(path, sizeof path);
    if (!html) {
        char t[1500];
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.zadig_write_err"), path);
        set_status_k(t, 2);
        return;
    }
    zadig_build(html, ZHTML_CAP);
    if (u8_fopen_s(&f, path, "wb") != 0 || !f) {
        char t[1500];
        free(html);
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.zadig_write_err"), path);
        set_status_k(t, 2);
        return;
    }
    hlen = strlen(html);
    if (hlen && fwrite(html, 1, hlen, f) != hlen) {
        char t[1500];
        fclose(f);
        free(html);
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.zadig_write_err"), path);
        set_status_k(t, 2);
        return;
    }
    fclose(f);
    free(html);
    if (g_o.shot != 0) {
        printf("[open] %s\n", path);
        set_status(LT("gui.zadig_open_ok"));
        return;
    }
    {
        wchar_t w[1200];
        HINSTANCE rc;
        u8_to_w(path, w, 1200);
        rc = ShellExecuteW(g_hwnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)rc <= 32) {
            char t[1500];
            _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.zadig_open_err"), path);
            set_status_k(t, 2);
        } else {
            set_status(LT("gui.zadig_open_ok"));
        }
    }
}

void update_sbc_status(void)
{
    /* W171: Ergebnis merken (R25: Controller gewaehlt, Geraet fehlt) */
    g_sbc_rc = lctrl_sbc_status(g_sbc_txt, sizeof g_sbc_txt);
}

/* W159: VR-Startknopf je OpenXR-Runtime freischalten + Hinweistext.
 * Nur Registry lesen (app_xr_runtime_present), nichts laden/aendern. */
void update_vr_button(void)
{
    char path[1024];
    g_vr_present = app_xr_runtime_present(path, sizeof path);
    /* W171: Sperre + Grund ohne Runtime = Regel R5 (ui_apply_rules) */
    _snprintf_s(g_vr_hint, sizeof g_vr_hint, _TRUNCATE, "%s", LT("gui.vr_hint"));
}

/* W282 ISSUE6: DirectInput-Joysticks auch in der Gamepad-Karte nennen
 * (vorher nur im HOTAS-Dialog sichtbar). Einmal beim ersten Oeffnen der
 * Steuerungsseite und nach dem HOTAS-Dialog; nie offscreen (Shots,
 * Selbsttest: keine echte Hardware). */
int is_offscreen(HWND h);
static char g_joy_card_names[400];
static int g_joy_card_n;

void joy_card_scan(void)
{
    int i, n;
    g_joy_card_n = 0;
    g_joy_card_names[0] = 0;
    if (g_o.shot || !g_hwnd || is_offscreen(g_hwnd) || ijoy_init() != 0) {
        return;
    }
    n = ijoy_scan();
    for (i = 0; i < n; i++) {
        const ijoy_info_t *in = ijoy_info(i);
        if (!in) {
            break;
        }
        if (g_joy_card_names[0]) {
            strcat_s(g_joy_card_names, sizeof g_joy_card_names, ", ");
        }
        strncat_s(g_joy_card_names, sizeof g_joy_card_names,
                  in->product[0] ? in->product : in->name, _TRUNCATE);
        g_joy_card_n = i + 1;
    }
}

int update_pad_status(void)
{
    lctrl_pad_t p;
    int port = lctrl_pad_first(&p);
    char a[160], b[512];
    if (port < 0) {
        strncpy_s(a, sizeof a, LT("gui.pad_off"), _TRUNCATE);
        strncpy_s(b, sizeof b, LT("gui.pad_none"), _TRUNCATE);
    } else {
        _snprintf_s(a, sizeof a, _TRUNCATE, LT("gui.pad_on"), port);
        lctrl_pad_format(&p, b, sizeof b);
    }
    if (g_joy_card_n > 0) { /* W282 */
        size_t o = strlen(a);
        _snprintf_s(a + o, sizeof a - o, _TRUNCATE, LT("gui.pad_joy_n"), g_joy_card_n);
        if (port < 0) {
            _snprintf_s(b, sizeof b, _TRUNCATE, LT("gui.pad_joy_names"), g_joy_card_names);
        }
    }
    if (strcmp(a, g_pad_txt) == 0 && strcmp(b, g_padlive_txt) == 0) {
        return 0;
    }
    strcpy_s(g_pad_txt, sizeof g_pad_txt, a);
    strcpy_s(g_padlive_txt, sizeof g_padlive_txt, b);
    return 1;
}

void refresh_profiles(void)
{
    HWND cb = ctl(ID_PROF_COMBO);
    char names[64][64], dflt[64], cur[128];
    int n, i, sel = -1;
    ui_gettext(cb, cur, sizeof cur);
    trim_u8(cur);
    g_updating++;
    ComboBox_ResetContent(cb);
    n = lcfg_profile_list(g_client_dir, names, 64);
    for (i = 0; i < n; i++) {
        combo_add(cb, names[i]);
        if (cur[0] && strcmp(names[i], cur) == 0) {
            sel = i;
        }
    }
    if (lcfg_profile_get_default(g_client_dir, dflt, sizeof dflt) != 0) {
        dflt[0] = '\0';
    }
    strcpy_s(g_dflt_setup, sizeof g_dflt_setup, dflt); /* W171: R28 */
    if (sel >= 0) {
        ComboBox_SetCurSel(cb, sel);
    } else if (cur[0]) {
        ui_settext(cb, cur);
    } else if (dflt[0]) {
        for (i = 0; i < n; i++) {
            if (strcmp(names[i], dflt) == 0) {
                ComboBox_SetCurSel(cb, i);
            }
        }
    }
    g_updating--;
    _snprintf_s(g_prof_info, sizeof g_prof_info, _TRUNCATE, LT("gui.prof_line"), n,
                dflt[0] ? dflt : LT("gui.prof_none"));
    setup_auto_sync(0); /* W172/P7 */
}

void profile_name_from_ui(char *out, size_t cap)
{
    ui_gettext(ctl(ID_PROF_COMBO), out, cap);
    trim_u8(out);
}

/* W173 LAUNCHER-P4: g_gfx = Werte des einheitlichen Grafik-Presets (shx[]
 * inkl. hud_label_avoid, fx[], HD). Die Seite Grafik zeigt einen Teil; alle
 * anderen Werte (Feinregler aus dem ESC-Menue) bleiben erhalten und zaehlen
 * fuer Benutzerdefiniert. */

/* Name eines Presets (LGP_*). W296 PRESET-ORIG (Nutzer 15.11.): nur ein
 * "Original" ohne Bildrate (es laeuft mit der Monitor-Rate), Spalte 3 heisst
 * "Performance". Vorher (PO 7.11.) "Original - <echte Rate> FPS". */
void gfx_name(int p, const launcher_settings_t *s, char *out, size_t cap)
{
    (void)s;
    strncpy_s(out, cap, LT(p == LGP_ORIGINAL ? "gui.gfx_p_orig"
                          : p == LGP_SUBTLE ? "gui.gfx_p_subtle"
                          : p == LGP_STRONG ? "gui.gfx_p_strong"
                          : p == LGP_PERFORMANCE ? "gui.gfx_p_performance"
                          : p == LGP_CLEAR ? "gui.gfx_p_clear"
                          : p == LGP_ENHANCED ? "gui.gfx_p_enh"
                          : p == LGP_VIVID ? "gui.gfx_p_vivid" : "gui.gfx_p_custom"), _TRUNCATE); /* W465, W478 */
}

/* Name des aktuellen Zustands (Bereitschaft, Statuszeile): Preset bzw.
 * "Benutzerdefiniert - basiert auf X". */
void gfx_state_name(const launcher_settings_t *s, char *out, size_t cap)
{
    lgp_mask_t lock = gfx_lock_now();
    int p = lcfg_gfx_preset(s, lock);
    if (p == LGP_CUSTOM) {
        char b[96];
        gfx_name(lcfg_gfx_base(s, lock), s, b, sizeof b);
        _snprintf_s(out, cap, _TRUNCATE, LT("gui.gfx_custom_of"), b);
    } else {
        gfx_name(p, s, out, cap);
    }
}

void ui_settext_if(HWND h, const char *u8)
{
    char cur[600];
    if (!h) {
        return;
    }
    ui_gettext(h, cur, sizeof cur);
    if (strcmp(cur, u8) != 0) {
        ui_settext(h, u8);
        InvalidateRect(h, NULL, FALSE);
    }
}

/* W105 MONITOR-FX: Auswahl -> [render] monitor_grain/monitor_glitch (%). */
const int kMonFx[4][2] = { { 100, 100 }, { 100, 0 }, { 0, 100 }, { 0, 0 } };
int g_monfx_grain = 100, g_monfx_glitch = 100;
static int monfx_index(int grain, int glitch)
{
    return grain > 0 ? (glitch > 0 ? 0 : 1) : (glitch > 0 ? 2 : 3);
}
/* W252 CLEAR-VIEW: Eintrag "Aus" = monitor_fx=0 (gehoert zum Grafik-Preset);
 * sonst Staerken wie W105 (alte cfg mit 0/0 zeigt ebenfalls "Aus"). */
static int monfx_sel_now(void)
{
    return g_gfx.monitor_fx == 0 ? 3 : monfx_index(g_monfx_grain, g_monfx_glitch);
}

/* Steuerelemente der Seite Grafik aus g_gfx (Optionsfelder, Einzeloptionen). */
void gfx_sync_ui(void)
{
    lgp_mask_t lock = gfx_lock_now(), dm = 0;
    int p = lcfg_gfx_preset(&g_gfx, lock), i, fl, aa = g_gfx.shx[LSHX_AA];
    launcher_settings_t cur;
    char t[600], n[160];
    if (!ctl(ID_GP0)) {
        return;
    }
    cur = g_cur;
    if (ctl(ID_IPFPS)) { /* aktuelle Bildrate der Oberflaeche */
        int sel = ComboBox_GetCurSel(ctl(ID_IPFPS));
        cur.interp_fps = ip_val(sel, cur.interp_fps); /* W223, W309 */
        sel = ComboBox_GetCurSel(ctl(ID_WINMODE));
        cur.window_mode = (sel >= 0 && sel <= 2) ? sel : cur.window_mode;
    }
    g_gfx.interp_fps = cur.interp_fps;
    { int sel = ComboBox_GetCurSel(ctl(ID_RTSCALE));
      g_gfx.rt_scale_dyn = sel == 0;
      g_gfx.rt_scale = sel >= 2 && sel <= 5 ? sel - 1 : 0;
    }
    if (ctl(ID_EDGE)) { /* W266 PRESET-EDGE: Breitbild-Raender (Seite Anzeige) gehoeren zum Preset */
        launcher_settings_t e = g_gfx;
        edge_to_settings(&e);
        g_gfx.edge_mode = e.edge_mode;
        g_gfx.edge_fade_width = e.edge_fade_width;
        g_gfx.edge_fade_strength = e.edge_fade_strength;
    }
    p = lcfg_gfx_preset(&g_gfx, lock);
    g_updating++;
    for (i = 0; i <= LGP_CUSTOM; i++) {
        if (i < LGP_CUSTOM) {
            gfx_name(i, &cur, n, sizeof n);
            if (xexp_preset(i)) { /* W287: zugaenglicher Name, sichtbar ist das Symbol */
                char sfx[80];
                xexp_sfx(sfx, sizeof sfx);
                strncat_s(n, sizeof n, sfx, _TRUNCATE);
            }
            _snprintf_s(t, sizeof t, _TRUNCATE, "%s\n%s", n,
                        LT(i == 0 ? "gui.gfx_p_orig_txt" : i == 1 ? "gui.gfx_p_subtle_txt"
                           : i == 2 ? "gui.gfx_p_strong_txt" : i == LGP_PERFORMANCE ? "gui.gfx_p_performance_txt"
                           : i == LGP_ENHANCED ? "gui.gfx_p_enh_txt"
                           : i == LGP_VIVID ? "gui.gfx_p_vivid_txt" : "gui.gfx_p_clear_txt")); /* W465, W478 */
        } else if (p == LGP_CUSTOM) {
            int base = lcfg_gfx_base(&g_gfx, lock), k, hidden = 0;
            lcfg_gfx_diff(&g_gfx, base, lock, &dm);
            for (k = 0; k < LGK_N; k++) { /* Abweichungen ohne Element (ESC-Feinregler) */
                if ((dm & ((lgp_mask_t)1 << k)) && k != LSHX_SHADING && k != LSHX_SHADOW &&
                    k != LSHX_SOFT && k != LSHX_AA && k != LGK_HD && k < LGK_FPS &&
                    !(k >= LGK_FX0 && k < LGK_FX0 + LFX_N)) {
                    hidden++;
                }
            }
            gfx_name(base, &cur, n, sizeof n);
            {
                char sub[300];
                if (hidden) {
                    _snprintf_s(sub, sizeof sub, _TRUNCATE, LT("gui.gfx_p_custom_esc"), n, hidden);
                } else {
                    _snprintf_s(sub, sizeof sub, _TRUNCATE, LT("gui.gfx_p_custom_txt"), n);
                }
                _snprintf_s(t, sizeof t, _TRUNCATE, "%s\n%s", LT("gui.gfx_p_custom"), sub);
            }
        } else {
            _snprintf_s(t, sizeof t, _TRUNCATE, "%s\n%s", LT("gui.gfx_p_custom"),
                        LT("gui.gfx_p_custom_off"));
        }
        ui_settext_if(ctl(ID_GP0 + i), t);
        if (ui_checked(ID_GP0 + i) != (p == i)) {
            ui_set_checked(ID_GP0 + i, p == i);
        }
    }
    gfx_preset_layout(); /* W465: Lage folgt dem aktiven Spiel (Umschalten SB <-> LoC) */
    ComboBox_SetCurSel(ctl(ID_GFX_LIGHT), g_gfx.shx[LSHX_SHADING] ? 0 : 1);
    ComboBox_SetCurSel(ctl(ID_GFX_SHADOW), g_gfx.shx[LSHX_SHADOW] ? 1 : 0);
    ComboBox_SetCurSel(ctl(ID_GFX_SOFT), g_gfx.shx[LSHX_SOFT] ? 1 : 0);
    fl = lcfg_fx_level_game(&g_gfx, lock, g_game == LGAME_LOC);
    { /* Eintrag "Benutzerdefiniert" nur, wenn die Werte keiner Stufe entsprechen (W466: LoC eigene Stufen) */
        HWND c = ctl(ID_GFX_FX);
        static const char *const kFxS[] = { "gui.gfx_fx_orig", "gui.gfx_fx_subtle", "gui.gfx_fx_strong" };
        static const char *const kFxL[] = { "gui.gfx_fx_orig", "gui.gfx_p_enh", "gui.gfx_p_vivid" }; /* W478: + Vivid */
        int loc = g_game == LGAME_LOC, nst = 3, i;
        const char *const *lab = loc ? kFxL : kFxS;
        static int s_fx_game = -1; /* Liste zuletzt fuer dieses Spiel befuellt (Umschalten SB <-> LoC; W478: beide 3) */
        if (s_fx_game != loc || ComboBox_GetCount(c) < nst) {
            s_fx_game = loc;
            ComboBox_ResetContent(c);
            for (i = 0; i < nst; i++) { combo_add(c, LT(lab[i])); }
        }
        {
            int want = fl == LGP_CUSTOM ? nst + 1 : nst;
            while (ComboBox_GetCount(c) > want) {
                ComboBox_DeleteString(c, ComboBox_GetCount(c) - 1);
            }
            if (ComboBox_GetCount(c) < want) {
                combo_add(c, LT("gui.gfx_fx_custom"));
            }
        }
        ComboBox_SetCurSel(c, fl == LGP_CUSTOM ? nst : fl);
    }
    ComboBox_SetCurSel(ctl(ID_GFX_TEX), lcfg_gfx_value(&g_gfx, LGK_HD) ? 1 : 0);
    ComboBox_SetCurSel(ctl(ID_AAMODE), (aa >= 0 && aa <= 3) ? aa : 0);
    ComboBox_SetCurSel(ctl(ID_MONFX), monfx_sel_now()); /* W252: Preset setzt monitor_fx */
    ui_set_checked(ID_REM_LABEL, g_gfx.shx[LSHX_LABEL] ? 1 : 0); /* nicht im Preset */
    g_updating--;
}

/* Preset-Werte in die Einstellungen (Ableitungen: remaster = HD, fx_enhanced). */
static void gfx_to_settings(launcher_settings_t *s)
{
    memcpy(s->shx, g_gfx.shx, sizeof s->shx);
    memcpy(s->fx, g_gfx.fx, sizeof s->fx);
    s->fs_interp_match = g_gfx.fs_interp_match;
    s->remaster = g_gfx.remaster;
    s->hd_textures = g_gfx.hd_textures;
    s->gfx_custom = g_gfx.gfx_custom; /* W476: Custom ausdruecklich gewaehlt */
    /* W114: nur 2x/4x; die Freigaben aus der Bewertung gelten bei beiden */
    s->hd_scale = ComboBox_GetCurSel(ctl(ID_REM_SCALE)) == 0 ? 2 : 4;
    lcfg_gfx_normalize(s);
}


/* W319 MONITOR-VIEW: Auswahl "Cockpit" (Grafik > HUD & Cockpit): 0 Cockpit (Original), 1 Vollbild-Ansicht
 * des Spiels (cockpit_hide=1), 2 Monitoransicht, 3 + Radar/FRONT VIEW (monitor_view 1/2; hat Vorrang). */
static int cview_sel(int cockpit_hide, int monitor_view)
{
    if (monitor_view >= 1 && monitor_view <= 2) { return 1 + monitor_view; }
    return cockpit_hide ? 1 : 0;
}

void ui_from_settings(const launcher_settings_t *s)
{
    HWND c;
    g_updating++;
    g_setup_force = 0; /* W172: neue Werte -> Einrichtungskarte nur per Regel */
    { /* W172/P6: Programmdatei = <Spielordner>\default.xbe -> Feld leer
       * (automatisch, Hinweis im Feld); user.cfg bekommt beim Speichern
       * wieder den aufgeloesten Pfad (lcfg_save_user). */
        char x[1200];
        xbe_derived(s, x, sizeof x);
        ui_settext(ctl(ID_XBE), (x[0] && path_eq(x, s->xbe_path)) ? "" : s->xbe_path);
    }
    ui_settext(ctl(ID_MEDIA), s->media_root);
    ui_settext(ctl(ID_HDD), s->hdd_root);
    ui_settext(ctl(ID_IMAGE), s->game_image); /* W150 */
    if (s->game_image[0]) {
        check_image_ex(0); /* Ergebnis gleich sichtbar (Seite Pfade) */
    } else {
        g_img_msg[0] = 0;
    }
    set_num(ID_W, s->width);
    set_num(ID_H, s->height);
    ComboBox_SetCurSel(ctl(ID_VSYNC), s->present_interval ? 1 : 0);
    ComboBox_SetCurSel(ctl(ID_PACING), (s->frame_pacing >= 0 && s->frame_pacing <= 2) ? s->frame_pacing : 1); /* W468 */
    c = ctl(ID_FPS);
    ComboBox_SetCurSel(c, s->fps_limit == 30 ? 1 : s->fps_limit == 60 ? 2
                          : s->fps_limit == 120 ? 3 : 0);
    ui_set_checked(ID_AUDIO, s->audio);
    ui_set_checked(ID_LIVE, s->audio_live);
    ui_set_checked(ID_HRTF, s->audio_hrtf); /* W356 */
    { /* W303 AUDIO-VOLUME: auf 5 gerundet anzeigen, Rohwert merken (ui_to_settings behaelt ihn, wenn unveraendert) */
        const int vv[4] = { s->volume_master, s->volume_music, s->volume_sfx, s->volume_voice };
        int k;
        for (k = 0; k < 4; k++) {
            int v = vv[k] < 0 ? 0 : (vv[k] > 100 ? 100 : vv[k]);
            g_vol_raw[k] = v;
            ComboBox_SetCurSel(ctl(kVolId[k]), (100 - (v + 2) / 5 * 5) / 5);
        }
    }
    ui_set_checked(ID_SAVE_LOCK, s->save_lock); /* W190 */
    ui_settext(ctl(ID_PH_DIR), s->photo_dir); /* W195 PHOTO-MODE */
    { /* W290 QUICK-SHOT: Groesse Fenster/4K/8K/x2/x4/x8, Ansicht, Bildformat 4:3/breit, Replay-Schalter */
        int sz = lcfg_photo_size_eff(s);
        ComboBox_SetCurSel(ctl(ID_PH_RES), sz == -4 ? 1 : sz == -8 ? 2 : sz == 2 ? 3 : sz == 4 ? 4 : sz == 8 ? 5 : 0);
        ComboBox_SetCurSel(ctl(ID_PH_VIEW), (s->photo_view >= 0 && s->photo_view <= 3) ? s->photo_view : 0);
        ComboBox_SetCurSel(ctl(ID_PH_ASP), s->photo_aspect == 1 ? 1 : 0);
        ui_set_checked(ID_RP_LABEL, s->replay_label);
        ui_set_checked(ID_RP_LOGO, s->replay_logo);
        ui_set_checked(ID_WM_MEDIA, s->watermark_media);
        ui_set_checked(ID_PH_HUD, s->photo_hud);
        ComboBox_SetCurSel(ctl(ID_PH_CSELF), lcfg_cself_index(s->photo_cine_self)); /* W445 (Freigabe: ui_apply_rules) */
    }
    ComboBox_SetCurSel(ctl(ID_PH_FMT), s->photo_jpg ? 1 : 0);
    ui_set_checked(ID_ASPECT, s->aspect_mode ? 1 : 0);
    refresh_ctrl_profiles(s->input_profile);
    ui_set_checked(ID_CTRL_SBC, s->sbc_usb);
    ui_set_checked(ID_CTRL_MOUSEOFF, s->sbc_mouse_off);
    ComboBox_SetCurSel(ctl(ID_CTRL_AIMASSIST),
                       (s->aim_assist >= 0 && s->aim_assist <= 2) ? s->aim_assist : 0); /* W236 */
    ui_set_checked(ID_CTRL_AUTOGEAR, s->auto_gear);
    ui_set_checked(ID_CTRL_ALLY, s->sb_allied_squads); /* W424 */
    ui_set_checked(ID_CTRL_DESPIKE, s->ctrl_despike); /* W439 */
    ComboBox_SetCurSel(ctl(ID_CHIDE), cview_sel(s->cockpit_hide, s->monitor_view)); /* W268, W319 */
    ComboBox_SetCurSel(ctl(ID_STICK_AIM), s->stick_aim ? 1 : 0); /* W271 */
    ComboBox_SetCurSel(ctl(ID_STICK_SENS),
                       (s->stick_aim_sens >= 1 && s->stick_aim_sens <= 16)
                           ? s->stick_aim_sens - 1 : 3);
    ui_set_checked(ID_PAL50, s->game_tick == 25); /* W234 */
    ComboBox_SetCurSel(ctl(ID_IPFPS), -1); /* W309: Liste nach dem geladenen Wert */
    g_ip_keep = s->interp_fps;
    ip_fill(ctl(ID_IPFPS)); /* W234: "25 (Original)" bei PAL50 */
    ip_set(s->interp_fps); /* W223 */
    ComboBox_SetCurSel(ctl(ID_WINMODE),
                       (s->window_mode >= 0 && s->window_mode <= 2) ? s->window_mode : 0);
    /* W159: Kombi = Vorschau-Art (Startweg entscheidet an/aus, s. do_start) */
    ComboBox_SetCurSel(ctl(ID_VR),
                       _stricmp(s->vr_preview, "sbs") == 0 ? 0 :
                       _stricmp(s->vr_preview, "right") == 0 ? 2 :
                       (_stricmp(s->vr_preview, "off") == 0 || _stricmp(s->vr_preview, "xr") == 0) ? 3 : 1);
    { /* W183 VR-FIX3: Preset (0..2 = Leistung/Ausgewogen/Qualitaet, 3 = Eigene) + Aufloesung */
        int pr = lcfg_vr_preset_match(s);
        ComboBox_SetCurSel(ctl(ID_VRPRESET), pr >= 1 ? pr - 1 : 3);
        ComboBox_SetCurSel(ctl(ID_VRRES), vrres_index(s->vr_render_scale));
    }
    ComboBox_SetCurSel(ctl(ID_RTSCALE), s->rt_scale_dyn ? 0 : /* W148 */
                       (s->rt_scale <= 0 ? 1 : s->rt_scale <= 4 ? s->rt_scale + 1 : 5));
    {
        int k, hsel = 0;
        c = ctl(ID_FSHZ);
        /* unbekannte Rate (von Hand in user.cfg) bleibt als Roheintrag */
        while (ComboBox_GetCount(c) > KFSHZ_N) {
            ComboBox_DeleteString(c, KFSHZ_N);
        }
        for (k = 0; k < KFSHZ_N; k++) {
            if (kFsHz[k].hz == s->fs_hz) { hsel = k; }
        }
        if (s->fs_hz && kFsHz[hsel].hz != s->fs_hz) {
            char t[32];
            _snprintf_s(t, sizeof t, _TRUNCATE, "%d Hz", s->fs_hz);
            combo_add(c, t);
            hsel = KFSHZ_N;
        }
        ComboBox_SetCurSel(c, hsel);
    }
    ComboBox_SetCurSel(ctl(ID_MONFX), monfx_index(s->monitor_grain, s->monitor_glitch));
    g_monfx_grain = s->monitor_grain; /* W105: Zwischenwerte (von Hand) bleiben erhalten */
    g_monfx_glitch = s->monitor_glitch;
    edge_from_settings(s); /* W107 */
    g_gfx = *s; /* W173: alle Preset-Werte bleiben (Benutzerdefiniert) */
    lcfg_gfx_migrate(&g_gfx); /* alte remaster+fx_enhanced -> Effekte dezent */
    g_gfx.remaster = g_gfx.hd_textures = (s->remaster && s->hd_textures) ? 1 : 0;
    ComboBox_SetCurSel(ctl(ID_REM_SCALE), s->hd_scale == 2 ? 0 : 1); /* alt 0 -> 4x */
    ComboBox_SetCurSel(ctl(ID_MOUSE_MODE), s->mouse_mode ? 0 : 1);
    ComboBox_SetCurSel(ctl(ID_MOUSE_SENS),
                       (s->mouse_sens >= 1 && s->mouse_sens <= 16)
                           ? s->mouse_sens - 1 : 3);
    ComboBox_SetCurSel(ctl(ID_LANG), /* W142 */
                       _stricmp(s->lang, "de") == 0 ? 1 : 0);
    update_ctrl_overview();
    xbe_cue_update(); /* W172 */
    g_updating--;
    gfx_sync_ui(); /* W173 */
    sync_preset();
    update_aspect_info();
    ui_apply_rules(); /* W171: alle Freigaben/Begruendungen aus dem Regelwerk */
}

void ui_to_settings(launcher_settings_t *s)
{
    static const int fps_map[4] = { 0, 30, 60, 120 };
    int sel;
    ui_gettext(ctl(ID_XBE), s->xbe_path, sizeof s->xbe_path);
    ui_gettext(ctl(ID_MEDIA), s->media_root, sizeof s->media_root);
    ui_gettext(ctl(ID_HDD), s->hdd_root, sizeof s->hdd_root);
    ui_gettext(ctl(ID_IMAGE), s->game_image, sizeof s->game_image); /* W150 */
    lcfg_image_folder_mode(s); /* W369: .xbe/Spielordner im Abbild-Feld = Ordner-Modus */
    s->width = get_num(ID_W);
    s->height = get_num(ID_H);
    s->present_interval = ComboBox_GetCurSel(ctl(ID_VSYNC)) == 1 ? 1 : 0;
    { int ps = ComboBox_GetCurSel(ctl(ID_PACING)); s->frame_pacing = (ps >= 0 && ps <= 2) ? ps : 1; } /* W468 */
    sel = ComboBox_GetCurSel(ctl(ID_FPS));
    s->fps_limit = (sel >= 0 && sel < 4) ? fps_map[sel] : 0;
    s->audio = ui_checked(ID_AUDIO);
    s->audio_live = ui_checked(ID_LIVE);
    s->audio_hrtf = ui_checked(ID_HRTF) ? 1 : 0; /* W356 */
    { /* W303 AUDIO-VOLUME */
        int *vp[4] = { &s->volume_master, &s->volume_music, &s->volume_sfx, &s->volume_voice };
        int k;
        for (k = 0; k < 4; k++) {
            int sl = ComboBox_GetCurSel(ctl(kVolId[k])), v = (sl >= 0 && sl < VOL_N) ? 100 - sl * 5 : 100;
            *vp[k] = (v == (g_vol_raw[k] + 2) / 5 * 5) ? g_vol_raw[k] : v;
        }
    }
    s->save_lock = ui_checked(ID_SAVE_LOCK); /* W190 */
    { /* W195 PHOTO-MODE; W290 QUICK-SHOT: eine Konfiguration fuer Fotomodus und Schnell-Screenshot */
        static const int ph_map[6] = { 1, -4, -8, 2, 4, 8 };
        ui_gettext(ctl(ID_PH_DIR), s->photo_dir, sizeof s->photo_dir);
        trim_u8(s->photo_dir);
        sel = ComboBox_GetCurSel(ctl(ID_PH_RES));
        s->photo_size = (sel >= 0 && sel < 6) ? ph_map[sel] : -4;
        s->photo_scale = s->photo_size > 0 ? s->photo_size : 1; /* alter Schluessel (W195) passend mitschreiben */
        s->photo_jpg = ComboBox_GetCurSel(ctl(ID_PH_FMT)) == 1 ? 1 : 0;
        sel = ComboBox_GetCurSel(ctl(ID_PH_VIEW));
        s->photo_view = (sel >= 0 && sel <= 3) ? sel : 0;
        s->photo_aspect = ComboBox_GetCurSel(ctl(ID_PH_ASP)) == 1 ? 1 : 2;
        s->replay_label = ui_checked(ID_RP_LABEL) ? 1 : 0;
        s->replay_logo = ui_checked(ID_RP_LOGO) ? 1 : 0;
        s->watermark_media = ui_checked(ID_WM_MEDIA) ? 1 : 0;
        s->photo_hud = ui_checked(ID_PH_HUD) ? 1 : 0;
        sel = ComboBox_GetCurSel(ctl(ID_PH_CSELF)); /* W445: 0 = aus, k = LO + (k-1)*STEP */
        s->photo_cine_self = (sel > 0 && sel < LCFG_CSELF_N) ? LCFG_CSELF_LO + (sel - 1) * LCFG_CSELF_STEP : (sel == 0 ? 0 : LCFG_CSELF_DEF);
    }
    /* Bildformat fest (S3.10, nie strecken). */
    s->aspect_mode = 1;
    ctrl_profile_from_ui(s->input_profile, sizeof s->input_profile);
    s->sbc_usb = ui_checked(ID_CTRL_SBC);
    s->sbc_mouse_off = ui_checked(ID_CTRL_MOUSEOFF);
    s->aim_assist = ComboBox_GetCurSel(ctl(ID_CTRL_AIMASSIST)); /* W236 */
    if (s->aim_assist < 0 || s->aim_assist > 2) s->aim_assist = 0;
    s->auto_gear = ui_checked(ID_CTRL_AUTOGEAR);
    s->sb_allied_squads = ui_checked(ID_CTRL_ALLY); /* W424 */
    s->ctrl_despike = ui_checked(ID_CTRL_DESPIKE); /* W439 */
    sel = ComboBox_GetCurSel(ctl(ID_CHIDE)); /* W268, W319: 0 Cockpit, 1 Vollbild-Ansicht, 2/3 Monitoransicht (+ Felder) */
    s->cockpit_hide = sel == 1 ? 1 : 0;
    s->monitor_view = sel >= 2 && sel <= 3 ? sel - 1 : 0;
    s->stick_aim = ComboBox_GetCurSel(ctl(ID_STICK_AIM)) == 1 ? 1 : 0; /* W271 */
    sel = ComboBox_GetCurSel(ctl(ID_STICK_SENS));
    s->stick_aim_sens = (sel >= 0 && sel < 16) ? sel + 1 : 4;
    s->game_tick = ui_checked(ID_PAL50) && g_game != LGAME_LOC ? 25 : 30; /* W234; W402: LoC immer 30 (20-Hz-Spiel) */
    sel = ComboBox_GetCurSel(ctl(ID_IPFPS));
    s->interp_fps = ip_val(sel, 0); /* W223, W309 */
    sel = ComboBox_GetCurSel(ctl(ID_WINMODE));
    s->window_mode = (sel >= 0 && sel <= 2) ? sel : 0;
    /* W159: Kombi setzt nur die Vorschau-Art; s->vr_enable bleibt wie
     * geladen (alte user.cfg mit 1 aendert keinen Startweg mehr). */
    sel = ComboBox_GetCurSel(ctl(ID_VR));
    {
        static const char *const prev[4] = { "sbs", "left", "right", "off" };
        if (sel >= 0 && sel <= 3) {
            strcpy_s(s->vr_preview, sizeof s->vr_preview, prev[sel]);
        }
    }
    { /* W183 VR-FIX3: Preset setzt die sechs Werte, die Aufloesung-Kombi gilt zuletzt */
        int ps = ComboBox_GetCurSel(ctl(ID_VRPRESET)), rs = ComboBox_GetCurSel(ctl(ID_VRRES));
        if (ps >= 0 && ps <= 2 && lcfg_vr_preset_match(s) != ps + 1) { lcfg_vr_preset_apply(s, ps + 1); }
        if (rs >= 0 && rs < KVRRES_N) { s->vr_render_scale = kVrRes[rs]; }
        s->vr_preset = lcfg_vr_preset_match(s);
    }
    sel = ComboBox_GetCurSel(ctl(ID_RTSCALE)); /* W148 */
    if (sel == 0) { s->rt_scale_dyn = 1; s->rt_scale = 0; }
    else if (sel == 1) { s->rt_scale_dyn = 0; s->rt_scale = 0; }
    else if (sel >= 2 && sel <= 5) { s->rt_scale_dyn = 0; s->rt_scale = sel - 1; }
    sel = ComboBox_GetCurSel(ctl(ID_MONFX));
    s->monitor_fx = g_gfx.monitor_fx; /* W252: gehoert zum Grafik-Preset */
    s->monitor_grain = g_monfx_grain;
    s->monitor_glitch = g_monfx_glitch;
    if (sel >= 0 && sel < 4 && sel != monfx_sel_now()) { /* Auswahl ohne Ereignis (Tests) */
        if (sel == 3) {
            s->monitor_fx = 0;
        } else {
            s->monitor_fx = 1;
            s->monitor_grain = kMonFx[sel][0];
            s->monitor_glitch = kMonFx[sel][1];
        }
    }
    sel = ComboBox_GetCurSel(ctl(ID_FSHZ));
    if (sel >= 0 && sel < KFSHZ_N) {
        s->fs_hz = kFsHz[sel].hz;
    } else if (sel == KFSHZ_N) {
        char t[32];
        ui_gettext(ctl(ID_FSHZ), t, sizeof t);
        s->fs_hz = atoi(t);
    } else {
        s->fs_hz = 0;
    }
    edge_to_settings(s); /* W107 */
    s->mouse_mode = ComboBox_GetCurSel(ctl(ID_MOUSE_MODE)) == 1 ? 0 : 1;
    sel = ComboBox_GetCurSel(ctl(ID_MOUSE_SENS));
    s->mouse_sens = (sel >= 0 && sel < 16) ? sel + 1 : 4;
    sel = ComboBox_GetCurSel(ctl(ID_LANG)); /* W142 */
    if (sel == 1) {
        strcpy_s(s->lang, sizeof s->lang, "de");
    } else if (sel == 0) {
        strcpy_s(s->lang, sizeof s->lang, "en");
    }
    gfx_to_settings(s); /* W173: einheitliches Grafik-Preset */
}

/* Kurzstatus der Startseite aus dem aktuellen Oberflaechen-Stand. */
void update_summary(void)
{
    launcher_settings_t s;
    s = g_s;
    ui_to_settings(&s);
    /* W172/P6: Zeilen Spiel, Grafik, Anzeige, Packs, Steuerung; jede mit
     * "Aendern" (Sprung zur Seite der Ursache). Client fehlt = Startsperre
     * R4 unter den Startknoepfen, Einstellungsdatei = Erweitert > Diagnose. */
    g_sum[0].label = LT("gui.sum_game");
    /* W220: Ergebnis der Spieldaten-Pruefung aus dem Regelwerk (rules_data,
     * je Pfadsatz gemerkt) statt erneut das Abbild zu lesen */
    if (g_data_ok) {
        if (s.game_image[0]) { /* W150: Abbild-Modus */
            const char *b = s.game_image + strlen(s.game_image);
            while (b > s.game_image && b[-1] != '\\' && b[-1] != '/') {
                b--;
            }
            _snprintf_s(g_sum[0].value, sizeof g_sum[0].value, _TRUNCATE,
                        LT("gui.play_sum_image"), b);
        } else {
            _snprintf_s(g_sum[0].value, sizeof g_sum[0].value, _TRUNCATE,
                        LT("gui.play_sum_folder"), s.media_root);
        }
        if (g_edition == LCFG_ED_US || g_edition == LCFG_ED_EU) { /* W231: Fassung */
            strcat_s(g_sum[0].value, sizeof g_sum[0].value, " \xC2\xB7 ");
            strcat_s(g_sum[0].value, sizeof g_sum[0].value,
                     LT(g_edition == LCFG_ED_US ? "gui.ed_us_exp" : "gui.ed_eu"));
        }
        g_sum[0].col = C_OK;
    } else {
        strncpy_s(g_sum[0].value, sizeof g_sum[0].value,
                 g_data_err[0] ? g_data_err : LT("gui.rule_r3"), _TRUNCATE);
        g_sum[0].col = C_ERR;
    }
    g_sum[2].label = LT("gui.page_display");
    {
        char fpsb[48]; /* W223: alle Raten + Monitor (auto) */
        const char *fps = fpsb;
        if (s.interp_fps == 0) { fps = LT(s.game_tick == 25 ? "gui.sum_25" : "gui.sum_30"); } /* W234 */
        else if (s.interp_fps == SB_FPS_AUTO) { fps = LT("gui.sum_auto"); }
        else if (s.interp_fps == 60) { fps = LT("gui.sum_60"); }
        else if (s.interp_fps == 120) { fps = LT("gui.sum_120"); }
        else { _snprintf_s(fpsb, sizeof fpsb, _TRUNCATE, "%d FPS", s.interp_fps); }
        if (s.window_mode == 1 || s.window_mode == 2) {
            _snprintf_s(g_sum[2].value, sizeof g_sum[2].value, _TRUNCATE,
                        LT("gui.play_sum_fs"),
                        s.window_mode == 1 ? LT("gui.wm_borderless") : LT("gui.wm_excl"),
                        fps);
        } else {
            _snprintf_s(g_sum[2].value, sizeof g_sum[2].value, _TRUNCATE,
                        LT("gui.play_sum_win"), s.width, s.height, fps);
        }
    }
    g_sum[2].col = (s.width >= 320 && s.height >= 200) ? C_OK : C_ERR;
    if (g_rr[10].active) { /* R10: 120 fps auf langsamerem Monitor */
        g_sum[2].col = C_WARN;
    }
    g_sum[3].label = LT("gui.page_packs");
    {
        int i, act = 0, warn = 0, bad = 0, tex = 0;
        for (i = 0; i < packs_count(); i++) {
            int st = packs_status_of(i);
            if (!packs_enabled_of(i)) {
                continue;
            }
            if (st < 1) {
                bad++;
                continue;
            }
            act++;
            warn += st == 1;
            tex += !packs_is_fx(i);
        }
        if (packs_count() == 0) {
            strncpy_s(g_sum[3].value, sizeof g_sum[3].value, LT("gui.play_sum_nopacks"), _TRUNCATE);
            g_sum[3].col = C_FAINT;
        } else if (act == 0) {
            strncpy_s(g_sum[3].value, sizeof g_sum[3].value, LT("gui.play_sum_packsoff"), _TRUNCATE);
            g_sum[3].col = bad ? C_WARN : C_FAINT;
        } else {
            char lv[32];
            strcpy_s(lv, sizeof lv, packs_level_word());
            if (tex) {
                _snprintf_s(g_sum[3].value, sizeof g_sum[3].value, _TRUNCATE,
                            LT("gui.play_sum_packs"), act, lv);
            } else {
                _snprintf_s(g_sum[3].value, sizeof g_sum[3].value, _TRUNCATE,
                            LT("gui.play_sum_fxonly"), act);
            }
            if (warn || bad) {
                size_t n = strlen(g_sum[3].value);
                _snprintf_s(g_sum[3].value + n, sizeof g_sum[3].value - n, _TRUNCATE,
                            LT("gui.play_sum_packwarn"), warn + bad);
            }
            g_sum[3].col = (warn || bad) ? C_WARN : C_OK;
        }
    }
    g_sum[1].label = LT("gui.page_graphics");
    gfx_state_name(&s, g_sum[1].value, sizeof g_sum[1].value); /* W173: ein Preset */
    g_sum[1].col = C_OK;
    g_sum[4].label = LT("gui.page_controls");
    if (s.input_profile[0]) {
        _snprintf_s(g_sum[4].value, sizeof g_sum[4].value, _TRUNCATE, LT("gui.sum_prof"), s.input_profile,
                    s.sbc_usb ? LT("gui.sum_sbc") : "");
    } else {
        _snprintf_s(g_sum[4].value, sizeof g_sum[4].value, _TRUNCATE, LT("gui.sum_legacy"),
                    s.sbc_usb ? LT("gui.sum_sbc") : "");
    }
    g_sum[4].col = g_rr[25].active ? C_WARN : C_OK; /* R25: Controller fehlt */
}

/* ------------------------------------------------------------------ */
