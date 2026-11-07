/* sb_sha256.c - SHA-256 nach FIPS 180-4 (W148). Eigene kurze Fassung
 * (Transform + Streaming); Testvektoren: "" / "abc" / 56-Byte-NIST-String.
 */
#include "sb_sha256.h"
#include <stdio.h>
#include <string.h>

static const uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define RR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void xform(uint32_t h[8], const uint8_t b[64])
{
    uint32_t w[64], a, bb, c, d, e, f, g, hh, t1, t2;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)b[i * 4] << 24) | ((uint32_t)b[i * 4 + 1] << 16) |
               ((uint32_t)b[i * 4 + 2] << 8) | (uint32_t)b[i * 4 + 3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = RR(w[i - 15], 7) ^ RR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = RR(w[i - 2], 17) ^ RR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0];
    bb = h[1];
    c = h[2];
    d = h[3];
    e = h[4];
    f = h[5];
    g = h[6];
    hh = h[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = RR(e, 6) ^ RR(e, 11) ^ RR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        t1 = hh + S1 + ch + kK[i] + w[i];
        {
            uint32_t S0 = RR(a, 2) ^ RR(a, 13) ^ RR(a, 22);
            uint32_t maj = (a & bb) ^ (a & c) ^ (bb & c);
            t2 = S0 + maj;
        }
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = bb;
        bb = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += bb;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

void sb_sha256_init(sb_sha256_t *c)
{
    c->h[0] = 0x6a09e667u;
    c->h[1] = 0xbb67ae85u;
    c->h[2] = 0x3c6ef372u;
    c->h[3] = 0xa54ff53au;
    c->h[4] = 0x510e527fu;
    c->h[5] = 0x9b05688cu;
    c->h[6] = 0x1f83d9abu;
    c->h[7] = 0x5be0cd19u;
    c->len = 0;
    c->nbuf = 0;
}

void sb_sha256_update(sb_sha256_t *c, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    c->len += (uint64_t)n;
    while (n > 0) {
        size_t want = 64 - c->nbuf, take = n < want ? n : want;
        memcpy(c->buf + c->nbuf, p, take);
        c->nbuf += (unsigned)take;
        p += take;
        n -= take;
        if (c->nbuf == 64) {
            xform(c->h, c->buf);
            c->nbuf = 0;
        }
    }
}

void sb_sha256_final(sb_sha256_t *c, uint8_t out[32])
{
    uint8_t pad[64];
    uint8_t lb[8];
    unsigned i;
    uint64_t bits = c->len * 8u;
    pad[0] = 0x80;
    memset(pad + 1, 0, sizeof pad - 1);
    for (i = 0; i < 8; i++) { lb[i] = (uint8_t)(bits >> (56 - 8 * i)); }
    if (c->nbuf < 56) {
        sb_sha256_update(c, pad, 56 - c->nbuf);
    } else {
        sb_sha256_update(c, pad, 120 - c->nbuf);
    }
    sb_sha256_update(c, lb, 8);
    for (i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)c->h[i];
    }
    c->nbuf = 0;
}

void sb_sha256(const void *data, size_t n, uint8_t out[32])
{
    sb_sha256_t c;
    sb_sha256_init(&c);
    sb_sha256_update(&c, data, n);
    sb_sha256_final(&c, out);
}

int sb_sha256_file(const char *path, uint8_t out[32])
{
    FILE *f = NULL;
    sb_sha256_t c;
    uint8_t b[65536];
    size_t n;
    if (fopen_s(&f, path, "rb") != 0 || !f) { return 0; }
    sb_sha256_init(&c);
    for (;;) {
        n = fread(b, 1, sizeof b, f);
        if (n) { sb_sha256_update(&c, b, n); }
        if (n < sizeof b) { break; }
    }
    fclose(f);
    sb_sha256_final(&c, out);
    return 1;
}

void sb_sha256_hex(const uint8_t in[32], char hex[65])
{
    static const char *d = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) {
        hex[i * 2] = d[in[i] >> 4];
        hex[i * 2 + 1] = d[in[i] & 15];
    }
    hex[64] = '\0';
}
