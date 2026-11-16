#pragma once
/* loc_render.h - W285 LOC-RENDER: Renderer-Anpassungen nur fuer das LoC-Profil
 * (Line of Contact, [expert] loc_boot=1). Steel Battalion laeuft nie hier durch. */

#ifdef __cplusplus
extern "C" {
#endif

extern unsigned g_loc_draws; /* Draws im laufenden Bild (nur LoC-Profil gezaehlt) */

/* Je Present (aus m3d_present_tick): Bildzaehler weiterschalten. */
void loc_render_frame(unsigned f);
/* DIAG: 1 = Zensus in diesem Bild ausloesen (loc_census_from + loc_census_draws). */
int loc_render_census_go(unsigned f);

/* W285: Backbuffer als Textur (GetBackBuffer2-Surface als Textur gesetzt, nur LoC, loc_bbtex=1).
 * 1 = Treffer; srv = Kopie der 4:3-Box (kann NULL sein), w/h = Texel-Groesse des Xbox-Puffers. */
struct ID3D11ShaderResourceView;
int loc_render_is_bbtex(unsigned xptr);
/* DIAG(loc_psdump): PS-Def-Woerter + HLSL (src NULL = nicht unterstuetzt) in eine Datei. */
void loc_render_dump_ps(unsigned handle, const unsigned *d, unsigned nd, const char *src);
int loc_render_bb_lookup(unsigned xptr, struct ID3D11ShaderResourceView **srv, unsigned *w, unsigned *h);
/* W297: Textebene (XFONT) - Lock merkt Daten/CPU-Zeiger, Clear auf dieselbe Datenadresse loescht den
 * CPU-Speicher mit (Xbox: gemeinsamer Speicher). surf = gebundenes Ziel (0 = Backbuffer). */
void loc_render_text_lock(unsigned surf, unsigned bits, unsigned pitch);
void loc_render_on_clear(unsigned surf, unsigned flags, unsigned color);
/* W311: 1 = surf ist die LoC-Sichtkarte (loc_vismap); host/pitch = Lesezeiger des Spiels (Dauer-Lock, 0 = keiner). */
int loc_render_vismap(unsigned surf, unsigned *host, unsigned *pitch);

#ifdef __cplusplus
}
#endif
