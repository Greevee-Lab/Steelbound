#pragma once
/* render_vexp.h - W255 VIDEO-EXPORT (experimentell): ein Replay offline (nicht
 * in Echtzeit) in Zielaufloesung rendern und als MP4 (H.264, Ton AAC) speichern.
 *
 * [video] video_export=<datei.mp4|auto> + replay_autoplay=<ID> (W254):
 *  - Fenster unsichtbar (window_hidden=1), Backbuffer = video_width x video_height,
 *    Testtakt (vblank_hz=0, Spiel so schnell wie gerendert), 60 FPS mit
 *    Zwischenbildern (interp_fps=60, K=2 je Tick), Live-Ton aus.
 *  - Ab der Replay-Wiedergabe (REPLAY-AUTO Phase PLAY) laeuft ein virtueller
 *    VBlank-Takt: jeder Feld-Poll ohne Fortschritt seit dem letzten Swap bzw.
 *    jedes BlockUntilVerticalBlank = 1 VBlank (wie eine Xbox, die nie zu spaet
 *    ist). Jedes gezeigte Bild bekommt so eine Zeit (VBlanks); Ausgabebild n
 *    (n/fps s) zeigt das letzte Bild davor (vexp_enc.h: Zeitachse).
 *  - Der Mischer laeuft im selben virtuellen Takt (44100 Hz je VBlank-Zeit) und
 *    gibt seine Bloecke an den Encoder; der Spiel-Thread wartet am Swap, bis der
 *    Mischer aufgeholt hat (Bild und Ton bleiben auf +-1 Mischblock synchron).
 *  - Ende der Wiedergabe: Datei abschliessen (.part -> .mp4), Client beendet
 *    sich (Exit 0 = ok, 2 = Replay-Start gescheitert, 3 = Encoder/Datei).
 * Ohne video_export: alle Funktionen kehren sofort zurueck (Standard bitgleich). */
struct ID3D11RenderTargetView;

void sb_vexp_init(void);  /* emu_main: nach cfg/--set und sb_rauto_init, vor sb_render_init */
int sb_vexp_on(void);     /* 1 = Export angefordert */
int sb_vexp_rec(void);    /* 1 = Aufnahme laeuft (Bilder abgreifen) */
void sb_vexp_frame(struct ID3D11RenderTargetView *rtv); /* unter ctx_lock, je gezeigtem Bild */
void sb_vexp_swap(void);  /* Spiel-Thread je Swap (hooks_d3d, nach sb_rauto_tick) */

/* virtueller VBlank-Takt (hooks_timing, nur im Testtakt) */
int sb_vexp_clock(void);
unsigned sb_vexp_field_poll(void);
void sb_vexp_vblank(void);

/* Ton (audio_mix, Mischer-Thread) */
int sb_vexp_audio(void);  /* 1 = Mischer im virtuellen Takt */
void sb_vexp_audio_push(const short *pcm, unsigned frames);
void sb_vexp_audio_pace(void);
