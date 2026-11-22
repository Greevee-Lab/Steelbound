/* sb_ally.h - W424 SB-ALLIED-SQUADS: verbuendete Trupps kaempfen auch fern vom
 * Spieler (nur Steel Battalion, [gameplay] sb_allied_squads, Standard 1).
 * Logik: sb_ally_core.h; Adressen/Belege: BERICHT_M2 "W424". */
#ifndef SB_ALLY_H
#define SB_ALLY_H

/* hooks_install (SB-Profil): Thunk an der Aufrufstelle setzen, falls die
 * Option beim Start an ist (sonst bleibt der Spielcode unveraendert). */
void sb_ally_install(void);
/* Present (Spiel-Thread, nie mitten in der Einheitenschleife): Option erst im
 * ESC-Menue eingeschaltet -> Thunk nachinstallieren (wirkt ab der naechsten
 * Mission). */
void sb_ally_live(void);
/* Kernel (NtCreateFile/NtClose): Replay-Dateien (*.rep) auf der Festplatte.
 * write = Handle mit Schreibrecht. host = Host-Pfad. */
void sb_ally_rep_open(const char *host, int write);
void sb_ally_rep_close(const char *host);
/* Laufende Mission: 1 = Zusatzregel aktiv (Overlay/Diagnose). */
int sb_ally_mission_on(void);

#endif
