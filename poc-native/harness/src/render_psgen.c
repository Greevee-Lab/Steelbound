/* render_psgen.c - RENDER-QA (W77): NV2A-Register-Combiner -> HLSL-Pixelshader.
 * Rein (keine D3D-Abhaengigkeit). Eingang: D3DPIXELSHADERDEF (60 DWORDs, Xbox-
 * D3D8: PSAlphaInputs[8] 0x00, PSFinalCombinerInputsABCD 0x20, ...EFG 0x24,
 * PSConstant0[8] 0x28, PSConstant1[8] 0x48, PSAlphaOutputs[8] 0x68,
 * PSRGBInputs[8] 0x88, PSCompareMode 0xA8, PSFinalCombinerConstant0/1 0xAC/
 * 0xB0, PSRGBOutputs[8] 0xB4, PSCombinerCount 0xD4, PSTextureModes 0xD8,
 * PSDotMapping 0xDC, PSInputTexture 0xE0, PSC0Mapping 0xE4, PSC1Mapping
 * 0xE8, PSFinalCombinerConstants 0xEC).
 * Semantik (NV2A-Register-Combiner, wie NV_register_combiners + Xbox-
 * Erweiterungen; Wissen aus Khronos-Spezifikation NV_register_combiners):
 *  Register: 0 ZERO, 1 C0, 2 C1, 3 FOG (rgb = FogColor, a = Nebelfaktor),
 *   4 V0 (oD0), 5 V1 (oD1), 8-11 T0-T3, 12 R0, 13 R1, 14 V1R0_SUM, 15 EF_PROD
 *   (14/15 nur Final-Combiner). R0 = R1 = (0,0,0,1), R0.a = T0.a bzw.
 *   R1.a = T1.a bei aktiver Stufe.
 *  Eingang (8 Bit): Register 0-3, Kanal 0x10 (RGB-Teil: .aaa statt .rgb;
 *   Alpha-Teil: .a statt .b), Abbildung 0xE0: 00 max(0,x), 20 1-sat(x),
 *   40 2*max(0,x)-1, 60 1-2*max(0,x), 80 max(0,x)-0.5, A0 0.5-max(0,x),
 *   C0 x, E0 -x.
 *  Stufe i: AB = A*B (oder A.B, Bit 13), CD = C*D (oder C.D, Bit 12), SUM =
 *   AB+CD bzw. MUX (Bit 14: R0.a >= 0.5 bzw. LSB -> CD sonst AB); Ausgabe-
 *   abbildung (Bits 15-17): x, x-0.5, 2x, 2(x-0.5), 4x, 4(x-0.5), x/2,
 *   (x-0.5)/2; Ergebnisse auf [-1,1] geklemmt; Register-Nummern CD 0-3,
 *   AB 4-7, SUM 8-11; RGB-Bits 18/19 = CD/AB blau -> Alpha. Alle Eingaenge
 *   lesen den Stand vor der Stufe.
 *  Final: rgb = A*B + (1-A)*C + D, a = G; EF_PROD = E*F, V1R0_SUM = V1 + R0
 *   (Flags 0x20/0x40 Komplement, 0x80 Klemmen); Final-Eingaenge nur
 *   vorzeichenlos (sat bzw. 1-sat). ABCD = EFG = 0 -> Ausgabe R0.
 *  Konstanten: C0/C1 je Stufe (PSCombinerCount Bit 8/12 = je Stufe eigene,
 *   sonst Stufe 0), im Final-Combiner FC0/FC1.
 *  Texturmodi (5 Bit je Stufe): 0 NONE, 1 PROJECT2D, 2 PROJECT3D (als 2D),
 *   4 PASSTHRU, 5 CLIPPLANE; andere -> nicht unterstuetzt (alter Pfad). */
#include "render_psgen.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct { char *p; size_t n, o; int ovf; } rps_t;

static void rps(rps_t *s, const char *fmt, ...)
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

static const char *kReg[16] = { "ZR", "C0", "C1", "FOG", "V0", "V1", "ZR", "ZR",
                                "T0", "T1", "T2", "T3", "R0", "R1", "SUM", "PROD" };

/* Eingangsausdruck. rgb = 1: float3, sonst float. fin = Final-Combiner. */
static void rp_in(rps_t *s, unsigned code, int rgb, int fin)
{
    unsigned reg = code & 0xFu, alpha = code & 0x10u, map = code & 0xE0u;
    char b[24];
    if (rgb) { _snprintf_s(b, sizeof b, _TRUNCATE, "%s.%s", kReg[reg], alpha ? "aaa" : "rgb"); }
    else { _snprintf_s(b, sizeof b, _TRUNCATE, "%s.%s", kReg[reg], alpha ? "a" : "b"); }
    if (fin) {
        if (map == 0x20u) { rps(s, "(1.0 - saturate(%s))", b); }
        else { rps(s, "saturate(%s)", b); }
        return;
    }
    switch (map) {
    case 0x00u: rps(s, "max(%s, 0.0)", b); break;
    case 0x20u: rps(s, "(1.0 - saturate(%s))", b); break;
    case 0x40u: rps(s, "(2.0 * max(%s, 0.0) - 1.0)", b); break;
    case 0x60u: rps(s, "(1.0 - 2.0 * max(%s, 0.0))", b); break;
    case 0x80u: rps(s, "(max(%s, 0.0) - 0.5)", b); break;
    case 0xA0u: rps(s, "(0.5 - max(%s, 0.0))", b); break;
    case 0xC0u: rps(s, "(%s)", b); break;
    default: rps(s, "(-%s)", b); break;
    }
}

static const char *kOutMap[8] = { "(%s)", "(%s - 0.5)", "(%s * 2.0)", "((%s - 0.5) * 2.0)",
                                  "(%s * 4.0)", "((%s - 0.5) * 4.0)", "(%s * 0.5)",
                                  "((%s - 0.5) * 0.5)" };

static void rp_outmap(rps_t *s, unsigned map, const char *v)
{
    char b[64];
    _snprintf_s(b, sizeof b, _TRUNCATE, kOutMap[map & 7u], v);
    rps(s, "clamp(%s, -1.0, 1.0)", b);
}

int rqp_hlsl(const unsigned *d, unsigned stage_tex, char *buf, size_t bufn, rqp_info_t *info)
{
    rps_t s;
    unsigned nst, i, tm[4], cnt;
    int uniq0, uniq1, mux_msb;
    s.p = buf; s.n = bufn; s.o = 0; s.ovf = 0;
    memset(info, 0, sizeof *info);
    cnt = d[53];
    nst = cnt & 0xFFu;
    if (nst > 8u) { return 0; }
    uniq0 = (cnt & 0x100u) != 0;
    uniq1 = (cnt & 0x1000u) != 0;
    mux_msb = (cnt & 0x10000u) != 0;
    for (i = 0; i < 4u; i++) {
        tm[i] = (d[54] >> (i * 5u)) & 0x1Fu;
        info->texmode[i] = tm[i];
        if (tm[i] != 0u && tm[i] != 1u && tm[i] != 2u && tm[i] != 4u && tm[i] != 5u) {
            info->unsupported = 1 + (int)i;
            return 0;
        }
    }
    info->nstages = nst;
    rps(&s, "cbuffer PC : register(b4) { float4 pc0[8]; float4 pc1[8]; float4 fc0; float4 fc1;\n"
            "  float4 fogc; float4 tsc[4]; float4 pflags; float4 sfx0; float4 sfx1; float4 sfx2; float4 sfx3; };\n"
            "Texture2D tx0 : register(t0); Texture2D tx1 : register(t1);\n"
            "Texture2D tx2 : register(t2); Texture2D tx3 : register(t3);\n"
            "SamplerState sm0 : register(s0); SamplerState sm1 : register(s1);\n"
            "SamplerState sm2 : register(s2); SamplerState sm3 : register(s3);\n"
            "struct PSIN { float4 pos : SV_Position; float4 col : COLOR0; float2 uv : TEXCOORD0;\n"
            "  float4 spec : COLOR1; float4 t0 : TEXCOORD1; float4 t1 : TEXCOORD2;\n"
            "  float4 t2 : TEXCOORD3; float4 t3 : TEXCOORD4; float fog : TEXCOORD5; };\n"
            /* RENDER-QA3 (W100): Schattenpuffer-Vergleich (NV2A-Tiefentextur):
             * r FUNC Tiefe (D3D-Reihenfolge, Funktion pflags.z Bits 8-10 wie
             * D3D11_COMPARISON - 1), 2x2-PCF wie der lineare Filter. */
            "float rq_cmp(float r, float d) { uint f = (asuint(pflags.z) >> 8) & 7u;\n"
            "  return f == 1u ? (float)(r < d) : f == 2u ? (float)(r == d) : f == 3u ? (float)(r <= d) :\n"
            "         f == 4u ? (float)(r > d) : f == 5u ? (float)(r != d) : f == 6u ? (float)(r >= d) : f == 7u ? 1.0 : 0.0; }\n"
            "float4 rq_shadow(Texture2D t, float2 uv, float r, float2 idim) {\n"
            "  float2 dim = 1.0 / idim; float2 p = uv * dim - 0.5; float2 f = frac(p); int2 b = (int2)floor(p);\n"
            "  int2 mx = (int2)dim - 1;\n"
            "  float s00 = rq_cmp(r, t.Load(int3(clamp(b, int2(0, 0), mx), 0)).r);\n"
            "  float s10 = rq_cmp(r, t.Load(int3(clamp(b + int2(1, 0), int2(0, 0), mx), 0)).r);\n"
            "  float s01 = rq_cmp(r, t.Load(int3(clamp(b + int2(0, 1), int2(0, 0), mx), 0)).r);\n"
            "  float s11 = rq_cmp(r, t.Load(int3(clamp(b + int2(1, 1), int2(0, 0), mx), 0)).r);\n"
            /* SHADER-OPT2 (W131) shadow_soft (sfx3.z > 0): 3x3-Gauss (1-2-1) gefaltet mit dem
             * bilinearen 2x2-PCF = 4x4 Vergleiche mit separablen Gewichten (Hypothese NV2A
             * MAG GAUSSIANCUBIC, weichere Kante). Sonst 2x2 wie bisher. */
            "  if (sfx3.z > 0.0) { float4 wx = float4((1.0 - f.x) * 0.25, (1.0 - f.x) * 0.5 + f.x * 0.25, (1.0 - f.x) * 0.25 + f.x * 0.5, f.x * 0.25);\n"
            "    float4 wy = float4((1.0 - f.y) * 0.25, (1.0 - f.y) * 0.5 + f.y * 0.25, (1.0 - f.y) * 0.25 + f.y * 0.5, f.y * 0.25);\n"
            "    float acc = 0.0; [unroll] for (int yy = 0; yy < 4; yy++) { [unroll] for (int xx = 0; xx < 4; xx++) {\n"
            "      acc += wx[xx] * wy[yy] * rq_cmp(r, t.Load(int3(clamp(b + int2(xx - 1, yy - 1), int2(0, 0), mx), 0)).r); } }\n"
            "    return acc.xxxx; }\n"
            "  return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y).xxxx; }\n"
            /* SHADE-FX (W121): Optionen beim Lesen der Welt-RTs (sfx0.x Bit 1+k):
             * kontrastadaptives Schaerfen (sfx0.z), Kontrast (sfx1.x um sfx1.z),
             * Gamma (sfx1.y). Alle Bits 0 = Original (Zweig wird nie betreten). */
            /* SHADER-OPT2 (W131) aa_mode Bit 2 (sfx3.y > 0): FXAA beim Lesen der Welt-RT -
             * eigene Umsetzung des veroeffentlichten Verfahrens (Lottes 2009: Luma-Kontrast,
             * Kantenrichtung, Kantensuche, Subpixel-Anteil), kein uebernommener Code. */
            "float rq_lu(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
            "float3 rq_fxaa(Texture2D t, SamplerState s, float2 uv, float3 cm, float2 d) {\n"
            "  float lm = rq_lu(cm);\n"
            "  float ln = rq_lu(t.SampleLevel(s, uv + float2(0.0, -d.y), 0).rgb), ls = rq_lu(t.SampleLevel(s, uv + float2(0.0, d.y), 0).rgb);\n"
            "  float le = rq_lu(t.SampleLevel(s, uv + float2(d.x, 0.0), 0).rgb), lw = rq_lu(t.SampleLevel(s, uv + float2(-d.x, 0.0), 0).rgb);\n"
            "  float lmin = min(lm, min(min(ln, ls), min(le, lw))), lmax = max(lm, max(max(ln, ls), max(le, lw)));\n"
            "  float rng = lmax - lmin; if (rng < max(0.0312, lmax * 0.125)) { return cm; }\n"
            "  float lnw = rq_lu(t.SampleLevel(s, uv - d, 0).rgb), lse = rq_lu(t.SampleLevel(s, uv + d, 0).rgb);\n"
            "  float lne = rq_lu(t.SampleLevel(s, uv + float2(d.x, -d.y), 0).rgb), lsw = rq_lu(t.SampleLevel(s, uv + float2(-d.x, d.y), 0).rgb);\n"
            "  float eh = abs(0.25 * lnw - 0.5 * ln + 0.25 * lne) + abs(0.5 * lw - lm + 0.5 * le) + abs(0.25 * lsw - 0.5 * ls + 0.25 * lse);\n"
            "  float ev = abs(0.25 * lnw - 0.5 * lw + 0.25 * lsw) + abs(0.5 * ln - lm + 0.5 * ls) + abs(0.25 * lne - 0.5 * le + 0.25 * lse);\n"
            "  bool hz = eh >= ev;\n"
            "  float sub = saturate(abs((2.0 * (ln + ls + le + lw) + lnw + lne + lsw + lse) / 12.0 - lm) / rng);\n"
            "  sub = smoothstep(0.0, 1.0, sub); sub = sub * sub * 0.75;\n"
            "  float l1 = hz ? ln : lw, l2 = hz ? ls : le, g1 = abs(l1 - lm), g2 = abs(l2 - lm);\n"
            "  float st = hz ? d.y : d.x, ll, gr; if (g1 >= g2) { st = -st; ll = 0.5 * (l1 + lm); gr = g1; } else { ll = 0.5 * (l2 + lm); gr = g2; }\n"
            "  float2 e = uv; if (hz) { e.y += st * 0.5; } else { e.x += st * 0.5; }\n"
            "  float2 of = hz ? float2(d.x, 0.0) : float2(0.0, d.y); float2 p1 = e - of, p2 = e + of;\n"
            "  float e1 = rq_lu(t.SampleLevel(s, p1, 0).rgb) - ll, e2 = rq_lu(t.SampleLevel(s, p2, 0).rgb) - ll;\n"
            "  bool r1 = abs(e1) >= gr * 0.25, r2 = abs(e2) >= gr * 0.25;\n"
            "  static const float qs[8] = { 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 4.0, 8.0 };\n"
            "  [unroll] for (int k = 1; k < 8; k++) {\n"
            "    if (!r1) { p1 -= of * qs[k]; e1 = rq_lu(t.SampleLevel(s, p1, 0).rgb) - ll; r1 = abs(e1) >= gr * 0.25; }\n"
            "    if (!r2) { p2 += of * qs[k]; e2 = rq_lu(t.SampleLevel(s, p2, 0).rgb) - ll; r2 = abs(e2) >= gr * 0.25; } }\n"
            "  float d1 = hz ? uv.x - p1.x : uv.y - p1.y, d2 = hz ? p2.x - uv.x : p2.y - uv.y;\n"
            "  bool dir1 = d1 < d2; float po = 0.5 - min(d1, d2) / max(d1 + d2, 1e-6);\n"
            "  bool ok = ((dir1 ? e1 : e2) < 0.0) != (lm < ll);\n"
            "  float fo = max(ok ? po : 0.0, sub); float2 fu = uv; if (hz) { fu.y += fo * st; } else { fu.x += fo * st; }\n"
            "  return t.SampleLevel(s, fu, 0).rgb; }\n"
            "float3 rq_sfx(Texture2D t, SamplerState s, float2 uv, float3 c) {\n"
            "  if (sfx3.y > 0.0) { uint fw, fh; t.GetDimensions(fw, fh); c = rq_fxaa(t, s, uv, c, float2(1.0 / (float)fw, 1.0 / (float)fh)); }\n"
            "  if (sfx0.z > 0.0) { uint w, h; t.GetDimensions(w, h); float2 d = float2(1.0 / (float)w, 1.0 / (float)h);\n"
            "    float3 n = t.Sample(s, uv - float2(0.0, d.y)).rgb, so = t.Sample(s, uv + float2(0.0, d.y)).rgb;\n"
            "    float3 e = t.Sample(s, uv + float2(d.x, 0.0)).rgb, wv = t.Sample(s, uv - float2(d.x, 0.0)).rgb;\n"
            "    float3 mn = min(c, min(min(n, so), min(e, wv))), mx = max(c, max(max(n, so), max(e, wv)));\n"
            "    float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1.0 / 1024.0)));\n"
            "    float3 wt = -amp * (sfx0.z * 0.2);\n"
            "    c = saturate((c + (n + so + e + wv) * wt) / (1.0 + 4.0 * wt)); }\n"
            "  if (sfx1.x != 1.0) { c = saturate((c - sfx1.z) * sfx1.x + sfx1.z); }\n"
            "  if (sfx1.y != 1.0) { c = pow(max(c, 0.0), sfx1.y); }\n"
            "  return c; }\n"
            "float4 main(PSIN i) : SV_Target {\n"
            "  float4 ZR = 0, C0 = 0, C1 = 0, SUM = 0, PROD = 0;\n"
            "  float4 V0 = saturate(i.col), V1 = saturate(i.spec);\n"
            "  float4 FOG = float4(fogc.rgb, saturate(i.fog));\n"
            "  if ((asuint(sfx0.x) & 1u) != 0u) { FOG.a = 1.0 - (1.0 - FOG.a) * sfx0.y; }\n" /* W121 Nebelstaerke */
            /* W121 Teil 2: Licht-Intensitaet = Lichteingaenge skalieren (Vertex-Licht V0,
             * Spekular V1, Umgebungslicht C0 je Stufe) VOR den Combiner-Klemmungen -
             * so kommen ueberstrahlte Details zurueck. Nebelfarbe bleibt. */
            "  if ((asuint(sfx0.x) & 0xA00u) != 0u) { V0.rgb = saturate(i.col.rgb * sfx2.z); V1.rgb = saturate(i.spec.rgb * sfx2.z); }\n"
            "  float4 T0 = 0, T1 = 0, T2 = 0, T3 = 0;\n");
    for (i = 0; i < 4u; i++) {
        static const char *tn[4] = { "tx0", "tx1", "tx2", "tx3" };
        static const char *sn[4] = { "sm0", "sm1", "sm2", "sm3" };
        static const char *cn[4] = { "i.t0", "i.t1", "i.t2", "i.t3" };
        switch (tm[i]) {
        case 1u: case 2u:
            /* tsc[i].xy = Texel->UV-Skala (Linear-Textur) bzw. 1, z = Textur da */
            /* RENDER-QA3 (W100): Xbox D3DTSS_ALPHAKILL je Stufe (pflags.z Bit i,
             * NV2A TEXTURE_CONTROL0.ALPHAKILLEN): Texel-Alpha 0 -> verwerfen. */
            /* RENDER-QA3 (W100): tsc[i].w != 0 = Schattenpuffer (Tiefe als SRV,
             * Vergleich mit r = z/w * tsc[i].w, s. rq_shadow). */
            rps(&s, "  if (tsc[%u].z != 0.0) { float iw%u = %s.w != 0.0 ? 1.0 / %s.w : 1.0; float2 uv%u = %s.xy * iw%u * tsc[%u].xy;\n"
                    "    if (tsc[%u].w != 0.0) { T%u = rq_shadow(%s, uv%u, %s.z * iw%u * tsc[%u].w, tsc[%u].xy); } else {\n"
                    "    T%u = %s.Sample(%s, uv%u);\n"
                    "    if ((asuint(sfx0.x) & %uu) != 0u) { T%u.rgb = rq_sfx(%s, %s, uv%u, T%u.rgb); }\n"
                    "    if ((asuint(sfx0.x) & %uu) != 0u) { T%u.rgb = max(T%u.rgb - sfx1.w, 0.0) / (1.0 - sfx1.w) * sfx0.w; }\n"
                    "    if ((asuint(sfx0.x) & %uu) != 0u) { T%u.a = 1.0 - (1.0 - T%u.a) * sfx3.x; }\n"
                    "    if ((asuint(pflags.z) & %uu) != 0u && T%u.a < (0.5 / 255.0)) { discard; } }\n"
                    "  } else { T%u = float4(1, 1, 1, 1); }\n",
                i, i, cn[i], cn[i], i, cn[i], i, i,
                i, i, tn[i], i, cn[i], i, i, i,
                i, tn[i], sn[i], i,
                2u << i, i, tn[i], sn[i], i, i,
                0x20u << i, i, i,
                0x1000u << i, i, i,
                1u << i, i, i);
            info->sample_mask |= 1u << i;
            break;
        case 4u:
            rps(&s, "  T%u = saturate(%s);\n", i, cn[i]);
            break;
        case 5u: {
            unsigned cm = (d[42] >> (i * 4u)) & 0xFu, k;
            static const char cc[4] = { 'x', 'y', 'z', 'w' };
            for (k = 0; k < 4u; k++) {
                if (cm & (1u << k)) { rps(&s, "  if (%s.%c >= 0.0) discard;\n", cn[i], cc[k]); }
                else { rps(&s, "  if (%s.%c < 0.0) discard;\n", cn[i], cc[k]); }
            }
            break;
        }
        default:
            break;
        }
    }
    (void)stage_tex;
    rps(&s, "  float4 R0 = float4(0, 0, 0, 1), R1 = float4(0, 0, 0, 1);\n");
    if (tm[0]) { rps(&s, "  R0.a = T0.a;\n"); }
    if (tm[1]) { rps(&s, "  R1.a = T1.a;\n"); }
    for (i = 0; i < nst; i++) {
        unsigned rin = d[34 + i], ain = d[0 + i], rout = d[45 + i], aout = d[26 + i];
        unsigned rcd = rout & 0xFu, rab = (rout >> 4) & 0xFu, rsum = (rout >> 8) & 0xFu;
        unsigned acd = aout & 0xFu, aab = (aout >> 4) & 0xFu, asum = (aout >> 8) & 0xFu;
        int rcdd = (rout >> 12) & 1u, rabd = (rout >> 13) & 1u, rmux = (rout >> 14) & 1u;
        unsigned rmap = (rout >> 15) & 7u, amap = (aout >> 15) & 7u;
        int amux = (aout >> 14) & 1u;
        int cdb2a = (rout >> 18) & 1u, abb2a = (rout >> 19) & 1u;
        unsigned c0i = uniq0 ? i : 0u, c1i = uniq1 ? i : 0u;
        { /* Konstanten als Faktor (Partner im Produkt != 1 = 0x20/0x30)? */
            unsigned pr[4][2], q;
            pr[0][0] = (rin >> 24) & 0xFFu; pr[0][1] = (rin >> 16) & 0xFFu;
            pr[1][0] = (rin >> 8) & 0xFFu;  pr[1][1] = rin & 0xFFu;
            pr[2][0] = (ain >> 24) & 0xFFu; pr[2][1] = (ain >> 16) & 0xFFu;
            pr[3][0] = (ain >> 8) & 0xFFu;  pr[3][1] = ain & 0xFFu;
            for (q = 0; q < 4u; q++) {
                unsigned x, y;
                for (x = 0; x < 2u; x++) {
                    unsigned r = pr[q][x] & 0xFu, o = pr[q][1u - x];
                    int one = ((o & 0xEFu) == 0x20u); /* 1 - sat(ZERO) */
                    y = r == 1u ? 1u : (r == 2u ? 2u : 0u);
                    if (y && !one && (o & 0xFu) != 0u) { info->cmul[i] |= y; }
                }
            }
        }
        rps(&s, "  { C0 = pc0[%u]; C1 = pc1[%u];\n", c0i, c1i);
        rps(&s, "    if ((asuint(sfx0.x) & 0xA00u) != 0u) { C0.rgb *= sfx2.z; }\n"); /* W121/2 Umgebungslicht */
        /* RGB */
        rps(&s, "    float3 a = "); rp_in(&s, (rin >> 24) & 0xFFu, 1, 0);
        rps(&s, ";\n    float3 b = "); rp_in(&s, (rin >> 16) & 0xFFu, 1, 0);
        rps(&s, ";\n    float3 c = "); rp_in(&s, (rin >> 8) & 0xFFu, 1, 0);
        rps(&s, ";\n    float3 d = "); rp_in(&s, rin & 0xFFu, 1, 0);
        rps(&s, ";\n    float3 ab = %s;\n    float3 cd = %s;\n",
            rabd ? "dot(a, b).xxx" : "a * b", rcdd ? "dot(c, d).xxx" : "c * d");
        if (rmux) {
            rps(&s, "    float3 sm = %s ? cd : ab;\n",
                mux_msb ? "(R0.a >= 0.5)" : "((uint)(saturate(R0.a) * 255.0 + 0.5) & 1u) != 0u");
        } else {
            rps(&s, "    float3 sm = ab + cd;\n");
        }
        rps(&s, "    float3 oab = "); rp_outmap(&s, rmap, "ab");
        rps(&s, ";\n    float3 ocd = "); rp_outmap(&s, rmap, "cd");
        rps(&s, ";\n    float3 osm = "); rp_outmap(&s, rmap, "sm");
        rps(&s, ";\n");
        /* Alpha */
        rps(&s, "    float aa = "); rp_in(&s, (ain >> 24) & 0xFFu, 0, 0);
        rps(&s, ";\n    float ab_ = "); rp_in(&s, (ain >> 16) & 0xFFu, 0, 0);
        rps(&s, ";\n    float ac = "); rp_in(&s, (ain >> 8) & 0xFFu, 0, 0);
        rps(&s, ";\n    float ad = "); rp_in(&s, ain & 0xFFu, 0, 0);
        rps(&s, ";\n    float aab = aa * ab_;\n    float acd = ac * ad;\n");
        if (amux) {
            rps(&s, "    float asm_ = %s ? acd : aab;\n",
                mux_msb ? "(R0.a >= 0.5)" : "((uint)(saturate(R0.a) * 255.0 + 0.5) & 1u) != 0u");
        } else {
            rps(&s, "    float asm_ = aab + acd;\n");
        }
        rps(&s, "    float oaab = "); rp_outmap(&s, amap, "aab");
        rps(&s, ";\n    float oacd = "); rp_outmap(&s, amap, "acd");
        rps(&s, ";\n    float oasm = "); rp_outmap(&s, amap, "asm_");
        rps(&s, ";\n");
        /* Schreiben (Register 0 = verwerfen; 14/15 in Stufen ungueltig) */
        if (rab && rab < 14u) { rps(&s, "    %s.rgb = oab;\n", kReg[rab]); }
        if (rcd && rcd < 14u) { rps(&s, "    %s.rgb = ocd;\n", kReg[rcd]); }
        if (rsum && rsum < 14u) { rps(&s, "    %s.rgb = osm;\n", kReg[rsum]); }
        if (aab && aab < 14u) { rps(&s, "    %s.a = oaab;\n", kReg[aab]); }
        if (acd && acd < 14u) { rps(&s, "    %s.a = oacd;\n", kReg[acd]); }
        if (asum && asum < 14u) { rps(&s, "    %s.a = oasm;\n", kReg[asum]); }
        if (abb2a && rab && rab < 14u) { rps(&s, "    %s.a = oab.b;\n", kReg[rab]); }
        if (cdb2a && rcd && rcd < 14u) { rps(&s, "    %s.a = ocd.b;\n", kReg[rcd]); }
        rps(&s, "  }\n");
    }
    rps(&s, "  float4 R0c = R0, R1c = R1;\n"); /* DIAG(W100): Stand vor dem Final-Combiner */
    /* Final-Combiner */
    if (d[8] == 0u && d[9] == 0u) {
        rps(&s, "  float4 o = R0;\n");
        info->final_default = 1;
    } else {
        unsigned abcd = d[8], efg = d[9], fl = efg & 0xFFu;
        rps(&s, "  C0 = fc0; C1 = fc1;\n");
        rps(&s, "  { float3 sv1 = %s; float3 sr0 = %s;\n",
            (fl & 0x40u) ? "1.0 - saturate(V1.rgb)" : "saturate(V1.rgb)",
            (fl & 0x20u) ? "1.0 - saturate(R0.rgb)" : "saturate(R0.rgb)");
        rps(&s, "    SUM = float4(%s, 0.0); }\n", (fl & 0x80u) ? "saturate(sv1 + sr0)" : "(sv1 + sr0)");
        rps(&s, "  PROD = float4("); rp_in(&s, (efg >> 24) & 0xFFu, 1, 1);
        rps(&s, " * "); rp_in(&s, (efg >> 16) & 0xFFu, 1, 1);
        rps(&s, ", 0.0);\n");
        rps(&s, "  float3 fa = "); rp_in(&s, (abcd >> 24) & 0xFFu, 1, 1);
        rps(&s, ";\n  float3 fb = "); rp_in(&s, (abcd >> 16) & 0xFFu, 1, 1);
        rps(&s, ";\n  float3 fcc = "); rp_in(&s, (abcd >> 8) & 0xFFu, 1, 1);
        rps(&s, ";\n  float3 fd = "); rp_in(&s, abcd & 0xFFu, 1, 1);
        rps(&s, ";\n  float fg = "); rp_in(&s, (efg >> 8) & 0xFFu, 0, 1);
        rps(&s, ";\n  float4 o = float4(fa * fb + (1.0 - fa) * fcc + fd, fg);\n");
        info->uses_fog = ((abcd >> 24) & 0xF) == 3u || ((abcd >> 16) & 0xF) == 3u ||
                         ((abcd >> 8) & 0xF) == 3u || (abcd & 0xF) == 3u;
    }
    /* RENDER-QA3 (W100): NV2A-Alpha-Test auf die Final-Ausgabe (pflags.x =
     * GL-Funktion - 0x1FF, 0 = aus; pflags.y = Ref 8 Bit; Vergleich in 8 Bit
     * wie der alte Pfad W90) und DIAG-Ausgabe einzelner Terme (pflags.w). */
    /* W121 Teil 2: Aussenwelt-Ausgang VOR der Klemmung: Belichtung, Tonemapping
     * (1 Reinhard mit Weisspunkt 2, 2 ACES-aehnliche Kurvenanpassung nach
     * Narkowicz 2015 - eigene Formel-Umsetzung), Gamma (nur Welt im BB).
     * Alpha (= Nebelfaktor) bleibt. Alle Bits 0 = Original. */
    rps(&s, "  if ((asuint(sfx0.x) & 0x200u) != 0u) { float3 x = max(o.rgb, 0.0) * sfx2.x;\n"
            "    if (sfx2.y == 1.0) { x = x * (1.0 + x * 0.25) / (1.0 + x); }\n"
            "    else if (sfx2.y == 2.0) { x *= 0.6; x = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14); }\n"
            "    if ((asuint(sfx0.x) & 0x400u) != 0u) { x = pow(saturate(x), sfx2.w); }\n"
            "    o.rgb = x; }\n");
    rps(&s, "  float4 res = saturate(o);\n"
            "  { uint af = asuint(pflags.x); if (af != 0u) { uint a8 = (uint)(res.a * 255.0 + 0.5); uint r8 = asuint(pflags.y);\n"
            "    bool ok = (af == 2u) ? (a8 < r8) : (af == 3u) ? (a8 == r8) : (af == 4u) ? (a8 <= r8) :\n"
            "              (af == 5u) ? (a8 > r8) : (af == 6u) ? (a8 != r8) : (af == 7u) ? (a8 >= r8) : (af == 8u);\n"
            "    if (!ok) { discard; } } }\n"
            "  { uint dm = asuint(pflags.w); if (dm != 0u) { float4 dv = res;\n"
            "    if (dm == 1u) dv = T0; else if (dm == 2u) dv = T1; else if (dm == 3u) dv = V0; else if (dm == 4u) dv = V1;\n"
            "    else if (dm == 5u) dv = pc0[0]; else if (dm == 6u) dv = R0c; else if (dm == 7u) dv = FOG.aaaa;\n"
            "    else if (dm == 8u) dv = float4(frac((i.t0.w != 0.0 ? i.t0.xy / i.t0.w : i.t0.xy) * tsc[0].xy), 0.0, 1.0);\n"
            "    else if (dm == 9u) dv = R1c;\n"
            "    res = float4(saturate(dv.rgb), 1.0); } }\n"
            "  return res;\n}\n");
    return s.ovf ? 0 : 1;
}
