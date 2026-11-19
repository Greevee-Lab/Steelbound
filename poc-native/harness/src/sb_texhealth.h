#pragma once
/* sb_texhealth.h - W315 TEX-WHITE-AMD: Textur-Gesundheit (nur Zaehler) und
 * tex_missing_mark (DIAG). Beschreibung in sb_texhealth.c. */
#include <stddef.h>

struct ID3D11ShaderResourceView;
struct ID3D11Device;

/* sb_tex.c: D3D hat Anlegen abgelehnt. what: 0 Textur, 1 SRV, 2 lineare Textur, 3 lineare SRV. */
void sb_th_create_fail(int what, long hr, unsigned dxgi, unsigned w, unsigned h, unsigned xfmt, unsigned xptr);
/* sb_tex.c: Upload an der Drossel gescheitert (present = Bildnummer). */
void sb_th_throttle(unsigned present);
/* sb_tex.c: neuer Cache-Eintrag, Kopf oberhalb 2 GB (Contig-HIGH-Block, W315 host_hi_addr). */
void sb_th_hi_entry(void);
/* sb_tex.c: Eintrag ist dauerhaft ohne SRV (TEX dauer-ohne-srv). */
void sb_th_dauer(void);
/* render_im.c/render_shade.c: Draw erwartet eine Textur (Stufe belegt), hat aber keine SRV.
 * Zaehlt (je Grund, je Bild) und liefert mit tex_missing_mark=1 die Markierungs-SRV, sonst NULL. */
struct ID3D11ShaderResourceView *sb_th_missing(unsigned why, unsigned present, unsigned xptr);
/* W360 render_shade.c: Stufe 1..3 belegt (Textur-Modus aktiv), aber keine SRV. Zaehlt immer, merkt die
 * Textur fuer F11 und liefert mit tex_missing_mark=1 die Markierungs-SRV, sonst NULL (bitgleich). */
struct ID3D11ShaderResourceView *sb_th_stage_missing(unsigned stage, unsigned why, unsigned xptr, unsigned present);
/* Markierungs-SRV je Grund, ohne Zaehlung. */
struct ID3D11ShaderResourceView *sb_th_mark(unsigned why);
/* render_d3ddbg.c: Geraetefaehigkeiten nach dem Anlegen (einmal). */
void sb_th_device(struct ID3D11Device *dev, int driver_cmdlists, int warp);
/* Je Spiel-Present (d3d11_render.c): VRAM-Abfrage alle 1800 Presents (sonst ein Vergleich). */
void sb_th_present(unsigned present);
/* Block "texture health" als Text (CRLF-Zeilen, ASCII, ohne Pfade/Eingaben). live=1: VRAM jetzt
 * abfragen (nicht im Absturz-/Haenger-Pfad), 0 = letzter Stand. */
void sb_th_format(char *o, size_t cap, int live);
/* Block ins Log (TEXHEALTH-Zeilen), z. B. beim Beenden oder F11. */
void sb_th_log(const char *why);
