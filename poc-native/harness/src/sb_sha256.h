#pragma once
/* sb_sha256.h - kleine SHA-256-Implementierung (W148, PACK-FORMAT V9/V2).
 *
 * Gemeinsamer Baustein fuer Client (sb_pack.c Pruefung) und Launcher
 * (Packs-Seite, XBE-Hash). Nur C89-Ausschnitt + stdint, keine Abhaengigkeit.
 * Streaming-API: init/update/final (final schreibt 32 Bytes Digest).
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    unsigned nbuf;
} sb_sha256_t;

void sb_sha256_init(sb_sha256_t *c);
void sb_sha256_update(sb_sha256_t *c, const void *data, size_t n);
void sb_sha256_final(sb_sha256_t *c, uint8_t out[32]);
/* Einmal-Aufruf: Digest von n Bytes. */
void sb_sha256(const void *data, size_t n, uint8_t out[32]);
/* Datei-Hash (binaer, gestreamt): 1 = ok, 0 = nicht lesbar. */
int sb_sha256_file(const char *path, uint8_t out[32]);
/* 32 Bytes Digest -> 64 Kleinbuchstaben-Hex (+ NUL, hex braucht 65 Bytes). */
void sb_sha256_hex(const uint8_t in[32], char hex[65]);

#ifdef __cplusplus
}
#endif
