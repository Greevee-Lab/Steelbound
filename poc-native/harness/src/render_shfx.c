/* render_shfx.c - W121 SHADE-FX: Shader-Optionen (Remaster, Standard = Original).
 *
 * Alle Optionen sind Abweichungen vom Original und stehen per Standard auf
 * "Original" (wirkungslos). Sie sind live umschaltbar (ESC-Overlay, Seite
 * "Shader") und werden wie die anderen Overlay-Werte in user.cfg gespeichert.
 *
 * 1. Aussenwelt (nur shading=1, wirkt in den uebersetzten Combinern):
 *    - world_fog      Nebelstaerke 0..100 % (100 = Original): FOG.a des
 *                     Final-Combiners -> 1 - (1 - FOG.a) * k.
 *    - world_contrast Kontrast 50..200 % (Drehpunkt 0,2 = mittlere Weltwelt-
 *                     Helligkeit), world_gamma 50..200 % (Exponent v/100),
 *    - world_sharpen  Schaerfe 0..100 %: kontrastadaptives Schaerfen (eigene
 *                     Umsetzung der bekannten Idee "Gewicht der Nachbarn nach
 *                     lokalem Min/Max", vgl. AMD FidelityFX CAS (MIT) - kein
 *                     uebernommener Code).
 *    Die drei letzten wirken beim LESEN der Welt-RTs (512^2 Hauptmonitor,
 *    256^2 FRONT VIEW: RTs mit >= 32 3D-Draws im laufenden/letzten Bild) durch
 *    Filter/Monitor-Komposition des Spiels - gleiche Wirkung wie eine
 *    Nachbearbeitung der Welt-RT, ohne Kopie. Selbst-Sampling (Hitzeflimmer-
 *    Sprites, W106) bleibt unberuehrt (sonst doppelt).
 *    - bloom_strength 0..200 % (100 = Original): Lesen der kleinen Bloom-RTs
 *                     (<= 32^2, 20^2/6^2) durch Draws in grosse Ziele (>= 128,
 *                     Monitor-Komposition: y^4 + T2) skaliert.
 *    Umsetzung: zwei PS-Konstanten sfx0/sfx1 hinter pflags (render_psgen.c,
 *    Fenster bleibt 32 Konstanten), Bits je Stufe; alles 0 = Original.
 *
 * 2. Cockpit-Licht (Nachbearbeitung des fertigen Bildes, nur Mission):
 *    - cockpit_glow   0..100 %: Leuchten heller Lampen-/LED-/Anzeigepixel
 *                     (Hellpass auf max(R,G,B), 1/4-Aufloesung, Gauss).
 *    - cockpit_spill  0..100 %: weites Licht-Abstrahlen derselben Quellen auf
 *                     die Umgebung (1/16-Aufloesung, breiter Gauss).
 *    - cockpit_light  30..100 % (100 = Original): Helligkeit unbeleuchteter
 *                     Flaechen; helle (leuchtende) Pixel bleiben (mehr Kontrast).
 *    Aufrufstellen wie EDGE-MODE (vor der Randmaske): ohne FPS-INTERP im
 *    Present vor dem Screenshot, mit FPS-INTERP im Swapchain-BB nach der Kopie
 *    -> Aufzeichnung/interp_verify sehen die Nachbearbeitung nie.
 *
 * DIAG/TEST: shfx_ab=1 -> Screenshot-Bild mit Original-Werten, Partnerbild
 * (+1) mit den eingestellten Optionen; zusaetzlich <shot>_fx = dasselbe
 * Bild mit Cockpit-Nachbearbeitung (gleiches Bild, Kopie). shfx_ab=2 wie 1,
 * je Paar die naechste Option aus kSfxSets (Log "SHFX ab #k"). shot_mission=1 ->
 * screenshot_frame zaehlt ab Beginn der Kampagnen-Mission (Modus 4). */
#include "render_int.h"
#include <math.h> /* W465: expf, ldexp */
#include "sb_profile.h" /* W229 SBA() */
#include "render_qa.h"
#include "sb_tex.h"
#include "sb_gfxpreset.h" /* W173 LAUNCHER-P4: einheitliches Grafik-Preset */
#include "sb_remaster.h" /* W173: sb_rem_fx_locked (Effect-Pack-Ausnahme) */
#include "sb_state.h" /* W232: sb_in_mission */
#include "render_sblook.h" /* W459: SB look (LoC) */
#include "loc_render.h" /* W465: loc_render_post_scene */

int rq_ab_base(void); /* render_shade.c */

#define SFX_W3D_MIN 32u

/* ---- Welt-RT-Erkennung ---- */
static unsigned g_sfx_w3d_f[RTT_MAX], g_sfx_w3d_n[RTT_MAX], g_sfx_world_f[RTT_MAX];
static unsigned g_sfx_nflag[5];

static int sfx_pct(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

int g_shfx_bypass; /* Overlay "Vergleich (B)": 1 = Optionen voruebergehend aus (Original) */

/* A-Bild eines shfx_ab-Paares bzw. Overlay-Vergleich: Original-Werte */
static int sfx_neutral(void)
{
    return g_shfx_bypass || (g_cfg.shfx_ab && rq_ab_base());
}

/* W459 LOC-FX-LIGHT: wirksame Bloom-Staerke. SB look (Experimental, nur LoC, Bit SBLOOK_BLOOM_BOOST) legt den
 * SB-Bloom-Regler auf den spieleigenen LoC-Bloom: die LoC-Komposition (PS T0*T1.a + T1*(1-T1.a) + T2 + C0*T3)
 * liest das 14^2-Bloom-RT (Hauptbild) bzw. 9^2 (Lock-On-Kamera) in ein Ziel >= 128 - dieselbe Regel wie die
 * SB-Monitor-Komposition (render_sblook.h). Sonst bloom_strength (100 = Original). */
static int sfx_bloom_pct(void)
{
    if (g_sba_loc && (sblook_bits(g_cfg.loc_sblook, g_cfg.loc_sblook_mask) & SBLOOK_BLOOM_BOOST)) {
        return g_cfg.loc_sblook_bloom;
    }
    return g_cfg.bloom_strength;
}

int shfx_world_on(void)
{
    return g_cfg.world_fog != 100 || g_cfg.world_contrast != 100 || g_cfg.world_gamma != 100 ||
           g_cfg.world_sharpen > 0 || sfx_bloom_pct() != 100 || g_cfg.bloom_threshold > 0 ||
           g_cfg.world_exposure != 100 || g_cfg.tonemap != 0 || g_cfg.light_world != 100 ||
           g_cfg.light_cockpit != 100 || g_cfg.world_focus != 100 ||
           (g_cfg.aa_mode & 2) || (g_cfg.shadow_soft && g_cfg.shade_shadow) || /* W131 SHADER-OPT2 */
           (g_sba_loc && g_cfg.shade_shadow && g_cfg.shadow_sun > 0); /* W466 */
}

int shfx_cockpit_on(void)
{
    return g_cfg.cockpit_glow > 0 || g_cfg.cockpit_spill > 0 || g_cfg.cockpit_light < 100;
}

/* W131 SHADER-OPT2: Farbabstimmung des Endbilds aktiv? */
static int shfx_grade_on(void)
{
    return g_cfg.color_sat != 100 || g_cfg.color_temp != 0 || g_cfg.color_lift != 0 ||
           g_cfg.color_gamma != 100 || g_cfg.color_gain != 100;
}

/* W465 LOC-ENHANCED: Licht bei Muendungsfeuer, Treffern und Explosionen im LoC-Cockpit (fx_light_cockpit, in SB
 * macht das render_fx_rem.c aus den Explosionsdaten des Spiels). LoC: bildbasiert - Hellpass des Endbilds
 * (1/16), Mittelwert 1x1, gleitender Mittelwert ~0,6 s; Licht = Anstieg ueber den Mittelwert (Farbe des
 * Feuers), Abklingen ~0,2 s, aufs Cockpit (nicht auf leuchtende Pixel), am staerksten zum Fenster hin. Reine
 * Darstellung, keine Rueckwirkung auf das Spiel; ganz auf der GPU (keine Rueckleseverzoegerung). */
static int sfx_flash_on(void)
{
    return g_sba_loc && g_cfg.fx_light_cockpit > 0;
}

/* W131 SHADER-OPT2: irgendeine Nachbearbeitung des Endbilds (Mission) aktiv?
 * Cockpit-Licht (W121), Farbabstimmung, FXAA Endbild (aa_mode Bit 1), SSAO. */
int shfx_post_on(void)
{
    return shfx_cockpit_on() || shfx_grade_on() || (g_cfg.aa_mode & 1) || g_cfg.ssao > 0 || sfx_flash_on();
}

/* RT-Slot zu einer Texturadresse (wie sb_rtt_lookup, ohne Nebenwirkung) */
static int sfx_slot(unsigned xptr)
{
    unsigned data = 0;
    int i;
    if (!xptr) { return -1; }
    __try { data = *(volatile unsigned *)(uintptr_t)(xptr + 4); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    if (!data) { return -1; }
    for (i = 0; i < g_rtt_n; i++) {
        if (g_rtt[i].surf && g_rtt[i].data == data && !g_rtt[i].isdepth) { return i; }
    }
    return -1;
}

/* Shading-Pfad (rq_bind): 3D-Draw (kind 0, kein RHW) in ein RT */
void shfx_note_3d(int slot)
{
    if (slot < 0 || slot >= RTT_MAX) { return; }
    if (g_sfx_w3d_f[slot] != g_present_no) { g_sfx_w3d_f[slot] = g_present_no; g_sfx_w3d_n[slot] = 0; }
    if (++g_sfx_w3d_n[slot] == SFX_W3D_MIN) { g_sfx_world_f[slot] = g_present_no; }
}

static int sfx_is_world(int slot)
{
    return slot >= 0 && slot < RTT_MAX && g_sfx_world_f[slot] && g_present_no - g_sfx_world_f[slot] <= 2u;
}

/* rq_bind: sfx0/sfx1 (8 floats) fuer den aktuellen Shading-Draw.
 * srvk = Stufe k hat eine SRV, tgt = gebundener RT-Slot (-1 = BB). */
/* W121 Teil 2: Draw-Klassen fuer den Ausgabezweig (render_psgen.c):
 *   Aussenwelt = 3D-Draw (kind 0, kein RHW) mit FOGENABLE an und Nebel im
 *                Final-Combiner des Spiel-PS (Welt in RT31/32 und im BB),
 *   Cockpit    = 3D-Draw in den Backbuffer ohne Nebel, Stufe 0 kein RT
 *                (Monitor-Flaechen zeigen RT-Inhalte und bleiben unberuehrt).
 * Bits in sfx0.x: 0x200 Aussenwelt (Belichtung/Tonemapping/Licht), 0x400
 * Gamma am Ausgang (Aussenwelt direkt im BB; Welt-RTs bekommen Gamma beim
 * Lesen), 0x800 Cockpit-Licht. sfx2 = (Belichtung, Tonemapping, Licht, Gamma). */
/* W193 PAUSE-LIVE: gemeinsamer Kern. xp_in = Stufen-Texturadressen der Aufzeichnung (Pause-
 * Wiedergabe, ohne Zaehler) bzw. NULL = aktuelle Stufen (rq_bind, bisheriger Weg). */
static void sfx_consts_core(float *sx, const int srvk[4], const unsigned *xp_in, int tgt, int is3d, int fog_ps,
                            int st0rt, unsigned nstages);
void shfx_rq_consts(float *sx, const int srvk[4], int tgt, int is3d, int fog_ps, int st0rt, unsigned nstages)
{
    sfx_consts_core(sx, srvk, NULL, tgt, is3d, fog_ps, st0rt, nstages);
}

void shfx_rq_consts_rep(float *sx, const unsigned xp[4], int tgt, int is3d, int fog_ps, int st0rt, unsigned nstages)
{
    unsigned nf[5];
    int srvk[4], k;
    memcpy(nf, g_sfx_nflag, sizeof nf);
    for (k = 0; k < 4; k++) { srvk[k] = xp[k] != 0u; }
    sfx_consts_core(sx, srvk, xp, tgt, is3d, fog_ps, st0rt, nstages);
    memcpy(g_sfx_nflag, nf, sizeof nf); /* Statistik nur aus dem Spielpfad */
}

static void sfx_consts_core(float *sx, const int srvk[4], const unsigned *xp_in, int tgt, int is3d, int fog_ps,
                            int st0rt, unsigned nstages)
{
    unsigned flags = 0, xp[4], nn[4], k;
    memset(sx, 0, 16 * sizeof(float));
    sx[12] = 1.0f;
    if (!g_cfg.shading || sfx_neutral() || !shfx_world_on()) { return; }
    sx[8] = 1.0f; sx[9] = 0.0f; sx[10] = 1.0f; sx[11] = 1.0f;
    if (is3d && fog_ps && (g_cfg.world_exposure != 100 || g_cfg.tonemap != 0 || g_cfg.light_world != 100 ||
                           (tgt < 0 && g_cfg.world_gamma != 100))) {
        flags |= 0x200u;
        sx[8] = (float)sfx_pct(g_cfg.world_exposure, 25, 400) / 100.0f;
        sx[9] = (float)sfx_pct(g_cfg.tonemap, 0, 2);
        sx[10] = (float)sfx_pct(g_cfg.light_world, 0, 200) / 100.0f;
        if (tgt < 0 && g_cfg.world_gamma != 100) {
            flags |= 0x400u;
            sx[11] = (float)sfx_pct(g_cfg.world_gamma, 50, 200) / 100.0f;
        }
        g_sfx_nflag[3]++;
    } else if (is3d && !fog_ps && tgt < 0 && !st0rt && g_cfg.light_cockpit != 100) {
        flags |= 0x800u;
        sx[10] = (float)sfx_pct(g_cfg.light_cockpit, 0, 200) / 100.0f;
        g_sfx_nflag[4]++;
    }
    if (g_cfg.world_fog != 100) {
        flags |= 1u;
        sx[1] = (float)sfx_pct(g_cfg.world_fog, 0, 100) / 100.0f;
    }
    if (xp_in) { memcpy(xp, xp_in, sizeof xp); } else { sb_tex_stages(xp, nn); }
    for (k = 0; k < 4u; k++) {
        int s;
        if (!srvk[k]) { continue; }
        s = sfx_slot(xp[k]);
        if (s < 0 || s == tgt) { continue; } /* kein RT bzw. Selbst-Sampling */
        if ((g_cfg.world_contrast != 100 || g_cfg.world_gamma != 100 || g_cfg.world_sharpen > 0 || (g_cfg.aa_mode & 2)) &&
            sfx_is_world(s)) {
            flags |= 2u << k;
            g_sfx_nflag[0]++;
        } else if ((sfx_bloom_pct() != 100 || g_cfg.bloom_threshold > 0) && g_rtt[s].w <= 32u && g_rtt[s].h <= 32u &&
                   tgt >= 0 && tgt < g_rtt_n && g_rtt[tgt].w >= 128u) {
            flags |= 0x20u << k;
            g_sfx_nflag[1]++;
        } else if (g_cfg.world_focus != 100 && nstages >= 5u && g_rtt[s].w > 32u && g_rtt[s].w < 256u &&
                   tgt >= 0 && tgt < g_rtt_n && g_rtt[tgt].w >= 256u) {
            /* Fokus-Unschaerfe: kleine Unschaerfe-RT (200^2 bzw. 128^2, Maske im Alpha)
             * in der Monitor-Komposition (>= 5 Stufen) -> Maske Richtung 1 = scharf */
            flags |= 0x1000u << k;
            sx[12] = (float)sfx_pct(g_cfg.world_focus, 0, 100) / 100.0f;
            g_sfx_nflag[1]++;
        }
    }
    {
        union { unsigned u; float f; } fu;
        fu.u = flags;
        sx[0] = fu.f;
    }
    sx[2] = (float)sfx_pct(g_cfg.world_sharpen, 0, 100) / 100.0f;
    sx[3] = (float)sfx_pct(sfx_bloom_pct(), 0, 200) / 100.0f; /* W459: LoC SB look */
    sx[4] = (float)sfx_pct(g_cfg.world_contrast, 50, 200) / 100.0f;
    sx[5] = (float)sfx_pct(g_cfg.world_gamma, 50, 200) / 100.0f;
    sx[6] = 0.2f; /* Drehpunkt Kontrast */
    sx[7] = (float)sfx_pct(g_cfg.bloom_threshold, 0, 90) / 100.0f; /* Bloom-Schwelle */
    sx[13] = (g_cfg.aa_mode & 2) ? 1.0f : 0.0f;                         /* W131: FXAA Welt-RT (rq_sfx) */
    sx[14] = (g_cfg.shadow_soft && g_cfg.shade_shadow) ? 1.0f : 0.0f;   /* W131: 3x3-Gauss-PCF (rq_shadow) */
    sx[15] = (g_sba_loc && g_cfg.shade_shadow && g_cfg.shadow_sun > 0) ?  /* W466: Sonnen-/Schattenkontrast (LoC) */
             (float)sfx_pct(g_cfg.shadow_sun, 0, 200) / 100.0f : 0.0f;
    if (flags) { g_sfx_nflag[2]++; }
}

/* ------------------------------------------------------------------ */
/* Cockpit-Nachbearbeitung                                             */
/* ------------------------------------------------------------------ */
#define SFX_BPTR(b) ((b)->lpVtbl->GetBufferPointer(b))
#define SFX_BSIZE(b) ((b)->lpVtbl->GetBufferSize(b))
#define SFX_BREL(b) ((b)->lpVtbl->Release(b))

static const char kSFX_VS[] =
    "struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "O main(uint id : SV_VertexID) { O o; float2 t = float2((id << 1) & 2, id & 2);\n"
    "  o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = t; return o; }\n";

/* p0 = (Texelgroesse Quelle x/y, Schwelle, Knie), p1 = (Richtung x/y, Sigma-Schritt, -),
 * p2 = (glow, spill, light, -), p3 = (SSAO: Radius px, -, -, Staerke),
 * p4 = (Farbtemperatur r/g/b, Saettigung), p5 = (Lift, 1/Gamma, Gain, Farbe an),
 * p6 = (DIAG-Ansicht, -, -, -) */
#define SFX_PS_HEAD \
    "cbuffer P : register(b0) { float4 p0; float4 p1; float4 p2; float4 p3; float4 p4; float4 p5; float4 p6; float4 p7; };\n" \
    "Texture2D t0 : register(t0); Texture2D t1 : register(t1); Texture2D t2 : register(t2); Texture2D t3 : register(t3);\n" \
    "Texture2D t4 : register(t4);\n" \
    "SamplerState s0 : register(s0);\n" \
    "struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"

/* Hellpass + 4x4-Mittel (vier bilineare Abgriffe) */
static const char kSFX_PS_BRIGHT[] = SFX_PS_HEAD
    "float3 bp(float3 c, float th) { float m = max(c.r, max(c.g, c.b));\n"
    "  return c * saturate((m - th) / p0.w); }\n"
    "float4 main(O i) : SV_Target { float2 d = p0.xy;\n"
    "  float th = p0.z;\n"
    "  if (p1.w > 0.0) { float3 l = t1.SampleLevel(s0, i.uv, 0).rgb; th = max(th, max(l.r, max(l.g, l.b)) * p1.w); }\n" /* W465 LoC: lokaler Kontrast */
    "  float3 c = bp(t0.Sample(s0, i.uv + float2(-d.x, -d.y)).rgb, th) + bp(t0.Sample(s0, i.uv + float2(d.x, -d.y)).rgb, th)\n"
    "           + bp(t0.Sample(s0, i.uv + float2(-d.x, d.y)).rgb, th) + bp(t0.Sample(s0, i.uv + float2(d.x, d.y)).rgb, th);\n"
    "  return float4(c * 0.25, 1); }\n";

/* 4x4-Verkleinerung ohne Hellpass */
static const char kSFX_PS_DOWN[] = SFX_PS_HEAD
    "float4 main(O i) : SV_Target { float2 d = p0.xy;\n"
    "  float3 c = t0.Sample(s0, i.uv + float2(-d.x, -d.y)).rgb + t0.Sample(s0, i.uv + float2(d.x, -d.y)).rgb\n"
    "           + t0.Sample(s0, i.uv + float2(-d.x, d.y)).rgb + t0.Sample(s0, i.uv + float2(d.x, d.y)).rgb;\n"
    "  return float4(c * 0.25, 1); }\n";

/* Gauss 9 Abgriffe (bilinear, effektiv 17 Texel), Richtung p1.xy */
static const char kSFX_PS_BLUR[] = SFX_PS_HEAD
    "float4 main(O i) : SV_Target {\n"
    "  static const float w[5] = { 0.2270270270, 0.3162162162, 0.0702702703, 0.0, 0.0 };\n"
    "  static const float o[3] = { 0.0, 1.3846153846, 3.2307692308 };\n"
    "  float2 d = p1.xy * p1.z; float3 c = t0.Sample(s0, i.uv).rgb * w[0];\n"
    "  [unroll] for (int k = 1; k < 3; k++) { c += (t0.Sample(s0, i.uv + d * o[k]).rgb + t0.Sample(s0, i.uv - d * o[k]).rgb) * w[k]; }\n"
    "  return float4(c, 1); }\n";

/* Zusammensetzen: Original * Abdunklung(unbeleuchtet) + Glow + Spill (W121, unveraendert) */
static const char kSFX_PS_COMP[] = SFX_PS_HEAD
    "float4 main(O i) : SV_Target {\n"
    "  float4 c = t0.Load(int3(int2(i.p.xy), 0));\n"
    "  float m = max(c.r, max(c.g, c.b));\n"
    "  float k = lerp(p2.z, 1.0, smoothstep(0.35, 0.8, m));\n"
    "  float3 g = t1.Sample(s0, i.uv).rgb * p2.x + t2.Sample(s0, i.uv).rgb * p2.y;\n"
    "  float3 r = c.rgb * k + g * (1.0 - c.rgb * 0.5);\n"
    "  return float4(saturate(r), c.a); }\n";

/* W131 SHADER-OPT2: wie COMP, dazu SSAO (t3, ausser an leuchtenden Pixeln) und
 * Farbabstimmung (Farbtemperatur, Lift/Gain, Gamma, Saettigung). */
static const char kSFX_PS_COMPX[] = SFX_PS_HEAD
    "float4 main(O i) : SV_Target {\n"
    "  float4 c = t0.Load(int3(int2(i.p.xy), 0));\n"
    "  float m = max(c.r, max(c.g, c.b));\n"
    "  float k = lerp(p2.z, 1.0, smoothstep(0.35, 0.8, m));\n"
    "  float3 g = t1.Sample(s0, i.uv).rgb * p2.x + t2.Sample(s0, i.uv).rgb * p2.y;\n"
    "  float3 r = c.rgb * k + g * (1.0 - c.rgb * 0.5);\n"
    "  float ao = 1.0;\n"
    "  if (p3.w > 0.0) { ao = t3.Load(int3(int2(i.p.xy), 0)).r; r *= saturate(1.0 - (1.0 - ao) * 2.0 * p3.w * (1.0 - smoothstep(0.6, 0.9, m))); }\n" /* 100 % = doppelte Verdeckung (W131_d1: AO min 0,62) */
    "  if (p5.w > 0.0) { r = saturate(r) * p4.rgb;\n"
    "    r = saturate(r * p5.z + p5.x * (1.0 - r));\n"
    "    r = pow(max(r, 0.0), p5.y);\n"
    "    float l = dot(r, float3(0.2126, 0.7152, 0.0722)); r = l + (r - l) * p4.w; }\n"
    "  if (p7.x > 0.0) { float3 fl = t4.Load(int3(1, 0, 0)).rgb * p7.x;\n" /* W465: LoC-Blitzlicht */
    "    float2 q = (i.p.xy - p7.yz) / max(p7.w, 1.0); float fa = 0.45 + 0.55 * saturate(1.0 - 0.6 * dot(q, q));\n"
    "    r += (r * 1.6 + 0.035) * fl * fa * (1.0 - smoothstep(0.55, 0.9, m)); }\n"
    "  if (p6.x == 1.0) { r = ao.xxx; }\n"
    "  return float4(saturate(r), c.a); }\n";

/* W465: warm gewichteter Mittelwert des Hellpass-Bilds (1/16, t0, lokaler Kontrast) im Fensterbereich (Mitte,
 * x 0,2..0,8, y 0,05..0,6) -> 1x1. Warm = Feuer/Explosion/Muendungsfeuer; Himmel (blau), Radar/HUD (gruen) und
 * grosse helle Flaechen zaehlen kaum (Messung W465: Schuss 5x Grundwert). */
static const char kSFX_PS_MEAN[] = SFX_PS_HEAD
    "float4 main(O i) : SV_Target { float3 c = float3(0, 0, 0);\n"
    "  [unroll] for (int y = 0; y < 12; y++) { [unroll] for (int x = 0; x < 16; x++) {\n"
    "    float2 uv = float2(0.2 + 0.6 * (x + 0.5) / 16.0, 0.05 + 0.55 * (y + 0.5) / 12.0);\n"
    "    float3 s = t0.SampleLevel(s0, uv, 0).rgb; c += s * saturate((s.r - s.b) * 3.0 / max(max(s.r, s.g), 1e-4)); } }\n"
    "  return float4(c / 192.0, 1); }\n";

/* W465: Blitz-Zustand 2x1: x=0 gleitender Mittelwert, x=1 Lichtfarbe (Anstieg, abklingend).
 * t0 = Mittelwert jetzt (1x1), t1 = Zustand vorher; p0 = (Mischrate Mittelwert, Abklingfaktor, Verstaerkung, Neustart) */
static const char kSFX_PS_FLASH[] = SFX_PS_HEAD
    "float4 main(O i) : SV_Target { float3 cur = t0.Load(int3(0, 0, 0)).rgb;\n"
    "  float3 avg = t1.Load(int3(0, 0, 0)).rgb, fl = t1.Load(int3(1, 0, 0)).rgb;\n"
    "  if (p0.w > 0.5) { avg = cur; fl = float3(0, 0, 0); }\n"
    "  if (i.p.x < 1.0) { return float4(lerp(avg, cur, p0.x), 1); }\n"
    "  float3 d = max(cur - avg * 1.1 - 0.0004, 0.0) * p0.z;\n"
    "  return float4(min(max(fl * p0.y, d), 1.5), 1); }\n";

/* W131 SHADER-OPT2: FXAA (eigene Umsetzung des veroeffentlichten Verfahrens,
 * Lottes 2009 - Luma-Kontrast, Kantenrichtung, Kantensuche, Subpixel-Anteil;
 * kein uebernommener Code). Gleiche Rechnung wie rq_fxaa (render_psgen.c). */
static const char kSFX_PS_FXAA[] = SFX_PS_HEAD
    "float lu(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
    "float3 sm(float2 uv) { return t0.SampleLevel(s0, uv, 0).rgb; }\n"
    "float4 main(O i) : SV_Target {\n"
    "  float2 d = p0.xy, uv = i.uv; float4 c4 = t0.Load(int3(int2(i.p.xy), 0)); float3 cm = c4.rgb;\n"
    "  float lm = lu(cm), ln = lu(sm(uv + float2(0.0, -d.y))), ls = lu(sm(uv + float2(0.0, d.y)));\n"
    "  float le = lu(sm(uv + float2(d.x, 0.0))), lw = lu(sm(uv + float2(-d.x, 0.0)));\n"
    "  float lmin = min(lm, min(min(ln, ls), min(le, lw))), lmax = max(lm, max(max(ln, ls), max(le, lw)));\n"
    "  float rng = lmax - lmin; if (rng < max(0.0312, lmax * 0.125)) { return c4; }\n"
    "  float lnw = lu(sm(uv - d)), lse = lu(sm(uv + d)), lne = lu(sm(uv + float2(d.x, -d.y))), lsw = lu(sm(uv + float2(-d.x, d.y)));\n"
    "  float eh = abs(0.25 * lnw - 0.5 * ln + 0.25 * lne) + abs(0.5 * lw - lm + 0.5 * le) + abs(0.25 * lsw - 0.5 * ls + 0.25 * lse);\n"
    "  float ev = abs(0.25 * lnw - 0.5 * lw + 0.25 * lsw) + abs(0.5 * ln - lm + 0.5 * ls) + abs(0.25 * lne - 0.5 * le + 0.25 * lse);\n"
    "  bool hz = eh >= ev;\n"
    "  float sub = saturate(abs((2.0 * (ln + ls + le + lw) + lnw + lne + lsw + lse) / 12.0 - lm) / rng);\n"
    "  sub = smoothstep(0.0, 1.0, sub); sub = sub * sub * 0.75;\n"
    "  float l1 = hz ? ln : lw, l2 = hz ? ls : le, g1 = abs(l1 - lm), g2 = abs(l2 - lm);\n"
    "  float st = hz ? d.y : d.x, ll, gr; if (g1 >= g2) { st = -st; ll = 0.5 * (l1 + lm); gr = g1; } else { ll = 0.5 * (l2 + lm); gr = g2; }\n"
    "  float2 e = uv; if (hz) { e.y += st * 0.5; } else { e.x += st * 0.5; }\n"
    "  float2 of = hz ? float2(d.x, 0.0) : float2(0.0, d.y); float2 q1 = e - of, q2 = e + of;\n"
    "  float e1 = lu(sm(q1)) - ll, e2 = lu(sm(q2)) - ll;\n"
    "  bool r1 = abs(e1) >= gr * 0.25, r2 = abs(e2) >= gr * 0.25;\n"
    "  static const float qs[10] = { 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 4.0, 8.0 };\n"
    "  [unroll] for (int k = 1; k < 10; k++) {\n"
    "    if (!r1) { q1 -= of * qs[k]; e1 = lu(sm(q1)) - ll; r1 = abs(e1) >= gr * 0.25; }\n"
    "    if (!r2) { q2 += of * qs[k]; e2 = lu(sm(q2)) - ll; r2 = abs(e2) >= gr * 0.25; } }\n"
    "  float d1 = hz ? uv.x - q1.x : uv.y - q1.y, d2 = hz ? q2.x - uv.x : q2.y - uv.y;\n"
    "  bool dir1 = d1 < d2; float po = 0.5 - min(d1, d2) / max(d1 + d2, 1e-6);\n"
    "  bool ok = ((dir1 ? e1 : e2) < 0.0) != (lm < ll);\n"
    "  float fo = max(ok ? po : 0.0, sub); float2 fu = uv; if (hz) { fu.y += fo * st; } else { fu.x += fo * st; }\n"
    "  return float4(sm(fu), c4.a); }\n";

/* W131 SHADER-OPT2: SSAO aus der Backbuffer-Tiefe (t0 = D24 als R24). Eigene
 * Umsetzung: Tiefe linearisiert (1/(1-z) ~ w/n bei fernem far), Ebene der
 * Mitte aus den kleineren einseitigen Gradienten, 8 Abgriffe auf einer Spirale
 * (4x4-Rotationsmuster, das AOBLUR wieder ausmittelt); ein Abgriff verdeckt,
 * wenn er vor der Ebene liegt (relativ 1..5 % voll, ab 20..40 % ausgeblendet). */
#define SFX_PS_LZ "float lz(float z) { return 1.0 / max(1.0 - z, 1e-6); }\n"
static const char kSFX_PS_AO[] = SFX_PS_HEAD SFX_PS_LZ
    "float4 main(O i) : SV_Target {\n"
    "  int2 px = int2(i.p.xy), mx = int2(p0.zw) - 1;\n"
    "  float z = t0.Load(int3(px, 0)).r;\n"
    "  if (p3.y == 2.0) { float v = saturate(-log10(max(1.0 - z, 1e-7)) / 7.0); return float4(v, v, v, 1); }\n" /* DIAG ssao_view=2 */
    "  if (z >= 0.999999) { return float4(1, 1, 1, 1); }\n"
    "  float dc = lz(z);\n"
    "  float dl = lz(t0.Load(int3(clamp(px + int2(-1, 0), int2(0, 0), mx), 0)).r), dr = lz(t0.Load(int3(clamp(px + int2(1, 0), int2(0, 0), mx), 0)).r);\n"
    "  float du = lz(t0.Load(int3(clamp(px + int2(0, -1), int2(0, 0), mx), 0)).r), dd = lz(t0.Load(int3(clamp(px + int2(0, 1), int2(0, 0), mx), 0)).r);\n"
    "  float gx = abs(dr - dc) < abs(dc - dl) ? dr - dc : dc - dl;\n"
    "  float gy = abs(dd - dc) < abs(dc - du) ? dd - dc : dc - du;\n"
    "  float ang = ((float)((px.x & 3) * 4 + (px.y & 3)) + 0.5) * 0.39269908;\n"
    "  float occ = 0.0;\n"
    "  [unroll] for (int k = 0; k < 8; k++) {\n"
    "    float a = ang + (float)k * 2.3999632, rr = ((float)k + 0.5) / 8.0;\n"
    "    float2 o = float2(cos(a), sin(a)) * (rr * p3.x);\n"
    "    int2 q = clamp(px + int2(round(o)), int2(0, 0), mx);\n"
    "    float ds = lz(t0.Load(int3(q, 0)).r);\n"
    "    float df = (dc + gx * o.x + gy * o.y - ds) / dc;\n"
    "    occ += saturate(df * 25.0 - 0.25) * saturate((0.4 - df) * 5.0); }\n"
    "  float ao = 1.0 - occ / 8.0;\n"
    "  return float4(ao, ao, ao, 1); }\n";

/* 4x4-Mittel der AO (t0) mit Tiefengewicht (t1 = Tiefe) */
static const char kSFX_PS_AOBLUR[] = SFX_PS_HEAD SFX_PS_LZ
    "float4 main(O i) : SV_Target {\n"
    "  int2 px = int2(i.p.xy), mx = int2(p0.zw) - 1;\n"
    "  float dc = lz(t1.Load(int3(px, 0)).r), acc = 0.0, ws = 0.0;\n"
    "  [unroll] for (int y = -2; y < 2; y++) { [unroll] for (int x = -2; x < 2; x++) {\n"
    "    int2 q = clamp(px + int2(x, y), int2(0, 0), mx);\n"
    "    float w = saturate(1.0 - abs(lz(t1.Load(int3(q, 0)).r) - dc) / (dc * 0.05)) + 0.001;\n"
    "    acc += t0.Load(int3(q, 0)).r * w; ws += w; } }\n"
    "  float ao = acc / ws;\n"
    "  return float4(ao, ao, ao, 1); }\n";

enum { SP_BRIGHT, SP_DOWN, SP_BLUR, SP_COMP, SP_COMPX, SP_FXAA, SP_AO, SP_AOBLUR, SP_MEAN, SP_FLASH, SP_N };
#define SFX_NTEX 11 /* W465: 8 = Mittelwert 1x1, 9/10 = Blitz-Zustand 2x1 (Wechselpuffer) */
static ID3D11VertexShader *sfx_vs;
static ID3D11PixelShader *sfx_ps[SP_N];
static ID3D11Buffer *sfx_cb;
static ID3D11SamplerState *sfx_smp;
static ID3D11BlendState *sfx_bs;
static ID3D11RasterizerState *sfx_rs;
static ID3D11DepthStencilState *sfx_ds;
static int sfx_state; /* 0 noch nicht, 1 bereit, -1 gescheitert */
/* Zwischenbilder: 0 = Kopie (BB-Format), 1/2 = 1/4, 3/4 = 1/16,
 * W131: 5 = Zwischenstand vor FXAA (BB-Format), 6/7 = SSAO roh/geglaettet (R8, bei Bedarf) */
static ID3D11Texture2D *sfx_tex[SFX_NTEX];
static ID3D11ShaderResourceView *sfx_srv[SFX_NTEX];
static ID3D11RenderTargetView *sfx_rtv[SFX_NTEX];
static unsigned sfx_w[SFX_NTEX], sfx_h[SFX_NTEX];
static DXGI_FORMAT sfx_fmt;
static unsigned sfx_napply;
static ID3D11ShaderResourceView *sfx_t4; /* W465: 5. Eingang (t4) fuer sfx_pass, sonst NULL */
static int sfx_fl_cur = 9, sfx_fl_reset = 1; /* W465: aktueller Blitz-Zustand (9/10), Neustart */
static LONGLONG sfx_fl_t; /* W465: letzte Auswertung (QPC) */
static int sfx_noupd; /* TEST(W465) shfx_var: Blitz-Zustand nur lesen */
static float sfx_fl_hold; /* W465: Sekunden seit Neustart des Blitz-Zustands */
static int sfx_scene_last = -1; /* W465: Szenenwechsel -> Blitz-Zustand neu */
static int sfx_scene; /* W465: LoC-Szene dieses Bilds (loc_render_post_scene), SB 1 */

static int sfx_init(void)
{
    static const char *const ps_src[SP_N] = { kSFX_PS_BRIGHT, kSFX_PS_DOWN, kSFX_PS_BLUR, kSFX_PS_COMP,
                                              kSFX_PS_COMPX, kSFX_PS_FXAA, kSFX_PS_AO, kSFX_PS_AOBLUR,
                                              kSFX_PS_MEAN, kSFX_PS_FLASH };
    ID3DBlob *b = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_SAMPLER_DESC sd;
    D3D11_BLEND_DESC bld;
    D3D11_RASTERIZER_DESC rd;
    D3D11_DEPTH_STENCIL_DESC dsd;
    int k;
    if (sfx_state) { return sfx_state > 0; }
    sfx_state = -1;
    if (!g_dev) { return 0; }
    if (!sb_im_compile(kSFX_VS, "vs_4_0", &b)) { HLOG_E("SHFX: VS-Uebersetzung gescheitert"); return 0; }
    ID3D11Device_CreateVertexShader(g_dev, SFX_BPTR(b), SFX_BSIZE(b), NULL, &sfx_vs);
    SFX_BREL(b);
    for (k = 0; k < SP_N; k++) {
        b = NULL;
        if (!sb_im_compile(ps_src[k], "ps_4_0", &b)) { HLOG_E("SHFX: PS %d gescheitert", k); return 0; }
        ID3D11Device_CreatePixelShader(g_dev, SFX_BPTR(b), SFX_BSIZE(b), NULL, &sfx_ps[k]);
        SFX_BREL(b);
        if (!sfx_ps[k]) { return 0; }
    }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 128;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &sfx_cb))) { return 0; }
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(ID3D11Device_CreateSamplerState(g_dev, &sd, &sfx_smp))) { return 0; }
    memset(&bld, 0, sizeof bld);
    bld.RenderTarget[0].BlendEnable = FALSE;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(ID3D11Device_CreateBlendState(g_dev, &bld, &sfx_bs))) { return 0; }
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(ID3D11Device_CreateRasterizerState(g_dev, &rd, &sfx_rs))) { return 0; }
    memset(&dsd, 0, sizeof dsd);
    dsd.DepthEnable = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(ID3D11Device_CreateDepthStencilState(g_dev, &dsd, &sfx_ds))) { return 0; }
    sfx_state = 1;
    HLOG_I("SHFX: Nachbearbeitung bereit (%d Shader, W131: Farbe/FXAA/SSAO)", SP_N);
    return 1;
}

static void sfx_free_tex(void)
{
    int k;
    for (k = 0; k < SFX_NTEX; k++) {
        if (sfx_rtv[k]) { ID3D11RenderTargetView_Release(sfx_rtv[k]); sfx_rtv[k] = NULL; }
        if (sfx_srv[k]) { ID3D11ShaderResourceView_Release(sfx_srv[k]); sfx_srv[k] = NULL; }
        if (sfx_tex[k]) { ID3D11Texture2D_Release(sfx_tex[k]); sfx_tex[k] = NULL; }
        sfx_w[k] = sfx_h[k] = 0;
    }
    sfx_fl_reset = 1; /* W465 */
}

static int sfx_mk(int k, unsigned w, unsigned h, DXGI_FORMAT f, int rt)
{
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = w ? w : 1;
    td.Height = h ? h : 1;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = f;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (rt ? D3D11_BIND_RENDER_TARGET : 0);
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &sfx_tex[k]))) { return 0; }
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)sfx_tex[k], NULL, &sfx_srv[k]))) { return 0; }
    if (rt && FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)sfx_tex[k], NULL, &sfx_rtv[k]))) { return 0; }
    sfx_w[k] = td.Width;
    sfx_h[k] = td.Height;
    return 1;
}

static int sfx_size(unsigned w, unsigned h, DXGI_FORMAT f)
{
    if (sfx_tex[0] && sfx_w[0] == w && sfx_h[0] == h && sfx_fmt == f) { return 1; }
    sfx_free_tex();
    sfx_fmt = f;
    if (!sfx_mk(0, w, h, f, 0) ||
        !sfx_mk(1, (w + 3) / 4, (h + 3) / 4, DXGI_FORMAT_R16G16B16A16_FLOAT, 1) ||
        !sfx_mk(2, (w + 3) / 4, (h + 3) / 4, DXGI_FORMAT_R16G16B16A16_FLOAT, 1) ||
        !sfx_mk(3, (w + 15) / 16, (h + 15) / 16, DXGI_FORMAT_R16G16B16A16_FLOAT, 1) ||
        !sfx_mk(4, (w + 15) / 16, (h + 15) / 16, DXGI_FORMAT_R16G16B16A16_FLOAT, 1)) {
        HLOG_W("SHFX: Zwischenbilder %ux%u fmt=%d nicht anlegbar", w, h, (int)f);
        sfx_free_tex();
        return 0;
    }
    HLOG_I("SHFX: Zwischenbilder %ux%u (1/4 %ux%u, 1/16 %ux%u)", w, h, sfx_w[1], sfx_h[1], sfx_w[3], sfx_h[3]);
    return 1;
}

/* W131: Zwischenbild k (5..7) bei Bedarf in voller Groesse anlegen */
static int sfx_need(int k, DXGI_FORMAT f)
{
    if (sfx_tex[k]) { return 1; }
    if (!sfx_mk(k, sfx_w[0], sfx_h[0], f, 1)) {
        HLOG_W("SHFX: Zwischenbild %d (%ux%u fmt=%d) nicht anlegbar", k, sfx_w[0], sfx_h[0], (int)f);
        return 0;
    }
    return 1;
}

static void sfx_pass(int ps, ID3D11RenderTargetView *rtv, unsigned w, unsigned h,
                     ID3D11ShaderResourceView *a, ID3D11ShaderResourceView *b, ID3D11ShaderResourceView *c,
                     ID3D11ShaderResourceView *d, const float p[32])
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *sv[5];
    ID3D11ShaderResourceView *nul[5] = { NULL, NULL, NULL, NULL, NULL };
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)sfx_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, p, 128);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)sfx_cb, 0);
    }
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 5, nul); /* Ziel nie zugleich als SRV */
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &rtv, NULL); /* auch kein DSV (BB-Tiefe als SRV) */
    vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
    vp.Width = (float)w; vp.Height = (float)h;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_PSSetShader(g_ctx, sfx_ps[ps], NULL, 0);
    sv[0] = a; sv[1] = b; sv[2] = c; sv[3] = d; sv[4] = sfx_t4;
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 5, sv);
    ID3D11DeviceContext_Draw(g_ctx, 3, 0);
}

/* W131: Parameter der Farbabstimmung (p4/p5) */
static void sfx_grade_params(float p[32])
{
    float t = (float)sfx_pct(g_cfg.color_temp, -100, 100) / 100.0f;
    float r = 1.0f + 0.10f * t, g = 1.0f + 0.01f * t, b = 1.0f - 0.10f * t;
    float l = 0.2126f * r + 0.7152f * g + 0.0722f * b; /* Helligkeit erhalten */
    p[16] = r / l; p[17] = g / l; p[18] = b / l;
    p[19] = (float)sfx_pct(g_cfg.color_sat, 0, 200) / 100.0f;
    p[20] = (float)sfx_pct(g_cfg.color_lift, -50, 50) / 500.0f;          /* +-0,1 */
    p[21] = 100.0f / (float)sfx_pct(g_cfg.color_gamma, 50, 200);         /* Exponent 1/Gamma */
    p[22] = (float)sfx_pct(g_cfg.color_gain, 50, 200) / 100.0f;
    p[23] = 1.0f;
}

/* DIAG(W465) shfx_flash_diag=1: Blitz-Zustand (2x1) und Mittelwert (1x1) zuruecklesen und alle 10 Bilder loggen
 * (Kalibrierung; blockiert die GPU, nur fuer Messlaeufe). */
static float sfx_h2f(unsigned short h)
{
    unsigned e = (h >> 10) & 31u, m = h & 1023u;
    float v = e == 0u ? (float)m / 16777216.0f : (float)ldexp((double)(1024u + m), (int)e - 25);
    return (h & 0x8000u) ? -v : v;
}

static void sfx_flash_diag(ID3D11Texture2D *st, ID3D11Texture2D *cur)
{
    static ID3D11Texture2D *stg;
    static unsigned n;
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_BOX bx = { 0, 0, 0, 2, 1, 1 };
    if ((++n % (unsigned)(g_cfg.shfx_flash_diag > 1 ? g_cfg.shfx_flash_diag : 10)) != 0u) { return; }
    if (!stg) {
        memset(&td, 0, sizeof td);
        td.Width = 4; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &stg))) { stg = NULL; return; }
    }
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)stg, 0, 0, 0, 0, (ID3D11Resource *)st, 0, &bx);
    bx.right = 1;
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)stg, 0, 2, 0, 0, (ID3D11Resource *)cur, 0, &bx);
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
        const unsigned short *h = (const unsigned short *)m.pData;
        float v[12];
        int i;
        for (i = 0; i < 12; i++) { v[i] = sfx_h2f(h[(i / 3) * 4 + (i % 3)]); }
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)stg, 0);
        HLOG_I("SHFXFLASH f=%u cur=%.3f/%.3f/%.3f avg=%.3f/%.3f/%.3f licht=%.3f/%.3f/%.3f", g_present_no,
               v[6], v[7], v[8], v[0], v[1], v[2], v[3], v[4], v[5]);
    }
}

/* DIAG(W465) shfx_gpu_diag=1: GPU-Zeit je Stufe der Nachbearbeitung (Zeitstempel-Abfragen, Auslesen 4 Bilder
 * spaeter ohne Warten). Stufen: 0 Kopie, 1 Leuchten/Spill (Hellpass + Unschaerfe), 2 Blitzlicht, 3 SSAO,
 * 4 Komposition (Licht/Farbe), 5 FXAA. Log SHFXGPU alle 300 ausgewerteten Bilder (Mittel in ms). */
#define SFX_GQ_N 4
#define SFX_GQ_ST 7
static ID3D11Query *sfx_gq_dis[SFX_GQ_N], *sfx_gq_ts[SFX_GQ_N][SFX_GQ_ST];
static int sfx_gq_used[SFX_GQ_N], sfx_gq_i, sfx_gq_on;
static double sfx_gq_sum[SFX_GQ_ST];
static unsigned sfx_gq_n;
static void sfx_gq_mark(int st)
{
    if (sfx_gq_on && sfx_gq_ts[sfx_gq_i][st]) { ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)sfx_gq_ts[sfx_gq_i][st]); }
}
static void sfx_gq_begin(void)
{
    int j, k;
    sfx_gq_on = 0;
    if (!g_cfg.shfx_gpu_diag) { return; }
    j = (sfx_gq_i + 1) % SFX_GQ_N;
    if (!sfx_gq_dis[j]) {
        D3D11_QUERY_DESC qd;
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &sfx_gq_dis[j]))) { sfx_gq_dis[j] = NULL; return; }
        qd.Query = D3D11_QUERY_TIMESTAMP;
        for (k = 0; k < SFX_GQ_ST; k++) {
            if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &sfx_gq_ts[j][k]))) { sfx_gq_ts[j][k] = NULL; return; }
        }
    }
    if (sfx_gq_used[j]) { /* aelteste Messung auswerten (4 Bilder alt), sonst verwerfen */
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 t[SFX_GQ_ST];
        int ok = ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)sfx_gq_dis[j], &dj, sizeof dj,
                                             D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && !dj.Disjoint && dj.Frequency;
        for (k = 0; ok && k < SFX_GQ_ST; k++) {
            ok = ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)sfx_gq_ts[j][k], &t[k], sizeof t[k],
                                             D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        }
        if (ok) {
            for (k = 1; k < SFX_GQ_ST; k++) { sfx_gq_sum[k - 1] += (double)(t[k] - t[k - 1]) * 1000.0 / (double)dj.Frequency; }
            if (++sfx_gq_n % 300u == 0u) {
                HLOG_I("SHFXGPU n=%u ms: kopie=%.3f leuchten=%.3f blitz=%.3f ssao=%.3f komp=%.3f fxaa=%.3f summe=%.3f bb=%ux%u",
                       sfx_gq_n, sfx_gq_sum[0] / 300.0, sfx_gq_sum[1] / 300.0, sfx_gq_sum[2] / 300.0, sfx_gq_sum[3] / 300.0,
                       sfx_gq_sum[4] / 300.0, sfx_gq_sum[5] / 300.0,
                       (sfx_gq_sum[0] + sfx_gq_sum[1] + sfx_gq_sum[2] + sfx_gq_sum[3] + sfx_gq_sum[4] + sfx_gq_sum[5]) / 300.0,
                       sfx_w[0], sfx_h[0]);
                memset(sfx_gq_sum, 0, sizeof sfx_gq_sum);
            }
        }
        sfx_gq_used[j] = 0;
    }
    sfx_gq_i = j;
    sfx_gq_on = 1;
    ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous *)sfx_gq_dis[j]);
    sfx_gq_mark(0);
}
static void sfx_gq_end(void)
{
    if (!sfx_gq_on) { return; }
    sfx_gq_mark(SFX_GQ_ST - 1);
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)sfx_gq_dis[sfx_gq_i]);
    sfx_gq_used[sfx_gq_i] = 1;
    sfx_gq_on = 0;
}

/* Nachbearbeitung auf rtv (Ressource res, Backbuffer-Groesse). Aufruf unter ctx_lock. */
static void sfx_apply(ID3D11RenderTargetView *rtv, ID3D11Resource *res, int quiet)
{
    D3D11_TEXTURE2D_DESC dd;
    float p[32];
    float glow = (float)sfx_pct(g_cfg.cockpit_glow, 0, 100) / 100.0f;
    float spill = (float)sfx_pct(g_cfg.cockpit_spill, 0, 100) / 100.0f;
    float light = (float)sfx_pct(g_cfg.cockpit_light, 30, 100) / 100.0f;
    int grade = shfx_grade_on(), aa = (g_cfg.aa_mode & 1) != 0, ssao = g_cfg.ssao > 0;
    int flash = sfx_flash_on() && sfx_scene == 1; /* W465: nur Cockpit-Mission */
    int bloom = glow > 0.0f || spill > 0.0f || flash;
    int comp = shfx_cockpit_on() || grade || ssao || flash;
    ID3D11ShaderResourceView *ao_srv = NULL;
    /* gesicherter Zustand */
    ID3D11RenderTargetView *s_rtv = NULL;
    ID3D11DepthStencilView *s_dsv = NULL;
    ID3D11BlendState *s_bs = NULL;
    FLOAT s_bf[4];
    UINT s_mask = 0, s_sref = 0, s_nvp = 1;
    ID3D11DepthStencilState *s_ds = NULL;
    ID3D11RasterizerState *s_rs = NULL;
    D3D11_VIEWPORT s_vp;
    ID3D11InputLayout *s_lay = NULL;
    D3D11_PRIMITIVE_TOPOLOGY s_topo;
    ID3D11VertexShader *s_vs = NULL;
    ID3D11PixelShader *s_ps = NULL;
    ID3D11ShaderResourceView *s_srv[5] = { NULL, NULL, NULL, NULL, NULL };
    ID3D11SamplerState *s_smp = NULL;
    ID3D11Buffer *s_cb = NULL;
    int k;

    if (sfx_scene == 2) { light = 1.0f; } /* W465: LoC Replay/Aussenkamera - kein Cockpit zum Abdunkeln */
    if (!rtv || !res || !g_ctx || !sfx_init()) { return; }
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &dd);
    if (!sfx_size(dd.Width, dd.Height, dd.Format)) { return; }
    if (aa && comp && !sfx_need(5, dd.Format)) { aa = 0; }
    if (ssao) {
        /* BB-Tiefe als SRV (typlos, d3d11_render.c) in gleicher Groesse? */
        D3D11_TEXTURE2D_DESC zd;
        ID3D11Resource *zr = NULL;
        if (g_bb_dsrv) {
            ID3D11ShaderResourceView_GetResource(g_bb_dsrv, &zr);
            if (zr) {
                ID3D11Texture2D_GetDesc((ID3D11Texture2D *)zr, &zd);
                ID3D11Resource_Release(zr);
                if (zd.Width == dd.Width && zd.Height == dd.Height && sfx_need(6, DXGI_FORMAT_R8_UNORM) &&
                    sfx_need(7, DXGI_FORMAT_R8_UNORM)) {
                    ao_srv = sfx_srv[7];
                }
            }
        }
    }

    ID3D11DeviceContext_OMGetRenderTargets(g_ctx, 1, &s_rtv, &s_dsv);
    ID3D11DeviceContext_OMGetBlendState(g_ctx, &s_bs, s_bf, &s_mask);
    ID3D11DeviceContext_OMGetDepthStencilState(g_ctx, &s_ds, &s_sref);
    ID3D11DeviceContext_RSGetState(g_ctx, &s_rs);
    ID3D11DeviceContext_RSGetViewports(g_ctx, &s_nvp, &s_vp);
    ID3D11DeviceContext_IAGetInputLayout(g_ctx, &s_lay);
    ID3D11DeviceContext_IAGetPrimitiveTopology(g_ctx, &s_topo);
    ID3D11DeviceContext_VSGetShader(g_ctx, &s_vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(g_ctx, &s_ps, NULL, NULL);
    ID3D11DeviceContext_PSGetShaderResources(g_ctx, 0, 5, s_srv);
    ID3D11DeviceContext_PSGetSamplers(g_ctx, 0, 1, &s_smp);
    ID3D11DeviceContext_PSGetConstantBuffers(g_ctx, 0, 1, &s_cb);

    if (flash && !sfx_tex[8] && (!sfx_mk(8, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 1) ||
                                 !sfx_mk(9, 2, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 1) ||
                                 !sfx_mk(10, 2, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 1))) {
        flash = 0; /* Zwischenbilder fehlen: ohne Blitzlicht weiter */
    }
    sfx_gq_begin(); /* DIAG(W465) */
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)sfx_tex[0], res);
    sfx_gq_mark(1);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, sfx_bs, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, sfx_ds, 0);
    ID3D11DeviceContext_RSSetState(g_ctx, sfx_rs);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_ctx, sfx_vs, NULL, 0);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &sfx_smp);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &sfx_cb);

    memset(p, 0, sizeof p);
    if (bloom) {
        ID3D11ShaderResourceView *loc_mean = NULL;
        if (g_sba_loc) { /* W465 LoC: lokales Mittel (1/16) - grosse helle Flaechen (Himmel) leuchten nicht, nur
                          * kleine Lichtquellen (Lampen, LEDs, Anzeigen, Feuer, Funken) */
            p[0] = 1.0f / (float)sfx_w[0]; p[1] = 1.0f / (float)sfx_h[0];
            sfx_pass(SP_DOWN, sfx_rtv[2], sfx_w[2], sfx_h[2], sfx_srv[0], NULL, NULL, NULL, p);
            p[0] = 1.0f / (float)sfx_w[2]; p[1] = 1.0f / (float)sfx_h[2];
            sfx_pass(SP_DOWN, sfx_rtv[4], sfx_w[4], sfx_h[4], sfx_srv[2], NULL, NULL, NULL, p);
            loc_mean = sfx_srv[4];
            p[7] = (float)sfx_pct(g_cfg.shfx_loc_lc, 0, 300) / 100.0f; /* Schwelle = lokales Mittel x Faktor */
        }
        /* Hellpass Kopie -> 1/4 (Abgriffe in der Mitte der 2x2-Bloecke) */
        p[0] = 1.0f / (float)sfx_w[0]; p[1] = 1.0f / (float)sfx_h[0];
        p[2] = 0.55f; p[3] = 0.35f; /* Schwelle max(R,G,B) 0,55 .. 0,9 */
        if (g_sba_loc) { p[2] = (float)sfx_pct(g_cfg.shfx_loc_th, 0, 100) / 100.0f; p[3] = 0.25f; }
        sfx_pass(SP_BRIGHT, sfx_rtv[1], sfx_w[1], sfx_h[1], sfx_srv[0], loc_mean, NULL, NULL, p);
        p[7] = 0.0f;
        /* Glow: Gauss H/V auf 1/4 */
        p[4] = 1.0f / (float)sfx_w[1]; p[5] = 0.0f; p[6] = 1.0f;
        sfx_pass(SP_BLUR, sfx_rtv[2], sfx_w[2], sfx_h[2], sfx_srv[1], NULL, NULL, NULL, p);
        p[4] = 0.0f; p[5] = 1.0f / (float)sfx_h[1];
        sfx_pass(SP_BLUR, sfx_rtv[1], sfx_w[1], sfx_h[1], sfx_srv[2], NULL, NULL, NULL, p);
        /* Spill: 1/4 -> 1/16, zweimal Gauss H/V (breit) */
        p[0] = 1.0f / (float)sfx_w[1]; p[1] = 1.0f / (float)sfx_h[1];
        sfx_pass(SP_DOWN, sfx_rtv[3], sfx_w[3], sfx_h[3], sfx_srv[1], NULL, NULL, NULL, p);
        for (k = 0; k < 2; k++) {
            p[4] = 1.0f / (float)sfx_w[3]; p[5] = 0.0f; p[6] = 1.5f;
            sfx_pass(SP_BLUR, sfx_rtv[4], sfx_w[4], sfx_h[4], sfx_srv[3], NULL, NULL, NULL, p);
            p[4] = 0.0f; p[5] = 1.0f / (float)sfx_h[3];
            sfx_pass(SP_BLUR, sfx_rtv[3], sfx_w[3], sfx_h[3], sfx_srv[4], NULL, NULL, NULL, p);
        }
    }
    sfx_gq_mark(2);
    if (flash && sfx_noupd) { sfx_t4 = sfx_srv[sfx_fl_cur]; } /* TEST(W465) Variante: Zustand nur lesen */
    else if (flash) { /* W465: Mittelwert des Hellpass (1/16) -> 1x1, Zustand fortschreiben (Wechselpuffer 9/10) */
        LARGE_INTEGER qn, qf;
        float dt, q[32];
        int nx = sfx_fl_cur == 9 ? 10 : 9;
        QueryPerformanceCounter(&qn); QueryPerformanceFrequency(&qf);
        dt = sfx_fl_t ? (float)((double)(qn.QuadPart - sfx_fl_t) / (double)qf.QuadPart) : 0.0f;
        if (dt > 0.25f || dt <= 0.0f) { dt = dt > 0.25f ? 0.25f : 0.0f; }
        sfx_fl_t = qn.QuadPart;
        memset(q, 0, sizeof q);
        q[0] = 1.0f - expf(-dt / 0.6f);  /* gleitender Mittelwert ~0,6 s */
        q[1] = expf(-dt / 0.2f);         /* Abklingen ~0,2 s */
        if (sfx_fl_reset) { sfx_fl_hold = 0.0f; }
        sfx_fl_hold += dt;               /* Szenenbeginn (Einblenden) ~2 s ohne Blitzlicht */
        q[2] = sfx_fl_hold < 2.0f ? 0.0f : (float)sfx_pct(g_cfg.shfx_flash_gain, 0, 1000); /* Verstaerkung Anstieg -> Lichtfarbe */
        q[3] = sfx_fl_reset ? 1.0f : 0.0f;
        sfx_pass(SP_MEAN, sfx_rtv[8], 1, 1, sfx_srv[3], NULL, NULL, NULL, q);
        sfx_pass(SP_FLASH, sfx_rtv[nx], 2, 1, sfx_srv[8], sfx_srv[sfx_fl_cur], NULL, NULL, q);
        sfx_fl_cur = nx;
        sfx_fl_reset = 0;
        sfx_t4 = sfx_srv[nx];
        if (g_cfg.shfx_flash_diag) { sfx_flash_diag(sfx_tex[nx], sfx_tex[8]); }
    }
    sfx_gq_mark(3);
    if (ao_srv) { /* W131 SSAO: roh (6) -> geglaettet (7), Radius 2 % der Bildhoehe */
        float q[32];
        memset(q, 0, sizeof q);
        q[2] = (float)sfx_w[0]; q[3] = (float)sfx_h[0];
        q[12] = (float)sfx_h[0] * 0.02f;
        q[13] = g_cfg.ssao_view == 2 ? 2.0f : 0.0f; /* DIAG: Tiefenansicht -log10(1-z)/7 */
        if (q[12] < 4.0f) { q[12] = 4.0f; }
        sfx_pass(SP_AO, sfx_rtv[6], sfx_w[0], sfx_h[0], g_bb_dsrv, NULL, NULL, NULL, q);
        sfx_pass(SP_AOBLUR, sfx_rtv[7], sfx_w[0], sfx_h[0], sfx_srv[6], g_bb_dsrv, NULL, NULL, q);
    }
    sfx_gq_mark(4);
    p[8] = glow * 1.6f;   /* Glow-Faktor bei 100 % */
    p[9] = spill * 2.5f;  /* Spill-Faktor bei 100 % (breit verteilt -> schwach) */
    p[10] = light;
    if (flash) { /* W465: Staerke, Zentrum (Fenster: Mitte, obere 40 % des Bilds), Radius */
        p[28] = (float)sfx_pct(g_cfg.fx_light_cockpit, 0, 200) / 100.0f;
        p[29] = 0.5f * (float)dd.Width; p[30] = 0.36f * (float)dd.Height; p[31] = 0.75f * (float)dd.Height;
    }
    if (comp) {
        ID3D11RenderTargetView *ct = aa ? sfx_rtv[5] : rtv;
        if (!grade && !ao_srv && !g_cfg.ssao_view && !flash) {
            sfx_pass(SP_COMP, ct, dd.Width, dd.Height, sfx_srv[0], bloom ? sfx_srv[1] : NULL,
                     bloom ? sfx_srv[3] : NULL, NULL, p);
        } else {
            if (ao_srv) { p[15] = (float)sfx_pct(g_cfg.ssao, 0, 100) / 100.0f; }
            if (grade) { sfx_grade_params(p); }
            p[24] = (ao_srv && g_cfg.ssao_view) ? 1.0f : 0.0f;
            sfx_pass(SP_COMPX, ct, dd.Width, dd.Height, sfx_srv[0], bloom ? sfx_srv[1] : NULL,
                     bloom ? sfx_srv[3] : NULL, ao_srv, p);
        }
    }
    sfx_gq_mark(5);
    if (aa) { /* W131 FXAA Endbild: nach Licht/Farbe (Quelle 5 bzw. Kopie 0) */
        float q[32];
        memset(q, 0, sizeof q);
        q[0] = 1.0f / (float)sfx_w[0]; q[1] = 1.0f / (float)sfx_h[0];
        sfx_pass(SP_FXAA, rtv, dd.Width, dd.Height, comp ? sfx_srv[5] : sfx_srv[0], NULL, NULL, NULL, q);
    }

    sfx_gq_end(); /* DIAG(W465) */
    sfx_t4 = NULL;
    {
        ID3D11ShaderResourceView *nul[5] = { NULL, NULL, NULL, NULL, NULL };
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 5, nul);
    }
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &s_rtv, s_dsv);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, s_bs, s_bf, s_mask);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, s_ds, s_sref);
    ID3D11DeviceContext_RSSetState(g_ctx, s_rs);
    if (s_nvp) { ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &s_vp); }
    ID3D11DeviceContext_IASetInputLayout(g_ctx, s_lay);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, s_topo);
    ID3D11DeviceContext_VSSetShader(g_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, s_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 5, s_srv);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &s_smp);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &s_cb);
    if (s_rtv) { ID3D11RenderTargetView_Release(s_rtv); }
    if (s_dsv) { ID3D11DepthStencilView_Release(s_dsv); }
    if (s_bs) { ID3D11BlendState_Release(s_bs); }
    if (s_ds) { ID3D11DepthStencilState_Release(s_ds); }
    if (s_rs) { ID3D11RasterizerState_Release(s_rs); }
    if (s_lay) { ID3D11InputLayout_Release(s_lay); }
    if (s_vs) { ID3D11VertexShader_Release(s_vs); }
    if (s_ps) { ID3D11PixelShader_Release(s_ps); }
    for (k = 0; k < 5; k++) { if (s_srv[k]) { ID3D11ShaderResourceView_Release(s_srv[k]); } }
    if (s_smp) { ID3D11SamplerState_Release(s_smp); }
    if (s_cb) { ID3D11Buffer_Release(s_cb); }
    if (quiet) { return; }
    if (++sfx_napply == 1u || (sfx_napply % 3000u) == 0u) {
        HLOG_I("SHFX cockpit #%u f=%u glow=%d spill=%d light=%d bb=%ux%u", sfx_napply, g_present_no,
               g_cfg.cockpit_glow, g_cfg.cockpit_spill, g_cfg.cockpit_light, dd.Width, dd.Height);
        HLOG_I("SHFX post #%u f=%u aa=%d ssao=%d(tiefe=%d) farbe=%d (sat=%d temp=%d lift=%d gamma=%d gain=%d)",
               sfx_napply, g_present_no, g_cfg.aa_mode, g_cfg.ssao, ao_srv != NULL, grade, g_cfg.color_sat,
               g_cfg.color_temp, g_cfg.color_lift, g_cfg.color_gamma, g_cfg.color_gain);
    }
}

static int sfx_mission(void)
{
    unsigned s = 0, m = 0;
    if (g_sba_loc) { /* W465 LOC-ENHANCED: LoC-eigene Szenenerkennung statt SB-Adressen (Null-Seite) */
        sfx_scene = loc_render_post_scene();
        if (sfx_scene != sfx_scene_last) { sfx_fl_reset = 1; sfx_scene_last = sfx_scene; }
        return sfx_scene != 0;
    }
    sfx_scene = 1;
    __try {
        s = *(volatile unsigned *)(uintptr_t)M3D_SCENE_ADDR;
        m = *(volatile unsigned *)(uintptr_t)M3D_MLOOP_ADDR;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return s == 0x2Cu && m == 1u;
}

/* TEST(W465) shfx_var="name:key=v,key=v|name2:...": zu jedem LoC-Testbild (loc_shot_tick_every, Bild ohne
 * Zwischenbilder) zusaetzlich je Variante dasselbe Bild mit anderen Nachbearbeitungs-Werten als
 * loct_<Tick>_<name>.jpg (gleicher Augenblick, Vergleich/Abstimmung der Effekte). Das gezeigte Bild bleibt
 * unveraendert (vorher/nachher gesichert); Blitzlicht-Zustand wird gelesen, nicht fortgeschrieben. */
static ID3D11Texture2D *sfx_vpre, *sfx_vfin;
static int sfx_vok;
static int sfx_vtex(ID3D11Texture2D **t, ID3D11Resource *res)
{
    D3D11_TEXTURE2D_DESC td, cd;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &td);
    if (*t) {
        ID3D11Texture2D_GetDesc(*t, &cd);
        if (cd.Width == td.Width && cd.Height == td.Height && cd.Format == td.Format) { return 1; }
        ID3D11Texture2D_Release(*t);
        *t = NULL;
    }
    td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1; td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags = 0; td.MiscFlags = 0;
    return SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, t));
}
void sb_shfx_var_pre(ID3D11Resource *res)
{
    sfx_vok = 0;
    if (!g_cfg.shfx_var[0] || !g_sba_loc || !res) { return; }
    if (!sfx_vtex(&sfx_vpre, res)) { return; }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)sfx_vpre, res);
    sfx_vok = 1;
}
void sb_shfx_var_shots(ID3D11RenderTargetView *rtv, ID3D11Resource *res, const char *base)
{
    static hcfg_t save;
    char list[sizeof g_cfg.shfx_var], *v, *ctx = NULL;
    if (!sfx_vok || !rtv || !res || !sfx_vtex(&sfx_vfin, res)) { return; }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)sfx_vfin, res);
    strcpy_s(list, sizeof list, g_cfg.shfx_var);
    save = g_cfg;
    for (v = strtok_s(list, "|", &ctx); v; v = strtok_s(NULL, "|", &ctx)) {
        char nm[96], set[512], *c = strchr(v, ':'), *q;
        if (!c) { continue; }
        *c = '\0';
        strcpy_s(set, sizeof set, c + 1);
        for (q = set; *q; q++) { if (*q == ',') { *q = ';'; } }
        g_cfg = save;
        g_cfg.shfx_var[0] = '\0';
        hcfg_apply_set(set, "shfx_var");
        ID3D11DeviceContext_CopyResource(g_ctx, res, (ID3D11Resource *)sfx_vpre);
        if (shfx_post_on() && sfx_mission()) { sfx_noupd = 1; sfx_apply(rtv, res, 1); sfx_noupd = 0; }
        _snprintf_s(nm, sizeof nm, _TRUNCATE, "%s_%s", base, v);
        sb_render_shot_name(nm, NULL);
    }
    g_cfg = save;
    ID3D11DeviceContext_CopyResource(g_ctx, res, (ID3D11Resource *)sfx_vfin);
}

/* Aufrufstelle Present/Blit (unter ctx_lock): rtv/res = gezeigtes Bild */
void sb_shfx_post(ID3D11RenderTargetView *rtv, ID3D11Resource *res)
{
    /* Im Present gilt g_present_no = dieses Bild (rq_ab_base prueft das naechste) */
    unsigned sf = (unsigned)g_cfg.screenshot_frame, ev = (unsigned)g_cfg.screenshot_every;
    int abase = g_cfg.shfx_ab && sf && ev && g_present_no >= sf && ((g_present_no - sf) % ev) == 0u;
    if (!shfx_post_on() || g_shfx_bypass || abase || !sfx_mission()) { return; }
    sfx_apply(rtv, res, 0);
}

/* DIAG(W121) shfx_ab: zum Screenshot <base> (Original-Werte) dasselbe Bild
 * mit Cockpit-Nachbearbeitung als <base>_fx (Kopie; Spiel-Bild unberuehrt). */
void sb_shfx_ab_shot(const char *base)
{
    ID3D11Resource *bb = NULL;
    ID3D11Texture2D *tmp = NULL;
    ID3D11RenderTargetView *trtv = NULL;
    D3D11_TEXTURE2D_DESC dd;
    if (!g_cfg.shfx_ab || !shfx_post_on() || !g_rtv || !g_dev || !sfx_mission()) { return; }
    ID3D11RenderTargetView_GetResource(g_rtv, &bb);
    if (!bb) { return; }
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)bb, &dd);
    dd.BindFlags = D3D11_BIND_RENDER_TARGET;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.CPUAccessFlags = 0;
    dd.MiscFlags = 0;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &tmp)) &&
        SUCCEEDED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)tmp, NULL, &trtv))) {
        char nm[64];
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)tmp, bb);
        sfx_apply(trtv, (ID3D11Resource *)tmp, 1);
        _snprintf_s(nm, sizeof nm, _TRUNCATE, "%s_fx", base);
        sb_render_shot_name(nm, (ID3D11Resource *)tmp);
    }
    if (trtv) { ID3D11RenderTargetView_Release(trtv); }
    if (tmp) { ID3D11Texture2D_Release(tmp); }
    ID3D11Resource_Release(bb);
}

/* DIAG(W121) shfx_ab=2: je Screenshot-Paar eine Option (Board-Paare aus einem
 * Lauf). Reihenfolge = Titel der A/B-Paare (pairs.json). */
typedef struct {
    const char *name;
    int fog, con, gam, sharp, bloom, glow, spill, light;   /* Teil 1 */
    int expo, tone, bthr, lworld, lckp, focus;              /* Teil 2 */
} sfx_set_t;
/* shfx_ab=2: Serie Teil 1 (M2/M5) */
static const sfx_set_t kSfxSets[] = {
    { "nebel_reduziert",   50, 100, 100,  0, 100,  0,  0, 100, 100, 0, 0, 100, 100, 100 },
    { "nebel_aus",          0, 100, 100,  0, 100,  0,  0, 100, 100, 0, 0, 100, 100, 100 },
    { "kontrast_130",     100, 130, 100,  0, 100,  0,  0, 100, 100, 0, 0, 100, 100, 100 },
    { "gamma_130",        100, 100, 130,  0, 100,  0,  0, 100, 100, 0, 0, 100, 100, 100 },
    { "schaerfe_60",      100, 100, 100, 60, 100,  0,  0, 100, 100, 0, 0, 100, 100, 100 },
    { "bloom_50",         100, 100, 100,  0,  50,  0,  0, 100, 100, 0, 0, 100, 100, 100 },
    { "bloom_150",        100, 100, 100,  0, 150,  0,  0, 100, 100, 0, 0, 100, 100, 100 },
    { "leuchten_60",      100, 100, 100,  0, 100, 60,  0, 100, 100, 0, 0, 100, 100, 100 },
    { "lichtschein_60",   100, 100, 100,  0, 100,  0, 60, 100, 100, 0, 0, 100, 100, 100 },
    { "grundlicht_60",    100, 100, 100,  0, 100,  0,  0,  60, 100, 0, 0, 100, 100, 100 },
    { "kombi_welt",        50, 120, 110, 50, 100,  0,  0, 100, 100, 0, 0, 100, 100, 100 },
    { "kombi_cockpit",    100, 100, 100,  0, 100, 50, 40,  70, 100, 0, 0, 100, 100, 100 },
};
/* shfx_ab=3: Serie Teil 2 (Hangar-Start M1 / Aussenwelt: Belichtung, Tonemapping,
 * Gamma, Licht Welt/Cockpit, Bloom-Schwelle/-Staerke) */
static const sfx_set_t kSfxSets2[] = {
    { "belichtung_70",    100, 100, 100,  0, 100,  0,  0, 100,  70, 0,  0, 100, 100, 100 },
    { "belichtung_50",    100, 100, 100,  0, 100,  0,  0, 100,  50, 0,  0, 100, 100, 100 },
    { "tonemap_reinhard", 100, 100, 100,  0, 100,  0,  0, 100, 100, 1,  0, 100, 100, 100 },
    { "tonemap_aces",     100, 100, 100,  0, 100,  0,  0, 100, 100, 2,  0, 100, 100, 100 },
    { "gamma_130",        100, 100, 130,  0, 100,  0,  0, 100, 100, 0,  0, 100, 100, 100 },
    { "licht_welt_60",    100, 100, 100,  0, 100,  0,  0, 100, 100, 0,  0,  60, 100, 100 },
    { "licht_welt_40",    100, 100, 100,  0, 100,  0,  0, 100, 100, 0,  0,  40, 100, 100 },
    { "licht_cockpit_70", 100, 100, 100,  0, 100,  0,  0, 100, 100, 0,  0, 100,  70, 100 },
    { "bloom_schwelle_30",100, 100, 100,  0, 100,  0,  0, 100, 100, 0, 30, 100, 100, 100 },
    { "bloom_s30_150",    100, 100, 100,  0, 150,  0,  0, 100, 100, 0, 30, 100, 100, 100 },
    { "randschaerfe_0",   100, 100, 100,  0, 100,  0,  0, 100, 100, 0,  0, 100, 100,   0 },
    { "randschaerfe_50",  100, 100, 100,  0, 100,  0,  0, 100, 100, 0,  0, 100, 100,  50 },
    { "kombi_belichtung", 100, 100, 110,  0, 100,  0,  0, 100,  85, 2,  0,  80, 100, 100 },
    { "kombi_cockpit",    100, 100, 100,  0, 100, 50, 40,  70, 100, 0,  0, 100, 100, 100 },
};

static void sfx_cycle(unsigned n)
{
    static int k = -1, saved, sv[14];
    unsigned sf = (unsigned)g_cfg.screenshot_frame, ev = (unsigned)g_cfg.screenshot_every;
    const sfx_set_t *tab = g_cfg.shfx_ab == 3 ? kSfxSets2 : kSfxSets, *t;
    int nsets = g_cfg.shfx_ab == 3 ? (int)(sizeof kSfxSets2 / sizeof kSfxSets2[0]) : (int)(sizeof kSfxSets / sizeof kSfxSets[0]);
    if (g_cfg.shfx_ab < 2 || g_cfg.shfx_ab > 3 || !sf || !ev || n < sf) { return; } /* W131: 4/5 in sb_shfx_present_end */
    if (!saved) {
        saved = 1;
        sv[0] = g_cfg.world_fog; sv[1] = g_cfg.world_contrast; sv[2] = g_cfg.world_gamma; sv[3] = g_cfg.world_sharpen;
        sv[4] = g_cfg.bloom_strength; sv[5] = g_cfg.cockpit_glow; sv[6] = g_cfg.cockpit_spill; sv[7] = g_cfg.cockpit_light;
        sv[8] = g_cfg.world_exposure; sv[9] = g_cfg.tonemap; sv[10] = g_cfg.bloom_threshold; sv[11] = g_cfg.light_world;
        sv[12] = g_cfg.light_cockpit; sv[13] = g_cfg.world_focus;
    }
    /* nach dem letzten Paar die Ausgangswerte zurueck (z. B. fuer overlay_test danach) */
    if (k == nsets - 1 && (n - sf) / ev >= (unsigned)k && (n - sf) % ev == 2u) {
        g_cfg.world_fog = sv[0]; g_cfg.world_contrast = sv[1]; g_cfg.world_gamma = sv[2]; g_cfg.world_sharpen = sv[3];
        g_cfg.bloom_strength = sv[4]; g_cfg.cockpit_glow = sv[5]; g_cfg.cockpit_spill = sv[6]; g_cfg.cockpit_light = sv[7];
        g_cfg.world_exposure = sv[8]; g_cfg.tonemap = sv[9]; g_cfg.bloom_threshold = sv[10]; g_cfg.light_world = sv[11];
        g_cfg.light_cockpit = sv[12]; g_cfg.world_focus = sv[13];
        HLOG_I("SHFX ab: Zyklus fertig (%d Paare), Ausgangswerte zurueck f=%u", k + 1, n);
        k = nsets;
        return;
    }
    if (k >= nsets - 1 || (n - sf) % ev != 0u) { return; }
    k++;
    t = &tab[k];
    g_cfg.world_fog = t->fog; g_cfg.world_contrast = t->con; g_cfg.world_gamma = t->gam;
    g_cfg.world_sharpen = t->sharp; g_cfg.bloom_strength = t->bloom; g_cfg.cockpit_glow = t->glow;
    g_cfg.cockpit_spill = t->spill; g_cfg.cockpit_light = t->light;
    g_cfg.world_exposure = t->expo; g_cfg.tonemap = t->tone; g_cfg.bloom_threshold = t->bthr;
    g_cfg.light_world = t->lworld; g_cfg.light_cockpit = t->lckp; g_cfg.world_focus = t->focus;
    HLOG_I("SHFX ab #%d f=%u A=shot_%06u (Original) B=shot_%06u (%s)", k, n, n, n + 1u, t->name);
}

/* ------------------------------------------------------------------ */
/* W131 SHADER-OPT2 / W173 LAUNCHER-P4: einheitliches Grafik-Preset      */
/* ------------------------------------------------------------------ */
/* Ein Preset setzt die Einzelwerte; danach bleiben sie einzeln aenderbar.
 * Werte: sb_gfxpreset.h (einzige Quelle, gleiche Tabelle im Launcher).
 * Begruendung der Shader-Werte BERICHT_M2 W131, neu abgestimmt W136 nach
 * Nutzer-Feedback 6.11. (Nebel immer Original, Schaerfe 60, gleiche mittlere
 * Helligkeit wie das Original +-5 % in M1-Hangar/M2/M5):
 *  Original = Client-Standard (W173: shading=1 explizit, Effekte aus, keine
 *             HD-Texturen; vorher blieb shading stehen),
 *  dezent   = technische Glaettung: Schattenpuffer (Original-Detail der
 *             Xbox, W127), FXAA Endbild + Welt, Schaerfe 60, SSAO 25, Saettigung
 *             105, Gain 103 (gleicht Schatten/SSAO-Abdunklung aus) + Effekte dezent,
 *  stark    = deutlich: dazu weiche Schatten, Randunschaerfe 50, Kontrast 110,
 *             Reinhard + Bloom-Schwelle 20 (Lichter komprimiert, kein Ausbrennen),
 *             kraeftigere SSAO/Farbe (Saettigung 115, waermer, Gain 110), Nebel
 *             Original, kein Cockpit-Leuchten/Lichtschein/Grundlicht (W131: M5-Himmel
 *             ueberstrahlt bzw. M1-Hangar zu dunkel), Lift 0 + Effekte stark.
 * W173 (PO 7.11.): hud_label_avoid gehoert nicht mehr zum Preset (vorher stark=1).
 * Effect-Pack-Ausnahme: Schluessel aktiver Effect Packs (sb_rem_fx_locked)
 * zaehlen bei der Erkennung nicht und bleiben beim Anwenden stehen. */
typedef struct {
    const char *name;
    int shading;                                             /* -1 = unveraendert (DIAG-Ausgangswerte) */
    int fog, con, gam, sharp, bloom, glow, spill, light;     /* W121 Teil 1 */
    int expo, tone, bthr, lworld, lckp, focus;               /* W121 Teil 2 */
    int aa, ssao, sat, temp, lift, cgam, gain, shadow, soft; /* W131 */
    int sun;                                                 /* W471 */
} sfx_preset_t;
static const char *const kSfxName[SB_GP_NPRE] = { "original", "dezent", "stark", "performance", "klar", "enhanced",
                                                  "vivid" /* W478 */ };

/* cfg-Feld je Preset-Schluessel (Reihenfolge = kSbGfxPreset). */
typedef char sb_gp_nkey_chk[(SB_GP_NKEY == 39 && SB_GP_FX0 == 25 && SB_GP_HD == 30 && SB_GP_MFX == 35 &&
                              SB_GP_EDGE == 36 && SB_GP_EDGE + SB_GP_NEDGE == SB_GP_NKEY) ? 1 : -1];
static int *const kGpField[SB_GP_NKEY] = {
    &g_cfg.shading, &g_cfg.world_fog, &g_cfg.world_contrast, &g_cfg.world_gamma, &g_cfg.world_sharpen,
    &g_cfg.bloom_strength, &g_cfg.cockpit_glow, &g_cfg.cockpit_spill, &g_cfg.cockpit_light,
    &g_cfg.world_exposure, &g_cfg.tonemap, &g_cfg.bloom_threshold, &g_cfg.light_world, &g_cfg.light_cockpit,
    &g_cfg.world_focus, &g_cfg.aa_mode, &g_cfg.ssao, &g_cfg.color_sat, &g_cfg.color_temp, &g_cfg.color_lift,
    &g_cfg.color_gamma, &g_cfg.color_gain, &g_cfg.shade_shadow, &g_cfg.shadow_soft,
    &g_cfg.shadow_sun, /* W471 */
    &g_cfg.fx_soft, &g_cfg.fx_embers, &g_cfg.fx_light, &g_cfg.fx_light_cockpit, &g_cfg.fx_interp,
    &g_cfg.hd_textures,
    &g_cfg.interp_fps, &g_cfg.rt_scale, &g_cfg.rt_scale_dyn, &g_cfg.fs_interp_match,
    &g_cfg.monitor_fx, /* W252 */
    &g_cfg.edge_mode, &g_cfg.edge_fade_width, &g_cfg.edge_fade_strength, /* W266 */
};

/* W471: Preset-Wert fuer das laufende Spiel (LoC-Original = Spalte 0 mit den Schatten des Spiels) */
static int gp_loc(void) { return g_sba_loc || g_cfg.loc_boot; }
static int gp_want(int k, int q) { return sb_gp_val(k, q, gp_loc()); }

static void gp_range(int part, int *k0, int *k1)
{
    *k0 = part == SB_GFX_FX ? SB_GP_FX0 : 0;
    *k1 = part == SB_GFX_SHADER ? SB_GP_FX0 : part == SB_GFX_FX ? SB_GP_HD : SB_GP_NKEY;
}

/* Aktueller Wert (HD wirksam: remaster && hd_textures; fx_interp 0/1). */
static int gp_cur(int k)
{
    if (k == SB_GP_HD) { return (g_cfg.remaster && g_cfg.hd_textures) ? 1 : 0; }
    if (k == SB_GP_FX0 + 4) { return *kGpField[k] != 0; }
    return *kGpField[k];
}

/* Entsprechen die Werte des Teils dem Preset q? (gesperrte Schluessel zaehlen nicht) */
int sb_gfx_part_is(int q, int part)
{
    int k, k0, k1;
    if (q < 0 || q >= SB_GP_NPRE) { return 0; }
    gp_range(part, &k0, &k1);
    for (k = k0; k < k1; k++) {
        int want = gp_want(k, q);
        if (sb_rem_fx_locked(kSbGfxPreset[k].key)) { continue; }
        if (k == SB_GP_FX0 + 4) { want = want != 0; }
        if (gp_cur(k) != want) { return 0; }
    }
    return 1;
}

/* Preset q auf den Teil anwenden (ohne HD: das schaltet das ESC-Menue live). */
void sb_gfx_part_apply(int q, int part)
{
    int k, k0, k1;
    if (q < 0 || q >= SB_GP_NPRE) { return; }
    gp_range(part, &k0, &k1);
    for (k = k0; k < k1 && k < SB_GP_HD; k++) {
        if (sb_rem_fx_locked(kSbGfxPreset[k].key)) { continue; }
        *kGpField[k] = gp_want(k, q);
    }
}

/* W266 PRESET-EDGE: Breitbild-Raender (edge_mode/-width/-strength) wie Preset q? */
int sb_gfx_edge_is(int q)
{
    int k;
    if (q < 0 || q >= SB_GP_NPRE) { return 0; }
    for (k = SB_GP_EDGE; k < SB_GP_EDGE + SB_GP_NEDGE; k++) {
        if (!sb_rem_fx_locked(kSbGfxPreset[k].key) && *kGpField[k] != kSbGfxPreset[k].v[q]) { return 0; }
    }
    return 1;
}

/* W266: Breitbild-Raender des Presets q setzen (live: edge_* wird je Present gelesen). */
void sb_gfx_edge_apply(int q)
{
    int k;
    if (q < 0 || q >= SB_GP_NPRE) { return; }
    for (k = SB_GP_EDGE; k < SB_GP_EDGE + SB_GP_NEDGE; k++) {
        if (!sb_rem_fx_locked(kSbGfxPreset[k].key)) { *kGpField[k] = kSbGfxPreset[k].v[q]; }
    }
}

/* Einheitliches Preset: 0..4 (W252: 4 = Klare Sicht), -1 = Benutzerdefiniert (gleiche Semantik wie
 * launcher_cfg.c lcfg_gfx_preset). */
int sb_gfx_preset_match_for(int fps, int rt)
{
    int q;
    for (q = 0; q < SB_GP_NPRE; q++) {
        if (!sb_gp_visible(q, g_sba_loc || g_cfg.loc_boot)) { continue; } /* W465: nur waehlbare Presets des Spiels */
        if (sb_gfx_part_is(q, SB_GFX_SHADER) && sb_gfx_part_is(q, SB_GFX_FX) &&
            gp_cur(SB_GP_HD) == kSbGfxPreset[SB_GP_HD].v[q] &&
            fps == kSbGfxPreset[SB_GP_FPS].v[q] &&
            rt == kSbGfxPreset[SB_GP_RT].v[q] &&
            g_cfg.rt_scale_dyn == kSbGfxPreset[SB_GP_DYN].v[q] &&
            g_cfg.fs_interp_match == kSbGfxPreset[SB_GP_MATCH].v[q] &&
            (sb_rem_fx_locked(kSbGfxPreset[SB_GP_MFX].key) ||
             g_cfg.monitor_fx == kSbGfxPreset[SB_GP_MFX].v[q]) && /* W252 */
            sb_gfx_edge_is(q)) { return q; }
    }
    return -1;
}

/* W465 LOC-ENHANCED: LoC-Spieler-cfg ohne Marke gfx_loc_rev (vor Enhanced bzw. ohne Launcher-Umstellung) ->
 * Optik-Schluessel (Shader + Effekte, ohne Bildrate/Skalierung/Raender) auf Enhanced, SB look (W459) aus - er ist
 * Teil von Enhanced. Gleiche Regel wie der Launcher (Migration user_loc.cfg). Nur zur Laufzeit, Datei unberuehrt.
 * W466: Marke 1 (W465-Enhanced ohne Schatten) -> Enhanced mit Schatten.
 * W471 LOC-SHADOW-ORIG: Marke 1/2 -> genau das alte Enhanced (Rev 1 bzw. 2, sb_gp_enh_old) wird das neue Enhanced
 * (Kontrast 100, gedeckter, Sonne 115); genau das alte Original (Spalte 0 ohne Schatten) wird Original mit den
 * Schatten des Spiels; eigene Werte bleiben, nur shadow_sun bekommt (falls nicht in der Datei) den bis W471
 * wirksamen Wert 140. shadow_sun < 0 = nicht in der Datei (hcfg). */
static int gp_optic_is(int col, int rev)
{
    int k;
    for (k = 0; k < SB_GP_HD; k++) {
        int v = *kGpField[k], want;
        if (k == SB_GP_SUN) {
            if (col == 0 || rev == 1) { continue; } /* ohne Schatten wirkungslos (ESC schrieb 140 auch in Original) */
            if (v < 0) { continue; }               /* fehlt = 140 (alt) */
        }
        want = col == 0 ? kSbGfxPreset[k].v[0] : sb_gp_enh_old(k, rev);
        if (k == SB_GP_FX0 + 4) { v = v != 0; want = want != 0; }
        if (v != want) { return 0; }
    }
    return 1;
}

static void gp_optic_set(int q)
{
    int k;
    for (k = 0; k < SB_GP_HD; k++) { *kGpField[k] = sb_gp_val(k, q, 1); }
}

void sb_gfx_loc_default(void)
{
    int rev = g_cfg.gfx_loc_rev;
    if (rev == 1 || rev == 2) {
        if (gp_optic_is(SB_GP_ENHANCED, rev)) {
            gp_optic_set(SB_GP_ENHANCED);
            HLOG_I("cfg: LoC %s=%d (altes Enhanced) -> Enhanced W471", SB_GP_LOC_REV_KEY, rev);
        } else if (gp_optic_is(0, rev)) {
            gp_optic_set(0);
            HLOG_I("cfg: LoC %s=%d (Original) -> Original mit den Schatten des Spiels (W471)", SB_GP_LOC_REV_KEY, rev);
        } else {
            if (g_cfg.shadow_sun < 0) { g_cfg.shadow_sun = SB_GP_SUN_REV2; }
            HLOG_I("cfg: LoC %s=%d, eigene Optik -> unveraendert (shadow_sun=%d, W471)", SB_GP_LOC_REV_KEY, rev,
                   g_cfg.shadow_sun);
        }
        return;
    }
    gp_optic_set(SB_GP_ENHANCED);
    g_cfg.loc_sblook = 0;
    HLOG_I("cfg: LoC ohne %s -> Grafik-Preset Enhanced (W465)", SB_GP_LOC_REV_KEY);
}

int sb_gfx_preset_match(void)
{
    return sb_gfx_preset_match_for(g_cfg.interp_auto ? -1 : g_cfg.interp_fps, g_cfg.rt_scale); /* W223: auto = -1 (SB_FPS_AUTO) */
}

/* DIAG-Kopie der Shader-Werte des Presets q (Reihenfolge = kSbGfxPreset). */
static void sfx_preset_get(int q, sfx_preset_t *t)
{
    int *f[SB_GP_FX0];
    int k;
    memset(t, 0, sizeof *t);
    f[0] = &t->shading; f[1] = &t->fog; f[2] = &t->con; f[3] = &t->gam; f[4] = &t->sharp;
    f[5] = &t->bloom; f[6] = &t->glow; f[7] = &t->spill; f[8] = &t->light; f[9] = &t->expo;
    f[10] = &t->tone; f[11] = &t->bthr; f[12] = &t->lworld; f[13] = &t->lckp; f[14] = &t->focus;
    f[15] = &t->aa; f[16] = &t->ssao; f[17] = &t->sat; f[18] = &t->temp; f[19] = &t->lift;
    f[20] = &t->cgam; f[21] = &t->gain; f[22] = &t->shadow; f[23] = &t->soft; f[24] = &t->sun;
    t->name = kSfxName[q];
    for (k = 0; k < SB_GP_FX0; k++) { *f[k] = gp_want(k, q); }
}

static void sfx_preset_set(const sfx_preset_t *t)
{
    if (t->shading >= 0) { g_cfg.shading = t->shading; }
    g_cfg.world_fog = t->fog; g_cfg.world_contrast = t->con; g_cfg.world_gamma = t->gam;
    g_cfg.world_sharpen = t->sharp; g_cfg.bloom_strength = t->bloom; g_cfg.cockpit_glow = t->glow;
    g_cfg.cockpit_spill = t->spill; g_cfg.cockpit_light = t->light;
    g_cfg.world_exposure = t->expo; g_cfg.tonemap = t->tone; g_cfg.bloom_threshold = t->bthr;
    g_cfg.light_world = t->lworld; g_cfg.light_cockpit = t->lckp; g_cfg.world_focus = t->focus;
    g_cfg.aa_mode = t->aa; g_cfg.ssao = t->ssao; g_cfg.color_sat = t->sat; g_cfg.color_temp = t->temp;
    g_cfg.color_lift = t->lift; g_cfg.color_gamma = t->cgam; g_cfg.color_gain = t->gain;
    g_cfg.shade_shadow = t->shadow; g_cfg.shadow_soft = t->soft; g_cfg.shadow_sun = t->sun;
}

/* 0 Original, 1 dezent, 2 stark (nur Shader-Teil) */
void shfx_preset_apply(int which)
{
    if (which < 0 || which > 2) { return; }
    sb_gfx_part_apply(which, SB_GFX_SHADER);
    HLOG_I("SHFX preset -> %s (f=%u)", kSfxName[which], g_present_no);
}

/* Welches Preset entspricht den Shader-Werten? 0..2, -1 = Benutzerdefiniert */
int shfx_preset_match(void)
{
    int k;
    for (k = 0; k < SB_GP_NPRE; k++) {
        if (sb_gfx_part_is(k, SB_GFX_SHADER)) { return k; }
    }
    return -1;
}

const char *shfx_preset_name(int which)
{
    return (which >= 0 && which < SB_GP_NPRE) ? kSfxName[which] : "benutzer";
}

/* DIAG(W131) shfx_ab=4: Serie der neuen Optionen; shfx_ab=5: Preset-Tripel.
 * Umschalten am ENDE des Presents (Werte gelten fuer Zeichnen UND Nachbearbeitung
 * des naechsten Bildes). Phase ph des naechsten Bildes = (n+1-sf) % ev:
 *  4: ph 0 -> naechste Option (A-Bild bleibt per abase/sfx_neutral Original, sein
 *     _fx-Shot zeigt die Nachbearbeitungs-Optionen am GLEICHEN Bild), B = +1;
 *     ph 2 -> Ausgangswerte zurueck.
 *  5: ph 0 -> dezent (A = Original per abase, A_fx = Nachbearbeitung dezent),
 *     ph 1 -> dezent (B = +1), ph 2 -> stark (C = +2), ph 3 -> zurueck. Der Lauf
 *     braucht shading=1;shade_shadow=1 (A-Bild ohne Schattenvergleich, s. render_shade.c). */
typedef struct { const char *name; int aa, ssao, sat, temp, lift, cgam, gain, soft; } sfx_set3_t;
static const sfx_set3_t kSfxSets3[] = {
    { "aa_fxaa_endbild",  1,   0, 100,   0,   0, 100, 100, 0 },
    { "aa_fxaa_welt",     2,   0, 100,   0,   0, 100, 100, 0 },
    { "aa_fxaa_beides",   3,   0, 100,   0,   0, 100, 100, 0 },
    { "ssao_50",          0,  50, 100,   0,   0, 100, 100, 0 },
    { "ssao_100",         0, 100, 100,   0,   0, 100, 100, 0 },
    { "saettigung_130",   0,   0, 130,   0,   0, 100, 100, 0 },
    { "saettigung_70",    0,   0,  70,   0,   0, 100, 100, 0 },
    { "temp_warm_50",     0,   0, 100,  50,   0, 100, 100, 0 },
    { "temp_kalt_50",     0,   0, 100, -50,   0, 100, 100, 0 },
    { "lift_m20",         0,   0, 100,   0, -20, 100, 100, 0 },
    { "farbgamma_120",    0,   0, 100,   0,   0, 120, 100, 0 },
    { "gain_120",         0,   0, 100,   0,   0, 100, 120, 0 },
    { "schatten_weich",   0,   0, 100,   0,   0, 100, 100, 1 },
    { "kombi_farbe",      0,   0, 115,  10, -10, 100, 105, 0 },
};

void sb_shfx_present_end(unsigned n)
{
    static int k = -1, saved;
    static sfx_preset_t sv;
    unsigned sf = (unsigned)g_cfg.screenshot_frame, ev = (unsigned)g_cfg.screenshot_every, ph;
    int nsets = (int)(sizeof kSfxSets3 / sizeof kSfxSets3[0]);
    if (g_cfg.shfx_ab < 4 || !sf || !ev || n + 1u < sf) { return; }
    if (g_cfg.shfx_ab == 7) { /* TEST(W471) LoC-Quartett: +0 Original alt (ohne Schatten, Bypass), +1 Original neu
                               * (mit den Schatten des Spiels), +2 Enhanced bis W471, +3 Werte der cfg (Kandidat) */
        static int sv7[SB_GP_HD], have7;
        int kk;
        ph = (n + 1u - sf) % ev; /* Versatz des naechsten Bilds; die Werte gelten ab dem naechsten Bild */
        if (!have7) { for (kk = 0; kk < SB_GP_HD; kk++) { sv7[kk] = *kGpField[kk]; } have7 = 1; }
        if (ph <= 1u) { for (kk = 0; kk < SB_GP_HD; kk++) { *kGpField[kk] = sb_gp_val(kk, 0, 1); } }
        else if (ph == 2u) { for (kk = 0; kk < SB_GP_HD; kk++) { *kGpField[kk] = sb_gp_enh_old(kk, 2); } }
        else if (ph == 3u) { for (kk = 0; kk < SB_GP_HD; kk++) { *kGpField[kk] = sv7[kk]; } }
        return;
    }
    if (!saved) {
        saved = 1;
        memset(&sv, 0, sizeof sv);
        sv.name = "ausgang"; sv.shading = -1;
        sv.fog = g_cfg.world_fog; sv.con = g_cfg.world_contrast; sv.gam = g_cfg.world_gamma;
        sv.sharp = g_cfg.world_sharpen; sv.bloom = g_cfg.bloom_strength; sv.glow = g_cfg.cockpit_glow;
        sv.spill = g_cfg.cockpit_spill; sv.light = g_cfg.cockpit_light; sv.expo = g_cfg.world_exposure;
        sv.tone = g_cfg.tonemap; sv.bthr = g_cfg.bloom_threshold; sv.lworld = g_cfg.light_world;
        sv.lckp = g_cfg.light_cockpit; sv.focus = g_cfg.world_focus; sv.aa = g_cfg.aa_mode; sv.ssao = g_cfg.ssao;
        sv.sat = g_cfg.color_sat; sv.temp = g_cfg.color_temp; sv.lift = g_cfg.color_lift;
        sv.cgam = g_cfg.color_gamma; sv.gain = g_cfg.color_gain; sv.shadow = g_cfg.shade_shadow;
        sv.soft = g_cfg.shadow_soft;
    }
    ph = (n + 1u - sf) % ev;
    if (g_cfg.shfx_ab == 4) {
        if (ph == 0u && k < nsets - 1) {
            const sfx_set3_t *t;
            sfx_preset_set(&sv);
            k++;
            t = &kSfxSets3[k];
            g_cfg.aa_mode = t->aa; g_cfg.ssao = t->ssao; g_cfg.color_sat = t->sat; g_cfg.color_temp = t->temp;
            g_cfg.color_lift = t->lift; g_cfg.color_gamma = t->cgam; g_cfg.color_gain = t->gain;
            g_cfg.shadow_soft = t->soft;
            HLOG_I("SHFX ab #%d f=%u A=shot_%06u (Original) B=shot_%06u (%s)", k, n + 1u, n + 1u, n + 2u, t->name);
        } else if (ph == 2u && k >= 0 && k < nsets) { /* nach dem Zyklus nichts mehr anfassen (Overlay-Tests) */
            sfx_preset_set(&sv);
            if (k == nsets - 1) { HLOG_I("SHFX ab: Zyklus fertig (%d Paare), Ausgangswerte zurueck f=%u", k + 1, n); k = nsets; }
        }
        return;
    }
    if (g_cfg.shfx_ab == 6) { /* DIAG(W131): "stark ohne X" - A Original, B = +1 Variante */
        static const char *const nm[9] = { "stark", "stark_ohne_bloom", "stark_ohne_tonemap", "stark_ohne_nebel",
                                           "stark_ohne_kontrast", "stark_ohne_schaerfe", "stark_ohne_randunschaerfe",
                                           "stark_ohne_farbe", "stark_ohne_cockpitlicht" };
        static int k6 = -1;
        if (ph == 0u && k6 < 8) {
            sfx_preset_t t;
            sfx_preset_get(2, &t);
            k6++;
            switch (k6) {
            case 1: t.bloom = 100; t.bthr = 0; break;
            case 2: t.tone = 0; break;
            case 3: t.fog = 100; break;
            case 4: t.con = 100; break;
            case 5: t.sharp = 0; break;
            case 6: t.focus = 100; break;
            case 7: t.sat = 100; t.temp = 0; t.lift = 0; t.gain = 100; break;
            case 8: t.glow = 0; t.spill = 0; t.light = 100; break;
            default: break;
            }
            sfx_preset_set(&t);
            HLOG_I("SHFX ab #%d f=%u A=shot_%06u (Original) B=shot_%06u (%s)", k6, n + 1u, n + 1u, n + 2u, nm[k6]);
        } else if (ph == 2u && k6 >= 0 && k6 <= 8) {
            sfx_preset_set(&sv);
            if (k6 == 8) { k6 = 9; }
        }
        return;
    }
    if (g_cfg.shfx_ab == 5) {
        static int nt, done;
        if (done) { return; } /* nach dem letzten Tripel nichts mehr anfassen (Overlay-Tests) */
        if (ph == 0u && nt < g_cfg.screenshot_max) {
            sfx_preset_t t;
            sfx_preset_get(1, &t);
            sfx_preset_set(&t);
            HLOG_I("SHFX tripel #%d f=%u A=shot_%06u (Original) B=shot_%06u (dezent) C=shot_%06u (stark)", nt, n + 1u,
                   n + 1u, n + 2u, n + 3u);
            nt++;
        } else if (ph == 2u) {
            sfx_preset_t t;
            sfx_preset_get(2, &t);
            sfx_preset_set(&t);
        } else if (ph == 3u && nt > 0) {
            sfx_preset_set(&sv);
            if (nt >= g_cfg.screenshot_max) { done = 1; HLOG_I("SHFX tripel: fertig (%d), Ausgangswerte zurueck f=%u", nt, n); }
        }
    }
}

/* FIX(SHADE-FX,W121) shade_live_reset: Wechselt `shading` zur Laufzeit (ESC-Menue),
 * halten die zeitlich gemischten Bloom-RTs (20^2/6^2, Blend SRCALPHA/INVSRCALPHA)
 * den Stand des anderen Pfads: Der Bright-Pass ueberschreibt nur, wo er anspringt.
 * Beleg W121_h3: T2 = 0,225 (Rest des alten Pfads) statt 0,000 bei Shading ab Start
 * (W121_h1) -> Hangar-Komposition 0,37 statt 0,13. Auf der Xbox gibt es keinen
 * Pfadwechsel. Daher beim Wechsel alle kleinen RTs (beide Kanten <= 64) leeren;
 * pro Bild neu gezeichnete RTs ueberschreibt das Spiel ohnehin. Aufruf am
 * Present-Anfang (vor den Draws des naechsten Bildes). */
void sb_shfx_shading_watch(void)
{
    static int last = -1;
    int i, n = 0;
    if (last < 0) { last = g_cfg.shading; return; }
    if (g_cfg.shading == last) { return; }
    last = g_cfg.shading;
    if (!g_cfg.shade_live_reset || !g_ctx) { return; }
    sb_ctx_enter();
    for (i = 0; i < g_rtt_n; i++) {
        static const float z[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        if (!g_rtt[i].rtv || g_rtt[i].isdepth || g_rtt[i].w > 64u || g_rtt[i].h > 64u) { continue; }
        ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_rtt[i].rtv, z);
        n++;
    }
    sb_ctx_leave();
    HLOG_I("SHFX: shading -> %d zur Laufzeit, %d kleine RTs (Bloom-Akkumulatoren) geleert, f=%u", last, n, g_present_no);
}

/* Je Present (vor der Screenshot-Logik): shot_mission-Anker, DIAG-Zaehler. */
void sb_shfx_present(unsigned n)
{
    static int anchored, init;
    static int off;
    static unsigned last;
    sfx_cycle(n);
    if (g_cfg.shot_mission) {
        if (!init) { init = 1; off = g_cfg.screenshot_frame; g_cfg.screenshot_frame = 0; }
        if (!anchored) {
            if (g_cfg.shot_mission == 2 ? p2z_scene_mission() : sb_in_mission()) { /* W232: Spielermission statt Ladezaehler == 4; W313: 2 = erstes Missionsbild inkl. Titel-Demo */
                anchored = 1;
                g_cfg.screenshot_frame = (int)n + off;
                HLOG_I("SHFX shot_mission: Missionsbeginn f=%u -> screenshot_frame=%d (every=%d)", n,
                       g_cfg.screenshot_frame, g_cfg.screenshot_every);
            }
        }
    }
    if (g_cfg.shfx_diag && n - last >= 600u && sfx_mission()) {
        last = n;
        HLOG_I("SHFX stat f=%u welt=%d (fog=%d kontrast=%d gamma=%d schaerfe=%d bloom=%d) cockpit=%d "
               "(glow=%d spill=%d licht=%d) weltstufen=%u bloomstufen=%u draws=%u | belichtung=%d tonemap=%d "
               "licht_welt=%d licht_cockpit=%d bloom_schwelle=%d weltdraws=%u cockpitdraws=%u",
               n, shfx_world_on(), g_cfg.world_fog, g_cfg.world_contrast, g_cfg.world_gamma,
               g_cfg.world_sharpen, g_cfg.bloom_strength, shfx_cockpit_on(), g_cfg.cockpit_glow,
               g_cfg.cockpit_spill, g_cfg.cockpit_light, g_sfx_nflag[0], g_sfx_nflag[1], g_sfx_nflag[2],
               g_cfg.world_exposure, g_cfg.tonemap, g_cfg.light_world, g_cfg.light_cockpit, g_cfg.bloom_threshold,
               g_sfx_nflag[3], g_sfx_nflag[4]);
    }
}
