/* render_int.h - interner Header des D3D11-Renderers (W68 SPLIT).
 * Gemeinsame Includes/Konstanten, Typen und die zwischen d3d11_render.c
 * (Fenster/Device/Context-Lock/Present/Aspect/Screenshot), render_im.c
 * (IM-/VB-Draw-Aufzeichnung + Submit), render_shader.c (IM-Pipeline,
 * VSH-Analyse), render_state.c (RenderState-Spiegel, Blend/ColorMask/
 * Stencil), render_rtt.c (Render-to-Texture) und render_diag.c (M3D-
 * Zensus, Drawdrop) geteilten Globals/Funktionen. Nur von diesen
 * Modulen einzubinden. */
#ifndef RENDER_INT_H
#define RENDER_INT_H
#define COBJMACROS /* ID3D11Device_* etc. als C-Makros */
#include "d3d11_render.h"
#include "sb_tex.h"
#include "hlog.h"
#include "hcfg.h"
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <mmsystem.h>
#include <wincodec.h>
#include <string.h>
#include <stdio.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "winmm.lib")

/* Interne Clear-Bits der D3D11-Operationen (Werte wie PC-D3D8: TARGET=1,
 * ZBUFFER=2, STENCIL=4). Achtung: das sind NICHT die Xbox-Werte, die das
 * Spiel an D3DDevice_Clear uebergibt (s. XBX_CLEAR_*); clear_xbflags=0
 * (alt) wertet die Spiel-Flags trotzdem direkt mit diesen Bits aus. */
#define XB_CLEAR_TARGET 0x1u
#define XB_CLEAR_ZBUFFER 0x2u
#define XB_CLEAR_STENCIL 0x4u
/* W177 CLEAR-FLAGS: Xbox-D3DDevice_Clear-Flags, belegt aus der eigenen
 * Disassembly der XBE (D3DDevice_Clear 0x20DF80): Bits 0xF0 = Farbkanaele
 * R/G/B/A (0x10/0x20/0x40/0x80; nur dann wird die Farbe ins RT-Format
 * gewandelt), Bit 0x1 = Tiefe (Z-Wert je Tiefenformat), Bit 0x2 = Stencil;
 * ohne Tiefenpuffer werden 0x1|0x2 gestrichen; die Flags gehen unveraendert
 * in NV2A-Methode 0x1D94 (CLEAR_SURFACE). */
#define XBX_CLEAR_TARGET 0xF0u
#define XBX_CLEAR_ZBUFFER 0x1u
#define XBX_CLEAR_STENCIL 0x2u
/* Spiel-Flags -> interne Bits (clear_xbflags=1: Xbox-Werte, 0: alt/direkt). */
unsigned sb_clear_eff(unsigned xflags);

/* RTT (W23): Offscreen-Render-Targets des Spiels (SetRenderTarget 0x20E370).
 * Notiz (surf/data/Dims) immer, D3D11-Objekte + Umleitung nur bei rtt=1. */
#define RTT_MAX 64
typedef struct {
    unsigned surf;
    unsigned data;
    unsigned w, h;      /* Xbox-Groesse (logisch: NDC-/Texel-Abbildung, Spiel-Viewport) */
    unsigned pw, ph;    /* W88 RENDER-RES: D3D11-Groesse (w*rt_scale bzw. w) */
    ID3D11Texture2D *tex;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
    ID3D11DepthStencilView *dsv;
    unsigned draws, clears;
    unsigned nlog;
    unsigned vpw, vph, vpseen;
    unsigned clr, clrflags;
    unsigned dumpf;
    unsigned fprim, fn, frhw, fxptr;
    /* RENDER-QA3 (W100, shade_shadow): Eintrag ist eine Xbox-Tiefenflaeche
     * (Schattenpuffer, Format 0x2A..0x31), Tiefe zusaetzlich als SRV. */
    int isdepth;
    ID3D11ShaderResourceView *dsrv;
    float zscale;       /* Viewport-z-Skala des Tiefenpasses (VS c[-38].z) */
    /* FIX(FX-HOLE,W106, rtt_selfcopy): Selbst-Sampling liest eine Kopie
     * (Ring aus RTT_SCOPY Texturen, Kopie nur nach fremden Schreibzugriffen). */
    unsigned wgen;      /* Schreib-Generation (Draws/Clears ausser Kopie-Lesern) */
    unsigned cgen;      /* wgen zum Zeitpunkt der letzten Kopie (+1, 0 = keine) */
    unsigned cring;     /* aktueller Ring-Index */
    ID3D11Texture2D *ctex[3];
    ID3D11ShaderResourceView *csrv[3];
    /* FIX(MAP-RETRY,W124, rtt_stale): letzter SetRenderTarget-Frame (+1, 0 =
     * nie) und Veraltet-Markierung (Datenadresse freigegeben/neu vergeben). */
    unsigned lastset;
    int stale;
} rtt_entry_t;
#define RTT_SCOPY 3u

typedef struct {
    float pos[4], col[4], uv[2];
} im_vertex_t;

#define IM_VB_MAX 65536

/* VSH2D (W38): analysiertes Vertex-Shader-Programm (render_shader.c). */
typedef struct {
    unsigned handle;    /* VS-Handle (Heap-Adresse, lauf-spezifisch) */
    unsigned hash;      /* FNV-1a ueber Slot-Worte (lauf-uebergreifend) */
    unsigned nslots;
    unsigned chk0, chk1; /* Rekreations-Wache: Progsize + Slot0-Wort1 */
    int kind;           /* 0 = alter Pfad, 1 = P1 (oPos=v0), 2 = P2 (DP4+Tail) */
    unsigned rowe;      /* P2: DP4-Zeilen Encoding E..E+3 (= Spiegel-Index) */
    int zadd;           /* P2: oPos.z zusaetzlich + cK.z */
    unsigned zadde;     /* P2: dessen Encoding */
    int od0;            /* W50 WASH: oD0-Quellregister (v-N, -1 = unbekannt/kein MOV) */
    int ot0;            /* W90 FX-ORIG: oT0-Quellregister (MOV vN -> oT0, -1 = unbekannt) */
    int ot0i;           /* W84 TERRAIN-UV: oT0-Quellregister ueber ILU-MOV (gepaarter Slot 'MOV Rn.w, c1 : MOV oT0, vN', Licht-Programme), -1 = keins */
} vsh_prog_t;

/* MISSION3D-Zensus (W25, render_diag.c). */
#define M3D_SCENE_ADDR 0x6B96DCu
#define M3D_MLOOP_ADDR 0x356F58u
#define M3D_MAXFRAMES 6u
#define M3D_MAXDRAWS 1600u
#define M3D_MAXQ 1664u
#define M3D_VTONLINE_ADDR 0x53831Cu /* u32, 1 = Mech online (nach START, Master-Messung V_MOMs1) */

/* DIAG(TEX3D,W31): Pixelshader-/Combiner-Stand (hooks_d3d.c). */
extern unsigned g_im_pshader, g_npshader;
extern unsigned g_tss_n, g_tss_a, g_tss_b, g_tss_c;

/* Oeffentliche Globals aus d3d11_render.c (dort definiert). */
extern unsigned g_present_no; /* DIAG: letztes Present (global f. TEX-Throttle) */
extern int g_fix_blend; /* W2B-FIX: scoped Blend-Off aktiv (Save/Restore) */

/* ---- Geteilte Globals (vorher static) ---- */
extern ID3D11Device *g_dev;
extern ID3D11DeviceContext *g_ctx;
extern unsigned g_im_vb_pos;
extern unsigned g_im_frame_draws;
extern ID3D11RenderTargetView *g_rtv;
extern ID3D11DepthStencilView *g_dsv;
extern ID3D11ShaderResourceView *g_bb_dsrv; /* W131 SHADER-OPT2 */
extern ID3D11RasterizerState *g_rs_norm;
extern ID3D11RasterizerState *g_rs_scis;
extern unsigned g_bb_w, g_bb_h;
extern sb_mat4 g_view, g_proj_game, g_world[4];
extern sb_viewport g_vp_game;
extern sb_eye g_rig[2];
extern unsigned g_last_clear, g_last_clear_flags;
extern int g_submit_vb;
extern unsigned g_im_draws, g_im_vdata, g_im_prim_hist[16];
extern unsigned g_im_tris, g_im_rhw, g_im_3d;
extern unsigned g_im_texd, g_im_notex;
extern unsigned g_im_shader;
extern ID3D11VertexShader *g_im_vs;
extern ID3D11PixelShader *g_im_ps;
extern ID3D11InputLayout *g_im_layout;
extern ID3D11Buffer *g_im_vb, *g_im_cb;
extern ID3D11DepthStencilState *g_im_ds_off, *g_im_ds_on;
extern unsigned g_im_submits;
extern int g_im_ready;
extern unsigned rs_cmask;
extern unsigned rs_dmask;
extern unsigned rs_st_enable;
extern unsigned rs_st_func;
extern int rs_bars;
extern rtt_entry_t g_rtt[RTT_MAX];
extern int g_rtt_n;
extern int g_rtt_bound;
extern int g_rtt_dummy;
extern unsigned m3d_nframes;
extern int m3d_on;
extern unsigned m3d_ndraw;
extern unsigned m3d_nskip;
void sbar_probe_after(int rhw, unsigned nout); /* DIAG(W98) */ /* DIAG(W98): uebersprungene Zensus-Draws */
extern unsigned g_sub_xptr, g_sub_tex;
extern unsigned g_shot_census;
extern unsigned m3d_zenable;
extern volatile int g_fx_census_req; /* DIAG(FX-HOLE,W106) */
extern ID3D11Query *m3d_q[M3D_MAXQ];
extern unsigned m3d_qdraw[M3D_MAXQ];
extern unsigned m3d_nq;

/* ---- Modulgrenzen-Funktionen (vorher static) ---- */
void sb_ctx_enter(void);
void sb_ctx_leave(void);
/* PERF(PERF-FIX,W138) present_decouple: Present-Takt-Thread (render_interp.c) */
int sb_ctx_release_all(void);    /* W138: ctx_lock fuer reine Device-Arbeit ganz abgeben (nur mit Takt-Thread), Rueckgabe fuer sb_ctx_reacquire */
void sb_ctx_reacquire(int r);
void sb_ctx_yield(void);          /* lange Warteschleifen unter ctx_lock: Sperre an den Takt-Thread abgeben */
extern volatile LONG g_pd_want;   /* Takt-Thread will ctx_lock */
extern volatile DWORD g_pd_tid;   /* Tid des Takt-Threads (0 = keiner) */
extern LONGLONG g_pd_last_q;      /* letztes echtes Present (QPC, unter ctx_lock) */
/* DIAG(PERF-WARM,W139) warm_diag=1: CPU-Zeit je D3D-Context-Aufruf (render_warm.c) */
LONGLONG sb_wd_q(void);
void sb_wd_end(LONGLONG q0, const char *site, ID3D11DeviceContext *c);
/* PERF(PERF-WARM,W139) warm_shaders: Shader-Cache vorab laden/anlegen (render_warm.c) */
void sb_warm_start(void);
int sb_warm_blob(const char *path, HRESULT(WINAPI *cb)(SIZE_T, ID3DBlob **), ID3DBlob **out);
HRESULT sb_warm_create_vs(const void *d, SIZE_T n, ID3D11VertexShader **out);
HRESULT sb_warm_create_ps(const void *d, SIZE_T n, ID3D11PixelShader **out);
void sb_warm_stat_log(void);
void sb_warm_gpu_start(void);                 /* W139 warm_gpu */
void sb_interp_prealloc(void);                /* W139 warm_vbpre (render_interp.c) */
void sb_warm_note_game_present(LONGLONG q);
void sb_warm_gpu_step(LONGLONG now);
#define WD_CALL(site, c, ...) do { LONGLONG wd_q_ = g_cfg.warm_diag ? sb_wd_q() : 0; __VA_ARGS__;                                    if (wd_q_) { sb_wd_end(wd_q_, (site), (c)); } } while (0)
extern int g_phud_src;            /* perf_trace: Quelle des laufenden Presents (0 Spiel, 1 Takt, 2 Wiederholung) */
int sb_render_present_hold(void); /* letztes Bild erneut zeigen (unter ctx_lock) */
void sb_pd_start(void);           /* Takt-Thread einmalig starten (Spiel-Thread) */
int sb_render_offscreen(void);     /* 1 = FPS-INTERP-Offscreen-BB (d3d11_render.c) */
int sb_pd_on(void);               /* 1 = Takt-Thread fuehrt den Bildtakt (Echtzeit, Offscreen-BB) */
void sb_aspect_box(unsigned *bx, unsigned *by, unsigned *bw, unsigned *bh);
void sb_im_submit(void);
void sb_im_init(void);
const vsh_prog_t *vsh_program(unsigned handle);
int vsh_read_mat(const vsh_prog_t *vp, float m[4][4], float zadd[4]);
float vsh_rcc(float x);
void rs_apply_states(int rhw);
int rtt_active(void);
/* FX-ORIG (W90, render_fx.c): NV-RenderState-Spiegel + Effekt-Zensus */
void fx_nv_note(unsigned method, unsigned value);
int fx_nv_has(unsigned m);
unsigned fx_nv(unsigned m, unsigned def);
int fx_draw_pre(const im_vertex_t *out, unsigned nout, int rhw, unsigned prim, const float *mvp);
void fx_draw_post(int q);
/* DIAG(SHADE-BLOOM,W112, render_bloom.c): Bloom-Kette Draw fuer Draw */
void bloom_diag_pre(const im_vertex_t *out, unsigned nout, unsigned nsrc, int rhw, unsigned prim, ID3D11ShaderResourceView *srv0);
void bloom_diag_post(void);
void bloom_diag_clear(unsigned flags, unsigned color);
void bloom_diag_present(unsigned p);
void fx_present(void);
int fx_ps_add_c0(float c0[4]); /* FIX(FX-ORIG,W90): PS-Muster (V0+C0)*T0 -> C0 */
/* MONITOR-FX (W105, render_mfx.c): Korn/Scanlines + Signalstoerung der Monitore */
#define MFX_NONE 0
#define MFX_WASH 1
#define MFX_LINES 2
extern int g_mfx_kind, g_mfx_nonorm;
int mfx_classify(unsigned prim, unsigned n, int is_vb);
/* SHADE-FX (W121, render_shfx.c): Shader-Optionen (Standard = Original) */
int shfx_world_on(void);
int shfx_cockpit_on(void);
int shfx_post_on(void); /* W131 SHADER-OPT2: Cockpit-Licht, Farbe, FXAA Endbild, SSAO */
void shfx_note_3d(int slot);
void shfx_rq_consts(float *sx, const int srvk[4], int tgt, int is3d, int fog_ps, int st0rt, unsigned nstages);
void sb_shfx_post(ID3D11RenderTargetView *rtv, ID3D11Resource *res);
void sb_shfx_ab_shot(const char *base);
void sb_shfx_present(unsigned n);
void sb_shfx_present_end(unsigned n); /* W131 SHADER-OPT2: DIAG shfx_ab=4/5 (Ende des Presents) */
void shfx_preset_apply(int which);    /* W131: 0 Original, 1 dezent, 2 stark */
int shfx_preset_match(void);          /* W131: 0..2, -1 = Benutzerdefiniert */
const char *shfx_preset_name(int which);
/* W173 LAUNCHER-P4: einheitliches Grafik-Preset (Werte sb_gfxpreset.h,
 * gleiche Semantik wie der Launcher). Teil: Shader / Effekte / alles (+HD). */
enum { SB_GFX_SHADER = 0, SB_GFX_FX = 1, SB_GFX_ALL = 2 };
int sb_gfx_part_is(int q, int part);  /* 1 = Teil entspricht Preset q (Effect-Pack-Schluessel zaehlen nicht) */
void sb_gfx_part_apply(int q, int part); /* setzt Shader/Effekte (HD schaltet das ESC-Menue live) */
int sb_gfx_preset_match(void);        /* 0..2, -1 = Benutzerdefiniert */
void sb_shfx_shading_watch(void);
int mfx_apply(float (*buf)[16][4], unsigned *n, unsigned *prim, unsigned nmax);
void mfx_end(void);
void mfx_present(void);
extern float g_fx_raw[4][4], g_fx_mat[4][4];
extern unsigned g_fx_raw_n, g_fx_slotmask; extern int g_fx_vb;
extern float g_fx_s2[4], g_fx_s3[4], g_fx_s9[4];
extern int g_fx_vsh_em, g_fx_vsh_kind;
ID3D11RenderTargetView *rtt_cur_rtv(void);
ID3D11DepthStencilView *rtt_cur_dsv(void);
void rtt_cur_dims(unsigned *w, unsigned *h);
int rtt_unscale(int idx); /* W88: RT auf Xbox-Groesse zuruecksetzen (CPU-Upload) */
unsigned sb_rtt_base_factor(void); /* W146 rt_scale_dyn: auto-/rt_scale-Faktor */
unsigned sb_rtt_dyn_factor(void);  /* W146: aktueller Faktor (dynamisch oder Grundfaktor) */
int sb_rtt_rescale(unsigned n);    /* W146: skalierte RTs neu anlegen (-1 = jetzt nicht moeglich, W165 keep: -2 = Saetze noch im Bau) */
void sb_rtt_keep_prefetch(unsigned n); /* W165 rt_scale_dyn_keep: Saetze fuer Faktor n vorab (Hilfs-Thread) */
void sb_rtt_keep_stat(unsigned *made, unsigned *jobs, unsigned *swaps, double *job_ms); /* W165 */
void sb_rt_dyn_apply(void);        /* W146: Swap-Anfang (Spiel-Thread): gewuenschten Faktor umsetzen */
int rtt_dims(unsigned fmt, unsigned size, unsigned *w, unsigned *h);
struct ID3D11ShaderResourceView *sb_rtt_depth_srv(unsigned xptr, unsigned *w, unsigned *h, float *zscale); /* W100 */
void rtt_present_restore(void);
int m3d_vs_mvp_read(float out[16]);
void mon_dump_slot(int slot, unsigned f, unsigned di, int comp);
void m3d_present_tick(unsigned f);
void dd_sum(unsigned f);
/* FPS-INTERP (W69, render_interp.c) */
int sb_im_compile(const char *src, const char *target, ID3DBlob **out);
extern int g_ip_recording;
extern unsigned g_ip_sigparts[3]; /* DIAG(FPS-INTERP-2): VS/Textur/PS des naechsten rec_draw */
unsigned sb_interp_hash(const void *base, unsigned stride, unsigned n);
void sb_interp_rec_draw(const im_vertex_t *v, unsigned n, unsigned topo,
                        const float mvp[16], unsigned has_tex, int cls,
                        unsigned sig, unsigned rawhash, unsigned vstart);
int sb_interp_vb_write(const im_vertex_t *v, unsigned n, ID3D11Buffer **vb, unsigned *start);
/* PERF(IM-PERF,W99): direkte Expansion in den Tick-VB (render_interp.c) */
im_vertex_t *sb_interp_vb_begin(unsigned n, ID3D11Buffer **vb, unsigned *start);
void sb_interp_vb_end(unsigned n);
unsigned sb_im_exp_src(unsigned prim, unsigned n, unsigned k);
void im_fast_check_present(unsigned f); /* DIAG(IM-PERF,W99): Zeile IMCHK (im_fast=2) */
void sb_interp_rec_draw_src(const im_vertex_t *src, unsigned prim, unsigned nsrc, unsigned n,
                            unsigned topo, const float mvp[16], unsigned has_tex, int cls,
                            unsigned sig, unsigned rawhash, unsigned vstart);
void sb_interp_rec_clear(ID3D11RenderTargetView *rtv, const float c[4],
                         ID3D11DepthStencilView *dsv, unsigned dsflags,
                         float z, unsigned stencil);
int sb_interp_swap(void);
unsigned sb_interp_pairs(void); /* W81 Test: Interp-Paare seit Start (Missionsbeleg) */
void sb_interp_reset(const char *why); /* W81: alle Listen/Schatten/Staging frei (vor ResizeBuffers) */
/* W81 WINMODE (render_winmode.c, d3d11_render.c) */
#include <dxgi.h>
extern unsigned g_swap_flags;
int sb_render_resize(unsigned w, unsigned h, const char *why);
void sb_winmode_ui_dpi(void);
void sb_winmode_init(HWND hwnd, IDXGISwapChain *swap);
int sb_winmode_wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp, LRESULT *res);
void sb_winmode_on_close(void);
void sb_winmode_tick(unsigned n);
int sb_winmode_current(void);
void sb_render_present_raw(void);
void sb_pf_note(float t, unsigned src_frame); /* DIAG(POPIN,W91): Art des naechsten present_raw */
int sb_pf_active(void); /* DIAG(POPIN,W91): 1 = Bildfolge (pf_ring) laeuft */
void sb_render_blit(ID3D11Resource *src);
void sb_edge_apply(ID3D11RenderTargetView *rtv); /* W107 EDGE-MODE (render_edge.c), unter ctx_lock */
void sb_edge_ab_shots(const char *base, ID3D11Resource *src); /* DIAG(EDGE-MODE,W107): edge_ab=1, unter ctx_lock */
void sb_render_shot_name(const char *name, ID3D11Resource *src);
/* RTT-Notizen (render_rtt.c) */
void rtt_note_draw(unsigned prim, unsigned n, int rhw, unsigned xptr);
extern int g_rtt_selfread; /* W106 rtt_selfcopy */
void rtt_note_clear(unsigned flags, unsigned color);

#endif /* RENDER_INT_H */
