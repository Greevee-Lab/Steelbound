#pragma once
/* xbe_loader.h - XBE parsen + an Original-VAs mappen (32-bit-Prozess). */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t vaddr, vsize, rawaddr, rawsize, flags;
    char name[16];
} section_info_t;

typedef struct {
    uint8_t *file;
    uint32_t file_size;
    uint32_t base;
    uint32_t size_headers, size_image;
    uint32_t entry;      /* dekodiert */
    uint32_t kt_addr;    /* dekodiert */
    uint32_t tls_index_addr;
    uint32_t tls_zerofill;
    uint32_t stack_commit, heap_reserve, heap_commit;
    uint32_t n_sections;
    section_info_t *sections;
    uint32_t image_end;  /* hoechste VA, auf 64K aufgerundet */
    uint8_t *loader_hdr; /* gerettete Loader-Header (fuer Thread-Erstellung) */
    uint32_t loader_hdr_size;
} xbe_image_t;

/* Datei laden + parsen (noch nicht mappen). */
int xbe_parse(const char *path, xbe_image_t *img);
/* W150: wie xbe_parse, aber aus einem malloc-Puffer (gehoert danach img). */
int xbe_parse_buf(uint8_t *buf, uint32_t len, const char *path, xbe_image_t *img);
/* Region reservieren + Sections mappen (M1: alles RWX). */
int xbe_map(xbe_image_t *img);
/* Dateipuffer freigeben (Mapping bleibt). */
void xbe_free_file(xbe_image_t *img);
uint32_t xbe_entry(const xbe_image_t *img);
uint32_t xbe_kernel_thunk(const xbe_image_t *img);
/* Header umschalten: 1 = Loader-PE (fuer CreateThread), 0 = XBE (Spiel).
 * Spiel-Header werden per Shadow erhalten (Spiel schreibt sie!). */
void xbe_swap_headers(xbe_image_t *img, int to_loader);
/* FIX(HDRSWAP,W94): Hybrid-Header. XBE-Header bleibt an der Basis, nur
 * "MZ" (Byte 0-1) und e_lfanew (0x3C, im ungenutzten XBE-Signaturfeld)
 * werden gesetzt; e_lfanew zeigt auf eine Kopie der Loader-PE-Header
 * (ohne NO_SEH) hinter dem XBE-Header in der Header-Seite. Danach sind
 * PE- und XBE-Sicht gleichzeitig gueltig -> kein Tausch je CreateThread.
 * 0 = aktiv, <0 = nicht moeglich (alter Tausch bleibt). */
int xbe_hdr_hybrid(xbe_image_t *img);
/* 1 = Hybrid-Header aktiv (krnl_CreateThread tauscht nicht). */
int xbe_hdr_hybrid_active(void);

#ifdef __cplusplus
}
#endif
