/* render_vsh.h - RENDER-QA (W77): NV2A-Vertex-Shader-Interpreter (CPU).
 * Rein (keine Windows-/D3D-Abhaengigkeit): Mikrocode-Slots (je 4 DWORDs,
 * Wort 0 ungenutzt) -> vorab dekodiertes Programm -> Ausfuehrung je Vertex.
 * Semantik nach der bekannten NV2A-Spezifikation (Befehlssatz MAC 0-13 /
 * ILU 0-7, gepaarte ILU schreibt R1, R12 = oPos-Rueckleser, relative
 * Konstanten c[A0.x+n], Ausgaben oPos/oD0/oD1/oFog/oPts/oB0/oB1/oT0..3). */
#ifndef RENDER_VSH_H
#define RENDER_VSH_H

#define RQV_MAX_OPS 136
#define RQV_NCONST 192

/* Ausgaberegister-Index (OUT_ADDRESS bei ORB=1) */
enum { RQV_OPOS = 0, RQV_OD0 = 3, RQV_OD1 = 4, RQV_OFOG = 5, RQV_OPTS = 6,
       RQV_OB0 = 7, RQV_OB1 = 8, RQV_OT0 = 9, RQV_OT1 = 10, RQV_OT2 = 11, RQV_OT3 = 12 };

typedef struct {
    unsigned char mux, reg, neg, sw[4]; /* mux: 1 = R, 2 = V, 3 = C */
} rqv_in_t;

typedef struct {
    unsigned char mac, ilu, vin, a0x;
    rqv_in_t in[3];                 /* A, B, C */
    unsigned short cidx;            /* Konstanten-Index 0..191 (= c-96..95) */
    unsigned char macmask, outr, ilumask, omask, orb, oaddr, omux, final;
} rqv_op_t;

typedef struct {
    int nops;
    unsigned outmask;  /* Bit k = Ausgaberegister k beschrieben */
    unsigned vinmask;  /* Bit k = v-Register k gelesen */
    unsigned cwrite;   /* 1 = schreibt Konstanten (ORB=0) */
    unsigned rel;      /* 1 = relative Konstanten-Adressierung */
    rqv_op_t ops[RQV_MAX_OPS];
} rqv_prog_t;

typedef struct {
    float o[13][4];    /* Ausgaberegister (Index wie RQV_O*) */
} rqv_out_t;

/* Slots: n * 4 DWORDs. Rueckgabe 1 = ok. */
int rqv_decode(const unsigned *slots, unsigned n, rqv_prog_t *p);
/* Ein Vertex: vin[16][4] Eingaenge, c[192][4] Konstanten (ORB=0-Schriebe
 * gehen in c - Aufrufer gibt eine Kopie). */
void rqv_run(const rqv_prog_t *p, const float (*vin)[4], float (*c)[4], rqv_out_t *out);
/* Einzeltests (render_selftest.exe): Anzahl Fehler, Log ueber cb. */
int rqv_selftest(void (*logf)(const char *line));
/* render_vshgen.c: GPU-VS (HLSL vs_4_0) aus dem Programm; lerp = Zwischen-
 * bild-Variante (Stream 1 = pos0). cidx_of[k] = Spiegel-Index der k-ten
 * gepackten Konstante (*ncmap Stueck). Rueckgabe 1 = ok. */
#include <stddef.h>
int rqv_hlsl(const rqv_prog_t *p, int lerp, char *buf, size_t bufn,
             unsigned char cidx_of[RQV_NCONST], unsigned *ncmap);

/* SHADE-PERF (W80): Roh-Eingaenge. Statt CPU-dekodierter, expandierter
 * v-Register (Stream 2) liest der VS die Xbox-Vertexdaten roh aus einem
 * Buffer<uint> (t0) und dekodiert die Decl-Formate selbst; der Quell-Vertex
 * folgt aus SV_VertexID + Primitivtyp (gleiche Expansion wie der CPU-Pfad)
 * und optional einer Indexliste. Konstanten nach rq_misc: rq_raw (x Basis
 * in Bytes, y Stride, z Primitivtyp, w n), rq_raw2 (x Index-Basis in DWORDs
 * oder 0xFFFFFFFF), rq_inl[ninl] (Inline-Werte nicht belegter Register). */
typedef struct {
    unsigned char fmt[16];   /* X_D3DVSDT-Format, 0 = Inline-Register */
    unsigned short off[16];  /* Byte-Versatz im Vertex */
    unsigned char inl[16];   /* Index in rq_inl (nur fmt 0) */
    unsigned ninl;
    unsigned need;           /* benoetigte Bytes je Vertex (max Versatz+Groesse) */
} rqv_raw_t;
/* Bytes eines Formats (0 = nicht unterstuetzt/NONE -> Inline-Register). */
unsigned rqv_fmt_size(unsigned fmt);
/* Ein Register dekodieren (NV2A-Semantik: fehlende Komponenten 0, w = 1). */
void rqv_vfetch(unsigned fmt, const unsigned char *p, const float cur[4], float o[4]);
/* Decl (je Register Stream/Versatz/Format) -> Roh-Beschreibung fuer die im
 * Programm gelesenen Register (Stride kommt je Draw ueber rq_raw.y; der
 * Aufrufer prueft need <= Stride). 0 = nicht moeglich. */
int rqv_raw_build(const unsigned stream[16], const unsigned off[16], const unsigned fmt[16],
                  unsigned vinmask, rqv_raw_t *r);
/* wie rqv_hlsl, raw != NULL -> Roh-Variante (keine ATTR-Eingaenge). */
int rqv_hlsl_ex(const rqv_prog_t *p, int lerp, const rqv_raw_t *raw, char *buf, size_t bufn,
                unsigned char cidx_of[RQV_NCONST], unsigned *ncmap);

#endif /* RENDER_VSH_H */
