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

/* W493 LOC-MEMLEAK: Aufrufer-Kennung, Rueckgabe-Modus, VB/IB-Koepfe, Statistik */
enum { LOCMEM_T_VB = 0, LOCMEM_T_IB, LOCMEM_T_CONTIG, LOCMEM_T_CONTIGEX, LOCMEM_T_N };
uint32_t loc_mem_alloc_t(uint32_t size, uint32_t align, int zero, int tag, uint32_t site); /* site = Rueckgabeadresse im Spiel (0 = unbekannt) */
/* reclaim 1 = Freigaben ueber Quarantaene (quar_frames Bilder) in Freilisten nach Groessenklassen, oberster freier Block
 * senkt den Fuellstand; 0 = alt (nur gleiche Groesse, sofort). clock = Bildzaehler (NULL = 0). */
void loc_mem_config(int reclaim, uint32_t quar_frames, uint32_t (*clock)(void));
void loc_mem_tick(void);                                 /* je Bild: abgelaufene Quarantaene freigeben */
/* CreateVertex/IndexBuffer2 im Rueckgabe-Modus: Kopf {Common, Data, Lock} wie XDK (Refcount 1), Daten aus LOCMEM. */
uint32_t loc_mem_res2_create(uint32_t len, int ib, uint32_t site);
/* D3DResource_Release fuer einen solchen Kopf: 1 = unserer (*rc_out = neuer Refcount); bei 0 Daten + Kopf in die
 * Quarantaene. spec = spekulativer Vorlauf: nichts veraendern, nur Rueckgabewert. 0 = nicht unserer. */
int loc_mem_res2_release(uint32_t res, int spec, uint32_t *rc_out);
int loc_mem_res2_owns(uint32_t res);
typedef struct {
    unsigned t_nalloc[LOCMEM_T_N], t_nreuse[LOCMEM_T_N], t_nfree[LOCMEM_T_N], t_live_n[LOCMEM_T_N];
    unsigned long long t_live_b[LOCMEM_T_N];
    unsigned q_n, f_n;                 /* Bloecke in Quarantaene / wiederverwendbar */
    unsigned long long q_b, f_b;
    uint32_t used, size;               /* Fuellstand, Bereichsgroesse */
    unsigned nblk, nalloc, nreuse, nfree, nfull, nforce, nretract;
    unsigned h_live, h_rel, h_relint, h_odd, h_spec; /* Koepfe: lebend, freigegeben, interner Refcount, doppelt/0, Vorlauf */
} loc_mem_info_t;
void loc_mem_info(loc_mem_info_t *o);
int loc_mem_init_test(uint32_t base, uint32_t size);     /* nur Selbsttest: Bereich ohne Spiegel, base 0 = zuruecksetzen */

#ifdef __cplusplus
}
#endif
