/* vr.h - W134 VR-PROTO: Stereo-Cockpit aus der Interp-Aufzeichnung.
 *
 * [vr] vr_enable=0 (Standard): kein Code dieses Moduls laeuft ausser
 * sb_vr_init() (kehrt sofort zurueck) -> bitgleich zum bisherigen Pfad.
 * Architektur: BERICHT_M2 "W134". Kurz:
 *  - Je VR-Bild wird ein Tick-Paar (A, B, t) der Interp-Aufzeichnung
 *    (render_interp.c) wiedergegeben. RT-Paesse (Aussenwelt im Hauptmonitor,
 *    Sub-Monitore) laufen unveraendert in ihre Schatten-Ziele; jeder Draw in
 *    den Backbuffer (Cockpit-Mesh, Monitor-Komposition, HUD) wird je Auge in
 *    das Augenziel gezeichnet, mit einer je Draw gebauten Matrix
 *    Spiel-Clip -> Sitz-Sicht (RH) -> Augen-Clip.
 *  - Augen-Clip = Sicht der Augenpose (Kopfpose x Recentering x Sitzhoehe x
 *    Massstab, IPD) x Projektion aus dem Augen-Sichtfeld (OpenXR XrFovf bzw.
 *    Vorschau symmetrisch). */
#ifndef VR_H
#define VR_H
#ifdef __cplusplus
extern "C" {
#endif

struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;

/* Augen-Durchgang fuer die Interp-Wiedergabe (render_interp.c). */
typedef struct {
    int neyes;                 /* 1 oder 2 */
    float E[2][16];            /* Sitz-Sicht (RH, Spiel-Einheiten) -> Augen-Clip; row-major, Zeilenvektor */
    float ia, ib;              /* 1/a' (Spiel-Projektion x inkl. Hor+), 1/b */
    float d2d;                 /* Abstand der 2D-Ebene (HUD/Komposition) in Spiel-Einheiten */
    struct ID3D11RenderTargetView *rtv; /* Augenziel (beide Augen nebeneinander oder eins) */
    struct ID3D11DepthStencilView *dsv;
    float vp[2][4];            /* Viewport x, y, w, h je Auge (Pixel im Augenziel) */
    /* Ausgabe (Diagnose): Draws je Klasse dieser Wiedergabe */
    unsigned n3d, n2d, np2, nrq, nrqfb, nclr;
    float p2w_sum, w3_min;      /* Summe der Clip-w der P2-Bezugsvertices, kleinstes w der 3D-Draws */
    unsigned p2w_n;
    float rz0, rl;              /* P2-Relief: d = d2d + rl * (z/w - rz0) (Spiel-Einheiten) */
    float p2z_min, p2z_max, p2z_sum; /* Ausgabe: z/w der P2-Bezugsvertices */
    float rmax;                 /* Relief-Grenze: d in [d2d - rmax, d2d + rmax] */
    int wantz;                  /* 1 = z/w-Stichproben sammeln (erste Wiedergabe) */
    unsigned zs_n;
    float zs[1024];
    int dump;                  /* DIAG: >0 = die ersten BB-Draws dieser Wiedergabe protokollieren */
    int wfix;                  /* W164 vr_world_depth: 1 = 3D-Pfad (Cockpit-Mesh) mit affiner Augen-Tiefe statt Original-z/w x w_Auge */
    int ifix;                  /* W164 vr_inst_depth: 1 = P2-Draws mit perspektivischem z/w (> 0,2: Instrumente, Anzeigen) auf ihre metrische Tiefe */
    unsigned ws_n;              /* W164: Clip-w-Stichproben der 3D-Pfad-Draws (erste Wiedergabe, wantz) */
    float ws[512];
    /* W183 VR-FIX3 */
    int wtick;                  /* Eingang vr_world_tick: 1 = RT-Paesse (Aussenwelt, Monitore) nicht je VR-Bild neu, sondern
                                 * Tick-Endstand bak[B] (Spieltakt); nur BB-Draws (Cockpit, HUD) je Auge mit der aktuellen Pose */
    int noshade;                /* Eingang vr_shading=0: Shading-Draws des 3D-Pfads im Augenbild ueber den IM-PS (billiger) */
    int wtick_used;             /* Ausgabe: 1 = Welt aus dem Tick-Endstand (Scan sicher), 0 = volle Wiedergabe */
    unsigned nskip;             /* Ausgabe: uebersprungene RT-Ops (wtick_used) */
    int hold;                   /* Ausgabe: 1 = juengster Tick gehalten (naechster fehlte zum Zeitpunkt) */
    unsigned bser, aser;        /* Ausgabe: Seriennummern des wiedergegebenen Paars (0 = keins) */
} sb_vr_pass_t;

void sb_vr_init(void);          /* nach Device + Fenster (d3d11_render.c); vr_enable=0 -> sofort zurueck */
int sb_vr_preview_active(void); /* 1 = Vorschau ersetzt das Interp-Bild im Fenster (sbs/left/right) */
/* render_interp.c ip_show: Vorschau-Bild fuer Paar (A,B,t) zeigen. Ruft
 * sb_interp_vr_replay. 1 = gezeigt (Aufrufer praesentiert nicht mehr). */
int sb_vr_preview_show(void *A, void *B, float t);
/* Fensternachricht (d3d11_render.c sb_wndproc): Recentering-Taste, Kopf-
 * Simulation (Ziffernblock 4/6/8/2, 5). 1 = verbraucht. */
int sb_vr_wndproc(unsigned msg, unsigned wp, long lp);
void sb_vr_recenter(const char *why); /* ESC-Menue / Taste */
/* ESC-Menue: Werte lesen/aendern (Sitzhoehe mm, Massstab %, IPD 1/10 mm). */
const char *sb_vr_state_text(void);
/* W145 DIAG(LOADLOCK): Tid des OpenXR-Threads (0 = keiner) fuer den Halter-Stack. */
unsigned long sb_vr_thread_tid(void);
/* W145: Watchdog-Takt (5 s): OpenXR-Aufruf haengt > 3 s -> eine Logzeile. */
void sb_vr_watch(void);
/* W164 VR-FIX2: sb_render_blit (unter ctx_lock, vr_enable=1, nach Kopie/Nachbearbeitung/Overlays
 * des Monitorbilds): Leinwand-Quelle (ausserhalb der Mission) sichern; im OpenXR-Modus mit
 * vr_preview sbs/left/right das Fenster durch das eingereichte Augenbild ersetzen (1 = ersetzt). */
int sb_vr_after_blit(void *swap_res, void *swap_rtv);
/* W183 VR-FIX3: 1 = im OpenXR-Modus zeigt das Fenster in der Mission den VR-Spiegel (bzw. schwarz bei
 * vr_mirror=0) - die Mono-Wiedergabe des Fensters (ip_show) und Kopie/Nachbearbeitung in sb_render_blit
 * entfallen. 0 = Fenster wie bisher. vr_enable=0: immer 0. */
int sb_vr_window_mirrors(void);
/* W183: ESC-Menue VR: Leistungs-Preset (1 Leistung, 2 Ausgewogen, 3 Qualitaet) auf die Einzelwerte anwenden;
 * sb_vr_preset_match: welches Preset passt zu den aktuellen Werten (0 = eigene). */
void sb_vr_preset_apply(int preset);
int sb_vr_preset_match(void);

/* W197 VR-MENU: 1 = ESC-Menue/F2 erscheinen als eigene Flaechen in der Brille (OpenXR-Session + vr_menu=1). */
int sb_vr_panels_on(void);
/* W197: sb_render_blit (unter ctx_lock) vor ESC-Menue/F2: Leinwand-Quelle ohne die eigenen Overlays sichern. */
void sb_vr_canvas_source(void *swap_res);
/* W197: Zeile "VR" der F2-Flaeche (VR-Bildrate, Augengroesse). */
const char *sb_vr_hud_text(void);

/* render_interp.c: Augen-Wiedergabe (unter ctx_lock). A/B = ip_list_t*
 * (NULL -> juengstes Paar, Zeitpunkt due_qpc, 0 = jetzt). Rueckgabe 1 =
 * gezeichnet, 0 = keine Aufzeichnung (ausserhalb der Mission). */
int sb_interp_vr_replay(sb_vr_pass_t *p, void *A, void *B, float t, long long due_qpc, float *t_out);
int sb_interp_vr_available(void);
void sb_interp_vr_mono_shot(void *B, const char *name); /* DIAG: exaktes Mono-Bild von B */

#ifdef __cplusplus
}
#endif
#endif /* VR_H */
