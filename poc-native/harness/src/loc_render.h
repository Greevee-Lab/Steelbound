#pragma once
/* loc_render.h - W285 LOC-RENDER: Renderer-Anpassungen nur fuer das LoC-Profil
 * (Line of Contact, [expert] loc_boot=1). Steel Battalion laeuft nie hier durch. */

#ifdef __cplusplus
extern "C" {
#endif

extern unsigned g_loc_draws; /* Draws im laufenden Bild (nur LoC-Profil gezaehlt) */

/* Je Present (aus m3d_present_tick): Bildzaehler weiterschalten. */
void loc_render_frame(unsigned f);
void loc_render_sblook_apply(void); /* W459: SB look (Experimental), render_sblook.h */
/* DIAG: 1 = Zensus in diesem Bild ausloesen (loc_census_from + loc_census_draws). */
int loc_render_census_go(unsigned f);

/* W285: Backbuffer als Textur (GetBackBuffer2-Surface als Textur gesetzt, nur LoC, loc_bbtex=1).
 * 1 = Treffer; srv = Kopie der 4:3-Box (kann NULL sein), w/h = Texel-Groesse des Xbox-Puffers. */
struct ID3D11ShaderResourceView;
int loc_render_is_bbtex(unsigned xptr);
/* W326 LOC-WIDE: Present-Parameter aus Direct3D_CreateDevice (Seitenverhaeltnis, Supersampling). */
void loc_render_on_present_params(unsigned pp);
/* DIAG(loc_psdump): PS-Def-Woerter + HLSL (src NULL = nicht unterstuetzt) in eine Datei. */
void loc_render_dump_ps(unsigned handle, const unsigned *d, unsigned nd, const char *src);
int loc_render_bb_lookup(unsigned xptr, struct ID3D11ShaderResourceView **srv, unsigned *w, unsigned *h);
/* W297: Textebene (XFONT) - Lock merkt Daten/CPU-Zeiger, Clear auf dieselbe Datenadresse loescht den
 * CPU-Speicher mit (Xbox: gemeinsamer Speicher). surf = gebundenes Ziel (0 = Backbuffer). */
void loc_render_text_lock(unsigned surf, unsigned bits, unsigned pitch);
void loc_render_on_clear(unsigned surf, unsigned flags, unsigned color);
/* W485 LIVEQOS (Live-Dialoge): obere Grenze der 2D-Heuristik fuer IM-Draws (sb_im_submit). Standard 704x544
 * (640x480 + 64). Nur Xbox-Live-Modus (loc_live) und nur fuer Draws, die die Textebene sampeln: Grenze =
 * LoC-Framebuffer + 64 (720x480 -> 784x544). Sonst unveraendert (SB, System Link bitgleich). */
void loc_render_rhw_range(unsigned xptr, float *xmax, float *ymax);
/* W311: 1 = surf ist die LoC-Sichtkarte (loc_vismap); host/pitch = Lesezeiger des Spiels (Dauer-Lock, 0 = keiner). */
int loc_render_vismap(unsigned surf, unsigned *host, unsigned *pitch);
/* W422 LOC-INSIGNIA-PEN: 1 = surf ist ein Stift-RT des Insignien-Editors (Speicher mit der CPU geteilt:
 * vor der Session hochladen, danach synchron zurueckschreiben; loc_insignia_rt, nur LoC-Profil). */
int loc_render_insignia_rt(unsigned surf);
/* W323 LOC-INTERP: 1 = dieser Swap ist ein Missionsbild (Zwischenbilder, nur LoC-Profil + loc_interp). */
int loc_render_interp_mission(void);
/* W323: Missionstakt in Hz (vblank_hz / Felder je Bild, Original 60/3 = 20). */
int loc_render_tick_hz(void);
/* W465 LOC-ENHANCED: Szene fuer die Endbild-Nachbearbeitung (render_shfx.c): 1 = Cockpit-Mission (Lockstep aktiv,
 * eigener VT abgesetzt, nicht Karte), 2 = Replay/Aussenkamera/Demo (Welt im Backbuffer, [0x306BA8] != 0), 0 = sonst
 * (Menues, Lobby, Karte) -> keine Nachbearbeitung. W509: nach dem Missionsende (Ergebnis, Debriefing) 0, obwohl die
 * Lockstep-Maske noch steht (loc_postscene.h). */
int loc_render_post_scene(void);
/* W465: 1 = LoC 4:3 mit vollem Hor+ -> Spiel-Komposition (Welt im BB) auf volle Breite (loc_horp_comp, Standard 1). */
int loc_render_horp_full(void);
/* DIAG(W358 LOC-LOOK) loc_lookstat: Zustandszaehler je Draw (aus render_im.c, nur LoC) */
void loc_render_lookstat(int rhw, unsigned has_tex);
/* DIAG(W349 LOC-FRAMES-2) loc_ui_diag: Farbe der UI-Rahmen (FUN_000ECF10) mit Aufrufkette */
void loc_render_ui_color(unsigned reg, unsigned color, const unsigned *ret);

#ifdef __cplusplus
}
#endif
