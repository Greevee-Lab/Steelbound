/* loc_vpf.h - W394 LOC-VIEW-PERFRAME: Sicht und Fadenkreuz je Bild aus Maus bzw. Hebel vorzeichnen (nur LoC, nur Anzeige).
 *
 * Gemeinsamer Akkumulator: Die Raw-Deltas der Maus sammelt der UI-Thread wie bisher in input_mouse.c. Die Simulation holt
 * sie genau einmal je Poll ab (imouse_poll, unveraendert) und bekommt den daraus abgebildeten Rahmen deterministisch.
 * Die Anzeige liest nur: nach jedem Poll veroeffentlicht der Spiel-Thread den Akku-Stand der Abbildung (Seqlock), der
 * Render-Thread rechnet daraus plus den noch nicht abgeholten Deltas (imouse_raw_peek) den Rahmen, den der naechste Poll
 * liefern wuerde - auf Kopien, ohne Doppelzaehlung (die Deltas gehoeren erst nach dem Abholen zum Stand).
 * Abgebildet werden nur Achsen, die im letzten Poll aus der Legacy-Belegung kamen und danach unveraendert blieben
 * (Zielhebel aus der Maus, Sicht aus der Maus mit mouse_sight=1, im Spielerbetrieb Pfeiltasten); sonst gilt der Wert des
 * letzten Polls. Keine Eingaben im Log. */
#pragma once
#include <stdint.h>


/* Spiel-Thread (hooks_input.c, nur g_sba_loc): vor imouse_poll; nach der Abbildung (Zustand/Snapshot der Abbildung,
 * Block am Ende des Snapshot-Blocks, legacy = 1 Legacy-Belegung); nach sbc_merge (endgueltiger Block). */
void loc_vpf_begin(void);
void loc_vpf_mapped(const void *st, const void *snap, const void *blk, int legacy);
void loc_vpf_end(const void *blk);

/* DIAG (Spiel-Thread): Block-Werte (aiming_x/y, sight_x/y) des Polls poll; 0 = nicht mehr vorhanden. */
int loc_vpf_fin(uint32_t poll, uint8_t fin[4]);

/* Laufende Nummer des letzten vollstaendigen Polls (Spiel-Thread: am Ruhepunkt fuer loc_aim). */
uint32_t loc_vpf_poll_seq(void);
/* QPC-Zeit des letzten vollstaendigen Polls (0 = keiner). */
long long loc_vpf_poll_time(void);

/* Render-Thread: aus dem Eingabesatz base (14 Byte, Rahmen des Polls *poll) den Vorschau-Rahmen out bilden (Achsen
 * Zielhebel/Sicht durch den jetzigen Akku-Stand ersetzt). Rueckgabe 1 = gueltig (*poll = Poll, auf dem die Vorschau
 * aufsetzt), 0 = gerade ein Poll im Gang bzw. nichts veroeffentlicht. */
int loc_vpf_preview(const uint8_t base[14], uint8_t out[14], uint32_t *poll);

/* Umrechnung Block-Wert -> Byte im 14-Byte-Eingabesatz (Zielhebel uint8 Mitte 0x7F/0x80 -> int8, Sicht int8). */
int loc_vpf_conv_aim(int v);

/* TEST loc_vpf_test (nur input_test_hook + Testfolge): Pseudo-Taste 337 gehalten -> Testthread speist Mausschritte zu
 * zufaelliger Phase gegen den Tick ein (Messung Eingabe-zu-Bild). Spiel-Thread je Poll. */
void loc_vpf_test_drive(void);
/* TEST loc_vpf_test_alt: 1 = je Bild in dieser Gruppe von 4 Schritten aus (Vergleich im selben Lauf). */
int loc_vpf_test_off(void);
