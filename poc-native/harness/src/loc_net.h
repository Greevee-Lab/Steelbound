/* loc_net.h - W286 LOC-SYSLINK: XNet-/Winsock-HLE fuer Line of Contact (System Link).
 * Nur im LoC-Profil ([expert] loc_boot=1) und mit [expert] loc_net=1 (Standard).
 * Beide Fassungen (W292 LOC-COMM); mit loc_net=0 meldet LoC "Netz nicht bereit" (W275-Stubs). */
#pragma once

/* Installiert die Netz-Hooks (Bytevergleich vor dem Patchen, sonst exit 7).
 * Rueckgabe 0 = Netz aus (loc_net=0 oder keine Adresse) -> Aufrufer setzt die
 * W275-Stubs "Netz nicht bereit". */
int loc_net_install(void);
void loc_net_tick(int waited_s);
/* W297: Netz-Zustand fuer Testfolgen: 0 = Netz aus, 1 = Typ nicht gesehen, 2 = Typ gesehen (typ >= 256: nur Zaehler);
 * typ 512: 3 = Lockstep aktiv (eigener 0x34-Rahmen < 1 s alt). */
int loc_net_test_state(long *accepts, long *connects, int out, unsigned typ);
