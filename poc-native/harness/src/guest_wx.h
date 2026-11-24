#pragma once
/* guest_wx.h - W339 SEC-A3: W^X fuer das Gastabbild (SECURITY.md §6.6 Punkt 4, §7).
 *
 * xbe_map legt jede XBE-Section als PAGE_EXECUTE_READWRITE an (die Xbox kennt kein NX; alle Sections tragen das
 * X-Flag). Nach dem Setzen der Hooks schaltet guest_wx_apply das Abbild seitengenau um:
 *   Code (.text, XONLINE, XNET; SB: BINK*)               -> PAGE_EXECUTE_READ
 *   Nur-Lese-Daten (.rdata, XON_RD, DOLBY)               -> PAGE_READONLY
 *   Daten (Header, .data, segNN, .data1, $$XT...)        -> PAGE_READWRITE
 *   XDK-Bibliotheken D3D/D3DX/XGRPH/DSOUND/XACTENG/WMADEC/XPP enthalten Code UND beschreibbare Daten (gemessen) und
 *   bleiben RWX (gezaehlt und geloggt, dokumentierte Ausnahme). Laufzeit-Patches der
 * Hooks benutzen VirtualProtect(alt) und bleiben damit vertraeglich; XeLoadSection setzt eine nachgeladene Section
 * neu (guest_wx_section_loaded).
 * cfg guest_wx: -1 = automatisch (LoC 1, SB 0; Standard), 0 aus, 1 strikt, 2 Diagnose (Verletzung wird geloggt,
 * die Seite wird RWX und das Spiel laeuft weiter - nur zum Finden von Schreibern/Ausfuehrungen, nie ausliefern). */
#include <stdint.h>
#include "xbe_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int g_guest_wx; /* wirksamer Modus nach guest_wx_apply (0 = aus) */

/* Nach Thunks/Hooks, vor dem Sprung in den Entry. mode wie cfg (bereits aufgeloest: 0/1/2). */
void guest_wx_apply(const xbe_image_t *img, int mode);
/* XeLoadSection hat Section-Bytes neu kopiert (vorher RWX gesetzt): Schutz wiederherstellen. */
void guest_wx_section_loaded(const section_info_t *s);
/* Prozessweite Zaehlung beschreibbar+ausfuehrbarer Bereiche (Log, ohne Inhalte). */
void guest_wx_scan(const char *when);
/* TEST (loc_crash_test, Modus 2): Adresse einer beschreibbaren, nicht ausfuehrbaren Gastseite (0 = keine). */
uint32_t guest_wx_data_probe_addr(void);

#ifdef __cplusplus
}
#endif
