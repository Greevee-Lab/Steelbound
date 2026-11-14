#pragma once
/* d3d11_render.h - Echter Present-Pfad (M2a) + Renderer-Kern.
 *
 * Architektur (fuer Ultrawide/4K und spaetere VR-Mods):
 * - Kamera ist strikt getrennt: View (Wo steht sie) + Projektion (Wie wird
 *   abgebildet). Das Spiel liefert beide via SetTransform; der Renderer
 *   speichert sie getrennt und kombiniert erst beim Zeichnen.
 * - Aufloesungsunabhaengig: Spiel denkt in 640x480 (Viewport + Matrizen),
 *   Renderer rechnet auf echte Backbuffer-Groesse um (Hor+-Modus: vertikales
 *   Sichtfeld bleibt, horizontal kommt bei 21:9 dazu statt zu stretchen).
 * - VR-offen: Stereo-Rig mit 2 Augen (je View/Proj/Viewport). Heute: mono
 *   (Auge 0 = Vollbild). Eine spaetere VR-Mod setzt stereo=1, schreibt pro
 *   Auge View/Proj und submittet beide Bilder — kein VR-SDK heute verlinkt.
 */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float m[16]; /* row-major, wie D3DMATRIX */
} sb_mat4;

typedef struct {
    unsigned x, y, w, h;
    float minz, maxz;
} sb_viewport;

/* Ein Auge: eigene Kamera (View + Projektion) + eigener Bildausschnitt. */
typedef struct {
    sb_mat4 view;
    sb_mat4 proj;   /* bereits aspect-korrigiert (Hor+) */
    sb_viewport vp; /* in echten Backbuffer-Pixeln */
} sb_eye;

void sb_render_init(void);   /* Fenster + D3D11-Device + Swapchain (einmalig) */
void sb_render_shutdown(void);
int sb_render_is_open(void); /* 0 nach Fenster-Schliessen */

/* Spiel-Meldungen (werden von den D3D-Hooks gerufen): */
void sb_render_on_create_device(unsigned want_w, unsigned want_h);
void sb_render_clear(unsigned flags, unsigned color, float z, unsigned stencil);
void sb_render_set_viewport(const sb_viewport *vp); /* Spiel-Viewport (640x480-Denke) */
void sb_render_blend(unsigned state, unsigned value); /* 19/20/27 */
/* XBLEND: NV2A-Methode (0x304 BLEND_ENABLE, 0x344 SFACTOR, 0x348 DFACTOR)
 * mit GL-Enum-Werten, wie sie D3DDevice_SetRenderState_Simple bekommt. */
void sb_render_blend_nv(unsigned method, unsigned value);
/* DIAG(MOVIE): aktuellen Blend-State lesen (on/src/dst, PC-Codes). */
void sb_render_blend_state(unsigned *on, unsigned *src, unsigned *dst);
void fx_nv_note(unsigned method, unsigned value); /* FX-ORIG (W90): NV-RenderState-Spiegel */
/* MISSION3D (W25, DIAG): Spiel-Z-/Cull-State merken + lesen. */
void sb_render_rs_note(unsigned method, unsigned value);
void sb_render_rs_stencil_enable(unsigned v);
void sb_render_note_zenable(unsigned v);
void sb_render_note_cullmode(unsigned v);
unsigned sb_render_zenable(void);
unsigned sb_render_cullmode(void);
void sb_render_set_view(const float *m16);          /* D3DTS_VIEW (Kamera-Standort) */
void sb_render_set_proj(const float *m16);          /* D3DTS_PROJECTION (Abbildung) */
void sb_render_set_world(int idx, const float *m16);
void sb_render_present(unsigned flags); /* Msg-Pump + Present (FPS-Limit bleibt im Hook) */

/* Immediate-Mode (Xbox Begin/End + SetVertexData): M2a sammelt nur Statistik
 * (prim-Typ, Vertex-Anzahl) fuer den M2b-Geometriepfad. */
void sb_render_im_begin(unsigned prim);
void sb_render_im_vdata(unsigned reg, float a, float b, float c, float d);
void sb_render_im_color(unsigned reg, unsigned color);
void sb_render_im_end(void);
void sb_render_im_shader(unsigned handle); /* FVF/Shader (RHW vs. 3D) */
void sb_render_vb_draw(unsigned prim, const unsigned char *vdata,
                       unsigned stride, const unsigned short *idx,
                       unsigned idxcount, unsigned base_vtx, unsigned start,
                       unsigned count); /* M2c: Stream-Draw */
/* Draw-Drop-Zensus (drawdrop_diag): verworfene Draws zaehlen (why: 0 kein VB-Datenzeiger,
 * 1 VS ohne Decl/pos, 2 n=0, 3 Lesefehler, 4 kein Indexpuffer, 5 SetVertexShader(0));
 * caller = Spiel-Ruecksprung des Hooks. Nur bei [log] drawdrop_diag=1. */
void sb_render_drawdrop(unsigned why, unsigned vs, unsigned n, unsigned caller);
void sb_render_drawcall(unsigned kind, unsigned prim, unsigned n, unsigned caller);
int sb_render_census_on(void);

/* Lesezugriff fuer Diagnose/ spaetere Pfade (M2b-Geometrie, VR-Mod): */
/* RTT (W23): Render-to-Texture. Das Spiel rendert Titel-/Menue-Hintergrund
 * (u.a. lineare 1024x512-Textur) per D3DDevice_SetRenderTarget (0x20E370)
 * in Offscreen-Surfaces und sampelt sie danach als Textur. Jede Surface
 * bekommt eine D3D11-Textur mit RTV+SRV (+DSV); Draws/Clears laufen
 * umgeleitet, Sampling liefert die SRV. Schalter [render] rtt (0 = nur
 * notieren + DIAG, 1 = umleiten). */
struct ID3D11ShaderResourceView;
void sb_rtt_note_backbuffer(unsigned surf);
void sb_rtt_on_set_target(unsigned surf, unsigned depth, unsigned caller);
void sb_rtt_rbc_im(unsigned prim, const float *verts, unsigned n, unsigned slots, int colreg); /* W215 rtt_rb_sync=2 */
void sb_rtt_rb_sync_resolve(int why); /* W215 CULL-POPIN: anhaengigen Zellenkarten-Rueckschrieb abholen */
/* Offscreen-Textur zu xptr? 1 = Treffer (SRV + Dims), 0 = nein.
 * Loggt pro xptr einmal TEX rt-match (auch bei rtt=0, reiner DIAG). */
int sb_rtt_lookup(unsigned xptr, struct ID3D11ShaderResourceView **srv,
                  unsigned *w, unsigned *h);
void sb_rtt_release_surf(unsigned surf); /* Notiz bei Release rc=0 loeschen */
void sb_rtt_release_data(unsigned res); /* W124 rtt_stale: freigegebene Textur -> RT-Eintraege mit gleicher Datenadresse veraltet */
const sb_eye *sb_render_eye(int idx); /* idx 0..1, mono: nur 0 gueltig */
unsigned sb_render_backbuffer_w(void);
/* JPG schreiben (WIC); bgr = 24-bit BGR top-down. 1 = ok. */
int sb_render_write_jpg(const char *path, const unsigned char *bgr,
                        unsigned w, unsigned h);
unsigned sb_render_backbuffer_h(void);

/* FPS-INTERP (W69, render_interp.c): Zwischenbilder in der VBlank-Warte-
 * schleife zeigen (Spiel-Thread). Nur bei [render] interp_fps>0 aktiv. */
int sb_interp_pending(void);          /* 1 = Bilder des laufenden Ticks offen */
void sb_interp_idle(unsigned max_ms); /* faelliges Bild zeigen oder schlafen (<= max_ms) */
void sb_interp_field(unsigned counter); /* Feld-Poll: Wechsel merken (Anker), faelliges Bild zeigen */

#ifdef __cplusplus
}
#endif
