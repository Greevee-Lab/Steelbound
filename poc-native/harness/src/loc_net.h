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
/* W338 LOC-ROLLBACK-R2: Netz-Kern (Adresse, Portbasis, Mitspieler, Verzoegerungsstufe) fuer den eigenen
 * Eingabekanal; NULL = Netz aus. */
struct locnet *loc_net_core(void);
/* W353 LOC-DURCHSTICH: Netzwerte fuer das F11-Paket (Transport, Treffpunkt, Weg, Ping, Rollback) als eine Zeile;
 * nur Zustaende und Zahlen (keine Adressen, kein Raumcode, keine Eingaben). 0 = kein LoC-Netz. */
int loc_net_diag_line(char *buf, int cap);
/* W357 LOC-PAUSE-NET: 1 = Mitspieler verbunden (eigener Transport mit Verbindung, eigener Lockstep-Rahmen oder
 * Empfang auf den Spiel-Sockets in den letzten SB_PN_HOLD_MS); nur lesend, je Bild vom Spiel-Thread (Overlay). */
int loc_net_peer_live(void);
/* W350 LOC-REJOIN (DIAG, nur loc_net_diag=1): Belegung der Verbindungstabelle des Spiels je Bild lesen und bei
 * Aenderung eine Zeile LOCROSTER loggen (Plaetze, Seiten-Zaehler; keine Namen, keine Inhalte). */
void loc_net_lobby_frame(unsigned f);
