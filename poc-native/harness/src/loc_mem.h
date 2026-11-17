#pragma once
/* loc_mem.h - W277 LOC-DISC: Contig-Speicher mit Xbox-Spiegeln (0x8/0xF0000000 | phys)
 * fuer Line of Contact (siehe loc_mem.c). Nur im LoC-Profil aktiv. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void loc_mem_prereserve(void);                          /* vor D3D: Bereich + Spiegel reservieren */
void loc_mem_drop_prereserve(void);                     /* kein LoC: Reservierung freigeben */
int loc_mem_init(void);                                  /* 1 = Bereich + Spiegel eingeblendet */
int loc_mem_active(void);
uint32_t loc_mem_alloc(uint32_t size, uint32_t align, int zero); /* 0 = voll */
int loc_mem_free(uint32_t va);                           /* 1 = gehoerte uns */
uint32_t loc_mem_size(uint32_t va);                      /* 0 = nicht unserer */
int loc_mem_owns(uint32_t va);
int loc_mem_range(uint32_t *lo, uint32_t *size, uint32_t *used); /* W330: untere Ansicht, Groesse, Fuellstand */
void loc_mem_stats(void);

#ifdef __cplusplus
}
#endif
