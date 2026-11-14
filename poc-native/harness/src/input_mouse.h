#pragma once
/* input_mouse.h - W79 INPUT-REAL: echte Maus/Tastatur im Spielerbetrieb.
 *
 * input_source=1: echte Tasten + Maustasten aus Fenster-Nachrichten
 * (WM_KEYDOWN/UP, WM_SYSKEYDOWN/UP, WM_xBUTTONDOWN/UP) statt
 * GetAsyncKeyState. Wirkt dadurch nur, wenn das Client-Fenster die Eingabe
 * bekommt (Fokus); Fokusverlust (WM_KILLFOCUS/WM_ACTIVATE/WM_ACTIVATEAPP)
 * loest alle Tasten. Kurze Tipps bleiben >= 2 Spiel-Frames sichtbar.
 * mouse_capture=1: Maus in der Mission bei Fokus fangen; eigene Menues
 * und Spielmenues ausserhalb der Mission bleiben sichtbar/frei
 * (mouse_capture_outside_mission=1 erlaubt dort das alte Fangverhalten)
 * (Cursor versteckt + per ClipCursor aufs Fenster begrenzt, Deltas per Raw
 * Input WM_INPUT ohne Randsaettigung). F10 = freigeben/fangen, Fokusverlust
 * gibt sofort frei. Fokus-Rueckkehr und Menue-Schliessen fangen automatisch,
 * ausser nach manueller F10-Freigabe. F10 oder Klick faengt dann wieder (der Klick selbst feuert
 * nicht: Maustasten bleiben bis zum Loslassen gesperrt).
 * W225 MOUSE-ARM: Mission = Szene [0x6B96DC]=0x2C + Missionsschleife
 * [0x356F58]=1, ohne Titel-Demo ([0x3B8CDC]=1); der Ladezaehler [0x2EEA34]
 * ist kein Missionsmerkmal (Mission 1 ohne vorherige Demo = 2). Der Soll-
 * Zustand wird bei jeder Fenster-Nachricht und jedem Spiel-Poll neu
 * abgeleitet (Abweichung -> IMOUSE_MSG_SYNC an den UI-Thread). F10 ausserhalb
 * der Mission faengt/gibt manuell frei (bis zur naechsten Missionsflanke).
 * Log bei jedem Wechsel: MOUSE: soll=gefangen|frei grund=... mission=...
 * menue=... fokus=... modus=absolut|relativ (+ Rohwerte der Zustandsadressen).
 * input_focus_only=1: echte Tasten/Maus nur bei Fokus des Client-Fensters.
 * input_test_hook=1 (nur Tests): IMOUSE_MSG_TEST_FOCUS (wParam 1/0) setzt
 * den Fokus logisch (Simulation bleibt auch bei Verlust aktiv), IMOUSE_MSG_TEST_MOVE (wParam=dx, lParam=dy, signed)
 * speist ein Mausdelta wie ein Raw-Input-Paket ein - per PostMessage an das
 * (unsichtbare) Fenster, ohne Vordergrund. Physische Wirkung (ClipCursor,
 * Cursor verstecken, SetCapture) gibt es nur, wenn das Fenster wirklich im
 * Vordergrund ist.
 *
 * Das Fenster gehoert dem Renderer (d3d11_render.c, UI-Thread); dieses Modul
 * fasst den Render-Code nicht an, sondern sucht das eigene SB_NATIVE-Fenster
 * und klinkt sich per Subclassing (GWLP_WNDPROC) ein. */

#define IMOUSE_MSG_TEST_FOCUS (0x8000 + 0x7A) /* WM_APP + 0x7A */
#define IMOUSE_MSG_TEST_STATE (0x8000 + 0x7C) /* W196: logical capture/cursor evidence */
#define IMOUSE_MSG_TEST_MOVE  (0x8000 + 0x7B) /* WM_APP + 0x7B */

/* Pro XInput-Poll (Spiel-Thread): installiert bei Bedarf die Fenster-
 * Eingabe (einmal, sobald das Fenster existiert; input_source=1 oder
 * mouse_capture=1), liefert die Raw-Deltas seit dem letzten Poll (*dx/*dy,
 * 0 ohne mouse_capture) und *blocked = 1, wenn echte Maustasten/-deltas
 * nicht wirken duerfen (nicht gefangen, Klick-Sperre nach dem Fangen, oder
 * input_focus_only ohne Fokus).
 * Rueckgabe 1 = Raw-Pfad aktiv (Fang installiert), 0 = alter
 * GetCursorPos-Pfad (mouse_capture=0, Fenster fehlt noch oder Subclass
 * scheiterte - dann bleibt die Maus wie bisher nutzbar). */
int imouse_poll(int *dx, int *dy, int *blocked);

/* 1 = Client-Fenster hat den Fokus (Fenster-Eingabe installiert: logischer
 * Fokus aus Nachrichten/Test-Hook; sonst Vordergrundfenster). */
int imouse_has_focus(void);

/* input_source=1: Nachrichten-Zustand der Taste vk (0..255) im Spiel-Frame
 * frame (gedrueckt oder kurzer Tipp juenger als 2 Frames). */
int imsg_key(int vk, unsigned frame);

/* Echte Taste/Maustaste vk: input_source=1 -> imsg_key(vk, g_frames),
 * sonst GetAsyncKeyState (bisher). */
int imsg_real_vk(int vk);

/* W236 AIM-ASSIST: 1 = Steuerhilfen duerfen wirken (Spiel-Thread): Mission
 * ohne Titel-Demo (W225-Merkmale), kein eigenes Menue offen und die Maus
 * nicht per F10 freigegeben (mouse_capture=1). */
int imouse_assist_allowed(void);

/* W196: UI thread, idempotent aggregate state for all own menus/panels. */
void imouse_menu_capture(int open);
