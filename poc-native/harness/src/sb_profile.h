#pragma once
/* sb_profile.h - W229 US-PROFILE: Versionsprofil der Spieladressen.
 *
 * Alle festen Spieladressen im Client sind EU/PAL-Adressen. SBA(eu) liefert
 * die Adresse fuer die geladene Fassung:
 *   - EU (Standard): unveraendert (g_sba_us == 0, keine Tabelle, keine Pruefung).
 *   - US (NTSC-U, nur mit [expert] allow_ntsc_u=1): .text ueber die aus
 *     tools/region/us_map.json generierte Tabelle (sb_profile_us.inc),
 *     alle anderen Sektionen konstant +0x740, XBE-Kopf unveraendert.
 * Eine .text-Adresse ohne Tabelleneintrag beendet den Prozess (exit 8)
 * mit Logzeile - nie raten. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { SB_PROFILE_EU = 0, SB_PROFILE_US = 1 };

extern int g_sba_us;                         /* 1 = US-Profil aktiv */
uint32_t sba_us_map(uint32_t eu_va);         /* nur bei g_sba_us */
/* Fehlerausgabe (Logzeile) vor exit 8; von emu_main gesetzt, sonst nur exit. */
extern void (*g_sba_fatal)(const char *msg);

static __inline uint32_t SBA(uint32_t eu_va)
{
    return g_sba_us ? sba_us_map(eu_va) : eu_va;
}

/* US-Profil aktivieren (vor Hooks/Patches, nach Abbild-Mapping). */
void sb_profile_select_us(void);
int sb_profile_id(void);
const char *sb_profile_name(void);
/* Erstbytes aller Profileintraege im geladenen (ungepatchten) Abbild pruefen.
 * 0 = ok; sonst Anzahl Abweichungen, msg = erste Abweichung. EU: immer 0. */
int sb_profile_verify(char *msg, unsigned cap);
/* Zaehler fuer Log/Bericht. */
unsigned sb_profile_text_entries(void);
unsigned sb_profile_check_entries(void);

#ifdef __cplusplus
}
#endif
