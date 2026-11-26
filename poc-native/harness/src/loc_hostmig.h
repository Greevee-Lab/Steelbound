/* loc_hostmig.h - W520 HOSTMIG: Host-Wechsel der LoC-Lobby im Spiel (LOC_BOOT §96). */
#pragma once

/* Je Bild aus loc_render.c (nur LoC-Profil): Ereignis des Transports (host_changed) -> Hinweis; Lobby-Zustand des
 * Spiels beobachten (0x31 = Host verloren -> host_gone an den Dienst); Neuaufbau der Spiel-Session ueber das Menue-Pad
 * (neuer Host: Match erstellen, uebrige: suchen und beitreten). */
void loc_hostmig_frame(unsigned f);
/* Tasten des Neuaufbaus fuer das virtuelle Menue-Pad (loc_pad.c): Bit 0..3 = hoch/runter/links/rechts, 4 = START,
 * 8 = A, 9 = B. 0 ohne laufenden Neuaufbau. */
unsigned loc_hostmig_pad_bits(void);
