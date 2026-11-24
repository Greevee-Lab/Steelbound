/* render_interp_int.h - W463 RCORE: interner Header der FPS-INTERP-Module (render_interp*.c).
 * Typen, Konstanten und der modulübergreifende Zustand der ehemals einen Datei render_interp.c
 * (reines Verschieben, keine Verhaltensänderung). Nur von render_interp*.c einbinden; die
 * öffentliche Schnittstelle bleibt in render_int.h / render_qa.h. Modulkarte: docs/RENDER_ARCH.md. */
#ifndef RENDER_INTERP_INT_H
#define RENDER_INTERP_INT_H
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

/* RENDER-QA (W77): Tick-Puffer fuer Stream 2 / Konstanten-Fenster des
 * Shading-Pfads (nur shading=1). 1 = vorhanden. */
#define IP_RQX_INIT (64u << 20)
#define IP_RQX_MAX (256u << 20)
#define IP_RQC_INIT (256u * 1024u)
#define IP_RQC_MAX (2048u * 1024u)
/* DIAG(PERF-M2,W174) ip_gdiag: GPU-Zeit der Kopien am Swap (Timestamps, asynchron) */
#define GC_N 8
/* W265 DIAG: Pipeline-Statistik je Zwischenbild-Wiedergabe (nur Immediate) */
#define IP_GD_Q 64

/* Partnersuche ueber den ganzen Vortick (W73): Draws von A je Signatur-Eimer
 * in aufsteigender Reihenfolge verkettet. Frueher Fenster von 256 Draws ab dem
 * letzten Treffer - ein im Vortick fehlender/geaenderter Block > 256 Draws
 * (z. B. ein Monitor-Pass, der nur jeden 2. Tick neu gebaut wird) liess alle
 * folgenden Draws des Ticks ohne Partner (W73_d1: ~600 nomatch je 2. Tick). */
#define IP_HBITS 12
#define IP_HB(k) (((k) ^ ((k) >> IP_HBITS) ^ ((k) >> 24)) & ((1u << IP_HBITS) - 1u))

/* PERF(PERF-M2,W174) ip_match_compact: Signatur, Eingangs-Hash und Bezugspunkte der Draws von A
 * dicht gepackt (ip_inst_best lief die Hash-Kette ueber die grossen ip_op_t; 46 % der Swap-Proben
 * in schweren M2-Ticks, W174_r2). Gleiche Vergleiche, gleiche Reihenfolge -> gleiche Zuordnung. */
typedef struct { unsigned sig, rawhash; float rn[4][3]; } ip_ck_t;

/* Geometrie-Partner (W73): gleiche Eingangs-Positionen, gleiches Ziel und
 * gleiche Vertexzahl, aber anderer Shader/andere Textur (W73_d3: Sub-Monitor-
 * Paesse wechseln zwischen Ticks Material/Shader, ~760 Draws ohne Partner).
 * Freie Partner bevorzugt, sonst darf ein schon vergebener Draw des Vorticks
 * ein zweites Mal als Positionsquelle dienen (Farben/UV kommen aus Tick N). */
#define IP_COST_INF 1e9f


/* FIX(W284 INTERP-RAWCHG) interp_rawzc: Kamerabewegungen D = M_A^-1 * M_B der (bis zu) 8 groessten
 * plausibel gelerpten Welt-Draws eines Ziels (wie interp_zcons, dort im Block). Rueckgabe: Anzahl. */
/* PERF(W284): die 8 groessten plausibel gelerpten Welt-Draws je Ziel (rtv, dsv) in EINEM Durchlauf
 * ueber B (vorher ein Durchlauf je Ziel, ~0,2 ms je Tick); gleiche Auswahl und Reihenfolge wie dort. */
enum { RZ_PT = 32 };

/* W294 Bit 1: Kamera-Kandidaten nach Mehrheit statt Groesse. Ruhende Welt-Geometrie mit Matrix = VP
 * (Kacheln, Gelaendestreifen) teilt sich bitgenau dasselbe Matrix-Paar (M_A, M_B); in M2 faehrt die Kamera
 * mit dem Landungsboot, die groessten 3D-Draws sind Boot-Teile (D ~ I) und fahrende Mechs. Gezaehlt wird je
 * Paar (gleiche Matrizen N und N-1) unter den gelerpten Kandidaten; die 8 haeufigsten Paare je Ziel
 * (Gleichstand: groesseres n, dann Reihenfolge) liefern D. */
enum { RZ_HB = 4096 };
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
typedef struct { ID3D11RenderTargetView *rtv; ID3D11DepthStencilView *dk; int ok, ref; unsigned nstat; double Di[16]; } pr_tg_t;
typedef struct { float c; int o, a; } pr_pair_t;

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

/* PERF(RENDER-THREAD,W126): Ziel-Context einer Wiedergabe. Immediate (Spiel-
 * Thread, bisheriger Pfad) oder Deferred (Render-Thread, eigene Konstanten-
 * puffer; Zustand beginnt leer, wird nicht gesichert/wiederhergestellt). */
typedef struct {
    ID3D11DeviceContext *c;
    ID3D11DeviceContext1 *c1;
    ID3D11Buffer *cb0, *cb1;
    int deferred;
} ip_cx_t;

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

/* DIAG(PERF-M2,W174) ip_gdiag: Zaehler je Wiedergabe */
typedef struct {
    unsigned nops, ndraw, nlerp, nrq, nclr, nmap, nrtv, nsrv, ncb, slots, var, nverts, rqxb, ring, vbg;
    unsigned long long vbytes;
} ip_rc_t;

/* DIAG(TICK-PERF,W125) perf_gsplit=N: jede N-te Zwischenbild-Wiedergabe mit
 * Timestamps an jedem Zielwechsel; synchron ausgewertet (Diagnose, kostet
 * einen GPU-Sync je Messung). Zeile GPUSPLIT: Summe je Zielgroesse. */
#define IP_GS_MAX 512

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
#define IP_PL_SRV0(s) (ip_pl_tex ? sb_tex_pause_map(s) : (s))
#define IP_PL_SRV(s) ip_ph_srv(IP_PL_SRV0(s))
#define IP_RP_ISW(o, rw, rd) ((((o)->kind == IP_DRAW || (o)->kind == IP_CLR_RTV) && (o)->sh_rtv && (o)->sh_rtv == (rw)) ||     ((rd) && !(o)->sh_rtv && (o)->kind != IP_CLR_RTV && (o)->sh_dsv == (rd)))
#define IP_DEAD_MAX 8

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

/* ---------- W361 LOC-INTERP-CRASH: verzoegerte Freigabe ----------
 * Die Ops der Zwischenbild-Listen tragen RENDER-QA-Shader/Layouts roh (rq_rec_t vs/vsl/lay/layl/ps). Ersetzt
 * render_shade.c einen Cache-Eintrag (Handle mit neuem Programm, Roh-Variante mit anderem Vertex-Format), darf
 * das alte Objekt erst weg, wenn keine Liste es mehr enthalten kann: eine Liste lebt hoechstens Aufnahme (1 Swap)
 * + Historie (IP_HIST Swaps), danach IP_LISTS + 1 Swaps Abstand und vorher den Render-Thread abwarten. Ohne Listen
 * (Zwischenbilder aus, keine Foto-Aufnahme) sofort. Aufruf auch ohne ctx_lock (render_shade gibt ihn beim
 * Erstellen ab) - eigene Sperre. */
typedef struct { IUnknown *u; unsigned due; } ip_rl_t;

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

/* PERF(TICK-PERF,W125) interp_gpu_q=N: Event-Query nach jedem Present; ein
 * Zwischenbild (Wiedergabe t<1) wird nur eingereiht, wenn die GPU das N-letzte
 * gezeigte Bild fertig hat. Ursache (W125_b2/b3): bei GPU-Last stapeln sich
 * Wiedergaben vor den synchronen Stellen des Spiel-Ticks (Sichtbarkeitstest
 * GetVisibilityTestResult wartet blockierend wie das Original, visq 7-29 ms/
 * Tick) -> Tick-Last > 33 ms, Spiel < 30 Ticks/s. Mit der Grenze verfaellt
 * dann ein Zwischenbild statt dass der Tick wartet. */
#define IP_EV_N 8

/* ---------- DIAG(W393 LOC-PACING) pace_diag: Bildtakt je 10 s (Zeile PACE) ----------
 * Je gezeigtem Zwischenbild: Abstand der Presents (QPC vor Present), gezeigte Spielzeit g = Listen-Nummer von A +
 * t (in Ticks), Phase der Faelligkeit im Tick, Latenz-Wechsel. Nur Zaehler/Zeiten, keine Eingaben. */
#define PC_N 4096

void *sb_iml_holder_site(void);                 /* DIAG(LOC-IM-LOCK), d3d11_render.c */
void sb_iml_pd_wait(LONGLONG dq, void *site);
void sb_iml_log(double secs);

/* ctx_lock mit Vortritt nehmen (Takt-Thread). */
#include "hlock.h" /* W168 PERF-TICK pd_gw_nopri */
extern const hlock_t *sb_ctx_hlock(void);

/* ---------- DIAG(W412 LOC-WORLD-LAG) loc_wlag_diag Bit 1 ----------
 * Je Liste (am Swap): Blickrichtung (Gier) der simulierten Kamera VP und der gezeigten Kamera (VP mit K, W390) aus dem
 * Augpunkt und der Bildmitte; Vergleich der gezeigten Gier eines Ticks mit der simulierten Gier s Ticks spaeter
 * (s = -3..+4): kleinster Fehler bei s = Vorsprung. Nach der Zuordnung: Draws des Weltziels je Klasse (o->wlc) und ob sie
 * in den Zwischenbildern gelerpt (mit der Kamera) oder exakt aus Tick N stehen. Zeile LOCWLAG alle 10 s (nur Zaehler,
 * Winkel, keine Eingaben). */
#define WL_SH 8 /* Verschiebungen s = -3..+4 */
#define WL_RING 16u

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
typedef struct { float px; unsigned f, i, nv, tex, why, st, wlc, g; float x, y; } ls15_top_t;

/* DIAG(LOC-IM-LOCK) imlock_diag=1: Teilzeiten des Swaps unter ctx_lock bis zur Zuordnung (lange > 1 ms summiert).
 * 0 gpu_poll, 1 snap, 2 gpu_copy, 3 w_upload, 4 aim, 5 smooth, 6 bis Freigabe */
#define ISW_N 7
#define ISW(k) do { if (isw_last) { long long t_ = ip_now(); isw_q[k] += t_ - isw_last; isw_last = t_; } } while (0)

/* ---------- Modulübergreifender Zustand (Definition im genannten Modul) ---------- */
/* render_interp.c */
extern volatile LONG li_cp_rec, li_cp_ok, li_cp_rep;
extern volatile LONG wl_fix_lists, wl_fix_draws;
extern volatile LONG sk_lists, sk_draws, sk_rep, sk_rep_on, sk_rep_sky, sk_rep_skyc, sk_rep_na, sk_klists, sk_klists_sky, sk_klists_has;
extern volatile LONG sk_rejlerp;
extern double sk_last_ang, sk_ang_sum, sk_ang_max;
extern volatile LONG li_cp_seen;
extern unsigned g_ip_skip_tot;
extern unsigned g_ip_sigparts[3];
extern unsigned g_ip_ib1;
extern float (*ip_cpos[IP_LISTS])[4];
extern const float *g_ip_tw;
extern unsigned g_ip_tw_n, g_ip_tw_prim;
extern ID3D11Texture2D *ip_vstg[2];
/* render_interp_rec.c */
extern float *ip_cw[IP_LISTS];
extern float (*ip_cj[IP_LISTS])[2];
extern unsigned char *ip_cp2[IP_LISTS];
extern unsigned ip_cwcap[IP_LISTS];
extern unsigned st_eyex_draws, st_eyex_verts, st_eyex_ln;
extern unsigned st_p2x_draws, st_p2x_verts, st_p2x_hit, st_jmp_verts, st_jmp_fp, st_p2x_ln;
extern unsigned st_p2rej;
extern unsigned st_p2hom[2];
extern unsigned st_jf[IP_W_N], st_jb[IP_W_N], st_jfd[IP_W_N];
extern unsigned st_wwhy[IP_W_N];
extern ip_list_t g_ipl[IP_LISTS];
extern unsigned st_hw_up, st_hw_fail;
extern unsigned st_hw_draws, st_hw_one;
extern ip_sh_t g_ish[IP_SH_MAX];
extern int ip_rec;
extern int ip_hist[IP_HIST], ip_nhist;
extern int ip_init_done, ip_init_fail;
extern ID3D11VertexShader *ip_vs2;
extern ID3D11InputLayout *ip_lay2;
extern ID3D11Buffer *ip_cb0, *ip_cb1;
extern ID3D11PixelShader *ip_dbg_ps;
extern ID3D11PixelShader *ip_id_ps;
extern unsigned ip_dd_lo, ip_dd_ser;
extern ID3D11Buffer *ip_dbg_cb;
extern LARGE_INTEGER ip_qpf;
extern int ip_K;
extern int ip_live;
extern unsigned st_vbg_copy, st_vbg_fail;
extern double st_vbg_mb;
extern double gc_ms, gc_mx;
extern unsigned gc_n;
extern unsigned st_vbg_async;
extern ID3D11GeometryShader *ip_gd_im, *ip_gd_sh;
extern ID3D11Query *ip_gd_q[IP_GD_Q];
extern volatile int ip_gd_qbusy[IP_GD_Q];
extern volatile unsigned ip_gd_qi;
extern unsigned long long st_gd_tris, st_gd_hit;
extern unsigned st_gd_frames, st_gd_fhit, st_gd_max, st_gd_lost;
/* render_interp_present.c */
extern LONGLONG ip_tnext;
extern LONGLONG ip_dt;
extern LONGLONG ip_lat;
extern unsigned ip_nneed;
extern double ip_rbar;
extern LONGLONG ip_tlast_present;
extern HANDLE pd_wake;
extern unsigned ip_shot_seq, ip_shot_left, ip_shot_next, ip_pairs_total;
extern unsigned ip_shot_ser;
extern unsigned st_ticks, st_pairs, st_presents, st_lerp, st_static, st_nomatch, st_rawchg, st_reject, st_2d, st_cut, st_skip, st_hold, st_late, st_nopair, st_ovf, st_verify, st_verify_bad, st_exact, st_rawlerp, st_rt2d, st_geo;
extern unsigned st_imulti, st_iwrong, st_iamb, st_ipass;
extern unsigned st_ipass_false;
extern double st_rec_ms, st_rep_ms, st_rep_max, st_pres_ms, st_r_ms, st_r_max, st_lat_ms;
extern unsigned st_r_n, st_rep_n, st_lat_n;
extern LONGLONG st_cap_q, st_t0;
extern double st_gpu_ms, st_gpu_max;
extern unsigned st_gpu_n;
extern int ip_in_rec;
extern int ip_rt_ok, ip_rt_tried;
extern unsigned st_rt_hit, st_rt_miss, st_rt_wait, st_rt_stale, st_rt_n, st_rt_fail, st_rt_exec;
extern double st_rt_ms, st_rt_max, st_rt_wait_ms, st_rt_exec_ms;
extern ip_list_t *ip_rt_nA, *ip_rt_nB;
extern float ip_rt_nt;
extern double ip_r_last_ms;
extern unsigned pc_kept, pc_latchg, pc_ph[12], pc_skip0, pc_lockw1, pc_ts_s, pc_ts_snap, pc_ts_big;
/* render_interp_match.c */
extern unsigned st_w448_eye;
extern unsigned st_rr[4];
extern unsigned st_zc_ticks, st_zc_tgt, st_zc_draws, st_zc_rel, st_zc_nocand, st_zc_cam, st_zc_ln;
extern unsigned st_sk_rl, st_sk_rlcam, st_sk_rlofs, st_sk_nod, st_sk_ln;
extern unsigned st_w410_rzb, st_w410_big, st_w410_cam, st_w410_mov, st_w410_rzm;
extern unsigned st_bb_ok, st_bb_no, st_bb_far, st_bb_build;
extern double st_bb_ms;
extern unsigned st_rz_nm, st_rz_nmno, st_rz_nmcam, st_rz_n;
extern double st_rz_ms, st_rz_max;
extern unsigned st_rz_relw, st_rz_rel, st_rz_noofs, st_rz_cam, st_rz_nocam, st_rz_other, st_rz_ln;
extern const char *const kPR_CLS[PR_NC];
extern unsigned st_pz[PR_NC][PR_NO];
extern unsigned st_pr_ticks, st_pr_set, st_pr_pair, st_pr_raw, st_pr_cam, st_pr_keep, st_pr_follow, st_pr_chg, st_pr_still, st_pr_ln, st_pr_lnd;
extern double st_pr_ms, st_pr_max, st_pr_ms1, st_pr_ms2, st_pr_ms3;
extern unsigned st_w415_rzm, st_w415_nm, st_w415_nocam, st_w415_cam, st_w415_rej;
extern unsigned li_rj_w, li_rj_j[4], li_rj_big, li_rj_n, li_rj_cam;
extern unsigned li_rz_alt, li_rz_vl, li_rj_cam2;
extern unsigned st_p2c[5];
extern unsigned st_p2e[6];
extern unsigned st_p2ok[4];
extern long long isw_cp_q, isw_un_q, isw_gr_q;
extern unsigned isw_cp_n, isw_gr_n;
extern volatile LONG st_sh_stale, st_sh_stale_tot;
/* render_interp_shadow.c */
extern const unsigned char *ip_sh_noskip;
extern ip_ring_t ip_ring[2];
extern ip_rc_t ip_rc_ctx[2];
extern ID3D11RenderTargetView *ip_gs_rtv[IP_GS_MAX];
extern unsigned ip_gs_draws[IP_GS_MAX], ip_gs_rq[IP_GS_MAX];
extern int ip_gs_n;
/* render_interp_replay.c */
extern sb_vr_pass_t *ip_vrp;
extern int ip_pl_on;
extern int ip_pl_noshade;
extern unsigned ip_pl_mfx;
extern ID3D11Buffer *ip_pl_cbsrc;
extern ID3D11Buffer *ip_pl_cb;
extern ID3D11Buffer *ip_pl_vb;
extern float (*ip_pl_mvp)[16];
extern unsigned ip_pl_nhook, ip_pl_nmfx, ip_pl_nnoshade;
extern int ip_pl_tex;
extern sb_ph_rep_t ip_ph;
extern unsigned ip_ph_ncam, ip_ph_nred, ip_ph_nclr;
extern unsigned ip_ph_nhud;
extern unsigned ip_ph_nself;
extern unsigned ip_ph_nextra;
extern const ip_list_t *ip_ph_self_L;
extern unsigned ip_ph_ncmfx, ip_ph_ncovl;
extern ID3D11ShaderResourceView *ip_ph_shsrv[8];
extern volatile LONG li_la_rep, li_la_on;
extern volatile LONG li_la_lists, li_la_draws, li_la_rets;
extern volatile LONG li_vpf_n[4];
extern volatile LONG lr_rep, lr_ret, lr_lost, lr_exact, lr_nola, lr_jumps, lr_jumps_all;
extern volatile LONG lr_retx;
extern volatile LONG lr_rjmv;
extern volatile LONG lrm_st_lists, lrm_st_form, lrm_st_pair, lrm_st_seed, lrm_st_fremd, lrm_st_mem;
extern double lr_err_max;
extern volatile LONG li_hm_rep, li_hm_on;
extern volatile LONG li_hm_lists, li_hm_draws, li_hm_cmp, li_hm_near;
extern double li_hm_res_max, li_hm_res_sum;
extern unsigned li_hm_res_n;
extern volatile LONG li_hm_rad;
extern volatile LONG li_hm_cand, li_hm_rej_mv, li_hm_rej_st;
extern double li_rad_k;
extern int li_rad_n;
extern double li_cmp_k;
extern double li_hm_px_max, li_hm_px_sum;
extern unsigned li_hm_px_n;
/* render_interp_diag.c */
extern int ip_rp_we[IP_LISTS], ip_rp_wd[IP_LISTS];
extern unsigned ip_rp_last[IP_LISTS], ip_rp_ser[IP_LISTS];
extern unsigned st_rp_skip[4];
extern ID3D11RenderTargetView *ip_rp_idrtv;
extern int ip_rp_prep;
extern unsigned ip_rp_stop;
extern volatile LONG st_rc_bad, st_rc_fix, st_rc_dead;
extern ID3D11View *volatile ip_dead_v[IP_DEAD_MAX];
extern volatile LONG ip_dead_n;
extern unsigned li_mid, li_exact, li_same, li_ticks, li_dev2, li_kept;
extern double li_dev_max;
extern unsigned li_last_ser;
extern float li_last_t;
/* render_interp_loc.c */
extern long long isw_q[ISW_N], isw_last;

/* ---------- Modulübergreifende Funktionen (Definition im genannten Modul) ---------- */
/* render_interp.c */
int ip_stage(int k, ID3D11Resource *src);
int ip_gpu_begin(void);
void ip_gpu_end(void);
int ip_list_index(const ip_list_t *L);
/* render_interp_rec.c */
void ip_w_upload(ip_list_t *L);
void ip_hw_bind(ID3D11DeviceContext *c, const ip_list_t *B, const ip_list_t *A, unsigned bstart, unsigned astart);
int ip_dd_on(void);
int ip_ddf_has(unsigned rel);
void ip_list_unref(ip_list_t *L);
void ip_list_reset(ip_list_t *L);
void ip_bset_rotate(ip_list_t *L);
int ip_vb_ensure(ip_list_t *L);
int ip_rq_ensure(ip_list_t *L);
void ip_gpu_copy(ip_list_t *L);
ID3D11Buffer *ip_vbsel(const ip_list_t *L, int g);
void ip_gd_bind(ID3D11DeviceContext *c, ID3D11GeometryShader **cur, ID3D11GeometryShader *want);
void ip_gd_poll(void);
int ip_init(void);
int sb_ip_hlerp(void);
/* render_interp_present.c */
int ip_pace_on(void);
double ip_ms(LONGLONG a, LONGLONG b);
LONGLONG ip_now(void);
int ip_tick_hz(void);
LONGLONG ip_vb_get(void);
const ip_cx_t *ip_cx_imm(void);
int ip_rt_active(void);
void ip_rt_quiesce(void);
void ip_rt_drop(int li);
ID3D11CommandList *ip_rt_get(ip_list_t *A, ip_list_t *B, float t);
void ip_rt_prefetch(void);
void ip_rt_exec(ID3D11CommandList *cl);
void ip_show(ip_list_t *A, ip_list_t *B, float t, const char *tag);
void ip_pair_at(LONGLONG due, ip_list_t **pA, ip_list_t **pB, float *pt, int *phold);
void ip_poll(void);
int ip_mission(void);
/* render_interp_match.c */
void ip_clip(const float p[4], const float m[16], float c[4]);
int zc_dmat(double D[16], const float MA[16], const float MB[16]);
double zc_derr(const double X[16], const double Y[16]);
int ip_mvp_affine(const float m[16]);
void rz_cam_pre(const ip_list_t *A, const ip_list_t *B);
int rz_cam(const ip_list_t *A, const ip_list_t *B, const ip_op_t *o, double D[8][16]);
int w410_cam_inv(const ip_list_t *A, const ip_list_t *B, const ip_op_t *o, double Di[16]);
int w415_world_cam(const ip_list_t *A, const ip_list_t *B, const ip_op_t *o, double Di[16], int *ia);
const ip_op_t *w415_cam_ref(const ip_list_t *B, const ip_op_t *o);
const char *pr_why(unsigned w);
int ip_match(ip_list_t *A, ip_list_t *B, int log);
void ip_dump(ip_list_t *B);
/* render_interp_shadow.c */
void ip_sh_free(ip_sh_t *e);
void ip_snap_classify(const ip_list_t *L);
void ip_list_snap(ip_list_t *L, int li);
void ip_sh_start(ip_list_t *B, int bi, ip_list_t *A, int ai, const ip_cx_t *x);
void ip_state_save(ip_state_t *s);
void ip_state_restore(ip_state_t *s);
void ip_cb_write(const ip_cx_t *x, ID3D11Buffer *cb, const float m[16], unsigned u, float f, int isf);
int ip_ring_fill(const ip_cx_t *x, const ip_list_t *B, const ip_list_t *A, int lerp_ok, float t, ip_rc_t *rc,
                        int laOn, float laM[LA_NM][16]);
void ip_ring_bind(const ip_cx_t *x, unsigned slot, int which, unsigned cur[3], ip_rc_t *rc);
unsigned ip_var_pick(const ip_list_t *B, float t);
void ip_replay(ip_list_t *B, int bi, ip_list_t *A, int ai, float t);
void ip_gs_mark(ID3D11RenderTargetView *rtv);
int ip_gs_begin(float t);
void ip_gs_end(unsigned nops);
/* render_interp_replay.c */
void ip_m4mul(float r[16], const float a[16], const float b[16]);
float ip_ph_near_ref(const ip_op_t *o);
void ip_dead_purge(void);
void ip_la_mul(float out[16], const float m[16], const float *C);
void ip_replay_ex(ip_list_t *B, int bi, ip_list_t *A, int ai, float t, int dbg, const ip_cx_t *x);
/* render_interp_diag.c */
void ip_rh_take(const ip_list_t *L, unsigned out[6]);
void ip_rh_check(const ip_list_t *L, const char *who, unsigned fr, int k);
void ip_rh_inputs(const ip_list_t *B, int bi, const ip_list_t *A, int ai, float t, const ip_cx_t *x,
                         int vbgA, int vbgB, int ring);
int ip_list_refs_check(const ip_list_t *L, int li, const char *wo);
int ip_sh_rc_watch(int fix, const ip_list_t *A, const ip_list_t *B, const char *wo);
void ip_diag_tick(ip_list_t *A, int ai, ip_list_t *B, int bi);
void ip_seam_diag(ip_list_t *A, int ai, ip_list_t *B, int bi);
int gd_begin(void);
void gd_end(int k, const ip_rc_t *rc, unsigned ser, float t);
void ip_id_dump(ip_list_t *A, int ai, ip_list_t *B, int bi, float t);
void li_note(const ip_list_t *A, const ip_list_t *B, float t);
void li_sh_dump(const ip_list_t *B, float t);
void ip_stat_log(void);
void ip_hist_clear(void);
/* render_interp_photo.c */
void ip_shots_tick(ip_list_t *B);
int ip_m4inv(float out[16], const float m[16]);
void ip_rp_tick(ip_list_t *A, int ai, ip_list_t *B, int bi);
/* render_interp_loc.c */
ID3D11RenderTargetView *la_world_rtv(const ip_list_t *L, unsigned *split);
void li_aim_diag(const ip_list_t *L);
void li_hud_plain(ip_list_t *L);
void li_wlag_after(const ip_list_t *L);
void li_stat_after(const ip_list_t *A, const ip_list_t *B);
void li_smooth_apply(ip_list_t *L);
void li_aim_attach(ip_list_t *L);
void li_aim_reticle(ip_list_t *L, const ip_list_t *A);
void isw_end(void);
void isw_log(double secs);
void li_hud_census(const ip_list_t *L);
void li_hud_mark(ip_list_t *L, const ip_list_t *A);

#endif /* RENDER_INTERP_INT_H */
