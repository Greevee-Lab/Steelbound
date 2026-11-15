/* loc_net.h - W286 LOC-SYSLINK: XNet-/Winsock-HLE fuer Line of Contact (System Link).
 * Nur im LoC-Profil ([expert] loc_boot=1) und mit [expert] loc_net=1 (Standard).
 * Beide Fassungen (W292 LOC-COMM); mit loc_net=0 meldet LoC "Netz nicht bereit" (W275-Stubs). */
#pragma once

/* Installiert die Netz-Hooks (Bytevergleich vor dem Patchen, sonst exit 7).
 * Rueckgabe 0 = Netz aus (loc_net=0 oder keine Adresse) -> Aufrufer setzt die
 * W275-Stubs "Netz nicht bereit". */
int loc_net_install(void);
void loc_net_tick(int waited_s);
