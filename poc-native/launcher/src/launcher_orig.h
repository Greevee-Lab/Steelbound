#pragma once
/* launcher_orig.h - W278 ORIG-FLAG (Launcher): experimentelle Grafik
 * erkennen, Hinweistext, "Back to original" (nur Grafik), Einmal-Start im
 * Original-Modus (--set nur fuer diesen Start, user.cfg unveraendert).
 * Einordnung: harness/src/sb_origflag.h (eine Tabelle fuer Client und
 * Launcher). ASCII only. */
#include <stddef.h>
#include "launcher_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bitmaske SB_OFG_* der abweichenden Gruppen. fx_pack = 1: ein aktives
 * Effect Pack setzt Werte (Gruppe Effect Packs). HD zaehlt wirksam
 * (remaster && hd_textures). */
unsigned lorig_mask(const launcher_settings_t *s, int fx_pack);
/* Gruppenliste "HD textures, widescreen, lighting" (aktive Sprache, UTF-8). */
void lorig_list(unsigned mask, char *out, size_t cap);
/* W287: wie lorig_list, eigener Trenner und Vorsatz je Eintrag (z. B.
 * "
" + "- " fuer Tooltips mit einer Gruppe je Zeile). */
void lorig_list_sep(unsigned mask, const char *sep, const char *pre, char *out, size_t cap);
/* W287: Gruppen, die ein Grafik-Preset (LGP_*) gegenueber dem Original
 * aendert (0 = Preset ist original). */
unsigned lorig_preset_mask(int preset);
/* "Back to original": NUR die experimentellen Grafikwerte auf Original
 * (4:3, HD aus, Shader/Licht/Effekte/Monitor/Cockpit/HUD original).
 * Bildrate, Aufloesung, Fenster, Steuerung, Ton, VR, Packs-Liste bleiben. */
void lorig_reset(launcher_settings_t *s);
/* --set-Kette fuer den Einmal-Start im Original-Modus: alle experimentellen
 * Schluessel original, remaster=0, leere Packs-Liste (keine Effect/Texture
 * Packs), orig_once=1 (Einblendung im Spiel). 0 = ok, 1 = zu lang. */
int lorig_once_args(char *out, size_t cap);
/* Wert eines Schluessels aus der Tabelle (-1 = unbekannt); Selbsttest/GUI. */
int lorig_value(const launcher_settings_t *s, const char *key);
/* CLI-Selbsttest (Einordnung, Hinweis an/aus, Zuruecksetzen nur Grafik,
 * Standard 4:3, alte cfg unveraendert, Einmal-Kette). */
void lorig_selftest(int *pass, int *fail);

#ifdef __cplusplus
}
#endif
