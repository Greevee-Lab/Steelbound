/* render_vsh.c - RENDER-QA (W77): NV2A-Vertex-Shader-Interpreter (CPU).
 * Bitlayout je Slot (Wort 1..3, wie vsh_decode_slot in render_shader.c):
 *  W1: ILU 25-27, MAC 21-24, CONST 13-20, V 9-12, A_NEG 8, A_SWZ 0-7
 *  W2: A_R 28-31, A_MUX 26-27, B_NEG 25, B_SWZ 17-24, B_R 13-16, B_MUX 11-12,
 *      C_NEG 10, C_SWZ 2-9, C_R_HI 0-1
 *  W3: C_R_LO 30-31, C_MUX 28-29, MAC_MASK 24-27, OUT_R 20-23, ILU_MASK 16-19,
 *      O_MASK 12-15, ORB 11, OUT_ADDR 3-10, OUT_MUX 2, A0X 1, FINAL 0
 * Swizzle-Byte (x<<6)|(y<<4)|(z<<2)|w, Masken Bit3=X..Bit0=W.
 * Operationen (NV2A, vgl. Public-Domain-Referenz nv2a_vsh_cpu + Khronos
 * NV_vertex_program1_1):
 *  MAC: NOP MOV(A) MUL(A,B) ADD(A,C) MAD(A,B,C) DP3 DPH DP4 DST MIN MAX SLT
 *       SGE (A,B) ARL(A: A0 = floor(A.x + 0.001))
 *  ILU (Quelle C, Skalar = C.x nach Swizzle): NOP MOV(C) RCP RCC RSQ EXP LOG LIT
 *  MAC und ILU lesen ihre Eingaenge vor dem Schreiben (parallel); ist die MAC
 *  aktiv, schreibt die ILU ihr Temp-Ergebnis nach R1. R12 lesen = oPos.
 *  Ausgabe: O_MASK != 0 -> Ziel = OUT_MUX ? ILU : MAC; ORB 1 = Ausgaberegister
 *  OUT_ADDR (0 oPos, 3 oD0, 4 oD1, 5 oFog, 6 oPts, 7/8 oB0/1, 9-12 oT0-3),
 *  ORB 0 = Konstante c[OUT_ADDR]. NV2A laesst Produkte endlicher Zahlen nicht
 *  unendlich werden (MUL/MAD/DPx geklemmt auf groesste endliche Zahl). */
#include "render_vsh.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

static void rqv_swz(unsigned char sw[4], unsigned v)
{
    sw[0] = (unsigned char)((v >> 6) & 3u);
    sw[1] = (unsigned char)((v >> 4) & 3u);
    sw[2] = (unsigned char)((v >> 2) & 3u);
    sw[3] = (unsigned char)(v & 3u);
}

int rqv_decode(const unsigned *slots, unsigned n, rqv_prog_t *p)
{
    unsigned s;
    memset(p, 0, sizeof *p);
    if (n < 1u || n > RQV_MAX_OPS) { return 0; }
    for (s = 0; s < n; s++) {
        const unsigned *t = slots + s * 4u;
        unsigned w1 = t[1], w2 = t[2], w3 = t[3];
        rqv_op_t *o = &p->ops[s];
        o->ilu = (unsigned char)((w1 >> 25) & 7u);
        o->mac = (unsigned char)((w1 >> 21) & 0xFu);
        o->cidx = (unsigned short)((w1 >> 13) & 0xFFu);
        o->vin = (unsigned char)((w1 >> 9) & 0xFu);
        o->in[0].neg = (unsigned char)((w1 >> 8) & 1u);
        rqv_swz(o->in[0].sw, w1 & 0xFFu);
        o->in[0].reg = (unsigned char)((w2 >> 28) & 0xFu);
        o->in[0].mux = (unsigned char)((w2 >> 26) & 3u);
        o->in[1].neg = (unsigned char)((w2 >> 25) & 1u);
        rqv_swz(o->in[1].sw, (w2 >> 17) & 0xFFu);
        o->in[1].reg = (unsigned char)((w2 >> 13) & 0xFu);
        o->in[1].mux = (unsigned char)((w2 >> 11) & 3u);
        o->in[2].neg = (unsigned char)((w2 >> 10) & 1u);
        rqv_swz(o->in[2].sw, (w2 >> 2) & 0xFFu);
        o->in[2].reg = (unsigned char)(((w2 & 3u) << 2) | ((w3 >> 30) & 3u));
        o->in[2].mux = (unsigned char)((w3 >> 28) & 3u);
        o->macmask = (unsigned char)((w3 >> 24) & 0xFu);
        o->outr = (unsigned char)((w3 >> 20) & 0xFu);
        o->ilumask = (unsigned char)((w3 >> 16) & 0xFu);
        o->omask = (unsigned char)((w3 >> 12) & 0xFu);
        o->orb = (unsigned char)((w3 >> 11) & 1u);
        o->oaddr = (unsigned char)((w3 >> 3) & 0xFFu);
        o->omux = (unsigned char)((w3 >> 2) & 1u);
        o->a0x = (unsigned char)((w3 >> 1) & 1u);
        o->final = (unsigned char)(w3 & 1u);
        if (o->mac > 13u) { return 0; }
        if (o->omask) {
            if (o->orb) {
                unsigned a = o->oaddr & 0xFu;
                if (a > 12u) { return 0; }
                p->outmask |= 1u << a;
            } else {
                p->cwrite = 1;
            }
        }
        if (o->a0x) { p->rel = 1; }
        {
            int k;
            for (k = 0; k < 3; k++) {
                if (o->in[k].mux == 2) { p->vinmask |= 1u << o->vin; }
            }
        }
        p->nops = (int)s + 1;
        if (o->final) { break; }
    }
    return 1;
}

/* Produkt ohne Ueberlauf nach unendlich (NV2A). */
static float rqv_fmul(float a, float b)
{
    float r = a * b;
    if (isinf(r) && !isinf(a) && !isinf(b)) {
        union { float f; unsigned u; } x;
        x.f = r;
        x.u = (x.u & 0xFF000000u) + 0x7FFFFFu;
        r = x.f;
    }
    return r;
}

static float rqv_rcp(float x)
{
    if (x == 1.0f) { return 1.0f; }
    if (x == 0.0f) { return INFINITY; }
    return 1.0f / x;
}

static float rqv_rcc(float x)
{
    float r;
    if (x == 1.0f) { return 1.0f; }
    r = 1.0f / x;
    if (r > 0.0f) {
        if (r < 5.42101e-020f) { r = 5.42101e-020f; }
        else if (r > 1.884467e+019f) { r = 1.884467e+019f; }
    } else {
        if (r < -1.884467e+019f) { r = -1.884467e+019f; }
        else if (r > -5.42101e-020f) { r = -5.42101e-020f; }
    }
    return r;
}

static void rqv_fetch(float out[4], const rqv_in_t *in, const rqv_op_t *o,
                      const float (*R)[4], const float *opos, const float (*vin)[4],
                      float (*c)[4], int a0)
{
    const float *src;
    static const float zero[4] = { 0, 0, 0, 0 };
    switch (in->mux) {
    case 1:
        src = (in->reg == 12u) ? opos : (in->reg < 12u ? R[in->reg] : zero);
        break;
    case 2:
        src = vin[o->vin];
        break;
    case 3: {
        int ci = (int)o->cidx + (o->a0x ? a0 : 0);
        src = (ci >= 0 && ci < RQV_NCONST) ? c[ci] : zero;
        break;
    }
    default:
        src = zero;
        break;
    }
    if (in->neg) {
        out[0] = -src[in->sw[0]]; out[1] = -src[in->sw[1]];
        out[2] = -src[in->sw[2]]; out[3] = -src[in->sw[3]];
    } else {
        out[0] = src[in->sw[0]]; out[1] = src[in->sw[1]];
        out[2] = src[in->sw[2]]; out[3] = src[in->sw[3]];
    }
}

static void rqv_wr(float *dst, const float v[4], unsigned mask)
{
    if (mask & 8u) { dst[0] = v[0]; }
    if (mask & 4u) { dst[1] = v[1]; }
    if (mask & 2u) { dst[2] = v[2]; }
    if (mask & 1u) { dst[3] = v[3]; }
}

void rqv_run(const rqv_prog_t *p, const float (*vin)[4], float (*c)[4], rqv_out_t *out)
{
    float R[12][4];
    int a0 = 0, k;
    memset(R, 0, sizeof R);
    for (k = 0; k < 13; k++) {
        out->o[k][0] = out->o[k][1] = out->o[k][2] = 0.0f;
        out->o[k][3] = 1.0f;
    }
    for (k = 0; k < p->nops; k++) {
        const rqv_op_t *o = &p->ops[k];
        float A[4], B[4], C[4], m[4], l[4];
        int hm = 0, hl = 0, na0 = a0, ma = 0;
        if (o->mac) {
            rqv_fetch(A, &o->in[0], o, (const float (*)[4])R, out->o[0], vin, c, a0);
            if (o->mac == 2 || (o->mac >= 4 && o->mac <= 12)) {
                rqv_fetch(B, &o->in[1], o, (const float (*)[4])R, out->o[0], vin, c, a0);
            }
            if (o->mac == 3 || o->mac == 4) {
                rqv_fetch(C, &o->in[2], o, (const float (*)[4])R, out->o[0], vin, c, a0);
            }
        }
        if (o->ilu) {
            float S[4];
            rqv_fetch(S, &o->in[2], o, (const float (*)[4])R, out->o[0], vin, c, a0);
            hl = 1;
            switch (o->ilu) {
            case 1: memcpy(l, S, sizeof l); break;
            case 2: l[0] = l[1] = l[2] = l[3] = rqv_rcp(S[0]); break;
            case 3: l[0] = l[1] = l[2] = l[3] = rqv_rcc(S[0]); break;
            case 4: {
                float x = fabsf(S[0]);
                float r = (x == 1.0f) ? 1.0f : (x == 0.0f ? INFINITY : 1.0f / sqrtf(x));
                l[0] = l[1] = l[2] = l[3] = r;
                break;
            }
            case 5: {
                float f = floorf(S[0]);
                l[0] = powf(2.0f, f); l[1] = S[0] - f; l[2] = powf(2.0f, S[0]); l[3] = 1.0f;
                break;
            }
            case 6: {
                float x = fabsf(S[0]);
                if (x == 0.0f) { l[0] = -INFINITY; l[1] = 1.0f; l[2] = -INFINITY; }
                else if (isinf(x)) { l[0] = INFINITY; l[1] = 1.0f; l[2] = INFINITY; }
                else {
                    union { float f; unsigned u; } xi, mi;
                    xi.f = x;
                    mi.u = (xi.u & 0x7FFFFFu) | 0x3F800000u;
                    l[0] = (float)((int)((xi.u >> 23) & 0xFFu) - 127);
                    l[1] = mi.f;
                    l[2] = log2f(x);
                }
                l[3] = 1.0f;
                break;
            }
            default: { /* 7 LIT */
                const float kmax = 127.9961f;
                float pw = S[3] < -kmax ? -kmax : (S[3] > kmax ? kmax : S[3]);
                l[0] = 1.0f; l[1] = 0.0f; l[2] = 0.0f; l[3] = 1.0f;
                if (S[0] > 0.0f) {
                    l[1] = S[0];
                    if (S[1] > 0.0f) { l[2] = powf(S[1], pw); }
                }
                break;
            }
            }
        }
        if (o->mac) {
            hm = 1;
            switch (o->mac) {
            case 1: memcpy(m, A, sizeof m); break;
            case 2: m[0] = rqv_fmul(A[0], B[0]); m[1] = rqv_fmul(A[1], B[1]);
                    m[2] = rqv_fmul(A[2], B[2]); m[3] = rqv_fmul(A[3], B[3]); break;
            case 3: m[0] = A[0] + C[0]; m[1] = A[1] + C[1]; m[2] = A[2] + C[2]; m[3] = A[3] + C[3]; break;
            case 4: m[0] = rqv_fmul(A[0], B[0]) + C[0]; m[1] = rqv_fmul(A[1], B[1]) + C[1];
                    m[2] = rqv_fmul(A[2], B[2]) + C[2]; m[3] = rqv_fmul(A[3], B[3]) + C[3]; break;
            case 5: m[0] = m[1] = m[2] = m[3] = rqv_fmul(A[0], B[0]) + rqv_fmul(A[1], B[1]) + rqv_fmul(A[2], B[2]); break;
            case 6: m[0] = m[1] = m[2] = m[3] = rqv_fmul(A[0], B[0]) + rqv_fmul(A[1], B[1]) + rqv_fmul(A[2], B[2]) + B[3]; break;
            case 7: m[0] = m[1] = m[2] = m[3] = rqv_fmul(A[0], B[0]) + rqv_fmul(A[1], B[1]) + rqv_fmul(A[2], B[2]) + rqv_fmul(A[3], B[3]); break;
            case 8: m[0] = 1.0f; m[1] = rqv_fmul(A[1], B[1]); m[2] = A[2]; m[3] = B[3]; break;
            case 9: m[0] = A[0] < B[0] ? A[0] : B[0]; m[1] = A[1] < B[1] ? A[1] : B[1];
                    m[2] = A[2] < B[2] ? A[2] : B[2]; m[3] = A[3] < B[3] ? A[3] : B[3]; break;
            case 10: m[0] = A[0] > B[0] ? A[0] : B[0]; m[1] = A[1] > B[1] ? A[1] : B[1];
                     m[2] = A[2] > B[2] ? A[2] : B[2]; m[3] = A[3] > B[3] ? A[3] : B[3]; break;
            case 11: m[0] = A[0] < B[0] ? 1.0f : 0.0f; m[1] = A[1] < B[1] ? 1.0f : 0.0f;
                     m[2] = A[2] < B[2] ? 1.0f : 0.0f; m[3] = A[3] < B[3] ? 1.0f : 0.0f; break;
            case 12: m[0] = A[0] >= B[0] ? 1.0f : 0.0f; m[1] = A[1] >= B[1] ? 1.0f : 0.0f;
                     m[2] = A[2] >= B[2] ? 1.0f : 0.0f; m[3] = A[3] >= B[3] ? 1.0f : 0.0f; break;
            default: { /* 13 ARL */
                float v = floorf(A[0] + 0.001f);
                na0 = (int)v; ma = 1;
                m[0] = m[1] = m[2] = m[3] = v;
                break;
            }
            }
        }
        /* Schreiben (nach beiden Berechnungen). */
        if (hm && !ma && o->macmask && o->outr < 12u) { rqv_wr(R[o->outr], m, o->macmask); }
        if (hl && o->ilumask) {
            unsigned d = hm ? 1u : o->outr;
            if (d < 12u) { rqv_wr(R[d], l, o->ilumask); }
        }
        if (o->omask && (o->omux ? hl : hm) && !(o->omux == 0 && ma)) {
            const float *v = o->omux ? l : m;
            if (o->orb) {
                unsigned a = o->oaddr & 0xFu;
                if (a <= 12u) { rqv_wr(out->o[a], v, o->omask); }
            } else if (o->oaddr < RQV_NCONST) {
                rqv_wr(c[o->oaddr], v, o->omask);
            }
        }
        a0 = na0;
        if (o->final) { break; }
    }
}

/* ---------- Einzeltests ---------- */
/* Encoder fuer konstruierte Slots (Gegenstueck zu rqv_decode). Eingaenge
 * als (mux, reg, neg, swz); out: (macmask, outr, ilumask, omask, orb, oaddr, omux). */
typedef struct { unsigned mux, reg, neg, swz; } te_in_t;
static void te_enc(unsigned *t, unsigned mac, unsigned ilu, unsigned cidx, unsigned vin,
                   te_in_t a, te_in_t b, te_in_t c, unsigned macmask, unsigned outr,
                   unsigned ilumask, unsigned omask, unsigned orb, unsigned oaddr,
                   unsigned omux, unsigned a0x, unsigned fin)
{
    t[0] = 0;
    t[1] = (ilu << 25) | (mac << 21) | (cidx << 13) | (vin << 9) | (a.neg << 8) | a.swz;
    t[2] = (a.reg << 28) | (a.mux << 26) | (b.neg << 25) | (b.swz << 17) | (b.reg << 13) |
           (b.mux << 11) | (c.neg << 10) | (c.swz << 2) | ((c.reg >> 2) & 3u);
    t[3] = ((c.reg & 3u) << 30) | (c.mux << 28) | (macmask << 24) | (outr << 20) |
           (ilumask << 16) | (omask << 12) | (orb << 11) | (oaddr << 3) | (omux << 2) |
           (a0x << 1) | fin;
}

#define SWI 0x1Bu /* xyzw */
static const te_in_t TN = { 0, 0, 0, SWI };
static te_in_t TV(unsigned r) { te_in_t x; x.mux = 2; x.reg = 0; x.neg = 0; x.swz = SWI; (void)r; return x; }
static te_in_t TR(unsigned r, unsigned swz, unsigned neg) { te_in_t x; x.mux = 1; x.reg = r; x.neg = neg; x.swz = swz; return x; }
static te_in_t TC(unsigned swz, unsigned neg) { te_in_t x; x.mux = 3; x.reg = 0; x.neg = neg; x.swz = swz; return x; }

static int te_near(float a, float b)
{
    float d = fabsf(a - b), s = fabsf(b) > 1.0f ? fabsf(b) : 1.0f;
    return d <= 1e-5f * s;
}

static int te_check(void (*logf)(const char *), const char *name, const float *got,
                    const float *want)
{
    char line[256];
    int ok = te_near(got[0], want[0]) && te_near(got[1], want[1]) &&
             te_near(got[2], want[2]) && te_near(got[3], want[3]);
    snprintf(line, sizeof line, "VSH %-28s %s got=(%g,%g,%g,%g) want=(%g,%g,%g,%g)",
             name, ok ? "ok  " : "FAIL", got[0], got[1], got[2], got[3],
             want[0], want[1], want[2], want[3]);
    logf(line);
    return ok ? 0 : 1;
}

int rqv_selftest(void (*logf)(const char *line))
{
    unsigned sl[16 * 4];
    rqv_prog_t p;
    rqv_out_t o;
    float vin[16][4], c[192][4];
    int fails = 0, i;
    memset(vin, 0, sizeof vin);
    memset(c, 0, sizeof c);
    vin[0][0] = 1; vin[0][1] = 2; vin[0][2] = 3; vin[0][3] = 1;
    vin[2][0] = 0.5f; vin[2][1] = -0.25f; vin[2][2] = 4; vin[2][3] = 8;
    for (i = 0; i < 4; i++) { c[100][i] = (float)(i + 1); }      /* c4 = (1,2,3,4) */
    c[101][0] = 0.5f; c[101][1] = 0.5f; c[101][2] = 0.5f; c[101][3] = 0.5f;
    c[110][0] = 10; c[110][1] = 20; c[110][2] = 30; c[110][3] = 40;
    /* 1: DP4 oPos.x, v0, c[100]; MOV oD0, v0 (final) */
    te_enc(&sl[0], 7, 0, 100, 0, TV(0), TC(SWI, 0), TN, 0, 0, 0, 8, 1, 0, 0, 0, 0);
    te_enc(&sl[4], 1, 0, 0, 2, TV(2), TN, TN, 0, 0, 0, 0xF, 1, RQV_OD0, 0, 0, 1);
    rqv_decode(sl, 2, &p); rqv_run(&p, (const float (*)[4])vin, c, &o);
    { float w[4] = { 1 * 1 + 2 * 2 + 3 * 3 + 1 * 4, 0, 0, 1 }; fails += te_check(logf, "DP4 oPos.x", o.o[0], w); }
    { float w[4] = { 0.5f, -0.25f, 4, 8 }; fails += te_check(logf, "MOV oD0,v2", o.o[RQV_OD0], w); }
    /* 2: MUL R0, v0, c[101]; ADD R1, R0, c(neg, wzyx); MOV oT1, R1 */
    te_enc(&sl[0], 2, 0, 101, 0, TV(0), TC(SWI, 0), TN, 0xF, 0, 0, 0, 0, 0, 0, 0, 0);
    te_enc(&sl[4], 3, 0, 100, 0, TR(0, SWI, 0), TN, TC(0xE4u /*wzyx*/, 1), 0xF, 1, 0, 0, 0, 0, 0, 0, 0);
    te_enc(&sl[8], 1, 0, 0, 0, TR(1, SWI, 0), TN, TN, 0, 0, 0, 0xF, 1, RQV_OT1, 0, 0, 1);
    rqv_decode(sl, 3, &p); rqv_run(&p, (const float (*)[4])vin, c, &o);
    { float w[4] = { 0.5f - 4, 1 - 3, 1.5f - 2, 0.5f - 1 }; fails += te_check(logf, "MUL/ADD neg+swz oT1", o.o[RQV_OT1], w); }
    /* 3: MAD R2, v2, c4, c4 -> oD1 mit Maske xz (Rest Startwert 0,0,0,1) */
    te_enc(&sl[0], 4, 0, 100, 2, TV(2), TC(SWI, 0), TC(SWI, 0), 0xF, 2, 0, 0xA, 1, RQV_OD1, 0, 0, 1);
    rqv_decode(sl, 1, &p); rqv_run(&p, (const float (*)[4])vin, c, &o);
    { float w[4] = { 0.5f * 1 + 1, 0, 4 * 3 + 3, 1 }; fails += te_check(logf, "MAD Maske xz oD1", o.o[RQV_OD1], w); }
    /* 4: DP3 / DPH / DST / MIN / MAX / SLT / SGE */
    {
        static const struct { unsigned mac; const char *n; float w[4]; } t4[] = {
            { 5, "DP3", { 14, 14, 14, 14 } },
            { 6, "DPH", { 18, 18, 18, 18 } },
            { 8, "DST", { 1, 4, 3, 4 } },
            { 9, "MIN", { 1, 2, 3, 1 } },
            { 10, "MAX", { 1, 2, 3, 4 } },
            { 11, "SLT", { 0, 0, 0, 1 } },
            { 12, "SGE", { 1, 1, 1, 0 } },
        };
        unsigned q;
        for (q = 0; q < sizeof t4 / sizeof t4[0]; q++) {
            te_enc(&sl[0], t4[q].mac, 0, 100, 0, TV(0), TC(SWI, 0), TN, 0, 0, 0, 0xF, 1, RQV_OT0, 0, 0, 1);
            rqv_decode(sl, 1, &p); rqv_run(&p, (const float (*)[4])vin, c, &o);
            fails += te_check(logf, t4[q].n, o.o[RQV_OT0], t4[q].w);
        }
    }
    /* 5: ILU allein: RCP/RCC/RSQ/EXP/LOG/LIT auf v2 (Swizzle z -> 4) */
    {
        static const struct { unsigned ilu, swz; const char *n; float w[4]; } t5[] = {
            { 2, 0xAA, "RCP(4)", { 0.25f, 0.25f, 0.25f, 0.25f } },
            { 3, 0xAA, "RCC(4)", { 0.25f, 0.25f, 0.25f, 0.25f } },
            { 4, 0xAA, "RSQ(4)", { 0.5f, 0.5f, 0.5f, 0.5f } },
            { 5, 0x00, "EXP(0.5)", { 1, 0.5f, 1.41421356f, 1 } },
            { 6, 0xFF, "LOG(8)", { 3, 1, 3, 1 } },
            { 7, 0x1B, "LIT(0.5,-0.25,..)", { 1, 0.5f, 0, 1 } },
        };
        unsigned q;
        for (q = 0; q < sizeof t5 / sizeof t5[0]; q++) {
            te_in_t cc; cc.mux = 2; cc.reg = 0; cc.neg = 0; cc.swz = t5[q].swz;
            te_enc(&sl[0], 0, t5[q].ilu, 0, 2, TN, TN, cc, 0, 3, 0, 0xF, 1, RQV_OT2, 1, 0, 1);
            rqv_decode(sl, 1, &p); rqv_run(&p, (const float (*)[4])vin, c, &o);
            fails += te_check(logf, t5[q].n, o.o[RQV_OT2], t5[q].w);
        }
    }
    /* 6: LIT mit Glanz: (N.L=0.8, N.H=0.5, -, Exp 2) -> (1, 0.8, 0.25, 1) */
    {
        te_in_t cc; cc.mux = 3; cc.reg = 0; cc.neg = 0; cc.swz = SWI;
        c[120][0] = 0.8f; c[120][1] = 0.5f; c[120][2] = 0; c[120][3] = 2;
        te_enc(&sl[0], 0, 7, 120, 0, TN, TN, cc, 0, 0, 0, 0xF, 1, RQV_OT3, 1, 0, 1);
        rqv_decode(sl, 1, &p); rqv_run(&p, (const float (*)[4])vin, c, &o);
        { float w[4] = { 1, 0.8f, 0.25f, 1 }; fails += te_check(logf, "LIT Glanz", o.o[RQV_OT3], w); }
    }
    /* 7: gepaart: MUL R4 = v0*c4 und RCP v0.y -> ILU-Temp landet in R1 (nicht R4) */
    {
        te_in_t cc; cc.mux = 2; cc.reg = 0; cc.neg = 0; cc.swz = 0x55; /* yyyy */
        te_enc(&sl[0], 2, 2, 100, 0, TV(0), TC(SWI, 0), cc, 0xF, 4, 0xF, 0, 0, 0, 0, 0, 0);
        te_enc(&sl[4], 1, 0, 0, 0, TR(1, SWI, 0), TN, TN, 0, 0, 0, 0xF, 1, RQV_OT0, 0, 0, 0);
        te_enc(&sl[8], 1, 0, 0, 0, TR(4, SWI, 0), TN, TN, 0, 0, 0, 0xF, 1, RQV_OT1, 0, 0, 1);
        rqv_decode(sl, 3, &p); rqv_run(&p, (const float (*)[4])vin, c, &o);
        { float w[4] = { 0.5f, 0.5f, 0.5f, 0.5f }; fails += te_check(logf, "gepaart ILU->R1", o.o[RQV_OT0], w); }
        { float w[4] = { 1, 4, 9, 4 }; fails += te_check(logf, "gepaart MAC->R4", o.o[RQV_OT1], w); }
    }
    /* 8: R12 = oPos-Rueckleser + Ausgabe-MUX ILU */
    {
        te_in_t cc; cc.mux = 1; cc.reg = 12; cc.neg = 0; cc.swz = 0xFF; /* R12.w */
        te_enc(&sl[0], 1, 0, 0, 0, TV(0), TN, TN, 0, 0, 0, 0xF, 1, RQV_OPOS, 0, 0, 0);
        te_enc(&sl[4], 1, 3, 110, 0, TC(SWI, 0), TN, cc, 0, 0, 0, 0xF, 1, RQV_OFOG, 1, 0, 1);
        rqv_decode(sl, 2, &p); rqv_run(&p, (const float (*)[4])vin, c, &o);
        { float w[4] = { 1, 1, 1, 1 }; fails += te_check(logf, "R12.w RCC -> oFog (MUX ILU)", o.o[RQV_OFOG], w); }
    }
    /* 9: ARL A0 = floor(v2.z=4) -> MOV oT0, c[106 + A0] = c[110] */
    {
        te_in_t aa; aa.mux = 2; aa.reg = 0; aa.neg = 0; aa.swz = 0xAA;
        te_enc(&sl[0], 13, 0, 0, 2, aa, TN, TN, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        te_enc(&sl[4], 1, 0, 106, 0, TC(SWI, 0), TN, TN, 0, 0, 0, 0xF, 1, RQV_OT0, 0, 1, 1);
        rqv_decode(sl, 2, &p); rqv_run(&p, (const float (*)[4])vin, c, &o);
        { float w[4] = { 10, 20, 30, 40 }; fails += te_check(logf, "ARL + c[A0+106]", o.o[RQV_OT0], w); }
    }
    /* 10: Konstanten-Schrieb (ORB=0) + spaeteres Lesen */
    {
        te_enc(&sl[0], 1, 0, 0, 0, TV(0), TN, TN, 0, 0, 0, 0xF, 0, 130, 0, 0, 0);
        te_enc(&sl[4], 1, 0, 130, 0, TC(SWI, 1), TN, TN, 0, 0, 0, 0xF, 1, RQV_OB0, 0, 0, 1);
        rqv_decode(sl, 2, &p); rqv_run(&p, (const float (*)[4])vin, c, &o);
        { float w[4] = { -1, -2, -3, -1 }; fails += te_check(logf, "ORB=0 c-Schrieb", o.o[RQV_OB0], w); }
    }
    /* 11: Ueberlauf: MUL 1e30*1e30 bleibt endlich */
    {
        c[140][0] = 1e30f; c[140][1] = 1e30f; c[140][2] = 1; c[140][3] = 1;
        te_enc(&sl[0], 2, 0, 140, 0, TC(SWI, 0), TC(SWI, 0), TN, 0, 0, 0, 0xF, 1, RQV_OT3, 0, 0, 1);
        rqv_decode(sl, 1, &p); rqv_run(&p, (const float (*)[4])vin, c, &o);
        {
            char line[128];
            int ok = !isinf(o.o[RQV_OT3][0]) && o.o[RQV_OT3][0] > 1e38f;
            snprintf(line, sizeof line, "VSH %-28s %s got=%g", "MUL ohne Inf", ok ? "ok  " : "FAIL", o.o[RQV_OT3][0]);
            logf(line);
            fails += ok ? 0 : 1;
        }
    }
    return fails;
}
