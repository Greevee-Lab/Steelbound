/* render_psgen.h - RENDER-QA (W77): NV2A-Register-Combiner -> HLSL-PS (rein). */
#ifndef RENDER_PSGEN_H
#define RENDER_PSGEN_H
#include <stddef.h>

typedef struct {
    unsigned nstages;
    unsigned texmode[4];
    unsigned sample_mask;   /* Stufen, die eine Textur sampeln */
    int unsupported;        /* 1 + Stufe mit nicht unterstuetztem Texturmodus */
    int final_default;      /* ABCD = EFG = 0 -> Ausgabe R0 */
    int uses_fog;
    unsigned cmul[8];       /* Bit 0/1: C0/C1 der Stufe multiplikativ benutzt (Partner != 1) */
} rqp_info_t;

/* d = 60 DWORDs D3DPIXELSHADERDEF. PS-Konstantenpuffer b4:
 * pc0[8], pc1[8], fc0, fc1, fogc, tsc[4] (xy = Koordinatenskala, z = Textur
 * vorhanden), pflags. Rueckgabe 1 = ok, 0 = nicht unterstuetzt/zu lang. */
int rqp_hlsl(const unsigned *d, unsigned stage_tex, char *buf, size_t bufn, rqp_info_t *info);

#endif /* RENDER_PSGEN_H */
