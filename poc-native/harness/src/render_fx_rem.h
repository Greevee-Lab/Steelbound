/* render_fx_rem.h - W133 REM-FX: Effekt-Remaster-Optionen (Standard aus = Original).
 * Schnittstelle fuer render_im.c (Effekt-Draws), render_interp.c (120-FPS-
 * Wiedergabe), d3d11_render.c (Nachbearbeitung/Present) und sb_overlay.c
 * (ESC-Seite "Effekte"). Beschreibung s. render_fx_rem.c. */
#ifndef RENDER_FX_REM_H
#define RENDER_FX_REM_H

/* has_tex-Bits der IM-Draws (render_im.c / Interp-Aufzeichnung). Bit 0 und
 * 8..24 sind belegt (Textur, Alpha-Test, ALPHAKILL). */
#define FXR_BIT_SOFT  0x02000000u /* weicher Partikel: Tiefen-Kopie t4, Parameter b2 */
#define FXR_BIT_RGB   0x04000000u /* mit SOFT: Faktor auch auf RGB (additive Quelle ONE) */
#define FXR_BIT_SPARK 0x08000000u /* prozeduraler Glutpunkt (uv = lokale Quad-Koordinate) */
#define FXR_BIT_LIGHT 0x10000000u /* Licht-Quad (Radialabfall, Blend DEST_COLOR/ONE) */
#define FXR_MASK (FXR_BIT_SOFT | FXR_BIT_SPARK | FXR_BIT_LIGHT)

extern int g_fxr_bypass;      /* Overlay "Vergleich (B)" / A/B: 1 = alle Effekt-Optionen voruebergehend aus */
extern unsigned g_fxr_reck;   /* naechster rec_draw: 1 = Effekt-Draw des Spiels, 2 = eigener Zusatz-Draw */
extern float g_fxr_recp[8];   /* naechster rec_draw mit FXR_MASK: Parameter (b2) */

int fxr_active(void);         /* irgendeine Option an (billig, je Draw) */
int fxr_ckp_active(void);     /* Explosions-Licht Cockpit an (Nachbearbeitung) */

/* render_im.c: vor der VSH-Transformation (Rohpositionen pos[i*pst]) -
 * klassifiziert den Draw; Rueckgabe 1 = Effekt-Draw in einer Welt-RT. */
int fxr_pre(int vsh_em, const float m[4][4], const float zadd[4], const float *pos, size_t pst, unsigned n, int rhw,
            unsigned prim);
/* has_tex-Bits fuer den laufenden Draw (0 = keine) */
unsigned fxr_draw_bits(void);
/* direkt vor dem Draw (Zustand gesetzt): Tiefen-Kopie, PS/b2/t4 binden */
void fxr_bind(void);
/* nach dem Draw (ov = Vertices vor der Expansion, RT-Pixel): t4 frei,
 * Energie/Glut-Ausloeser, Zusatz-Draws (Glut, Licht) */
void fxr_after(const im_vertex_t *ov, unsigned n);
/* Draw ohne Effekt-Klasse (Tiefe der Welt kann sich geaendert haben) */
void fxr_note_other(void);

/* render_interp.c: Wiedergabe */
typedef struct { void *dsv; int dirty; int bound; int used; } fxr_rep_t;
void fxr_rep_begin(fxr_rep_t *s);
int fxr_rep_op(ID3D11DeviceContext *c, fxr_rep_t *s, unsigned has_tex, const float fxp[8],
               ID3D11DepthStencilView *dsv, int deferred);
void fxr_rep_end(ID3D11DeviceContext *c, fxr_rep_t *s);
void fxr_interp_note(unsigned fxk, unsigned why, int lerp); /* Statistik je Effekt-Draw */

/* d3d11_render.c */
void fxr_present(unsigned n);                                 /* je Present (Spiel-Bild) */
void fxr_post(ID3D11RenderTargetView *rtv, ID3D11Resource *res); /* Cockpit-Licht auf das gezeigte Bild */
void fxr_shot_tick(unsigned n);                               /* DIAG fxr_ab: Bildpaare */

/* sb_overlay.c: Presets */
int fxr_preset_match(void);   /* 0 Original, 1 dezent, 2 stark, -1 benutzerdefiniert */
void fxr_preset_apply(int q);
const char *fxr_preset_key(int q); /* W142: Sprachschluessel (ovl.v_*), Anzeige via sb_lang */

#endif
