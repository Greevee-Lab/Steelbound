/* audio_fmt.c - DirectSound-HLE: Spielspeicher-Zugriffe (SEH), Format-
 * Parsing (WAVEFORMATEX/DSBUFFERDESC), Dekoder (PCM 8/16, Xbox-ADPCM) und
 * Stimmen-FIFO. W68 SPLIT: 1:1 aus audio_out.c verschoben. */
#include "audio_int.h"

/* ---------- SEH-gesicherte Spielspeicher-Zugriffe ---------- */
int au_probe_read(au32 p, void *dst, au32 n)
{
    int ok = 0;
    if (!p)
        return 0;
    __try {
        memcpy(dst, (void *)(uintptr_t)p, n);
        ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = 0;
    }
    return ok;
}

int au_probe_write(au32 p, const void *src, au32 n)
{
    int ok = 0;
    if (!p)
        return 0;
    __try {
        memcpy((void *)(uintptr_t)p, src, n);
        ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = 0;
    }
    return ok;
}

static int au_probe_u32(au32 p, au32 *out)
{
    return au_probe_read(p, out, 4);
}

/* WAVEFORMATEX validieren (PCM 8/16, Xbox-ADPCM 105). */
int au_wfx_ok(const wfx_t *w)
{
    if (!w)
        return 0;
    if (w->ch < 1 || w->ch > 6)
        return 0;
    if (w->rate < 4000 || w->rate > 192000)
        return 0;
    if (w->tag == 1)
        return (w->bits == 8 || w->bits == 16) &&
            w->align == w->ch * (w->bits / 8);
    if (w->tag == 105)
        return w->bits == 4 && w->align == 36 * w->ch;
    return 0;
}

/* Desc defensiv parsen: DSBUFFERDESC/DSSTREAMDESC beginnen mit dwords, eines
 * davon ist lpwfxFormat. Wir testen desc[0..7] als Zeiger auf gueltiges WFX
 * und desc[1] zusaetzlich als dwBufferBytes (Plausibilitaet). */
int au_parse_desc(au32 kind, au32 desc, wfx_t *wfx, au32 *buflen,
                         au32 *flags)
{
    au32 d[8];
    int i;
    if (wfx)
        memset(wfx, 0, sizeof *wfx);
    if (buflen)
        *buflen = 0;
    if (flags)
        *flags = 0;
    if (!au_probe_read(desc, d, sizeof d))
        return 0;
    if (flags)
        *flags = d[0];
    for (i = 0; i < 8; i++) {
        wfx_t w;
        if (d[i] < 0x10000 || d[i] >= 0x80000000)
            continue; /* kein Spielspeicher -> kein Probe-Fault */
        if (au_probe_read(d[i], &w, sizeof w) && au_wfx_ok(&w)) {
            if (wfx)
                *wfx = w;
            break;
        }
    }
    /* A7B: dwBufferBytes ist desc[2] (DSBUFFERDESC: size/flags/bytes/wfx)!
     * desc[1] = dwFlags (Bink: 160): Lock meldete 160-Byte-Puffer, Bink schrieb
     * 53 KB hinein -> Heap-Tod am Filmstart (4/4 Laeufe). Streams: 0 (Lock
     * alloziert faul 64K). */
    if (buflen) {
        *buflen = 0;
        if (g_cfg.audio_buflen) {
            if (kind == 1 && d[2] > 0 && d[2] < 64 * 1024 * 1024)
                *buflen = d[2];
        } else if (d[1] > 0 && d[1] < 64 * 1024 * 1024) {
            *buflen = d[1]; /* alt (W4): falsch bei Bink */
        }
    }
    return wfx == NULL || ((wfx_t *)wfx)->rate != 0;
}

/* ---------- Xbox-ADPCM (Tag 105, IMA; verifiziert an BGM00.WAV) ----------
 * Blockeinheit: pro Kanal 4-Byte-Header (s16-Predictor, u8-Index, rsv),
 * dann Nutzdaten in 4-Byte-Stuecken reihum (mono: 32 B, stereo: [4L][4R]x8).
 * 64 Samples pro Block, Low-Nibble zuerst. */
static const short ADPCM_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190,
    209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499,
    2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845,
    8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767
};
static const signed char ADPCM_IDX[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};

/* Dekodiert ADPCM-Bytes nach Stereo-float. Rueckgabe: Frames. */
au32 au_adpcm_decode(const uint8_t *src, au32 nbytes, int ch, float *dst,
                            au32 dst_cap_frames)
{
    au32 frame = 36u * (au32)ch; /* Blockeinheit */
    au32 nblocks = nbytes / frame;
    au32 f = 0, b;
    for (b = 0; b < nblocks && f + 64 <= dst_cap_frames; b++) {
        const uint8_t *blk = src + b * frame;
        int c;
        int pred[6];
        int idx[6];
        for (c = 0; c < ch && c < 6; c++) {
            pred[c] = (int)(int16_t)(blk[c * 4] | (blk[c * 4 + 1] << 8));
            idx[c] = blk[c * 4 + 2];
            if (idx[c] < 0)
                idx[c] = 0;
            if (idx[c] > 88)
                idx[c] = 88;
        }
        /* Nutzdaten: 4-Byte-Stuecke reihum auf Kanaele */
        {
            int s;
            for (s = 0; s < 64; s++) {
                int c2;
                float l = 0, r = 0;
                for (c2 = 0; c2 < ch && c2 < 6; c2++) {
                    /* s-tes Nibble des Kanals: Byte-Index im Kanalstrom */
                    au32 boff = (au32)(s / 2);
                    au32 chunk = boff / 4; /* 4-Byte-Stueck-Nr im Kanal */
                    au32 sub = boff % 4;
                    au32 pay = 4u * (au32)ch + (chunk * (au32)ch + (au32)c2) * 4u + sub;
                    uint8_t by = (pay < frame) ? blk[pay] : 0;
                    int nib = (s & 1) ? (by >> 4) : (by & 0xF);
                    int step = ADPCM_STEP[idx[c2]];
                    int diff = step >> 3;
                    if (nib & 1)
                        diff += step >> 2;
                    if (nib & 2)
                        diff += step >> 1;
                    if (nib & 4)
                        diff += step;
                    if (nib & 8)
                        pred[c2] -= diff;
                    else
                        pred[c2] += diff;
                    if (pred[c2] < -32768)
                        pred[c2] = -32768;
                    if (pred[c2] > 32767)
                        pred[c2] = 32767;
                    idx[c2] += ADPCM_IDX[nib];
                    if (idx[c2] < 0)
                        idx[c2] = 0;
                    if (idx[c2] > 88)
                        idx[c2] = 88;
                    if (c2 == 0)
                        l = (float)pred[c2] / 32768.0f;
                    else if (c2 == 1)
                        r = (float)pred[c2] / 32768.0f;
                }
                if (ch == 1)
                    r = l;
                dst[(f + s) * 2] = l;
                dst[(f + s) * 2 + 1] = r;
            }
        }
        f += 64;
    }
    return f;
}

/* PCM (8/16 Bit, 1-6 Kanaele) nach Stereo-float, mit Downmix 5.1->Stereo
 * (FL FR C LFE SL SR) und linearem Resampling auf MIXRATE. Rueckgabe Frames.
 * ratio = Quellrate/MIXRATE (>1 = Quelle schneller). */
au32 au_pcm_convert(const uint8_t *src, au32 nbytes, int ch, int bits,
                           au32 src_rate, float *dst, au32 dst_cap_frames)
{
    au32 bps = (au32)(bits / 8);
    au32 sframe = bps * (au32)ch;
    au32 nsrc = sframe ? nbytes / sframe : 0;
    au32 out = 0;
    uint64_t pos = 0; /* 16.16 Quell-Position */
    uint64_t step = src_rate ? ((uint64_t)src_rate << 16) / MIXRATE : 65536;
    if (!sframe || !nsrc || !dst_cap_frames)
        return 0;
    if (step == 0)
        step = 65536;
    while (out < dst_cap_frames) {
        au32 i0 = (au32)(pos >> 16);
        au32 frac = (au32)(pos & 0xFFFF);
        au32 i1;
        float s0[6], s1[6];
        int c;
        float l, r;
        if (i0 >= nsrc)
            break;
        i1 = (i0 + 1 < nsrc) ? i0 + 1 : i0;
        for (c = 0; c < ch && c < 6; c++) {
            const uint8_t *p0 = src + i0 * sframe + c * bps;
            const uint8_t *p1 = src + i1 * sframe + c * bps;
            float v0, v1;
            if (bits == 16) {
                v0 = (float)(int16_t)(p0[0] | (p0[1] << 8)) / 32768.0f;
                v1 = (float)(int16_t)(p1[0] | (p1[1] << 8)) / 32768.0f;
            } else {
                v0 = ((float)p0[0] - 128.0f) / 128.0f;
                v1 = ((float)p1[0] - 128.0f) / 128.0f;
            }
            s0[c] = v0;
            s1[c] = v1;
        }
        /* linear interpolieren + Downmix */
        {
            float m[6];
            for (c = 0; c < ch && c < 6; c++)
                m[c] = s0[c] + (s1[c] - s0[c]) * ((float)frac / 65536.0f);
            if (ch == 1) {
                l = r = m[0];
            } else if (ch == 2) {
                l = m[0];
                r = m[1];
            } else {
                /* 5.1: FL FR C LFE SL SR */
                float fl = m[0], fr = m[1];
                float ce = ch > 2 ? m[2] : 0;
                float lfe = ch > 3 ? m[3] : 0;
                float sl = ch > 4 ? m[4] : 0;
                float sr = ch > 5 ? m[5] : 0;
                l = fl + 0.707f * ce + 0.5f * lfe + 0.707f * sl;
                r = fr + 0.707f * ce + 0.5f * lfe + 0.707f * sr;
            }
        }
        dst[out * 2] = l;
        dst[out * 2 + 1] = r;
        out++;
        pos += step;
    }
    return out;
}

/* ---------- FIFO ---------- */
static void au_fifo_reserve(audio_voice_t *v, au32 add)
{
    if (v->fifo_n + add <= v->fifo_cap)
        return;
    {
        au32 want = v->fifo_n + add;
        au32 ncap = v->fifo_cap ? v->fifo_cap : 16384;
        float *nf;
        while (ncap < want)
            ncap *= 2;
        if ((uint64_t)ncap * 8 > FIFO_HARD_CAP)
            return; /* Drop statt OOM (Zaehler in append) */
        nf = (float *)malloc((size_t)ncap * 2 * sizeof(float));
        if (!nf)
            return;
        if (v->fifo && v->fifo_n) {
            au32 i;
            for (i = 0; i < v->fifo_n; i++) {
                au32 s = (v->fifo_rd + i) % v->fifo_cap;
                nf[i * 2] = v->fifo[s * 2];
                nf[i * 2 + 1] = v->fifo[s * 2 + 1];
            }
            free(v->fifo);
        }
        v->fifo = nf;
        v->fifo_cap = ncap;
        v->fifo_rd = 0;
        v->fifo_wr = v->fifo_n;
    }
}

au32 au_fifo_append(audio_voice_t *v, const float *s, au32 n)
{
    au32 i;
    au_fifo_reserve(v, n);
    for (i = 0; i < n; i++) {
        if (v->fifo_n >= v->fifo_cap) {
            g_stat_drops++;
            break;
        }
        v->fifo[v->fifo_wr * 2] = s[i * 2];
        v->fifo[v->fifo_wr * 2 + 1] = s[i * 2 + 1];
        v->fifo_wr = (v->fifo_wr + 1) % v->fifo_cap;
        v->fifo_n++;
    }
    return i;
}
