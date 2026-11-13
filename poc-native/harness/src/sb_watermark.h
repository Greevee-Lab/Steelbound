#pragma once
/* sb_watermark.h - W219 WATERMARK: temporaeres Alpha-Wasserzeichen.
 *
 * Kleiner halbtransparenter Text unten rechts in der 4:3-Spielbox
 * (sb_aspect_box: liegt bei jedem Seitenverhaeltnis im Bild, nie in den
 * schwarzen Balken), Schrift wie F2 (Consolas, GDI -> vormultiplizierte
 * Textur, 1:1 per sb_ovl_quad). Text: "Steelbound <Version> . ALPHA" aus dem
 * Build-Define SB_VERSION_STR (CMake -DSB_VERSION=..., gesetzt von
 * make_alpha.py/release.sh); ohne Version "Steelbound dev".
 *
 * Schalter [overlay] watermark: 0 = aus (Code-Standard, Test-cfg
 * sb_native.cfg -> Golden bitgleich), 1 = an (Spieler-cfg sb_player.cfg =
 * Community-Edition). Entfernen: watermark=0 in sb_player.cfg.
 *
 * Sichtbar auf jedem gezeigten Bild, in Feedback-Clips (vor dem Clip-Abgriff
 * gezeichnet) und Feedback-Screenshots (CPU-Mischung in sb_render_pause_jpg).
 * Nicht im Fotomodus (Bildschirm und Aufnahmen) und nicht in VR (weder
 * Brille noch Desktop-Spiegel). Ohne FPS-INTERP wird der Spiel-BB-Bereich
 * gesichert und nach dem Present zurueckgelegt (Pausen-Standbild bleibt
 * sauber). Alles unter ctx_lock. */
#ifdef __cplusplus
extern "C" {
#endif

struct ID3D11RenderTargetView;

/* 1 = Wasserzeichen jetzt zeichnen (Schalter an, kein VR, kein Fotomodus) */
int sb_wm_on(void);
/* auf rtv zeichnen; save=1: Bereich sichern (Spiel-BB ohne FPS-INTERP) */
void sb_wm_draw(struct ID3D11RenderTargetView *rtv, int save);
/* gesicherten Bereich zurueck (nach dem Present) */
void sb_wm_restore(void);
/* in ein BGR-Bild w x h (Zeilen top-down, 3 Byte/Pixel) mischen, gleiche Lage
 * wie am Bildschirm (Feedback-Screenshot). 1 = gemischt. */
int sb_wm_blend_bgr(unsigned char *bgr, unsigned w, unsigned h);

const char *sb_version_str(void); /* Release-Version oder "" (Entwickler-Build) */

#ifdef __cplusplus
}
#endif
