#pragma once
/* sb_lang.h - W142 L10N-EN: Spielertexte ueber Sprachschluessel.
 *
 * Ein gemeinsamer Baustein fuer Launcher und Client (ESC-Menue). Intern
 * UTF-8. Englisch ist eingebaut (aus poc-native/lang/en.lang zur Buildzeit
 * erzeugt, sb_lang_en.inc); Deutsch (de.lang) liegt neben den Binaries
 * (Ordner lang/). Fehlt ein Schluessel, gilt der englische Text; fehlt
 * auch der, der Schluessel selbst plus einmal eine Warnung (Callback).
 * Log-Zeilen, cfg-Schluessel und Konsolenwerkzeuge bleiben unuebersetzt.
 *
 * Hinweis: Das Makro heisst LT(), nicht L() - L"" ist das C-Praefix fuer
 * Wide-Literale und darf nicht als Makroname belegt werden.
 */
#include <stddef.h> /* wchar_t */
#ifdef __cplusplus
extern "C" {
#endif

/* Einmal beim Start: lang_dir = Ordner mit <code>.lang (UTF-8-Pfad, darf
 * NULL/leer sein: nur eingebautes Englisch), code = "en"/"de" (NULL/leer
 * oder unbekannt = "en"). */
void sb_lang_init(const char *lang_dir, const char *code);
/* Sprache zur Laufzeit wechseln ("en"/"de", sonst "en"). Kein Datei-
 * Zugriff: beide Tabellen liegen im Speicher. */
void sb_lang_set(const char *code);
/* Aktive Sprache ("en" oder "de"). */
const char *sb_lang_code(void);
/* UTF-8-Text zum Schluessel. Nie NULL (Fallback: en, sonst der
 * Schluessel selbst). Zeiger bleibt gueltig (statisch). */
const char *sb_lang(const char *key);
#define LT(k) sb_lang(k)
/* Wie sb_lang, nach UTF-16 gewandelt. Rueckgabe 0 = ok. */
int sb_lang_w(const char *key, wchar_t *out, int cap);
/* Warn-Callback bei fehlendem Schluessel (einmal je Schluessel, NULL =
 * still). Client: HLOG_W; Launcher: Debug-Ausgabe. */
void sb_lang_set_warn(void (*cb)(const char *key));

#ifdef __cplusplus
}
#endif
