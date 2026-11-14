/* render_vshgen.c - RENDER-QA (W77): NV2A-VSH-Programm -> HLSL-Vertex-Shader.
 * Rein (keine D3D-Abhaengigkeit). Der erzeugte vs_4_0-Shader fuehrt das
 * Mikrocode-Programm je Vertex auf der GPU aus (gleiche Semantik wie der
 * CPU-Interpreter render_vsh.c, der als Referenz in render_selftest dient).
 * oPos des Programms wird NICHT benutzt: die Position kommt wie im bisherigen
 * Pfad aus Stream 0 (CPU-Pfad: P1/P2/Hor+/RT-Viewport) mit derselben Formel
 * wie g_im_vs (exakt) bzw. kIP_VS (Zwischenbild, lerp=1) - Bildlage und
 * 120-FPS-Interpolation bleiben dadurch unveraendert.
 * Eingaenge: Stream 0 = im_vertex_t (pos/col/uv), [Stream 1 = pos0 (lerp)],
 * Stream 2 = gepackte v-Register (vinmask, aufsteigend, je float4, ATTRk).
 * Konstanten (b3): vc[n] = statisch benutzte Konstanten (umnummeriert; bei
 * relativer Adressierung alle 192), rq_fog (x Modus 0 aus/1 linear/2 exp/
 * 3 exp2, y p0, z p1 - NV2A FOG_PARAMS wie vom XDK-D3D berechnet), rq_misc
 * (x: oD0 statt i.col, y: oT0 statt i.uv, z: oD1 statt 0).
 * Nebelfaktor (NV2A, Abstand d = oFog.x bei Vertex-Programmen):
 *   linear  f = p0 + d*p1 - 1          (Tabelle NONE: p0 = p1 = 1 -> f = d)
 *   exp     f = p0 + 2^(16*d*p1) - 1.5
 *   exp2    f = p0 + 2^(-32*d^2*p1^2) - 1.5,   danach f auf [0,1] geklemmt.
 * Ausgabefarben werden wie auf der NV2A auf [0,1] geklemmt. */
#include "render_vsh.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct { char *p; size_t n, o; int ovf; } rqs_t;

static void rqs(rqs_t *s, const char *fmt, ...)
{
    va_list ap;
    int k;
    if (s->ovf || s->o >= s->n) { s->ovf = 1; return; }
    va_start(ap, fmt);
    k = vsnprintf(s->p + s->o, s->n - s->o, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= s->n - s->o) { s->ovf = 1; return; }
    s->o += (size_t)k;
}

static const char *rqv_msk(unsigned m, char *b)
{
    int k = 0;
    b[k++] = '.';
    if (m & 8u) { b[k++] = 'x'; }
    if (m & 4u) { b[k++] = 'y'; }
    if (m & 2u) { b[k++] = 'z'; }
    if (m & 1u) { b[k++] = 'w'; }
    b[k] = 0;
    return b;
}

static void rqv_src(rqs_t *s, const rqv_in_t *in, const rqv_op_t *o, const unsigned char *cmap, int rel)
{
    static const char sc[4] = { 'x', 'y', 'z', 'w' };
    const char *neg = in->neg ? "-" : "";
    char sw[6];
    sw[0] = '.'; sw[1] = sc[in->sw[0]]; sw[2] = sc[in->sw[1]];
    sw[3] = sc[in->sw[2]]; sw[4] = sc[in->sw[3]]; sw[5] = 0;
    switch (in->mux) {
    case 1:
        if (in->reg == 12u) { rqs(s, "%soPos%s", neg, sw); }
        else if (in->reg < 12u) { rqs(s, "%sR%u%s", neg, (unsigned)in->reg, sw); }
        else { rqs(s, "float4(0,0,0,0)"); }
        break;
    case 2:
        rqs(s, "%sv%u%s", neg, (unsigned)o->vin, sw);
        break;
    case 3:
        if (rel) {
            if (o->a0x) { rqs(s, "%sc[clamp(a0 + %u, 0, 191)]%s", neg, (unsigned)o->cidx, sw); }
            else { rqs(s, "%sc[%u]%s", neg, (unsigned)o->cidx, sw); }
        } else {
            rqs(s, "%sc[%u]%s", neg, (unsigned)cmap[o->cidx], sw);
        }
        break;
    default:
        rqs(s, "float4(0,0,0,0)");
        break;
    }
}

unsigned rqv_fmt_size(unsigned f)
{
    switch (f) {
    case 0x12u: case 0x40u: case 0x16u: case 0x21u: case 0x25u: case 0x44u: return 4u;
    case 0x22u: case 0x41u: case 0x45u: return 8u;
    case 0x32u: case 0x72u: return 12u;
    case 0x42u: return 16u;
    case 0x11u: case 0x15u: case 0x24u: return 2u;
    case 0x31u: case 0x35u: return 6u;
    case 0x14u: return 1u;
    case 0x34u: return 3u;
    default: return 0u;
    }
}

void rqv_vfetch(unsigned f, const unsigned char *p, const float cur[4], float o[4])
{
    o[0] = 0.0f; o[1] = 0.0f; o[2] = 0.0f; o[3] = 1.0f;
    switch (f) {
    case 0x12u: o[0] = ((const float *)p)[0]; break;
    case 0x22u: o[0] = ((const float *)p)[0]; o[1] = ((const float *)p)[1]; break;
    case 0x32u: o[0] = ((const float *)p)[0]; o[1] = ((const float *)p)[1]; o[2] = ((const float *)p)[2]; break;
    case 0x42u: memcpy(o, p, 16); break;
    case 0x72u: o[0] = ((const float *)p)[0]; o[1] = ((const float *)p)[1]; o[3] = ((const float *)p)[2]; break;
    case 0x40u: {
        unsigned c;
        memcpy(&c, p, 4);
        o[0] = (float)((c >> 16) & 0xFFu) / 255.0f; o[1] = (float)((c >> 8) & 0xFFu) / 255.0f;
        o[2] = (float)(c & 0xFFu) / 255.0f; o[3] = (float)(c >> 24) / 255.0f;
        break;
    }
    case 0x11u: case 0x21u: case 0x31u: case 0x41u: {
        unsigned k, nn = f >> 4;
        for (k = 0; k < nn; k++) { o[k] = (float)((const short *)p)[k] / 32767.0f; }
        break;
    }
    case 0x15u: case 0x25u: case 0x35u: case 0x45u: {
        unsigned k, nn = f >> 4;
        for (k = 0; k < nn; k++) { o[k] = (float)((const short *)p)[k]; }
        break;
    }
    case 0x14u: case 0x24u: case 0x34u: case 0x44u: {
        unsigned k, nn = f >> 4;
        for (k = 0; k < nn; k++) { o[k] = (float)p[k] / 255.0f; }
        break;
    }
    case 0x16u: { /* NORMPACKED3: 11:11:10 vorzeichenbehaftet */
        unsigned c;
        int x, y, z;
        memcpy(&c, p, 4);
        x = (int)(c << 21) >> 21; y = (int)((c >> 11) << 21) >> 21; z = (int)c >> 22;
        o[0] = (float)x / 1023.0f; o[1] = (float)y / 1023.0f; o[2] = (float)z / 511.0f;
        break;
    }
    default: /* 0x02 NONE / unbekannt: Inline-Register */
        memcpy(o, cur, 16);
        break;
    }
}

int rqv_raw_build(const unsigned stream[16], const unsigned off[16], const unsigned fmt[16],
                  unsigned vinmask, rqv_raw_t *r)
{
    unsigned k;
    memset(r, 0, sizeof *r);
    for (k = 0; k < 16u; k++) {
        unsigned f = (stream[k] == 0u) ? (fmt[k] & 0xFFu) : 0x02u, sz = rqv_fmt_size(f);
        if (!(vinmask & (1u << k))) { continue; }
        if (!sz) { r->fmt[k] = 0; r->inl[k] = (unsigned char)r->ninl++; continue; }
        if (off[k] > 4096u) { return 0; }
        if (off[k] + sz > r->need) { r->need = off[k] + sz; }
        r->fmt[k] = (unsigned char)f;
        r->off[k] = (unsigned short)off[k];
    }
    return 1;
}

/* HLSL-Ausdruck fuer ein Roh-Register (va = Vertexadresse in Bytes). */
static void rqv_rawfetch(rqs_t *s, unsigned k, const rqv_raw_t *r)
{
    unsigned f = r->fmt[k], o = r->off[k], q, nn = f >> 4;
    static const char cc[4] = { 'x', 'y', 'z', 'w' };
    rqs(s, "  float4 v%u = float4(0, 0, 0, 1);\n", k);
    switch (f) {
    case 0: rqs(s, "  v%u = rq_inl[%u];\n", k, (unsigned)r->inl[k]); break;
    case 0x12u: case 0x22u: case 0x32u: case 0x42u:
        for (q = 0; q < nn; q++) { rqs(s, "  v%u.%c = asfloat(rq_ld(va + %uu));\n", k, cc[q], o + 4u * q); }
        break;
    case 0x72u:
        rqs(s, "  v%u.x = asfloat(rq_ld(va + %uu)); v%u.y = asfloat(rq_ld(va + %uu)); v%u.w = asfloat(rq_ld(va + %uu));\n",
            k, o, k, o + 4u, k, o + 8u);
        break;
    case 0x40u:
        rqs(s, "  { uint c = rq_ld(va + %uu); v%u = float4((float)((c >> 16) & 255u) / 255.0, (float)((c >> 8) & 255u) / 255.0,"
               " (float)(c & 255u) / 255.0, (float)(c >> 24) / 255.0); }\n", o, k);
        break;
    case 0x11u: case 0x21u: case 0x31u: case 0x41u:
        for (q = 0; q < nn; q++) { rqs(s, "  v%u.%c = rq_s16(va + %uu) / 32767.0;\n", k, cc[q], o + 2u * q); }
        break;
    case 0x15u: case 0x25u: case 0x35u: case 0x45u:
        for (q = 0; q < nn; q++) { rqs(s, "  v%u.%c = rq_s16(va + %uu);\n", k, cc[q], o + 2u * q); }
        break;
    case 0x14u: case 0x24u: case 0x34u: case 0x44u:
        for (q = 0; q < nn; q++) { rqs(s, "  v%u.%c = (float)(rq_ld(va + %uu) & 255u) / 255.0;\n", k, cc[q], o + q); }
        break;
    case 0x16u:
        rqs(s, "  { uint c = rq_ld(va + %uu); v%u.x = (float)((int)(c << 21) >> 21) / 1023.0;"
               " v%u.y = (float)((int)((c >> 11) << 21) >> 21) / 1023.0; v%u.z = (float)((int)c >> 22) / 511.0; }\n",
            o, k, k, k);
        break;
    default: break;
    }
}

int rqv_hlsl(const rqv_prog_t *p, int lerp, char *buf, size_t bufn,
             unsigned char cidx_of[RQV_NCONST], unsigned *ncmap)
{
    return rqv_hlsl_ex(p, lerp, NULL, buf, bufn, cidx_of, ncmap);
}

int rqv_hlsl_ex(const rqv_prog_t *p, int lerp, const rqv_raw_t *raw, char *buf, size_t bufn,
                unsigned char cidx_of[RQV_NCONST], unsigned *ncmap)
{
    rqs_t s;
    unsigned char cmap[RQV_NCONST];
    unsigned nc = 0, k, nv = 0;
    int rel = p->rel ? 1 : 0, i;
    s.p = buf; s.n = bufn; s.o = 0; s.ovf = 0;
    memset(cmap, 0xFF, sizeof cmap);
    if (!rel) {
        for (i = 0; i < p->nops; i++) {
            const rqv_op_t *o = &p->ops[i];
            int used = 0, q;
            for (q = 0; q < 3; q++) { if (o->in[q].mux == 3) { used = 1; } }
            if (o->omask && !o->orb && o->oaddr < RQV_NCONST && cmap[o->oaddr] == 0xFF) {
                cidx_of[nc] = o->oaddr; cmap[o->oaddr] = (unsigned char)nc++;
            }
            if (used && cmap[o->cidx] == 0xFF) {
                cidx_of[nc] = (unsigned char)o->cidx; cmap[o->cidx] = (unsigned char)nc++;
            }
        }
        if (nc == 0) { cidx_of[0] = 0; nc = 1; }
    } else {
        for (k = 0; k < RQV_NCONST; k++) { cidx_of[k] = (unsigned char)k; }
        nc = RQV_NCONST;
    }
    *ncmap = nc;
    rqs(&s, "cbuffer CB : register(b0) { row_major float4x4 mvp; uint has_tex; };\n");
    if (lerp) { rqs(&s, "cbuffer IP : register(b1) { row_major float4x4 mvp0; float ip_t; };\n"); }
    if (raw) {
        rqs(&s, "cbuffer VC : register(b3) { float4 vc[%u]; float4 rq_fog; float4 rq_misc; uint4 rq_raw; uint4 rq_raw2;"
                " float4 rq_inl[%u]; };\n", nc, raw->ninl ? raw->ninl : 1u);
        rqs(&s, "Buffer<uint> rqx : register(t0);\n"
                "uint rq_ld(uint a) { uint w = a >> 2; uint sh = (a & 3u) << 3; uint lo = rqx.Load(w);\n"
                "  if (sh == 0u) return lo; return (lo >> sh) | (rqx.Load(w + 1u) << (32u - sh)); }\n"
                "float rq_s16(uint a) { return (float)((int)(rq_ld(a) << 16) >> 16); }\n"
                "uint rq_src(uint id) { uint pr = rq_raw.z; uint t = id / 3u; uint k = id - t * 3u;\n"
                "  if (pr == 6u) return t + k;\n"
                "  if (pr == 7u || pr == 10u) return k == 0u ? 0u : t + k;\n"
                "  if (pr == 8u || pr == 9u) { uint q = id / 6u; uint j = id - q * 6u;\n"
                "    uint m = (j == 0u || j == 3u) ? 0u : (j == 1u ? 1u : ((j == 2u || j == 4u) ? 2u : 3u));\n"
                "    return (pr == 8u ? 4u : 2u) * q + m; }\n"
                "  if (pr == 3u) return id >= rq_raw.w ? 0u : id;\n"
                "  return id; }\n");
    } else {
        rqs(&s, "cbuffer VC : register(b3) { float4 vc[%u]; float4 rq_fog; float4 rq_misc; };\n", nc);
    }
    rqs(&s, "struct VSIN { float4 pos : POSITION0; float4 col : COLOR0; float2 uv : TEXCOORD0;\n");
    if (lerp) { rqs(&s, "  float4 pos0 : POSITION1;\n"); }
    if (raw) {
        rqs(&s, "  uint vid : SV_VertexID;\n");
    } else {
        for (k = 0; k < 16u; k++) {
            if (p->vinmask & (1u << k)) { rqs(&s, "  float4 a%u : ATTR%u;\n", k, nv++); }
        }
    }
    rqs(&s, "};\n"
            "struct VSOUT { float4 pos : SV_Position; float4 col : COLOR0; float2 uv : TEXCOORD0;\n"
            "  float4 spec : COLOR1; float4 t0 : TEXCOORD1; float4 t1 : TEXCOORD2;\n"
            "  float4 t2 : TEXCOORD3; float4 t3 : TEXCOORD4; float fog : TEXCOORD5; };\n"
            "float rq_rcp(float x) { return x == 1.0 ? 1.0 : 1.0 / x; }\n"
            "float rq_rcc(float x) { if (x == 1.0) return 1.0; float r = 1.0 / x;\n"
            "  return r >= 0.0 ? clamp(r, 5.42101e-020, 1.884467e+019) : clamp(r, -1.884467e+019, -5.42101e-020); }\n"
            "float rq_rsq(float x) { x = abs(x); return x == 1.0 ? 1.0 : rsqrt(x); }\n"
            "float4 rq_log(float x) { x = abs(x); if (x == 0.0) return float4(-3.0e38, 1.0, -3.0e38, 1.0);\n"
            "  float e = floor(log2(x)); float mt = x / exp2(e);\n"
            "  if (mt >= 2.0) { e += 1.0; mt *= 0.5; } if (mt < 1.0) { e -= 1.0; mt *= 2.0; }\n"
            "  return float4(e, mt, log2(x), 1.0); }\n"
            "float4 rq_lit(float4 s) { float pw = clamp(s.w, -127.9961, 127.9961); float4 r = float4(1, 0, 0, 1);\n"
            "  if (s.x > 0.0) { r.y = s.x; if (s.y > 0.0) r.z = pow(s.y, pw); } return r; }\n"
            "VSOUT main(VSIN i) { VSOUT o;\n");
    if (lerp) {
        rqs(&s, "  float4 c0 = mul(i.pos0, mvp0), c1 = mul(i.pos, mvp);\n"
                "  o.pos = c0 + ip_t * (c1 - c0);\n");
        if (lerp == 2) { /* FIX(W224 LINE-FLICKER): w-Wechsel N-1/N -> Tick N */
            rqs(&s, "  if (c0.w * c1.w <= 0.0) { o.pos = c1; }\n");
        }
    } else {
        rqs(&s, "  o.pos = mul(i.pos, mvp);\n");
    }
    if (raw) {
        rqs(&s, "  uint si = rq_src(i.vid);\n"
                "  uint vi = rq_raw2.x != 0xFFFFFFFFu ? rqx.Load(rq_raw2.x + si) : si;\n"
                "  uint va = rq_raw.x + vi * rq_raw.y;\n");
        for (k = 0; k < 16u; k++) {
            if (p->vinmask & (1u << k)) { rqv_rawfetch(&s, k, raw); }
        }
    } else {
        for (k = 0; k < 16u; k++) {
            if (p->vinmask & (1u << k)) { rqs(&s, "  float4 v%u = i.a%u;\n", k, k); }
        }
    }
    rqs(&s, "  float4 R0 = 0, R1 = 0, R2 = 0, R3 = 0, R4 = 0, R5 = 0, R6 = 0, R7 = 0, R8 = 0, R9 = 0, R10 = 0, R11 = 0;\n"
            "  float4 oPos = float4(0,0,0,1), oD0 = float4(0,0,0,1), oD1 = float4(0,0,0,1), oFog = float4(0,0,0,1);\n"
            "  float4 oPts = float4(0,0,0,1), oB0 = float4(0,0,0,1), oB1 = float4(0,0,0,1);\n"
            "  float4 oT0 = float4(0,0,0,1), oT1 = float4(0,0,0,1), oT2 = float4(0,0,0,1), oT3 = float4(0,0,0,1);\n"
            "  float4 c[%u]; [unroll] for (int ci = 0; ci < %u; ci++) { c[ci] = vc[ci]; }\n"
            "  int a0 = 0;\n", nc, nc);
    for (i = 0; i < p->nops; i++) {
        const rqv_op_t *o = &p->ops[i];
        char mb[6];
        int hm = o->mac != 0, hl = o->ilu != 0, ma = (o->mac == 13);
        rqs(&s, "  { float4 A = 0, B = 0, C = 0, m = 0, l = 0;\n");
        if (hm) {
            rqs(&s, "    A = "); rqv_src(&s, &o->in[0], o, cmap, rel); rqs(&s, ";\n");
            if (o->mac == 2 || (o->mac >= 4 && o->mac <= 12)) {
                rqs(&s, "    B = "); rqv_src(&s, &o->in[1], o, cmap, rel); rqs(&s, ";\n");
            }
        }
        if ((hm && (o->mac == 3 || o->mac == 4)) || hl) {
            rqs(&s, "    C = "); rqv_src(&s, &o->in[2], o, cmap, rel); rqs(&s, ";\n");
        }
        if (hm) {
            static const char *mx[14] = { "0", "A", "A * B", "A + C", "A * B + C",
                "dot(A.xyz, B.xyz).xxxx", "(dot(A.xyz, B.xyz) + B.w).xxxx", "dot(A, B).xxxx",
                "float4(1.0, A.y * B.y, A.z, B.w)", "min(A, B)", "max(A, B)",
                "float4(A < B)", "float4(A >= B)", "floor(A.x + 0.001).xxxx" };
            rqs(&s, "    m = %s;\n", mx[o->mac]);
        }
        if (hl) {
            static const char *lx[8] = { "0", "C", "rq_rcp(C.x).xxxx", "rq_rcc(C.x).xxxx",
                "rq_rsq(C.x).xxxx", "float4(exp2(floor(C.x)), C.x - floor(C.x), exp2(C.x), 1.0)",
                "rq_log(C.x)", "rq_lit(C)" };
            rqs(&s, "    l = %s;\n", lx[o->ilu]);
        }
        if (hm && !ma && o->macmask && o->outr < 12u) {
            rqs(&s, "    R%u%s = m%s;\n", (unsigned)o->outr, rqv_msk(o->macmask, mb), mb);
        }
        if (hl && o->ilumask) {
            unsigned d = hm ? 1u : o->outr;
            if (d < 12u) { rqs(&s, "    R%u%s = l%s;\n", d, rqv_msk(o->ilumask, mb), mb); }
        }
        if (o->omask && (o->omux ? hl : hm) && !(o->omux == 0 && ma)) {
            const char *src = o->omux ? "l" : "m";
            if (o->orb) {
                static const char *on[13] = { "oPos", 0, 0, "oD0", "oD1", "oFog", "oPts", "oB0", "oB1",
                                              "oT0", "oT1", "oT2", "oT3" };
                unsigned a = o->oaddr & 0xFu;
                if (a <= 12u && on[a]) {
                    rqs(&s, "    %s%s = %s%s;\n", on[a], rqv_msk(o->omask, mb), src, mb);
                }
            } else if (o->oaddr < RQV_NCONST) {
                unsigned ci = rel ? o->oaddr : cmap[o->oaddr];
                rqs(&s, "    c[%u]%s = %s%s;\n", ci, rqv_msk(o->omask, mb), src, mb);
            }
        }
        if (ma) { rqs(&s, "    a0 = (int)m.x;\n"); }
        rqs(&s, "  }\n");
    }
    rqs(&s, "  o.col = rq_misc.x != 0.0 ? saturate(oD0) : i.col;\n"
            "  o.uv = i.uv;\n"
            "  o.spec = rq_misc.z != 0.0 ? saturate(oD1) : float4(0, 0, 0, 0);\n"
            "  o.t0 = rq_misc.y != 0.0 ? oT0 : float4(i.uv, 0.0, 1.0);\n"
            "  o.t1 = oT1; o.t2 = oT2; o.t3 = oT3;\n"
            "  float fd = oFog.x, ff = 1.0;\n"
            "  if (rq_fog.x == 1.0) ff = rq_fog.y + fd * rq_fog.z - 1.0;\n"
            "  else if (rq_fog.x == 2.0) ff = rq_fog.y + exp2(fd * rq_fog.z * 16.0) - 1.5;\n"
            "  else if (rq_fog.x == 3.0) ff = rq_fog.y + exp2(-fd * fd * rq_fog.z * rq_fog.z * 32.0) - 1.5;\n"
            "  o.fog = saturate(ff);\n"
            "  return o; }\n");
    return s.ovf ? 0 : 1;
}
