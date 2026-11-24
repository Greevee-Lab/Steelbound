/* loc_replayctl.h - W457 LOC-REPLAY-CTRL (GitHub #88): Replay-Steuerung in Line of Contact.
 *
 * Befund (Dekompilat, LOC_BOOT §W457): Der Replay-Viewer FUN_0002CFB0 (Aufruf aus FUN_0002F100, solange der Spielmodus
 * [0x306B30] == 2 ist) liest Gamepad UND Original-Controller. Unten steht eine Menueleiste mit 9 Punkten
 * ([0x306AA0]): 0 Pause, 1 Zeitlupe, 2 Kamera-Abstand, 3 Spieler, 4 Replay-Effekt, 5 Cockpit (Pilotensicht), 6 Map (Karte),
 * 7 Sub (Nebenmonitor), 8 Exit. Auswahl Drehhebel bzw. Steuerkreuz links/rechts, Ausloesen Hauptwaffe bzw. A
 * (Lock-On/X = rueckwaerts), dazu je Punkt eine Direkttaste am SBC (Tabelle 0x231970: F1, Tank Detach, FSS, F2,
 * Override, Manipulator, F3, Night Scope, Line Color Change). Ohne Pad und ohne Kenntnis dieser Tasten wirkt das
 * Replay unbedienbar (#88).
 *
 * Loesung: Client-Aktionen replay_pause/_next/_prev/_camera/_speed/_help (input_map.h, imap_lrp_t) plus die
 * bestehende replay_exit. Solange der Viewer laeuft (Modus 2, keine Titel-Demo [0x306914] == 0, kein Pausenmenue
 * [0x3071B4] != 1), druecken sie im SBC-Block die passende Direkttaste - genau das, was ein Original-Controller
 * schickt. Im Match/Menue wirken sie nicht. Bedienhilfe als dezentes Overlay (oben rechts), replay_help schaltet.
 * Nur LoC-Profil. Keine Eingaben im Log (nur Viewer-Zustand: Pause, Tempo, Kamera, Spieler, Menuepunkt). */
#pragma once
#include <stdint.h>
#include "input_map.h"

struct ID3D11RenderTargetView;

/* 1 = Replay-Viewer von LoC laeuft (liest Spielspeicher; ausserhalb LoC 0). */
int loc_rpc_viewer(void);

/* Eingabe-Hook (nach imap_profile_apply, nur LoC): Aktionen auswerten, im Viewer Tasten in b[3] setzen,
 * replay_help schalten. */
void loc_rpc_apply(const imap_profile_t *prof, const imap_snap_t *snap, uint16_t b[3]);

/* Spiel-Thread (loc_render.c, je Bild, nur LoC): Zustand/Log, Hilfe-Zeilen (2x/s), TEST-Haken. */
void loc_rpc_frame(unsigned f);
/* Profil fuer die Hilfe-Zeilen (Tastennamen); Eingabe-Hook beim Laden. */
void loc_rpc_set_profile(const imap_profile_t *prof);
/* Render-Thread (unter ctx_lock): sichtbar? / zeichnen (save = Bereich sichern) / Bereich zurueck. */
int loc_rpc_visible(void);
void loc_rpc_draw(struct ID3D11RenderTargetView *rtv, int save);
void loc_rpc_restore(void);
/* TEST (loc_replay_ctrl_test): 1 = jetzt ein Bild aufnehmen (Name in name). Render-Thread. */
int loc_rpc_shot_due(char *name, unsigned cap);
