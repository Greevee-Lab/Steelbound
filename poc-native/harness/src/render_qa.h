/* render_qa.h - RENDER-QA (W77): Zensus (render_combiner.c), Shading-Pfad
 * (render_shade.c), Interp-Anbindung (render_interp.c). Nur Renderer-Module
 * und Hooks. */
#ifndef RENDER_QA_H
#define RENDER_QA_H

/* render_combiner.c: Zensus (rq_diag) */
void rq_census_draw(int rhw, unsigned nverts);
void rq_census_present(unsigned f);
void rq_text_flush(void);
void rq_text_on_clear(unsigned surf, unsigned flags, unsigned color); /* FIX(DLGTEXT2,W97) */
extern unsigned g_rq_fogcolor, g_rq_fogcolor_set;
extern unsigned g_rq_psconst[8], g_rq_psconst_set, g_rq_psconst_calls;
extern unsigned g_rq_rs[64], g_rq_rs_calls;
extern unsigned long long g_rq_rs_set;
unsigned __stdcall hook_D3DDevice_SetPixelShaderConstant(unsigned reg, unsigned pdata, unsigned count);

/* DIAG(SHADE-PERF,W80): rq_perf=1 -> QPC-Zaehler je Abschnitt, Zeile RQPERF
 * alle rq_every_perf Missions-Bilder (render_shade.c). */
enum { RQP_VBLOOP, RQP_SUBMIT, RQP_BEGIN, RQP_EXP, RQP_BIND, RQP_DRAW, RQP_COMPILE, RQP_CAP, RQP_PREP, RQP_PRESENT, RQP_N };
extern long long g_rqp_q[RQP_N];
/* W122 PERF-HUD: Zaehler laufen auch bei sichtbarem Leistungs-Overlay
 * (g_phud_meas, sb_perfhud.h); RQPERF-Zeilen nur mit rq_perf. */
extern volatile int g_phud_meas;
#define RQP_ON (g_cfg.rq_perf || g_phud_meas)
/* PERF(PERF-TICK,W168): Teilzeiten, die nur RQPERF/IMPERF ausgeben (begin/exp/bind, IMP_*):
 * nur mit rq_perf messen. Das Leistungs-Overlay/perf_log braucht nur vbloop/submit/prep/cap/draw;
 * die ~11 weiteren QPC-Aufrufe je Draw verfaelschten dort die Tick-Last (Messkosten). */
#define RQP_FULL (g_cfg.rq_perf)
/* DIAG(IM-PERF,W99): Teilabschnitte von sb_im_submit (Zeile IMPERF mit RQPERF):
 * pre = bis vor den Upload (Heuristik, Umrechnung, alte Expansion), up =
 * Upload (alt memcpy, fast Expansion), cb = Textur + Konstanten, st = Zustaende/
 * Diagnosen bis zur Aufzeichnung. */
enum { IMP_PRE, IMP_UP, IMP_CB, IMP_ST, IMP_P0, IMP_P1, IMP_P2, IMP_N }; /* pre = Rest vor dem Upload (Expansion), p0..p2 = Teile davor */
extern long long g_imp_q[IMP_N];
extern unsigned g_imp_fastd, g_imp_vbv, g_imp_imv;
extern unsigned g_rqp_draws, g_rqp_shaded, g_rqp_vin, g_rqp_vout, g_rqp_compiles, g_rqp_creuse;
extern unsigned long long g_rqp_xbytes, g_rqp_cbytes;
long long rq_qpc(void);
void rq_perf_present(unsigned f);

#ifdef __d3d11_h__
/* Zustand eines Shading-Draws (fuer Draw und 120-FPS-Wiedergabe). */
typedef struct {
    ID3D11VertexShader *vs, *vsl;   /* exakt / Zwischenbild */
    ID3D11InputLayout *lay, *layl;
    ID3D11PixelShader *ps;
    ID3D11Buffer *xbuf;             /* Stream 2 */
    UINT xoff, xstride;
    ID3D11ShaderResourceView *xsrv; /* SHADE-PERF (W80): Rohdaten als VS-t0 (statt Stream 2), sonst NULL */
    ID3D11Buffer *cbuf;             /* Konstanten-Fenster b3 (VS) / b4 (PS) */
    UINT vfirst, vnum, pfirst, pnum;
    ID3D11ShaderResourceView *srv[4];
    ID3D11SamplerState *smp[4];
} rq_rec_t;

extern int g_rq_active;   /* 1 = aktueller Draw im Shading-Pfad */
extern rq_rec_t g_rq_rec;
struct ID3D11DeviceContext1;
extern struct ID3D11DeviceContext1 *g_rq_ctx1;

/* render_shade.c */
int rq_begin(const float (*vin)[16][4], unsigned n, int vb);
int rq_expand_upload(unsigned prim, unsigned n, unsigned nout);
int rq_bind(ID3D11ShaderResourceView *srv0, int rhw);
void rq_draw(ID3D11Buffer *vb0, unsigned nout, unsigned vb_start);
void rq_stat_log(unsigned f);
ID3D11SamplerState *rq_default_sampler(void);
void rq_dump_srv(ID3D11ShaderResourceView *srv, unsigned xptr); /* DIAG: Textur als BMP */
void rq_dump_srv_named(ID3D11ShaderResourceView *srv, unsigned xptr, const char *name);
int rq_in_mission(void);
int rq_ab_on(void); /* DIAG(W100) */
/* render_im.c: FIX(TEXADDR,W84) Adressmodus Stufe 0 im alten Pfad */
extern ID3D11SamplerState *g_ta_smp;
ID3D11SamplerState *sb_ta_sampler(unsigned au, unsigned av);
void rq_dlg_draw(const im_vertex_t *v, unsigned n, int rhw, ID3D11ShaderResourceView *srv, unsigned prim);
float (*rq_vb_vin(void))[16][4];
int rq_vb_decode(unsigned handle, const unsigned char *v, const float (*cur)[4], float out[16][4], unsigned mask);
unsigned rq_vb_prepare(void);
int rq_vb_raw_set(const unsigned char *vdata, unsigned stride, const unsigned short *idx,
                  unsigned base, unsigned start, unsigned n, const float (*cur)[4]);
void rq_vb_raw_clear(void);
int rq_xsrv_supported(void);
ID3D11ShaderResourceView *rq_mksrv(ID3D11Buffer *b, unsigned bytes);

/* render_interp.c: Tick-Puffer fuer Stream 2 / Konstanten (1 = geschrieben) */
int sb_interp_rq_xwrite(const void *data, unsigned bytes, ID3D11Buffer **buf, UINT *off);
void *sb_interp_rq_xmap(unsigned bytes, ID3D11Buffer **buf, UINT *off, ID3D11ShaderResourceView **srv);
ID3D11Buffer *sb_interp_rq_cpos(UINT *pos);
int sb_interp_rq_cwrite(const void *data, unsigned nconst, ID3D11Buffer **buf, UINT *first);
#endif

#endif /* RENDER_QA_H */
