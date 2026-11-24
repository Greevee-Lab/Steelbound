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
#include "../src/loc_wma.h" /* W367 */
#include "../src/loc_audio_scope.h" /* W375 */

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
    int i, k, ok = 0, miss = 0, codec[3] = {0}, loops = 0, tracks = 0, egs = 0, eg_rel = 0, n3d = 0, n3d100 = 0, n3d1 = 0;
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
        egs += c.eg_on; /* W456: Huellkurven-Ereignisse (0x0A) */
        n3d += c.has3d; /* W456: 3D-Satz je Eintrag */
        n3d100 += c.has3d && c.min3d == 100.0f;
        n3d1 += c.has3d && c.min3d == 1.0f;
        if (c.eg_on && c.eg[5] > eg_rel) eg_rel = c.eg[5];
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
    printf("echte Baenke W456: Cues mit Huellkurve %d, laengstes Release %d (%.2f s); Cues mit 3D-Satz %d (Min 100: %d, Min 1: %d)\n",
           egs, eg_rel, eg_rel * 512.0 / 48000.0, n3d, n3d100, n3d1);
    free(xsb);
    return miss ? 1 : 0;
}


/* ---- W367 LOC-WMA: Playlist-Modell, Media-Foundation-Dekoder, Fehlerfaelle (keine Spieldaten) ---- */
/* W375 (GitHub #51): Missions-Cues beim Missionsende still; Rollback-Waisen (Schleife) gestoppt. Spielmodell:
 * Mission 1 beginnt (Maske 0 -> 3), Eject-Alarm (Schleife), Motor (Schleife, vom Spiel gestoppt), Einschlag (einmal),
 * Musik (WMA-Playlist, Spiel stoppt selbst), Menue-Schleife von vorher; Ende (3 -> 0): nur der Alarm wird gestoppt,
 * danach 0 aktive Schleifen der Mission. Mission 2: Menue-Cue nach dem Ende bleibt unberuehrt. */
static void scope_tests(void)
{
    las_cue_t c[8];
    int idx[8], n, i, act;
    uint32_t mask = 0, gen = 0;
    memset(c, 0, sizeof c);
    CHECK(las_mission_edge(0, 0) == LAS_EDGE_NONE && las_mission_edge(0, 3) == LAS_EDGE_BEGIN &&
          las_mission_edge(3, 1) == LAS_EDGE_NONE && las_mission_edge(1, 0) == LAS_EDGE_END, "Missionsgrenzen");
    c[0].state = 2; c[0].loops = 1; c[0].serial = 1; c[0].mgen = 0; /* Menue-Schleife vor der Mission */
    if (las_mission_edge(mask, 3) == LAS_EDGE_BEGIN) gen++;
    mask = 3;
    c[1].state = 2; c[1].loops = 1; c[1].serial = 2; c[1].mgen = gen; /* Eject-Alarm */
    c[2].state = 2; c[2].loops = 1; c[2].serial = 3; c[2].mgen = gen; /* Motor ... */
    c[3].state = 2; c[3].loops = 0; c[3].serial = 4; c[3].mgen = gen; /* Einschlag (einmal) */
    c[4].state = 2; c[4].loops = 1; c[4].wma = 1; c[4].serial = 5; c[4].mgen = gen; /* Musik */
    c[5].state = 1; c[5].loops = 1; c[5].serial = 6; c[5].mgen = gen; /* vorbereitet, nie gestartet */
    c[2].state = 0; /* ... vom Spiel gestoppt */
    c[4].state = 0; /* Musik: FUN_0008D8D0 Stop(ST_CUE) */
    CHECK(las_mission_edge(mask, 0) == LAS_EDGE_END, "Ende erkannt");
    mask = 0;
    n = las_mission_end_pick(c, 8, gen, idx, 8);
    CHECK(n == 1 && idx[0] == 1, "Missionsende: nur Eject-Alarm (n=%d, idx0=%d)", n, n ? idx[0] : -1);
    for (i = 0; i < n; i++) c[idx[i]].state = 0;
    for (act = 0, i = 0; i < 8; i++) act += c[i].state == 2 && c[i].loops && c[i].mgen == gen;
    CHECK(act == 0, "nach dem Missionsende %d aktive Schleifen der Mission", act);
    CHECK(c[0].state == 2 && c[3].state == 2, "Menue-Schleife/Einmal-Cue unberuehrt");
    CHECK(las_mission_end_pick(c, 8, 0, idx, 8) == 0, "keine Mission -> nichts");
    c[6].state = 2; c[6].loops = 1; c[6].serial = 7; c[6].mgen = 0; /* Menue nach dem Ende */
    if (las_mission_edge(mask, 1) == LAS_EDGE_BEGIN) gen++;
    CHECK(gen == 2 && las_mission_end_pick(c, 8, gen, idx, 8) == 0, "Mission 2: alte Cues nicht ihre");
    /* Rollback-Waise: Nummer passt + spielt + Schleife -> stoppen; Einmal-Cue, Musik, unbekannte Nummer -> nicht */
    c[1].state = 2; c[1].mgen = gen;
    CHECK(las_orphan_pick(c, 8, 2) == 1, "Waise Schleife");
    CHECK(las_orphan_pick(c, 8, 4) == -1, "Waise einmal: laeuft aus");
    c[4].state = 2;
    CHECK(las_orphan_pick(c, 8, 5) == -1, "Waise Musik");
    CHECK(las_orphan_pick(c, 8, 99) == -1 && las_orphan_pick(c, 8, 0) == -1, "unbekannte Nummer");
    c[1].state = 0; c[1].serial = 8; /* Slot wiederverwendet: alte Nummer trifft ihn nicht */
    c[1].state = 2;
    CHECK(las_orphan_pick(c, 8, 2) == -1, "wiederverwendeter Slot");
    printf("Missionsende/Waisen (W375): Grenzen, Auswahl, Musik/Menue/Einmal unberuehrt, Slot-Nummer geprueft\n");
    { /* W390 LOC-OWN-PREDICT: im spekulativen Vorlauf kein Ton, kein Stopp, keine Freigabe, kein Setter */
        static const int play[] = {4, 23, 24, 25, 38, 32, 10}; /* DoWork, PrepareEx, PlayEx, Stop, StopSoundCues, SourceRelease, UnRegister */
        static const int set[] = {11, 12, 13, 14, 15, 16, 17, 20, 21, 33, 34, 35, 36, 37, 27, 28, 29, 31};
        static const int ro[] = {22, 30, 39, 5, 6, 7, 8, 9, 26};
        int i2, bad = 0, nskip = 0;
        for (i2 = 0; i2 < (int)(sizeof play / sizeof play[0]); i2++) bad += las_spec_action(play[i2]) != LAS_SPEC_SKIP;
        for (i2 = 0; i2 < (int)(sizeof set / sizeof set[0]); i2++) bad += las_spec_action(set[i2]) != LAS_SPEC_SKIP;
        for (i2 = 0; i2 < (int)(sizeof ro / sizeof ro[0]); i2++) bad += las_spec_action(ro[i2]) != LAS_SPEC_ALLOW;
        CHECK(bad == 0, "W390: Vorlauf-Tabelle XACT (%d falsch)", bad);
        CHECK(las_spec_action(18) == LAS_SPEC_FAIL, "W390: GetNotification im Vorlauf = keine Meldung");
        for (i2 = 0; i2 < 64; i2++) nskip += las_spec_action(i2) == LAS_SPEC_SKIP;
        CHECK(nskip >= 64 - 10, "W390: unbekannte Einstiege muessen ohne Wirkung bleiben (%d von 64)", nskip);
        printf("Vorlauf (W390): Start/Vorbereiten/Stopp/Freigabe/Setter/DoWork ohne Wirkung, Lesen erlaubt\n");
    }
}

/* W379 (GitHub #54): Titelmusik nach der Titel-Demo. Spielmodell (Lauf W379_T4): Titel startet B01 (Streaming-Bank
 * bgm, Welle 5, Handle h1); Demo; Tastendruck in der Demo -> zurueck zum Titel, das Spiel startet B01 erneut (h2) ohne
 * h1 zu stoppen; naechster Tastendruck -> Stop(h2) -> Menue. Erwartet: danach keine Streaming-Musik mehr aktiv.
 * Gegenprobe ohne Regel: h1 laeuft in den Menues weiter. Dazu: andere Welle, andere Bank, vorbereitete Cues, eigener
 * Cue und mit memset genullte Eintraege werden nie gewaehlt. */
static void steal_model(las_cue_t *c, int with_rule)
{
    int idx[8], n, i;
    memset(c, 0, sizeof(las_cue_t) * 8);
    c[0].state = 2; c[0].loops = 1; c[0].swb[0] = 3; c[0].swave[0] = 5; /* h1: B01 (Bank 2 -> swb 3) */
    c[1].state = 2; /* Menue-Ton aus Speicher-Bank (keine Streaming-Welle) */
    c[2].state = 2; c[2].swb[0] = 3; c[2].swave[0] = 6; /* andere Streaming-Welle derselben Bank */
    c[3].state = 2; c[3].swb[1] = 4; c[3].swave[1] = 5; /* gleiche Welle, andere Bank */
    c[4].state = 1; c[4].swb[0] = 3; c[4].swave[0] = 5; /* nur vorbereitet */
    n = with_rule ? las_stream_steal_pick(c, 8, 5, 2, 5, idx, 8) : 0; /* h2 startet B01 in Slot 5 */
    for (i = 0; i < n; i++) c[idx[i]].state = 0;
    c[5].state = 2; c[5].loops = 1; c[5].swb[0] = 3; c[5].swave[0] = 5; /* h2 */
    c[5].state = 0; /* Tastendruck: Stop(Cue -1, h2) */
}

static void steal_tests(void)
{
    las_cue_t c[8];
    int idx[8], n, i, act;
    memset(c, 0, sizeof c);
    for (i = 0; i < 8; i++) c[i].state = 2;
    CHECK(las_stream_steal_pick(c, 8, -1, 0, 0, idx, 8) == 0, "genullte Eintraege nie gewaehlt");
    CHECK(las_stream_steal_pick(NULL, 8, -1, 2, 5, idx, 8) == 0 && las_stream_steal_pick(c, 8, -1, -1, 5, idx, 8) == 0,
          "ungueltige Argumente");
    steal_model(c, 1);
    for (act = 0, i = 0; i < 8; i++) act += c[i].state == 2 && c[i].swb[0] == 3 && c[i].swave[0] == 5;
    CHECK(act == 0, "#54: nach dem Tastendruck %d Instanz(en) der Titelmusik aktiv", act);
    CHECK(c[1].state == 2 && c[2].state == 2 && c[3].state == 2 && c[4].state == 1,
          "andere Welle/Bank, Menue-Ton, vorbereiteter Cue unberuehrt");
    steal_model(c, 0);
    for (act = 0, i = 0; i < 8; i++) act += c[i].state == 2 && c[i].swb[0] == 3 && c[i].swave[0] == 5;
    CHECK(act == 1, "Gegenprobe ohne Regel: alte Titelmusik laeuft weiter (%d)", act);
    memset(c, 0, sizeof c);
    c[0].state = 2; c[0].swb[2] = 1; c[0].swave[2] = 0; /* Bank 0, Welle 0 auf Spur 2 */
    c[1].state = 2; c[1].swb[0] = 1; c[1].swave[0] = 0;
    n = las_stream_steal_pick(c, 8, 1, 0, 0, idx, 8);
    CHECK(n == 1 && idx[0] == 0, "Bank 0/Welle 0, beliebige Spur, eigener Cue ausgenommen (n=%d)", n);
    CHECK(las_stream_steal_pick(c, 8, -1, 0, 0, idx, 1) == 1, "Obergrenze cap");
    printf("Streaming-Welle einfach (W379, #54): Titel->Demo->Titel->Menue still, Gegenprobe, Randfaelle geprueft\n");
}

static void wma_tests(void)
{
    static lw_pl_t pl;
    uint32_t id = 0, id2 = 0;
    int i, k, prev, same = 0, seen[4] = { 0, 0, 0, 0 }, seq[16], seq2[16];
    char err[256];
    lw_pl_init(&pl, 0x1234, 7, 2u);
    CHECK(pl.kind == LW_K_PL && pl.n == 0 && pl.cur == -1 && !lw_pl_shuffle(&pl), "WMA Liste leer");
    CHECK(lw_pl_next(&pl) == -1 && pl.cur == -1, "WMA Next auf leerer Liste");
    CHECK(lw_pl_after(&pl, 0) == -1, "WMA after leer");
    CHECK(lw_pl_add(&pl, LW_ENTRY_FILE, "z:\\A.wma", 0, 0, &id) == 0 && id == LW_SONG_ID_BASE, "WMA Add 1 id=%X", id);
    CHECK(lw_pl_add(&pl, LW_ENTRY_SOUNDTRACK, NULL, 3, 9, &id2) == 0 && id2 == LW_SONG_ID_BASE + 1, "WMA Add HDD");
    CHECK(lw_pl_add(&pl, LW_ENTRY_FILE, "z:\\B.wma", 0, 0, NULL) == 0 && pl.n == 3, "WMA Add 3");
    CHECK(lw_pl_playable(&pl, 0) && !lw_pl_playable(&pl, 1) && lw_pl_playable(&pl, 2) && !lw_pl_playable(&pl, 3) &&
              !lw_pl_playable(&pl, -1),
          "WMA abspielbar: Datei ja, Festplatten-Soundtrack nein");
    CHECK(strcmp(pl.e[2].path, "z:\\B.wma") == 0 && pl.e[1].st_id == 3 && pl.e[1].song_id == 9, "WMA Eintraege");
    CHECK(lw_pl_next(&pl) == 0 && lw_pl_next(&pl) == 1 && lw_pl_next(&pl) == 2 && lw_pl_next(&pl) == 0, "WMA Next reihum");
    CHECK(lw_pl_after(&pl, 0) == 1 && lw_pl_after(&pl, 2) == 0, "WMA after mit Wiederholen (Flag 2)");
    pl.flags = 0;
    CHECK(lw_pl_after(&pl, 2) == -1 && lw_pl_after(&pl, 1) == 2, "WMA after ohne Wiederholen endet");
    /* Zufall: nie dasselbe Lied hintereinander, alle Lieder kommen vor, gleiche Folge bei gleichem Startwert */
    lw_pl_init(&pl, 1, 1, 3u);
    for (i = 0; i < 4; i++) {
        lw_pl_add(&pl, LW_ENTRY_FILE, "z:\\X.wma", 0, 0, NULL);
    }
    CHECK(lw_pl_shuffle(&pl), "WMA Zufall aus Flag 1");
    for (prev = 0, k = 0; k < 400; k++) {
        int nx = lw_pl_after(&pl, prev);
        same += nx == prev;
        if (nx >= 0 && nx < 4) {
            seen[nx]++;
        }
        if (k < 16) {
            seq[k] = nx;
        }
        prev = nx;
    }
    CHECK(same == 0 && seen[0] && seen[1] && seen[2] && seen[3], "WMA Zufall (gleich %d, %d/%d/%d/%d)", same, seen[0],
          seen[1], seen[2], seen[3]);
    lw_pl_init(&pl, 1, 1, 3u);
    for (i = 0; i < 4; i++) {
        lw_pl_add(&pl, LW_ENTRY_FILE, "z:\\X.wma", 0, 0, NULL);
    }
    for (prev = 0, k = 0; k < 16; k++) {
        seq2[k] = prev = lw_pl_after(&pl, prev);
    }
    CHECK(memcmp(seq, seq2, sizeof seq) == 0, "WMA Zufall deterministisch");
    pl.behavior = 2u;
    CHECK(!lw_pl_shuffle(&pl) && lw_pl_after(&pl, 1) == 2, "WMA SetPlaybackBehavior 2 = der Reihe nach");
    /* Volle Liste */
    lw_pl_init(&pl, 1, 1, 2u);
    for (i = 0; i < LW_MAX_SONGS; i++) {
        lw_pl_add(&pl, LW_ENTRY_FILE, "z:\\X.wma", 0, 0, NULL);
    }
    CHECK(lw_pl_add(&pl, LW_ENTRY_FILE, "z:\\X.wma", 0, 0, &id) != 0 && pl.n == LW_MAX_SONGS, "WMA Liste voll");
    { /* langer Pfad wird gekuerzt, nie ueber den Puffer */
        char lp[300];
        memset(lp, 'a', sizeof lp - 1);
        lp[sizeof lp - 1] = 0;
        lw_pl_init(&pl, 1, 1, 2u);
        CHECK(lw_pl_add(&pl, LW_ENTRY_FILE, lp, 0, 0, NULL) == 0 && strlen(pl.e[0].path) == LW_PATH - 1, "WMA Pfad gekuerzt");
    }
    /* Fehlerfaelle Dekoder: kein Absturz, Fehlertext, NULL */
    {
        static const uint8_t asf[16] = { 0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                         0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C };
        uint8_t junk[4096];
        int mf = lw_mf_thread_init(err, sizeof err);
        printf("WMA: Media Foundation %s%s\n", mf == 0 ? "verfuegbar" : "FEHLT: ", mf == 0 ? "" : err);
        err[0] = 0;
        CHECK(lw_dec_open(NULL, 0, err, sizeof err) == NULL && err[0], "WMA Dekoder: leere Daten");
        for (i = 0; i < (int)sizeof junk; i++) {
            junk[i] = (uint8_t)(i * 37 + 11);
        }
        err[0] = 0;
        CHECK(lw_dec_open(junk, sizeof junk, err, sizeof err) == NULL && err[0], "WMA Dekoder: kein ASF");
        CHECK(!lw_is_asf(junk, sizeof junk) && !lw_is_asf(asf, 16), "WMA ASF-Kennung");
        memcpy(junk, asf, 16);
        CHECK(lw_is_asf(junk, sizeof junk), "WMA ASF-Kennung erkannt");
        if (mf == 0) {
            lw_dec_t *d;
            err[0] = 0;
            d = lw_dec_open(junk, sizeof junk, err, sizeof err);
            CHECK(d == NULL && err[0], "WMA Dekoder: kaputter ASF-Kopf abgelehnt (%s)", err);
            if (d) {
                lw_dec_close(d);
            }
            printf("WMA: kaputte Datei -> \"%s\"\n", err);
        }
    }
    lw_dec_close(NULL);
    printf("WMA (W367): Playlist-Modell, Zufall, Fehlerfaelle geprueft\n");
}

/* W367: echte WMA-Datei (nur lesen) vollstaendig dekodieren und nur Kennzahlen ausgeben. */
static int wma_file(const char *path)
{
    uint32_t n = 0;
    uint8_t *d = NULL;
    char err[256];
    FILE *f = fopen(path, "rb");
    if (f) { /* ganz lesen (slurp liest nur 1 MB Kopf) */
        long sz;
        fseek(f, 0, SEEK_END);
        sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < (64L << 20) && (d = (uint8_t *)malloc((size_t)sz)) != NULL) {
            n = (uint32_t)fread(d, 1, (size_t)sz, f);
        }
        fclose(f);
    }
    lw_dec_t *dec;
    uint8_t *buf;
    uint64_t frames = 0;
    double sum = 0, pk = 0;
    int rate = 0, ch = 0, r;
    if (!d) {
        printf("WMA: %s nicht lesbar\n", path);
        return 1;
    }
    err[0] = 0;
    dec = lw_dec_open(d, n, err, sizeof err);
    if (!dec) {
        printf("WMA: Dekoder-Fehler: %s\n", err);
        free(d);
        return 1;
    }
    lw_dec_format(dec, &rate, &ch);
    buf = (uint8_t *)malloc(65536);
    for (;;) {
        uint32_t got = 0, i;
        r = lw_dec_read(dec, buf, 65536, &got);
        for (i = 0; i + 1 < got; i += 2) {
            short v;
            memcpy(&v, buf + i, 2);
            sum += (double)v * v;
            pk = fabs((double)v) > pk ? fabs((double)v) : pk;
        }
        frames += got / (uint32_t)(2 * ch);
        if (r != 0) {
            break;
        }
    }
    printf("WMA: %u Bytes, %d Hz, %d Kan., Dauer laut Kopf %.2f s, dekodiert %.2f s (%llu Frames), RMS %.1f dBFS, "
           "Spitze %.1f dBFS, Ende %s\n",
           n, rate, ch, lw_dec_secs(dec), rate ? (double)frames / rate : 0, (unsigned long long)frames,
           frames ? 20 * log10(sqrt(sum / ((double)frames * ch)) / 32768.0) : -999.0, pk > 0 ? 20 * log10(pk / 32768.0) : -999.0,
           r == 1 ? "sauber" : "FEHLER");
    lw_dec_close(dec);
    free(buf);
    free(d);
    return r == 1 && frames > 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    uint32_t nx, ns, i;
    short *ad_src = NULL;
    uint8_t *x;
    if (argc > 1) {
        size_t al = strlen(argv[1]);
        if (al > 4 && _stricmp(argv[1] + al - 4, ".wma") == 0) {
            return wma_file(argv[1]); /* W367 */
        }
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
        /* W413 (#65): Schleifenzahl je Spur - direkter Eintrag einmal, Sound-Spur aus +6, Tabelle ohne Angabe */
        CHECK(lxf_xsb_cue(&xs, 0, 0, &c) == 0 && c.track[0].loopc == 0, "W413 direkter Eintrag loopc=%d", c.track[0].loopc);
        CHECK(lxf_xsb_cue(&xs, 2, 0, &c) == 0 && c.track[0].loopc == 0 && c.track[1].loopc == -1,
              "W413 Sound-Liste loopc=%d/%d", c.track[0].loopc, c.track[1].loopc);
        {
            uint8_t *s2 = (uint8_t *)malloc(ns);
            uint32_t snd = 0x38 + 3 * 0x14 + 4 * 0x14 + 3 * 16 + 32 + 20 + 20 + 8; /* wie make_xsb */
            int k2, mode_ok;
            memcpy(s2, s, ns);
            s2[snd + 26] = 0xFF; s2[snd + 27] = 0xFF; /* direkte Sound-Spur: endlos */
            CHECK(lxf_xsb_parse(s2, ns, &xs) == 0 && lxf_xsb_cue(&xs, 2, 0, &c) == 0 && c.track[0].loopc == 0xFFFF,
                  "W413 Sound-Spur endlos loopc=%d", c.track[0].loopc);
            s2[snd + 26] = 5; s2[snd + 27] = 0;
            CHECK(lxf_xsb_parse(s2, ns, &xs) == 0 && lxf_xsb_cue(&xs, 2, 0, &c) == 0 && c.track[0].loopc == 5,
                  "W413 Sound-Spur 5 loopc=%d", c.track[0].loopc);
            /* W456 (#93): Huellkurven-Ereignis 0x0A wie TE017 (Bank.xsb): Delay 0, Attack 0x2E, Hold 0, Decay 0,
             * Release 0x232, Sustain 0xFF, Modus 1; Spuren unveraendert, direkter Eintrag ohne Huellkurve */
            s2[snd + 10] = 0x2E; s2[snd + 11] = 0; s2[snd + 16] = 0x32; s2[snd + 17] = 0x02; s2[snd + 18] = 0xFF;
            s2[snd + 19] = 1; s2[snd + 8] = 3; s2[snd + 12] = 4; s2[snd + 14] = 5;
            CHECK(lxf_xsb_parse(s2, ns, &xs) == 0 && lxf_xsb_cue(&xs, 2, 0, &c) == 0 && c.eg_on == 1 && c.eg[0] == 1 &&
                      c.eg[1] == 3 && c.eg[2] == 0x2E && c.eg[3] == 4 && c.eg[4] == 5 && c.eg[5] == 0x232 && c.eg[6] == 0xFF &&
                      c.ntrack == 2 && c.track[0].bank == 2 && c.track[1].bank == 0,
                  "W456 Huellkurve on=%d %d/%d/%d/%d/%d/%d/%d", c.eg_on, c.eg[0], c.eg[1], c.eg[2], c.eg[3], c.eg[4], c.eg[5],
                  c.eg[6]);
            CHECK(lxf_xsb_cue(&xs, 0, 0, &c) == 0 && c.eg_on == 0, "W456 direkter Eintrag ohne Huellkurve");
            CHECK(c.has3d == 0, "W456 ohne 3D-Tabelle (Kopf +0x10 = 0) kein 3D-Satz");
            free(s2);
            /* W456: 3D-Tabelle (Kopf +0x10) mit 2 Saetzen wie Bank.xsb (Satz 0: Min 100/Max 100000, Satz 1: Min 1);
             * Eintrag 0 (Flag 0x09, Bit 0) -> Satz 1, Eintrag 3 (Flag 0x01) -> Satz 0, Eintrag 1 (Flag 0x09) Index
             * ausserhalb -> kein Satz */
            {
                uint32_t n3 = ns + 2 * 0x28, t3 = ns, ent = 0x38 + 3 * 0x14;
                float f100 = 100.0f, f1e5 = 100000.0f, f1 = 1.0f;
                s2 = (uint8_t *)calloc(1, n3);
                memcpy(s2, s, ns);
                memcpy(s2 + 0x10, &t3, 4);
                memcpy(s2 + t3 + 8, &f100, 4); memcpy(s2 + t3 + 0xC, &f1e5, 4); memcpy(s2 + t3 + 0x14, &f1, 4);
                memcpy(s2 + t3 + 0x28 + 8, &f1, 4); memcpy(s2 + t3 + 0x28 + 0xC, &f1e5, 4); memcpy(s2 + t3 + 0x28 + 0x14, &f1, 4);
                s2[ent + 0xC] = 1; /* Eintrag 0 -> Satz 1 */
                s2[ent + 0x14 + 0xC] = 7; /* Eintrag 1 -> Satz 7 (ausserhalb) */
                CHECK(lxf_xsb_parse(s2, n3, &xs) == 0 && lxf_xsb_cue(&xs, 0, 0, &c) == 0 && c.has3d == 1 && c.min3d == 1.0f &&
                          c.max3d == 100000.0f && c.rolloff3d == 1.0f && c.mode3d == 0,
                      "W456 3D-Satz 1: has=%d min=%.1f max=%.1f", c.has3d, c.min3d, c.max3d);
                CHECK(lxf_xsb_cue(&xs, 2, 0, &c) == 0 && c.has3d == 1 && c.min3d == 100.0f && c.ntrack == 2,
                      "W456 3D-Satz 0 (Sound-Liste): has=%d min=%.1f", c.has3d, c.min3d);
                for (k2 = 0; k2 < 16; k2++) { /* Cue 1: Zufall auf Eintrag 1 (Satz ausserhalb) bzw. 2 (Flag 0x11, Bit 0, Satz 0) */
                    CHECK(lxf_xsb_cue(&xs, 1, (uint32_t)k2, &c) == 0, "W456 Cue 1");
                    if (c.entry == 1) CHECK(c.has3d == 0, "W456 Satz ausserhalb der Datei angenommen");
                }
                printf("W456 XSB-3D-Satz: Min/Max/Rolloff/Modus je Eintrag, ausserhalb der Datei verworfen: OK\n");
            }
            printf("W456 XSB-Huellkurve (Ereignis 0x0A): Delay/Attack/Hold/Decay/Release/Sustain/Modus: OK\n");
            free(s2);
            (void)lxf_xsb_parse(s, ns, &xs);
            mode_ok = lxf_track_loop_mode(0, 0xFFFF) == LXF_LOOP_ONCE && lxf_track_loop_mode(1, 0) == LXF_LOOP_ONCE &&
                      lxf_track_loop_mode(1, 0xFFFF) == LXF_LOOP_INF && lxf_track_loop_mode(1, -1) == LXF_LOOP_INF &&
                      lxf_track_loop_mode(1, 5) == LXF_LOOP_N;
            for (k2 = 1; k2 < 0xFFFF; k2 += 97) mode_ok &= lxf_track_loop_mode(1, k2) == LXF_LOOP_N;
            CHECK(mode_ok, "W413 lxf_track_loop_mode");
            printf("W413 XSB-Schleifenzahl: direkt einmal, Spur 0/5/0xFFFF, Tabelle ohne Angabe = Wellen-Flag: OK\n");
        }
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
    { /* W363 LOC-ORIGINAL: RPC-Kurven wie die XACT-Bibliothek (0x1C5392), synthetische Tabelle */
        uint8_t b[0x200];
        lxf_xsb_t xs;
        lxf_rpc_t r[LXF_MAX_TRACKS];
        int h, nrpc_ok = 0;
        static const struct { int x, y; } pt[4] = { { 0, -2000 }, { 16384, 0 }, { 32768, 1000 }, { 65535, 4000 } };
        memset(b, 0, sizeof b);
        memcpy(b, "SDBK", 4);
        w32(b + 0x14, 0x40);
        w16(b + 0x20, 3);
        /* Eintraege: Sound 7 "RPM_A" (2 Kurven), Sound 7 "OTHER" (1 Kurve), Sound 9 "RPM_A" (1 Kurve) */
        w16(b + 0x40, 7); b[0x43] = 2; w32(b + 0x44, 0x100); memcpy(b + 0x48, "RPM_A", 5);
        w16(b + 0x58, 7); b[0x5B] = 1; w32(b + 0x5C, 0x130); memcpy(b + 0x60, "OTHER", 5);
        w16(b + 0x70, 9); b[0x73] = 1; w32(b + 0x74, 0x130); memcpy(b + 0x78, "RPM_A", 5);
        { /* Kurve 0: Spur 0 Tonhoehe; Kurve 1: Spur 0 Filter; Kurve 2 (0x130): Spur 1 Lautstaerke */
            int k, j;
            for (k = 0; k < 3; k++) {
                uint8_t *cv = b + 0x100 + k * 24;
                w16(cv, k == 2 ? 1 : 0);
                w16(cv + 2, k == 0 ? 1 : k == 1 ? 2 : 0);
                for (j = 0; j < 4; j++) {
                    w16(cv + 4 + 4 * j, (uint16_t)pt[j].x);
                    w16(cv + 6 + 4 * j, (uint16_t)(int16_t)(k == 1 ? 1000 * (j + 1) : pt[j].y));
                }
            }
        }
        memset(&xs, 0, sizeof xs);
        xs.d = b;
        xs.n = sizeof b;
        /* Stuetzpunkte, Klemmen, Interpolation (ganzzahlig, zur Null) */
        CHECK(lxf_rpc_eval(b + 0x100, -5) == -2000, "RPC x<x0");
        CHECK(lxf_rpc_eval(b + 0x100, 0) == -2000, "RPC x=x0");
        CHECK(lxf_rpc_eval(b + 0x100, 8192) == -1000, "RPC Mitte Segment 1: %d", lxf_rpc_eval(b + 0x100, 8192));
        CHECK(lxf_rpc_eval(b + 0x100, 16384) == 0, "RPC Stuetzpunkt 1");
        CHECK(lxf_rpc_eval(b + 0x100, 16385) == 0, "RPC zur Null gerundet: %d", lxf_rpc_eval(b + 0x100, 16385));
        CHECK(lxf_rpc_eval(b + 0x100, 49151) == 2499, "RPC Segment 3: %d", lxf_rpc_eval(b + 0x100, 49151));
        CHECK(lxf_rpc_eval(b + 0x100, 70000) == 4000, "RPC x>x3");
        /* Suche: Sound + Name, Ziele, Wert * 65536 */
        h = lxf_xsb_rpc(&xs, 7, "RPM_A", 0.125f, r); /* x = 8192 */
        CHECK(h == 2 && r[0].pitch == -1000 && r[0].filt_hz == 1500 && r[0].has == 6 && r[1].has == 0,
              "RPC Sound 7: h=%d p=%d f=%d has=%d", h, r[0].pitch, r[0].filt_hz, r[0].has);
        h = lxf_xsb_rpc(&xs, 7, "OTHER", 1.0f, r);
        CHECK(h == 1 && r[1].vol_mb == 4000 && r[1].has == 1 && r[0].has == 0, "RPC OTHER Spur 1: h=%d v=%d", h, r[1].vol_mb);
        CHECK(lxf_xsb_rpc(&xs, 8, "RPM_A", 0.5f, r) == 0, "RPC fremder Sound");
        CHECK(lxf_xsb_rpc(&xs, 7, "RPM_B", 0.5f, r) == 0, "RPC fremder Name");
        CHECK(lxf_xsb_rpc(&xs, 9, "RPM_A", 0.0f, r) == 1 && r[1].vol_mb == -2000, "RPC Sound 9");
        { /* Kuerzung: Tabelle/Kurven ausserhalb -> nie lesen */
            uint32_t n;
            for (n = 0; n <= sizeof b; n += 8) {
                xs.n = n;
                nrpc_ok += lxf_xsb_rpc(&xs, 7, "RPM_A", 0.5f, r) >= 0;
            }
            xs.n = sizeof b;
        }
        CHECK(nrpc_ok == (int)(sizeof b / 8) + 1, "RPC Kuerzung");
        printf("RPC (W363): Kurven, Klemmen, Suche, Kuerzung geprueft\n");
    }
    wma_tests(); /* W367 */
    scope_tests(); /* W375 */
    steal_tests(); /* W379 */
    if (g_fail) {
        printf("loc_audio_selftest: %d FEHLER\n", g_fail);
    } else {
        printf("loc_audio_selftest: ALLE OK\n");
    }
    return g_fail ? 1 : 0;
}
