/* render_fx_rem.c - W133 REM-FX: Effekt-Remaster-Optionen (Explosionen, Rauch,
 * Feuer, Funken, Muendungsfeuer). Alles Standard aus = Original bitgleich.
 *
 * Effekt-Draws des Spiels (W90): Immediate-Mode-Draws mit Transform-Programm
 * (VSH-Emulation P2), in den Welt-RTs als Bildschirm-Pixel (rhw) mit
 * z = z_ndc, geblendet ohne Tiefenschreiben, Textur DXT (Effekt-Atlas 1024²
 * DXT3, Muendungsfeuer 256x128 DXT3). Erkennung je Draw in fxr_pre.
 *
 * Optionen (cfg, ESC-Seite "Effekte"):
 *  fx_soft          weiche Partikel: Effekt-Alpha (bei additiver Quelle ONE auch
 *                   RGB) mit saturate((w_szene - w_partikel) / Reichweite) skalieren.
 *                   w = Blicktiefe, aus z_ndc ueber die Projektion des Draws
 *                   (z = A + B / w, A/B aus Zeile 2/3 der VS-Matrix) rekonstruiert;
 *                   Reichweite = Rohradius des Billboards * 0,2 * Staerke. Die
 *                   Szenentiefe kommt aus einer Kopie des RT-Tiefenpuffers (D3D11
 *                   bindet einen Tiefenpuffer nicht zugleich als DSV und SRV), neu
 *                   kopiert nur nach Fremd-Draws. Im Shading-Pfad laufen
 *                   Effekt-Draws dafuer ueber den alten IM-Pfad (gleiche Rechnung
 *                   (V0 + C0) * T0, fx_ps_c0).
 *  fx_embers        Glut/Funken: neue additive Effekt-Draws (Explosion, Feuer,
 *                   Muendungsfeuer) erzeugen einen Ausbruch aus Glutpunkten
 *                   (Weltraum der Rohpositionen, Schwerkraft entlang "oben" der
 *                   Kamera, Lebensdauer 0,6..1,4 s), gezeichnet je Welt-RT als ein
 *                   Draw fester Laenge (prozeduraler Punkt-Shader, keine Assets).
 *  fx_light         Explosions-Licht Welt: Licht-Quad je Welt-RT (c += c * L * warm,
 *                   Radialabfall um das Energiezentrum der additiven Effekte).
 *  fx_light_cockpit Explosions-Licht Cockpit: Nachbearbeitung des gezeigten Bildes
 *                   (wie W121 Cockpit-Licht), Abklingen in Echtzeit je gezeigtem Bild.
 *  fx_interp        120 FPS: Effekt-Draws mit gleicher Signatur und neuen Positionen
 *                   (Partikel) mit der normalen statt der strengen (0,3x) Sprunggrenze
 *                   zwischenrechnen (render_interp.c ip_match).
 *
 * 120 FPS: Soft-/Glut-/Licht-Draws stehen mit ihren Parametern (has_tex-Bits,
 * ip_op_t.fxp) in der Aufzeichnung; die Wiedergabe kopiert die Tiefe ihres
 * Schatten-Ziels an derselben Stelle -> exakte Wiedergabe bitgleich (verify).
 * Eigene Ressourcen: Satz 0 = Spiel-Thread, Satz 1 = Wiedergabe (Render-Thread). */
#include "render_int.h"
#include "sb_profile.h" /* W229 SBA() */
#include "render_qa.h"
#include "render_fx_rem.h"
#include <math.h>
#include "sb_state.h" /* W232: sb_in_mission */

void sb_render_shot_name(const char *name, ID3D11Resource *src); /* d3d11_render.c */

int g_fxr_bypass;
unsigned g_fxr_reck;
float g_fxr_recp[8];
static unsigned g_fxr_mask = 0xFu; /* A/B: 1 soft, 2 Glut, 4 Licht Welt, 8 Licht Cockpit */

#define FXB_SOFT 1u
#define FXB_EMB 2u
#define FXB_LIGHT 4u
#define FXB_CKP 8u

static int fx_on(int v, unsigned b) { return v > 0 && (g_fxr_mask & b) && !g_fxr_bypass; }

/* Launcher: [remaster] remaster=1 + fx_enhanced=1 (bisher Platzhalter, W103) und
 * keine Einzelwerte gesetzt -> Preset "Remastered dezent" (einmal beim Start). */
static int fx_cfg_done;
static void fx_cfg_once(void)
{
    fx_cfg_done = 1;
    if (g_cfg.remaster && g_cfg.fx_enhanced && !g_cfg.fx_soft && !g_cfg.fx_embers && !g_cfg.fx_light &&
        !g_cfg.fx_light_cockpit && !g_cfg.fx_interp) {
        fxr_preset_apply(1);
        HLOG_I("FXR: remaster=1 fx_enhanced=1 -> Preset dezent (soft=%d glut=%d licht=%d cockpit=%d interp=%d)",
               g_cfg.fx_soft, g_cfg.fx_embers, g_cfg.fx_light, g_cfg.fx_light_cockpit, g_cfg.fx_interp);
    }
}

int fxr_active(void)
{
    if (!fx_cfg_done) { fx_cfg_once(); }
    return g_cfg.fx_soft > 0 || g_cfg.fx_embers > 0 || g_cfg.fx_light > 0 || g_cfg.fx_light_cockpit > 0 ||
           g_cfg.fx_interp || g_cfg.fxr_diag || g_cfg.fxr_ab;
}
int fxr_ckp_active(void) { return g_cfg.fx_light_cockpit > 0 || g_cfg.fxr_ab; }

/* ------------------------------------------------------------------ */
/* Shader                                                              */
/* ------------------------------------------------------------------ */
/* IM-kompatibel (gleiche Eingaenge wie g_im_ps, b0 = mvp + has_tex). */
static const char kFXR_PS[] =
    "cbuffer CB : register(b0) { row_major float4x4 mvp; uint has_tex; };\n"
    "cbuffer FX : register(b2) { float4 fxp; float4 fxq; };\n"
    "Texture2D tex : register(t0);\n"
    "Texture2D<float> dep : register(t4);\n"
    "SamplerState samp : register(s0);\n"
    "struct PSIN { float4 pos : SV_Position; float4 col : COLOR0; float2 uv : TEXCOORD0; };\n"
    "float4 main(PSIN i) : SV_Target {\n"
    "  float4 c = i.col;\n"
    "  if ((has_tex & 0x08000000u) != 0u) {\n"   /* Glutpunkt */
    "    float2 d = i.uv * 2.0 - 1.0; float a = saturate(1.0 - dot(d, d)); a = a * a;\n"
    "    return float4(c.rgb, c.a * a); }\n"
    "  if ((has_tex & 0x10000000u) != 0u) {\n"   /* Licht-Quad: c * L * Abfall (Blend DEST_COLOR/ONE) */
    "    float2 d = (i.pos.xy * fxq.w - fxq.xy) / max(fxq.z, 1.0); float f = saturate(1.0 - dot(d, d));\n"
    "    return float4(c.rgb * (f * f), 0.0); }\n"
    "  if ((has_tex & 1u) != 0) { float4 t = tex.Sample(samp, i.uv); c *= t;\n"
    "    if ((has_tex & 0x1000000u) != 0u && t.a < (0.5 / 255.0)) { discard; } }\n"
    "  uint af = (has_tex >> 8) & 15u;\n"
    "  if (af != 0u) { uint a8 = (uint)(saturate(c.a) * 255.0 + 0.5); uint r8 = (has_tex >> 16) & 255u;\n"
    "    bool ok = (af == 2u) ? (a8 < r8) : (af == 3u) ? (a8 == r8) : (af == 4u) ? (a8 <= r8) :\n"
    "              (af == 5u) ? (a8 > r8) : (af == 6u) ? (a8 != r8) : (af == 7u) ? (a8 >= r8) : (af == 8u);\n"
    "    if (!ok) { discard; } }\n"
    "  if ((has_tex & 0x02000000u) != 0u) {\n"   /* weicher Partikel */
    "    float zs = dep.Load(int3(i.pos.xy, 0));\n"
    "    float wp = fxp.y / (i.pos.z - fxp.x); float ws = fxp.y / (zs - fxp.x);\n"
    "    float f = (zs >= 1.0) ? 1.0 : saturate((ws - wp) * fxp.z);\n"
    "    c.a *= f; if ((has_tex & 0x04000000u) != 0u) { c.rgb *= f; } }\n"
    "  return c;\n"
    "}\n";

/* Cockpit-Licht (Nachbearbeitung): Vollbild-Dreieck, Blend DEST_COLOR/ONE. */
static const char kFXR_PVS[] =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 p = float2((id == 1u) ? 3.0 : -1.0, (id == 2u) ? -3.0 : 1.0); return float4(p, 0.0, 1.0); }\n";
static const char kFXR_PPS[] =
    "cbuffer P : register(b0) { float4 col; float4 geo; };\n" /* col.rgb = warm * L; geo = Zentrum xy, Radius, Grundanteil */
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    "  float2 d = (pos.xy - geo.xy) / max(geo.z, 1.0); float f = geo.w + (1.0 - geo.w) * saturate(1.0 - 0.5 * dot(d, d));\n"
    "  return float4(col.rgb * f, 0.0); }\n";

#define FXR_BPTR(b) ((b)->lpVtbl->GetBufferPointer(b))
#define FXR_BSIZE(b) ((b)->lpVtbl->GetBufferSize(b))
#define FXR_BREL(b) ((b)->lpVtbl->Release(b))

static ID3D11PixelShader *fx_ps;
static ID3D11VertexShader *fx_pvs;
static ID3D11PixelShader *fx_pps;
static ID3D11Buffer *fx_cb[2];   /* b2: Satz 0 Spiel, 1 Wiedergabe */
static ID3D11Buffer *fx_pcb;     /* Cockpit-Licht */
static ID3D11Buffer *fx_vb;      /* Glut-/Licht-Vertices (Spiel-Thread) */
static ID3D11BlendState *fx_bs_add, *fx_bs_light;
static ID3D11DepthStencilState *fx_ds_off;
static ID3D11RasterizerState *fx_rs;
static int fx_state; /* 0 = noch nicht, 1 = bereit, -1 = gescheitert */

#define FXR_EMAX 192u                 /* Glutpunkte gesamt */
#define FXR_VBN (FXR_EMAX * 6u)

typedef struct { ID3D11Texture2D *t; ID3D11ShaderResourceView *srv; unsigned w, h; } fx_dc_t;
static fx_dc_t fx_dc[2][4];           /* Tiefen-Kopien je Satz */

static int fx_init(void)
{
    ID3DBlob *b = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_BLEND_DESC bl;
    D3D11_DEPTH_STENCIL_DESC dsd;
    D3D11_RASTERIZER_DESC rd;
    HRESULT hr = S_OK;
    int k;
    if (fx_state) { return fx_state > 0; }
    fx_state = -1;
    if (!g_dev) { return 0; }
    if (!sb_im_compile(kFXR_PS, "ps_4_0", &b)) { HLOG_E("FXR: Effekt-PS nicht uebersetzt -> Optionen wirkungslos"); return 0; }
    hr = ID3D11Device_CreatePixelShader(g_dev, FXR_BPTR(b), FXR_BSIZE(b), NULL, &fx_ps);
    FXR_BREL(b); b = NULL;
    if (SUCCEEDED(hr) && sb_im_compile(kFXR_PVS, "vs_4_0", &b)) {
        hr = ID3D11Device_CreateVertexShader(g_dev, FXR_BPTR(b), FXR_BSIZE(b), NULL, &fx_pvs);
        FXR_BREL(b); b = NULL;
    } else { hr = E_FAIL; }
    if (SUCCEEDED(hr) && sb_im_compile(kFXR_PPS, "ps_4_0", &b)) {
        hr = ID3D11Device_CreatePixelShader(g_dev, FXR_BPTR(b), FXR_BSIZE(b), NULL, &fx_pps);
        FXR_BREL(b); b = NULL;
    } else { hr = E_FAIL; }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 32;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    for (k = 0; k < 2 && SUCCEEDED(hr); k++) { hr = ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &fx_cb[k]); }
    if (SUCCEEDED(hr)) { hr = ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &fx_pcb); }
    if (SUCCEEDED(hr)) {
        bd.ByteWidth = (UINT)(sizeof(im_vertex_t) * FXR_VBN);
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        hr = ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &fx_vb);
    }
    if (SUCCEEDED(hr)) {
        memset(&bl, 0, sizeof bl);
        bl.RenderTarget[0].BlendEnable = TRUE;
        bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bl.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = ID3D11Device_CreateBlendState(g_dev, &bl, &fx_bs_add);
        if (SUCCEEDED(hr)) {
            bl.RenderTarget[0].SrcBlend = D3D11_BLEND_DEST_COLOR; /* c + c * L */
            bl.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
            hr = ID3D11Device_CreateBlendState(g_dev, &bl, &fx_bs_light);
        }
    }
    if (SUCCEEDED(hr)) {
        memset(&dsd, 0, sizeof dsd);
        dsd.DepthEnable = FALSE;
        dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        hr = ID3D11Device_CreateDepthStencilState(g_dev, &dsd, &fx_ds_off);
    }
    if (SUCCEEDED(hr)) {
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        hr = ID3D11Device_CreateRasterizerState(g_dev, &rd, &fx_rs);
    }
    if (FAILED(hr) || !fx_ps || !fx_pvs || !fx_pps) {
        HLOG_E("FXR: Ressourcen gescheitert (hr=0x%08lX) -> Effekt-Optionen wirkungslos", (unsigned long)hr);
        return 0;
    }
    fx_state = 1;
    HLOG_I("FXR: bereit (soft=%d glut=%d licht=%d cockpit=%d interp=%d)", g_cfg.fx_soft, g_cfg.fx_embers,
           g_cfg.fx_light, g_cfg.fx_light_cockpit, g_cfg.fx_interp);
    return 1;
}

static void fx_cb_write(ID3D11DeviceContext *c, ID3D11Buffer *cb, const float p[8])
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(c, (ID3D11Resource *)cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, p, 32);
        ID3D11DeviceContext_Unmap(c, (ID3D11Resource *)cb, 0);
    }
}

/* Tiefe des gebundenen DSV in eine lesbare Kopie (Satz set). Rueckgabe SRV. */
static ID3D11ShaderResourceView *fx_depth_copy(ID3D11DeviceContext *c, ID3D11DepthStencilView *dsv, int set)
{
    ID3D11Resource *res = NULL;
    D3D11_TEXTURE2D_DESC td;
    fx_dc_t *e = NULL;
    int k;
    static unsigned nlog;
    if (!dsv) { return NULL; }
    ID3D11DepthStencilView_GetResource(dsv, &res);
    if (!res) { return NULL; }
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &td);
    if (td.SampleDesc.Count != 1 || (td.Format != DXGI_FORMAT_D24_UNORM_S8_UINT && td.Format != DXGI_FORMAT_R24G8_TYPELESS)) {
        ID3D11Resource_Release(res);
        return NULL;
    }
    for (k = 0; k < 4; k++) { if (fx_dc[set][k].t && fx_dc[set][k].w == td.Width && fx_dc[set][k].h == td.Height) { e = &fx_dc[set][k]; break; } }
    if (!e) {
        for (k = 0; k < 4 && fx_dc[set][k].t; k++) {}
        if (k == 4) { /* voll: ersten Eintrag ersetzen */
            k = 0;
            if (fx_dc[set][0].srv) { ID3D11ShaderResourceView_Release(fx_dc[set][0].srv); }
            if (fx_dc[set][0].t) { ID3D11Texture2D_Release(fx_dc[set][0].t); }
            memset(&fx_dc[set][0], 0, sizeof fx_dc[set][0]);
        }
        e = &fx_dc[set][k];
        {
            D3D11_TEXTURE2D_DESC nd = td;
            D3D11_SHADER_RESOURCE_VIEW_DESC vd;
            nd.Format = DXGI_FORMAT_R24G8_TYPELESS;
            nd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            nd.Usage = D3D11_USAGE_DEFAULT;
            nd.CPUAccessFlags = 0;
            nd.MiscFlags = 0;
            nd.MipLevels = 1;
            nd.ArraySize = 1;
            if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &nd, NULL, &e->t)) || !e->t) {
                e->t = NULL; ID3D11Resource_Release(res); return NULL;
            }
            memset(&vd, 0, sizeof vd);
            vd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MipLevels = 1;
            if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)e->t, &vd, &e->srv)) || !e->srv) {
                ID3D11Texture2D_Release(e->t); e->t = NULL; e->srv = NULL; ID3D11Resource_Release(res); return NULL;
            }
            e->w = td.Width; e->h = td.Height;
            if (nlog < 8u) { nlog++; HLOG_I("FXR Tiefen-Kopie #%u: Satz %d %ux%u (Quelle fmt %u)", nlog, set, td.Width, td.Height, (unsigned)td.Format); }
        }
    }
    ID3D11DeviceContext_CopyResource(c, (ID3D11Resource *)e->t, res);
    ID3D11Resource_Release(res);
    return e->srv;
}

/* ------------------------------------------------------------------ */
/* Zustand je Bild / je Draw                                           */
/* ------------------------------------------------------------------ */
static unsigned fx_frame = 0xFFFFFFFFu;       /* Present-Nr. des laufenden Bildes */
static int fx_mission;                         /* laufendes Bild: Mission (Szene 0x2C, mloop 1) */
static unsigned fx_n3d[RTT_MAX], fx_n3d_prev[RTT_MAX];
static unsigned fx_done[RTT_MAX];              /* Bild, in dem Zusatz-Draws fuer das RT liefen (+1) */
static float fx_e[RTT_MAX], fx_ex[RTT_MAX], fx_ey[RTT_MAX];    /* Energie + Schwerpunkt (laufendes Bild) */
static float fx_ep[RTT_MAX], fx_epx[RTT_MAX], fx_epy[RTT_MAX]; /* Vorbild */
static float fx_lw[RTT_MAX];                   /* Licht Welt je RT (Tick-Takt) */
static float fx_lmain;                         /* Energie des groessten Welt-RTs (Vorbild) */
static int fx_main_slot = -1;
static unsigned fx_main_pw;

typedef struct {
    int cls;            /* 1 = Effekt-Draw in Welt-RT */
    unsigned bits;      /* has_tex-Zusatz */
    float p[8];         /* b2 */
    float rc[3], rr;    /* Roh-Schwerpunkt/-Radius (Weltraum) */
    int additive, srcone;
    int slot;
    float m[4][4];      /* VS-Matrix (Clip) */
    float scx, scy, ofx, ofy;
} fx_cur_t;
static fx_cur_t fx_cur;

/* Glut: Glutpunkte im Objektraum ihres Emitters (Billboard des Effekt-Draws;
 * die VS-Matrix der Effekte enthaelt die Modell-Transformation, Rohpositionen
 * liegen um den Ursprung). Der Emitter folgt seinem Billboard (gleiches RT,
 * gleicher Rohradius, Bildschirmnaehe) und uebernimmt je Bild dessen Matrix. */
typedef struct { float p[3], v[3], age, life, size, heat; int em; } fx_ember_t;
static fx_ember_t fx_em[FXR_EMAX];
static unsigned fx_em_alive, fx_em_spawned;
typedef struct {
    int used, slot;
    float m[4][4], scx, scy, ofx, ofy;
    float rr, sx, sy, up[3], age;
    unsigned upd;
    float uv[2];   /* UV von Vertex 0 beim letzten Bild (Animation = Explosion/Feuer, statisch = Lampe) */
    int burst;     /* Ausbruch schon erzeugt */
} fx_emit_t;
#define FXR_NEMIT 24
static fx_emit_t fx_emit[FXR_NEMIT];
static unsigned fx_rng = 0x13572468u;

static float fx_rand(void) /* 0..1, deterministisch */
{
    fx_rng = fx_rng * 1664525u + 1013904223u;
    return (float)(fx_rng >> 8) * (1.0f / 16777216.0f);
}

static void fx_sim_step(void)
{
    const float dt = 1.0f / 30.0f; /* ein Spiel-Tick */
    unsigned i, alive = 0, nem[FXR_NEMIT];
    memset(nem, 0, sizeof nem);
    for (i = 0; i < FXR_EMAX; i++) {
        fx_ember_t *e = &fx_em[i];
        const fx_emit_t *m;
        float g, drag;
        int k;
        if (e->life <= 0.0f) { continue; }
        e->age += dt;
        if (e->age >= e->life || e->em < 0 || e->em >= FXR_NEMIT) { e->life = 0.0f; continue; }
        m = &fx_emit[e->em];
        g = m->rr * 2.6f; /* Schwerkraft relativ zur Billboard-Groesse (Massstab des Objektraums) */
        drag = 1.0f - 1.1f * dt;
        for (k = 0; k < 3; k++) {
            e->v[k] = e->v[k] * drag - m->up[k] * g * dt;
            e->p[k] += e->v[k] * dt;
        }
        nem[e->em]++;
        alive++;
    }
    for (i = 0; i < (unsigned)FXR_NEMIT; i++) {
        if (!fx_emit[i].used) { continue; }
        fx_emit[i].age += dt;
        if (g_present_no - fx_emit[i].upd > 6u && !nem[i]) { fx_emit[i].used = 0; } /* Billboard weg, Glut verloschen */
    }
    fx_em_alive = alive;
}

static void fx_new_frame(void)
{
    int s;
    fx_frame = g_present_no;
    fx_mission = rq_in_mission();
    fx_lmain = 0.0f;
    fx_main_slot = -1; fx_main_pw = 0;
    for (s = 0; s < g_rtt_n && s < RTT_MAX; s++) {
        float ep = fx_e[s];
        fx_ep[s] = ep;
        fx_epx[s] = ep > 0.0f ? fx_ex[s] / ep : 0.0f;
        fx_epy[s] = ep > 0.0f ? fx_ey[s] / ep : 0.0f;
        fx_e[s] = fx_ex[s] = fx_ey[s] = 0.0f;
        fx_n3d_prev[s] = fx_n3d[s];
        fx_n3d[s] = 0;
        /* Licht: sofort hoch, Abklingen ~0,2 s */
        {
            float tgt = ep * 3.0f;
            if (tgt > 1.0f) { tgt = 1.0f; }
            fx_lw[s] = fx_lw[s] * 0.82f;
            if (tgt > fx_lw[s]) { fx_lw[s] = tgt; }
        }
        if ((fx_n3d_prev[s] >= 32u) && g_rtt[s].pw > fx_main_pw) { fx_main_pw = g_rtt[s].pw; fx_main_slot = s; }
    }
    if (fx_main_slot >= 0) { fx_lmain = fx_ep[fx_main_slot]; }
    if (fx_mission) { fx_sim_step(); }
}

static int fx_world_slot(int s)
{
    return s >= 0 && s < RTT_MAX && (fx_n3d[s] >= 32u || fx_n3d_prev[s] >= 32u);
}

/* ------------------------------------------------------------------ */
/* render_im.c-Hooks                                                   */
/* ------------------------------------------------------------------ */
int fxr_pre(int vsh_em, const float m[4][4], const float zadd[4], const float *pos, size_t pst, unsigned n, int rhw,
            unsigned prim)
{
    unsigned on = 0, bs = 0, bd = 0, tf = 0, i;
    int tl = 0, slot;
    fx_cur.cls = 0; fx_cur.bits = 0;
    g_fxr_reck = 0;
    if (g_present_no != fx_frame) { fx_new_frame(); }
    if (!fx_mission || !rtt_active()) { return 0; }
    slot = g_rtt_bound;
    if (slot < 0 || slot >= RTT_MAX) { return 0; }
    if (!rhw && vsh_em != 2) { fx_n3d[slot]++; return 0; }
    if (vsh_em != 2 || g_submit_vb || n < 3u || prim < 5u || !fx_world_slot(slot)) { return 0; }
    sb_render_blend_state(&on, &bs, &bd);
    if (!on || rs_dmask != 0u) { return 0; }
    if (!sb_tex_xptr()) { return 0; }
    sb_tex_fmt(&tf, &tl);
    tf = (tf >> 8) & 0xFFu;
    if (tl || (tf != 0x0Cu && tf != 0x0Eu && tf != 0x0Fu)) { return 0; }
    fx_cur.cls = 1;
    fx_cur.slot = slot;
    fx_cur.additive = (bd == 2u);
    fx_cur.srcone = (bs == 2u);
    memcpy(fx_cur.m, m, sizeof fx_cur.m);
    fx_cur.m[2][3] += zadd[2];
    {   /* Bildschirm-Abbildung wie render_im.c (RT-Pfad der VSH-Emulation) */
        float vpw = (float)(g_vp_game.w ? g_vp_game.w : 640);
        float vph = (float)(g_vp_game.h ? g_vp_game.h : 480);
        fx_cur.scx = vpw * 0.5f; fx_cur.scy = -vph * 0.5f;
        fx_cur.ofx = vpw * 0.5f + (float)g_vp_game.x;
        fx_cur.ofy = vph * 0.5f + (float)g_vp_game.y;
    }
    {   /* Roh-Schwerpunkt/-Radius */
        float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
        int k;
        for (i = 0; i < n; i++) {
            const float *p = pos + (size_t)i * pst;
            for (k = 0; k < 3; k++) { if (p[k] < mn[k]) { mn[k] = p[k]; } if (p[k] > mx[k]) { mx[k] = p[k]; } }
        }
        for (k = 0; k < 3; k++) { fx_cur.rc[k] = 0.5f * (mn[k] + mx[k]); }
        fx_cur.rr = 0.5f * sqrtf((mx[0] - mn[0]) * (mx[0] - mn[0]) + (mx[1] - mn[1]) * (mx[1] - mn[1]) +
                                 (mx[2] - mn[2]) * (mx[2] - mn[2]));
    }
    g_fxr_reck = 1;
    if (fx_on(g_cfg.fx_soft, FXB_SOFT) && (bs == 5u || bs == 2u) && fx_cur.rr > 0.0f) {
        const float *r2 = fx_cur.m[2], *r3 = fx_cur.m[3];
        float d33 = r3[0] * r3[0] + r3[1] * r3[1] + r3[2] * r3[2];
        float d22 = r2[0] * r2[0] + r2[1] * r2[1] + r2[2] * r2[2];
        if (d33 > 1e-12f && d22 > 1e-12f) {
            float A = (r2[0] * r3[0] + r2[1] * r3[1] + r2[2] * r3[2]) / d33;
            float B = r2[3] - A * r3[3];
            float ex = r2[0] - A * r3[0], ey = r2[1] - A * r3[1], ez = r2[2] - A * r3[2];
            float res = sqrtf((ex * ex + ey * ey + ez * ez) / d22);
            float range = fx_cur.rr * 0.2f * (float)g_cfg.fx_soft / 100.0f;
            static unsigned nlog;
            if (res < 0.02f && fabsf(B) > 1e-9f && range > 1e-6f) {
                memset(fx_cur.p, 0, sizeof fx_cur.p);
                fx_cur.p[0] = A; fx_cur.p[1] = B; fx_cur.p[2] = 1.0f / range;
                fx_cur.bits = FXR_BIT_SOFT | (fx_cur.srcone ? FXR_BIT_RGB : 0u);
            }
            if (nlog < 6u || (g_cfg.fxr_diag && nlog < 40u)) {
                nlog++;
                HLOG_I("FXR soft #%u f=%u slot=%d vs=0x%X A=%.6f B=%.6f rest=%.4f rr=%.3f reichweite=%.3f bl=%u/%u -> %s",
                       nlog, g_present_no, slot, g_im_shader, A, B, res, fx_cur.rr, range, bs, bd,
                       fx_cur.bits ? "weich" : "aus (Projektion)");
            }
        }
    }
    if (fx_cur.bits && g_rq_active) { g_rq_active = 0; } /* Shading-Pfad: Effekt ueber den IM-Pfad (gleiche Rechnung) */
    return 1;
}

unsigned fxr_draw_bits(void)
{
    if (fx_cur.bits) { memcpy(g_fxr_recp, fx_cur.p, sizeof g_fxr_recp); }
    return fx_cur.bits;
}

static int fx_ldirty = 1;
static void *fx_ldsv;
void fxr_note_other(void) { fx_ldirty = 1; }

void fxr_bind(void)
{
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11DepthStencilView *dsv = NULL;
    ID3D11ShaderResourceView *srv;
    if (!fx_cur.bits) { fx_ldirty = 1; return; }
    if (!fx_init()) { return; }
    ID3D11DeviceContext_OMGetRenderTargets(g_ctx, 1, &rtv, &dsv);
    if (rtv) { ID3D11RenderTargetView_Release(rtv); }
    if (!dsv) { return; }
    if (fx_ldirty || (void *)dsv != fx_ldsv) {
        srv = fx_depth_copy(g_ctx, dsv, 0);
        fx_ldirty = 0; fx_ldsv = dsv;
    } else {
        srv = NULL;
        { int k; for (k = 0; k < 4; k++) { if (fx_dc[0][k].t) { D3D11_TEXTURE2D_DESC td; ID3D11Resource *r = NULL;
              ID3D11DepthStencilView_GetResource(dsv, &r);
              if (r) { ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &td); ID3D11Resource_Release(r);
                       if (td.Width == fx_dc[0][k].w && td.Height == fx_dc[0][k].h) { srv = fx_dc[0][k].srv; break; } } } } }
    }
    ID3D11DepthStencilView_Release(dsv);
    if (!srv) { return; }
    fx_cb_write(g_ctx, fx_cb[0], fx_cur.p);
    ID3D11DeviceContext_PSSetShader(g_ctx, fx_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 2, 1, &fx_cb[0]);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 4, 1, &srv);
}

/* Zusatz-Draw (Glut/Licht) mit dem Zustand des eben gezeichneten Effekts
 * (RT, DSV, Viewport, Tiefentest ohne Schreiben); eigener Blend/PS/VB. */
static void fx_extra_draw(const im_vertex_t *v, unsigned n, unsigned bits, const float p[8], int light)
{
    D3D11_MAPPED_SUBRESOURCE map;
    float mvp[16];
    unsigned rw = 0, rh = 0;
    UINT stride = sizeof(im_vertex_t), off = 0;
    ID3D11BlendState *sbs = NULL; FLOAT sbf[4]; UINT smask = 0;
    ID3D11DepthStencilState *sds = NULL; UINT sref = 0;
    ID3D11RasterizerState *srs = NULL;
    rtt_cur_dims(&rw, &rh);
    if (!rw || !rh || !n) { return; }
    memset(mvp, 0, sizeof mvp);
    mvp[0] = 2.0f / (float)rw; mvp[5] = -2.0f / (float)rh; mvp[10] = 1.0f;
    mvp[12] = -1.0f; mvp[13] = 1.0f; mvp[15] = 1.0f;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)fx_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) { return; }
    memcpy(map.pData, v, n * sizeof *v);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)fx_vb, 0);
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_im_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) { return; }
    memcpy(map.pData, mvp, sizeof mvp);
    *(unsigned *)((unsigned char *)map.pData + 64) = bits;
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_im_cb, 0);
    fx_cb_write(g_ctx, fx_cb[0], p);
    ID3D11DeviceContext_OMGetBlendState(g_ctx, &sbs, sbf, &smask);
    ID3D11DeviceContext_OMGetDepthStencilState(g_ctx, &sds, &sref);
    ID3D11DeviceContext_RSGetState(g_ctx, &srs);
    {
        static const FLOAT one[4] = { 1, 1, 1, 1 };
        ID3D11DeviceContext_OMSetBlendState(g_ctx, light ? fx_bs_light : fx_bs_add, one, 0xFFFFFFFFu);
    }
    if (light) { ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, fx_ds_off, 0); }
    ID3D11DeviceContext_RSSetState(g_ctx, fx_rs);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, g_im_layout);
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &fx_vb, &stride, &off);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_im_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 0, 1, &g_im_cb);
    ID3D11DeviceContext_PSSetShader(g_ctx, fx_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_im_cb);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 2, 1, &fx_cb[0]);
    if (g_ip_recording) { /* 120 FPS: Zusatz-Draw mit Parametern aufzeichnen (Zustand jetzt gebunden) */
        g_fxr_reck = 2;
        memcpy(g_fxr_recp, p, sizeof g_fxr_recp);
        g_ip_sigparts[0] = 0xF0E3u; g_ip_sigparts[1] = 0; g_ip_sigparts[2] = bits;
        sb_interp_rec_draw(v, n, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, mvp, bits, 1,
                           0x3B3E0000u ^ bits ^ (light ? 0x55u : 0x77u), 0x3B3E0000u ^ n, 0xFFFFFFFFu);
        g_fxr_reck = 0;
        /* rec_draw kann ein faelliges Zwischenbild zeigen (Zustand gesichert/zurueck) -> VB/CB neu */
        ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &fx_vb, &stride, &off);
    }
    ID3D11DeviceContext_Draw(g_ctx, n, 0);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, sbs, sbf, smask);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, sds, sref);
    ID3D11DeviceContext_RSSetState(g_ctx, srs);
    if (sbs) { ID3D11BlendState_Release(sbs); }
    if (sds) { ID3D11DepthStencilState_Release(sds); }
    if (srs) { ID3D11RasterizerState_Release(srs); }
}

/* additiver Effekt-Draw (Bildschirm-Schwerpunkt cx/cy, Groesse sz in RT-Pixeln):
 * vorhandenen Emitter nachfuehren oder neuen anlegen + Ausbruch */
static void fx_embers_note(float cx, float cy, float sz, const float uv[2])
{
    unsigned i, want, got = 0;
    int fe = -1, hit = -1;
    float r = fx_cur.rr, lim = sz > 8.0f ? sz : 8.0f;
    for (i = 0; i < (unsigned)FXR_NEMIT; i++) {
        fx_emit_t *e = &fx_emit[i];
        if (!e->used) { if (fe < 0) { fe = (int)i; } continue; }
        if (e->slot == fx_cur.slot && fabsf(e->rr - r) <= 0.02f * r && g_present_no - e->upd <= 4u &&
            fabsf(e->sx - cx) < lim && fabsf(e->sy - cy) < lim) { hit = (int)i; break; }
    }
    if (hit >= 0) { /* Billboard dieses Emitters: Matrix/Lage nachfuehren */
        fx_emit_t *e = &fx_emit[hit];
        memcpy(e->m, fx_cur.m, sizeof e->m);
        e->scx = fx_cur.scx; e->scy = fx_cur.scy; e->ofx = fx_cur.ofx; e->ofy = fx_cur.ofy;
        e->sx = cx; e->sy = cy; e->upd = g_present_no;
        if (e->burst || (fabsf(uv[0] - e->uv[0]) < 1e-4f && fabsf(uv[1] - e->uv[1]) < 1e-4f)) {
            e->uv[0] = uv[0]; e->uv[1] = uv[1];
            return; /* statisch (Lampe/Leuchte) oder Ausbruch schon erzeugt */
        }
        e->uv[0] = uv[0]; e->uv[1] = uv[1];
        e->burst = 1;
        fe = hit; /* animiertes Billboard (Explosion/Feuer): einmal Ausbruch */
    } else {
    if (fe < 0 || r <= 0.0f) { return; }
    {
        fx_emit_t *e = &fx_emit[fe];
        float ux = fx_cur.m[1][0], uy = fx_cur.m[1][1], uz = fx_cur.m[1][2];
        float l = sqrtf(ux * ux + uy * uy + uz * uz);
        memset(e, 0, sizeof *e);
        e->used = 1; e->slot = fx_cur.slot;
        memcpy(e->m, fx_cur.m, sizeof e->m);
        e->scx = fx_cur.scx; e->scy = fx_cur.scy; e->ofx = fx_cur.ofx; e->ofy = fx_cur.ofy;
        e->rr = r; e->sx = cx; e->sy = cy; e->upd = g_present_no;
        /* "oben" = Bildschirm-y-Zeile der Matrix im Objektraum */
        if (l > 1e-12f) { e->up[0] = ux / l; e->up[1] = uy / l; e->up[2] = uz / l; } else { e->up[1] = 1.0f; }
        e->uv[0] = uv[0]; e->uv[1] = uv[1];
    }
    return; /* neu: erst beim naechsten Bild mit geaenderter UV (Animation) ausbrechen */
    }
    want = (unsigned)(18.0f * (float)g_cfg.fx_embers / 100.0f + 0.5f);
    if (want < 2u) { want = 2u; }
    fx_rng ^= (unsigned)(cx * 131.0f) ^ ((unsigned)(cy * 977.0f) << 7) ^ g_present_no;
    for (i = 0; i < FXR_EMAX && got < want; i++) {
        fx_ember_t *e = &fx_em[i];
        const float *up = fx_emit[fe].up;
        float d[3], l, sp;
        int k;
        if (e->life > 0.0f) { continue; }
        for (k = 0; k < 3; k++) { d[k] = fx_rand() * 2.0f - 1.0f; }
        l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l < 1e-3f) { d[0] = 0.0f; d[1] = 0.0f; d[2] = 0.0f; l = 1.0f; }
        sp = r * (1.0f + 1.4f * fx_rand());
        for (k = 0; k < 3; k++) {
            e->p[k] = fx_cur.rc[k] + d[k] / l * r * 0.3f * fx_rand();
            e->v[k] = (d[k] / l * 0.9f + up[k] * 1.0f) * sp;
        }
        e->age = 0.0f;
        e->life = 0.6f + 0.8f * fx_rand();
        e->size = r * (0.02f + 0.015f * fx_rand());
        e->heat = 0.7f + 0.3f * fx_rand();
        e->em = fe;
        got++;
    }
    fx_em_spawned += got;
    if (g_cfg.fxr_diag && fx_em_spawned <= 400u) {
        HLOG_I("FXR glut f=%u neu=%u (gesamt %u) emitter=%d slot=%d bild=(%.1f,%.1f) r=%.3f vs=0x%X", g_present_no, got,
               fx_em_spawned, fe, fx_cur.slot, cx, cy, r, g_im_shader);
    }
}

static void fx_embers_draw(int slot)
{
    static im_vertex_t v[FXR_VBN];
    unsigned i, nv = 0, nvis = 0;
    float k = (float)g_cfg.fx_embers / 100.0f;
    float zp[8];
    if (k > 1.0f) { k = 1.0f; }
    memset(zp, 0, sizeof zp);
    for (i = 0; i < FXR_EMAX; i++) {
        const fx_ember_t *e = &fx_em[i];
        im_vertex_t q;
        unsigned j;
        static const float co[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
        memset(&q, 0, sizeof q);
        q.pos[3] = 1.0f;
        if (e->life > 0.0f && e->em >= 0 && fx_emit[e->em].slot == slot) {
            const fx_emit_t *em = &fx_emit[e->em];
            const float (*m)[4] = (const float (*)[4])em->m;
            float cx = e->p[0] * m[0][0] + e->p[1] * m[0][1] + e->p[2] * m[0][2] + m[0][3];
            float cy = e->p[0] * m[1][0] + e->p[1] * m[1][1] + e->p[2] * m[1][2] + m[1][3];
            float cz = e->p[0] * m[2][0] + e->p[1] * m[2][1] + e->p[2] * m[2][2] + m[2][3];
            float cw = e->p[0] * m[3][0] + e->p[1] * m[3][1] + e->p[2] * m[3][2] + m[3][3];
            if (cw > 1e-6f && cz > 0.0f && cz < cw) {
                float sxm = sqrtf(m[0][0] * m[0][0] + m[0][1] * m[0][1] + m[0][2] * m[0][2]);
                float px = cx / cw * em->scx + em->ofx;
                float py = cy / cw * em->scy + em->ofy;
                float pz = cz / cw;
                float s = e->size * sxm * em->scx / cw;
                float t = e->age / e->life, a;
                if (s < 0.7f) { s = 0.7f; }
                if (s > 8.0f) { s = 8.0f; }
                a = (1.0f - t); a = a * sqrtf(a) * k * e->heat;
                q.col[0] = 1.0f; q.col[1] = 0.8f - 0.45f * t; q.col[2] = 0.4f - 0.32f * t; q.col[3] = a;
                for (j = 0; j < 6u; j++) {
                    v[nv + j] = q;
                    v[nv + j].pos[0] = px + (co[j][0] * 2.0f - 1.0f) * s;
                    v[nv + j].pos[1] = py + (co[j][1] * 2.0f - 1.0f) * s;
                    v[nv + j].pos[2] = pz;
                    v[nv + j].uv[0] = co[j][0]; v[nv + j].uv[1] = co[j][1];
                }
                nv += 6u;
                nvis++;
                continue;
            }
        }
        for (j = 0; j < 6u; j++) { v[nv + j] = q; } /* tot/unsichtbar: entartet (feste Laenge -> Interp-Partner) */
        nv += 6u;
    }
    if (nvis) { fx_extra_draw(v, nv, FXR_BIT_SPARK, zp, 0); }
}

static void fx_light_draw(int slot)
{
    im_vertex_t v[6];
    float p[8], L = fx_lw[slot] * 0.45f * (float)g_cfg.fx_light / 100.0f; /* 100 %: hoechstens +45 % */
    unsigned rw = 0, rh = 0, j;
    static const float co[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    rtt_cur_dims(&rw, &rh);
    if (L < 0.01f || !rw || !rh) { return; }
    memset(v, 0, sizeof v);
    memset(p, 0, sizeof p);
    p[4] = fx_epx[slot]; p[5] = fx_epy[slot];
    p[6] = 0.55f * (float)(rw > rh ? rw : rh);
    p[7] = g_rtt[slot].pw ? (float)g_rtt[slot].w / (float)g_rtt[slot].pw : 1.0f; /* Ziel-Pixel -> Xbox-Pixel */
    for (j = 0; j < 6u; j++) {
        v[j].pos[0] = co[j][0] * (float)rw; v[j].pos[1] = co[j][1] * (float)rh; v[j].pos[2] = 0.0f; v[j].pos[3] = 1.0f;
        v[j].col[0] = L * 1.0f; v[j].col[1] = L * 0.62f; v[j].col[2] = L * 0.30f; v[j].col[3] = 1.0f;
    }
    fx_extra_draw(v, 6u, FXR_BIT_LIGHT, p, 1);
}

void fxr_after(const im_vertex_t *ov, unsigned n)
{
    int slot = fx_cur.slot;
    if (fx_cur.bits) {
        ID3D11ShaderResourceView *nul = NULL;
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 4, 1, &nul);
    }
    if (!fx_cur.cls) { return; }
    /* Energie der additiven Effekte (Licht) */
    if (fx_cur.additive && ov && n) {
        float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mxy = -1e30f, lum = 0.0f, al = 0.0f, ar;
        unsigned rw = 0, rh = 0, i;
        rtt_cur_dims(&rw, &rh);
        for (i = 0; i < n; i++) {
            float c = ov[i].col[0];
            if (ov[i].col[1] > c) { c = ov[i].col[1]; }
            if (ov[i].col[2] > c) { c = ov[i].col[2]; }
            lum += c; al += ov[i].col[3];
            if (ov[i].pos[0] < mnx) { mnx = ov[i].pos[0]; } if (ov[i].pos[0] > mxx) { mxx = ov[i].pos[0]; }
            if (ov[i].pos[1] < mny) { mny = ov[i].pos[1]; } if (ov[i].pos[1] > mxy) { mxy = ov[i].pos[1]; }
        }
        lum /= (float)n; al /= (float)n;
        if (rw && rh && mxx > mnx && mxy > mny) {
            float cxp = 0.5f * (mnx + mxx), cyp = 0.5f * (mny + mxy), e;
            if (mnx < 0.0f) { mnx = 0.0f; } if (mxx > (float)rw) { mxx = (float)rw; }
            if (mny < 0.0f) { mny = 0.0f; } if (mxy > (float)rh) { mxy = (float)rh; }
            ar = (mxx > mnx && mxy > mny) ? (mxx - mnx) * (mxy - mny) / ((float)rw * (float)rh) : 0.0f;
            e = ar * lum * (fx_cur.srcone ? 1.0f : al);
            if (e > 0.0f) { fx_e[slot] += e; fx_ex[slot] += e * cxp; fx_ey[slot] += e * cyp; }
            /* Glut: auch waehrend A/B-Original (Szene bleibt gleich, nur Zeichnen faellt weg) */
            if (g_cfg.fx_embers > 0) { fx_embers_note(cxp, cyp, 0.5f * ((mxx - mnx) + (mxy - mny)), ov[0].uv); }
        }
    }
    /* Zusatz-Draws einmal je Bild und Welt-RT, nach dem ersten Effekt-Draw */
    if (fx_done[slot] == g_present_no + 1u) { return; }
    fx_done[slot] = g_present_no + 1u;
    if (!fx_init()) { return; }
    if (fx_on(g_cfg.fx_light, FXB_LIGHT)) { fx_light_draw(slot); }
    if (fx_on(g_cfg.fx_embers, FXB_EMB) && fx_em_alive) { fx_embers_draw(slot); }
    fx_ldirty = 1;
}

/* ------------------------------------------------------------------ */
/* 120 FPS: Wiedergabe                                                 */
/* ------------------------------------------------------------------ */
void fxr_rep_begin(fxr_rep_t *s) { memset(s, 0, sizeof *s); s->dirty = 1; }

int fxr_rep_op(ID3D11DeviceContext *c, fxr_rep_t *s, unsigned has_tex, const float fxp[8],
               ID3D11DepthStencilView *dsv, int deferred)
{
    (void)deferred;
    if (!(has_tex & FXR_MASK)) {
        s->dirty = 1;
        if (s->bound) { ID3D11DeviceContext_PSSetShader(c, g_im_ps, NULL, 0); s->bound = 0; }
        return 0;
    }
    if (fx_state <= 0 && !fx_init()) { return 0; }
    if (has_tex & FXR_BIT_SOFT) {
        ID3D11ShaderResourceView *srv = NULL;
        if (s->dirty || (void *)dsv != s->dsv) {
            srv = fx_depth_copy(c, dsv, 1);
            s->dirty = 0; s->dsv = dsv;
        } else if (dsv) {
            int k; ID3D11Resource *r = NULL; D3D11_TEXTURE2D_DESC td;
            ID3D11DepthStencilView_GetResource(dsv, &r);
            if (r) {
                ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &td); ID3D11Resource_Release(r);
                for (k = 0; k < 4; k++) { if (fx_dc[1][k].t && fx_dc[1][k].w == td.Width && fx_dc[1][k].h == td.Height) { srv = fx_dc[1][k].srv; break; } }
            }
        }
        ID3D11DeviceContext_PSSetShaderResources(c, 4, 1, &srv);
        s->used = 1;
    } else {
        s->dirty = 1; /* Glut/Licht schreiben Farbe, keine Tiefe; Kopie bleibt gueltig - konservativ neu */
    }
    fx_cb_write(c, fx_cb[1], fxp);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 2, 1, &fx_cb[1]);
    if (!s->bound) { ID3D11DeviceContext_PSSetShader(c, fx_ps, NULL, 0); s->bound = 1; }
    return 1;
}

void fxr_rep_end(ID3D11DeviceContext *c, fxr_rep_t *s)
{
    if (s->used) { ID3D11ShaderResourceView *nul = NULL; ID3D11DeviceContext_PSSetShaderResources(c, 4, 1, &nul); }
    if (s->bound) { ID3D11DeviceContext_PSSetShader(c, g_im_ps, NULL, 0); s->bound = 0; }
}

/* Statistik der Zuordnung (render_interp.c, nur fxr_diag) */
static unsigned fx_ist[3][12];
void fxr_interp_note(unsigned fxk, unsigned why, int lerp)
{
    if (fxk > 2u) { fxk = 0u; }
    if (why > 9u) { why = 9u; }
    fx_ist[fxk][why]++;
    if (lerp) { fx_ist[fxk][10]++; }
    fx_ist[fxk][11]++;
}

/* ------------------------------------------------------------------ */
/* Cockpit-Licht (Nachbearbeitung, je gezeigtem Bild)                  */
/* ------------------------------------------------------------------ */
static float fx_lc;            /* aktuelle Cockpit-Lichtstaerke */
static LONGLONG fx_lc_t;       /* letzte Auswertung (QPC) */

void fxr_post(ID3D11RenderTargetView *rtv, ID3D11Resource *res)
{
    LARGE_INTEGER q, f;
    float tgt, dt, L, p[8];
    D3D11_TEXTURE2D_DESC td;
    unsigned bx, by, bw, bh;
    D3D11_VIEWPORT vp, svp;
    UINT nvp = 1, smask = 0, sref = 0;
    ID3D11RenderTargetView *srtv = NULL; ID3D11DepthStencilView *sdsv = NULL;
    ID3D11BlendState *sbs = NULL; FLOAT sbf[4];
    ID3D11DepthStencilState *sds = NULL; ID3D11RasterizerState *srs = NULL;
    ID3D11VertexShader *svs = NULL; ID3D11PixelShader *sps = NULL;
    ID3D11InputLayout *slay = NULL; D3D11_PRIMITIVE_TOPOLOGY stopo;
    ID3D11Buffer *spcb = NULL;
    static const FLOAT one[4] = { 1, 1, 1, 1 };
    if (!rtv || !res || !fx_on(g_cfg.fx_light_cockpit, FXB_CKP) || !rq_in_mission()) { fx_lc = 0.0f; return; }
    QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
    dt = fx_lc_t ? (float)((double)(q.QuadPart - fx_lc_t) / (double)f.QuadPart) : 0.0f;
    if (dt > 0.25f) { dt = 0.25f; }
    fx_lc_t = q.QuadPart;
    tgt = fx_lmain * 3.0f;
    if (tgt > 1.0f) { tgt = 1.0f; }
    fx_lc *= expf(-dt / 0.18f);
    if (tgt > fx_lc) { fx_lc = tgt; }
    L = fx_lc * 0.35f * (float)g_cfg.fx_light_cockpit / 100.0f; /* 100 %: hoechstens +35 % */
    if (L < 0.004f || !fx_init()) { return; }
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &td);
    sb_aspect_box(&bx, &by, &bw, &bh);
    memset(p, 0, sizeof p);
    p[0] = L * 1.0f; p[1] = L * 0.62f; p[2] = L * 0.32f; p[3] = 1.0f;
    p[4] = (float)bx + 0.5f * (float)bw; p[5] = (float)by + 0.38f * (float)bh; /* Hauptmonitor oben in der Box */
    p[6] = 0.7f * (float)bh; p[7] = 0.35f;
    /* Zustand sichern */
    ID3D11DeviceContext_OMGetRenderTargets(g_ctx, 1, &srtv, &sdsv);
    ID3D11DeviceContext_OMGetBlendState(g_ctx, &sbs, sbf, &smask);
    ID3D11DeviceContext_OMGetDepthStencilState(g_ctx, &sds, &sref);
    ID3D11DeviceContext_RSGetState(g_ctx, &srs);
    ID3D11DeviceContext_RSGetViewports(g_ctx, &nvp, &svp);
    ID3D11DeviceContext_VSGetShader(g_ctx, &svs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(g_ctx, &sps, NULL, NULL);
    ID3D11DeviceContext_IAGetInputLayout(g_ctx, &slay);
    ID3D11DeviceContext_IAGetPrimitiveTopology(g_ctx, &stopo);
    ID3D11DeviceContext_PSGetConstantBuffers(g_ctx, 0, 1, &spcb);
    fx_cb_write(g_ctx, fx_pcb, p);
    vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = (FLOAT)td.Width; vp.Height = (FLOAT)td.Height;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, fx_bs_light, one, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, fx_ds_off, 0);
    ID3D11DeviceContext_RSSetState(g_ctx, fx_rs);
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_ctx, fx_pvs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, fx_pps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &fx_pcb);
    ID3D11DeviceContext_Draw(g_ctx, 3, 0);
    /* zurueck */
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &srtv, sdsv);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, sbs, sbf, smask);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, sds, sref);
    ID3D11DeviceContext_RSSetState(g_ctx, srs);
    if (nvp) { ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &svp); }
    ID3D11DeviceContext_VSSetShader(g_ctx, svs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, sps, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, slay);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, stopo);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &spcb);
    if (srtv) { ID3D11RenderTargetView_Release(srtv); }
    if (sdsv) { ID3D11DepthStencilView_Release(sdsv); }
    if (sbs) { ID3D11BlendState_Release(sbs); }
    if (sds) { ID3D11DepthStencilState_Release(sds); }
    if (srs) { ID3D11RasterizerState_Release(srs); }
    if (svs) { ID3D11VertexShader_Release(svs); }
    if (sps) { ID3D11PixelShader_Release(sps); }
    if (slay) { ID3D11InputLayout_Release(slay); }
    if (spcb) { ID3D11Buffer_Release(spcb); }
}

/* ------------------------------------------------------------------ */
/* Present: Diagnose, A/B-Bildpaare                                    */
/* ------------------------------------------------------------------ */
static int fx_ab_state;          /* 0 warten, 1 = dieses Bild Original (A), 2 = dieses Bild Optionen (B) */
static unsigned fx_ab_n, fx_ab_last, fx_ab_fa;
static const unsigned kFxAbMask[5] = { FXB_SOFT, FXB_EMB, FXB_LIGHT, FXB_CKP, 0xFu };
static const char *const kFxAbName[5] = { "Soft Particles", "Glut/Funken", "Explosions-Licht Welt",
                                          "Explosions-Licht Cockpit", "alle Effekt-Optionen" };

void fxr_present(unsigned n)
{
    static unsigned lastlog;
    (void)n;
    if (g_cfg.fxr_diag && g_present_no - lastlog >= 600u && rq_in_mission()) {
        unsigned k;
        lastlog = g_present_no;
        HLOG_I("FXR stat f=%u glut=%u (neu gesamt %u) licht_main=%.3f slot=%d lc=%.3f", g_present_no, fx_em_alive,
               fx_em_spawned, fx_lmain, fx_main_slot, fx_lc);
        for (k = 1; k < 3u; k++) {
            unsigned *s = fx_ist[k];
            if (!s[11]) { continue; }
            HLOG_I("FXR interp %s: n=%u lerp=%u | static=%u lerp=%u rawlerp=%u nomatch=%u rawchg=%u reject=%u rt2dlerp=%u rt2dex=%u",
                   k == 1 ? "spiel" : "zusatz", s[11], s[10], s[1], s[2], s[3], s[4], s[5], s[6], s[8], s[9]);
        }
    }
}

void fxr_shot_tick(unsigned n)
{
    char nm[48];
    if (!g_cfg.fxr_ab) { return; }
    if (fx_ab_state == 1) { /* Bild A (Original) fertig */
        _snprintf_s(nm, sizeof nm, _TRUNCATE, "fxab_%02u_a_%06u", fx_ab_n, n);
        sb_render_shot_name(nm, NULL);
        fx_ab_fa = n;
        g_fxr_bypass = 0;
        fx_ab_state = 2;
        return;
    }
    if (fx_ab_state == 2) { /* Bild B (Option) fertig; naechstes Bild wieder Original (Klammer C) */
        unsigned k = fx_ab_n % 5u;
        _snprintf_s(nm, sizeof nm, _TRUNCATE, "fxab_%02u_b_%06u", fx_ab_n, n);
        sb_render_shot_name(nm, NULL);
        HLOG_I("FXR ab #%u f=%u A=fxab_%02u_a_%06u B=fxab_%02u_b_%06u opt=%s glut=%u licht=%.3f", fx_ab_n, n, fx_ab_n,
               fx_ab_fa, fx_ab_n, n, kFxAbName[k], fx_em_alive, fx_lmain);
        g_fxr_bypass = 1;
        fx_ab_state = 3;
        return;
    }
    if (fx_ab_state == 3) { /* Bild C (wieder Original): A/C zeigen die Bewegung eines Bildes ohne Option */
        _snprintf_s(nm, sizeof nm, _TRUNCATE, "fxab_%02u_c_%06u", fx_ab_n, n);
        sb_render_shot_name(nm, NULL);
        fx_ab_n++;
        fx_ab_last = n;
        g_fxr_mask = 0xFu;
        g_fxr_bypass = 0;
        fx_ab_state = 0;
        return;
    }
    /* Ausloeser: Explosion im Vorbild (Energie), Abstand >= 45 Bilder */
    if (!sb_in_mission()) { return; } /* nur Kampagnen-Mission (Titel-Demo zaehlt nicht, wie W106; W232 statt Ladezaehler == 4) */
    if (fx_ab_n < (unsigned)g_cfg.fxr_ab && rq_in_mission() && n - fx_ab_last >= 45u &&
        (fx_lmain > 0.004f || (fx_em_alive >= 6u && (fx_ab_n % 5u) == 1u))) {
        g_fxr_mask = kFxAbMask[fx_ab_n % 5u];
        g_fxr_bypass = 1;
        fx_ab_state = 1;
    }
}

/* ------------------------------------------------------------------ */
/* Presets (ESC-Seite "Effekte")                                       */
/* ------------------------------------------------------------------ */
/* W173 LAUNCHER-P4: Werte aus sb_gfxpreset.h (Effekt-Teil des einheitlichen
 * Grafik-Presets; Schluessel aktiver Effect Packs zaehlen nicht). */
int fxr_preset_match(void)
{
    int q;
    for (q = 0; q < 3; q++) {
        if (sb_gfx_part_is(q, SB_GFX_FX)) { return q; }
    }
    return -1;
}
void fxr_preset_apply(int q)
{
    if (q < 0 || q > 2) { return; }
    sb_gfx_part_apply(q, SB_GFX_FX);
}
const char *fxr_preset_key(int q)
{
    /* W173: Effekt-Stufe (Teil des Grafik-Presets): Original/Dezent/Stark. */
    static const char *const pn[3] = { "ovl.v_orig", "ovl.v_fx_subtle", "ovl.v_fx_strong" };
    return (q >= 0 && q < 3) ? pn[q] : "ovl.v_custom";
}
