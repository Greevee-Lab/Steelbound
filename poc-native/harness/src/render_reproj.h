/* render_reproj.h - W295 WORLD-REPROJ (Kolben, interp_mode=reproj).
 * Schnittstelle zwischen render_interp.c (Aufzeichnung/Wiedergabe) und render_reproj.c (Reprojektion des Welt-RT). */
#ifndef RENDER_REPROJ_H
#define RENDER_REPROJ_H
#include <d3d11.h>

/* Ein Welt-Draw des Ticks B (Op-Index idx) mit den Daten, aus denen seine Bildbewegung N-1 -> N folgt. */
typedef struct {
    unsigned idx;        /* Op-Index in B (ID im Draw-ID-Bild = idx + 1) */
    int lerp;            /* 1 = Partner in A (o->flag und o->match gueltig) */
    int same;            /* 1 = gleiche Eingangsdaten bzw. rzb -> Matrix-Lerp (T = M_B^-1 * M_A exakt) */
    int stat;            /* 1 = unveraendert (gleiche Matrix + Daten) -> bildfest */
    int flat;            /* 1 = 2D ohne Clip-w (Bildschirm-Quad im Welt-RT) */
    int pre;             /* 1 = 3D, aber vorab transformiert (Clip-w = 1): Matrix ohne Aussage -> Kamerabewegung */
    unsigned nv;
    float mvp[16], amvp[16];
    float pref[4][4], apref[4][4];
    float vp[6];         /* D3D11_VIEWPORT des Draws (x, y, w, h, minz, maxz) */
} sb_rp_op_t;

int sb_rp_want(void);  /* interp_mode=reproj und anwendbar (nicht VR) */
/* Am Swap (Spiel-Thread, ctx_lock, Immediate): Tick B der Liste li vorbereiten. sb_rp_begin: col/dep = Welt-RT
 * bzw. Welt-Tiefe am Ende des Welt-Durchgangs (exakte Teil-Wiedergabe t = 1); sb_rp_tick: idsh = Welt-Schatten nach
 * der Draw-ID-Teil-Wiedergabe, dazu die Bewegung je Welt-Draw. 1 = bereit. */
int sb_rp_begin(int li, ID3D11Texture2D *col, ID3D11Texture2D *dep);
int sb_rp_tick(int li, unsigned ser, const sb_rp_op_t *ops, unsigned n, unsigned nops, ID3D11Texture2D *idsh);
int sb_rp_ready(int li, unsigned ser);
/* Zwischenbild t (0..1) des Paars (A, B = Liste li) in das Welt-Ziel dst (Textur dtex) schreiben (beliebiger Context). */
void sb_rp_warp(ID3D11DeviceContext *c, int li, ID3D11RenderTargetView *dst, ID3D11Texture2D *dtex, float t);
void sb_rp_stat_log(void);

#endif
