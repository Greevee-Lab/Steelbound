#pragma once
/* sb_origview.h - W278 ORIG-FLAG (Client): experimentelle Grafik erkennen,
 * live zwischen "Original" und den eigenen Einstellungen umschalten,
 * kurze Einblendung (Start-Hinweis, Umschalten). Einordnung: sb_origflag.h.
 *
 * - Taste = Profil-Aktion orig_view (W283, Standard KEY:SHIFT+KEY:F8), als
 *   Rueckfall [overlay] orig_key (Standard SHIFT+F8): schaltet im laufenden Spiel
 *   alle Schluessel aus sb_origflag.h auf ihren Original-Wert und wieder
 *   zurueck (HD-Texturen live ueber sb_rem_live_set; Effect-Pack-Werte liegen
 *   in g_cfg und schalten mit). Packs selbst (Liste) wechseln nicht live.
 *   Oeffnet sich das ESC-Menue im Original-Modus, kommen zuerst die eigenen
 *   Einstellungen zurueck ("Als Standard speichern" speichert nie den
 *   Vergleichszustand). In VR ohne Wirkung (Log).
 * - Einblendung: kleines Band oben in der 4:3-Box (nie in den Balken), nicht
 *   im Fotomodus und nicht in VR; zeichnet nur, solange sie laeuft (sonst
 *   Standardpfad bitgleich). Start-Hinweis nur mit orig_hint=1 (Spieler),
 *   Test-cfg sb_native.cfg hat ihn nie.
 * Alles im Render-Thread ausser sb_ov_key (UI-Thread: setzt nur eine Anfrage). */
#ifdef __cplusplus
extern "C" {
#endif

struct ID3D11RenderTargetView;

/* Bitmaske SB_OFG_* der gerade abweichenden Gruppen (wirksame Werte). */
unsigned sb_ov_mask(void);
/* Zustand eines cfg-Schluessels fuers ESC-Menue: 0 = neutral (nicht
 * experimentell), 1 = experimentell, Wert original, 2 = weicht ab. */
int sb_ov_key_state(const char *key);
/* Wie sb_ov_key_state fuer eine Gruppenmaske (z. B. Preset-Zeile). */
int sb_ov_group_state(unsigned groups);
/* W291 ESC-REWORK: Feld in g_cfg zu einem Tabellen-Schluessel (NULL = unbekannt). */
int *sb_ov_field(const char *key);
/* 1 = gerade Original-Vergleich aktiv (Taste) */
int sb_ov_compare_on(void);
/* UI-Thread (WM_KEYDOWN): 1 = Taste gehoert der Umschaltung (verschluckt,
 * auch Autowiederholung; nur der erste Druck schaltet). */
int sb_ov_key(unsigned vk, int repeat);
/* Render-Thread, je Bild (aus dem Overlay-Takt): Anfragen ausfuehren,
 * Start-Hinweis, Ablauf der Einblendung. */
void sb_ov_tick(unsigned present_no);
/* Taste als Text ("Shift+F8"), leer = aus. */
const char *sb_ov_key_name(void);
/* W283 ORIG-KEY-BIND: Tasten aus der Profil-Aktion orig_view uebernehmen
 * (ersetzt den Rueckfall orig_key). vks/mods wie imap_bind_key_combos
 * (mods Bit 0 Shift, 1 Ctrl, 2 Alt), n = 0: Taste aus. pad = 1: das Profil
 * hat zusaetzlich Pad/Controller-Quellen (nur fuer das Log). */
void sb_ov_set_keys(const int *vks, const int *mods, int n, int pad);
/* W283: Umschalten anfordern aus Pad/Controller/Joystick (Hook, Flanke). */
void sb_ov_request(void);

/* W290: Einblendung mit eigenem Text (Titel, Zeile 2), ms Dauer */
void sb_ov_toast_msg(const wchar_t *t1, const wchar_t *t2, unsigned ms);
/* Einblendung zeichnen (wie sb_wm_*): 1 = gerade sichtbar */
int sb_ov_toast_on(void);
void sb_ov_toast_draw(struct ID3D11RenderTargetView *rtv, int save);
void sb_ov_toast_restore(void);

#ifdef __cplusplus
}
#endif
