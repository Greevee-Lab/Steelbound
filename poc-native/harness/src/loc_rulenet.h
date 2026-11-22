/* loc_rulenet.h - W420 LOC-CLIPDUMP: Match-Regel "Clip dumping unterbinden" im LoC-Profil (Eingriff + Abgleich).
 * Kern (Paket, Entscheidung) in loc_rule.c; hier Spielstelle, Regelkanal, Trennen, Anzeige. LOC_BOOT §83. */
#pragma once

/* Eingriff 0xCF4BE (Bytevergleich, sonst exit 7) und Regelkanal (Thread, 5 Hz). Nur LoC. */
void loc_rule_install(void);
/* Anzeige (F3): *on = Regel aktiv, *src = LRU_SRC_*, *mission = in der Mission (Regel fest). 0 = nicht installiert. */
int loc_rule_hud(int *on, int *src, int *mission);
