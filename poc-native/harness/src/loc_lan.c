/* loc_lan.c - W454 LOC-LAN-MODE: reiner Kern des LAN-Modus (Entdeckungspaket SBL1, Raumschluessel). Siehe loc_lan.h
 * und net/SECURITY.md §36. Keine Sockets, kein Log, keine Konfiguration. */
#include "loc_lan.h"
#include "sb_sha256.h"
#include "sb_e2e.h"
#include <string.h>

#define B_MAGIC 0
#define B_PROTO 4
#define B_RSV0 5
#define B_TPORT 6
#define B_CID 8
#define B_ROOMID 12
#define B_NONCE 16
#define B_PUB 32
#define B_ROOM 64  /* u8 Laenge + 16 Byte (NUL-gefuellt) */
#define B_PILOT 81 /* u8 Laenge + 16 Byte */
#define B_VER 98   /* 16 Byte NUL-gefuellt (hoechstens 15 Zeichen) */
#define B_MODE 114
#define B_RSV1 115

static void be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32w(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static int name_ok(const char *s, int n)
{
    int i;
    if (n < 1 || n > LLAN_NAME_MAX) return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7E) return 0;
    }
    return 1;
}

static int ver_char(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '+' ||
           c == '-';
}

static int ver_ok(const char *s, int n)
{
    int i;
    if (n < 1 || n > LLAN_VER_MAX) return 0;
    for (i = 0; i < n; i++) {
        if (!ver_char((unsigned char)s[i])) return 0;
    }
    return 1;
}

int llan_room_norm(const char *in, char *out, int cap)
{
    int a = 0, e, n, i;
    if (!out || cap < LLAN_NAME_MAX + 1) return 0;
    out[0] = 0;
    if (!in) in = "";
    e = (int)strlen(in);
    while (a < e && in[a] == ' ') a++;
    while (e > a && in[e - 1] == ' ') e--;
    n = e - a;
    if (n == 0) {
        strcpy_s(out, (size_t)cap, "LAN");
        return 1;
    }
    if (!name_ok(in + a, n)) return 0;
    for (i = 0; i < n; i++) out[i] = in[a + i];
    out[n] = 0;
    return 1;
}

void llan_pilot_norm(const char *in, char *out, int cap)
{
    int i, n = 0;
    if (!out || cap < 2) return;
    if (!in) in = "";
    for (i = 0; in[i] && n < LLAN_NAME_MAX && n < cap - 1; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c & 0xC0) == 0x80) continue;          /* UTF-8-Folgebytes: ein '?' je Zeichen */
        out[n++] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = 0;
    if (n == 0 || out[0] == ' ') { /* leer bzw. fuehrendes Leerzeichen -> Standard */
        int k = 0;
        while (out[k] == ' ') k++;
        if (!out[k]) {
            strcpy_s(out, (size_t)cap, "Pilot");
        } else {
            memmove(out, out + k, strlen(out + k) + 1);
        }
    }
}

uint32_t llan_room_id(const char *room)
{
    static const char L[] = "steelbound-lan-room";
    uint8_t h[32];
    sb_sha256_t c;
    sb_sha256_init(&c);
    sb_sha256_update(&c, L, sizeof L - 1);
    sb_sha256_update(&c, room, strlen(room));
    sb_sha256_final(&c, h);
    return rd32(h);
}

void llan_psk(const char *room, const char *pass, int iter, uint8_t psk[32])
{
    /* PBKDF2-HMAC-SHA256 (RFC 8018) mit einem Block (32 Byte). Schluessel = Passwort (hoechstens 63 Byte, also ohne
     * Vor-Hash in HMAC). */
    static const char S[] = "steelbound-lan-v1|";
    uint8_t salt[sizeof S - 1 + LLAN_NAME_MAX + 4], u[32], t[32];
    int sl = (int)(sizeof S - 1), rl = room ? (int)strnlen(room, LLAN_NAME_MAX) : 0, pl, i, k;
    if (!pass) pass = "";
    pl = (int)strnlen(pass, LLAN_PASS_MAX);
    if (iter <= 0) iter = LLAN_KDF_ITER;
    memcpy(salt, S, (size_t)sl);
    if (rl) memcpy(salt + sl, room, (size_t)rl);
    sl += rl;
    salt[sl++] = 0;
    salt[sl++] = 0;
    salt[sl++] = 0;
    salt[sl++] = 1; /* INT(1) */
    sbe2e_hmac((const uint8_t *)pass, pl, salt, sl, u);
    memcpy(t, u, 32);
    for (i = 1; i < iter; i++) {
        sbe2e_hmac((const uint8_t *)pass, pl, u, 32, u);
        for (k = 0; k < 32; k++) t[k] ^= u[k];
    }
    memcpy(psk, t, 32);
    memset(u, 0, sizeof u);
    memset(t, 0, sizeof t);
}

void llan_pair_nonce(const uint8_t nonce_lo[16], const uint8_t nonce_hi[16], uint8_t pn[16])
{
    static const char L[] = "steelbound-lan-pn";
    uint8_t h[32];
    sb_sha256_t c;
    sb_sha256_init(&c);
    sb_sha256_update(&c, L, sizeof L - 1);
    sb_sha256_update(&c, nonce_lo, 16);
    sb_sha256_update(&c, nonce_hi, 16);
    sb_sha256_final(&c, h);
    memcpy(pn, h, 16);
}

void llan_th_mix(uint8_t th[32], const uint8_t psk[32])
{
    uint8_t m[7 + 32];
    memcpy(m, "sbl1 th", 7);
    memcpy(m + 7, th, 32);
    sbe2e_hmac(psk, 32, m, (int)sizeof m, th);
    memset(m, 0, sizeof m);
}

int llan_beacon_build(uint8_t *b, int cap, const llan_beacon *in)
{
    int rl, pl, vl;
    if (!b || !in || cap < LLAN_BEACON_LEN) return 0;
    rl = (int)strnlen(in->room, sizeof in->room);
    pl = (int)strnlen(in->pilot, sizeof in->pilot);
    vl = (int)strnlen(in->ver, sizeof in->ver);
    if (!in->tport || !in->cid || !name_ok(in->room, rl) || !name_ok(in->pilot, pl) || !ver_ok(in->ver, vl) ||
        (in->mode != 1 && in->mode != 2) || in->room_id != llan_room_id(in->room)) {
        return 0;
    }
    memset(b, 0, LLAN_BEACON_LEN);
    memcpy(b + B_MAGIC, "SBL1", 4);
    b[B_PROTO] = LLAN_PROTO;
    be16(b + B_TPORT, in->tport);
    be32w(b + B_CID, in->cid);
    be32w(b + B_ROOMID, in->room_id);
    memcpy(b + B_NONCE, in->nonce, 16);
    memcpy(b + B_PUB, in->pub, 32);
    b[B_ROOM] = (uint8_t)rl;
    memcpy(b + B_ROOM + 1, in->room, (size_t)rl);
    b[B_PILOT] = (uint8_t)pl;
    memcpy(b + B_PILOT + 1, in->pilot, (size_t)pl);
    memcpy(b + B_VER, in->ver, (size_t)vl);
    b[B_MODE] = (uint8_t)in->mode;
    return LLAN_BEACON_LEN;
}

static int zero_from(const uint8_t *p, int from, int to)
{
    int i;
    for (i = from; i < to; i++) {
        if (p[i]) return 0;
    }
    return 1;
}

int llan_beacon_parse(const uint8_t *b, int len, llan_beacon *out)
{
    llan_beacon o;
    int rl, pl, vl, i, zero_pub = 1;
    if (!b || !out || len != LLAN_BEACON_LEN) return 0;
    if (memcmp(b + B_MAGIC, "SBL1", 4) != 0 || b[B_PROTO] != LLAN_PROTO || b[B_RSV0] != 0 || b[B_RSV1] != 0) return 0;
    memset(&o, 0, sizeof o);
    o.tport = rd16(b + B_TPORT);
    o.cid = rd32(b + B_CID);
    o.room_id = rd32(b + B_ROOMID);
    if (o.tport == 0 || o.cid == 0) return 0;
    memcpy(o.nonce, b + B_NONCE, 16);
    memcpy(o.pub, b + B_PUB, 32);
    for (i = 0; i < 32; i++) zero_pub &= o.pub[i] == 0;
    if (zero_pub) return 0;
    rl = b[B_ROOM];
    pl = b[B_PILOT];
    if (rl < 1 || rl > LLAN_NAME_MAX || pl < 1 || pl > LLAN_NAME_MAX) return 0;
    if (!name_ok((const char *)b + B_ROOM + 1, rl) || !zero_from(b, B_ROOM + 1 + rl, B_PILOT)) return 0;
    if (!name_ok((const char *)b + B_PILOT + 1, pl) || !zero_from(b, B_PILOT + 1 + pl, B_VER)) return 0;
    for (vl = 0; vl < LLAN_VER_MAX && b[B_VER + vl]; vl++) {
    }
    if (!ver_ok((const char *)b + B_VER, vl) || !zero_from(b, B_VER + vl, B_MODE)) return 0;
    if (b[B_MODE] != 1 && b[B_MODE] != 2) return 0;
    memcpy(o.room, b + B_ROOM + 1, (size_t)rl);
    memcpy(o.pilot, b + B_PILOT + 1, (size_t)pl);
    memcpy(o.ver, b + B_VER, (size_t)vl);
    o.mode = b[B_MODE];
    if (o.room_id != llan_room_id(o.room)) return 0;
    *out = o;
    return 1;
}
