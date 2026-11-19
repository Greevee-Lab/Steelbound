/* loc_audio_selftest.c - W298 LOC-AUDIO: Selbsttest XACT-1-Parser (loc_xact_fmt.c)
 * + Wave-Bank-Dekodierung ueber den Mischer-Dekoder (audio_fmt.c) mit rein
 * synthetischen Daten (keine Spieldaten).
 *
 * Faelle: XWB-Kopf/Eintraege (PCM16 mit Schleife, Xbox-ADPCM stereo, WMA),
 * WAVEFORMATEX-Abbildung (vom Mischer akzeptiert), Dekodierung mit Pegel-
 * und Fehlermass, XSB-Cues (einfach, Zufallstabelle, Sound-Liste mit
 * Ereignis + Tabelle), Namenssuche, Kuerzungen jeder Laenge ohne Absturz.
 * Exit 0 = alles gruen. */
#include "../src/loc_xact_fmt.h"
#include "../src/audio_int.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

hcfg_t g_cfg;      /* audio_fmt.c liest g_cfg.audio_buflen */
au32 g_stat_drops; /* audio_fmt.c */

static int g_fail;
#define CHECK(c, ...)                                     \
    do {                                                  \
        if (!(c)) {                                       \
            printf("FEHLER %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
            g_fail++;                                     \
        }                                                 \
    } while (0)

static void w32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void w16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

/* ---- Xbox-ADPCM-Kodierer (IMA, Layout wie au_adpcm_decode) ---- */
static const short STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};
static const signed char IDX[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static int enc_nib(int x, int *pred, int *idx)
{
    int step = STEP[*idx], diff = x - *pred, nib = 0, dq = step >> 3;
    if (diff < 0) {
        nib = 8;
        diff = -diff;
    }
    if (diff >= step) { nib |= 4; diff -= step; dq += step; }
    if (diff >= step >> 1) { nib |= 2; diff -= step >> 1; dq += step >> 1; }
    if (diff >= step >> 2) { nib |= 1; dq += step >> 2; }
    *pred += (nib & 8) ? -dq : dq;
    if (*pred > 32767) *pred = 32767;
    if (*pred < -32768) *pred = -32768;
    *idx += IDX[nib];
    if (*idx < 0) *idx = 0;
    if (*idx > 88) *idx = 88;
    return nib;
}

/* src: ch-verschraenkte s16, nblocks*64 Frames -> nblocks*36*ch Bytes */
static void adpcm_encode(const short *src, int ch, int nblocks, uint8_t *out)
{
    int pred[6] = {0}, idx[6] = {0}, b, c, s;
    for (b = 0; b < nblocks; b++) {
        uint8_t *blk = out + (size_t)b * 36 * ch;
        memset(blk, 0, (size_t)36 * ch);
        for (c = 0; c < ch; c++) {
            w16(blk + c * 4, (uint16_t)(short)pred[c]);
            blk[c * 4 + 2] = (uint8_t)idx[c];
        }
        for (s = 0; s < 64; s++) {
            for (c = 0; c < ch; c++) {
                int nib = enc_nib(src[((size_t)b * 64 + s) * ch + c], &pred[c], &idx[c]);
                unsigned boff = (unsigned)s / 2, chunk = boff / 4, sub = boff % 4;
                unsigned pay = 4u * ch + (chunk * ch + c) * 4u + sub;
                blk[pay] |= (uint8_t)((s & 1) ? (nib << 4) : nib);
            }
        }
    }
}

/* ---- synthetische Wave-Bank ---- */
#define PCM_RATE 11025
#define PCM_FRAMES 11025
#define AD_RATE 22050
#define AD_BLOCKS 200
static uint8_t *make_xwb(uint32_t *size, short **ad_src)
{
    uint32_t pcm_bytes = PCM_FRAMES * 2, ad_bytes = AD_BLOCKS * 72, wma_bytes = 64;
    uint32_t data_off = 0x50 + 3 * 0x18, data_len = pcm_bytes + ad_bytes + wma_bytes;
    uint8_t *d = (uint8_t *)calloc(1, data_off + data_len);
    short *ad = (short *)malloc(sizeof(short) * AD_BLOCKS * 64 * 2);
    int i;
    memcpy(d, "WBND", 4);
    w32(d + 4, 3);
    w32(d + 0x08, 0x28); w32(d + 0x0C, 0x28);
    w32(d + 0x10, 0x50); w32(d + 0x14, 3 * 0x18);
    w32(d + 0x20, data_off); w32(d + 0x24, data_len);
    w32(d + 0x28, 0); w32(d + 0x2C, 3);
    memcpy(d + 0x30, "Synth", 5);
    w32(d + 0x40, 0x18); w32(d + 0x44, 0x40); w32(d + 0x48, 4);
    /* e0: PCM16 mono 11025, Schleife 200..+400 Bytes */
    w32(d + 0x50, 2);
    w32(d + 0x54, 0x80000000u | ((uint32_t)PCM_RATE << 5) | (1u << 2) | 0u);
    w32(d + 0x58, 0); w32(d + 0x5C, pcm_bytes); w32(d + 0x60, 200); w32(d + 0x64, 400);
    /* e1: ADPCM stereo 22050 */
    w32(d + 0x68, 0x10000);
    w32(d + 0x6C, ((uint32_t)AD_RATE << 5) | (2u << 2) | 1u);
    w32(d + 0x70, pcm_bytes); w32(d + 0x74, ad_bytes);
    /* e2: WMA */
    w32(d + 0x80, 0);
    w32(d + 0x84, 0x80000000u | (44100u << 5) | (2u << 2) | 2u);
    w32(d + 0x88, pcm_bytes + ad_bytes); w32(d + 0x8C, wma_bytes);
    for (i = 0; i < PCM_FRAMES; i++) { /* 440 Hz, Amplitude 0,5 */
        short v = (short)(16384.0 * sin(2.0 * 3.14159265358979 * 440.0 * i / PCM_RATE));
        w16(d + data_off + 2 * i, (uint16_t)v);
    }
    for (i = 0; i < AD_BLOCKS * 64; i++) { /* L 300 Hz 0,25 / R 700 Hz 0,5 */
        ad[2 * i] = (short)(8192.0 * sin(2.0 * 3.14159265358979 * 300.0 * i / AD_RATE));
        ad[2 * i + 1] = (short)(16384.0 * sin(2.0 * 3.14159265358979 * 700.0 * i / AD_RATE));
    }
    adpcm_encode(ad, 2, AD_BLOCKS, d + data_off + pcm_bytes);
    *ad_src = ad;
    *size = data_off + data_len;
    return d;
}

/* ---- synthetische Sound-Bank (v11) ----
 * 3 Wave-Banks (Synth, Other, Synth2), 4 Eintraege, 3 Cues:
 *  cue0 "SIMPLE" -> Eintrag 0 (einfach: Welle 1 / Bank 0), -6,0 dB, Pitch -682, Kat 4
 *  cue1 "RANDOM" -> Zufallstabelle {Eintrag 1, Eintrag 2}; 1 = einfach (Welle 0/Bank 0), 2 = Tabelle 0x10
 *  cue2 "LAYERS" -> Eintrag 3 (Sound-Liste: Ereignis 0x0A, Welle 0/Bank 2, Tabelle sf=4) */
static uint8_t *make_xsb(uint32_t *size)
{
    uint32_t idx = 0x38, ent = idx + 3 * 0x14, wb = ent + 4 * 0x14, names = wb + 3 * 16;
    uint32_t tab_rand = names + 32, tab_e2 = tab_rand + 4 + 16, list = tab_e2 + 4 + 16, snd = list + 8,
             tab_snd = snd + 44, end = tab_snd + 4 + 16;
    uint8_t *d = (uint8_t *)calloc(1, end);
    memcpy(d, "SDBK", 4);
    w16(d + 4, 11);
    w32(d + 8, wb);
    w16(d + 0x1C, 4); w16(d + 0x1E, 3); w16(d + 0x22, 3);
    memcpy(d + 0x28, "Bank", 4);
    memcpy(d + wb, "Synth", 5); memcpy(d + wb + 16, "Other", 5); memcpy(d + wb + 32, "Synth2", 6);
    memcpy(d + names, "SIMPLE", 7); memcpy(d + names + 8, "RANDOM", 7); memcpy(d + names + 16, "LAYERS", 7);
    /* Cue-Index */
    w16(d + idx + 2, 0); w32(d + idx + 4, names); w32(d + idx + 8, 0xFFFFFFFFu);
    w16(d + idx + 0x14 + 2, 0xFFFF); w32(d + idx + 0x14 + 4, names + 8); w32(d + idx + 0x14 + 8, tab_rand);
    w16(d + idx + 0x28 + 2, 3); w32(d + idx + 0x28 + 4, names + 16); w32(d + idx + 0x28 + 8, 0xFFFFFFFFu);
    /* Eintraege */
    w16(d + ent + 0, 1); w16(d + ent + 2, 0); d[ent + 4] = 60; w16(d + ent + 6, (uint16_t)(short)-682);
    d[ent + 0xA] = 4; d[ent + 0xB] = 0x09;
    d[ent + 5] = 0xFE; d[ent + 0xF] = 3; /* W356: LFE-Stufe 127 (Bit 9-15), I3DL2-Send 3 */
    w16(d + ent + 0x14, 0); w16(d + ent + 0x16, 0); d[ent + 0x14 + 0xA] = 5; d[ent + 0x14 + 0xB] = 0x09;
    w32(d + ent + 0x28, tab_e2); d[ent + 0x28 + 0xA] = 1; d[ent + 0x28 + 0xB] = 0x11;
    w32(d + ent + 0x3C, list); d[ent + 0x3C + 4] = 25; d[ent + 0x3C + 0xA] = 5; d[ent + 0x3C + 0xB] = 0x01;
    /* Zufallstabelle auf Eintraege 1/2 */
    d[tab_rand] = 2; w16(d + tab_rand + 4, 1); w16(d + tab_rand + 12, 2);
    /* Tabelle Eintrag 2: Welle 1/Bank 1, Welle 0/Bank 1 */
    d[tab_e2] = 2; w16(d + tab_e2 + 4, 1); w16(d + tab_e2 + 6, 1); w16(d + tab_e2 + 12, 0); w16(d + tab_e2 + 14, 1);
    /* Sound-Liste: 3 Sounds ab snd */
    d[list] = 3; w32(d + list + 1, snd); /* 24-Bit-Offset, Byte 4 bleibt 0 */
    d[snd] = 0x0A; d[snd + 4] = 12; /* Ereignis, 8+12 B */
    d[snd + 20] = 0; d[snd + 24] = 4; d[snd + 25] = 0; w16(d + snd + 28, 0); w16(d + snd + 30, 2); /* direkt */
    d[snd + 32] = 1; d[snd + 36] = 4; d[snd + 37] = 0x04; w32(d + snd + 40, tab_snd); /* Tabelle */
    d[tab_snd] = 2; w16(d + tab_snd + 4, 1); w16(d + tab_snd + 6, 0); w16(d + tab_snd + 12, 0); w16(d + tab_snd + 14, 1);
    *size = end;
    return d;
}

static double rms(const float *st, uint32_t n, int c)
{
    double e = 0;
    uint32_t i;
    for (i = 0; i < n; i++) {
        e += (double)st[2 * i + c] * st[2 * i + c];
    }
    return n ? sqrt(e / n) : 0;
}

/* Optional: echte Baenke (Ordner mit Bank.xsb + *.xwb, nur lesen, nichts wird
 * ausgegeben ausser Zaehlwerten): jeder Cue wird aufgeloest und gegen die
 * Wave-Bank-Eintraege geprueft. */
static uint8_t *slurp(const char *p, uint32_t *n)
{
    FILE *f = fopen(p, "rb");
    uint8_t *d;
    long sz;
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    if (sz > (1 << 20)) {
        sz = 1 << 20; /* Kopf genuegt (Eintragstabellen) */
    }
    fseek(f, 0, SEEK_SET);
    d = (uint8_t *)malloc(sz ? sz : 1);
    *n = (uint32_t)fread(d, 1, sz, f);
    fclose(f);
    return d;
}

static int real_banks(const char *dir)
{
    char p[1024], nm[17];
    uint32_t n;
    uint8_t *xsb;
    lxf_xsb_t xs;
    int i, k, ok = 0, miss = 0, codec[3] = {0}, loops = 0, tracks = 0;
    snprintf(p, sizeof p, "%s/Bank.xsb", dir);
    xsb = slurp(p, &n);
    if (!xsb || lxf_xsb_parse(xsb, n, &xs) != 0) {
        printf("Bank.xsb fehlt/unlesbar\n");
        return 1;
    }
    for (i = 0; i < xs.n_cues; i++) {
        lxf_cue_t c;
        int good = 1;
        if (lxf_xsb_cue(&xs, i, (uint32_t)i * 2654435761u, &c) != 0) {
            miss++;
            continue;
        }
        for (k = 0; k < c.ntrack; k++) {
            uint32_t nb;
            uint8_t *wb;
            lxf_bank_t b;
            lxf_wave_t w;
            lxf_xsb_bank_name(&xs, c.track[k].bank, nm);
            snprintf(p, sizeof p, "%s/%s.xwb", dir, nm);
            wb = slurp(p, &nb);
            if (!wb || lxf_xwb_parse(wb, nb, &b) != 0 || lxf_xwb_wave(&b, wb, nb, (uint32_t)c.track[k].wave, &w) != 0) {
                good = 0;
            } else {
                codec[w.codec]++;
                loops += w.loop;
                tracks++;
            }
            free(wb);
        }
        ok += good;
        miss += !good;
    }
    printf("echte Baenke: %d Cues, aufgeloest %d, Fehler %d; Spuren %d (PCM %d, ADPCM %d, WMA %d), Schleifen %d\n",
           xs.n_cues, ok, miss, tracks, codec[0], codec[1], codec[2], loops);
    free(xsb);
    return miss ? 1 : 0;
}

int main(int argc, char **argv)
{
    uint32_t nx, ns, i;
    short *ad_src = NULL;
    uint8_t *x;
    if (argc > 1) {
        return real_banks(argv[1]);
    }
    x = make_xwb(&nx, &ad_src);
    uint8_t *s = make_xsb(&ns);
    lxf_bank_t b;
    lxf_wave_t w0, w1, w2;
    uint8_t wfx[20];
    wfx_t wt;

    /* --- XWB --- */
    CHECK(lxf_xwb_header_size(x, 0x50) == 0x50 + 3 * 0x18, "Kopfgroesse %u", lxf_xwb_header_size(x, 0x50));
    CHECK(lxf_xwb_parse(x, nx, &b) == 0, "XWB parse");
    CHECK(b.count == 3 && strcmp(b.name, "Synth") == 0 && b.flags == 0, "Bankdaten count=%u name=%s", b.count, b.name);
    CHECK(lxf_xwb_wave(&b, x, nx, 0, &w0) == 0 && w0.codec == LXF_CODEC_PCM && w0.ch == 1 && w0.rate == PCM_RATE &&
              w0.bits == 16 && w0.loop && w0.loop_off == 200 && w0.loop_len == 400 && w0.len == PCM_FRAMES * 2,
          "Welle 0 (PCM)");
    CHECK(lxf_xwb_wave(&b, x, nx, 1, &w1) == 0 && w1.codec == LXF_CODEC_ADPCM && w1.ch == 2 && w1.rate == AD_RATE &&
              !w1.loop && w1.len == AD_BLOCKS * 72,
          "Welle 1 (ADPCM)");
    CHECK(lxf_xwb_wave(&b, x, nx, 2, &w2) == 0 && w2.codec == LXF_CODEC_WMA, "Welle 2 (WMA)");
    CHECK(lxf_xwb_wave(&b, x, nx, 3, &w2) != 0, "Welle 3 ausserhalb");

    /* --- WAVEFORMATEX -> vom Mischer akzeptiert, Desc-Erkennung --- */
    lxf_wave_wfx(&w0, wfx);
    memcpy(&wt, wfx, sizeof wt);
    CHECK(au_wfx_ok(&wt) && wt.tag == 1 && wt.align == 2 && wt.bits == 16, "wfx PCM");
    lxf_wave_wfx(&w1, wfx);
    memcpy(&wt, wfx, sizeof wt);
    CHECK(au_wfx_ok(&wt) && wt.tag == 105 && wt.align == 72 && wt.bits == 4 && wt.ch == 2, "wfx ADPCM");
    {
        au32 desc[9] = {36, 0, 0, (au32)(uintptr_t)wfx, 0, 0, 0, 0, 0};
        wfx_t got;
        au32 bl, fl;
        au_parse_desc(1, (au32)(uintptr_t)desc, &got, &bl, &fl);
        CHECK(got.tag == 105 && got.rate == AD_RATE, "au_parse_desc findet wfx (tag %u)", got.tag);
    }

    /* --- Dekodierung: PCM ueber au_pcm_convert, ADPCM ueber au_adpcm_decode --- */
    {
        uint32_t cap = 50000, n;
        float *st = (float *)malloc(sizeof(float) * 2 * cap);
        double r, pk = 0;
        n = au_pcm_convert(x + w0.off, w0.len, 1, 16, PCM_RATE, st, cap);
        for (i = 0; i < n; i++) {
            pk = fabs(st[2 * i]) > pk ? fabs(st[2 * i]) : pk;
        }
        r = rms(st, n, 0);
        printf("PCM16 mono 11025: %u Frames @44100, RMS %.3f (soll 0,354), Spitze %.3f\n", n, r, pk);
        CHECK(n >= 44090 && n <= 44110, "PCM-Frames %u", n);
        CHECK(fabs(r - 0.3536) < 0.01 && pk > 0.49 && pk < 0.51, "PCM-Pegel");
        n = au_adpcm_decode(x + w1.off, w1.len, 2, st, cap);
        CHECK(n == AD_BLOCKS * 64, "ADPCM-Frames %u", n);
        {
            double el = 0, er = 0;
            for (i = 0; i < n; i++) { /* Abweichung zum Original */
                double dl = st[2 * i] - ad_src[2 * i] / 32768.0, dr = st[2 * i + 1] - ad_src[2 * i + 1] / 32768.0;
                el += dl * dl;
                er += dr * dr;
            }
            el = sqrt(el / n);
            er = sqrt(er / n);
            printf("ADPCM stereo 22050: %u Frames, RMS L %.3f (soll 0,177) R %.3f (soll 0,354), Fehler L %.4f R %.4f "
                   "(SNR %.1f/%.1f dB)\n",
                   n, rms(st, n, 0), rms(st, n, 1), el, er, 20 * log10(0.177 / el), 20 * log10(0.354 / er));
            CHECK(fabs(rms(st, n, 0) - 0.177) < 0.01 && fabs(rms(st, n, 1) - 0.354) < 0.015, "ADPCM-Pegel");
            CHECK(20 * log10(0.177 / el) > 20 && 20 * log10(0.354 / er) > 20, "ADPCM-SNR");
        }
        free(st);
    }

    /* --- XSB --- */
    {
        lxf_xsb_t xs;
        lxf_cue_t c;
        char nm[17];
        int k, seen1 = 0, seen2 = 0;
        CHECK(lxf_xsb_parse(s, ns, &xs) == 0 && xs.n_cues == 3 && xs.n_ent == 4 && xs.n_wb == 3, "XSB parse");
        lxf_xsb_bank_name(&xs, 2, nm);
        CHECK(strcmp(nm, "Synth2") == 0, "Bankname 2 = %s", nm);
        CHECK(lxf_xsb_find(&xs, "RANDOM") == 1 && lxf_xsb_find(&xs, "nix") == -1, "Namenssuche");
        CHECK(lxf_xsb_cue(&xs, 0, 0, &c) == 0 && c.ntrack == 1 && c.track[0].wave == 1 && c.track[0].bank == 0 &&
                  c.db100 == -600 && c.pitch == -682 && c.category == 4,
              "Cue 0 einfach");
        /* W356: Pegelfelder wie die XACT-Bibliothek (Volumen 9 Bit x -16 mB, LFE 7 Bit x -50, Send x -256) */
        CHECK(c.db100_xact == -960 && c.lfe_mb == -6350 && c.room_mb == -768, "Cue 0 XACT-Pegel %d/%d/%d",
              c.db100_xact, c.lfe_mb, c.room_mb);
        printf("XACT-Pegel Cue 0: Lautstaerke %d mB, LFE %d mB, Hall-Send %d mB\n", c.db100_xact, c.lfe_mb, c.room_mb);
        for (k = 0; k < 16; k++) {
            CHECK(lxf_xsb_cue(&xs, 1, (uint32_t)k, &c) == 0 && c.ntrack == 1, "Cue 1 Zufall %d", k);
            if (c.entry == 1) {
                seen1 = c.track[0].wave == 0 && c.track[0].bank == 0;
            } else if (c.entry == 2) {
                seen2 |= c.track[0].bank == 1 && c.category == 1;
            }
        }
        CHECK(seen1 && seen2, "Cue 1 beide Varianten (%d/%d)", seen1, seen2);
        CHECK(lxf_xsb_cue(&xs, 2, 0, &c) == 0 && c.ntrack == 2 && c.track[0].bank == 2 && c.track[0].wave == 0 &&
                  c.track[1].wave == 1 && c.track[1].bank == 0 && c.db100 == -250,
              "Cue 2 Sound-Liste (Spuren %d)", c.ntrack);
        CHECK(lxf_xsb_cue(&xs, 3, 0, &c) != 0 && lxf_xsb_cue(&xs, -1, 0, &c) != 0, "Cue ausserhalb");
    }

    /* --- Kuerzungen: jede Laenge, kein Absturz, nur Fehler oder gueltige Werte --- */
    {
        uint32_t n, bad = 0;
        for (n = 0; n < nx && n < 0x200; n++) {
            uint8_t *t = (uint8_t *)malloc(n ? n : 1);
            lxf_bank_t bb;
            lxf_wave_t ww;
            memcpy(t, x, n);
            if (lxf_xwb_parse(t, n, &bb) == 0) {
                for (i = 0; i < bb.count; i++) {
                    lxf_xwb_wave(&bb, t, n, i, &ww);
                }
                bad += n < 0x50 + 3 * 0x18; /* darf erst mit voller Tabelle gelingen */
            }
            free(t);
        }
        for (n = 0; n < ns; n++) {
            uint8_t *t = (uint8_t *)malloc(n ? n : 1);
            lxf_xsb_t xs;
            lxf_cue_t c;
            int k;
            memcpy(t, s, n);
            if (lxf_xsb_parse(t, n, &xs) == 0) {
                for (k = 0; k < 3; k++) {
                    lxf_xsb_cue(&xs, k, 7, &c);
                    lxf_xsb_cue_name(&xs, k);
                }
            }
            free(t);
        }
        CHECK(bad == 0, "XWB vor voller Tabelle angenommen (%u)", bad);
        printf("Kuerzungen: XWB 0..%u, XSB 0..%u Bytes ohne Absturz\n", nx < 0x200 ? nx : 0x200, ns);
    }
    free(x);
    free(s);
    free(ad_src);
    if (g_fail) {
        printf("loc_audio_selftest: %d FEHLER\n", g_fail);
    } else {
        printf("loc_audio_selftest: ALLE OK\n");
    }
    return g_fail ? 1 : 0;
}
