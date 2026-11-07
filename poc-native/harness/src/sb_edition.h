/* sb_edition.h - Edition des Builds (W116 EDITIONS).
 *
 * CMake: -DSB_EDITION=private (Standard) | community -> SB_EDITION_COMMUNITY
 * 0/1 fuer Client (sb_emu) und Launcher. Private Edition = Hauptlinie
 * (lokal, HD-Paket, Text-Rekonstruktionen); Community Edition = spaeterer
 * Clean-Export ohne Spielinhalte, ohne private Pfade, ohne Netzwerk
 * (s. poc-native/docs/EDITIONEN.md, editions/community.include).
 *
 * Private Standardpfade (Spieldaten des Entwicklers) stehen nur in
 * sb_private_paths.h; diese Datei ist nicht im Community-Export. */
#ifndef SB_EDITION_H
#define SB_EDITION_H

#ifndef SB_EDITION_COMMUNITY
#define SB_EDITION_COMMUNITY 0
#endif

#if SB_EDITION_COMMUNITY
#define SB_EDITION_NAME "community"
#define SB_EDITION_BADGE "COMMUNITY EDITION"
/* Kein Standardpfad: Spieler waehlt eigene default.xbe / XISO im Launcher. */
#define SB_DEFAULT_XBE_PATH ""
#define SB_DEFAULT_MEDIA_ROOT ""
#else
#define SB_EDITION_NAME "private"
#define SB_EDITION_BADGE "PRIVATE EDITION"
#include "sb_private_paths.h"
#endif

#endif /* SB_EDITION_H */
