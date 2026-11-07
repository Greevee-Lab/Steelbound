#pragma once
/* input_tap.h - W92 INPUT-TAPS: schnelle Tastendruecke gehen nicht verloren.
 *
 * Problem: Das Spiel fragt den Controller nur einmal je Abfrage (Poll =
 * hook_XInputGetState) ab. Mehrere Tipper zwischen zwei Polls verschmelzen
 * im Nachrichtenpfad (input_mouse.c: g_kpend) zu einem gedrueckten Zustand -
 * im Menue kommen von 3x schnell "Cursor rechts" nur 1 Schritt an.
 *
 * Loesung (Schalter [input] key_tap_queue=1, nur Nachrichtenpfad
 * input_source=1): je Taste (VK 1..255, Tastatur + Maustasten) eine kleine
 * Warteschlange von Druck-Flanken. Jeder Druck wird mindestens einen Poll
 * lang gedrueckt gemeldet, zwischen zwei Druecken mindestens einen Poll
 * "los". Mehrfach-Tipper werden der Reihe nach ausgespielt (Deckel 8).
 * Gehaltene Tasten bleiben unveraendert gedrueckt (keine Luecken).
 * Replay, Bots und der GetAsyncKeyState-Pfad (input_source=0) nutzen dieses
 * Modul nicht und bleiben bitgenau. Die Kippschalter-Rastung
 * (imap_toggles_apply) ist unveraendert (sieht nur mehr Flanken).
 *
 * Threads: Schreiber ist der UI-Thread (Fenster-Nachrichten: down/up),
 * Leser ist der Spiel-Thread (Poll: poll_begin + eval). Die Uebergabe-
 * felder (unseen/pending) sind per Interlocked synchronisiert, die
 * Poll-Wiedergabe (phase/prev/Cache) gehoert allein dem Spiel-Thread
 * (wie bisher g_kseen in input_mouse.c: ein pollender Thread).
 *
 * Harness-unabhaengig (nur C + Win32-Interlocked): derselbe Code laeuft im
 * Vergleichs-Selbsttest (input_selftest.c, Teil 6).
 */

#ifdef __cplusplus
extern "C" {
#endif

#define ITAP_MAX 8 /* max. angestaute Tipper je Taste */

/* Alles zuruecksetzen (Start/Selbsttest). */
void itap_init(void);

/* Fokusverlust: alle angestauten Tipper verwerfen (nichts klemmt). */
void itap_clear(void);

/* UI-Thread: Flanke auf/ab (Aufrufer erkennt die Flanke am Pegel:
 * down nur bei up->down, up nur bei down->up; Autowiederholung
 * (lp & 0x40000000) ruft down NICHT erneut). vk 1..255, sonst ignoriert. */
void itap_down_edge(int vk);
void itap_up_edge(int vk);

/* Spiel-Thread: naechste Abfrage beginnt (genau einmal je
 * hook_XInputGetState, aus imouse_poll). Danach liefert itap_eval fuer
 * diesen Poll bei mehrfachem Aufruf denselben Wert (ein Poll = ein
 * Wiedergabe-Schritt, egal wie oft key_down fragt). */
void itap_poll_begin(void);

/* Spiel-Thread: Taste vk im laufenden Poll (phys_down = aktueller
 * Nachrichten-Pegel g_kdown). 1 = gedrueckt, 0 = los. */
int itap_eval(int vk, int phys_down);

/* Angestaute Tipper (nur Selbsttest/DIAG). */
int itap_pending(int vk);

#ifdef __cplusplus
}
#endif
