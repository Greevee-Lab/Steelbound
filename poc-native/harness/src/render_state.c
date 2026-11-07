/* render_state.c - D3D11-Renderer: RenderState-Spiegel (COLORMASK/DEPTH/
 * Stencil, W33), Blend-States (D3DRS 19/20/27, XBLEND/NV-Methoden) und deren
 * Anwendung (Depth-Stencil-/Blend-State-Cache). W68 SPLIT: 1:1 aus
 * d3d11_render.c verschoben. */
#include "render_int.h"
#include "sb_perfhud.h" /* W125 TICK-PERF */

/* ---------- RenderState-Spiegel COLORMASK (W33) ----------
 * Methoden (XDK-Tabelle @0x25D038, statisch verifiziert): 0x358 COLOR_MASK
 * (Bytes B@0 G@8 R@16 A@24, an = 0x1010101), 0x35C DEPTH_MASK (0/1),
 * Stencil 0x360 WRITEMASK / 0x364 FUNC (GL-Enum) / 0x368 REF / 0x36C MASK /
 * 0x374 ZFAIL / 0x378 ZPASS; 0x370 FAIL + 0x32C ENABLE nur via eigene
 * Inline-Funktionen (0x20D060/0x20CFD0, eigene Hooks). Aufzeichnung immer
 * (billig, wie m3d_zenable); Anwendung hinter rs_colormask/rs_stencil. */
unsigned rs_cmask = 0x1010101u; /* COLOR_MASK (Default: alles an) */
unsigned rs_dmask = 1u;          /* DEPTH_MASK */
unsigned rs_st_enable = 0u;      /* StencilEnable */
unsigned rs_st_func = 0x207u;    /* STENCIL_FUNC (GL_ALWAYS) */
static unsigned rs_st_ref = 0u;         /* STENCIL_REF */
static unsigned rs_st_rmask = 0xFFu;    /* STENCIL_MASK (Lese-Maske) */
static unsigned rs_st_wmask = 0xFFu;    /* STENCIL_WRITEMASK */
static unsigned rs_st_fail = 0u;        /* STENCIL_OP_FAIL (0 = KEEP) */
static unsigned rs_st_zfail = 0u;       /* STENCIL_OP_ZFAIL */
static unsigned rs_st_zpass = 0u;       /* STENCIL_OP_ZPASS */
int rs_bars = 0; /* 1 = Balken-Overlay (spielt Maske ALL, kein Spiel-State) */

void sb_render_rs_note(unsigned method, unsigned value)
{
    unsigned *slot = NULL;
    switch (method) {
    case 0x358u: slot = &rs_cmask; break;
    case 0x35Cu: slot = &rs_dmask; break;
    case 0x360u: slot = &rs_st_wmask; break;
    case 0x364u: slot = &rs_st_func; break;
    case 0x368u: slot = &rs_st_ref; break;
    case 0x36Cu: slot = &rs_st_rmask; break;
    case 0x370u: slot = &rs_st_fail; break;
    case 0x374u: slot = &rs_st_zfail; break;
    case 0x378u: slot = &rs_st_zpass; break;
    default: return;
    }
    if (value != *slot) {
        unsigned old = *slot;
        *slot = value;
    }
}

void sb_render_rs_stencil_enable(unsigned v)
{
    v = v ? 1u : 0u;
    if (v != rs_st_enable) {
        unsigned old = rs_st_enable;
        rs_st_enable = v;
    }
}

/* Blend-States (D3DRS 19/20/27): D3DBLEND -> D3D11, bei Aenderung
 * neu erstellen (State-Wechsel sind selten). */
static unsigned g_blend_src = 5, g_blend_dst = 6, g_blend_on = 1;
static unsigned g_blend_eq = 0x8006u, g_blend_col = 0u; /* FX-ORIG (W90): NV 0x350/0x34C */

static unsigned blend_map(unsigned v)
{
    switch (v) {
    case 1: return D3D11_BLEND_ZERO;
    case 2: return D3D11_BLEND_ONE;
    case 3: return D3D11_BLEND_SRC_COLOR;
    case 4: return D3D11_BLEND_INV_SRC_COLOR;
    case 5: return D3D11_BLEND_SRC_ALPHA;
    case 6: return D3D11_BLEND_INV_SRC_ALPHA;
    case 7: return D3D11_BLEND_DEST_ALPHA;
    case 8: return D3D11_BLEND_INV_DEST_ALPHA;
    case 9: return D3D11_BLEND_DEST_COLOR;
    case 10: return D3D11_BLEND_INV_DEST_COLOR;
    case 11: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 14: return D3D11_BLEND_BLEND_FACTOR;
    case 15: return D3D11_BLEND_INV_BLEND_FACTOR;
    default: return D3D11_BLEND_ONE;
    }
}
/* FIX(RADAR,W56): Alpha-Faktor aus demselben Spiel-Code wie Farbe (Xbox/D3D8
 * kennt kein separates Alpha-Blend). D3D11 verbietet Farb-Faktoren im
 * Alpha-Slot -> dort Fallback auf Replace (ONE/ZERO = altes Verhalten). */
static unsigned blend_alpha_map(unsigned v, int is_src)
{
    switch (v) {
    case 1: return D3D11_BLEND_ZERO;
    case 2: return D3D11_BLEND_ONE;
    case 5: return D3D11_BLEND_SRC_ALPHA;
    case 6: return D3D11_BLEND_INV_SRC_ALPHA;
    case 7: return D3D11_BLEND_DEST_ALPHA;
    case 8: return D3D11_BLEND_INV_DEST_ALPHA;
    default: return is_src ? D3D11_BLEND_ONE : D3D11_BLEND_ZERO;
    }
}
/* XBLEND (31.10.): Xbox-D3D setzt Renderstates als NV2A-Methoden mit
 * GL-Enums, nicht als PC-D3DRS 19/20/27. Vorher kam nie ein Blend-Wechsel
 * an -> fest SRCALPHA/INVSRCALPHA -> Bink-Movie (Alpha=0) unsichtbar,
 * darunter weiss. Abbildung auf die PC-Codes von blend_map(). */
static unsigned gl_blend_to_pc(unsigned v)
{
    switch (v) {
    case 0x0000: return 1;  /* ZERO */
    case 0x0001: return 2;  /* ONE */
    case 0x0300: return 3;  /* SRC_COLOR */
    case 0x0301: return 4;  /* ONE_MINUS_SRC_COLOR */
    case 0x0302: return 5;  /* SRC_ALPHA */
    case 0x0303: return 6;  /* ONE_MINUS_SRC_ALPHA */
    case 0x0304: return 7;  /* DST_ALPHA */
    case 0x0305: return 8;  /* ONE_MINUS_DST_ALPHA */
    case 0x0306: return 9;  /* DST_COLOR */
    case 0x0307: return 10; /* ONE_MINUS_DST_COLOR */
    case 0x0308: return 11; /* SRC_ALPHA_SATURATE */
    case 0x8001: return 14; /* CONSTANT_COLOR */
    case 0x8002: return 15; /* ONE_MINUS_CONSTANT_COLOR */
    default: return 2;
    }
}

/* DIAG(MOVIE): Blend-State zum Draw-Zeitpunkt (Film: Alpha 0 x SRC_ALPHA?). */
void sb_render_blend_state(unsigned *on, unsigned *src, unsigned *dst)
{
    if (on) { *on = g_blend_on; }
    if (src) { *src = g_blend_src; }
    if (dst) { *dst = g_blend_dst; }
}

/* ---------- COLORMASK-Anwendung (W33) ----------
 * COLOR_MASK -> RenderTargetWriteMask (rs_colormask, in sb_render_blend);
 * Stencil/DepthWrite -> DepthStencil-State (rs_stencil, rs_apply_ds mit
 * 1-Slot-Cache: Neuaufbau nur bei Key-Wechsel). rs_stencil=0 nimmt exakt
 * den alten Pfad (g_im_ds_off/on). */
static unsigned rs_blend_cmask = 0x1010101u; /* zuletzt angewendete Maske */
static ID3D11DepthStencilState *rs_ds_cur = NULL;
static unsigned rs_ds_key = 0xFFFFFFFFu;
static unsigned rs_ds_ref = 0xFFFFFFFFu;
#define RS_DS_CACHE 16u
static ID3D11DepthStencilState *rs_dsc[RS_DS_CACHE];
static unsigned rs_dsc_key[RS_DS_CACHE], rs_dsc_next;
static unsigned rs_zf_cur = 0x201u; /* FX-ORIG (W90): wirksame DEPTH_FUNC (GL) */

/* NV097 STENCIL_FUNC = GL-Enum (0x207 ALWAYS, statisch belegt). */
static unsigned rs_func_map(unsigned v)
{
    switch (v) {
    case 0x200u: return D3D11_COMPARISON_NEVER;
    case 0x201u: return D3D11_COMPARISON_LESS;
    case 0x202u: return D3D11_COMPARISON_EQUAL;
    case 0x203u: return D3D11_COMPARISON_LESS_EQUAL;
    case 0x204u: return D3D11_COMPARISON_GREATER;
    case 0x205u: return D3D11_COMPARISON_NOT_EQUAL;
    case 0x206u: return D3D11_COMPARISON_GREATER_EQUAL;
    case 0x207u: return D3D11_COMPARISON_ALWAYS;
    default: break;
    }
    { static unsigned seenf[8]; unsigned i;
      for (i = 0; i < 8u; i++) { if (seenf[i] == v + 1u) { break; } }
      if (i >= 8u) { for (i = 0; i < 8u; i++) { if (!seenf[i]) { break; } } }
      if (i < 8u && !seenf[i]) { seenf[i] = v + 1u;
          HLOG_W("RS: unbekannte STENCIL_FUNC 0x%X -> ALWAYS", v); } }
    return D3D11_COMPARISON_ALWAYS;
}

/* Stencil-Ops: nativ 0..7 (0 = KEEP, Szenen-Init schreibt 0) + GL-Enums
 * (0x1E00.., XDK-Spiegel vergleicht mit 0x1E00). 0 ist als KEEP gewaehlt
 * (inerter Default); GL-ZERO kollidiert, tritt hier nie auf (Stencil aus). */
static unsigned rs_op_map(unsigned v)
{
    switch (v) {
    case 0u: return D3D11_STENCIL_OP_KEEP;
    case 1u: return D3D11_STENCIL_OP_ZERO;
    case 2u: return D3D11_STENCIL_OP_REPLACE;
    case 3u: return D3D11_STENCIL_OP_INCR_SAT;
    case 4u: return D3D11_STENCIL_OP_DECR_SAT;
    case 5u: return D3D11_STENCIL_OP_INVERT;
    case 6u: return D3D11_STENCIL_OP_INCR;
    case 7u: return D3D11_STENCIL_OP_DECR;
    case 0x1E00u: return D3D11_STENCIL_OP_KEEP;
    case 0x1E01u: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02u: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03u: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150Au: return D3D11_STENCIL_OP_INVERT;
    default: break;
    }
    { static unsigned seeno[8]; unsigned i;
      for (i = 0; i < 8u; i++) { if (seeno[i] == v + 1u) { break; } }
      if (i >= 8u) { for (i = 0; i < 8u; i++) { if (!seeno[i]) { break; } } }
      if (i < 8u && !seeno[i]) { seeno[i] = v + 1u;
          HLOG_W("RS: unbekannte STENCIL_OP 0x%X -> KEEP", v); } }
    return D3D11_STENCIL_OP_KEEP;
}

static void rs_apply_ds(int rhw)
{
    unsigned sten, dwr, func, fail, zfail, zpass, rmask, wmask, ref, key;
    D3D11_DEPTH_STENCIL_DESC dd;
    ID3D11DepthStencilState *ns = NULL;
    HRESULT hr;
    /* FIX(SHADOWBUF,W127): Tiefenpass in den Schattenpuffer (W100, nur
     * shade_shadow=1) zeichnet ueber den P2-Pfad (Bildschirm-Pixel aus dem
     * Viewport-Tail, rhw=1) - "RHW: kein Depth-Test" schrieb dort keine
     * Tiefe, der Puffer blieb leer (W127_d1: alle Texel 0xFFFFFF, T1 = 1).
     * Ins Tiefen-RT gilt der Tiefenzustand wie bei 3D (NV2A: ZENABLE/
     * ZWRITE/DEPTH_FUNC des Spiels, z = z/w wie im Texgen-Vergleich). */
    if (rhw && rtt_active() && g_rtt_bound >= 0 && g_rtt_bound < g_rtt_n && g_rtt[g_rtt_bound].isdepth) { rhw = 0; }
    if (!g_cfg.rs_stencil) { /* exakt alter Pfad */
        sb_ctx_enter();
        ID3D11DeviceContext_OMSetDepthStencilState(g_ctx,
                                                  rhw ? g_im_ds_off
                                                      : g_im_ds_on,
                                                  0);
        sb_ctx_leave();
        return;
    }
    sten = rs_st_enable ? 1u : 0u;
    dwr = rs_dmask ? 1u : 0u;
    if (g_cfg.mon_dummy_nodepth && g_rtt_dummy && rs_cmask == 0u) {
        dwr = 0u; /* FIX(MONITORVIEW,W36): Dummy-Tiefenpass -> keine BB-Tiefe (keine Loecher) */
    }
    func = rs_st_func & 0xFFFFu; fail = rs_st_fail & 0xFFFFu;
    zfail = rs_st_zfail & 0xFFFFu; zpass = rs_st_zpass & 0xFFFFu;
    rmask = rs_st_rmask & 0xFFu; wmask = rs_st_wmask & 0xFFu;
    ref = rs_st_ref & 0xFFu;
    /* FIX(FX-ORIG,W90): DEPTH_FUNC (NV 0x354, GL-Enum; Spiel: LEQUAL 0x203,
     * ALWAYS 0x207). 0x201 LESS = alter fester Wert. */
    { unsigned zf = g_cfg.fx_zfunc ? (fx_nv(0x354u, 0x201u) & 0xFFFFu) : 0x201u;
      if (zf < 0x200u || zf > 0x207u) { zf = 0x201u; }
      rs_zf_cur = zf; }
    /* Key: rhw + dwr + sten + ops/func/masken (ref separat, OM-Parameter). */
    key = (rhw ? 1u : 0u) | (dwr << 1) | (sten << 2)
        | ((func & 0xFu) << 3) | ((fail & 0xFu) << 7)
        | ((zfail & 0xFu) << 11) | ((zpass & 0xFu) << 15)
        | (rmask << 19) | ((wmask & 0x1Fu) << 27);
    key ^= (wmask >> 5) * 0x9E3779B1u + func + fail * 3u + zfail * 7u
        + zpass * 13u;
    key ^= (rs_zf_cur - 0x200u) * 0x85EBCA6Bu; /* FX-ORIG (W90) */
    if (key == rs_ds_key && ref == rs_ds_ref && rs_ds_cur) {
        sb_ctx_enter();
        ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, rs_ds_cur, ref);
        sb_ctx_leave();
        return;
    }
    /* FX-ORIG (W90): mit fx_zfunc wechselt der Schluessel im Bild oefter
     * (LEQUAL/ALWAYS) -> kleiner Zusatz-Cache statt Neuaufbau je Wechsel. */
    { unsigned c;
      for (c = 0; c < RS_DS_CACHE; c++) {
          if (rs_dsc[c] && rs_dsc_key[c] == key) {
              if (rs_ds_cur) { ID3D11DepthStencilState_Release(rs_ds_cur); }
              rs_ds_cur = rs_dsc[c]; ID3D11DepthStencilState_AddRef(rs_ds_cur);
              rs_ds_key = key; rs_ds_ref = ref;
              sb_ctx_enter();
              ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, rs_ds_cur, ref);
              sb_ctx_leave();
              return;
          }
      } }
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = rhw ? FALSE : TRUE; /* RHW: kein Depth-Test (alt) */
    dd.DepthWriteMask = dwr ? D3D11_DEPTH_WRITE_MASK_ALL
                            : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = (D3D11_COMPARISON_FUNC)rs_func_map(rs_zf_cur); /* FX-ORIG (W90); alt LESS */
    dd.StencilEnable = sten ? TRUE : FALSE;
    dd.StencilReadMask = (UINT8)rmask;
    dd.StencilWriteMask = (UINT8)wmask;
    dd.FrontFace.StencilFailOp = (D3D11_STENCIL_OP)rs_op_map(rs_st_fail);
    dd.FrontFace.StencilDepthFailOp = (D3D11_STENCIL_OP)rs_op_map(rs_st_zfail);
    dd.FrontFace.StencilPassOp = (D3D11_STENCIL_OP)rs_op_map(rs_st_zpass);
    dd.FrontFace.StencilFunc = (D3D11_COMPARISON_FUNC)rs_func_map(rs_st_func);
    dd.BackFace = dd.FrontFace; /* NV2A: kein Two-Sided-Stencil */
    hr = ID3D11Device_CreateDepthStencilState(g_dev, &dd, &ns);
    if (FAILED(hr) || !ns) {
        sb_ctx_enter();
        ID3D11DeviceContext_OMSetDepthStencilState(g_ctx,
                                                  rhw ? g_im_ds_off
                                                      : g_im_ds_on,
                                                  0);
        sb_ctx_leave();
        return;
    }
    if (rs_ds_cur) { ID3D11DepthStencilState_Release(rs_ds_cur); }
    rs_ds_cur = ns; rs_ds_key = key; rs_ds_ref = ref;
    { unsigned c = rs_dsc_next++ % RS_DS_CACHE; /* FX-ORIG (W90): Cache-Eintrag (eigene Referenz) */
      if (rs_dsc[c]) { ID3D11DepthStencilState_Release(rs_dsc[c]); }
      rs_dsc[c] = ns; ID3D11DepthStencilState_AddRef(ns); rs_dsc_key[c] = key; }
    sb_ctx_enter();
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, rs_ds_cur, ref);
    sb_ctx_leave();
}

void rs_apply_states(int rhw)
{
    /* Blend-Maske: sb_render_blend baut bei Masken-Wechsel neu (s. dort),
     * sonst No-Op. */
    sb_render_blend(27, g_blend_on);
    rs_apply_ds(rhw);
}

void sb_render_blend_nv(unsigned method, unsigned value)
{
    if (method == 0x304u) {
        sb_render_blend(27, value);
    } else if (method == 0x344u) {
        sb_render_blend(19, gl_blend_to_pc(value));
    } else if (method == 0x348u) {
        sb_render_blend(20, gl_blend_to_pc(value));
    } else if (method == 0x350u) {
        sb_render_blend(28, value); /* FX-ORIG (W90): BLEND_EQUATION */
    } else if (method == 0x34Cu) {
        sb_render_blend(29, value); /* FX-ORIG (W90): BLEND_COLOR (ARGB) */
    }
}

void sb_render_blend(unsigned state, unsigned value)
{
    D3D11_BLEND_DESC bd;
    ID3D11BlendState *bs = NULL;
    HRESULT hr;
    long long tbq; /* DIAG(TICK-PERF,W125) */
    if (!g_dev) {
        return;
    }
    /* No-Op bei unveraendertem State (CreateBlendState-Churn wuerde
     * den Treiber wuergen). */
    unsigned effmask; int maskchg; /* W33 (init unten) */
    effmask = (g_cfg.rs_colormask && !rs_bars) ? rs_cmask : 0x1010101u;
    maskchg = (effmask != rs_blend_cmask);
    if (state == 19) {
        if (!maskchg && g_blend_src == value) { return; }
        g_blend_src = value;
    } else if (state == 20) {
        if (!maskchg && g_blend_dst == value) { return; }
        g_blend_dst = value;
    } else if (state == 27) {
        if (!maskchg && g_blend_on == ((value != 0) ? 1u : 0u)) { return; }
        g_blend_on = (value != 0) ? 1u : 0u;
    } else if (state == 28) { /* FX-ORIG (W90): BLEND_EQUATION (GL-Enum) */
        if (!maskchg && g_blend_eq == value) { return; }
        g_blend_eq = value;
        if (!g_cfg.fx_blend_eq && !maskchg) { return; }
    } else if (state == 29) { /* FX-ORIG (W90): BLEND_COLOR */
        if (!maskchg && g_blend_col == value) { return; }
        g_blend_col = value;
        if (!g_cfg.fx_blend_eq && !maskchg) { return; }
    } else {
        return;
    }
    memset(&bd, 0, sizeof bd);
    bd.RenderTarget[0].BlendEnable = g_blend_on ? TRUE : FALSE;
    bd.RenderTarget[0].SrcBlend = (D3D11_BLEND)blend_map(g_blend_src);
    bd.RenderTarget[0].DestBlend = (D3D11_BLEND)blend_map(g_blend_dst);
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = /* W33: COLOR_MASK */
        (UINT8)(((effmask & 0xFF0000u) ? D3D11_COLOR_WRITE_ENABLE_RED : 0)
        | ((effmask & 0xFF00u) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0)
        | ((effmask & 0xFFu) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0)
        | ((effmask & 0xFF000000u) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0));
    if (g_cfg.blend_alpha) { /* FIX(RADAR,W56): Alpha mit denselben Faktoren blenden (Xbox/D3D8), statt Replace (ONE/ZERO oben) */
        bd.RenderTarget[0].SrcBlendAlpha = (D3D11_BLEND)blend_alpha_map(g_blend_src, 1);
        bd.RenderTarget[0].DestBlendAlpha = (D3D11_BLEND)blend_alpha_map(g_blend_dst, 0);
    }
    /* FIX(FX-ORIG,W90): Blend-Gleichung (NV 0x350) statt fest ADD - das Spiel
     * nutzt MIN (0x8007) und REVERSE_SUBTRACT (0x800B), u. a. fuer Rauch/
     * Abdunkeln; Blend-Farbe (0x34C) fuer CONSTANT_COLOR-Faktoren. */
    if (g_cfg.fx_blend_eq) {
        D3D11_BLEND_OP op = D3D11_BLEND_OP_ADD;
        switch (g_blend_eq) {
        case 0x800Au: op = D3D11_BLEND_OP_SUBTRACT; break;
        case 0x800Bu: op = D3D11_BLEND_OP_REV_SUBTRACT; break;
        case 0x8007u: op = D3D11_BLEND_OP_MIN; break;
        case 0x8008u: op = D3D11_BLEND_OP_MAX; break;
        default: break; /* 0x8006 FUNC_ADD (+ unbekannt) */
        }
        bd.RenderTarget[0].BlendOp = op;
        if (g_cfg.blend_alpha) { bd.RenderTarget[0].BlendOpAlpha = op; }
    }
    rs_blend_cmask = effmask; /* W33 */
    tbq = TB_BEGIN(); /* DIAG(TICK-PERF,W125) */
    hr = ID3D11Device_CreateBlendState(g_dev, &bd, &bs);
    if (FAILED(hr)) {
        return;
    }
    sb_ctx_enter(); /* A6 ctx_lock */
    if (g_cfg.fx_blend_eq) {
        float bf[4];
        bf[0] = (float)((g_blend_col >> 16) & 0xFFu) / 255.0f;
        bf[1] = (float)((g_blend_col >> 8) & 0xFFu) / 255.0f;
        bf[2] = (float)(g_blend_col & 0xFFu) / 255.0f;
        bf[3] = (float)(g_blend_col >> 24) / 255.0f;
        ID3D11DeviceContext_OMSetBlendState(g_ctx, bs, bf, 0xFFFFFFFFu);
    } else
    ID3D11DeviceContext_OMSetBlendState(g_ctx, bs, NULL, 0xFFFFFFFFu);
    sb_ctx_leave();
    ID3D11BlendState_Release(bs);
    TB_END(TB_BLEND, tbq); /* DIAG(TICK-PERF,W125) */
}
