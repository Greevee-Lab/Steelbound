/* render_interp.c - FPS-INTERP (W69): Zwischenbilder ueber den 30-Hz-Spieltakt.
 *
 * Die Mission tickt mit 30 Hz (1 Update + 1 Swap + wait(2 VBlanks), FPS120.md).
 * Mit [render] interp_fps>0 zeichnet dieses Modul in der Mission jeden Tick
 * (Swap bis Swap) auf D3D11-Ebene auf und gibt ihn als Zwischenbilder aus:
 *  - Aufzeichnung: jeder Draw des IM-Pfads (sb_im_submit) und der Aspect-
 *    Balken mit seinen fertigen Vertexdaten (liegen im Tick-VB, den der
 *    Spiel-Draw selbst beschreibt), MVP + has_tex und dem Pipeline-Zustand
 *    per Get* (RTV/DSV, Viewport, Rasterizer/Scissor, Blend, DepthStencil +
 *    Ref, Textur t0); Clears mit Ziel-View und Werten. Shader/Layout/Sampler
 *    sind im IM-Pfad konstant. Keine Spiel-Speicher-Zugriffe bei Wiedergabe.
 *  - Schatten-Ziele: das Spiel zeichnet (bei interp_fps>0) in einen Offscreen-
 *    Backbuffer (d3d11_render.c). Jede vom Tick benutzte Render-Ressource
 *    (BB, BB-Tiefe, RTs als Ziel oder Textur) bekommt einen Schatten; am Swap
 *    wird der Spiel-Stand jeder Ressource je Liste gesichert. Wiedergaben
 *    zeichnen nur in Schatten (Anfangsstand = Ende des Vorticks), nie in
 *    Spiel-Ziele -> der Takt darf mitten im naechsten Spiel-Tick praesentieren.
 *  - Exakte Wiedergabe (t = 1) pixelgleich zum Spiel-Bild (interp_verify);
 *    das gezeigte Tick-Bild selbst ist die Sicherung des Spiel-BB.
 *  - Zwischenbild (0 < t < 1): Welt-Draws (3D-Pfad oder VSH-P2) des Ticks N
 *    werden ihrem Partner im Tick N-1 zugeordnet (Signatur aus Ziel, VS,
 *    Textur, PS, Primitiv, Vertexzahl + Hash der Eingangs-Positionen) und
 *    clip-linear interpoliert: pos = lerp(p0*M0, p1*M1, t) im Interp-VS (zwei
 *    Streams, zwei Matrizen) - Kamera, bewegte Objekte, CPU-transformierte
 *    RT-Paesse. 2D/RHW (HUD), unveraenderte und nicht zuordenbare Draws exakt
 *    aus Tick N.
 *  - Takt Echtzeit (vblank_hz>0): fester 1/interp_fps-Takt (QPC). Ein Bild zur
 *    Zeit tau zeigt den Spielzustand zur Zeit tau - L (L ~ 1 Tick + Jitter);
 *    Zustandszeit eines Ticks = Fensteranfang (Feldwechsel) + mittlere Rechen-
 *    zeit. Gezeigt wird aus den Spiel-Hooks (Draws/Clears des naechsten Ticks,
 *    Feld-Poll, VBlank-Warten) - alle auf dem Context-Thread.
 *    Testtakt (vblank_hz=0): K = interp_fps/30 Bilder (t = k/K) sofort am Swap.
 * interp_fps=0: g_ip_recording bleibt 0, sb_interp_swap kehrt sofort mit 0
 * zurueck -> exakt der bisherige Pfad. */
#include "render_int.h"
#include "sb_state.h" /* W232: sb_in_mission */
#include <share.h> /* W174: _fsopen/_SH_DENYNO */
#include <d3d11_1.h>
#include "render_qa.h"
#include "render_interp_camfit.h" /* W349 LOC-FRAMES-2 */
#include "sb_perfhud.h" /* W122 PERF-HUD */
#include "render_fx_rem.h" /* W133 REM-FX */
#include "vr.h" /* W134 VR-PROTO */
#include "render_reproj.h" /* W295 WORLD-REPROJ */
#include "render_interp_bb.h" /* W347 GHOST: Billboard-Paarung per Kamera */
#include "render_interp_eye.h" /* W448 LOC-CHAFF-LOOK: Netze hinter der Kamera exakt */
#include "render_interp_shref.h" /* W361 LOC-INTERP-CRASH: Listen halten ihre Schatten-Views */
#include "sb_profile.h" /* W323 LOC-INTERP: g_sba_loc */
#include "loc_render.h" /* W323 LOC-INTERP: Missions-Weiche und 20-Hz-Takt */
#include "loc_aim.h" /* W376 LOC-LOCAL-AIM */
#include "loc_aimassist.h" /* W442 LOC-AIMASSIST */
#include "loc_vpf.h" /* W394 LOC-VIEW-PERFRAME */
#include "loc_hud_math.h" /* W394 */
#include "loc_ret_mark.h" /* W444 LOC-RETICLE-MARK */
#include "loc_aim_law.h" /* W376: Kamera-Korrektur la_cam_corr */
#include "loc_smooth.h" /* W385 LOC-SMOOTH */
#include "loc_smooth_math.h"
#include "render_pace.h" /* W393 LOC-PACING */
#include "loc_rollback.h" /* W439: Rollback-Zaehler (DIAG locret_rep.bin) */
/* W394: Matrizen je Wiedergabe: 0 Kamera (W376), 1 Fadenkreuz (W376), 2 + j Marken des VT j, Kompass, Radar */
#define LA_NM (2 + LS_MAXV + 3)
#define LA_LAW_CMP (3 + LS_MAXV)
#define LA_LAW_RAD (4 + LS_MAXV)
#define LA_LAW_RETX (5 + LS_MAXV) /* W439: Fadenkreuz-Draw, der im Zwischenbild exakt aus Tick N gezeichnet wird */
#define LA_IS_RET(law) ((law) == 2 || (law) == LA_LAW_RETX)
#define LA_ON(on, law) ((law) && (((unsigned)(on) >> ((law) - 1)) & 1u))
#include "xdk_hooks.h" /* W376: hooks_frame_count (DIAG) */
#include <math.h>

#define IP_LISTS 4
#define IP_HIST 3
#define IP_DRAW 1
#define IP_CLR_RTV 2
#define IP_CLR_DSV 3
#define IP_COPY 4 /* W340 LOC-FRAMES: Kopie BB-Ausschnitt -> Textur (LoC-Glow-Kette), Wiedergabe Schatten -> Schatten */
#define IP_VERTS_INIT (1024u * 1024u)      /* Tick-VB 40 MB (Mission ~0,7-1,2 Mio Vertices/Tick) */
#define IP_VERTS_MAX (4u * 1024u * 1024u) /* Deckel je Tick (160 MB) -> sonst Tick ohne Interp */
#define IP_OPS_MAX 65536u
#define IP_SH_MAX 64
#define IP_REFS_MAX 64
/* Zuordnungs-Ergebnis je Draw (ip_op_t.why) */
#define IP_W_NONE 0
#define IP_W_STATIC 1   /* Welt, unveraendert -> exakt */
#define IP_W_LERP 2     /* Welt, Matrix-Interpolation (gleiche Eingangsdaten) */
#define IP_W_RAWLERP 3  /* Welt, neue Eingangsdaten, Vertex i <-> i */
#define IP_W_NOMATCH 4  /* Welt, kein Partner */
#define IP_W_RAWCHG 5   /* Welt, neue Eingangsdaten ueber der Grenze -> exakt */
#define IP_W_REJECT 6   /* Welt, unplausibler Sprung -> exakt */
#define IP_W_BB2D 7     /* 2D im Backbuffer (HUD) -> exakt */
#define IP_W_RT2DLERP 8 /* 2D in RT, Vertex-Zuordnung */
#define IP_W_RT2DEX 9   /* 2D in RT, exakt (kein Partner/unveraendert/unplausibel) */
#define IP_W_N 10

typedef struct {
    unsigned char kind, cls, flag, taken; /* flag: 0 exakt, 1 lerp */
    unsigned char why;  /* Zuordnungs-Ergebnis IP_W_* (Diagnose/Statistik) */
    unsigned sig0;      /* Signatur ohne Vertexzahl (Diagnose: Partner mit anderem n) */
    unsigned gkey;      /* Geometrie-Schluessel: Ziel, Klasse, Vertexzahl, Eingangs-Hash (ohne Shader/Textur) */
    unsigned pvs, pxp, pps; /* DIAG: Signatur-Bestandteile (VS, Textur, PS) */
    unsigned topo;
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    ID3D11BlendState *bs;
    float bf[4];
    UINT smask;
    ID3D11DepthStencilState *ds;
    UINT sref;
    ID3D11RasterizerState *rs;
    D3D11_VIEWPORT vp;
    D3D11_RECT sc;
    ID3D11ShaderResourceView *srv;
    /* Schatten-Views (nicht besitzend; Eintraege leben laenger als Listen) */
    ID3D11RenderTargetView *sh_rtv;
    ID3D11DepthStencilView *sh_dsv;
    ID3D11ShaderResourceView *sh_srv;
    float mvp[16];
    unsigned has_tex;
    unsigned vstart, nv;
    unsigned ib1, nsv;  /* W264 DRAW-OPT2: Indexpuffer-Abschnitt+1 (0 = Liste, Draw(nv)); Vertices im Tick-VB (indiziert: Quell-Vertices) */
    int rzb;          /* FIX(W284) interp_rawzc: Lerp-Strom N-1 aus B (Tick-N-Vertices mit Matrix N-1) statt aus A */
    int rzm;          /* W317 INTERP-PAIR: Matrix N-1 = mva statt A->ops[match].mvp (nur mit rzb; Kamera-Bewegung bzw. Matrix des Mehrpass-Leiters) */
    float mva[16];    /* W317: Matrix N-1 bei rzm */
    float pref[4][4]; /* 4 Bezugs-Vertices (erster, 1/3, 2/3, letzter): Plausibilitaet */
    float rn[4][3];   /* POPIN (W91): deren NDC x/y + w (einmal je Draw, fuer ip_cost) */
    float prefw[4];   /* FIX(W429): echte Clip-w der Bezugs-Vertices bei P2 im RT (Strom schon durch w geteilt) */
    unsigned char p2w; /* W429: 1 = prefw gueltig (w-Nebenpuffer dieses Draws aufgezeichnet) */
    int exj;           /* W429 DIAG: exakter Durchgang, der interp_pass ausloeste */
    unsigned char exr; /* W429 DIAG: gelerpter Draw nachtraeglich exakt (1 = interp_pass, 2 = Ablage Liste/indiziert) */
    unsigned sig, rawhash, outhash;
    int match;
    float col[4]; /* Clear */
    UINT dsflags;
    float z;
    unsigned stencil;
    int rq;                 /* RENDER-QA (W77): Shading-Draw */
    rq_rec_t rqr;           /* dessen Zustand (srv[1..3] mit AddRef) */
    ID3D11ShaderResourceView *rq_shsrv[4]; /* Schatten-SRVs der Stufen 1..3 */
    ID3D11SamplerState *ta_smp; /* FIX(TEXADDR,W84): s0 des alten Draws (NULL = Init-CLAMP; Cache, nicht besitzend) */
    int nolerp;             /* MONITOR-FX (W105): Korn-Wash/Stoerzeilen nie interpolieren (Zufall je Tick) */
    ID3D11GeometryShader *lgs; /* FIX(ISSUE-EMP,W352) line_scale: Linien-Band wie im Tick-Bild (Cache, nicht besitzend) */
    unsigned fxk;           /* W133 REM-FX: 1 = Effekt-Draw des Spiels, 2 = eigener Zusatz-Draw (Glut/Licht) */
    float fxp[8];           /* W133 REM-FX: Parameter (b2) bei has_tex & FXR_MASK */
    /* W195 PHOTO-MODE: Spiel-Viewport (x,y,w,h) und logische RT-Groesse (w,h) bei RT-Draws (0 = BB/unbekannt)
     * sowie die Spiel-Projektion (m0, m5, m10, m14) zur Zeit des Draws - nur fuer die Foto-Wiedergabe
     * (freie Kamera: Clip -> Sicht -> Clip); kein Einfluss auf Aufzeichnung/Wiedergabe sonst. */
    unsigned short vpg[4], rtl[2];
    float prj[4];
    /* W340 LOC-FRAMES (IP_COPY): Quelle (BB-RTV) und Ziel (Textur-SRV) mit AddRef, Ausschnitt, Schatten-Eintraege
     * (am Swap gesetzt, -1 = keiner -> Op ohne Wirkung). rtv/dsv/srv bleiben NULL (keine Draw-/Clear-Op). */
    ID3D11RenderTargetView *cps;
    ID3D11ShaderResourceView *cpd;
    D3D11_BOX cpbox;
    int cp_es, cp_ed;
    unsigned char law; /* W376 LOC-LOCAL-AIM: 1 = Welt-Draw (Kamera-Korrektur), 2 = Fadenkreuz-Draw (Verschiebung);
                        * W394: 3 + j = HUD-Marke des VT j (Verschiebung), LA_LAW_CMP Kompassband, LA_LAW_RAD Radar */
    unsigned char wlc; /* W412 LOC-WORLD-LAG: Klasse im Weltziel (li_smooth_apply): 1 Welt mit Kamera K, 2 eigener VT,
                        * 3 nicht affin zur Kamera, 4 anderer VT (Versatz X), 5 Welt ohne K (keine Vorhersage),
                        * 6 Himmel (Weltkoordinaten, eigene Tiefe) mit K (loc_wlag_fix) */
    unsigned char lsk; /* W435 LOC-SKY-LAG-REG: 1 = Himmel (Weltkoordinaten, eigene Tiefe) am Swap erkannt (li_aim_attach) */
} ip_op_t;
static volatile LONG li_cp_rec, li_cp_ok, li_cp_rep; /* W340: Kopier-Ops aufgezeichnet / mit Schatten / wiedergegeben (LOCINTERP stat) */
static volatile LONG wl_fix_lists, wl_fix_draws; /* W412 LOC-WORLD-LAG: Listen/Draws mit Himmel-Kamera (Zeile LOCINTERP wlag) */
/* W435 LOC-SKY-LAG-REG: Listen/Draws mit Himmel-Sicht-Korrektur (Zeile LOCINTERP sky435); DIAG Bit 4: Wiedergaben mit
 * Sicht-Korrektur, Himmel-Draws darin, davon korrigiert; Listen mit K (W390) und davon mit Himmel-K (W412) */
static volatile LONG sk_lists, sk_draws, sk_rep, sk_rep_on, sk_rep_sky, sk_rep_skyc, sk_rep_na, sk_klists, sk_klists_sky, sk_klists_has;
static volatile LONG sk_rejlerp; /* W435: Himmel unplausibel -> Matrix-Lerp (statt exakt aus Tick N) */
static double sk_last_ang, sk_ang_sum, sk_ang_max; /* DIAG(W435): Winkel der Sicht-Korrektur (rad), Summe/Max in Wiedergaben mit Korrektur */
static volatile LONG li_cp_seen; /* W347: Kopien waehrend der Aufzeichnung (SB: INTERP selfcopy) */
/* W317 INTERP-PAIR: Matrix N-1 eines gelerpten Draws (rzm: eigene Matrix, sonst die des Partners a) */
#define IP_AMVP(o, a) ((o)->rzm ? (o)->mva : (a)->mvp)

/* W209: sparse originals, allocated only for CPU-shifted P2 draws. */
typedef struct ip_cp_s {
    struct ip_cp_s *next;
    unsigned start, n;
    float pos[1][4];
} ip_cp_t;

typedef struct {
    ip_cp_t *cp_original;
    float cp_camera[3]; /* d, k*zoom, zoom of this tick (not a later game frame) */
    ip_op_t *ops;
    unsigned nops, capops;
    unsigned nverts;  /* belegte Vertices im Tick-VB */
    ID3D11Buffer *vb; /* Tick-VB: das Spiel schreibt seine Draws direkt hinein */
    unsigned vbcap, want;
    unsigned frame;   /* Spiel-Present-Nr am Swap */
    unsigned serial;  /* fortlaufend je fertigem Tick */
    unsigned prevser; /* Vortick, gegen den zugeordnet wurde (0 = keiner) */
    int pair_ok;      /* Zuordnung brauchbar (kein Schnitt) */
    int overflow;
    int ready;        /* fertig + gesichert, Wiedergabe moeglich */
    int snap_ok;      /* alle Ressourcen gesichert */
    int refs[IP_REFS_MAX], nrefs; /* benutzte Schatten-Eintraege */
    /* FIX(W361 LOC-INTERP-CRASH): Generation je benutztem Eintrag (beim Sichern) und eigene Referenzen auf
     * dessen Views (rtv/dsv/srv, AddRef beim Sichern, Release in ip_list_reset). Die Ops tragen die Views als
     * rohe Zeiger (sh_rtv/sh_dsv/sh_srv/rq_shsrv); so bleiben sie gueltige Objekte, solange die Liste lebt -
     * auch wenn der Eintrag inzwischen verdraengt/ersetzt wurde (sonst Deferred-Context mit zerstoerter SRV,
     * d3d11 AllocateDependentCLS liest NULL). */
    ip_shref_t refo[IP_REFS_MAX]; /* render_interp_shref.h */
    int bbref;        /* Eintrag des Spiel-BB */
    LONGLONG tstate;  /* Zustandszeit (QPC) */
    LONGLONG tswap;   /* Swap-Zeit (QPC) */
    LONGLONG tvb0;    /* W340 LOC-FRAMES: Feldgrenze beim ersten aufgezeichneten Op (Tick-Anfang), 0 = keiner */
    unsigned ncopy;   /* W340: Anzahl IP_COPY-Ops */
    /* RENDER-QA (W77): Tick-Puffer fuer Stream 2 (Bytes) und Konstanten (float4) */
    ID3D11Buffer *rqx, *rqc;
    ID3D11ShaderResourceView *rqxsrv; /* SHADE-PERF (W80): rqx als Buffer<uint> */
    unsigned rqxcap, rqxpos, rqxwant, rqccap, rqcpos, rqcwant;
    unsigned nodisc;  /* PERF(TICK-PERF,W125): Bit 1/2/4 = vb/rqx/rqc frei (GPU-Fence) -> erstes Map NO_OVERWRITE */
    ID3D11SamplerState *smp0; /* PERF(RENDER-THREAD,W126): s0 des Spiels beim Aufnahme-Start (AddRef) - Anfangszustand der Deferred-Wiedergabe */
    /* PERF(PERF-M2,W174) ip_vb_gpu: GPU-Kopien (DEFAULT) von Tick-VB/Stream 2 je Liste (bleiben bei der Liste, nicht im Reserve-Satz) */
    ID3D11Buffer *vbg, *rqxg;
    ID3D11ShaderResourceView *rqxgsrv;
    unsigned vbgcap, rqxgcap;
    unsigned gpu_ok;  /* Bits 1/2: vbg/rqxg tragen den Inhalt dieses Ticks */
    volatile LONG ga_st; /* W174: Anlage im Hilfs-Thread 0 frei, 1 laeuft, 2 fertig (abholen am Swap) */
    ID3D11Buffer *ga_vbg, *ga_rqxg;
    ID3D11ShaderResourceView *ga_srv;
    unsigned ga_vbcap, ga_rqxcap;
    /* FIX(W237 LINE-FLICKER2) interp_hlerp: echte Clip-w je Tick-VB-Vertex (1.0 = Strom ist schon homogen) */
    float *ipw;
    unsigned ipwcap;
    int ipw_bad;      /* Luecke/Allokation fehlgeschlagen -> Tick ohne homogenen Lerp */
    ID3D11Buffer *wb; /* GPU-Kopie (Strom 3/4 der Lerp-Draws) */
    unsigned wbcap;
    int wb_ok;        /* wb traegt die w dieses Ticks */
    unsigned rh[6];   /* DIAG(W273) interp_rhash Bit 2: GPU-Inhalt am Swap (vb, vbg, rqx, rqxg, rqc, wb) */
    int ddsel;        /* DIAG(W284) interp_ddfile: Tick gewaehlt -> Draw-ID-Bilder je Wiedergabe */
    int la_ok;        /* W376 LOC-LOCAL-AIM: Vorhersage-Delta + Weltkamera gueltig */
    float la_d[4];    /* W376: Sicht Gier/Nick, Arm x/y (vorhergesagt minus simuliert, rad) */
    double la_vp[16]; /* W376: Weltkamera (haeufigste mvp der Welt-Draws vor der ersten BB-Kopie) */
    unsigned la_seq;  /* W376: Nummer des Ruhepunkts der Vorhersage */
    unsigned la_nsk, la_nskl, la_nna; /* W435: Himmel-Draws, davon mit Sicht-Korrektur (law 1), sonstige nicht-affine Welt-Draws mit Tiefe */
    float la_rc[2];   /* W376: Fadenkreuz-Mitte im Weltziel (NDC) laut Simulation */
    float la_rd[2];   /* W376: vorhergesagte Verschiebung des Fadenkreuzes (NDC) */
    int la_ret;       /* W376: Fadenkreuz-Draws markiert (o->law == 2) */
    unsigned la_nret, la_rjmv; /* W439: markierte Fadenkreuz-Draws, wegen Bewegungs-Pruefung verworfene Kandidaten */
    float la_s[2];    /* W394: simulierte Sicht Gier/Nick dieser Liste (rad) */
    /* W394 LOC-HUD-VIEW: je VT simulierte Bildlage a (NDC) und angezeigte Lage als Clip-Vektor c = [q, 1] * VP (q = Lage
     * nach W387-Versatz und W390-Kamera), Marken-Draws je VT; Kompass/Radar: Gier-Abweichung der eigenen Kamera (rad) */
    int hm_ok;
    unsigned hm_vt;   /* Bit j = VT j hat markierte Draws */
    float hm_a[LS_MAXV][2];
    double hm_c[LS_MAXV][4];
    float hm_dyaw;    /* eigene Gier angezeigt - simuliert (W390), rad */
    float hm_gier;    /* eigene simulierte Gier dieser Liste (Kompass-Eichung) */
    int hm_gok;
    unsigned hm_n;    /* markierte Draws (Zaehler) */
    unsigned hm_ncmp; /* Kompass-Draws */
    unsigned hm_nrad; /* Radar-Draws */
} ip_list_t;

typedef struct {
    ID3D11Resource *res; /* Spiel-Ressource (AddRef: Adresse bleibt eindeutig) */
    ID3D11Texture2D *sh;
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    ID3D11ShaderResourceView *srv;
    ID3D11Texture2D *bak[IP_LISTS];
    unsigned bakser[IP_LISTS];
    unsigned used;
    unsigned gen; /* W361: Generation (beim Anlegen, nie 0; 0 = frei) */
    unsigned pin[3]; /* W361: nach fremder Ueberfreigabe selbst gehaltene Zusatz-Referenzen (rtv/dsv/srv) */
} ip_sh_t;

int g_ip_recording; /* 1 = Draws/Clears dieses Ticks werden aufgezeichnet */
unsigned g_ip_skip_tot; /* W122 PERF-HUD: verfallene Zwischenbilder gesamt */
unsigned g_ip_sigparts[3]; /* DIAG: VS/Textur/PS des naechsten rec_draw (render_im.c) */
unsigned g_ip_ib1; /* W264 DRAW-OPT2: Indexpuffer-Abschnitt+1 des naechsten rec_draw_src (render_im.c) */

/* DIAG (interp_diag>=3): CPU-Kopie der Positionen je Liste (Ausreisser-Analyse) */
static float (*ip_cpos[IP_LISTS])[4];
static unsigned ip_ccap[IP_LISTS];
/* DIAG(W224): Clip-w je Tick-VB-Vertex (nur interp_eyediag > 0).
 * W237: bei P2-Draws im RT (Strom w = 1, CPU hat schon durch w geteilt) die echte Clip-w
 * aus render_im.c (g_ip_tw), dazu NDC x/y des Stroms (Sprung je Vertex, ip_cj). */
const float *g_ip_tw;
unsigned g_ip_tw_n, g_ip_tw_prim;
static float *ip_cw[IP_LISTS];
static float (*ip_cj[IP_LISTS])[2];
static unsigned char *ip_cp2[IP_LISTS]; /* 1 = w aus g_ip_tw (P2 im RT) */
static unsigned ip_cwcap[IP_LISTS];
static unsigned st_eyex_draws, st_eyex_verts, st_eyex_ln;
/* W237: Vertex-Sprung-Statistik in zwischengerechneten Draws */
static unsigned st_p2x_draws, st_p2x_verts, st_p2x_hit, st_jmp_verts, st_jmp_fp, st_p2x_ln;
static unsigned st_p2rej; /* W406 interp_p2rej: P2-Draws statt reject gelerpt */
static unsigned st_p2hom[2]; /* W429 interp_p2hom: reject mit gleichen Eingangsdaten gelerpt [0] 3D, [1] P2 */
static unsigned st_jf[IP_W_N], st_jb[IP_W_N], st_jfd[IP_W_N]; /* W237: Sprung vor (beide w>0) / hinter der Kamera je why; Draws mit Sprung vorn */
static unsigned st_wwhy[IP_W_N];
static void ip_cw_rec(int li, unsigned vbcap, unsigned vstart, unsigned n, const float mvp[16],
                      const im_vertex_t *v, const im_vertex_t *src, unsigned prim, unsigned nsrc,
                      const float *tw, unsigned twn, unsigned twp)
{
    unsigned k;
    if (ip_cwcap[li] < vbcap) {
        free(ip_cw[li]); free(ip_cj[li]); free(ip_cp2[li]);
        ip_cw[li] = (float *)malloc((size_t)vbcap * sizeof(float));
        ip_cj[li] = (float (*)[2])malloc((size_t)vbcap * sizeof ip_cj[li][0]);
        ip_cp2[li] = (unsigned char *)malloc((size_t)vbcap);
        ip_cwcap[li] = (ip_cw[li] && ip_cj[li] && ip_cp2[li]) ? vbcap : 0u;
    }
    if (!ip_cwcap[li] || vstart + n > ip_cwcap[li]) { return; }
    for (k = 0; k < n; k++) {
        const float *p = v ? v[k].pos : src[sb_im_exp_src(prim, nsrc, k)].pos;
        float cx = p[0] * mvp[0] + p[1] * mvp[4] + p[2] * mvp[8] + p[3] * mvp[12];
        float cy = p[0] * mvp[1] + p[1] * mvp[5] + p[2] * mvp[9] + p[3] * mvp[13];
        float cw = p[0] * mvp[3] + p[1] * mvp[7] + p[2] * mvp[11] + p[3] * mvp[15];
        ip_cj[li][vstart + k][0] = cw != 0.0f ? cx / cw : 0.0f;
        ip_cj[li][vstart + k][1] = cw != 0.0f ? cy / cw : 0.0f;
        ip_cp2[li][vstart + k] = 0;
        if (tw) {
            unsigned s = sb_im_exp_src(twp, twn, k);
            if (s < twn) { cw = tw[s]; ip_cp2[li][vstart + k] = 1; }
        }
        ip_cw[li][vstart + k] = cw;
    }
}

static ip_list_t g_ipl[IP_LISTS];
static ID3D11Buffer *ip_hw_one; /* FIX(W237): w = 1 (Stride 0) */

/* FIX(W237 LINE-FLICKER2) interp_hlerp: w der n Tick-VB-Vertices ab vstart merken. P2-Draws im RT
 * (render_im.c teilt dort schon durch w, Strom w = 1) liefern die echte Clip-w ihrer Quell-Vertices
 * (tw, Expansion wie im Tick-VB); alle anderen Draws tragen w schon im Strom -> 1.0. */
static void ip_w_rec(ip_list_t *L, unsigned vstart, unsigned n, const float *tw, unsigned twn, unsigned twp)
{
    unsigned k;
    float *d;
    if (L->ipw_bad) { return; }
    if (L->ipwcap < L->vbcap) {
        float *nw = (float *)realloc(L->ipw, (size_t)L->vbcap * sizeof(float));
        if (!nw) { L->ipw_bad = 1; return; }
        L->ipw = nw;
        L->ipwcap = L->vbcap;
    }
    if (vstart + n > L->ipwcap) { L->ipw_bad = 1; return; }
    d = L->ipw + vstart;
    if (!tw) {
        for (k = 0; k < n; k++) { d[k] = 1.0f; }
    } else if (twp != 3u && twp != 6u && twp != 7u && twp != 8u && twp != 9u && twp != 10u && n <= twn) {
        memcpy(d, tw, (size_t)n * sizeof(float)); /* Liste: Vertex k = Quelle k */
    } else {
        for (k = 0; k < n; k++) {
            unsigned q = sb_im_exp_src(twp, twn, k);
            d[k] = q < twn ? tw[q] : 1.0f;
        }
    }
}

/* Am Swap: w des Ticks in den GPU-Puffer (ein UpdateSubresource, 4 Byte je Vertex). */
static unsigned st_hw_up, st_hw_fail;
static void ip_w_upload(ip_list_t *L)
{
    D3D11_BOX b;
    L->wb_ok = 0;
    if (!sb_ip_hlerp() || L->ipw_bad || !L->ipw || !L->nverts || L->nverts > L->ipwcap) {
        if (sb_ip_hlerp() && L->nverts) { st_hw_fail++; }
        return;
    }
    if (!L->wb || L->wbcap < L->nverts) {
        D3D11_BUFFER_DESC bd;
        unsigned cap = L->vbcap > L->nverts ? L->vbcap : L->nverts;
        if (L->wb) { ID3D11Buffer_Release(L->wb); L->wb = NULL; L->wbcap = 0; }
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = cap * (UINT)sizeof(float);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &L->wb))) { L->wb = NULL; st_hw_fail++; return; }
        L->wbcap = cap;
    }
    memset(&b, 0, sizeof b);
    b.right = L->nverts * (UINT)sizeof(float);
    b.bottom = 1;
    b.back = 1;
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)L->wb, 0, &b, L->ipw, 0, 0);
    L->wb_ok = 1;
    st_hw_up++;
}

static unsigned st_hw_draws, st_hw_one;
static void ip_hw_bind(ID3D11DeviceContext *c, const ip_list_t *B, const ip_list_t *A, unsigned bstart, unsigned astart)
{
    ID3D11Buffer *vb[2];
    UINT st[2], of[2];
    if (!sb_ip_hlerp() || !ip_hw_one) { return; }
    if (B->wb_ok && A->wb_ok && B->wb && A->wb) {
        vb[0] = B->wb; vb[1] = A->wb;
        st[0] = st[1] = (UINT)sizeof(float);
        of[0] = bstart * (UINT)sizeof(float); of[1] = astart * (UINT)sizeof(float);
        st_hw_draws++;
    } else {
        vb[0] = vb[1] = ip_hw_one;
        st[0] = st[1] = 0;
        of[0] = of[1] = 0;
        st_hw_one++;
    }
    ID3D11DeviceContext_IASetVertexBuffers(c, 3, 2, vb, st, of);
}
static ip_sh_t g_ish[IP_SH_MAX];
static int ip_rec = -1;
static int ip_hist[IP_HIST], ip_nhist;
static unsigned ip_serial;
static int ip_init_done, ip_init_fail;
static ID3D11VertexShader *ip_vs2;
static ID3D11InputLayout *ip_lay2;

/* FIX(W237) interp_hlerp: Strom 3/4 = Clip-w von Tick N (B, ab o->vstart) und N-1 (A, ab a->vstart);
 * ohne w-Puffer eines der beiden Ticks w = 1 fuer beide (alter Bildraum-Lerp). */
static void ip_hw_bind(ID3D11DeviceContext *c, const ip_list_t *B, const ip_list_t *A, unsigned bstart, unsigned astart);
static ID3D11Buffer *ip_cb0, *ip_cb1;
static ID3D11PixelShader *ip_dbg_ps; /* DIAG: Klassifikationsbild (interp_diag>=3) */
static ID3D11PixelShader *ip_id_ps;  /* DIAG(W251 INTERP-GROUND): Draw-ID-Bild (nur SV_Position, Farbe aus b1) */
static unsigned ip_dd_lo, ip_dd_ser; /* DIAG(W251): Listen B (Serien lo..ser), deren Bilder ID-Bilder bekommen (interp_ddrel) */
/* DIAG(W284 INTERP-RAWCHG) interp_ddfile: Tick-Liste (relativ zum Missionsbeginn) statt eines Bereichs */
static unsigned *ip_ddf_t, ip_ddf_n;
static int ip_ddf_state; /* 0 = nicht geladen, 1 = geladen, -1 = aus */
static int ip_dd_on(void) { return g_cfg.interp_ddrel > 0 || g_cfg.interp_ddfile[0] != 0; }
static int ip_ddf_has(unsigned rel)
{
    unsigned k;
    if (ip_ddf_state == 0) {
        FILE *f = NULL;
        unsigned cap = 0;
        ip_ddf_state = -1;
        if (fopen_s(&f, g_cfg.interp_ddfile, "r") == 0 && f) {
            int c, have = 0;
            unsigned v = 0;
            for (;;) {
                c = fgetc(f);
                if (c >= '0' && c <= '9') { v = v * 10u + (unsigned)(c - '0'); have = 1; continue; }
                if (have) {
                    if (ip_ddf_n >= cap) {
                        unsigned nc = cap ? cap * 2u : 256u;
                        unsigned *nn = (unsigned *)realloc(ip_ddf_t, nc * sizeof *nn);
                        if (!nn) { break; }
                        ip_ddf_t = nn; cap = nc;
                    }
                    ip_ddf_t[ip_ddf_n++] = v;
                }
                v = 0; have = 0;
                if (c == EOF) { break; }
            }
            fclose(f);
            ip_ddf_state = ip_ddf_n ? 1 : -1;
        }
        HLOG_I("INTERP ddfile: %u Ticks geladen", ip_ddf_n);
    }
    if (ip_ddf_state <= 0) { return 0; }
    for (k = 0; k < ip_ddf_n; k++) { if (ip_ddf_t[k] == rel) { return 1; } }
    return 0;
}
static ID3D11Buffer *ip_dbg_cb;
static LARGE_INTEGER ip_qpf;
static int ip_K = 1;

/* Takt (Echtzeit) */
static int ip_live;           /* 1 = Interp-Takt zeigt die Bilder */
static int ip_in_poll;        /* Rekursionsschutz */
static LONGLONG ip_tnext;     /* naechstes Present (QPC) */
static LONGLONG ip_dt;        /* 1/interp_fps (QPC) */
static LONGLONG ip_lat;       /* Latenz L (QPC) */
/* W393 LOC-PACING interp_pace_clock: Anzeige-Latenz, die ip_lat (Ziel) nur weich folgt (stetige Bilduhr), und die
 * Faelligkeit, fuer die sie gilt. Schreiben nur ip_poll (Bild gezeigt), lesen ip_pair_at - beide unter ctx_lock. */
static LONGLONG ip_lat_d, ip_lat_due;
static LONGLONG ip_ts_laste; /* W393: vorige Abweichung der Tick-Zeitbasis (pace_ts_step) */
static int ip_pace_on(void)
{
    return g_cfg.interp_pace_clock >= 2 || (g_cfg.interp_pace_clock == 1 && g_sba_loc);
}
/* Latenz fuer die Faelligkeit due ohne Uebernahme (Vorbestellung und Bild sehen denselben Wert). */
static LONGLONG ip_lat_at(LONGLONG due)
{
    if (!ip_pace_on() || ip_lat_d <= 0) { return ip_lat; }
    if (due <= ip_lat_due) { return ip_lat_d; }
    /* L steigt: Uhr bis 20 % langsamer; L faellt: bis 2 % schneller; > 100 ms (Haenger) sofort */
    return (LONGLONG)pace_lat_step(ip_lat_d, ip_lat, due - ip_lat_due, g_cfg.interp_pace_up > 0 ? g_cfg.interp_pace_up : 200,
                                   g_cfg.interp_pace_dn > 0 ? g_cfg.interp_pace_dn : 20, ip_qpf.QuadPart / 10);
}
/* W393: Halten (naechster Tick fehlt, juengster Stand B steht exakt). Die Bilduhr dehnt sich mit: Latenz so, dass die
 * gezeigte Zeit genau bei B bleibt. Kommt der Tick, laeuft das Bild nahtlos von B weiter (vorher: Sprung um die
 * Haltezeit nach vorn) und holt die Latenz mit hoechstens interp_pace_dn wieder ab. Ueber Ziel + 100 ms (Haenger)
 * nicht weiter dehnen - dann springt die Uhr wie bisher (Snap). */
static unsigned pc_elastic; /* DIAG */
static void ip_lat_hold(LONGLONG due, const ip_list_t *B)
{
    LONGLONG need;
    if (!g_cfg.interp_pace_hold || !ip_pace_on() || ip_lat_d <= 0 || !B) { return; }
    need = due - B->tstate;
    if (pace_lat_hold(ip_lat_at(due), need, ip_lat, ip_qpf.QuadPart / 10) == need) {
        ip_lat_d = need;
        ip_lat_due = due;
        pc_elastic++;
    }
}
/* Nach dem gezeigten Bild: Latenz fuer due uebernehmen. */
static void ip_lat_commit(LONGLONG due)
{
    if (!ip_pace_on()) { ip_lat_d = 0; return; }
    ip_lat_d = ip_lat_d > 0 ? ip_lat_at(due) : ip_lat;
    ip_lat_due = due;
}
static LONGLONG ip_need[32];  /* Swap_N - Zustandszeit_N-1 */
static unsigned ip_nneed;
static LONGLONG ip_vb_qpc;    /* letzter Feldwechsel im Feld-Poll (QPC) */
static double ip_rbar;        /* mittlere Rechenzeit r (QPC-Einheiten) */
static LONGLONG ip_tlast_present;
static LONGLONG ip_tlast_present_pre; /* DIAG(W393): QPC direkt vor dem letzten Present */
static volatile LONG pv_presents; /* W393: Zwischenbilder seit dem letzten VBlank (pace_vsync/pace_diag) */
static HANDLE pd_wake; /* W138 PERF-FIX: Takt-Thread wecken (Interp-Takt startet) */

static unsigned ip_shot_seq, ip_shot_left, ip_shot_next, ip_pairs_total;
static unsigned ip_shot_ser; /* Liste, deren Bilder gespeichert werden (0 = keine) */

/* Statistik */
static unsigned st_ticks, st_pairs, st_presents, st_lerp, st_static, st_nomatch,
    st_rawchg, st_reject, st_2d, st_cut, st_skip, st_hold, st_late, st_nopair, st_ovf,
    st_verify, st_verify_bad, st_exact, st_rawlerp, st_rt2d, st_geo;
/* POPIN (W91): Instanz-Zuordnung (gleiche Signatur + gleiche Eingangsdaten,
 * mehrere Kandidaten = mehrfach gezeichnetes Modell, z. B. Haeuser) */
static unsigned st_imulti, st_iwrong, st_iamb, st_ipass;
static unsigned st_ipass_false; /* W273: Fehltreffer der alten 16-Bit-Maske (mit interp_pass_exact verworfen) */
static double st_rec_ms, st_rep_ms, st_rep_max, st_pres_ms, st_r_ms, st_r_max, st_lat_ms;
static unsigned st_r_n, st_rep_n, st_lat_n;
static LONGLONG st_cap_q, st_t0;
static double st_gpu_ms, st_gpu_max;
static unsigned st_gpu_n;

static void ip_poll(void);
static void ip_rt_quiesce(void); /* W126 RENDER-THREAD */
static void ip_ev_mark(void); /* W125 TICK-PERF */
static int ip_in_rec; /* W122 PERF-HUD: ip_poll aus rec_draw/rec_clear (Zeit steckt in cap) */
static void ip_clip(const float p[4], const float m[16], float c[4]);

static double ip_ms(LONGLONG a, LONGLONG b)
{
    return (double)(b - a) * 1000.0 / (double)ip_qpf.QuadPart;
}

static LONGLONG ip_now(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}

/* Hash ueber die Position (erste 4 Worte) von hoechstens 64 gleichmaessig
 * verteilten Vertices + n. Stichprobe genuegt: "gleich" heisst nur "exakt
 * aus Tick N zeichnen" bzw. "Vertex i auf Vertex i lerpen" - beides bleibt
 * bei einem unentdeckten Unterschied wohldefiniert. */
/* W323 LOC-INTERP: Spieltakt der Zwischenbilder. SB: 30 Hz (unveraendert). LoC-Profil: Missionstakt
 * 20 Hz (FUN_00033050(3) = 3 VBlank-Felder je Bild). Nur fuer K (Testtakt: K Bilder je Tick) und Logs;
 * der Echtzeit-Takt ist zeitbasiert (t aus den Zustandszeiten), braucht also keine Taktannahme. */
static int ip_tick_hz(void)
{
    return g_sba_loc ? loc_render_tick_hz() : 30;
}

/* W323: Zeit des letzten Feldwechsels. Im LoC-Profil schreibt der Haupt-Thread (Feld-Poll) und liest der
 * Spiel-Render-Thread (Swap) -> 64 Bit atomar (x86-32). SB: ein Thread, unveraendert. */
static LONGLONG ip_vb_get(void)
{
    return g_sba_loc ? InterlockedCompareExchange64(&ip_vb_qpc, 0, 0) : ip_vb_qpc;
}

unsigned sb_interp_hash(const void *base, unsigned stride, unsigned n)
{
    const unsigned char *b = (const unsigned char *)base;
    unsigned h = 2166136261u, i, step = n > 64u ? n / 64u : 1u;
    for (i = 0; i < n; i += step) {
        const unsigned *w = (const unsigned *)(b + (size_t)i * stride);
        h = (h ^ w[0]) * 16777619u;
        h = (h ^ w[1]) * 16777619u;
        h = (h ^ w[2]) * 16777619u;
        h = (h ^ w[3]) * 16777619u;
    }
    return h ^ n;
}

static void ip_op_release(ip_op_t *o)
{
    if (o->rtv) { ID3D11RenderTargetView_Release(o->rtv); }
    if (o->dsv) { ID3D11DepthStencilView_Release(o->dsv); }
    if (o->bs) { ID3D11BlendState_Release(o->bs); }
    if (o->ds) { ID3D11DepthStencilState_Release(o->ds); }
    if (o->rs) { ID3D11RasterizerState_Release(o->rs); }
    if (o->srv) { ID3D11ShaderResourceView_Release(o->srv); }
    if (o->cps) { ID3D11RenderTargetView_Release(o->cps); } /* W340 */
    if (o->cpd) { ID3D11ShaderResourceView_Release(o->cpd); } /* W340 */
    if (o->rq) {
        int k;
        for (k = 1; k < 4; k++) { if (o->rqr.srv[k]) { ID3D11ShaderResourceView_Release(o->rqr.srv[k]); } }
    }
}

/* W361: eigene Referenzen der Liste auf Schatten-Views abgeben (vor nrefs = 0). */
static void ip_list_unref(ip_list_t *L)
{
    int j;
    for (j = 0; j < L->nrefs && j < IP_REFS_MAX; j++) { ip_shref_drop(&L->refo[j]); }
}

static void ip_list_reset(ip_list_t *L)
{
    unsigned i;
    ip_list_unref(L); /* W361 */
    while (L->cp_original) {
        ip_cp_t *p = L->cp_original;
        L->cp_original = p->next;
        free(p);
    }
    for (i = 0; i < L->nops; i++) {
        ip_op_release(&L->ops[i]);
    }
    L->cp_camera[0] = 0.0f; L->cp_camera[1] = L->cp_camera[2] = 1.0f;
    L->nops = 0;
    L->nverts = 0;
    L->rqxpos = 0;
    L->rqcpos = 0;
    L->overflow = 0;
    L->ready = 0;
    L->snap_ok = 0;
    L->frame = 0;
    L->serial = 0;
    L->prevser = 0;
    L->pair_ok = 0;
    L->nrefs = 0;
    L->bbref = -1;
    L->nodisc = 0;
    L->gpu_ok = 0; /* W174 */
    L->ipw_bad = 0; L->wb_ok = 0; /* W237 */
    L->ddsel = 0; /* W284 */
    L->tvb0 = 0; L->ncopy = 0; /* W340 */
    L->la_ok = 0; L->la_seq = 0; /* W376 */
    if (L->smp0) { ID3D11SamplerState_Release(L->smp0); L->smp0 = NULL; } /* W126 */
}

static ip_op_t *ip_new_op(ip_list_t *L)
{
    ip_op_t *o;
    if (L->nops >= L->capops) {
        unsigned nc = L->capops ? L->capops * 2u : 2048u;
        ip_op_t *no;
        if (nc > IP_OPS_MAX) { L->overflow = 1; return NULL; }
        no = (ip_op_t *)realloc(L->ops, nc * sizeof *no);
        if (!no) { L->overflow = 1; return NULL; }
        L->ops = no;
        L->capops = nc;
    }
    if (!L->nops && g_sba_loc && g_cfg.loc_interp_anchor) {
        /* W340 LOC-FRAMES: Tick-Anfang merken. Der Spiel-Render-Thread beginnt ~0,2 ms nach dem Feldwechsel, der den
         * Tick freigibt; am Swap (Renderdauer bis ~18 ms bei 3440x1440) ist oft schon das naechste Feld da -> die
         * Zustandszeit sprang um 16,7 ms (zustand_abw), die Zwischenbilder liefen ungleichmaessig. */
        L->tvb0 = ip_vb_get();
    }
    o = &L->ops[L->nops++];
    memset(o, 0, sizeof *o);
    o->match = -1;
    return o;
}

/* PERF(TICK-PERF,W125) interp_bsets=N: Reserve-Saetze der Tick-Puffer.
 * Ursache (W125_b2/b3, TICKBLK ipmap 5-18 ms/Tick): die Aufnahme nimmt die
 * gerade aus der Historie gefallene Liste; deren Tick-Puffer (40-MB-VB,
 * 64-MB-Stream-2, Konstanten) liest die GPU noch fuer eingereihte
 * Zwischenbilder (Paar A/B); das erste DISCARD-Map je Tick wartet dann bzw.
 * legt neuen Speicher an (W125_n1: auch bei freiem Satz noch 3,5 ms/Tick ->
 * freier Satz wird ab 0 per NO_OVERWRITE beschrieben, interp_bsets_nod). Neu: beim
 * Aufnahme-Start gibt die Liste ihren Satz mit einer Event-Query (= nach allen
 * bisher eingereihten GPU-Befehlen) in die Reserve und nimmt einen Satz, den
 * die GPU fertig gelesen hat (sonst den aeltesten). Inhalt und Lage der Daten
 * je Draw sind unveraendert (gleiche Offsets), nur der Speicher wechselt. */
#define IP_BS_MAX 4
typedef struct {
    ID3D11Buffer *vb, *rqx, *rqc;
    ID3D11ShaderResourceView *rqxsrv;
    unsigned vbcap, rqxcap, rqccap;
    ID3D11Query *ev;  /* Fence: GPU fertig mit allen Befehlen vor der Abgabe */
    unsigned ser;     /* Abgabe-Nr (aeltester = kleinste) */
    int used;
} ip_bset_t;
static ip_bset_t ip_bs[IP_BS_MAX];
static unsigned ip_bs_ser;
unsigned g_ip_bs_idle, g_ip_bs_busy, g_ip_bs_new; /* DIAG(TICK-PERF,W125): TICKBLK */
static int g_ip_bs_last_idle;

/* PERF(PERF-SPIKE,W318) interp_bsets_imm=N: ohne Render-Thread (render_thread=0,
 * AMD-Weg W302 ohne treibereigene Command Lists) spielt der Takt-Thread die
 * Zwischenbilder direkt auf dem Immediate-Context ab; bei GPU-Last liest die GPU
 * dann laenger an den Tick-Puffern, beide Reserve-Saetze sind belegt und das
 * DISCARD-Map der 40-64-MB-Puffer blockiert den Spiel-Thread (W318_g50r0:
 * ipmap 9-12 ms/Tick, 25,9 Ticks/s, 16,8 s Stillstand im Kampf M1). Dort gilt
 * mindestens N Saetze (Standard 4 = IP_BS_MAX). interp_bsets=0 bleibt "aus". */
static int ip_bsets_n(void)
{
    int n = g_cfg.interp_bsets;
    if (n > 0 && g_cfg.render_thread <= 0 && g_cfg.interp_bsets_imm > n) { n = g_cfg.interp_bsets_imm; }
    return n > IP_BS_MAX ? IP_BS_MAX : n;
}

static void ip_bset_rotate(ip_list_t *L)
{
    int n = ip_bsets_n(), i, best = -1, empty = -1; /* W318: interp_bsets_imm */
    unsigned bser = 0xFFFFFFFFu;
    ip_bset_t t;
    if (n <= 0) { return; }
    if (!L->vb && !L->rqx && !L->rqc) { return; } /* erster Lauf: Liste legt neu an */
    for (i = 0; i < n; i++) {
        BOOL done = FALSE;
        if (!ip_bs[i].used) { if (empty < 0) { empty = i; } continue; }
        if (ip_bs[i].ev &&
            ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)ip_bs[i].ev, &done, sizeof done,
                                        D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && done) {
            if (ip_bs[i].ser < bser) { bser = ip_bs[i].ser; best = i; }
        }
    }
    g_ip_bs_last_idle = 0;
    if (best >= 0) { g_ip_bs_idle++; g_ip_bs_last_idle = (g_cfg.interp_bsets_nod != 0); }
    else if (empty >= 0) { best = empty; g_ip_bs_new++; } /* leerer Platz: Liste legt neue Puffer an */
    else { /* alle noch in Arbeit: aeltesten nehmen (wartet ggf. wie bisher) */
        for (i = 0; i < n; i++) { if (ip_bs[i].ser < bser) { bser = ip_bs[i].ser; best = i; } }
        g_ip_bs_busy++;
    }
    if (best < 0) { return; }
    t = ip_bs[best];
    ip_bs[best].vb = L->vb; ip_bs[best].vbcap = L->vbcap;
    ip_bs[best].rqx = L->rqx; ip_bs[best].rqxsrv = L->rqxsrv; ip_bs[best].rqxcap = L->rqxcap;
    ip_bs[best].rqc = L->rqc; ip_bs[best].rqccap = L->rqccap;
    if (!ip_bs[best].ev) {
        D3D11_QUERY_DESC qd;
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_EVENT;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &ip_bs[best].ev))) { ip_bs[best].ev = NULL; }
    }
    if (ip_bs[best].ev) { ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)ip_bs[best].ev); }
    ip_bs[best].ser = ++ip_bs_ser;
    ip_bs[best].used = 1;
    if (t.used) {
        L->vb = t.vb; L->vbcap = t.vbcap;
        L->rqx = t.rqx; L->rqxsrv = t.rqxsrv; L->rqxcap = t.rqxcap;
        L->rqc = t.rqc; L->rqccap = t.rqccap;
        /* Fence fertig: die GPU liest den Satz nicht mehr -> erstes Map ab 0
         * per NO_OVERWRITE (DISCARD legt bei 40-64-MB-Puffern neuen Speicher an,
         * W125_n1: ipmap 3,5 ms/Tick trotz freiem Satz). Belegt -> DISCARD. */
        if (g_ip_bs_last_idle) { L->nodisc = (L->vb ? 1u : 0u) | (L->rqx ? 2u : 0u) | (L->rqc ? 4u : 0u); }
    } else {
        L->vb = NULL; L->vbcap = 0;
        L->rqx = NULL; L->rqxsrv = NULL; L->rqxcap = 0;
        L->rqc = NULL; L->rqccap = 0;
    }
}

/* DIAG(TICK-PERF,W125): Map der Tick-Puffer; DISCARD (erster Zugriff je Tick)
 * mit Zeitmesser TB_IPMAP (perf_blk). */
static HRESULT ip_map(ID3D11Buffer *b, D3D11_MAP t, D3D11_MAPPED_SUBRESOURCE *m)
{
    HRESULT hr;
    long long q = (t == D3D11_MAP_WRITE_DISCARD) ? TB_BEGIN() : 0;
    WD_CALL(t == D3D11_MAP_WRITE_DISCARD ? "ip_map_disc" : "ip_map", NULL,
            hr = ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)b, 0, t, 0, m)); /* DIAG(PERF-WARM,W139) */
    if (t == D3D11_MAP_WRITE_DISCARD) { TB_END(TB_IPMAP, q); }
    return hr;
}

/* PERF(PERF-WARM,W139) warm_vb=1: Tick-Puffer (VB 40-60 MB, Stream 2 64 MB,
 * Konstanten 4 MB je Liste) mit Anfangsdaten (Nullen) anlegen und ohne
 * ctx_lock. Befund W139_c0m1/W139_b0m1 (warm_diag): Das erste
 * Map(WRITE_DISCARD) eines neu angelegten Tick-Puffers dauert 8-36 ms unter
 * ctx_lock (der Treiber legt den Speicher erst dort an) - beim Missionsstart
 * und Retry je Liste einmal -> Present-Takt steht. Mit Anfangsdaten legt der
 * Treiber den Speicher schon beim Anlegen an (Device-Aufruf, frei-threaded).
 * Inhalt ohne Wirkung: jede Aufnahme schreibt ihren Bereich vor dem Lesen
 * (erstes Map je Tick ist DISCARD). Die Nullen kommen aus einem kurzlebigen
 * VirtualAlloc-Block (Nullseiten des Systems, danach freigegeben). */
static HRESULT ip_mkbuf(const D3D11_BUFFER_DESC *bd, ID3D11Buffer **out)
{
    HRESULT hr;
    int cr;
    void *z = NULL;
    D3D11_SUBRESOURCE_DATA sd;
    if (g_cfg.warm_vb <= 0) { return ID3D11Device_CreateBuffer(g_dev, bd, NULL, out); }
    cr = sb_ctx_release_all();
    z = VirtualAlloc(NULL, bd->ByteWidth, MEM_COMMIT | MEM_RESERVE, PAGE_READONLY);
    memset(&sd, 0, sizeof sd);
    sd.pSysMem = z;
    hr = ID3D11Device_CreateBuffer(g_dev, bd, z ? &sd : NULL, out);
    if (z) { VirtualFree(z, 0, MEM_RELEASE); }
    sb_ctx_reacquire(cr);
    return hr;
}

/* Tick-VB anlegen/vergroessern (beim Start einer Aufnahme). */
static int ip_vb_ensure(ip_list_t *L)
{
    D3D11_BUFFER_DESC bd;
    if (!L->want) { /* PERF(PERF-BENCH,W137): interp_vb_kv = Startgroesse in 1024 Vertices (0 = alt 1024K) */
        L->want = g_cfg.interp_vb_kv > 0 ? (unsigned)g_cfg.interp_vb_kv * 1024u : IP_VERTS_INIT;
        if (L->want > IP_VERTS_MAX) { L->want = IP_VERTS_MAX; }
    }
    if (L->vb && L->vbcap >= L->want) { return 1; }
    L->nodisc &= ~1u; /* W125: neuer Puffer -> erstes Map DISCARD */
    if (L->vb) { ID3D11Buffer_Release(L->vb); L->vb = NULL; L->vbcap = 0; }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = L->want * (unsigned)sizeof(im_vertex_t);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    { int cr = g_cfg.warm_vb > 0 ? 0 : sb_ctx_release_all(); /* W138: 40-60-MB-Puffer ohne ctx_lock anlegen (Device; L ist die neue Aufnahme, nicht in der Historie) */
    HRESULT hr = ip_mkbuf(&bd, &L->vb); /* W139 warm_vb: mit Anfangsdaten, gibt selbst ab */
    sb_ctx_reacquire(cr);
    if (FAILED(hr)) {
        L->vb = NULL;
        return 0;
    } }
    L->vbcap = L->want;
    HLOG_I("INTERP: Tick-VB %u Vertices (%u MB)", L->vbcap,
           (unsigned)((unsigned long long)L->vbcap * sizeof(im_vertex_t) >> 20));
    return 1;
}

/* RENDER-QA (W77): Tick-Puffer fuer Stream 2 / Konstanten-Fenster des
 * Shading-Pfads (nur shading=1). 1 = vorhanden. */
#define IP_RQX_INIT (64u << 20)
#define IP_RQX_MAX (256u << 20)
#define IP_RQC_INIT (256u * 1024u)
#define IP_RQC_MAX (2048u * 1024u)
static int ip_rq_ensure(ip_list_t *L)
{
    D3D11_BUFFER_DESC bd;
    if (!g_cfg.shading) { return 1; }
    if (!L->rqxwant) { L->rqxwant = IP_RQX_INIT; }
    if (!L->rqcwant) { L->rqcwant = IP_RQC_INIT; }
    if (!L->rqx || L->rqxcap < L->rqxwant) {
        L->nodisc &= ~2u; /* W125 */
        if (L->rqxsrv) { ID3D11ShaderResourceView_Release(L->rqxsrv); L->rqxsrv = NULL; }
        if (L->rqx) { ID3D11Buffer_Release(L->rqx); L->rqx = NULL; L->rqxcap = 0; }
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = L->rqxwant;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        /* SHADE-PERF (W80): auch als Buffer<uint> (Roh-Eingaenge, VS-t0) */
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER | (rq_xsrv_supported() ? D3D11_BIND_SHADER_RESOURCE : 0u);
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ip_mkbuf(&bd, &L->rqx))) { L->rqx = NULL; return 0; } /* W139 warm_vb */
        L->rqxcap = L->rqxwant;
        if (rq_xsrv_supported()) { L->rqxsrv = rq_mksrv(L->rqx, L->rqxcap); }
    }
    if (!L->rqc || L->rqccap < L->rqcwant) {
        L->nodisc &= ~4u; /* W125 */
        if (L->rqc) { ID3D11Buffer_Release(L->rqc); L->rqc = NULL; L->rqccap = 0; }
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = L->rqcwant * 16u;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ip_mkbuf(&bd, &L->rqc))) { L->rqc = NULL; return 0; } /* W139 warm_vb */
        L->rqccap = L->rqcwant;
    }
    return 1;
}

/* PERF(PERF-M2,W174) ip_vb_gpu: Die Tick-Puffer sind DYNAMIC (CPU schreibt,
 * Treiber legt sie i. d. R. in Systemspeicher). Jede Zwischenbild-Wiedergabe
 * liest den ganzen Tick (lerp-Draws zwei Stroeme) erneut ueber PCIe. Am Swap
 * eine GPU-Kopie in einen DEFAULT-Puffer der Liste (einmal je Tick); die
 * Wiedergabe liest daraus. Inhalt und Offsets gleich -> gleiche Bilder.
 * Bits: 1 VB, 2 Stream 2/Roh-Eingaenge (rqx), 4 Konstanten-Fenster (rqc). */
static unsigned st_vbg_copy, st_vbg_fail;
static double st_vbg_mb;
/* DIAG(PERF-M2,W174) ip_gdiag: GPU-Zeit der Kopien am Swap (Timestamps, asynchron) */
#define GC_N 8
static ID3D11Query *gc_dj[GC_N], *gc_t0[GC_N], *gc_t1[GC_N];
static int gc_st[GC_N], gc_fail;
static double gc_ms, gc_mx;
static unsigned gc_n;
static void gc_poll(void)
{
    int k;
    for (k = 0; k < GC_N; k++) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 t0, t1;
        if (gc_st[k] != 2) { continue; }
        if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)gc_dj[k], &dj, sizeof dj,
                                        D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) { continue; }
        gc_st[k] = 0;
        if (dj.Disjoint || !dj.Frequency ||
            ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)gc_t0[k], &t0, sizeof t0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)gc_t1[k], &t1, sizeof t1, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            t1 < t0) { continue; }
        {
            double ms = (double)(t1 - t0) * 1000.0 / (double)dj.Frequency;
            gc_ms += ms; gc_n++;
            if (ms > gc_mx) { gc_mx = ms; }
        }
    }
}
static int gc_begin(void)
{
    int k;
    if (g_cfg.ip_gdiag <= 0 || gc_fail) { return -1; }
    gc_poll();
    for (k = 0; k < GC_N; k++) { if (gc_st[k] == 0) { break; } }
    if (k >= GC_N) { return -1; }
    if (!gc_dj[k]) {
        D3D11_QUERY_DESC qd;
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &gc_dj[k]))) { gc_fail = 1; return -1; }
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &gc_t0[k])) ||
            FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &gc_t1[k]))) { gc_fail = 1; return -1; }
    }
    ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous *)gc_dj[k]);
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)gc_t0[k]);
    return k;
}
static void gc_end(int k)
{
    if (k < 0) { return; }
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)gc_t1[k]);
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)gc_dj[k]);
    gc_st[k] = 2;
}
static ID3D11Buffer *ip_mk_default(unsigned bytes, UINT bind)
{
    D3D11_BUFFER_DESC bd;
    ID3D11Buffer *b = NULL;
    int cr = sb_ctx_release_all(); /* Device-Aufruf ohne ctx_lock (40-64 MB) */
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = bind;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &b))) { b = NULL; }
    sb_ctx_reacquire(cr);
    return b;
}

/* Neuanlage (Tick-VB gewachsen, z. B. M2 60 -> 90 MB): Ein DEFAULT-Puffer
 * dieser Groesse kostet beim Anlegen 65-75 ms (W174_r1) - am Swap haette das den
 * Tick aufgehalten. Daher im Hilfs-Thread (Device-Aufruf, frei-threaded);
 * bis zur Abholung liest die Wiedergabe dieser Liste wie bisher den DYNAMIC-
 * Puffer (gleiche Daten). Beim ersten Present (sb_interp_prealloc) synchron. */
static unsigned st_vbg_async;
static DWORD WINAPI ip_ga_work(LPVOID p)
{
    ip_list_t *L = (ip_list_t *)p;
    if (L->ga_vbcap) { L->ga_vbg = ip_mk_default(L->ga_vbcap * (unsigned)sizeof(im_vertex_t), D3D11_BIND_VERTEX_BUFFER); }
    if (L->ga_rqxcap) {
        L->ga_rqxg = ip_mk_default(L->ga_rqxcap, D3D11_BIND_VERTEX_BUFFER | (rq_xsrv_supported() ? D3D11_BIND_SHADER_RESOURCE : 0u));
        if (L->ga_rqxg && rq_xsrv_supported()) { L->ga_srv = rq_mksrv(L->ga_rqxg, L->ga_rqxcap); }
    }
    InterlockedExchange(&L->ga_st, 2);
    return 0;
}

static void ip_ga_adopt(ip_list_t *L)
{
    if (L->ga_vbg) {
        if (L->vbg) { ID3D11Buffer_Release(L->vbg); }
        L->vbg = L->ga_vbg; L->vbgcap = L->ga_vbcap;
        HLOG_I("INTERP vb_gpu: GPU-VB Liste %d %u Vertices (%u MB, Hilfs-Thread)", (int)(L - g_ipl), L->vbgcap,
               (unsigned)(((unsigned long long)L->vbgcap * sizeof(im_vertex_t)) >> 20));
    } else if (L->ga_vbcap) { st_vbg_fail++; }
    if (L->ga_rqxg && (L->ga_srv || !rq_xsrv_supported())) {
        if (L->rqxgsrv) { ID3D11ShaderResourceView_Release(L->rqxgsrv); }
        if (L->rqxg) { ID3D11Buffer_Release(L->rqxg); }
        L->rqxg = L->ga_rqxg; L->rqxgsrv = L->ga_srv; L->rqxgcap = L->ga_rqxcap;
    } else {
        if (L->ga_srv) { ID3D11ShaderResourceView_Release(L->ga_srv); }
        if (L->ga_rqxg) { ID3D11Buffer_Release(L->ga_rqxg); }
        if (L->ga_rqxcap) { st_vbg_fail++; }
    }
    L->ga_vbg = NULL; L->ga_rqxg = NULL; L->ga_srv = NULL;
    L->ga_vbcap = L->ga_rqxcap = 0;
    InterlockedExchange(&L->ga_st, 0);
}

static void ip_gpu_alloc(ip_list_t *L, int async)
{
    unsigned want = (unsigned)g_cfg.ip_vb_gpu;
    int needvb, needrq;
    if (!want || !g_dev) { return; }
    if (L->ga_st == 2) { ip_ga_adopt(L); }
    needvb = (want & 1u) && L->vb && (!L->vbg || L->vbgcap < L->vbcap);
    needrq = (want & 2u) && L->rqx && (!L->rqxg || L->rqxgcap < L->rqxcap);
    if (!needvb && !needrq) { return; }
    if (async) {
        if (L->ga_st != 0) { return; } /* laeuft schon */
        L->ga_vbcap = needvb ? L->vbcap : 0u;
        L->ga_rqxcap = needrq ? L->rqxcap : 0u;
        L->ga_vbg = NULL; L->ga_rqxg = NULL; L->ga_srv = NULL;
        InterlockedExchange(&L->ga_st, 1);
        if (!QueueUserWorkItem(ip_ga_work, L, WT_EXECUTELONGFUNCTION)) {
            L->ga_vbcap = L->ga_rqxcap = 0;
            InterlockedExchange(&L->ga_st, 0);
            st_vbg_fail++;
            return;
        }
        st_vbg_async++;
        return;
    }
    if (needvb) {
        if (L->vbg) { ID3D11Buffer_Release(L->vbg); L->vbg = NULL; L->vbgcap = 0; }
        L->vbg = ip_mk_default(L->vbcap * (unsigned)sizeof(im_vertex_t), D3D11_BIND_VERTEX_BUFFER);
        if (L->vbg) {
            L->vbgcap = L->vbcap;
            HLOG_I("INTERP vb_gpu: GPU-VB Liste %d %u Vertices (%u MB)", (int)(L - g_ipl), L->vbgcap,
                   (unsigned)(((unsigned long long)L->vbgcap * sizeof(im_vertex_t)) >> 20));
        } else { st_vbg_fail++; }
    }
    if (needrq) {
        if (L->rqxgsrv) { ID3D11ShaderResourceView_Release(L->rqxgsrv); L->rqxgsrv = NULL; }
        if (L->rqxg) { ID3D11Buffer_Release(L->rqxg); L->rqxg = NULL; L->rqxgcap = 0; }
        L->rqxg = ip_mk_default(L->rqxcap, D3D11_BIND_VERTEX_BUFFER |
                                               (rq_xsrv_supported() ? D3D11_BIND_SHADER_RESOURCE : 0u));
        if (L->rqxg) {
            L->rqxgcap = L->rqxcap;
            if (rq_xsrv_supported()) { L->rqxgsrv = rq_mksrv(L->rqxg, L->rqxgcap); }
        } else { st_vbg_fail++; }
    }
}

static void ip_gpu_copy(ip_list_t *L)
{
    D3D11_BOX bx;
    unsigned want = (unsigned)g_cfg.ip_vb_gpu;
    int gk = -1;
    L->gpu_ok = 0;
    if (!want || !g_ctx) { return; }
    ip_gpu_alloc(L, 1); /* fehlende/zu kleine Kopien im Hilfs-Thread anlegen (sonst schon in sb_interp_prealloc) */
    memset(&bx, 0, sizeof bx);
    bx.bottom = 1; bx.back = 1;
    if ((want & 1u) && L->vb && L->nverts) {
        if (L->vbg && L->vbgcap >= L->nverts) { /* alte Kopie reicht, solange der Tick hineinpasst */
            gk = gc_begin(); /* DIAG(PERF-M2,W174) */
            bx.left = 0; bx.right = L->nverts * (unsigned)sizeof(im_vertex_t);
            ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)L->vbg, 0, 0, 0, 0,
                                                      (ID3D11Resource *)L->vb, 0, &bx);
            L->gpu_ok |= 1u;
            st_vbg_copy++;
            st_vbg_mb += (double)bx.right / 1048576.0;
        }
    }
    if ((want & 2u) && L->rqx && L->rqxpos) {
        if (L->rqxg && L->rqxgcap >= L->rqxpos && (L->rqxgsrv || !L->rqxsrv)) {
            bx.left = 0; bx.right = L->rqxpos > L->rqxcap ? L->rqxcap : L->rqxpos;
            ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)L->rqxg, 0, 0, 0, 0,
                                                      (ID3D11Resource *)L->rqx, 0, &bx);
            L->gpu_ok |= 2u;
            st_vbg_mb += (double)bx.right / 1048576.0;
        }
    }
    gc_end(gk);
}

/* Puffer der Wiedergabe: GPU-Kopie, wenn vorhanden und gewuenscht. */
static ID3D11Buffer *ip_vbsel(const ip_list_t *L, int g)
{
    return (g && (L->gpu_ok & 1u)) ? L->vbg : L->vb;
}

/* PERF(PERF-WARM,W139) warm_vbpre=1: alle Tick-Puffersaetze (IP_LISTS Listen +
 * interp_bsets Reserve-Saetze, je VB 40-60 MB + Stream 2 64 MB + Konstanten
 * 4 MB) schon beim ersten Present mit Offscreen-BB (Bootbild, unsichtbar und
 * ungemessen) anlegen und je einmal per Map(DISCARD)/Unmap beruehren.
 * Befund W139_w7m1: Beim ersten Missionsstart legte die Aufnahme sechs Saetze
 * an; waehrend der Anlage (Device-Aufruf ohne ctx_lock) blockierte der Treiber
 * auch die Map-Aufrufe des Present-Takt-Threads (ip_cb_map 28-36 ms) und das
 * erste Map(DISCARD) je neuem Puffer dauerte 8-36 ms. Beim Retry existieren die
 * Puffer schon (dort gruen). Inhalt ohne Wirkung (jede Aufnahme schreibt ihren
 * Bereich vor dem Lesen); Reserve-Saetze erhalten ihren Fence wie in
 * ip_bset_rotate (gleich fertig -> NO_OVERWRITE wie ein freier Satz). */
static void ip_touch(ID3D11Buffer *b)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (b && SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)b, 0);
    }
}

void sb_interp_prealloc(void)
{
    static int done;
    static ip_list_t T;
    int i, k, n, nl = 0, ns = 0;
    LARGE_INTEGER f, q0, q1;
    if (done || g_cfg.warm_vbpre <= 0 || g_cfg.interp_fps <= 0 || !g_dev || !g_ctx) { return; }
    done = 1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q0);
    sb_ctx_enter();
    for (i = 0; i < IP_LISTS; i++) {
        ip_list_t *L = &g_ipl[i];
        if (!ip_vb_ensure(L) || !ip_rq_ensure(L)) { break; }
        ip_touch(L->vb); ip_touch(L->rqx); ip_touch(L->rqc);
        nl++;
        ip_gpu_alloc(L, 0); /* PERF(PERF-M2,W174) ip_vb_gpu: GPU-Kopien gleich mit anlegen */
    }
    n = ip_bsets_n(); /* W318: interp_bsets_imm */
    for (k = 0; k < n && nl == IP_LISTS; k++) {
        if (ip_bs[k].used) { continue; }
        memset(&T, 0, sizeof T);
        if (!ip_vb_ensure(&T) || !ip_rq_ensure(&T)) { break; }
        ip_touch(T.vb); ip_touch(T.rqx); ip_touch(T.rqc);
        ip_bs[k].vb = T.vb; ip_bs[k].vbcap = T.vbcap;
        ip_bs[k].rqx = T.rqx; ip_bs[k].rqxsrv = T.rqxsrv; ip_bs[k].rqxcap = T.rqxcap;
        ip_bs[k].rqc = T.rqc; ip_bs[k].rqccap = T.rqccap;
        if (!ip_bs[k].ev) {
            D3D11_QUERY_DESC qd;
            memset(&qd, 0, sizeof qd);
            qd.Query = D3D11_QUERY_EVENT;
            if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &ip_bs[k].ev))) { ip_bs[k].ev = NULL; }
        }
        if (ip_bs[k].ev) { ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)ip_bs[k].ev); }
        ip_bs[k].ser = ++ip_bs_ser;
        ip_bs[k].used = 1;
        ns++;
    }
    sb_ctx_leave();
    QueryPerformanceCounter(&q1);
    HLOG_I("INTERP: Tick-Puffer vorab angelegt (W139 warm_vbpre): %d Listen + %d Reserve-Saetze in %.1f ms", nl, ns,
           (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)f.QuadPart);
}

/* SHADE-PERF (W80): Platz im Tick-Puffer direkt beschreiben (ein Map, der
 * Aufrufer schreibt und ruft Unmap auf *buf). NULL = kein Platz. */
void *sb_interp_rq_xmap(unsigned bytes, ID3D11Buffer **buf, UINT *off, ID3D11ShaderResourceView **srv)
{
    ip_list_t *L;
    D3D11_MAPPED_SUBRESOURCE m;
    unsigned pos;
    if (ip_rec < 0 || !bytes) { return NULL; }
    L = &g_ipl[ip_rec];
    if (L->overflow || !L->rqx) { return NULL; }
    pos = L->rqxpos;
    if (pos + bytes > L->rqxcap) {
        L->overflow = 1;
        if (L->rqxwant < IP_RQX_MAX) { L->rqxwant += L->rqxwant / 2u; }
        return NULL;
    }
    if (FAILED(ip_map(L->rqx, (pos || (L->nodisc & 2u)) ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, &m))) {
        L->overflow = 1;
        return NULL;
    }
    *buf = L->rqx;
    *off = pos;
    *srv = L->rqxsrv;
    L->rqxpos = pos + ((bytes + 255u) & ~255u);
    return (unsigned char *)m.pData + pos;
}

int sb_interp_rq_xwrite(const void *data, unsigned bytes, ID3D11Buffer **buf, UINT *off)
{
    ip_list_t *L;
    D3D11_MAPPED_SUBRESOURCE m;
    unsigned pos;
    if (ip_rec < 0 || !bytes) { return 0; }
    L = &g_ipl[ip_rec];
    if (L->overflow || !L->rqx) { return 0; }
    pos = L->rqxpos;
    if (pos + bytes > L->rqxcap) {
        L->overflow = 1;
        if (L->rqxwant < IP_RQX_MAX) { L->rqxwant += L->rqxwant / 2u; }
        return 0;
    }
    if (FAILED(ip_map(L->rqx, (pos || (L->nodisc & 2u)) ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, &m))) {
        L->overflow = 1;
        return 0;
    }
    memcpy((unsigned char *)m.pData + pos, data, bytes);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)L->rqx, 0);
    *buf = L->rqx;
    *off = pos;
    L->rqxpos = pos + ((bytes + 255u) & ~255u);
    return 1;
}

/* SHADE-PERF (W80): aktueller Konstanten-Tick-Puffer + Schreibposition
 * (NULL = keine Aufnahme bzw. Ueberlauf). */
ID3D11Buffer *sb_interp_rq_cpos(UINT *pos)
{
    ip_list_t *L;
    if (ip_rec < 0) { return NULL; }
    L = &g_ipl[ip_rec];
    if (L->overflow || !L->rqc) { return NULL; }
    *pos = L->rqcpos;
    return L->rqc;
}

int sb_interp_rq_cwrite(const void *data, unsigned nconst, ID3D11Buffer **buf, UINT *first)
{
    ip_list_t *L;
    D3D11_MAPPED_SUBRESOURCE m;
    unsigned pos;
    if (ip_rec < 0 || !nconst) { return 0; }
    L = &g_ipl[ip_rec];
    if (L->overflow || !L->rqc) { return 0; }
    pos = L->rqcpos;
    if (pos + nconst > L->rqccap) {
        L->overflow = 1;
        if (L->rqcwant < IP_RQC_MAX) { L->rqcwant += L->rqcwant / 2u; }
        return 0;
    }
    if (FAILED(ip_map(L->rqc, (pos || (L->nodisc & 4u)) ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, &m))) {
        L->overflow = 1;
        return 0;
    }
    memcpy((unsigned char *)m.pData + (size_t)pos * 16u, data, (size_t)nconst * 16u);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)L->rqc, 0);
    *buf = L->rqc;
    *first = pos;
    L->rqcpos = pos + nconst;
    return 1;
}

/* Draw-Vertices in den Tick-VB der laufenden Aufnahme schreiben (statt in den
 * IM-Ring): der Spiel-Draw liest von dort, die Wiedergabe ebenso -> keine
 * Kopie. 1 = geschrieben (*vb/*start gesetzt), 0 = alter Ring-Pfad. */
/* PERF(IM-PERF,W99): Tick-VB-Bereich fuer n Vertices mappen (Aufrufer
 * schreibt direkt, z. B. die Primitiv-Expansion), danach sb_interp_vb_end.
 * NULL = alter Ring-Pfad (Ueberlauf wie in sb_interp_vb_write markiert). */
im_vertex_t *sb_interp_vb_begin(unsigned n, ID3D11Buffer **vb, unsigned *start)
{
    ip_list_t *L;
    D3D11_MAPPED_SUBRESOURCE m;
    if (ip_rec < 0 || !n) { return NULL; }
    L = &g_ipl[ip_rec];
    if (L->overflow || !L->vb) { return NULL; }
    if (L->nverts + n > L->vbcap) {
        L->overflow = 1;
        if (L->want < IP_VERTS_MAX) { L->want = L->want + L->want / 2u; }
        return NULL;
    }
    if (FAILED(ip_map(L->vb, (L->nverts || (L->nodisc & 1u)) ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, &m))) {
        L->overflow = 1;
        return NULL;
    }
    *vb = L->vb;
    *start = L->nverts;
    return (im_vertex_t *)m.pData + L->nverts;
}
void sb_interp_vb_end(unsigned n)
{
    ip_list_t *L = &g_ipl[ip_rec];
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)L->vb, 0);
    L->nverts += n;
}

int sb_interp_vb_write(const im_vertex_t *v, unsigned n, ID3D11Buffer **vb, unsigned *start)
{
    ip_list_t *L;
    D3D11_MAPPED_SUBRESOURCE m;
    if (ip_rec < 0 || !n) { return 0; }
    L = &g_ipl[ip_rec];
    if (L->overflow || !L->vb) { return 0; }
    if (L->nverts + n > L->vbcap) {
        L->overflow = 1;
        if (L->want < IP_VERTS_MAX) { L->want = L->want + L->want / 2u; }
        return 0;
    }
    if (FAILED(ip_map(L->vb, (L->nverts || (L->nodisc & 1u)) ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, &m))) {
        L->overflow = 1;
        return 0;
    }
    memcpy((im_vertex_t *)m.pData + L->nverts, v, (size_t)n * sizeof *v);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)L->vb, 0);
    if (g_cfg.interp_diag >= 3) {
        int li = ip_rec;
        unsigned k;
        if (ip_ccap[li] < L->vbcap) {
            free(ip_cpos[li]);
            ip_cpos[li] = (float (*)[4])malloc((size_t)L->vbcap * sizeof ip_cpos[li][0]);
            ip_ccap[li] = ip_cpos[li] ? L->vbcap : 0u;
        }
        if (ip_cpos[li]) {
            for (k = 0; k < n; k++) { memcpy(ip_cpos[li][L->nverts + k], v[k].pos, 16); }
        }
    }
    *vb = L->vb;
    *start = L->nverts;
    L->nverts += n;
    return 1;
}

/* Interp-VS (Stream 0 = Tick N, Stream 1 = Tick N-1), b0 wie IM-Pfad. */
static const char *kIP_VS =
    "cbuffer CB : register(b0) { row_major float4x4 mvp; uint has_tex; };\n"
    "cbuffer IP : register(b1) { row_major float4x4 mvp0; float ip_t; };\n"
    "struct VSIN { float4 pos : POSITION0; float4 col : COLOR0;\n"
    "              float2 uv : TEXCOORD0; float4 pos0 : POSITION1;\n"
    "#ifdef IP_HW\n" /* FIX(W237): echte Clip-w Tick N / N-1 (Strom 3/4) */
    "              float hw1 : IPW0; float hw0 : IPW1;\n"
    "#endif\n"
    "};\n"
    "struct VSOUT { float4 pos : SV_Position; float4 col : COLOR0;\n"
    "               float2 uv : TEXCOORD0;\n"
    "#ifdef IP_GUARD\n" /* FIX(W265 LINE-GUARD): Lagen fuer den Dreieck-Waechter (GS) */
    "               float4 g1 : IPGA; float4 g0 : IPGB; float2 gw : IPGW;\n"
    "#endif\n"
    "};\n"
    "VSOUT main(VSIN i) { VSOUT o;\n"
    "  float4 c0 = mul(i.pos0, mvp0), c1 = mul(i.pos, mvp);\n"
    "#ifdef IP_GUARD\n"
    "  o.g0 = c0; o.g1 = c1;\n" /* Strom-Lagen N-1/N (g1 = exakt die Tick-N-Ausgabe) */
    "#endif\n"
    "  float ew0 = c0.w, ew1 = c1.w;\n" /* w fuer die W224-Pruefung */
    "#ifdef IP_HW\n" /* FIX(W237 LINE-FLICKER2): P2 im RT (Strom w = 1): echte Clip-w aus Strom 3/4 */
    "  ew0 *= i.hw0; ew1 *= i.hw1;\n"
    "#ifdef IP_HWH\n" /* interp_hlerp=2: homogener Lerp im Clip-Raum */
    "  bool pre = (c0.w == 1.0 && c1.w == 1.0);\n" /* Strom schon durch w geteilt (wie das exakte Bild) */
    "  c0 *= i.hw0; c1 *= i.hw1;\n"
    "#endif\n"
    "#endif\n"
    "  o.pos = c0 + ip_t * (c1 - c0);\n" /* gleiche Komponenten bleiben bitgenau (W73) */
    "#ifdef IP_EYE\n" /* FIX(W224 LINE-FLICKER): w-Wechsel -> Tick N (kein Lauf durch w=0) */
    "  if (ew0 * ew1 <= 0.0) { o.pos = c1; }\n"
    "#ifdef IP_JUMP\n" /* FIX(W237): unplausibler Bildsprung vor der Kamera (Nahebene, Fehlpaarung) -> Tick N (nicht im W429-Modus 3) */
    "  else if (ew0 > 0.0 && ew1 > 0.0) {\n"
    "    float2 n0 = c0.xy / c0.w, n1 = c1.xy / c1.w;\n"
    "    if (any(abs(n1 - n0) > IP_JUMP * (1.0 + max(abs(n0), abs(n1))))) { o.pos = c1; } }\n"
    "#endif\n"
    "#endif\n"
    "#ifdef IP_HWH\n" /* W237: wieder durch w teilen -> gleiche Rasterung (w = 1, affine Attribute) wie das Tick-Bild */
    "  if (pre) { o.pos = float4(o.pos.xyz / o.pos.w, 1.0); }\n"
    "#endif\n"
    "#ifdef IP_GUARD\n"
    "  o.gw = float2(ew0, ew1);\n"
    "#endif\n"
    "  o.col = i.col; o.uv = i.uv; return o; }\n";

/* FIX(W265 LINE-GUARD): Dreieck-Waechter fuer gelerpte Dreiecke im Zwischenbild (Geometry Shader).
 * Der Lerp-VS (IM bzw. Shading, IP_GUARD/Flag 16) liefert je Vertex zusaetzlich die Strom-Lagen
 * g0 (Tick N-1) und g1 (Tick N, bitgleich zur exakten Ausgabe) sowie die (echten) w beider Ticks.
 * Je Dreieck: Regel 1 = ein Vertex wechselt zwischen N-1 und N das w-Vorzeichen; liegen alle
 * Vertices in beiden Ticks vor der Kamera, Regel 2 = Bildflaeche bei t > k_A * max(Flaeche N-1,
 * Flaeche N) + A_min, Regel 4 = Umklappen (N-1 und N gleiche Orientierung, t umgekehrt),
 * Regel 8 = eine Kante bei t > k_E * max(Laenge N-1, Laenge N) + L_min (NDC). Trifft eine Regel
 * aus GMASK, wird das GANZE Dreieck in Tick-N-Lage gezeichnet (nicht verworfen: keine Loecher,
 * Sichtbarkeit/Umlauf wie im Tick-Bild). Flaechenverhaeltnisse sind unabhaengig vom
 * Seitenverhaeltnis. GDIAG: Treffer der Regelmaske als zusaetzliches, entartetes Dreieck ausserhalb
 * des Bildes (Pipeline-Statistik GSPrimitives - GSInvocations = Treffer, Bild unveraendert). */
static const char *kIP_GS =
    "#ifdef GS_SHADE\n"
    "#define GSX float4 spec : COLOR1; float4 t0 : TEXCOORD1; float4 t1 : TEXCOORD2; float4 t2 : TEXCOORD3; float4 t3 : TEXCOORD4; float fog : TEXCOORD5;\n"
    "#define GSCP(o, v) o.spec = v.spec; o.t0 = v.t0; o.t1 = v.t1; o.t2 = v.t2; o.t3 = v.t3; o.fog = v.fog;\n"
    "#else\n"
    "#define GSX\n"
    "#define GSCP(o, v)\n"
    "#endif\n"
    "struct GSI { float4 pos : SV_Position; float4 col : COLOR0; float2 uv : TEXCOORD0; GSX\n"
    "             float4 g1 : IPGA; float4 g0 : IPGB; float2 gw : IPGW; };\n"
    "struct GSO { float4 pos : SV_Position; float4 col : COLOR0; float2 uv : TEXCOORD0; GSX };\n"
    "float gar(float2 a, float2 b, float2 c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }\n"
    "uint grules(GSI i[3]) {\n"
    "  uint r = 0u; int k;\n"
    "  [unroll] for (k = 0; k < 3; k++) { if (i[k].gw.x * i[k].gw.y <= 0.0) r |= 1u; }\n"
    "  float wm = min(min(min(i[0].g0.w, i[1].g0.w), i[2].g0.w), min(min(i[0].g1.w, i[1].g1.w), i[2].g1.w));\n"
    "  float wt = min(min(i[0].pos.w, i[1].pos.w), i[2].pos.w);\n"
    "  if (wm <= GW_EPS || wt <= GW_EPS) return r;\n"
    "  float2 p0[3], p1[3], pt[3];\n"
    "  [unroll] for (k = 0; k < 3; k++) { p0[k] = i[k].g0.xy / i[k].g0.w; p1[k] = i[k].g1.xy / i[k].g1.w;\n"
    "    pt[k] = i[k].pos.xy / i[k].pos.w; }\n"
    "  float a0 = gar(p0[0], p0[1], p0[2]), a1 = gar(p1[0], p1[1], p1[2]), at = gar(pt[0], pt[1], pt[2]);\n"
    "  if (abs(at) > GK_A * max(abs(a0), abs(a1)) + GA_MIN) r |= 2u;\n"
    "  if (a0 * a1 > 0.0 && at * a0 < 0.0 && abs(at) > GA_MIN) r |= 4u;\n"
    "  [unroll] for (k = 0; k < 3; k++) { int j = k == 2 ? 0 : k + 1;\n"
    "    float lt = length(pt[j] - pt[k]), lm = max(length(p0[j] - p0[k]), length(p1[j] - p1[k]));\n"
    "    if (lt > GK_E * lm + GL_MIN) r |= 8u; }\n"
    "  return r; }\n"
    "[maxvertexcount(GMAXV)]\n"
    "void main(triangle GSI i[3], inout TriangleStream<GSO> s) {\n"
    "  uint r = grules(i);\n"
    "  bool g = (r & GMASK) != 0u;\n"
    "  [unroll] for (int k = 0; k < 3; k++) { GSO o; o.pos = g ? i[k].g1 : i[k].pos; o.col = i[k].col; o.uv = i[k].uv;\n"
    "    GSCP(o, i[k]) s.Append(o); }\n"
    "#if GDIAG\n"
    "  if ((r & GDIAG) != 0u) { s.RestartStrip(); GSO z = (GSO)0; z.pos = float4(4.0, 4.0, 0.5, 1.0);\n"
    "    s.Append(z); s.Append(z); s.Append(z); }\n"
    "#endif\n"
    "}\n";
static ID3D11GeometryShader *ip_gd_im, *ip_gd_sh; /* W265: IM- bzw. Shading-Ausgabe */
/* W265 DIAG: Pipeline-Statistik je Zwischenbild-Wiedergabe (nur Immediate) */
#define IP_GD_Q 64
static ID3D11Query *ip_gd_q[IP_GD_Q];
static volatile int ip_gd_qbusy[IP_GD_Q]; /* 0 frei, sonst ip_gd_qi beim End (Verfall nach 4*IP_GD_Q Wiedergaben) */
static volatile unsigned ip_gd_qi;
static unsigned long long st_gd_tris, st_gd_hit;
static unsigned st_gd_frames, st_gd_fhit, st_gd_max, st_gd_lost;

static ID3D11GeometryShader *ip_gd_make(int shade)
{
    char src[6144];
    ID3DBlob *b = NULL;
    ID3D11GeometryShader *gs = NULL;
    int ge = g_cfg.interp_guard, dm = g_cfg.interp_guard_diag & 15;
    unsigned gmask = ((ge & 1) ? 14u : 0u) | ((ge & 2) ? 1u : 0u);
    _snprintf_s(src, sizeof src, _TRUNCATE,
                "%s#define GW_EPS 1e-5\n#define GA_MIN 0.002\n#define GL_MIN 0.03\n#define GK_A %d.0\n#define GK_E %.2f\n"
                "#define GMASK %uu\n#define GDIAG %du\n#define GMAXV %d\n%s",
                shade ? "#define GS_SHADE 1\n" : "", g_cfg.interp_guard_ka > 0 ? g_cfg.interp_guard_ka : 4,
                (g_cfg.interp_guard_ke > 0 ? g_cfg.interp_guard_ke : 20) / 10.0, gmask, dm, dm ? 6 : 3, kIP_GS);
    if (!sb_im_compile(src, "gs_4_0", &b)) { HLOG_W("INTERP guard: GS (%s) nicht uebersetzbar", shade ? "Shading" : "IM"); return NULL; }
    if (FAILED(ID3D11Device_CreateGeometryShader(g_dev, b->lpVtbl->GetBufferPointer(b), b->lpVtbl->GetBufferSize(b), NULL, &gs))) { gs = NULL; }
    b->lpVtbl->Release(b);
    return gs;
}

/* W265: GS nur fuer gelerpte Dreiecks-Draws (Liste/Strip) binden; *cur = gebundener GS (Cache). */
static void ip_gd_bind(ID3D11DeviceContext *c, ID3D11GeometryShader **cur, ID3D11GeometryShader *want)
{
    if (*cur != want) { ID3D11DeviceContext_GSSetShader(c, want, NULL, 0); *cur = want; }
}

static void ip_gd_poll(void)
{
    unsigned k;
    for (k = 0; k < IP_GD_Q; k++) {
        D3D11_QUERY_DATA_PIPELINE_STATISTICS ps;
        if (!ip_gd_qbusy[k]) { continue; }
        if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)ip_gd_q[k], &ps, sizeof ps,
                                        D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
            if (ip_gd_qi - (unsigned)ip_gd_qbusy[k] > 4u * IP_GD_Q) { ip_gd_qbusy[k] = 0; st_gd_lost++; } /* nie ausgefuehrt */
            continue;
        }
        ip_gd_qbusy[k] = 0;
        {
            unsigned long long hit = ps.GSPrimitives > ps.GSInvocations ? ps.GSPrimitives - ps.GSInvocations : 0ull;
            st_gd_tris += ps.GSInvocations;
            st_gd_hit += hit;
            st_gd_frames++;
            if (hit) { st_gd_fhit++; }
            if (hit > st_gd_max) { st_gd_max = (unsigned)hit; }
        }
    }
}

static int ip_init(void)
{
    ID3DBlob *vsb = NULL;
    D3D11_INPUT_ELEMENT_DESC el[6];
    D3D11_BUFFER_DESC bd;
    HRESULT hr;
    int i;
    if (ip_init_done) { return !ip_init_fail; }
    QueryPerformanceFrequency(&ip_qpf);
    if (!g_dev || !g_im_ready) { return 0; }
    ip_init_done = 1;
    ip_init_fail = 1;
    for (i = 0; i < IP_LISTS; i++) { g_ipl[i].bbref = -1; }
    {
        /* FIX(W224 LINE-FLICKER): interp_eyefix=1 -> Variante mit IP_EYE */
        char ipsrc[4096];
        const int hl = sb_ip_hlerp(); /* W429: Modus 3 = homogen ohne Bildsprung-Regel */
        _snprintf_s(ipsrc, sizeof ipsrc, _TRUNCATE, "%s%s%s%s%s",
                    (g_cfg.interp_guard || g_cfg.interp_guard_diag) ? "#define IP_GUARD 1\n" : "", /* W265 */
                    g_cfg.interp_eyefix ? "#define IP_EYE 1\n" : "",
                    hl == 3 ? "#define IP_HW 1\n" : (hl ? "#define IP_HW 1\n#define IP_JUMP 0.5\n" : ""),
                    hl >= 2 ? "#define IP_HWH 1\n" : "", kIP_VS); /* W237 */
        if (!sb_im_compile(ipsrc, "vs_4_0", &vsb)) { return 0; }
    }
    hr = ID3D11Device_CreateVertexShader(g_dev, vsb->lpVtbl->GetBufferPointer(vsb),
                                         vsb->lpVtbl->GetBufferSize(vsb), NULL, &ip_vs2);
    if (FAILED(hr)) { vsb->lpVtbl->Release(vsb); return 0; }
    memset(el, 0, sizeof el);
    el[0].SemanticName = "POSITION"; el[0].SemanticIndex = 0;
    el[0].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; el[0].InputSlot = 0; el[0].AlignedByteOffset = 0;
    el[1].SemanticName = "COLOR"; el[1].SemanticIndex = 0;
    el[1].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; el[1].InputSlot = 0; el[1].AlignedByteOffset = 16;
    el[2].SemanticName = "TEXCOORD"; el[2].SemanticIndex = 0;
    el[2].Format = DXGI_FORMAT_R32G32_FLOAT; el[2].InputSlot = 0; el[2].AlignedByteOffset = 32;
    el[3].SemanticName = "POSITION"; el[3].SemanticIndex = 1;
    el[3].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; el[3].InputSlot = 1; el[3].AlignedByteOffset = 0;
    el[4].SemanticName = "IPW"; el[4].SemanticIndex = 0; /* FIX(W237): Clip-w Tick N (Strom 3) */
    el[4].Format = DXGI_FORMAT_R32_FLOAT; el[4].InputSlot = 3; el[4].AlignedByteOffset = 0;
    el[5].SemanticName = "IPW"; el[5].SemanticIndex = 1; /* Tick N-1 (Strom 4) */
    el[5].Format = DXGI_FORMAT_R32_FLOAT; el[5].InputSlot = 4; el[5].AlignedByteOffset = 0;
    hr = ID3D11Device_CreateInputLayout(g_dev, el, sb_ip_hlerp() ? 6 : 4, vsb->lpVtbl->GetBufferPointer(vsb),
                                        vsb->lpVtbl->GetBufferSize(vsb), &ip_lay2);
    vsb->lpVtbl->Release(vsb);
    if (FAILED(hr)) { return 0; }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 80;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &ip_cb0))) { return 0; }
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &ip_cb1))) { return 0; }
    if (sb_ip_hlerp()) { /* FIX(W237): Ersatz-Strom w = 1 (Stride 0) fuer Ticks ohne w-Puffer */
        static const float ones[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        D3D11_SUBRESOURCE_DATA sd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        memset(&sd, 0, sizeof sd);
        sd.pSysMem = ones;
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, &sd, &ip_hw_one))) { return 0; }
        bd.ByteWidth = 80;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    }
    if (g_cfg.interp_diag >= 3 || sb_rp_want()) { /* W295: Draw-ID-Shader auch fuer interp_mode=reproj */
        static const char *kIP_DBG_PS =
            "cbuffer CB : register(b0) { row_major float4x4 mvp; uint has_tex; };\n"
            "cbuffer D : register(b1) { float4 dcol; };\n"
            "Texture2D tex : register(t0);\n"
            "SamplerState samp : register(s0);\n"
            "struct PSIN { float4 pos : SV_Position; float4 col : COLOR0;\n"
            "              float2 uv : TEXCOORD0; };\n"
            "float4 main(PSIN i) : SV_Target {\n"
            "  float a = i.col.a; if ((has_tex & 1u) != 0) { a *= tex.Sample(samp, i.uv).a; }\n"
            "  return float4(dcol.rgb, a); }\n";
        ID3DBlob *psb = NULL;
        if (sb_im_compile(kIP_DBG_PS, "ps_4_0", &psb)) {
            ID3D11Device_CreatePixelShader(g_dev, psb->lpVtbl->GetBufferPointer(psb),
                                           psb->lpVtbl->GetBufferSize(psb), NULL, &ip_dbg_ps);
            psb->lpVtbl->Release(psb);
        }
        bd.ByteWidth = 16;
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &ip_dbg_cb))) { ip_dbg_cb = NULL; }
        if (ip_dd_on() || sb_rp_want()) { /* DIAG(W251): passt zu jedem VS (liest nur SV_Position) */
            static const char *kIP_ID_PS =
                "cbuffer D : register(b1) { float4 dcol; };\n"
                "float4 main(float4 p : SV_Position) : SV_Target { return float4(dcol.rgb, 1.0); }\n";
            if (sb_im_compile(kIP_ID_PS, "ps_4_0", &psb)) {
                ID3D11Device_CreatePixelShader(g_dev, psb->lpVtbl->GetBufferPointer(psb),
                                               psb->lpVtbl->GetBufferSize(psb), NULL, &ip_id_ps);
                psb->lpVtbl->Release(psb);
            }
        }
    }
    if (g_cfg.interp_guard || g_cfg.interp_guard_diag) { /* FIX(W265 LINE-GUARD) */
        ip_gd_im = ip_gd_make(0);
        ip_gd_sh = ip_gd_make(1);
        if (g_cfg.interp_guard_diag) {
            D3D11_QUERY_DESC qd;
            int k;
            memset(&qd, 0, sizeof qd);
            qd.Query = D3D11_QUERY_PIPELINE_STATISTICS;
            for (k = 0; k < IP_GD_Q; k++) {
                if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &ip_gd_q[k]))) { ip_gd_q[k] = NULL; }
            }
        }
        HLOG_I("INTERP guard: modus=%d k_A=%d k_E=%d diag=%d GS IM=%s Shading=%s", g_cfg.interp_guard,
               g_cfg.interp_guard_ka, g_cfg.interp_guard_ke, g_cfg.interp_guard_diag,
               ip_gd_im ? "ok" : "FEHLT", ip_gd_sh ? "ok" : "FEHLT");
    }
    ip_init_fail = 0;
    st_t0 = ip_now();
    HLOG_I("INTERP: bereit (interp_fps=%d, K=%d, vblank_hz=%d, vblank_qpc=%d, Takt=%s)",
           g_cfg.interp_fps, g_cfg.interp_fps / ip_tick_hz() < 1 ? 1 : g_cfg.interp_fps / ip_tick_hz(),
           g_cfg.vblank_hz, g_cfg.vblank_qpc,
           g_cfg.vblank_hz > 0 ? "Echtzeit" : "Testtakt (K Bilder am Swap)");
    return 1;
}

/* ---------- Aufzeichnung ---------- */

static void ip_snap(ip_op_t *o)
{
    UINT n;
    ID3D11DeviceContext_OMGetRenderTargets(g_ctx, 1, &o->rtv, &o->dsv);
    ID3D11DeviceContext_OMGetBlendState(g_ctx, &o->bs, o->bf, &o->smask);
    ID3D11DeviceContext_OMGetDepthStencilState(g_ctx, &o->ds, &o->sref);
    ID3D11DeviceContext_RSGetState(g_ctx, &o->rs);
    n = 1;
    ID3D11DeviceContext_RSGetViewports(g_ctx, &n, &o->vp);
    n = 1;
    ID3D11DeviceContext_RSGetScissorRects(g_ctx, &n, &o->sc);
    ID3D11DeviceContext_PSGetShaderResources(g_ctx, 0, 1, &o->srv);
}

/* PERF(IM-PERF,W99): Quell-Vertex des expandierten Vertex k (Spiegel der
 * Expansion in sb_im_submit: STRIP/FAN/POLYGON/QUADS/QUADSTRIP/LINELOOP). */
unsigned sb_im_exp_src(unsigned prim, unsigned n, unsigned k)
{
    static const unsigned char q6[6] = { 0, 1, 2, 0, 2, 3 };
    switch (prim) {
    case 3: return (k < n) ? k : 0u;
    case 6: return k / 3u + k % 3u;
    case 7: case 10: return (k % 3u) ? k / 3u + k % 3u : 0u;
    case 8: return (k / 6u) * 4u + q6[k % 6u];
    case 9: return (k / 6u) * 2u + q6[k % 6u];
    default: return k;
    }
}

static void ip_rec_draw_core(const float pref[4][4], unsigned outhash, unsigned n, unsigned topo,
                             const float mvp[16], unsigned has_tex, int cls,
                             unsigned sig, unsigned rawhash, unsigned vstart, LONGLONG q0,
                             unsigned ib1, unsigned nsv);
static float s_prefw[4]; /* FIX(W429): echte w der Bezugs-Vertices des naechsten Draws (P2 im RT) */
static int s_prefw_ok;

/* FIX(W429 M3-ROAD-FIX): wirksamer interp_hlerp-Modus. interp_hlerp gesetzt -> dieser (1/2 wie W237); sonst mit
 * interp_p2hom (nur SB) Modus 3 = homogener Lerp der P2-Draws im RT mit echter Clip-w, W224-Regel mit echter w,
 * OHNE die W237-Bildsprung-Regel - also dieselbe Regel je Vertex wie beim 3D-Pass derselben Geometrie. */
int sb_ip_hlerp(void)
{
    if (g_cfg.interp_hlerp) { return g_cfg.interp_hlerp; }
    return (g_cfg.interp_p2hom && !g_sba_loc) ? 3 : 0;
}

void sb_interp_rec_draw(const im_vertex_t *v, unsigned n, unsigned topo,
                        const float mvp[16], unsigned has_tex, int cls,
                        unsigned sig, unsigned rawhash, unsigned vstart)
{
    ip_list_t *L;
    LONGLONG q0;
    float pref[4][4];
    unsigned q;
    if (ip_live) { ip_in_rec = 1; ip_poll(); ip_in_rec = 0; } /* Interp-Takt: faelliges Bild (Schatten-Ziele); W122: in cap */
    if (ip_rec < 0 || !n) { return; }
    L = &g_ipl[ip_rec];
    if (L->overflow) { return; }
    q0 = ip_now();
    if (vstart == 0xFFFFFFFFu) { /* Aufrufer nutzt den IM-Ring (Balken): hier kopieren */
        ID3D11Buffer *vb;
        if (!sb_interp_vb_write(v, n, &vb, &vstart)) { L->overflow = 1; return; }
    }
    for (q = 0; q < 4u; q++) { memcpy(pref[q], v[(n - 1u) * q / 3u].pos, sizeof pref[q]); }
    {   /* W237: echte Clip-w (P2 im RT) fuer interp_hlerp / Diagnose */
        const float *tw = g_ip_tw;
        unsigned twn = g_ip_tw_n, twp = g_ip_tw_prim;
        g_ip_tw = NULL;
        if (sb_ip_hlerp()) { ip_w_rec(L, vstart, n, tw, twn, twp); }
        if (tw && sb_ip_hlerp()) { /* FIX(W429): echte w der Bezugs-Vertices (Abbildung wie ip_w_rec) */
            for (q = 0; q < 4u; q++) {
                unsigned k = (n - 1u) * q / 3u;
                unsigned s = (twp != 3u && twp != 6u && twp != 7u && twp != 8u && twp != 9u && twp != 10u && n <= twn) ? k : sb_im_exp_src(twp, twn, k);
                s_prefw[q] = s < twn ? tw[s] : 1.0f;
            }
            s_prefw_ok = 1;
        }
        if (g_cfg.interp_eyediag > 0) { ip_cw_rec(ip_rec, L->vbcap, vstart, n, mvp, v, NULL, 0, 0, tw, twn, twp); } /* DIAG(W224) */
    }
    ip_rec_draw_core((const float (*)[4])pref, sb_interp_hash(v, sizeof *v, n), n, topo, mvp,
                     has_tex, cls, sig, rawhash, vstart, q0, 0u, n);
}

/* Save originals before the active camera modifies them. The next VB upload
 * starts at L->nverts. Expansion matches render_im.c (including its cap).
 * No call/copy/allocation at all in the camera-0 draw path. On failure the
 * tick is invalidated, so replay never mixes old P2 with a new mesh camera. */
void sb_interp_rec_cpersp(const float camera[3], const float *pos, size_t st, unsigned prim, unsigned nsrc)
{
    ip_list_t *L;
    ip_cp_t *p;
    unsigned i, n;
    if (ip_rec < 0 || !nsrc) { return; }
    L = &g_ipl[ip_rec];
    if (L->overflow) { return; }
    memcpy(L->cp_camera, camera, sizeof L->cp_camera);
    if (!pos) { return; }
    if (nsrc > IM_VB_MAX) { nsrc = IM_VB_MAX; }
    n = nsrc;
    switch (prim) {
    case 3: if (n < IM_VB_MAX) { n++; } break;
    case 6: case 7: case 10: n = nsrc > 2u ? (nsrc - 2u) * 3u : 0u; break;
    case 8: n = (nsrc / 4u) * 6u; break;
    case 9: n = nsrc >= 4u ? ((nsrc - 4u) / 2u + 1u) * 6u : 0u; break;
    default: break;
    }
    if (n > IM_VB_MAX) { n = IM_VB_MAX; }
    if (!n) { return; }
    p = (ip_cp_t *)malloc(sizeof *p + (size_t)(n - 1u) * sizeof p->pos[0]);
    if (!p) { L->overflow = 1; return; }
    p->start = L->nverts; p->n = n;
    for (i = 0; i < n; i++) { memcpy(p->pos[i], pos + st * sb_im_exp_src(prim, nsrc, i), sizeof p->pos[i]); }
    p->next = L->cp_original;
    L->cp_original = p;
}

/* PERF(IM-PERF,W99): wie sb_interp_rec_draw, die expandierten Vertices liegen
 * aber nur im Tick-VB (direkt geschrieben): Stichproben (pref, Hash) ueber die
 * Quell-Vertices src[sb_im_exp_src(k)] - bitgleich zu den expandierten. */
void sb_interp_rec_draw_src(const im_vertex_t *src, unsigned prim, unsigned nsrc, unsigned n,
                            unsigned topo, const float mvp[16], unsigned has_tex, int cls,
                            unsigned sig, unsigned rawhash, unsigned vstart)
{
    ip_list_t *L;
    LONGLONG q0;
    float pref[4][4];
    unsigned q, h = 2166136261u, i, step;
    const unsigned ib1 = g_ip_ib1, nsv = ib1 ? nsrc : n; /* W264 DRAW-OPT2: indiziert -> nsrc Quell-Vertices im Tick-VB */
    g_ip_ib1 = 0;
    if (ip_live) { ip_in_rec = 1; ip_poll(); ip_in_rec = 0; } /* W122: in cap */
    if (ip_rec < 0 || !n) { return; }
    L = &g_ipl[ip_rec];
    if (L->overflow) { return; }
    q0 = ip_now();
    if (vstart == 0xFFFFFFFFu) { L->overflow = 1; return; } /* = Fehlschlag von vb_write */
    for (q = 0; q < 4u; q++) {
        memcpy(pref[q], src[sb_im_exp_src(prim, nsrc, (n - 1u) * q / 3u)].pos, sizeof pref[q]);
    }
    {   /* W237: echte Clip-w (P2 im RT) fuer interp_hlerp / Diagnose */
        const float *tw = g_ip_tw;
        unsigned twn = g_ip_tw_n, twp = g_ip_tw_prim;
        g_ip_tw = NULL;
        if (sb_ip_hlerp()) { ip_w_rec(L, vstart, nsv, tw, twn, ib1 ? 5u : twp); } /* W264: indiziert w je Quell-Vertex */
        if (tw && sb_ip_hlerp()) { /* FIX(W429): echte w der Bezugs-Vertices (tw je Quell-Vertex) */
            for (q = 0; q < 4u; q++) {
                unsigned s = sb_im_exp_src(prim, nsrc, (n - 1u) * q / 3u);
                s_prefw[q] = s < twn ? tw[s] : 1.0f;
            }
            s_prefw_ok = 1;
        }
        if (g_cfg.interp_eyediag > 0) { ip_cw_rec(ip_rec, L->vbcap, vstart, n, mvp, NULL, src, prim, nsrc, tw, twn, twp); } /* DIAG(W224) */
    }
    step = n > 64u ? n / 64u : 1u; /* Stichprobe exakt wie sb_interp_hash */
    for (i = 0; i < n; i += step) {
        const unsigned *w = (const unsigned *)src[sb_im_exp_src(prim, nsrc, i)].pos;
        h = (h ^ w[0]) * 16777619u;
        h = (h ^ w[1]) * 16777619u;
        h = (h ^ w[2]) * 16777619u;
        h = (h ^ w[3]) * 16777619u;
    }
    ip_rec_draw_core((const float (*)[4])pref, h ^ n, n, topo, mvp, has_tex, cls, sig, rawhash,
                     vstart, q0, ib1, nsv);
}

static void ip_rec_draw_core(const float pref[4][4], unsigned outhash, unsigned n, unsigned topo,
                             const float mvp[16], unsigned has_tex, int cls,
                             unsigned sig, unsigned rawhash, unsigned vstart, LONGLONG q0,
                             unsigned ib1, unsigned nsv)
{
    ip_list_t *L = &g_ipl[ip_rec];
    ip_op_t *o;
    const int pw = s_prefw_ok; /* W429 */
    s_prefw_ok = 0;
    o = ip_new_op(L);
    if (!o) { return; }
    o->kind = IP_DRAW;
    if (pw) { memcpy(o->prefw, s_prefw, sizeof o->prefw); o->p2w = 1; } /* W429 */
    o->cls = (unsigned char)(cls ? 1 : 0);
    o->topo = topo;
    memcpy(o->mvp, mvp, sizeof o->mvp);
    o->has_tex = has_tex;
    o->vstart = vstart;
    o->nv = n;
    o->ib1 = ib1; o->nsv = nsv; /* W264 DRAW-OPT2 */
    o->lgs = g_line_gs_rec; /* FIX(ISSUE-EMP,W352) */
    {
        unsigned q;
        for (q = 0; q < 4u; q++) {
            float c[4];
            memcpy(o->pref[q], pref[q], sizeof o->pref[q]);
            ip_clip(o->pref[q], mvp, c); /* POPIN (W91): NDC fuer ip_cost */
            o->rn[q][2] = c[3];
            o->rn[q][0] = c[3] > 1e-6f ? c[0] / c[3] : 0.0f;
            o->rn[q][1] = c[3] > 1e-6f ? c[1] / c[3] : 0.0f;
        }
    }
    o->outhash = outhash;
    o->rawhash = rawhash;
    o->sig = sig;
    o->pvs = g_ip_sigparts[0];
    o->pxp = g_ip_sigparts[1];
    o->pps = g_ip_sigparts[2];
    g_ip_sigparts[0] = g_ip_sigparts[1] = g_ip_sigparts[2] = 0;
    o->ta_smp = g_ta_smp; /* FIX(TEXADDR,W84) */
    o->nolerp = g_mfx_kind; /* MONITOR-FX (W105); W193: Art (MFX_WASH/MFX_LINES) fuer die Pause-Neuzeichnung */
    o->fxk = g_fxr_reck; /* W133 REM-FX */
    if (has_tex & FXR_MASK) { memcpy(o->fxp, g_fxr_recp, sizeof o->fxp); }
    g_fxr_reck = 0;
    if (g_rq_active) { /* RENDER-QA (W77): Shading-Zustand mitschreiben */
        int k;
        o->rq = 1;
        o->rqr = g_rq_rec;
        for (k = 1; k < 4; k++) { if (o->rqr.srv[k]) { ID3D11ShaderResourceView_AddRef(o->rqr.srv[k]); } }
    }
    if (rtt_active()) { /* W195 PHOTO-MODE: Spiel-Viewport/RT-Groesse fuer die freie Kamera der Foto-Wiedergabe */
        unsigned rw = 0, rh = 0;
        rtt_cur_dims(&rw, &rh);
        o->vpg[0] = (unsigned short)g_vp_game.x; o->vpg[1] = (unsigned short)g_vp_game.y;
        o->vpg[2] = (unsigned short)g_vp_game.w; o->vpg[3] = (unsigned short)g_vp_game.h;
        o->rtl[0] = (unsigned short)rw; o->rtl[1] = (unsigned short)rh;
    }
    o->prj[0] = g_proj_game.m[0]; o->prj[1] = g_proj_game.m[5]; /* W195 */
    o->prj[2] = g_proj_game.m[10]; o->prj[3] = g_proj_game.m[14];
    ip_snap(o);
    o->sig = (o->sig ^ (unsigned)(uintptr_t)o->rtv) * 16777619u;
    o->sig0 = (o->sig ^ topo ^ (has_tex << 8) ^ ((unsigned)o->cls << 9)) * 16777619u;
    o->sig = (o->sig ^ n) * 16777619u;
    o->gkey = ((((rawhash * 16777619u) ^ (unsigned)(uintptr_t)o->rtv) * 16777619u ^ n) * 16777619u) ^
              ((unsigned)o->cls << 31);
    o->sig = (o->sig ^ topo ^ (has_tex << 8) ^ ((unsigned)o->cls << 9)) * 16777619u;
    st_cap_q += ip_now() - q0;
}

void sb_interp_rec_clear(ID3D11RenderTargetView *rtv, const float c[4],
                         ID3D11DepthStencilView *dsv, unsigned dsflags,
                         float z, unsigned stencil)
{
    ip_list_t *L;
    ip_op_t *o;
    if (ip_live) { ip_in_rec = 1; ip_poll(); ip_in_rec = 0; } /* W122: in cap */
    if (ip_rec < 0) { return; }
    L = &g_ipl[ip_rec];
    if (L->overflow) { return; }
    o = ip_new_op(L);
    if (!o) { return; }
    if (rtv) {
        o->kind = IP_CLR_RTV;
        o->rtv = rtv;
        ID3D11RenderTargetView_AddRef(rtv);
        memcpy(o->col, c, sizeof o->col);
    } else if (dsv) {
        o->kind = IP_CLR_DSV;
        o->dsv = dsv;
        ID3D11DepthStencilView_AddRef(dsv);
        o->dsflags = dsflags;
        o->z = z;
        o->stencil = stencil;
    } else {
        L->nops--;
    }
}

/* W340 LOC-FRAMES: LoC zeichnet die Welt in den Backbuffer, kopiert den 4:3-Ausschnitt in eine Textur
 * (loc_render.c bbc_copy, "Backbuffer als Textur") und zeichnet daraus Glow-Verkleinerung und das
 * Kompositions-Quad zurueck in den Backbuffer. Die Kopie war keine Op: Zwischenbilder lerpten die Welt in den
 * BB-Schatten, das Kompositions-Quad sampelte aber die Spiel-Kopie (Stand Tick N) und deckte die gelerpte Welt
 * zu -> Welt nur 20 Bilder/s, Cockpit 120. Jetzt Op: Wiedergabe kopiert aus dem BB-Schatten in den Schatten der
 * Textur, die sampelnden Draws bekommen dessen SRV (ip_list_snap). */
void sb_interp_rec_copy(ID3D11RenderTargetView *src, ID3D11ShaderResourceView *dst, const D3D11_BOX *box)
{
    ip_list_t *L;
    ip_op_t *o;
    if (!src || !dst || ip_rec < 0) { return; }
    InterlockedIncrement(&li_cp_seen); /* W347 DIAG: Kopien waehrend der Aufzeichnung (auch mit Schalter aus) */
    /* FIX(W347 GHOST): SB-Selbstkopien (W106-Ring) ebenso, eigener Schalter */
    if (!(g_sba_loc ? g_cfg.loc_interp_copy : g_cfg.interp_selfcopy)) { return; }
    L = &g_ipl[ip_rec];
    if (L->overflow) { return; }
    o = ip_new_op(L);
    if (!o) { return; }
    o->kind = IP_COPY;
    o->cps = src;
    ID3D11RenderTargetView_AddRef(src);
    o->cpd = dst;
    ID3D11ShaderResourceView_AddRef(dst);
    if (box) { o->cpbox = *box; } else { o->cpbox.back = 0; } /* back = 0: ganze Ressource (CopyResource) */
    o->cp_es = o->cp_ed = -1;
    L->ncopy++;
    InterlockedIncrement(&li_cp_rec);
}

/* ---------- Zuordnung Tick N-1 -> N ---------- */

static void ip_clip(const float p[4], const float m[16], float c[4])
{
    int j;
    for (j = 0; j < 4; j++) {
        c[j] = p[0] * m[0 * 4 + j] + p[1] * m[1 * 4 + j] + p[2] * m[2 * 4 + j] + p[3] * m[3 * 4 + j];
    }
}

/* Plausibilitaet einer Zuordnung a (Tick N-1) -> o (Tick N): jeder der 4
 * Bezugs-Vertices, der in beiden Ticks vor der Kamera liegt (w > 0), darf
 * hoechstens interp_maxjump (NDC) springen; ein Vorzeichenwechsel von w ist
 * unplausibel; mind. ein Bezug muss vor der Kamera liegen, ausser alle
 * liegen in beiden Ticks dahinter (unsichtbar). 1 = lerpen. */
static int ip_plausible(const ip_op_t *a, const ip_op_t *o, float maxjump)
{
    int q, front = 0, behind = 0;
    for (q = 0; q < 4; q++) {
        float c0[4], c1[4];
        ip_clip(a->pref[q], a->mvp, c0);
        ip_clip(o->pref[q], o->mvp, c1);
        if (c0[3] > 1e-6f && c1[3] > 1e-6f) {
            float dx = c1[0] / c1[3] - c0[0] / c0[3], dy = c1[1] / c1[3] - c0[1] / c0[3];
            if (dx < 0) { dx = -dx; }
            if (dy < 0) { dy = -dy; }
            if (dx > maxjump || dy > maxjump) { return 0; }
            front++;
        } else if (c0[3] < -1e-6f && c1[3] < -1e-6f) {
            behind++;
        } else {
            return 0; /* w-Wechsel oder w ~ 0 */
        }
    }
    return front > 0 || behind == 4;
}

/* FIX(W448 LOC-CHAFF-LOOK, GitHub #83) loc_interp_eyeblind: Matrix-Lerp eines Netzes (gleiche Eingangsdaten, > 8
 * Vertices), dessen 4 Bezugs-Vertices in beiden Ticks hinter der Kamera liegen, nicht zulassen -> exakt aus Tick N
 * (Regel und Begruendung in render_interp_eye.h; Chaff-Huelsen an der Augenebene). Nur LoC. 1 = exakt. */
static unsigned st_w448_eye;
static int w448_eye_blind(const ip_op_t *a, const ip_op_t *o)
{
    if (!g_sba_loc || !g_cfg.loc_interp_eyeblind || g_cfg.vr_enable) { return 0; }
    return ip_eye_blind((const float (*)[4])a->pref, a->mvp, a->nv, (const float (*)[4])o->pref, o->mvp, o->nv);
}

/* OPTION(W406 M3-ROAD-JITTER) interp_p2rej: vorab transformierte Welt-Draws (P2 im Renderziel, Strom schon
 * durch w geteilt, affine Bildraum-Matrix) mit GLEICHEN Eingangsdaten scheitern an ip_plausible, sobald EIN
 * Bezugs-Vertex nahe der Augenebene liegt: dessen Pixellage ist riesig (|ndc| >> 1) und springt bei kleinster
 * Kamerabewegung weit. Der ganze Draw (z. B. ein Gelaendestueck unter dem VT) steht dann im Zwischenbild in
 * Tick-N-Lage, die gelerpten Nachbarn um (1-t)*Sprung daneben -> Versatz/Wackeln und Spalt (SEAMRJ, W406).
 * Hier zaehlen nur Bezugs-Vertices, die in beiden Ticks im erweiterten Bild (|ndc| <= 2) liegen; mindestens
 * einer muss es sein, und alle diese springen hoechstens maxjump. 1 = lerpen. */
static int ip_mvp_affine(const float m[16]);
static int ip_p2rej_ok(const ip_op_t *a, const ip_op_t *o, float maxjump)
{
    int q, nin = 0;
    if (!ip_mvp_affine(o->mvp) || !ip_mvp_affine(a->mvp) || a->rawhash != o->rawhash || a->nv != o->nv) { return 0; }
    for (q = 0; q < 4; q++) {
        float c0[4], c1[4], x0, y0, x1, y1;
        ip_clip(a->pref[q], a->mvp, c0);
        ip_clip(o->pref[q], o->mvp, c1);
        if (c0[3] <= 1e-6f || c1[3] <= 1e-6f) { return 0; }
        x0 = c0[0] / c0[3]; y0 = c0[1] / c0[3]; x1 = c1[0] / c1[3]; y1 = c1[1] / c1[3];
        if (fabsf(x0) > 2.0f || fabsf(y0) > 2.0f || fabsf(x1) > 2.0f || fabsf(y1) > 2.0f) { continue; }
        if (fabsf(x1 - x0) > maxjump || fabsf(y1 - y0) > maxjump) { return 0; }
        nin++;
    }
    return nin > 0;
}

/* FIX(W429 M3-ROAD-FIX) interp_p2hom: Welt-Draw mit GLEICHEN Eingangsdaten (gleicher Eingangs-Hash und Vertexzahl:
 * Vertex i <-> i ist derselbe Punkt), der an ip_plausible scheitert. Das Bodenstueck vor dem VT zeichnet das Spiel
 * zweimal (P2 vorab transformiert und 3D mit Matrix, W406); ein einziger Bezugs-Vertex an der Augenebene bzw.
 * hinter der Kamera (riesige bzw. gefaltete Bildlage) liess bisher einen Pass exakt stehen, waehrend der andere
 * gelerpt wurde -> Schichten senkrecht versetzt, Spalt. Mit dem homogenen Lerp (hlerp-Modus 3) behandelt der VS
 * jeden Vertex wie im 3D-Pass (Clip-Lerp, w-Wechsel -> Tick N); hier zaehlen darum nur Bezugs-Vertices, die in
 * beiden Ticks vor der Kamera (echte w > 0) und im erweiterten Bild (|ndc| <= 2) liegen: mindestens einer, und alle
 * diese springen hoechstens maxjump (Schnitt bzw. falsche Instanz -> weiter exakt). Fuer P2 und 3D dieselbe Regel.
 * P2 nur mit echter w beider Ticks (sonst Bildraum-Lerp, bleibt exakt). 1 = lerpen. */
static unsigned st_rr[4]; /* W429 Ablehnungsgruende: Daten neu, ohne echte w, kein Bezug im Bild, Sprung */
static int ip_rej_relax(const ip_list_t *A, const ip_list_t *B, const ip_op_t *a, const ip_op_t *o, float maxjump)
{
    int q, nin = 0;
    const int p2 = ip_mvp_affine(o->mvp);
    if (a->rawhash != o->rawhash || a->nv != o->nv || p2 != ip_mvp_affine(a->mvp)) { st_rr[0]++; return 0; }
    if (p2 && (!a->p2w || !o->p2w || !A->wb_ok || !B->wb_ok || sb_ip_hlerp() < 2)) { st_rr[1]++; return 0; }
    for (q = 0; q < 4; q++) {
        float c0[4], c1[4], w0, w1, x0, y0, x1, y1;
        ip_clip(a->pref[q], a->mvp, c0);
        ip_clip(o->pref[q], o->mvp, c1);
        w0 = p2 ? a->prefw[q] * c0[3] : c0[3];
        w1 = p2 ? o->prefw[q] * c1[3] : c1[3];
        if (w0 <= 1e-6f || w1 <= 1e-6f || c0[3] == 0.0f || c1[3] == 0.0f) { continue; } /* hinter der Kamera: W224 je Vertex */
        x0 = c0[0] / c0[3]; y0 = c0[1] / c0[3]; x1 = c1[0] / c1[3]; y1 = c1[1] / c1[3];
        if (fabsf(x0) > 2.0f || fabsf(y0) > 2.0f || fabsf(x1) > 2.0f || fabsf(y1) > 2.0f) { continue; }
        if (fabsf(x1 - x0) > maxjump || fabsf(y1 - y0) > maxjump) { st_rr[3]++; return 0; }
        nin++;
    }
    if (!nin) { st_rr[2]++; }
    return nin > 0;
}

/* Partnersuche ueber den ganzen Vortick (W73): Draws von A je Signatur-Eimer
 * in aufsteigender Reihenfolge verkettet. Frueher Fenster von 256 Draws ab dem
 * letzten Treffer - ein im Vortick fehlender/geaenderter Block > 256 Draws
 * (z. B. ein Monitor-Pass, der nur jeden 2. Tick neu gebaut wird) liess alle
 * folgenden Draws des Ticks ohne Partner (W73_d1: ~600 nomatch je 2. Tick). */
#define IP_HBITS 12
#define IP_HB(k) (((k) ^ ((k) >> IP_HBITS) ^ ((k) >> 24)) & ((1u << IP_HBITS) - 1u))
static int ip_hhead[1u << IP_HBITS], ip_ghead[1u << IP_HBITS];
static int *ip_hnext, *ip_gnext;
static unsigned ip_hcap;

/* PERF(PERF-M2,W174) ip_match_compact: Signatur, Eingangs-Hash und Bezugspunkte der Draws von A
 * dicht gepackt (ip_inst_best lief die Hash-Kette ueber die grossen ip_op_t; 46 % der Swap-Proben
 * in schweren M2-Ticks, W174_r2). Gleiche Vergleiche, gleiche Reihenfolge -> gleiche Zuordnung. */
typedef struct { unsigned sig, rawhash; float rn[4][3]; } ip_ck_t;
static ip_ck_t *ip_ck;
static unsigned ip_ckcap;
static int ip_hash_build(ip_list_t *A)
{
    unsigned j;
    if (ip_hcap < A->nops) {
        int *nn = (int *)realloc(ip_hnext, A->nops * sizeof *nn);
        if (!nn) { return 0; }
        ip_hnext = nn;
        nn = (int *)realloc(ip_gnext, A->nops * sizeof *nn);
        if (!nn) { return 0; }
        ip_gnext = nn;
        ip_hcap = A->nops;
    }
    if (g_cfg.ip_match_compact && ip_ckcap < A->nops) {
        ip_ck_t *nc = (ip_ck_t *)realloc(ip_ck, A->nops * sizeof *nc);
        if (!nc) { return 0; }
        ip_ck = nc;
        ip_ckcap = A->nops;
    }
    memset(ip_hhead, 0xFF, sizeof ip_hhead);
    memset(ip_ghead, 0xFF, sizeof ip_ghead);
    for (j = A->nops; j-- > 0;) {
        const ip_op_t *a = &A->ops[j];
        unsigned h;
        if (a->kind != IP_DRAW) { continue; }
        if (g_cfg.ip_match_compact) { ip_ck[j].sig = a->sig; ip_ck[j].rawhash = a->rawhash; memcpy(ip_ck[j].rn, a->rn, sizeof a->rn); }
        h = IP_HB(a->sig);
        ip_hnext[j] = ip_hhead[h];
        ip_hhead[h] = (int)j;
        h = IP_HB(a->gkey);
        ip_gnext[j] = ip_ghead[h];
        ip_ghead[h] = (int)j;
    }
    return 1;
}

/* Freier Partner gleicher Signatur, raw: 1 = gleicher Eingangs-Hash, 0 =
 * anderer, -1 = egal. Bevorzugt der naechste ab jp (Reihenfolge), sonst der
 * naechstliegende davor. -1 = keiner. */
static int ip_pick(ip_list_t *A, const ip_op_t *o, unsigned jp, int raw)
{
    unsigned h = IP_HB(o->sig);
    int j, before = -1;
    for (j = ip_hhead[h]; j >= 0; j = ip_hnext[j]) {
        const ip_op_t *a = &A->ops[j];
        if (a->taken || a->sig != o->sig) { continue; }
        if (raw == 1 && a->rawhash != o->rawhash) { continue; }
        if (raw == 0 && a->rawhash == o->rawhash) { continue; }
        if ((unsigned)j >= jp) {
            if (before < 0 || (unsigned)j - jp <= jp - (unsigned)before) { return j; }
            return before;
        }
        before = j;
    }
    return before;
}

/* Geometrie-Partner (W73): gleiche Eingangs-Positionen, gleiches Ziel und
 * gleiche Vertexzahl, aber anderer Shader/andere Textur (W73_d3: Sub-Monitor-
 * Paesse wechseln zwischen Ticks Material/Shader, ~760 Draws ohne Partner).
 * Freie Partner bevorzugt, sonst darf ein schon vergebener Draw des Vorticks
 * ein zweites Mal als Positionsquelle dienen (Farben/UV kommen aus Tick N). */
#define IP_COST_INF 1e9f
static float ip_cost(const ip_op_t *a, const ip_op_t *o); /* POPIN (W91) */
static int ip_pick_geo(ip_list_t *A, const ip_op_t *o, unsigned jp)
{
    int j, best = -1, besttk = -1;
    unsigned bd = 0xFFFFFFFFu, bdt = 0xFFFFFFFFu;
    for (j = ip_ghead[IP_HB(o->gkey)]; j >= 0; j = ip_gnext[j]) {
        const ip_op_t *a = &A->ops[j];
        unsigned d;
        if (a->gkey != o->gkey || a->rtv != o->rtv || a->nv != o->nv || a->rawhash != o->rawhash) {
            continue;
        }
        d = (unsigned)j >= jp ? (unsigned)j - jp : jp - (unsigned)j;
        if (g_cfg.interp_inst) { /* POPIN (W91): naechste Instanz statt Reihenfolge */
            float c = ip_cost(a, o);
            d = c >= IP_COST_INF ? 0xFFFFFFFEu : (unsigned)(c * 1.0e6f);
        }
        if (!a->taken) { if (d < bd) { bd = d; best = j; } }
        else if (d < bdt) { bdt = d; besttk = j; }
    }
    return best >= 0 ? best : besttk;
}

/* Praktisch unbewegt (W73): alle 4 Bezugs-Vertices springen < 1e-4 NDC
 * (~0,13 px bei 2560). Solche Draws (Cockpit-Teile mit bitweise leicht
 * anderer Matrix) exakt aus Tick N zeichnen - sonst weichen Zwischenbilder
 * durch Rundung um 1 Farbstufe vom stehenden Bild ab (W73_d7 A4). */
static int ip_still(const ip_op_t *a, const ip_op_t *o)
{
    int q;
    for (q = 0; q < 4; q++) {
        float c0[4], c1[4];
        ip_clip(a->pref[q], a->mvp, c0);
        ip_clip(o->pref[q], o->mvp, c1);
        if (c0[3] <= 1e-6f || c1[3] <= 1e-6f) { return 0; }
        if (fabsf(c1[0] / c1[3] - c0[0] / c0[3]) > 1e-4f || fabsf(c1[1] / c1[3] - c0[1] / c0[3]) > 1e-4f ||
            fabsf(c1[2] / c1[3] - c0[2] / c0[3]) > 1e-6f) {
            return 0;
        }
    }
    return 1;
}

/* ---------- POPIN (W91): Instanz-Zuordnung ----------
 * Befund W91: ein Modell, das mehrfach im Tick gezeichnet wird (Haeuser,
 * Cockpit-Teile: gleiche VS/Textur/PS/Vertexzahl UND gleiche Objektraum-
 * Vertices, nur andere Matrix), hat mehrere gleichwertige Partner. ip_pick
 * waehlt den naechsten freien in Draw-Reihenfolge - kommt eine Instanz ins
 * Bild (Culling) oder faellt eine heraus, verschiebt sich die Reihenfolge und
 * der Draw wird mit einer ANDEREN Instanz des Vorticks interpoliert: das Haus
 * fliegt in den Zwischenbildern von der Stelle des Nachbarn heran (Aufpoppen
 * fuer 1-3 Bilder je Tick), die richtige Instanz bleibt ohne Partner.
 * interp_inst=1: Partner = kleinster Bildsprung der 4 Bezugs-Vertices, nur
 * wenn auch umgekehrt kein anderer offener Draw des Ticks naeher an diesem
 * Partner liegt (gegenseitig naechster) und die Wahl eindeutig ist; sonst
 * exakt aus Tick N (30 Hz, aber nie falsch). */
static int ip_bhead[1u << IP_HBITS];
static int *ip_bnext;
static unsigned ip_bcap;

/* Bildsprung a -> o: groesster NDC-Sprung der 4 Bezugs-Vertices (wie
 * ip_plausible, aus den beim Aufzeichnen berechneten NDC). */
static float ip_cost_rn(const float arn[4][3], const ip_op_t *o)
{
    int q, front = 0;
    float m = 0.0f;
    for (q = 0; q < 4; q++) {
        float w0 = arn[q][2], w1 = o->rn[q][2];
        if (w0 > 1e-6f && w1 > 1e-6f) {
            float dx = fabsf(o->rn[q][0] - arn[q][0]), dy = fabsf(o->rn[q][1] - arn[q][1]);
            if (dx > m) { m = dx; }
            if (dy > m) { m = dy; }
            front++;
        } else if (!(w0 < -1e-6f && w1 < -1e-6f)) {
            return IP_COST_INF; /* w-Wechsel */
        }
    }
    return front ? m : 0.0f;
}
static float ip_cost(const ip_op_t *a, const ip_op_t *o) { return ip_cost_rn(a->rn, o); }

static int ip_bhash_build(ip_list_t *B)
{
    unsigned j;
    if (ip_bcap < B->nops) {
        int *nn = (int *)realloc(ip_bnext, B->nops * sizeof *nn);
        if (!nn) { return 0; }
        ip_bnext = nn;
        ip_bcap = B->nops;
    }
    memset(ip_bhead, 0xFF, sizeof ip_bhead);
    for (j = B->nops; j-- > 0;) {
        unsigned h;
        if (B->ops[j].kind != IP_DRAW) { continue; }
        h = IP_HB(B->ops[j].sig);
        ip_bnext[j] = ip_bhead[h];
        ip_bhead[h] = (int)j;
    }
    return 1;
}

/* Kleinster-Sprung-Kandidat in A (frei, gleiche Signatur + Eingangs-Hash).
 * *n = Zahl der Kandidaten, *c1/*c2 = bester/zweitbester Sprung. */
static unsigned st_ck_chk, st_ck_bad; /* DIAG(PERF-M2,W174) ip_match_compact=2: Selbstpruefung */
static int ip_inst_best_x(ip_list_t *A, const ip_op_t *o, int *n, float *c1, float *c2, int *second, int compact);
static int ip_inst_best(ip_list_t *A, const ip_op_t *o, int *n, float *c1, float *c2, int *second)
{
    int b = ip_inst_best_x(A, o, n, c1, c2, second, g_cfg.ip_match_compact > 0);
    if (g_cfg.ip_match_compact == 2) {
        int n2, s2, b2;
        float d1, d2;
        b2 = ip_inst_best_x(A, o, &n2, &d1, &d2, &s2, 0);
        st_ck_chk++;
        if (b2 != b || n2 != *n || s2 != *second || memcmp(&d1, c1, sizeof d1) != 0 || memcmp(&d2, c2, sizeof d2) != 0) {
            if (st_ck_bad++ < 8u) { HLOG_W("INTERP match_compact: Abweichung best %d/%d n %d/%d", b, b2, *n, n2); }
        }
        if ((st_ck_chk % 200000u) == 0u) { HLOG_I("INTERP match_compact Pruefung: %u Vergleiche, abweichend=%u", st_ck_chk, st_ck_bad); }
    }
    return b;
}
static int ip_inst_best_x(ip_list_t *A, const ip_op_t *o, int *n, float *c1, float *c2, int *second, int compact)
{
    int j, best = -1;
    *n = 0;
    *c1 = *c2 = IP_COST_INF;
    *second = -1;
    if (compact && ip_ck && ip_ckcap >= A->nops) { /* PERF(PERF-M2,W174): gleiche Pruefungen, dicht gepackt */
        for (j = ip_hhead[IP_HB(o->sig)]; j >= 0; j = ip_hnext[j]) {
            const ip_ck_t *k = &ip_ck[j];
            float c;
            if (k->sig != o->sig || k->rawhash != o->rawhash || A->ops[j].taken) { continue; }
            (*n)++;
            c = ip_cost_rn(k->rn, o);
            if (c < *c1) { *c2 = *c1; *second = best; *c1 = c; best = j; }
            else if (c < *c2) { *c2 = c; *second = j; }
        }
        return best;
    }
    for (j = ip_hhead[IP_HB(o->sig)]; j >= 0; j = ip_hnext[j]) {
        const ip_op_t *a = &A->ops[j];
        float c;
        if (a->taken || a->sig != o->sig || a->rawhash != o->rawhash) { continue; }
        (*n)++;
        c = ip_cost(a, o);
        if (c < *c1) { *c2 = *c1; *second = best; *c1 = c; best = j; }
        else if (c < *c2) { *c2 = c; *second = j; }
    }
    return best;
}

/* Gibt es in B einen anderen offenen Draw gleicher Art, der naeher an a liegt? */
static int ip_inst_closer(ip_list_t *B, unsigned oi, const ip_op_t *a, float c)
{
    int k;
    const ip_op_t *o = &B->ops[oi];
    for (k = ip_bhead[IP_HB(o->sig)]; k >= 0; k = ip_bnext[k]) {
        const ip_op_t *o2 = &B->ops[k];
        if ((unsigned)k == oi || o2->sig != o->sig || o2->rawhash != o->rawhash || o2->match >= 0) { continue; }
        if (o2->kind != IP_DRAW || o2->why != IP_W_NONE) { continue; } /* schon entschieden */
        if (ip_cost(a, o2) < c) { return 1; }
    }
    return 0;
}

/* FIX(W429 M3-ROAD-FIX) interp_p2hom: zwei Vortick-Kandidaten, die Durchgaenge DERSELBEN Lage sind (gleiche
 * Eingangsdaten, Vertexzahl, Matrix, Ziel und bitgleiche Bezugs-Vertices samt echter w), liefern im Zwischenbild
 * dieselbe Lage (aus A kommt nur die Position, Farbe/UV aus Tick N) - die Wahl ist dann gleichgueltig und nicht
 * mehrdeutig. Bisher wurden mehrfach gezeichnete Bodenstuecke (P2, ein Bezugs-Vertex an der Augenebene -> grosser
 * Sprung c1 = c2 > 0,02) "nicht eindeutig" exakt, und per interp_pass alle Durchgaenge der Instanz mit ihnen
 * (M3: ~8000 von ~8800 Spalt-Ecken exakt/gelerpt). Nur SB. */
static int ip_same_pass(const ip_op_t *x, const ip_op_t *y)
{
    if (!g_cfg.interp_p2hom || g_sba_loc) { return 0; }
    return x->nv == y->nv && x->rawhash == y->rawhash && x->rtv == y->rtv && x->p2w == y->p2w &&
           memcmp(x->mvp, y->mvp, sizeof x->mvp) == 0 && memcmp(x->pref, y->pref, sizeof x->pref) == 0 &&
           (!x->p2w || memcmp(x->prefw, y->prefw, sizeof x->prefw) == 0);
}

/* interp_inst=1: Partner fuer o (Index oi). -1 = keiner (neu im Bild bzw. nicht
 * eindeutig -> exakt). *amb = 1, wenn es Kandidaten gab, aber keiner eindeutig. */
static int ip_pick_inst(ip_list_t *A, ip_list_t *B, unsigned oi, int *amb)
{
    const ip_op_t *o = &B->ops[oi];
    int n, best, second;
    float c1, c2;
    *amb = 0;
    best = ip_inst_best(A, o, &n, &c1, &c2, &second);
    if (best < 0) { return -1; }
    if (n > 1) { st_imulti++; }
    if (c1 >= IP_COST_INF) { *amb = 1; return -1; }
    /* eindeutig: Zweitbester deutlich weiter weg (oder beide praktisch gleich nah) */
    if (second >= 0 && c2 < 2.0f * c1 && c2 > 0.02f && !ip_same_pass(&A->ops[best], &A->ops[second])) { *amb = 1; return -1; }
    if (ip_inst_closer(B, oi, &A->ops[best], c1)) { *amb = 1; return -1; }
    return best;
}

/* DIAG(POPIN,W91): Pixel-Huellrechteck der 4 Bezugs-Vertices im Ziel-Viewport
 * (grob, nur Lagevergleich). 0 = nicht vor der Kamera. */
static int ip_refbox(const ip_op_t *o, int bb[4])
{
    int q, n = 0;
    bb[0] = bb[1] = 1 << 30;
    bb[2] = bb[3] = -(1 << 30);
    for (q = 0; q < 4; q++) {
        float c[4];
        int x, y;
        ip_clip(o->pref[q], o->mvp, c);
        if (c[3] <= 1e-6f) { continue; }
        x = (int)(o->vp.TopLeftX + (c[0] / c[3] + 1.0f) * 0.5f * o->vp.Width);
        y = (int)(o->vp.TopLeftY + (1.0f - c[1] / c[3]) * 0.5f * o->vp.Height);
        if (x < bb[0]) { bb[0] = x; }
        if (y < bb[1]) { bb[1] = y; }
        if (x > bb[2]) { bb[2] = x; }
        if (y > bb[3]) { bb[3] = y; }
        n++;
    }
    return n;
}

static void ip_iwrong_log(const ip_list_t *A, const ip_list_t *B, unsigned i, int found, int best)
{
    static unsigned nlog;
    int b0[4], b1[4], b2[4];
    const ip_op_t *o = &B->ops[i];
    if (!sb_in_mission() || nlog >= 4000u) { return; } /* nur Spielermission (W232 statt Ladezaehler == 4), gedeckelt */
    if (g_cfg.pf_ring > 0 && !sb_pf_active()) { return; } /* mit pf_ring nur waehrend der Bildfolgen */
    nlog++;
    ip_refbox(o, b0);
    ip_refbox(&A->ops[found], b1);
    ip_refbox(&A->ops[best], b2);
    HLOG_I("INTERP iwrong f=%u i=%u n=%u %s sig=%08X raw=%08X partner=%d sprung=%.3f naechste=%d sprung=%.3f "
           "box=%d,%d,%d,%d partnerbox=%d,%d,%d,%d naechstebox=%d,%d,%d,%d",
           B->frame, i, o->nv, o->rtv == g_rtv ? "BB" : "RT", o->sig, o->rawhash, found,
           (double)ip_cost(&A->ops[found], o), best, (double)ip_cost(&A->ops[best], o),
           b0[0], b0[1], b0[2], b0[3], b1[0], b1[1], b1[2], b1[3], b2[0], b2[1], b2[2], b2[3]);
}

/* POPIN (W91): 16-Bit-Schluessel Instanz-Durchgang = gkey + Matrix (FNV). */
/* FIX(W251) interp_zcons: Kamerabewegung D = M_A^-1 * M_B in double (W kuerzt sich heraus). */
static int zc_dmat(double D[16], const float MA[16], const float MB[16])
{
    double a[4][8];
    int i, j, k;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) { a[i][j] = MA[i * 4 + j]; a[i][j + 4] = (i == j) ? 1.0 : 0.0; }
    }
    for (i = 0; i < 4; i++) {
        int pv = i;
        double d;
        for (k = i + 1; k < 4; k++) { if (fabs(a[k][i]) > fabs(a[pv][i])) { pv = k; } }
        if (fabs(a[pv][i]) < 1e-12) { return 0; }
        if (pv != i) { for (j = 0; j < 8; j++) { double t = a[i][j]; a[i][j] = a[pv][j]; a[pv][j] = t; } }
        d = a[i][i];
        for (j = 0; j < 8; j++) { a[i][j] /= d; }
        for (k = 0; k < 4; k++) {
            if (k != i) { double f = a[k][i]; for (j = 0; j < 8; j++) { a[k][j] -= f * a[i][j]; } }
        }
    }
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            D[i * 4 + j] = a[i][4] * MB[0 * 4 + j] + a[i][5] * MB[1 * 4 + j] + a[i][6] * MB[2 * 4 + j] + a[i][7] * MB[3 * 4 + j];
        }
    }
    return 1;
}

static double zc_derr(const double X[16], const double Y[16])
{
    double mx = 0.0, er = 0.0;
    int q;
    for (q = 0; q < 16; q++) {
        double d = fabs(X[q] - Y[q]), m = fabs(Y[q]);
        if (m > mx) { mx = m; }
        if (d > er) { er = d; }
    }
    return mx > 0.0 ? er / mx : 1.0;
}

static int ip_m4inv(float out[16], const float m[16]);
static void ip_m4mul(float r[16], const float a[16], const float b[16]);
static unsigned st_zc_ticks, st_zc_tgt, st_zc_draws, st_zc_rel, st_zc_nocand, st_zc_cam, st_zc_ln; /* FIX(W251) interp_zcons: Zaehler */
static unsigned ip_mvpkey(const ip_op_t *o)
{
    const unsigned char *b = (const unsigned char *)o->mvp;
    unsigned h = 2166136261u ^ o->gkey, k;
    for (k = 0; k < sizeof o->mvp; k++) { h = (h ^ b[k]) * 16777619u; }
    return (h ^ (h >> 16)) & 0xFFFFu;
}


/* FIX(W284 INTERP-RAWCHG) interp_rawzc: Kamerabewegungen D = M_A^-1 * M_B der (bis zu) 8 groessten
 * plausibel gelerpten Welt-Draws eines Ziels (wie interp_zcons, dort im Block). Rueckgabe: Anzahl. */
/* PERF(W284): die 8 groessten plausibel gelerpten Welt-Draws je Ziel (rtv, dsv) in EINEM Durchlauf
 * ueber B (vorher ein Durchlauf je Ziel, ~0,2 ms je Tick); gleiche Auswahl und Reihenfolge wie dort. */
enum { RZ_PT = 32 };
static ID3D11RenderTargetView *rz_prt[RZ_PT];
static ID3D11DepthStencilView *rz_pds[RZ_PT];
static int rz_prr[RZ_PT][8], rz_pn, rz_povf;
static unsigned rz_prc[RZ_PT][8]; /* W294 Bit 1: Anzahl Draws je Matrix-Paar der Eintraege */
/* FIX(W294 LINE-STREAK) interp_streak Bit 1: traegt das Paar a (N-1) -> r (N) eine Kamerabewegung?
 * P2-Draws im RT haben eine affine Bildraum-Matrix (die Kamera steckt schon in den Vertices), gleiche
 * Matrizen liefern D = I. In P2-lastigen Zielen (M2: Meer, Boot) waren die 8 groessten gelerpten Draws
 * alle so -> keine 3D-Kachel bestand den Welt-Test, obwohl sie in der Welt ruht. */
static int ip_mvp_affine(const float m[16]) { return m[3] == 0.0f && m[7] == 0.0f && m[11] == 0.0f; }
static int zc_cam_src(const ip_op_t *a, const ip_op_t *r)
{
    if (r->rzm) { return 0; } /* W317: mit der Kamera bewegt bzw. Folge-Durchgang traegt keine eigene Kamera */
    if (!(g_cfg.interp_streak & 1)) { return 1; }
    return !ip_mvp_affine(r->mvp) && memcmp(a->mvp, r->mvp, sizeof r->mvp) != 0;
}
/* W294 Bit 2: Kamera je Renderziel (Tiefenpuffer egal), damit auch Welt-Draws ohne Tiefenpuffer
 * (Meeres-Kacheln) den Welt-Test bekommen. */
static ID3D11DepthStencilView *rz_dkey(const ip_op_t *o) { return (g_cfg.interp_streak & 2) ? NULL : o->dsv; }
static unsigned st_sk_rl, st_sk_rlcam, st_sk_rlofs, st_sk_nod, st_sk_ln; /* W294: Zaehler */
static unsigned st_w410_rzb, st_w410_big, st_w410_cam, st_w410_mov, st_w410_rzm; /* W410 loc_interp_rawrzb: Matrix-Lerp statt Vertex-Lerp (davon >= 1000 Vertices), Matrix passt nicht zur Kamera, Bezugs-Vertices bewegt */

/* W294 Bit 1: Kamera-Kandidaten nach Mehrheit statt Groesse. Ruhende Welt-Geometrie mit Matrix = VP
 * (Kacheln, Gelaendestreifen) teilt sich bitgenau dasselbe Matrix-Paar (M_A, M_B); in M2 faehrt die Kamera
 * mit dem Landungsboot, die groessten 3D-Draws sind Boot-Teile (D ~ I) und fahrende Mechs. Gezaehlt wird je
 * Paar (gleiche Matrizen N und N-1) unter den gelerpten Kandidaten; die 8 haeufigsten Paare je Ziel
 * (Gleichstand: groesseres n, dann Reihenfolge) liefern D. */
enum { RZ_HB = 4096 };
static int rz_hq[RZ_HB];
static unsigned rz_hn[RZ_HB];
static unsigned rz_pkey(const ip_list_t *A, const ip_op_t *r)
{
    const unsigned *u = (const unsigned *)r->mvp, *v = (const unsigned *)A->ops[r->match].mvp;
    unsigned h = 2166136261u;
    int i;
    for (i = 0; i < 16; i++) { h = (h ^ u[i]) * 16777619u; h = (h ^ v[i]) * 16777619u; }
    return h;
}
static int rz_psame(const ip_list_t *A, const ip_list_t *B, int q1, const ip_op_t *r)
{
    const ip_op_t *o1 = &B->ops[q1];
    return memcmp(o1->mvp, r->mvp, sizeof r->mvp) == 0 &&
           memcmp(A->ops[o1->match].mvp, A->ops[r->match].mvp, sizeof r->mvp) == 0;
}
static unsigned rz_pcount(const ip_list_t *A, const ip_list_t *B, const ip_op_t *r, int add, int q)
{
    unsigned h = rz_pkey(A, r) & (RZ_HB - 1u), k;
    for (k = 0; k < RZ_HB; k++, h = (h + 1u) & (RZ_HB - 1u)) {
        if (rz_hq[h] < 0) {
            if (!add) { return 0; }
            rz_hq[h] = q; rz_hn[h] = 1;
            return 1;
        }
        if (rz_psame(A, B, rz_hq[h], r)) { if (add) { rz_hn[h]++; } return rz_hn[h]; }
    }
    return 0;
}

static void rz_cam_pre(const ip_list_t *A, const ip_list_t *B)
{
    unsigned q;
    int x;
    int maj = (g_cfg.interp_streak & 1) != 0; /* W294 */
    rz_pn = 0; rz_povf = 0;
    if (maj) {
        memset(rz_hq, 0xFF, sizeof rz_hq);
        for (q = 0; q < B->nops; q++) {
            const ip_op_t *r = &B->ops[q];
            if (r->kind != IP_DRAW || r->why != IP_W_LERP || !r->flag || r->match < 0 || (unsigned)r->match >= A->nops) { continue; }
            if (!zc_cam_src(&A->ops[r->match], r)) { continue; }
            rz_pcount(A, B, r, 1, (int)q);
        }
    }
    for (q = 0; q < B->nops; q++) {
        const ip_op_t *r = &B->ops[q];
        int w, t = -1, *rr;
        if (r->kind != IP_DRAW || r->why != IP_W_LERP || !r->flag || r->match < 0 || (unsigned)r->match >= A->nops) { continue; }
        if (!zc_cam_src(&A->ops[r->match], r)) { continue; } /* W294 Bit 1 */
        for (x = 0; x < rz_pn; x++) { if (rz_prt[x] == r->rtv && rz_pds[x] == rz_dkey(r)) { t = x; break; } }
        if (t < 0) {
            if (rz_pn >= RZ_PT) { rz_povf = 1; continue; }
            t = rz_pn++;
            rz_prt[t] = r->rtv; rz_pds[t] = rz_dkey(r);
            for (x = 0; x < 8; x++) { rz_prr[t][x] = -1; }
        }
        rr = rz_prr[t];
        if (maj) { /* W294 Bit 1: haeufigste Matrix-Paare, je Paar ein Eintrag */
            unsigned c = rz_pcount(A, B, r, 0, 0), *rc = rz_prc[t];
            int dup = 0;
            for (w = 0; w < 8 && rr[w] >= 0; w++) { if (rc[w] == c && rz_psame(A, B, rr[w], r)) { dup = 1; break; } }
            if (dup) { continue; }
            for (w = 0; w < 8; w++) {
                if (rr[w] < 0 || c > rc[w] || (c == rc[w] && r->nv > B->ops[rr[w]].nv)) {
                    int z;
                    for (z = 7; z > w; z--) { rr[z] = rr[z - 1]; rc[z] = rc[z - 1]; }
                    rr[w] = (int)q; rc[w] = c;
                    break;
                }
            }
            continue;
        }
        for (w = 0; w < 8; w++) {
            if (rr[w] < 0 || r->nv > B->ops[rr[w]].nv) {
                int z;
                for (z = 7; z > w; z--) { rr[z] = rr[z - 1]; }
                rr[w] = (int)q;
                break;
            }
        }
    }
}
/* Kamerabewegungen D = M_A^-1 * M_B des Ziels von o (nach rz_cam_pre). Rueckgabe: Anzahl. */
static int rz_cam(const ip_list_t *A, const ip_list_t *B, const ip_op_t *o, double D[8][16])
{
    int x, t = -1, n = 0;
    for (x = 0; x < rz_pn; x++) { if (rz_prt[x] == o->rtv && rz_pds[x] == rz_dkey(o)) { t = x; break; } }
    if (t < 0) { return 0; }
    for (x = 0; x < 8 && rz_prr[t][x] >= 0; x++) {
        const ip_op_t *r = &B->ops[rz_prr[t][x]];
        if (zc_dmat(D[n], A->ops[r->match].mvp, r->mvp)) { n++; }
    }
    return n;
}

/* FIX(W284) interp_rawzc: Bezugs-Vertices von o = die von a + fester Vektor d (w = 1)?
 * Toleranz: Rundung von p + d in float (relativ 1e-5 zum Betrag). 1 = ja, d gesetzt. */
static int rz_offset(const ip_op_t *a, const ip_op_t *o, float d[3])
{
    int q, c;
    for (q = 0; q < 4; q++) {
        if (a->pref[q][3] != 1.0f || o->pref[q][3] != 1.0f) { return 0; }
    }
    for (c = 0; c < 3; c++) { d[c] = o->pref[0][c] - a->pref[0][c]; }
    for (q = 1; q < 4; q++) {
        for (c = 0; c < 3; c++) {
            float e = o->pref[q][c] - a->pref[q][c] - d[c], m = fabsf(o->pref[q][c]) + fabsf(a->pref[q][c]) + fabsf(d[c]);
            if (fabsf(e) > 1e-5f * (1.0f + m)) { return 0; }
        }
    }
    return 1;
}
/* FIX(W294) interp_streak Bit 4: Versatz-Test fuer neu gelegte Gitter (Boden-/Wasserkacheln). Das Spiel legt
 * Gelaende- und Meeresgitter in festen Schritten (5, 10, ...) neu um die Kamera; der Partner gleicher Signatur
 * ist dann ein anderes Gitterstueck: waagerecht (x, z) um d verschoben, die Hoehe y aber verschieden
 * (M2: Wellen bis ~0,01, M4: Gelaendereihe bis ~0,4). rz_offset (Toleranz ~1e-3 in allen Achsen) lehnte
 * diese ab -> Vertex-Lerp ueber das Gitter = Kachel wandert/verformt sich, Naht zum Nachbarn. Hier:
 * d = Versatz des ersten Bezugs-Vertex, x/z aller vier Bezugs-Vertices innerhalb 0,01 + 2 % von |d_xz|,
 * y frei. rzb nutzt nur die Tick-N-Vertices und die Matrix N-1; der genaue Versatz geht nicht ein. */
static int rz_offset_grid(const ip_op_t *a, const ip_op_t *o, float d[3])
{
    int q;
    float tol;
    for (q = 0; q < 4; q++) {
        if (a->pref[q][3] != 1.0f || o->pref[q][3] != 1.0f) { return 0; }
    }
    d[0] = o->pref[0][0] - a->pref[0][0]; d[1] = o->pref[0][1] - a->pref[0][1]; d[2] = o->pref[0][2] - a->pref[0][2];
    if (fabsf(d[0]) < 0.5f && fabsf(d[2]) < 0.5f) { return 0; } /* kein Gitterschritt (Wellen/Animation: Vertex-Lerp richtig) */
    tol = 0.01f + 0.02f * (fabsf(d[0]) > fabsf(d[2]) ? fabsf(d[0]) : fabsf(d[2]));
    for (q = 1; q < 4; q++) {
        if (fabsf(o->pref[q][0] - a->pref[q][0] - d[0]) > tol || fabsf(o->pref[q][2] - a->pref[q][2] - d[2]) > tol) { return 0; }
    }
    return 1;
}
/* W349 LOC-FRAMES-2: Kamera-Pruefung der Bezugs-Vertices (render_interp_camfit.h, Selbsttest CAMFIT). */
static double li_camfit_pm(const float pa[4][4], const float *ma, const ip_op_t *o, double D[8][16], int nD)
{
    return ipcf_camfit(pa, ma, (const float (*)[4])o->pref, o->mvp, (const double (*)[16])D, nD);
}
/* Vertex-Lerp-Pruefung: Bezugs-Vertices des Partners c mit dessen Matrix (Vortick) gegen o. Matrix-Lerp-Pruefung
 * (Tick-N-Vertices mit M_A): li_camfit_pm(o->pref, a->mvp, o, ...). */
static double li_camfit(const ip_op_t *c, const ip_op_t *o, double D[8][16], int nD)
{
    return li_camfit_pm((const float (*)[4])c->pref, c->mvp, o, D, nD);
}
/* FIX(W284) interp_rawzc=2: Hash-Kette der Vortick-Draws nach sig0 (Signatur ohne Vertexzahl), bei Bedarf je Tick */
static int rz_s0head[1u << IP_HBITS], *rz_s0next;
static unsigned rz_s0cap;
static int rz_s0_build(const ip_list_t *A)
{
    unsigned j;
    if (rz_s0cap < A->nops) {
        int *nn = (int *)realloc(rz_s0next, A->nops * sizeof *nn);
        if (!nn) { return 0; }
        rz_s0next = nn;
        rz_s0cap = A->nops;
    }
    memset(rz_s0head, 0xFF, sizeof rz_s0head);
    for (j = A->nops; j-- > 0;) {
        const ip_op_t *a = &A->ops[j];
        unsigned h;
        if (a->kind != IP_DRAW) { continue; }
        h = IP_HB(a->sig0);
        rz_s0next[j] = rz_s0head[h];
        rz_s0head[h] = (int)j;
    }
    return 1;
}
/* FIX(W347 GHOST) interp_bbcam: Partner eines exakt gebliebenen rawchg-Draws (Billboard, z. B. Baum) per Kamera.
 * Baeume sind Quads (n=6/12), die das Spiel je Tick neu zur Kamera ausgerichtet in den VB schreibt (Matrix
 * gleich, Eingangsdaten neu, W347_ld5). Ein Billboard ruht in der Welt um seinen Mittelpunkt; fuer den richtigen
 * Vortick-Partner c gilt dann Schwerpunkt(c)*M_c*D ~ Schwerpunkt(o)*M_o (D = Kamerabewegung des Ziels,
 * Zeilenvektoren), die Ecken weichen nur um die Eigendrehung ab. Gewaehlt wird der kleinste Schwerpunkt-
 * Fehler (Gleichstand: Reihenfolge, deterministisch). -1 = keiner. */
static unsigned st_bb_ok, st_bb_no, st_bb_far, st_bb_build; /* W347: Zaehler (INTERP bbcam) */
static double st_bb_ms; /* W347: Zeit in ip_bb_pick (inkl. Rasteraufbau) je Statistikfenster */
/* Raster der Vortick-Kandidaten nach vorhergesagter NDC-Lage (Schwerpunkt * D[0]) je Signatur: ohne Raster
 * prueft jeder Baum alle gleichen Baeume des Vorticks (W347_lf1: 43 ms je Tick). Neu je Vortick und Ziel. */
enum { BBG_BITS = 12, BBG_N = 8 };
#define BBG_CELL 0.05
typedef struct {
    int head[1u << BBG_BITS], *next;
    float *px, *py; /* vorhergesagte NDC-Lage (Schwerpunkt * D0) je Vortick-Draw: billige Vorpruefung */
    double (*pv)[5][4]; /* je Vortick-Draw: Clip der 4 Bezugs-Vertices + Schwerpunkt (ipbb_prep, einmal je Tick) */
    unsigned cap, ser, used;
    ID3D11RenderTargetView *rtv;
    double D0[16];
} bbg_t;
static bbg_t bbg[BBG_N]; /* je Ziel ein Raster (Ziele wechseln innerhalb des Ticks; ein einzelnes Raster wurde je Draw neu gebaut) */
static unsigned bbg_clock;
static unsigned bbg_key(unsigned sig, int cx, int cy)
{
    unsigned h = (sig * 2654435761u) ^ ((unsigned)cx * 40503u) ^ ((unsigned)cy * 2246822519u);
    return (h ^ (h >> 15) ^ (h >> 27)) & ((1u << BBG_BITS) - 1u);
}
static bbg_t *bbg_build(const ip_list_t *A, const ip_op_t *o, const double D0[16])
{
    unsigned j;
    int k, v = 0;
    bbg_t *g;
    for (k = 0; k < BBG_N; k++) {
        g = &bbg[k];
        if (g->ser == A->serial && g->rtv == o->rtv && memcmp(g->D0, D0, sizeof g->D0) == 0) { g->used = ++bbg_clock; return g; }
    }
    for (k = 1; k < BBG_N; k++) { if (bbg[k].used < bbg[v].used) { v = k; } } /* aeltestes ersetzen */
    g = &bbg[v];
    if (g->cap < A->nops) {
        int *nn = (int *)realloc(g->next, A->nops * sizeof *nn);
        float *fx, *fy;
        if (!nn) { g->ser = 0; return NULL; }
        g->next = nn;
        fx = (float *)realloc(g->px, A->nops * sizeof *fx);
        if (!fx) { g->ser = 0; g->cap = 0; return NULL; }
        g->px = fx;
        fy = (float *)realloc(g->py, A->nops * sizeof *fy);
        if (!fy) { g->ser = 0; g->cap = 0; return NULL; }
        g->py = fy;
        {
            double (*pv)[5][4] = (double (*)[5][4])realloc(g->pv, A->nops * sizeof *pv);
            if (!pv) { g->ser = 0; g->cap = 0; return NULL; }
            g->pv = pv;
        }
        g->cap = A->nops;
    }
    memset(g->head, 0xFF, sizeof g->head);
    for (j = A->nops; j-- > 0;) {
        const ip_op_t *c = &A->ops[j];
        double nx, ny, p[4];
        unsigned h;
        g->next[j] = -1;
        if (c->kind != IP_DRAW || !c->cls || c->nv > 64u || c->rtv != o->rtv) { continue; }
        ipbb_prep((const float (*)[4])c->pref, c->mvp, g->pv[j], g->pv[j][4]);
        ipbb_row(g->pv[j][4], D0, p);
        if (p[3] <= 1e-6) { continue; }
        nx = p[0] / p[3]; ny = p[1] / p[3];
        if (fabs(nx) > 1e6 || fabs(ny) > 1e6) { continue; }
        g->px[j] = (float)nx; g->py[j] = (float)ny;
        h = bbg_key(c->sig, (int)floor(nx / BBG_CELL), (int)floor(ny / BBG_CELL));
        g->next[j] = g->head[h];
        g->head[h] = (int)j;
    }
    g->ser = A->serial; g->rtv = o->rtv; g->used = ++bbg_clock;
    st_bb_build++;
    memcpy(g->D0, D0, sizeof g->D0);
    return g;
}
static int ip_bb_pick_x(const ip_list_t *A, const ip_op_t *o, double D[8][16], int nd);
static int ip_bb_pick(const ip_list_t *A, const ip_op_t *o, double D[8][16], int nd)
{
    LONGLONG q0 = ip_now();
    int r = ip_bb_pick_x(A, o, D, nd);
    st_bb_ms += ip_ms(q0, ip_now());
    return r;
}
static int ip_bb_pick_x(const ip_list_t *A, const ip_op_t *o, double D[8][16], int nd)
{
    double best = 1e30, nx, ny, vb[4][4], cb[4];
    int j, x, bj = -1, cx, cy, gx, gy, nfull = 0;
    bbg_t *g;
    if (nd <= 0 || o->nv > 64u) { st_bb_no++; return -1; } /* nur kleine Draws (Billboards, Sprites) */
    ipbb_prep((const float (*)[4])o->pref, o->mvp, vb, cb);
    if (cb[3] <= 1e-6) { st_bb_no++; return -1; }
    nx = cb[0] / cb[3]; ny = cb[1] / cb[3];
    if (fabs(nx) > 1e6 || fabs(ny) > 1e6) { st_bb_no++; return -1; }
    g = bbg_build(A, o, D[0]);
    if (!g) { st_bb_no++; return -1; }
    cx = (int)floor((nx - 2.0 * IPBB_EC) / BBG_CELL); cy = (int)floor((ny - 2.0 * IPBB_EC) / BBG_CELL); /* nur Zellen im Vorpruef-Fenster */
    for (gy = cy; gy <= (int)floor((ny + 2.0 * IPBB_EC) / BBG_CELL) && nfull <= 24; gy++) {
        for (gx = cx; gx <= (int)floor((nx + 2.0 * IPBB_EC) / BBG_CELL) && nfull <= 24; gx++) {
            for (j = g->head[bbg_key(o->sig, gx, gy)]; j >= 0; j = g->next[j]) {
                const ip_op_t *c = &A->ops[j];
                double ec = 1e30, ev = 1e30;
                if (c->taken || c->sig != o->sig || c->nv != o->nv || c->dsv != o->dsv || c->topo != o->topo ||
                    c->ib1 != o->ib1) { continue; } /* ib1: Vertex-Lerp nur gleich abgelegt (W264) */
                /* Vorpruefung mit D[0] (Spielraum fuer die anderen Kamera-Kandidaten); dichte Partikelwolken: Deckel */
                if (fabs(g->px[j] - nx) > 2.0 * IPBB_EC || fabs(g->py[j] - ny) > 2.0 * IPBB_EC) { continue; }
                if (++nfull > 24) { break; }
                for (x = 0; x < nd; x++) {
                    double e2, e3;
                    ipbb_err2((const double (*)[4])g->pv[j], g->pv[j][4], (const double (*)[4])vb, cb, D[x], &e2, &e3);
                    if (e2 < ec) { ec = e2; ev = e3; }
                }
                if (!ipbb_ok(ec, ev)) { continue; } /* Schwerpunkt ruht nicht bzw. Ecken passen nicht -> anderes Objekt */
                if (ec < best || (ec == best && j < bj)) { best = ec; bj = j; } /* Gleichstand: Reihenfolge (deterministisch) */
            }
        }
    }
    if (bj < 0) { st_bb_far++; }
    return bj;
}
static unsigned st_rz_nm, st_rz_nmno, st_rz_nmcam, st_rz_n;
static double st_rz_ms, st_rz_max; /* W284: Zeit des rawzc-Blocks je Tick */
static unsigned st_rz_relw, st_rz_rel, st_rz_noofs, st_rz_cam, st_rz_nocam, st_rz_other, st_rz_ln; /* FIX(W284) interp_rawzc: Zaehler */

/* ---------- W317 INTERP-PAIR: robuste Zuordnung der Welt-Draws zwischen Tick N-1 und N ----------
 * Befund W295/W294: Die Paarung nach Reihenfolge (gleiche Signatur, andere Eingangsdaten: ip_pick) bzw. nach
 * rohem Bildsprung (interp_inst) vertauscht gleichartige Instanzen (Kacheln, neu gelegte Gitter, je Tick neu
 * erzeugte Boden-Quads); ohne Partner wird exakt (Tick N) zwischen gelerpten Nachbarn gezeichnet (W251-Mischung).
 * Hier (Bit 1): Kamerabewegung D = M_A^-1*M_B je Ziel aus der Mehrheit der Matrix-Paare (W294 Bit 1); fuer jeden
 * Welt-Draw o die Lage seiner 4 Bezugs-Vertices in N-1, WENN er in der Welt ruht: c_pred = p_B*M_B*D^-1. Kosten zu
 * einem Kandidaten a (gleiche Signatur und Ziel) = groesste Abweichung c_pred <-> p_A*M_A (Clip-Raum, je |w|
 * normiert). Ruhende Objekte haben beim richtigen Partner Kosten ~0, falsche Instanzen nicht - unabhaengig von
 * der Schwenkgeschwindigkeit. Zuordnung global gierig nach Kosten (sortiert, eindeutig): <= interp_pair_thr
 * immer (ersetzt auch eine bestehende Paarung), bis interp_pair_mov nur gegenseitig beste/eindeutige Wahl und nur
 * fuer bisher exakte Draws (bewegte Objekte). Ohne neuen Partner bleibt die alte Paarung (Ergebnis von W91/W251/
 * W284/W294); bisher exakte werden mit der Kamera bewegt (Matrix N-1 = M_B*D^-1 auf die Tick-N-Vertices, rzm)
 * statt exakt. Bit 2: Durchgaenge derselben Instanz folgen ihrem ersten Durchgang (gleiche Matrix N-1).
 * Zensus (interp_pair_diag): Endstand der alten Zuordnung je Klasse x Ergebnis.
 * Deterministisch: Kandidatenmengen haengen nicht von der Hash-Reihenfolge ab, Gleichstand nach Index. */
enum { PR_NC = 6, PR_NO = 7, PR_T = 32, PR_HB = 14, PR_CMAX = 24 }; /* PR_CMAX: Kandidaten je Draw (Deckel, Zellen mit vielen gleichartigen Draws) */
static const char *const kPR_CLS[PR_NC] = { "inst", "gitter", "quad", "vorab", "einzel", "ohne_kamera" };
static const char *const kPR_OUT[PR_NO] = { "ok", "falsch", "verpasst", "neu", "bewegt", "exakt", "statisch" };
static unsigned st_pz[PR_NC][PR_NO];
static unsigned st_pr_ticks, st_pr_set, st_pr_pair, st_pr_raw, st_pr_cam, st_pr_keep, st_pr_follow,
                st_pr_chg, st_pr_still, st_pr_ln, st_pr_lnd;
static double st_pr_ms, st_pr_max, st_pr_ms1, st_pr_ms2, st_pr_ms3; /* W317: Zeit gesamt bzw. Kamera+A, Kandidaten, Zuordnung */
typedef struct { ID3D11RenderTargetView *rtv; ID3D11DepthStencilView *dk; int ok, ref; unsigned nstat; double Di[16]; } pr_tg_t;
static pr_tg_t pr_tg[PR_T];
static int pr_ntg;
typedef struct { float c; int o, a; } pr_pair_t;
static float (*pr_pc)[4][4], (*pr_ac)[4][4], *pr_c1, *pr_c2, *pr_ac1, *pr_ac2;
static int *pr_b1, *pr_ab1, *pr_hn, *pr_tgo, *pr_old;
static unsigned *pr_hk;
static int (*pr_acl)[3]; /* Rasterzelle (x, y, vor der Kamera) je A-Draw */
static unsigned char *pr_in, *pr_at, *pr_orzb;
static unsigned pr_capb, pr_capa, pr_cpp;
static pr_pair_t *pr_pp;
static int pr_hh[1u << PR_HB];

static int pr_grow(void **p, unsigned n, size_t sz) { void *q = realloc(*p, (size_t)n * sz); if (!q) { return 0; } *p = q; return 1; }
static int pr_tgt(const ip_op_t *o)
{
    int x;
    ID3D11DepthStencilView *dk = rz_dkey(o);
    for (x = 0; x < pr_ntg; x++) { if (pr_tg[x].rtv == o->rtv && pr_tg[x].dk == dk) { return x; } }
    if (pr_ntg >= PR_T) { return -1; }
    memset(&pr_tg[pr_ntg], 0, sizeof pr_tg[0]);
    pr_tg[pr_ntg].rtv = o->rtv; pr_tg[pr_ntg].dk = dk; pr_tg[pr_ntg].ref = -1;
    return pr_ntg++;
}
/* vorab transformiert (Clip-w = 1 an allen Bezugs-Vertices) bzw. affine Bildraum-Matrix (P2) */
static int pr_pre(const ip_op_t *o)
{
    int q;
    if (ip_mvp_affine(o->mvp)) { return 1; }
    for (q = 0; q < 4; q++) { if (!(o->rn[q][2] > 0.999f && o->rn[q][2] < 1.001f)) { return 0; } }
    return 1;
}
static float pr_cost(const float pc[4][4], const float ac[4][4])
{
    int q;
    float m = 0.0f;
    for (q = 0; q < 4; q++) {
        float wa = ac[q][3], wp = pc[q][3], den, d;
        if ((wa > 1e-6f) != (wp > 1e-6f)) { return IP_COST_INF; } /* Augenebene gekreuzt */
        den = fmaxf(fabsf(wa), fabsf(wp));
        if (den < 1e-3f) { den = 1e-3f; }
        d = fmaxf(fabsf(pc[q][0] - ac[q][0]), fmaxf(fabsf(pc[q][1] - ac[q][1]), fabsf(pc[q][3] - ac[q][3]))) / den;
        if (d > m) { m = d; }
    }
    return m;
}
/* Raster der Lage von Bezugs-Vertex 0 (Zellen 0,25 NDC, ausserhalb +-8 geklemmt) */
static void pr_cell(const float c[4], int *cx, int *cy, int *s)
{
    float den = fmaxf(fabsf(c[3]), 1e-3f), x = c[0] / den, y = c[1] / den;
    x = fminf(fmaxf(x, -8.0f), 8.0f); y = fminf(fmaxf(y, -8.0f), 8.0f);
    *cx = (int)floorf(x * 4.0f); *cy = (int)floorf(y * 4.0f); *s = c[3] > 1e-6f;
}
static unsigned pr_hkey(unsigned sig, int cx, int cy, int s)
{
    unsigned h = sig * 2654435761u;
    h ^= (unsigned)(cx + 64) * 40503u; h = (h ^ (h >> 13)) * 16777619u;
    h ^= (unsigned)(cy + 64) * 9973u + (unsigned)s * 7919u;
    return (h ^ (h >> 15)) & ((1u << PR_HB) - 1u);
}
static int pr_pcmp(const void *x, const void *y)
{
    const pr_pair_t *p = (const pr_pair_t *)x, *q = (const pr_pair_t *)y;
    if (p->c != q->c) { return p->c < q->c ? -1 : 1; }
    if (p->o != q->o) { return p->o < q->o ? -1 : 1; }
    return p->a < q->a ? -1 : (p->a > q->a);
}
static void pr_push(unsigned *npp, float c, int o, int a)
{
    if (*npp >= pr_cpp) {
        unsigned nc = pr_cpp ? 2u * pr_cpp : 4096u;
        if (!pr_grow((void **)&pr_pp, nc, sizeof *pr_pp)) { return; }
        pr_cpp = nc;
    }
    pr_pp[*npp].c = c; pr_pp[*npp].o = o; pr_pp[*npp].a = a; (*npp)++;
}
static int pr_cand_a(const ip_op_t *a) { return a->kind == IP_DRAW && a->cls && !a->nolerp; }
static void pr_mul_di(float out[16], const float m[16], const double Di[16])
{
    int r, c;
    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            out[r * 4 + c] = (float)(m[r * 4 + 0] * Di[0 * 4 + c] + m[r * 4 + 1] * Di[1 * 4 + c] +
                                     m[r * 4 + 2] * Di[2 * 4 + c] + m[r * 4 + 3] * Di[3 * 4 + c]);
        }
    }
}
/* W410: D^-1 = M_B^-1 * M_A des haeufigsten gelerpten Matrix-Paars im Ziel von o (nach rz_cam_pre). 1 = ok. */
static int w410_cam_inv(const ip_list_t *A, const ip_list_t *B, const ip_op_t *o, double Di[16])
{
    int x;
    for (x = 0; x < rz_pn; x++) {
        if (rz_prt[x] == o->rtv && rz_pds[x] == rz_dkey(o) && rz_prr[x][0] >= 0) {
            const ip_op_t *r = &B->ops[rz_prr[x][0]];
            return zc_dmat(Di, r->mvp, A->ops[r->match].mvp);
        }
    }
    return 0;
}
/* FIX(W415 LOC-STATIC-LAG) loc_stat_fix: Kamera der Welt fuer "mit der Kamera bewegen" (rzm) in LoC.
 * Befund W415_R2d (Mission 02 Stadt, Drehen, 120 FPS): die W410-Kamera (haeufigstes gleiche Matrix-Paar unter den
 * GELERPTEN Draws, rz_cam_pre) ist dort ein fast ruhendes Paar (Tickweg der Bildmitte 0,0-0,4 px), denn die Geometrie in
 * Weltkoordinaten (Gelaende-Kacheln, grosses Bodennetz) wird je Tick neu gebaut (rawlerp/nomatch) und zaehlt nicht mit;
 * Gebaeude haben je eigene Weltmatrix. Die gelerpten Gebaeude und die Kamera der Weltkoordinaten-Draws (haeufigste
 * gleiche Matrix der Liste, wie la_cam_of) zeigen dagegen 7-12 px Tickweg. Folge: Gelaende-Kacheln (rzm) und das
 * grosse Bodennetz (nomatch, Vertexzahl wechselt je Tick -> exakt) standen in den Zwischenbildern still, die Gebaeude
 * glitten -> Gebaeude/Masten schwimmen gegen den Boden. Kamera hier: D^-1 = C_B^-1 * C_A der haeufigsten gleichen Matrix
 * im Weltziel beider Listen, bestaetigt durch >= 3 gelerpte bewegte Matrix-Draws mit demselben Tickweg (9 Probepunkte,
 * < 2e-4 NDC). Je Tick-Paar einmal berechnet. */
static ID3D11RenderTargetView *la_world_rtv(const ip_list_t *L, unsigned *split);
static struct {
    unsigned serA, serB;
    int ok, ia;
    ID3D11RenderTargetView *rtv;
    double Di[16];
    float camB[16];
} w415_wc;
static unsigned st_w415_rzm, st_w415_nm, st_w415_nocam, st_w415_cam, st_w415_rej; /* Zaehler (LOCINTERP stat415) */
static int w415_cam_find(const ip_list_t *L, ID3D11RenderTargetView **wt, int *idx, unsigned *cnt)
{
    enum { NC = 64 };
    unsigned cidx[NC], cn[NC], nc = 0, i, k, best = 0, sp = L->nops;
    *wt = la_world_rtv(L, &sp);
    if (!*wt) { return 0; }
    for (i = 0; i < sp; i++) {
        const ip_op_t *o = &L->ops[i];
        if (o->kind != IP_DRAW || o->rtv != *wt || o->nv < 3u || !o->cls || ip_mvp_affine(o->mvp)) { continue; }
        for (k = 0; k < nc; k++) { if (memcmp(L->ops[cidx[k]].mvp, o->mvp, 64) == 0) { cn[k]++; break; } }
        if (k == nc && nc < NC) { cidx[nc] = i; cn[nc] = 1; nc++; }
    }
    if (!nc) { return 0; }
    for (k = 1; k < nc; k++) { if (cn[k] > cn[best]) { best = k; } }
    *idx = (int)cidx[best];
    *cnt = cn[best];
    return 1;
}
/* groesste Abweichung (NDC) zweier Tick-Bewegungen D an 9 Probepunkten im Bild (Tiefe 0,99) */
static double w415_dev(const double X[16], const double Y[16])
{
    int px, py, j;
    double mx = 0.0;
    for (px = -1; px <= 1; px++) {
        for (py = -1; py <= 1; py++) {
            double c[4] = { 0.6 * px, 0.6 * py, 0.99, 1.0 }, a[4], b[4];
            for (j = 0; j < 4; j++) {
                a[j] = c[0] * X[0 * 4 + j] + c[1] * X[1 * 4 + j] + c[2] * X[2 * 4 + j] + c[3] * X[3 * 4 + j];
                b[j] = c[0] * Y[0 * 4 + j] + c[1] * Y[1 * 4 + j] + c[2] * Y[2 * 4 + j] + c[3] * Y[3 * 4 + j];
            }
            if (a[3] <= 1e-9 || b[3] <= 1e-9) { return 1e9; }
            mx = fmax(mx, fmax(fabs(a[0] / a[3] - b[0] / b[3]), fabs(a[1] / a[3] - b[1] / b[3])));
        }
    }
    return mx;
}
static int w415_world_cam(const ip_list_t *A, const ip_list_t *B, const ip_op_t *o, double Di[16], int *ia)
{
    if (!g_sba_loc || g_cfg.vr_enable || !(g_cfg.loc_stat_fix || (g_cfg.loc_stat_diag & 1))) { return 0; } /* Diagnose misst auch ohne Fix */
    if (w415_wc.serA != A->serial || w415_wc.serB != B->serial) {
        ID3D11RenderTargetView *wa = NULL, *wb = NULL;
        int xa = -1, xb = -1;
        unsigned ca = 0, cb = 0, i, nok = 0;
        w415_wc.serA = A->serial; w415_wc.serB = B->serial; w415_wc.ok = 0;
        if (w415_cam_find(A, &wa, &xa, &ca) && w415_cam_find(B, &wb, &xb, &cb) && wa == wb && ca >= 4u && cb >= 4u &&
            zc_dmat(w415_wc.Di, B->ops[xb].mvp, A->ops[xa].mvp)) {
            for (i = 0; i < B->nops && nok < 3u; i++) { /* Bestaetigung: gelerpte, bewegte Matrix-Draws mit demselben Tickweg */
                const ip_op_t *r = &B->ops[i];
                const ip_op_t *q = (r->match >= 0 && (unsigned)r->match < A->nops) ? &A->ops[r->match] : NULL;
                double Dx[16];
                if (r->kind != IP_DRAW || !r->cls || r->rtv != wb || !r->flag || !q || r->why != IP_W_LERP || r->rzb || r->rzm ||
                    ip_mvp_affine(r->mvp) || memcmp(q->mvp, r->mvp, sizeof r->mvp) == 0) { continue; }
                if (zc_dmat(Dx, r->mvp, q->mvp) && w415_dev(Dx, w415_wc.Di) < 2e-4) { nok++; }
            }
            if (nok >= 3u) {
                w415_wc.ok = 1; w415_wc.rtv = wb; w415_wc.ia = xa;
                memcpy(w415_wc.camB, B->ops[xb].mvp, sizeof w415_wc.camB);
                st_w415_cam++;
            } else { st_w415_rej++; }
        } else { st_w415_nocam++; }
    }
    if (!w415_wc.ok || o->rtv != w415_wc.rtv) { return 0; }
    memcpy(Di, w415_wc.Di, sizeof w415_wc.Di);
    if (ia) { *ia = w415_wc.ia; }
    return 1;
}
/* W415: Bezugs-Draw (Tick N) der W410-Kamera im Ziel von o (nach rz_cam_pre), NULL = keiner */
static const ip_op_t *w415_cam_ref(const ip_list_t *B, const ip_op_t *o)
{
    int x;
    for (x = 0; x < rz_pn; x++) {
        if (rz_prt[x] == o->rtv && rz_pds[x] == rz_dkey(o) && rz_prr[x][0] >= 0) { return &B->ops[rz_prr[x][0]]; }
    }
    return NULL;
}
static const char *pr_why(unsigned w)
{
    return w == IP_W_LERP ? "lerp" : w == IP_W_RAWLERP ? "rawlerp" : w == IP_W_NOMATCH ? "nomatch" :
           w == IP_W_RAWCHG ? "rawchg" : w == IP_W_REJECT ? "reject" : w == IP_W_STATIC ? "static" : "?";
}

static void ip_pair_robust(ip_list_t *A, ip_list_t *B)
{
    const int on = g_cfg.interp_pair & 1, diag = g_cfg.interp_pair_diag > 0;
    const float thr = g_cfg.interp_pair_thr, mov = g_cfg.interp_pair_mov;
    unsigned i, j, npp = 0;
    int x;
    LONGLONG q0 = ip_now(), q1;
    if (g_cfg.vr_enable || g_cfg.interp_mode != 0 || !(g_cfg.interp_streak & 1)) { return; }
    if (pr_capb < B->nops) {
        if (!pr_grow((void **)&pr_pc, B->nops, sizeof *pr_pc) || !pr_grow((void **)&pr_c1, B->nops, sizeof *pr_c1) ||
            !pr_grow((void **)&pr_c2, B->nops, sizeof *pr_c2) || !pr_grow((void **)&pr_b1, B->nops, sizeof *pr_b1) ||
            !pr_grow((void **)&pr_in, B->nops, sizeof *pr_in) || !pr_grow((void **)&pr_tgo, B->nops, sizeof *pr_tgo) ||
            !pr_grow((void **)&pr_old, B->nops, sizeof *pr_old) || !pr_grow((void **)&pr_orzb, B->nops, sizeof *pr_orzb)) { return; }
        pr_capb = B->nops;
    }
    if (pr_capa < A->nops) {
        if (!pr_grow((void **)&pr_ac, A->nops, sizeof *pr_ac) || !pr_grow((void **)&pr_ac1, A->nops, sizeof *pr_ac1) ||
            !pr_grow((void **)&pr_ac2, A->nops, sizeof *pr_ac2) || !pr_grow((void **)&pr_ab1, A->nops, sizeof *pr_ab1) ||
            !pr_grow((void **)&pr_hn, A->nops, sizeof *pr_hn) || !pr_grow((void **)&pr_hk, A->nops, sizeof *pr_hk) ||
            !pr_grow((void **)&pr_at, A->nops, sizeof *pr_at) || !pr_grow((void **)&pr_acl, A->nops, sizeof *pr_acl)) { return; }
        pr_capa = A->nops;
    }
    /* 1. Kamera je Ziel: haeufigstes Matrix-Paar gelerpter 3D-Draws (W294 Bit 1); steht die Kamera (mehr ruhende
     *    als bewegte Welt-Draws), D = I. */
    rz_cam_pre(A, B);
    pr_ntg = 0;
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        pr_tgo[i] = -1;
        if (o->kind != IP_DRAW || !o->cls || o->nolerp) { continue; }
        pr_tgo[i] = pr_tgt(o);
        if (pr_tgo[i] >= 0 && o->why == IP_W_STATIC && !pr_pre(o)) { pr_tg[pr_tgo[i]].nstat++; }
    }
    for (x = 0; x < pr_ntg; x++) {
        pr_tg_t *g = &pr_tg[x];
        int t, k;
        unsigned cnt = 0;
        for (t = 0; t < rz_pn; t++) { if (rz_prt[t] == g->rtv && rz_pds[t] == g->dk) { break; } }
        if (t < rz_pn && rz_prr[t][0] >= 0) { cnt = rz_prc[t][0]; }
        if (cnt >= 4u && cnt >= g->nstat) {
            const ip_op_t *r = &B->ops[rz_prr[t][0]];
            if (zc_dmat(g->Di, r->mvp, A->ops[r->match].mvp)) { g->ok = 1; g->ref = r->match; } /* D^-1 = M_B^-1*M_A */
        } else if (g->nstat >= 8u) {
            for (k = 0; k < 16; k++) { g->Di[k] = (k % 5) == 0 ? 1.0 : 0.0; }
            for (i = 0; i < B->nops && g->ref < 0; i++) { /* Bezug fuer match: ein ruhender Draw des Ziels */
                const ip_op_t *o = &B->ops[i];
                if (pr_tgo[i] == x && o->why == IP_W_STATIC && o->match >= 0 && (unsigned)o->match < A->nops) { g->ref = o->match; }
            }
            g->ok = g->ref >= 0;
        }
    }
    /* 2. Lagen: A tatsaechlich (N-1), Raster ueber A */
    memset(pr_hh, 0xFF, sizeof pr_hh);
    for (j = A->nops; j-- > 0;) {
        const ip_op_t *a = &A->ops[j];
        int q, cx, cy, s;
        pr_at[j] = 0; pr_ac1[j] = pr_ac2[j] = IP_COST_INF; pr_ab1[j] = -1;
        if (!pr_cand_a(a) || pr_pre(a)) { continue; }
        for (q = 0; q < 4; q++) { /* Clip x/y/w aus den beim Aufzeichnen berechneten NDC (z geht nicht ein) */
            float w = a->rn[q][2];
            if (w > 1e-6f) { pr_ac[j][q][0] = a->rn[q][0] * w; pr_ac[j][q][1] = a->rn[q][1] * w; pr_ac[j][q][2] = 0.0f; pr_ac[j][q][3] = w; }
            else { ip_clip(a->pref[q], a->mvp, pr_ac[j][q]); }
        }
        pr_cell(pr_ac[j][0], &cx, &cy, &s);
        pr_acl[j][0] = cx; pr_acl[j][1] = cy; pr_acl[j][2] = s;
        pr_hk[j] = pr_hkey(a->sig, cx, cy, s);
        pr_hn[j] = pr_hh[pr_hk[j]];
        pr_hh[pr_hk[j]] = (int)j;
    }
    q1 = ip_now();
    st_pr_ms1 += ip_ms(q0, q1);
    /* Menge: Welt-3D-Draws mit Kamera (ohne P2/vorab, Effekte, statisch, Geometrie-Partner W73). Reserviert bleiben
     * die Partner aller anderen Draws. */
    for (i = 0; i < B->nops; i++) {
        ip_op_t *o = &B->ops[i];
        int in;
        pr_in[i] = 0; pr_old[i] = -1; pr_b1[i] = -1; pr_c1[i] = pr_c2[i] = IP_COST_INF;
        if (o->kind != IP_DRAW) { continue; }
        in = o->cls && !o->nolerp && !o->fxk && pr_tgo[i] >= 0 && pr_tg[pr_tgo[i]].ok && !pr_pre(o) && o->why != IP_W_STATIC &&
             !(o->why == IP_W_LERP && o->match >= 0 && A->ops[o->match].rawhash != o->rawhash);
        if (in) {
            pr_in[i] = 1;
            pr_orzb[i] = (unsigned char)(o->rzb != 0);
            if (o->flag && o->match >= 0) { pr_old[i] = o->match; }
        } else if (o->match >= 0 && (unsigned)o->match < A->nops && o->why != IP_W_RAWCHG && o->why != IP_W_NOMATCH) {
            pr_at[o->match] = 1;
        }
    }
    /* 3. Kandidaten (9 Rasterzellen um die vorhergesagte Lage) und Kosten */
    for (i = 0; i < B->nops; i++) {
        ip_op_t *o = &B->ops[i];
        float Mp[16];
        int q, cx, cy, s, dx, dy, ncand = 0, nsame = 0;
        if (!pr_in[i] && !(diag && o->kind == IP_DRAW && o->cls && !o->nolerp && pr_tgo[i] >= 0 && pr_tg[pr_tgo[i]].ok && !pr_pre(o))) { continue; }
        pr_mul_di(Mp, o->mvp, pr_tg[pr_tgo[i]].Di);
        for (q = 0; q < 4; q++) { ip_clip(o->pref[q], Mp, pr_pc[i][q]); }
        pr_cell(pr_pc[i][0], &cx, &cy, &s);
        for (dy = -1; dy <= 1 && ncand < PR_CMAX; dy++) {
            for (dx = -1; dx <= 1 && ncand < PR_CMAX; dx++) {
                unsigned h = pr_hkey(o->sig, cx + dx, cy + dy, s);
                int jj;
                for (jj = pr_hh[h]; jj >= 0 && ncand < PR_CMAX; jj = pr_hn[jj]) {
                    const ip_op_t *a;
                    float c;
                    if (pr_acl[jj][0] != cx + dx || pr_acl[jj][1] != cy + dy || pr_acl[jj][2] != s) { continue; } /* Hash-Kollision */
                    a = &A->ops[jj];
                    if (a->sig != o->sig || a->rtv != o->rtv || a->nv != o->nv || a->topo != o->topo) { continue; }
                    ncand++;
                    if (a->rawhash == o->rawhash) { nsame++; }
                    c = pr_cost(pr_pc[i], pr_ac[jj]);
                    if (c < pr_c1[i] || (c == pr_c1[i] && jj < pr_b1[i])) { pr_c2[i] = pr_c1[i]; pr_c1[i] = c; pr_b1[i] = jj; }
                    else if (c < pr_c2[i]) { pr_c2[i] = c; }
                    if (!pr_in[i] || pr_at[jj] || c > mov) { continue; }
                    if (c < pr_ac1[jj] || (c == pr_ac1[jj] && (int)i < pr_ab1[jj])) { pr_ac2[jj] = pr_ac1[jj]; pr_ac1[jj] = c; pr_ab1[jj] = (int)i; }
                    else if (c < pr_ac2[jj]) { pr_ac2[jj] = c; }
                    if (c <= thr) { pr_push(&npp, c, (int)i, jj); } /* Welt-Partner; bewegte nur als bester (unten) */
                }
            }
        }
        if (pr_in[i] && pr_old[i] < 0 && pr_c1[i] > thr && pr_c1[i] <= mov && pr_b1[i] >= 0 && !pr_at[pr_b1[i]]) { /* bewegt: nur fuer bisher exakte */
            pr_push(&npp, pr_c1[i], (int)i, pr_b1[i]);
        }
        if (diag) { /* Zensus: Klasse und Ergebnis der bisherigen Zuordnung */
            int cl, out;
            const ip_op_t *m = (o->match >= 0 && (unsigned)o->match < A->nops) ? &A->ops[o->match] : NULL;
            float cm = IP_COST_INF, dd[3];
            if (nsame >= 2) { cl = 0; }
            else if (m && m->rawhash != o->rawhash && m->nv == o->nv && pr_cand_a(m) && !pr_pre(m) && rz_offset_grid(m, o, dd)) { cl = 1; }
            else if (o->nv <= 12u && !nsame) { cl = 2; }
            else { cl = 4; }
            if (m && o->flag && pr_cand_a(m) && !pr_pre(m)) {
                float mc[4][4];
                const ip_op_t *mm = o->rzb ? o : m; /* rzb: Tick-N-Vertices mit Matrix N-1 */
                for (q = 0; q < 4; q++) { ip_clip(mm->pref[q], o->rzb ? IP_AMVP(o, m) : m->mvp, mc[q]); }
                cm = pr_cost(pr_pc[i], mc);
            }
            if (o->why == IP_W_STATIC) { out = 6; }
            else if (o->flag) { out = cm <= thr ? 0 : (pr_c1[i] <= thr ? 1 : 4); }
            else { out = pr_c1[i] <= thr ? 2 : (!ncand ? 3 : 5); }
            st_pz[cl][out]++;
            if (g_cfg.interp_pair_diag >= 2 && out != 0 && out != 6 && st_pr_ln < 400u && (g_cfg.pf_ring <= 0 || sb_pf_active())) {
                st_pr_ln++;
                HLOG_I("INTERP pzd f=%u i=%u n=%u why=%s rzb=%d klasse=%s ergebnis=%s m=%d kosten_alt=%.4f best=%d kosten=%.4f zweit=%.4f kand=%d gleich=%d",
                       B->frame, i, o->nv, pr_why(o->why), o->rzb, kPR_CLS[cl], kPR_OUT[out], o->match, (double)cm,
                       pr_b1[i], (double)pr_c1[i], (double)pr_c2[i], ncand, nsame);
            }
        }
    }
    for (i = 0; diag && i < B->nops; i++) { /* Zensus: vorab transformierte und Ziele ohne Kamera nach altem Ergebnis */
        const ip_op_t *o = &B->ops[i];
        int cl, out;
        if (o->kind != IP_DRAW || !o->cls || o->nolerp || pr_tgo[i] < 0) { continue; }
        if (pr_pre(o)) { cl = 3; } else if (!pr_tg[pr_tgo[i]].ok) { cl = 5; } else { continue; }
        out = o->why == IP_W_STATIC ? 6 : (o->flag ? 0 : 5);
        st_pz[cl][out]++;
    }
    st_pr_ms2 += ip_ms(q1, ip_now());
    q1 = ip_now();
    if (!on) { goto done; }
    /* 4. global gierig nach Kosten */
    if (npp > 1u) { qsort(pr_pp, npp, sizeof *pr_pp, pr_pcmp); }
    for (i = 0; i < B->nops; i++) { if (pr_in[i]) { B->ops[i].match = -1; } }
    for (j = 0; j < npp; j++) {
        const pr_pair_t *p = &pr_pp[j];
        ip_op_t *o = &B->ops[p->o];
        const ip_op_t *a = &A->ops[p->a];
        if (o->match >= 0 || pr_at[p->a]) { continue; }
        if (p->c > thr) { /* bewegter Partner: nur gegenseitig beste und eindeutige Wahl */
            float lim = 2.0f * p->c + 0.01f;
            if (pr_b1[p->o] != p->a || pr_ab1[p->a] != p->o || pr_c2[p->o] < lim || pr_ac2[p->a] < lim) { continue; }
            if (!ip_plausible(a, o, g_cfg.interp_maxjump * (a->rawhash == o->rawhash ? 1.0f : 0.3f))) { continue; }
        }
        if (a->rawhash != o->rawhash && a->ib1 != o->ib1) { continue; } /* Vertex-Lerp nur gleich abgelegt (W264) */
        o->match = p->a;
        pr_at[p->a] = 1;
    }
    /* 5. Ergebnis je Draw */
    for (i = 0; i < B->nops; i++) {
        ip_op_t *o = &B->ops[i];
        unsigned char oldwhy;
        if (!pr_in[i]) { continue; }
        st_pr_set++;
        oldwhy = o->why;
        o->rzb = 0; o->rzm = 0;
        if (o->match >= 0) {
            const ip_op_t *a = &A->ops[o->match];
            if (a->rawhash == o->rawhash) {
                if ((a->outhash == o->outhash && memcmp(a->mvp, o->mvp, sizeof o->mvp) == 0) || ip_still(a, o)) {
                    o->flag = 0; o->why = IP_W_STATIC; st_pr_still++;
                } else {
                    o->flag = 1; o->why = IP_W_LERP; st_pr_pair++;
                    if (a->ib1 != o->ib1) { o->rzb = 1; } /* gleiche Daten, anders abgelegt: Tick-N-Vertices mit Matrix N-1 (W264-Pruefung) */
                }
            } else { o->flag = 1; o->why = IP_W_RAWLERP; st_pr_raw++; }
            if (pr_old[i] != o->match) { st_pr_chg++; }
            continue;
        }
        if (pr_old[i] >= 0) { /* alte Paarung bleibt (bewegtes Objekt; Partner ggf. mit einem weltkonsistenten Draw geteilt) */
            o->match = pr_old[i];
            pr_at[pr_old[i]] = 1;
            o->flag = 1;
            o->rzb = pr_orzb[i]; /* W284/W294: Tick-N-Vertices mit Matrix N-1 des alten Partners */
            o->why = oldwhy;
            st_pr_keep++;
            continue;
        }
        /* mit der Kamera bewegen: Matrix N-1 = M_B * D^-1 auf die Tick-N-Vertices */
        pr_mul_di(o->mva, o->mvp, pr_tg[pr_tgo[i]].Di);
        o->match = pr_tg[pr_tgo[i]].ref;
        o->rzm = 1; o->rzb = 1; o->flag = 1; o->why = IP_W_LERP;
        st_pr_cam++;
        if (oldwhy == IP_W_LERP || oldwhy == IP_W_RAWLERP) { st_pr_chg++; }
    }
    for (i = 0; g_cfg.interp_pair_diag >= 2 && i < B->nops; i++) { /* DIAG: geaenderte Entscheidungen */
        const ip_op_t *o = &B->ops[i];
        if (!pr_in[i] || st_pr_lnd >= 2000u || (g_cfg.pf_ring > 0 && !sb_pf_active())) { continue; }
        if (o->match == pr_old[i] && !o->rzm) { continue; }
        st_pr_lnd++;
        HLOG_I("INTERP prd f=%u i=%u n=%u alt=%d neu=%s/%d kamera=%d kosten=%.4f zweit=%.4f", B->frame, i, o->nv, pr_old[i], pr_why(o->why), o->match,
               o->rzm, (double)pr_c1[i], (double)pr_c2[i]);
    }
    /* 6. Bit 2: Durchgaenge derselben Instanz (Ziel, Geometrie, Matrix) folgen dem ersten Durchgang */
    if (g_cfg.interp_pair & 2) {
        static int gh[1u << IP_HBITS];
        memset(gh, 0xFF, sizeof gh);
        for (i = 0; i < B->nops; i++) { /* pr_old wird hier Hash-Kette */
            ip_op_t *o = &B->ops[i];
            const ip_op_t *L, *la;
            const float *Mprev;
            unsigned h;
            int k, lead = -1;
            if (!pr_in[i]) { continue; } /* Gruppen innerhalb der Menge (Leiter = erster Durchgang darin) */
            h = IP_HB(ip_mvpkey(o) ^ (o->gkey << 4));
            for (k = gh[h]; k >= 0; k = pr_old[k]) {
                const ip_op_t *e = &B->ops[k];
                if (e->gkey == o->gkey && e->rtv == o->rtv && e->rawhash == o->rawhash && e->nv == o->nv &&
                    memcmp(e->mvp, o->mvp, sizeof o->mvp) == 0) { lead = k; break; }
            }
            if (lead < 0) { pr_old[i] = gh[h]; gh[h] = (int)i; continue; }
            L = &B->ops[lead];
            if (!L->flag || L->match < 0 || (unsigned)L->match >= A->nops) { continue; }
            la = &A->ops[L->match];
            if (!L->rzb && la->rawhash != L->rawhash) { continue; } /* Leiter mit Vertex-Lerp: keine gemeinsame Matrix */
            Mprev = IP_AMVP(L, la);
            if (o->flag && o->match >= 0 && !o->rzb && A->ops[o->match].rawhash == o->rawhash &&
                memcmp(A->ops[o->match].mvp, Mprev, sizeof o->mvp) == 0) { continue; } /* schon gleich */
            memcpy(o->mva, Mprev, sizeof o->mva);
            o->match = L->match;
            o->rzm = 1; o->rzb = 1; o->flag = 1; o->why = IP_W_LERP;
            st_pr_follow++;
        }
    }
    st_pr_ms3 += ip_ms(q1, ip_now());
done:
    st_pr_ticks++;
    {
        double ms = ip_ms(q0, ip_now());
        st_pr_ms += ms;
        if (ms > st_pr_max) { st_pr_max = ms; }
    }
}

/* DIAG(W340 LOC-FRAMES, interp_diag>=1, nur LoC): Gruende der unplausiblen Zuordnungen (reject) */
static unsigned li_rj_w, li_rj_j[4], li_rj_big, li_rj_n, li_rj_cam;
static unsigned li_rz_alt, li_rz_vl, li_rj_cam2; /* W349: rawchg mit anderem Kamera-passenden Partner gleicher Signatur (LOCINTERP stat rzalt) */
static void li_rej_note(const ip_op_t *a, const ip_op_t *o)
{
    int q, wx = 0;
    float mj = 0.0f;
    for (q = 0; q < 4; q++) {
        float c0[4], c1[4];
        ip_clip(a->pref[q], a->mvp, c0);
        ip_clip(o->pref[q], o->mvp, c1);
        if (c0[3] > 1e-6f && c1[3] > 1e-6f) {
            float dx = fabsf(c1[0] / c1[3] - c0[0] / c0[3]), dy = fabsf(c1[1] / c1[3] - c0[1] / c0[3]);
            if (dx > mj) { mj = dx; }
            if (dy > mj) { mj = dy; }
        } else if (!(c0[3] < -1e-6f && c1[3] < -1e-6f)) {
            wx = 1;
        }
    }
    li_rj_n++;
    if (o->nv >= 1000u) { li_rj_big++; }
    if (wx) { li_rj_w++; }
    else { li_rj_j[mj < 0.75f ? 0 : mj < 1.0f ? 1 : mj < 2.0f ? 2 : 3]++; }
}

/* FIX(W429 M3-ROAD-FIX) interp_p2hom, zweite Stufe (nach der Zuordnung, vor interp_rawzc/interp_pass): exakte Welt-Draws
 * mit gleichen Eingangsdaten wie ein Vortick-Draw.
 * - reject (Partner gleicher Daten, an ip_plausible und ip_rej_relax gescheitert): Die 4 Bezugs-Vertices des grossen
 *   Bodenstuecks unter dem VT liegen oft alle ausserhalb des Bildes bzw. hinter der Kamera (M3: ~1300-3600 je 300 Ticks).
 * - nomatch mit Geometrie-Partner (gleiche Daten, Vertexzahl, Ziel; andere VS-Signatur) - so faellt der 3D-Pass des
 *   Bodenstuecks oft aus, waehrend der P2-Pass gelerpt wird (bis ~10 px Versatz). Nur ein freier Partner.
 * Ruht der Draw in der Welt, gilt fuer seine Clip-Lagen c_B ~ c_A * D mit der Kamerabewegung D = M_A^-1 * M_B des Ziels
 * (rz_cam, 3D-Matrix-Paare wie W251/W284); geprueft an allen 4 Bezugs-Vertices in x, y, w (auch hinter der Kamera; P2:
 * Clip-Lage aus Bildlage und echter w). Relative Abweichung <= 0,05 -> lerpen (Vertex i <-> i mit den eigenen
 * Matrizen bzw. homogen; nomatch <= 0,02). Die Bodenstuecke weichen ~0,4 % vom Kamera-D ab (gleich fuer P2- und 3D-Pass, daher bleiben
 * W251/W284 mit 1e-3 dort exakt); falsche Instanzen, Schnitte und Spruenge liegen bei O(1). reject zaehlt weiter als
 * reject (Schnitt-Erkennung unveraendert). */
#define P2HC_TOL 0.05f    /* reject: Partner aus der Instanz-Zuordnung */
#define P2HC_TOL_NM 0.02f /* nomatch: Geometrie-Partner nur nach Lage gewaehlt -> strenger */
static unsigned st_p2c[5]; /* gelerpt reject 3D, reject P2, nomatch 3D, nomatch P2, Kamera passt nicht */
static unsigned st_p2e[6]; /* abgelehnt nach Fehler: <0,1 / <0,2 / <0,5 / <1 / >=1 / keine Kamera; DIAG-Verteilung */
static unsigned st_p2ok[4]; /* angenommen nach Fehler: <1e-3 / <5e-3 / <2e-2 / <=0,05 */
static float ip_p2hom_err(const ip_op_t *a, const ip_op_t *o, int p2, const double D[8][16], int nD)
{
    int x, q, j;
    float best = 1e30f;
    for (x = 0; x < nD; x++) {
        float emax = 0.0f;
        for (q = 0; q < 4 && emax <= best; q++) {
            float c0[4], c1[4];
            double p[4], dx, dy, dw, nb, e;
            ip_clip(a->pref[q], a->mvp, c0);
            ip_clip(o->pref[q], o->mvp, c1);
            if (p2) { /* Strom schon durch w geteilt (w = 1): Clip-Lage = Bildlage * echte w */
                for (j = 0; j < 3; j++) { c0[j] *= a->prefw[q]; c1[j] *= o->prefw[q]; }
                c0[3] = a->prefw[q]; c1[3] = o->prefw[q];
            }
            for (j = 0; j < 4; j++) {
                p[j] = c0[0] * D[x][0 * 4 + j] + c0[1] * D[x][1 * 4 + j] + c0[2] * D[x][2 * 4 + j] + c0[3] * D[x][3 * 4 + j];
            }
            dx = p[0] - c1[0]; dy = p[1] - c1[1]; dw = p[3] - c1[3];
            nb = fabs(c1[0]) + fabs(c1[1]) + fabs(c1[3]) + fabs(p[0]) + fabs(p[1]) + fabs(p[3]);
            e = nb > 1e-9 ? 2.0 * (fabs(dx) + fabs(dy) + fabs(dw)) / nb : 1.0;
            if ((float)e > emax) { emax = (float)e; }
        }
        if (emax < best) { best = emax; }
    }
    return best;
}
static void ip_p2hom_cam(ip_list_t *A, ip_list_t *B)
{
    unsigned i;
    int pre = 0;
    for (i = 0; i < B->nops; i++) {
        ip_op_t *o = &B->ops[i];
        ip_op_t *a;
        double D[8][16];
        int nD, p2, g = -1, nm;
        float best;
        if (o->kind != IP_DRAW || !o->cls || o->flag) { continue; }
        nm = o->why == IP_W_NOMATCH;
        if (nm) {
            if (o->match >= 0) { continue; }
            g = ip_pick_geo(A, o, 0);
            if (g < 0 || A->ops[g].taken) { continue; }
            a = &A->ops[g];
        } else {
            if (o->why != IP_W_REJECT || o->match < 0 || (unsigned)o->match >= A->nops) { continue; }
            a = &A->ops[o->match];
        }
        p2 = ip_mvp_affine(o->mvp);
        if (a->kind != IP_DRAW || !a->cls || a->rawhash != o->rawhash || a->nv != o->nv || p2 != ip_mvp_affine(a->mvp) ||
            a->rtv != o->rtv || a->ib1 != o->ib1) { continue; }
        if (p2 && (!a->p2w || !o->p2w || !A->wb_ok || !B->wb_ok || sb_ip_hlerp() < 2)) { continue; }
        if (!pre) { rz_cam_pre(A, B); pre = 1; }
        nD = rz_cam(A, B, o, D);
        best = nD ? ip_p2hom_err(a, o, p2, D, nD) : 1e30f;
        if (best <= (nm ? P2HC_TOL_NM : P2HC_TOL)) {
            if (nm) { o->match = g; a->taken = 1; }
            o->flag = 1; o->why = IP_W_LERP; /* reject-Zaehler bleibt (Schnitt-Erkennung) */
            st_p2c[(nm ? 2 : 0) + (p2 ? 1 : 0)]++;
            st_p2ok[best < 1e-3f ? 0 : best < 5e-3f ? 1 : best < 2e-2f ? 2 : 3]++;
        } else {
            static unsigned nl;
            st_p2c[4]++;
            st_p2e[!nD ? 5 : best < 0.1f ? 0 : best < 0.2f ? 1 : best < 0.5f ? 2 : best < 1.0f ? 3 : 4]++;
            if (g_cfg.seam_diag > 0 && nl < (unsigned)g_cfg.seam_diag && (g_cfg.pf_ring <= 0 || sb_pf_active())) { /* W429 DIAG */
                nl++;
                HLOG_I("P2HC f=%u i=%u n=%u p2=%d %s kameras=%d fehler=%.3g", B->frame, i, o->nv, p2, nm ? "nomatch" : "reject", nD, (double)best);
            }
        }
    }
}

static int ip_match(ip_list_t *A, ip_list_t *B, int log)
{
    unsigned i, jp = 0, ndraw = 0, nclr = 0, nworld = 0, nmatch = 0, nlerp = 0,
             nstatic = 0, nnomatch = 0, nrawchg = 0, nreject = 0, n2d = 0, inorder = 0,
             nrawlerp = 0, nrt2d = 0, ngeo = 0;
    unsigned bbdraws = 0;
    for (i = 0; i < A->nops; i++) { A->ops[i].taken = 0; }
    if (!ip_hash_build(A)) { return 0; }
    if (g_cfg.interp_inst) { /* POPIN (W91): ip_inst_closer prueft noch offene Draws von B */
        for (i = 0; i < B->nops; i++) { B->ops[i].match = -1; B->ops[i].why = IP_W_NONE; }
        if (!ip_bhash_build(B)) { return 0; }
    }
    for (i = 0; i < B->nops; i++) {
        ip_op_t *o = &B->ops[i];
        int found = -1, sigonly = -1, iwbest = -1;
        o->match = -1;
        o->flag = 0;
        o->rzb = 0; /* W284 */
        o->rzm = 0; /* W317 */
        o->why = IP_W_NONE;
        if (o->kind != IP_DRAW) { nclr++; continue; }
        ndraw++;
        if (o->rtv == g_rtv) { bbdraws++; }
        if (o->nolerp) { n2d++; o->why = IP_W_RT2DEX; continue; } /* MONITOR-FX (W105): Korn/Stoerung wechselt im Spieltakt, exakt aus Tick N */
        if (!o->cls && o->rtv == g_rtv) { n2d++; o->why = IP_W_BB2D; continue; } /* HUD/2D im BB: immer exakt */
        if (!o->cls) {
            /* 2D/RHW-Draw in ein RT (vom Spiel vorprojizierte Welt- oder
             * Monitor-Inhalte): nur Vertex-Zuordnung mit strenger Grenze;
             * unveraendert -> exakt. */
            int s2;
            n2d++;
            o->why = IP_W_RT2DEX;
            s2 = ip_pick(A, o, jp, 1);
            if (s2 < 0) { s2 = ip_pick(A, o, jp, -1); }
            if (s2 >= 0) {
                ip_op_t *a = &A->ops[s2];
                a->taken = 1;
                jp = (unsigned)s2 + 1u;
                o->match = s2;
                if (!(a->outhash == o->outhash && memcmp(a->mvp, o->mvp, sizeof o->mvp) == 0) &&
                    ip_plausible(a, o, g_cfg.interp_maxjump * 0.3f)) {
                    o->flag = 1;
                    o->why = IP_W_RT2DLERP;
                    nrt2d++;
                }
            }
            continue;
        }
        nworld++;
        if (g_cfg.interp_inst) {
            int amb = 0;
            found = ip_pick_inst(A, B, i, &amb);
            if (amb) { /* mehrere gleichwertige Instanzen, keine eindeutig -> exakt */
                st_iamb++;
                nnomatch++;
                o->why = IP_W_NOMATCH;
                continue;
            }
        } else {
            found = ip_pick(A, o, jp, 1);
            if (found >= 0 && g_cfg.interp_diag >= 1) { /* DIAG(POPIN,W91): Reihenfolge-Wahl vs. naechste Instanz */
                int n, second, best;
                float c1, c2, cf = ip_cost(&A->ops[found], o);
                best = ip_inst_best(A, o, &n, &c1, &c2, &second);
                if (n > 1) { st_imulti++; }
                if (best >= 0 && best != found && cf > 2.0f * c1 + 0.02f) { st_iwrong++; iwbest = best; }
            }
        }
        if (found < 0) { sigonly = ip_pick(A, o, jp, 0); }
        if (found < 0 && sigonly >= 0) {
            /* Gleiche Signatur, neue Eingangs-Positionen (z. B. kamera-relativ
             * neu gebackenes Gelaende, CPU-animiert): Vertex i <-> Vertex i,
             * nur bei kleinem Sprung aller 4 Bezugs-Vertices (strenger als
             * bei Matrix-Draws, schuetzt vor vertauschten Partikeln). */
            ip_op_t *a = &A->ops[sigonly];
            if (ip_plausible(a, o, g_cfg.interp_maxjump * ((o->fxk && g_cfg.fx_interp) ? 1.0f : 0.3f))) { /* W133 fx_interp */
                a->taken = 1;
                jp = (unsigned)sigonly + 1u;
                o->match = sigonly;
                o->flag = 1;
                o->why = IP_W_RAWLERP;
                nmatch++;
                nlerp++;
                nrawlerp++;
                continue;
            }
        }
        if (found < 0) {
            int g = ip_pick_geo(A, o, jp);
            if (g >= 0 && ip_plausible(&A->ops[g], o, g_cfg.interp_maxjump * ((o->fxk && g_cfg.fx_interp) ? 1.0f : 0.3f))) { /* W133 fx_interp */
                A->ops[g].taken = 1;
                o->match = g;
                nmatch++;
                if (memcmp(A->ops[g].mvp, o->mvp, sizeof o->mvp) == 0) {
                    nstatic++;
                    o->why = IP_W_STATIC;
                } else {
                    o->flag = 1;
                    o->why = IP_W_LERP;
                    nlerp++;
                    ngeo++;
                }
                continue;
            }
            if (sigonly >= 0) { nrawchg++; o->why = IP_W_RAWCHG; o->match = sigonly; }
            else { nnomatch++; o->why = IP_W_NOMATCH; }
            continue;
        }
        {
            ip_op_t *a = &A->ops[found];
            if ((unsigned)found == jp) { inorder++; }
            a->taken = 1;
            jp = (unsigned)found + 1u;
            o->match = found;
            nmatch++;
            if (a->outhash == o->outhash && memcmp(a->mvp, o->mvp, sizeof o->mvp) == 0) {
                nstatic++;
                o->why = IP_W_STATIC;
                continue;
            }
            if (ip_still(a, o)) { nstatic++; o->why = IP_W_STATIC; }
            else if (w448_eye_blind(a, o)) { nnomatch++; o->why = IP_W_NOMATCH; st_w448_eye++; } /* FIX(W448): exakt */
            else if (ip_plausible(a, o, g_cfg.interp_maxjump)) { o->flag = 1; nlerp++; o->why = IP_W_LERP; }
            else if (g_cfg.interp_p2rej && ip_p2rej_ok(a, o, g_cfg.interp_maxjump)) { o->flag = 1; nlerp++; o->why = IP_W_LERP; st_p2rej++; } /* W406 */
            else if (g_cfg.interp_p2hom && !g_sba_loc && !g_cfg.vr_enable && ip_rej_relax(A, B, a, o, g_cfg.interp_maxjump)) {
                /* FIX(W429): reject-Zaehler bleibt (Schnitt-Erkennung "viele unplausible Spruenge" unveraendert) */
                nreject++; o->flag = 1; o->why = IP_W_LERP; st_p2hom[ip_mvp_affine(o->mvp) ? 1 : 0]++;
            }
            else if (g_sba_loc && g_cfg.loc_sky_fix && o->lsk && a->lsk && !g_cfg.vr_enable) {
                /* W435 LOC-SKY-LAG-REG: Himmel in beiden Listen (Spalten x, y, w = Kamera) -> der Matrix-Lerp zeigt ihn genau
                 * mit der Zwischenkamera der Welt (Bezugs-Vertices des grossen Quads springen beim Drehen > interp_maxjump).
                 * reject-Zaehler bleibt (Schnitt-Erkennung unveraendert, wie W429). */
                nreject++; o->flag = 1; o->why = IP_W_LERP; InterlockedIncrement(&sk_rejlerp);
            }
            else { nreject++; o->why = IP_W_REJECT; if (g_sba_loc && g_cfg.interp_diag >= 1) { li_rej_note(a, o); } }
            if (iwbest >= 0 && o->why == IP_W_LERP) { ip_iwrong_log(A, B, i, found, iwbest); } /* DIAG(POPIN,W91) */
        }
    }
    if (g_cfg.interp_p2hom && !g_sba_loc && !g_cfg.vr_enable) { ip_p2hom_cam(A, B); } /* FIX(W429) vor interp_pass */
    if (g_cfg.interp_rawzc) {
        /* FIX(W284 INTERP-RAWCHG): "rawchg" = gleiche Signatur, neue Eingangsdaten, Vertex-Lerp an
         * ip_plausible gescheitert (Bezugs-Vertices wechseln die Augenebene oder springen weit) -> bisher
         * exakt (Tick N) zwischen gelerpten Nachbarn = W251-Tiefenmischung (W273_id2: Wasser/Fels-Draw
         * n=42, Eingangsdaten um (0,0,-5) verschoben, gewinnt bei t=0,25 gegen die gelerpte Strasse).
         * Ist die Aenderung eine reine Verschiebung p_B = p_A + d (Gelaende kamera-relativ neu
         * gebacken), gilt p_B*M_B = p_A*(T(d)*M_B): derselbe Fall wie interp_zcons mit M_B' = T(d)*M_B.
         * Passt M_A^-1*M_B' zur Kamerabewegung des Ziels, ist der Vertex-Lerp i <-> i exakt die
         * Zwischenkamera des uebrigen Bildes (auch fuer Vertices hinter der Kamera: linear im
         * Clip-Raum, die Hardware schneidet an der Nahebene). Vor interp_pass, damit weitere
         * Durchgaenge derselben Instanz nicht wegen dieses Draws exakt werden. */
        enum { RZ_T = 8 };
        ID3D11RenderTargetView *zr[RZ_T];
        ID3D11DepthStencilView *zd[RZ_T];
        double zD[RZ_T][8][16];
        int znr[RZ_T], s0ok = 0;
        unsigned nz = 0, k;
        LONGLONG rzq0 = ip_now();
        rz_cam_pre(A, B); /* Quellen = vor diesem Block gelerpte Draws (in diesem Block umgestellte zaehlen nicht mit) */
        for (i = 0; i < B->nops; i++) {
            ip_op_t *o = &B->ops[i];
            ip_op_t *a;
            float d[3] = { 0, 0, 0 };
            double Dx[16], e = 1.0, e0 = 1.0;
            float MBf[16];
            int t = -1, x, c, ofs;
            if (o->kind != IP_DRAW || !o->cls) { continue; }
            if (!o->dsv && !(g_cfg.interp_streak & 2)) { continue; }
            if (g_sba_loc && g_cfg.loc_stat_fix && !g_cfg.vr_enable && o->dsv && !o->rzb && !ip_mvp_affine(o->mvp) &&
                (o->why == IP_W_NOMATCH || o->why == IP_W_RAWCHG || o->why == IP_W_REJECT ||
                 (o->why == IP_W_RAWLERP && o->flag && o->nv >= 1000u))) {
                /* FIX(W415 LOC-STATIC-LAG): Draw in Weltkoordinaten (Matrix bitgleich der Kamera der Welt) ohne brauchbaren
                 * Partner: das grosse Bodennetz der Stadt (~45 700 Vertices; Vertexzahl wechselt je Tick -> nomatch, sonst
                 * rawlerp mit falschen Vertex-Partnern bzw. rawchg), Fern-Streifen (nomatch), weite Spruenge (reject). Blieb exakt
                 * (Tick-N-Lage, 20 Hz) bzw. wurde Vertex i <-> i gemischt, waehrend die Gebaeude glitten. Tick-N-Vertices mit der
                 * Welt-Kamera bewegen (rzm, wie W410 Regel 2): exakt fuer ruhende Welt, ohne Drehung D = I = wie bisher; fuer in
                 * der Welt Bewegtes (Partikel in Weltkoordinaten) nie schlechter als exakt. Partner ohne Zuordnung = Platzhalter
                 * (rzb liest nur Tick-N-Vertices). */
                double Dw[16];
                int ia = -1;
                if (w415_world_cam(A, B, o, Dw, &ia) && memcmp(o->mvp, w415_wc.camB, sizeof o->mvp) == 0 &&
                    ((o->match >= 0 && (unsigned)o->match < A->nops) || ia >= 0)) {
                    if (!(o->match >= 0 && (unsigned)o->match < A->nops)) { o->match = ia; }
                    if (o->why == IP_W_NOMATCH) { nnomatch--; nlerp++; }
                    else if (o->why == IP_W_RAWCHG) { nrawchg--; nlerp++; }
                    /* reject: Zaehler bleiben (Schnitt-Erkennung "viele unplausible Spruenge" unveraendert) */
                    pr_mul_di(o->mva, o->mvp, Dw);
                    o->rzm = 1; o->rzb = 1; o->flag = 1;
                    o->why = IP_W_LERP;
                    st_w415_nm++;
                    continue;
                }
            }
            if (o->flag && o->why == IP_W_RAWLERP && !o->rzb && !g_cfg.vr_enable &&
                (g_cfg.loc_interp_rawrzb >= 2 || (g_cfg.loc_interp_rawrzb == 1 && g_sba_loc)) &&
                o->match >= 0 && (unsigned)o->match < A->nops && !ip_mvp_affine(o->mvp)) {
                /* FIX(W410 LOC-GFX-JITTER) loc_interp_rawrzb: "rawlerp" = gleiche Signatur (also gleiche Vertexzahl), neue
                 * Eingangsdaten, Vertex i <-> i, weil die 4 Bezugs-Vertices kaum sprangen. LoC baut das Gelaende-Netz
                 * vor dem VT (~8400-9200 Vertices, ein Draw) je Tick neu; bei gleicher Vertexzahl gehoert Vertex i im
                 * Vortick oft zu einem ANDEREN Weltpunkt (W410 KEIL: 450-720 Vertices > 8 px, bis 914 px daneben) ->
                 * Dreiecke klappen im Zwischenbild um, durch die Loecher scheint der Himmel (weisse Keile, nur > 20 FPS).
                 * (1) Ruht der Draw in der Welt (M_A^-1*M_B passt zur Kamerabewegung des Ziels und die Bezugs-Vertices des
                 * Partners folgen ihr), ist der Matrix-Lerp der Tick-N-Vertices (rzb) genau die Zwischenkamera -
                 * unabhaengig davon, wie das Netz neu gebaut wurde. (2) Grosse Gelaende-Netze (>= 1000 Vertices, mit
                 * Tiefenpuffer), deren Gitter bzw. Ursprung neu gelegt wurde (Matrix-Test scheitert, Bezugs-Vertices folgen
                 * der Kamera nur grob, camfit <= 0,25): Tick-N-Vertices mit der Kamera bewegen (rzm). Bewegte bzw. mit
                 * der Kamera wandernde kleine Netze (Himmel, Billboards, Partikel) bleiben beim Vertex-Lerp. */
                double ew = 1.0, cf = 1e9, Di[16];
                const int big = o->nv >= 1000u && o->dsv != NULL;
                a = &A->ops[o->match];
                if (a->kind == IP_DRAW && a->cls && a->rtv == o->rtv) {
                    for (k = 0; k < nz; k++) { if (zr[k] == o->rtv && zd[k] == rz_dkey(o)) { t = (int)k; break; } }
                    if (t < 0 && nz < RZ_T) {
                        t = (int)nz++;
                        zr[t] = o->rtv; zd[t] = rz_dkey(o);
                        znr[t] = rz_cam(A, B, o, zD[t]);
                    }
                }
                if (t >= 0 && znr[t]) {
                    if (a->dsv == o->dsv && a->topo == o->topo && zc_dmat(Dx, a->mvp, o->mvp)) {
                        for (x = 0; x < znr[t]; x++) { double ex = zc_derr(Dx, zD[t][x]); if (ex < ew) { ew = ex; } }
                    }
                    /* Bezugs-Vertices des Partners: folgen sie wie ruhende Weltpunkte der Kamera (camfit)? In der Welt
                     * bewegte Netze (Partikel, Geschosse: Vertices in Weltkoordinaten, Matrix = Kamera) bestehen den
                     * Matrix-Test auch, ihre Bezugs-Vertices wandern aber -> dort bleibt der Vertex-Lerp. */
                    if (ew <= 1e-3 || big) { cf = li_camfit(a, o, zD[t], znr[t]); }
                    if ((g_cfg.loc_interp_dbg & 2) && (big || (ew <= 1e-3 && cf > 0.01)) && (g_cfg.pf_ring <= 0 || sb_pf_active()) && st_rz_ln < 1400u) { /* DIAG(W410) */
                        st_rz_ln++;
                        HLOG_I("LOCINTERP rawrzb f=%u i=%u n=%u m=%d welt=%.6f camfit=%.5f topo=%u tex=%u", B->frame, i, o->nv, o->match, ew, cf, o->topo, o->has_tex);
                    }
                    if (ew <= 1e-3 && cf <= 0.01) { /* Matrix-Lerp der Tick-N-Vertices; richtig zugeordnete Vertices unveraendert */
                        o->rzb = 1; st_w410_rzb++; if (o->nv >= 1000u) { st_w410_big++; } continue;
                    }
                    if (big && cf <= 0.25 && ((g_cfg.loc_stat_fix && w415_world_cam(A, B, o, Di, NULL)) || w410_cam_inv(A, B, o, Di))) { /* W415: Welt-Kamera */
                        if (g_cfg.loc_stat_fix && w415_wc.ok && w415_wc.rtv == o->rtv) { st_w415_rzm++; }
                        /* Gelaende-Netz mit neu gelegtem Gitter bzw. verschobenem Ursprung (Matrix-Test scheitert, Bezugs-
                         * Vertices sind andere Weltpunkte, folgen aber grob der Kamera): Tick-N-Vertices mit der Kamera
                         * bewegen, Matrix N-1 = M_B * D^-1 (wie W317 rzm). Exakt fuer ruhende Welt, ohne Vertex-Partner. */
                        pr_mul_di(o->mva, o->mvp, Di);
                        o->rzm = 1; o->rzb = 1; st_w410_rzm++;
                        continue;
                    }
                    if (ew <= 1e-3) { st_w410_mov++; } else { st_w410_cam++; }
                }
                t = -1;
            }
            if (o->flag) {
                /* FIX(W294 LINE-STREAK) interp_streak Bit 4: "rawlerp" mit reinem Eingangs-Versatz d != 0
                 * (M2-Meer: Kacheln n=6, Gitter in 5er-Schritten neu um die Kamera gelegt, Partner nach
                 * Reihenfolge). Der Vertex-Lerp schiebt die Kachel um d durch das Zwischenbild, die
                 * rawchg-Nachbarn stehen in Tick-N-Lage -> Naht zwischen den Kachelreihen, durch die der
                 * Hintergrund scheint (heller Strich, Breite ~ (1-t)). Passt M_A^-1*M_B zur Kamera (Kachel
                 * ruht in der Welt), wie rawchg: Matrix-Lerp der Tick-N-Vertices (rzb). */
                if (!(g_cfg.interp_streak & 4) || o->why != IP_W_RAWLERP || o->rzb || g_cfg.vr_enable ||
                    o->match < 0 || (unsigned)o->match >= A->nops || ip_mvp_affine(o->mvp)) { continue; }
                a = &A->ops[o->match];
                if (a->kind != IP_DRAW || !a->cls || a->rtv != o->rtv || a->dsv != o->dsv || a->topo != o->topo ||
                    !rz_offset_grid(a, o, d)) { continue; }
                st_sk_rlofs++;
                for (k = 0; k < nz; k++) { if (zr[k] == o->rtv && zd[k] == rz_dkey(o)) { t = (int)k; break; } }
                if (t < 0) {
                    if (nz >= RZ_T) { continue; }
                    t = (int)nz++;
                    zr[t] = o->rtv; zd[t] = rz_dkey(o);
                    znr[t] = rz_cam(A, B, o, zD[t]);
                }
                if (znr[t] && zc_dmat(Dx, a->mvp, o->mvp)) {
                    for (x = 0; x < znr[t]; x++) { double ex = zc_derr(Dx, zD[t][x]); if (ex < e0) { e0 = ex; } }
                }
                if (g_cfg.interp_diag >= 1 && st_sk_ln < 300u && (g_cfg.pf_ring <= 0 || sb_pf_active())) { /* DIAG(W294) */
                    st_sk_ln++;
                    HLOG_I("INTERP skd f=%u i=%u n=%u m=%d d=%.3f,%.3f,%.3f kameras=%d fehler_welt=%.6f", B->frame, i, o->nv, o->match,
                           (double)d[0], (double)d[1], (double)d[2], znr[t], e0);
                }
                if (e0 > 1e-3) { st_sk_rlcam++; continue; }
                o->rzb = 1;
                st_sk_rl++;
                continue;
            }
            if (!o->dsv) { st_sk_nod++; }
            if (o->why == IP_W_NOMATCH) {
                /* nomatch: kein Vortick-Draw gleicher Signatur und Vertexzahl (z. B. Gelaende-Stueck mit anderer
                 * Vertexzahl, W284 pf7_0540). Welt-Test mit Kandidaten gleicher Signatur ohne Vertexzahl (sig0):
                 * passt M_A^-1*M_B zur Kamera, ruht das Objekt in der Welt -> Matrix-Lerp der Tick-N-Vertices. */
                int j, cand = -1, nc = 0;
                double eb = 1.0;
                if (g_cfg.interp_rawzc < 2 || g_cfg.vr_enable) { continue; }
                if (!s0ok) {
                    s0ok = rz_s0_build(A) ? 1 : -1;
                }
                if (s0ok < 0) { continue; }
                for (k = 0; k < nz; k++) { if (zr[k] == o->rtv && zd[k] == rz_dkey(o)) { t = (int)k; break; } }
                if (t < 0) {
                    if (nz >= RZ_T) { st_rz_other++; continue; }
                    t = (int)nz++;
                    zr[t] = o->rtv; zd[t] = rz_dkey(o);
                    znr[t] = rz_cam(A, B, o, zD[t]);
                }
                if (!znr[t]) { st_rz_nocam++; continue; }
                for (j = rz_s0head[IP_HB(o->sig0)]; j >= 0 && nc < 16; j = rz_s0next[j]) {
                    const ip_op_t *c2 = &A->ops[j];
                    double ex = 1.0;
                    if (c2->sig0 != o->sig0 || !c2->cls || c2->rtv != o->rtv || c2->dsv != o->dsv || c2->topo != o->topo) { continue; }
                    nc++;
                    if (!zc_dmat(Dx, c2->mvp, o->mvp)) { continue; }
                    for (x = 0; x < znr[t]; x++) { double e2 = zc_derr(Dx, zD[t][x]); if (e2 < ex) { ex = e2; } }
                    if (ex < eb) { eb = ex; cand = j; }
                }
                if (!nc || cand < 0 || eb > 1e-3) {
                    /* FIX(W347 GHOST) interp_bbcam: gleiche Billboard-Instanzen (Baeume, gleiche Daten, je Baum eine
                     * zur Kamera gedrehte Matrix) sind beim Schwenk nach Bildsprung mehrdeutig (W91 amb -> nomatch)
                     * und bestehen den Welt-Test nicht -> Partner per Kamera-Schwerpunkt, Matrix-/Vertex-Lerp. */
                    int bj = (g_cfg.interp_bbcam && !g_sba_loc) ? ip_bb_pick(A, o, zD[t], znr[t]) : -1;
                    if (bj < 0) {
                        if (!nc) { st_rz_nmno++; } else { st_rz_nmcam++; }
                        continue;
                    }
                    A->ops[bj].taken = 1;
                    o->match = bj;
                    o->flag = 1;
                    o->why = IP_W_LERP;
                    st_bb_ok++;
                    nnomatch--;
                    nlerp++;
                    continue;
                }
                o->match = cand;
                o->rzb = 1;
                o->flag = 1;
                o->why = IP_W_LERP;
                st_rz_nm++;
                nnomatch--;
                nlerp++;
                continue;
            }
            if (o->why == IP_W_REJECT && g_sba_loc && g_cfg.loc_interp_rejcam && !g_cfg.vr_enable) {
                /* W340 LOC-FRAMES: "reject" = gleiche Eingangsdaten, aber Bezugs-Vertices springen > interp_maxjump
                 * (NDC). Bei 20 Hz und Drehung meist Vertices nahe der Augenebene (Sprung >= 2 NDC, LOCINTERP
                 * reject). Passt M_A^-1*M_B zur Kamerabewegung des Ziels, ruht das Objekt in der Welt -> der
                 * Matrix-Lerp ist genau die Zwischenkamera (linear im Clip-Raum). */
                double er = 1.0;
                if (o->match < 0 || (unsigned)o->match >= A->nops) { continue; }
                a = &A->ops[o->match];
                if (a->kind != IP_DRAW || !a->cls || a->rtv != o->rtv || a->dsv != o->dsv) { continue; }
                for (k = 0; k < nz; k++) { if (zr[k] == o->rtv && zd[k] == rz_dkey(o)) { t = (int)k; break; } }
                if (t < 0) {
                    if (nz >= RZ_T) { continue; }
                    t = (int)nz++;
                    zr[t] = o->rtv; zd[t] = rz_dkey(o);
                    znr[t] = rz_cam(A, B, o, zD[t]);
                }
                if (!znr[t] || !zc_dmat(Dx, a->mvp, o->mvp)) { continue; }
                for (x = 0; x < znr[t]; x++) { double ex = zc_derr(Dx, zD[t][x]); if (ex < er) { er = ex; } }
                if (er > 1e-3 && (g_cfg.loc_interp_dbg & 2) && (g_cfg.pf_ring <= 0 || sb_pf_active()) && st_rz_ln < 1400u) { /* DIAG(W349) */
                    st_rz_ln++;
                    HLOG_I("LOCINTERP rjerr f=%u i=%u n=%u m=%d er=%.5f nD=%d camfit=%.3f", B->frame, i, o->nv, o->match, er, znr[t],
                           li_camfit(a, o, zD[t], znr[t]));
                }
                if (er > 1e-3 && !(g_cfg.loc_interp_rzalt && er <= 0.01 && li_camfit(a, o, zD[t], znr[t]) <= 0.05)) { continue; } /* W349: Geometrie bestaetigt */
                if (er > 1e-3) { li_rj_cam2++; }
                o->flag = 1;
                o->why = IP_W_LERP;
                nreject--;
                nlerp++;
                li_rj_cam++;
                continue;
            }
            if (o->why != IP_W_RAWCHG) { continue; }
            if (o->match < 0 || (unsigned)o->match >= A->nops) { st_rz_other++; continue; }
            a = &A->ops[o->match];
            if (a->kind != IP_DRAW || !a->cls || a->rtv != o->rtv || a->dsv != o->dsv ||
                a->topo != o->topo) { st_rz_other++; continue; }
            for (k = 0; k < nz; k++) { if (zr[k] == o->rtv && zd[k] == rz_dkey(o)) { t = (int)k; break; } }
            if (t < 0) {
                if (nz >= RZ_T) { st_rz_other++; continue; }
                t = (int)nz++;
                zr[t] = o->rtv; zd[t] = rz_dkey(o);
                znr[t] = rz_cam(A, B, o, zD[t]);
            }
            if (!znr[t]) { st_rz_nocam++; continue; }
            /* e0: Objekt ruht in der Welt, nur sein Gitter wurde verschoben (Weltmatrix gleich) -> D = M_A^-1*M_B
             *     passt zur Kamera; richtig ist dann der Matrix-Lerp der Tick-N-Vertices (p_B*M_A -> p_B*M_B).
             * e1: Ursprung verschoben (p_B = p_A + d, Weltmatrix gleicht d aus) -> D = M_A^-1*(T(d)*M_B) passt;
             *     richtig ist der Vertex-Lerp i <-> i (p_A*M_A -> p_B*M_B). */
            if (zc_dmat(Dx, a->mvp, o->mvp)) {
                for (x = 0; x < znr[t]; x++) { double ex = zc_derr(Dx, zD[t][x]); if (ex < e0) { e0 = ex; } }
            }
            if (e0 > 1e-3 && g_sba_loc && g_cfg.loc_interp_rzalt && !g_cfg.vr_enable) {
                /* W349 LOC-FRAMES-2: Mehrpass-Draws gleicher Signatur (LoC-Himmel/Wolken: 3 Durchgaenge mit je 258
                 * Vertices) waehlten alle denselben Vortick-Partner (ip_pick: erster freier ab jp, der rawchg-Partner
                 * wird nicht belegt). Nur der erste passte zur Kamera, die weiteren blieben exakt -> weiche Flecken
                 * im Himmel auf dem Tickstand. Der Matrix-Lerp der Tick-N-Vertices braucht vom Partner nur M_A ->
                 * beste zur Kamera passende Matrix unter den Kandidaten gleicher Signatur ohne Vertexzahl (sig0). */
                int j2, cand = -1, nc = 0;
                double eb = e0;
                if (!s0ok) { s0ok = rz_s0_build(A) ? 1 : -1; }
                for (j2 = s0ok > 0 ? rz_s0head[IP_HB(o->sig0)] : -1; j2 >= 0 && nc < 16; j2 = rz_s0next[j2]) {
                    const ip_op_t *c2 = &A->ops[j2];
                    double ex = 1.0;
                    if (j2 == o->match || c2->sig0 != o->sig0 || !c2->cls || c2->rtv != o->rtv || c2->dsv != o->dsv ||
                        c2->topo != o->topo) { continue; }
                    nc++;
                    if (!zc_dmat(Dx, c2->mvp, o->mvp)) { continue; }
                    for (x = 0; x < znr[t]; x++) { double e2 = zc_derr(Dx, zD[t][x]); if (e2 < ex) { ex = e2; } }
                    if (ex < eb) { eb = ex; cand = j2; }
                }
                if (cand >= 0 && eb <= 1e-3) { o->match = cand; a = &A->ops[cand]; e0 = eb; li_rz_alt++; }
                else if (e0 <= 0.01 && li_camfit_pm((const float (*)[4])o->pref, a->mvp, o, zD[t], znr[t]) <= 0.05) {
                    e0 = 1e-3; /* knapp ueber der Matrix-Schwelle, Geometrie bestaetigt (Rundung grosser Weltkoordinaten) */
                    li_rz_alt++;
                }
            }
            /* Welt-Fall zuerst; der Versatz-Test (zweite Inversion) nur, wenn er scheitert (W284: Zeit je Tick halbiert) */
            ofs = (e0 > 1e-3 || g_cfg.vr_enable) && !a->taken && a->nv == o->nv && rz_offset(a, o, d); /* Vertex-Lerp braucht den Partner exklusiv */
            if (ofs) {
                /* M_B' = T(d) * M_B (Zeilenvektoren): Zeilen 0..2 wie M_B, Zeile 3 = d*M_B[0..2] + M_B[3] */
                for (c = 0; c < 16; c++) { MBf[c] = o->mvp[c]; }
                for (c = 0; c < 4; c++) {
                    MBf[12 + c] = (float)((double)d[0] * o->mvp[c] + (double)d[1] * o->mvp[4 + c] + (double)d[2] * o->mvp[8 + c] + o->mvp[12 + c]);
                }
                if (zc_dmat(Dx, a->mvp, MBf)) {
                    for (x = 0; x < znr[t]; x++) { double ex = zc_derr(Dx, zD[t][x]); if (ex < e) { e = ex; } }
                }
            }
            if (g_cfg.interp_diag >= 1 && (g_cfg.pf_ring <= 0 || sb_pf_active()) &&
                (g_cfg.interp_ddrel > 0 ? (sb_pf_m0() && B->frame >= sb_pf_m0() + (unsigned)g_cfg.interp_ddrel &&
                                           B->frame < sb_pf_m0() + (unsigned)g_cfg.interp_ddrel + 3u)
                                        : st_rz_ln < 400u)) { /* DIAG(W284) */
                st_rz_ln++;
                HLOG_I("INTERP rzd f=%u i=%u n=%u/%u m=%d versatz=%d d=%.3f,%.3f,%.3f fehler_welt=%.6f fehler_versatz=%.6f", B->frame, i,
                       o->nv, a->nv, o->match, ofs, (double)d[0], (double)d[1], (double)d[2], e0, e);
            }
            if (e0 <= 1e-3 && e0 <= e && !g_cfg.vr_enable) {
                o->rzb = 1; /* Matrix-Lerp der Tick-N-Vertices */
                st_rz_relw++;
            } else if (e <= 1e-3 && ofs) {
                st_rz_rel++;
            } else {
                /* FIX(W347 GHOST) interp_bbcam: Billboards (Baeume: n=6/12, je Tick neu zur Kamera gedreht) bestehen
                 * weder den Welt- noch den Versatz-Test und blieben exakt (Tick-N-Lage in allen Zwischenbildern) ->
                 * beim Drehen stehen die Baeume, der Rest gleitet = Doppelkanten. Partner = Vortick-Draw, dessen
                 * Drehpunkt (Ursprung der Matrix) mit der Kamera des Ziels auf den von o faellt; dann Vertex-Lerp. */
                int bj = (g_cfg.interp_bbcam && !g_sba_loc && !g_cfg.vr_enable) ? ip_bb_pick(A, o, zD[t], znr[t]) : -1;
                int vl = -1;
                if (g_sba_loc && g_cfg.loc_interp_rzalt && !g_cfg.vr_enable) {
                    /* W349 LOC-FRAMES-2: Vertex-Lerp i <-> i mit dem Kandidaten gleicher Signatur (gleiche Vertexzahl),
                     * dessen Bezugs-Vertices der Kamera folgen (li_camfit <= 0,25 der Objektgroesse): neu gebaute
                     * Billboards (Wolken/Glow-Sprites des Himmels, 6 Vertices, je Tick neu um die Kamera gedreht) und
                     * mitwandernde Himmelsgitter. Ohne Kamera-Bezug des Ziels kein Lerp. */
                    unsigned h2 = IP_HB(o->sig);
                    int j3, nc3 = 0;
                    double eb3 = 0.25;
                    for (j3 = ip_hhead[h2]; j3 >= 0 && nc3 < 64; j3 = ip_hnext[j3]) {
                        const ip_op_t *c3 = &A->ops[j3];
                        double ex3;
                        if (c3->sig != o->sig || c3->nv != o->nv || c3->kind != IP_DRAW || c3->rtv != o->rtv) { continue; }
                        nc3++;
                        ex3 = li_camfit(c3, o, zD[t], znr[t]);
                        if (c3->taken) { ex3 += 0.05; } /* freie Partner bevorzugt */
                        if (ex3 < eb3) { eb3 = ex3; vl = j3; }
                    }
                    if ((g_cfg.loc_interp_dbg & 2) && (g_cfg.pf_ring <= 0 || sb_pf_active()) && st_rz_ln < 1400u) { /* DIAG(W349) */
                        st_rz_ln++;
                        HLOG_I("LOCINTERP rzv f=%u i=%u n=%u m=%d e0=%.4f kand=%d best=%d fehler=%.3f", B->frame, i, o->nv,
                               o->match, e0, nc3, vl, vl >= 0 ? eb3 : -1.0);
                    }
                }
                if (bj < 0 && vl < 0) {
                    if (!ofs) { st_rz_noofs++; }
                    st_rz_cam++;
                    continue;
                }
                if (bj >= 0) {
                    a = &A->ops[bj];
                    o->match = bj;
                    st_bb_ok++;
                } else {
                    o->match = vl;
                    a = &A->ops[vl];
                    o->rzb = 0; /* Vertex-Lerp i <-> i */
                    li_rz_vl++;
                }
            }
            a->taken = 1;
            o->flag = 1;
            o->why = IP_W_RAWLERP;
            nrawchg--;
            nlerp++;
            nrawlerp++;
        }
        {
            double ms = ip_ms(rzq0, ip_now());
            st_rz_ms += ms; st_rz_n++;
            if (ms > st_rz_max) { st_rz_max = ms; }
        }
    }
    if (g_cfg.interp_pass) {
        /* POPIN (W91): Mehrpass-Konsistenz. Durchgaenge derselben Instanz
         * (gleiches Ziel, gleiche Geometrie = gkey, gleiche Matrix) muessen
         * gleich behandelt werden: ist einer exakt (ohne Partner, neue
         * Eingangsdaten, unplausibel), wird ein gelerpter zweiter Durchgang
         * gegen ihn verschoben gezeichnet -> Z-Fighting-Streifen/Aufblitzen
         * in Zwischenbildern. Dann alle Durchgaenge der Instanz exakt. */
        /* FIX(W273 INTERP-RACE) interp_pass_exact=1: Die alte 16-Bit-Bitmaske (ip_mvpkey) kollidiert
         * (~600 exakte Eintraege je Tick in 65536 Bit -> ~1 % der gelerpten Draws faelschlich exakt),
         * und weil gkey den Host-Zeiger der RTV enthaelt, trifft es je Lauf andere Draws: exakte Draws
         * zwischen gelerpten (Tiefenmischung W251) = helle Keile, die in einem Lauf erscheinen und im
         * anderen nicht (W265-Streuung). Jetzt Eimer per Schluessel, Mitglied nur bei voller Gleichheit
         * (Ziel, Geometrie, Vertexzahl, Matrix) -> zeigerunabhaengig und ohne Fehltreffer. */
        static unsigned char ex[1u << 13]; /* 65536 Bit */
        static int ph[1u << 16];
        static int *pn;
        static unsigned pcap;
        unsigned npass = 0, nfalse = 0;
        int exact = g_cfg.interp_pass_exact > 0;
        if (exact && pcap < B->nops) {
            int *nn = (int *)realloc(pn, B->nops * sizeof *nn);
            if (nn) { pn = nn; pcap = B->nops; } else { exact = 0; }
        }
        memset(ex, 0, sizeof ex);
        if (exact) { memset(ph, 0xFF, sizeof ph); }
        for (i = 0; i < B->nops; i++) {
            const ip_op_t *o = &B->ops[i];
            unsigned k;
            if (o->kind != IP_DRAW || !o->cls) { continue; }
            if (o->why != IP_W_NOMATCH && o->why != IP_W_RAWCHG && o->why != IP_W_REJECT) { continue; }
            k = ip_mvpkey(o);
            ex[k >> 3] |= (unsigned char)(1u << (k & 7u));
            if (exact) { pn[i] = ph[k]; ph[k] = (int)i; }
        }
        for (i = 0; i < B->nops; i++) {
            ip_op_t *o = &B->ops[i];
            unsigned k;
            int hit;
            if (o->kind != IP_DRAW || !o->cls || !o->flag) { continue; }
            k = ip_mvpkey(o);
            hit = (ex[k >> 3] & (1u << (k & 7u))) != 0;
            if (exact && hit) {
                int j, same = 0;
                for (j = ph[k]; j >= 0; j = pn[j]) {
                    const ip_op_t *e = &B->ops[j];
                    if (e->gkey == o->gkey && e->rtv == o->rtv && e->rawhash == o->rawhash && e->nv == o->nv &&
                        memcmp(e->mvp, o->mvp, sizeof o->mvp) == 0 &&
                        /* FIX(W429): P2 (vorab transformiert) traegt nur die Bildraum-Matrix - gleiche Matrix heisst dort
                         * NICHT gleiche Instanz (dasselbe Gelaende-Netz an anderer Stelle). Instanz = gleiche Bildlage. */
                        !(g_cfg.interp_p2hom && !g_sba_loc && ip_mvp_affine(o->mvp) && memcmp(e->pref, o->pref, sizeof o->pref) != 0)) {
                        same = 1; o->exj = j; break;
                    }
                }
                if (!same) { nfalse++; hit = 0; }
            }
            if (hit) { o->flag = 0; npass++; o->exr = 1; }
        }
        st_ipass += npass;
        st_ipass_false += nfalse;
        if (npass <= nlerp) { nlerp -= npass; }
    }
    if (g_cfg.interp_zcons == 1) {
        /* FIX(W251 INTERP-GROUND): Tiefen-Konsistenz. Exakte (Tick N) und gelerpte Welt-Draws im
         * selben Tiefenpuffer sind nicht vergleichbar - Beleg W251_d1 (M1-Hangar, shading=0): der
         * Hangarboden ist exakt (VS-Signatur wechselt, Bezugspunkte an der Augenebene), die spaeter
         * gezeichneten Gelaendestreifen darunter sind gelerpt und gewinnen den Tiefentest ->
         * Bodentextur ueber dem halben Hauptmonitor; umgekehrt (W251_d2) steht eine exakte Wand
         * (reject, 300 px Sprung beim Schwenk) als heller Splitter vor dem gelerpten Boden.
         * Darum: ein exakter Welt-Draw (nomatch/reject) mit einem Vortick-Draw derselben
         * Objektraum-Vertices im selben Ziel wird per Matrix gelerpt, wenn dessen Matrix-Aenderung
         * zur Kamerabewegung passt. Fuer ein ruhendes Objekt gilt D = M_A^-1 * M_B = VP_A^-1 * VP_B
         * (die Weltmatrix kuerzt sich heraus); verglichen wird mit D der 8 groessten plausibel
         * gelerpten Draws des Ziels (ein Treffer genuegt). Falsche Instanz oder bewegtes Objekt
         * passen zu keinem; unter mehreren Instanzen gewinnt die mit der kleinsten Abweichung.
         * Clip-Lerp gleicher Vertices = Transformation mit der Zwischenmatrix: dieselbe
         * Zwischenkamera wie das uebrige Bild. */
        enum { ZC_T = 8, ZC_R = 8 };
        ID3D11RenderTargetView *zr[ZC_T];
        ID3D11DepthStencilView *zd[ZC_T];
        double zD[ZC_T][ZC_R][16];
        int znr[ZC_T];
        unsigned nz = 0, k;
        for (i = 0; i < B->nops; i++) {
            ip_op_t *o = &B->ops[i];
            int t = -1, j, cand = -1, nc = 0;
            double ebest = 1.0;
            unsigned q;
            if (o->kind != IP_DRAW || !o->cls || o->flag || !o->dsv) { continue; }
            if (o->why != IP_W_NOMATCH && o->why != IP_W_REJECT) { continue; }
            for (k = 0; k < nz; k++) { if (zr[k] == o->rtv && zd[k] == o->dsv) { t = (int)k; break; } }
            if (t < 0) { /* Kamerabewegungen dieses Ziels einmal je Tick */
                int rr[ZC_R], x;
                if (nz >= ZC_T) { continue; }
                t = (int)nz++;
                zr[t] = o->rtv; zd[t] = o->dsv; znr[t] = 0;
                for (x = 0; x < ZC_R; x++) { rr[x] = -1; }
                for (q = 0; q < B->nops; q++) {
                    const ip_op_t *r = &B->ops[q];
                    int w;
                    if (r->kind != IP_DRAW || r->why != IP_W_LERP || !r->flag || r->rtv != o->rtv || r->dsv != o->dsv ||
                        r->match < 0 || (unsigned)r->match >= A->nops) { continue; }
                    if (!zc_cam_src(&A->ops[r->match], r)) { continue; } /* W294 Bit 1 */
                    for (w = 0; w < ZC_R; w++) {
                        if (rr[w] < 0 || r->nv > B->ops[rr[w]].nv) {
                            int z;
                            for (z = ZC_R - 1; z > w; z--) { rr[z] = rr[z - 1]; }
                            rr[w] = (int)q;
                            break;
                        }
                    }
                }
                for (x = 0; x < ZC_R && rr[x] >= 0; x++) {
                    if (zc_dmat(zD[t][znr[t]], A->ops[B->ops[rr[x]].match].mvp, B->ops[rr[x]].mvp)) { znr[t]++; }
                }
                if (znr[t]) { st_zc_tgt++; }
            }
            if (!znr[t]) { continue; }
            for (j = ip_ghead[IP_HB(o->gkey)]; j >= 0 && nc < 32; j = ip_gnext[j]) {
                const ip_op_t *a = &A->ops[j];
                double Dx[16], e = 1.0;
                int x;
                if (a->kind != IP_DRAW || (a->taken && j != o->match) || a->gkey != o->gkey || a->rtv != o->rtv ||
                    a->dsv != o->dsv || a->nv != o->nv || a->rawhash != o->rawhash || !a->cls) { continue; }
                nc++;
                if (memcmp(a->mvp, o->mvp, sizeof o->mvp) == 0 || !zc_dmat(Dx, a->mvp, o->mvp)) { continue; }
                for (x = 0; x < znr[t]; x++) { double ex = zc_derr(Dx, zD[t][x]); if (ex < e) { e = ex; } }
                if (e < ebest) { ebest = e; cand = j; }
            }
            if (o->nv >= 1000u && g_cfg.interp_diag >= 1 && st_zc_ln < 400u && (g_cfg.pf_ring <= 0 || sb_pf_active())) { /* DIAG(W251) */
                st_zc_ln++;
                HLOG_I("INTERP zcd f=%u i=%u n=%u why=%u nc=%d cand=%d kamera_fehler=%.6f", B->frame, i, o->nv, o->why, nc, cand, ebest);
            }
            if (!nc) { st_zc_nocand++; continue; }
            if (cand < 0 || ebest > 1e-3) { st_zc_cam++; continue; }
            A->ops[cand].taken = 1;
            o->match = cand;
            o->flag = 1;
            o->why = IP_W_LERP;
            st_zc_rel++;
            nlerp++;
        }
    } else if (g_cfg.interp_zcons == 2) {
        /* Versuch (W251): Ziel mit grossem exaktem Welt-Draw ganz exakt - Hauptmonitor dann kaum
         * noch interpoliert (W251_z0: 97 % der Zwischenbilder wie Tick N), nicht Standard. */
        ID3D11RenderTargetView *zr[8];
        ID3D11DepthStencilView *zd[8];
        unsigned nz = 0, k, nzd = 0;
        for (i = 0; i < B->nops && nz < 8u; i++) {
            const ip_op_t *o = &B->ops[i];
            int q, cross = 0, nf = 0, dup = 0;
            float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
            if (o->kind != IP_DRAW || !o->cls || o->flag || !o->dsv) { continue; }
            if (o->why != IP_W_NOMATCH && o->why != IP_W_RAWCHG && o->why != IP_W_REJECT) { continue; }
            for (q = 0; q < 4; q++) {
                if (o->rn[q][2] <= 1e-6f) { cross = 1; continue; }
                nf++;
                if (o->rn[q][0] < x0) { x0 = o->rn[q][0]; }
                if (o->rn[q][0] > x1) { x1 = o->rn[q][0]; }
                if (o->rn[q][1] < y0) { y0 = o->rn[q][1]; }
                if (o->rn[q][1] > y1) { y1 = o->rn[q][1]; }
            }
            if (!((cross && nf > 0) || (nf > 1 && (x1 - x0 > 1.0f || y1 - y0 > 1.0f)))) { continue; }
            for (k = 0; k < nz; k++) { if (zr[k] == o->rtv && zd[k] == o->dsv) { dup = 1; } }
            if (!dup) { zr[nz] = o->rtv; zd[nz] = o->dsv; nz++; }
        }
        for (i = 0; nz && i < B->nops; i++) {
            ip_op_t *o = &B->ops[i];
            if (o->kind != IP_DRAW || !o->cls || !o->flag) { continue; }
            for (k = 0; k < nz; k++) {
                if (zr[k] == o->rtv && zd[k] == o->dsv) { o->flag = 0; nzd++; break; }
            }
        }
        if (nz) { st_zc_ticks++; st_zc_draws += nzd; }
        if (nzd <= nlerp) { nlerp -= nzd; }
    }
    if (g_cfg.interp_pair || g_cfg.interp_pair_diag) { ip_pair_robust(A, B); } /* W317 INTERP-PAIR: nach W284/W294/W91/W251 (deren Ergebnis = alte Zuordnung) */
    for (i = 0; i < B->nops; i++) { /* W264 DRAW-OPT2: Lerp nur zwischen gleich abgelegten Draws (beide Liste oder beide indiziert) */
        ip_op_t *o = &B->ops[i];
        if (o->kind == IP_DRAW && o->flag && o->match >= 0 && (unsigned)o->match < A->nops &&
            A->ops[o->match].ib1 != o->ib1 && !(o->rzb && (g_cfg.interp_streak & 4))) { o->flag = 0; o->exr = 2; if (nlerp) { nlerp--; } } /* W294: rzb liest nur B */
    }
    if (g_cfg.interp_eyediag > 0) { /* DIAG(W237): Zuordnungs-Ergebnis der Welt-Draws (cls=1) */
        for (i = 0; i < B->nops; i++) {
            const ip_op_t *o = &B->ops[i];
            if (o->kind == IP_DRAW && o->cls) { st_wwhy[o->why < IP_W_N ? o->why : 0]++; }
        }
    }
    if (g_cfg.interp_eyediag > 0) {
        /* DIAG(W224 LINE-FLICKER): zwischengerechnete Draws, in denen einzelne
         * Vertices zwischen Tick N-1 und N die Augenebene kreuzen (w-Wechsel).
         * Clip-Lerp laeuft dort durch w=0 -> Projektion ins Unendliche. */
        const float *WA = ip_cw[(int)(A - g_ipl)], *WB = ip_cw[(int)(B - g_ipl)];
        for (i = 0; WA && WB && i < B->nops; i++) {
            const ip_op_t *o = &B->ops[i];
            const ip_op_t *a;
            unsigned k, nx = 0, ref = 0;
            float wn = 0.0f, wa = 0.0f;
            if (o->kind != IP_DRAW || !o->flag || o->match < 0) { continue; }
            a = &A->ops[o->match];
            if (a->nv != o->nv) { continue; }
            if (a->vstart + a->nv > ip_cwcap[(int)(A - g_ipl)] ||
                o->vstart + o->nv > ip_cwcap[(int)(B - g_ipl)]) { continue; }
            {   /* W237: P2-Vertices mit echtem w-Wechsel (Strom w = 1) und Sprung-Kriterium je Vertex */
                const float (*JA)[2] = ip_cj[(int)(A - g_ipl)], (*JB)[2] = ip_cj[(int)(B - g_ipl)];
                const unsigned char *PB2 = ip_cp2[(int)(B - g_ipl)];
                unsigned np2 = 0, nhit = 0, njmp = 0, kref = 0, nfront = 0;
                float jw0 = 0, jw1 = 0, jx0 = 0, jx1 = 0;
                for (k = 0; k < o->nv; k++) {
                    float w0 = WA[a->vstart + k], w1 = WB[o->vstart + k];
                    const float *n0 = JA[a->vstart + k], *n1 = JB[o->vstart + k];
                    float dx = fabsf(n1[0] - n0[0]), dy = fabsf(n1[1] - n0[1]);
                    float mx = 1.0f + fmaxf(fabsf(n0[0]), fabsf(n1[0])), my = 1.0f + fmaxf(fabsf(n0[1]), fabsf(n1[1]));
                    int jmp = (dx > 0.5f * mx || dy > 0.5f * my);
                    int p2 = PB2[o->vstart + k] && w0 * w1 <= 0.0f;
                    if (jmp) {
                        njmp++;
                        if (w0 > 0.0f && w1 > 0.0f) { st_jf[o->why < IP_W_N ? o->why : 0]++; nfront++; }
                        else if (w0 < 0.0f && w1 < 0.0f) { st_jb[o->why < IP_W_N ? o->why : 0]++; }
                    }
                    if (p2) {
                        if (!np2) { kref = k; jw0 = w0; jw1 = w1; jx0 = n0[0]; jx1 = n1[0]; }
                        np2++;
                        if (jmp) { nhit++; }
                    }
                }
                st_jmp_verts += njmp;
                if (nfront) { st_jfd[o->why < IP_W_N ? o->why : 0]++; }
                st_jmp_fp += njmp - nhit;
                if (np2) {
                    st_p2x_draws++;
                    st_p2x_verts += np2;
                    st_p2x_hit += nhit;
                    if (st_p2x_ln < (unsigned)g_cfg.interp_eyediag && (g_cfg.pf_ring <= 0 || sb_pf_active())) { /* mit pf_ring nur in den Bildfolgen */
                        st_p2x_ln++;
                        HLOG_I("INTERP p2x f=%u i=%u why=%u n=%u nx=%u hit=%u jmp=%u k=%u w=%.3f->%.3f x=%.2f->%.2f vs=%X",
                               B->frame, i, o->why, o->nv, np2, nhit, njmp, kref, jw0, jw1, jx0, jx1, o->pvs);
                    }
                }
            }
            for (k = 0; k < o->nv; k++) {
                float w0 = WA[a->vstart + k], w1 = WB[o->vstart + k];
                if (w0 * w1 <= 0.0f) {
                    if (!nx) { wa = w0; wn = w1; ref = k; }
                    nx++;
                }
            }
            if (!nx) { continue; }
            st_eyex_draws++;
            st_eyex_verts += nx;
            if (st_eyex_ln < (unsigned)g_cfg.interp_eyediag) {
                st_eyex_ln++;
                HLOG_I("INTERP eyex f=%u i=%u why=%s cls=%u rt=%d n=%u nx=%u k=%u w=%.3f->%.3f topo=%u vs=%X",
                       B->frame, i, o->why == IP_W_LERP ? "lerp" : o->why == IP_W_RAWLERP ? "rawlerp" :
                       o->why == IP_W_RT2DLERP ? "rt2dlerp" : "?", o->cls, o->rtv != g_rtv, o->nv, nx,
                       ref, wa, wn, o->topo, o->pvs);
            }
        }
    }
    if ((g_cfg.loc_interp_dbg & 2) && g_sba_loc && (g_cfg.pf_ring <= 0 || sb_pf_active())) { /* DIAG(W349): exakte Welt-Draws */
        static unsigned nln;
        for (i = 0; i < B->nops && nln < 1500u; i++) {
            const ip_op_t *o = &B->ops[i];
            const ip_op_t *a = (o->match >= 0 && (unsigned)o->match < A->nops) ? &A->ops[o->match] : NULL;
            if (o->kind != IP_DRAW || !o->cls || o->flag || o->why == IP_W_STATIC) { continue; }
            nln++;
            HLOG_I("LOCINTERP ex f=%u i=%u why=%s bb=%d n=%u/%d topo=%u vs=%X xp=%X ps=%X raw=%08X m=%d "
                   "ndc=%.2f,%.2f w=%.1f / %.2f,%.2f w=%.1f a=%.2f,%.2f w=%.1f / %.2f,%.2f w=%.1f",
                   B->frame, i, pr_why(o->why), o->rtv == g_rtv, o->nv, a ? (int)a->nv : -1, o->topo, o->pvs, o->pxp,
                   o->pps, o->rawhash, o->match, o->rn[0][0], o->rn[0][1], o->rn[0][2], o->rn[3][0], o->rn[3][1], o->rn[3][2],
                   a ? a->rn[0][0] : 0.f, a ? a->rn[0][1] : 0.f, a ? a->rn[0][2] : 0.f, a ? a->rn[3][0] : 0.f,
                   a ? a->rn[3][1] : 0.f, a ? a->rn[3][2] : 0.f);
        }
    }
    st_lerp += nlerp; st_static += nstatic; st_nomatch += nnomatch;
    st_rawchg += nrawchg; st_reject += nreject; st_2d += n2d; st_rawlerp += nrawlerp;
    st_rt2d += nrt2d;
    st_geo += ngeo;
    if (log) {
        HLOG_I("INTERP pair f=%u ops=%u draws=%u clears=%u bb=%u 2d=%u world=%u matched=%u inorder=%u lerp=%u (rawlerp=%u rt2d=%u) static=%u nomatch=%u rawchg=%u reject=%u verts=%u/%u",
               B->frame, B->nops, ndraw, nclr, bbdraws, n2d, nworld, nmatch, inorder,
               nlerp, nrawlerp, nrt2d, nstatic, nnomatch, nrawchg, nreject, A->nverts, B->nverts);
    }
    /* Schnitt (Kamerasprung): viele unplausible Spruenge -> Paar exakt. */
    if (nreject > 4u && nreject * 4u > nlerp + nreject) {
        st_cut++;
        for (i = 0; i < B->nops; i++) { B->ops[i].flag = 0; }
        if (log || st_cut <= 8u) {
            HLOG_I("INTERP cut f=%u reject=%u lerp=%u (Paar exakt)", B->frame, nreject, nlerp);
        }
        return 0;
    }
    return 1;
}

/* Diagnose: Draw-Liste eines Ticks (interp_diag=2, erste volle Ticks). */
static void ip_dump(ip_list_t *B)
{
    unsigned i;
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        if (o->kind != IP_DRAW) {
            HLOG_I("INTERP op f=%u i=%u %s rtv=%p dsv=%p", B->frame, i,
                   o->kind == IP_CLR_RTV ? "clr-rt" : o->kind == IP_COPY ? "copy" : "clr-ds", (void *)o->rtv, (void *)o->dsv);
            continue;
        }
        HLOG_I("INTERP op f=%u i=%u draw rtv=%p%s cls=%u n=%u topo=%u tex=%u sig=%08X raw=%08X out=%08X m=%d fl=%u mvp0=%.4f,%.4f,%.4f,%.4f",
               B->frame, i, (void *)o->rtv, o->rtv == g_rtv ? "(BB)" : "", o->cls, o->nv, o->topo,
               o->has_tex, o->sig, o->rawhash, o->outhash, o->match, o->flag,
               o->mvp[0], o->mvp[5], o->mvp[12], o->mvp[13]);
    }
}

/* ---------- Schatten-Ziele + Sicherung je Liste ---------- */

static long long isw_cp_q, isw_un_q, isw_gr_q; static unsigned isw_cp_n, isw_gr_n; /* DIAG(LOC-IM-LOCK): CopyResource / unref / ip_sh_ref je Swap */
static unsigned ip_sh_gen; /* W361: Generationszaehler der Schatten-Eintraege */
static volatile LONG st_sh_stale, st_sh_stale_tot; /* W361: Wiedergaben mit ersetztem Eintrag (je Fenster / gesamt) */

static void ip_sh_free(ip_sh_t *e)
{
    int i;
    if (e->res) { ID3D11Resource_Release(e->res); }
    { /* W361: Zusatz-Referenzen der Zaehlerwache mit abgeben */
        ID3D11View *vw[3];
        int q;
        vw[0] = (ID3D11View *)e->rtv; vw[1] = (ID3D11View *)e->dsv; vw[2] = (ID3D11View *)e->srv;
        for (q = 0; q < 3; q++) { while (vw[q] && e->pin[q]) { ID3D11View_Release(vw[q]); e->pin[q]--; } }
    }
    if (e->rtv) { ID3D11RenderTargetView_Release(e->rtv); }
    if (e->dsv) { ID3D11DepthStencilView_Release(e->dsv); }
    if (e->srv) { ID3D11ShaderResourceView_Release(e->srv); }
    if (e->sh) { ID3D11Texture2D_Release(e->sh); }
    for (i = 0; i < IP_LISTS; i++) {
        if (e->bak[i]) { ID3D11Texture2D_Release(e->bak[i]); }
    }
    memset(e, 0, sizeof *e);
}

static int ip_sh_find(ID3D11Resource *res)
{
    int i, free_i = -1;
    D3D11_TEXTURE2D_DESC dd;
    ip_sh_t *e;
    HRESULT hr = S_OK;
    for (i = 0; i < IP_SH_MAX; i++) {
        if (g_ish[i].res == res) { return i; }
        if (!g_ish[i].res && free_i < 0) { free_i = i; }
    }
    if (free_i < 0) {
        /* Eintrag ohne gueltige Sicherung einer lebenden Liste verdraengen */
        unsigned best = 0xFFFFFFFFu;
        for (i = 0; i < IP_SH_MAX; i++) {
            int live = 0, k;
            for (k = 0; k < IP_LISTS; k++) {
                if (g_ipl[k].serial && g_ish[i].bakser[k] == g_ipl[k].serial) { live = 1; }
            }
            if (!live && g_ish[i].used < best) { best = g_ish[i].used; free_i = i; }
        }
        if (free_i < 0) { return -1; }
        ip_rt_quiesce(); /* W126 RENDER-THREAD */
        ip_sh_free(&g_ish[free_i]);
    }
    e = &g_ish[free_i];
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &dd);
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &e->sh)) || !e->sh) {
        memset(e, 0, sizeof *e);
        return -1;
    }
    if (dd.BindFlags & D3D11_BIND_RENDER_TARGET) {
        hr = ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)e->sh, NULL, &e->rtv);
    }
    {   /* FIX(SHADOWBUF,W127): typlose Tiefentextur (Schattenpuffer, W100:
         * R24G8_TYPELESS mit DSV + SRV) braucht explizite View-Formate - mit
         * NULL-Beschreibung schlug die Schattenkopie fehl, jede Tick-Liste mit
         * Tiefenpass war snap_ok=0 und wurde verworfen (W100_oni2: pairs=0).
         * Typisierte Ressourcen wie bisher (NULL). */
        DXGI_FORMAT vs = DXGI_FORMAT_UNKNOWN, vd = DXGI_FORMAT_UNKNOWN;
        if (dd.Format == DXGI_FORMAT_R24G8_TYPELESS) { vs = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; vd = DXGI_FORMAT_D24_UNORM_S8_UINT; }
        else if (dd.Format == DXGI_FORMAT_R32_TYPELESS) { vs = DXGI_FORMAT_R32_FLOAT; vd = DXGI_FORMAT_D32_FLOAT; }
        else if (dd.Format == DXGI_FORMAT_R16_TYPELESS) { vs = DXGI_FORMAT_R16_UNORM; vd = DXGI_FORMAT_D16_UNORM; }
        if (SUCCEEDED(hr) && (dd.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sv;
            memset(&sv, 0, sizeof sv);
            sv.Format = vs;
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = dd.MipLevels ? dd.MipLevels : 1u;
            hr = ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)e->sh,
                                                       vs != DXGI_FORMAT_UNKNOWN ? &sv : NULL, &e->srv);
        }
        if (SUCCEEDED(hr) && (dd.BindFlags & D3D11_BIND_DEPTH_STENCIL)) {
            D3D11_DEPTH_STENCIL_VIEW_DESC dv;
            memset(&dv, 0, sizeof dv);
            dv.Format = vd;
            dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            hr = ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource *)e->sh,
                                                     vd != DXGI_FORMAT_UNKNOWN ? &dv : NULL, &e->dsv);
        }
    }
    for (i = 0; SUCCEEDED(hr) && i < IP_LISTS; i++) {
        hr = ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &e->bak[i]);
    }
    if (FAILED(hr)) {
        static unsigned nfail; /* W127: Fehlschlag sichtbar (vorher stumm -> Liste verworfen) */
        if (nfail++ < 8u) {
            HLOG_W("INTERP Schatten fehlgeschlagen: %ux%u fmt=%u bind=0x%X hr=0x%08X", dd.Width, dd.Height,
                   (unsigned)dd.Format, dd.BindFlags, (unsigned)hr);
        }
        ip_sh_free(e);
        return -1;
    }
    e->res = res;
    ID3D11Resource_AddRef(res);
    if (++ip_sh_gen == 0u) { ip_sh_gen = 1u; } /* W361 */
    e->gen = ip_sh_gen;
    if (g_cfg.interp_diag >= 1) {
        static unsigned nlog;
        if (nlog++ < 48u) {
            HLOG_I("INTERP Schatten #%d: %ux%u fmt=%u bind=0x%X", free_i, dd.Width, dd.Height,
                   (unsigned)dd.Format, dd.BindFlags);
        }
    }
    return free_i;
}

/* Ressource der View fuer Liste L (Index li) sichern (einmal je Liste),
 * Eintrag liefern. need_rt: Textur -> nur Render-Ziele haben Schatten. */
static int ip_sh_ref(ip_list_t *L, int li, ID3D11View *v, int need_rt)
{
    ID3D11Resource *res = NULL;
    int e, j;
    if (!v) { return -1; }
    ID3D11View_GetResource(v, &res);
    if (!res) { L->snap_ok = 0; return -1; }
    if (need_rt) {
        D3D11_TEXTURE2D_DESC dd;
        D3D11_RESOURCE_DIMENSION dim;
        ID3D11Resource_GetType(res, &dim);
        if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) { ID3D11Resource_Release(res); return -1; }
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &dd);
        /* FIX(SHADOWBUF,W127): Schattenpuffer (Tiefen-RT als Textur der Stufe 1)
         * bekommt ebenfalls einen Schatten - die Wiedergabe sampelt dann den in
         * der Wiedergabe gezeichneten Tiefenpass statt des Spielstands. */
        if (!(dd.BindFlags & D3D11_BIND_RENDER_TARGET) &&
            !(g_cfg.shade_shadow && (dd.BindFlags & D3D11_BIND_DEPTH_STENCIL))) { ID3D11Resource_Release(res); return -1; }
    }
    e = ip_sh_find(res);
    if (e < 0) { ID3D11Resource_Release(res); L->snap_ok = 0; return -1; }
    for (j = 0; j < L->nrefs && L->refs[j] != e; j++) {}
    if (j == L->nrefs) {
        if (L->nrefs >= IP_REFS_MAX) { ID3D11Resource_Release(res); L->snap_ok = 0; return -1; }
        /* W361: Views selbst halten, Generation merken (Abgabe in ip_list_unref) */
        ip_shref_take(&L->refo[L->nrefs], (ID3D11View *)g_ish[e].rtv, (ID3D11View *)g_ish[e].dsv,
                      (ID3D11View *)g_ish[e].srv, g_ish[e].gen);
        L->refs[L->nrefs++] = e;
        if (g_cfg.imlock_diag) { /* DIAG(LOC-IM-LOCK) */
            LONGLONG cq0 = ip_now();
            ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_ish[e].bak[li], res);
            isw_cp_q += ip_now() - cq0; isw_cp_n++;
        } else
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_ish[e].bak[li], res);
        g_ish[e].bakser[li] = L->serial;
        g_ish[e].used = L->serial;
    }
    ID3D11Resource_Release(res);
    return e;
}

/* DIAG(W340 LOC-FRAMES): Texturen ohne Schatten, die Draws sampeln (bis 24 verschiedene, je Ressource einmal) */
static void li_srv_note(const ip_op_t *o, unsigned i)
{
    static void *seen[24];
    static unsigned nseen;
    ID3D11Resource *res = NULL;
    D3D11_TEXTURE2D_DESC dd;
    D3D11_RESOURCE_DIMENSION dim;
    unsigned k;
    if (nseen >= 24u || !o->srv) { return; }
    ID3D11View_GetResource((ID3D11View *)o->srv, &res);
    if (!res) { return; }
    for (k = 0; k < nseen && seen[k] != (void *)res; k++) {}
    if (k == nseen) {
        ID3D11Resource_GetType(res, &dim);
        memset(&dd, 0, sizeof dd);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) { ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &dd); }
        seen[nseen++] = (void *)res;
        HLOG_I("LOCINTERP srv ohne Schatten #%u: op=%u %ux%u fmt=%u bind=0x%X ziel=%s cls=%u n=%u f=%u", nseen, i, dd.Width,
               dd.Height, (unsigned)dd.Format, dd.BindFlags, o->rtv == g_rtv ? "BB" : "RT", o->cls, o->nv, g_present_no);
    }
    ID3D11Resource_Release(res);
}

/* Am Swap: alle Ressourcen der Liste sichern, Schatten-Views eintragen. */
/* LOC-IM-LOCK im_lock_split=1: Vorab-Einordnung der Textur-Quellen fuer ip_list_snap OHNE ctx_lock (Spiel-Thread am
 * Anfang von sb_interp_swap). Messung (imlock_diag, Zeile IMLOCK swap): ip_list_snap hielt die Sperre je Swap 1-2 ms
 * (bis 10 ms), fast nur fuer GetResource/GetType/GetDesc/Release je Draw-Textur, um festzustellen, dass eine
 * gewoehnliche Textur keinen Schatten braucht (ip_sh_ref need_rt=1 -> -1 ohne Seiteneffekt). Diese Abfragen betreffen
 * nur Device-Objekte (frei-threaded), keinen Context; die Aufnahmeliste L gehoert dem Spiel-Thread (nicht in der
 * Historie, der Present-Takt liest sie nicht), sb_interp_reset laeuft nur auf diesem Thread. Ergebnis je Op: Bit 0 =
 * srv ohne Schatten, Bit k = rq-Stufe k ohne Schatten; ip_list_snap ueberspringt dann genau diese ip_sh_ref-Aufrufe
 * (gleiches Ergebnis, gleiche Liste). */
static unsigned char ip_snap_cls[IP_OPS_MAX];
static const ip_list_t *ip_snap_cls_L;
static unsigned ip_snap_cls_n;
static int ip_view_no_shadow(ID3D11View *v) /* 1 = ip_sh_ref(..., need_rt=1) liefert sicher -1 ohne Seiteneffekt */
{
    ID3D11Resource *res = NULL;
    D3D11_RESOURCE_DIMENSION dim;
    D3D11_TEXTURE2D_DESC dd;
    int r;
    ID3D11View_GetResource(v, &res);
    if (!res) { return 0; } /* ip_sh_ref entscheidet (snap_ok = 0) */
    ID3D11Resource_GetType(res, &dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) { r = 1; }
    else {
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &dd);
        r = !(dd.BindFlags & D3D11_BIND_RENDER_TARGET) && !(g_cfg.shade_shadow && (dd.BindFlags & D3D11_BIND_DEPTH_STENCIL));
    }
    ID3D11Resource_Release(res);
    return r;
}
static void ip_snap_classify(const ip_list_t *L)
{
    unsigned i, n = L->nops;
    ID3D11View *lv = NULL;
    int lr = 0;
    ip_snap_cls_L = NULL;
    if (n > IP_OPS_MAX) { return; }
    for (i = 0; i < n; i++) {
        const ip_op_t *o = &L->ops[i];
        unsigned char c = 0;
        if (o->kind != IP_COPY) {
            if (o->srv) {
                if ((ID3D11View *)o->srv != lv) { lv = (ID3D11View *)o->srv; lr = ip_view_no_shadow(lv); }
                if (lr) { c |= 1u; }
            }
            if (o->rq) {
                int k2;
                for (k2 = 1; k2 < 4; k2++) {
                    if (o->rqr.srv[k2] && ip_view_no_shadow((ID3D11View *)o->rqr.srv[k2])) { c |= (unsigned char)(1u << k2); }
                }
            }
        }
        ip_snap_cls[i] = c;
    }
    ip_snap_cls_n = n;
    ip_snap_cls_L = L;
}

static void ip_list_snap(ip_list_t *L, int li)
{
    unsigned i;
    ID3D11View *lv[3] = { NULL, NULL, NULL };
    int le[3] = { -1, -1, -1 };
    const int cls = (ip_snap_cls_L == L && ip_snap_cls_n == L->nops); /* LOC-IM-LOCK: Vorab-Einordnung gilt */
    LONGLONG uq0 = g_cfg.imlock_diag ? ip_now() : 0;
    ip_snap_cls_L = NULL;
    L->snap_ok = 1;
    ip_list_unref(L); /* W361 */
    if (uq0) { isw_un_q += ip_now() - uq0; }
    L->nrefs = 0;
    L->bbref = ip_sh_ref(L, li, (ID3D11View *)g_rtv, 0);
    for (i = 0; i < L->nops; i++) {
        ip_op_t *o = &L->ops[i];
        ID3D11View *vv[3];
        int q;
        /* LOC-IM-LOCK im_lock_split=1: alle 64 Ops dem Present-Takt die Sperre ueberlassen, wenn er wartet (sb_ctx_yield:
         * nur aeusserste Ebene). Sicher wie die Zuordnung ohne Sperre (swap_match_unlock): L ist noch nicht in der Historie,
         * Schreibzugriffe gehen nur auf Eintraege/Sicherungen von L (bak[li]); der Takt-Thread stellt den Pipeline-Zustand
         * nach seiner Wiedergabe wieder her. Messung: Snap 1-2 ms (bis 10 ms) je Swap am Stueck. */
        if (g_cfg.im_lock_split > 0 && (i & 63u) == 63u) { sb_ctx_yield(); }
        if (o->kind == IP_COPY) { /* W340: Quelle = Schatten des BB, Ziel = Schatten der Textur (braucht RT-Bindung) */
            o->cp_es = ip_sh_ref(L, li, (ID3D11View *)o->cps, 0);
            o->cp_ed = ip_sh_ref(L, li, (ID3D11View *)o->cpd, 1);
            if (o->cp_es >= 0 && o->cp_ed >= 0) { InterlockedIncrement(&li_cp_ok); }
            continue;
        }
        vv[0] = (ID3D11View *)o->rtv;
        vv[1] = (ID3D11View *)o->dsv;
        vv[2] = (ID3D11View *)o->srv;
        o->sh_rtv = NULL;
        o->sh_dsv = NULL;
        o->sh_srv = o->srv;
        for (q = 0; q < 3; q++) {
            int e;
            if (!vv[q]) { continue; }
            if (vv[q] == lv[q]) { e = le[q]; } /* gleiche View wie zuvor (lebt in der Liste) */
            else if (q == 2 && cls && (ip_snap_cls[i] & 1u)) { e = -1; lv[q] = vv[q]; le[q] = e; } /* LOC-IM-LOCK: kein RT */
            else if (g_cfg.imlock_diag) { LONGLONG gq0 = ip_now(); e = ip_sh_ref(L, li, vv[q], q == 2); isw_gr_q += ip_now() - gq0; isw_gr_n++; lv[q] = vv[q]; le[q] = e; } /* DIAG */
            else { e = ip_sh_ref(L, li, vv[q], q == 2); lv[q] = vv[q]; le[q] = e; }
            if (q == 0) { o->sh_rtv = (e >= 0) ? g_ish[e].rtv : NULL; }
            else if (q == 1) { o->sh_dsv = (e >= 0) ? g_ish[e].dsv : NULL; }
            else if (e >= 0) { o->sh_srv = g_ish[e].srv; }
            else if (g_sba_loc && g_cfg.interp_diag >= 1) { li_srv_note(o, i); } /* DIAG(W340) */
        }
        if (o->rq) { /* RENDER-QA: Stufen 1..3 (RT-Texturen bekommen Schatten) */
            int k2;
            for (k2 = 1; k2 < 4; k2++) {
                int e2;
                o->rq_shsrv[k2] = o->rqr.srv[k2];
                if (!o->rqr.srv[k2]) { continue; }
                if (cls && (ip_snap_cls[i] & (1u << k2))) { continue; } /* LOC-IM-LOCK: kein RT */
                e2 = ip_sh_ref(L, li, (ID3D11View *)o->rqr.srv[k2], 1);
                if (e2 >= 0) { o->rq_shsrv[k2] = g_ish[e2].srv; }
            }
        }
        if ((o->rtv && !o->sh_rtv) || (o->dsv && !o->sh_dsv)) { L->snap_ok = 0; }
    }
    if (L->bbref < 0) { L->snap_ok = 0; }
}

/* PERF(RENDER-THREAD,W126): Ziel-Context einer Wiedergabe. Immediate (Spiel-
 * Thread, bisheriger Pfad) oder Deferred (Render-Thread, eigene Konstanten-
 * puffer; Zustand beginnt leer, wird nicht gesichert/wiederhergestellt). */
typedef struct {
    ID3D11DeviceContext *c;
    ID3D11DeviceContext1 *c1;
    ID3D11Buffer *cb0, *cb1;
    int deferred;
} ip_cx_t;

/* Schatten vor der Wiedergabe von B auf den Tick-Anfang setzen: Stand vom
 * Ende des Vorticks A (falls dort gesichert), sonst Ende von B (in A nicht
 * benutzt -> seit davor unveraendert bzw. wird in B neu gezeichnet). */
static const unsigned char *ip_sh_noskip; /* W146: Eintraege ohne Startkopie (NULL = alle kopieren) */
static void ip_sh_start(ip_list_t *B, int bi, ip_list_t *A, int ai, const ip_cx_t *x)
{
    int j;
    for (j = 0; j < B->nrefs; j++) {
        ip_sh_t *e = &g_ish[B->refs[j]];
        ID3D11Texture2D *src = NULL;
        if (ip_sh_noskip && ip_sh_noskip[B->refs[j]]) { continue; } /* W146 GPU-COST: erst Clear bzw. ruhig */
        if (A && ai >= 0 && e->bakser[ai] == A->serial) { src = e->bak[ai]; }
        else if (e->bakser[bi] == B->serial) { src = e->bak[bi]; }
        if (src) {
            ID3D11DeviceContext_CopyResource(x->c, (ID3D11Resource *)e->sh, (ID3D11Resource *)src);
        }
    }
}

/* ---------- Wiedergabe (in Schatten-Ziele) ---------- */

typedef struct {
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    ID3D11BlendState *bs;
    float bf[4];
    UINT smask;
    ID3D11DepthStencilState *ds;
    UINT sref;
    ID3D11RasterizerState *rs;
    D3D11_VIEWPORT vp;
    UINT nvp;
    D3D11_RECT sc;
    UINT nsc;
    ID3D11ShaderResourceView *srv;
    ID3D11Buffer *vb[2];
    UINT vbs[2], vbo[2];
    ID3D11InputLayout *lay;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11Buffer *vcb[2], *pcb;
    ID3D11SamplerState *smp[4];          /* RENDER-QA: Shading-Wiedergabe setzt s0..s3 */
    ID3D11ShaderResourceView *srvx[3];   /* t1..t3 */
    ID3D11Buffer *vb2; UINT vbs2, vbo2;  /* RENDER-QA: Stream 2 */
    ID3D11Buffer *vcb3, *pcb4;           /* RENDER-QA: Konstanten-Fenster b3/b4 */
    ID3D11ShaderResourceView *vsrv0;     /* SHADE-PERF (W80): Roh-Eingaenge VS-t0 */
    UINT vf3, vn3, pf4, pn4;
} ip_state_t;

static void ip_state_save(ip_state_t *s)
{
    memset(s, 0, sizeof *s);
    ID3D11DeviceContext_OMGetRenderTargets(g_ctx, 1, &s->rtv, &s->dsv);
    ID3D11DeviceContext_OMGetBlendState(g_ctx, &s->bs, s->bf, &s->smask);
    ID3D11DeviceContext_OMGetDepthStencilState(g_ctx, &s->ds, &s->sref);
    ID3D11DeviceContext_RSGetState(g_ctx, &s->rs);
    s->nvp = 1;
    ID3D11DeviceContext_RSGetViewports(g_ctx, &s->nvp, &s->vp);
    s->nsc = 1;
    ID3D11DeviceContext_RSGetScissorRects(g_ctx, &s->nsc, &s->sc);
    ID3D11DeviceContext_PSGetShaderResources(g_ctx, 0, 1, &s->srv);
    ID3D11DeviceContext_IAGetVertexBuffers(g_ctx, 0, 2, s->vb, s->vbs, s->vbo);
    ID3D11DeviceContext_IAGetInputLayout(g_ctx, &s->lay);
    ID3D11DeviceContext_IAGetPrimitiveTopology(g_ctx, &s->topo);
    ID3D11DeviceContext_VSGetShader(g_ctx, &s->vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(g_ctx, &s->ps, NULL, NULL);
    ID3D11DeviceContext_VSGetConstantBuffers(g_ctx, 0, 2, s->vcb);
    ID3D11DeviceContext_PSGetConstantBuffers(g_ctx, 0, 1, &s->pcb);
    ID3D11DeviceContext_PSGetSamplers(g_ctx, 0, 4, s->smp);
    ID3D11DeviceContext_PSGetShaderResources(g_ctx, 1, 3, s->srvx);
    /* RENDER-QA (W77): Wiedergaben laufen im Echtzeit-Takt auch zwischen
     * rq_bind und dem Draw eines Shading-Draws (ip_poll in rec_draw) ->
     * Stream 2 und die Konstanten-Fenster ebenfalls sichern. */
    if (g_cfg.shading) { ID3D11DeviceContext_IAGetVertexBuffers(g_ctx, 2, 1, &s->vb2, &s->vbs2, &s->vbo2); }
    if (g_cfg.shading) { ID3D11DeviceContext_VSGetShaderResources(g_ctx, 0, 1, &s->vsrv0); }
    if (g_rq_ctx1) {
        ID3D11DeviceContext1_VSGetConstantBuffers1(g_rq_ctx1, 3, 1, &s->vcb3, &s->vf3, &s->vn3);
        ID3D11DeviceContext1_PSGetConstantBuffers1(g_rq_ctx1, 4, 1, &s->pcb4, &s->pf4, &s->pn4);
    }
}

static void ip_state_restore(ip_state_t *s)
{
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &s->rtv, s->dsv);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, s->bs, s->bf, s->smask);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, s->ds, s->sref);
    ID3D11DeviceContext_RSSetState(g_ctx, s->rs);
    if (s->nvp) { ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &s->vp); }
    if (s->nsc) { ID3D11DeviceContext_RSSetScissorRects(g_ctx, 1, &s->sc); }
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &s->srv);
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 2, s->vb, s->vbs, s->vbo);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, s->lay);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, s->topo);
    ID3D11DeviceContext_VSSetShader(g_ctx, s->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, s->ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 0, 2, s->vcb);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &s->pcb);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 4, s->smp);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 1, 3, s->srvx);
    if (g_cfg.shading) { ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 2, 1, &s->vb2, &s->vbs2, &s->vbo2); }
    if (s->vb2) { ID3D11Buffer_Release(s->vb2); }
    if (g_cfg.shading) { ID3D11DeviceContext_VSSetShaderResources(g_ctx, 0, 1, &s->vsrv0); }
    if (s->vsrv0) { ID3D11ShaderResourceView_Release(s->vsrv0); }
    if (g_rq_ctx1) {
        if (s->vcb3) {
            ID3D11DeviceContext1_VSSetConstantBuffers1(g_rq_ctx1, 3, 1, &s->vcb3, &s->vf3, &s->vn3);
            ID3D11Buffer_Release(s->vcb3);
        }
        if (s->pcb4) {
            ID3D11DeviceContext1_PSSetConstantBuffers1(g_rq_ctx1, 4, 1, &s->pcb4, &s->pf4, &s->pn4);
            ID3D11Buffer_Release(s->pcb4);
        }
    }
    {
        int k;
        for (k = 0; k < 4; k++) { if (s->smp[k]) { ID3D11SamplerState_Release(s->smp[k]); } }
        for (k = 0; k < 3; k++) { if (s->srvx[k]) { ID3D11ShaderResourceView_Release(s->srvx[k]); } }
    }
    if (s->rtv) { ID3D11RenderTargetView_Release(s->rtv); }
    if (s->dsv) { ID3D11DepthStencilView_Release(s->dsv); }
    if (s->bs) { ID3D11BlendState_Release(s->bs); }
    if (s->ds) { ID3D11DepthStencilState_Release(s->ds); }
    if (s->rs) { ID3D11RasterizerState_Release(s->rs); }
    if (s->srv) { ID3D11ShaderResourceView_Release(s->srv); }
    if (s->vb[0]) { ID3D11Buffer_Release(s->vb[0]); }
    if (s->vb[1]) { ID3D11Buffer_Release(s->vb[1]); }
    if (s->lay) { ID3D11InputLayout_Release(s->lay); }
    if (s->vs) { ID3D11VertexShader_Release(s->vs); }
    if (s->ps) { ID3D11PixelShader_Release(s->ps); }
    if (s->vcb[0]) { ID3D11Buffer_Release(s->vcb[0]); }
    if (s->vcb[1]) { ID3D11Buffer_Release(s->vcb[1]); }
    if (s->pcb) { ID3D11Buffer_Release(s->pcb); }
}

static void ip_cb_write(const ip_cx_t *x, ID3D11Buffer *cb, const float m[16], unsigned u, float f, int isf)
{
    D3D11_MAPPED_SUBRESOURCE map;
    HRESULT hrm;
    WD_CALL("ip_cb_map", NULL, hrm = ID3D11DeviceContext_Map(x->c, (ID3D11Resource *)cb, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &map)); /* DIAG(PERF-WARM,W139) */
    if (FAILED(hrm)) {
        return;
    }
    memcpy(map.pData, m, 64);
    if (isf) { *(float *)((unsigned char *)map.pData + 64) = f; }
    else { *(unsigned *)((unsigned char *)map.pData + 64) = u; }
    ID3D11DeviceContext_Unmap(x->c, (ID3D11Resource *)cb, 0);
}

/* ---------- PERF(PERF-M2,W174): Konstanten-Ring, Zaehler, Diagnose-Varianten ----------
 * ip_cb_ring=1: Statt je Draw b0 (und b1 bei lerp) per Map(DISCARD) eines
 * 80-Byte-Puffers zu erneuern (3400 Draws -> ~5000 Maps je Wiedergabe), schreibt
 * die Wiedergabe vorab alle Draw-Konstanten in einen Ring-Puffer (ein Map je
 * Wiedergabe, 256-Byte-Plaetze, gleiche aufeinanderfolgende Saetze nur einmal)
 * und bindet je Draw nur den Versatz (VS/PS-SetConstantBuffers1). Gleiche
 * Bytes im gelesenen Bereich (mvp, has_tex bzw. ip_t, cw-Parameter) -> gleiche
 * Bilder. Nicht mit VR-Augen/Diagnosebild (alter Pfad). */
#define IP_RING_SLOT 256u
typedef struct {
    ID3D11Buffer *buf;
    unsigned cap;          /* Plaetze */
    unsigned *ofs;         /* je Op 2 Plaetze (b0, b1); ~0 = keiner */
    unsigned ofscap;
} ip_ring_t;
static ip_ring_t ip_ring[2]; /* 0 = Immediate, 1 = Deferred (Render-Thread) */

/* DIAG(PERF-M2,W174) ip_gdiag: Zaehler je Wiedergabe */
typedef struct {
    unsigned nops, ndraw, nlerp, nrq, nclr, nmap, nrtv, nsrv, ncb, slots, var, nverts, rqxb, ring, vbg;
    unsigned long long vbytes;
} ip_rc_t;
static ip_rc_t ip_rc_ctx[2];

static void ip_la_mul(float out[16], const float m[16], const float *C); /* W376 */
static int ip_ring_fill(const ip_cx_t *x, const ip_list_t *B, const ip_list_t *A, int lerp_ok, float t, ip_rc_t *rc,
                        int laOn, float laM[LA_NM][16])
{
    ip_ring_t *R = &ip_ring[x->deferred ? 1 : 0];
    unsigned i, need = 0, n = 0, last0 = 0xFFFFFFFFu, last1 = 0xFFFFFFFFu;
    unsigned char r[80], p0[80], p1[80];
    D3D11_MAPPED_SUBRESOURCE m;
    unsigned char *d;
    HRESULT hr;
    if (!x->c1 || !B->nops) { return 0; }
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        if (o->kind != IP_DRAW) { continue; }
        need += (lerp_ok && o->flag && o->match >= 0 && (unsigned)o->match < A->nops) ? 2u : 1u;
    }
    if (!need) { return 0; }
    if (R->ofscap < 2u * B->nops) {
        unsigned nc = 2u * B->nops + 1024u;
        unsigned *no = (unsigned *)realloc(R->ofs, (size_t)nc * sizeof *no);
        if (!no) { return 0; }
        R->ofs = no;
        R->ofscap = nc;
    }
    if (!R->buf || R->cap < need) {
        D3D11_BUFFER_DESC bd;
        unsigned nc = need + need / 4u;
        if (nc < 4096u) { nc = 4096u; }
        if (R->buf) { ID3D11Buffer_Release(R->buf); R->buf = NULL; R->cap = 0; }
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = nc * IP_RING_SLOT;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &R->buf))) { R->buf = NULL; return 0; }
        R->cap = nc;
        HLOG_I("INTERP cb_ring: Ring-Puffer %s %u Plaetze (%u KB)", x->deferred ? "Render-Thread" : "Spiel-Thread", nc,
               nc * IP_RING_SLOT / 1024u);
    }
    WD_CALL("ip_ring_map", NULL, hr = ID3D11DeviceContext_Map(x->c, (ID3D11Resource *)R->buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m));
    if (FAILED(hr)) { return 0; }
    d = (unsigned char *)m.pData;
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        R->ofs[2u * i] = R->ofs[2u * i + 1u] = 0xFFFFFFFFu;
        if (o->kind != IP_DRAW) { continue; }
        if (LA_ON(laOn, o->law)) { float lm[16]; ip_la_mul(lm, o->mvp, laM[o->law - 1]); memcpy(r, lm, 64); } /* W376, W394 */
        else { memcpy(r, o->mvp, 64); }
        memcpy(r + 64, &o->has_tex, 4);
        memset(r + 68, 0, 12); /* Padding (frueher W166-Parameter, entfernt in W181) */
        if (last0 == 0xFFFFFFFFu || memcmp(r, p0, 80) != 0) {
            memcpy(d + (size_t)n * IP_RING_SLOT, r, 80);
            memcpy(p0, r, 80);
            last0 = n++;
        }
        R->ofs[2u * i] = last0;
        if (lerp_ok && o->flag && o->match >= 0 && (unsigned)o->match < A->nops) {
            if (LA_ON(laOn, o->law)) { float lm[16]; ip_la_mul(lm, IP_AMVP(o, &A->ops[o->match]), laM[o->law - 1]); memcpy(r, lm, 64); } /* W376, W394 */
            else { memcpy(r, IP_AMVP(o, &A->ops[o->match]), 64); } /* W317 rzm */
            memcpy(r + 64, &t, 4);
            if (last1 == 0xFFFFFFFFu || memcmp(r, p1, 80) != 0) {
                memcpy(d + (size_t)n * IP_RING_SLOT, r, 80);
                memcpy(p1, r, 80);
                last1 = n++;
            }
            R->ofs[2u * i + 1u] = last1;
        }
    }
    ID3D11DeviceContext_Unmap(x->c, (ID3D11Resource *)R->buf, 0);
    rc->slots = n;
    rc->nmap++;
    return 1;
}

/* Versatz binden (nur bei Wechsel). which: 0 = b0 (VS+PS), 1 = b1 (VS). */
static void ip_ring_bind(const ip_cx_t *x, unsigned slot, int which, unsigned cur[3], ip_rc_t *rc)
{
    ID3D11Buffer *b = ip_ring[x->deferred ? 1 : 0].buf;
    UINT f = slot * (IP_RING_SLOT / 16u), nc = IP_RING_SLOT / 16u;
    if (slot == 0xFFFFFFFFu) { return; }
    if (which == 0) {
        if (cur[0] != slot) { ID3D11DeviceContext1_VSSetConstantBuffers1(x->c1, 0, 1, &b, &f, &nc); cur[0] = slot; rc->ncb++; }
        if (cur[2] != slot) { ID3D11DeviceContext1_PSSetConstantBuffers1(x->c1, 0, 1, &b, &f, &nc); cur[2] = slot; rc->ncb++; }
    } else if (cur[1] != slot) {
        ID3D11DeviceContext1_VSSetConstantBuffers1(x->c1, 1, 1, &b, &f, &nc);
        cur[1] = slot;
        rc->ncb++;
    }
}

/* DIAG(PERF-M2,W174) ip_ablate: Variante dieser Wiedergabe (0 = normal). Im
 * Wechsel je Tick und t, damit alle Varianten dieselbe Szene/Last sehen. */
static unsigned ip_var_pick(const ip_list_t *B, float t)
{
    unsigned ab = (unsigned)g_cfg.ip_ablate & 127u, vars[8], nv = 0, k;
    if (!ab || t <= 0.0f || t >= 1.0f) { return 0u; }
    vars[nv++] = 0u;
    for (k = 1u; k <= 64u; k <<= 1) { if (ab & k) { vars[nv++] = k; } }
    k = (unsigned)(t * 8.0f + 0.5f) >> 1;
    return vars[(B->serial + k) % nv];
}

/* Tick B in die Schatten wiedergeben (Anfangsstand aus A bzw. B);
 * 0<t<1 und gueltiges Paar -> Welt-Draws mit flag=1 lerpen. */
static const float kIP_DBG_COL[IP_W_N][3] = {
    { 1, 1, 1 }, { 0.5f, 0.5f, 0.5f }, { 0, 1, 0 }, { 0, 1, 1 }, { 1, 0, 0 },
    { 1, 1, 0 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 0, 1 }, { 1, 0.5f, 0 } };

/* dbg = 1 (interp_diag>=3): Welt-/RT-Draws in der Farbe ihres Zuordnungs-
 * Ergebnisses (kIP_DBG_COL), 2D im BB normal (Monitore zeigen die RTs). */
static void ip_replay_ex(ip_list_t *B, int bi, ip_list_t *A, int ai, float t, int dbg, const ip_cx_t *x);
static const ip_cx_t *ip_cx_imm(void);
static void ip_replay(ip_list_t *B, int bi, ip_list_t *A, int ai, float t)
{
    ip_replay_ex(B, bi, A, ai, t, 0, ip_cx_imm());
}

/* DIAG(TICK-PERF,W125) perf_gsplit=N: jede N-te Zwischenbild-Wiedergabe mit
 * Timestamps an jedem Zielwechsel; synchron ausgewertet (Diagnose, kostet
 * einen GPU-Sync je Messung). Zeile GPUSPLIT: Summe je Zielgroesse. */
#define IP_GS_MAX 512
static ID3D11Query *ip_gs_dj, *ip_gs_q[IP_GS_MAX];
static ID3D11RenderTargetView *ip_gs_rtv[IP_GS_MAX];
static unsigned ip_gs_draws[IP_GS_MAX], ip_gs_rq[IP_GS_MAX];
static int ip_gs_n = -1; /* -1 = keine Messung in dieser Wiedergabe */
static unsigned ip_gs_cnt;

static void ip_gs_mark(ID3D11RenderTargetView *rtv)
{
    if (ip_gs_n < 0 || ip_gs_n >= IP_GS_MAX) { return; }
    if (!ip_gs_q[ip_gs_n]) {
        D3D11_QUERY_DESC qd;
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &ip_gs_q[ip_gs_n]))) { ip_gs_n = -1; return; }
    }
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)ip_gs_q[ip_gs_n]);
    ip_gs_rtv[ip_gs_n] = rtv;
    ip_gs_draws[ip_gs_n] = 0;
    ip_gs_rq[ip_gs_n] = 0;
    ip_gs_n++;
}

static int ip_gs_begin(float t)
{
    D3D11_QUERY_DESC qd;
    ip_gs_n = -1;
    if (g_cfg.perf_gsplit <= 0 || t >= 1.0f || !rq_in_mission()) { return 0; }
    if ((++ip_gs_cnt % (unsigned)g_cfg.perf_gsplit) != 0u) { return 0; }
    if (!ip_gs_dj) {
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &ip_gs_dj))) { ip_gs_dj = NULL; return 0; }
    }
    ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous *)ip_gs_dj);
    ip_gs_n = 0;
    ip_gs_mark(NULL); /* Segment 0 = Schatten-Startkopien */
    return 1;
}

static void ip_gs_end(unsigned nops)
{
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    UINT64 ts[IP_GS_MAX];
    int i, n, k, nk = 0;
    struct { unsigned w, h, segs, draws, rq; double ms; } ag[24];
    double tot, copy;
    char line[1400];
    int o = 0;
    DWORD t0;
    if (ip_gs_n < 0) { return; }
    ip_gs_mark((ID3D11RenderTargetView *)1); /* Ende */
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)ip_gs_dj);
    n = ip_gs_n;
    ip_gs_n = -1;
    t0 = GetTickCount();
    while (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)ip_gs_dj, &dj, sizeof dj, 0) == S_FALSE) {
        if (GetTickCount() - t0 > 500u) { return; }
        YieldProcessor();
    }
    if (dj.Disjoint || !dj.Frequency || n < 2) { return; }
    for (i = 0; i < n; i++) {
        if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)ip_gs_q[i], &ts[i], sizeof ts[i], 0) != S_OK) { return; }
    }
    memset(ag, 0, sizeof ag);
    tot = (double)(ts[n - 1] - ts[0]) * 1000.0 / (double)dj.Frequency;
    copy = (double)(ts[1] - ts[0]) * 1000.0 / (double)dj.Frequency;
    for (i = 1; i < n - 1; i++) {
        unsigned w = 0, h = 0;
        double ms = (double)(ts[i + 1] - ts[i]) * 1000.0 / (double)dj.Frequency;
        if ((uintptr_t)ip_gs_rtv[i] > 16u) { /* 1/2 = Marken */
            ID3D11Resource *r = NULL;
            D3D11_TEXTURE2D_DESC td;
            ID3D11View_GetResource((ID3D11View *)ip_gs_rtv[i], &r);
            if (r) {
                ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &td);
                w = td.Width; h = td.Height;
                ID3D11Resource_Release(r);
            }
        }
        for (k = 0; k < nk && (ag[k].w != w || ag[k].h != h); k++) {}
        if (k == nk) { if (nk >= 24) { continue; } ag[k].w = w; ag[k].h = h; nk++; }
        ag[k].ms += ms; ag[k].segs++; ag[k].draws += ip_gs_draws[i]; ag[k].rq += ip_gs_rq[i];
    }
    for (k = 0; k < nk && o >= 0; k++) {
        o += _snprintf_s(line + o, sizeof line - (size_t)o, _TRUNCATE, " | %ux%u ms=%.2f segs=%u draws=%u rq=%u",
                         ag[k].w, ag[k].h, ag[k].ms, ag[k].segs, ag[k].draws, ag[k].rq);
    }
    HLOG_I("GPUSPLIT f=%u ops=%u total=%.2fms startkopien=%.2fms segs=%d%s", g_present_no, nops, tot, copy, n - 2, line);
}

/* ---------- W146 GPU-COST: dynamische Render-Skalierung (rt_scale_dyn) ----------
 * GPU-Zeit je Zwischenbild (Wiedergabe + Blit + Nachbearbeitung, asynchrone
 * Timestamps, kein Sync) in 250-ms-Fenstern. Ueber rt_scale_dyn_hi fuer 1 s ->
 * Faktor der Welt-/Monitor-RTs eine Stufe runter (nicht unter rt_scale_dyn_min),
 * unter rt_scale_dyn_lo fuer 8 s -> eine Stufe hoch (nicht ueber den auto-/
 * rt_scale-Faktor). Mindestens 2 s zwischen Wechseln; folgt einem Hochschalten
 * binnen 15 s wieder ein Runterschalten, ruht das Hochschalten 60 s (kein
 * Pendeln). Umgesetzt im Spiel-Thread am Swap-Anfang (sb_rt_dyn_apply). Das
 * Seitenverhaeltnis bleibt (logische RT-Groesse und Abbildung unveraendert). */
#define DY_N 6
static ID3D11Query *dy_dj[DY_N], *dy_t0[DY_N], *dy_t1[DY_N];
static int dy_st[DY_N], dy_open = -1, dy_last_dir, dy_fail;
static double dy_acc, dy_last_avg;
static unsigned dy_n, dy_switches;
static LONGLONG dy_win_q, dy_hi_since, dy_lo_since, dy_last_switch, dy_up_block;
static volatile LONG g_rt_dyn_want;
static unsigned dy_up_veto, dy_wait_n; /* W165: Hochschalten per Prognose verworfen / Wechsel wartet auf Saetze */
static int dy_pref_done;               /* W165: Satz eine Stufe tiefer vorab bestellt */

static void dy_poll(void)
{
    int k;
    for (k = 0; k < DY_N; k++) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 a, b;
        if (dy_st[k] != 2) { continue; }
        if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)dy_dj[k], &dj, sizeof dj,
                                        D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) { continue; }
        dy_st[k] = 0;
        if (dj.Disjoint || !dj.Frequency) { continue; }
        if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)dy_t0[k], &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)dy_t1[k], &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            b < a) { continue; }
        dy_acc += (double)(b - a) * 1000.0 / (double)dj.Frequency;
        dy_n++;
    }
}

static void dy_begin(void)
{
    int k;
    if (g_cfg.rt_scale_dyn <= 0 || dy_fail || dy_open >= 0 || !rq_in_mission()) { return; }
    dy_poll();
    for (k = 0; k < DY_N && dy_st[k] != 0; k++) {}
    if (k >= DY_N) { return; }
    if (!dy_dj[k]) {
        D3D11_QUERY_DESC qd;
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &dy_dj[k]))) { dy_dj[k] = NULL; dy_fail = 1; return; }
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &dy_t0[k])) ||
            FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &dy_t1[k]))) { dy_fail = 1; return; }
    }
    ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous *)dy_dj[k]);
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)dy_t0[k]);
    dy_st[k] = 1;
    dy_open = k;
}

static void dy_ctrl(void)
{
    LONGLONG now = ip_now(), f = ip_qpf.QuadPart;
    unsigned cur, base, mn;
    double avg, hi, lo;
    LONG want = 0;
    if (!dy_win_q) { dy_win_q = now; }
    if (now - dy_win_q < f / 4) { return; }
    dy_win_q = now;
    if (!dy_n) { return; }
    avg = dy_acc / (double)dy_n;
    dy_acc = 0.0;
    dy_n = 0;
    dy_last_avg = avg;
    cur = sb_rtt_dyn_factor();
    base = sb_rtt_base_factor();
    mn = (unsigned)(g_cfg.rt_scale_dyn_min > 0 ? g_cfg.rt_scale_dyn_min : 1);
    if (mn > base) { mn = base; }
    hi = (double)g_cfg.rt_scale_dyn_hi / 10.0;
    lo = (double)g_cfg.rt_scale_dyn_lo / 10.0;
    if (avg > hi) { if (!dy_hi_since) { dy_hi_since = now; } } else { dy_hi_since = 0; }
    if (avg < lo) { if (!dy_lo_since) { dy_lo_since = now; } } else { dy_lo_since = 0; }
    if (dy_last_switch && now - dy_last_switch < 2 * f) { return; }
    if (dy_hi_since && now - dy_hi_since >= f && cur > mn) { want = (LONG)cur - 1; }
    else if (dy_lo_since && now - dy_lo_since >= 8 * f && cur < base && now >= dy_up_block) {
        /* W165 rt_scale_dyn_keep: nur hoch, wenn auch die obere Stufe nach Prognose (Pixel ~ n^2,
         * obere Schranke: alles skaliert) unter der Runter-Schwelle bleibt -> kein Pendeln */
        double pr = avg * (double)((cur + 1u) * (cur + 1u)) / (double)(cur * cur);
        if (g_cfg.rt_scale_dyn_keep <= 0 || pr < hi) { want = (LONG)cur + 1; }
        else { dy_up_veto++; }
    }
    if (want) { InterlockedExchange(&g_rt_dyn_want, want); }
}

static void dy_end(void)
{
    if (dy_open < 0) { return; }
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)dy_t1[dy_open]);
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)dy_dj[dy_open]);
    dy_st[dy_open] = 2;
    dy_open = -1;
    dy_ctrl();
}

void sb_rt_dyn_apply(void)
{
    LONG w;
    unsigned cur;
    int r;
    LONGLONG now;
    if (g_cfg.rt_scale_dyn <= 0) { return; }
    if (g_cfg.rt_scale_dyn_keep > 0 && rq_in_mission()) {
        /* W165: Saetze eine Stufe unter dem aktuellen Faktor vorab im Hilfs-Thread anlegen
         * (einmal je Stufe; neue RTs werden beim Wechsel nachbestellt) */
        unsigned c0 = sb_rtt_dyn_factor(), mn0 = (unsigned)(g_cfg.rt_scale_dyn_min > 0 ? g_cfg.rt_scale_dyn_min : 1);
        if (c0 > mn0 && dy_pref_done != (int)c0) {
            sb_ctx_enter();
            sb_rtt_keep_prefetch(c0 - 1u);
            sb_ctx_leave();
            dy_pref_done = (int)c0;
        }
    }
    w = InterlockedExchange(&g_rt_dyn_want, 0);
    if (!w) { return; }
    cur = sb_rtt_dyn_factor();
    if ((unsigned)w == cur) { return; }
    sb_ctx_enter();
    r = sb_rtt_rescale((unsigned)w);
    sb_ctx_leave();
    if (r == -2) { /* W165 keep: Saetze entstehen noch im Hilfs-Thread -> naechster Swap */
        if (dy_wait_n++ == 0) { HLOG_I("RT-DYN: Faktor %u -> %ld wartet auf vorab angelegte RTs (W165 keep)", cur, w); }
        InterlockedCompareExchange(&g_rt_dyn_want, w, 0);
        return;
    }
    if (r < 0) { InterlockedCompareExchange(&g_rt_dyn_want, w, 0); return; } /* RT gebunden: naechster Swap */
    now = ip_now();
    if ((unsigned)w < cur && dy_last_dir > 0 && now - dy_last_switch < 15 * ip_qpf.QuadPart) {
        dy_up_block = now + 60 * ip_qpf.QuadPart; /* gependelt: 60 s nicht hochschalten */
    }
    dy_last_dir = (unsigned)w > cur ? 1 : -1;
    dy_last_switch = now;
    dy_hi_since = dy_lo_since = 0;
    dy_switches++;
    HLOG_I("RT-DYN: Faktor %u -> %ld (%d RTs neu angelegt, GPU %.2f ms je Zwischenbild, Wechsel #%u, f=%u)",
           cur, w, r, dy_last_avg, dy_switches, g_present_no);
    if (g_cfg.rt_scale_dyn_keep > 0) { /* W165: neue Zeile (obige bleibt fuer Skripte gleich) */
        unsigned mk, jb, sw;
        double jms;
        sb_rtt_keep_stat(&mk, &jb, &sw, &jms);
        HLOG_I("RT-DYN keep: getauscht ohne Neuanlage, Saetze vorab=%u (Auftraege %u, letzter %.1f ms im Hilfs-Thread), "
               "gewartet=%u Swaps, hoch verworfen (Prognose)=%u", mk, jb, jms, dy_wait_n, dy_up_veto);
        dy_wait_n = 0;
    }
}

/* ---------- W146 GPU-COST: Zwischenbild-Cache (interp_cockpit_cache) ----------
 * Ein Zwischenbild zeichnet die komplette Aufzeichnung des Ticks neu. Zwei
 * Teile davon sind fuer alle t eines Paars (A,B) gleich:
 *  a) Ziele (Schatten-Eintraege), deren schreibende Ops in B alle exakt sind
 *     (Clears, Draws mit flag = 0 = exakt aus B, kein REM-FX) und nur feste
 *     Texturen oder selbst solche "ruhigen" Ziele lesen - typisch Sub-Monitore,
 *     Karte, Komposition. Ihr Endstand ist der gesicherte Tick-Stand bak[B]
 *     (Start = Ende von A = Start von B im Spiel). Bedingung: kein Lesen des
 *     Ziels vor seinem letzten Schreiben in B. -> Ops ueberspringen, bak[B] in
 *     den Schatten kopieren (Modus "ruhig", Bit 1 von interp_cockpit_cache).
 *  b) Der Anfang der Backbuffer-Ops (Clear, Cockpit-Gehaeuse ...), solange jede
 *     BB-Op exakt ist und nur feste Texturen/ruhige Ziele liest (Bit 2). Im
 *     ersten Zwischenbild des Paars gezeichnet und gesichert (Schatten-BB +
 *     BB-Tiefe), in den weiteren zurueckkopiert, Ops uebersprungen. Wackelt
 *     das Cockpit (flag = 1), endet der Anfang dort (kein 30-Hz-Wackeln).
 * Tick-Bilder unberuehrt (bak/Spiel-BB), Verify (t = 1), VR, Render-Thread und
 * Diagnosebild ohne Cache. interp_cockpit_cache: 0 aus, 1 = a, 2 = b, 3 = a+b. */
#define CC_MAXOPS 16384
static ID3D11Texture2D *cc_col, *cc_dep;
static unsigned cc_ser_a, cc_ser_b, cc_scan_ser;
static int cc_valid, cc_pfx = -1, cc_dep_e = -1, cc_why, cc_bbe = -1;
static unsigned cc_npfx, cc_nbb, cc_nskip, cc_nquiet;
static ID3D11DepthStencilView *cc_bdsv;
static unsigned char cc_skip[CC_MAXOPS];   /* 1 = Op schreibt nur ruhige Ziele */
static signed char cc_ent[CC_MAXOPS][3];   /* Eintrag rtv/dsv/srv (-1) */
static unsigned char cc_dirty[IP_SH_MAX], cc_wr[IP_SH_MAX];
static unsigned char cc_nosc[IP_SH_MAX];   /* 1 = keine Startkopie noetig (erste Beruehrung = Clear bzw. ruhig) */
static unsigned cc_nnosc, cc_st_nosc;
static unsigned cc_st_built, cc_st_used, cc_st_off, cc_st_why[8], cc_st_pfx, cc_st_bb, cc_st_n, cc_st_skip, cc_st_q,
    cc_st_ops, cc_st_rep;

static int cc_ent_of(const ip_list_t *B, const void *v, int q)
{
    int j;
    if (!v) { return -1; }
    for (j = 0; j < B->nrefs; j++) {
        const ip_sh_t *e = &g_ish[B->refs[j]];
        if ((q == 0 && (const void *)e->rtv == v) || (q == 1 && (const void *)e->dsv == v) ||
            (q == 2 && (const void *)e->srv == v)) { return B->refs[j]; }
    }
    return -1;
}

static int cc_op_exact(const ip_op_t *o)
{
    if (o->kind != IP_DRAW) { return 1; }
    return !o->flag && !(o->has_tex & FXR_MASK);
}

/* liest die Op ein unruhiges Ziel (Schatten-SRV, auch Stufen 1..3)? */
static int cc_reads_dirty(const ip_list_t *B, unsigned i)
{
    const ip_op_t *o = &B->ops[i];
    int k, e;
    if (o->kind != IP_DRAW) { return 0; }
    if (cc_ent[i][2] >= 0 && cc_dirty[cc_ent[i][2]]) { return 1; }
    if (o->rq) {
        for (k = 1; k < 4; k++) {
            if (o->rq_shsrv[k] && o->rq_shsrv[k] != o->rqr.srv[k]) {
                e = cc_ent_of(B, o->rq_shsrv[k], 2);
                if (e < 0 || cc_dirty[e]) { return 1; }
            }
        }
    }
    return 0;
}

static void cc_scan(ip_list_t *B, int bi)
{
    unsigned i, it, k;
    unsigned lastw[IP_SH_MAX];
    int changed;
    cc_scan_ser = B->serial;
    cc_pfx = 0; cc_why = 0; cc_npfx = cc_nbb = cc_nskip = cc_nquiet = 0;
    cc_bdsv = NULL; cc_dep_e = -1; cc_bbe = B->bbref;
    memset(cc_dirty, 0, sizeof cc_dirty);
    memset(cc_wr, 0, sizeof cc_wr);
    memset(lastw, 0, sizeof lastw);
    if (B->nops > CC_MAXOPS) { cc_why = 6; cc_pfx = 0; memset(cc_skip, 0, sizeof cc_skip); return; }
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        cc_ent[i][0] = (signed char)cc_ent_of(B, o->sh_rtv, 0);
        cc_ent[i][1] = (signed char)cc_ent_of(B, o->sh_dsv, 1);
        cc_ent[i][2] = (signed char)((o->kind == IP_DRAW && o->sh_srv != o->srv) ? cc_ent_of(B, o->sh_srv, 2) : -1);
        if (o->kind == IP_DRAW && o->sh_srv != o->srv && cc_ent[i][2] < 0) { cc_ent[i][2] = -2; } /* unbekannt */
        for (k = 0; k < 2; k++) {
            int e = cc_ent[i][k];
            if (e >= 0) { cc_wr[e] = 1; lastw[e] = i + 1u; if (!cc_op_exact(o)) { cc_dirty[e] = 1; } }
        }
        if (!cc_bdsv && o->rtv == g_rtv && o->dsv) { cc_bdsv = o->dsv; cc_dep_e = cc_ent[i][1]; }
    }
    /* gelesene Ziele: Lesen vor dem letzten Schreiben -> Endstand passt nicht */
    for (i = 0; i < B->nops; i++) {
        int r = cc_ent[i][2];
        if (r >= 0 && cc_wr[r] && i < lastw[r]) { cc_dirty[r] = 1; }
    }
    if (cc_bbe >= 0) { cc_dirty[cc_bbe] = 1; } /* BB nie ganz ueberspringen (Anfangs-Cache b) */
    if (cc_dep_e >= 0) { cc_dirty[cc_dep_e] = 1; }
    for (it = 0; it < 16u; it++) { /* Ausbreitung: liest unruhig / gemeinsames Ziel+Tiefe */
        changed = 0;
        for (i = 0; i < B->nops; i++) {
            const ip_op_t *o = &B->ops[i];
            int e0 = cc_ent[i][0], e1 = cc_ent[i][1];
            int dyn = (cc_ent[i][2] == -2) || cc_reads_dirty(B, i) || (e0 >= 0 && cc_dirty[e0]) || (e1 >= 0 && cc_dirty[e1]);
            if (o->rtv && e0 < 0) { dyn = 1; }
            if (o->dsv && e1 < 0) { dyn = 1; }
            if (dyn) {
                if (e0 >= 0 && !cc_dirty[e0]) { cc_dirty[e0] = 1; changed = 1; }
                if (e1 >= 0 && !cc_dirty[e1]) { cc_dirty[e1] = 1; changed = 1; }
            }
        }
        if (!changed) { break; }
    }
    for (k = 0; k < IP_SH_MAX; k++) {
        if (cc_wr[k] && !cc_dirty[k]) {
            if (g_ish[k].bakser[bi] == B->serial && g_ish[k].bak[bi]) { cc_nquiet++; }
            else { cc_dirty[k] = 1; } /* ohne Sicherung kein Endstand */
        }
    }
    for (i = 0; i < B->nops; i++) {
        int e0 = cc_ent[i][0], e1 = cc_ent[i][1];
        cc_skip[i] = (unsigned char)((e0 >= 0 || e1 >= 0) && (e0 < 0 || !cc_dirty[e0]) && (e1 < 0 || !cc_dirty[e1]));
        if (cc_skip[i]) { cc_nskip++; }
    }
    /* c) Startkopie unnoetig: erste Beruehrung des Eintrags (Schreiben/Lesen) ist ein
     *    Clear, der ihn ganz ueberschreibt (D3D11-Clear = ganze View; Tiefe nur mit
     *    Tiefe + Stencil), oder der Eintrag ist ruhig (bekommt bak[B]). */
    {
        unsigned char seen[IP_SH_MAX];
        memset(seen, 0, sizeof seen);
        memset(cc_nosc, 0, sizeof cc_nosc);
        cc_nnosc = 0;
        for (i = 0; i < B->nops; i++) {
            const ip_op_t *o = &B->ops[i];
            int e0 = cc_ent[i][0], e1 = cc_ent[i][1], e2 = cc_ent[i][2];
            if (o->kind == IP_CLR_RTV && e0 >= 0 && !seen[e0]) { seen[e0] = 1; cc_nosc[e0] = 1; continue; }
            if (o->kind == IP_CLR_DSV && e1 >= 0 && !seen[e1]) { seen[e1] = 1; cc_nosc[e1] = (o->dsflags & 3u) == 3u; continue; }
            if (e0 >= 0) { seen[e0] = 1; }
            if (e1 >= 0) { seen[e1] = 1; }
            if (e2 >= 0) { seen[e2] = 1; }
            if (o->kind == IP_DRAW && o->rq) {
                for (k = 1; k < 4; k++) {
                    if (o->rq_shsrv[k] && o->rq_shsrv[k] != o->rqr.srv[k]) {
                        int e = cc_ent_of(B, o->rq_shsrv[k], 2);
                        if (e >= 0) { seen[e] = 1; }
                    }
                }
            }
        }
        for (k = 0; k < IP_SH_MAX; k++) {
            if (cc_wr[k] && !cc_dirty[k] && (g_cfg.interp_cockpit_cache & 1)) { cc_nosc[k] = 1; }
            if (cc_nosc[k]) { cc_nnosc++; }
        }
    }
    /* b) BB-Anfang */
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        int bb = (o->rtv == g_rtv) || (cc_bdsv && o->dsv == cc_bdsv);
        int r = cc_ent[i][2];
        if (o->kind == IP_DRAW && (r == cc_bbe || (cc_dep_e >= 0 && r == cc_dep_e))) { cc_why = 1; break; }
        if (!bb) { continue; }
        if (o->kind != IP_DRAW) { cc_pfx = (int)i + 1; continue; }
        if (o->rtv != g_rtv) { cc_why = 2; break; }
        if (o->flag) { cc_why = 3; break; }
        if (o->has_tex & FXR_MASK) { cc_why = 4; break; }
        if (r == -2 || cc_reads_dirty(B, i)) { cc_why = 5; break; }
        cc_pfx = (int)i + 1;
        cc_npfx++;
    }
    for (i = 0; i < B->nops; i++) {
        if (B->ops[i].kind == IP_DRAW && B->ops[i].rtv == g_rtv) { cc_nbb++; }
    }
}

static int cc_mk(ID3D11Texture2D **dst, ID3D11Texture2D *like)
{
    D3D11_TEXTURE2D_DESC d, h;
    ID3D11Texture2D_GetDesc(like, &d);
    if (*dst) {
        ID3D11Texture2D_GetDesc(*dst, &h);
        if (h.Width == d.Width && h.Height == d.Height && h.Format == d.Format && h.SampleDesc.Count == d.SampleDesc.Count) {
            return 1;
        }
        ID3D11Texture2D_Release(*dst);
        *dst = NULL;
    }
    d.MiscFlags = 0;
    d.CPUAccessFlags = 0;
    d.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, dst))) { *dst = NULL; return 0; }
    return 1;
}

/* Stand nach dem BB-Anfang sichern (Modus 1) */
static void cc_store(ip_list_t *A, ip_list_t *B, const ip_cx_t *x)
{
    ID3D11Texture2D *bc = g_ish[B->bbref].sh, *bd = cc_dep_e >= 0 ? g_ish[cc_dep_e].sh : NULL;
    if (!bc || !cc_mk(&cc_col, bc) || (bd && !cc_mk(&cc_dep, bd))) { cc_valid = 0; return; }
    ID3D11DeviceContext_CopyResource(x->c, (ID3D11Resource *)cc_col, (ID3D11Resource *)bc);
    if (bd) { ID3D11DeviceContext_CopyResource(x->c, (ID3D11Resource *)cc_dep, (ID3D11Resource *)bd); }
    cc_valid = 1;
    cc_ser_a = A->serial;
    cc_ser_b = B->serial;
    cc_st_built++;
}

/* Vor ip_sh_start: Scan (einmal je Tick) und Startkopie-Maske. 1 = Cache aktiv. */
static int cc_prep(ip_list_t *B, int bi, int lerp_ok)
{
    int mode = g_cfg.interp_cockpit_cache;
    ip_sh_noskip = NULL;
    if (mode <= 0 || !lerp_ok || B->bbref < 0) { return 0; }
    if (B->ncopy) { return 0; } /* W340: Kopier-Ops kennt die Ruhe-/Anfangs-Analyse nicht -> ohne Cache */
    if (cc_scan_ser != B->serial) {
        cc_scan(B, bi);
        cc_st_n++;
        cc_st_why[cc_why & 7]++;
        cc_st_pfx += cc_npfx;
        cc_st_bb += cc_nbb;
        cc_st_skip += cc_nskip;
        cc_st_q += cc_nquiet;
        cc_st_ops += B->nops;
        cc_st_nosc += cc_nnosc;
    }
    if (mode & 4) { ip_sh_noskip = cc_nosc; } /* Bit 3: Startkopien sparen */
    return 1;
}

/* Rueckgabe: Bit 0..1 = BB-Anfang (0 aus, 1 aufbauen, 2 nutzen), Bit 4 = ruhige Ziele ueberspringen */
static int cc_begin(ip_list_t *B, int bi, ip_list_t *A, int lerp_ok, const ip_cx_t *x)
{
    int m = 0, k;
    int mode = g_cfg.interp_cockpit_cache;
    ip_sh_noskip = NULL;
    if (mode <= 0 || !lerp_ok || B->bbref < 0 || cc_scan_ser != B->serial || B->ncopy) { return 0; }
    cc_st_rep++;
    if ((mode & 1) && cc_nskip) { /* a) ruhige Ziele: Tick-Endstand */
        for (k = 0; k < IP_SH_MAX; k++) {
            if (cc_wr[k] && !cc_dirty[k]) {
                ID3D11DeviceContext_CopyResource(x->c, (ID3D11Resource *)g_ish[k].sh, (ID3D11Resource *)g_ish[k].bak[bi]);
            }
        }
        m |= 16;
    }
    if ((mode & 2) && cc_pfx > 0 &&
        cc_npfx >= (unsigned)(g_cfg.interp_cockpit_cache_min > 0 ? g_cfg.interp_cockpit_cache_min : 1)) {
        if (cc_valid && cc_ser_a == A->serial && cc_ser_b == B->serial) {
            ID3D11DeviceContext_CopyResource(x->c, (ID3D11Resource *)g_ish[B->bbref].sh, (ID3D11Resource *)cc_col);
            if (cc_dep_e >= 0 && cc_dep) {
                ID3D11DeviceContext_CopyResource(x->c, (ID3D11Resource *)g_ish[cc_dep_e].sh, (ID3D11Resource *)cc_dep);
            }
            cc_st_used++;
            m |= 2;
        } else {
            cc_valid = 0;
            m |= 1;
        }
    } else if (mode & 2) {
        cc_st_off++;
    }
    if ((cc_st_rep % 1200u) == 1199u) {
        HLOG_I("INTERP ccache: bilder=%u ticks=%u ops/tick=%.0f ohne-startkopie/tick=%.1f ruhig: ziele/tick=%.1f ops/tick=%.0f | bb-anfang: aufgebaut=%u "
               "genutzt=%u aus=%u bb-draws/tick=%.0f davon im Anfang=%.0f | ende: alle=%u liest-bb=%u fremd-ziel=%u lerp=%u "
               "fx=%u liest-unruhig=%u gross=%u",
               cc_st_rep, cc_st_n, cc_st_n ? (double)cc_st_ops / cc_st_n : 0.0, cc_st_n ? (double)cc_st_nosc / cc_st_n : 0.0,
               cc_st_n ? (double)cc_st_q / cc_st_n : 0.0,
               cc_st_n ? (double)cc_st_skip / cc_st_n : 0.0, cc_st_built, cc_st_used, cc_st_off,
               cc_st_n ? (double)cc_st_bb / cc_st_n : 0.0, cc_st_n ? (double)cc_st_pfx / cc_st_n : 0.0, cc_st_why[0],
               cc_st_why[1], cc_st_why[2], cc_st_why[3], cc_st_why[4], cc_st_why[5], cc_st_why[6]);
    }
    return m;
}

/* ---------- W134 VR-PROTO: Augen-Durchgang ----------
 * Mit ip_vrp != NULL zeichnet ip_replay_ex jeden Draw/Clear, der im Spiel in
 * den Backbuffer ging (o->rtv == g_rtv), je Auge ins Augenziel statt in den
 * BB-Schatten. RT-Paesse laufen unveraendert (Welt im Monitor, Sub-Monitore).
 * VR-VS je Draw-Klasse: Spiel-Clip c -> Sitz-Sicht v (RH) -> Augen-Clip E:
 *  k2.z = 1  3D-Pfad (Objektraum, w = 1): v = (c.x/a', c.y/b, -c.w)
 *  k2.z = 0  2D/RHW (Pixel -> NDC):        v = (n.x*D/a', n.y*D/b, -D)
 *  k2.z = 2  P2 = Cockpit (fast orthografisch, w ~ 1e5): Relief um D aus z/w
 * Tiefe: z_Auge = (c.z/c.w) * w_Auge -> Tiefentest wie im Original (die
 * Klassen haben im Spiel nicht vergleichbare Tiefen, W134_e2). Shading-Draws
 * des 3D-Pfads behalten ihren VS: lineare Augen-Matrix (mvp*S + e44)*E, Tiefe
 * aus der Augenprojektion mit Near/Far der Spielkamera (= Original bei
 * geradem Kopf). Shading-Draws P2/2D im BB: IM-PS-Ersatz. */
static sb_vr_pass_t *ip_vrp;
static ID3D11VertexShader *ip_vr_vs1, *ip_vr_vs2;
static ID3D11Buffer *ip_vr_cb;
static int ip_vr_init_done;
static const char *kIP_VR_VS =
    "cbuffer CB : register(b0) { row_major float4x4 mvp; uint has_tex; };\n"
    "cbuffer IP : register(b1) { row_major float4x4 mvp0; float ip_t; };\n"
    "cbuffer VR : register(b2) { row_major float4x4 E; float4 k; float4 k2; };\n"
    "struct VSIN { float4 pos : POSITION0; float4 col : COLOR0;\n"
    "              float2 uv : TEXCOORD0;\n"
    "#ifdef VR_LERP\n"
    "              float4 pos0 : POSITION1;\n"
    "#endif\n"
    "};\n"
    "struct VSOUT { float4 pos : SV_Position; float4 col : COLOR0;\n"
    "               float2 uv : TEXCOORD0;\n"
    "#if defined(VR_LERP) && defined(IP_GUARD)\n" /* FIX(W265 LINE-GUARD): Lagen N/N-1 im Augenraum fuer den GS */
    "               float4 g1 : IPGA; float4 g0 : IPGB; float2 gw : IPGW;\n"
    "#endif\n"
    "};\n"
    "float4 vrpos(float4 c) {\n" /* Spiel-Clip -> Augen-Clip (je Draw-Klasse, s. oben) */
    "  float2 n = c.xy / c.w; float zn = c.z / c.w; float3 v; float md = 0.0; float4 p;\n"
    "  if (k2.z > 1.5) { float d = clamp(k.z + k.w * (zn - k2.x), k.z - k2.y, k.z + k2.y);\n"
    "    if (k2.w > 1.5 && zn > 0.2 && zn < 0.999) { d = min(10.0 / (100.0 - 99.9 * zn), 90.0); md = 1.0; }\n"
    "    v = float3(n.x * d * k.x, n.y * d * k.y, -d); }\n"
    "  else if (k2.z > 0.5) { v = float3(c.x * k.x, c.y * k.y, -c.w); }\n"
    "  else { v = float3(n.x * k.z * k.x, n.y * k.z * k.y, -k.z); }\n"
    "  p = mul(float4(v, 1.0), E);\n"
    "  if (md > 0.5) { p.z -= 2e-5 * p.w; }\n"
    "  else if (k2.w < 0.5 || k2.w > 1.5) { p.z = zn * p.w; }\n"
    "  return p; }\n"
    "VSOUT main(VSIN i) { VSOUT o;\n"
    "  float4 c = mul(i.pos, mvp);\n"
    "#ifdef VR_LERP\n"
    "  float4 c0 = mul(i.pos0, mvp0); float4 cn = c; c = c0 + ip_t * (c - c0);\n"
    "#ifdef IP_EYE\n" /* FIX(W224) wie kIP_VS */
    "  if (c0.w * cn.w <= 0.0) { c = cn; }\n"
    "#endif\n"
    "#ifdef IP_GUARD\n"
    "  o.g0 = vrpos(c0); o.g1 = vrpos(cn); o.gw = float2(c0.w, cn.w);\n"
    "#endif\n"
    "#endif\n"
    "  o.pos = vrpos(c);\n"
    "  o.col = i.col; o.uv = i.uv; return o; }\n";

static int ip_vr_init(void)
{
    ID3DBlob *b = NULL;
    D3D11_BUFFER_DESC bd;
    char src[2048];
    if (ip_vr_init_done) { return ip_vr_vs1 && ip_vr_vs2 && ip_vr_cb; }
    ip_vr_init_done = 1;
    if (sb_im_compile(kIP_VR_VS, "vs_4_0", &b)) {
        ID3D11Device_CreateVertexShader(g_dev, b->lpVtbl->GetBufferPointer(b), b->lpVtbl->GetBufferSize(b), NULL, &ip_vr_vs1);
        b->lpVtbl->Release(b);
        b = NULL;
    }
    _snprintf_s(src, sizeof src, _TRUNCATE, "#define VR_LERP 1\n%s%s%s",
                g_cfg.interp_eyefix ? "#define IP_EYE 1\n" : "", /* W224 */
                (g_cfg.interp_guard || g_cfg.interp_guard_diag) ? "#define IP_GUARD 1\n" : "", kIP_VR_VS); /* W265 */
    if (sb_im_compile(src, "vs_4_0", &b)) {
        ID3D11Device_CreateVertexShader(g_dev, b->lpVtbl->GetBufferPointer(b), b->lpVtbl->GetBufferSize(b), NULL, &ip_vr_vs2);
        b->lpVtbl->Release(b);
    }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 96;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &ip_vr_cb))) { ip_vr_cb = NULL; }
    HLOG_I("VR: Wiedergabe-Shader %s", (ip_vr_vs1 && ip_vr_vs2 && ip_vr_cb) ? "bereit" : "FEHLER");
    return ip_vr_vs1 && ip_vr_vs2 && ip_vr_cb;
}

/* P2 im BB: render_im.c setzt mvp = diag(vk, 1, 1, 1) (Clip liegt per CPU vor). */
/* FIX(W265 LINE-GUARD): braucht dieser gelerpte Dreiecks-Draw den Waechter? Nicht noetig ist er nur
 * fuer einen Matrix-Lerp gleicher Objektraum-Vertices mit echter Perspektive (IP_W_LERP, Matrizen
 * verschieden, w haengt von der Lage ab): dort ist der Clip-Lerp exakt die Transformation mit der
 * Zwischenmatrix M_t (ein Dreieck bleibt ein Dreieck, die Rasterung clippt an der Nahebene). Alles
 * andere - neue Eingangsdaten (Vertex i <-> i), gleiche Matrix mit neuem Strom, vorab durch w
 * geteilte P2-/2D-Stroeme (w-Spalte 0, Lerp im Bildraum) - bekommt den GS. interp_guard Bit 4: alle. */
static int ip_gd_need(const ip_op_t *o, const ip_op_t *a)
{
    if (o->topo != 4u && o->topo != 5u) { return 0; } /* GS nur fuer Dreiecks-Liste/Strip */
    if (g_cfg.interp_guard & 4) { return 1; }
    if (o->why != IP_W_LERP || !a) { return 1; }
    if (!memcmp(a->mvp, o->mvp, sizeof o->mvp)) { return 1; }
    return o->mvp[3] == 0.0f && o->mvp[7] == 0.0f && o->mvp[11] == 0.0f;
}

static int ip_vr_isp2(const float m[16])
{
    return m[1] == 0.0f && m[2] == 0.0f && m[3] == 0.0f && m[4] == 0.0f && m[5] == 1.0f && m[6] == 0.0f &&
           m[7] == 0.0f && m[8] == 0.0f && m[9] == 0.0f && m[10] == 1.0f && m[11] == 0.0f && m[12] == 0.0f &&
           m[13] == 0.0f && m[14] == 0.0f && m[15] == 1.0f;
}

static void ip_m4mul(float r[16], const float a[16], const float b[16])
{
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
                           a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
        }
    }
}

/* 3D-Pfad (Objektraum, w = 1): Spiel-mvp -> Augen-mvp, linear:
 * v = (X/a', Y/b, -W, pos.w) = pos * (mvp * S + e44), danach * E. */
static void ip_vr_mat3d(float out[16], const float mvp[16], const sb_vr_pass_t *p, int e)
{
    float S[16], T[16];
    memset(S, 0, sizeof S);
    S[0] = p->ia; S[5] = p->ib; S[14] = -1.0f;
    ip_m4mul(T, mvp, S);
    T[15] += 1.0f;
    ip_m4mul(out, T, p->E[e]);
}

static void ip_vr_eye_target(const ip_cx_t *x, const sb_vr_pass_t *p, int e, const ip_op_t *o)
{
    D3D11_VIEWPORT v;
    D3D11_RECT r;
    v.TopLeftX = p->vp[e][0]; v.TopLeftY = p->vp[e][1];
    v.Width = p->vp[e][2]; v.Height = p->vp[e][3];
    v.MinDepth = o->vp.MinDepth; v.MaxDepth = o->vp.MaxDepth; /* Tiefenbereich des Spiels (Schichtung) */
    r.left = (LONG)p->vp[e][0]; r.top = (LONG)p->vp[e][1];
    r.right = (LONG)(p->vp[e][0] + p->vp[e][2]); r.bottom = (LONG)(p->vp[e][1] + p->vp[e][3]);
    ID3D11DeviceContext_OMSetRenderTargets(x->c, 1, (ID3D11RenderTargetView *const *)&p->rtv,
                                           o->dsv ? (ID3D11DepthStencilView *)p->dsv : NULL);
    ID3D11DeviceContext_RSSetViewports(x->c, 1, &v);
    ID3D11DeviceContext_RSSetScissorRects(x->c, 1, &r);
}

static void ip_vr_cb_write(const ip_cx_t *x, const sb_vr_pass_t *p, int e, int kind)
{
    D3D11_MAPPED_SUBRESOURCE map;
    float *f;
    if (FAILED(ID3D11DeviceContext_Map(x->c, (ID3D11Resource *)ip_vr_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) { return; }
    f = (float *)map.pData;
    /* W164: k2.w = 1 (vr_world_depth, 3D-Pfad = Cockpit-Mesh): Tiefe affin aus der
     * Augenprojektion statt Original-z/w x w_Auge (das extrapoliert bei Dreiecken, die
     * die Augen-Ebene schneiden -> Flaechen mit Tiefe ~0 verdecken alles);
     * k2.w = 2 (vr_inst_depth, P2): Vertices mit perspektivischem z/w (> 0,2) auf die
     * metrische Tiefe w = n f / (f - z/w (f - n)) (Instrumente bleiben auf ihrer
     * Tafel), Tiefe ebenfalls affin (+ 2e-5 zum Auge: Decals mit LESS_EQUAL) */
    memcpy(f, p->E[e], 64);
    f[16] = p->ia; f[17] = p->ib; f[18] = p->d2d; f[19] = p->rl;
    f[20] = p->rz0; f[21] = p->rmax; f[22] = (float)kind;
    f[23] = (kind == 1 && p->wfix) ? 1.0f : ((kind == 2 && p->ifix) ? 2.0f : 0.0f);
    ID3D11DeviceContext_Unmap(x->c, (ID3D11Resource *)ip_vr_cb, 0);
}

/* Nicht-Shading-Draw je Auge. VS/Layout/Streams des bisherigen Pfads sind
 * gebunden (mode 0 = exakt, 1 = Lerp); P2 bindet den VR-VS und setzt mode = -1. */
/* W264 DRAW-OPT2: Draw eines aufgezeichneten Ops - indiziert (o->ib1) ueber den
 * festen Strip/Fan/Quad-Indexpuffer, sonst als Liste; base = erster Vertex
 * (0, wenn die Stroeme per Versatz gebunden sind). */
static void ip_draw_op(ID3D11DeviceContext *c, const ip_op_t *o, unsigned n, unsigned base)
{
    /* FIX(ISSUE-EMP,W352) line_scale: Linie als Band wie im Tick-Bild (GS je Draw an/aus; Linien-Draws
     * haben nie den W265-Waechter, mit interp_guard traegt der Lerp-VS Zusatzausgaben -> dann alt). */
    if (o->lgs && !g_cfg.interp_guard) {
        ID3D11DeviceContext_GSSetShader(c, o->lgs, NULL, 0);
        if (o->ib1) { sb_im_ib_bind(c); ID3D11DeviceContext_DrawIndexed(c, n, o->ib1 - 1u, (INT)base); }
        else { ID3D11DeviceContext_Draw(c, n, base); }
        ID3D11DeviceContext_GSSetShader(c, NULL, NULL, 0);
        return;
    }
    if (o->ib1) {
        sb_im_ib_bind(c);
        ID3D11DeviceContext_DrawIndexed(c, n, o->ib1 - 1u, (INT)base);
    } else {
        ID3D11DeviceContext_Draw(c, n, base);
    }
}

static void ip_vr_draw(const ip_cx_t *x, sb_vr_pass_t *p, const ip_op_t *o, const ip_op_t *a, int kind,
                       float t, int lerp, int *mode)
{
    int e;
    ID3D11Buffer *cbs[3];
    if (g_cfg.vr_diag_skip & (1 << kind)) { return; } /* DIAG(VR-FIX2,W164): Draw-Klasse weglassen */
    if (!ip_vr_init()) { return; }
    ID3D11DeviceContext_VSSetShader(x->c, lerp ? ip_vr_vs2 : ip_vr_vs1, NULL, 0);
    cbs[0] = x->cb0; cbs[1] = x->cb1; cbs[2] = ip_vr_cb;
    ID3D11DeviceContext_VSSetConstantBuffers(x->c, 0, 3, cbs);
    if (lerp) { ip_cb_write(x, x->cb1, IP_AMVP(o, a), 0, t, 1); }
    *mode = -1;
    for (e = 0; e < p->neyes; e++) {
        ip_vr_eye_target(x, p, e, o);
        ip_vr_cb_write(x, p, e, kind);
        if (lerp) { ip_draw_op(x->c, o, o->nv, 0); }
        else { ip_draw_op(x->c, o, o->nv, o->vstart); }
    }
}

/* ---------- W183 VR-FIX3: Aussenwelt im Spieltakt (vr_world_tick=1) ----------
 * Je VR-Bild muss nur das Kopfabhaengige neu: die Draws, die im Spiel in den
 * Backbuffer gingen (Cockpit-Mesh, Instrumente, Monitor-Komposition, HUD) -
 * je Auge mit der aktuellen Pose. Die RT-Paesse (Aussenwelt im Hauptmonitor,
 * Sub-Monitore, Karte; ~75 % der Draws) haengen nicht vom Kopf ab: statt sie
 * je VR-Bild (90 Hz) neu zu zeichnen, bekommen ihre Schatten den gesicherten
 * Tick-Endstand bak[B] (Spieltakt 30 Hz, wie das Original). Sicher nur, wenn
 * kein BB-Draw ein RT liest, bevor B es zum letzten Mal schreibt, und jedes
 * geschriebene RT eine Sicherung von B hat - sonst volle Wiedergabe. Einmal je
 * Tick (Seriennummer) geprueft. */
static unsigned char vt_skip[CC_MAXOPS], vt_wr[IP_SH_MAX], vt_nosc[IP_SH_MAX];
static unsigned vt_ser, vt_nskip;
static int vt_ok;

static int vt_reads_late(const ip_list_t *B, const ip_op_t *o, unsigned i, const unsigned *lastw)
{
    int k, e;
    if (o->sh_srv && o->sh_srv != o->srv) {
        e = cc_ent_of(B, o->sh_srv, 2);
        if (e < 0) { return 1; }
        if (vt_wr[e] && i < lastw[e]) { return 1; }
    }
    if (o->rq) {
        for (k = 1; k < 4; k++) {
            if (o->rq_shsrv[k] && o->rq_shsrv[k] != o->rqr.srv[k]) {
                e = cc_ent_of(B, o->rq_shsrv[k], 2);
                if (e < 0 || (vt_wr[e] && i < lastw[e])) { return 1; }
            }
        }
    }
    return 0;
}

static void vt_scan(const ip_list_t *B, int bi)
{
    unsigned i, lastw[IP_SH_MAX];
    int k, bbread = 0, bbdep = -1;
    vt_ser = B->serial;
    vt_ok = 1;
    vt_nskip = 0;
    memset(vt_wr, 0, sizeof vt_wr);
    memset(vt_nosc, 0, sizeof vt_nosc);
    memset(lastw, 0, sizeof lastw);
    if (B->nops > CC_MAXOPS || B->ncopy) { vt_ok = 0; return; } /* W340: Kopier-Ops nicht in der VR-Analyse */
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        int bb = (o->kind == IP_DRAW && o->rtv == g_rtv) || (o->kind == IP_CLR_RTV && o->rtv == g_rtv) ||
                 (o->kind == IP_CLR_DSV && o->dsv == g_dsv);
        vt_skip[i] = (unsigned char)!bb;
        if (bb) {
            if (o->kind == IP_DRAW && o->sh_dsv && bbdep < 0) { bbdep = cc_ent_of(B, o->sh_dsv, 1); }
            continue;
        }
        vt_nskip++;
        if (o->kind != IP_CLR_DSV) {
            int e0 = cc_ent_of(B, o->sh_rtv, 0);
            if (o->sh_rtv && e0 < 0) { vt_ok = 0; return; }
            if (e0 >= 0) { vt_wr[e0] = 1; lastw[e0] = i + 1u; }
        }
        if (o->kind != IP_CLR_RTV) {
            int e1 = cc_ent_of(B, o->sh_dsv, 1);
            if (o->sh_dsv && e1 < 0) { vt_ok = 0; return; }
            if (e1 >= 0) { vt_wr[e1] = 1; lastw[e1] = i + 1u; }
        }
    }
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        int r;
        if (vt_skip[i] || o->kind != IP_DRAW) { continue; }
        if (vt_reads_late(B, o, i, lastw)) { vt_ok = 0; return; }
        r = (o->sh_srv && o->sh_srv != o->srv) ? cc_ent_of(B, o->sh_srv, 2) : -1;
        if (r >= 0 && (r == B->bbref || r == bbdep)) { bbread = 1; }
    }
    for (k = 0; k < IP_SH_MAX; k++) {
        if (!vt_wr[k]) { continue; }
        if (g_ish[k].bakser[bi] != B->serial || !g_ish[k].bak[bi]) { vt_ok = 0; return; }
        vt_nosc[k] = 1; /* bekommt bak[B] statt der Startkopie bak[A] */
    }
    /* BB-Schatten: im Augenbild ungenutzt (Augenziel), ausser ein BB-Draw liest ihn. Die BB-Tiefe
     * behaelt ihre Startkopie (Effekt-Draws kopieren sie, W133). */
    if (!bbread && B->bbref >= 0 && B->bbref < IP_SH_MAX && !vt_wr[B->bbref]) { vt_nosc[B->bbref] = 1; }
}

/* ---------- W193 PAUSE-LIVE: Wiedergabe-Abweichungen der Pause-Neuzeichnung ----------
 * Nur waehrend sb_interp_pause_render (Spiel-Thread in der Pause, ctx_lock) gesetzt; alle
 * anderen Wiedergaben (Zwischenbilder, Verify, VR) sehen ip_pl_on = 0 -> unveraendert. */
static int ip_pl_on;                   /* 1 = laufende Wiedergabe ist eine Pause-Neuzeichnung */
static int ip_pl_noshade;              /* Shading-Draws mit IM-PS-Ersatz (shading jetzt aus, wie VR vr_shading=0) */
static unsigned ip_pl_mfx;             /* Bit 0: Korn-Wash auslassen, Bit 1: Stoerzeilen auslassen (monitor_fx) */
static ID3D11Buffer *ip_pl_cbsrc;      /* Konstanten-Fenster der Liste (Aufzeichnung) ... */
static ID3D11Buffer *ip_pl_cb;         /* ... und die neu berechnete Kopie (sfx0..sfx3, Schattenstufen) */
static ID3D11Buffer *ip_pl_vb;         /* W209: pause-only rebuilt vertex buffer */
static float (*ip_pl_mvp)[16];         /* matrices paired with that buffer */
static sb_interp_mvp_hook_t ip_pl_hook; /* W191-Schnittstelle: MVP der BB-Draws (Cockpit-Projektion) */
static unsigned ip_pl_nhook, ip_pl_nmfx, ip_pl_nnoshade; /* Zaehler der letzten Neuzeichnung */
static int ip_pl_tex;                  /* 1 = in der Pause neu geladene Texturen tauschen (sb_tex_pause_map) */
#define IP_PL_SRV0(s) (ip_pl_tex ? sb_tex_pause_map(s) : (s))
static ID3D11ShaderResourceView *ip_ph_srv(ID3D11ShaderResourceView *s); /* W195: skalierte Zwischenziele */
#define IP_PL_SRV(s) ip_ph_srv(IP_PL_SRV0(s))

/* ---------- W195 PHOTO-MODE: Abweichungen der Foto-Wiedergabe (zusaetzlich zu ip_pl_*) ----------
 * Nur waehrend sb_interp_photo_render (Pause, ctx_lock): ip_ph.on = 1. Sonst 0 -> alle Pfade unveraendert.
 *  - Umlenkung: Draws/Clears in das Ziel tgt (Farbe) bzw. tgtd (Tiefe) gehen in die Foto-Kachel
 *    (rtv/dsv), Viewport und Scissor werden affin abgebildet (x' = x*kx + ox, y' = y*ky + oy).
 *  - Freie Kamera: Welt-Draws (Ziel wld, Clip-w echt) bekommen MVP * M (Clip -> Sicht der
 *    Spielkamera -> verschobene/gedrehte Sicht -> Clip), M je Draw aus seinem Spiel-Viewport. */
static sb_ph_rep_t ip_ph;
static unsigned ip_ph_ncam, ip_ph_nred, ip_ph_nclr;
static unsigned ip_ph_nhud; /* W290: ausgelassene HUD-Draws (hud_off) */

/* W290 QUICK-SHOT: HUD-Draw des Hauptmonitors (nur Foto-Wiedergabe mit hud_off): 2D-Draw in das Monitor-RT, das
 * nicht das Welt-RT kopiert und kein Korn/Stoerzeilen-Draw (W105, o->nolerp) ist. */
static int ip_ph_is_hud(const ip_op_t *o)
{
    if (!ip_ph.hud_off || ip_ph.mon < 0 || o->kind != IP_DRAW || o->cls || o->nolerp) { return 0; }
    if (o->sh_rtv != g_ish[ip_ph.mon].rtv) { return 0; }
    if (ip_ph.wld >= 0 && o->sh_srv && o->sh_srv == g_ish[ip_ph.wld].srv) { return 0; }
    return 1;
}
/* W428 PHOTO-CINEMATIC: Draw des eigenen VT (nur Foto-Wiedergabe mit cine): 3D-Draw in das Welt-RT, dessen
 * Modell-Ursprung naeher als photo_cine_self/10 Einheiten an der Spielkamera liegt. Lage aus der mvp und der
 * Spiel-Projektion des Draws (Zeilenvektoren: Ursprung -> Clip = Zeile 3; x = m12/a, y = m13/b, Tiefe = w = m15).
 * Draws mit Pixel-Matrix (P2/RHW, auf der CPU transformiert) tragen keine Sicht-Lage und bleiben. */
/* W445 PHOTO-CINE-SELF: kleinster Abstand zur Spielkamera aus Modell-Ursprung und den 4 Bezugs-Vertices (pref: erster,
 * 1/3, 2/3, letzter) in Kamera-Sicht (x = c0/a, y = c1/b, Tiefe = w). 1e30 = keine Sicht-Projektion. */
static float ip_ph_near_ref(const ip_op_t *o)
{
    float a = o->prj[0], b = o->prj[1], x, y, z, d2, best;
    int q;
    if (a <= 1e-4f || b <= 1e-4f) { return 1e30f; }
    x = o->mvp[12] / a;
    y = o->mvp[13] / b;
    z = o->mvp[15];
    best = x * x + y * y + z * z;
    for (q = 0; q < 4; q++) {
        float c[4];
        ip_clip(o->pref[q], o->mvp, c);
        x = c[0] / a;
        y = c[1] / b;
        z = c[3];
        d2 = x * x + y * y + z * z;
        if (d2 < best) { best = d2; }
    }
    return sqrtf(best);
}
static unsigned ip_ph_nself; /* ausgelassene Draws des eigenen VT (cine) */
static unsigned ip_ph_nextra; /* W445: davon Luecke (Draw mitten in der Folge eigener Teile) */
static int ip_ph_is_self_dist(const ip_op_t *o)
{
    const vsh_prog_t *vp;
    float a, b, x, y, z, r;
    if (!ip_ph.cine || ip_ph.wld < 0 || o->kind != IP_DRAW || !o->cls || o->sh_rtv != g_ish[ip_ph.wld].rtv) { return 0; }
    vp = o->pvs ? vsh_program(o->pvs) : NULL;
    if (vp && vp->kind == 2) { return (g_cfg.photo_cine_diag & 2) ? 1 : 0; } /* DIAG(W428): P2 testweise aus */
    if (o->mvp[3] == 0.0f && o->mvp[7] == 0.0f && o->mvp[11] == 0.0f && o->mvp[15] == 1.0f) { return 0; } /* Pixel-Matrix */
    a = o->prj[0];
    b = o->prj[1];
    r = (float)g_cfg.photo_cine_self * 0.1f;
    if (a <= 1e-4f || b <= 1e-4f || r <= 0.0f) { return 0; }
    x = o->mvp[12] / a;
    y = o->mvp[13] / b;
    z = o->mvp[15];
    if (x * x + y * y + z * z > r * r) { return 0; }
    static unsigned s_ndg;
    if ((g_cfg.photo_cine_diag & 1) && s_ndg < 200u) {
        s_ndg++;
        HLOG_I("PHOTO cine: eigener VT nv=%u tex=%x Ursprung %.2f,%.2f,%.2f (Abstand %.2f)", o->nv, o->has_tex, (double)x,
               (double)y, (double)z, sqrt((double)(x * x + y * y + z * z)));
    }
    return 1;
}
/* W445 PHOTO-CINE-SELF: Merker je Op, einmal je Foto-Wiedergabe (ip_ph_self_prep): 1 = eigener Teil (Abstandsregel),
 * 3 = Luecke (3D-Draw mitten in der Folge eigener Teile). Gepruefte und verworfene Merkmale: BERICHT_M2 W445 (P2-Kopien
 * ohne Bildwirkung, der P2-Draw direkt vor den eigenen Teilen ist der Himmel, naechster Vertex trifft Gelaende/Strassen). */
static unsigned char ip_ph_self_f[IP_OPS_MAX];
static const ip_list_t *ip_ph_self_L; /* NULL = neu bestimmen (je sb_interp_photo_render) */
static int ip_ph_world3d(const ip_op_t *o)
{
    return o->kind == IP_DRAW && o->cls && o->sh_rtv == g_ish[ip_ph.wld].rtv;
}
/* W445: Sichtlage-Abstand des Ursprungs (wie ip_ph_is_self_dist, ohne Schwelle); 1e30 = keine Sicht-Matrix/P2 */
static float ip_ph_org_dist(const ip_op_t *o)
{
    const vsh_prog_t *vp = o->pvs ? vsh_program(o->pvs) : NULL;
    float a = o->prj[0], b = o->prj[1], x, y, z;
    if ((vp && vp->kind == 2) || a <= 1e-4f || b <= 1e-4f) { return 1e30f; }
    if (o->mvp[3] == 0.0f && o->mvp[7] == 0.0f && o->mvp[11] == 0.0f && o->mvp[15] == 1.0f) { return 1e30f; }
    x = o->mvp[12] / a;
    y = o->mvp[13] / b;
    z = o->mvp[15];
    return sqrtf(x * x + y * y + z * z);
}
static void ip_ph_self_prep(const ip_list_t *B)
{
    unsigned i, j, n = B->nops < IP_OPS_MAX ? B->nops : IP_OPS_MAX, nown = 0, nsw = 0;
    float r2 = (float)g_cfg.photo_cine_self * 0.2f; /* Lueckenregel: hoechstens doppelte Reichweite */
    ip_ph_self_L = B;
    memset(ip_ph_self_f, 0, n);
    for (i = 0; i < n; i++) {
        if (ip_ph_is_self_dist(&B->ops[i])) { ip_ph_self_f[i] = 1; nown++; }
    }
    if (!nown) { return; }
    /* Luecke: 3D-Draw (Sicht-Matrix) zwischen zwei eigenen Teilen der Zeichenfolge (naechster 3D-Draw davor und danach
     * im Welt-RT eigen) und naeher als 2x Reichweite - z. B. das Dreieck (Sprite) ~6,2-6,8 vor dem Auge zwischen Rohr
     * und Arm, das mit der Kamera mitfaehrt (Zensus W445). Fremde Objekte stehen nicht mitten in der VT-Folge. */
    for (j = 0; j < n; j++) {
        unsigned p, q;
        if (ip_ph_self_f[j] || !ip_ph_world3d(&B->ops[j]) || ip_ph_org_dist(&B->ops[j]) > r2) { continue; }
        for (p = j; p-- > 0;) { if (ip_ph_world3d(&B->ops[p])) { break; } }
        for (q = j + 1; q < n; q++) { if (ip_ph_world3d(&B->ops[q])) { break; } }
        if (p < j && q < n && ip_ph_self_f[p] == 1 && ip_ph_self_f[q] == 1) { ip_ph_self_f[j] = 3; nsw++; }
    }
    if (g_cfg.photo_cine_diag & 1) { /* DIAG(W445): je Wiedergabe-Liste Merker (begrenzt) */
        static unsigned s_nsum;
        if (s_nsum < 400u) {
            char t[1024];
            int k = 0;
            s_nsum++;
            t[0] = 0;
            for (j = 0; j < n && k < (int)sizeof t - 24; j++) {
                if (ip_ph_self_f[j]) { k += _snprintf_s(t + k, sizeof t - (size_t)k, _TRUNCATE, " %u:%u/%u", j, ip_ph_self_f[j], B->ops[j].nv); }
            }
            HLOG_I("PHOTO cine: Merker ser=%u eigen=%u Luecke=%u |%s", B->serial, nown, nsw, t);
        }
    }
}
static int ip_ph_is_self(const ip_list_t *B, unsigned i)
{
    if (!ip_ph.cine || ip_ph.wld < 0 || i >= IP_OPS_MAX) { return 0; }
    if (ip_ph_self_L != B) { ip_ph_self_prep(B); }
    if (ip_ph_self_f[i] > 1) { ip_ph_nextra++; }
    return ip_ph_self_f[i] != 0;
}
/* W428: Einblendung (Cinematic): Draw ins Monitor-RT, der nicht das Welt-RT kopiert (HUD-Glyphen, Marken, Radar,
 * FRONT VIEW, Texte), bzw. Draw in den Backbuffer, der weder das Monitor-RT noch das Welt-RT zeigt (Einblendungen
 * und Cockpit-Reste der Vollbild-Ansicht). Ohne Monitor-RT bzw. Welt-RT im Zensus: nichts auslassen. */
static unsigned ip_ph_ncmfx, ip_ph_ncovl;
static int ip_ph_cine_ovl(const ip_op_t *o, int bb)
{
    ID3D11ShaderResourceView *ws = ip_ph.wld >= 0 ? g_ish[ip_ph.wld].srv : NULL;
    ID3D11ShaderResourceView *ms = ip_ph.mon >= 0 ? g_ish[ip_ph.mon].srv : NULL;
    if (!ws) { return 0; }
    if (ms && o->sh_rtv == g_ish[ip_ph.mon].rtv) { return o->sh_srv != ws; }
    if (bb) { return o->sh_srv != ws && (!ms || o->sh_srv != ms); }
    return 0;
}
static ID3D11ShaderResourceView *ip_ph_shsrv[8]; /* Schatten-SRV der skalierten Eintraege (gesetzt in ip_pl_render) */

static ID3D11ShaderResourceView *ip_ph_srv(ID3D11ShaderResourceView *s)
{
    int j;
    if (!ip_ph.on || !ip_ph.nsc || !s) { return s; }
    for (j = 0; j < ip_ph.nsc; j++) { if (s == ip_ph_shsrv[j]) { return ip_ph.sc_srv[j]; } }
    return s;
}

static int ip_ph_sc_rtv(ID3D11RenderTargetView *v)
{
    int j;
    for (j = 0; v && j < ip_ph.nsc; j++) { if (ip_ph.sc_e[j] >= 0 && v == g_ish[ip_ph.sc_e[j]].rtv) { return j; } }
    return -1;
}

static int ip_ph_sc_dsv(ID3D11DepthStencilView *v)
{
    int j;
    for (j = 0; v && j < ip_ph.nsc; j++) { if (ip_ph.sc_d[j] >= 0 && v == g_ish[ip_ph.sc_d[j]].dsv) { return j; } }
    return -1;
}

static int ip_m4inv(float out[16], const float m[16]);
static void ip_m4mul(float r[16], const float a[16], const float b[16]);

/* Clip-Matrix (Zeilenvektor, c' = c * M) eines Welt-Draws: Viewport-Abbildung rv (Spiel-Viewport im RT)
 * zurueck, Projektion zurueck, Kamera-Delta, Projektion, rv. 0 = nicht anwendbar. */
static int ip_ph_cam_matrix(const ip_op_t *o, float M[16])
{
    float P[16], Pi[16], RV[16], RVi[16], T[16], U[16];
    /* eine Spielkamera fuer alle Welt-Draws: Projektion aus dem Zensus (a aus dem Anzeige-Seitenverhaeltnis,
     * s. render_photo.c ph_rep_base); o->prj ist g_proj_game zur Zeit des Draws und nicht fuer jeden Draw die
     * Projektion seines Vertex-Programms (W195-Befund: a = b im Zensus, Monitor zeigt 4:3) */
    float a = ip_ph.def_prj[0], b = ip_ph.def_prj[1], A = ip_ph.def_prj[2], Bz = ip_ph.def_prj[3];
    (void)o->prj;
    memset(P, 0, sizeof P);
    P[0] = a; P[5] = b; P[10] = A; P[11] = -1.0f; P[14] = Bz;
    if (!ip_m4inv(Pi, P)) { return 0; }
    memset(RV, 0, sizeof RV);
    RV[0] = 1.0f; RV[5] = 1.0f; RV[10] = 1.0f; RV[15] = 1.0f;
    if (o->rtl[0] && o->rtl[1] && o->vpg[2] && o->vpg[3] &&
        !(o->vpg[0] == 0 && o->vpg[1] == 0 && o->vpg[2] == o->rtl[0] && o->vpg[3] == o->rtl[1])) {
        float rw = (float)o->rtl[0], rh = (float)o->rtl[1];
        RV[0] = (float)o->vpg[2] / rw;
        RV[12] = ((float)o->vpg[2] + 2.0f * (float)o->vpg[0]) / rw - 1.0f;
        RV[5] = (float)o->vpg[3] / rh;
        RV[13] = 1.0f - ((float)o->vpg[3] + 2.0f * (float)o->vpg[1]) / rh;
    }
    if (!ip_m4inv(RVi, RV)) { return 0; }
    /* Zeilenvektor: c_f' = c_f * RVi * Pi * D * P * RV */
    ip_m4mul(T, RVi, Pi);
    ip_m4mul(U, T, ip_ph.cam_d);
    ip_m4mul(T, U, P);
    ip_m4mul(M, T, RV);
    return 1;
}

static int ip_ph_is_world(const ip_op_t *o)
{
    if (ip_ph.wld < 0 || o->kind != IP_DRAW) { return 0; }
    if (o->sh_rtv != g_ish[ip_ph.wld].rtv) { return 0; }
    if (o->cls) { return 1; }
    /* 2D/RHW im Welt-RT: nur mit echtem Clip-w (vorab transformierte Welt, nicht Bildschirm-Quads) */
    return o->rn[0][2] > 1.001f || o->rn[0][2] < 0.999f || o->rn[3][2] > 1.001f || o->rn[3][2] < 0.999f;
}

/* ---------- DIAG(W273 INTERP-RACE) interp_rhash ----------
 * Bit 1: je Zwischenbild-Wiedergabe (0<t<1, beide Contexte) Pruefsummen der CPU-Eingangsdaten
 *        (ohne Zeigerwerte): hB = Draws von B, hM = Zuordnung, hA = Partner aus A, hS = Puffer-
 *        wahl/Startkopien. Vergleich zwischen zwei Laeufen je (Tick-Bild-Nr, t).
 * Bit 2: GPU-Inhalt der Tick-Puffer per Ruecklesen (Staging): am Swap von L gemerkt und vor den
 *        Zwischenbildern von (A, B) erneut gelesen -> Abweichung = Puffer nach dem Swap ueberschrieben.
 * Nur Diagnose, Bild unveraendert (Ruecklesen nur im Spiel-Thread, synchron). */
static unsigned ip_rh_fnv(unsigned h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    size_t i;
    for (i = 0; i < n; i++) { h = (h ^ b[i]) * 16777619u; }
    return h;
}
static ID3D11Buffer *ip_rh_stg;
static unsigned ip_rh_stgcap;
static unsigned st_rh_chk, st_rh_bad[6];
static unsigned ip_rh_buf(ID3D11Buffer *b, unsigned bytes)
{
    D3D11_BOX bx;
    D3D11_MAPPED_SUBRESOURCE m;
    unsigned h = 2166136261u, i, n;
    const unsigned *w;
    if (!b || !bytes) { return 0u; }
    if (!ip_rh_stg || ip_rh_stgcap < bytes) {
        D3D11_BUFFER_DESC bd;
        if (ip_rh_stg) { ID3D11Buffer_Release(ip_rh_stg); ip_rh_stg = NULL; }
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = bytes + bytes / 2u;
        bd.Usage = D3D11_USAGE_STAGING;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &ip_rh_stg))) { ip_rh_stg = NULL; ip_rh_stgcap = 0; return 1u; }
        ip_rh_stgcap = bd.ByteWidth;
    }
    memset(&bx, 0, sizeof bx);
    bx.right = bytes; bx.bottom = 1; bx.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)ip_rh_stg, 0, 0, 0, 0, (ID3D11Resource *)b, 0, &bx);
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)ip_rh_stg, 0, D3D11_MAP_READ, 0, &m))) { return 2u; }
    w = (const unsigned *)m.pData;
    n = bytes / 4u;
    for (i = 0; i < n; i++) { h = (h ^ w[i]) * 16777619u; }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)ip_rh_stg, 0);
    return h | 16u; /* nie 0..15 (Fehlercodes) */
}
static void ip_rh_take(const ip_list_t *L, unsigned out[6])
{
    out[0] = ip_rh_buf(L->vb, L->nverts * (unsigned)sizeof(im_vertex_t));
    out[1] = (L->gpu_ok & 1u) ? ip_rh_buf(L->vbg, L->nverts * (unsigned)sizeof(im_vertex_t)) : 0u;
    out[2] = ip_rh_buf(L->rqx, L->rqxpos > L->rqxcap ? L->rqxcap : L->rqxpos);
    out[3] = (L->gpu_ok & 2u) ? ip_rh_buf(L->rqxg, L->rqxpos > L->rqxcap ? L->rqxcap : L->rqxpos) : 0u;
    out[4] = ip_rh_buf(L->rqc, L->rqcpos * 16u);
    out[5] = L->wb_ok ? ip_rh_buf(L->wb, L->nverts * (unsigned)sizeof(float)) : 0u;
}
static void ip_rh_check(const ip_list_t *L, const char *who, unsigned fr, int k)
{
    static const char *const nm[6] = { "vb", "vbg", "rqx", "rqxg", "rqc", "wb" };
    unsigned now[6];
    int q;
    if (!L || !L->ready) { return; }
    ip_rh_take(L, now);
    st_rh_chk++;
    for (q = 0; q < 6; q++) {
        if (now[q] != L->rh[q]) {
            st_rh_bad[q]++;
            if (st_rh_bad[q] <= 40u) {
                HLOG_W("INTERP rhash: Puffer %s von %s (Tick f=%u ser=%u) geaendert seit Swap: %08X -> %08X (Bild f=%u k=%d)",
                       nm[q], who, L->frame, L->serial, L->rh[q], now[q], fr, k);
            }
        }
    }
    if ((st_rh_chk % 2000u) == 0u) {
        HLOG_I("INTERP rhash: %u Pruefungen, geaendert vb=%u vbg=%u rqx=%u rqxg=%u rqc=%u wb=%u", st_rh_chk,
               st_rh_bad[0], st_rh_bad[1], st_rh_bad[2], st_rh_bad[3], st_rh_bad[4], st_rh_bad[5]);
    }
}
static void ip_rh_inputs(const ip_list_t *B, int bi, const ip_list_t *A, int ai, float t, const ip_cx_t *x,
                         int vbgA, int vbgB, int ring)
{
    unsigned hB = 2166136261u, hM = 2166136261u, hA = 2166136261u, hS = 2166136261u, i, nl = 0;
    int j;
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        unsigned v[10];
        v[0] = o->kind; v[1] = o->topo; v[2] = o->vstart; v[3] = o->nv; v[4] = o->ib1; v[5] = o->nsv;
        v[6] = o->has_tex; v[7] = o->outhash; v[8] = o->rq; v[9] = o->nolerp;
        hB = ip_rh_fnv(hB, v, sizeof v);
        hB = ip_rh_fnv(hB, o->mvp, sizeof o->mvp);
        if (o->kind != IP_DRAW) { continue; }
        v[0] = o->flag; v[1] = o->why; v[2] = (unsigned)o->match; v[3] = (unsigned)o->rzb;
        hM = ip_rh_fnv(hM, v, (o->rzb ? 4 : 3) * sizeof v[0]); /* W284: rzb nur wenn gesetzt (Pruefsumme sonst wie bisher) */
        if (o->flag && o->match >= 0 && (unsigned)o->match < A->nops) {
            const ip_op_t *a = &A->ops[o->match];
            v[0] = a->vstart; v[1] = a->nv; v[2] = a->ib1; v[3] = a->nsv; v[4] = a->outhash;
            hA = ip_rh_fnv(hA, v, 5 * sizeof v[0]);
            hA = ip_rh_fnv(hA, a->mvp, sizeof a->mvp);
            nl++;
        }
    }
    {
        unsigned v[8];
        v[0] = (unsigned)vbgA; v[1] = (unsigned)vbgB; v[2] = (unsigned)ring; v[3] = A->gpu_ok; v[4] = B->gpu_ok;
        v[5] = (unsigned)A->wb_ok | ((unsigned)B->wb_ok << 1); v[6] = B->nverts; v[7] = A->nverts;
        hS = ip_rh_fnv(hS, v, sizeof v);
    }
    for (j = 0; j < B->nrefs; j++) {
        const ip_sh_t *e = &g_ish[B->refs[j]];
        unsigned v[2];
        v[0] = (unsigned)B->refs[j];
        v[1] = (ai >= 0 && e->bakser[ai] == A->serial) ? 1u : (e->bakser[bi] == B->serial ? 2u : 0u);
        hS = ip_rh_fnv(hS, v, sizeof v);
    }
    HLOG_I("INTERP rhash f=%u t=%.3f ctx=%d ops=%u lerp=%u hB=%08X hM=%08X hA=%08X hS=%08X", B->frame, (double)t,
           x->deferred, B->nops, nl, hB, hM, hA, hS);
}

/* W295 WORLD-REPROJ (interp_mode=reproj): je Liste Welt-Schatten-Eintrag, Tiefe, letzte Welt-Op; waehrend der
 * Vorbereitung am Swap: Teil-Wiedergaben bis zur letzten Welt-Op (ip_rp_prep, ip_rp_stop). */
static int ip_rp_we[IP_LISTS] = { -1, -1, -1, -1 }, ip_rp_wd[IP_LISTS] = { -1, -1, -1, -1 };
static unsigned ip_rp_last[IP_LISTS], ip_rp_ser[IP_LISTS];
static unsigned st_rp_skip[4]; /* W295 DIAG: Ticks ohne Reprojektion (kein Welt-RT, keine Tiefe, kein Durchgang, Ressourcen) */
static ID3D11RenderTargetView *ip_rp_idrtv; /* Welt-Schatten waehrend der Vorbereitung (ID-Durchgang: auf 0 loeschen) */
static int ip_rp_prep;      /* Vorbereitung am Swap: 1 = exakte Teil-Wiedergabe (Farbe/Tiefe), 2 = Draw-ID */
static unsigned ip_rp_stop; /* ... nur Ops 0..ip_rp_stop (bis zur letzten Welt-Op vor der Komposition) */
#define IP_RP_ISW(o, rw, rd) ((((o)->kind == IP_DRAW || (o)->kind == IP_CLR_RTV) && (o)->sh_rtv && (o)->sh_rtv == (rw)) ||     ((rd) && !(o)->sh_rtv && (o)->kind != IP_CLR_RTV && (o)->sh_dsv == (rd)))

/* W361 LOC-INTERP-CRASH: Sind die Schatten-Eintraege der Liste noch die beim Sichern? (Generation je Eintrag.)
 * Nach Entwurf nie verletzt (Verdraengung nur ohne lebende Sicherung, vorher ip_rt_quiesce); falls doch, bleiben
 * die Views dank refv gueltig - hier nur zaehlen und melden (Bild unveraendert). */
static int ip_list_refs_check(const ip_list_t *L, int li, const char *wo)
{
    int j;
    for (j = 0; j < L->nrefs && j < IP_REFS_MAX; j++) {
        int e = L->refs[j];
        if (e < 0 || e >= IP_SH_MAX || !ip_shref_same(&L->refo[j], g_ish[e].gen)) {
            LONG n = InterlockedIncrement(&st_sh_stale_tot);
            InterlockedIncrement(&st_sh_stale);
            if (n <= 8) {
                HLOG_W("INTERP W361: Liste %d (%s, Tick f=%u ser=%u) nutzt ersetzten Schatten-Eintrag %d (Generation %u -> %u) "
                       "- Views der Liste gehalten, kein Absturz", li, wo, L->frame, L->serial, e, L->refo[j].gen,
                       (e >= 0 && e < IP_SH_MAX) ? g_ish[e].gen : 0u);
            }
            return 0;
        }
    }
    return 1;
}

/* W361: oeffentlicher Referenzzaehler einer View (AddRef/Release-Rueckgabe). */
static ULONG ip_view_refs(ID3D11View *v)
{
    ID3D11View_AddRef(v);
    return ID3D11View_Release(v);
}

/* W361 LOC-INTERP-CRASH: Zaehlerwache der Schatten-Views. Soll = 1 (Eintrag) + je Liste, die den Eintrag mit
 * eigener Referenz haelt (refo), 1. Weniger = fremde Ueberfreigabe (Minidumps P0/A5a: SRV mit Zaehler -4/-3
 * zerstoert, RTV/DSV desselben Eintrags heil) -> melden und die Differenz selbst halten (Leck statt Absturz;
 * Abgabe beim Freigeben des Eintrags). Am Swap unter ctx_lock (fix=1) und vor jedem Render-Thread-Auftrag (fix=0,
 * nur Eintraege der Auftragslisten). */
static volatile LONG st_rc_bad, st_rc_fix, st_rc_dead;
#define IP_DEAD_MAX 8
static ID3D11View *volatile ip_dead_v[IP_DEAD_MAX]; /* tot gefundene Views (Render-Thread), Quarantaene in ip_show */
static volatile LONG ip_dead_n;
static int ip_sh_rc_watch(int fix, const ip_list_t *A, const ip_list_t *B, const char *wo)
{
    int e, k, j, q, dead = 0;
    for (e = 0; e < IP_SH_MAX; e++) {
        ip_sh_t *s = &g_ish[e];
        ID3D11View *vw[3];
        if (!s->res) { continue; }
        if ((A || B) && !fix) { /* nur Eintraege der Auftragslisten */
            int use = 0;
            for (j = 0; B && j < B->nrefs && !use; j++) { use = (B->refs[j] == e); }
            for (j = 0; A && j < A->nrefs && !use; j++) { use = (A->refs[j] == e); }
            if (!use) { continue; }
        }
        vw[0] = (ID3D11View *)s->rtv; vw[1] = (ID3D11View *)s->dsv; vw[2] = (ID3D11View *)s->srv;
        for (q = 0; q < 3; q++) {
            ULONG soll = 1u + s->pin[q], ist;
            if (!vw[q]) { continue; }
            for (k = 0; k < IP_LISTS; k++) {
                const ip_list_t *L = &g_ipl[k];
                if (!fix && L != A && L != B) { continue; } /* Render-Thread: nur gepinnte Listen (Untergrenze) */
                for (j = 0; j < L->nrefs && j < IP_REFS_MAX; j++) {
                    if (L->refs[j] == e && L->refo[j].v[q] == vw[q]) { soll++; }
                }
            }
            ist = ip_view_refs(vw[q]);
            if (fix ? (ist == soll) : (ist >= soll)) { continue; }
            if (ist >= 0x80000000u) { /* d3d11-Zerstoerungsmarke (Zaehler + 0xC0000001): nicht mehr binden */
                LONG n = InterlockedIncrement(&ip_dead_n);
                if (n <= IP_DEAD_MAX) { ip_dead_v[n - 1] = vw[q]; }
                InterlockedIncrement(&st_rc_dead);
                dead = 1;
            }
            if (InterlockedIncrement(&st_rc_bad) <= 24) {
                HLOG_W("INTERP W361 Zaehler (%s): Schatten %d %s %p soll %lu ist %lu (Tick f=%u)%s", wo, e,
                       q == 0 ? "rtv" : (q == 1 ? "dsv" : "srv"), (void *)vw[q], soll, ist, g_present_no,
                       ist >= 0x80000000u ? " -> zerstoert, Bild entfaellt, Quarantaene" :
                       (ist < soll && ist >= 1u ? " -> Differenz gehalten" : ""));
            }
            if (ist < soll && ist >= 1u) { /* lebt noch: Differenz selbst halten (auch im Render-Thread, atomar) */
                ULONG d = soll - ist;
                InterlockedExchangeAdd((volatile LONG *)&s->pin[q], (LONG)d);
                while (d--) { ID3D11View_AddRef(vw[q]); }
                InterlockedIncrement(&st_rc_fix);
            }
        }
    }
    return dead;
}

static void ip_hist_clear(void);
/* W361: tot gemeldete Views unter Quarantaene stellen (nie wieder binden oder freigeben - Release auf ein
 * zerstoertes Objekt waere selbst ein Fehler), Historie verwerfen. Unter ctx_lock, Render-Thread ruhig. */
static void ip_dead_purge(void)
{
    LONG n = ip_dead_n, i;
    int e, k, j, q;
    if (n > IP_DEAD_MAX) { n = IP_DEAD_MAX; }
    ip_rt_quiesce();
    for (i = 0; i < n; i++) {
        ID3D11View *d = ip_dead_v[i];
        if (!d) { continue; }
        for (k = 0; k < IP_LISTS; k++) {
            ip_list_t *L = &g_ipl[k];
            for (j = 0; j < L->nrefs && j < IP_REFS_MAX; j++) {
                for (q = 0; q < 3; q++) { if (L->refo[j].v[q] == d) { L->refo[j].v[q] = NULL; } }
            }
        }
        for (e = 0; e < IP_SH_MAX; e++) {
            ip_sh_t *s = &g_ish[e];
            int hit = 0;
            if ((ID3D11View *)s->rtv == d) { s->rtv = NULL; s->pin[0] = 0; hit = 1; }
            if ((ID3D11View *)s->dsv == d) { s->dsv = NULL; s->pin[1] = 0; hit = 1; }
            if ((ID3D11View *)s->srv == d) { s->srv = NULL; s->pin[2] = 0; hit = 1; }
            if (hit && s->res) { ip_sh_free(s); } /* Rest frei, Eintrag wird beim naechsten Sichern neu angelegt */
        }
        ip_dead_v[i] = NULL;
    }
    ip_dead_n = 0;
    ip_hist_clear(); /* Ops der Listen zeigen auf die tote View */
    HLOG_W("INTERP W361: %ld zerstoerte Schatten-View(s) in Quarantaene, Historie verworfen (Zwischenbilder setzen kurz aus)", n);
}

/* ---------- W376 LOC-LOCAL-AIM: eigene Sicht sofort im Bild ----------
 * Je Wiedergabe (Tick- und Zwischenbild) das Vorhersage-Delta der Sicht lerp(A, B, t) als Kameradrehung C auf die
 * Welt-Draws (o->law, gesetzt am Swap) anwenden: mvp' = mvp * C, ebenso die Matrix N-1. Spielzustand unberuehrt.
 * Vorzeichen (Messlauf W376_A2): Sicht-Gier des Spiels faellt beim Blick nach rechts, Nick steigt beim Blick nach oben. */
static const double LA_TURN_K = -1.0, LA_NICK_K = 1.0, LA_MAXANG = 0.6;
static volatile LONG li_la_rep, li_la_on; /* Wiedergaben mit Liste gueltig / mit Korrektur (LOCAIM-Zeile) */
static volatile LONG li_la_lists, li_la_draws, li_la_rets; /* Listen mit Vorhersage, Welt-/Fadenkreuz-Draws darin */
static void ip_la_mul(float out[16], const float m[16], const float *C)
{
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            out[i * 4 + j] = m[i * 4] * C[j] + m[i * 4 + 1] * C[4 + j] + m[i * 4 + 2] * C[8 + j] + m[i * 4 + 3] * C[12 + j];
}
/* DIAG(W376) loc_aim_diag Bit 32: je Wiedergabe Zeit, Tick, t, Drehung (angewandt bzw. mit Schalter aus: berechnet) */
static CRITICAL_SECTION li_la_cs;
static volatile LONG li_la_cs_init;
static void li_la_note(const ip_list_t *B, float t, double dy, double dp, int on, double rx, double ry)
{
    static FILE *f;
    struct { LONGLONG q; unsigned ser, seq; float t, dy, dp; int on; float rx, ry; } r;
    LARGE_INTEGER q;
    if (InterlockedCompareExchange(&li_la_cs_init, 1, 0) == 0) { InitializeCriticalSection(&li_la_cs); InterlockedExchange(&li_la_cs_init, 2); }
    while (li_la_cs_init != 2) { Sleep(0); }
    QueryPerformanceCounter(&q);
    r.q = q.QuadPart; r.ser = B->serial; r.seq = B->la_seq; r.t = t; r.dy = (float)dy; r.dp = (float)dp; r.on = on;
    r.rx = (float)rx; r.ry = (float)ry;
    EnterCriticalSection(&li_la_cs);
    if (!f) { f = (FILE *)loc_aim_diag_file("locaim_rep.bin"); }
    if (f) { fwrite(&r, sizeof r, 1, f); fflush(f); }
    LeaveCriticalSection(&li_la_cs);
}
/* W394 LOC-VIEW-PERFRAME: angezeigte Sicht/Arm (absolut, rad) fuer diese Wiedergabe. Basis = juengste Vorhersage (D eigene
 * Saetze), dazu ein Zusatz-Schritt mit dem Satz, den der naechste Poll jetzt liefern wuerde (Maus-Akku nur gelesen),
 * gewichtet mit der Phase bis zu diesem Poll (w 0 -> 1). Zwischen Poll und naechster Vorhersage (der Satz ist schon
 * abgeholt, aber noch nicht in der Vorhersage) gilt der letzte volle Zusatz-Schritt. Rueckgabe 0 = nichts (alter Weg),
 * 1 = Basis ohne Zusatz-Schritt, 2 = mit Zusatz-Schritt, 3 = gehalten. */
static SRWLOCK li_vpf_lk = SRWLOCK_INIT;
static la_state_t li_vpf_full;
static uint32_t li_vpf_full_seq;
static volatile LONG li_vpf_n[4]; /* LOCVPF-Zeile: Wiedergaben je Rueckgabe */
static int li_vpf_view(double v[4])
{
    loc_aim_pub_t P;
    uint8_t F[14];
    uint32_t q = 0;
    la_state_t s1;
    LARGE_INTEGER now;
    int r, mode;
    double w;
    if (!loc_aim_get(&P) || !P.have_pst) { InterlockedIncrement(&li_vpf_n[0]); return 0; }
    r = loc_vpf_preview(P.last_in, F, &q);
    if (r) (void)loc_aimassist_preview(&P.pst, P.last_in, F); /* W442: naechster Poll mit Zielhilfe (sonst Sicht-Ruckeln) */
    QueryPerformanceCounter(&now);
    AcquireSRWLockExclusive(&li_vpf_lk);
    if (r && q == P.poll) {
        s1 = P.pst;
        if (!la_step(&s1, F)) { s1 = P.pst; }
        w = lh_weight((double)(now.QuadPart - P.tq) * 1000.0 / (double)ip_qpf.QuadPart, P.wspan_ms);
        lh_blend(&P.pst, &s1, w, v);
        li_vpf_full = s1;
        li_vpf_full_seq = P.seq;
        mode = 2;
    } else if (r && q == P.poll + 1u && li_vpf_full_seq == P.seq) {
        v[0] = li_vpf_full.ys; v[1] = li_vpf_full.ps; v[2] = li_vpf_full.ax; v[3] = li_vpf_full.ay;
        mode = 3;
    } else {
        v[0] = P.pst.ys; v[1] = P.pst.ps; v[2] = P.pst.ax; v[3] = P.pst.ay;
        mode = 1;
    }
    ReleaseSRWLockExclusive(&li_vpf_lk);
    InterlockedIncrement(&li_vpf_n[mode]);
    return mode;
}
/* DIAG(W394) loc_hud_diag Bit 4: je Wiedergabe angezeigte Sicht-Gier und Fadenkreuz-Mitte (nur Testfolge) */
static void li_vpf_note(const ip_list_t *B, float t, int mode, double yaw, double rcx, double rcy, double syaw, double sx)
{
    static FILE *f;
    static SRWLOCK lk = SRWLOCK_INIT;
    struct { LONGLONG q; unsigned ser; float t; int mode; float yaw, rcx, rcy, syaw, sx; } r;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    r.q = q.QuadPart; r.ser = B->serial; r.t = t; r.mode = mode; r.yaw = (float)yaw; r.rcx = (float)rcx; r.rcy = (float)rcy;
    r.syaw = (float)syaw; r.sx = (float)sx;
    AcquireSRWLockExclusive(&lk);
    if (!f) { f = (FILE *)loc_aim_diag_file("locvpf_rep.bin"); }
    if (f) { fwrite(&r, sizeof r, 1, f); }
    ReleaseSRWLockExclusive(&lk);
}

/* ---------- W439 LOC-RETICLE-JUMP: Messung Fadenkreuz je Wiedergabe ----------
 * Soll = angezeigte Fadenkreuz-Mitte laut Sicht je Bild (bx + rx); gezeichnet = Mitte, wie die Fadenkreuz-Draws dieser
 * Wiedergabe tatsaechlich liegen (Bezugs-Vertex gelerpt bzw. exakt aus Tick N, plus Verschiebung M[1], minus Versatz des
 * Draws zur simulierten Mitte). Nur Spielzustand und Bildlagen, keine Eingaben. */
typedef struct {
    int stage;            /* 0 = nicht in LoC/aus, 1 = B ohne Vorhersage (la_ok 0), 2 = normal */
    int mode;             /* li_vpf_view-Rueckgabe (0..3) */
    double tx, ty;        /* Soll-Mitte (NDC) */
    double bx, by;        /* simulierte Mitte (lerp A/B) */
    double vy, vp;        /* angezeigte Sicht Gier/Nick (rad) */
    double rx, ry;        /* Fadenkreuz-Verschiebung dieser Wiedergabe */
} li_ret_t;
static volatile LONG lr_rep, lr_ret, lr_lost, lr_exact, lr_nola, lr_jumps, lr_jumps_all;
static volatile LONG lr_retx; /* W439: Fadenkreuz-Draws mit Verschiebung gegen B (exakt gezeichnet) */
static volatile LONG lr_rjmv; /* W439: Fadenkreuz-Kandidaten, die die Bewegungs-Pruefung verwarf */
/* W444: Listen mit Fadenkreuz, Teile je Regel (Form/Partner/Start), verworfene fremde Start-Kandidaten, Listen mit um das Gedaechtnis ergaenzter Vorlage */
static volatile LONG lrm_st_lists, lrm_st_form, lrm_st_pair, lrm_st_seed, lrm_st_fremd, lrm_st_mem;
static double lr_err_max;
static int lr_hist_n;
static double lr_hist[3][2];
static LONGLONG lr_hist_q[3];
/* F11: Sprung-Zaehler (nur Zaehler) */
unsigned loc_ret_jumps(void) { return (unsigned)lr_jumps_all; }
static void li_ret_note(const ip_list_t *B, const ip_list_t *A, int lerp_ok, float t, int on, const li_ret_t *rr,
                        float M[][16])
{
    static FILE *f;
    static SRWLOCK lk = SRWLOCK_INIT;
    struct {
        LONGLONG q; unsigned ser, aser; float t; int lerp_ok, stage, mode, on, retB, retA, nret, nlerp, nlost;
        float tx, ty, gx, gy, bx, by, vy, vp, rx, ry, rcBx, rcBy, rn0x, rn0y; unsigned rb, rjB, nrA;
        int fi, fm; float dx, dy; /* erster Fadenkreuz-Draw: Op-Index, Partner in A, gezeichnete Lage (Bezugs-Vertex) */
        int npl; float epl;       /* Teile ohne Markierung in B, deren Partner in A Fadenkreuz war; groesste Abweichung vom Soll */
        float gx2, gy2, epl2, emk2; /* W444: Mitte mit Teil-Versatz je Teil (gelerpt zwischen A und B), groesste Abweichung je Teil
                                   * vom eigenen Soll (Lage ohne Vorhersage + Verschiebung) fuer unmarkierte bzw. markierte Teile */
    } r;
    LARGE_INTEGER q;
    double gx = 0.0, gy = 0.0, gx2 = 0.0, gy2 = 0.0;
    int n = 0, nl = 0, nlost = 0;
    unsigned i;
    if (!g_sba_loc) { return; }
    memset(&r, 0, sizeof r);
    r.rn0x = r.rn0y = 9.0f;
    InterlockedIncrement(&lr_rep);
    if (rr->stage == 2 && B->la_ret) {
        for (i = 0; i < B->nops; i++) {
            const ip_op_t *o = &B->ops[i];
            double px, py;
            int lp;
            if (o->kind != IP_DRAW || !LA_IS_RET(o->law)) { continue; }
            lp = lerp_ok && o->flag && o->match >= 0 && A && (unsigned)o->match < A->nops;
            px = o->rn[0][0]; py = o->rn[0][1];
            if (lp) { const ip_op_t *a = &A->ops[o->match]; px = a->rn[0][0] + (px - a->rn[0][0]) * (double)t; py = a->rn[0][1] + (py - a->rn[0][1]) * (double)t; nl++; }
            if (LA_ON(on, o->law)) { px += M[o->law - 1][12]; py += M[o->law - 1][13]; } /* angewandte Verschiebung */
            gx += px - ((double)o->rn[0][0] - B->la_rc[0]);
            gy += py - ((double)o->rn[0][1] - B->la_rc[1]);
            { /* W444: Soll je Teil = eigener Versatz zur simulierten Mitte (gelerpt wie der Draw) + Soll-Mitte */
                double ox = (double)o->rn[0][0] - B->la_rc[0], oy = (double)o->rn[0][1] - B->la_rc[1], e2;
                if (lp && A->la_ok) {
                    const ip_op_t *a = &A->ops[o->match];
                    ox = ((double)a->rn[0][0] - A->la_rc[0]) + (ox - ((double)a->rn[0][0] - A->la_rc[0])) * (double)t;
                    oy = ((double)a->rn[0][1] - A->la_rc[1]) + (oy - ((double)a->rn[0][1] - A->la_rc[1])) * (double)t;
                }
                gx2 += px - ox; gy2 += py - oy;
                e2 = sqrt((px - ox - rr->tx) * (px - ox - rr->tx) + (py - oy - rr->ty) * (py - oy - rr->ty));
                if (e2 > r.emk2) { r.emk2 = (float)e2; }
            }
            if (!n) { r.rn0x = o->rn[0][0]; r.rn0y = o->rn[0][1]; r.fi = (int)i; r.fm = o->match; r.dx = (float)px; r.dy = (float)py; }
            n++;
        }
        if (A && A->la_ret && lerp_ok) { /* Teile, die B nicht markierte (ohne Verschiebung gezeichnet) */
            for (i = 0; i < B->nops; i++) {
                const ip_op_t *o = &B->ops[i];
                const ip_op_t *a;
                double px, py, e;
                if (o->kind != IP_DRAW || LA_IS_RET(o->law) || o->match < 0 || (unsigned)o->match >= A->nops) { continue; }
                a = &A->ops[o->match];
                if (!LA_IS_RET(a->law)) { continue; }
                px = o->rn[0][0]; py = o->rn[0][1];
                if (o->flag) { px = a->rn[0][0] + (px - a->rn[0][0]) * (double)t; py = a->rn[0][1] + (py - a->rn[0][1]) * (double)t; }
                px -= (double)a->rn[0][0] - A->la_rc[0];
                py -= (double)a->rn[0][1] - A->la_rc[1];
                e = sqrt((px - rr->tx) * (px - rr->tx) + (py - rr->ty) * (py - rr->ty));
                r.npl++;
                if (e > r.epl) { r.epl = (float)e; }
                /* W444: fehlende Verschiebung = Abweichung vom eigenen Soll (gelerpt rx, exakt Soll - Mitte B) */
                e = o->flag ? sqrt(rr->rx * rr->rx + rr->ry * rr->ry)
                            : sqrt((rr->tx - B->la_rc[0]) * (rr->tx - B->la_rc[0]) + (rr->ty - B->la_rc[1]) * (rr->ty - B->la_rc[1]));
                if (e > r.epl2) { r.epl2 = (float)e; }
            }
        }
    } else if (A && A->la_ret && lerp_ok) {
        /* Fadenkreuz in A markiert, in B nicht (bzw. B ohne Vorhersage): Partner der A-Fadenkreuz-Draws ohne Verschiebung */
        for (i = 0; i < B->nops; i++) {
            const ip_op_t *o = &B->ops[i];
            const ip_op_t *a;
            double px, py;
            if (o->kind != IP_DRAW || o->match < 0 || (unsigned)o->match >= A->nops) { continue; }
            a = &A->ops[o->match];
            if (!LA_IS_RET(a->law)) { continue; }
            px = o->rn[0][0]; py = o->rn[0][1];
            if (o->flag) { px = a->rn[0][0] + (px - a->rn[0][0]) * (double)t; py = a->rn[0][1] + (py - a->rn[0][1]) * (double)t; nl++; }
            gx += px - ((double)a->rn[0][0] - A->la_rc[0]);
            gy += py - ((double)a->rn[0][1] - A->la_rc[1]);
            nlost++;
            n++;
        }
    }
    if (n) { gx /= n; gy /= n; gx2 /= n; gy2 /= n; }
    if (rr->stage == 1) { InterlockedIncrement(&lr_nola); }
    if (n && !nlost) { InterlockedIncrement(&lr_ret); }
    if (nlost) { InterlockedIncrement(&lr_lost); }
    if (n && nl < n) { InterlockedIncrement(&lr_exact); }
    QueryPerformanceCounter(&q);
    { /* Sprung: gezeichnete Mitte weicht vom Mittel der Nachbarn ab und kehrt zurueck (Nachbar-Abstand klein) */
        AcquireSRWLockExclusive(&lk);
        if (n) {
            double e;
            lr_hist[0][0] = lr_hist[1][0]; lr_hist[0][1] = lr_hist[1][1]; lr_hist_q[0] = lr_hist_q[1];
            lr_hist[1][0] = lr_hist[2][0]; lr_hist[1][1] = lr_hist[2][1]; lr_hist_q[1] = lr_hist_q[2];
            lr_hist[2][0] = gx; lr_hist[2][1] = gy; lr_hist_q[2] = q.QuadPart;
            if (lr_hist_n < 3) { lr_hist_n++; }
            if (lr_hist_n == 3 && (double)(lr_hist_q[2] - lr_hist_q[0]) * 1000.0 / (double)ip_qpf.QuadPart < 120.0) {
                double mx = 0.5 * (lr_hist[0][0] + lr_hist[2][0]), my = 0.5 * (lr_hist[0][1] + lr_hist[2][1]);
                double ex = lr_hist[1][0] - mx, ey = lr_hist[1][1] - my;
                double nx = lr_hist[2][0] - lr_hist[0][0], ny = lr_hist[2][1] - lr_hist[0][1];
                e = sqrt(ex * ex + ey * ey);
                if (e > lr_err_max) { lr_err_max = e; }
                if (e > 0.02 && e > 2.0 * sqrt(nx * nx + ny * ny)) { InterlockedIncrement(&lr_jumps); InterlockedIncrement(&lr_jumps_all); }
            }
        } else {
            lr_hist_n = 0;
        }
        ReleaseSRWLockExclusive(&lk);
    }
    if (!(g_cfg.loc_ret_diag & 1) || (!g_cfg.loc_replay_tick[0] && !g_cfg.loc_steps[0])) { return; }
    r.q = q.QuadPart; r.ser = B->serial; r.aser = A ? A->serial : 0u; r.t = t; r.lerp_ok = lerp_ok; r.stage = rr->stage;
    r.mode = rr->mode; r.on = on; r.retB = B->la_ret; r.retA = A ? A->la_ret : 0; r.nret = n; r.nlerp = nl; r.nlost = nlost;
    r.tx = (float)rr->tx; r.ty = (float)rr->ty; r.gx = n ? (float)gx : 9.0f; r.gy = n ? (float)gy : 9.0f;
    r.gx2 = n ? (float)gx2 : 9.0f; r.gy2 = n ? (float)gy2 : 9.0f;
    r.bx = (float)rr->bx; r.by = (float)rr->by; r.vy = (float)rr->vy; r.vp = (float)rr->vp; r.rx = (float)rr->rx; r.ry = (float)rr->ry;
    r.rcBx = B->la_rc[0]; r.rcBy = B->la_rc[1];
    r.rjB = B->la_rjmv; r.nrA = A ? A->la_nret : 0u;
    { loc_rb_hud h; r.rb = loc_rb_hud_read(&h) ? h.rb_total : 0u; }
    AcquireSRWLockExclusive(&lk);
    if (!f) { f = (FILE *)loc_aim_diag_file("locret_rep.bin"); }
    if (f) { fwrite(&r, sizeof r, 1, f); }
    ReleaseSRWLockExclusive(&lk);
}

/* W394 LOC-HUD-VIEW: Marken je VT (o->law 3 + j) um die Abweichung der angezeigten von der simulierten Bildlage
 * verschieben: b = c * C (C = Sicht-Korrektur dieser Wiedergabe, sonst Einheit), Verschiebung b/w - a (Clip-Translation).
 * Rueckgabe: Bits 2 + j. */
static volatile LONG li_hm_rep, li_hm_on; /* LOCHUD-Zeile */
static volatile LONG li_hm_lists, li_hm_draws, li_hm_cmp, li_hm_near; /* LOCHUD-Zeile (near: Gegner < 100 m im Bild) */
static double li_hm_res_max, li_hm_res_sum; static unsigned li_hm_res_n;
static volatile LONG li_hm_rad; /* Listen mit Radar-Draws */
static volatile LONG li_hm_cand, li_hm_rej_mv, li_hm_rej_st; /* Kandidaten nahe a_j, verworfen (Bewegung, ruhend) */
static double li_rad_k; static int li_rad_n; /* Radar: Drehung der Karte je rad eigener Gier (+-1 nach 6 gleichen Messungen) */ /* Rest: Verschiebung aus B gegen lerp(A, B, t) */
static double li_cmp_k = 0.477; /* NDC je rad Gier (Eichung C3; laufend nachgefuehrt) */

static double li_hm_px_max, li_hm_px_sum; static unsigned li_hm_px_n;
static int ip_hm_prep(const ip_list_t *B, const ip_list_t *A, float t, const float *C, float M[LA_NM][16])
{
    int j, on = 0;
    InterlockedIncrement(&li_hm_rep);
    for (j = 0; j < LS_MAXV; j++) {
        double dx, dy, ax, ay, r;
        if (!(B->hm_vt & (1u << j))) { continue; }
        if (!lh_mark_shift(B->hm_c[j], C, B->hm_a[j][0], B->hm_a[j][1], &dx, &dy)) { continue; }
        if (A && A->hm_ok && A->hm_c[j][3] > 0.0 && lh_mark_shift(A->hm_c[j], C, A->hm_a[j][0], A->hm_a[j][1], &ax, &ay)) {
            /* Statistik: angewandt (aus B) gegen lerp(A, B, t) */
            r = (1.0 - t) * sqrt((ax - dx) * (ax - dx) * 102400.0 + (ay - dy) * (ay - dy) * 57600.0);
            if (r > li_hm_res_max) { li_hm_res_max = r; } li_hm_res_sum += r; li_hm_res_n++;
        }
        if (fabs(dx) + fabs(dy) < 1e-5) { continue; }
        memset(M[2 + j], 0, 64);
        M[2 + j][0] = M[2 + j][5] = M[2 + j][10] = M[2 + j][15] = 1.0f;
        M[2 + j][12] = (float)dx;
        M[2 + j][13] = (float)dy;
        on |= 1 << (2 + j);
        { double px = sqrt(dx * 320.0 * dx * 320.0 + dy * 240.0 * dy * 240.0); /* Statistik (ungeschuetzt, nur Anzeige) */
          if (px > li_hm_px_max) { li_hm_px_max = px; } li_hm_px_sum += px; li_hm_px_n++; }
    }
    if (on) { InterlockedIncrement(&li_hm_on); }
    return on;
}

static int ip_la_prep0(const ip_list_t *B, const ip_list_t *A, int lerp_ok, float t, int dbg, int vr, float M[LA_NM][16],
                       li_ret_t *rr);
/* Rueckgabe Bitmaske: 1 = Kamera-Korrektur M[0] (o->law 1), 2 = Fadenkreuz-Verschiebung M[1] (o->law 2),
 * 4 << j = Marken des VT j (W394). */
static int ip_la_prep(const ip_list_t *B, const ip_list_t *A, int lerp_ok, float t, int dbg, int vr, float M[LA_NM][16])
{
    li_ret_t rr;
    int on;
    memset(&rr, 0, sizeof rr);
    on = ip_la_prep0(B, A, lerp_ok, t, dbg, vr, M, &rr);
    if (rr.stage) { li_ret_note(B, A, lerp_ok, t, on, &rr, M); } /* W439: Fadenkreuz-Zeitreihe/Sprung-Zaehler */
    if (g_sba_loc && (g_cfg.loc_wlag_diag & 4) && !dbg && !vr && B->la_ok) { /* DIAG(W435): Himmel in Wiedergaben mit Sicht-Korrektur */
        InterlockedIncrement(&sk_rep);
        if (on & 1) {
            InterlockedIncrement(&sk_rep_on);
            InterlockedExchangeAdd(&sk_rep_sky, (LONG)B->la_nsk);
            InterlockedExchangeAdd(&sk_rep_skyc, (LONG)B->la_nskl);
            InterlockedExchangeAdd(&sk_rep_na, (LONG)B->la_nna);
            sk_ang_sum += sk_last_ang; /* nur Anzeige-Statistik */
            if (sk_last_ang > sk_ang_max) { sk_ang_max = sk_last_ang; }
        }
    }
    if (g_sba_loc && g_cfg.loc_hud_predict && !dbg && !vr && !ip_ph.on && !ip_pl_on && g_cfg.interp_mode != 1) {
        if (B->hm_ok && B->hm_vt) { on |= ip_hm_prep(B, lerp_ok ? A : NULL, t, (on & 1) ? M[0] : NULL, M); }
        if (B->hm_ncmp && fabs(B->hm_dyaw) > 1e-6) { /* Kompassband: eigene Gier angezeigt - simuliert (W390) */
            float *m = M[LA_LAW_CMP - 1];
            memset(m, 0, 64);
            m[0] = m[5] = m[10] = m[15] = 1.0f;
            m[12] = (float)la_clamp(li_cmp_k * B->hm_dyaw, -0.5, 0.5);
            on |= 1 << (LA_LAW_CMP - 1);
        }
        if (B->hm_nrad && li_rad_n >= 6 && fabs(B->hm_dyaw) > 1e-6) { /* Radar: Karte um die Bildmitte drehen */
            float *m = M[LA_LAW_RAD - 1];
            double ph = la_clamp(li_rad_k * B->hm_dyaw, -0.5, 0.5), cs = cos(ph), sn = sin(ph);
            memset(m, 0, 64);
            m[0] = (float)cs; m[1] = (float)sn; m[4] = (float)-sn; m[5] = (float)cs; m[10] = m[15] = 1.0f;
            on |= 1 << (LA_LAW_RAD - 1);
        }
    }
    return on;
}
static int ip_la_prep0(const ip_list_t *B, const ip_list_t *A, int lerp_ok, float t, int dbg, int vr, float M[LA_NM][16],
                       li_ret_t *rr)
{
    static const double up[3] = {0.0, 1.0, 0.0};
    double dy, dp, Cd[16], rx, ry;
    int i, apply, on = 0;
    if (!g_sba_loc || dbg || vr || ip_ph.on || ip_pl_on || g_cfg.interp_mode == 1 || !B->la_ok) {
        if (g_sba_loc && !dbg && !vr && !ip_ph.on && !ip_pl_on && g_cfg.interp_mode != 1 && g_cfg.loc_local_aim) { rr->stage = 1; }
        return 0;
    }
    apply = g_cfg.loc_local_aim != 0;
    dy = B->la_d[0]; dp = B->la_d[1];
    if (lerp_ok && A && A->la_ok) {
        dy = A->la_d[0] + (B->la_d[0] - A->la_d[0]) * (double)t;
        dp = A->la_d[1] + (B->la_d[1] - A->la_d[1]) * (double)t;
    }
    dy = la_clamp(dy * LA_TURN_K, -LA_MAXANG, LA_MAXANG);
    dp = la_clamp(dp * LA_NICK_K, -LA_MAXANG, LA_MAXANG);
    rx = ry = 0.0;
    if (B->la_ret) {
        rx = B->la_rd[0]; ry = B->la_rd[1];
        if (lerp_ok && A && A->la_ok) {
            rx = A->la_rd[0] + (B->la_rd[0] - A->la_rd[0]) * (double)t;
            ry = A->la_rd[1] + (B->la_rd[1] - A->la_rd[1]) * (double)t;
        }
    }
    if (apply && (g_cfg.loc_view_perframe || (g_cfg.loc_hud_diag & 4))) { /* W394 LOC-VIEW-PERFRAME */
        double v[4], sy = B->la_s[0], sp = B->la_s[1], bx = B->la_rc[0], by = B->la_rc[1];
        double c1 = sqrt(B->la_vp[1] * B->la_vp[1] + B->la_vp[5] * B->la_vp[5] + B->la_vp[9] * B->la_vp[9]);
        double c3 = sqrt(B->la_vp[3] * B->la_vp[3] + B->la_vp[7] * B->la_vp[7] + B->la_vp[11] * B->la_vp[11]);
        double py = c3 > 0.0 ? c1 / c3 : 0.0;
        int mode = 0;
        if (lerp_ok && A && A->la_ok) { /* so zeichnet die Zwischenbild-Wiedergabe den simulierten Stand */
            sy = la_wrap(A->la_s[0] + la_wrap(B->la_s[0] - A->la_s[0]) * (double)t);
            sp = la_wrap(A->la_s[1] + la_wrap(B->la_s[1] - A->la_s[1]) * (double)t);
            bx = A->la_rc[0] + (B->la_rc[0] - A->la_rc[0]) * (double)t;
            by = A->la_rc[1] + (B->la_rc[1] - A->la_rc[1]) * (double)t;
        }
        if (g_cfg.loc_view_perframe && !loc_vpf_test_off() && (mode = li_vpf_view(v)) != 0) {
            dy = la_clamp(la_wrap(v[0] - sy) * LA_TURN_K, -LA_MAXANG, LA_MAXANG);
            dp = la_clamp(la_wrap(v[1] - sp) * LA_NICK_K, -LA_MAXANG, LA_MAXANG);
            if (B->la_ret) {
                rx = -py * tan(la_clamp(v[2] - v[0], -1.4, 1.4)) - bx;
                ry = -py * tan(la_clamp(v[3] + v[1], -1.4, 1.4)) - by;
            }
        }
        if ((g_cfg.loc_hud_diag & 4) && (g_cfg.loc_replay_tick[0] || g_cfg.loc_steps[0])) {
            li_vpf_note(B, t, mode, sy + dy / LA_TURN_K, bx + rx, by + ry, sy, bx);
        }
        rr->stage = 2; rr->mode = mode; rr->tx = bx + rx; rr->ty = by + ry; rr->bx = bx; rr->by = by; /* W439 */
        rr->vy = sy + dy / LA_TURN_K; rr->vp = sp + dp / LA_NICK_K; rr->rx = rx; rr->ry = ry;
    }
    if (g_cfg.loc_aim_diag & 32) { li_la_note(B, t, dy, dp, apply, rx, ry); }
    sk_last_ang = sqrt(dy * dy + dp * dp); /* DIAG(W435): Winkel der Sicht-Korrektur dieser Wiedergabe (rad) */
    if (!apply) { return 0; }
    InterlockedIncrement(&li_la_rep);
    if (fabs(dy) + fabs(dp) >= 1e-6 && la_cam_corr(Cd, B->la_vp, up, dy, dp)) {
        for (i = 0; i < 16; i++) { M[0][i] = (float)Cd[i]; }
        on |= 1;
    }
    if (B->la_ret) { /* Fadenkreuz: Clip-Verschiebung x += dx * w, y += dy * w (Zeilenvektor: Zeile 3) */
        if (fabs(rx) + fabs(ry) >= 1e-5) {
            memset(M[1], 0, 64);
            M[1][0] = M[1][5] = M[1][10] = M[1][15] = 1.0f;
            M[1][12] = (float)la_clamp(rx, -1.0, 1.0); M[1][13] = (float)la_clamp(ry, -1.0, 1.0);
            on |= 2;
        }
        if (g_cfg.loc_ret_fix) {
            /* W439 LOC-RETICLE-JUMP: Die Verschiebung rx = Soll - lerp(A, B, t) der simulierten Mitte gilt nur fuer
             * gelerpte Fadenkreuz-Draws. Draws, die im Zwischenbild exakt aus Tick N gezeichnet werden (Sprung zu gross
             * fuer die strenge 2D-Zuordnung, o->law LA_LAW_RETX), liegen bei der Mitte von B -> Soll - B. Vorher lag das
             * Fadenkreuz dort bei jedem Tick um (1 - t) * Tickschritt neben dem Soll (Ruck beim schnellen Zielen). */
            double e2[2], ex, ey;
            lh_ret_exact_shift(rx, ry, A ? A->la_rc : B->la_rc, B->la_rc, (double)t, lerp_ok && A && A->la_ok, e2);
            ex = e2[0]; ey = e2[1];
            if (fabs(ex) + fabs(ey) >= 1e-5) {
                float *m = M[LA_LAW_RETX - 1];
                memset(m, 0, 64);
                m[0] = m[5] = m[10] = m[15] = 1.0f;
                m[12] = (float)la_clamp(ex, -1.0, 1.0); m[13] = (float)la_clamp(ey, -1.0, 1.0);
                on |= 1 << (LA_LAW_RETX - 1);
            }
        }
    }
    if (on) { InterlockedIncrement(&li_la_on); }
    return on;
}

static void ip_replay_ex(ip_list_t *B, int bi, ip_list_t *A, int ai, float t, int dbg, const ip_cx_t *x)
{
    int cps = -1; /* DIAG: 1 = Diagnose-PS gebunden */
    unsigned cwhy = 0xFFFFFFFFu;
    ip_state_t sv;
    unsigned i;
    UINT stride = sizeof(im_vertex_t), zero = 0;
    ID3D11RenderTargetView *crtv = (ID3D11RenderTargetView *)1;
    ID3D11DepthStencilView *cdsv = (ID3D11DepthStencilView *)1;
    ID3D11BlendState *cbs = (ID3D11BlendState *)1;
    ID3D11DepthStencilState *cds = (ID3D11DepthStencilState *)1;
    UINT csref = 0xFFFFFFFFu;
    ID3D11RasterizerState *crs = (ID3D11RasterizerState *)1;
    ID3D11ShaderResourceView *csrv = (ID3D11ShaderResourceView *)1;
    D3D11_VIEWPORT cvp;
    D3D11_RECT csc;
    unsigned ctopo = 0xFFFFFFFFu;
    int mode = -1; /* 0 exakt-VS, 1 interp-VS */
    int rq_dirty = 0; /* RENDER-QA: letzter Draw war ein Shading-Draw */
    ID3D11SamplerState *csmp = (ID3D11SamplerState *)1; /* FIX(TEXADDR,W84): gebundener s0 (1 = unbekannt) */
    int lerp_ok = (A != NULL && t > 0.0f && t < 1.0f && A->ready && B->pair_ok &&
                   B->prevser == A->serial);
    fxr_rep_t fxs; /* W133 REM-FX: Effekt-PS/Tiefen-Kopie der Wiedergabe */
    sb_vr_pass_t *vrp = (ip_vrp && !x->deferred && !dbg) ? ip_vrp : NULL; /* W134 VR-PROTO */
    int vrbb = 0, vrkind = -1; /* W134 VR-PROTO */
    int ccm = 0; /* W146 GPU-COST: Cockpit-Cache 0 aus, 1 aufbauen, 2 nutzen */
    int vtk = 0; /* W183 VR-FIX3: 1 = RT-Ops uebersprungen (Tick-Endstand) */
    ip_rc_t rc; /* DIAG(PERF-M2,W174) ip_gdiag */
    unsigned var = 0u, rcur[3] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu }; /* W174: Variante, gebundene Ring-Plaetze VS b0/b1, PS b0 */
    int ring = 0, vbgB = 0, vbgA = 0; /* PERF(PERF-M2,W174) */
    unsigned *rofs = NULL;
    ID3D11Buffer *bvb, *avb = NULL; /* W174: Tick-VB von B/A (GPU-Kopie oder DYNAMIC) */
    ID3D11GeometryShader *cgd = NULL; /* FIX(W265 LINE-GUARD): gebundener Waechter-GS */
    int gdon, gdq = -1;
    ID3D11RenderTargetView *rpw = NULL; /* W295 WORLD-REPROJ: Welt-Schatten (Welt-Ops bis rplast durch den Warp ersetzen) */
    ID3D11DepthStencilView *rpd = NULL;
    unsigned rplast = 0xFFFFFFFFu;
    float la_M[LA_NM][16], la_tmp[16]; /* W376 LOC-LOCAL-AIM, W394 */
    int la_on = ip_la_prep(B, A, lerp_ok, t, dbg, vrp != NULL, la_M);
#define IP_LAM(o, m) (LA_ON(la_on, (o)->law) ? (ip_la_mul(la_tmp, (m), la_M[(o)->law - 1]), (const float *)la_tmp) : (const float *)(m))
    (void)ip_list_refs_check(B, bi, "B"); /* W361 */
    if (A) { (void)ip_list_refs_check(A, ai, "A"); }
    fxr_rep_begin(&fxs);
    memset(&rc, 0, sizeof rc);
    if (!dbg && !vrp) {
        unsigned ab = (unsigned)g_cfg.ip_ablate;
        var = ip_var_pick(B, t);
        ring = (ab & 80u) ? (var == 16u || var == 64u) : (g_cfg.ip_cb_ring > 0);
        vbgB = (ab & 96u) ? (var == 32u || var == 64u) : (g_cfg.ip_vb_gpu > 0);
        if (ip_pl_on) { ring = 0; var = 0u; } /* W193: Konstanten je Draw (MVP-Haken), keine Diagnose-Varianten */
    }
    vbgA = vbgB && A && (A->gpu_ok & 1u);
    vbgB = vbgB && (B->gpu_ok & 1u);
    bvb = (ip_pl_on && ip_pl_vb) ? ip_pl_vb : ip_vbsel(B, vbgB);
    if (A) { avb = ip_vbsel(A, vbgA); }
    if ((g_cfg.interp_rhash & 1) && lerp_ok && !dbg) { ip_rh_inputs(B, bi, A, ai, t, x, vbgA, vbgB, ring); } /* DIAG(W273) */
    rc.var = var;
    rc.nops = B->nops;
    rc.nverts = B->nverts;
    rc.rqxb = B->rqxpos;
    rc.vbg = (unsigned)vbgB;
    memset(&cvp, 0xFF, sizeof cvp);
    memset(&csc, 0xFF, sizeof csc);
    gdon = lerp_ok && (ip_gd_im || ip_gd_sh); /* W265: Waechter nur in Zwischenbildern (t < 1) */
    if (gdon && g_cfg.interp_guard_diag && !dbg) { /* DIAG(W265): Pipeline-Statistik dieser Wiedergabe (auch Deferred) */
        unsigned q = ip_gd_qi % IP_GD_Q;
        if (!x->deferred) { ip_gd_poll(); }
        if (ip_gd_q[q] && !ip_gd_qbusy[q]) {
            gdq = (int)q;
            ID3D11DeviceContext_Begin(x->c, (ID3D11Asynchronous *)ip_gd_q[gdq]);
        } else { st_gd_lost++; }
        ip_gd_qi++;
    }
    if (!x->deferred) {
        ip_state_save(&sv);
        if (!dbg) { ip_gs_begin(t); } /* DIAG(TICK-PERF,W125) */
    } else {
        /* PERF(RENDER-THREAD,W126): Deferred-Context beginnt leer. Einziger
         * geerbter Zustand der Wiedergabe ist s0 (tex_addr=0, Draw ohne
         * eigenen Sampler) -> s0 des Spiels beim Aufnahme-Start von B. */
        ID3D11DeviceContext_PSSetSamplers(x->c, 0, 1, &B->smp0);
    }
    if (lerp_ok && !dbg && !vrp && !ip_pl_on && !ip_ph.on && g_cfg.interp_mode == 1 && ip_rp_ser[bi] == B->serial &&
             ip_rp_we[bi] >= 0 && sb_rp_ready(bi, B->serial)) {
        rpw = g_ish[ip_rp_we[bi]].rtv;
        rpd = ip_rp_wd[bi] >= 0 ? g_ish[ip_rp_wd[bi]].dsv : NULL;
        rplast = ip_rp_last[bi];
    }
    if (!x->deferred && !vrp && !dbg && !ip_rp_prep) { cc_prep(B, bi, lerp_ok); } /* W146 GPU-COST: Scan + Startkopie-Maske */
    if (vrp && vrp->wtick) { /* W183 VR-FIX3: Aussenwelt im Spieltakt (Tick-Endstand), nur BB-Ops je Auge */
        static int vt_have;
        if (!vt_have || vt_ser != B->serial) { vt_scan(B, bi); vt_have = 1; }
        vtk = vt_ok;
        vrp->wtick_used = vtk;
        vrp->nskip = vtk ? vt_nskip : 0u;
        if (vtk) { ip_sh_noskip = vt_nosc; }
    }
    ip_sh_start(B, bi, A, ai, x);
    ip_sh_noskip = NULL;
    if (ip_rp_prep == 2 && ip_rp_idrtv) { static const float k0[4] = { 0, 0, 0, 0 }; ID3D11DeviceContext_ClearRenderTargetView(x->c, ip_rp_idrtv, k0); } /* W295: ID 0 */
    if (ip_ph.on && ip_ph.init && ip_ph.tgt >= 0) { ip_ph.init(ip_ph.ctx, g_ish[ip_ph.tgt].sh); } /* W195: Kachel vorbelegen */
    if (vtk) {
        int k;
        for (k = 0; k < IP_SH_MAX; k++) {
            if (vt_wr[k]) { ID3D11DeviceContext_CopyResource(x->c, (ID3D11Resource *)g_ish[k].sh, (ID3D11Resource *)g_ish[k].bak[bi]); }
        }
    }
    if (!x->deferred && !vrp && !dbg && !ip_rp_prep) { ccm = cc_begin(B, bi, A, lerp_ok, x); } /* W146 GPU-COST */
    if (ip_gs_n >= 0) { ip_gs_mark((ID3D11RenderTargetView *)2); } /* DIAG(TICK-PERF,W125): Kopien fertig */
    if (ring) { /* PERF(PERF-M2,W174) ip_cb_ring: alle Draw-Konstanten in einem Map */
        ring = ip_ring_fill(x, B, A, lerp_ok, t, &rc, la_on, la_M);
        rofs = ip_ring[x->deferred ? 1 : 0].ofs;
    }
    rc.ring = (unsigned)ring;
    ID3D11DeviceContext_PSSetShader(x->c, g_im_ps, NULL, 0);
    if (!ring) { ID3D11DeviceContext_PSSetConstantBuffers(x->c, 0, 1, &x->cb0); }
    for (i = 0; i < B->nops; i++) {
        ip_op_t *o = &B->ops[i];
        int lerp;
        int phred = 0; /* W195: Viewport/Scissor dieser Op schon umgelenkt gesetzt */
        if ((ccm & 16) && cc_skip[i]) { continue; } /* W146 GPU-COST: ruhiges Ziel (Tick-Endstand kopiert) */
        if (ip_rp_prep && i > ip_rp_stop) { break; } /* W295: Vorbereitung nur bis zur letzten Welt-Op vor der Komposition */
        if (rpw && i <= rplast) { /* W295 WORLD-REPROJ: Welt-Ops bis zur Komposition durch die Reprojektion ersetzen */
            if (IP_RP_ISW(o, rpw, rpd)) {
                if (i == rplast) {
                    sb_rp_warp(x->c, bi, rpw, g_ish[ip_rp_we[bi]].sh, t);
                    crtv = (ID3D11RenderTargetView *)1; cdsv = (ID3D11DepthStencilView *)1; cbs = (ID3D11BlendState *)1;
                    cds = (ID3D11DepthStencilState *)1; csref = 0xFFFFFFFFu; crs = (ID3D11RasterizerState *)1;
                    csrv = (ID3D11ShaderResourceView *)1; csmp = (ID3D11SamplerState *)1; ctopo = 0xFFFFFFFFu;
                    memset(&cvp, 0xFF, sizeof cvp); memset(&csc, 0xFF, sizeof csc);
                    mode = -1; rq_dirty = 1; fxs.dirty = 1; cgd = NULL;
                    rcur[0] = rcur[1] = rcur[2] = 0xFFFFFFFFu;
                    if (!ring) { ID3D11DeviceContext_PSSetConstantBuffers(x->c, 0, 1, &x->cb0); }
                }
                continue;
            }
        }
        if (ip_pl_on && o->nolerp && (ip_pl_mfx & (o->nolerp == MFX_WASH ? 1u : 2u))) { ip_pl_nmfx++; continue; } /* W193: monitor_fx aus */
        if (ip_ph.on && ip_ph.hud_off && ip_ph_is_hud(o)) { ip_ph_nhud++; continue; } /* W290: HUD aus (nur Foto) */
        if (ip_ph.on && ip_ph.cine && o->kind == IP_DRAW) { /* W428 PHOTO-CINEMATIC (nur Foto-Wiedergabe) */
            if (ip_ph_is_self(B, i)) { ip_ph_nself++; continue; } /* eigener VT im Welt-RT (W445: mit Luecke) */
            if (o->nolerp) { ip_ph_ncmfx++; continue; } /* Monitor-Effekt (Korn-Wash/Stoerzeilen) */
            if (ip_ph_cine_ovl(o, B->bbref >= 0 && o->sh_rtv == g_ish[B->bbref].rtv)) { ip_ph_ncovl++; continue; } /* Einblendungen (Radar, FRONT VIEW, Marken, Texte) */
        }
        if (vtk && vt_skip[i]) { continue; } /* W183 VR-FIX3: RT-Op, Ziel hat den Tick-Endstand */
        if ((g_cfg.loc_interp_dbg & 1) && g_sba_loc && lerp_ok && !dbg && o->kind == IP_DRAW && o->cls && !o->flag) { continue; } /* DIAG(W349) */
        if ((ccm & 3) && i < (unsigned)cc_pfx) { /* W146 GPU-COST: BB-Anfang aus dem Cockpit-Cache */
            if ((ccm & 3) == 2 && (o->rtv == g_rtv || (cc_bdsv && o->dsv == cc_bdsv))) { continue; }
        } else if ((ccm & 3) == 1 && i == (unsigned)cc_pfx) {
            cc_store(A, B, x);
            ccm &= ~3;
        }
        if (g_gp_open && !x->deferred) { /* DIAG(GPU-COST,W146) perf_gpass: Kategorie je Op */
            int gc;
            if (o->kind == IP_DRAW && (o->has_tex & FXR_MASK)) { gc = GP_FX; }
            else if (o->kind == IP_DRAW && o->nolerp) { gc = GP_MFX; }
            else { gc = sb_gp_rt_class(o->sh_rtv, o->rtv == g_rtv, o->kind == IP_DRAW && o->rq); }
            sb_gp_mark(gc);
        }
        if (ip_gs_n > 0 && o->sh_rtv && o->sh_rtv != ip_gs_rtv[ip_gs_n - 1]) { ip_gs_mark(o->sh_rtv); } /* DIAG(TICK-PERF,W125) */
        if (ip_gs_n > 0 && o->kind == IP_DRAW) { ip_gs_draws[ip_gs_n - 1]++; if (o->rq) { ip_gs_rq[ip_gs_n - 1]++; } }
        if (o->kind == IP_COPY) { /* W340 LOC-FRAMES: BB-Schatten (Stand dieser Wiedergabe) -> Schatten der Textur */
            if (o->cp_es >= 0 && o->cp_ed >= 0 && g_ish[o->cp_es].sh && g_ish[o->cp_ed].sh) {
                ID3D11Resource *dr = (ID3D11Resource *)g_ish[o->cp_ed].sh, *sr = (ID3D11Resource *)g_ish[o->cp_es].sh;
                if (vrp) { /* W134 VR: BB-Draws gingen ins Augenziel -> Textur bleibt Tick-Stand */ }
                else {
                    if (o->cpbox.back) { ID3D11DeviceContext_CopySubresourceRegion(x->c, dr, 0, 0, 0, 0, sr, 0, &o->cpbox); }
                    else { ID3D11DeviceContext_CopyResource(x->c, dr, sr); }
                    InterlockedIncrement(&li_cp_rep);
                }
            }
            continue;
        }
        if (o->kind == IP_CLR_RTV) {
            if (ip_ph.on && ip_ph.tgt >= 0 && o->sh_rtv && o->sh_rtv == g_ish[ip_ph.tgt].rtv) { /* W195: Foto-Kachel */
                D3D11_RECT cr;
                cr.left = ip_ph.cl[0]; cr.top = ip_ph.cl[1]; cr.right = ip_ph.cl[2]; cr.bottom = ip_ph.cl[3];
                if (x->c1 && cr.right > cr.left && cr.bottom > cr.top) {
                    ID3D11DeviceContext1_ClearView(x->c1, (ID3D11View *)ip_ph.rtv, o->col, &cr, 1);
                } else {
                    ID3D11DeviceContext_ClearRenderTargetView(x->c, ip_ph.rtv, o->col);
                }
                ip_ph_nclr++;
                continue;
            }
            if (ip_ph.on && ip_ph.nsc && ip_ph_sc_rtv(o->sh_rtv) >= 0) { /* W195: skaliertes Zwischenziel */
                ID3D11DeviceContext_ClearRenderTargetView(x->c, ip_ph.sc_rtv[ip_ph_sc_rtv(o->sh_rtv)], o->col);
                ip_ph_nclr++;
                continue;
            }
            if (vrp && o->rtv == g_rtv) { /* W134 VR-PROTO: BB-Clear -> Augenziel */
                ID3D11DeviceContext_ClearRenderTargetView(x->c, vrp->rtv, o->col);
                vrp->nclr++;
                continue;
            }
            if (o->sh_rtv) {
                static const float kz[4] = { 0, 0, 0, 0 }; /* DIAG(W251) dbg=2: ID 0 = geloescht */
                WD_CALL("ip_clear_rtv", NULL, ID3D11DeviceContext_ClearRenderTargetView(x->c, o->sh_rtv, dbg == 2 ? kz : o->col));
            }
            rc.nclr++;
            continue;
        }
        if (o->kind == IP_CLR_DSV) {
            if (ip_ph.on && ip_ph.tgtd >= 0 && ip_ph.dsv && o->sh_dsv && o->sh_dsv == g_ish[ip_ph.tgtd].dsv) { /* W195 */
                ID3D11DeviceContext_ClearDepthStencilView(x->c, ip_ph.dsv, o->dsflags, o->z, (UINT8)o->stencil);
                ip_ph_nclr++;
                continue;
            }
            if (ip_ph.on && ip_ph.nsc && ip_ph_sc_dsv(o->sh_dsv) >= 0) { /* W195 */
                int j = ip_ph_sc_dsv(o->sh_dsv);
                if (ip_ph.sc_dsv[j]) {
                    ID3D11DeviceContext_ClearDepthStencilView(x->c, ip_ph.sc_dsv[j], o->dsflags, o->z, (UINT8)o->stencil);
                }
                ip_ph_nclr++;
                continue;
            }
            if (vrp && o->dsv == g_dsv) { /* W134 VR-PROTO: BB-Tiefe -> Augentiefe */
                if (vrp->dsv) {
                    ID3D11DeviceContext_ClearDepthStencilView(x->c, vrp->dsv, o->dsflags, o->z, (UINT8)o->stencil);
                }
                continue;
            }
            if (o->sh_dsv) {
                WD_CALL("ip_clear_dsv", NULL, ID3D11DeviceContext_ClearDepthStencilView(x->c, o->sh_dsv, o->dsflags, o->z,
                                                          (UINT8)o->stencil));
            }
            continue;
        }
        {
            ID3D11RenderTargetView *wr = o->sh_rtv; /* W195: Foto-Umlenkung (sonst die Op-Werte, unveraendert) */
            ID3D11DepthStencilView *wd = o->sh_dsv;
            if (ip_ph.on) {
                int red = 0;
                if (ip_ph.tgt >= 0 && wr && wr == g_ish[ip_ph.tgt].rtv) { wr = ip_ph.rtv; red = 1; }
                if (ip_ph.tgtd >= 0 && ip_ph.dsv && wd && wd == g_ish[ip_ph.tgtd].dsv && (red || !o->sh_rtv)) { wd = ip_ph.dsv; red = 1; }
                else if (red) { wd = NULL; } /* Umlenkung ohne passende Tiefe: keine (Groessen muessen passen) */
                if (red) {
                    D3D11_VIEWPORT v = o->vp;
                    D3D11_RECT s;
                    LONG l, t, r, bt;
                    v.TopLeftX = o->vp.TopLeftX * ip_ph.kx + ip_ph.ox;
                    v.TopLeftY = o->vp.TopLeftY * ip_ph.ky + ip_ph.oy;
                    v.Width = o->vp.Width * ip_ph.kx;
                    v.Height = o->vp.Height * ip_ph.ky;
                    l = (LONG)floorf((float)o->sc.left * ip_ph.kx + ip_ph.ox);
                    t = (LONG)floorf((float)o->sc.top * ip_ph.ky + ip_ph.oy);
                    r = (LONG)ceilf((float)o->sc.right * ip_ph.kx + ip_ph.ox);
                    bt = (LONG)ceilf((float)o->sc.bottom * ip_ph.ky + ip_ph.oy);
                    s.left = l < ip_ph.cl[0] ? ip_ph.cl[0] : (l > ip_ph.cl[2] ? ip_ph.cl[2] : l);
                    s.top = t < ip_ph.cl[1] ? ip_ph.cl[1] : (t > ip_ph.cl[3] ? ip_ph.cl[3] : t);
                    s.right = r < s.left ? s.left : (r > ip_ph.cl[2] ? ip_ph.cl[2] : r);
                    s.bottom = bt < s.top ? s.top : (bt > ip_ph.cl[3] ? ip_ph.cl[3] : bt);
                    ID3D11DeviceContext_RSSetViewports(x->c, 1, &v);
                    ID3D11DeviceContext_RSSetScissorRects(x->c, 1, &s);
                    memset(&cvp, 0xFF, sizeof cvp); /* naechster nicht umgelenkter Draw setzt neu */
                    memset(&csc, 0xFF, sizeof csc);
                    ip_ph_nred++;
                    phred = 1;
                } else if (ip_ph.nsc && (ip_ph_sc_rtv(wr) >= 0 || (!wr && ip_ph_sc_dsv(wd) >= 0))) {
                    int j = wr ? ip_ph_sc_rtv(wr) : ip_ph_sc_dsv(wd), jd = ip_ph_sc_dsv(wd);
                    float k = ip_ph.sc_k[j];
                    D3D11_VIEWPORT v = o->vp;
                    D3D11_RECT sr;
                    if (wr) { wr = ip_ph.sc_rtv[j]; }
                    wd = (jd >= 0 && (jd == j || !o->sh_rtv)) ? ip_ph.sc_dsv[jd] : NULL;
                    v.TopLeftX *= k; v.TopLeftY *= k; v.Width *= k; v.Height *= k;
                    sr.left = (LONG)floorf((float)o->sc.left * k); sr.top = (LONG)floorf((float)o->sc.top * k);
                    sr.right = (LONG)ceilf((float)o->sc.right * k); sr.bottom = (LONG)ceilf((float)o->sc.bottom * k);
                    if (sr.left < 0) { sr.left = 0; }
                    if (sr.top < 0) { sr.top = 0; }
                    if (sr.right > ip_ph.sc_w[j]) { sr.right = ip_ph.sc_w[j]; }
                    if (sr.bottom > ip_ph.sc_h[j]) { sr.bottom = ip_ph.sc_h[j]; }
                    if (sr.right < sr.left) { sr.right = sr.left; }
                    if (sr.bottom < sr.top) { sr.bottom = sr.top; }
                    ID3D11DeviceContext_RSSetViewports(x->c, 1, &v);
                    ID3D11DeviceContext_RSSetScissorRects(x->c, 1, &sr);
                    memset(&cvp, 0xFF, sizeof cvp);
                    memset(&csc, 0xFF, sizeof csc);
                    ip_ph_nred++;
                    phred = 1;
                }
            }
            if (wr != crtv || wd != cdsv) {
                ID3D11DeviceContext_OMSetRenderTargets(x->c, 1, &wr, wd);
                crtv = wr;
                cdsv = wd;
                rc.nrtv++;
            }
        }
        if (o->bs != cbs) {
            /* Blendfaktor/Maske setzt der IM-Pfad konstant (NULL/0xFFFFFFFF). */
            ID3D11DeviceContext_OMSetBlendState(x->c, o->bs, o->bf, o->smask);
            cbs = o->bs;
        }
        if (o->ds != cds || o->sref != csref) {
            ID3D11DeviceContext_OMSetDepthStencilState(x->c, o->ds, o->sref);
            cds = o->ds;
            csref = o->sref;
        }
        if (o->rs != crs) {
            ID3D11DeviceContext_RSSetState(x->c, o->rs);
            crs = o->rs;
        }
        if (!phred && memcmp(&o->vp, &cvp, sizeof cvp) != 0) {
            if (var == 1u) { /* DIAG(PERF-M2,W174) ip_ablate 1: Viewport 1x1 (keine Pixelarbeit) */
                D3D11_VIEWPORT v1 = o->vp;
                v1.Width = 1.0f; v1.Height = 1.0f;
                ID3D11DeviceContext_RSSetViewports(x->c, 1, &v1);
            } else {
                ID3D11DeviceContext_RSSetViewports(x->c, 1, &o->vp);
            }
            cvp = o->vp;
        }
        if (!phred && memcmp(&o->sc, &csc, sizeof csc) != 0) {
            ID3D11DeviceContext_RSSetScissorRects(x->c, 1, &o->sc);
            csc = o->sc;
        }
        if (IP_PL_SRV(o->sh_srv) != csrv) { /* W193: Textur-Tausch nur in der Pause-Neuzeichnung */
            ID3D11ShaderResourceView *ss = IP_PL_SRV(o->sh_srv);
            ID3D11DeviceContext_PSSetShaderResources(x->c, 0, 1, &ss);
            csrv = ss;
            rc.nsrv++;
        }
        if (o->topo != ctopo) {
            ID3D11DeviceContext_IASetPrimitiveTopology(x->c, (D3D11_PRIMITIVE_TOPOLOGY)o->topo);
            ctopo = o->topo;
        }
        if (ring) { ip_ring_bind(x, rofs[2u * i], 0, rcur, &rc); } /* PERF(PERF-M2,W174) */
        else if (ip_ph.on && (ip_ph.cam || (ip_ph.wcx > 0.0f && ip_ph.wcx != 1.0f)) && ip_ph_is_world(o)) {
            /* W195 PHOTO-MODE: freie Kamera bzw. Weitwinkel (Welt-RT) */
            float M[16], pm[16];
            if (ip_ph.cam && ip_ph_cam_matrix(o, M)) { ip_m4mul(pm, o->mvp, M); ip_ph_ncam++; }
            else { memcpy(pm, o->mvp, sizeof pm); }
            if (ip_ph.wcx > 0.0f && ip_ph.wcx != 1.0f) { /* Clip x * wcx (Zeilenvektor: Spalte 0 skalieren) */
                pm[0] *= ip_ph.wcx; pm[4] *= ip_ph.wcx; pm[8] *= ip_ph.wcx; pm[12] *= ip_ph.wcx;
            }
            ip_cb_write(x, x->cb0, pm, o->has_tex, 0.0f, 0);
            rc.nmap++;
        }
        else if (ip_pl_on && ip_pl_mvp && o->rtv == g_rtv) {
            ip_cb_write(x, x->cb0, ip_pl_mvp[i], o->has_tex, 0.0f, 0);
            rc.nmap++;
        }
        else if (var != 4u) { ip_cb_write(x, x->cb0, IP_LAM(o, o->mvp), o->has_tex, 0.0f, 0); rc.nmap++; } /* W376 */
        if (dbg == 2 && ip_id_ps && ip_dbg_cb) {
            /* DIAG(W251 INTERP-GROUND): Draw-ID-Bild - R/G = Op-Index+1, B = 255 gelerpt / 64 exakt;
             * Mischen aus (ausser reinen Tiefen-/Stencil-Draws ohne Farbschreiben) */
            D3D11_MAPPED_SUBRESOURCE dm;
            D3D11_BLEND_DESC bdd;
            int il = lerp_ok && o->flag && o->match >= 0 && A && (unsigned)o->match < A->nops;
            unsigned id = i + 1u;
            memset(&bdd, 0, sizeof bdd);
            bdd.RenderTarget[0].RenderTargetWriteMask = 15;
            if (o->bs) { ID3D11BlendState_GetDesc(o->bs, &bdd); }
            if (bdd.RenderTarget[0].RenderTargetWriteMask) {
                ID3D11DeviceContext_OMSetBlendState(x->c, NULL, NULL, 0xFFFFFFFFu);
                cbs = (ID3D11BlendState *)1;
            }
            if (cps != 2) {
                ID3D11DeviceContext_PSSetShader(x->c, ip_id_ps, NULL, 0);
                ID3D11DeviceContext_PSSetConstantBuffers(x->c, 1, 1, &ip_dbg_cb);
                cps = 2;
            }
            if (SUCCEEDED(ID3D11DeviceContext_Map(x->c, (ID3D11Resource *)ip_dbg_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &dm))) {
                float *f = (float *)dm.pData;
                f[0] = (float)(id & 255u) / 255.0f; f[1] = (float)((id >> 8) & 255u) / 255.0f;
                f[2] = il ? 1.0f : 64.0f / 255.0f; f[3] = 1;
                ID3D11DeviceContext_Unmap(x->c, (ID3D11Resource *)ip_dbg_cb, 0);
            }
        } else if (dbg && ip_dbg_ps && ip_dbg_cb) {
            /* gefaerbt nur Draws, die NICHT interpoliert/unveraendert sind */
            int want = (o->why != IP_W_BB2D && o->why != IP_W_LERP && o->why != IP_W_STATIC &&
                        o->why != IP_W_RT2DLERP) ? 1 : 0;
            if (want != cps) {
                ID3D11DeviceContext_PSSetShader(x->c, want ? ip_dbg_ps : g_im_ps, NULL, 0);
                if (want) { ID3D11DeviceContext_PSSetConstantBuffers(x->c, 1, 1, &ip_dbg_cb); }
                cps = want;
            }
            if (want && o->why != cwhy) {
                D3D11_MAPPED_SUBRESOURCE dm;
                if (SUCCEEDED(ID3D11DeviceContext_Map(x->c, (ID3D11Resource *)ip_dbg_cb, 0,
                                                      D3D11_MAP_WRITE_DISCARD, 0, &dm))) {
                    float *f = (float *)dm.pData;
                    unsigned w = o->why < IP_W_N ? o->why : 0u;
                    f[0] = kIP_DBG_COL[w][0]; f[1] = kIP_DBG_COL[w][1]; f[2] = kIP_DBG_COL[w][2]; f[3] = 1;
                    ID3D11DeviceContext_Unmap(x->c, (ID3D11Resource *)ip_dbg_cb, 0);
                }
                cwhy = o->why;
            }
        }
        lerp = lerp_ok && o->flag && o->match >= 0 && (unsigned)o->match < A->nops;
        rc.ndraw++; /* DIAG(PERF-M2,W174) */
        if (lerp) { rc.nlerp++; }
        rc.vbytes += (unsigned long long)o->nv * sizeof(im_vertex_t) * (lerp ? 2u : 1u);
        if (o->rq) { rc.nrq++; }
        vrbb = vrp && o->rtv == g_rtv; /* W134 VR-PROTO: Draw in den Backbuffer -> je Auge */
        vrkind = !vrbb ? -1 : (!o->cls ? 0 : (ip_vr_isp2(o->mvp) ? 2 : 1));
        if (vrbb) {
            if (vrkind == 0) { vrp->n2d++; } else if (vrkind == 1) { vrp->n3d++; } else { vrp->np2++; }
            if (vrkind == 2) {
                int q;
                for (q = 0; q < 4; q++) {
                    if (o->rn[q][2] > 0.0f) {
                        float zn = o->pref[q][2] / o->pref[q][3];
                        vrp->p2w_sum += o->rn[q][2];
                        vrp->p2w_n++;
                        vrp->p2z_sum += zn;
                        if (vrp->p2w_n == 1u || zn < vrp->p2z_min) { vrp->p2z_min = zn; }
                        if (vrp->p2w_n == 1u || zn > vrp->p2z_max) { vrp->p2z_max = zn; }
                        if (vrp->wantz && vrp->zs_n < 1024u) { vrp->zs[vrp->zs_n++] = zn; }
                    }
                }
            }
            if (vrkind == 1) { int q; for (q = 0; q < 4; q++) { if (o->rn[q][2] > 0.0f && (vrp->w3_min <= 0.0f || o->rn[q][2] < vrp->w3_min)) { vrp->w3_min = o->rn[q][2]; }
                                                       if (o->rn[q][2] > 0.0f && vrp->wantz && vrp->ws_n < 512u) { vrp->ws[vrp->ws_n++] = o->rn[q][2]; } } } /* W164 */
            if (o->rq) { vrp->nrq++; if (vrkind != 1 || !x->c1 || vrp->noshade) { vrp->nrqfb++; } }
            if (vrp->dump > 0) { /* DIAG(VR-PROTO,W134): Eingangsdaten der BB-Draws */
                vrp->dump--;
                HLOG_I("VR op i=%u kind=%d n=%u rq=%d tex=0x%X lerp=%d vpz=[%.3f,%.3f] ds=%p mvp=[%.3f %.3f %.3f %.3f|%.3f %.3f %.3f %.3f|%.3f %.3f %.3f %.3f|%.3f %.3f %.3f %.3f] "
                       "p0=(%.3f,%.3f,%.4f,%.3f) p1=(%.3f,%.3f,%.4f,%.3f) p3=(%.3f,%.3f,%.4f,%.3f)",
                       i, vrkind, o->nv, o->rq, o->has_tex, lerp, o->vp.MinDepth, o->vp.MaxDepth, (void *)o->ds,
                       o->mvp[0], o->mvp[1], o->mvp[2], o->mvp[3], o->mvp[4], o->mvp[5], o->mvp[6], o->mvp[7],
                       o->mvp[8], o->mvp[9], o->mvp[10], o->mvp[11], o->mvp[12], o->mvp[13], o->mvp[14], o->mvp[15],
                       o->pref[0][0], o->pref[0][1], o->pref[0][2], o->pref[0][3], o->pref[1][0], o->pref[1][1], o->pref[1][2], o->pref[1][3],
                       o->pref[3][0], o->pref[3][1], o->pref[3][2], o->pref[3][3]);
                { /* DIAG(VR-FIX2,W164): Tiefenzustand des Draws (Relief-Richtung, Decals) */
                    D3D11_DEPTH_STENCIL_DESC dd;
                    memset(&dd, 0, sizeof dd);
                    if (o->ds) { ID3D11DepthStencilState_GetDesc(o->ds, &dd); }
                    HLOG_I("VR op2 i=%u kind=%d tiefe=%d schreiben=%d func=%d dsv=%d zn=[%.5f %.5f %.5f %.5f] ndc0=(%.3f,%.3f) ndc3=(%.3f,%.3f) w=[%.3f %.3f %.3f %.3f]",
                           i, vrkind, (int)dd.DepthEnable, (int)dd.DepthWriteMask, (int)dd.DepthFunc, o->dsv ? 1 : 0,
                           o->pref[0][3] != 0.0f ? o->pref[0][2] / o->pref[0][3] : 0.0f, o->pref[1][3] != 0.0f ? o->pref[1][2] / o->pref[1][3] : 0.0f,
                           o->pref[2][3] != 0.0f ? o->pref[2][2] / o->pref[2][3] : 0.0f, o->pref[3][3] != 0.0f ? o->pref[3][2] / o->pref[3][3] : 0.0f,
                           o->rn[0][0], o->rn[0][1], o->rn[3][0], o->rn[3][1], o->rn[0][2], o->rn[1][2], o->rn[2][2], o->rn[3][2]);
                }
            }
        }
        if (o->rq && ip_pl_on && ip_pl_noshade) { ip_pl_nnoshade++; } /* W193: shading aus -> IM-PS-Ersatz */
        if (o->rq && x->c1 && !(ip_pl_on && ip_pl_noshade) && !(vrbb && (vrkind != 1 || vrp->noshade))) { /* W183: vr_shading=0 -> IM-PS-Ersatz */
            /* RENDER-QA (W77): Shading-Draw mit seinem Zustand (Stream 0 per
             * Versatz, Stream 2, Konstanten-Fenster, Stufen 0..3, Sampler). */
            const rq_rec_t *r = &o->rqr;
            ID3D11ShaderResourceView *sv[4];
            ID3D11Buffer *rcb = (ip_pl_on && ip_pl_cb && r->cbuf == ip_pl_cbsrc) ? ip_pl_cb : r->cbuf; /* W193 */
            UINT st2 = stride, o0 = o->vstart * stride;
            sv[0] = IP_PL_SRV(o->sh_srv); sv[1] = IP_PL_SRV(o->rq_shsrv[1]); sv[2] = IP_PL_SRV(o->rq_shsrv[2]); sv[3] = IP_PL_SRV(o->rq_shsrv[3]);
            ID3D11DeviceContext_PSSetShader(x->c, (dbg == 2 && ip_id_ps) ? ip_id_ps : r->ps, NULL, 0); /* DIAG(W251) */
            ID3D11DeviceContext1_VSSetConstantBuffers1(x->c1, 3, 1, &rcb, &r->vfirst, &r->vnum);
            ID3D11DeviceContext1_PSSetConstantBuffers1(x->c1, 4, 1, &rcb, &r->pfirst, &r->pnum);
            ID3D11DeviceContext_PSSetShaderResources(x->c, 0, 4, sv);
            ID3D11DeviceContext_PSSetSamplers(x->c, 0, 4, (ID3D11SamplerState *const *)r->smp);
            { /* PERF(PERF-M2,W174) ip_vb_gpu Bit 2: Stream 2/Roh-Eingaenge aus der GPU-Kopie (gleicher Versatz) */
                ID3D11ShaderResourceView *xs = (ID3D11ShaderResourceView *)r->xsrv;
                ID3D11Buffer *xb = (ID3D11Buffer *)r->xbuf;
                if (vbgB && (B->gpu_ok & 2u)) {
                    if (xs && xs == B->rqxsrv && B->rqxgsrv) { xs = B->rqxgsrv; }
                    if (xb && xb == B->rqx && B->rqxg) { xb = B->rqxg; }
                }
                if (xs) { ID3D11DeviceContext_VSSetShaderResources(x->c, 0, 1, &xs); } /* SHADE-PERF (W80) */
                else { ID3D11DeviceContext_IASetVertexBuffers(x->c, 2, 1, &xb, &r->xstride, &r->xoff); }
            }
            if (lerp) {
                const ip_op_t *a = &A->ops[o->match];
                ID3D11Buffer *cbs2[2];
                UINT o1 = (o->rzb ? o->vstart : a->vstart) * stride; /* W284 rzb: Strom N-1 = Tick-N-Vertices */
                if (ring) { ip_ring_bind(x, rofs[2u * i + 1u], 1, rcur, &rc); }
                else if (var != 4u) { ip_cb_write(x, x->cb1, IP_LAM(o, IP_AMVP(o, a)), 0, t, 1); rc.nmap++; } /* W376 */
                ID3D11DeviceContext_IASetInputLayout(x->c, r->layl);
                ID3D11DeviceContext_VSSetShader(x->c, r->vsl, NULL, 0);
                cbs2[0] = x->cb0; cbs2[1] = x->cb1;
                if (!ring) { ID3D11DeviceContext_VSSetConstantBuffers(x->c, 0, 2, cbs2); }
                ID3D11DeviceContext_IASetVertexBuffers(x->c, 0, 1, &bvb, &st2, &o0);
                ID3D11DeviceContext_IASetVertexBuffers(x->c, 1, 1, o->rzb ? &bvb : &avb, &st2, &o1);
                ip_hw_bind(x->c, B, o->rzb ? B : A, o->vstart, o->rzb ? o->vstart : a->vstart); /* FIX(W237) interp_hlerp */
                if (gdon) { ip_gd_bind(x->c, &cgd, ip_gd_need(o, a) ? ip_gd_sh : NULL); } /* FIX(W265) */
            } else {
                if (cgd) { ip_gd_bind(x->c, &cgd, NULL); } /* W265 */
                ID3D11DeviceContext_IASetInputLayout(x->c, r->lay);
                ID3D11DeviceContext_VSSetShader(x->c, r->vs, NULL, 0);
                if (!ring) { ID3D11DeviceContext_VSSetConstantBuffers(x->c, 0, 1, &x->cb0); }
                ID3D11DeviceContext_IASetVertexBuffers(x->c, 0, 1, &bvb, &st2, &o0);
            }
            if (vrbb) { /* W134 VR-PROTO: Shading-Draw 3D-Pfad je Auge (lineare Augen-Matrix in b0/b1) */
                int e;
                for (e = 0; e < vrp->neyes && !(g_cfg.vr_diag_skip & 2); e++) { /* DIAG(W164): vr_diag_skip Bit 1 = 3D */
                    float m[16];
                    ip_vr_eye_target(x, vrp, e, o);
                    ip_vr_mat3d(m, o->mvp, vrp, e);
                    ip_cb_write(x, x->cb0, m, o->has_tex, 0.0f, 0);
                    if (lerp) { ip_vr_mat3d(m, A->ops[o->match].mvp, vrp, e); ip_cb_write(x, x->cb1, m, 0, t, 1); }
                    ip_draw_op(x->c, o, o->nv, 0);
                }
                crtv = (ID3D11RenderTargetView *)1; cdsv = (ID3D11DepthStencilView *)1;
                memset(&cvp, 0xFF, sizeof cvp); memset(&csc, 0xFF, sizeof csc);
            } else {
                if (var != 8u) { WD_CALL("ip_draw_rq", x->c, ip_draw_op(x->c, o, var == 2u && o->nv > 3u ? 3u : o->nv, 0)); } /* DIAG(PERF-WARM,W139); W174 ip_ablate */
            }
            mode = -1;      /* naechster alter Draw bindet VS/Layout/Streams neu */
            rq_dirty = 1;   /* ... und PS/Sampler/Stufen */
            fxs.bound = 0; fxs.dirty = 1; /* W133: PS ersetzt, Tiefe evtl. geschrieben */
            csmp = (ID3D11SamplerState *)1;
            csrv = (ID3D11ShaderResourceView *)1;
            continue;
        }
        if (rq_dirty) {
            ID3D11ShaderResourceView *nul[3] = { NULL, NULL, NULL };
            ID3D11SamplerState *ds = rq_default_sampler();
            ID3D11DeviceContext_PSSetShader(x->c, (dbg && cps == 2) ? ip_id_ps : ((dbg && cps == 1) ? ip_dbg_ps : g_im_ps), NULL, 0);
            if (!ring) { ID3D11DeviceContext_PSSetConstantBuffers(x->c, 0, 1, &x->cb0); }
            ID3D11DeviceContext_PSSetShaderResources(x->c, 1, 3, nul);
            if (ds) { ID3D11DeviceContext_PSSetSamplers(x->c, 0, 1, &ds); }
            {
                ID3D11ShaderResourceView *ss = IP_PL_SRV(o->sh_srv); /* W193 */
                ID3D11DeviceContext_PSSetShaderResources(x->c, 0, 1, &ss);
                csrv = ss;
            }
            rq_dirty = 0;
            csmp = (ID3D11SamplerState *)1;
        }
        if (!dbg && ((o->has_tex & FXR_MASK) || fxs.bound)) { /* W133 REM-FX: Effekt-PS + Tiefen-Kopie des Schatten-Ziels */
            fxr_rep_op(x->c, &fxs, o->has_tex, o->fxp, o->sh_dsv, x->deferred);
        } else if (o->kind == IP_DRAW) { fxs.dirty = 1; }
        if (g_cfg.tex_addr || o->ta_smp || /* FIX(TEXADDR,W84): s0 wie beim Spiel-Draw; MONITOR-FX (W105): Wash-WRAP auch bei tex_addr=0 */
            (csmp != (ID3D11SamplerState *)1 && csmp != sb_ta_sampler(0, 0))) {
            ID3D11SamplerState *ws = o->ta_smp ? o->ta_smp : sb_ta_sampler(0, 0);
            if (ws && ws != csmp) { ID3D11DeviceContext_PSSetSamplers(x->c, 0, 1, &ws); csmp = ws; }
        }
        if (lerp) {
            const ip_op_t *a = &A->ops[o->match];
            ID3D11Buffer *vbs[2];
            UINT strides[2], offs[2];
            if (ring) { ip_ring_bind(x, rofs[2u * i + 1u], 1, rcur, &rc); } /* PERF(PERF-M2,W174) */
            else if (var != 4u) { ip_cb_write(x, x->cb1, IP_LAM(o, IP_AMVP(o, a)), 0, t, 1); rc.nmap++; } /* W376 */
            if (mode != 1) {
                ID3D11Buffer *cbs2[2];
                ID3D11DeviceContext_IASetInputLayout(x->c, ip_lay2);
                ID3D11DeviceContext_VSSetShader(x->c, ip_vs2, NULL, 0);
                cbs2[0] = x->cb0;
                cbs2[1] = x->cb1;
                if (!ring) { ID3D11DeviceContext_VSSetConstantBuffers(x->c, 0, 2, cbs2); }
                mode = 1;
            }
            vbs[0] = bvb; vbs[1] = o->rzb ? bvb : avb; /* W284 rzb */
            strides[0] = stride; strides[1] = stride;
            offs[0] = o->vstart * stride; offs[1] = (o->rzb ? o->vstart : a->vstart) * stride;
            ID3D11DeviceContext_IASetVertexBuffers(x->c, 0, 2, vbs, strides, offs);
            ip_hw_bind(x->c, B, o->rzb ? B : A, o->vstart, o->rzb ? o->vstart : a->vstart); /* FIX(W237) interp_hlerp */
            if (gdon) { ip_gd_bind(x->c, &cgd, ip_gd_need(o, a) ? ip_gd_im : NULL); } /* FIX(W265); VR: Lagen im Augenraum */
            if (vrbb) { /* W134 VR-PROTO */
                ip_vr_draw(x, vrp, o, a, vrkind, t, 1, &mode);
                crtv = (ID3D11RenderTargetView *)1; cdsv = (ID3D11DepthStencilView *)1;
                memset(&cvp, 0xFF, sizeof cvp); memset(&csc, 0xFF, sizeof csc);
            } else {
                if (var != 8u) { WD_CALL("ip_draw_lerp", x->c, ip_draw_op(x->c, o, var == 2u && o->nv > 3u ? 3u : o->nv, 0)); } /* W174 ip_ablate */
            }
        } else {
            if (cgd) { ip_gd_bind(x->c, &cgd, NULL); } /* W265 */
            if (mode != 0) {
                ID3D11Buffer *nb = NULL;
                ID3D11DeviceContext_IASetInputLayout(x->c, g_im_layout);
                ID3D11DeviceContext_VSSetShader(x->c, g_im_vs, NULL, 0);
                if (!ring) { ID3D11DeviceContext_VSSetConstantBuffers(x->c, 0, 1, &x->cb0); }
                ID3D11DeviceContext_IASetVertexBuffers(x->c, 0, 1, &bvb, &stride, &zero);
                ID3D11DeviceContext_IASetVertexBuffers(x->c, 1, 1, &nb, &stride, &zero);
                mode = 0;
            }
            if (vrbb) { /* W134 VR-PROTO */
                ip_vr_draw(x, vrp, o, NULL, vrkind, t, 0, &mode);
                crtv = (ID3D11RenderTargetView *)1; cdsv = (ID3D11DepthStencilView *)1;
                memset(&cvp, 0xFF, sizeof cvp); memset(&csc, 0xFF, sizeof csc);
            } else {
                if (var != 8u) { WD_CALL("ip_draw", x->c, ip_draw_op(x->c, o, var == 2u && o->nv > 3u ? 3u : o->nv, o->vstart)); } /* W174 ip_ablate */
            }
        }
    }
    if ((ccm & 3) == 1) { cc_store(A, B, x); } /* W146: BB-Anfang = alle BB-Ops */
    if (cgd) { ip_gd_bind(x->c, &cgd, NULL); } /* W265: Spiel-Draws nie mit GS */
    if (gdq >= 0) { ID3D11DeviceContext_End(x->c, (ID3D11Asynchronous *)ip_gd_q[gdq]); ip_gd_qbusy[gdq] = (int)ip_gd_qi; } /* DIAG(W265) */
    if (cps >= 0) {
        ID3D11Buffer *nb = NULL;
        ID3D11DeviceContext_PSSetConstantBuffers(x->c, 1, 1, &nb);
    }
    fxr_rep_end(x->c, &fxs); /* W133 REM-FX */
    ip_rc_ctx[x->deferred ? 1 : 0] = rc; /* DIAG(PERF-M2,W174) */
    if (!x->deferred) {
        ip_state_restore(&sv);
        ip_gs_end(B->nops); /* DIAG(TICK-PERF,W125) */
    }
}

/* ---------- DIAG (interp_diag>=3): Draw-Analyse eines Bildfolgen-Ticks ---------- */

static const char *kIP_WHY[IP_W_N] = { "-", "static", "lerp", "rawlerp", "nomatch",
                                       "rawchg", "reject", "bb2d", "rt2dlerp", "rt2dex" };

/* Pixel-Huellrechteck der Vertices [vs, vs+n) einer Liste (mvp, Viewport vp).
 * Rueckgabe: Zahl Vertices mit w <= 0. */
static unsigned ip_bbox(float (*P)[4], unsigned vs, unsigned n, const float m[16],
                        const D3D11_VIEWPORT *vp, float bb[4])
{
    unsigned i, behind = 0;
    bb[0] = bb[1] = 1e30f;
    bb[2] = bb[3] = -1e30f;
    for (i = 0; i < n; i++) {
        float c[4], x, y;
        ip_clip(P[vs + i], m, c);
        if (c[3] <= 1e-6f) { behind++; continue; }
        x = vp->TopLeftX + (c[0] / c[3] * 0.5f + 0.5f) * vp->Width;
        y = vp->TopLeftY + (0.5f - c[1] / c[3] * 0.5f) * vp->Height;
        if (x < bb[0]) { bb[0] = x; }
        if (y < bb[1]) { bb[1] = y; }
        if (x > bb[2]) { bb[2] = x; }
        if (y > bb[3]) { bb[3] = y; }
    }
    return behind;
}

static int ip_sh_of_rtv(ID3D11RenderTargetView *v)
{
    int e;
    if (!v) { return -1; }
    for (e = 0; e < IP_SH_MAX; e++) {
        if (g_ish[e].rtv == v) { return e; }
    }
    return -1;
}

static void ip_diag_tick(ip_list_t *A, int ai, ip_list_t *B, int bi)
{
    static unsigned ndone;
    unsigned i, cnt[IP_W_N];
    float (*PA)[4] = ip_cpos[ai], (*PB)[4] = ip_cpos[bi];
    if (!PA || !PB || (ndone >= 48u && !g_cfg.interp_ddfile[0])) { return; } /* W284: mit Tick-Liste ohne Grenze */
    ndone++;
    memset(cnt, 0, sizeof cnt);
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        float bb[4], ba[4];
        unsigned beh, behA = 0;
        char extra[200];
        if (o->kind != IP_DRAW) { continue; }
        cnt[o->why < IP_W_N ? o->why : 0]++;
        if (o->why == IP_W_BB2D) { continue; }
        beh = ip_bbox(PB, o->vstart, o->nv, o->mvp, &o->vp, bb);
        extra[0] = 0;
        ba[0] = ba[1] = ba[2] = ba[3] = 0;
        if (o->match >= 0 && (unsigned)o->match < A->nops) {
            const ip_op_t *a = &A->ops[o->match];
            behA = ip_bbox(PA, a->vstart, a->nv, a->mvp, &a->vp, ba);
            if (a->nv == o->nv && (o->why == IP_W_RAWCHG || o->why == IP_W_RAWLERP ||
                                   o->why == IP_W_RT2DLERP || o->why == IP_W_RT2DEX ||
                                   o->why == IP_W_REJECT || o->why == IP_W_LERP)) {
                /* Vertex i <-> i: Eingangs-Differenz (Mittel, max. Abweichung) und
                 * Bild-Sprung je Vertex (max / Anteil > 8 px). */
                double dm[3] = { 0, 0, 0 }, dev = 0, jmax = 0;
                unsigned k, nbig = 0, nj = 0;
                for (k = 0; k < o->nv; k++) {
                    int c;
                    for (c = 0; c < 3; c++) { dm[c] += PB[o->vstart + k][c] - PA[a->vstart + k][c]; }
                }
                for (k = 0; k < 3; k++) { dm[k] /= (double)o->nv; }
                for (k = 0; k < o->nv; k++) {
                    float c0[4], c1[4];
                    int c;
                    for (c = 0; c < 3; c++) {
                        double d = PB[o->vstart + k][c] - PA[a->vstart + k][c] - dm[c];
                        if (d < 0) { d = -d; }
                        if (d > dev) { dev = d; }
                    }
                    ip_clip(PA[a->vstart + k], a->mvp, c0);
                    ip_clip(PB[o->vstart + k], o->mvp, c1);
                    if (c0[3] > 1e-6f && c1[3] > 1e-6f) {
                        double dx = (c1[0] / c1[3] - c0[0] / c0[3]) * 0.5 * o->vp.Width;
                        double dy = (c1[1] / c1[3] - c0[1] / c0[3]) * 0.5 * o->vp.Height;
                        double j = sqrt(dx * dx + dy * dy);
                        if (j > jmax) { jmax = j; }
                        if (j > 8.0) { nbig++; }
                        nj++;
                    }
                }
                _snprintf_s(extra, sizeof extra, _TRUNCATE,
                            " dmean=%.3f,%.3f,%.3f dev=%.3f jmax=%.1f big=%u/%u",
                            dm[0], dm[1], dm[2], dev, jmax, nbig, nj);
            }
        } else if (o->why == IP_W_NOMATCH) {
            /* Kandidat gleicher Signatur ohne Vertexzahl im Vortick */
            unsigned j;
            for (j = 0; j < A->nops; j++) {
                const ip_op_t *a = &A->ops[j];
                if (a->kind == IP_DRAW && a->sig0 == o->sig0 && !a->taken) {
                    behA = ip_bbox(PA, a->vstart, a->nv, a->mvp, &a->vp, ba);
                    _snprintf_s(extra, sizeof extra, _TRUNCATE, " cand=%u n0=%u raw0=%s", j, a->nv,
                                a->rawhash == o->rawhash ? "gleich" : "neu");
                    break;
                }
            }
            if (!extra[0]) {
                /* gleiches Ziel + Vertexzahl + Eingangsdaten: welcher Signatur-Teil wechselt? */
                for (j = 0; j < A->nops; j++) {
                    const ip_op_t *a = &A->ops[j];
                    if (a->kind == IP_DRAW && a->rtv == o->rtv && a->nv == o->nv &&
                        a->rawhash == o->rawhash) {
                        behA = ip_bbox(PA, a->vstart, a->nv, a->mvp, &a->vp, ba);
                        _snprintf_s(extra, sizeof extra, _TRUNCATE,
                                    " same=%u tk=%u vs=%X/%X xp=%X/%X ps=%X/%X tex=%u/%u topo=%u/%u cls=%u/%u",
                                    j, a->taken, a->pvs, o->pvs, a->pxp, o->pxp, a->pps, o->pps,
                                    a->has_tex, o->has_tex, a->topo, o->topo, a->cls, o->cls);
                        break;
                    }
                }
            }
        }
        HLOG_I("INTERP dd f=%u i=%u w=%s e=%d vp=%.0fx%.0f cls=%u n=%u m=%d bb=%.0f,%.0f,%.0f,%.0f beh=%u bbA=%.0f,%.0f,%.0f,%.0f behA=%u mvpeq=%d%s",
               B->frame, i, kIP_WHY[o->why < IP_W_N ? o->why : 0], ip_sh_of_rtv(o->sh_rtv),
               o->vp.Width, o->vp.Height, o->cls, o->nv, o->match, bb[0], bb[1], bb[2], bb[3], beh,
               ba[0], ba[1], ba[2], ba[3], behA,
               (o->match >= 0 && (unsigned)o->match < A->nops &&
                memcmp(A->ops[o->match].mvp, o->mvp, sizeof o->mvp) == 0) ? 1 : 0, extra);
        if (ip_dd_on()) { /* DIAG(W251 INTERP-GROUND): Zustand + Huelle des Lerps bei t=0,25 */
            float tb[4] = { 1e30f, 1e30f, -1e30f, -1e30f };
            unsigned tbeh = 0;
            const ip_op_t *a = (o->match >= 0 && (unsigned)o->match < A->nops) ? &A->ops[o->match] : NULL;
            if (a && a->nv == o->nv && o->flag) {
                unsigned k;
                for (k = 0; k < o->nv; k++) {
                    float c0[4], c1[4], c[4], px, py;
                    int q;
                    ip_clip(PA[a->vstart + k], a->mvp, c0);
                    ip_clip(PB[o->vstart + k], o->mvp, c1);
                    for (q = 0; q < 4; q++) { c[q] = 0.75f * c0[q] + 0.25f * c1[q]; }
                    if (c[3] <= 1e-6f) { tbeh++; continue; }
                    px = o->vp.TopLeftX + (c[0] / c[3] * 0.5f + 0.5f) * o->vp.Width;
                    py = o->vp.TopLeftY + (0.5f - c[1] / c[3] * 0.5f) * o->vp.Height;
                    if (px < tb[0]) { tb[0] = px; }
                    if (py < tb[1]) { tb[1] = py; }
                    if (px > tb[2]) { tb[2] = px; }
                    if (py > tb[3]) { tb[3] = py; }
                }
            }
            HLOG_I("INTERP dd2 f=%u i=%u flag=%d rq=%d srv=%p asrv=%p pxp=%X/%X ht=%X nol=%d bs=%p ds=%p topo=%u vs=%u/%u tbb=%.0f,%.0f,%.0f,%.0f tbeh=%u pr=%.3f,%.3f,%.3f,%.3f",
                   B->frame, i, o->flag, o->rq, (void *)o->sh_srv, a ? (void *)a->sh_srv : NULL, a ? a->pxp : 0u, o->pxp,
                   o->has_tex, o->nolerp, (void *)o->bs, (void *)o->ds, o->topo, o->vstart, a ? a->vstart : 0u,
                   tb[0], tb[1], tb[2], tb[3], tbeh, o->prj[0], o->prj[1], o->prj[2], o->prj[3]);
        }
    }
    HLOG_I("INTERP ddsum f=%u static=%u lerp=%u rawlerp=%u nomatch=%u rawchg=%u reject=%u bb2d=%u rt2dlerp=%u rt2dex=%u",
           B->frame, cnt[1], cnt[2], cnt[3], cnt[4], cnt[5], cnt[6], cnt[7], cnt[8], cnt[9]);
    for (i = 0; i < IP_SH_MAX; i++) {
        if (g_ish[i].res) {
            D3D11_TEXTURE2D_DESC dd;
            ID3D11Texture2D_GetDesc(g_ish[i].sh, &dd);
            HLOG_I("INTERP ddsh e=%u %ux%u bind=0x%X%s", i, dd.Width, dd.Height, dd.BindFlags,
                   (int)i == B->bbref ? " (BB)" : "");
        }
    }
}

/* ---------- DIAG(W406 M3-ROAD-JITTER): Fugen zwischen Nachbar-Draws im Zwischenbild ----------
 * seam_diag=N (braucht interp_diag>=3, CPU-Kopie der Tick-Vertices): je Tick-Paar alle Welt-Vertices
 * (cls=1) gruppieren, die im Tick-Bild N in DERSELBEN Pixelposition liegen (1/32 px, je Renderziel) und
 * aus VERSCHIEDENEN Draws stammen - gemeinsame Ecken benachbarter Kacheln. Fuer jede Gruppe wird die
 * Lage im Zwischenbild t=0,25 so gerechnet, wie die Wiedergabe sie zeichnet (flag 0: Tick N; flag 1:
 * lerp(c0, c1, t) mit c0 = Vortick-Vertex bzw. Tick-N-Vertex mit Matrix N-1 bei rzb, W224-Regel bei
 * w-Wechsel). Streuung > 0 = Spalt/Versatz zwischen Nachbarn (Linie, Wackeln). Zusaetzlich die Streuung
 * der Vortick-Lagen bei rein gelerpten Gruppen (sprA: Partner-Vertices zeigen auf verschiedene Weltpunkte).
 * Zeilen SEAM (erste N Ticks mit Fund, mit pf_ring nur in den Bildfolgen), SEAMW (schlimmste Gruppen),
 * SEAMSUM (alle 200 Ticks, je Paar-Klasse). Nur Diagnose, Bild unveraendert. W410: KEIL/KEILSUM (alle 100 Ticks). */
typedef struct { unsigned long long key; unsigned op; float tx, ty, ax, ay, nx, ny, ew0, ew1; unsigned k; unsigned char why, flag, rzb, rt; } sd_ent_t; /* W429: ew0/ew1 echte w N-1/N, k Vertex */
static sd_ent_t *sd_e;
static unsigned sd_cap;
static unsigned sd_cls_n[IP_W_N * IP_W_N + 1], sd_cls_bad[IP_W_N * IP_W_N + 1];
static float sd_cls_max[IP_W_N * IP_W_N + 1];
static unsigned sd_ticks, sd_lines, sd_groups, sd_bad05, sd_bad2, sd_badA;
static unsigned sd_mx[2][7]; /* W429 DIAG: exakte Seite gemischter Spalte [3D|P2][Vertex-Regel, interp_pass, Ablage, nomatch, rawchg, reject, sonst] */
static unsigned sd_rj_draws[2], sd_rj_verts[2], sd_rj_bad[2], sd_rjl; /* [0] 3D-Matrix, [1] P2 (vorab transformiert) */
static float sd_rj_max[2], sd_rj_dy[2];
static double sd_rj_sum[2];
static double sd_maxs;
/* DIAG(W410 LOC-GFX-JITTER): falsche Vertex-Partner ("Keile"). Fuer jeden gelerpten Welt-Draw (flag) wird je
 * Vertex k die Vortick-Lage c0_k (wie in der Wiedergabe: Vortick-Vertex bzw. Tick-N-Vertex bei rzb, Matrix N-1)
 * mit der Kamerabewegung D = M_A^-1 * M_B des Ziels (rz_cam, beste von bis zu 8) nach Tick N gebracht und mit
 * c1_k verglichen (Pixel). Ruhende Welt mit richtigem Partner: Fehler ~0. Ein falscher Partner (anderer Weltpunkt
 * je Vertex) zieht im Zwischenbild Dreiecke von der alten zur neuen Lage = Keil. Gezaehlt: Draws mit einem Vertex
 * > 8 px UND > 4x Median des Draws (starr bewegte Objekte wie VTs haben gleichmaessigen Fehler). Nur Diagnose. */
static unsigned kd_ticks, kd_draws[IP_W_N], kd_bad[IP_W_N], kd_badrzb, kd_lines;
static float kd_max[IP_W_N];
static float *kd_e;
static unsigned kd_ecap;
static int kd_fcmp(const void *x, const void *y)
{
    float a = *(const float *)x, b = *(const float *)y;
    return a < b ? -1 : (a > b ? 1 : 0);
}
static void ip_wedge_diag(ip_list_t *A, float (*PA)[4], ip_list_t *B, float (*PB)[4])
{
    double D[8][16];
    unsigned i, k;
    rz_cam_pre(A, B);
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        const ip_op_t *a = (o->match >= 0 && (unsigned)o->match < A->nops) ? &A->ops[o->match] : NULL;
        int nD, x, bestx = -1;
        float best = 1e30f, bmed = 0.0f;
        unsigned bkmax = 0, bnbad = 0, bnv = 0, w = o->why < IP_W_N ? o->why : 0;
        if (o->kind != IP_DRAW || !o->cls || !o->flag || !a || (!o->rzb && a->nv != o->nv) || !o->nv) { continue; }
        nD = rz_cam(A, B, o, D);
        if (!nD) { continue; }
        if (kd_ecap < o->nv * 2u) {
            float *ne = (float *)realloc(kd_e, (size_t)o->nv * 2u * sizeof *kd_e);
            if (!ne) { return; }
            kd_e = ne; kd_ecap = o->nv * 2u;
        }
        for (x = 0; x < nD; x++) {
            unsigned nv = 0, kmax = 0, nbad = 0;
            float emax = 0.0f, med;
            for (k = 0; k < o->nv; k++) {
                float c0[4], c1[4], p[4], px1, py1, px0, py0, dx, dy, e;
                int j;
                ip_clip(o->rzb ? PB[o->vstart + k] : PA[a->vstart + k], IP_AMVP(o, a), c0);
                ip_clip(PB[o->vstart + k], o->mvp, c1);
                if (c0[3] <= 1e-6f || c1[3] <= 1e-6f) { continue; }
                px1 = (c1[0] / c1[3] * 0.5f + 0.5f) * o->vp.Width; py1 = (0.5f - c1[1] / c1[3] * 0.5f) * o->vp.Height;
                if (px1 < -0.5f * o->vp.Width || px1 > 1.5f * o->vp.Width || py1 < -0.5f * o->vp.Height || py1 > 1.5f * o->vp.Height) { continue; }
                for (j = 0; j < 4; j++) {
                    p[j] = (float)(c0[0] * D[x][0 * 4 + j] + c0[1] * D[x][1 * 4 + j] + c0[2] * D[x][2 * 4 + j] + c0[3] * D[x][3 * 4 + j]);
                }
                if (p[3] <= 1e-6f) { e = 1e6f; }
                else {
                    px0 = (p[0] / p[3] * 0.5f + 0.5f) * o->vp.Width; py0 = (0.5f - p[1] / p[3] * 0.5f) * o->vp.Height;
                    dx = px1 - px0; dy = py1 - py0;
                    e = sqrtf(dx * dx + dy * dy);
                }
                kd_e[nv++] = e;
                if (e > emax) { emax = e; kmax = k; }
            }
            if (!nv) { break; }
            memcpy(kd_e + nv, kd_e, nv * sizeof *kd_e);
            qsort(kd_e + nv, nv, sizeof *kd_e, kd_fcmp);
            med = kd_e[nv + nv / 2u];
            for (k = 0; k < nv; k++) { if (kd_e[k] > 8.0f && kd_e[k] > 4.0f * med) { nbad++; } }
            if (emax < best) { best = emax; bestx = x; bmed = med; bkmax = kmax; bnbad = nbad; bnv = nv; }
        }
        if (bestx < 0) { continue; }
        kd_draws[w]++;
        if (bnbad) {
            kd_bad[w]++;
            if (o->rzb) { kd_badrzb++; }
            if (best > kd_max[w]) { kd_max[w] = best; }
            if (kd_lines < (unsigned)g_cfg.seam_diag) {
                kd_lines++;
                HLOG_I("KEIL f=%u i=%u n=%u/%u %s rzb=%u m=%d raw=%s max=%.1fpx (Vertex %u) median=%.2fpx keil_vertices=%u/%u",
                       B->frame, i, o->nv, a->nv, kIP_WHY[w], o->rzb, o->match, a->rawhash == o->rawhash ? "gleich" : "neu",
                       (double)best, bkmax, (double)bmed, bnbad, bnv);
            }
        }
    }
    if (++kd_ticks >= 100u) {
        HLOG_I("KEILSUM ticks=%u lerp=%u/%u(max %.1f) rawlerp=%u/%u(max %.1f) rt2dlerp=%u/%u davon_rzb=%u (keil/gepruefte Draws)",
               kd_ticks, kd_bad[IP_W_LERP], kd_draws[IP_W_LERP], (double)kd_max[IP_W_LERP], kd_bad[IP_W_RAWLERP],
               kd_draws[IP_W_RAWLERP], (double)kd_max[IP_W_RAWLERP], kd_bad[IP_W_RT2DLERP], kd_draws[IP_W_RT2DLERP], kd_badrzb);
        kd_ticks = 0; kd_badrzb = 0;
        memset(kd_draws, 0, sizeof kd_draws); memset(kd_bad, 0, sizeof kd_bad); memset(kd_max, 0, sizeof kd_max);
    }
}

static int sd_cmp(const void *x, const void *y)
{
    const sd_ent_t *a = (const sd_ent_t *)x, *b = (const sd_ent_t *)y;
    if (a->key != b->key) { return a->key < b->key ? -1 : 1; }
    return a->op < b->op ? -1 : (a->op > b->op ? 1 : 0);
}
static void ip_seam_diag(ip_list_t *A, int ai, ip_list_t *B, int bi)
{
    const float T = 0.25f;
    float (*PA)[4] = ip_cpos[ai], (*PB)[4] = ip_cpos[bi];
    ID3D11RenderTargetView *rts[16];
    unsigned nrt = 0, ne = 0, i, k, g0, gtick = 0, gbad05 = 0, gbad2 = 0, gbadA = 0;
    double tmax = 0.0;
    struct { float s, dy; unsigned i0, i1, w0, w1; float x, y; float e[4]; unsigned k0, k1; } worst[3]; /* W429: e = echte w N-1/N beider Ecken */
    memset(worst, 0, sizeof worst);
    if (!PA || !PB || !A || !B || !(g_sba_loc ? loc_render_interp_mission() : sb_in_mission()) ||
        (g_cfg.pf_ring > 0 && !sb_pf_active())) { return; } /* mit pf_ring nur in den Bildfolgen; W410: auch LoC-Mission */
    ip_wedge_diag(A, PA, B, PB);
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        const ip_op_t *a = (o->match >= 0 && (unsigned)o->match < A->nops) ? &A->ops[o->match] : NULL;
        unsigned r;
        if (o->kind != IP_DRAW || !o->cls || o->why == IP_W_BB2D || o->why == IP_W_STATIC || !o->nv) { continue; } /* unbewegt (Cockpit) bewegt sich nicht gegen Nachbarn */
        for (r = 0; r < nrt && rts[r] != o->rtv; r++) {}
        if (r == nrt) { if (nrt >= 16u) { continue; } rts[nrt++] = o->rtv; }
        if (ne + o->nv > sd_cap) {
            unsigned nc = (ne + o->nv) * 2u;
            sd_ent_t *ne2 = (sd_ent_t *)realloc(sd_e, (size_t)nc * sizeof *sd_e);
            if (!ne2) { return; }
            sd_e = ne2; sd_cap = nc;
        }
        for (k = 0; k < o->nv; k++) {
            float c1[4], c[4], c0[4], px, py;
            int q, lerp = 0;
            long long qx, qy;
            float sew0 = 0.0f, sew1 = 0.0f; /* W429 DIAG */
            ip_clip(PB[o->vstart + k], o->mvp, c1);
            if (c1[3] <= 1e-6f) { continue; }
            px = o->vp.TopLeftX + (c1[0] / c1[3] * 0.5f + 0.5f) * o->vp.Width;
            py = o->vp.TopLeftY + (0.5f - c1[1] / c1[3] * 0.5f) * o->vp.Height;
            if (px < o->vp.TopLeftX || px > o->vp.TopLeftX + o->vp.Width || py < o->vp.TopLeftY || py > o->vp.TopLeftY + o->vp.Height) { continue; } /* nur sichtbare Ecken */
            memcpy(c, c1, sizeof c);
            c0[0] = c0[1] = c0[2] = 0.0f; c0[3] = -1.0f;
            if (o->flag && a && (o->rzb || k < a->nv)) {
                /* W429: wie der Zwischenbild-VS - echte w (interp_hlerp-Strom), homogener Lerp (Modus >= 2),
                 * W224-Regel mit echter w, Bildsprung-Regel nur in den W237-Modi 1/2 */
                float hw0 = 1.0f, hw1 = 1.0f, ew0, ew1, h0[4], h1[4];
                const int hl = sb_ip_hlerp();
                int pre = 0;
                ip_clip(o->rzb ? PB[o->vstart + k] : PA[a->vstart + k], IP_AMVP(o, a), c0);
                if (hl && A->wb_ok && B->wb_ok && A->ipw && B->ipw && o->vstart + k < B->ipwcap &&
                    (o->rzb || a->vstart + k < A->ipwcap)) {
                    hw1 = B->ipw[o->vstart + k];
                    hw0 = o->rzb ? hw1 : A->ipw[a->vstart + k];
                }
                ew0 = c0[3] * hw0; ew1 = c1[3] * hw1;
                sew0 = ew0; sew1 = ew1;
                memcpy(h0, c0, sizeof h0); memcpy(h1, c1, sizeof h1);
                if (hl >= 2 && c0[3] == 1.0f && c1[3] == 1.0f) {
                    pre = 1;
                    for (q = 0; q < 4; q++) { h0[q] *= hw0; h1[q] *= hw1; }
                }
                if (!(g_cfg.interp_eyefix && ew0 * ew1 <= 0.0f)) {
                    int jmp = 0;
                    if (g_cfg.interp_eyefix && hl && hl != 3 && ew0 > 0.0f && ew1 > 0.0f) {
                        float nx0 = c0[0] / c0[3], ny0 = c0[1] / c0[3], nx1 = c1[0] / c1[3], ny1 = c1[1] / c1[3];
                        jmp = fabsf(nx1 - nx0) > 0.5f * (1.0f + fmaxf(fabsf(nx0), fabsf(nx1))) ||
                              fabsf(ny1 - ny0) > 0.5f * (1.0f + fmaxf(fabsf(ny0), fabsf(ny1)));
                    }
                    if (!jmp) {
                        for (q = 0; q < 4; q++) { c[q] = (1.0f - T) * h0[q] + T * h1[q]; }
                        if (pre && c[3] != 0.0f) { c[0] /= c[3]; c[1] /= c[3]; c[2] /= c[3]; c[3] = 1.0f; }
                        lerp = 1;
                    }
                }
            }
            if (c[3] <= 1e-6f) { continue; }
            qx = (long long)floor((double)px * 32.0);
            qy = (long long)floor((double)py * 32.0);
            sd_e[ne].key = ((unsigned long long)r << 48) | ((unsigned long long)(qx & 0xFFFFFF) << 24) | (unsigned long long)(qy & 0xFFFFFF);
            sd_e[ne].op = i;
            sd_e[ne].nx = px; sd_e[ne].ny = py;
            sd_e[ne].tx = o->vp.TopLeftX + (c[0] / c[3] * 0.5f + 0.5f) * o->vp.Width;
            sd_e[ne].ty = o->vp.TopLeftY + (0.5f - c[1] / c[3] * 0.5f) * o->vp.Height;
            if (lerp && !o->rzb && c0[3] > 1e-6f) {
                sd_e[ne].ax = o->vp.TopLeftX + (c0[0] / c0[3] * 0.5f + 0.5f) * o->vp.Width;
                sd_e[ne].ay = o->vp.TopLeftY + (0.5f - c0[1] / c0[3] * 0.5f) * o->vp.Height;
            } else {
                sd_e[ne].ax = sd_e[ne].ay = 1e30f;
            }
            sd_e[ne].why = (unsigned char)(o->why < IP_W_N ? o->why : 0);
            sd_e[ne].flag = (unsigned char)(lerp ? 1 : 0);
            sd_e[ne].rzb = (unsigned char)(o->rzb != 0);
            sd_e[ne].rt = (unsigned char)r;
            sd_e[ne].ew0 = sew0; sd_e[ne].ew1 = sew1; sd_e[ne].k = k;
            ne++;
        }
    }
    /* Versatz exakt gezeichneter Draws gegenueber einer konsistenten Zwischenlage: "reject" = gleiche
     * Eingangsdaten (Vertex i <-> i ist derselbe Weltpunkt), nur ein Bezugs-Vertex sprang zu weit -> der
     * ganze Draw steht im Zwischenbild in Tick-N-Lage, gelerpte Nachbarn um (1-t)*(pN - pA) daneben. */
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        const ip_op_t *a = (o->match >= 0 && (unsigned)o->match < A->nops) ? &A->ops[o->match] : NULL;
        float dmax = 0.0f, dymax = 0.0f;
        unsigned non = 0, p2;
        if (o->kind != IP_DRAW || !o->cls || o->flag || o->why != IP_W_REJECT || !a || a->nv != o->nv) { continue; }
        p2 = ip_mvp_affine(o->mvp) ? 1u : 0u;
        for (k = 0; k < o->nv; k++) {
            float c0[4], c1[4], x0, y0, x1, y1, dx, dy, d;
            ip_clip(PA[a->vstart + k], a->mvp, c0);
            ip_clip(PB[o->vstart + k], o->mvp, c1);
            if (c0[3] <= 1e-6f || c1[3] <= 1e-6f) { continue; }
            x0 = (c0[0] / c0[3] * 0.5f + 0.5f) * o->vp.Width; y0 = (0.5f - c0[1] / c0[3] * 0.5f) * o->vp.Height;
            x1 = (c1[0] / c1[3] * 0.5f + 0.5f) * o->vp.Width; y1 = (0.5f - c1[1] / c1[3] * 0.5f) * o->vp.Height;
            if (x1 < 0.0f || x1 > o->vp.Width || y1 < 0.0f || y1 > o->vp.Height) { continue; } /* nur sichtbare Ecken */
            dx = (1.0f - T) * (x1 - x0); dy = (1.0f - T) * (y1 - y0);
            d = sqrtf(dx * dx + dy * dy);
            if (d > dmax) { dmax = d; }
            if (fabsf(dy) > dymax) { dymax = fabsf(dy); }
            non++;
        }
        if (!non) { continue; }
        sd_rj_draws[p2]++; sd_rj_verts[p2] += non;
        if (dmax > 0.5f) { sd_rj_bad[p2]++; }
        if (dmax > sd_rj_max[p2]) { sd_rj_max[p2] = dmax; }
        if (dymax > sd_rj_dy[p2]) { sd_rj_dy[p2] = dymax; }
        sd_rj_sum[p2] += dmax;
        if (sd_rjl < (unsigned)g_cfg.seam_diag && dmax > 0.5f && (g_cfg.pf_ring <= 0 || sb_pf_active())) {
            sd_rjl++;
            HLOG_I("SEAMRJ f=%u i=%u n=%u p2=%u sichtbar=%u versatz_max=%.2fpx dy_max=%.2fpx (t=%.2f, exakt statt gelerpt)",
                   B->frame, i, o->nv, p2, non, (double)dmax, (double)dymax, (double)T);
        }
    }
    if (!ne) { return; }
    qsort(sd_e, ne, sizeof *sd_e, sd_cmp);
    for (g0 = 0; g0 < ne;) {
        unsigned g1 = g0 + 1, x, y, multi = 0, allA = 1;
        float best = 0.0f, bdy = 0.0f, bestA = 0.0f;
        unsigned bx = g0, by = g0;
        while (g1 < ne && sd_e[g1].key == sd_e[g0].key) { if (sd_e[g1].op != sd_e[g0].op) { multi = 1; } g1++; }
        if (!multi) { g0 = g1; continue; }
        for (x = g0; x < g1; x++) {
            if (sd_e[x].ax > 1e29f) { allA = 0; }
            for (y = x + 1; y < g1; y++) {
                float dx, dy, s;
                if (sd_e[y].op == sd_e[x].op) { continue; }
                dx = sd_e[y].tx - sd_e[x].tx; dy = sd_e[y].ty - sd_e[x].ty;
                s = sqrtf(dx * dx + dy * dy);
                if (s > best) { best = s; bdy = dy < 0 ? -dy : dy; bx = x; by = y; }
                if (sd_e[x].ax < 1e29f && sd_e[y].ax < 1e29f) {
                    float ex = sd_e[y].ax - sd_e[x].ax, ey = sd_e[y].ay - sd_e[x].ay, sa = sqrtf(ex * ex + ey * ey);
                    if (sa > bestA) { bestA = sa; }
                }
            }
        }
        gtick++;
        if (allA && bestA > 0.5f) { gbadA++; }
        {
            unsigned w0 = sd_e[bx].why, w1 = sd_e[by].why, cl;
            if (w0 > w1) { unsigned tt = w0; w0 = w1; w1 = tt; }
            cl = w0 * IP_W_N + w1;
            if (sd_e[bx].flag != sd_e[by].flag) { cl = IP_W_N * IP_W_N; } /* gemischt exakt/gelerpt */
            sd_cls_n[cl]++;
            if (best > 0.5f) {
                sd_cls_bad[cl]++;
                gbad05++;
                if (cl == IP_W_N * IP_W_N) { /* W429 DIAG: warum ist die exakte Ecke exakt? */
                    const sd_ent_t *ee = sd_e[bx].flag ? &sd_e[by] : &sd_e[bx];
                    const ip_op_t *eo = &B->ops[ee->op];
                    unsigned c = eo->flag ? 0u : eo->exr == 1 ? 1u : eo->exr == 2 ? 2u : eo->why == IP_W_NOMATCH ? 3u :
                                 eo->why == IP_W_RAWCHG ? 4u : eo->why == IP_W_REJECT ? 5u : 6u;
                    sd_mx[ip_mvp_affine(eo->mvp) ? 1 : 0][c]++;
                }
                if (best > 2.0f) { gbad2++; }
                if (best > sd_cls_max[cl]) { sd_cls_max[cl] = best; }
                for (x = 0; x < 3; x++) {
                    if (best > worst[x].s) {
                        memmove(&worst[x + 1], &worst[x], (2u - x) * sizeof worst[0]);
                        worst[x].s = best; worst[x].dy = bdy;
                        worst[x].i0 = sd_e[bx].op; worst[x].i1 = sd_e[by].op;
                        worst[x].w0 = sd_e[bx].why | ((unsigned)sd_e[bx].flag << 8) | ((unsigned)sd_e[bx].rzb << 9);
                        worst[x].w1 = sd_e[by].why | ((unsigned)sd_e[by].flag << 8) | ((unsigned)sd_e[by].rzb << 9);
                        worst[x].x = sd_e[bx].nx; worst[x].y = sd_e[bx].ny;
                        worst[x].e[0] = sd_e[bx].ew0; worst[x].e[1] = sd_e[bx].ew1; worst[x].k0 = sd_e[bx].k;
                        worst[x].e[2] = sd_e[by].ew0; worst[x].e[3] = sd_e[by].ew1; worst[x].k1 = sd_e[by].k;
                        break;
                    }
                }
            }
        }
        if (best > tmax) { tmax = best; }
        g0 = g1;
    }
    sd_ticks++; sd_groups += gtick; sd_bad05 += gbad05; sd_bad2 += gbad2; sd_badA += gbadA;
    if (tmax > sd_maxs) { sd_maxs = tmax; }
    if (sd_lines < (unsigned)g_cfg.seam_diag && (gbad05 || gbadA) && (g_cfg.pf_ring <= 0 || sb_pf_active())) {
        sd_lines++;
        HLOG_I("SEAM f=%u t=%.2f gruppen=%u spalt>0.5px=%u spalt>2px=%u max=%.2fpx vortick_streu>0.5px=%u",
               B->frame, (double)T, gtick, gbad05, gbad2, tmax, gbadA);
        for (k = 0; k < 3 && worst[k].s > 0.0f; k++) {
            const ip_op_t *o0 = &B->ops[worst[k].i0], *o1 = &B->ops[worst[k].i1];
            HLOG_I("SEAMW f=%u #%u s=%.2fpx dy=%.2f bei=%.1f,%.1f A: i=%u n=%u %s flag=%u rzb=%u m=%d | B: i=%u n=%u %s flag=%u rzb=%u m=%d",
                   B->frame, k, (double)worst[k].s, (double)worst[k].dy, (double)worst[k].x, (double)worst[k].y,
                   worst[k].i0, o0->nv, kIP_WHY[worst[k].w0 & 0xFF], (worst[k].w0 >> 8) & 1u, (worst[k].w0 >> 9) & 1u, o0->match,
                   worst[k].i1, o1->nv, kIP_WHY[worst[k].w1 & 0xFF], (worst[k].w1 >> 8) & 1u, (worst[k].w1 >> 9) & 1u, o1->match);
            if (o0->exr == 1 && o0->exj >= 0 && (unsigned)o0->exj < B->nops) { /* W429 DIAG: ausloesender exakter Durchgang */
                const ip_op_t *e = &B->ops[o0->exj];
                HLOG_I("SEAMP f=%u i=%u -> j=%d %s m=%d n=%u p2w=%u gleiche_lage=%d", B->frame, worst[k].i0, o0->exj,
                       kIP_WHY[e->why < IP_W_N ? e->why : 0], e->match, e->nv, e->p2w,
                       memcmp(e->pref, o0->pref, sizeof e->pref) == 0);
            }
            HLOG_I("SEAMV f=%u #%u A: i=%u k=%u p2=%u ex=%u ib=%u/%u w=%.4g->%.4g | B: i=%u k=%u p2=%u ex=%u ib=%u/%u w=%.4g->%.4g", B->frame, k, /* W429 DIAG */
                   worst[k].i0, worst[k].k0, (unsigned)ip_mvp_affine(o0->mvp), o0->exr, o0->ib1,
                   (o0->match >= 0 && (unsigned)o0->match < A->nops) ? A->ops[o0->match].ib1 : 99u, (double)worst[k].e[0], (double)worst[k].e[1],
                   worst[k].i1, worst[k].k1, (unsigned)ip_mvp_affine(o1->mvp), o1->exr, o1->ib1,
                   (o1->match >= 0 && (unsigned)o1->match < A->nops) ? A->ops[o1->match].ib1 : 99u, (double)worst[k].e[2], (double)worst[k].e[3]);
        }
    }
    if (sd_ticks >= 200u) {
        char buf[900];
        int p = 0;
        unsigned c2;
        buf[0] = 0;
        for (c2 = 0; c2 <= IP_W_N * IP_W_N && p < (int)sizeof buf - 64; c2++) {
            if (!sd_cls_n[c2]) { continue; }
            if (c2 == IP_W_N * IP_W_N) {
                p += _snprintf_s(buf + p, sizeof buf - (size_t)p, _TRUNCATE, " exakt/gelerpt=%u/%u(max %.1f)", sd_cls_bad[c2], sd_cls_n[c2], (double)sd_cls_max[c2]);
            } else {
                p += _snprintf_s(buf + p, sizeof buf - (size_t)p, _TRUNCATE, " %s/%s=%u/%u(max %.1f)", kIP_WHY[c2 / IP_W_N], kIP_WHY[c2 % IP_W_N],
                                 sd_cls_bad[c2], sd_cls_n[c2], (double)sd_cls_max[c2]);
            }
        }
        HLOG_I("SEAMSUM ticks=%u gruppen=%u spalt>0.5px=%u spalt>2px=%u max=%.1fpx vortick_streu>0.5px=%u klassen(spalt/gruppen):%s",
               sd_ticks, sd_groups, sd_bad05, sd_bad2, sd_maxs, sd_badA, buf);
        HLOG_I("SEAMRJSUM ticks=%u p2: draws=%u ecken=%u versatz>0.5px=%u max=%.1fpx mittel=%.2fpx dy_max=%.1fpx | 3d: draws=%u ecken=%u versatz>0.5px=%u max=%.1fpx mittel=%.2fpx dy_max=%.1fpx",
               sd_ticks, sd_rj_draws[1], sd_rj_verts[1], sd_rj_bad[1], (double)sd_rj_max[1],
               sd_rj_draws[1] ? sd_rj_sum[1] / sd_rj_draws[1] : 0.0, (double)sd_rj_dy[1],
               sd_rj_draws[0], sd_rj_verts[0], sd_rj_bad[0], (double)sd_rj_max[0],
               sd_rj_draws[0] ? sd_rj_sum[0] / sd_rj_draws[0] : 0.0, (double)sd_rj_dy[0]);
        HLOG_I("SEAMX ticks=%u exakte_seite 3d: vertex=%u pass=%u ablage=%u nomatch=%u rawchg=%u reject=%u sonst=%u | p2: vertex=%u pass=%u ablage=%u nomatch=%u rawchg=%u reject=%u sonst=%u",
               sd_ticks, sd_mx[0][0], sd_mx[0][1], sd_mx[0][2], sd_mx[0][3], sd_mx[0][4], sd_mx[0][5], sd_mx[0][6],
               sd_mx[1][0], sd_mx[1][1], sd_mx[1][2], sd_mx[1][3], sd_mx[1][4], sd_mx[1][5], sd_mx[1][6]); /* W429 DIAG */
        memset(sd_mx, 0, sizeof sd_mx);
        memset(sd_rj_draws, 0, sizeof sd_rj_draws); memset(sd_rj_verts, 0, sizeof sd_rj_verts); memset(sd_rj_bad, 0, sizeof sd_rj_bad);
        memset(sd_rj_max, 0, sizeof sd_rj_max); memset(sd_rj_dy, 0, sizeof sd_rj_dy); memset(sd_rj_sum, 0, sizeof sd_rj_sum);
        sd_ticks = sd_groups = sd_bad05 = sd_bad2 = sd_badA = 0;
        sd_maxs = 0.0;
        memset(sd_cls_n, 0, sizeof sd_cls_n); memset(sd_cls_bad, 0, sizeof sd_cls_bad); memset(sd_cls_max, 0, sizeof sd_cls_max);
    }
}

/* ---------- Verifikation (interp_verify) ---------- */

static ID3D11Texture2D *ip_vstg[2];

static int ip_stage(int k, ID3D11Resource *src)
{
    if (!ip_vstg[k]) {
        D3D11_TEXTURE2D_DESC dd;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)src, &dd);
        dd.Usage = D3D11_USAGE_STAGING;
        dd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        dd.BindFlags = 0;
        dd.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &ip_vstg[k]))) {
            ip_vstg[k] = NULL;
            return 0;
        }
    }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)ip_vstg[k], src);
    return 1;
}

unsigned sb_interp_pairs(void)
{
    return ip_pairs_total;
}

/* ---------- W361 LOC-INTERP-CRASH: verzoegerte Freigabe ----------
 * Die Ops der Zwischenbild-Listen tragen RENDER-QA-Shader/Layouts roh (rq_rec_t vs/vsl/lay/layl/ps). Ersetzt
 * render_shade.c einen Cache-Eintrag (Handle mit neuem Programm, Roh-Variante mit anderem Vertex-Format), darf
 * das alte Objekt erst weg, wenn keine Liste es mehr enthalten kann: eine Liste lebt hoechstens Aufnahme (1 Swap)
 * + Historie (IP_HIST Swaps), danach IP_LISTS + 1 Swaps Abstand und vorher den Render-Thread abwarten. Ohne Listen
 * (Zwischenbilder aus, keine Foto-Aufnahme) sofort. Aufruf auch ohne ctx_lock (render_shade gibt ihn beim
 * Erstellen ab) - eigene Sperre. */
typedef struct { IUnknown *u; unsigned due; } ip_rl_t;
static ip_rl_t *ip_rl;
static unsigned ip_rl_n, ip_rl_cap, ip_swapno;
static SRWLOCK ip_rl_lock = SRWLOCK_INIT;
static volatile LONG st_rl_tot;

void sb_interp_release_later(IUnknown *u)
{
    if (!u) { return; }
    if (!ip_init_done || (g_cfg.interp_fps <= 0 && ip_nhist == 0 && ip_rec < 0)) { IUnknown_Release(u); return; }
    AcquireSRWLockExclusive(&ip_rl_lock);
    if (ip_rl_n == ip_rl_cap) {
        unsigned nc = ip_rl_cap ? ip_rl_cap * 2u : 64u;
        ip_rl_t *p = (ip_rl_t *)realloc(ip_rl, nc * sizeof *p);
        if (!p) { /* kein Speicher: lieber festhalten als zu frueh freigeben */
            ReleaseSRWLockExclusive(&ip_rl_lock);
            return;
        }
        ip_rl = p;
        ip_rl_cap = nc;
    }
    ip_rl[ip_rl_n].u = u;
    ip_rl[ip_rl_n].due = ip_swapno + IP_LISTS + 1u;
    ip_rl_n++;
    ReleaseSRWLockExclusive(&ip_rl_lock);
    if (InterlockedIncrement(&st_rl_tot) <= 4) {
        HLOG_I("INTERP W361: Objekt einer Zwischenbild-Liste ersetzt -> Freigabe nach %u Swaps (Render-Thread abgewartet)",
               IP_LISTS + 1u);
    }
}

/* Faellige (all: alle) Objekte freigeben. Unter ctx_lock. */
static void ip_rl_drain(int all)
{
    unsigned i, k = 0;
    int quiet = 0;
    if (!ip_rl_n) { return; }
    AcquireSRWLockExclusive(&ip_rl_lock);
    for (i = 0; i < ip_rl_n; i++) {
        if (all || (int)(ip_swapno - ip_rl[i].due) >= 0) {
            if (!quiet) { ip_rt_quiesce(); quiet = 1; }
            IUnknown_Release(ip_rl[i].u);
        } else {
            ip_rl[k++] = ip_rl[i];
        }
    }
    ip_rl_n = k;
    ReleaseSRWLockExclusive(&ip_rl_lock);
}

/* W81 WINMODE: vor ResizeBuffers alles freigeben, was Backbuffer-Ressourcen
 * (oder ihre Views) haelt: Aufzeichnungs-/Historienlisten (o->rtv/dsv/srv),
 * Schatten-Eintraege (AddRef auf Spiel-BB/RTs, Kopien in BB-Groesse) und
 * die Verify-Staging-Texturen. Der Takt stoppt; die naechsten zwei Ticks
 * bauen die Historie in der neuen Groesse neu auf. Aufruf unter ctx_lock. */
void sb_interp_reset(const char *why)
{
    int i, nsh = 0;
    if (!ip_init_done) { return; }
    ip_rt_quiesce(); /* W126 RENDER-THREAD: Render-Thread liest Listen/Schatten */
    for (i = 0; i < IP_LISTS; i++) { ip_list_reset(&g_ipl[i]); }
    ip_rl_drain(1); /* W361: keine Liste mehr -> alles Verzoegerte frei */
    ip_nhist = 0;
    ip_nneed = 0;
    ip_live = 0;
    ip_rec = -1;
    g_ip_recording = 0;
    ip_shot_ser = 0;
    for (i = 0; i < IP_SH_MAX; i++) {
        if (g_ish[i].res) { nsh++; }
        ip_sh_free(&g_ish[i]);
    }
    for (i = 0; i < 2; i++) {
        if (ip_vstg[i]) { ID3D11Texture2D_Release(ip_vstg[i]); ip_vstg[i] = NULL; }
    }
    HLOG_I("INTERP: Reset (%s): Listen, %d Schatten, Staging freigegeben", why ? why : "", nsh);
}

/* Schatten-BB (exakte Wiedergabe) gegen gesicherten Spiel-BB vergleichen. */
static void ip_verify_check(ip_list_t *B, int bi)
{
    D3D11_MAPPED_SUBRESOURCE m0, m1;
    unsigned x, y, ndiff = 0, maxd = 0, x0 = 0xFFFFFFFFu, y0 = 0xFFFFFFFFu, x1 = 0, y1 = 0;
    ip_sh_t *e;
    if (B->bbref < 0) { return; }
    e = &g_ish[B->bbref];
    if (!ip_stage(0, (ID3D11Resource *)e->sh) || !ip_stage(1, (ID3D11Resource *)e->bak[bi])) { return; }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)ip_vstg[0], 0, D3D11_MAP_READ, 0, &m0))) {
        return;
    }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)ip_vstg[1], 0, D3D11_MAP_READ, 0, &m1))) {
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)ip_vstg[0], 0);
        return;
    }
    for (y = 0; y < g_bb_h; y++) {
        const unsigned char *a = (const unsigned char *)m0.pData + (size_t)y * m0.RowPitch;
        const unsigned char *b = (const unsigned char *)m1.pData + (size_t)y * m1.RowPitch;
        if (memcmp(a, b, (size_t)g_bb_w * 4u) == 0) { continue; }
        for (x = 0; x < g_bb_w; x++) {
            unsigned c, d = 0;
            for (c = 0; c < 3u; c++) {
                unsigned dc = a[x * 4u + c] > b[x * 4u + c] ? a[x * 4u + c] - b[x * 4u + c]
                                                             : b[x * 4u + c] - a[x * 4u + c];
                if (dc > d) { d = dc; }
            }
            if (d) {
                ndiff++;
                if (d > maxd) { maxd = d; }
                if (x < x0) { x0 = x; }
                if (y < y0) { y0 = y; }
                if (x > x1) { x1 = x; }
                if (y > y1) { y1 = y; }
            }
        }
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)ip_vstg[1], 0);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)ip_vstg[0], 0);
    st_verify++;
    if (ndiff) { st_verify_bad++; }
    HLOG_I("INTERP verify f=%u: diff=%u px max=%u box=%u,%u-%u,%u (exakte Wiedergabe vs. Spiel-Bild)",
           B->frame, ndiff, maxd, ndiff ? x0 : 0, ndiff ? y0 : 0, x1, y1);
}

/* ---------- GPU-Zeit einer Wiedergabe (interp_diag, Timestamp-Queries) ---------- */
static ID3D11Query *ip_qd, *ip_qt[2];
static int ip_q_pend;

static int ip_gpu_begin(void)
{
    D3D11_QUERY_DESC qd;
    if (g_cfg.interp_diag < 1 || ip_q_pend || (st_presents % 37u) != 0u) { return 0; }
    if (!ip_qd) {
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &ip_qd))) { ip_qd = NULL; return 0; }
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &ip_qt[0])) ||
            FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &ip_qt[1]))) { return 0; }
    }
    ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous *)ip_qd);
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)ip_qt[0]);
    return 1;
}

static void ip_gpu_end(void)
{
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)ip_qt[1]);
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)ip_qd);
    ip_q_pend = 1;
}

static void ip_gpu_poll(void)
{
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    UINT64 t0, t1;
    if (!ip_q_pend) { return; }
    if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)ip_qd, &dj, sizeof dj,
                                    D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) { return; }
    ip_q_pend = 0;
    if (dj.Disjoint || !dj.Frequency) { return; }
    if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)ip_qt[0], &t0, sizeof t0, 0) != S_OK ||
        ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)ip_qt[1], &t1, sizeof t1, 0) != S_OK) {
        return;
    }
    {
        double ms = (double)(t1 - t0) * 1000.0 / (double)dj.Frequency;
        st_gpu_ms += ms;
        st_gpu_n++;
        if (ms > st_gpu_max) { st_gpu_max = ms; }
    }
}

/* ---------- Bilder zeigen ---------- */

static int ip_list_index(const ip_list_t *L)
{
    return (int)(L - g_ipl);
}

/* ---------- PERF(RENDER-THREAD,W126): Wiedergabe auf eigenem Thread ----------
 * render_thread=1: Ein Host-Thread (kein Xbox-Thread, haelt keine Host-Sperre)
 * zeichnet die Zwischenbild-Wiedergaben in einen D3D11-Deferred-Context auf
 * (ip_replay_ex mit eigenem Context und eigenen Konstantenpuffern). Der
 * Spiel-Thread fuehrt bei Faelligkeit nur ExecuteCommandList aus (Blit,
 * Present, Effekte wie bisher). Ein Auftrags-Platz: Nach jedem gezeigten Bild
 * und jedem Swap stellt der Spiel-Thread das naechste faellige Paar (A, B, t)
 * vor (ip_rt_prefetch). Passt es bei Faelligkeit nicht (Swap dazwischen,
 * anderes t), wird es verworfen und ueber denselben Pfad synchron erzeugt
 * (gleiche Bilder in jedem Fall). Die Listen eines offenen Auftrags sind
 * gepinnt: Historien-Abwurf, Reset und Schatten-Verdraengung warten, bis der
 * Render-Thread fertig ist (ip_rt_drop/ip_rt_quiesce). Den Immediate-Context
 * benutzt weiterhin nur der Spiel-Thread. */
#define IP_RT_FREE 0
#define IP_RT_REQ 1
#define IP_RT_DONE 2
typedef struct {
    volatile LONG st;
    int ai, bi;
    unsigned aser, bser;
    float t;
    ID3D11CommandList *cl;
    LONGLONG q0, q1; /* Aufzeichnung Start/Ende (QPC, Render-Thread) */
    ip_rc_t rc;      /* DIAG(PERF-M2,W174): Zaehler der Aufzeichnung */
} ip_rtjob_t;
static ip_rtjob_t ip_rtj;
static HANDLE ip_rt_th, ip_rt_ev;
static DWORD ip_rt_tid;
static ip_cx_t ip_rt_cx;
static int ip_rt_ok, ip_rt_tried;
static unsigned st_rt_hit, st_rt_miss, st_rt_wait, st_rt_stale, st_rt_n, st_rt_fail, st_rt_exec;
static double st_rt_ms, st_rt_max, st_rt_wait_ms, st_rt_exec_ms;
static ip_cx_t ip_cx_imm_v;
static ip_rc_t ip_rc_got; /* DIAG(PERF-M2,W174): Zaehler des zuletzt abgeholten Auftrags */
static ip_list_t *ip_rt_nA, *ip_rt_nB; /* Testtakt: Folgebild (A, B, t) fuer ip_rt_get */
static float ip_rt_nt;

static const ip_cx_t *ip_cx_imm(void)
{
    ip_cx_imm_v.c = g_ctx;
    ip_cx_imm_v.c1 = g_rq_ctx1;
    ip_cx_imm_v.cb0 = ip_cb0;
    ip_cx_imm_v.cb1 = ip_cb1;
    ip_cx_imm_v.deferred = 0;
    return &ip_cx_imm_v;
}

static DWORD WINAPI ip_rt_main(LPVOID p)
{
    (void)p;
    for (;;) {
        WaitForSingleObject(ip_rt_ev, INFINITE);
        if (ip_rtj.st == IP_RT_REQ) {
            ip_rtjob_t *j = &ip_rtj;
            ID3D11CommandList *cl = NULL;
            j->q0 = ip_now();
            if (ip_sh_rc_watch(0, j->ai >= 0 ? &g_ipl[j->ai] : NULL, &g_ipl[j->bi], "rt")) { /* W361: tote View -> nicht binden */
                cl = NULL;
            } else {
            ip_replay_ex(&g_ipl[j->bi], j->bi, j->ai >= 0 ? &g_ipl[j->ai] : NULL, j->ai, j->t, 0, &ip_rt_cx);
            j->rc = ip_rc_ctx[1]; /* DIAG(PERF-M2,W174) */
            if (FAILED(ID3D11DeviceContext_FinishCommandList(ip_rt_cx.c, FALSE, &cl))) { cl = NULL; }
            }
            j->cl = cl;
            j->q1 = ip_now();
            InterlockedExchange(&j->st, IP_RT_DONE);
        }
    }
}

static int ip_rt_start(void)
{
    D3D11_BUFFER_DESC bd;
    D3D11_FEATURE_DATA_THREADING th;
    ID3D11DeviceContext *dc = NULL;
    if (ip_rt_tried) { return ip_rt_ok; }
    ip_rt_tried = 1;
    if (!g_dev || !ip_cb0) { return 0; }
    if (FAILED(ID3D11Device_CreateDeferredContext(g_dev, 0, &dc)) || !dc) {
        HLOG_W("INTERP rt: CreateDeferredContext fehlgeschlagen -> Wiedergabe im Spiel-Thread");
        return 0;
    }
    memset(&ip_rt_cx, 0, sizeof ip_rt_cx);
    ip_rt_cx.c = dc;
    if (g_rq_ctx1 && FAILED(ID3D11DeviceContext_QueryInterface(dc, &IID_ID3D11DeviceContext1, (void **)&ip_rt_cx.c1))) {
        ip_rt_cx.c1 = NULL;
        HLOG_W("INTERP rt: Deferred-Context ohne ID3D11DeviceContext1 -> Wiedergabe im Spiel-Thread");
        return 0;
    }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 80;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &ip_rt_cx.cb0)) ||
        FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &ip_rt_cx.cb1))) {
        HLOG_W("INTERP rt: Konstantenpuffer fehlgeschlagen -> Wiedergabe im Spiel-Thread");
        return 0;
    }
    ip_rt_cx.deferred = 1;
    (void)sb_ta_sampler(0, 0); /* Cache vorwaermen: der Render-Thread liest ihn nur */
    ip_rt_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!ip_rt_ev) { return 0; }
    ip_rt_th = CreateThread(NULL, 0, ip_rt_main, NULL, 0, &ip_rt_tid);
    if (!ip_rt_th) { HLOG_W("INTERP rt: CreateThread fehlgeschlagen (%lu)", GetLastError()); return 0; }
    if (g_cfg.render_thread_core >= 0 && g_cfg.render_thread_core < 32) {
        SetThreadAffinityMask(ip_rt_th, (DWORD_PTR)1 << g_cfg.render_thread_core);
    }
    memset(&th, 0, sizeof th);
    ID3D11Device_CheckFeatureSupport(g_dev, D3D11_FEATURE_THREADING, &th, sizeof th);
    ip_rt_ok = 1;
    HLOG_I("INTERP rt: Render-Thread T%04lx bereit (Deferred-Context, Kern %d, DriverCommandLists=%d)",
           ip_rt_tid, g_cfg.render_thread_core, th.DriverCommandLists ? 1 : 0);
    return 1;
}

/* Render-Thread fuer diesen Lauf aktiv? Diagnosen, die den Immediate-Context
 * waehrend der Wiedergabe brauchen (perf_gsplit, interp_diag>=3), behalten den
 * alten Pfad. */
static int ip_rt_active(void)
{
    if (g_cfg.render_thread <= 0 || g_cfg.interp_diag >= 3 || g_cfg.perf_gsplit > 0) { return 0; }
    return ip_rt_start();
}

static void ip_rt_wait(void)
{
    while (ip_rtj.st == IP_RT_REQ) {
        int k;
        for (k = 0; k < 64 && ip_rtj.st == IP_RT_REQ; k++) { YieldProcessor(); }
        if (ip_rtj.st == IP_RT_REQ) { SwitchToThread(); }
    }
}

/* Fertigen Auftrag abholen (Statistik), Platz freigeben; Command-List an den Aufrufer. */
static ID3D11CommandList *ip_rt_collect(void)
{
    ID3D11CommandList *cl = ip_rtj.cl;
    double ms = ip_ms(ip_rtj.q0, ip_rtj.q1);
    ip_rtj.cl = NULL;
    ip_rc_got = ip_rtj.rc; /* DIAG(PERF-M2,W174) */
    st_rt_n++;
    st_rt_ms += ms;
    if (ms > st_rt_max) { st_rt_max = ms; }
    if (!cl) { st_rt_fail++; }
    InterlockedExchange(&ip_rtj.st, IP_RT_FREE);
    return cl;
}

/* Offenen Auftrag abwarten und verwerfen (vor Reset/Verdraengung). */
static void ip_rt_quiesce(void)
{
    ID3D11CommandList *cl;
    if (ip_rtj.st == IP_RT_FREE) { return; }
    ip_rt_wait();
    st_rt_stale++;
    cl = ip_rt_collect();
    if (cl) { ID3D11CommandList_Release(cl); }
}

/* Liste li verlaesst die Historie: offenen Auftrag mit li verwerfen. */
static void ip_rt_drop(int li)
{
    if (ip_rtj.st != IP_RT_FREE && (ip_rtj.ai == li || ip_rtj.bi == li)) { ip_rt_quiesce(); }
}

static int ip_rt_match(int ai, int bi, unsigned aser, unsigned bser, float t)
{
    return ip_rtj.ai == ai && ip_rtj.bi == bi && ip_rtj.aser == aser && ip_rtj.bser == bser &&
           memcmp(&ip_rtj.t, &t, sizeof t) == 0;
}

static void ip_rt_post(int ai, int bi, unsigned aser, unsigned bser, float t)
{
    ip_rtj.ai = ai;
    ip_rtj.bi = bi;
    ip_rtj.aser = aser;
    ip_rtj.bser = bser;
    ip_rtj.t = t;
    ip_rtj.cl = NULL;
    InterlockedExchange(&ip_rtj.st, IP_RT_REQ);
    SetEvent(ip_rt_ev);
}

/* Command-List der Wiedergabe (A, B, t): vorbereiteter Auftrag (ggf. abwarten)
 * oder neu ueber den Render-Thread (synchron). NULL = Fehlschlag. */
static ID3D11CommandList *ip_rt_get(ip_list_t *A, ip_list_t *B, float t)
{
    int ai = A ? ip_list_index(A) : -1, bi = ip_list_index(B);
    unsigned aser = A ? A->serial : 0u, bser = B->serial;
    LONGLONG q0 = ip_now();
    if (ip_rtj.st != IP_RT_FREE && ip_rt_match(ai, bi, aser, bser, t)) {
        if (ip_rtj.st == IP_RT_REQ) { st_rt_wait++; ip_rt_wait(); }
        st_rt_hit++;
    } else {
        ip_rt_quiesce();
        ip_rt_post(ai, bi, aser, bser, t);
        ip_rt_wait();
        st_rt_miss++;
    }
    st_rt_wait_ms += ip_ms(q0, ip_now());
    {
        ID3D11CommandList *cl = ip_rt_collect();
        if (ip_rt_nA) { /* Testtakt: naechstes Bild sofort vorbestellen (Aufzeichnung waehrend Ausfuehrung/Present) */
            ip_rt_post(ip_list_index(ip_rt_nA), ip_list_index(ip_rt_nB), ip_rt_nA->serial, ip_rt_nB->serial, ip_rt_nt);
            ip_rt_nA = NULL;
        }
        return cl;
    }
}

static void ip_pair_at(LONGLONG due, ip_list_t **pA, ip_list_t **pB, float *pt, int *phold);

/* Naechstes faelliges Zwischenbild vorbereiten (nach Bild bzw. Swap). */
static void ip_rt_prefetch(void)
{
    ip_list_t *A, *B;
    float t;
    int hold, ai, bi;
    if (!ip_rt_ok || g_cfg.render_thread <= 0 || !ip_live || ip_nhist < 1) { return; }
    ip_pair_at(ip_tnext, &A, &B, &t, &hold);
    if (!A || hold || t >= 1.0f) { return; }
    ai = ip_list_index(A);
    bi = ip_list_index(B);
    if (ip_rtj.st != IP_RT_FREE) {
        if (ip_rt_match(ai, bi, A->serial, B->serial, t)) { return; }
        if (ip_rtj.st == IP_RT_REQ) { return; } /* laeuft noch: nicht abbrechbar */
        ip_rt_quiesce();
    }
    ip_rt_post(ai, bi, A->serial, B->serial, t);
}

/* Command-List im Spiel-Thread ausfuehren (Immediate-Zustand bleibt). */
static void ip_rt_exec(ID3D11CommandList *cl)
{
    LONGLONG q0 = ip_now();
    WD_CALL("ip_exec_cl", NULL, ID3D11DeviceContext_ExecuteCommandList(g_ctx, cl, TRUE));
    ID3D11CommandList_Release(cl);
    st_rt_exec++;
    st_rt_exec_ms += ip_ms(q0, ip_now());
}

/* ---------- DIAG(PERF-M2,W174) ip_gdiag: GPU-Zeit je Wiedergabe ----------
 * Timestamps im Immediate-Context um die Wiedergabe (Command-List bzw.
 * direkte Wiedergabe inkl. Startkopien), asynchron gelesen. Je Messung eine
 * Zeile in <SB_OUT_DIR>/ipgpu.csv mit den Zaehlern der Wiedergabe, alle 10 s
 * Zeile IPGPU je Variante (ip_ablate). Funktioniert mit render_thread=1. */
#define GD_N 16
typedef struct {
    ID3D11Query *dj, *t0, *t1;
    int st; /* 0 frei, 2 wartet */
    ip_rc_t rc;
    unsigned ser;
    float t;
} gd_slot_t;
static gd_slot_t gd_s[GD_N];
static unsigned gd_cnt, gd_lost;
static int gd_fail;
static FILE *gd_f;
static double gd_ms[8], gd_dr[8], gd_vb[8], gd_mx[8];
static unsigned gd_n[8];
static LONGLONG gd_t0;
static const char *const gd_vn[8] = { "normal", "vp1x1", "v3", "nocb", "nodraw", "ring", "gpuvb", "ring+gpuvb" };

static int gd_vi(unsigned var)
{
    int k = 0;
    while (var && k < 7) { var >>= 1; k++; }
    return k;
}

static void gd_log(int force)
{
    LONGLONG now = ip_now();
    char b[1400];
    int o = 0, k;
    if (!gd_t0) { gd_t0 = now; return; }
    if (!force && ip_ms(gd_t0, now) < 10000.0) { return; }
    for (k = 0; k < 8 && o >= 0; k++) {
        if (!gd_n[k]) { continue; }
        o += _snprintf_s(b + o, sizeof b - (size_t)o, _TRUNCATE, " | %s n=%u gpu=%.3fms(max %.2f) draws=%.0f vb=%.1fMB",
                         gd_vn[k], gd_n[k], gd_ms[k] / gd_n[k], gd_mx[k], gd_dr[k] / gd_n[k], gd_vb[k] / gd_n[k]);
    }
    if (o > 0) {
        HLOG_I("IPGPU %.1fs:%s | verloren=%u vb_kopien=%u (%.1f MB/Kopie, fehl %u, GPU %.3fms(max %.2f) je Kopie, n=%u) neuanlagen=%u",
               ip_ms(gd_t0, now) / 1000.0, b, gd_lost, st_vbg_copy, st_vbg_copy ? st_vbg_mb / st_vbg_copy : 0.0, st_vbg_fail,
               gc_n ? gc_ms / gc_n : 0.0, gc_mx, gc_n, st_vbg_async);
    }
    gc_ms = gc_mx = 0.0; gc_n = 0;
    memset(gd_ms, 0, sizeof gd_ms); memset(gd_dr, 0, sizeof gd_dr); memset(gd_vb, 0, sizeof gd_vb);
    memset(gd_mx, 0, sizeof gd_mx); memset(gd_n, 0, sizeof gd_n);
    gd_lost = 0; st_vbg_copy = 0; st_vbg_mb = 0.0; st_vbg_fail = 0;
    gd_t0 = now;
}

static void gd_poll(void)
{
    int k;
    for (k = 0; k < GD_N; k++) {
        gd_slot_t *g = &gd_s[k];
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 t0, t1;
        double ms;
        int vi;
        if (g->st != 2) { continue; }
        if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)g->dj, &dj, sizeof dj,
                                        D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) { continue; }
        g->st = 0;
        if (dj.Disjoint || !dj.Frequency ||
            ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)g->t0, &t0, sizeof t0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)g->t1, &t1, sizeof t1, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            t1 < t0) {
            gd_lost++;
            continue;
        }
        ms = (double)(t1 - t0) * 1000.0 / (double)dj.Frequency;
        vi = gd_vi(g->rc.var);
        gd_ms[vi] += ms; gd_n[vi]++;
        gd_dr[vi] += g->rc.ndraw;
        gd_vb[vi] += (double)g->rc.vbytes / 1048576.0;
        if (ms > gd_mx[vi]) { gd_mx[vi] = ms; }
        if (!gd_f) {
            char dir[600], path[700];
            DWORD n = GetEnvironmentVariableA("SB_OUT_DIR", dir, sizeof dir);
            if (!n || n >= sizeof dir) { strcpy_s(dir, sizeof dir, "."); }
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s/ipgpu.csv", dir);
            gd_f = _fsopen(path, "w", _SH_DENYNO); /* mitlesbar waehrend des Laufs */
            if (!gd_f) { gd_fail = 1; HLOG_W("IPGPU: %s nicht schreibbar", path); }
            else {
                fprintf(gd_f, "ser,t,var,ms,nops,ndraw,nlerp,nrq,nclr,nverts,vb_mb,rqx_kb,nmap,ncb,slots,nrtv,nsrv,ring,vbg,qpc_s\n");
                HLOG_I("IPGPU: Messung je Wiedergabe nach %s (ip_gdiag=%d, ip_ablate=%d, ip_cb_ring=%d, ip_vb_gpu=%d)", path,
                       g_cfg.ip_gdiag, g_cfg.ip_ablate, g_cfg.ip_cb_ring, g_cfg.ip_vb_gpu);
            }
        }
        if (gd_f) {
            fprintf(gd_f, "%u,%.3f,%u,%.4f,%u,%u,%u,%u,%u,%u,%.2f,%u,%u,%u,%u,%u,%u,%u,%u,%.4f\n", g->ser, g->t, g->rc.var, ms,
                    g->rc.nops, g->rc.ndraw, g->rc.nlerp, g->rc.nrq, g->rc.nclr, g->rc.nverts,
                    (double)g->rc.vbytes / 1048576.0, g->rc.rqxb / 1024u, g->rc.nmap, g->rc.ncb, g->rc.slots,
                    g->rc.nrtv, g->rc.nsrv, g->rc.ring, g->rc.vbg, (double)ip_now() / (double)ip_qpf.QuadPart);
        }
    }
    gd_log(0);
}

static int gd_begin(void)
{
    int k;
    D3D11_QUERY_DESC qd;
    if (g_cfg.ip_gdiag <= 0 || gd_fail) { return -1; }
    gd_poll();
    if ((++gd_cnt % (unsigned)g_cfg.ip_gdiag) != 0u) { return -1; }
    for (k = 0; k < GD_N; k++) { if (gd_s[k].st == 0) { break; } }
    if (k >= GD_N) { gd_lost++; return -1; }
    if (!gd_s[k].dj) {
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &gd_s[k].dj))) { gd_fail = 1; return -1; }
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &gd_s[k].t0)) ||
            FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &gd_s[k].t1))) { gd_fail = 1; return -1; }
    }
    ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous *)gd_s[k].dj);
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)gd_s[k].t0);
    return k;
}

static void gd_end(int k, const ip_rc_t *rc, unsigned ser, float t)
{
    gd_slot_t *g = &gd_s[k];
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)g->t1);
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)g->dj);
    g->rc = *rc;
    g->ser = ser;
    g->t = t;
    g->st = 2;
}

/* DIAG(W251 INTERP-GROUND): Draw-ID-Bild des Bildes (A, B, t) - Ziele mit den meisten
 * RT-Draws (Welt) und BB als BMP (ipid_<f>_<t>_e<k>), danach das gezeigte Bild wiederherstellen. */
static void ip_tex_ppm(const char *name, ID3D11Texture2D *tex) /* DIAG(W251): Schatten-Ziel roh als PPM (Bytes 0..2 je Pixel) */
{
    D3D11_TEXTURE2D_DESC dd;
    ID3D11Texture2D *stg = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    char path[600];
    FILE *f;
    unsigned y, x;
    ID3D11Texture2D_GetDesc(tex, &dd);
    dd.MipLevels = 1; dd.ArraySize = 1; dd.SampleDesc.Count = 1; dd.SampleDesc.Quality = 0;
    dd.Usage = D3D11_USAGE_STAGING; dd.BindFlags = 0; dd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; dd.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &stg))) { return; }
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)stg, 0, 0, 0, 0, (ID3D11Resource *)tex, 0, NULL);
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
        sprintf_s(path, sizeof path, "%s/shots/%s_f%u.ppm", hcfg_out_dir(), name, (unsigned)dd.Format);
        if (fopen_s(&f, path, "wb") == 0 && f) {
            fprintf(f, "P6\n%u %u\n255\n", dd.Width, dd.Height);
            for (y = 0; y < dd.Height; y++) {
                const unsigned char *r = (const unsigned char *)m.pData + (size_t)y * m.RowPitch;
                for (x = 0; x < dd.Width; x++) { fwrite(r + 4u * x, 1, 3, f); }
            }
            fclose(f);
        }
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)stg, 0);
    }
    ID3D11Texture2D_Release(stg);
}

static void ip_id_dump(ip_list_t *A, int ai, ip_list_t *B, int bi, float t)
{
    unsigned cnt[IP_SH_MAX], i;
    int e, top[2] = { -1, -1 }, jpg = g_cfg.screenshot_jpg, k;
    char nm[64];
    memset(cnt, 0, sizeof cnt);
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        if (o->kind == IP_DRAW && o->rtv != g_rtv && (e = ip_sh_of_rtv(o->sh_rtv)) >= 0) { cnt[e]++; }
    }
    for (e = 0; e < IP_SH_MAX; e++) {
        if (!cnt[e]) { continue; }
        if (top[0] < 0 || cnt[e] > cnt[top[0]]) { top[1] = top[0]; top[0] = e; }
        else if (top[1] < 0 || cnt[e] > cnt[top[1]]) { top[1] = e; }
    }
    ip_replay_ex(B, bi, A, ai, t, 2, ip_cx_imm());
    g_cfg.screenshot_jpg = 0;
    for (k = 0; k < 2; k++) {
        if (top[k] < 0 || !g_ish[top[k]].sh) { continue; }
        _snprintf_s(nm, sizeof nm, _TRUNCATE, "ipid_%06u_%03u_e%d", B->frame, (unsigned)(t * 1000.0f + 0.5f), top[k]);
        ip_tex_ppm(nm, g_ish[top[k]].sh);
    }
    _snprintf_s(nm, sizeof nm, _TRUNCATE, "ipid_%06u_%03u_bb", B->frame, (unsigned)(t * 1000.0f + 0.5f));
    sb_render_shot_name(nm, (ID3D11Resource *)g_ish[B->bbref].sh);
    g_cfg.screenshot_jpg = jpg;
    HLOG_I("INTERP ddid f=%u t=%.3f e=%d(%u) e2=%d(%u) bb=%d", B->frame, (double)t, top[0], top[0] >= 0 ? cnt[top[0]] : 0u,
           top[1], top[1] >= 0 ? cnt[top[1]] : 0u, B->bbref);
    if (t < 1.0f && A) { ip_replay(B, bi, A, ai, t); } /* gezeigtes Bild wiederherstellen */
    else { ip_replay(B, bi, A, ai, 1.0f); }            /* Tick-Bild (nur Schatten, gezeigt wird bak) */
    for (k = 0; k < 2; k++) {
        if (top[k] < 0 || !g_ish[top[k]].sh) { continue; }
        _snprintf_s(nm, sizeof nm, _TRUNCATE, "iprt_%06u_%03u_e%d", B->frame, (unsigned)(t * 1000.0f + 0.5f), top[k]);
        ip_tex_ppm(nm, g_ish[top[k]].sh);
    }
}

/* Bild zwischen A und B zeigen: t >= 1 (oder ohne A) -> gesichertes Spiel-Bild
 * von B; sonst Wiedergabe in die Schatten. tag = Name fuer Bildfolgen. */
/* W323 LOC-INTERP: Zaehler der gezeigten Bilder im LoC-Profil (Zeile LOCINTERP alle 10 s, pd_stat_log). */
static unsigned li_mid, li_exact, li_same, li_ticks, li_dev2, li_kept;
static double li_dev_max; /* Abweichung des Zustandsabstands vom Spieltakt (ms): Anker-Guete */
static unsigned li_last_ser;
static float li_last_t = -1.0f;
static void li_note(const ip_list_t *A, const ip_list_t *B, float t)
{
    float tt = (t >= 1.0f || !A) ? 1.0f : t;
    if (tt >= 1.0f) { li_exact++; } else { li_mid++; }
    if (B->serial == li_last_ser && tt == li_last_t) { li_same++; } /* Wiederhol-Present: dasselbe Bild erneut */
    li_last_ser = B->serial;
    li_last_t = tt;
}

/* DIAG(W349 LOC-FRAMES-2) loc_interp_dbg Bit 4: nach Zwischenbild-Wiedergaben in den Bildfolgen (pf_ring) die kleinen
 * Render-Ziel-Schatten (<= 256 px, Glow-Kette) roh als PPM ablegen (12 Bilder): folgen sie t oder bleiben sie stehen? */
static void li_sh_dump(const ip_list_t *B, float t)
{
    static unsigned n;
    int j;
    char nm[80];
    int big = (g_cfg.loc_interp_dbg & 8) != 0; /* Bit 8: nur Ziele 257..512 px (Welt-RT), 24 Bilder */
    if (n >= (big ? 24u : 12u)) { return; }
    n++;
    for (j = 0; j < B->nrefs; j++) {
        int e = B->refs[j];
        D3D11_TEXTURE2D_DESC dd;
        if (!g_ish[e].sh) { continue; }
        ID3D11Texture2D_GetDesc(g_ish[e].sh, &dd);
        if ((big ? (dd.Width <= 256u || dd.Width > 512u) : dd.Width > 256u) || !(dd.BindFlags & D3D11_BIND_RENDER_TARGET) ||
            dd.SampleDesc.Count != 1) { continue; }
        _snprintf_s(nm, sizeof nm, _TRUNCATE, "lish_%06u_%03u_e%02d_%ux%u", B->frame, (unsigned)(t * 1000.0f + 0.5f), e, dd.Width, dd.Height);
        ip_tex_ppm(nm, g_ish[e].sh);
    }
}

static void ip_show(ip_list_t *A, ip_list_t *B, float t, const char *tag)
{
    int bi = ip_list_index(B), ai = A ? ip_list_index(A) : -1;
    LONGLONG t0 = ip_now(), t1, tph = t0; /* tph: W122 PERF-HUD */
    ID3D11Resource *img;
    double ms;
    int gpm = 0; /* DIAG(GPU-COST,W146) perf_gpass: Messung dieses Bildes */
    if (B->bbref < 0) { return; }
    if ((g_cfg.reproj_opt & 16) && A && t < 1.0f) { t = 0.001f; } /* W295 DIAG: Zwischenbild ~ Tick N-1 (Pruefmass Bewegung) */
    if (g_cfg.vr_enable && sb_vr_preview_active() && sb_vr_preview_show(A, B, t)) {
        /* W134 VR-PROTO: Desktop-Vorschau (beide Augen bzw. ein Auge aus derselben Aufzeichnung) */
        t1 = ip_now();
        ms = ip_ms(t0, t1);
        st_rep_ms += ms; st_rep_n++;
        if (ms > st_rep_max) { st_rep_max = ms; }
        if (t >= 1.0f || !A) { st_exact++; }
        t0 = ip_now();
        if (GetCurrentThreadId() == g_pd_tid) { g_phud_src = 1; } /* W138 perf_trace */
        sb_render_present_raw();
        g_phud_src = 0;
        ip_ev_mark();
        t1 = ip_now();
        st_pres_ms += ip_ms(t0, t1);
        if (ip_tlast_present && t1 - ip_tlast_present > ip_dt * 3 / 2) { st_late++; }
        ip_tlast_present = t1;
        st_presents++;
        (void)bi; (void)ai; (void)tph;
        return;
    }
    if (t >= 1.0f || !A || (g_cfg.vr_enable && sb_vr_window_mirrors())) {
        /* W183 VR-FIX3: zeigt das Fenster den VR-Spiegel, entfaellt die Mono-Wiedergabe (sb_render_blit
         * kopiert dann nicht, sondern zeigt das Augenbild) */
        img = (ID3D11Resource *)g_ish[B->bbref].bak[bi];
        st_exact++;
    } else {
        int gq, gd;
        ID3D11CommandList *cl = ip_rt_active() ? ip_rt_get(A, B, t) : NULL; /* W126 RENDER-THREAD */
        if (!cl && ip_dead_n) { ip_dead_purge(); return; } /* W361: tote Schatten-View - dieses Bild entfaellt */
        if (!cl) { gpm = sb_gp_begin(0); } /* DIAG(GPU-COST,W146) */
        dy_begin(); /* W146 rt_scale_dyn */
        gq = ip_gpu_begin();
        gd = gd_begin(); /* DIAG(PERF-M2,W174) */
        if (cl) { ip_rt_exec(cl); }
        else { ip_replay(B, bi, A, ai, t); }
        if (gd >= 0) { gd_end(gd, cl ? &ip_rc_got : &ip_rc_ctx[0], B->serial, t); }
        if (gq) { ip_gpu_end(); }
        img = (ID3D11Resource *)g_ish[B->bbref].sh;
        t1 = ip_now();
        ms = ip_ms(t0, t1);
        st_rep_ms += ms;
        st_rep_n++;
        if (ms > st_rep_max) { st_rep_max = ms; }
    }
    if (tag && ip_shot_ser && B->serial == ip_shot_ser) {
        char nm[64];
        _snprintf_s(nm, sizeof nm, _TRUNCATE, "ip_%06u_%s", B->frame, tag);
        sb_render_shot_name(nm, img);
        if (g_cfg.interp_diag >= 3 && A && t < 1.0f && ip_dbg_ps) {
            /* DIAG: Klassifikationsbild (Farbe je Zuordnungs-Ergebnis) */
            ip_replay_ex(B, bi, A, ai, t, 1, ip_cx_imm());
            _snprintf_s(nm, sizeof nm, _TRUNCATE, "ipc_%06u_%s", B->frame, tag);
            sb_render_shot_name(nm, img);
            ip_replay(B, bi, A, ai, t); /* gezeigtes Bild wiederherstellen */
        }
    }
    if (((ip_dd_ser && B->serial >= ip_dd_lo && B->serial <= ip_dd_ser) || B->ddsel) && ip_id_ps && A) { ip_id_dump(A, ai, B, bi, t); } /* DIAG(W251, W284) */
    if ((g_cfg.loc_interp_dbg & 12) && g_sba_loc && A && t < 1.0f && sb_pf_active()) { li_sh_dump(B, t); } /* DIAG(W349) */
    sb_render_blit(img);
    if (gpm) { sb_gp_end(); } /* DIAG(GPU-COST,W146) */
    dy_end(); /* W146 rt_scale_dyn */
    sb_pf_note((t >= 1.0f || !A) ? 1.0f : t, B->frame); /* DIAG(POPIN,W91): pf_ring */
    if (g_sba_loc) { li_note(A, B, t); } /* W323 */
    t0 = ip_now();
    ip_tlast_present_pre = t0; /* DIAG(W393) pace_diag */
    InterlockedIncrement(&pv_presents); /* W393: Bilder je VBlank-Intervall */
    if (GetCurrentThreadId() == g_pd_tid) { g_phud_src = 1; } /* W138 perf_trace */
    sb_render_present_raw();
    g_phud_src = 0;
    ip_ev_mark(); /* PERF(TICK-PERF,W125) interp_gpu_q */
    t1 = ip_now();
    st_pres_ms += ip_ms(t0, t1);
    if (ip_tlast_present && t1 - ip_tlast_present > ip_dt * 3 / 2) { st_late++; }
    ip_tlast_present = t1;
    st_presents++;
    if (g_phud_meas && GetCurrentThreadId() != g_pd_tid) { sb_phud_interp_q(ip_now() - tph, ip_in_rec ? 1 : (g_phud_in_wait ? 2 : 0)); } /* W122 PERF-HUD: Interp-CPU (Wiedergabe + Present); W138: nicht aus dem Takt-Thread */
}

/* PERF(TICK-PERF,W125) interp_gpu_q=N: Event-Query nach jedem Present; ein
 * Zwischenbild (Wiedergabe t<1) wird nur eingereiht, wenn die GPU das N-letzte
 * gezeigte Bild fertig hat. Ursache (W125_b2/b3): bei GPU-Last stapeln sich
 * Wiedergaben vor den synchronen Stellen des Spiel-Ticks (Sichtbarkeitstest
 * GetVisibilityTestResult wartet blockierend wie das Original, visq 7-29 ms/
 * Tick) -> Tick-Last > 33 ms, Spiel < 30 Ticks/s. Mit der Grenze verfaellt
 * dann ein Zwischenbild statt dass der Tick wartet. */
#define IP_EV_N 8
static ID3D11Query *ip_ev[IP_EV_N];
static unsigned ip_ev_n;
static double ip_r_last_ms; /* Rechenzeit des letzten Ticks (Feldwechsel -> Swap) */
unsigned g_ip_gskip; /* DIAG(TICK-PERF,W125): wegen GPU-Rueckstau verfallene Zwischenbilder */
unsigned g_ip_gtight; /* DIAG(TICK-PERF,W125): Pruefungen mit verschaerfter Grenze */

static void ip_ev_mark(void)
{
    ID3D11Query **q;
    if (g_cfg.interp_gpu_q <= 0) { return; }
    q = &ip_ev[ip_ev_n % IP_EV_N];
    if (!*q) {
        D3D11_QUERY_DESC qd;
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_EVENT;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, q))) { *q = NULL; return; }
    }
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)*q);
    ip_ev_n++;
}

static int ip_gpu_behind(void)
{
    unsigned f = (unsigned)g_cfg.interp_gpu_q;
    BOOL done = FALSE;
    HRESULT hr;
    ID3D11Query *q;
    if (g_cfg.interp_gpu_q <= 0) { return 0; }
    /* interp_gpu_tight=T: war der letzte Tick laenger als T ms, gilt Grenze 1
     * (nur Bilder einreihen, wenn die GPU alles Gezeigte fertig hat). */
    {
        /* PERF(PERF-SPIKE,W318) interp_gpu_tight_imm: ohne Render-Thread (render_thread=0, AMD-Weg) gilt die
         * Grenze 1 ab T ms Tick (Standard 30), wenn interp_gpu_tight nicht gesetzt ist - zusammen mit
         * interp_bsets_imm bleibt die GPU-Warteschlange flach und der Takt-Thread blockiert nicht unter
         * ctx_lock (W318_v2 gegen g50r0: Spiel wartet 0,2 statt 2,1 s je 10 s, 30,0 statt 25,9 Ticks/s) */
        int tight = g_cfg.interp_gpu_tight;
        if (tight <= 0 && g_cfg.render_thread <= 0) { tight = g_cfg.interp_gpu_tight_imm; }
        if (f > 1u && tight > 0 && ip_r_last_ms > (double)tight) { f = 1u; g_ip_gtight++; }
    }
    if (ip_ev_n < f) { return 0; }
    if (f > IP_EV_N - 1u) { f = IP_EV_N - 1u; }
    q = ip_ev[(ip_ev_n - f) % IP_EV_N];
    if (!q) { return 0; }
    hr = ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)q, &done, sizeof done,
                                     D3D11_ASYNC_GETDATA_DONOTFLUSH);
    return (hr == S_FALSE) ? 1 : 0;
}

/* Paar fuer die Faelligkeit due: tstate(A) <= due - L < tstate(B) (Historie:
 * aelteste zuerst); hold = juengster Tick exakt halten (naechster fehlt). */
static void ip_pair_at(LONGLONG due, ip_list_t **pA, ip_list_t **pB, float *pt, int *phold)
{
    LONGLONG v = due - ip_lat_at(due); /* W393: stetige Bilduhr (aus: ip_lat) */
    ip_list_t *A = NULL, *B;
    float t = 1.0f;
    int i;
    *phold = 0;
    B = &g_ipl[ip_hist[ip_nhist - 1]];
    if (v >= B->tstate) {
        *phold = 1; /* naechster Tick noch nicht da: juengsten exakt halten */
    } else {
        for (i = ip_nhist - 1; i >= 1; i--) {
            ip_list_t *a = &g_ipl[ip_hist[i - 1]], *b = &g_ipl[ip_hist[i]];
            if (v >= a->tstate) {
                A = a;
                B = b;
                t = (float)((double)(v - a->tstate) / (double)(b->tstate - a->tstate));
                break;
            }
        }
        if (!A) { B = &g_ipl[ip_hist[0]]; t = 1.0f; }
    }
    *pA = A;
    *pB = B;
    *pt = t;
}

/* Echtzeit-Takt: faelliges Bild zeigen (Zustand zur Zeit tau - L). */
static int pd_owns_clock(void); /* W138 PERF-FIX */
/* ---------- W393 LOC-PACING pace_vsync: Zwischenbild-Takt an den VBlank koppeln ----------
 * Mit present_interval=1 (VSync an) liefen die Zwischenbilder bisher mit dem Timer (Present-Intervall 0): Bei
 * interp_fps = Monitorfrequenz wandert die Phase Timer<->VBlank langsam, liegt sie am VBlank, fallen zwei Bilder in
 * ein Komponier-Intervall (eins nie gezeigt, eins doppelt). Ein eigener Thread wartet auf den VBlank (DXGI) und
 * schaetzt Periode und Phase; die Faelligkeiten liegen dann auf dem Raster VBlank + Periode/2 (Mitte zwischen zwei
 * VBlanks, weit weg vom Komponier-Zeitpunkt), alle k VBlanks (k = Monitor/interp_fps, nur ganzzahlig +-3 %).
 * Present selbst bleibt Intervall 0 (blockiert nie unter ctx_lock). Ohne Kopplung zaehlt der Thread nur (pace_diag). */
static HANDLE pv_th;
static volatile LONGLONG pv_last, pv_per; /* letzter VBlank, Periode (QPC, geglaettet) */
static volatile LONG pv_n0, pv_n1, pv_n2, pv_snap_n;
static LONGLONG pv_st_d, pv_of_d; /* DIAG: Raster fuer die PACE-Zeile */
static int pv_lock_want(void)
{
    if (g_cfg.pace_vsync == 3) { return g_sba_loc && !sb_render_exclusive(); } /* auch VSync aus, nur im Fenster (DWM) */
    return g_cfg.present_interval == 1 && (g_cfg.pace_vsync == 2 || (g_cfg.pace_vsync == 1 && g_sba_loc));
}
/* Raster der Kopplung (pace_vb_grid), 0 = keine (Periode unbekannt, Thread steht oder nicht ganzzahlig) */
static int pv_lock_grid(LONGLONG *step, LONGLONG *off)
{
    LONGLONG per = InterlockedCompareExchange64(&pv_per, 0, 0);
    if (!pv_lock_want() || per <= 0 || g_cfg.interp_fps <= 0) { return 0; }
    if (ip_now() - InterlockedCompareExchange64(&pv_last, 0, 0) > ip_qpf.QuadPart / 4) { return 0; } /* Thread steht */
    return pace_vb_grid((double)g_cfg.interp_fps, per, ip_qpf.QuadPart, step, off);
}
/* Faelligkeit t auf den naechstgelegenen Rasterpunkt legen. */
static LONGLONG pv_snap(LONGLONG t, LONGLONG step, LONGLONG off)
{
    LONGLONG vb = InterlockedCompareExchange64(&pv_last, 0, 0);
    InterlockedIncrement(&pv_snap_n);
    return (LONGLONG)pace_vb_next(t - step / 2, vb, step, off);
}
static DWORD WINAPI pv_main(LPVOID p)
{
    LONGLONG last = 0;
    (void)p;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    for (;;) {
        LONGLONG q;
        if (!(pv_lock_want() || g_cfg.pace_diag) || !sb_pd_on() || !ip_live) { last = 0; Sleep(50); continue; }
        if (!sb_render_vblank_wait()) { last = 0; Sleep(50); continue; }
        q = ip_now();
        if (last) {
            LONGLONG d = q - last, per = pv_per;
            /* 30..400 Hz; Ausreisser (verpasster VBlank) nicht in die Periode */
            if (d > ip_qpf.QuadPart / 400 && d < ip_qpf.QuadPart / 30) {
                if (per <= 0 || d < per * 3 / 2) {
                    per = per > 0 ? per + (d - per) / 16 : d;
                    InterlockedExchange64(&pv_per, per);
                }
            }
            { LONG c = InterlockedExchange(&pv_presents, 0);
              if (c <= 0) { InterlockedIncrement(&pv_n0); } else if (c == 1) { InterlockedIncrement(&pv_n1); } else { InterlockedIncrement(&pv_n2); } }
        } else {
            InterlockedExchange(&pv_presents, 0);
        }
        InterlockedExchange64(&pv_last, q);
        last = q;
    }
}

/* ---------- DIAG(W393 LOC-PACING) pace_diag: Bildtakt je 10 s (Zeile PACE) ----------
 * Je gezeigtem Zwischenbild: Abstand der Presents (QPC vor Present), gezeigte Spielzeit g = Listen-Nummer von A +
 * t (in Ticks), Phase der Faelligkeit im Tick, Latenz-Wechsel. Nur Zaehler/Zeiten, keine Eingaben. */
#define PC_N 4096
static float pc_ft[PC_N], pc_dg[PC_N];
static int pc_nft, pc_ndg;
static LONGLONG pc_last_q, pc_last_lat;
static double pc_last_g = -1.0;
static unsigned pc_kept, pc_latchg, pc_ph[12], pc_skip0, pc_lockw1, pc_ts_s, pc_ts_snap, pc_ts_big;
static double pc_latchg_max, pc_lockw_max;
static void pc_note(const ip_list_t *A, const ip_list_t *B, float t, LONGLONG due)
{
    double g = (A && t < 1.0f) ? (double)A->serial + (double)t * (double)(B->serial - A->serial) : (double)B->serial;
    LONGLONG q = ip_tlast_present_pre, lat = ip_lat_at(due);
    if (pc_last_q && pc_nft < PC_N) { pc_ft[pc_nft++] = (float)ip_ms(pc_last_q, q); }
    if (pc_last_g >= 0.0 && pc_ndg < PC_N && g >= pc_last_g - 8.0) { pc_dg[pc_ndg++] = (float)(g - pc_last_g); }
    if (pc_last_lat && lat != pc_last_lat) {
        double d = fabs(ip_ms(pc_last_lat, lat));
        if (d > 0.5) { pc_latchg++; }
        if (d > pc_latchg_max) { pc_latchg_max = d; }
    }
    if (B && ip_tick_hz() > 0) { /* Phase der Faelligkeit gegen die Zustandszeit des juengsten Ticks */
        const ip_list_t *N = &g_ipl[ip_hist[ip_nhist - 1]];
        pc_ph[pace_phase_bin(due, N->tstate, ip_qpf.QuadPart / ip_tick_hz(), 12)]++;
    }
    pc_last_q = q;
    pc_last_g = g;
    pc_last_lat = lat;
}

static void ip_poll(void)
{
    LONGLONG now;
    ip_list_t *A = NULL, *B = NULL;
    float t = 1.0f;
    int hold;
    char tag[16];
    if (!ip_live || ip_in_poll || ip_nhist < 1) { return; }
    if (pd_owns_clock()) { return; } /* W138 present_decouple: Bilder zeigt nur der Takt-Thread */
    now = ip_now();
    if (now < ip_tnext) { return; }
    ip_in_poll = 1;
    ip_pair_at(ip_tnext, &A, &B, &t, &hold);
    if (hold) { st_hold++; ip_lat_hold(ip_tnext, B); } /* W393: elastische Bilduhr */
    if (g_sba_loc && (hold || !A || t >= 1.0f) && B->serial == li_last_ser && li_last_t >= 1.0f) {
        /* W323 LOC-INTERP: naechster Tick noch nicht da und das juengste Bild steht schon exakt -> nicht erneut
         * praesentieren (keine Wiederhol-Presents im LoC-Profil, W320); das Fenster zeigt es ohnehin weiter. */
        li_kept++;
        pc_kept++; /* DIAG(W393) */
        ip_tnext += ip_dt;
        now = ip_now();
        while (ip_tnext <= now) { ip_tnext += ip_dt; st_skip++; g_ip_skip_tot++; }
        ip_in_poll = 0;
        return;
    }
    _snprintf_s(tag, sizeof tag, _TRUNCATE, "t%03u", (unsigned)(t * 1000.0f + 0.5f));
    sb_ctx_enter();
    if (A && t < 1.0f && ip_gpu_behind()) { /* PERF(TICK-PERF,W125): GPU hinterher -> Zwischenbild verfaellt */
        sb_ctx_leave();
        g_ip_gskip++; st_skip++; g_ip_skip_tot++;
        ip_tnext += ip_dt;
        now = ip_now();
        while (ip_tnext <= now) { ip_tnext += ip_dt; st_skip++; g_ip_skip_tot++; }
        ip_rt_prefetch(); /* W126 RENDER-THREAD */
        ip_in_poll = 0;
        return;
    }
    { unsigned np0 = st_presents;
    ip_show(A, B, t, tag);
    if (st_presents != np0) {
        ip_lat_commit(ip_tnext); /* W393 */
        if (g_cfg.pace_diag) { pc_note(A, B, t, ip_tnext); } /* DIAG(W393) */
    } }
    sb_ctx_leave();
    ip_tnext += ip_dt;
    now = ip_now();
    while (ip_tnext <= now) { ip_tnext += ip_dt; st_skip++; g_ip_skip_tot++; }
    { LONGLONG st, of; if (pv_lock_grid(&st, &of)) { ip_tnext = pv_snap(ip_tnext, st, of); } } /* W393 pace_vsync */
    ip_rt_prefetch(); /* W126 RENDER-THREAD: naechstes Bild vorbereiten */
    ip_in_poll = 0;
}


/* ---------- PERF(PERF-FIX,W138): Present-Takt-Thread (present_decouple=1) ----------
 * Bisher zeigte nur der Spiel-Thread Bilder: Zwischenbilder aus den Hooks
 * (Feld-Poll, rec_draw/rec_clear, Swap), Menues/Filme/Ladebilder am Present
 * des Spiels. Steckt der Spiel-Thread in Spielcode ohne Hook (Spiellogik
 * ~11 ms jeden 2. Tick, Ladecode 113-120 ms), stand die Ausgabe (W137 §3).
 * Der Takt-Thread (Host-Thread, kein Xbox-Thread, wird nie suspendiert,
 * haelt ausser ctx keine Sperre) wartet mit hochaufloesendem Timer auf die
 * naechste Faelligkeit und zeigt dann unter ctx_lock (Vortritt g_pd_want):
 *   - Interp-Takt aktiv (Mission, Film): das faellige Bild wie bisher ip_poll
 *     (gleiche Paarwahl ip_pair_at, gleiche Wiedergabe, gleicher Blit); der
 *     Spiel-Thread zeigt selbst keine Zwischenbilder mehr.
 *   - sonst: liegt das letzte echte Present laenger als 1/interp_fps + 0,5 ms
 *     zurueck, den Swapchain-BB erneut (Bild unveraendert, nur keine
 *     eingefrorene Ausgabe). Das Spiel-Bild selbst bleibt unberuehrt.
 * Nur Echtzeit (vblank_hz>0) mit Offscreen-BB (interp_fps>0), nicht mit VR.
 * Testtakt (Golden, interp_verify im Testtakt): Thread ohne Wirkung. */
static HANDLE pd_th, pd_timer;
static DWORD pd_tid_l;
static volatile LONG pd_started; /* 0 = nie, 1 = laeuft, 2 = Start fehlgeschlagen */
static LONGLONG pd_deadline;
static unsigned pd_n_ip, pd_n_hold, pd_n_late, pd_n_lockw, pd_n_iter, pd_n_gw, pd_n_gw_ok;
static unsigned pd_n_rbwait, pd_n_rbskip; /* W390: Ruhe (Vorlauf/Neurechnen) abgewartet bzw. Bild ausgelassen */
static double pd_lockw_ms, pd_lockw_max, pd_late_max, pd_wake_err_max, pd_gw_ms;
static LONGLONG pd_stat_q;

extern volatile LONG g_sb_paused; /* sb_pause.c (W188) */
int sb_pd_on(void)
{
    /* W188 GAME-MENU: in der Pause zeigt die Pause-Schleife die Bilder (Takt-Thread ruht) */
    return pd_started == 1 && g_cfg.present_decouple > 0 && g_cfg.vblank_hz > 0 && !g_cfg.vr_enable &&
           g_cfg.interp_fps > 0 && sb_render_offscreen() && !g_sb_paused;
}

static int pd_owns_clock(void)
{
    return pd_started == 1 && GetCurrentThreadId() != g_pd_tid && sb_pd_on();
}

void *sb_iml_holder_site(void);                 /* DIAG(LOC-IM-LOCK), d3d11_render.c */
void sb_iml_pd_wait(LONGLONG dq, void *site);
void sb_iml_log(double secs);
static void isw_log(double secs);
static double pd_hold_ms, pd_hold_max; /* DIAG(PERF-TICK,W168) ctx_wait_diag */
static unsigned pd_hold_n05;
static void pd_stat_log(LONGLONG now)
{
    double secs = pd_stat_q ? ip_ms(pd_stat_q, now) / 1000.0 : 0.0;
    if (pd_stat_q && secs < 10.0) { return; }
    if (pd_stat_q) {
        HLOG_I("PRESENT-TAKT: %.1fs zwischenbilder=%u wiederholt=%u spaet(>1ms)=%u max_spaet=%.2fms "
               "sperre_gewartet=%u (%.3fms mittel, max %.2fms) weckfehler_max=%.2fms runden=%u live=%d "
               "gpu_gewartet=%u (danach gezeigt %u, %.2fms mittel)",
               secs, pd_n_ip, pd_n_hold, pd_n_late, pd_late_max, pd_n_lockw,
               pd_n_lockw ? pd_lockw_ms / pd_n_lockw : 0.0, pd_lockw_max, pd_wake_err_max, pd_n_iter, ip_live,
               pd_n_gw, pd_n_gw_ok, pd_n_gw ? pd_gw_ms / pd_n_gw : 0.0);
    }
    if (pd_stat_q && g_cfg.pace_diag) { /* DIAG(W393 LOC-PACING): eigene Zeile, nur Zaehler und Zeiten */
        double soll = 1000.0 / (double)(g_cfg.interp_fps > 0 ? g_cfg.interp_fps : 120);
        pace_ft_t f = pace_ft_sum(pc_ft, pc_nft, soll);
        pace_g_t gg = pace_g_sum(pc_dg, pc_ndg);
        unsigned k, phm = 0, pht = 0, sk = g_ip_skip_tot - pc_skip0;
        for (k = 0; k < 12u; k++) { pht += pc_ph[k]; if (pc_ph[k] > phm) { phm = pc_ph[k]; } }
        HLOG_I("PACE: %.1fs fps=%d bilder=%d abstand p50=%.2f p99=%.2f max=%.2fms ausserhalb1ms=%d (%.2f%%) "
               "ausgelassen=%u (%.1f/min) ruhe_ausgelassen=%u ruhe_gewartet=%u doppelt=%d gehalten=%u g_med=%.4f g_abw99=%.1f%% "
               "spruenge=%d L=%.1fms L_ziel=%.1fms L_wechsel=%u (max %.1fms) sperre>1ms=%u (max %.2fms) phase_max=%.0f%% uhr=%d vorlauf_frei=%d "
               "vblank=%.2fHz gekoppelt=%d vb_bilder0/1/2+=%ld/%ld/%ld tick_glatt=%u (abw>1/8 %u) tick_neu=%u gedehnt=%u",
               secs, g_cfg.interp_fps, f.n + (f.n ? 1 : 0), f.p50, f.p99, f.mx, f.out1, f.n ? 100.0 * f.out1 / f.n : 0.0,
               sk, sk * 60.0 / secs, pd_n_rbskip, pd_n_rbwait, gg.same, pc_kept, gg.med, gg.dev99 * 100.0, gg.jump,
               (double)ip_lat_at(ip_tnext) * 1000.0 / (double)ip_qpf.QuadPart, (double)ip_lat * 1000.0 / (double)ip_qpf.QuadPart,
               pc_latchg, pc_latchg_max, pc_lockw1, pc_lockw_max, pht ? 100.0 * phm / pht : 0.0, ip_pace_on(), g_cfg.loc_pd_spec,
               pv_per > 0 ? (double)ip_qpf.QuadPart / (double)pv_per : 0.0, pv_lock_grid(&pv_st_d, &pv_of_d), pv_n0, pv_n1, pv_n2, pc_ts_s, pc_ts_big, pc_ts_snap, pc_elastic);
        pc_ts_s = pc_ts_big = pc_ts_snap = pc_elastic = 0;
        InterlockedExchange(&pv_n0, 0); InterlockedExchange(&pv_n1, 0); InterlockedExchange(&pv_n2, 0);
        pc_nft = pc_ndg = 0;
        pc_kept = pc_latchg = pc_lockw1 = 0;
        pc_latchg_max = pc_lockw_max = 0.0;
        memset(pc_ph, 0, sizeof pc_ph);
        pc_skip0 = g_ip_skip_tot;
    }
    if (pd_stat_q && g_cfg.imlock_diag) { sb_iml_log(secs); sb_ctx_enter(); isw_log(secs); sb_ctx_leave(); } /* DIAG(LOC-IM-LOCK) */
    if (pd_stat_q && g_sba_loc && (pd_n_rbwait || pd_n_rbskip)) { /* W390: eigene Zeile (PRESENT-TAKT wird geparst) */
        HLOG_I("PRESENT-RUHE: %.1fs Ruhe abgewartet=%u Bild ausgelassen=%u", secs, pd_n_rbwait, pd_n_rbskip);
    }
    if (pd_stat_q && g_cfg.ctx_wait_diag) { /* DIAG(PERF-TICK,W168) */
        extern volatile LONGLONG g_ctxw_q, g_ctxw_tick_max;
        extern volatile LONG g_ctxw_n;
        HLOG_I("CTXWAIT %.1fs: spiel_wartet=%.1fms (n=%ld, max je Tick %.2fms) takt_haelt=%.3fms mittel (max %.2fms, >0.5ms: %u) runden=%u",
               secs, (double)g_ctxw_q * 1000.0 / (double)ip_qpf.QuadPart, (long)g_ctxw_n,
               (double)g_ctxw_tick_max * 1000.0 / (double)ip_qpf.QuadPart,
               pd_n_iter ? pd_hold_ms / pd_n_iter : 0.0, pd_hold_max, pd_hold_n05, pd_n_iter);
        g_ctxw_q = 0; g_ctxw_n = 0; g_ctxw_tick_max = 0;
        pd_hold_ms = pd_hold_max = 0.0; pd_hold_n05 = 0;
    }
    if (pd_stat_q && g_sba_loc && (li_mid || li_exact)) { /* W323 LOC-INTERP */
        HLOG_I("LOCINTERP stat: %.1fs takt=%dHz interp_fps=%d ticks=%u bilder=%u zwischen=%u exakt=%u wiederholt=%u "
               "gehalten=%u L=%.1fms r=%.1fms zustand_abw>2ms=%u (max %.1fms) kopien=%ld/%ld/%ld rawchg_matrix=%u rawchg_vertex=%u reject_geo=%u",
               secs, ip_tick_hz(), g_cfg.interp_fps, li_ticks, li_mid + li_exact, li_mid, li_exact, li_same, li_kept,
               (double)ip_lat * 1000.0 / (double)ip_qpf.QuadPart, ip_rbar * 1000.0 / (double)ip_qpf.QuadPart,
               li_dev2, li_dev_max, li_cp_rec, li_cp_ok, li_cp_rep, li_rz_alt, li_rz_vl, li_rj_cam2);
        li_cp_rec = li_cp_ok = li_cp_rep = 0;
        li_rz_alt = li_rz_vl = li_rj_cam2 = 0;
        if (g_cfg.loc_wlag_fix) { /* W412 LOC-WORLD-LAG: eigene Zeile (LOCINTERP stat bleibt unveraendert) */
            HLOG_I("LOCINTERP wlag: %.1fs Himmel mit vorhergesagter Kamera: Listen=%ld Draws=%ld", secs,
                   (long)InterlockedExchange(&wl_fix_lists, 0), (long)InterlockedExchange(&wl_fix_draws, 0));
        }
        if (g_cfg.loc_sky_fix || (g_cfg.loc_wlag_diag & 4)) { /* W435 LOC-SKY-LAG-REG: eigene Zeile (LOCINTERP stat bleibt unveraendert) */
            HLOG_I("LOCINTERP sky435: %.1fs Himmel mit Sicht-Korrektur: Listen=%ld Draws=%ld | Himmel unplausibel->Lerp=%ld", secs,
                   (long)InterlockedExchange(&sk_lists, 0), (long)InterlockedExchange(&sk_draws, 0),
                   (long)InterlockedExchange(&sk_rejlerp, 0));
            if (g_cfg.loc_wlag_diag & 4) { /* DIAG(W435) */
                long rep = InterlockedExchange(&sk_rep, 0), ron = InterlockedExchange(&sk_rep_on, 0);
                long rsk = InterlockedExchange(&sk_rep_sky, 0), rskc = InterlockedExchange(&sk_rep_skyc, 0);
                long rna = InterlockedExchange(&sk_rep_na, 0), kl = InterlockedExchange(&sk_klists, 0);
                long kls = InterlockedExchange(&sk_klists_sky, 0), klh = InterlockedExchange(&sk_klists_has, 0);
                HLOG_I("LOCWLAG sicht: %.1fs Wiedergaben=%ld mit Sicht-Korrektur=%ld (Winkel Mittel %.3f max %.3f Grad) | darin "
                       "Himmel-Draws=%ld korrigiert=%ld (%.1f %%) sonstige nicht-affin=%ld | Listen mit K=%ld Himmel mit K=%ld Himmel erkannt=%ld",
                       secs, rep, ron, ron ? sk_ang_sum / (double)ron * 57.29577951 : 0.0, sk_ang_max * 57.29577951, rsk, rskc,
                       rsk ? 100.0 * (double)rskc / (double)rsk : 0.0, rna, kl, kls, klh);
                sk_ang_sum = sk_ang_max = 0.0;
            }
        }
        if (g_cfg.loc_ret_fix) { /* W439 LOC-RETICLE-JUMP: eigene Zeile (LOCINTERP stat bleibt unveraendert) */
            HLOG_I("LOCINTERP ret439: %.1fs Fadenkreuz-Draws exakt aus Tick N, Verschiebung gegen B=%ld | Kandidaten "
                   "verworfen (Bewegung)=%ld", secs, (long)InterlockedExchange(&lr_retx, 0), (long)InterlockedExchange(&lr_rjmv, 0));
        }
        if (g_cfg.loc_ret_mark) { /* W444 LOC-RETICLE-MARK: eigene Zeile */
            HLOG_I("LOCINTERP retmark444: %.1fs Listen mit Fadenkreuz=%ld | Teile ueber Form=%ld Partner=%ld Start-Pruefung=%ld | "
                   "fremd verworfen=%ld | Vorlage um Gedaechtnis ergaenzt=%ld", secs, (long)InterlockedExchange(&lrm_st_lists, 0),
                   (long)InterlockedExchange(&lrm_st_form, 0), (long)InterlockedExchange(&lrm_st_pair, 0),
                   (long)InterlockedExchange(&lrm_st_seed, 0), (long)InterlockedExchange(&lrm_st_fremd, 0),
                   (long)InterlockedExchange(&lrm_st_mem, 0));
        }
        if (g_cfg.loc_ret_diag & 2) { /* DIAG(W439 LOC-RETICLE-JUMP): eigene Zeile */
            HLOG_I("LOCRET: %.1fs Wiedergaben=%ld mit Fadenkreuz=%ld verloren=%ld exakt=%ld ohne Vorhersage=%ld | Spruenge=%ld (gesamt %ld) "
                   "Abweichung max %.4f NDC", secs, (long)InterlockedExchange(&lr_rep, 0), (long)InterlockedExchange(&lr_ret, 0),
                   (long)InterlockedExchange(&lr_lost, 0), (long)InterlockedExchange(&lr_exact, 0), (long)InterlockedExchange(&lr_nola, 0),
                   (long)InterlockedExchange(&lr_jumps, 0), (long)lr_jumps_all, lr_err_max);
            lr_err_max = 0.0;
        }
        if (g_cfg.loc_stat_fix) { /* W415 LOC-STATIC-LAG: eigene Zeile (LOCINTERP stat bleibt unveraendert) */
            HLOG_I("LOCINTERP stat415: %.1fs Welt-Kamera ok=%u verworfen=%u keine=%u | Gelaende mit Welt-Kamera=%u Weltkoordinaten ohne Partner mit Welt-Kamera=%u",
                   secs, st_w415_cam, st_w415_rej, st_w415_nocam, st_w415_rzm, st_w415_nm);
            st_w415_cam = st_w415_rej = st_w415_nocam = st_w415_rzm = st_w415_nm = 0;
        }
        if (g_cfg.loc_interp_rawrzb) { /* W410 LOC-GFX-JITTER: eigene Zeile (LOCINTERP stat bleibt unveraendert) */
            HLOG_I("LOCINTERP rawrzb: %.1fs rawlerp->Matrix-Lerp=%u (>=1000 Vertices %u) Gelaende mit Kamera=%u Kamera passt nicht=%u bewegt=%u",
                   secs, st_w410_rzb, st_w410_big, st_w410_rzm, st_w410_cam, st_w410_mov);
            st_w410_rzb = st_w410_big = st_w410_cam = st_w410_mov = st_w410_rzm = 0;
        }
        if (g_cfg.loc_interp_eyeblind) { /* W448 LOC-CHAFF-LOOK: eigene Zeile */
            HLOG_I("LOCINTERP eye448: %.1fs Netze hinter der Kamera exakt statt Matrix-Lerp=%u", secs, st_w448_eye);
            st_w448_eye = 0;
        }
        if (g_cfg.interp_diag >= 1 && li_rj_n) { /* DIAG(W340) */
            HLOG_I("LOCINTERP reject: %u (w-Wechsel %u, Sprung <0.75 %u, <1 %u, <2 %u, >=2 %u NDC; >=1000 Vertices %u), davon Kamera-Lerp %u",
                   li_rj_n, li_rj_w, li_rj_j[0], li_rj_j[1], li_rj_j[2], li_rj_j[3], li_rj_big, li_rj_cam);
            li_rj_n = li_rj_w = li_rj_big = li_rj_cam = 0;
            memset(li_rj_j, 0, sizeof li_rj_j);
        }
        li_mid = li_exact = li_same = li_ticks = li_dev2 = li_kept = 0;
        li_dev_max = 0.0;
        if (g_cfg.loc_local_aim) { /* W376 LOC-LOCAL-AIM: eigene Zeile (LOCINTERP stat bleibt unveraendert) */
            HLOG_I("LOCAIM render: %.1fs Listen mit Vorhersage=%ld (Welt-Draws je Liste %ld, Fadenkreuz-Draws gesamt %ld) "
                   "Wiedergaben gueltig=%ld korrigiert=%ld",
                   secs, li_la_lists, li_la_lists ? li_la_draws / li_la_lists : 0L, li_la_rets, li_la_rep, li_la_on);
            InterlockedExchange(&li_la_lists, 0); InterlockedExchange(&li_la_draws, 0); InterlockedExchange(&li_la_rets, 0);
            InterlockedExchange(&li_la_rep, 0); InterlockedExchange(&li_la_on, 0);
        }
        if (g_cfg.loc_hud_predict || g_cfg.loc_view_perframe) { /* W394 LOC-HUD-VIEW: eigene Zeile, nur Zaehler */
            HLOG_I("LOCHUD: %.1fs Listen Gegner<100m=%ld mit Marken=%ld (Draws %ld, Kandidaten %ld, verworfen Bewegung %ld "
                   "ruhend %ld) Kompass=%ld | Wiedergaben=%ld verschoben=%ld "
                   "Abweichung Marke vorher mittel %.1f max %.1f, Rest mittel %.2f max %.2f px@640 | Kompass %.3f NDC/rad | "
                   "Radar Listen=%ld Richtung %+.0f (n=%d) | Sicht je Bild ohne/Basis/Zusatz/gehalten=%ld/%ld/%ld/%ld",
                   secs, li_hm_near, li_hm_lists, li_hm_draws, li_hm_cand, li_hm_rej_mv, li_hm_rej_st, li_hm_cmp, li_hm_rep, li_hm_on,
                   li_hm_px_n ? li_hm_px_sum / li_hm_px_n : 0.0, li_hm_px_max,
                   li_hm_res_n ? li_hm_res_sum / li_hm_res_n : 0.0, li_hm_res_max, li_cmp_k, li_hm_rad, li_rad_k, li_rad_n,
                   li_vpf_n[0], li_vpf_n[1], li_vpf_n[2], li_vpf_n[3]);
            InterlockedExchange(&li_hm_near, 0); InterlockedExchange(&li_hm_rad, 0);
            InterlockedExchange(&li_hm_cand, 0); InterlockedExchange(&li_hm_rej_mv, 0); InterlockedExchange(&li_hm_rej_st, 0); li_hm_res_n = 0; li_hm_res_sum = 0.0; li_hm_res_max = 0.0;
            InterlockedExchange(&li_hm_lists, 0); InterlockedExchange(&li_hm_draws, 0); InterlockedExchange(&li_hm_cmp, 0);
            InterlockedExchange(&li_hm_rep, 0); InterlockedExchange(&li_hm_on, 0);
            li_hm_px_n = 0; li_hm_px_sum = 0.0; li_hm_px_max = 0.0;
            InterlockedExchange(&li_vpf_n[0], 0); InterlockedExchange(&li_vpf_n[1], 0);
            InterlockedExchange(&li_vpf_n[2], 0); InterlockedExchange(&li_vpf_n[3], 0);
        }
    }
    {   /* W361: sollte nie vorkommen (ip_list_refs_check, ip_sh_rc_watch) */
        static LONG rc_last;
        if (pd_stat_q && (st_sh_stale || st_rc_bad != rc_last)) {
            HLOG_W("INTERP W361 stat: %.1fs %ld Wiedergaben mit ersetztem Schatten-Eintrag (gesamt %ld), Zaehler-Abweichungen "
                   "gesamt %ld, davon gehalten %ld", secs, st_sh_stale, st_sh_stale_tot, st_rc_bad, st_rc_fix);
            InterlockedExchange(&st_sh_stale, 0);
            rc_last = st_rc_bad;
        }
    }
    pd_stat_q = now;
    pd_n_ip = pd_n_hold = pd_n_late = pd_n_lockw = pd_n_iter = pd_n_gw = pd_n_gw_ok = 0;
    pd_n_rbwait = pd_n_rbskip = 0;
    pd_lockw_ms = pd_lockw_max = pd_late_max = pd_wake_err_max = pd_gw_ms = 0.0;
}

/* Waere das faellige Bild wegen GPU-Rueckstau (interp_gpu_q) verfallen? (unter ctx_lock) */
static int pd_gpu_would_skip(void)
{
    ip_list_t *A, *B;
    float t;
    int hold;
    if (!ip_live || ip_nhist < 1) { return 0; }
    ip_pair_at(ip_tnext, &A, &B, &t, &hold);
    return A && t < 1.0f && ip_gpu_behind();
}

/* ctx_lock mit Vortritt nehmen (Takt-Thread). */
#include "hlock.h" /* W168 PERF-TICK pd_gw_nopri */
extern const hlock_t *sb_ctx_hlock(void);
static void pd_lock(void)
{
    LONGLONG q0 = g_cfg.imlock_diag ? ip_now() : 0;
    void *site = q0 ? sb_iml_holder_site() : NULL;
    InterlockedExchange(&g_pd_want, 1);
    sb_ctx_enter();
    InterlockedExchange(&g_pd_want, 0);
    if (q0) { sb_iml_pd_wait(ip_now() - q0, site ? site : sb_iml_holder_site()); }
}

/* Ein Schritt unter ctx_lock; setzt pd_deadline. */
static void pd_step(void)
{
    LONGLONG now = ip_now();
    LONGLONG dt = ip_qpf.QuadPart / (g_cfg.interp_fps > 0 ? g_cfg.interp_fps : 30);
    if (!sb_pd_on()) { pd_deadline = now + dt; return; }
    if (ip_live && ip_nhist >= 1) {
        if (now >= ip_tnext) {
            unsigned np = st_presents;
            double late;
            /* GPU hinterher (interp_gpu_q): bis 1/2 Bildperiode ohne Sperre auf die GPU
             * warten, statt das Bild sofort verfallen zu lassen (Luecke 2 x 8,3 ms).
             * Der Spiel-Thread laeuft dabei weiter. Danach entscheidet ip_poll wie bisher. */
            if (pd_gpu_would_skip()) {
                int wp = g_cfg.pd_gpu_wait > 0 ? (g_cfg.pd_gpu_wait > 90 ? 90 : g_cfg.pd_gpu_wait) : 50; /* W165 GPU-COST2 */
                LONGLONG lim = ip_tnext + ip_dt * wp / 100, g0 = ip_now();
                pd_n_gw++;
                while (ip_now() < lim) {
                    LONGLONG w0;
                    sb_ctx_leave();
                    w0 = ip_now();
                    while (ip_now() - w0 < ip_qpf.QuadPart / 4000) { YieldProcessor(); } /* 0,25 ms */
                    /* PERF(PERF-TICK,W168) pd_gw_nopri=1: im GPU-Warten die Sperre ohne Vortritt nehmen - der
                     * Spiel-Thread musste sonst alle 0,25 ms weichen (CTXWAIT: bis 5800 Wartevorgaenge je 10 s) */
                    if (g_cfg.pd_gw_nopri) {
                        /* frei werdende Sperre abwarten (bis 0,5 ms), dann ohne Vortritt nehmen; sonst wie bisher */
                        const hlock_t *hl = sb_ctx_hlock();
                        LONGLONG s0 = ip_now();
                        int got = 0;
                        while (ip_now() - s0 < ip_qpf.QuadPart / 2000) {
                            if (!hlock_busy(hl)) { sb_ctx_enter(); got = 1; break; }
                            YieldProcessor();
                        }
                        if (!got) { pd_lock(); }
                    } else { pd_lock(); }
                    if (!pd_gpu_would_skip()) { pd_n_gw_ok++; break; }
                }
                pd_gw_ms += ip_ms(g0, ip_now());
                now = ip_now();
            }
            late = ip_ms(ip_tnext, now);
            ip_poll();
            if (st_presents != np) {
                pd_n_ip++;
                if (late > 1.0) { pd_n_late++; }
                if (late > pd_late_max) { pd_late_max = late; }
            }
        }
        pd_deadline = ip_tnext;
        return;
    }
    if (g_sba_loc) { /* W323 LOC-INTERP: keine Wiederhol-Presents im LoC-Profil (W320: Ruckeln); Menue/Lobby und
                      * die ersten Missionsbilder praesentiert das Spiel selbst */
        extern int sb_lm_step(LONGLONG now, LONGLONG *deadline); /* W377 LOC-MENU-FPS: Menues mit der Bildrate */
        if (sb_lm_step(now, &pd_deadline)) { return; }
        pd_deadline = now + dt;
        return;
    }
    {
        LONGLONG hp = dt + ip_qpf.QuadPart / 2000; /* 1/interp_fps + 0,5 ms */
        if (g_pd_last_q && now - g_pd_last_q >= hp) {
            if (g_cfg.warm_gpu_soft & 4) { /* W165: erst zeigen, dann Last -> sie hat die ganze Bildperiode */
                if (sb_render_present_hold()) { pd_n_hold++; }
                sb_warm_gpu_step(ip_now());
            } else {
                sb_warm_gpu_step(now); /* W139 PERF-WARM warm_gpu: GPU-Takt in der Ladephase halten */
                if (sb_render_present_hold()) { pd_n_hold++; }
            }
            pd_deadline = ip_now() + dt;
        } else {
            pd_deadline = g_pd_last_q ? g_pd_last_q + hp : now + dt;
        }
    }
}

/* W330 LOC-ROLLBACK-R1 (nur LoC-Profil): Present-Takt-Thread ruht, solange der Rollback den Gast-Speicher
 * laedt und Ticks neu rechnet (W325 D2: sonst Absturz im Treiber, Render-Thread der Zwischenbilder).
 * on=1 wartet, bis ein laufender Schritt und ein offener Auftrag des Render-Threads fertig sind
 * (hoechstens 200 ms). Rueckgabe 1 = ruhig. */
static volatile LONG pd_rb_pause, pd_rb_busy;
int sb_interp_rb_pause(int on)
{
    LONGLONG t0;
    if (!on) {
        InterlockedExchange(&pd_rb_pause, 0);
        return 1;
    }
    InterlockedExchange(&pd_rb_pause, 1);
    if (!ip_qpf.QuadPart) { QueryPerformanceFrequency(&ip_qpf); }
    t0 = ip_now();
    while (InterlockedCompareExchange(&pd_rb_busy, 0, 0) || InterlockedCompareExchange(&ip_rtj.st, 0, 0) == IP_RT_REQ) {
        if (ip_now() - t0 > ip_qpf.QuadPart / 5) { return 0; }
        SwitchToThread();
    }
    return 1;
}

static DWORD WINAPI pd_main(LPVOID p)
{
    HANDLE hs[2];
    (void)p;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    hs[0] = pd_timer;
    hs[1] = pd_wake;
    pd_deadline = ip_now();
    for (;;) {
        LONGLONG now = ip_now(), early = ip_qpf.QuadPart / 2500; /* 0,4 ms vorher wach, Rest spinnen */
        LONGLONG q0;
        double w;
        if (pd_deadline - now > early) {
            LARGE_INTEGER due; /* relativ, 100-ns-Einheiten */
            due.QuadPart = -(LONGLONG)((double)(pd_deadline - early - now) * 1e7 / (double)ip_qpf.QuadPart);
            if (due.QuadPart > -1) { due.QuadPart = -1; }
            if (SetWaitableTimer(pd_timer, &due, 0, NULL, NULL, FALSE)) {
                WaitForMultipleObjects(2, hs, FALSE, 100);
            } else {
                Sleep(1);
            }
        }
        if (WaitForSingleObject(pd_wake, 0) == WAIT_OBJECT_0) { pd_deadline = ip_now(); }
        while ((now = ip_now()) < pd_deadline) { YieldProcessor(); } /* <= 0,4 ms; SwitchToThread gaebe den Kern ganze Zeitscheiben ab */
        w = ip_ms(pd_deadline, now);
        if (w > pd_wake_err_max) { pd_wake_err_max = w; }
        if (!sb_pd_on()) { /* Testtakt / ohne Offscreen-BB / VR: ruhen, keine Sperre nehmen */
            pd_deadline = now + (ip_dt > 0 ? ip_dt : ip_qpf.QuadPart / 120);
            continue;
        }
        q0 = ip_now();
        if (g_sba_loc) { /* W330 LOC-ROLLBACK-R1: Ruhe beim Laden/Neurechnen (sb_interp_rb_pause) */
            extern int sb_lm_prelock(LONGLONG now, LONGLONG *deadline); /* W377 LOC-MENU-FPS */
            InterlockedExchange(&pd_rb_busy, 1);
            if (pd_rb_pause) {
                LONGLONG w0 = ip_now();
                InterlockedExchange(&pd_rb_busy, 0);
                /* W390 LOC-OWN-PREDICT: kurze Ruhe (Vorlauf je Tick ~1 ms, kurzes Neurechnen) abwarten statt das Bild
                 * auszulassen (sonst bei jedem Vorlauf, der in einen Bildtermin faellt, ein fehlendes Bild). Hoechstens
                 * 4 ms, dann wie bisher auslassen. Gegenseitiger Ausschluss wie oben (busy setzen, Ruhe erneut pruefen). */
                while (pd_rb_pause && ip_now() - w0 < ip_qpf.QuadPart / 250) { SwitchToThread(); }
                if (!pd_rb_pause) {
                    InterlockedExchange(&pd_rb_busy, 1);
                    if (!pd_rb_pause) { pd_n_rbwait++; goto pd_rb_go; }
                    InterlockedExchange(&pd_rb_busy, 0);
                }
                pd_n_rbskip++;
                pd_deadline = now + (ip_dt > 0 ? ip_dt : ip_qpf.QuadPart / 120);
                continue;
            }
        pd_rb_go:
            if (sb_lm_prelock(now, &pd_deadline)) { /* W377: Menue, Spiel arbeitet gerade -> ctx_lock nicht anfassen */
                InterlockedExchange(&pd_rb_busy, 0);
                continue;
            }
        }
        pd_lock();
        w = ip_ms(q0, ip_now());
        if (w > 1.0) { pc_lockw1++; } /* DIAG(W393) */
        if (w > pc_lockw_max) { pc_lockw_max = w; }
        if (w > 0.05) {
            pd_n_lockw++;
            pd_lockw_ms += w;
            if (w > pd_lockw_max) { pd_lockw_max = w; }
        }
        pd_n_iter++;
        q0 = ip_now();
        pd_step();
        sb_ctx_leave();
        if (g_sba_loc) { InterlockedExchange(&pd_rb_busy, 0); } /* W330 */
        if (g_cfg.ctx_wait_diag) { /* DIAG(PERF-TICK,W168): Haltezeit des Takt-Threads */
            double hm = ip_ms(q0, ip_now());
            pd_hold_ms += hm;
            if (hm > pd_hold_max) { pd_hold_max = hm; }
            if (hm > 0.5) { pd_hold_n05++; }
        }
        pd_stat_log(ip_now());
    }
}

void sb_pd_start(void)
{
    if (pd_started || g_cfg.present_decouple <= 0) { return; }
    if (g_cfg.vr_enable) { /* W183 VR-FIX3: im VR-Betrieb ruht der Takt ohnehin (sb_pd_on) -> Thread gar nicht starten */
        if (InterlockedCompareExchange(&pd_started, 2, 0) == 0) {
            HLOG_I("PRESENT-TAKT: aus im VR-Betrieb (Fenster folgt dem Interp-Takt, VR-Thread taktet die Brille)");
        }
        return;
    }
    if (InterlockedCompareExchange(&pd_started, 2, 0) != 0) { return; }
    if (!ip_qpf.QuadPart) { QueryPerformanceFrequency(&ip_qpf); }
    if (ip_dt <= 0) { ip_dt = ip_qpf.QuadPart / (g_cfg.interp_fps > 0 ? g_cfg.interp_fps : 120); }
    /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (0x2, Windows 10 1803+); sonst normal */
    pd_timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002u, TIMER_ALL_ACCESS);
    if (!pd_timer) { pd_timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS); }
    pd_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!pd_timer || !pd_wake) {
        HLOG_W("PRESENT-TAKT: Timer/Event fehlgeschlagen (%lu) -> aus", GetLastError());
        return; /* pd_started bleibt 2: sb_pd_on = 0 */
    }
    pd_th = CreateThread(NULL, 0, pd_main, NULL, CREATE_SUSPENDED, &pd_tid_l);
    if (!pd_th) {
        HLOG_W("PRESENT-TAKT: CreateThread fehlgeschlagen (%lu) -> aus", GetLastError());
        return;
    }
    g_pd_tid = pd_tid_l;
    if (g_cfg.present_core >= 0 && g_cfg.present_core < 32) {
        SetThreadAffinityMask(pd_th, (DWORD_PTR)1 << g_cfg.present_core);
    }
    if ((g_cfg.pace_vsync > 0 && g_sba_loc) || g_cfg.pace_vsync == 2 || g_cfg.pace_diag) { /* W393: VBlank-Thread (ruht ohne Kopplung/Messung; SB nur mit pace_vsync=2) */
        pv_th = CreateThread(NULL, 0, pv_main, NULL, 0, NULL);
        if (pv_th && g_cfg.present_core >= 0 && g_cfg.present_core < 32) { SetThreadAffinityMask(pv_th, (DWORD_PTR)1 << g_cfg.present_core); }
    }
    HLOG_I("PRESENT-TAKT: Thread T%04lx gestartet (Kern %d, Takt %d Hz, Echtzeit=%d, Offscreen=%d)", pd_tid_l,
           g_cfg.present_core, g_cfg.interp_fps, g_cfg.vblank_hz > 0, sb_render_offscreen());
    InterlockedExchange(&pd_started, 1);
    ResumeThread(pd_th);
}

int sb_interp_pending(void)
{
    return ip_live;
}

void sb_interp_field(unsigned counter)
{
    static unsigned last;
    if (counter != last) {
        last = counter;
        if (g_sba_loc) { /* W323: Leser ist der Spiel-Render-Thread -> 64 Bit atomar. Anker = Feldgrenze selbst
                          * (vblank_qpc: Feld k beginnt bei QPC k*f/hz), nicht der Zeitpunkt, zu dem der Haupt-Thread
                          * sie sieht - der sieht sie unter Last bis ~15 ms spaet (A1/A2, ein Kern je Client). */
            LONGLONG now = ip_now(), vb = now;
            if (g_cfg.vblank_qpc && g_cfg.vblank_hz > 0 && ip_qpf.QuadPart > 0) {
                unsigned long long hz = (unsigned long long)g_cfg.vblank_hz, f = (unsigned long long)ip_qpf.QuadPart;
                unsigned long long k = (unsigned long long)now * hz / f;
                vb = (LONGLONG)(k * f / hz);
                if (vb > now || now - vb > (LONGLONG)(f / hz)) { vb = now; }
            }
            InterlockedExchange64(&ip_vb_qpc, vb);
        }
        else { ip_vb_qpc = ip_now(); } /* Feldwechsel: Spiel-Warten endet hier */
    }
    if (ip_live) {
        if (g_sba_loc) { /* W323 LOC-INTERP: Feld-Poll (Haupt-Thread) und Swap (Spiel-Render-Thread) laufen
                          * nebeneinander -> Paarwahl nur unter ctx_lock. Mit Takt-Thread zeigt nur er die Bilder:
                          * dann gar nicht sperren (sonst sieht der Haupt-Thread den Feldwechsel bis 15 ms spaet, A1) */
            if (pd_owns_clock()) { return; }
            sb_ctx_enter();
            ip_poll();
            sb_ctx_leave();
        } else { ip_poll(); }
    }
}

void sb_interp_idle(unsigned max_ms)
{
    LONGLONG now;
    unsigned ms;
    ip_poll();
    if (!ip_live || pd_owns_clock()) { /* W138: Takt-Thread zeigt die Bilder -> nur warten */
        Sleep(max_ms > 1u ? max_ms - 1u : 0u);
        return;
    }
    now = ip_now();
    ms = (ip_tnext > now) ? (unsigned)((ip_tnext - now) * 1000 / ip_qpf.QuadPart) : 0u;
    if (max_ms > 1u && ms > max_ms - 1u) { ms = max_ms - 1u; }
    Sleep(ms);
}

static int ip_mission(void)
{
    unsigned scene = 0, mloop = 0;
    if (g_sba_loc) { return loc_render_interp_mission(); } /* W323 LOC-INTERP: LoC-Missionsbild (Spiel-Render-Thread) */
    __try {
        scene = *(volatile unsigned *)(uintptr_t)M3D_SCENE_ADDR;
        mloop = *(volatile unsigned *)(uintptr_t)M3D_MLOOP_ADDR;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return scene == 0x2Cu && mloop == 1u;
}

static void ip_stat_log(void)
{
    if (g_cfg.interp_mode == 1) { /* W295 WORLD-REPROJ */
        if (g_cfg.reproj_diag >= 1) { HLOG_I("REPROJ ohne: welt=%u tiefe=%u durchgang=%u ressourcen=%u", st_rp_skip[0], st_rp_skip[1], st_rp_skip[2], st_rp_skip[3]); }
        memset(st_rp_skip, 0, sizeof st_rp_skip);
        sb_rp_stat_log();
    }
    LONGLONG now = ip_now();
    double secs = ip_ms(st_t0, now) / 1000.0;
    if (secs <= 0.0) { secs = 1e-3; }
    HLOG_I("INTERP stat: ticks=%u pairs=%u presents=%u rate=%.1f/s ticks/s=%.1f K=%d lerp=%u rawlerp=%u rt2d=%u static=%u nomatch=%u rawchg=%u reject=%u 2d=%u cut=%u exact=%u hold=%u skip=%u late=%u nopair=%u ovf=%u rec=%.2fms cap=%.2fms replay=%.2fms(max %.2f) present=%.2fms gpu=%.2fms(max %.2f, n=%u) r=%.1fms(max %.1f) L=%.1fms verify=%u/%u",
           st_ticks, st_pairs, st_presents, (double)st_presents / secs,
           (double)st_ticks / secs, ip_K, st_lerp, st_rawlerp, st_rt2d, st_static, st_nomatch, st_rawchg, st_reject,
           st_2d, st_cut, st_exact, st_hold, st_skip, st_late, st_nopair, st_ovf,
           st_ticks ? st_rec_ms / st_ticks : 0.0,
           st_ticks ? (double)st_cap_q * 1000.0 / (double)ip_qpf.QuadPart / st_ticks : 0.0,
           st_rep_n ? st_rep_ms / st_rep_n : 0.0, st_rep_max,
           st_presents ? st_pres_ms / st_presents : 0.0,
           st_gpu_n ? st_gpu_ms / st_gpu_n : 0.0, st_gpu_max, st_gpu_n,
           st_r_n ? st_r_ms / st_r_n : 0.0, st_r_max, st_lat_n ? st_lat_ms / st_lat_n : 0.0,
           st_verify - st_verify_bad, st_verify);
    HLOG_I("INTERP stat2: geo=%u (Geometrie-Partner, in lerp enthalten)", st_geo);
    if (!g_sba_loc) { /* W347 GHOST: RT-Selbstkopien (gesehen / als Op aufgezeichnet / mit Schatten / wiedergegeben) */
        HLOG_I("INTERP selfcopy: gesehen=%ld ops=%ld schatten=%ld wiedergabe=%ld (interp_selfcopy=%d)",
               li_cp_seen, li_cp_rec, li_cp_ok, li_cp_rep, g_cfg.interp_selfcopy);
        li_cp_seen = li_cp_rec = li_cp_ok = li_cp_rep = 0;
        HLOG_I("INTERP bbcam: gepaart=%u ohne_partner=%u ungeeignet=%u raster=%u zeit=%.2fms (interp_bbcam=%d, Billboards per Kamera-Schwerpunkt)",
               st_bb_ok, st_bb_far, st_bb_no, st_bb_build, st_bb_ms, g_cfg.interp_bbcam);
        st_bb_ok = st_bb_far = st_bb_no = st_bb_build = 0;
        st_bb_ms = 0.0;
    }
    if (sb_ip_hlerp()) { /* FIX(W237) */
        HLOG_I("INTERP hlerp: ticks=%u fehl=%u draws=%u ersatz=%u (homogener Zwischenbild-Lerp, w-Puffer je Tick)",
               st_hw_up, st_hw_fail, st_hw_draws, st_hw_one);
    }
    if (g_cfg.interp_guard_diag) { /* DIAG(W265 LINE-GUARD) */
        ip_gd_poll();
        HLOG_I("INTERP guard: modus=%d maske=%d bilder=%u dreiecke=%llu treffer=%llu (%.4f%%) bilder_mit_treffer=%u max_je_bild=%u ohne_abfrage=%u",
               g_cfg.interp_guard, g_cfg.interp_guard_diag, st_gd_frames, st_gd_tris, st_gd_hit,
               st_gd_tris ? 100.0 * (double)st_gd_hit / (double)st_gd_tris : 0.0, st_gd_fhit, st_gd_max, st_gd_lost);
    }
    if (g_cfg.interp_zcons) { /* FIX(W251 INTERP-GROUND) */
        HLOG_I("INTERP zcons: modus=%d relerp=%u ohne_partner=%u kamera_abw=%u ziele_ok=%u ticks_exakt=%u draws_exakt=%u (Tiefen-Konsistenz)",
               g_cfg.interp_zcons, st_zc_rel, st_zc_nocand, st_zc_cam, st_zc_tgt, st_zc_ticks, st_zc_draws);
    }
    if (g_cfg.interp_rawzc) { /* FIX(W284 INTERP-RAWCHG) */
        HLOG_I("INTERP rawzc: relerp_welt=%u relerp_versatz=%u ohne_versatz=%u kamera_abw=%u ohne_kamera=%u sonst=%u zeit=%.3fms(max %.3f, n=%u) (verschobene Eingangsdaten)",
               st_rz_relw, st_rz_rel, st_rz_noofs, st_rz_cam, st_rz_nocam, st_rz_other,
               st_rz_n ? st_rz_ms / st_rz_n : 0.0, st_rz_max, st_rz_n);
        st_rz_ms = st_rz_max = 0.0; st_rz_n = 0;
        if (g_cfg.interp_rawzc >= 2) {
            HLOG_I("INTERP rawzc nomatch: relerp_welt=%u ohne_kandidat=%u kamera_abw=%u", st_rz_nm, st_rz_nmno, st_rz_nmcam);
            st_rz_nm = st_rz_nmno = st_rz_nmcam = 0;
        }
        st_rz_relw = st_rz_rel = st_rz_noofs = st_rz_cam = st_rz_nocam = st_rz_other = 0;
    }
    if (g_cfg.interp_pair || g_cfg.interp_pair_diag) { /* W317 INTERP-PAIR */
        HLOG_I("INTERP pair2: modus=%d ticks=%u menge=%u partner=%u vertex=%u ruhend=%u alt_frei=%u kamera=%u folge=%u geaendert=%u zeit=%.3fms(max %.3f; kamera+A %.3f, kandidaten %.3f, zuordnung %.3f)",
               g_cfg.interp_pair, st_pr_ticks, st_pr_set, st_pr_pair, st_pr_raw, st_pr_still, st_pr_keep, st_pr_cam, st_pr_follow, st_pr_chg,
               st_pr_ticks ? st_pr_ms / st_pr_ticks : 0.0, st_pr_max, st_pr_ticks ? st_pr_ms1 / st_pr_ticks : 0.0,
               st_pr_ticks ? st_pr_ms2 / st_pr_ticks : 0.0, st_pr_ticks ? st_pr_ms3 / st_pr_ticks : 0.0);
        if (g_cfg.interp_pair_diag) {
            int c;
            for (c = 0; c < PR_NC; c++) {
                const unsigned *z = st_pz[c];
                HLOG_I("INTERP pairz: klasse=%s ok=%u falsch=%u verpasst=%u neu=%u bewegt=%u exakt=%u statisch=%u",
                       kPR_CLS[c], z[0], z[1], z[2], z[3], z[4], z[5], z[6]);
            }
            memset(st_pz, 0, sizeof st_pz);
        }
        st_pr_ticks = st_pr_set = st_pr_pair = st_pr_raw = st_pr_still = st_pr_keep = st_pr_cam = st_pr_follow = st_pr_chg = 0;
        st_pr_ms = st_pr_max = st_pr_ms1 = st_pr_ms2 = st_pr_ms3 = 0.0;
    }
    if (g_cfg.interp_streak) { /* FIX(W294 LINE-STREAK) */
        HLOG_I("INTERP streak: modus=%d rawlerp_versatz=%u davon_welt=%u kamera_abw=%u rawchg_ohne_tiefe=%u",
               g_cfg.interp_streak, st_sk_rlofs, st_sk_rl, st_sk_rlcam, st_sk_nod);
        st_sk_rlofs = st_sk_rl = st_sk_rlcam = st_sk_nod = 0;
    }
    if (g_cfg.interp_p2hom && !g_sba_loc) { /* FIX(W429) */
        HLOG_I("INTERP p2hom: hlerp=%d reject_gelerpt_3d=%u reject_gelerpt_p2=%u (gleiche Eingangsdaten, Bezugs-Vertices im Bild springen wenig)"
               " exakt_daten_neu=%u exakt_ohne_w=%u exakt_kein_bezug=%u exakt_sprung=%u",
               sb_ip_hlerp(), st_p2hom[0], st_p2hom[1], st_rr[0], st_rr[1], st_rr[2], st_rr[3]);
        HLOG_I("INTERP p2cam: reject_3d=%u reject_p2=%u nomatch_3d=%u nomatch_p2=%u kamera_abw=%u fehler_ok=%u/%u/%u/%u"
               " fehler_abw=%u/%u/%u/%u/%u ohne_kamera=%u (Welt-Ruhe-Pruefung, Grenze 0,05 bzw. nomatch 0,02)",
               st_p2c[0], st_p2c[1], st_p2c[2], st_p2c[3], st_p2c[4], st_p2ok[0], st_p2ok[1], st_p2ok[2], st_p2ok[3],
               st_p2e[0], st_p2e[1], st_p2e[2], st_p2e[3], st_p2e[4], st_p2e[5]);
        st_p2hom[0] = st_p2hom[1] = 0;
        memset(st_p2c, 0, sizeof st_p2c); memset(st_p2e, 0, sizeof st_p2e); memset(st_p2ok, 0, sizeof st_p2ok);
        memset(st_rr, 0, sizeof st_rr);
    }
    if (g_cfg.interp_p2rej) { /* OPTION(W406) */
        HLOG_I("INTERP p2rej: gelerpt=%u (P2-Draws mit gleichen Eingangsdaten, sonst reject)", st_p2rej);
        st_p2rej = 0;
    }
    if (g_cfg.interp_eyediag > 0) { /* DIAG(W224) */
        HLOG_I("INTERP eyesum: draws=%u verts=%u (w-Wechsel in zwischengerechneten Draws, eyefix=%d)",
               st_eyex_draws, st_eyex_verts, g_cfg.interp_eyefix);
        st_eyex_draws = st_eyex_verts = 0;
        HLOG_I("INTERP p2sum: draws=%u verts=%u erkannt=%u sprung=%u ohne_wwechsel=%u welt: static=%u lerp=%u rawlerp=%u nomatch=%u rawchg=%u reject=%u",
               st_p2x_draws, st_p2x_verts, st_p2x_hit, st_jmp_verts, st_jmp_fp, st_wwhy[IP_W_STATIC], st_wwhy[IP_W_LERP],
               st_wwhy[IP_W_RAWLERP], st_wwhy[IP_W_NOMATCH], st_wwhy[IP_W_RAWCHG], st_wwhy[IP_W_REJECT]); /* DIAG(W237) */
        HLOG_I("INTERP jumpsum: vorn lerp=%u rawlerp=%u rt2d=%u (Draws %u/%u/%u) hinten lerp=%u rawlerp=%u rt2d=%u",
               st_jf[IP_W_LERP], st_jf[IP_W_RAWLERP], st_jf[IP_W_RT2DLERP], st_jfd[IP_W_LERP], st_jfd[IP_W_RAWLERP],
               st_jfd[IP_W_RT2DLERP], st_jb[IP_W_LERP], st_jb[IP_W_RAWLERP], st_jb[IP_W_RT2DLERP]); /* DIAG(W237) */
        memset(st_jf, 0, sizeof st_jf); memset(st_jb, 0, sizeof st_jb); memset(st_jfd, 0, sizeof st_jfd);
        st_p2x_draws = st_p2x_verts = st_p2x_hit = st_jmp_verts = st_jmp_fp = 0;
        memset(st_wwhy, 0, sizeof st_wwhy);
    }
    HLOG_I("INTERP inst: multi=%u wrong=%u amb=%u pass=%u (interp_inst=%d interp_pass=%d)", st_imulti,
           st_iwrong, st_iamb, st_ipass, g_cfg.interp_inst, g_cfg.interp_pass); /* POPIN (W91) */
    if (g_cfg.interp_pass && g_cfg.interp_pass_exact > 0) { /* W273 */
        HLOG_I("INTERP pass_exact: Fehltreffer der 16-Bit-Maske verworfen=%u", st_ipass_false);
        st_ipass_false = 0;
    }
    if (ip_rt_ok) { /* W126 RENDER-THREAD */
        HLOG_I("INTERP rt: auftraege=%u vorbereitet=%u nachgefordert=%u gewartet=%u verworfen=%u fehl=%u aufzeichnung=%.2fms(max %.2f, Render-Thread) spiel: holen=%.2fms ausfuehren=%.2fms je Bild",
               st_rt_n, st_rt_hit, st_rt_miss, st_rt_wait, st_rt_stale, st_rt_fail,
               st_rt_n ? st_rt_ms / st_rt_n : 0.0, st_rt_max,
               (st_rt_hit + st_rt_miss) ? st_rt_wait_ms / (st_rt_hit + st_rt_miss) : 0.0,
               st_rt_exec ? st_rt_exec_ms / st_rt_exec : 0.0);
        st_rt_n = st_rt_hit = st_rt_miss = st_rt_wait = st_rt_stale = st_rt_fail = st_rt_exec = 0;
        st_rt_ms = st_rt_max = st_rt_wait_ms = st_rt_exec_ms = 0.0;
    }
    st_geo = 0;
    st_imulti = st_iwrong = st_iamb = st_ipass = 0;
    st_t0 = now;
    st_presents = st_ticks = st_pairs = 0;
    st_lerp = st_static = st_nomatch = st_rawchg = st_reject = st_2d = st_rawlerp = st_rt2d = 0;
    st_cut = st_exact = st_hold = st_skip = st_late = st_nopair = st_ovf = 0;
    st_rec_ms = st_rep_ms = st_rep_max = st_pres_ms = 0.0;
    st_cap_q = 0;
    st_rep_n = 0;
    st_gpu_ms = st_gpu_max = 0.0;
    st_gpu_n = 0;
    st_r_ms = st_r_max = st_lat_ms = 0.0;
    st_r_n = st_lat_n = 0;
}

static void ip_hist_clear(void)
{
    int i;
    ip_rt_quiesce(); /* W126 RENDER-THREAD */
    for (i = 0; i < ip_nhist; i++) { ip_list_reset(&g_ipl[ip_hist[i]]); }
    ip_nhist = 0;
    ip_live = 0;
    ip_nneed = 0;
}

/* Bildfolge (interp_shots) fuer diesen Tick? Speichert das Spiel-Bild. */
static void ip_shots_tick(ip_list_t *B)
{
    char nm[64];
    ip_shot_ser = 0;
    if (g_cfg.interp_shots <= 0 || ip_pairs_total < (unsigned)g_cfg.interp_shots) { return; }
    if (!ip_shot_left && ip_pairs_total >= ip_shot_next) {
        if (ip_shot_seq >= (unsigned)g_cfg.interp_shot_max) { return; }
        ip_shot_left = (unsigned)(g_cfg.interp_shot_ticks > 0 ? g_cfg.interp_shot_ticks : 6);
        ip_shot_seq++;
        ip_shot_next = g_cfg.interp_shot_every > 0 ? ip_pairs_total + (unsigned)g_cfg.interp_shot_every
                                                   : 0xFFFFFFFFu;
    }
    if (!ip_shot_left) { return; }
    ip_shot_left--;
    ip_shot_ser = B->serial;
    _snprintf_s(nm, sizeof nm, _TRUNCATE, "ip_%06u_g", B->frame);
    sb_render_shot_name(nm, (ID3D11Resource *)g_ish[B->bbref].bak[ip_list_index(B)]);
}

/* W134 VR-PROTO: Augen-Wiedergabe eines Paars (unter ctx_lock, Context-Thread
 * oder VR-Thread). pA/pB = NULL -> Paar zum Zeitpunkt due (Echtzeit-Takt wie
 * ip_poll) bzw. im Testtakt das juengste Paar mit t aus der Zeit seit dem Swap. */
/* DIAG(VR-PROTO,W134): exaktes Mono-Bild des Ticks B (Vergleich Einzelauge, IPD 0, Kopf still) */
void sb_interp_vr_mono_shot(void *pB, const char *name)
{
    ip_list_t *B = (ip_list_t *)pB;
    if (!B || B->bbref < 0) { return; }
    sb_render_shot_name(name, (ID3D11Resource *)g_ish[B->bbref].bak[ip_list_index(B)]);
}

int sb_interp_vr_available(void)
{
    return ip_init_done && !ip_init_fail && ip_nhist >= 1;
}

int sb_interp_vr_replay(sb_vr_pass_t *p, void *pA, void *pB, float t, long long due, float *tout)
{
    ip_list_t *A = (ip_list_t *)pA, *B = (ip_list_t *)pB;
    ID3D11Buffer *vcb2 = NULL;
    int hold = 0;
    if (!ip_init_done || ip_init_fail || ip_nhist < 1 || !p || !p->rtv) { return 0; }
    if (!B) {
        LONGLONG now = ip_now();
        if (ip_live) {
            ip_pair_at(due ? (LONGLONG)due : now, &A, &B, &t, &hold);
        } else {
            B = &g_ipl[ip_hist[ip_nhist - 1]];
            A = ip_nhist >= 2 ? &g_ipl[ip_hist[ip_nhist - 2]] : NULL;
            t = 1.0f;
            if (A && B->tswap > A->tswap && now > B->tswap) {
                double f = (double)(now - B->tswap) / (double)(B->tswap - A->tswap);
                t = f >= 1.0 ? 1.0f : (float)f;
            }
        }
    }
    if (!B || B->bbref < 0 || !B->ready) { return 0; }
    if (A && !A->ready) { A = NULL; }
    p->hold = hold; /* W183 VR-FIX3: Diagnose Paarwahl */
    p->bser = B->serial;
    p->aser = A ? A->serial : 0u;
    ID3D11DeviceContext_VSGetConstantBuffers(g_ctx, 2, 1, &vcb2); /* VR-VS nutzt b2 */
    ip_vrp = p;
    {
        int gpm = sb_gp_begin(1); /* DIAG(GPU-COST,W146): GPU je VR-Bild (beide Augen) */
        ip_replay_ex(B, ip_list_index(B), A, A ? ip_list_index(A) : -1, t, 0, ip_cx_imm());
        if (gpm) { sb_gp_end(); }
    }
    ip_vrp = NULL;
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 2, 1, &vcb2);
    if (vcb2) { ID3D11Buffer_Release(vcb2); }
    if (tout) { *tout = (A && t < 1.0f) ? t : 1.0f; }
    return 1;
}

/* ---------- W193 PAUSE-LIVE: Standbild der Pause mit den aktuellen Einstellungen ----------
 * Der juengste Tick liegt vollstaendig in der Aufzeichnung (Draws, Clears, Zustaende,
 * Shading-Konstanten, Schatten-Startstand). Die exakte Wiedergabe (t = 1) zeichnet ihn neu;
 * was erst bei der Wiedergabe bzw. im Blit gelesen wird, wirkt damit sofort. Zusaetzlich
 * weichen nur in dieser Wiedergabe ab (ip_pl_*):
 *  - Shading-Konstanten: sfx0..sfx3 (Nebel, Kontrast, Gamma, Schaerfe, Bloom, Belichtung,
 *    Tonemapping, Licht Welt/Cockpit, Fokus, FXAA Welt-RT, weiche Schatten) je Shading-Draw aus den
 *    Eingaben der Aufzeichnung neu (shfx_rq_consts_rep); Schatten aus -> Schattenstufe = 1
 *    (tsc.z = 0, wie ohne Schattenpuffer). Das Fenster wird einmal je Liste zurueckgelesen.
 *  - shading aus -> Shading-Draws mit IM-PS-Ersatz (wie VR vr_shading=0).
 *  - monitor_fx aus bzw. Staerke 0 -> Korn-Wash/Stoerzeilen ausgelassen.
 *  - MVP-Haken der Backbuffer-Draws (W191 Cockpit-Projektion).
 * Mit unveraenderten Einstellungen ist das Ergebnis die exakte Wiedergabe (= Spiel-Bild,
 * Abgleich sb_interp_pause_cmp). */
static float *ip_pl_corig, *ip_pl_cwork; /* CPU-Kopie des Konstanten-Fensters (Original / neu) */
static unsigned ip_pl_cn, ip_pl_ccap;    /* belegte Konstanten / Kapazitaet (float4) */
static unsigned ip_pl_cser;              /* Liste, fuer die ip_pl_corig gilt */
static unsigned ip_pl_cbcap;             /* ip_pl_cb: Konstanten */

void sb_interp_set_pause_mvp_hook(sb_interp_mvp_hook_t fn)
{
    ip_pl_hook = fn;
}

/* Konstanten-Fenster der Liste B zurueckholen (einmal je Liste) und neu berechnen.
 * 1 = Kopie gebunden, 0 = keine Shading-Konstanten, -1 = Fehler (Aufzeichnungswerte). */
static int ip_pl_consts(ip_list_t *B, sb_pl_info_t *inf)
{
    D3D11_BUFFER_DESC bd;
    D3D11_MAPPED_SUBRESOURCE m;
    unsigned i, k, n;
    ip_pl_cbsrc = NULL;
    if (!B->rqc || !B->rqcpos) { return 0; }
    n = B->rqcpos;
    if (ip_pl_cser != B->serial || !ip_pl_corig || ip_pl_cn != n) {
        ID3D11Buffer *stg = NULL;
        if (ip_pl_ccap < n) {
            free(ip_pl_corig);
            free(ip_pl_cwork);
            ip_pl_corig = (float *)malloc((size_t)n * 16u);
            ip_pl_cwork = (float *)malloc((size_t)n * 16u);
            ip_pl_ccap = (ip_pl_corig && ip_pl_cwork) ? n : 0u;
            if (!ip_pl_ccap) { return -1; }
        }
        ID3D11Buffer_GetDesc(B->rqc, &bd);
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        bd.MiscFlags = 0;
        bd.StructureByteStride = 0;
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &stg))) { return -1; }
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)stg, (ID3D11Resource *)B->rqc);
        if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
            ID3D11Buffer_Release(stg);
            return -1;
        }
        memcpy(ip_pl_corig, m.pData, (size_t)n * 16u);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)stg, 0);
        ID3D11Buffer_Release(stg);
        ip_pl_cser = B->serial;
        ip_pl_cn = n;
    }
    memcpy(ip_pl_cwork, ip_pl_corig, (size_t)n * 16u);
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        const rq_rec_t *r = &o->rqr;
        float *pb, sx[16];
        if (o->kind != IP_DRAW || !o->rq || r->cbuf != B->rqc || r->pnum < 28u || r->pfirst + 28u > n) { continue; }
        pb = ip_pl_cwork + (size_t)r->pfirst * 4u;
        inf->ncb++;
        shfx_rq_consts_rep(sx, r->sfx_xp, r->sfx_tgt, (r->sfx_fl & 1u) != 0, (r->sfx_fl & 2u) != 0,
                           (r->sfx_fl & 4u) != 0, r->sfx_nst);
        if (memcmp(&pb[24u * 4u], sx, sizeof sx) != 0) {
            memcpy(&pb[24u * 4u], sx, sizeof sx);
            inf->nsfx++;
        }
        if (!g_cfg.shade_shadow) { /* Schattenpuffer (tsc[k].w != 0) aus: Stufe = 1 */
            for (k = 1; k < 4u; k++) {
                float *ts = &pb[(19u + k) * 4u];
                if (ts[3] != 0.0f && ts[2] != 0.0f) {
                    ts[2] = 0.0f;
                    ts[3] = 0.0f;
                    inf->nshadow++;
                }
            }
        }
    }
    if (!ip_pl_cb || ip_pl_cbcap < B->rqccap) {
        if (ip_pl_cb) { ID3D11Buffer_Release(ip_pl_cb); ip_pl_cb = NULL; ip_pl_cbcap = 0; }
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = B->rqccap * 16u;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &ip_pl_cb))) { ip_pl_cb = NULL; return -1; }
        ip_pl_cbcap = B->rqccap;
    }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)ip_pl_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { return -1; }
    memcpy(m.pData, ip_pl_cwork, (size_t)n * 16u);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)ip_pl_cb, 0);
    ip_pl_cbsrc = B->rqc;
    return 1;
}

/* W209: no extra VB work during normal play or an unchanged pause. Read the
 * recorded VB once per dirty camera replay, restore sparse originals, then
 * transform vertices and matrices together. The recording remains immutable. */
static int ip_pl_cpersp(ip_list_t *B)
{
    D3D11_BUFFER_DESC bd;
    D3D11_MAPPED_SUBRESOURCE map;
    D3D11_SUBRESOURCE_DATA init;
    D3D11_BOX box;
    ID3D11Buffer *stage = NULL;
    im_vertex_t *v = NULL;
    ip_cp_t *p;
    unsigned i, bytes = B->nverts * (unsigned)sizeof *v;
    int ok = 0;
    if (!sb_cp_pause_begin(B->cp_camera) || !ip_pl_hook) { return 1; }
    if (!bytes) { return 0; }
    v = (im_vertex_t *)malloc(bytes);
    ip_pl_mvp = (float (*)[16])malloc((size_t)B->nops * sizeof *ip_pl_mvp);
    if (!v || !ip_pl_mvp) { goto done; }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = bytes; bd.Usage = D3D11_USAGE_STAGING; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &stage))) { goto done; }
    memset(&box, 0, sizeof box);
    box.right = bytes; box.bottom = box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)stage, 0, 0, 0, 0,
                                            (ID3D11Resource *)B->vb, 0, &box);
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)stage, 0, D3D11_MAP_READ, 0, &map))) { goto done; }
    memcpy(v, map.pData, bytes);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)stage, 0);
    for (p = B->cp_original; p; p = p->next) {
        if (p->start > B->nverts || p->n > B->nverts - p->start) { goto done; }
        for (i = 0; i < p->n; i++) { memcpy(v[p->start + i].pos, p->pos[i], sizeof p->pos[i]); }
    }
    for (i = 0; i < B->nops; i++) {
        ip_op_t *o = &B->ops[i];
        memcpy(ip_pl_mvp[i], o->mvp, sizeof o->mvp);
        if (o->kind == IP_DRAW && o->rtv == g_rtv) {
            if (o->vstart > B->nverts || o->nsv > B->nverts - o->vstart) { goto done; }
            if (ip_pl_hook(ip_pl_mvp[i], o->cls, v[o->vstart].pos,
                           sizeof *v / sizeof(float), o->nsv)) { ip_pl_nhook++; } /* W264: Vertices im Tick-VB */
        }
    }
    bd.Usage = D3D11_USAGE_IMMUTABLE; bd.CPUAccessFlags = 0; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    memset(&init, 0, sizeof init); init.pSysMem = v;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, &init, &ip_pl_vb))) { goto done; }
    ok = 1;
done:
    if (stage) { ID3D11Buffer_Release(stage); }
    free(v);
    if (!ok) { free(ip_pl_mvp); ip_pl_mvp = NULL; }
    return ok;
}

static ID3D11Resource *ip_pl_render(sb_pl_info_t *inf, const sb_ph_rep_t *ph);
ID3D11Resource *sb_interp_pause_render(sb_pl_info_t *inf)
{
    return ip_pl_render(inf, NULL);
}

/* W195 PHOTO-MODE: wie sb_interp_pause_render, zusaetzlich mit den Foto-Abweichungen ph (Umlenkung in eine
 * Kachel, freie Kamera). Rueckgabe Schatten-BB (Cockpit-Bild) bzw. NULL. */
ID3D11Resource *sb_interp_photo_render(const sb_ph_rep_t *ph, sb_pl_info_t *inf, unsigned *ncam, unsigned *nred)
{
    ID3D11Resource *r;
    ip_ph_ncam = ip_ph_nred = ip_ph_nclr = 0;
    ip_ph_nhud = 0;
    ip_ph_nself = ip_ph_ncmfx = ip_ph_ncovl = 0;
    ip_ph_nextra = 0;
    ip_ph_self_L = NULL; /* W445: Merker eigener VT je Wiedergabe neu (Liste/Reichweite koennen wechseln) */
    r = ip_pl_render(inf, ph);
    ip_ph_self_L = NULL;
    if (ph && ph->cine) { /* W428 */
        static unsigned nlog;
        if (nlog++ < 12u || (g_cfg.photo_cine_diag & 1)) { /* W445: mit DIAG jede Wiedergabe */
            HLOG_I("PHOTO: Cinematic - ausgelassen: %u Draws des eigenen VT (Welt-RT e%d), %u Monitor-Effekt, %u Einblendungen "
                   "(Monitor-RT e%d / Backbuffer); W445: davon %u Luecke, Reichweite %d", ip_ph_nself, ph->wld, ip_ph_ncmfx,
                   ip_ph_ncovl, ph->mon, ip_ph_nextra, g_cfg.photo_cine_self);
        }
    }
    if (ph && ph->hud_off && (ip_ph_nhud || ph->mon < 0)) { /* W290 */
        static unsigned nlog;
        if (nlog++ < 4u) { HLOG_I("PHOTO: HUD aus - %u HUD-Draws im Monitor-RT e%d ausgelassen", ip_ph_nhud, ph->mon); }
    }
    if (ncam) { *ncam = ip_ph_ncam; }
    if (nred) { *nred = ip_ph_nred + ip_ph_nclr; }
    return r;
}

static ID3D11Resource *ip_pl_render(sb_pl_info_t *inf, const sb_ph_rep_t *ph)
{
    ip_list_t *A, *B;
    sb_pl_info_t dummy;
    unsigned i;
    if (!inf) { inf = &dummy; }
    memset(inf, 0, sizeof *inf);
    if (!ip_init_done || ip_init_fail || ip_nhist < 1 || !g_dev || !g_ctx) { return NULL; }
    B = &g_ipl[ip_hist[ip_nhist - 1]];
    A = ip_nhist >= 2 ? &g_ipl[ip_hist[ip_nhist - 2]] : NULL;
    if (!B->ready || B->bbref < 0) { return NULL; }
    if (A && !A->ready) { A = NULL; }
    ip_rt_quiesce(); /* W126: Render-Thread liest Listen/Schatten */
    inf->ser = B->serial;
    inf->frame = B->frame;
    inf->nops = B->nops;
    for (i = 0; i < B->nops; i++) {
        if (B->ops[i].kind == IP_DRAW && B->ops[i].rq) { inf->nrq++; }
    }
    inf->cb_state = ip_pl_consts(B, inf);
    if (inf->cb_state < 0) { ip_pl_cbsrc = NULL; }
    ip_pl_noshade = !g_cfg.shading;
    ip_pl_mfx = 0u;
    if (!g_cfg.monitor_fx || (g_cfg.monitor_grain <= 0 && g_cfg.monitor_glitch <= 0)) { ip_pl_mfx |= 1u; }
    if (!g_cfg.monitor_fx || g_cfg.monitor_glitch <= 0) { ip_pl_mfx |= 2u; }
    ip_pl_nhook = ip_pl_nmfx = ip_pl_nnoshade = 0;
    if (!ip_pl_cpersp(B)) { HLOG_W("CPERSP pause: rebuild failed, keeping snapshot"); return NULL; }
    ip_pl_tex = sb_tex_pause_nmap() > 0;
    ip_pl_on = 1;
    if (ph) { /* W195 */
        int j;
        ip_ph = *ph;
        ip_ph.on = 1;
        if (ip_ph.nsc > 8) { ip_ph.nsc = 8; }
        for (j = 0; j < ip_ph.nsc; j++) { ip_ph_shsrv[j] = ip_ph.sc_e[j] >= 0 ? g_ish[ip_ph.sc_e[j]].srv : NULL; }
    }
    ip_replay_ex(B, ip_list_index(B), A, A ? ip_list_index(A) : -1, 1.0f, 0, ip_cx_imm());
    memset(&ip_ph, 0, sizeof ip_ph);
    if (ip_pl_vb) { ID3D11Buffer_Release(ip_pl_vb); ip_pl_vb = NULL; }
    free(ip_pl_mvp); ip_pl_mvp = NULL;
    ip_pl_on = 0;
    ip_pl_tex = 0;
    ip_pl_cbsrc = NULL;
    inf->nhook = ip_pl_nhook;
    inf->nmfx = ip_pl_nmfx;
    inf->noshade = ip_pl_nnoshade;
    return (ID3D11Resource *)g_ish[B->bbref].sh;
}

int sb_interp_pause_cmp(ID3D11Resource *img, unsigned *ndiff, unsigned *maxd)
{
    D3D11_MAPPED_SUBRESOURCE m0, m1;
    ip_list_t *B;
    unsigned x, y, nd = 0, mx = 0;
    int bi;
    if (!ip_init_done || ip_nhist < 1 || !img) { return 0; }
    B = &g_ipl[ip_hist[ip_nhist - 1]];
    bi = ip_list_index(B);
    if (B->bbref < 0 || g_ish[B->bbref].bakser[bi] != B->serial) { return 0; }
    if (!ip_stage(0, img) || !ip_stage(1, (ID3D11Resource *)g_ish[B->bbref].bak[bi])) { return 0; }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)ip_vstg[0], 0, D3D11_MAP_READ, 0, &m0))) { return 0; }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)ip_vstg[1], 0, D3D11_MAP_READ, 0, &m1))) {
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)ip_vstg[0], 0);
        return 0;
    }
    for (y = 0; y < g_bb_h; y++) {
        const unsigned char *a = (const unsigned char *)m0.pData + (size_t)y * m0.RowPitch;
        const unsigned char *b = (const unsigned char *)m1.pData + (size_t)y * m1.RowPitch;
        if (memcmp(a, b, (size_t)g_bb_w * 4u) == 0) { continue; }
        for (x = 0; x < g_bb_w; x++) {
            unsigned c, d = 0;
            for (c = 0; c < 3u; c++) {
                unsigned dc = a[x * 4u + c] > b[x * 4u + c] ? a[x * 4u + c] - b[x * 4u + c] : b[x * 4u + c] - a[x * 4u + c];
                if (dc > d) { d = dc; }
            }
            if (d) {
                nd++;
                if (d > mx) { mx = d; }
            }
        }
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)ip_vstg[1], 0);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)ip_vstg[0], 0);
    if (ndiff) { *ndiff = nd; }
    if (maxd) { *maxd = mx; }
    return 1;
}

/* ---------- W195 PHOTO-MODE: Zensus des juengsten Ticks und Matrix-Hilfen ---------- */
static int ip_m4inv(float out[16], const float m[16])
{
    double a[4][8];
    int i, j, k;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) { a[i][j] = m[i * 4 + j]; a[i][j + 4] = (i == j) ? 1.0 : 0.0; }
    }
    for (i = 0; i < 4; i++) {
        int p = i;
        double d;
        for (k = i + 1; k < 4; k++) { if (fabs(a[k][i]) > fabs(a[p][i])) { p = k; } }
        if (fabs(a[p][i]) < 1e-12) { return 0; }
        if (p != i) { for (j = 0; j < 8; j++) { double t = a[i][j]; a[i][j] = a[p][j]; a[p][j] = t; } }
        d = a[i][i];
        for (j = 0; j < 8; j++) { a[i][j] /= d; }
        for (k = 0; k < 4; k++) {
            if (k != i && a[k][i] != 0.0) {
                double f = a[k][i];
                for (j = 0; j < 8; j++) { a[k][j] -= f * a[i][j]; }
            }
        }
    }
    for (i = 0; i < 4; i++) { for (j = 0; j < 4; j++) { out[i * 4 + j] = (float)a[i][j + 4]; } }
    return 1;
}

static int ip_ph_ent_rtv(ID3D11RenderTargetView *v)
{
    int k;
    if (!v) { return -1; }
    for (k = 0; k < IP_SH_MAX; k++) { if (g_ish[k].rtv == v) { return k; } }
    return -1;
}

static int ip_ph_ent_dsv(ID3D11DepthStencilView *v)
{
    int k;
    if (!v) { return -1; }
    for (k = 0; k < IP_SH_MAX; k++) { if (g_ish[k].dsv == v) { return k; } }
    return -1;
}

/* W195: Vertices (Position, UV, NDC nach MVP) eines Draws der Liste B ins Log (Beleg, welcher Ausschnitt
 * des Welt-RT auf dem Hauptmonitor landet). Nur aus dem Foto-Zensus. */
static void ip_ph_dump_draw(ip_list_t *B, const ip_op_t *o, const char *what, float *bbx)
{
    D3D11_BUFFER_DESC bd;
    D3D11_BOX bx;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Buffer *stg = NULL;
    unsigned n = bbx ? (o->nsv < 65536u ? o->nsv : 65536u) : (o->nsv < 6u ? o->nsv : 6u), q; /* Grenzen ueber alle Vertices (W264: im Tick-VB) */
    char line[900];
    size_t pos = 0;
    if (!B->vb || !n) { return; }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = n * sizeof(im_vertex_t);
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &stg))) { return; }
    bx.left = o->vstart * sizeof(im_vertex_t); bx.right = bx.left + n * sizeof(im_vertex_t);
    bx.top = 0; bx.bottom = 1; bx.front = 0; bx.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)stg, 0, 0, 0, 0, (ID3D11Resource *)B->vb, 0, &bx);
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
        const im_vertex_t *v = (const im_vertex_t *)m.pData;
        if (bbx) { bbx[0] = bbx[2] = bbx[4] = bbx[6] = 1e30f; bbx[1] = bbx[3] = bbx[5] = bbx[7] = -1e30f; }
        for (q = 0; q < n; q++) {
            float c[4];
            ip_clip(v[q].pos, o->mvp, c);
            if (bbx && c[3] != 0.0f) { /* NDC-x/y- und UV-Grenzen */
                float nx = c[0] / c[3], ny = c[1] / c[3];
                if (nx < bbx[0]) { bbx[0] = nx; } if (nx > bbx[1]) { bbx[1] = nx; }
                if (ny < bbx[2]) { bbx[2] = ny; } if (ny > bbx[3]) { bbx[3] = ny; }
                if (v[q].uv[0] < bbx[4]) { bbx[4] = v[q].uv[0]; } if (v[q].uv[0] > bbx[5]) { bbx[5] = v[q].uv[0]; }
                if (v[q].uv[1] < bbx[6]) { bbx[6] = v[q].uv[1]; } if (v[q].uv[1] > bbx[7]) { bbx[7] = v[q].uv[1]; }
            }
            if (q < 6u) pos += (size_t)_snprintf_s(line + pos, sizeof line - pos, _TRUNCATE, " v%u ndc=(%.3f,%.3f) uv=(%.4f,%.4f)", q,
                                       c[3] != 0.0f ? c[0] / c[3] : 0.0f, c[3] != 0.0f ? c[1] / c[3] : 0.0f, v[q].uv[0],
                                       v[q].uv[1]);
        }
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)stg, 0);
        if (bbx) {
            pos += (size_t)_snprintf_s(line + pos, sizeof line - pos, _TRUNCATE, " | alle: ndc x %.3f..%.3f y %.3f..%.3f uv %.4f..%.4f x %.4f..%.4f",
                                       bbx[0], bbx[1], bbx[2], bbx[3], bbx[4], bbx[5], bbx[6], bbx[7]);
        }
        HLOG_I("PHOTO: %s n=%u vp=%.0fx%.0f@%.0f,%.0f vpg=%u,%u %ux%u tex=0x%X%s", what, o->nv, o->vp.Width, o->vp.Height,
               o->vp.TopLeftX, o->vp.TopLeftY, o->vpg[0], o->vpg[1], o->vpg[2], o->vpg[3], o->has_tex, line);
    }
    ID3D11Buffer_Release(stg);
}

/* DIAG(W249) PHOTO-PIXEL: Raster-Lage der Eckpunkte je Welt-Draw (photo_diag=1, nur Foto-Zensus).
 * Position im logischen Welt-RT (Pixel, wie das Spiel sie sieht): Clip -> NDC -> Viewport -> / RT-Skala.
 * "auf Raster" = x und y je hoechstens 1/64 Pixel neben einer ganzen Zahl. Klasse: VS-Programm (P1/P2/sonst,
 * vsh_program), Draw mit Pixel-Matrix (RHW-Pfad) oder 3D. */
static int ip_ph_ent_srv(ID3D11ShaderResourceView *v)
{
    int k;
    if (!v) { return -1; }
    for (k = 0; k < IP_SH_MAX; k++) { if (g_ish[k].srv == v) { return k; } }
    return -1;
}

static void ip_ph_quant(ip_list_t *B, int best, unsigned w_tw, unsigned w_lw)
{
    unsigned i, nlog = 0, tot[4][3], rd[IP_SH_MAX];
    int k;
    float rsc = (w_lw && w_tw) ? (float)w_tw / (float)w_lw : 1.0f;
    static const char *const kn[4] = { "3D", "RHW", "P1", "P2" };
    memset(tot, 0, sizeof tot);
    memset(rd, 0, sizeof rd);
    if (!B->vb) { return; }
    for (i = 0; i < B->nops; i++) { /* welche Schatten-Ziele (RTs) lesen Welt-Draws als Textur (Stufe 0..3)? */
        const ip_op_t *o = &B->ops[i];
        int st;
        if (o->kind != IP_DRAW || o->sh_rtv != g_ish[best].rtv) { continue; }
        k = ip_ph_ent_srv(o->sh_srv);
        if (k >= 0) { rd[k]++; }
        for (st = 1; st < 4 && o->rq; st++) { k = ip_ph_ent_srv(o->rq_shsrv[st]); if (k >= 0) { rd[k]++; } }
    }
    for (k = 0; k < IP_SH_MAX; k++) {
        if (rd[k] && g_ish[k].sh) {
            D3D11_TEXTURE2D_DESC td;
            unsigned j, nw = 0;
            ID3D11Texture2D_GetDesc(g_ish[k].sh, &td);
            for (j = 0; j < B->nops; j++) { if (B->ops[j].kind == IP_DRAW && B->ops[j].sh_rtv == g_ish[k].rtv) { nw++; } }
            HLOG_I("PHOTO diag: Welt-Draws lesen Ziel e%d %ux%u (%u Draws, im Tick %u Draws in dieses Ziel)", k, td.Width, td.Height,
                   rd[k], nw);
        }
    }
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        const vsh_prog_t *vp;
        D3D11_BUFFER_DESC bd;
        D3D11_BOX bx;
        D3D11_MAPPED_SUBRESOURCE m;
        ID3D11Buffer *stg = NULL;
        unsigned n, q, ngrid = 0, nhalf = 0, nok = 0, kc;
        int pix;
        float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, r0[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        if (o->kind != IP_DRAW || o->sh_rtv != g_ish[best].rtv || !o->nv) { continue; }
        n = o->nsv < 65536u ? o->nsv : 65536u; /* W264: Vertices im Tick-VB */
        vp = o->pvs ? vsh_program(o->pvs) : NULL;
        pix = o->mvp[3] == 0.0f && o->mvp[7] == 0.0f && o->mvp[11] == 0.0f && o->mvp[15] == 1.0f;
        kc = (vp && vp->kind == 1) ? 2u : (vp && vp->kind == 2) ? 3u : pix ? 1u : 0u;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = n * sizeof(im_vertex_t);
        bd.Usage = D3D11_USAGE_STAGING;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &stg))) { continue; }
        bx.left = o->vstart * sizeof(im_vertex_t); bx.right = bx.left + n * sizeof(im_vertex_t);
        bx.top = 0; bx.bottom = 1; bx.front = 0; bx.back = 1;
        ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)stg, 0, 0, 0, 0, (ID3D11Resource *)B->vb, 0, &bx);
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
            const im_vertex_t *v = (const im_vertex_t *)m.pData;
            memcpy(r0, v[0].pos, sizeof r0);
            for (q = 0; q < n; q++) {
                float c[4], X, Y, fx, fy;
                ip_clip(v[q].pos, o->mvp, c);
                if (c[3] <= 1e-6f) { continue; }
                X = (o->vp.TopLeftX + (c[0] / c[3] + 1.0f) * 0.5f * o->vp.Width) / rsc;
                Y = (o->vp.TopLeftY + (1.0f - c[1] / c[3]) * 0.5f * o->vp.Height) / rsc;
                fx = fabsf(X - floorf(X + 0.5f));
                fy = fabsf(Y - floorf(Y + 0.5f));
                nok++;
                if (fx <= 1.0f / 64.0f && fy <= 1.0f / 64.0f) { ngrid++; }
                else if (fabsf(fx - 0.5f) <= 1.0f / 64.0f && fabsf(fy - 0.5f) <= 1.0f / 64.0f) { nhalf++; }
                if (X < x0) { x0 = X; } if (X > x1) { x1 = X; }
                if (Y < y0) { y0 = Y; } if (Y > y1) { y1 = Y; }
            }
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)stg, 0);
        }
        ID3D11Buffer_Release(stg);
        tot[kc][0]++;
        tot[kc][1] += nok;
        tot[kc][2] += ngrid;
        if (nlog < 600u) {
            D3D11_DEPTH_STENCIL_DESC dd;
            D3D11_BLEND_DESC bd2;
            D3D11_RASTERIZER_DESC rd;
            memset(&dd, 0, sizeof dd);
            memset(&bd2, 0, sizeof bd2);
            memset(&rd, 0, sizeof rd);
            if (o->ds) { ID3D11DepthStencilState_GetDesc(o->ds, &dd); }
            if (o->bs) { ID3D11BlendState_GetDesc(o->bs, &bd2); }
            if (o->rs) { ID3D11RasterizerState_GetDesc(o->rs, &rd); }
            nlog++;
            HLOG_I("PHOTO diag: Welt-Draw #%u %s vs=0x%X tex=0x%X srv=e%d n=%u rq=%d auf Raster %u/%u (halb %u) x %.1f..%.1f y %.1f..%.1f "
                   "v0=(%.4f,%.4f,%.4f,%.4f) dsv=e%d z=%d/%d/%d st=%d bl=%d/%d/%d wm=%X bias=%d/%.3f",
                   i, kn[kc], o->pvs, o->pxp, ip_ph_ent_srv(o->sh_srv), o->nv, o->rq, ngrid, nok, nhalf, (double)x0, (double)x1, (double)y0, (double)y1,
                   (double)r0[0], (double)r0[1], (double)r0[2], (double)r0[3], ip_ph_ent_dsv(o->sh_dsv), (int)dd.DepthEnable,
                   (int)dd.DepthWriteMask, (int)dd.DepthFunc, (int)dd.StencilEnable, (int)bd2.RenderTarget[0].BlendEnable,
                   (int)bd2.RenderTarget[0].SrcBlend, (int)bd2.RenderTarget[0].DestBlend,
                   (unsigned)bd2.RenderTarget[0].RenderTargetWriteMask, (int)rd.DepthBias, (double)rd.SlopeScaledDepthBias);
        }
    }
    for (i = 0; i < 4u; i++) {
        if (tot[i][0]) {
            HLOG_I("PHOTO diag: Klasse %s: %u Draws, %u Eckpunkte, davon auf dem Pixelraster des Welt-RT %u (%.1f %%)", kn[i],
                   tot[i][0], tot[i][1], tot[i][2], tot[i][1] ? 100.0 * tot[i][2] / tot[i][1] : 0.0);
        }
    }
}

/* DIAG(W428) PHOTO-CINEMATIC: je Welt-Draw Lage des Modell-Ursprungs in der Sicht der Spielkamera
 * (Zeile 3 der mvp / Projektion: x = m12/a, y = m13/b, z = m15), Bezugs-NDC und -w (photo_diag Bit 4). */
static void ip_ph_selfdiag(ip_list_t *B, int best)
{
    unsigned i, n = 0;
    const ip_list_t *A = NULL; /* W445 DIAG: Vortick (Partner o->match) */
    if (ip_nhist >= 2 && g_ipl[ip_hist[ip_nhist - 2]].serial == B->prevser && B->pair_ok) { A = &g_ipl[ip_hist[ip_nhist - 2]]; }
    for (i = 0; i < B->nops && n < 400u; i++) {
        const ip_op_t *o = &B->ops[i];
        const vsh_prog_t *vp;
        float a, b, wmin = 1e30f, x0 = 9.0f, x1 = -9.0f, y0 = 9.0f, y1 = -9.0f, dm = -1.0f;
        int q;
        if (o->kind != IP_DRAW || o->sh_rtv != g_ish[best].rtv) { continue; }
        vp = o->pvs ? vsh_program(o->pvs) : NULL;
        a = o->prj[0] > 1e-4f ? o->prj[0] : 1.0f;
        b = o->prj[1] > 1e-4f ? o->prj[1] : 1.0f;
        for (q = 0; q < 4; q++) {
            if (o->rn[q][2] < wmin) { wmin = o->rn[q][2]; }
            if (o->rn[q][0] < x0) { x0 = o->rn[q][0]; } if (o->rn[q][0] > x1) { x1 = o->rn[q][0]; }
            if (o->rn[q][1] < y0) { y0 = o->rn[q][1]; } if (o->rn[q][1] > y1) { y1 = o->rn[q][1]; }
        }
        if (A && o->match >= 0 && (unsigned)o->match < A->nops) { /* W445: Ursprung in Kamera-Sicht gegen den Vortick */
            const ip_op_t *p = &A->ops[o->match];
            float pa = p->prj[0] > 1e-4f ? p->prj[0] : 1.0f, pb = p->prj[1] > 1e-4f ? p->prj[1] : 1.0f;
            float dx = o->mvp[12] / a - p->mvp[12] / pa, dy = o->mvp[13] / b - p->mvp[13] / pb, dz = o->mvp[15] - p->mvp[15];
            dm = sqrtf(dx * dx + dy * dy + dz * dz);
        }
        n++;
        HLOG_I("PHOTO cine: i=%u cls=%u vsk=%d nv=%u tex=%x rq=%d pxp=%08x pps=%08x nolerp=%d fxk=%u org=%.2f,%.2f,%.2f "
               "prj=%.3f,%.3f,%.4f,%.4f w=%.2f ndc=%.2f..%.2f,%.2f..%.2f r0=%.3f,%.3f,%.3f r1=%.3f,%.3f,%.3f near=%.2f dm=%.3f",
               i, (unsigned)o->cls, vp ? (int)vp->kind : -1, o->nv, o->has_tex, o->rq, o->pxp, o->pps, o->nolerp, o->fxk,
               (double)(o->mvp[12] / a), (double)(o->mvp[13] / b), (double)o->mvp[15], (double)o->prj[0], (double)o->prj[1],
               (double)o->prj[2], (double)o->prj[3], (double)wmin, (double)x0, (double)x1, (double)y0, (double)y1,
               (double)(o->mvp[0] / a), (double)(o->mvp[1] / b), (double)o->mvp[3], (double)(o->mvp[4] / a),
               (double)(o->mvp[5] / b), (double)o->mvp[7], (double)ip_ph_near_ref(o), (double)dm);
    }
}

int sb_interp_photo_census(sb_ph_census_t *c, int log)
{
    ip_list_t *B;
    unsigned cnt[IP_SH_MAX], cnt3[IP_SH_MAX], dcnt[IP_SH_MAX], bdcnt[IP_SH_MAX], i;
    int k, best = -1, bd = -1, wd = -1;
    D3D11_TEXTURE2D_DESC td;
    memset(c, 0, sizeof *c);
    c->bb = c->bbd = c->wld = c->wldd = -1;
    if (!ip_init_done || ip_init_fail || ip_nhist < 1) { return 0; }
    B = &g_ipl[ip_hist[ip_nhist - 1]];
    if (!B->ready || B->bbref < 0) { return 0; }
    memset(cnt, 0, sizeof cnt);
    memset(cnt3, 0, sizeof cnt3);
    memset(dcnt, 0, sizeof dcnt);
    memset(bdcnt, 0, sizeof bdcnt);
    c->ser = B->serial;
    c->frame = B->frame;
    c->nops = B->nops;
    c->bb = B->bbref;
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        int e;
        if (o->kind != IP_DRAW) { continue; }
        e = ip_ph_ent_rtv(o->sh_rtv);
        if (e < 0) { continue; }
        cnt[e]++;
        if (o->cls) { cnt3[e]++; }
        if (e == B->bbref) {
            int d = ip_ph_ent_dsv(o->sh_dsv);
            c->n_bb++;
            if (o->cls) { c->n_bb3d++; } else { c->n_bb2d++; }
            if (d >= 0) { bdcnt[d]++; }
        }
    }
    for (k = 0; k < IP_SH_MAX; k++) {
        if (k != B->bbref && cnt[k]) { c->n_rts++; }
        if (k != B->bbref && cnt3[k] >= 8u && (best < 0 || cnt3[k] > cnt3[best])) { best = k; }
        if (bdcnt[k] && (bd < 0 || bdcnt[k] > bdcnt[bd])) { bd = k; }
    }
    c->bbd = bd;
    c->wld = best;
    c->mon = -1;
    if (best >= 0) { /* W290: Monitor-RT = groesstes Ziel (ohne BB), das das Welt-RT liest (wie der Beleg unten) */
        unsigned mw = 0;
        for (i = 0; i < B->nops; i++) {
            const ip_op_t *o = &B->ops[i];
            int e;
            if (o->kind != IP_DRAW || !o->sh_srv || o->sh_srv != g_ish[best].srv || o->sh_rtv == g_ish[best].rtv) { continue; }
            e = ip_ph_ent_rtv(o->sh_rtv);
            if (e >= 0 && e != B->bbref && g_ish[e].sh) {
                ID3D11Texture2D_GetDesc(g_ish[e].sh, &td);
                if (td.Width > mw) { mw = td.Width; c->mon = e; }
            }
        }
    }
    if (g_ish[B->bbref].sh) {
        ID3D11Texture2D_GetDesc(g_ish[B->bbref].sh, &td);
        c->bb_w = td.Width;
        c->bb_h = td.Height;
        c->bb_tex = g_ish[B->bbref].sh;
    }
    if (best >= 0) {
        int gotvp = 0, gotprj = 0;
        for (i = 0; i < B->nops; i++) {
            const ip_op_t *o = &B->ops[i];
            int d;
            if (o->kind != IP_DRAW || o->sh_rtv != g_ish[best].rtv) { continue; }
            c->n_w++;
            if (o->rq) { c->n_wrq++; }
            if (o->cls) { c->n_w3d++; } else {
                c->n_w2d++;
                if (o->rn[0][2] > 1.001f || o->rn[0][2] < 0.999f || o->rn[3][2] > 1.001f || o->rn[3][2] < 0.999f) { c->n_w2dw++; }
            }
            d = ip_ph_ent_dsv(o->sh_dsv);
            if (d >= 0) { dcnt[d]++; }
            if (!gotvp && o->vpg[2] && o->vpg[3] && o->rtl[0] && o->rtl[1]) {
                c->g_x = o->vpg[0]; c->g_y = o->vpg[1]; c->g_w = o->vpg[2]; c->g_h = o->vpg[3];
                c->w_lw = o->rtl[0]; c->w_lh = o->rtl[1];
                gotvp = 1;
            }
            if (!gotprj && o->cls && o->prj[0] > 0.05f && o->prj[1] > 0.05f) {
                memcpy(c->prj, o->prj, sizeof c->prj);
                gotprj = 1;
            }
        }
        for (k = 0; k < IP_SH_MAX; k++) { if (dcnt[k] && (wd < 0 || dcnt[k] > dcnt[wd])) { wd = k; } }
        c->wldd = wd;
        if (g_ish[best].sh) {
            ID3D11Texture2D_GetDesc(g_ish[best].sh, &td);
            c->w_tw = td.Width;
            c->w_th = td.Height;
            c->w_tex = g_ish[best].sh;
        }
        if (!gotvp && c->w_tw) { c->g_w = c->w_lw = c->w_tw; c->g_h = c->w_lh = c->w_th; }
        if (log && (g_cfg.photo_diag & ~16)) { ip_ph_quant(B, best, c->w_tw, c->w_lw); } /* DIAG(W249) */
        if (log && (g_cfg.photo_diag & 16)) { ip_ph_selfdiag(B, best); } /* DIAG(W428) */
    }
    for (k = 0; k < IP_SH_MAX && c->n_rt < 16u; k++) { /* RT-Ziele (ohne BB) mit Tiefe, Groesse, Draws */
        if (k == B->bbref || !cnt[k] || !g_ish[k].sh) { continue; }
        c->rt[c->n_rt].e = k;
        c->rt[c->n_rt].d = -1;
        c->rt[c->n_rt].nd = cnt[k];
        c->rt[c->n_rt].tex = g_ish[k].sh;
        ID3D11Texture2D_GetDesc(g_ish[k].sh, &td);
        c->rt[c->n_rt].w = td.Width;
        c->rt[c->n_rt].h = td.Height;
        for (i = 0; i < B->nops; i++) {
            const ip_op_t *o = &B->ops[i];
            if (o->kind == IP_DRAW && o->sh_rtv == g_ish[k].rtv && o->sh_dsv) {
                c->rt[c->n_rt].d = ip_ph_ent_dsv(o->sh_dsv);
                break;
            }
        }
        c->n_rt++;
    }
    c->ok = best >= 0 && c->w_tw > 0 && c->bb_w > 0;
    if (log) {
        char top[256];
        size_t pos = 0;
        unsigned used[IP_SH_MAX];
        int r;
        memset(used, 0, sizeof used);
        top[0] = 0;
        for (r = 0; r < 6; r++) { /* groesste RT-Ziele (Draws) fuer den Beleg */
            int m = -1;
            for (k = 0; k < IP_SH_MAX; k++) {
                if (k != B->bbref && cnt[k] && !used[k] && (m < 0 || cnt[k] > cnt[m])) { m = k; }
            }
            if (m < 0) { break; }
            used[m] = 1;
            memset(&td, 0, sizeof td);
            if (g_ish[m].sh) { ID3D11Texture2D_GetDesc(g_ish[m].sh, &td); }
            pos += (size_t)_snprintf_s(top + pos, sizeof top - pos, _TRUNCATE, "%se%d %ux%u %u/%u", r ? ", " : "", m,
                                       td.Width, td.Height, cnt[m], cnt3[m]);
        }
        if (best >= 0) { /* Beleg: Komposition Welt-RT -> Monitor-RT -> BB (Ausschnitt/UV) */
            int ncomp = 0, mon = -1, nbb = 0;
            unsigned monw = 0;
            for (i = 0; i < B->nops && ncomp < 3; i++) {
                const ip_op_t *o = &B->ops[i];
                if (o->kind == IP_DRAW && o->sh_srv && o->sh_srv == g_ish[best].srv && o->sh_rtv != g_ish[best].rtv) {
                    int e = ip_ph_ent_rtv(o->sh_rtv);
                    if (e >= 0 && g_ish[e].sh) { /* Monitor-RT = groesstes Ziel, das das Welt-RT liest */
                        ID3D11Texture2D_GetDesc(g_ish[e].sh, &td);
                        if (td.Width > monw) { monw = td.Width; mon = e; }
                    }
                    ip_ph_dump_draw(B, o, e == B->bbref ? "Welt-RT -> BB" : "Welt-RT -> Monitor-RT", NULL);
                    ncomp++;
                }
            }
            for (i = 0; mon >= 0 && mon != B->bbref && i < B->nops && nbb < 2; i++) {
                const ip_op_t *o = &B->ops[i];
                if (o->kind == IP_DRAW && o->sh_srv && o->sh_srv == g_ish[mon].srv && o->sh_rtv == g_ish[B->bbref].rtv) {
                    float bx[8];
                    ip_ph_dump_draw(B, o, "Monitor-RT -> BB", bx);
                    nbb++;
                    /* Anzeige-Seitenverhaeltnis des Kamerabilds: Pixel auf dem BB je Anteil des Kamerabilds
                     * (UV-Breite = Anteil der Monitor-RT-Breite; UV-Hoehe / Viewport-Anteil des Welt-RT). */
                    if (nbb == 1 && bx[1] > bx[0] && bx[3] > bx[2] && bx[5] > bx[4] && bx[7] > bx[6] && c->w_lh && c->g_h) {
                        float px = (bx[1] - bx[0]) * 0.5f * (float)c->bb_w, py = (bx[3] - bx[2]) * 0.5f * (float)c->bb_h;
                        float fu = bx[5] - bx[4], fv = (bx[7] - bx[6]) / ((float)c->g_h / (float)c->w_lh);
                        float as = (px / fu) / (py / fv);
                        if (as > 0.9f && as < 2.5f) { c->disp_asp = as; }
                    }
                }
            }
        }
        HLOG_I("PHOTO: Zensus Tick f=%u ser=%u ops=%u | BB e%d %ux%u Draws %u (3D %u, 2D %u) Tiefe e%d | Welt-RT e%d %ux%u "
               "logisch %ux%u Spiel-Viewport %u,%u %ux%u Draws %u (3D %u, 2D %u davon mit Clip-w %u, Shading %u) Tiefe e%d | "
               "Projektion a=%.4f b=%.4f A=%.5f B=%.5f (b/a=%.4f) | Monitor zeigt %.4f:1 | RT-Ziele %u: %s",
               c->frame, c->ser, c->nops, c->bb, c->bb_w, c->bb_h, c->n_bb, c->n_bb3d, c->n_bb2d, c->bbd, c->wld, c->w_tw,
               c->w_th, c->w_lw, c->w_lh, c->g_x, c->g_y, c->g_w, c->g_h, c->n_w, c->n_w3d, c->n_w2d, c->n_w2dw, c->n_wrq,
               c->wldd, (double)c->prj[0], (double)c->prj[1], (double)c->prj[2], (double)c->prj[3],
               c->prj[0] > 0.0f ? (double)(c->prj[1] / c->prj[0]) : 0.0, (double)c->disp_asp, c->n_rts, top);
    }
    return c->ok;
}

/* W295 WORLD-REPROJ: Tick B (Liste bi) fuer die Reprojektion vorbereiten (Spiel-Thread, ctx_lock, Immediate):
 * Welt-RT wie der Foto-Zensus (meiste 3D-Draws, >= 8), Draw-ID-Wiedergabe nur der Welt-Ops (t = 1), Bewegungsdaten
 * je Welt-Draw an render_reproj.c. Die naechste Wiedergabe setzt alle Schatten per Startkopie zurueck. */
static sb_rp_op_t *ip_rp_ops;
static unsigned ip_rp_cap;
static void ip_rp_tick(ip_list_t *A, int ai, ip_list_t *B, int bi)
{
    unsigned cnt3[IP_SH_MAX], dcnt[IP_SH_MAX], i, n = 0, last = 0, nshare = 0;
    int k, we = -1, wd = -1;
    ID3D11RenderTargetView *rw;
    ID3D11DepthStencilView *rd;
    ip_rp_ser[bi] = 0;
    ip_rp_we[bi] = ip_rp_wd[bi] = -1;
    if (B->bbref < 0 || !ip_id_ps || !ip_dbg_cb) { return; }
    memset(cnt3, 0, sizeof cnt3);
    memset(dcnt, 0, sizeof dcnt);
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        if (o->kind == IP_DRAW && o->cls && (k = ip_ph_ent_rtv(o->sh_rtv)) >= 0 && k != B->bbref) { cnt3[k]++; }
    }
    { /* Hauptmonitor-Welt = flaechengroesstes RT mit >= 8 3D-Draws (Frontsicht/Sub-Monitore sind kleiner) */
        unsigned ba = 0;
        static int lastwe = -2;
        for (k = 0; k < IP_SH_MAX; k++) {
            D3D11_TEXTURE2D_DESC td;
            unsigned ar;
            if (cnt3[k] < 8u || !g_ish[k].sh) { continue; }
            ID3D11Texture2D_GetDesc(g_ish[k].sh, &td);
            ar = td.Width * td.Height;
            if (we < 0 || ar > ba || (ar == ba && cnt3[k] > cnt3[we])) { we = k; ba = ar; }
        }
        if (we != lastwe && g_cfg.reproj_diag >= 1) {
            static unsigned nl;
            if (nl++ < 40u) { HLOG_I("REPROJ welt f=%u ser=%u: Welt-RT e%d (Flaeche %u, 3D-Draws %u), vorher e%d", B->frame, B->serial, we, ba, we >= 0 ? cnt3[we] : 0u, lastwe); }
            lastwe = we;
        }
    }
    if (we < 0 || !g_ish[we].sh || !g_ish[we].bak[bi] || g_ish[we].bakser[bi] != B->serial) { st_rp_skip[0]++; return; }
    rw = g_ish[we].rtv;
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        if (o->kind == IP_DRAW && o->sh_rtv == rw && (k = ip_ph_ent_dsv(o->sh_dsv)) >= 0) { dcnt[k]++; }
    }
    for (k = 0; k < IP_SH_MAX; k++) { if (dcnt[k] && (wd < 0 || dcnt[k] > dcnt[wd])) { wd = k; } }
    if (wd < 0 || !g_ish[wd].bak[bi] || g_ish[wd].bakser[bi] != B->serial) { st_rp_skip[1]++; return; }
    rd = g_ish[wd].dsv;
    if (ip_rp_cap < B->nops) {
        sb_rp_op_t *p = (sb_rp_op_t *)realloc(ip_rp_ops, B->nops * sizeof *p);
        if (!p) { return; }
        ip_rp_ops = p;
        ip_rp_cap = B->nops;
    }
    { /* Welt-Durchgang = Welt-Ops bis zur ersten Op, die das Welt-RT liest (Komposition ins Monitor-RT). Spaetere
       * Welt-Ops (z. B. weitere Kamera derselben RT-Ressource) laufen wie bisher. */
        unsigned first_rd = 0xFFFFFFFFu, after = 0;
        ID3D11ShaderResourceView *ws = g_ish[we].srv;
        last = 0xFFFFFFFFu;
        for (i = 0; i < B->nops; i++) {
            const ip_op_t *o = &B->ops[i];
            int w = IP_RP_ISW(o, rw, rd);
            int rdw = o->kind == IP_DRAW && ws && (o->sh_srv == ws || (o->rq && (o->rq_shsrv[1] == ws || o->rq_shsrv[2] == ws || o->rq_shsrv[3] == ws)));
            if (rdw && !w && first_rd == 0xFFFFFFFFu) { first_rd = i; }
            if (w) { if (first_rd == 0xFFFFFFFFu) { last = i; } else { after++; } }
            else if (first_rd == 0xFFFFFFFFu && o->sh_dsv == rd && o->kind != IP_CLR_RTV) { nshare++; } /* Welt-Tiefe fremd */
        }
        if (last == 0xFFFFFFFFu) { st_rp_skip[2]++; return; }
        if (g_cfg.reproj_diag >= 1) {
            static unsigned nl2;
            if (nl2++ < 6u) {
                HLOG_I("REPROJ zensus ser=%u Welt e%d Tiefe e%d: Welt-Durchgang bis Op %u, Komposition Op %u, spaetere Welt-Ops %u, "
                       "Tiefe fremd benutzt %u, ops %u", B->serial, we, wd, last, first_rd, after, nshare, B->nops);
            }
        }
    }
    for (i = 0; i <= last; i++) {
        const ip_op_t *o = &B->ops[i];
        sb_rp_op_t *r;
        if (o->kind != IP_DRAW || !IP_RP_ISW(o, rw, rd)) { continue; }
        r = &ip_rp_ops[n++];
        memset(r, 0, sizeof *r);
        r->idx = i;
        r->lerp = B->pair_ok && o->flag && o->match >= 0 && (unsigned)o->match < A->nops;
        r->same = r->lerp && (o->rzb || A->ops[o->match].rawhash == o->rawhash);
        r->stat = o->why == IP_W_STATIC;
        r->flat = !o->cls && o->rn[0][2] > 0.999f && o->rn[0][2] < 1.001f && o->rn[3][2] > 0.999f && o->rn[3][2] < 1.001f;
        r->pre = o->cls && o->rn[0][2] > 0.999f && o->rn[0][2] < 1.001f && o->rn[3][2] > 0.999f && o->rn[3][2] < 1.001f;
        r->nv = o->nv;
        memcpy(r->mvp, o->mvp, sizeof r->mvp);
        memcpy(r->pref, o->pref, sizeof r->pref);
        if (r->lerp) {
            const ip_op_t *a = &A->ops[o->match];
            memcpy(r->amvp, IP_AMVP(o, a), sizeof r->amvp); /* W317 rzm */
            memcpy(r->apref, o->rzb ? o->pref : a->pref, sizeof r->apref); /* rzb: Tick-N-Vertices mit Matrix N-1 */
        }
        r->vp[0] = o->vp.TopLeftX; r->vp[1] = o->vp.TopLeftY; r->vp[2] = o->vp.Width; r->vp[3] = o->vp.Height;
        r->vp[4] = o->vp.MinDepth; r->vp[5] = o->vp.MaxDepth;
    }
    /* exakte Teil-Wiedergabe (t = 1) bis zur letzten Welt-Op: Farbe + Tiefe des Welt-Durchgangs; danach dieselbe mit
     * dem Draw-ID-Shader. Die naechste Wiedergabe setzt alle Schatten per Startkopie zurueck. */
    ip_rp_stop = last;
    ip_rp_idrtv = rw;
    ip_rp_prep = 1;
    ip_replay_ex(B, bi, A, ai, 1.0f, 0, ip_cx_imm());
    ip_rp_prep = 0;
    if (!g_ish[wd].sh || !sb_rp_begin(bi, g_ish[we].sh, g_ish[wd].sh)) { ip_rp_idrtv = NULL; st_rp_skip[3]++; return; }
    ip_rp_prep = 2;
    ip_replay_ex(B, bi, A, ai, 1.0f, 2, ip_cx_imm());
    ip_rp_prep = 0;
    ip_rp_idrtv = NULL;
    if (!sb_rp_tick(bi, B->serial, ip_rp_ops, n, B->nops, g_ish[we].sh)) { return; }
    ip_rp_we[bi] = we;
    ip_rp_wd[bi] = wd;
    ip_rp_last[bi] = last;
    ip_rp_ser[bi] = B->serial;
}

/* Swap N (sb_render_present, vor dem Present). 1 = Present uebernommen. */

/* ---------- DIAG(W376 LOC-LOCAL-AIM) loc_aim_diag ----------
 * Bit 2: je aufgezeichnetem Tick-Bild die Kamera-Matrix der Welt im Backbuffer (haeufigste mvp der Draws vor der
 * ersten BB-Kopie) nach locaim_cam.bin. Bit 8: Draw-Zensus (jedes 200. Bild, hoechstens 8) nach locaim_census.txt
 * mit M = mvp * Kamera^-1 (affin = Welt-Objekt; Verschiebung relativ zum Auge). Nur Spielbild, keine Eingaben. */
/* Weltziel der Liste (Messlaeufe W376_A1/A4): vor dem Absetzen zeichnet LoC die Aussenwelt in den Backbuffer, danach in
 * ein eigenes Render-Ziel (Hauptsicht), das das Cockpit im Backbuffer als Fenster-Textur zeigt; der Lock-On-Monitor
 * folgt als weiteres Ziel. Je Ziel folgt auf die Welt eine Kopie dieses Ziels (W340 IP_COPY: Glow-Kette), danach
 * HUD/2D. Weltziel = erstes Ziel (Reihenfolge der Ops) mit >= 32 3D-Draws vor seiner ersten Kopie; split = diese Kopie.
 * Kein solches Ziel: NULL (keine Welt, keine Korrektur). */
static ID3D11RenderTargetView *la_world_rtv(const ip_list_t *L, unsigned *split)
{
    enum { NT = 8 };
    ID3D11RenderTargetView *tg[NT];
    unsigned cnt[NT], nt = 0, i, k;
    for (i = 0; i < L->nops; i++) {
        const ip_op_t *o = &L->ops[i];
        if (o->kind == IP_DRAW && o->cls && o->nv >= 3u && o->rtv) {
            for (k = 0; k < nt && tg[k] != o->rtv; k++) {}
            if (k == nt) { if (nt == NT) { continue; } tg[nt] = o->rtv; cnt[nt] = 0; nt++; }
            cnt[k]++;
        } else if (o->kind == IP_COPY && o->cps) {
            for (k = 0; k < nt && tg[k] != o->cps; k++) {}
            if (k < nt && cnt[k] >= 32u) { *split = i; return tg[k]; }
            if (k < nt) { cnt[k] = 0; } /* kleine Gruppe (z. B. Kartenfenster): weiter suchen */
        }
    }
    *split = 0;
    return NULL;
}
static int la_cam_of(const ip_list_t *L, float cam[16], unsigned *cnt, unsigned *split)
{
    enum { NC = 64 };
    unsigned cidx[NC], cn[NC], nc = 0, i, k, best = 0, sp = L->nops;
    ID3D11RenderTargetView *wt = la_world_rtv(L, &sp);
    if (!wt) { return 0; }
    for (i = 0; i < sp; i++) {
        const ip_op_t *o = &L->ops[i];
        if (o->kind != IP_DRAW || o->rtv != wt || o->nv < 3u || !o->cls) { continue; }
        for (k = 0; k < nc; k++) { if (memcmp(L->ops[cidx[k]].mvp, o->mvp, 64) == 0) { cn[k]++; break; } }
        if (k == nc && nc < NC) { cidx[nc] = i; cn[nc] = 1; nc++; }
    }
    if (!nc) { return 0; }
    for (k = 1; k < nc; k++) { if (cn[k] > cn[best]) { best = k; } }
    memcpy(cam, L->ops[cidx[best]].mvp, 64);
    *cnt = cn[best];
    *split = sp;
    return 1;
}

static void li_aim_diag(const ip_list_t *L)
{
    static FILE *fc, *fz;
    static unsigned nl, ncz;
    float cam[16], ci[16], M[16];
    unsigned cnt = 0, sp = 0, i;
    nl++;
    if (!la_cam_of(L, cam, &cnt, &sp)) {
        static unsigned nfail;
        nfail++;
        if ((g_cfg.loc_aim_diag & 8) && (nfail == 1u || nfail == 100u || nfail == 1000u || nfail == 3000u)) { /* Zensus ohne Kamera */
            if (!fz) { fz = (FILE *)loc_aim_diag_file("locaim_census.txt"); }
            if (fz) {
                fprintf(fz, "NOCAM frame=%u serial=%u nops=%u g_rtv=%p\n", L->frame, L->serial, L->nops, (void *)g_rtv);
                for (i = 0; i < L->nops; i++) {
                    const ip_op_t *o = &L->ops[i];
                    fprintf(fz, "%u k=%u rtv=%p cps=%p cls=%u nv=%u\n", i, o->kind, (void *)o->rtv, (void *)o->cps, o->cls, o->nv);
                }
                fflush(fz);
            }
        }
        return;
    }
    if (g_cfg.loc_aim_diag & 2) {
        unsigned hdr[12];
        float pv[4];
        loc_aim_pub_t pb;
        if (!fc) { fc = (FILE *)loc_aim_diag_file("locaim_cam.bin"); }
        if (fc) {
            loc_aim_peek(&pb);
            hdr[0] = 0x3241434Cu; hdr[1] = L->frame; hdr[2] = L->serial; hdr[3] = (unsigned)L->tswap;
            hdr[4] = cnt; hdr[5] = L->nops; hdr[6] = sp; hdr[7] = (unsigned)(L->tswap >> 32);
            hdr[8] = pb.seq; hdr[9] = pb.gframe; hdr[10] = hooks_frame_count(); hdr[11] = (unsigned)pb.n;
            pv[0] = pb.ys; pv[1] = pb.ps; pv[2] = pb.ax; pv[3] = pb.ay;
            fwrite(hdr, sizeof hdr, 1, fc);
            fwrite(cam, 64, 1, fc);
            fwrite(pv, sizeof pv, 1, fc);
            fflush(fc);
        }
    }
    if ((g_cfg.loc_aim_diag & 8) && ip_m4inv(ci, cam) &&
        (g_cfg.loc_aim_diag_n > 0 ? (L->serial >= (unsigned)g_cfg.loc_aim_diag_from &&
                                     L->serial < (unsigned)(g_cfg.loc_aim_diag_from + g_cfg.loc_aim_diag_n) && (L->serial & 3u) == 0u)
                                  : ((nl % 200u) == 0u && ncz < 8u))) { /* Fenster (Serial, jede 4. Liste) bzw. jede 200. */
        if (!fz) { fz = (FILE *)loc_aim_diag_file("locaim_census.txt"); }
        if (!fz) { return; }
        ncz++;
        fprintf(fz, "LIST frame=%u serial=%u nops=%u split=%u camcnt=%u cam=", L->frame, L->serial, L->nops, sp, cnt);
        for (i = 0; i < 16; i++) { fprintf(fz, "%.6g%s", cam[i], i < 15 ? "," : "\n"); }
        for (i = 0; i < L->nops; i++) {
            const ip_op_t *o = &L->ops[i];
            float aff;
            if (o->kind != IP_DRAW) { fprintf(fz, "%u k=%u\n", i, o->kind); continue; }
            ip_m4mul(M, o->mvp, ci);
            aff = fabsf(M[3]) + fabsf(M[7]) + fabsf(M[11]) + fabsf(M[15] - 1.0f);
            fprintf(fz, "%u bb=%d rt=%p cls=%u why=%u fl=%u nv=%u tex=%u eq=%d aff=%.3g T=%.3f,%.3f,%.3f s=%.3f rn0=%.3f,%.3f,%.3f\n",
                    i, o->rtv == g_rtv, (void *)o->sh_rtv, o->cls, o->why, o->flag, o->nv, o->has_tex,
                    memcmp(o->mvp, cam, 64) == 0, aff, M[12], M[13], M[14],
                    sqrtf(M[0] * M[0] + M[1] * M[1] + M[2] * M[2]), o->rn[0][0], o->rn[0][1], o->rn[0][2]);
        }
        fflush(fz);
    }
}

/* W394 LOC-HUD-VIEW: je anderem VT simulierte Bildlage a = proj_VP(p) und angezeigte Lage c = [q, 1] * VP mit
 * q = K * X_j * p (W387-Versatz X_j, W390-Kamera K; jeweils nur wenn aktiv). VP = simulierte Kamera der Liste. */
static void li_hud_data(ip_list_t *L, const double VP[16], const loc_smooth_pub_t *pb, const double *K, double X[][16],
                        unsigned objon)
{
    int j, k;
    L->hm_ok = 0;
    L->hm_gok = pb->own >= 0 && pb->own < LS_MAXV && pb->v[pb->own].ok;
    L->hm_gier = L->hm_gok ? pb->v[pb->own].yaw : 0.0f;
    L->hm_dyaw = (K && L->hm_gok && pb->v[pb->own].on) ? pb->v[pb->own].off[3] : 0.0f;
    for (j = 0; j < LS_MAXV; j++) {
        double p[3], q[3], a[4];
        if (j == pb->own || !pb->v[j].ok) { L->hm_c[j][3] = 0.0; continue; }
        for (k = 0; k < 3; k++) { p[k] = pb->v[j].p[k]; }
        for (k = 0; k < 4; k++) { a[k] = p[0] * VP[k] + p[1] * VP[4 + k] + p[2] * VP[8 + k] + VP[12 + k]; }
        if (a[3] < 1e-3) { L->hm_c[j][3] = 0.0; continue; }
        L->hm_a[j][0] = (float)(a[0] / a[3]);
        L->hm_a[j][1] = (float)(a[1] / a[3]);
        memcpy(q, p, sizeof q);
        if (objon & (1u << j)) { ls_xform_pt(q, q, X[j]); }
        if (K) { ls_xform_pt(q, q, K); }
        for (k = 0; k < 4; k++) { L->hm_c[j][k] = q[0] * VP[k] + q[1] * VP[4 + k] + q[2] * VP[8 + k] + VP[12 + k]; }
        L->hm_ok = 1;
    }
}

/* W394: ohne Versatz (li_smooth_apply ohne Wirkung): Daten aus dem juengsten Stand, simulierte Kamera der Liste. */
static void li_hud_plain(ip_list_t *L)
{
    loc_smooth_pub_t pb;
    float cam[16];
    double VP[16];
    unsigned cnt = 0, sp = 0, i;
    if (!loc_smooth_peek(&pb) || !la_cam_of(L, cam, &cnt, &sp) || cnt < 8u) { return; }
    for (i = 0; i < 16; i++) { VP[i] = cam[i]; }
    li_hud_data(L, VP, &pb, NULL, NULL, 0u);
}

/* ---------- DIAG(W412 LOC-WORLD-LAG) loc_wlag_diag Bit 1 ----------
 * Je Liste (am Swap): Blickrichtung (Gier) der simulierten Kamera VP und der gezeigten Kamera (VP mit K, W390) aus dem
 * Augpunkt und der Bildmitte; Vergleich der gezeigten Gier eines Ticks mit der simulierten Gier s Ticks spaeter
 * (s = -3..+4): kleinster Fehler bei s = Vorsprung. Nach der Zuordnung: Draws des Weltziels je Klasse (o->wlc) und ob sie
 * in den Zwischenbildern gelerpt (mit der Kamera) oder exakt aus Tick N stehen. Zeile LOCWLAG alle 10 s (nur Zaehler,
 * Winkel, keine Eingaben). */
#define WL_SH 8 /* Verschiebungen s = -3..+4 */
#define WL_RING 16u
static struct {
    unsigned ok[WL_RING], gt[WL_RING];
    double ysim[WL_RING], ydisp[WL_RING];
} wl_r;
static unsigned wl_lists, wl_cam, wl_turn, wl_agree, wl_oppo, wl_errn[WL_SH];
static double wl_lead_sum, wl_lead_abs, wl_rate_sum, wl_err[WL_SH], wl_lead_max;
static unsigned wl_cls_n[7], wl_cls_v[7], wl_cls_ex[7], wl_cls_exv[7], wl_cls_cm[7]; /* Draws, Vertices, davon exakt, Kamera-Lerp */
static unsigned wl_cls_lists;
static unsigned wl_why[4][IP_W_N + 2]; /* Welt mit K: Gruppe 0 Netz >= 1000 Vertices, 1 untexturiert <= 32 (Linien/Pfosten), 2 sonst, 3 Himmel (W435);
                                         * Spalte IP_W_N = davon rzb (Tick-N-Vertices per Matrix), IP_W_N + 1 = rzm (mit der Kamera) */
static double wl_cls_lagdeg[7]; /* Summe Versatz der Klasse gegen die gezeigte Kamera (Grad) */
static LONGLONG wl_tlog;
static double wl_yaw_of(const double e[3], const double f[3])
{
    return atan2(f[0] - e[0], f[2] - e[2]);
}
static double wl_wrap(double a)
{
    while (a > 3.14159265358979) { a -= 6.28318530717959; }
    while (a < -3.14159265358979) { a += 6.28318530717959; }
    return a;
}
static double wl_last_lead; /* Gier gezeigt - simuliert der juengsten Liste (rad) */
static void wl_cam_note(const double VPi[16], const double *K, uint32_t gtick)
{
    double e[3], f[3], q4[4], e2[3], f2[3], Ki[16], ys, yd, rate = 0.0;
    unsigned k, s, slot;
    int have_prev;
    wl_lists++;
    if (!ls_eye_of(e, VPi)) { return; }
    for (k = 0; k < 4; k++) { q4[k] = 0.5 * VPi[8 + k] + VPi[12 + k]; }
    if (fabs(q4[3]) < 1e-12) { return; }
    for (k = 0; k < 3; k++) { f[k] = q4[k] / q4[3]; }
    ys = wl_yaw_of(e, f);
    yd = ys;
    if (K && la_m4inv_d(Ki, K)) { /* gezeigt: Weltpunkt q erscheint, wo die simulierte Kamera q * K sieht -> Auge e * K^-1 */
        ls_xform_pt(e2, e, Ki);
        ls_xform_pt(f2, f, Ki);
        yd = wl_yaw_of(e2, f2);
        wl_cam++;
    }
    wl_last_lead = wl_wrap(yd - ys);
    slot = gtick % WL_RING;
    have_prev = wl_r.ok[(gtick - 1u) % WL_RING] && wl_r.gt[(gtick - 1u) % WL_RING] == gtick - 1u;
    if (have_prev) { rate = wl_wrap(ys - wl_r.ysim[(gtick - 1u) % WL_RING]); }
    wl_r.ok[slot] = 1; wl_r.gt[slot] = gtick; wl_r.ysim[slot] = ys; wl_r.ydisp[slot] = yd;
    if (!have_prev || fabs(rate) < 0.1 * 3.14159265358979 / 180.0) { return; } /* nur gleichmaessiges Drehen (>= 0,1 Grad/Tick) */
    wl_turn++;
    wl_rate_sum += fabs(rate);
    wl_lead_sum += wl_last_lead * (rate > 0.0 ? 1.0 : -1.0); /* positiv = in Drehrichtung voraus */
    wl_lead_abs += fabs(wl_last_lead);
    if (fabs(wl_last_lead) > wl_lead_max) { wl_lead_max = fabs(wl_last_lead); }
    if (fabs(wl_last_lead) > 0.02 * 3.14159265358979 / 180.0) {
        if ((wl_last_lead > 0.0) == (rate > 0.0)) { wl_agree++; } else { wl_oppo++; }
    }
    for (s = 0; s < WL_SH; s++) { /* gezeigt bei Tick g gegen simuliert bei g + (s - 3) */
        int sh = (int)s - 3;
        uint32_t gd = sh >= 0 ? gtick - (uint32_t)sh : gtick, gs = sh >= 0 ? gtick : gtick + (uint32_t)(-sh);
        unsigned id = gd % WL_RING, is = gs % WL_RING;
        if (sh < 0) { /* simuliert spaeter als jetzt: gezeigt von frueher (g + sh) gegen simuliert jetzt */
            gd = gtick - (uint32_t)(-sh); gs = gtick; id = gd % WL_RING; is = gs % WL_RING;
            if (!(wl_r.ok[id] && wl_r.gt[id] == gd)) { continue; }
            wl_err[s] += fabs(wl_wrap(wl_r.ysim[id] - wl_r.ydisp[is])); /* simuliert frueher gegen gezeigt jetzt */
            wl_errn[s]++;
            continue;
        }
        if (!(wl_r.ok[id] && wl_r.gt[id] == gd && wl_r.ok[is] && wl_r.gt[is] == gs)) { continue; }
        wl_err[s] += fabs(wl_wrap(wl_r.ydisp[id] - wl_r.ysim[is]));
        wl_errn[s]++;
    }
}

/* Nach der Zuordnung: Klassen im Weltziel, gelerpt oder exakt (nur Listen mit Drehung bzw. Vorhersage) */
static void li_wlag_after(const ip_list_t *L)
{
    unsigned i;
    int any = 0;
    LONGLONG now = ip_now();
    for (i = 0; i < L->nops; i++) {
        const ip_op_t *o = &L->ops[i];
        unsigned c = o->wlc;
        if (o->kind != IP_DRAW || !c || c > 6u) { continue; }
        any = 1;
        wl_cls_n[c]++;
        wl_cls_v[c] += o->nv;
        if (!o->flag) { wl_cls_ex[c]++; wl_cls_exv[c] += o->nv; }
        else if (o->rzm) { wl_cls_cm[c]++; }
        if (c == 2u || c == 3u || c == 5u) { wl_cls_lagdeg[c] += fabs(wl_last_lead) * 180.0 / 3.14159265358979; }
        if (c == 1u || c == 5u) {
            unsigned g = o->nv >= 1000u ? 0u : ((!o->has_tex && o->nv <= 32u) ? 1u : 2u);
            wl_why[g][o->why < IP_W_N ? o->why : 0]++;
            if (o->flag && o->rzb) { wl_why[g][IP_W_N]++; }
            if (o->flag && o->rzm) { wl_why[g][IP_W_N + 1]++; }
        }
        if (c == 6u || o->lsk) { /* W435: Himmel (mit K oder nur erkannt) */
            wl_why[3][o->why < IP_W_N ? o->why : 0]++;
            if (o->flag && o->rzb) { wl_why[3][IP_W_N]++; }
            if (o->flag && o->rzm) { wl_why[3][IP_W_N + 1]++; }
        }
    }
    if (any) { wl_cls_lists++; }
    if (!wl_tlog) { wl_tlog = now; }
    if (ip_ms(wl_tlog, now) >= 10000.0) {
        double dg = 180.0 / 3.14159265358979, tn = wl_turn ? (double)wl_turn : 1.0, cl = wl_cls_lists ? (double)wl_cls_lists : 1.0;
        unsigned s, best = 0;
        char eb[256];
        int n = 0;
        for (s = 0; s < WL_SH; s++) {
            double m = wl_errn[s] ? wl_err[s] / wl_errn[s] * dg : -1.0;
            n += _snprintf_s(eb + n, sizeof eb - (size_t)n, _TRUNCATE, "%s%+d:%.3f", s ? " " : "", (int)s - 3, m);
            if (wl_errn[s] && (!wl_errn[best] || wl_err[s] / wl_errn[s] < wl_err[best] / wl_errn[best])) { best = s; }
        }
        HLOG_I("LOCWLAG: %.1fs Listen=%u mit Kamera K=%u Drehung=%u (Rate %.3f Grad/Tick) | gezeigt-simuliert Mittel %+.3f "
               "(|.| %.3f, max %.3f) Grad, Richtung mit/gegen Drehung=%u/%u | Fehler gezeigt(g) gegen simuliert(g+s) Grad %s -> "
               "bester s=%+d",
               ip_ms(wl_tlog, now) / 1000.0, wl_lists, wl_cam, wl_turn, wl_rate_sum / tn * dg, wl_lead_sum / tn * dg,
               wl_lead_abs / tn * dg, wl_lead_max * dg, wl_agree, wl_oppo, eb, (int)best - 3);
        HLOG_I("LOCWLAG klassen: Listen=%u je Liste Draws/Vertices/exakt/Kamera-Lerp | WeltK %.1f/%.0f/%.1f/%.1f | eigen %.1f/%.0f/%.1f/%.1f "
               "(Versatz %.3f Grad) | nicht-affin %.1f/%.0f/%.1f/%.1f (Versatz %.3f Grad) | VT %.1f/%.0f/%.1f/%.1f | Welt-ohne-K %.1f/%.0f/%.1f/%.1f "
               "| Himmel-mit-K %.1f/%.0f/%.1f/%.1f",
               wl_cls_lists, wl_cls_n[1] / cl, wl_cls_v[1] / cl, wl_cls_ex[1] / cl, wl_cls_cm[1] / cl, wl_cls_n[2] / cl,
               wl_cls_v[2] / cl, wl_cls_ex[2] / cl, wl_cls_cm[2] / cl, wl_cls_n[2] ? wl_cls_lagdeg[2] / wl_cls_n[2] : 0.0,
               wl_cls_n[3] / cl, wl_cls_v[3] / cl, wl_cls_ex[3] / cl, wl_cls_cm[3] / cl,
               wl_cls_n[3] ? wl_cls_lagdeg[3] / wl_cls_n[3] : 0.0, wl_cls_n[4] / cl, wl_cls_v[4] / cl, wl_cls_ex[4] / cl,
               wl_cls_cm[4] / cl, wl_cls_n[5] / cl, wl_cls_v[5] / cl, wl_cls_ex[5] / cl, wl_cls_cm[5] / cl,
               wl_cls_n[6] / cl, wl_cls_v[6] / cl, wl_cls_ex[6] / cl, wl_cls_cm[6] / cl);
        for (s = 0; s < 4u; s++) {
            const unsigned *w = wl_why[s];
            HLOG_I("LOCWLAG why: Welt %s je Liste static=%.1f lerp=%.1f (rzb %.1f, rzm %.1f) rawlerp=%.1f nomatch=%.1f rawchg=%.1f reject=%.1f",
                   s == 0u ? "Netz>=1000" : (s == 1u ? "Linien/Pfosten" : (s == 2u ? "Objekte" : "Himmel")), w[IP_W_STATIC] / cl, w[IP_W_LERP] / cl,
                   w[IP_W_N] / cl, w[IP_W_N + 1] / cl, w[IP_W_RAWLERP] / cl, w[IP_W_NOMATCH] / cl, w[IP_W_RAWCHG] / cl,
                   w[IP_W_REJECT] / cl);
        }
        memset(wl_why, 0, sizeof wl_why);
        wl_lists = wl_cam = wl_turn = wl_agree = wl_oppo = 0;
        wl_lead_sum = wl_lead_abs = wl_rate_sum = wl_lead_max = 0.0;
        memset(wl_err, 0, sizeof wl_err); memset(wl_errn, 0, sizeof wl_errn);
        memset(wl_cls_n, 0, sizeof wl_cls_n); memset(wl_cls_v, 0, sizeof wl_cls_v); memset(wl_cls_ex, 0, sizeof wl_cls_ex);
        memset(wl_cls_exv, 0, sizeof wl_cls_exv); memset(wl_cls_cm, 0, sizeof wl_cls_cm); memset(wl_cls_lagdeg, 0, sizeof wl_cls_lagdeg);
        wl_cls_lists = 0;
        wl_tlog = now;
    }
}

/* ---------- DIAG(W415 LOC-STATIC-LAG) loc_stat_diag Bit 1 ----------
 * Nach der Zuordnung (je Tick-Paar A -> B): Bezug = das Gelaende (groesster Welt-Draw mit Tiefe im Hauptziel). Seine
 * Bildlage im Zwischenbild t ist c_B * T(t) mit T(t) = lerp(M_B^-1 * M_A', I, t) (clip-linear, wie der Interp-VS; M_A' =
 * Matrix N-1 bzw. rzm). Fuer jeden anderen Welt-Draw im selben Ziel (perspektivisch, mit Tiefe) und jeden seiner 4
 * Bezugs-Vertices: gezeigte Lage (gelerpt lerp(c_A, c_B, t) bzw. exakt c_B) gegen die Lage, an der das Gelaende
 * denselben Weltpunkt zeigt (c_B * T(t)), t = 1/6 .. 5/6 (120 FPS bei 20 Hz). Ruhende Objekte (Masten, Pfosten,
 * Gebaeude) muessen 0 px zeigen; bewegte (VTs, Geschosse) zeigen ihre eigene Bewegung. Zeile LOCSTAT alle 10 s je
 * Gruppe (Netz >= 1000, Linien/Pfosten untexturiert <= 32, Objekte) und Art (exakt, Matrix-Lerp, rzb, rzm, Vertex-Lerp):
 * Draws, max/p95 Versatz in Pixeln (Bildgroesse g_bb_w x g_bb_h). Bit 2: groesste Einzel-Draws (LOCSTAT top). Nur Zahlen. */
#define LS15_G 3
#define LS15_S 5
#define LS15_CAP 16384u
static float ls15_v[LS15_G][LS15_S][LS15_CAP];
static unsigned ls15_n[LS15_G][LS15_S], ls15_nd[LS15_G][LS15_S], ls15_big[LS15_G][LS15_S];
static double ls15_mx[LS15_G][LS15_S];
static unsigned ls15_lists, ls15_turn, ls15_noterr, ls15_terr_raw, ls15_law, ls15_ncons, ls15_nocons, ls15_ln, ls15_pln;
static double ls15_dev_sum[2], ls15_dev_max[2]; /* 0 = groesstes Netz, 1 = W410-Kamera (rzm) gegen die Mehrheits-Kamera, px */
static unsigned ls15_dev_n[2], ls15_dev_big[2];
static double ls15_cam_sum, ls15_cam_max;
static LONGLONG ls15_tlog;
typedef struct { float px; unsigned f, i, nv, tex, why, st, wlc, g; float x, y; } ls15_top_t;
static ls15_top_t ls15_top[8];
static int ls15_fcmp(const void *x, const void *y)
{
    float a = *(const float *)x, b = *(const float *)y;
    return a < b ? -1 : (a > b);
}
static int ls15_ndc(const float c[4], double *x, double *y)
{
    if (c[3] <= 1e-3f) { return 0; }
    *x = (double)c[0] / (double)c[3];
    *y = (double)c[1] / (double)c[3];
    return 1;
}
static void ls15_vmul(float out[4], const float c[4], const double D[16])
{
    int j;
    for (j = 0; j < 4; j++) {
        out[j] = (float)(c[0] * D[0 * 4 + j] + c[1] * D[1 * 4 + j] + c[2] * D[2 * 4 + j] + c[3] * D[3 * 4 + j]);
    }
}
/* Tickweg (waagrecht, px) der Bildmitte in Tiefe 0,99 unter D (Clip, Tick N -> N-1) */
static float ls15_cmove(const double D[16], double hw)
{
    float c[4] = { 0.0f, 0.0f, 0.99f, 1.0f }, d[4];
    ls15_vmul(d, c, D);
    if (d[3] <= 1e-6f) { return -999.0f; }
    return (float)((double)d[0] / (double)d[3] * hw);
}
static void li_stat_after(const ip_list_t *A, const ip_list_t *B)
{
    static const float ts[5] = { 1.0f / 6.0f, 2.0f / 6.0f, 3.0f / 6.0f, 4.0f / 6.0f, 5.0f / 6.0f };
    const double hw = 0.5 * (double)(g_bb_w ? g_bb_w : 640u), hh = 0.5 * (double)(g_bb_h ? g_bb_h : 480u);
    const ip_op_t *tr = NULL;
    double Di[16], cam = 0.0;
    unsigned i, q, k, g, s;
    int ti = -1;
    LONGLONG now = ip_now();
    if (A && B && B->pair_ok) {
        for (i = 0; i < B->nops; i++) { /* Gelaende = groesster perspektivische Welt-Draw mit Tiefe */
            const ip_op_t *o = &B->ops[i];
            if (o->kind != IP_DRAW || !o->cls || !o->dsv || ip_mvp_affine(o->mvp) || o->nv < 1000u) { continue; }
            if (!tr || o->nv > tr->nv) { tr = o; ti = (int)i; }
        }
        if (!tr) { ls15_noterr++; }
        else {
            /* Kamera = Mehrheit der Welt-Draws: fuer jeden ruhenden Welt-Draw (Matrix W*C*P) ist M_B^-1*M_A = P^-1*C_B^-1*C_A*P
             * unabhaengig von seiner Weltmatrix W -> haeufigstes D unter den gelerpten, bewegten Matrix-Draws des Ziels
             * (gezaehlt je eigener Matrix; Teile mit gleicher Matrix zaehlen einmal). */
            enum { NC = 16 };
            static double cD[NC][16];
            unsigned cn[NC], ncl = 0, bestc = 0;
            const float *clast[NC];
            const ip_op_t *ta = (tr->match >= 0 && (unsigned)tr->match < A->nops) ? &A->ops[tr->match] : NULL;
            double Dt[16], Dw[16], Dwc[16];
            int dtok = 0, dwok = 0, wcok = 0;
            static float mv[512];
            unsigned nmv = 0;
            for (i = 0; i < B->nops; i++) {
                const ip_op_t *o = &B->ops[i];
                const ip_op_t *a = (o->match >= 0 && (unsigned)o->match < A->nops) ? &A->ops[o->match] : NULL;
                double Dx[16];
                unsigned c;
                if (o->kind != IP_DRAW || !o->cls || !o->dsv || o->rtv != tr->rtv || !o->flag || !a || o->why != IP_W_LERP ||
                    o->rzb || o->rzm || ip_mvp_affine(o->mvp) || memcmp(a->mvp, o->mvp, sizeof o->mvp) == 0) { continue; }
                if (!zc_dmat(Dx, o->mvp, a->mvp)) { continue; }
                if (nmv < 512u) { mv[nmv++] = ls15_cmove(Dx, hw); }
                for (c = 0; c < ncl; c++) { if (zc_derr(Dx, cD[c]) <= 1e-3) { break; } }
                if (c == ncl) {
                    if (ncl >= NC) { continue; }
                    memcpy(cD[c], Dx, sizeof Dx); cn[c] = 0; clast[c] = NULL; ncl++;
                }
                if (!clast[c] || memcmp(clast[c], o->mvp, sizeof o->mvp) != 0) { cn[c]++; clast[c] = o->mvp; }
            }
            for (k = 1; k < ncl; k++) { if (cn[k] > cn[bestc]) { bestc = k; } }
            if (tr->flag && ta) {
                dtok = zc_dmat(Dt, tr->mvp, IP_AMVP(tr, ta)); /* M_B^-1 * M_A' des groessten Netzes */
                if (tr->why == IP_W_RAWLERP && !tr->rzb) { ls15_terr_raw++; } /* Vertex-Lerp: Matrizen allein beschreiben es nicht */
            } else if (!tr->flag) {
                for (k = 0; k < 16; k++) { Dt[k] = (k % 5) == 0 ? 1.0 : 0.0; }
                dtok = 1;
            }
            rz_cam_pre(A, B); /* W410-Kamera (haeufigstes Matrix-Paar, rzm) zum Vergleich */
            dwok = w410_cam_inv(A, B, tr, Dw);
            wcok = w415_world_cam(A, B, tr, Dwc, NULL); /* W415 Welt-Kamera (Bezug, auch ohne Fix) */
            if ((g_cfg.loc_stat_diag & 16) && ls15_pln < 400u && (g_cfg.pf_ring <= 0 || sb_pf_active())) {
                /* DIAG(W415): Tickweg der Bildmitte (waagrecht, px, Tick N-1 -> N gesehen von N) je Kamera-Kandidat */
                float ca[16], cb2[16];
                unsigned cnt = 0, sp = 0;
                double Dl[16];
                int lok = la_cam_of(A, ca, &cnt, &sp) && la_cam_of(B, cb2, &cnt, &sp) && zc_dmat(Dl, cb2, ca);
                if (nmv) { qsort(mv, nmv, sizeof(float), ls15_fcmp); }
                ls15_pln++;
                HLOG_I("LOCSTAT paar: f=%u Welt=%.2f W410=%.2f Mehrheit=%.2f (%u) groesstes=%.2f (n=%u why=%s rzm=%d) la_cam=%.2f | gelerpte Matrix-Draws %u: "
                       "p10 %.2f p50 %.2f p90 %.2f", B->frame, wcok ? ls15_cmove(Dwc, hw) : -999.0, dwok ? ls15_cmove(Dw, hw) : -999.0, ncl ? ls15_cmove(cD[bestc], hw) : -999.0,
                       ncl ? cn[bestc] : 0u, dtok ? ls15_cmove(Dt, hw) : -999.0, tr->nv, pr_why(tr->why), tr->rzm,
                       lok ? ls15_cmove(Dl, hw) : -999.0, nmv, nmv ? mv[nmv / 10u] : 0.0f, nmv ? mv[nmv / 2u] : 0.0f,
                       nmv ? mv[(nmv * 9u) / 10u] : 0.0f);
            }
            /* Bezug: Kamera der Welt-Draws in Weltkoordinaten (haeufigstes gleiches Matrix-Paar, W410/W294; damit bewegt rzm
             * das Gelaende). Bit 4: stattdessen die Mehrheit gleicher D (Messlauf W415_R2b: das sind die Teile des eigenen VT). */
            if ((g_cfg.loc_stat_diag & 4) && ncl && cn[bestc] >= 3u) { memcpy(Di, cD[bestc], sizeof Di); ls15_ncons += cn[bestc]; }
            else if (wcok) { memcpy(Di, Dwc, sizeof Di); ls15_ncons++; }
            else if (dwok) { memcpy(Di, Dw, sizeof Di); ls15_nocons++; }
            else if (dtok) { memcpy(Di, Dt, sizeof Di); ls15_nocons++; }
            else { ls15_noterr++; tr = NULL; }
            if (tr) { /* Abweichung groesstes Netz bzw. Mehrheits-D gegen den Bezug, Probepunkte im Bild (Tiefe 0,99) bei t = 1/6 */
                int px, py, w;
                for (w = 0; w < 2; w++) {
                    const double *Dq = w ? cD[bestc] : Dt;
                    double mx = 0.0;
                    if (!(w ? (ncl > 0u) : dtok)) { continue; }
                    for (px = -1; px <= 1; px++) {
                        for (py = -1; py <= 1; py++) {
                            float cb[4] = { 0.6f * (float)px, 0.6f * (float)py, 0.99f, 1.0f }, c1[4], c2[4], e1[4], e2[4];
                            double x1, y1, x2, y2, d;
                            ls15_vmul(c1, cb, Di); ls15_vmul(c2, cb, Dq);
                            for (k = 0; k < 4; k++) { e1[k] = c1[k] + (cb[k] - c1[k]) * ts[0]; e2[k] = c2[k] + (cb[k] - c2[k]) * ts[0]; }
                            if (!ls15_ndc(e1, &x1, &y1) || !ls15_ndc(e2, &x2, &y2)) { continue; }
                            d = sqrt((x1 - x2) * (x1 - x2) * hw * hw + (y1 - y2) * (y1 - y2) * hh * hh);
                            if (d > mx) { mx = d; }
                        }
                    }
                    ls15_dev_sum[w] += mx; ls15_dev_n[w]++;
                    if (mx > ls15_dev_max[w]) { ls15_dev_max[w] = mx; }
                    if (mx > 0.5) { ls15_dev_big[w]++; }
                }
            }
        }
        if (tr) {
            ls15_lists++;
            { /* Kamerabewegung: Probepunkte bei t = 1/6 gegen Tick N (px) */
                int px, py;
                for (px = -1; px <= 1; px++) {
                    for (py = -1; py <= 1; py++) {
                        float cb[4] = { 0.6f * (float)px, 0.6f * (float)py, 0.99f, 1.0f }, cd[4], cc[4];
                        double x0, y0, x1, y1, d;
                        ls15_vmul(cd, cb, Di);
                        for (k = 0; k < 4; k++) { cc[k] = cd[k] + (cb[k] - cd[k]) * ts[0]; }
                        if (!ls15_ndc(cb, &x0, &y0) || !ls15_ndc(cc, &x1, &y1)) { continue; }
                        d = sqrt((x1 - x0) * (x1 - x0) * hw * hw + (y1 - y0) * (y1 - y0) * hh * hh);
                        if (d > cam) { cam = d; }
                    }
                }
            }
            ls15_cam_sum += cam;
            if (cam > ls15_cam_max) { ls15_cam_max = cam; }
            if (cam >= 0.5) { ls15_turn++; }
            for (i = 0; i < B->nops; i++) {
                const ip_op_t *o = &B->ops[i];
                const ip_op_t *a = (o->match >= 0 && (unsigned)o->match < A->nops) ? &A->ops[o->match] : NULL;
                double best = 0.0, bx = 0.0, by = 0.0;
                int lerp, any = 0;
                if (o->kind != IP_DRAW || !o->cls || !o->dsv || o->rtv != tr->rtv || ip_mvp_affine(o->mvp)) { continue; }
                if (o->wlc == 2u || o->wlc == 4u) { continue; } /* eigener VT bzw. geglaetteter Gegner-VT (bewegt) */
                lerp = o->flag && a != NULL;
                g = o->nv >= 1000u ? 0u : ((!o->has_tex && o->nv <= 32u) ? 1u : 2u);
                s = !lerp ? 0u : (o->rzm ? 3u : (o->rzb ? 2u : (o->why == IP_W_RAWLERP ? 4u : 1u)));
                if (o->law != tr->law) { ls15_law++; }
                for (q = 0; q < 4; q++) {
                    float cb[4], ca[4], cd[4];
                    ip_clip(o->pref[q], o->mvp, cb);
                    if (cb[3] <= 1e-3f) { continue; }
                    if (lerp) { ip_clip(o->rzb ? o->pref[q] : a->pref[q], IP_AMVP(o, a), ca); if (ca[3] <= 1e-3f) { continue; } }
                    ls15_vmul(cd, cb, Di);
                    if (cd[3] <= 1e-3f) { continue; }
                    for (k = 0; k < 5; k++) {
                        float cs[4], cg[4];
                        double xs, ys, xg, yg, d;
                        unsigned j;
                        for (j = 0; j < 4; j++) {
                            cs[j] = lerp ? ca[j] + (cb[j] - ca[j]) * ts[k] : cb[j];  /* gezeigt */
                            cg[j] = cd[j] + (cb[j] - cd[j]) * ts[k];                 /* Gelaende an derselben Weltstelle */
                        }
                        if (!ls15_ndc(cs, &xs, &ys) || !ls15_ndc(cg, &xg, &yg)) { continue; }
                        if (fabs(xg) > 1.2 || fabs(yg) > 1.2) { continue; } /* ausserhalb des Bildes */
                        d = sqrt((xs - xg) * (xs - xg) * hw * hw + (ys - yg) * (ys - yg) * hh * hh);
                        any = 1;
                        if (d > best) { best = d; bx = xg; by = yg; }
                    }
                }
                if (!any) { continue; }
                if ((g_cfg.loc_stat_diag & 8) && s == 0u && best > 0.5 && ls15_ln < 600u && (g_cfg.pf_ring <= 0 || sb_pf_active())) {
                    /* DIAG(W415): exakter Draw mit Versatz - Matrix gegen die Kamera (X = C_B^-1 * M_B: reine Verschiebung = Weltkoordinaten
                     * mit eigenem Ursprung), Partner-Lage */
                    const ip_op_t *cr = w415_cam_ref(B, o);
                    double X[16], rot = -1.0;
                    int xok = cr && zc_dmat(X, cr->mvp, o->mvp);
                    if (xok) { rot = 0.0; for (k = 0; k < 12; k++) { if ((k & 3) != 3) { double d = fabs(X[k] - ((k % 5) == 0 ? 1.0 : 0.0)); if (d > rot) { rot = d; } } } }
                    ls15_ln++;
                    HLOG_I("LOCSTAT ex: f=%u i=%u n=%u tex=%u why=%s m=%d px=%.2f ndc=%.2f,%.2f w=%.0f gleich_kamera=%d rot=%.5f T=%.1f,%.1f,%.1f vs=%X ps=%X",
                           B->frame, i, o->nv, o->has_tex, pr_why(o->why), o->match, best, bx, by, (double)o->rn[0][2],
                           cr ? memcmp(cr->mvp, o->mvp, sizeof o->mvp) == 0 : -1, rot, xok ? X[12] : 0.0, xok ? X[13] : 0.0, xok ? X[14] : 0.0,
                           o->pvs, o->pps);
                }
                ls15_nd[g][s]++;
                if (ls15_n[g][s] < LS15_CAP) { ls15_v[g][s][ls15_n[g][s]++] = (float)best; }
                if (best > ls15_mx[g][s]) { ls15_mx[g][s] = best; }
                if (best > 0.5) { ls15_big[g][s]++; }
                if ((g_cfg.loc_stat_diag & 2) && best > ls15_top[7].px) {
                    ls15_top_t e;
                    int p = 7;
                    e.px = (float)best; e.f = B->frame; e.i = i; e.nv = o->nv; e.tex = o->has_tex; e.why = o->why; e.st = s;
                    e.wlc = o->wlc; e.g = g; e.x = (float)bx; e.y = (float)by;
                    while (p > 0 && ls15_top[p - 1].px < e.px) { ls15_top[p] = ls15_top[p - 1]; p--; }
                    ls15_top[p] = e;
                }
            }
        }
    }
    if (!ls15_tlog) { ls15_tlog = now; }
    if (ip_ms(ls15_tlog, now) >= 10000.0) {
        static const char *gn[LS15_G] = { "Netz>=1000", "Linien/Pfosten", "Objekte" };
        static const char *sn[LS15_S] = { "exakt", "lerp", "rzb", "rzm", "vlerp" };
        char b[900];
        int n;
        HLOG_I("LOCSTAT: %.1fs Paare=%u davon Kamera bewegt (>=0,5 px je Zwischenbild)=%u Kamera mittel %.2f max %.2f px "
               "| ohne Gelaende-Bezug=%u Gelaende Vertex-Lerp=%u law-abweichend=%u | Bild %ux%u",
               ip_ms(ls15_tlog, now) / 1000.0, ls15_lists, ls15_turn, ls15_lists ? ls15_cam_sum / ls15_lists : 0.0,
               ls15_cam_max, ls15_noterr, ls15_terr_raw, ls15_law, g_bb_w, g_bb_h);
        HLOG_I("LOCSTAT kamera: Bezug-Kamera %u Paare (Wert %.1f), ohne Bezug (groesstes Netz) %u | Abweichung bei t=1/6 "
               "gegen den Bezug: groesstes Netz mittel %.2f max %.2f px (>0,5px %u/%u) | Mehrheits-D mittel %.2f max %.2f px (>0,5px %u/%u)",
               ls15_lists - ls15_nocons, (ls15_lists > ls15_nocons) ? (double)ls15_ncons / (double)(ls15_lists - ls15_nocons) : 0.0,
               ls15_nocons, ls15_dev_n[0] ? ls15_dev_sum[0] / ls15_dev_n[0] : 0.0, ls15_dev_max[0], ls15_dev_big[0], ls15_dev_n[0],
               ls15_dev_n[1] ? ls15_dev_sum[1] / ls15_dev_n[1] : 0.0, ls15_dev_max[1], ls15_dev_big[1], ls15_dev_n[1]);
        for (g = 0; g < LS15_G; g++) {
            n = _snprintf_s(b, sizeof b, _TRUNCATE, "LOCSTAT %s:", gn[g]);
            for (s = 0; s < LS15_S; s++) {
                double p95 = 0.0;
                if (ls15_n[g][s]) {
                    qsort(ls15_v[g][s], ls15_n[g][s], sizeof(float), ls15_fcmp);
                    p95 = ls15_v[g][s][(unsigned)((ls15_n[g][s] - 1u) * 0.95)];
                }
                n += _snprintf_s(b + n, sizeof b - (size_t)n, _TRUNCATE, " %s n=%u >0,5px=%u p95=%.2f max=%.2f |", sn[s],
                                 ls15_nd[g][s], ls15_big[g][s], p95, ls15_mx[g][s]);
            }
            HLOG_I("%s", b);
        }
        if (g_cfg.loc_stat_diag & 2) {
            for (k = 0; k < 8 && ls15_top[k].px > 0.0f; k++) {
                const ls15_top_t *e = &ls15_top[k];
                HLOG_I("LOCSTAT top %u: px=%.2f f=%u i=%u n=%u tex=%u gruppe=%s art=%s why=%s wlc=%u ndc=%.2f,%.2f", k, e->px, e->f, e->i,
                       e->nv, e->tex, gn[e->g], sn[e->st], pr_why(e->why), e->wlc, e->x, e->y);
            }
        }
        memset(ls15_n, 0, sizeof ls15_n); memset(ls15_nd, 0, sizeof ls15_nd); memset(ls15_big, 0, sizeof ls15_big);
        memset(ls15_mx, 0, sizeof ls15_mx); memset(ls15_top, 0, sizeof ls15_top);
        ls15_lists = ls15_turn = ls15_noterr = ls15_terr_raw = ls15_law = ls15_ncons = ls15_nocons = 0;
        memset(ls15_dev_sum, 0, sizeof ls15_dev_sum); memset(ls15_dev_max, 0, sizeof ls15_dev_max);
        memset(ls15_dev_n, 0, sizeof ls15_dev_n); memset(ls15_dev_big, 0, sizeof ls15_dev_big);
        ls15_cam_sum = ls15_cam_max = 0.0;
        ls15_tlog = now;
    }
}

/* W412 LOC-WORLD-LAG: Draw in Weltkoordinaten mit eigener Tiefe (Himmel): Spalten x, y, w der mvp gleich der Kamera VP,
 * nur die Tiefenspalte z weicht ab (Messlauf W412_P6: Himmels-Quad n=6, Tiefe auf die Ferne gelegt). mvp * VP^-1 ist
 * dann nicht affin, die Bildlage aber dieselbe wie bei einem Welt-Draw -> Kamera K davor (K * mvp) ist exakt. */
static int wl_same_view(const float mvp[16], const double VP[16])
{
    static const int cols[3] = {0, 1, 3};
    int r, k;
    for (k = 0; k < 3; k++) {
        for (r = 0; r < 4; r++) {
            double a = (double)mvp[r * 4 + cols[k]], b = VP[r * 4 + cols[k]];
            if (fabs(a - b) > 1e-4 * (1.0 + fabs(b))) { return 0; }
        }
    }
    return 1;
}

/* W385 LOC-SMOOTH: Rollback-Korrektur weich zeigen. Am Swap (vor der Zuordnung) die Welt-Draws dieser Liste mit dem
 * abklingenden Versatz des Spiel-Threads verschieben: Draws, deren Weltursprung (mvp * Kamera^-1, affin) einem anderen
 * VT am naechsten liegt (Radius), um dessen Versatz (W' = W * X); mit eigenem Versatz die ganze Welt ausser den Draws am
 * eigenen VT so, dass die Kamera vom vorhergesagten Stand aus sieht (W' = W * K). Danach Bezugs-NDC neu. Ohne Versatz
 * bleibt die Liste unberuehrt (bitgleich). Die Zwischenbilder interpolieren zwischen den so verschobenen Listen. */
static void li_smooth_apply(ip_list_t *L)
{
    loc_smooth_pub_t pb;
    float cam[16];
    double VP[16], VPi[16], K[16], X[LS_MAXV][16], eye[3], r2;
    unsigned cnt = 0, sp = 0, i, nobj = 0, ncam = 0, nown = 0, nvt = 0, nwd = 0, nsky = 0;
    unsigned hist[LS_MAXV][6], scnt[LS_MAXV];
    double sraw[LS_MAXV][3], sdisp[LS_MAXV][3], sbd[LS_MAXV]; /* Anker-Draw je VT (naechster Weltursprung) */
    int j, camon = 0, objon = 0, diag = (g_cfg.loc_rb_smooth_diag & 2) != 0, apply, haveye = 0;
    int meas = (g_cfg.loc_rb_smooth_diag & 8) != 0; /* DIAG(W385): sichtbare Lageaenderung je Liste messen */
    int wlag = (g_cfg.loc_wlag_diag & 1) != 0; /* DIAG(W412) */
    static unsigned s_dn, s_dl, s_pser;
    static double s_pdisp[LS_MAXV][3], s_peye[3], s_pstep[LS_MAXV][3], s_pestep[3];
    static double s_praw[LS_MAXV][3], s_prstep[LS_MAXV][3], s_peyer[3], s_perstep[3]; /* rohe Folge (ohne Versatz) */
    static int s_pok[LS_MAXV], s_peok, s_psok[LS_MAXV], s_pesok;
    ID3D11RenderTargetView *wt;
    LARGE_INTEGER lsq0, lsq1, lsqf; /* W387: Kosten je Liste mit Versatz (F2-Regel) */
    QueryPerformanceCounter(&lsq0);
    apply = loc_smooth_get(&pb);
    if (!apply && wlag) { /* DIAG(W412): auch ohne Versatz Klassen und Kamera messen (Gegenprobe) */
        if (!loc_smooth_peek(&pb)) { memset(&pb, 0, sizeof pb); pb.own = -1; }
    } else if (!apply) {
        if (!(diag || meas) || !loc_smooth_peek(&pb)) { return; }
        if (!meas && ((++s_dl % 200u) != 0u || s_dn >= 12u)) { return; } /* DIAG-Zensus: jede 200. Liste, hoechstens 12 */
        if (meas && diag && ((++s_dl % 200u) != 0u || s_dn >= 12u)) { diag = 0; }
    }
    memset(scnt, 0, sizeof scnt);
    memset(sraw, 0, sizeof sraw);
    memset(sdisp, 0, sizeof sdisp);
    if (!la_cam_of(L, cam, &cnt, &sp) || cnt < 8u) { return; }
    wt = la_world_rtv(L, &sp);
    if (!wt) { return; }
    for (i = 0; i < 16; i++) { VP[i] = cam[i]; }
    if (!la_m4inv_d(VPi, VP)) { return; }
    haveye = ls_eye_of(eye, VPi);
    if (apply && pb.own >= 0 && pb.own < LS_MAXV && pb.v[pb.own].ok && pb.v[pb.own].on && haveye) {
        double off[4], oc[4], c3[3];
        for (j = 0; j < 4; j++) { off[j] = pb.v[pb.own].off[j]; }
        for (j = 0; j < 3; j++) { c3[j] = pb.v[pb.own].p[j]; }
        ls_cam_off_pivot(off, eye, c3, oc); /* W390: das Auge dreht mit dem VT um dessen Mittelpunkt */
        ls_cam_xform(K, eye, oc);
        camon = 1;
    }
    for (j = 0; j < LS_MAXV; j++) {
        double p[3], off[4];
        int k;
        if (!apply || j == pb.own || !pb.v[j].ok || !pb.v[j].on) { continue; }
        for (k = 0; k < 3; k++) { p[k] = pb.v[j].p[k]; }
        for (k = 0; k < 4; k++) { off[k] = pb.v[j].off[k]; }
        ls_obj_xform(X[j], p, off);
        objon |= 1 << j;
        nvt++;
    }
    if (g_cfg.loc_hud_predict && apply) { li_hud_data(L, VP, &pb, camon ? K : NULL, X, (unsigned)objon); } /* W394 */
    if (wlag) { wl_cam_note(VPi, camon ? K : NULL, pb.gtick ? pb.gtick : L->serial); } /* DIAG(W412) */
    if (camon || objon) { /* W390: Abweichung einer Zielmarkierung (simulierter Stand, Bildpixel bei 640x480) vom gezeichneten VT */
        double wmax = 0.0, wown = 0.0;
        for (j = 0; j < LS_MAXV; j++) {
            double p4[3], q[3], a[4], b[4], c4[4];
            int k;
            if (j == pb.own || !pb.v[j].ok) { continue; }
            for (k = 0; k < 3; k++) { p4[k] = pb.v[j].p[k]; }
            for (k = 0; k < 4; k++) { a[k] = p4[0] * VP[k] + p4[1] * VP[4 + k] + p4[2] * VP[8 + k] + VP[12 + k]; }
            if (a[3] < 1e-3 || fabs(a[0] / a[3]) > 1.2 || fabs(a[1] / a[3]) > 1.2) { continue; }
            if (camon) { /* nur eigene Kamera */
                ls_xform_pt(q, p4, K);
                for (k = 0; k < 4; k++) { c4[k] = q[0] * VP[k] + q[1] * VP[4 + k] + q[2] * VP[8 + k] + VP[12 + k]; }
                if (c4[3] > 1e-3) {
                    double dx = (c4[0] / c4[3] - a[0] / a[3]) * 320.0, dy = (c4[1] / c4[3] - a[1] / a[3]) * 240.0;
                    if (sqrt(dx * dx + dy * dy) > wown) { wown = sqrt(dx * dx + dy * dy); }
                }
            }
            if (objon & (1 << j)) { ls_xform_pt(q, p4, X[j]); } else { memcpy(q, p4, sizeof q); }
            if (camon) { ls_xform_pt(q, q, K); }
            for (k = 0; k < 4; k++) { b[k] = q[0] * VP[k] + q[1] * VP[4 + k] + q[2] * VP[8 + k] + VP[12 + k]; }
            if (b[3] > 1e-3) {
                double dx = (b[0] / b[3] - a[0] / a[3]) * 320.0, dy = (b[1] / b[3] - a[1] / a[3]) * 240.0;
                if (sqrt(dx * dx + dy * dy) > wmax) { wmax = sqrt(dx * dx + dy * dy); }
            }
        }
        if (camon) { /* gedachte Ziele geradeaus in 30 m und 100 m (unabhaengig davon, ob ein VT im Bild ist) */
            double q4[4], fw[3], fl, t30 = 0.0, t100 = 0.0;
            int k, rr;
            for (k = 0; k < 4; k++) { q4[k] = 0.5 * VPi[8 + k] + VPi[12 + k]; } /* NDC (0, 0, 0.5) zurueck in die Welt */
            if (fabs(q4[3]) > 1e-9) {
                for (k = 0; k < 3; k++) { fw[k] = q4[k] / q4[3] - eye[k]; }
                fl = sqrt(fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]);
                for (rr = 0; rr < 2 && fl > 1e-6; rr++) {
                    double R = rr ? 100000.0 : 30000.0, p4[3], q[3], a[4], b[4], dx, dy;
                    for (k = 0; k < 3; k++) { p4[k] = eye[k] + fw[k] / fl * R; }
                    ls_xform_pt(q, p4, K);
                    for (k = 0; k < 4; k++) {
                        a[k] = p4[0] * VP[k] + p4[1] * VP[4 + k] + p4[2] * VP[8 + k] + VP[12 + k];
                        b[k] = q[0] * VP[k] + q[1] * VP[4 + k] + q[2] * VP[8 + k] + VP[12 + k];
                    }
                    if (a[3] < 1e-3 || b[3] < 1e-3) { continue; }
                    dx = (b[0] / b[3] - a[0] / a[3]) * 320.0;
                    dy = (b[1] / b[3] - a[1] / a[3]) * 240.0;
                    if (rr) { t100 = sqrt(dx * dx + dy * dy); } else { t30 = sqrt(dx * dx + dy * dy); }
                }
            }
            loc_smooth_render_mark_virt(t30, t100);
        }
        loc_smooth_render_mark(wmax, wown);
    }
    if (!camon && !objon && !diag && !meas && !wlag) { return; }
    memset(hist, 0, sizeof hist);
    r2 = (double)pb.radius * (double)pb.radius;
    for (i = 0; i < L->nops && i < sp; i++) {
        ip_op_t *o = &L->ops[i];
        double W[16], M[16], bd = -1.0;
        int r, c, best = -1, q;
        if (o->kind != IP_DRAW || o->rtv != wt || !o->cls) { continue; }
        for (r = 0; r < 4; r++)
            for (c = 0; c < 4; c++)
                W[r * 4 + c] = (double)o->mvp[r * 4] * VPi[c] + (double)o->mvp[r * 4 + 1] * VPi[4 + c] +
                               (double)o->mvp[r * 4 + 2] * VPi[8 + c] + (double)o->mvp[r * 4 + 3] * VPi[12 + c];
        if (fabs(W[3]) + fabs(W[7]) + fabs(W[11]) + fabs(W[15] - 1.0) >= 0.02) { /* kein Weltobjekt */
            static unsigned s_wn;
            o->wlc = 3;
            if ((g_cfg.loc_wlag_diag & 2) && s_wn < 24u && (L->serial % 97u) == 0u) { /* DIAG(W412): Matrix nicht-affiner Draws */
                s_wn++;
                HLOG_I("LOCWLAG naff: ser=%u i=%u n=%u tex=%u yawoff=%.5f mvp=%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g "
                       "VP=%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g",
                       L->serial, i, o->nv, o->has_tex, camon ? pb.v[pb.own].off[3] : 0.0f, o->mvp[0], o->mvp[1], o->mvp[2], o->mvp[3],
                       o->mvp[4], o->mvp[5], o->mvp[6], o->mvp[7], o->mvp[8], o->mvp[9], o->mvp[10], o->mvp[11], o->mvp[12],
                       o->mvp[13], o->mvp[14], o->mvp[15], VP[0], VP[1], VP[2], VP[3], VP[4], VP[5], VP[6], VP[7], VP[8], VP[9],
                       VP[10], VP[11], VP[12], VP[13], VP[14], VP[15]);
            }
            if (camon && g_cfg.loc_wlag_fix && wl_same_view(o->mvp, VP)) { /* W412: Himmel mit derselben Kamera wie die Welt */
                double Md[16];
                for (q = 0; q < 16; q++) { Md[q] = (double)o->mvp[q]; }
                ls_m4_mul(Md, K, Md);
                for (q = 0; q < 16; q++) { o->mvp[q] = (float)Md[q]; }
                for (q = 0; q < 4; q++) { /* Bezugs-NDC wie bei der Aufzeichnung (Zuordnung/Plausibilitaet) */
                    float cc[4];
                    ip_clip(o->pref[q], o->mvp, cc);
                    o->rn[q][2] = cc[3];
                    o->rn[q][0] = cc[3] > 1e-6f ? cc[0] / cc[3] : 0.0f;
                    o->rn[q][1] = cc[3] > 1e-6f ? cc[1] / cc[3] : 0.0f;
                }
                o->wlc = 6;
                nsky++;
            }
            continue;
        }
        nwd++;
        for (j = 0; j < LS_MAXV; j++) {
            double dx, dy, dz, d2;
            if (!pb.v[j].ok) { continue; }
            dx = W[12] - pb.v[j].p[0]; dy = W[13] - pb.v[j].p[1]; dz = W[14] - pb.v[j].p[2];
            d2 = dx * dx + dy * dy + dz * dz;
            if (diag) {
                double d = sqrt(d2);
                hist[j][d < 2000.0 ? 0 : (d < 4000.0 ? 1 : (d < 7000.0 ? 2 : (d < 10000.0 ? 3 : (d < 20000.0 ? 4 : 5))))]++;
            }
            if (d2 <= r2 && (best < 0 || d2 < bd)) { best = j; bd = d2; }
        }
        if (best >= 0 && best == pb.own) { /* bewegt sich mit der Kamera */
            o->wlc = 2; /* W412 */
            static unsigned s_own_ln;
            if ((g_cfg.loc_rb_smooth_diag & 16) && camon && s_own_ln < 600u && (g_cfg.pf_ring <= 0 || sb_pf_active())) { /* DIAG(W410) */
                s_own_ln++;
                HLOG_I("LOCSMOOTH own: ser=%u i=%u n=%u tex=%u topo=%u d=%.0f W=%.0f,%.0f,%.0f ndc0=%.2f,%.2f w=%.1f ndc3=%.2f,%.2f w=%.1f",
                       L->serial, i, o->nv, o->has_tex, o->topo, sqrt(bd), W[12], W[13], W[14], o->rn[0][0], o->rn[0][1],
                       o->rn[0][2], o->rn[3][0], o->rn[3][1], o->rn[3][2]);
            }
            nown++;
            continue;
        }
        memcpy(M, W, sizeof M);
        if (best >= 0 && (objon & (1 << best))) { ls_m4_mul(M, M, X[best]); nobj++; }
        if (camon) { ls_m4_mul(M, M, K); ncam++; }
        if (meas && best >= 0 && (!scnt[best] || bd < sbd[best])) { /* Anker: roh W, angezeigt W * X * K (wie gesehen) */
            for (q = 0; q < 3; q++) { sraw[best][q] = W[12 + q]; sdisp[best][q] = M[12 + q]; }
            sbd[best] = bd;
            scnt[best] = 1;
        }
        o->wlc = camon ? 1 : ((best >= 0 && (objon & (1 << best))) ? 4 : 5); /* W412 */
        if (!(best >= 0 && (objon & (1 << best))) && !camon) { continue; }
        ls_m4_mul(M, M, VP);
        for (q = 0; q < 16; q++) { o->mvp[q] = (float)M[q]; }
        for (q = 0; q < 4; q++) { /* Bezugs-NDC wie bei der Aufzeichnung (Zuordnung/Plausibilitaet) */
            float cc[4];
            ip_clip(o->pref[q], o->mvp, cc);
            o->rn[q][2] = cc[3];
            o->rn[q][0] = cc[3] > 1e-6f ? cc[0] / cc[3] : 0.0f;
            o->rn[q][1] = cc[3] > 1e-6f ? cc[1] / cc[3] : 0.0f;
        }
    }
    if (nsky) { InterlockedIncrement(&wl_fix_lists); InterlockedExchangeAdd(&wl_fix_draws, (LONG)nsky); } /* W412 */
    if (camon && (g_cfg.loc_wlag_diag & 4)) { /* DIAG(W435): Listen mit K, davon Himmel mit K, davon Himmel erkannt (li_aim_attach) */
        InterlockedIncrement(&sk_klists);
        if (nsky) { InterlockedIncrement(&sk_klists_sky); }
        if (L->la_ok && L->la_nsk) { InterlockedIncrement(&sk_klists_has); }
    }
    if (apply) {
        loc_smooth_render_note(nobj, ncam, nown, nvt);
        QueryPerformanceCounter(&lsq1); /* W387 */
        QueryPerformanceFrequency(&lsqf);
        loc_smooth_render_cost((double)(lsq1.QuadPart - lsq0.QuadPart) * 1e6 / (double)lsqf.QuadPart);
    }
    if (meas) { /* DIAG(W385): Schritt gegen die zuletzt gezeigte Liste (Mittel der Draws je VT bzw. Auge) */
        int cont = s_pser && L->serial == s_pser + 1u;
        for (j = 0; j < LS_MAXV; j++) { /* Ruck (zweite Differenz) des Anker-Draws: roh gegen die zuletzt gezeigte Lage */
            double sr[3], sd[3], a = 0.0, b = 0.0;
            int q;
            if (j == pb.own || !scnt[j]) { s_pok[j] = 0; s_psok[j] = 0; continue; }
            if (cont && s_pok[j]) {
                for (q = 0; q < 3; q++) { sr[q] = sraw[j][q] - s_praw[j][q]; sd[q] = sdisp[j][q] - s_pdisp[j][q]; }
                if (s_psok[j]) {
                    for (q = 0; q < 3; q++) {
                        a += (sr[q] - s_prstep[j][q]) * (sr[q] - s_prstep[j][q]);
                        b += (sd[q] - s_pstep[j][q]) * (sd[q] - s_pstep[j][q]);
                    }
                    loc_smooth_render_step(0, sqrt(a), sqrt(b), pb.v[j].on);
                }
                memcpy(s_pstep[j], sd, sizeof sd);
                memcpy(s_prstep[j], sr, sizeof sr);
                s_psok[j] = 1;
            } else {
                s_psok[j] = 0;
            }
            memcpy(s_pdisp[j], sdisp[j], sizeof s_pdisp[j]);
            memcpy(s_praw[j], sraw[j], sizeof s_praw[j]);
            s_pok[j] = 1;
        }
        if (haveye && pb.own >= 0 && pb.own < LS_MAXV) {
            double ed[3], a = 0.0, b = 0.0;
            int q;
            for (q = 0; q < 3; q++) ed[q] = eye[q] + (camon ? pb.v[pb.own].off[q] : 0.0); /* Kamera sieht von e + E */
            if (cont && s_peok) {
                double sr[3], sd[3];
                for (q = 0; q < 3; q++) { sr[q] = eye[q] - s_peyer[q]; sd[q] = ed[q] - s_peye[q]; }
                if (s_pesok) {
                    for (q = 0; q < 3; q++) {
                        a += (sr[q] - s_perstep[q]) * (sr[q] - s_perstep[q]);
                        b += (sd[q] - s_pestep[q]) * (sd[q] - s_pestep[q]);
                    }
                    loc_smooth_render_step(1, sqrt(a), sqrt(b), camon);
                }
                memcpy(s_pestep, sd, sizeof sd);
                memcpy(s_perstep, sr, sizeof sr);
                s_pesok = 1;
            } else {
                s_pesok = 0;
            }
            memcpy(s_peye, ed, sizeof ed);
            memcpy(s_peyer, eye, sizeof s_peyer);
            s_peok = 1;
        }
        s_pser = L->serial;
    }
    if (diag && s_dn < 12u) { /* DIAG(W385): Abstand Weltursprung der Draws -> VT (Eichung Radius), keine Eingaben */
        static FILE *fz;
        if (!fz) { fz = (FILE *)loc_aim_diag_file("locsmooth_census.txt"); }
        if (fz) {
            s_dn++;
            fprintf(fz, "LIST serial=%u gtick=%u own=%d radius=%.0f welt=%u apply=%d obj=%u cam=%u eigen=%u\n", L->serial,
                    pb.gtick, pb.own, pb.radius, nwd, apply, nobj, ncam, nown);
            for (j = 0; j < LS_MAXV; j++) {
                if (!pb.v[j].ok) { continue; }
                fprintf(fz, "  VT%d p=%.1f,%.1f,%.1f gier=%.3f off=%.2f,%.2f,%.2f,%.4f  Draws <2000/4000/7000/10000/20000/sonst=%u/%u/%u/%u/%u/%u\n",
                        j, pb.v[j].p[0], pb.v[j].p[1], pb.v[j].p[2], pb.v[j].yaw, pb.v[j].off[0], pb.v[j].off[1],
                        pb.v[j].off[2], pb.v[j].off[3], hist[j][0], hist[j][1], hist[j][2], hist[j][3], hist[j][4], hist[j][5]);
            }
            fflush(fz);
        }
    }
}

/* W376 LOC-LOCAL-AIM: am Swap Vorhersage-Delta und Weltkamera an die Liste haengen, Welt-Draws markieren (Backbuffer,
 * 3D, vor der ersten BB-Kopie, mvp * Kamera^-1 affin = Objekt in der Welt; Cockpit/HUD liegen hinter der Kopie bzw.
 * haben eine eigene Projektion). */
static void li_aim_attach(ip_list_t *L)
{
    loc_aim_pub_t pb;
    float cam[16];
    double VPi[16];
    unsigned cnt = 0, sp = 0, i, nw = 0;
    ID3D11RenderTargetView *wt;
    L->la_ok = 0;
    if (g_cfg.loc_local_aim) { if (!loc_aim_get(&pb)) { return; } }
    else if (!loc_aim_peek(&pb) || !pb.ok) { return; } /* DIAG(W376) Bit 32: Vergleichslauf mit Schalter aus */
    if (!la_cam_of(L, cam, &cnt, &sp) || cnt < 8u) { return; }
    wt = la_world_rtv(L, &sp);
    for (i = 0; i < 16; i++) { L->la_vp[i] = cam[i]; }
    if (!la_m4inv_d(VPi, L->la_vp)) { return; }
    L->la_nsk = L->la_nskl = L->la_nna = 0;
    for (i = 0; i < L->nops && i < sp; i++) {
        ip_op_t *o = &L->ops[i];
        double c3[4];
        int k, j;
        o->law = 0;
        o->lsk = 0;
        if (o->kind != IP_DRAW || o->rtv != wt || !o->cls) { continue; }
        for (k = 0; k < 4; k++) {
            c3[k] = 0.0;
            for (j = 0; j < 4; j++) { c3[k] += (double)o->mvp[k * 4 + j] * VPi[j * 4 + 3]; }
        }
        if (fabs(c3[0]) + fabs(c3[1]) + fabs(c3[2]) + fabs(c3[3] - 1.0) < 0.02) { o->law = 1; nw++; }
        else if (wl_same_view(o->mvp, L->la_vp)) {
            /* W435 LOC-SKY-LAG-REG: Himmel (Spalten x, y, w = Kamera, eigene Tiefe; W412). Die Sicht-Korrektur C =
             * VP^-1 * G * VP dreht um das Auge; ihre Zeile z ist (0, 0, 1, 0), d. h. x, y, w des Bildes haengen nur von
             * x, y, w ab -> mvp * C zeigt den Himmel genau dort, wo die korrigierte Kamera ihn sieht (Selbsttest SKY435). */
            o->lsk = 1;
            L->la_nsk++;
            if (g_cfg.loc_sky_fix) { o->law = 1; L->la_nskl++; }
        } else if (o->dsv && !ip_mvp_affine(o->mvp)) { /* sonstige perspektivische Draws ohne affinen Weltbezug (DIAG) */
            static unsigned s_nal;
            L->la_nna++;
            if ((g_cfg.loc_wlag_diag & 4) && s_nal < 24u && (L->serial % 97u) == 0u) {
                s_nal++;
                HLOG_I("LOCWLAG sicht-na: ser=%u i=%u n=%u tex=%u mvp=%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g "
                       "VP=%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g",
                       L->serial, i, o->nv, o->has_tex, o->mvp[0], o->mvp[1], o->mvp[2], o->mvp[3], o->mvp[4], o->mvp[5],
                       o->mvp[6], o->mvp[7], o->mvp[8], o->mvp[9], o->mvp[10], o->mvp[11], o->mvp[12], o->mvp[13], o->mvp[14],
                       o->mvp[15], L->la_vp[0], L->la_vp[1], L->la_vp[2], L->la_vp[3], L->la_vp[4], L->la_vp[5], L->la_vp[6],
                       L->la_vp[7], L->la_vp[8], L->la_vp[9], L->la_vp[10], L->la_vp[11], L->la_vp[12], L->la_vp[13],
                       L->la_vp[14], L->la_vp[15]);
            }
        }
    }
    if (L->la_nskl) { InterlockedIncrement(&sk_lists); InterlockedExchangeAdd(&sk_draws, (LONG)L->la_nskl); } /* W435 */
    L->la_d[0] = pb.dys; L->la_d[1] = pb.dps; L->la_d[2] = pb.dax; L->la_d[3] = pb.day;
    L->la_s[0] = pb.ys; L->la_s[1] = pb.ps; /* W394 */
    { /* Fadenkreuz (Messlauf W376_A9): Mitte im Weltziel x = -py tan(Arm_x - Sicht_x), y = -py tan(Arm_y + Sicht_y),
       * py = Projektion der Weltkamera (|Spalte 1| / |Spalte 3|) */
        double c1 = sqrt(L->la_vp[1] * L->la_vp[1] + L->la_vp[5] * L->la_vp[5] + L->la_vp[9] * L->la_vp[9]);
        double c3 = sqrt(L->la_vp[3] * L->la_vp[3] + L->la_vp[7] * L->la_vp[7] + L->la_vp[11] * L->la_vp[11]);
        double py = c3 > 0.0 ? c1 / c3 : 0.0, rx = (double)pb.ax - pb.ys, ry = (double)pb.ay + pb.ps;
        double rx2 = rx + (double)pb.dax - pb.dys, ry2 = ry + (double)pb.day + pb.dps;
        if (py > 0.0) loc_aimassist_note_py(py); /* W442: Rand der Zielhilfe */
        L->la_rc[0] = (float)(-py * tan(la_clamp(rx, -1.4, 1.4)));
        L->la_rc[1] = (float)(-py * tan(la_clamp(ry, -1.4, 1.4)));
        L->la_rd[0] = (float)(-py * tan(la_clamp(rx2, -1.4, 1.4))) - L->la_rc[0];
        L->la_rd[1] = (float)(-py * tan(la_clamp(ry2, -1.4, 1.4))) - L->la_rc[1];
    }
    L->la_seq = pb.seq;
    L->la_ok = nw > 0u;
    InterlockedIncrement(&li_la_lists);
    InterlockedExchangeAdd(&li_la_draws, (LONG)nw);
}

/* W376: Fadenkreuz-Draws markieren (o->law = 2): kleine untexturierte 2D-Draws (<= 64 Vertices) im Weltziel nach dessen
 * Kopie, erster Vertex in der Box +-0,16 / +-0,12 NDC um die erwartete Mitte (A9: 7 Draws, Rahmen + Marken, alle
 * anderen HUD-Elemente >= 0,3 entfernt). Hat sich die Mitte gegen den Vortick bewegt, muss sich ein zugeordneter
 * Draw ebenso bewegen (+-0,01), sonst ist er ein ruhendes Element, das zufaellig in der Box liegt. */
/* DIAG(W444 LOC-RETICLE-MARK) loc_ret_diag Bit 4 (nur Testfolge): je Liste alle kleinen 2D-Draws im Weltziel nach der
 * Kopie im Umkreis 0,6 NDC der Formel-Mitte (Op, Partner, Signatur, Vertexzahl, Bildlage, Markierung) nach
 * locret_cand.txt. Nur Bildlagen/Draw-Merkmale, keine Eingaben. */
static void li_ret_cand_dump(const ip_list_t *L, const ip_list_t *A, unsigned sp, ID3D11RenderTargetView *wt)
{
    static FILE *f;
    static unsigned nl;
    unsigned i;
    if (nl >= 40000u) { return; }
    if (!f) { f = (FILE *)loc_aim_diag_file("locret_cand.txt"); if (!f) { return; } }
    nl++;
    fprintf(f, "L %u %u rc=%.5f,%.5f arc=%.5f,%.5f aok=%d aret=%d n=%u rj=%u sp=%u nops=%u\n", L->serial, A ? A->serial : 0u,
            L->la_rc[0], L->la_rc[1], A ? A->la_rc[0] : 0.0f, A ? A->la_rc[1] : 0.0f, A ? A->la_ok : 0, A ? A->la_ret : 0,
            L->la_nret, L->la_rjmv, sp, L->nops);
    for (i = sp + 1u; i < L->nops; i++) {
        const ip_op_t *o = &L->ops[i];
        const ip_op_t *a = (A && o->match >= 0 && (unsigned)o->match < A->nops) ? &A->ops[o->match] : NULL;
        if (o->kind != IP_DRAW || o->cls || o->nv > 256u || o->rtv != wt) { continue; }
        if (fabs(o->rn[0][0] - L->la_rc[0]) > 0.6f || fabs(o->rn[0][1] - L->la_rc[1]) > 0.6f) { continue; }
        fprintf(f, "D %u m=%d al=%d law=%d fl=%d tex=%u nv=%u tp=%u sig=%08x s0=%08x gk=%08x vs=%08x px=%08x ps=%08x bs=%p "
                "r=%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f a=%.5f,%.5f\n",
                i, o->match, a ? a->law : -1, o->law, o->flag, o->has_tex, o->nv, o->topo, o->sig, o->sig0, o->gkey, o->pvs,
                o->pxp, o->pps, (void *)o->bs, o->rn[0][0], o->rn[0][1], o->rn[1][0], o->rn[1][1], o->rn[2][0], o->rn[2][1],
                o->rn[3][0], o->rn[3][1], a ? a->rn[0][0] : 9.0f, a ? a->rn[0][1] : 9.0f);
    }
}

/* W444 LOC-RETICLE-MARK (loc_ret_mark, loc_ret_mark.h): Fadenkreuz-Teile ueber Form-Identitaet zur Vorliste (ergaenzt
 * um Teile aus dem Gedaechtnis, hoechstens LRM_MEMT Ticks alt), Zuordnungs-Partner mit Formwechsel und die alte Pruefung
 * (Kasten + Formel-Bewegung) als Start. Nur im Swap (ein Aufrufer). Zaehler fuer die Zeile LOCINTERP retmark444. */
static lrm_mem_t lrm_mem[LRM_MAXP];
static int lrm_mem_n;
static void lrm_part_of(const ip_op_t *o, lrm_part_t *p)
{
    int k;
    for (k = 0; k < 4; k++) { p->p[k][0] = o->rn[k][0]; p->p[k][1] = o->rn[k][1]; }
    p->nv = o->nv;
    p->grp = o->sig0;
}
static void li_aim_reticle_w444(ip_list_t *L, const ip_list_t *A, unsigned sp, ID3D11RenderTargetView *wt, int moved)
{
    lrm_part_t prev[LRM_MAXP], mk[LRM_MAXP];
    unsigned prev_op[LRM_MAXP], cop[LRM_MAXC], i, n = 0;
    lrm_cand_t c[LRM_MAXC];
    unsigned char why[LRM_MAXC];
    int np = 0, nc = 0, k, j, na;
    if (A && A->la_ret) {
        for (i = 0; i < A->nops && np < LRM_MAXP; i++) {
            const ip_op_t *a = &A->ops[i];
            if (a->kind != IP_DRAW || !LA_IS_RET(a->law)) { continue; }
            lrm_part_of(a, &prev[np]);
            prev_op[np] = i;
            np++;
        }
    }
    na = np;
    np = lrm_prev_add_mem(prev, NULL, np, lrm_mem, lrm_mem_n, L->serial);
    for (k = na; k < np; k++) { prev_op[k] = 0xFFFFFFFFu; }
    if (np > na) { InterlockedIncrement(&lrm_st_mem); }
    for (i = sp + 1u; i < L->nops && nc < LRM_MAXC; i++) {
        ip_op_t *o = &L->ops[i];
        int seed = 1;
        if (o->kind != IP_DRAW || o->cls || o->has_tex || o->nv > 64u || o->rtv != wt) { continue; }
        if (fabs(o->rn[0][0] - L->la_rc[0]) > LRM_WIN || fabs(o->rn[0][1] - L->la_rc[1]) > LRM_WIN) { continue; }
        /* alte Pruefung (W376/W394/W439) als Start-Regel */
        if (fabs(o->rn[0][0] - L->la_rc[0]) > 0.16f || fabs(o->rn[0][1] - L->la_rc[1]) > 0.12f) {
            if (!g_cfg.loc_view_perframe || !A || !A->la_ret || o->match < 0 || (unsigned)o->match >= A->nops ||
                !LA_IS_RET(A->ops[o->match].law) || fabs(o->rn[0][0] - L->la_rc[0]) > 0.35f || fabs(o->rn[0][1] - L->la_rc[1]) > 0.3f) {
                seed = 0;
            }
        }
        if (seed && moved && o->match >= 0 && (unsigned)o->match < A->nops) {
            const ip_op_t *a = &A->ops[o->match];
            float ex = (o->rn[0][0] - a->rn[0][0]) - (L->la_rc[0] - A->la_rc[0]);
            float ey = (o->rn[0][1] - a->rn[0][1]) - (L->la_rc[1] - A->la_rc[1]);
            if (fabs(ex) > 0.01f || fabs(ey) > 0.01f) { L->la_rjmv++; seed = 0; }
        }
        lrm_part_of(o, &c[nc].d);
        c[nc].seed = (unsigned char)seed;
        c[nc].prev = -1;
        if (A && o->match >= 0) {
            for (j = 0; j < np; j++) { if (prev_op[j] == (unsigned)o->match) { c[nc].prev = j; break; } }
        }
        cop[nc] = i;
        nc++;
    }
    lrm_mark(prev, np, c, nc, L->la_rc, why);
    for (k = 0; k < nc; k++) {
        ip_op_t *o;
        if (!why[k]) { if (c[k].seed) { InterlockedIncrement(&lrm_st_fremd); } continue; }
        o = &L->ops[cop[k]];
        o->law = 2;
        if (g_cfg.loc_ret_fix && !(o->flag && A && o->match >= 0 && (unsigned)o->match < A->nops)) {
            o->law = LA_LAW_RETX; /* W439: im Zwischenbild exakt aus Tick N -> Verschiebung gegen die Mitte von B */
            InterlockedIncrement(&lr_retx);
        }
        if (why[k] == LRM_FORM) { InterlockedIncrement(&lrm_st_form); }
        else if (why[k] == LRM_PAIR) { InterlockedIncrement(&lrm_st_pair); }
        else { InterlockedIncrement(&lrm_st_seed); }
        if (n < LRM_MAXP) { mk[n] = c[k].d; }
        n++;
    }
    if (n) {
        lrm_mem_n = lrm_mem_update(lrm_mem, lrm_mem_n, mk, n < LRM_MAXP ? (int)n : LRM_MAXP, L->serial);
        InterlockedIncrement(&lrm_st_lists);
    }
    L->la_ret = n > 0u;
    L->la_nret = n;
    InterlockedExchangeAdd(&li_la_rets, (LONG)n);
    InterlockedExchangeAdd(&lr_rjmv, (LONG)L->la_rjmv);
    if ((g_cfg.loc_ret_diag & 4) && (g_cfg.loc_replay_tick[0] || g_cfg.loc_steps[0])) { li_ret_cand_dump(L, A, sp, wt); }
}

static void li_aim_reticle(ip_list_t *L, const ip_list_t *A)
{
    unsigned sp = 0, i, n = 0;
    ID3D11RenderTargetView *wt = la_world_rtv(L, &sp);
    int moved = A && A->la_ok && (fabs(L->la_rc[0] - A->la_rc[0]) + fabs(L->la_rc[1] - A->la_rc[1]) > 0.01);
    L->la_ret = 0;
    L->la_nret = L->la_rjmv = 0;
    if (!wt || !sp) { return; }
    if (g_cfg.loc_ret_mark) { li_aim_reticle_w444(L, A, sp, wt, moved); return; } /* W444 LOC-RETICLE-MARK */
    for (i = sp + 1u; i < L->nops; i++) {
        ip_op_t *o = &L->ops[i];
        if (o->kind != IP_DRAW || o->cls || o->has_tex || o->nv > 64u || o->rtv != wt) { continue; }
        if (fabs(o->rn[0][0] - L->la_rc[0]) > 0.16f || fabs(o->rn[0][1] - L->la_rc[1]) > 0.12f) {
            /* W394 (nur mit loc_view_perframe): Partner eines Fadenkreuz-Draws der Vorliste bleibt Fadenkreuz (weitere Box),
             * sonst faellt das Fadenkreuz fuer einen Tick auf den simulierten Stand zurueck (Sprung, Lauf C4) */
            if (!g_cfg.loc_view_perframe || !A || !A->la_ret || o->match < 0 || (unsigned)o->match >= A->nops ||
                !LA_IS_RET(A->ops[o->match].law) || fabs(o->rn[0][0] - L->la_rc[0]) > 0.35f || fabs(o->rn[0][1] - L->la_rc[1]) > 0.3f) {
                continue;
            }
        }
        if (moved && o->match >= 0 && (unsigned)o->match < A->nops) {
            /* W439: Beim schnellen Zielen/Drehen weicht die Formel-Mitte vom gezeichneten Fadenkreuz ab; diese Pruefung
             * verwirft dann Teile (Messung V10: 145 Listen mit 1 statt 7 Draws). Mit loc_ret_mark (W444) ist sie nur noch
             * eine von drei Regeln (li_aim_reticle_w444, loc_ret_mark.h). */
            const ip_op_t *a = &A->ops[o->match];
            float ex = (o->rn[0][0] - a->rn[0][0]) - (L->la_rc[0] - A->la_rc[0]);
            float ey = (o->rn[0][1] - a->rn[0][1]) - (L->la_rc[1] - A->la_rc[1]);
            if (fabs(ex) > 0.01f || fabs(ey) > 0.01f) { L->la_rjmv++; continue; }
        }
        o->law = 2;
        if (g_cfg.loc_ret_fix && !(o->flag && A && o->match >= 0 && (unsigned)o->match < A->nops)) {
            o->law = LA_LAW_RETX; /* W439: im Zwischenbild exakt aus Tick N -> Verschiebung gegen die Mitte von B */
            InterlockedIncrement(&lr_retx);
        }
        n++;
    }
    L->la_ret = n > 0u;
    L->la_nret = n;
    InterlockedExchangeAdd(&li_la_rets, (LONG)n);
    InterlockedExchangeAdd(&lr_rjmv, (LONG)L->la_rjmv);
    if ((g_cfg.loc_ret_diag & 4) && (g_cfg.loc_replay_tick[0] || g_cfg.loc_steps[0])) { li_ret_cand_dump(L, A, sp, wt); }
}

/* DIAG(LOC-IM-LOCK) imlock_diag=1: Teilzeiten des Swaps unter ctx_lock bis zur Zuordnung (lange > 1 ms summiert).
 * 0 gpu_poll, 1 snap, 2 gpu_copy, 3 w_upload, 4 aim, 5 smooth, 6 bis Freigabe */
#define ISW_N 7
static long long isw_q[ISW_N], isw_last;
static unsigned isw_nc, isw_ns;
static double isw_sum[ISW_N], isw_mx, isw_mxp[ISW_N];
#define ISW(k) do { if (isw_last) { long long t_ = ip_now(); isw_q[k] += t_ - isw_last; isw_last = t_; } } while (0)
static void isw_end(void)
{
    double t = 0.0, v[ISW_N];
    int k;
    if (!isw_last) { return; }
    for (k = 0; k < ISW_N; k++) { v[k] = (double)isw_q[k] * 1000.0 / (double)ip_qpf.QuadPart; t += v[k]; }
    isw_nc++;
    if (t > 1.0) { isw_ns++; for (k = 0; k < ISW_N; k++) { isw_sum[k] += v[k]; } }
    if (t > isw_mx) { isw_mx = t; memcpy(isw_mxp, v, sizeof v); }
    isw_last = 0;
}
static void isw_log(double secs)
{
    HLOG_I("IMLOCK swap: %.1fs swaps=%u >1ms=%u teile_lang gpu_poll=%.1f snap=%.1f gpu_copy=%.1f w_upload=%.1f aim=%.1f smooth=%.1f rest=%.1f "
           "| laengster %.2fms gpu_poll=%.2f snap=%.2f gpu_copy=%.2f w_upload=%.2f aim=%.2f smooth=%.2f rest=%.2f",
           secs, isw_nc, isw_ns, isw_sum[0], isw_sum[1], isw_sum[2], isw_sum[3], isw_sum[4], isw_sum[5], isw_sum[6],
           isw_mx, isw_mxp[0], isw_mxp[1], isw_mxp[2], isw_mxp[3], isw_mxp[4], isw_mxp[5], isw_mxp[6]);
    HLOG_I("IMLOCK snap: %.1fs sh_ref=%u (%.1fms, davon CopyResource %u in %.1fms) unref=%.1fms",
           secs, isw_gr_n, (double)isw_gr_q * 1000.0 / (double)ip_qpf.QuadPart, isw_cp_n, (double)isw_cp_q * 1000.0 / (double)ip_qpf.QuadPart,
           (double)isw_un_q * 1000.0 / (double)ip_qpf.QuadPart);
    isw_cp_q = isw_un_q = isw_gr_q = 0; isw_cp_n = isw_gr_n = 0;
    isw_nc = isw_ns = 0; isw_mx = 0.0; memset(isw_sum, 0, sizeof isw_sum); memset(isw_mxp, 0, sizeof isw_mxp);
}

/* DIAG(W394) loc_hud_diag Bit 1: HUD-Zensus, solange ein anderer VT im Bild ist (vor li_smooth_apply: simulierte Kamera).
 * Je Liste die VT-Lagen in NDC und alle Draws nach der ersten Kopie des Weltziels bzw. in anderen Zielen (2D/HUD) mit
 * ihren 4 Bezugs-Vertices; jede 4. Ausgabe mit Bild beim naechsten Present. Nur Spielzustand, keine Eingaben. */
static void li_hud_census(const ip_list_t *L)
{
    static FILE *fz;
    static unsigned nseen, nout;
    loc_smooth_pub_t pb;
    float cam[16];
    double VP[16];
    unsigned cnt = 0, sp = 0, i;
    int j, vis = 0;
    ID3D11RenderTargetView *wt, *rts[16];
    unsigned nrt = 0;
    extern int loc_mouse_mission(void); /* loc_pad.c: eigener VT abgesetzt */
    if (nout >= 80u || !loc_mouse_mission() || !loc_smooth_peek(&pb) || !la_cam_of(L, cam, &cnt, &sp) || cnt < 8u) { return; }
    wt = la_world_rtv(L, &sp);
    for (i = 0; i < 16; i++) { VP[i] = cam[i]; }
    for (j = 0; j < LS_MAXV; j++) {
        double a[4];
        int k;
        if (j == pb.own || !pb.v[j].ok) { continue; }
        for (k = 0; k < 4; k++) { a[k] = pb.v[j].p[0] * VP[k] + pb.v[j].p[1] * VP[4 + k] + pb.v[j].p[2] * VP[8 + k] + VP[12 + k]; }
        if (a[3] > 1e-3 && a[3] < 250000.0 && fabs(a[0] / a[3]) < 1.0 && fabs(a[1] / a[3]) < 1.0) { vis = 1; } /* nah (< ~250 m) */
    }
    if (!vis || (nseen++ % 8u) != 0u) { return; }
    if (!fz) { fz = (FILE *)loc_aim_diag_file("lochud_census.txt"); }
    if (!fz) { return; }
    nout++;
    if ((nout % 4u) == 1u) { extern unsigned g_shot_census; g_shot_census = 1; }
    fprintf(fz, "LIST frame=%u serial=%u nops=%u split=%u shot=%d own=%d gtick=%u cam=", L->frame, L->serial, L->nops, sp,
            (nout % 4u) == 1u, pb.own, pb.gtick);
    for (i = 0; i < 16; i++) { fprintf(fz, "%.6g%s", cam[i], i < 15 ? "," : "\n"); }
    for (j = 0; j < LS_MAXV; j++) {
        double a[4];
        int k;
        if (!pb.v[j].ok) { continue; }
        for (k = 0; k < 4; k++) { a[k] = pb.v[j].p[0] * VP[k] + pb.v[j].p[1] * VP[4 + k] + pb.v[j].p[2] * VP[8 + k] + VP[12 + k]; }
        fprintf(fz, "  VT%d p=%.0f,%.0f,%.0f gier=%.4f ndc=%.4f,%.4f w=%.1f off=%.1f,%.1f,%.1f,%.4f on=%d\n", j, pb.v[j].p[0],
                pb.v[j].p[1], pb.v[j].p[2], pb.v[j].yaw, a[3] > 1e-6 ? a[0] / a[3] : 9.0, a[3] > 1e-6 ? a[1] / a[3] : 9.0, a[3],
                pb.v[j].off[0], pb.v[j].off[1], pb.v[j].off[2], pb.v[j].off[3], pb.v[j].on);
    }
    for (i = 0; i < L->nops; i++) {
        const ip_op_t *o = &L->ops[i];
        unsigned r;
        if (o->kind == IP_COPY) { fprintf(fz, "%u COPY\n", i); continue; }
        if (o->kind != IP_DRAW) { continue; }
        if (o->rtv == wt && i < sp && o->cls) { continue; } /* Welt (3D vor der Kopie) */
        for (r = 0; r < nrt && rts[r] != o->rtv; r++) {}
        if (r == nrt && nrt < 16u) { rts[nrt++] = o->rtv; }
        fprintf(fz, "%u rt=%u%s cls=%u nv=%u tex=%u srv=%p topo=%u vp=%.0f,%.0f,%.0f,%.0f sig=%08X rn=%.4f,%.4f;%.4f,%.4f;%.4f,%.4f;%.4f,%.4f w=%.3g\n",
                i, r, o->rtv == wt ? "W" : "", o->cls, o->nv, o->has_tex, (void *)o->srv, o->topo, o->vp.TopLeftX, o->vp.TopLeftY,
                o->vp.Width, o->vp.Height, o->sig, o->rn[0][0], o->rn[0][1], o->rn[1][0], o->rn[1][1], o->rn[2][0], o->rn[2][1],
                o->rn[3][0], o->rn[3][1], o->rn[0][2]);
    }
    fflush(fz);
}

/* W394 LOC-HUD-VIEW: HUD-Draws mit Weltbezug markieren (Zensus W394 C3, LoC-Hauptsicht 512x512 nach der Weltkopie):
 *  - Zielmarke/Lock-On-Rahmen je VT j: kleine untexturierte Linien-Draws (<= 32 Vertices, ausser dem festen 22er-Kreuz)
 *    mit Mitte in +-0,16 um die simulierte Bildlage a_j (Marke sitzt ~0,04 hoeher), dazu Beschriftungen (texturiert,
 *    6 Vertices) rechts oben daneben. Bewegt sich der Partner-Draw gegen die Vorliste anders als a_j (> 0,012), ist es
 *    ein ruhendes HUD-Element bzw. das Fadenkreuz -> nicht markiert; ohne Bewegung nur Linien mit 4/8/24 Vertices.
 *  - Kompassband: untexturierter Linien-Draw mit >= 100 Vertices, alle Bezugspunkte bei y -0,65..-0,48, Breite > 1.
 * Fadenkreuz-Draws (law 2) bleiben beim Fadenkreuz. */
static void li_hud_mark(ip_list_t *L, const ip_list_t *A)
{
    unsigned sp = 0, i, n = 0, ncmp = 0;
    int j;
    ID3D11RenderTargetView *wt;
    if (!L->hm_ok && !L->hm_gok) { return; }
    wt = la_world_rtv(L, &sp);
    if (!wt || !sp) { return; }
    for (j = 0; j < LS_MAXV; j++) {
        if (L->hm_c[j][3] > 0.0 && L->hm_c[j][3] < 100000.0 && fabs(L->hm_a[j][0]) < 0.9 && fabs(L->hm_a[j][1]) < 0.9) {
            InterlockedIncrement(&li_hm_near);
            break;
        }
    }
    if (L->hm_gok) { /* Radar (Kopf oben, Zensus C3): eigenes quadratisches Ziel <= 512 mit Karte (texturiert, >= 300 Vertices)
                       * und >= 8 kleinen untexturierten Linienzuegen (Ring, Marken) - die Lock-On-Kamera hat keine solchen */
        ID3D11RenderTargetView *rt = NULL, *cand[4];
        unsigned nc = 0, nr = 0, k2, nls;
        for (i = 0; i < L->nops; i++) {
            const ip_op_t *o = &L->ops[i];
            if (o->kind == IP_DRAW && o->cls && o->has_tex && o->nv >= 300u && o->rtv && o->rtv != wt && o->rtv != g_rtv &&
                o->vp.Width == o->vp.Height && o->vp.Width <= 512.0f) {
                for (k2 = 0; k2 < nc && cand[k2] != o->rtv; k2++) {}
                if (k2 == nc && nc < 4u) { cand[nc++] = o->rtv; }
            }
        }
        for (k2 = 0; k2 < nc && !rt; k2++) {
            for (i = 0, nls = 0; i < L->nops; i++) {
                const ip_op_t *o = &L->ops[i];
                if (o->kind == IP_DRAW && o->rtv == cand[k2] && !o->has_tex && o->topo == 3u && o->nv <= 16u) { nls++; }
            }
            if (nls >= 8u) { rt = cand[k2]; }
        }
        for (i = 0; rt && i < L->nops; i++) {
            ip_op_t *o = &L->ops[i];
            if (o->kind != IP_DRAW || !o->cls || o->rtv != rt || o->law) { continue; }
            if (A && A->hm_gok && o->has_tex && o->nv >= 300u && o->match >= 0 && (unsigned)o->match < A->nops) {
                /* Eichung: Drehung der Weltachse x im Clip (Karte) gegen die eigene Gier */
                const ip_op_t *a = &A->ops[o->match];
                double dg = la_wrap((double)L->hm_gier - A->hm_gier);
                double th = la_wrap(atan2(o->mvp[1], o->mvp[0]) - atan2(a->mvp[1], a->mvp[0]));
                if (fabs(dg) > 0.003 && fabs(dg) < 0.2 && fabs(fabs(th / dg) - 1.0) < 0.2) {
                    double sg = th / dg > 0.0 ? 1.0 : -1.0;
                    if (li_rad_n > 0 && sg != li_rad_k) { li_rad_n = 0; }
                    li_rad_k = sg;
                    if (li_rad_n < 1000) { li_rad_n++; }
                }
            }
            o->law = LA_LAW_RAD;
            nr++;
        }
        L->hm_nrad = nr;
        if (nr) { InterlockedIncrement(&li_hm_rad); }
    }
    for (i = sp + 1u; i < L->nops; i++) {
        ip_op_t *o = &L->ops[i];
        double cx, cy, ymin = 9.0, ymax = -9.0, xmin = 9.0, xmax = -9.0;
        int q, best = -1;
        if (o->kind != IP_DRAW || o->cls || o->rtv != wt || o->law) { continue; }
        for (q = 0; q < 4; q++) {
            if (o->rn[q][0] < xmin) { xmin = o->rn[q][0]; } if (o->rn[q][0] > xmax) { xmax = o->rn[q][0]; }
            if (o->rn[q][1] < ymin) { ymin = o->rn[q][1]; } if (o->rn[q][1] > ymax) { ymax = o->rn[q][1]; }
        }
        if (!o->has_tex && o->nv >= 100u && ymin > -0.65 && ymax < -0.48 && xmax - xmin > 1.0) { /* Kompassband */
            o->law = LA_LAW_CMP;
            ncmp++;
            if (A && A->hm_gok && L->hm_gok && o->match >= 0 && (unsigned)o->match < A->nops) { /* Eichung NDC je rad */
                double dg = la_wrap((double)L->hm_gier - A->hm_gier), dx = o->rn[0][0] - A->ops[o->match].rn[0][0];
                if (fabs(dg) > 0.003 && fabs(dg) < 0.2 && fabs(dx / dg - li_cmp_k) < 0.25) { li_cmp_k += 0.05 * (dx / dg - li_cmp_k); }
            }
            continue;
        }
        if (!L->hm_ok || o->nv > 32u || o->nv == 22u) { continue; }
        cx = (o->rn[0][0] + o->rn[1][0] + o->rn[2][0] + o->rn[3][0]) * 0.25;
        cy = (o->rn[0][1] + o->rn[1][1] + o->rn[2][1] + o->rn[3][1]) * 0.25;
        for (j = 0; j < LS_MAXV && best < 0; j++) {
            double ax = L->hm_a[j][0], ay = L->hm_a[j][1];
            if (L->hm_c[j][3] <= 0.0 || fabs(ax) > 1.3 || fabs(ay) > 1.3) { continue; }
            if (!o->has_tex) {
                if (fabs(cx - ax) < 0.16 && fabs(cy - ay - 0.04) < 0.16) { best = j; }
            } else if (o->nv == 6u && cx - ax > 0.05 && cx - ax < 0.5 && cy - ay > -0.05 && cy - ay < 0.25) {
                best = j;
            }
        }
        if (best < 0) { continue; }
        InterlockedIncrement(&li_hm_cand);
        {
            int moved_ok = -1; /* -1 unbekannt, 0 passt nicht, 1 passt */
            double ex = 0.0, ey = 0.0;
            if (A && A->hm_ok && A->hm_c[best][3] > 0.0 && o->match >= 0 && (unsigned)o->match < A->nops) {
                const ip_op_t *a = &A->ops[o->match];
                double ax0 = (a->rn[0][0] + a->rn[1][0] + a->rn[2][0] + a->rn[3][0]) * 0.25;
                double ay0 = (a->rn[0][1] + a->rn[1][1] + a->rn[2][1] + a->rn[3][1]) * 0.25;
                ex = L->hm_a[best][0] - A->hm_a[best][0];
                ey = L->hm_a[best][1] - A->hm_a[best][1];
                moved_ok = fabs((cx - ax0) - ex) < 0.012 && fabs((cy - ay0) - ey) < 0.012;
            }
            if (moved_ok == 0) { InterlockedIncrement(&li_hm_rej_mv); continue; }
            if (fabs(ex) + fabs(ey) < 0.004 && (o->has_tex || (o->nv != 4u && o->nv != 8u && o->nv != 24u))) {
                InterlockedIncrement(&li_hm_rej_st);
                continue;
            }
        }
        o->law = (unsigned char)(3 + best);
        L->hm_vt |= 1u << best;
        n++;
    }
    L->hm_n = n;
    L->hm_ncmp = ncmp;
    if (ncmp) { InterlockedIncrement(&li_hm_cmp); }
    if (n) { InterlockedIncrement(&li_hm_lists); InterlockedExchangeAdd(&li_hm_draws, (LONG)n); }
}

int sb_interp_swap(void)
{
    int mission, took = 0, k, li;
    static unsigned npair_log, ndump;
    LONGLONG t0, now;
    ip_list_t *L = NULL, *A = NULL;
    if (g_cfg.interp_fps <= 0) { return 0; }
    if (!ip_init()) { return 0; }
    t0 = ip_now();
    if (g_cfg.im_lock_split > 0 && ip_rec >= 0 && ip_rec < IP_LISTS) { ip_snap_classify(&g_ipl[ip_rec]); } /* LOC-IM-LOCK: ohne ctx_lock */
    sb_ctx_enter();
    if (g_cfg.imlock_diag) { memset(isw_q, 0, sizeof isw_q); isw_last = ip_now(); } /* DIAG(LOC-IM-LOCK) */
    ip_swapno++; /* W361: Takt der verzoegerten Freigabe */
    ip_gpu_poll();
    ISW(0);
    if (g_cfg.interp_guard_diag) { ip_gd_poll(); } /* DIAG(W265) */
    mission = ip_mission();
    ip_K = g_cfg.interp_fps / ip_tick_hz(); /* W323: LoC-Mission 20 Hz (120 -> K=6), SB 30 */
    if (ip_K < 1) { ip_K = 1; }
    if (ip_K > 8) { ip_K = 8; }
    ip_dt = ip_qpf.QuadPart / g_cfg.interp_fps;
    /* 1. Aufnahme abschliessen und sichern. */
    if (ip_rec >= 0) {
        li = ip_rec;
        L = &g_ipl[li];
        ip_rec = -1;
        st_ticks++;
        L->frame = g_present_no;
        L->serial = ++ip_serial;
        L->tswap = t0;
        if (!mission || L->overflow || !L->nops) {
            if (L->overflow) { st_ovf++; }
            ip_list_reset(L);
            ip_hist_clear();
            L = NULL;
        } else {
            ip_list_snap(L, li);
            ISW(1);
            if (!L->snap_ok) {
                ip_list_reset(L);
                ip_hist_clear();
                L = NULL;
            }
        }
    }
    if (L) {
        L->ready = 1;
        if (g_sba_loc) { li_ticks++; } /* W323 */
        if (g_cfg.ip_vb_gpu > 0) { ip_gpu_copy(L); } /* PERF(PERF-M2,W174) */
        ISW(2);
        ip_w_upload(L); /* FIX(W237) interp_hlerp */
        ISW(3);
        if (g_cfg.interp_rhash & 2) { ip_rh_take(L, L->rh); } /* DIAG(W273) */
        if (g_sba_loc && (g_cfg.loc_aim_diag & 10)) { li_aim_diag(L); } /* DIAG(W376) */
        if (g_sba_loc && (g_cfg.loc_local_aim || (g_cfg.loc_aim_diag & 32))) { li_aim_attach(L); } /* W376 LOC-LOCAL-AIM */
        ISW(4);
        if (g_sba_loc && (g_cfg.loc_hud_diag & 1)) { li_hud_census(L); } /* DIAG(W394) */
        L->hm_ok = 0; L->hm_vt = 0; L->hm_n = 0; L->hm_ncmp = 0; L->hm_nrad = 0; L->hm_gok = 0; L->hm_dyaw = 0.0f; /* W394 */
        if (g_sba_loc && (g_cfg.loc_rb_smooth || g_cfg.loc_own_predict || (g_cfg.loc_rb_smooth_diag & 10))) { li_smooth_apply(L); } /* W385 LOC-SMOOTH, W390 */
        if (g_sba_loc && g_cfg.loc_hud_predict && !L->hm_ok) { li_hud_plain(L); } /* W394 LOC-HUD-VIEW */
        ISW(5);
        A = ip_nhist ? &g_ipl[ip_hist[ip_nhist - 1]] : NULL;
        /* 2. Zuordnung gegen den Vortick. */
        if (A) {
            int log = (g_cfg.interp_diag >= 1) &&
                      ((npair_log < 20u && L->nops >= 500u) || (ip_pairs_total % 300u) == 0u);
            if (log) { npair_log++; }
            if (g_cfg.swap_match_unlock) {
                /* PERF(PERF-TICK,W168) swap_match_unlock=1: Zuordnung ist reine CPU-Arbeit (keine Context-Aufrufe)
                 * auf L (noch nicht in der Historie) und A->ops[].taken (liest die Wiedergabe nicht) -> ctx_lock
                 * waehrenddessen abgeben, damit der Present-Takt nicht 1-3 ms auf den Swap wartet. Ohne Takt-Thread
                 * (Testtakt) wirkungslos. */
                int cr;
                ISW(6); isw_end(); /* DIAG(LOC-IM-LOCK): Halte-Abschnitt endet mit der Freigabe */
                cr = sb_ctx_release_all();
                L->pair_ok = ip_match(A, L, log);
                sb_ctx_reacquire(cr);
            } else
            L->pair_ok = ip_match(A, L, log);
            L->prevser = A->serial;
            if (g_cfg.fxr_diag) { /* DIAG(REM-FX,W133): Zuordnung der Effekt-Draws */
                unsigned q;
                for (q = 0; q < L->nops; q++) {
                    const ip_op_t *fo = &L->ops[q];
                    if (fo->kind == IP_DRAW && fo->fxk) { fxr_interp_note(fo->fxk, fo->why, fo->flag && L->pair_ok); }
                }
            }
            if (g_cfg.interp_diag >= 2 && ndump < 2u && L->nops >= 500u) {
                ndump++;
                ip_dump(L);
            }
            st_pairs++;
            ip_pairs_total++;
        } else {
            st_nopair++;
        }
        if (g_sba_loc && L->la_ok) { li_aim_reticle(L, A); } /* W376 LOC-LOCAL-AIM: braucht die Zuordnung */
        if (g_sba_loc && g_cfg.loc_hud_predict) { li_hud_mark(L, A); } /* W394 LOC-HUD-VIEW: nach dem Fadenkreuz */
        if (g_sba_loc && (g_cfg.loc_wlag_diag & 1)) { li_wlag_after(L); } /* DIAG(W412 LOC-WORLD-LAG) */
        if (g_sba_loc && (g_cfg.loc_stat_diag & 1)) { li_stat_after(A, L); } /* DIAG(W415 LOC-STATIC-LAG) */
        /* 3. Zustandszeit: Fensteranfang (Feldwechsel) + mittlere Rechenzeit r
         *    (regelmaessiger als die Swap-Zeit selbst); Testtakt: Swap-Zeit. */
        L->tstate = t0;
        LONGLONG vb = ip_vb_get(); /* W323: LoC schreibt der Haupt-Thread */
        if (g_sba_loc && L->tvb0 && g_cfg.loc_interp_anchor) { vb = L->tvb0; } /* W340: Feld des Tick-Anfangs */
        if (g_cfg.vblank_hz > 0 && vb && t0 - vb < ip_qpf.QuadPart / 10) {
            double r = (double)(t0 - vb);
            double rms = r * 1000.0 / (double)ip_qpf.QuadPart;
            ip_rbar = ip_rbar > 0.0 ? ip_rbar * 0.9 + r * 0.1 : r;
            L->tstate = vb + (LONGLONG)ip_rbar;
            st_r_ms += rms;
            if (rms > st_r_max) { st_r_max = rms; }
            st_r_n++;
            ip_r_last_ms = rms; /* PERF(TICK-PERF,W125): interp_gpu_tight */
        }
        if (A && L->tstate <= A->tstate) { L->tstate = A->tstate + 1; }
        if (g_sba_loc && A) { /* W323: Zustandsabstand gegen 1/Takt */
            double dv = fabs(ip_ms(A->tstate, L->tstate) - 1000.0 / (double)ip_tick_hz());
            if (dv > li_dev_max) { li_dev_max = dv; }
            if (dv > 2.0) { li_dev2++; }
        }
        if (A && ip_pace_on() && ip_tick_hz() > 0) {
            /* W393 LOC-PACING: geglaettete Tick-Zeitbasis. Die Zustandszeit liegt auf dem Feldraster (Feld des Tick-
             * Anfangs + mittlere Rechenzeit); beginnt ein Tick ein Feld (16,7 ms) zu spaet und holt der naechste auf,
             * lief die Anzeige einen Tick 25 % langsamer und dann 50 % schneller (Mikroruckeln). Jetzt: Erwartung
             * = vorige Zustandszeit + 1/Takt, die Abweichung fliesst nur zu 1/8 ein (hoechstens 4 % des Takts je Tick);
             * mehr als 1/2 Takt bzw. zweimal in Folge mehr als 1/8 Takt daneben (Zeit verloren) setzt neu an. */
            LONGLONG P = ip_qpf.QuadPart / ip_tick_hz(), e = L->tstate - (A->tstate + P);
            int sn;
            L->tstate = (LONGLONG)pace_ts_step(A->tstate, L->tstate, P, &sn, &ip_ts_laste);
            if (sn) { pc_ts_snap++; } else { pc_ts_s++; }
            if (!sn && (e > P / 8 || e < -P / 8)) { pc_ts_big++; } /* DIAG: so viel haette die Anzeige geruckt */
        }
        /* 4. Latenz L = max(Swap_N - Zustand_N-1) der letzten 32 Ticks + 1 ms. */
        if (A) {
            unsigned w;
            LONGLONG mx = 0;
            ip_need[ip_nneed++ % 32u] = t0 - A->tstate;
            for (w = 0; w < 32u && w < ip_nneed; w++) {
                if (ip_need[w] > mx) { mx = ip_need[w]; }
            }
            ip_lat = mx + ip_qpf.QuadPart / 1000;
            if (ip_lat > ip_qpf.QuadPart / 12) { ip_lat = ip_qpf.QuadPart / 12; } /* <= 83 ms */
            st_lat_ms += (double)ip_lat * 1000.0 / (double)ip_qpf.QuadPart;
            st_lat_n++;
        }
        /* 5. Historie: juengste 3 Listen behalten. */
        if (ip_nhist == IP_HIST) {
            ip_rt_drop(ip_hist[0]); /* W126 RENDER-THREAD: Auftrag mit dieser Liste abwarten/verwerfen */
            ip_list_reset(&g_ipl[ip_hist[0]]);
            memmove(ip_hist, ip_hist + 1, (IP_HIST - 1) * sizeof ip_hist[0]);
            ip_nhist--;
        }
        ip_hist[ip_nhist++] = li;
        ip_shots_tick(L);
        if (A && g_cfg.interp_diag >= 3 && g_cfg.seam_diag > 0) { ip_seam_diag(A, ip_list_index(A), L, li); } /* DIAG(W406) */
        if (A && g_cfg.interp_diag >= 3 && g_cfg.interp_ddrel > 0) { /* DIAG(W251 INTERP-GROUND) */
            unsigned m0 = sb_pf_m0(), r = (unsigned)g_cfg.interp_ddrel;
            if (m0 && L->frame >= m0 + r && L->frame < m0 + r + 3u) {
                if (!ip_dd_lo) { ip_dd_lo = L->serial; }
                ip_dd_ser = L->serial;
                ip_diag_tick(A, ip_list_index(A), L, li);
            }
        } else if (A && g_cfg.interp_diag >= 3 && g_cfg.interp_ddfile[0]) { /* DIAG(W284 INTERP-RAWCHG) */
            unsigned m0 = sb_pf_m0();
            if (m0 && L->frame >= m0 && ip_ddf_has(L->frame - m0)) {
                L->ddsel = 1;
                ip_diag_tick(A, ip_list_index(A), L, li);
            }
        } else if (A && g_cfg.interp_diag >= 3 && ip_shot_ser == L->serial) {
            ip_diag_tick(A, ip_list_index(A), L, li);
        }
        if (A && g_cfg.interp_verify > 0 && (ip_pairs_total % (unsigned)g_cfg.interp_verify) == 1u) {
            /* W126 RENDER-THREAD: exakte Wiedergabe ueber denselben Pfad wie die
             * Zwischenbilder (Command-List des Render-Threads) pruefen. */
            ID3D11CommandList *vcl = ip_rt_active() ? ip_rt_get(A, L, 1.0f) : NULL;
            if (vcl) { ip_rt_exec(vcl); }
            else if (!ip_dead_n) { ip_replay(L, li, A, ip_list_index(A), 1.0f); } /* W361: tote View nie binden */
            ip_verify_check(L, li);
        }
        if (A && g_cfg.interp_mode == 1 && L->pair_ok && sb_rp_want()) { ip_rp_tick(A, ip_list_index(A), L, li); } /* W295 WORLD-REPROJ */
    }
    /* 6. Naechste Aufnahme (freie Liste). */
    if (mission) {
        for (k = 0; k < IP_LISTS; k++) {
            int inh = 0, j;
            for (j = 0; j < ip_nhist; j++) {
                if (ip_hist[j] == k) { inh = 1; }
            }
            if (!inh) { break; }
        }
        ip_list_reset(&g_ipl[k]);
        ip_bset_rotate(&g_ipl[k]); /* PERF(TICK-PERF,W125) interp_bsets */
        ip_rec = (ip_vb_ensure(&g_ipl[k]) && ip_rq_ensure(&g_ipl[k])) ? k : -1;
        if (ip_rec >= 0 && g_cfg.render_thread > 0) { /* W126: s0 am Tick-Anfang (Deferred-Wiedergabe) */
            ID3D11DeviceContext_PSGetSamplers(g_ctx, 0, 1, &g_ipl[k].smp0);
        }
    } else {
        ip_hist_clear();
        ip_rec = -1;
    }
    g_ip_recording = (ip_rec >= 0);
    now = ip_now();
    st_rec_ms += ip_ms(t0, now);
    if (g_phud_meas) { sb_phud_interp_q(now - t0, 0); } /* W122 PERF-HUD: Aufzeichnung/Zuordnung */
    /* 7. Zeigen. Testtakt: K Bilder (t = k/K) des Paars sofort am Swap.
     *    Echtzeit: Takt starten bzw. weiterlaufen lassen (zeigt auch das
     *    Spiel-Bild dieses Ticks, um L verzoegert). */
    if (L && ip_K > 1) {
        if (g_cfg.vblank_hz <= 0) {
            if (A) {
                char tag[8];
                if (g_cfg.interp_rhash & 2) { ip_rh_check(A, "A", L->frame, 0); ip_rh_check(L, "B", L->frame, 0); } /* DIAG(W273) */
                for (k = 1; k <= ip_K; k++) {
                    _snprintf_s(tag, sizeof tag, _TRUNCATE, "%d", k);
                    if (ip_rt_ok && k + 1 < ip_K) { ip_rt_nA = A; ip_rt_nB = L; ip_rt_nt = (float)(k + 1) / (float)ip_K; } /* W126 */
                    ip_show(A, L, k >= ip_K ? 1.0f : (float)k / (float)ip_K, tag);
                    ip_rt_nA = NULL;
                }
                if (g_cfg.interp_rhash & 2) { ip_rh_check(A, "A", L->frame, 9); } /* DIAG(W273): nach den Bildern */
                took = 1;
            }
        } else if (ip_nhist >= 2) {
            if (!ip_live) {
                ip_live = 1;
                ip_tnext = now;
                ip_tlast_present = 0;
                if (pd_wake) { SetEvent(pd_wake); } /* W138: Takt-Thread sofort auf den Interp-Takt */
            }
            took = 1;
        }
    }
    if (!took) { ip_live = 0; }
    ip_rl_drain(0); /* W361: faellige ersetzte Shader/Layouts (vor dem neuen Auftrag) */
    if (ip_sh_rc_watch(1, NULL, NULL, "swap") || ip_dead_n) { ip_dead_purge(); } /* W361: Zaehlerwache der Schatten-Views */
    if (ip_live) { ip_rt_prefetch(); } /* W126 RENDER-THREAD: Historie neu -> naechstes Bild vorbereiten */
    ISW(6); isw_end(); /* DIAG(LOC-IM-LOCK) */
    sb_ctx_leave();
    if (ip_live) { ip_poll(); }
    if (g_cfg.interp_diag >= 1 && st_ticks >= 300u) { sb_ctx_enter(); ip_stat_log(); sb_ctx_leave(); } /* W138: Zaehler auch vom Takt-Thread */
    return took;
}

/* ---------- ISSUE-PHOTO: Aufzeichnung bei Bedarf ohne FPS-INTERP (interp_fps=0) ----------
 * Fotos ueber Fenstergroesse (4K/8K/xN), "nur Aussenwelt", HUD aus und die freie Kamera brauchen die Draw-Liste
 * eines Ticks (sb_interp_photo_render). Mit interp_fps=0 zeichnet sb_interp_swap nichts auf; auf Wunsch des
 * Fotomodus bzw. Schnell-Screenshots (render_photo.c) werden hier ZWEI Ticks aufgezeichnet - derselbe Weg wie
 * mit FPS-INTERP (g_ip_recording, Tick-VB, Schatten), nur ohne Zwischenbilder, ohne Zuordnung und ohne
 * Offscreen-BB: das Spiel zeichnet weiter direkt in den Swapchain-BB, gesichert wird am Swap vor der
 * Endbild-Nachbearbeitung. Der erste Tick (A) liefert nur den Anfangsstand der Ziele fuer den zweiten (B, das
 * Foto) - wie der Vortick mit FPS-INTERP; mit B allein startet die Wiedergabe vom Endstand von B, und Ziele, die
 * das Spiel nicht loescht, sondern ueberblendet, weichen ab (Messung: 4600-9100 px, max 4-7). Ohne Wunsch je
 * Present zwei Vergleiche, keine Aufzeichnung, keine Puffer (Standardpfad bitgleich). Spiel-Thread. */
static volatile LONG ip_r1_want;
static int ip_r1_st;     /* SB_REC1_* (ohne WAIT) */
static int ip_r1_n;      /* fertige Ticks dieser Aufzeichnung (0 = A laeuft, 1 = B laeuft bzw. folgt) */
static int ip_r1_next;   /* A fertig: B beginnt nach dem naechsten Present */
static LONGLONG ip_r1_t0;

void sb_interp_rec1_request(void)
{
    InterlockedExchange(&ip_r1_want, 1);
}

int sb_interp_rec1_state(void)
{
    if (ip_r1_want || ip_r1_next) { return SB_REC1_WAIT; }
    if (ip_r1_st == SB_REC1_RUN && ip_rec < 0) { return SB_REC1_FAIL; } /* zwischendurch sb_interp_reset */
    if (ip_r1_st == SB_REC1_RUN && ip_r1_n == 0) { return SB_REC1_WAIT; } /* erst A; die Pause gilt nach B */
    if (ip_r1_st == SB_REC1_READY && !(ip_nhist >= 1 && g_ipl[ip_hist[ip_nhist - 1]].ready)) { return SB_REC1_NONE; }
    return ip_r1_st;
}

/* Naechste Liste aufnehmen (k = Listen-Index). 1 = laeuft. Unter ctx_lock. */
static int ip_r1_start(int k)
{
    ip_list_reset(&g_ipl[k]);
    ip_rec = (ip_vb_ensure(&g_ipl[k]) && ip_rq_ensure(&g_ipl[k])) ? k : -1;
    g_ip_recording = (ip_rec >= 0);
    ip_r1_st = g_ip_recording ? SB_REC1_RUN : SB_REC1_FAIL;
    return g_ip_recording;
}

/* sb_render_present nach sb_pause_point: der folgende Tick wird aufgezeichnet (A bzw. nach A B). */
void sb_interp_rec1_arm(void)
{
    int mission, ok = 0;
    if (!ip_r1_want && !ip_r1_next) { return; }
    if (ip_r1_next) { /* B direkt nach A */
        ip_r1_next = 0;
        if (ip_rec >= 0 || g_cfg.interp_fps > 0 || sb_render_offscreen()) { ip_r1_st = SB_REC1_FAIL; return; }
        sb_ctx_enter();
        ok = ip_r1_start(1);
        sb_ctx_leave();
        if (!ok) { HLOG_W("PHOTO: Aufzeichnung bei Bedarf: Tick-Puffer fuer B fehlgeschlagen"); }
        return;
    }
    InterlockedExchange(&ip_r1_want, 0);
    ip_r1_st = SB_REC1_FAIL;
    if (g_cfg.interp_fps > 0 || sb_render_offscreen() || ip_rec >= 0) { return; } /* FPS-INTERP zeichnet selbst auf */
    if (!ip_init()) {
        HLOG_W("PHOTO: Aufzeichnung bei Bedarf nicht moeglich (INTERP-Init)");
        return;
    }
    sb_ctx_enter();
    mission = ip_mission();
    if (mission) {
        ip_hist_clear(); /* alte Bedarfs-Listen */
        ip_r1_n = 0;
        ok = ip_r1_start(0);
        ip_r1_t0 = ip_now();
    }
    sb_ctx_leave();
    HLOG_I("PHOTO: Aufzeichnung bei Bedarf (interp_fps=0): %s (f=%u)",
           !mission ? "ausserhalb der Mission - keine" : ok ? "die naechsten 2 Ticks" : "Tick-Puffer fehlgeschlagen",
           g_present_no);
}

/* Swap (sb_render_present_body, direkter BB vor der Endbild-Nachbearbeitung): Tick sichern. */
void sb_interp_rec1_end(void)
{
    ip_list_t *L;
    int li, ok = 0;
    unsigned nops, nverts;
    if (ip_r1_st != SB_REC1_RUN) { return; }
    if (ip_rec < 0 || g_cfg.interp_fps > 0 || sb_render_offscreen()) { ip_r1_st = SB_REC1_FAIL; return; }
    sb_ctx_enter();
    li = ip_rec;
    L = &g_ipl[li];
    ip_rec = -1;
    g_ip_recording = 0;
    L->frame = g_present_no;
    L->serial = ++ip_serial;
    L->tswap = L->tstate = ip_now();
    nops = L->nops;
    nverts = L->nverts;
    if (ip_mission() && !L->overflow && L->nops) {
        ip_list_snap(L, li);
        ok = L->snap_ok;
    }
    if (ok) {
        L->ready = 1;
        ip_hist[ip_r1_n] = li;
        ip_nhist = ip_r1_n + 1;
        ip_r1_n++;
        if (ip_r1_n >= 2) { ip_r1_st = SB_REC1_READY; } else { ip_r1_next = 1; } /* A fertig -> B */
    } else {
        ip_list_reset(L);
        ip_r1_st = SB_REC1_FAIL;
    }
    sb_ctx_leave();
    HLOG_I("PHOTO: Aufzeichnung bei Bedarf %s %s: Tick f=%u ops=%u vertices=%u ziele=%d ueberlauf=%d (%.1f ms)",
           li ? "B" : "A", ok ? "fertig" : "fehlgeschlagen", L->frame, nops, nverts, ok ? L->nrefs : 0, L->overflow,
           ip_ms(ip_r1_t0, ip_now()));
}

/* Nach dem Foto bzw. beim Verlassen des Fotomodus: Listen, Schatten und Tick-Puffer frei (keine Dauerkosten). */
void sb_interp_rec1_free(void)
{
    int i;
    InterlockedExchange(&ip_r1_want, 0);
    ip_r1_next = 0;
    ip_r1_n = 0;
    if (ip_r1_st == SB_REC1_NONE || !ip_init_done || g_cfg.interp_fps > 0 || sb_render_offscreen()) {
        ip_r1_st = SB_REC1_NONE;
        return;
    }
    sb_ctx_enter();
    sb_interp_reset("Foto: Aufzeichnung bei Bedarf frei");
    for (i = 0; i < IP_LISTS; i++) {
        ip_list_t *L = &g_ipl[i];
        if (L->vb) { ID3D11Buffer_Release(L->vb); L->vb = NULL; }
        if (L->rqxsrv) { ID3D11ShaderResourceView_Release(L->rqxsrv); L->rqxsrv = NULL; }
        if (L->rqx) { ID3D11Buffer_Release(L->rqx); L->rqx = NULL; }
        if (L->rqc) { ID3D11Buffer_Release(L->rqc); L->rqc = NULL; }
        if (L->wb) { ID3D11Buffer_Release(L->wb); L->wb = NULL; }
        free(L->ipw);
        L->ipw = NULL;
        L->vbcap = L->rqxcap = L->rqccap = L->wbcap = L->ipwcap = 0;
        L->nodisc = 0;
    }
    ip_r1_st = SB_REC1_NONE;
    sb_ctx_leave();
}
