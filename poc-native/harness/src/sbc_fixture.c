/* sbc_fixture.c - Reports aus einer Textdatei (W75, s. sbc_fixture.h). */
#include "sbc_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *sbc_fixture_strerror(int err)
{
    switch (err) {
    case 0: return "ok";
    case -1: return "Datei fehlt/nicht lesbar";
    case -2: return "kein Speicher";
    case -3: return "kein gueltiger Dateneintrag";
    default: return "unbekannter Fehler";
    }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Rest der Zeile -> 26 B (Hexziffern, Rest ignoriert). 0 = ok. */
static int parse_hex26(const char *p, uint8_t out[26])
{
    unsigned n = 0;
    int hi = -1;
    for (; *p && *p != '#' && *p != '\r' && *p != '\n'; p++) {
        int v = hexval(*p);
        if (v < 0) {
            continue; /* Leerzeichen u.a. Deko */
        }
        if (hi < 0) {
            hi = v;
        } else {
            if (n >= 26) {
                return -1; /* zu lang */
            }
            out[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    if (hi >= 0 || n != 26) {
        return -1;
    }
    if (out[0] != 0x00 || out[1] != 0x1A) {
        return -1; /* IN-Header 00 1A (wie sbc_usb_parse_report) */
    }
    return 0;
}

/* Adressliste "0xA,0xB" -> addrs. 0 = ok, <0 = Fehler. */
static int parse_addrs(char *p, uint32_t *addrs, unsigned *naddr)
{
    unsigned n = 0;
    char *tok, *ctx = NULL;
    for (tok = strtok_s(p, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        char *end = NULL;
        unsigned long v;
        while (*tok == ' ' || *tok == '\t') {
            tok++;
        }
        if (!*tok || n >= SBC_FIX_MEMALL) {
            return -1;
        }
        v = strtoul(tok, &end, 0);
        if (end == tok) {
            return -1;
        }
        addrs[n++] = (uint32_t)v;
    }
    if (!n) {
        return -1;
    }
    *naddr = n;
    return 0;
}

int sbc_fixture_load(const char *path, sbc_fixture_t *fx)
{
    FILE *f = NULL;
    char line[1024];
    sbc_fix_entry_t *e = NULL;
    sbc_fix_anchor_t *a = NULL;
    unsigned n = 0, na = 0, skipped = 0;
    int cur_anchor = -1;

    if (!path || !fx) {
        return -1;
    }
    memset(fx, 0, sizeof *fx);
    if (fopen_s(&f, path, "r") != 0 || !f) {
        return -1;
    }
    e = (sbc_fix_entry_t *)calloc(SBC_FIX_MAX, sizeof *e);
    a = (sbc_fix_anchor_t *)calloc(SBC_FIX_ANCHORS, sizeof *a);
    if (!e || !a) {
        free(e);
        free(a);
        fclose(f);
        return -2;
    }
    while (fgets(line, sizeof line, f)) {
        char *p = line + strspn(line, " \t");
        if (*p == '#' || *p == '\r' || *p == '\n' || *p == '\0') {
            continue;
        }
        if (strncmp(p, "memall:", 7) == 0 || strncmp(p, "mem:", 4) == 0) {
            /* Ankerzeile: mem:0xA=0xV | memall:0xA,...=LINIE */
            int is_all = p[3] == 'a';
            char *eq;
            sbc_fix_anchor_t *an;
            if (na >= SBC_FIX_ANCHORS) {
                skipped++;
                continue;
            }
            eq = strchr(p, '=');
            if (!eq) {
                skipped++;
                continue;
            }
            *eq = '\0';
            an = &a[na];
            memset(an, 0, sizeof *an);
            an->is_memall = is_all;
            if (parse_addrs(p + (is_all ? 7 : 4), an->addr, &an->naddr) != 0) {
                skipped++;
                continue;
            }
            if (is_all && an->naddr > SBC_FIX_MEMALL) {
                skipped++;
                continue;
            }
            if (!is_all && an->naddr != 1) {
                skipped++;
                continue;
            }
            if (is_all) {
                /* LINIE: Float ("0.7") oder Hex-Bits ("0x3F333333"). */
                char *v = eq + 1;
                if (strchr(v, '.') || strchr(v, 'e') || strchr(v, 'E')) {
                    an->line = (float)strtod(v, NULL);
                } else {
                    uint32_t bits = (uint32_t)strtoul(v, NULL, 0);
                    memcpy(&an->line, &bits, sizeof bits);
                }
            } else {
                an->val = (uint32_t)strtoul(eq + 1, NULL, 0);
            }
            cur_anchor = (int)na;
            na++;
            continue;
        }
        {
            /* Datenzeile: <when> <hex26>. */
            char *sp;
            uint64_t when = 0;
            int is_ms = 0;
            sp = p + strcspn(p, " \t");
            if (*sp) {
                *sp = '\0';
            }
            {
                size_t L = strlen(p);
                if (L > 2 && p[L - 2] == 'm' && p[L - 1] == 's') {
                    is_ms = 1;
                    p[L - 2] = '\0';
                } else if (L > 1 && (p[0] == 'f' || p[0] == 'F')) {
                    p++;
                }
                when = _strtoui64(p, NULL, 10);
            }
            if (n >= SBC_FIX_MAX) {
                skipped++;
                continue;
            }
            if (parse_hex26(sp + 1, e[n].raw) != 0) {
                skipped++;
                continue;
            }
            e[n].when = when;
            e[n].is_ms = is_ms;
            e[n].anchor = is_ms ? -1 : cur_anchor;
            n++;
        }
    }
    fclose(f);
    if (!n) {
        free(e);
        free(a);
        fx->skipped = skipped;
        return -3;
    }
    fx->e = e;
    fx->n = n;
    fx->a = a;
    fx->na = na;
    fx->skipped = skipped;
    return 0;
}

void sbc_fixture_free(sbc_fixture_t *fx)
{
    if (!fx) {
        return;
    }
    free(fx->e);
    free(fx->a);
    memset(fx, 0, sizeof *fx);
}

static int anchor_true(const sbc_fix_anchor_t *an, sbc_fix_memread_fn memread)
{
    unsigned i;
    if (!memread) {
        return 0;
    }
    if (an->is_memall) {
        for (i = 0; i < an->naddr; i++) {
            uint32_t bits = memread(an->addr[i]);
            float v;
            memcpy(&v, &bits, sizeof v);
            if (!(v > an->line)) {
                return 0; /* strikt >, alle (wie replay memall) */
            }
        }
        return 1;
    }
    return memread(an->addr[0]) == an->val;
}

int sbc_fixture_at(sbc_fixture_t *fx, unsigned frame, uint64_t ms,
                   sbc_fix_memread_fn memread, unsigned *idx)
{
    unsigned i;
    int best = -1;
    if (!fx || !fx->e || !idx) {
        return 0;
    }
    /* Anker pruefen + feuern: der Reihe nach (n+1 erst nach n), je auf
     * die erste Flanke falsch->wahr. Reine Pegel wuerden auf
     * Null-speicher feuern (Beleg W75_s2fix: f30-Anker bei f=23 statt
     * am Missionsbeginn); replay_steps kennt das Problem nicht, weil
     * dort Schritte sequenziert sind. */
    for (i = 0; i < fx->na; i++) {
        sbc_fix_anchor_t *an = &fx->a[i];
        int t;
        if (an->fired) {
            continue;
        }
        if (i > 0 && !fx->a[i - 1].fired) {
            continue;
        }
        t = anchor_true(an, memread);
        if (!t) {
            an->seen_false = 1;
            continue;
        }
        if (!an->seen_false) {
            continue;
        }
        an->fired = 1;
        an->fire_frame = frame;
    }
    /* Letzter aktiver Eintrag in Dateireihenfolge gewinnt. */
    for (i = 0; i < fx->n; i++) {
        const sbc_fix_entry_t *en = &fx->e[i];
        if (en->is_ms) {
            if (en->when <= ms) {
                best = (int)i;
            }
            continue;
        }
        if (en->anchor >= 0) {
            const sbc_fix_anchor_t *an = &fx->a[en->anchor];
            uint64_t at;
            if (!an->fired) {
                continue;
            }
            at = (uint64_t)an->fire_frame + en->when;
            if (at <= frame) {
                best = (int)i;
            }
            continue;
        }
        if (en->when <= frame) {
            best = (int)i;
        }
    }
    if (best < 0) {
        return 0;
    }
    *idx = (unsigned)best;
    return 1;
}
