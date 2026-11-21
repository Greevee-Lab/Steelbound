/* sb_e2e.c - W334 LOC-SEC-CLIENT: SBT2 (siehe sb_e2e.h, SECURITY.md §4).
 * W407 CRYPTO-PORTABLE: X25519 und AES-256-GCM aus BearSSL 0.6 (third_party/bearssl, unveraendert, MIT) statt
 * Windows-CNG, damit jede Plattform dieselben Bausteine nutzt (CNG fehlte u. a. Curve25519 in manchen
 * Umgebungen -> Selbsttest rot -> Online aus). Varianten bewusst zeitkonstant: aes_ct (bitsliced), ghash_ctmul,
 * c25519_m31 (Montgomery-Leiter mit cswap). HMAC/HKDF ueber sb_sha256, Zufall weiter aus dem System-CSPRNG
 * (BCryptGenRandom). Protokoll und Bytes unveraendert; Selbsttest (RFC-Vektoren) und sbt2_vectors.json pruefen das. */
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "sb_e2e.h"
#include "sb_sha256.h"
#include "bearssl_block.h"
#include "bearssl_hash.h"
#include "bearssl_aead.h"
#include "bearssl_ec.h"

#pragma comment(lib, "bcrypt.lib")

/* Schluesselobjekte (frueher BCRYPT_KEY_HANDLE): nur lesend benutzt, daher von mehreren Threads gleichzeitig
 * verwendbar; der GCM-Zustand liegt je Aufruf auf dem Stack. Freigabe immer mit Loeschen. */
typedef struct e2e_gcmkey {
    br_aes_ct_ctr_keys ctr; /* AES-256-Schluesselplan (CTR, zeitkonstant) */
} e2e_gcmkey;

typedef struct e2e_xkey {
    uint8_t priv[32]; /* X25519-Privatschluessel, RFC-7748-Form, geklemmt */
} e2e_xkey;

static void *secure_alloc(size_t n)
{
    void *p = calloc(1, n);
    return p;
}

static void secure_free(void *p, size_t n)
{
    if (p) {
        SecureZeroMemory(p, n);
        free(p);
    }
}

static int alg_ready(void)
{
    return 1; /* keine Laufzeit-Abhaengigkeit mehr; Korrektheit prueft sbe2e_selftest */
}

/* ====================================================================== Hilfen */
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t rd32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void be64(uint8_t *p, uint64_t v) { be32(p, (uint32_t)(v >> 32)); be32(p + 4, (uint32_t)v); }
static uint64_t rd64(const uint8_t *p) { return ((uint64_t)rd32(p) << 32) | rd32(p + 4); }

int sbe2e_ct_eq(const void *a, const void *b, int n)
{
    const volatile uint8_t *x = (const volatile uint8_t *)a, *y = (const volatile uint8_t *)b;
    uint8_t d = 0;
    int i;
    for (i = 0; i < n; i++) d |= (uint8_t)(x[i] ^ y[i]);
    return d == 0;
}

int sbe2e_random(void *p, int n)
{
    if (n <= 0) return 0;
    return BCryptGenRandom(NULL, (PUCHAR)p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
}

/* ====================================================================== HMAC / HKDF (RFC 2104 / 5869) */
void sbe2e_hmac(const uint8_t *key, int klen, const void *msg, int mlen, uint8_t out[32])
{
    uint8_t k[64], pad[64], inner[32];
    sb_sha256_t c;
    int i;
    memset(k, 0, sizeof k);
    if (klen > 64) {
        sb_sha256(key, (size_t)klen, k);
    } else if (klen > 0) {
        memcpy(k, key, (size_t)klen);
    }
    for (i = 0; i < 64; i++) pad[i] = (uint8_t)(k[i] ^ 0x36);
    sb_sha256_init(&c);
    sb_sha256_update(&c, pad, 64);
    if (mlen > 0) sb_sha256_update(&c, msg, (size_t)mlen);
    sb_sha256_final(&c, inner);
    for (i = 0; i < 64; i++) pad[i] = (uint8_t)(k[i] ^ 0x5c);
    sb_sha256_init(&c);
    sb_sha256_update(&c, pad, 64);
    sb_sha256_update(&c, inner, 32);
    sb_sha256_final(&c, out);
    SecureZeroMemory(k, sizeof k);
    SecureZeroMemory(pad, sizeof pad);
    SecureZeroMemory(inner, sizeof inner);
    SecureZeroMemory(&c, sizeof c);
}

void sbe2e_hkdf_extract(const uint8_t *salt, int slen, const uint8_t *ikm, int ilen, uint8_t prk[32])
{
    static const uint8_t zero[32] = {0};
    if (!salt || slen <= 0) {
        salt = zero;
        slen = 32;
    }
    sbe2e_hmac(salt, slen, ikm, ilen, prk);
}

int sbe2e_hkdf_expand(const uint8_t prk[32], const void *info, int ilen, uint8_t *out, int len)
{
    uint8_t t[32], m[32 + 128 + 1];
    int done = 0, tl = 0;
    uint8_t i = 1;
    if (len <= 0 || len > 255 * 32 || ilen < 0 || ilen > 128) return -1;
    while (done < len) {
        int n = len - done > 32 ? 32 : len - done;
        memcpy(m, t, (size_t)tl);
        if (ilen) memcpy(m + tl, info, (size_t)ilen);
        m[tl + ilen] = i++;
        sbe2e_hmac(prk, 32, m, tl + ilen + 1, t);
        tl = 32;
        memcpy(out + done, t, (size_t)n);
        done += n;
    }
    SecureZeroMemory(t, sizeof t);
    SecureZeroMemory(m, sizeof m);
    return 0;
}

/* ====================================================================== AES-256-GCM (BearSSL aes_ct + ghash_ctmul) */
static void *gcm_key(const uint8_t key[32])
{
    e2e_gcmkey *k = (e2e_gcmkey *)secure_alloc(sizeof *k);
    if (!k) return NULL;
    br_aes_ct_ctr_init(&k->ctr, key, 32);
    return k;
}

static void gcm_key_free(void *key)
{
    secure_free(key, sizeof(e2e_gcmkey));
}

/* Ein Aufruf = ein Paket. in/out duerfen nicht ueberlappen (ausser in == out). Beim Oeffnen wird der Tag
 * zeitkonstant geprueft (br_gcm_check_tag); bei Fehler wird der Klartext geloescht (nie unauthentisch zurueck). */
static int gcm_run(void *key, int enc, const uint8_t iv[12], const uint8_t *aad, int alen, const uint8_t *in, int n,
                   uint8_t *out, uint8_t tag[16])
{
    /* nicht const: br_gcm_init erwartet einen veraenderlichen Zeiger, die CTR-Funktionen lesen den Plan aber nur
     * (br_aes_ct_ctr_run nimmt const) - gleichzeitige Nutzung desselben Schluessels ist daher sicher. */
    e2e_gcmkey *k = (e2e_gcmkey *)key;
    br_gcm_context g;
    int ok = 1;
    if (!k || n < 0 || alen < 0 || (n > 0 && (!in || !out))) return -1;
    br_gcm_init(&g, &k->ctr.vtable, &br_ghash_ctmul);
    br_gcm_reset(&g, iv, 12);
    if (alen > 0) br_gcm_aad_inject(&g, aad, (size_t)alen);
    br_gcm_flip(&g);
    if (n > 0) {
        if (out != in) memmove(out, in, (size_t)n);
        br_gcm_run(&g, enc ? 1 : 0, out, (size_t)n);
    }
    if (enc) {
        br_gcm_get_tag(&g, tag);
    } else {
        ok = br_gcm_check_tag(&g, tag) == 1;
        if (!ok && n > 0) SecureZeroMemory(out, (size_t)n); /* nie unauthentischen Klartext zurueckgeben */
    }
    SecureZeroMemory(&g, sizeof g);
    return ok ? 0 : -1;
}

int sbe2e_gcm(int enc, const uint8_t key[32], const uint8_t iv[12], const uint8_t *aad, int alen, const uint8_t *in,
              int n, uint8_t *out, uint8_t tag[16])
{
    void *k = gcm_key(key);
    int r;
    if (!k) return -1;
    r = gcm_run(k, enc, iv, aad, alen, in, n, out, tag);
    gcm_key_free(k);
    return r;
}

/* ====================================================================== X25519 (BearSSL c25519_m31)
 * RFC 7748: Privatschluessel und Punkte in Little-Endian-Form; BearSSL klemmt den Skalar selbst (k[0] &= 248,
 * k[31] &= 127, |= 64) und maskiert Bit 255 des Punkts. Wir klemmen zusaetzlich beim Speichern (wie bisher). */
static void x_clamp(uint8_t k[32])
{
    k[0] &= 248;
    k[31] &= 127;
    k[31] |= 64;
}

static int x_mul(const uint8_t priv[32], const uint8_t point[32], uint8_t out[32])
{
    uint8_t G[32];
    uint32_t ok;
    memcpy(G, point, 32);
    ok = br_ec_c25519_m31.mul(G, 32, priv, 32, BR_EC_curve25519);
    if (!ok) {
        SecureZeroMemory(G, sizeof G);
        return -1;
    }
    memcpy(out, G, 32);
    SecureZeroMemory(G, sizeof G);
    return 0;
}

static int x_agree(const e2e_xkey *mine, const uint8_t peer_pub[32], uint8_t z[32])
{
    uint8_t acc = 0;
    int i, r;
    if (!mine) return -1;
    r = x_mul(mine->priv, peer_pub, z);
    if (r == 0) {
        for (i = 0; i < 32; i++) acc |= z[i];
        r = acc ? 0 : -1; /* Z == 0^32 -> Punkt kleiner Ordnung (SECURITY.md §4.3) */
    }
    if (r != 0) SecureZeroMemory(z, 32);
    return r;
}

static int x_pub(const e2e_xkey *k, uint8_t pub[32])
{
    size_t glen = 0;
    const unsigned char *g = br_ec_c25519_m31.generator(BR_EC_curve25519, &glen);
    if (!g || glen != 32) return -1;
    return x_mul(k->priv, g, pub);
}

int sbe2e_kp_import(sbe2e_kp *kp, const uint8_t priv[32], uint8_t pub[32])
{
    e2e_xkey *k;
    kp->key = NULL;
    if (!alg_ready()) return -1;
    k = (e2e_xkey *)secure_alloc(sizeof *k);
    if (!k) return -1;
    memcpy(k->priv, priv, 32);
    x_clamp(k->priv);
    if (pub && x_pub(k, pub) != 0) {
        secure_free(k, sizeof *k);
        return -1;
    }
    kp->key = k;
    return 0;
}

int sbe2e_keypair(sbe2e_kp *kp, uint8_t pub[32])
{
    uint8_t priv[32];
    int r;
    kp->key = NULL;
    if (sbe2e_random(priv, 32) != 0) return -1;
    r = sbe2e_kp_import(kp, priv, pub);
    SecureZeroMemory(priv, sizeof priv);
    return r;
}

void sbe2e_kp_free(sbe2e_kp *kp)
{
    if (kp && kp->key) {
        secure_free(kp->key, sizeof(e2e_xkey));
        kp->key = NULL;
    }
}

int sbe2e_x25519(const uint8_t priv[32], const uint8_t pub[32], uint8_t out[32])
{
    sbe2e_kp kp;
    int r;
    if (sbe2e_kp_import(&kp, priv, NULL) != 0) return -1;
    r = x_agree((const e2e_xkey *)kp.key, pub, out);
    sbe2e_kp_free(&kp);
    return r;
}

/* ====================================================================== Schluessel je Paar */
static const char TH_LABEL[] = "steelnet-e2e-v2"; /* 15 Byte ohne Null */

static int side_ok(const sbe2e_side *s)
{
    size_t l = strnlen(s->pilot, sizeof s->pilot);
    size_t i;
    if (l < 1 || l > 16) return 0;
    for (i = 0; i < l; i++) {
        unsigned char c = (unsigned char)s->pilot[i];
        if (c < 0x20 || c > 0x7E) return 0;
    }
    return 1;
}

int sbe2e_transcript(uint8_t th[32], uint32_t lobby, const uint8_t pn[16], const sbe2e_side *a, const sbe2e_side *b)
{
    const sbe2e_side *lo, *hi, *s;
    sb_sha256_t c;
    uint8_t x[9];
    int k;
    if (a->cid == b->cid || !side_ok(a) || !side_ok(b)) return -1;
    lo = a->cid < b->cid ? a : b;
    hi = lo == a ? b : a;
    sb_sha256_init(&c);
    sb_sha256_update(&c, TH_LABEL, 15);
    x[0] = SBE2E_SUITE;
    be32(x + 1, lobby);
    sb_sha256_update(&c, x, 5);
    sb_sha256_update(&c, pn, 16);
    for (k = 0; k < 2; k++) {
        uint8_t pl;
        s = k ? hi : lo;
        pl = (uint8_t)strnlen(s->pilot, sizeof s->pilot);
        be32(x, s->cid);
        be32(x + 4, s->account);
        x[8] = pl;
        sb_sha256_update(&c, x, 9);
        sb_sha256_update(&c, s->pilot, pl);
        sb_sha256_update(&c, s->pub, 32);
    }
    sb_sha256_final(&c, th);
    return 0;
}

int sbe2e_link_from_prk(const uint8_t prk[32], int i_am_lo, sbe2e_link *L)
{
    uint8_t klh[32], khl[32], sid[4];
    memset(L, 0, sizeof *L);
    if (!alg_ready()) return -1;
    if (sbe2e_hkdf_expand(prk, "sbt2 k lo>hi", 12, klh, 32) != 0 || sbe2e_hkdf_expand(prk, "sbt2 k hi>lo", 12, khl, 32) != 0 ||
        sbe2e_hkdf_expand(prk, "sbt2 sid", 8, sid, 4) != 0) {
        SecureZeroMemory(klh, sizeof klh);
        SecureZeroMemory(khl, sizeof khl);
        return -1;
    }
    L->sid = rd32(sid);
    L->tx_dir = i_am_lo ? 1 : 2;
    L->rx_dir = i_am_lo ? 2 : 1;
    L->ktx = gcm_key(i_am_lo ? klh : khl);
    L->krx = gcm_key(i_am_lo ? khl : klh);
    SecureZeroMemory(klh, sizeof klh);
    SecureZeroMemory(khl, sizeof khl);
    if (!L->ktx || !L->krx) {
        sbe2e_link_free(L);
        return -1;
    }
    L->valid = 1;
    return 0;
}

int sbe2e_derive(const sbe2e_kp *kp, const uint8_t peer_pub[32], const uint8_t th[32], uint32_t my_cid,
                 uint32_t peer_cid, sbe2e_link *L)
{
    uint8_t z[32], prk[32];
    int r;
    memset(L, 0, sizeof *L);
    if (!kp || !kp->key || my_cid == peer_cid || !alg_ready()) return -1;
    if (x_agree((const e2e_xkey *)kp->key, peer_pub, z) != 0) return -1;
    sbe2e_hkdf_extract(th, 32, z, 32, prk);
    r = sbe2e_link_from_prk(prk, my_cid < peer_cid, L);
    SecureZeroMemory(z, sizeof z);
    SecureZeroMemory(prk, sizeof prk);
    return r;
}

int sbe2e_link_test(int i_am_lo, sbe2e_link *L)
{
    static const char salt[] = "steelnet-e2e-v2 test", ikm[] = "loc_transport_peers";
    uint8_t prk[32];
    int r;
    sbe2e_hkdf_extract((const uint8_t *)salt, (int)sizeof salt - 1, (const uint8_t *)ikm, (int)sizeof ikm - 1, prk);
    r = sbe2e_link_from_prk(prk, i_am_lo, L);
    SecureZeroMemory(prk, sizeof prk);
    return r;
}

void sbe2e_link_free(sbe2e_link *L)
{
    if (!L) return;
    gcm_key_free(L->ktx);
    gcm_key_free(L->krx);
    SecureZeroMemory(L, sizeof *L);
}

/* ====================================================================== Fenster (SECURITY.md §4.5) */
int sbe2e_win_check(const sbe2e_link *L, uint64_t ctr)
{
    uint64_t d;
    if (ctr == 0 || ctr > SBE2E_CTR_MAX) return 0;
    if (ctr > L->rx_hi) return 1;
    d = L->rx_hi - ctr;
    if (d >= SBE2E_WIN) return 0;
    return !((L->rx_bits[d >> 5] >> (d & 31)) & 1u);
}

void sbe2e_win_commit(sbe2e_link *L, uint64_t ctr)
{
    if (ctr > L->rx_hi) {
        uint64_t sh = ctr - L->rx_hi;
        if (sh >= SBE2E_WIN) {
            memset(L->rx_bits, 0, sizeof L->rx_bits);
        } else {
            int ws = (int)(sh >> 5), bs = (int)(sh & 31), w;
            for (w = SBE2E_WIN / 32 - 1; w >= 0; w--) {
                uint32_t v = 0;
                if (w - ws >= 0) {
                    v = L->rx_bits[w - ws] << bs;
                    if (bs && w - ws - 1 >= 0) v |= L->rx_bits[w - ws - 1] >> (32 - bs);
                }
                L->rx_bits[w] = v;
            }
        }
        L->rx_bits[0] |= 1u;
        L->rx_hi = ctr;
    } else {
        uint64_t d = L->rx_hi - ctr;
        if (d < SBE2E_WIN) L->rx_bits[d >> 5] |= 1u << (d & 31);
    }
}

/* ====================================================================== Pakete */
static int inner_ok(uint8_t t)
{
    return t == SBE2E_T_HELLO || (t >= SBE2E_T_DATA && t <= SBE2E_T_PROBE_ACK);
}

static void nonce_for(uint8_t n[12], uint8_t dir, uint64_t ctr)
{
    n[0] = dir;
    n[1] = n[2] = n[3] = 0;
    be64(n + 4, ctr);
}

int sbe2e_seal(sbe2e_link *L, const uint8_t *pt, int n, uint8_t *out, int cap)
{
    uint8_t nonce[12];
    uint64_t ctr;
    if (!L || !L->valid || n < 1 || n > SBE2E_MAX_PT || cap < n + SBE2E_OVERHEAD || !inner_ok(pt[0])) return -1;
    if (L->tx_ctr >= SBE2E_CTR_MAX) return -1; /* harte Grenze: kein Schluesselwechsel im Betrieb */
    ctr = ++L->tx_ctr;
    memcpy(out, "SBT2", 4);
    be32(out + 4, L->sid);
    be64(out + 8, ctr);
    nonce_for(nonce, L->tx_dir, ctr);
    if (gcm_run(L->ktx, 1, nonce, out, SBE2E_HDR, pt, n, out + SBE2E_HDR, out + SBE2E_HDR + n) != 0) return -1;
    return n + SBE2E_OVERHEAD;
}

int sbe2e_exhausted(const sbe2e_link *L)
{
    return L && L->tx_ctr >= SBE2E_CTR_MAX;
}

int sbe2e_peek_sid(const uint8_t *pkt, int n, uint32_t *sid)
{
    uint64_t ctr;
    if (!pkt || n < SBE2E_OVERHEAD + 1 || n > SBE2E_MAX_PACKET || memcmp(pkt, "SBT2", 4) != 0) return 0;
    ctr = rd64(pkt + 8);
    if (ctr == 0 || ctr > SBE2E_CTR_MAX) return 0;
    if (sid) *sid = rd32(pkt + 4);
    return 1;
}

int sbe2e_open(sbe2e_link *L, const uint8_t *pkt, int n, uint8_t *pt, int cap)
{
    uint8_t nonce[12], tag[16];
    uint32_t sid;
    uint64_t ctr;
    int clen;
    if (!L || !L->valid) return SBE2E_E_STATE;
    /* 1. Form: Laenge, Magic, sid, ctr-Bereich */
    if (!sbe2e_peek_sid(pkt, n, &sid) || sid != L->sid) return SBE2E_E_FORM;
    clen = n - SBE2E_OVERHEAD;
    if (cap < clen) return SBE2E_E_FORM;
    ctr = rd64(pkt + 8);
    /* 3. Fenster pruefen, ohne es zu aendern */
    if (!sbe2e_win_check(L, ctr)) return SBE2E_E_REPLAY;
    /* 4. GCM oeffnen (Empfangsschluessel, Empfangsrichtung, AAD = Kopf) */
    nonce_for(nonce, L->rx_dir, ctr);
    memcpy(tag, pkt + n - SBE2E_TAG, SBE2E_TAG);
    if (gcm_run(L->krx, 0, nonce, pkt, SBE2E_HDR, pkt + SBE2E_HDR, clen, pt, tag) != 0) return SBE2E_E_TAG;
    /* 5. erst jetzt Fenster setzen; unbekannter innerer Typ -> verwerfen */
    sbe2e_win_commit(L, ctr);
    if (!inner_ok(pt[0])) {
        SecureZeroMemory(pt, (size_t)clen);
        return SBE2E_E_TYPE;
    }
    return clen;
}

/* ====================================================================== Start-Selbsttest (§4.8) */
/* Werte aus net/tools/sbt2_vectors.json (RFC 7748 §6.1, RFC 5869 TC1, McGrew/Viega TC16, SBT2 Paket 1). */
static const uint8_t KAT_X_APRIV[32] = {0x77,0x07,0x6d,0x0a,0x73,0x18,0xa5,0x7d,0x3c,0x16,0xc1,0x72,0x51,0xb2,0x66,0x45,0xdf,0x4c,0x2f,0x87,0xeb,0xc0,0x99,0x2a,0xb1,0x77,0xfb,0xa5,0x1d,0xb9,0x2c,0x2a};
static const uint8_t KAT_X_APUB[32] = {0x85,0x20,0xf0,0x09,0x89,0x30,0xa7,0x54,0x74,0x8b,0x7d,0xdc,0xb4,0x3e,0xf7,0x5a,0x0d,0xbf,0x3a,0x0d,0x26,0x38,0x1a,0xf4,0xeb,0xa4,0xa9,0x8e,0xaa,0x9b,0x4e,0x6a};
static const uint8_t KAT_X_BPUB[32] = {0xde,0x9e,0xdb,0x7d,0x7b,0x7d,0xc1,0xb4,0xd3,0x5b,0x61,0xc2,0xec,0xe4,0x35,0x37,0x3f,0x83,0x43,0xc8,0x5b,0x78,0x67,0x4d,0xad,0xfc,0x7e,0x14,0x6f,0x88,0x2b,0x4f};
static const uint8_t KAT_X_SHARED[32] = {0x4a,0x5d,0x9d,0x5b,0xa4,0xce,0x2d,0xe1,0x72,0x8e,0x3b,0xf4,0x80,0x35,0x0f,0x25,0xe0,0x7e,0x21,0xc9,0x47,0xd1,0x9e,0x33,0x76,0xf0,0x9b,0x3c,0x1e,0x16,0x17,0x42};
static const uint8_t KAT_H_IKM[22] = {0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b};
static const uint8_t KAT_H_SALT[13] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c};
static const uint8_t KAT_H_INFO[10] = {0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9};
static const uint8_t KAT_H_PRK[32] = {0x07,0x77,0x09,0x36,0x2c,0x2e,0x32,0xdf,0x0d,0xdc,0x3f,0x0d,0xc4,0x7b,0xba,0x63,0x90,0xb6,0xc7,0x3b,0xb5,0x0f,0x9c,0x31,0x22,0xec,0x84,0x4a,0xd7,0xc2,0xb3,0xe5};
static const uint8_t KAT_H_OKM[42] = {0x3c,0xb2,0x5f,0x25,0xfa,0xac,0xd5,0x7a,0x90,0x43,0x4f,0x64,0xd0,0x36,0x2f,0x2a,0x2d,0x2d,0x0a,0x90,0xcf,0x1a,0x5a,0x4c,0x5d,0xb0,0x2d,0x56,0xec,0xc4,0xc5,0xbf,0x34,0x00,0x72,0x08,0xd5,0xb8,0x87,0x18,0x58,0x65};
static const uint8_t KAT_G_KEY[32] = {0xfe,0xff,0xe9,0x92,0x86,0x65,0x73,0x1c,0x6d,0x6a,0x8f,0x94,0x67,0x30,0x83,0x08,0xfe,0xff,0xe9,0x92,0x86,0x65,0x73,0x1c,0x6d,0x6a,0x8f,0x94,0x67,0x30,0x83,0x08};
static const uint8_t KAT_G_IV[12] = {0xca,0xfe,0xba,0xbe,0xfa,0xce,0xdb,0xad,0xde,0xca,0xf8,0x88};
static const uint8_t KAT_G_PT[60] = {0xd9,0x31,0x32,0x25,0xf8,0x84,0x06,0xe5,0xa5,0x59,0x09,0xc5,0xaf,0xf5,0x26,0x9a,0x86,0xa7,0xa9,0x53,0x15,0x34,0xf7,0xda,0x2e,0x4c,0x30,0x3d,0x8a,0x31,0x8a,0x72,0x1c,0x3c,0x0c,0x95,0x95,0x68,0x09,0x53,0x2f,0xcf,0x0e,0x24,0x49,0xa6,0xb5,0x25,0xb1,0x6a,0xed,0xf5,0xaa,0x0d,0xe6,0x57,0xba,0x63,0x7b,0x39};
static const uint8_t KAT_G_AAD[20] = {0xfe,0xed,0xfa,0xce,0xde,0xad,0xbe,0xef,0xfe,0xed,0xfa,0xce,0xde,0xad,0xbe,0xef,0xab,0xad,0xda,0xd2};
static const uint8_t KAT_G_CT[60] = {0x52,0x2d,0xc1,0xf0,0x99,0x56,0x7d,0x07,0xf4,0x7f,0x37,0xa3,0x2a,0x84,0x42,0x7d,0x64,0x3a,0x8c,0xdc,0xbf,0xe5,0xc0,0xc9,0x75,0x98,0xa2,0xbd,0x25,0x55,0xd1,0xaa,0x8c,0xb0,0x8e,0x48,0x59,0x0d,0xbb,0x3d,0xa7,0xb0,0x8b,0x10,0x56,0x82,0x88,0x38,0xc5,0xf6,0x1e,0x63,0x93,0xba,0x7a,0x0a,0xbc,0xc9,0xf6,0x62};
static const uint8_t KAT_G_TAG[16] = {0x76,0xfc,0x6e,0xce,0x0f,0x4e,0x17,0x68,0xcd,0xdf,0x88,0x53,0xbb,0x2d,0x55,0x1b};
static const uint8_t KAT_S_BPUB[32] = {0x49,0x3e,0x82,0xfc,0x74,0x46,0x4a,0x59,0x26,0x88,0x17,0x62,0x3d,0x20,0x53,0xc5,0xeb,0x8e,0x2c,0xc4,0xa9,0x88,0xb4,0xfe,0xe1,0x79,0xec,0x6b,0x01,0x0d,0x53,0x1d};
static const uint8_t KAT_S_APUB[32] = {0xd8,0x9e,0x3b,0xad,0x79,0x43,0x7d,0xbe,0xd9,0xf8,0x43,0x41,0x83,0x04,0xf4,0x60,0xff,0x05,0xc7,0xfe,0x81,0xfe,0x4a,0x95,0x77,0xa8,0x04,0xcb,0x93,0x67,0xff,0x66};
static const uint8_t KAT_S_PT[9] = {0x01,0x7f,0xa8,0x40,0x01,0x00,0x02,0x00,0x00};
static const uint8_t KAT_S_PKT[41] = {0x53,0x42,0x54,0x32,0xc6,0x5f,0xc5,0xe8,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x8e,0xb8,0xe7,0x39,0x73,0xa9,0x91,0x91,0x22,0xa1,0xd1,0xe9,0xeb,0x74,0x90,0x32,0xed,0xf3,0x12,0x9b,0xfc,0x43,0xd8,0x8b,0x9f};

static int st_fail(char *why, int cap, const char *w)
{
    if (why && cap > 0) _snprintf_s(why, (size_t)cap, _TRUNCATE, "%s", w);
    return -1;
}

int sbe2e_selftest_ex(char *why, int cap, int tamper)
{
    uint8_t out[64], tag[16], pub[32], prk[32], th[32];
    uint8_t shared_exp[32], KAT_S_APRIV[32], KAT_S_PN[16];
    sbe2e_kp kp;
    sbe2e_side a, b;
    sbe2e_link L, R;
    int n, ok;
    if (why && cap > 0) why[0] = 0;
    /* Zaehlfolgen der SBT2-Vektoren (Privatschluessel a = 10..2f, pair_nonce = 00..0f) zur Laufzeit */
    for (n = 0; n < 32; n++) KAT_S_APRIV[n] = (uint8_t)(0x10 + n);
    for (n = 0; n < 16; n++) KAT_S_PN[n] = (uint8_t)n;
    if (!alg_ready()) return st_fail(why, cap, "Krypto-Bausteine nicht verfuegbar");
    /* X25519 RFC 7748 §6.1: oeffentlicher Schluessel und gemeinsames Geheimnis */
    memcpy(shared_exp, KAT_X_SHARED, 32);
    if (tamper == 1) shared_exp[5] ^= 1;
    if (sbe2e_kp_import(&kp, KAT_X_APRIV, pub) != 0) return st_fail(why, cap, "X25519");
    ok = sbe2e_ct_eq(pub, KAT_X_APUB, 32) && x_agree((BCRYPT_KEY_HANDLE)kp.key, KAT_X_BPUB, out) == 0 &&
         sbe2e_ct_eq(out, shared_exp, 32);
    sbe2e_kp_free(&kp);
    if (!ok) return st_fail(why, cap, "X25519");
    /* HKDF RFC 5869 TC1 */
    sbe2e_hkdf_extract(KAT_H_SALT, sizeof KAT_H_SALT, KAT_H_IKM, sizeof KAT_H_IKM, prk);
    if (tamper == 2) prk[0] ^= 1;
    if (!sbe2e_ct_eq(prk, KAT_H_PRK, 32) || sbe2e_hkdf_expand(prk, KAT_H_INFO, sizeof KAT_H_INFO, out, 42) != 0 ||
        !sbe2e_ct_eq(out, KAT_H_OKM, 42))
        return st_fail(why, cap, "HKDF");
    /* AES-256-GCM TC16 (versiegeln und oeffnen, falscher Tag wird abgelehnt) */
    if (sbe2e_gcm(1, KAT_G_KEY, KAT_G_IV, KAT_G_AAD, sizeof KAT_G_AAD, KAT_G_PT, sizeof KAT_G_PT, out, tag) != 0 ||
        !sbe2e_ct_eq(out, KAT_G_CT, sizeof KAT_G_CT) || !sbe2e_ct_eq(tag, KAT_G_TAG, 16) || tamper == 3)
        return st_fail(why, cap, "AES-256-GCM");
    memcpy(tag, KAT_G_TAG, 16);
    if (sbe2e_gcm(0, KAT_G_KEY, KAT_G_IV, KAT_G_AAD, sizeof KAT_G_AAD, KAT_G_CT, sizeof KAT_G_CT, out, tag) != 0 ||
        !sbe2e_ct_eq(out, KAT_G_PT, sizeof KAT_G_PT))
        return st_fail(why, cap, "AES-256-GCM");
    tag[3] ^= 0x10;
    if (sbe2e_gcm(0, KAT_G_KEY, KAT_G_IV, KAT_G_AAD, sizeof KAT_G_AAD, KAT_G_CT, sizeof KAT_G_CT, out, tag) == 0)
        return st_fail(why, cap, "AES-256-GCM (falscher Tag angenommen)");
    /* SBT2: Ableitung Seite a (cid 9 = hi) mit b (cid 3 = lo), erstes Paket "lo>hi HELLO ctr 1" */
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.cid = 9;
    a.account = 1001;
    strcpy_s(a.pilot, sizeof a.pilot, "Alice");
    memcpy(a.pub, KAT_S_APUB, 32);
    b.cid = 3;
    b.account = 0;
    strcpy_s(b.pilot, sizeof b.pilot, "Guest-Bob");
    memcpy(b.pub, KAT_S_BPUB, 32);
    if (sbe2e_transcript(th, 7, KAT_S_PN, &a, &b) != 0) return st_fail(why, cap, "SBT2 (Transkript)");
    if (sbe2e_kp_import(&kp, KAT_S_APRIV, pub) != 0 || !sbe2e_ct_eq(pub, KAT_S_APUB, 32)) {
        sbe2e_kp_free(&kp);
        return st_fail(why, cap, "SBT2 (Schluessel)");
    }
    n = sbe2e_derive(&kp, KAT_S_BPUB, th, 9, 3, &R); /* a = hi: empfaengt lo->hi */
    sbe2e_kp_free(&kp);
    if (n != 0) return st_fail(why, cap, "SBT2 (Ableitung)");
    {
        /* Gegenseite lo mit denselben Schluesseln: PRK nicht verfuegbar -> Paket von R aus pruefen */
        uint8_t pt[64];
        uint8_t pkt[sizeof KAT_S_PKT];
        memcpy(pkt, KAT_S_PKT, sizeof pkt);
        if (tamper == 4) pkt[sizeof pkt - 1] ^= 1;
        n = sbe2e_open(&R, pkt, (int)sizeof pkt, pt, sizeof pt);
        ok = n == (int)sizeof KAT_S_PT && sbe2e_ct_eq(pt, KAT_S_PT, n);
        /* Wiederholung desselben Pakets wird verworfen */
        ok = ok && sbe2e_open(&R, pkt, (int)sizeof pkt, pt, sizeof pt) == SBE2E_E_REPLAY;
        SecureZeroMemory(pt, sizeof pt);
    }
    /* Rundweg: R (hi) versiegelt, eine lo-Seite aus demselben Geheimnis oeffnet */
    if (ok) {
        uint8_t z[32], p2[64], pt[64], q[] = {SBE2E_T_PING, 1, 2, 3, 4};
        ok = sbe2e_x25519(KAT_S_APRIV, KAT_S_BPUB, z) == 0;
        sbe2e_hkdf_extract(th, 32, z, 32, prk);
        ok = ok && sbe2e_link_from_prk(prk, 1, &L) == 0;
        n = ok ? sbe2e_seal(&R, q, sizeof q, p2, sizeof p2) : -1;
        ok = ok && n == (int)sizeof q + SBE2E_OVERHEAD && sbe2e_open(&L, p2, n, pt, sizeof pt) == (int)sizeof q &&
             sbe2e_ct_eq(pt, q, sizeof q);
        /* Reflexion an den Absender ist nie gueltig (dir im Nonce + getrennte Schluessel) */
        ok = ok && sbe2e_open(&R, p2, n, pt, sizeof pt) < 0;
        SecureZeroMemory(z, sizeof z);
        SecureZeroMemory(pt, sizeof pt);
        sbe2e_link_free(&L);
    }
    SecureZeroMemory(prk, sizeof prk);
    sbe2e_link_free(&R);
    if (!ok) return st_fail(why, cap, "SBT2 (Paket)");
    return 0;
}

int sbe2e_selftest(char *why, int cap)
{
    return sbe2e_selftest_ex(why, cap, 0);
}
