/* sb_e2e.h - W334 LOC-SEC-CLIENT: Ende-zu-Ende-Schutz des Spielverkehrs (SBT2), Spezifikation
 * poc-native/net/SECURITY.md §4.
 *
 * Bausteine: X25519 und AES-256-GCM aus BearSSL 0.6 (third_party/bearssl, zeitkonstante Varianten; seit W407, vorher
 * Windows CNG), Zufall aus BCryptGenRandom, HMAC/HKDF-SHA256 ueber das vorhandene sb_sha256 (RFC 2104/5869).
 * Keine eigenen Primitive. Schluesselmaterial wird nach Gebrauch geloescht (SecureZeroMemory),
 * nie geloggt. Fehlerwege sind "fail closed" (Paket verwerfen / Verbindung ablehnen).
 *
 * Paket SBT2 (SECURITY.md §4.4):  "SBT2" | sid u32 BE | ctr u64 BE | Chiffretext n | Tag 16, n = 1..1168
 *   Kopf (16 B) = AAD, Nonce = dir u8 | 00 00 00 | ctr u64 BE, dir 1 = lo->hi, 2 = hi->lo. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SBE2E_SUITE 1
#define SBE2E_HDR 16
#define SBE2E_TAG 16
#define SBE2E_OVERHEAD 32
#define SBE2E_MAX_PACKET 1200
#define SBE2E_MAX_PT (SBE2E_MAX_PACKET - SBE2E_OVERHEAD) /* 1168 */
#define SBE2E_CTR_MAX 0x100000000ull                   /* 2^32: danach Verbindung beenden */
#define SBE2E_WIN 1024

/* innere Typen (SECURITY.md §4.4) */
#define SBE2E_T_HELLO 1
#define SBE2E_T_DATA 3
#define SBE2E_T_PING 4
#define SBE2E_T_PONG 5
#define SBE2E_T_BYE 6
#define SBE2E_T_PROBE 7
#define SBE2E_T_PROBE_ACK 8

/* Rueckgaben von sbe2e_open (< 0 = verworfen) */
#define SBE2E_E_FORM (-1)   /* Laenge, Magic, sid, ctr-Bereich */
#define SBE2E_E_REPLAY (-2) /* Duplikat oder zu alt */
#define SBE2E_E_TAG (-3)    /* Authentisierung fehlgeschlagen */
#define SBE2E_E_TYPE (-4)   /* authentisch, aber unbekannter innerer Typ */
#define SBE2E_E_STATE (-5)  /* Verbindung ohne Schluessel */

typedef struct sbe2e_kp {
    void *key; /* fluechtiger X25519-Privatschluessel (intern, wird beim Freigeben geloescht) */
} sbe2e_kp;

typedef struct sbe2e_side {
    uint32_t cid, account; /* client_id, Kontonummer (0 = Gast) */
    char pilot[17];        /* wirksamer Pilotname, ASCII 1..16 */
    uint8_t pub[32];       /* oeffentlicher X25519-Schluessel (RFC-7748-Form) */
} sbe2e_side;

typedef struct sbe2e_link {
    int valid;
    uint32_t sid;
    uint8_t tx_dir, rx_dir;
    void *ktx, *krx;        /* AES-256-GCM-Schluesselplan je Richtung (intern, nur lesend genutzt) */
    uint64_t tx_ctr;        /* zuletzt gesendeter ctr (0 = noch keiner) */
    uint64_t rx_hi;         /* hoechster angenommener ctr */
    uint32_t rx_bits[SBE2E_WIN / 32]; /* Bit i = ctr (rx_hi - i) gesehen */
} sbe2e_link;

/* Start-Selbsttest (SECURITY.md §4.8): X25519 RFC 7748 §6.1, HKDF RFC 5869 TC1, AES-256-GCM TC16, SBT2-Paket
 * aus sbt2_vectors.json gegen einkompilierte Werte. 0 = ok, sonst -1 und why = Baustein. */
int sbe2e_selftest(char *why, int cap);
/* Test: tamper != 0 verfaelscht einen einkompilierten Erwartungswert (Gegenprobe "Selbsttest schlaegt fehl"). */
int sbe2e_selftest_ex(char *why, int cap, int tamper);

/* Zufall aus dem CSPRNG des Systems. 0 = ok, -1 = Fehler (Aufrufer bricht ab). */
int sbe2e_random(void *p, int n);

/* Fluechtiges Schluesselpaar erzeugen; pub = oeffentlicher Schluessel. 0 = ok. */
int sbe2e_keypair(sbe2e_kp *kp, uint8_t pub[32]);
/* TEST/Vektoren: Schluesselpaar aus festem Privatschluessel (RFC-7748-Form, wird geklemmt). */
int sbe2e_kp_import(sbe2e_kp *kp, const uint8_t priv[32], uint8_t pub[32]);
void sbe2e_kp_free(sbe2e_kp *kp);

/* Transkript-Hash (SECURITY.md §4.3). a/b in beliebiger Reihenfolge (lo = kleinere cid).
 * Rueckgabe 0 = ok, -1 = Seiten ungueltig (gleiche cid, Pilot nicht ASCII 1..16). */
int sbe2e_transcript(uint8_t th[32], uint32_t lobby, const uint8_t pn[16], const sbe2e_side *a,
                     const sbe2e_side *b);
/* Schluessel je Paar: Z = X25519(e, E_partner), PRK = HMAC(TH, Z), k_lohi/k_hilo/sid. -1 bei Z == 0,
 * gleicher cid oder Baustein-Fehler (L bleibt ungueltig). */
int sbe2e_derive(const sbe2e_kp *kp, const uint8_t peer_pub[32], const uint8_t th[32], uint32_t my_cid,
                 uint32_t peer_cid, sbe2e_link *L);
/* Schluessel aus einem PRK (Testweg §4.7 und Vektoren); i_am_lo = 1, wenn diese Seite "lo" ist. */
int sbe2e_link_from_prk(const uint8_t prk[32], int i_am_lo, sbe2e_link *L);
/* Testweg ohne Treffpunkt (SECURITY.md §4.7): PRK = HKDF-Extract("steelnet-e2e-v2 test", "loc_transport_peers"). */
int sbe2e_link_test(int i_am_lo, sbe2e_link *L);

/* Versiegeln: pt[0] = innerer Typ, n = 1..1168. Laenge des Pakets oder -1 (u. a. ctr-Grenze erreicht). */
int sbe2e_seal(sbe2e_link *L, const uint8_t *pt, int n, uint8_t *out, int cap);
/* 1 = der zuletzt versiegelte ctr war 2^32 -> Verbindung nach diesem Paket beenden. */
int sbe2e_exhausted(const sbe2e_link *L);
/* Oeffnen in der Reihenfolge SECURITY.md §4.5 Schritte 1, 3, 4, 5. Laenge des Klartexts (>= 1) oder SBE2E_E_*.
 * Das Fenster wird nur nach gueltigem Tag gesetzt. */
int sbe2e_open(sbe2e_link *L, const uint8_t *pkt, int n, uint8_t *pt, int cap);
/* Kopf lesen ohne Kryptografie: 1 = Form ok (Laenge, Magic, ctr-Bereich), sid gesetzt. */
int sbe2e_peek_sid(const uint8_t *pkt, int n, uint32_t *sid);
void sbe2e_link_free(sbe2e_link *L);

/* Fenster (fuer Vektoren und Selbsttest) */
int sbe2e_win_check(const sbe2e_link *L, uint64_t ctr);
void sbe2e_win_commit(sbe2e_link *L, uint64_t ctr);

/* Bausteine (Vektoren) */
int sbe2e_x25519(const uint8_t priv[32], const uint8_t pub[32], uint8_t out[32]);
void sbe2e_hmac(const uint8_t *key, int klen, const void *msg, int mlen, uint8_t out[32]);
void sbe2e_hkdf_extract(const uint8_t *salt, int slen, const uint8_t *ikm, int ilen, uint8_t prk[32]);
int sbe2e_hkdf_expand(const uint8_t prk[32], const void *info, int ilen, uint8_t *out, int len);
/* AES-256-GCM roh: enc=1 versiegeln (ct, tag ausgeben), enc=0 oeffnen (tag pruefen). 0 = ok, -1 = Fehler/Tag falsch. */
int sbe2e_gcm(int enc, const uint8_t key[32], const uint8_t iv[12], const uint8_t *aad, int alen, const uint8_t *in,
              int n, uint8_t *out, uint8_t tag[16]);

/* Laufzeitkonstanter Vergleich: 1 = gleich. */
int sbe2e_ct_eq(const void *a, const void *b, int n);

#ifdef __cplusplus
}
#endif
