/* loc_xact_fmt.c - W298 LOC-AUDIO: Parser fuer XACT-1 Wave-/Sound-Banks
 * (Line of Contact, siehe loc_xact_fmt.h). Jeder Lesezugriff ist gegen die
 * Puffergroesse geprueft; fremde Daten liefern Fehler statt Absturz. */
#include "loc_xact_fmt.h"
#include <string.h>

static uint32_t rd32(const uint8_t *d, uint32_t n, uint32_t o, int *ok)
{
    if (o > n || n - o < 4) {
        *ok = 0;
        return 0;
    }
    return (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8) | ((uint32_t)d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24);
}

static int rds16(const uint8_t *d, uint32_t n, uint32_t o, int *ok)
{
    if (o > n || n - o < 2) {
        *ok = 0;
        return 0;
    }
    return (int)(int16_t)(d[o] | (d[o + 1] << 8));
}

static int rd8(const uint8_t *d, uint32_t n, uint32_t o, int *ok)
{
    if (o >= n) {
        *ok = 0;
        return 0;
    }
    return d[o];
}

/* ---------------- XWB ---------------- */
uint32_t lxf_xwb_header_size(const uint8_t *d, uint32_t n)
{
    int ok = 1;
    uint32_t eo, es;
    if (n < 0x50 || memcmp(d, "WBND", 4) != 0) {
        return 0;
    }
    eo = rd32(d, n, 0x10, &ok);
    es = rd32(d, n, 0x14, &ok);
    if (!ok || eo > 0x1000000u || es > 0x1000000u) {
        return 0;
    }
    return eo + es;
}

int lxf_xwb_parse(const uint8_t *d, uint32_t n, lxf_bank_t *b)
{
    int ok = 1;
    uint32_t ver, bo, bs, es;
    memset(b, 0, sizeof *b);
    if (!d || n < 0x50 || memcmp(d, "WBND", 4) != 0) {
        return -1;
    }
    ver = rd32(d, n, 4, &ok);
    if (ver != 3) { /* nur belegte Fassung (alle LoC-Baenke) */
        return -2;
    }
    bo = rd32(d, n, 0x08, &ok);
    bs = rd32(d, n, 0x0C, &ok);
    b->entry_off = rd32(d, n, 0x10, &ok);
    es = rd32(d, n, 0x14, &ok);
    b->data_off = rd32(d, n, 0x20, &ok);
    b->data_len = rd32(d, n, 0x24, &ok);
    if (!ok || bs < 0x28 || bo + 0x28 > n) {
        return -3;
    }
    b->flags = rd32(d, n, bo, &ok);
    b->count = rd32(d, n, bo + 4, &ok);
    memcpy(b->name, d + bo + 8, 16);
    b->name[16] = 0;
    b->entry_size = rd32(d, n, bo + 0x18, &ok);
    b->align = rd32(d, n, bo + 0x20, &ok);
    if (!ok || b->entry_size < 0x18 || b->entry_size > 0x40 || b->count > 65536u ||
        (uint64_t)b->count * b->entry_size > es || (uint64_t)b->entry_off + es > n) {
        return -4;
    }
    return 0;
}

int lxf_xwb_wave(const lxf_bank_t *b, const uint8_t *d, uint32_t n, uint32_t i, lxf_wave_t *w)
{
    int ok = 1;
    uint32_t o, fl, fm, po, pl;
    memset(w, 0, sizeof *w);
    if (i >= b->count) {
        return -1;
    }
    o = b->entry_off + i * b->entry_size;
    fl = rd32(d, n, o, &ok);
    fm = rd32(d, n, o + 4, &ok);
    po = rd32(d, n, o + 8, &ok);
    pl = rd32(d, n, o + 12, &ok);
    w->loop_off = rd32(d, n, o + 16, &ok);
    w->loop_len = rd32(d, n, o + 20, &ok);
    if (!ok) {
        return -2;
    }
    w->codec = (int)(fm & 3u);
    w->ch = (int)((fm >> 2) & 7u);
    w->rate = (int)((fm >> 5) & 0x3FFFFFFu);
    w->bits = w->codec == LXF_CODEC_PCM ? ((fm >> 31) ? 16 : 8) : (w->codec == LXF_CODEC_ADPCM ? 4 : 16);
    w->loop = (fl & 2u) ? 1 : 0;
    if (b->data_len && ((uint64_t)po + pl > b->data_len)) {
        return -3;
    }
    w->off = b->data_off + po;
    w->len = pl;
    if ((uint64_t)w->loop_off + w->loop_len > pl) { /* unplausibel -> ganze Welle */
        w->loop_off = 0;
        w->loop_len = 0;
    }
    if (w->ch < 1 || w->ch > 6 || w->rate < 4000 || w->rate > 96000) {
        return -4;
    }
    return 0;
}

void lxf_wave_wfx(const lxf_wave_t *w, uint8_t wfx[20])
{
    uint16_t tag, align, bits;
    uint32_t avg;
    memset(wfx, 0, 20);
    if (w->codec == LXF_CODEC_ADPCM) {
        tag = 0x69;
        align = (uint16_t)(36 * w->ch);
        bits = 4;
        avg = (uint32_t)w->rate * align / 64u;
        wfx[16] = 2; /* cbSize */
        wfx[18] = 64; /* SamplesPerBlock */
    } else {
        tag = 1;
        bits = (uint16_t)(w->bits ? w->bits : 16);
        align = (uint16_t)(w->ch * bits / 8);
        avg = (uint32_t)w->rate * align;
    }
    wfx[0] = (uint8_t)tag;
    wfx[1] = (uint8_t)(tag >> 8);
    wfx[2] = (uint8_t)w->ch;
    memcpy(wfx + 4, &w->rate, 4);
    memcpy(wfx + 8, &avg, 4);
    wfx[12] = (uint8_t)align;
    wfx[13] = (uint8_t)(align >> 8);
    wfx[14] = (uint8_t)bits;
}

/* ---------------- XSB ---------------- */
int lxf_xsb_parse(const uint8_t *d, uint32_t n, lxf_xsb_t *x)
{
    int ok = 1;
    memset(x, 0, sizeof *x);
    if (!d || n < 0x38 || memcmp(d, "SDBK", 4) != 0) {
        return -1;
    }
    if (rds16(d, n, 4, &ok) != 11) { /* nur belegte Fassung (Bank.xsb) */
        return -2;
    }
    x->d = d;
    x->n = n;
    x->wb_off = rd32(d, n, 8, &ok);
    x->n_ent = rds16(d, n, 0x1C, &ok);
    x->n_cues = rds16(d, n, 0x1E, &ok);
    x->n_wb = rds16(d, n, 0x22, &ok);
    x->t3d_off = rd32(d, n, 0x10, &ok); /* W456 */
    x->idx_off = 0x38;
    x->ent_off = x->idx_off + (uint32_t)x->n_cues * 0x14u;
    if (!ok || x->n_ent < 0 || x->n_cues < 0 || x->n_wb < 0 || x->n_wb > 256 ||
        (uint64_t)x->ent_off + (uint64_t)x->n_ent * 0x14u > n || (uint64_t)x->wb_off + (uint64_t)x->n_wb * 16u > n) {
        x->d = 0;
        return -3;
    }
    return 0;
}

const char *lxf_xsb_cue_name(const lxf_xsb_t *x, int cue)
{
    int ok = 1;
    uint32_t no, k;
    if (!x->d || cue < 0 || cue >= x->n_cues) {
        return 0;
    }
    no = rd32(x->d, x->n, x->idx_off + (uint32_t)cue * 0x14u + 4, &ok);
    if (!ok || no == 0 || no >= x->n) {
        return 0;
    }
    for (k = no; k < x->n; k++) { /* nullterminiert innerhalb der Datei */
        if (x->d[k] == 0) {
            return (const char *)(x->d + no);
        }
    }
    return 0;
}

int lxf_xsb_find(const lxf_xsb_t *x, const char *name)
{
    int i;
    if (!name) {
        return -1;
    }
    for (i = 0; i < x->n_cues; i++) {
        const char *s = lxf_xsb_cue_name(x, i);
        if (s && strcmp(s, name) == 0) {
            return i;
        }
    }
    return -1;
}

void lxf_xsb_bank_name(const lxf_xsb_t *x, int i, char buf[17])
{
    buf[0] = 0;
    if (x->d && i >= 0 && i < x->n_wb) {
        memcpy(buf, x->d + x->wb_off + (uint32_t)i * 16u, 16);
        buf[16] = 0;
    }
}

static void add_track(const lxf_xsb_t *x, lxf_cue_t *c, int wave, int bank, int loopc)
{
    if (c->ntrack < LXF_MAX_TRACKS && wave >= 0 && wave < 0x10000 && bank >= 0 && bank < x->n_wb) {
        c->track[c->ntrack].wave = wave;
        c->track[c->ntrack].bank = bank;
        c->track[c->ntrack].loopc = loopc; /* W413 */
        c->ntrack++;
    }
}

/* Variantentabelle: Anzahl (Byte), ab +4 je 8 B {s16 Welle, s16 Bank, ...}. */
static void add_from_table(const lxf_xsb_t *x, lxf_cue_t *c, uint32_t j, uint32_t rnd)
{
    int ok = 1;
    int cnt = rd8(x->d, x->n, j, &ok);
    uint32_t k;
    if (!ok || cnt <= 0) {
        return;
    }
    k = rnd % (uint32_t)cnt;
    {
        int wave = rds16(x->d, x->n, j + 4 + 8 * k, &ok);
        int bank = rds16(x->d, x->n, j + 6 + 8 * k, &ok);
        if (ok) {
            add_track(x, c, wave, bank, -1); /* W413: keine Schleifenzahl bekannt (Bank.xsb ohne Tabellen) */
        }
    }
}

int lxf_xsb_cue(const lxf_xsb_t *x, int cue, uint32_t rnd, lxf_cue_t *c)
{
    int ok = 1, e, fl;
    uint32_t o, eo;
    memset(c, 0, sizeof *c);
    c->entry = -1;
    if (!x->d || cue < 0 || cue >= x->n_cues) {
        return -1;
    }
    o = x->idx_off + (uint32_t)cue * 0x14u;
    e = rds16(x->d, x->n, o + 2, &ok);
    if (e < 0) { /* Zufallstabelle auf Cue-Eintraege */
        uint32_t tab = rd32(x->d, x->n, o + 8, &ok);
        int cnt = rd8(x->d, x->n, tab, &ok);
        if (!ok || cnt <= 0) {
            return -2;
        }
        e = rds16(x->d, x->n, tab + 4 + 8 * (rnd % (uint32_t)cnt), &ok);
        rnd /= (uint32_t)cnt;
    }
    if (!ok || e < 0 || e >= x->n_ent) {
        return -3;
    }
    c->entry = e;
    eo = x->ent_off + (uint32_t)e * 0x14u;
    c->db100 = -10 * rd8(x->d, x->n, eo + 4, &ok);
    c->pitch = rds16(x->d, x->n, eo + 6, &ok);
    c->category = rd8(x->d, x->n, eo + 0xA, &ok);
    fl = rd8(x->d, x->n, eo + 0xB, &ok);
    { /* W356: Pegelfelder wie die XACT-Bibliothek (lxf_cue_t) */
        int w = rd8(x->d, x->n, eo + 4, &ok) | (rd8(x->d, x->n, eo + 5, &ok) << 8);
        int a = -16 * (w & 0x1FF), b = -50 * ((w >> 9) & 0x7F), r = -256 * rd8(x->d, x->n, eo + 0xF, &ok);
        c->db100_xact = a < -10000 ? -10000 : a;
        c->lfe_mb = b < -10000 ? -10000 : b;
        c->room_mb = r < -10000 ? -10000 : r;
    }
    if (!ok) {
        return -4;
    }
    if (fl & 1) { /* W456: 3D-Parametersatz (lxf_cue_t.has3d) */
        uint32_t r = x->t3d_off + 0x28u * (uint32_t)(rd8(x->d, x->n, eo + 0xC, &ok) | (rd8(x->d, x->n, eo + 0xD, &ok) << 8));
        if (ok && x->t3d_off >= 0x38u && (uint64_t)r + 0x28u <= x->n) {
            float f[3];
            uint32_t u[3];
            u[0] = rd32(x->d, x->n, r + 8, &ok);
            u[1] = rd32(x->d, x->n, r + 0xC, &ok);
            u[2] = rd32(x->d, x->n, r + 0x14, &ok);
            memcpy(f, u, sizeof f);
            if (ok && f[0] > 0.0f && f[0] < 1e9f && f[1] >= f[0] && f[1] <= 1e12f && f[2] >= 0.0f && f[2] <= 10.0f) {
                c->has3d = 1;
                c->min3d = f[0];
                c->max3d = f[1];
                c->rolloff3d = f[2];
                c->mode3d = rd8(x->d, x->n, r + 0x1C, &ok);
            }
        }
        ok = 1;
    }
    if (fl & 0x10) {
        add_from_table(x, c, rd32(x->d, x->n, eo, &ok), rnd);
    } else if (fl & 0x08) {
        add_track(x, c, rds16(x->d, x->n, eo, &ok), rds16(x->d, x->n, eo + 2, &ok), 0); /* W413: direkt = einmal */
    } else { /* Sound-Liste: Anzahl (Byte) + 24-Bit-Offset; je Sound 8 B Kopf + Groesse */
        uint32_t j = rd32(x->d, x->n, eo, &ok);
        int cnt = rd8(x->d, x->n, j, &ok);
        uint32_t so = rd32(x->d, x->n, j + 1, &ok) & 0x00FFFFFFu;
        int i;
        for (i = 0; ok && i < cnt && i < 32; i++) {
            int t = rd8(x->d, x->n, so, &ok);
            int sz = rd8(x->d, x->n, so + 4, &ok);
            if (!ok) {
                break;
            }
            if (t == 0x0A && sz >= 12) { /* W456: Amplituden-Huellkurve (lxf_cue_t.eg) */
                c->eg_on = 1;
                c->eg[0] = rd8(x->d, x->n, so + 19, &ok);
                c->eg[1] = 0xFFFF & rds16(x->d, x->n, so + 8, &ok);
                c->eg[2] = 0xFFFF & rds16(x->d, x->n, so + 10, &ok);
                c->eg[3] = 0xFFFF & rds16(x->d, x->n, so + 12, &ok);
                c->eg[4] = 0xFFFF & rds16(x->d, x->n, so + 14, &ok);
                c->eg[5] = 0xFFFF & rds16(x->d, x->n, so + 16, &ok);
                c->eg[6] = rd8(x->d, x->n, so + 18, &ok);
            } else if (t == 0 || t == 1) {
                int sf = rd8(x->d, x->n, so + 5, &ok);
                if (sf == 0x04 || sf == 0x44) {
                    add_from_table(x, c, rd32(x->d, x->n, so + 8, &ok), rnd);
                } else if (sf == 0x00 || sf == 0x40) {
                    add_track(x, c, rds16(x->d, x->n, so + 8, &ok), rds16(x->d, x->n, so + 10, &ok),
                              (int)((unsigned)rd8(x->d, x->n, so + 6, &ok) | ((unsigned)rd8(x->d, x->n, so + 7, &ok) << 8))); /* W413 */
                }
            }
            so += 8u + (uint32_t)sz;
        }
    }
    return c->ntrack > 0 ? 0 : -5;
}

/* W363 LOC-ORIGINAL: Laufzeit-Parameter (RPC, IXACTEngine_SetParameterControl) wie die XACT-Bibliothek des
 * Spiels (belegt am Code: Suche 0x1C4015/0x1C404F, Cue 0x1C5C46, Kurve 0x1C5392, Ziele 0x1C4208/0x1C437E/
 * 0x1C43C2). XSB-Kopf +0x14 = Tabelle, +0x20 = Anzahl; Eintrag 24 B {u16 Sound(=Cue-Eintrag), u8, u8 Kurven,
 * u32 Kurven-Versatz, char Name[16]}; Kurve 24 B {u16 Spur, u16 Ziel (0 Lautstaerke mB, 1 Tonhoehe 1/4096 Oktave,
 * 2 Filter-Eckfrequenz Hz), 4 x (u16 x, s16 y)}. x = (int)(Wert * 65536); x <= x0 -> y0, x >= x3 -> y3, sonst
 * ganzzahlig linear zwischen den Stuetzpunkten (idiv rundet zur Null). Mehrere Kurven je Ziel und Spur addieren. */
int lxf_rpc_eval(const uint8_t *cv, int xi)
{
    int k, x0, x1, y0, y1;
    if (xi <= (int)(cv[4] | cv[5] << 8)) {
        return (int)(int16_t)(cv[6] | cv[7] << 8);
    }
    if (xi >= (int)(cv[16] | cv[17] << 8)) {
        return (int)(int16_t)(cv[18] | cv[19] << 8);
    }
    for (k = 1; k < 4; k++) {
        x1 = cv[4 + 4 * k] | cv[5 + 4 * k] << 8;
        if (xi < x1) {
            x0 = cv[4 * k] | cv[1 + 4 * k] << 8;
            y0 = (int16_t)(cv[2 + 4 * k] | cv[3 + 4 * k] << 8);
            y1 = (int16_t)(cv[6 + 4 * k] | cv[7 + 4 * k] << 8);
            return x1 > x0 ? y0 + (y1 - y0) * (xi - x0) / (x1 - x0) : y0;
        }
    }
    return 0;
}

int lxf_xsb_rpc(const lxf_xsb_t *x, int entry, const char *name, float value, lxf_rpc_t out[LXF_MAX_TRACKS])
{
    int ok = 1, i, k, n, hits = 0, xi;
    uint32_t tab;
    double v = (double)value * 65536.0;
    memset(out, 0, sizeof(lxf_rpc_t) * LXF_MAX_TRACKS);
    if (!x || !x->d || !name || entry < 0 || !(value == value)) {
        return 0;
    }
    xi = v > 2147483647.0 ? 2147483647 : v < -2147483647.0 ? -2147483647 : (int)v; /* _ftol: zur Null */
    tab = rd32(x->d, x->n, 0x14, &ok);
    n = rds16(x->d, x->n, 0x20, &ok) & 0xFFFF;
    if (!ok || (uint64_t)tab + (uint64_t)n * 24u > x->n) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        const uint8_t *e = x->d + tab + (uint32_t)i * 24u;
        uint32_t co;
        int nc;
        if ((e[0] | e[1] << 8) != entry || strncmp((const char *)e + 8, name, 16) != 0) {
            continue;
        }
        nc = e[3];
        co = (uint32_t)(e[4] | e[5] << 8 | e[6] << 16) | (uint32_t)e[7] << 24;
        for (k = 0; k < nc; k++) {
            const uint8_t *cv;
            int trk, tgt, y;
            if ((uint64_t)co + (uint64_t)(k + 1) * 24u > x->n) {
                break;
            }
            cv = x->d + co + (uint32_t)k * 24u;
            trk = cv[0] | cv[1] << 8;
            tgt = cv[2] | cv[3] << 8;
            if (trk >= LXF_MAX_TRACKS || tgt > 2) {
                continue;
            }
            y = lxf_rpc_eval(cv, xi);
            if (tgt == 0) { out[trk].vol_mb += y; out[trk].has |= 1; }
            else if (tgt == 1) { out[trk].pitch += y; out[trk].has |= 2; }
            else { out[trk].filt_hz += y; out[trk].has |= 4; }
            hits++;
        }
    }
    return hits;
}

/* W413 LOC-SOUND-LOOP (GitHub #65): Schleife einer Spur aus Wellen-Flag und XSB-Schleifenzahl (loc_xact_fmt.h). */
int lxf_track_loop_mode(int wave_loop, int loopc)
{
    if (!wave_loop || loopc == 0) return LXF_LOOP_ONCE; /* ohne Schleifen-Welle nie; Schleifenzahl 0 = einmal */
    if (loopc < 0 || loopc == 0xFFFF) return LXF_LOOP_INF; /* keine Angabe (wie bisher) bzw. endlos */
    return LXF_LOOP_N;
}
