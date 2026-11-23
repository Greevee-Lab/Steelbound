/* loc_chat.c - W437 LOC-CHAT: reiner Kern (Paket, Bereinigung, Ratenbremse, Stummschalten, Verlauf, Eingabezeile).
 * Keine Abhaengigkeit von Spiel, Fenster oder Netz (loc_chat_selftest, fuzz_chat). Nichts wird geloggt. */
#include "loc_chat.h"

#include <string.h>

static void lch_zero(void *p, size_t n) /* nicht wegoptimierbar */
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static unsigned get16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------ Paket */
int lch_build(uint8_t *out, int cap, uint32_t nonce, uint32_t seq, const uint8_t *text, int len)
{
    if (!out || !text || len < 1 || len > LCH_MAX_TEXT_BYTES || cap < LCH_HDR + len || !nonce || !seq) return 0;
    out[0] = LCH_MAGIC0;
    out[1] = LCH_MAGIC1;
    out[2] = LCH_MAGIC2;
    out[3] = LCH_MAGIC3;
    out[4] = LCH_VER;
    out[5] = LCH_KIND_TEXT;
    put16(out + 6, (unsigned)len);
    put32(out + 8, nonce);
    put32(out + 12, seq);
    memcpy(out + LCH_HDR, text, (size_t)len);
    return LCH_HDR + len;
}

int lch_parse(const uint8_t *d, int len, lch_pkt *o)
{
    unsigned tl;
    if (!d || !o || len < LCH_HDR + 1 || len > LCH_PKT_MAX) return 0;
    if (d[0] != LCH_MAGIC0 || d[1] != LCH_MAGIC1 || d[2] != LCH_MAGIC2 || d[3] != LCH_MAGIC3) return 0;
    if (d[4] != LCH_VER || d[5] != LCH_KIND_TEXT) return 0;
    tl = get16(d + 6);
    if (tl < 1 || tl > LCH_MAX_TEXT_BYTES || (int)tl != len - LCH_HDR) return 0;
    o->nonce = get32(d + 8);
    o->seq = get32(d + 12);
    if (!o->nonce || !o->seq) return 0;
    o->len = (int)tl;
    o->text = d + LCH_HDR;
    return 1;
}

/* ------------------------------------------------------------------ Zeichenregeln */
static int lch_cp_space(uint32_t cp)
{
    return cp == 0x20 || cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F ||
           cp == 0x205F || cp == 0x3000;
}

int lch_cp_combining(uint32_t cp)
{
    return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x0483 && cp <= 0x0489) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
           (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE20 && cp <= 0xFE2F);
}

int lch_cp_ok(uint32_t cp)
{
    if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) return 0;           /* C0, DEL, C1 */
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;     /* ausserhalb, Surrogate */
    if ((cp & 0xFFFEu) == 0xFFFEu || (cp >= 0xFDD0 && cp <= 0xFDEF)) return 0; /* Nichtzeichen */
    if ((cp >= 0xE000 && cp <= 0xF8FF) || cp >= 0xF0000) return 0;    /* private Nutzung */
    if (cp >= 0xE0000 && cp <= 0xE0FFF) return 0;                     /* Tag-Zeichen, Variantenwahl Zusatz */
    switch (cp) {
    case 0x00AD: /* weiches Trennzeichen */
    case 0x034F: /* Grapheme Joiner */
    case 0x061C: /* Arabic Letter Mark (Bidi) */
    case 0x115F: case 0x1160: case 0x3164: case 0xFFA0: /* Hangul-Fueller (unsichtbar) */
    case 0x17B4: case 0x17B5:
    case 0xFEFF: /* BOM / ZWNBSP */
        return 0;
    default:
        break;
    }
    if (cp >= 0x180B && cp <= 0x180F) return 0; /* Mongolische Variantenwahl, MVS */
    if (cp >= 0x200B && cp <= 0x200F) return 0; /* ZWSP, ZWNJ, ZWJ, LRM, RLM */
    if (cp >= 0x2028 && cp <= 0x202E) return 0; /* Zeilen-/Absatztrenner, LRE RLE PDF LRO RLO */
    if (cp >= 0x2060 && cp <= 0x206F) return 0; /* WJ, unsichtbare Operatoren, LRI RLI FSI PDI, veraltete Formate */
    if (cp >= 0xFE00 && cp <= 0xFE0F) return 0; /* Variantenwahl */
    if (cp >= 0xFFF0 && cp <= 0xFFFC) return 0; /* Interlinear-Anker, Objekt-Ersatz */
    if (cp >= 0x1BCA0 && cp <= 0x1BCA3) return 0; /* Kurzschrift-Formatzeichen */
    if (cp >= 0x1D173 && cp <= 0x1D17A) return 0; /* Noten-Formatzeichen */
    if (cp >= 0x13430 && cp <= 0x1343F) return 0; /* Hieroglyphen-Formatzeichen */
    return 1;
}

/* Bereiniger als kleiner Zustandsautomat (UTF-8- und Codepunkt-Weg teilen ihn) */
typedef struct lch_san {
    uint32_t *out;
    int n, max, space, comb, full;
} lch_san;

static void san_push(lch_san *s, uint32_t cp)
{
    if (s->full) return;
    if (lch_cp_space(cp)) {
        if (s->n > 0) s->space = 1;
        s->comb = 0;
        return;
    }
    if (!lch_cp_ok(cp)) return; /* entfernt (auch zwischen Zeichen, aendert die Kombinierkette nicht) */
    if (lch_cp_combining(cp)) {
        if (s->n == 0 && !s->space) return; /* kein Grundzeichen */
        if (s->comb >= 2) return;           /* Stapel begrenzen */
        s->comb++;
    } else {
        s->comb = 0;
    }
    if (s->space) {
        if (s->n + 2 > s->max) {
            s->full = 1;
            return;
        }
        s->out[s->n++] = 0x20;
        s->space = 0;
    }
    if (s->n >= s->max) {
        s->full = 1;
        return;
    }
    s->out[s->n++] = cp;
}

static void san_init(lch_san *s, uint32_t *out, int cap)
{
    memset(s, 0, sizeof *s);
    s->out = out;
    s->max = cap < LCH_MAX_CHARS ? cap : LCH_MAX_CHARS;
    if (s->max < 0) s->max = 0;
}

int lch_sanitize_cps(const uint32_t *in, int n, uint32_t *cps, int cap)
{
    lch_san s;
    int i;
    if (!cps || cap <= 0) return 0;
    san_init(&s, cps, cap);
    for (i = 0; in && i < n && !s.full; i++) san_push(&s, in[i]);
    return s.n;
}

int lch_sanitize_u8(const uint8_t *in, int n, uint32_t *cps, int cap)
{
    lch_san s;
    int i = 0;
    if (!cps || cap <= 0) return 0;
    san_init(&s, cps, cap);
    while (in && i < n && !s.full) {
        unsigned b = in[i], need, k;
        uint32_t cp, lo = 0x80, hi = 0xBF;
        if (b < 0x80) {
            san_push(&s, b);
            i++;
            continue;
        }
        if (b >= 0xC2 && b <= 0xDF) {
            need = 1;
            cp = b & 0x1Fu;
        } else if (b >= 0xE0 && b <= 0xEF) {
            need = 2;
            cp = b & 0x0Fu;
            if (b == 0xE0) lo = 0xA0; /* ueberlang */
            if (b == 0xED) hi = 0x9F; /* Surrogate */
        } else if (b >= 0xF0 && b <= 0xF4) {
            need = 3;
            cp = b & 0x07u;
            if (b == 0xF0) lo = 0x90; /* ueberlang */
            if (b == 0xF4) hi = 0x8F; /* > U+10FFFF */
        } else {
            i++; /* ungueltiges Startbyte */
            continue;
        }
        if (i + (int)need >= n) { /* nicht genug Bytes (Folgebytes i+1 .. i+need) */
            i++;
            continue;
        }
        for (k = 1; k <= need; k++) {
            unsigned c = in[i + (int)k];
            unsigned l = k == 1 ? lo : 0x80, h = k == 1 ? hi : 0xBF;
            if (c < l || c > h) break;
            cp = (cp << 6) | (c & 0x3Fu);
        }
        if (k <= need) { /* kaputte Folge: nur das Startbyte verwerfen */
            i++;
            continue;
        }
        san_push(&s, cp);
        i += (int)need + 1;
    }
    return s.n;
}

int lch_cps_to_u8(const uint32_t *cps, int n, uint8_t *out, int cap)
{
    int i, o = 0;
    for (i = 0; i < n; i++) {
        uint32_t c = cps[i];
        int need = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return -1;
        if (o + need > cap) return -1;
        if (need == 1) {
            out[o++] = (uint8_t)c;
        } else if (need == 2) {
            out[o++] = (uint8_t)(0xC0 | (c >> 6));
            out[o++] = (uint8_t)(0x80 | (c & 0x3F));
        } else if (need == 3) {
            out[o++] = (uint8_t)(0xE0 | (c >> 12));
            out[o++] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (uint8_t)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (uint8_t)(0xF0 | (c >> 18));
            out[o++] = (uint8_t)(0x80 | ((c >> 12) & 0x3F));
            out[o++] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (uint8_t)(0x80 | (c & 0x3F));
        }
    }
    return o;
}

int lch_cps_to_w(const uint32_t *cps, int n, wchar_t *out, int cap)
{
    int i, o = 0;
    if (!out || cap <= 0) return -1;
    for (i = 0; i < n; i++) {
        uint32_t c = cps[i];
        if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
            out[o] = 0;
            return -1;
        }
        if (c >= 0x10000) {
            if (o + 2 >= cap) break;
            c -= 0x10000;
            out[o++] = (wchar_t)(0xD800 + (c >> 10));
            out[o++] = (wchar_t)(0xDC00 + (c & 0x3FF));
        } else {
            if (o + 1 >= cap) break;
            out[o++] = (wchar_t)c;
        }
    }
    out[o] = 0;
    return o;
}

void lch_name_clean(const char *in, char *out, int cap)
{
    int i, o = 0, lim;
    if (!out || cap <= 0) return;
    lim = cap - 1 < LCH_NAME_MAX ? cap - 1 : LCH_NAME_MAX;
    for (i = 0; in && in[i] && o < lim; i++) {
        unsigned char c = (unsigned char)in[i];
        out[o++] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    if (o == 0 && cap >= 2) out[o++] = '?';
    out[o] = 0;
}

/* ------------------------------------------------------------------ Ratenbremse */
int lch_rate_take(lch_rate *r, uint32_t key, uint32_t now)
{
    int i, slot = -1, old = 0, k, live = 0;
    uint32_t oldest_age = 0;
    for (i = 0; i < LCH_RATE_SLOTS; i++) {
        if (r->e[i].used && r->e[i].key == key) {
            slot = i;
            break;
        }
    }
    if (slot < 0) { /* freier Platz bzw. am laengsten ungenutzter (der kann hoechstens wieder 3 senden) */
        for (i = 0; i < LCH_RATE_SLOTS; i++) {
            uint32_t age;
            if (!r->e[i].used) {
                slot = i;
                break;
            }
            age = now - r->e[i].last;
            if (age >= oldest_age) {
                oldest_age = age;
                old = i;
            }
        }
        if (slot < 0) slot = old;
        memset(&r->e[slot], 0, sizeof r->e[slot]);
        r->e[slot].used = 1;
        r->e[slot].key = key;
    }
    for (k = 0; k < r->e[slot].n; k++) {
        if ((uint32_t)(now - r->e[slot].t[k]) < LCH_RATE_MS) live++;
    }
    r->e[slot].last = now;
    if (live >= LCH_RATE_N) return 0;
    /* aelteste Zeit ersetzen (Ring der letzten LCH_RATE_N erlaubten) */
    if (r->e[slot].n < LCH_RATE_N) {
        r->e[slot].t[r->e[slot].n++] = now;
    } else {
        int o = 0;
        for (k = 1; k < LCH_RATE_N; k++) {
            if ((uint32_t)(now - r->e[slot].t[k]) > (uint32_t)(now - r->e[slot].t[o])) o = k;
        }
        r->e[slot].t[o] = now;
    }
    return 1;
}

/* ------------------------------------------------------------------ Stummschalten */
static int name_eq(const char *a, const char *b)
{
    int i;
    for (i = 0; i <= LCH_NAME_MAX; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
        if (!x) return 1;
    }
    return 0;
}

int lch_mute_add(lch_mute *m, const char *name)
{
    char c[LCH_NAME_MAX + 1];
    int i;
    if (!name || !name[0] || strlen(name) > LCH_NAME_MAX) return -1;
    lch_name_clean(name, c, (int)sizeof c);
    for (i = 0; i < m->n; i++) {
        if (name_eq(m->name[i], c)) return 0;
    }
    if (m->n >= LCH_MUTE_MAX) return -1;
    memcpy(m->name[m->n++], c, sizeof c);
    return 1;
}

int lch_mute_del(lch_mute *m, const char *name)
{
    char c[LCH_NAME_MAX + 1];
    int i;
    if (!name || !name[0] || strlen(name) > LCH_NAME_MAX) return 0;
    lch_name_clean(name, c, (int)sizeof c);
    for (i = 0; i < m->n; i++) {
        if (name_eq(m->name[i], c)) {
            if (i != m->n - 1) memcpy(m->name[i], m->name[m->n - 1], sizeof m->name[i]);
            lch_zero(m->name[m->n - 1], sizeof m->name[0]);
            m->n--;
            return 1;
        }
    }
    return 0;
}

int lch_muted(const lch_mute *m, const char *name)
{
    char c[LCH_NAME_MAX + 1];
    int i;
    if (!name) return 0;
    lch_name_clean(name, c, (int)sizeof c);
    for (i = 0; i < m->n; i++) {
        if (name_eq(m->name[i], c)) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ Verlauf */
void lch_hist_push(lch_hist *h, int kind, const char *name, const wchar_t *text, uint32_t now)
{
    lch_line *l = &h->l[h->head];
    int i;
    lch_zero(l, sizeof *l);
    for (i = 0; name && name[i] && i < LCH_NAME_MAX; i++) l->name[i] = (wchar_t)(unsigned char)name[i];
    l->name[i] = 0;
    for (i = 0; text && text[i] && i < LCH_LINE_W - 1; i++) l->text[i] = text[i];
    l->text[i] = 0;
    l->t = now;
    l->kind = kind;
    h->head = (h->head + 1) % LCH_HIST;
    if (h->n < LCH_HIST) h->n++;
    h->gen++;
}

const lch_line *lch_hist_get(const lch_hist *h, int k)
{
    if (k < 0 || k >= h->n) return 0;
    return &h->l[(h->head - 1 - k + 2 * LCH_HIST) % LCH_HIST];
}

/* ------------------------------------------------------------------ Eingabezeile */
void lch_input_clear(lch_input *in) { lch_zero(in, sizeof *in); }

int lch_input_wchar(lch_input *in, unsigned u)
{
    uint32_t cp;
    if (u >= 0xD800 && u <= 0xDBFF) { /* hohes Surrogat: auf das niedrige warten */
        in->hs = (uint16_t)u;
        return 0;
    }
    if (u >= 0xDC00 && u <= 0xDFFF) {
        if (!in->hs) return 0;
        cp = 0x10000u + (((uint32_t)in->hs - 0xD800u) << 10) + (u - 0xDC00u);
        in->hs = 0;
    } else {
        in->hs = 0;
        cp = u;
    }
    if (lch_cp_space(cp)) cp = 0x20;
    if (!lch_cp_ok(cp) || in->n >= LCH_MAX_CHARS) return 0;
    in->cp[in->n++] = cp;
    return 1;
}

void lch_input_back(lch_input *in)
{
    in->hs = 0;
    if (in->n > 0) in->cp[--in->n] = 0;
}

int lch_input_w(const lch_input *in, wchar_t *out, int cap) { return lch_cps_to_w(in->cp, in->n, out, cap); }

/* ------------------------------------------------------------------ Befehle */
int lch_cmd(const lch_input *in, char *arg, int cap)
{
    char w[16];
    int i = 1, k = 0, a0, a1, o = 0;
    if (arg && cap > 0) arg[0] = 0;
    if (!in || in->n == 0 || in->cp[0] != '/') return LCH_CMD_NONE;
    while (i < in->n && in->cp[i] != ' ' && k < (int)sizeof w - 1) {
        uint32_t c = in->cp[i++];
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        if (c > 0x7E) return LCH_CMD_BAD;
        w[k++] = (char)c;
    }
    w[k] = 0;
    if (i < in->n && in->cp[i] != ' ') return LCH_CMD_BAD; /* Befehlswort zu lang */
    a0 = i;
    while (a0 < in->n && in->cp[a0] == ' ') a0++;
    a1 = in->n;
    while (a1 > a0 && in->cp[a1 - 1] == ' ') a1--;
    for (i = a0; i < a1; i++) {
        if (in->cp[i] < 0x20 || in->cp[i] > 0x7E || o >= LCH_NAME_MAX) return LCH_CMD_BAD;
        if (arg && o < cap - 1) arg[o] = (char)in->cp[i];
        o++;
    }
    if (arg && cap > 0) arg[o < cap ? o : cap - 1] = 0;
    if (strcmp(w, "mute") == 0) return o ? LCH_CMD_MUTE : LCH_CMD_MUTES;
    if (strcmp(w, "unmute") == 0) return o ? LCH_CMD_UNMUTE : LCH_CMD_BAD;
    if (strcmp(w, "mutes") == 0 && !o) return LCH_CMD_MUTES;
    if ((strcmp(w, "help") == 0 || strcmp(w, "?") == 0) && !o) return LCH_CMD_HELP;
    return LCH_CMD_BAD;
}

/* ------------------------------------------------------------------ Ablauf */
void lch_init(lch_state *s, uint32_t nonce)
{
    lch_zero(s, sizeof *s);
    s->nonce = nonce ? nonce : 1u;
}

static int lch_dd_seen(lch_state *s, uint32_t key, uint32_t nonce, uint32_t seq)
{
    int i;
    for (i = 0; i < LCH_DEDUP; i++) {
        if (s->dd[i].seq == seq && s->dd[i].nonce == nonce && s->dd[i].key == key) return 1;
    }
    s->dd[s->dd_pos].key = key;
    s->dd[s->dd_pos].nonce = nonce;
    s->dd[s->dd_pos].seq = seq;
    s->dd_pos = (s->dd_pos + 1) % LCH_DEDUP;
    return 0;
}

int lch_rx(lch_state *s, uint32_t now, uint32_t key, const char *pilot, const uint8_t *d, int len)
{
    lch_pkt p;
    uint32_t cps[LCH_MAX_CHARS];
    wchar_t w[LCH_LINE_W];
    char nm[LCH_NAME_MAX + 1];
    int n, r = 0;
    if (!lch_parse(d, len, &p)) {
        s->rx_bad++;
        return 0;
    }
    if (lch_dd_seen(s, key, p.nonce, p.seq)) {
        s->rx_dup++;
        return 0;
    }
    if (!lch_rate_take(&s->rx_rate, key, now)) {
        s->rx_rate_drop++;
        return 0;
    }
    lch_name_clean(pilot, nm, (int)sizeof nm);
    if (lch_muted(&s->mute, nm)) {
        s->rx_muted++;
        lch_zero(nm, sizeof nm);
        return 0;
    }
    n = lch_sanitize_u8(p.text, p.len, cps, LCH_MAX_CHARS);
    if (n <= 0 || lch_cps_to_w(cps, n, w, LCH_LINE_W) <= 0) {
        s->rx_empty++;
    } else {
        lch_hist_push(&s->hist, LCH_L_OTHER, nm, w, now);
        s->rx_ok++;
        r = 1;
    }
    lch_zero(cps, sizeof cps);
    lch_zero(w, sizeof w);
    lch_zero(nm, sizeof nm);
    return r;
}

int lch_tx(lch_state *s, uint32_t now, const lch_input *in, const char *self_name, uint8_t *out, int cap)
{
    uint32_t cps[LCH_MAX_CHARS];
    uint8_t u8[LCH_MAX_TEXT_BYTES];
    wchar_t w[LCH_LINE_W];
    char nm[LCH_NAME_MAX + 1];
    int n, ul, len = 0;
    n = in ? lch_sanitize_cps(in->cp, in->n, cps, LCH_MAX_CHARS) : 0;
    if (n <= 0) {
        s->tx_empty++;
        return 0;
    }
    if (!lch_rate_take(&s->tx_rate, 0u, now)) {
        s->tx_rate_drop++;
        lch_zero(cps, sizeof cps);
        return -1;
    }
    ul = lch_cps_to_u8(cps, n, u8, (int)sizeof u8);
    if (++s->seq == 0) s->seq = 1;
    if (ul > 0) len = lch_build(out, cap, s->nonce, s->seq, u8, ul);
    if (len > 0 && lch_cps_to_w(cps, n, w, LCH_LINE_W) > 0) {
        lch_name_clean(self_name, nm, (int)sizeof nm);
        lch_hist_push(&s->hist, LCH_L_SELF, nm, w, now);
        s->tx_ok++;
    }
    lch_zero(cps, sizeof cps);
    lch_zero(u8, sizeof u8);
    lch_zero(w, sizeof w);
    lch_zero(nm, sizeof nm);
    return len > 0 ? len : 0;
}

void lch_wipe(lch_state *s)
{
    lch_zero(&s->hist, sizeof s->hist);
    lch_zero(&s->mute, sizeof s->mute);
    lch_zero(s->dd, sizeof s->dd);
}

/* ------------------------------------------------------------------ W446 STEELNET-CHAT */
static int hex4(const char *p, const char *e, uint32_t *v)
{
    int i;
    uint32_t r = 0;
    if (e - p < 4) return 0;
    for (i = 0; i < 4; i++) {
        char c = p[i];
        r <<= 4;
        if (c >= '0' && c <= '9') r |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') r |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') r |= (uint32_t)(c - 'A' + 10);
        else return 0;
    }
    *v = r;
    return 1;
}

static int u8_put(uint8_t *out, int o, int cap, uint32_t c)
{
    uint32_t one = c;
    int r = lch_cps_to_u8(&one, 1, out + o, cap - o);
    return r < 0 ? -1 : o + r;
}

int lch_json_str_u8(const char *v, const char *ve, uint8_t *out, int cap)
{
    const char *p, *e;
    int o = 0;
    if (!v || !ve || !out || cap <= 0 || ve - v < 2 || *v != '"' || ve[-1] != '"') return -1;
    p = v + 1;
    e = ve - 1; /* schliessendes Anfuehrungszeichen */
    while (p < e) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20) return -1;
        if (c == '"') return -1; /* unescaptes Anfuehrungszeichen vor dem Ende */
        if (c != '\\') {
            if (o >= cap) return -1;
            out[o++] = c;
            p++;
            continue;
        }
        if (p + 1 >= e) return -1;
        c = (unsigned char)p[1];
        p += 2;
        switch (c) {
        case '"': case '\\': case '/':
            if (o >= cap) return -1;
            out[o++] = c;
            break;
        case 'b': case 'f': case 'n': case 'r': case 't':
            if (o >= cap) return -1;
            out[o++] = c == 'b' ? 8 : c == 'f' ? 12 : c == 'n' ? 10 : c == 'r' ? 13 : 9; /* Bereinigung entfernt sie */
            break;
        case 'u': {
            uint32_t hi, lo;
            if (!hex4(p, e, &hi)) return -1;
            p += 4;
            if (hi >= 0xD800 && hi <= 0xDBFF) {
                if (e - p >= 6 && p[0] == '\\' && p[1] == 'u' && hex4(p + 2, e, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    p += 6;
                    o = u8_put(out, o, cap, 0x10000u + ((hi - 0xD800u) << 10) + (lo - 0xDC00u));
                    if (o < 0) return -1;
                }
                break; /* einzelnes hohes Surrogat: verworfen */
            }
            if (hi >= 0xDC00 && hi <= 0xDFFF) break; /* einzelnes niedriges Surrogat: verworfen */
            o = u8_put(out, o, cap, hi);
            if (o < 0) return -1;
            break;
        }
        default:
            return -1;
        }
    }
    return o;
}

int lch_json_text(const uint32_t *cps, int n, char *out, int cap)
{
    uint8_t u8[LCH_MAX_TEXT_BYTES];
    int ul, i, o = 0;
    if (!out || cap <= 0) return -1;
    if (n > LCH_MAX_CHARS) n = LCH_MAX_CHARS;
    ul = lch_cps_to_u8(cps, n, u8, (int)sizeof u8);
    if (ul < 0) return -1;
    for (i = 0; i < ul; i++) {
        uint8_t c = u8[i];
        if (c == '"' || c == '\\') {
            if (o + 2 >= cap) { o = -1; break; }
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            static const char hx[] = "0123456789abcdef";
            if (o + 6 >= cap) { o = -1; break; }
            out[o++] = '\\'; out[o++] = 'u'; out[o++] = '0'; out[o++] = '0';
            out[o++] = hx[c >> 4]; out[o++] = hx[c & 15];
        } else {
            if (o + 1 >= cap) { o = -1; break; }
            out[o++] = (char)c;
        }
    }
    if (o >= 0) out[o] = 0;
    lch_zero(u8, sizeof u8);
    return o;
}

int lch_sn_rx(lch_sn *sn, const lch_mute *mute, uint32_t now, const char *from, const uint8_t *text, int len, int hist,
              const char *self_name)
{
    uint32_t cps[LCH_MAX_CHARS];
    wchar_t w[LCH_LINE_W];
    char nm[LCH_NAME_MAX + 1], me[LCH_NAME_MAX + 1];
    int n, r = 0;
    if (!sn || !from || !from[0] || !text || len < 1 || len > LCH_MAX_TEXT_BYTES * 2) {
        if (sn) sn->rx_bad++;
        return 0;
    }
    lch_name_clean(from, nm, (int)sizeof nm);
    if (mute && lch_muted(mute, nm)) {
        sn->rx_muted++;
        lch_zero(nm, sizeof nm);
        return 0;
    }
    n = lch_sanitize_u8(text, len, cps, LCH_MAX_CHARS);
    if (n <= 0 || lch_cps_to_w(cps, n, w, LCH_LINE_W) <= 0) {
        sn->rx_empty++;
    } else {
        int self = 0;
        if (self_name && self_name[0]) {
            lch_name_clean(self_name, me, (int)sizeof me);
            self = strcmp(me, nm) == 0; /* Dienst-Namen sind eindeutig (Konto bzw. Guest-...) */
            lch_zero(me, sizeof me);
        }
        lch_hist_push(&sn->hist, self ? LCH_L_SELF : LCH_L_OTHER, nm, w, now);
        sn->rx_ok++;
        if (hist) sn->rx_hist++;
        r = 1;
    }
    lch_zero(cps, sizeof cps);
    lch_zero(w, sizeof w);
    lch_zero(nm, sizeof nm);
    return r;
}

int lch_sn_tx(lch_sn *sn, uint32_t now, const lch_input *in, char *out, int cap)
{
    uint32_t cps[LCH_MAX_CHARS];
    char body[LCH_SN_LINE_MAX];
    int n, bl, len = 0;
    if (!sn || !out || cap < LCH_SN_LINE_MAX) return 0;
    out[0] = 0;
    n = in ? lch_sanitize_cps(in->cp, in->n, cps, LCH_MAX_CHARS) : 0;
    if (n <= 0) {
        sn->tx_empty++;
        return 0;
    }
    if (!lch_rate_take(&sn->tx_rate, 0u, now)) {
        sn->tx_rate_drop++;
        lch_zero(cps, sizeof cps);
        return -1;
    }
    bl = lch_json_text(cps, n, body, (int)sizeof body);
    if (bl > 0 && bl + 24 < cap) {
        memcpy(out, "{\"t\":\"chat\",\"text\":\"", 20);
        memcpy(out + 20, body, (size_t)bl);
        memcpy(out + 20 + bl, "\"}", 3); /* mit Null; den Zeilenumbruch setzt lb_send */
        len = 20 + bl + 2;
        sn->tx_ok++;
    }
    lch_zero(cps, sizeof cps);
    lch_zero(body, sizeof body);
    return len;
}

void lch_sn_wipe(lch_sn *sn)
{
    if (sn) lch_zero(&sn->hist, sizeof sn->hist);
}
