/* loc_postscene.h - W509 DEBRIEF-BLOOM: Szene der LoC-Endbild-Nachbearbeitung (render_shfx.c) - reine Regel (ohne
 * D3D, ohne Speicherzugriff), Selbsttest render_selftest Fall POSTSCENE.
 *
 * Befund (Capstone, LoC-VAs, nur gelesen; Laeufe W509 E0/O0): Die Lockstep-Aktiv-Maske [0x306830] und damit
 * loc_mouse_mission() bleiben nach TIME UP bis ins DEBRIEFING (und darueber hinaus, W375) gesetzt. Bis W509 galt das
 * Debriefing deshalb als Cockpit-Mission (Szene 1): Mit Enhanced/Enhanced Vivid liefen Cockpit-Leuchten/-Abstrahlung,
 * Abdunklung und das bildbasierte Blitzlicht (fx_light_cockpit, W465) auf den 2D-Seiten. Das Blitzlicht sucht warme,
 * helle, lokale Anstiege im Fensterbereich - ein Seitenwechsel mit gelben Tabellenlinien und Texten loest es aus:
 * das Bild blitzt hell auf.
 *
 * Missionsende im Spiel: FUN_00029C60(Art, Bilder) setzt [0x3068E0] = 1, [0x306C10] = Bilder (Abblende),
 * [0x306FC8] = Art. Die Missionsschleife FUN_0002F100 zaehlt [0x3068E0] je Bild hoch und endet, sobald der Wert
 * [0x306C10] erreicht (danach Auftrag 2 an den Render-Thread, kein Missionsbild mehr). Der Missionsstart
 * (0x4ADCE) setzt [0x3068E0] wieder auf 0. Also: [0x3068E0] != 0 und >= [0x306C10] = Missionsschleife beendet
 * (Ergebnis, DEBRIEFING, folgende Menues) -> keine Cockpit-Nachbearbeitung. Waehrend der Abblende (Zaehler laeuft)
 * bleibt das Missionsbild mit Nachbearbeitung wie bisher. */
#ifndef LOC_POSTSCENE_H
#define LOC_POSTSCENE_H

#define LPS_END_CTR 0x3068E0u /* Missionsende-Zaehler (0 = Mission laeuft) */
#define LPS_END_LEN 0x306C10u /* Laenge der Abblende in Missionsbildern */

/* 1 = Missionsschleife beendet (Zaehler gesetzt und abgelaufen). */
static __inline int lps_mission_over(unsigned end_ctr, unsigned end_len)
{
    return end_ctr != 0u && end_ctr >= end_len;
}

/* Szene: 1 = Cockpit-Mission, 2 = Replay/Aussenkamera/Demo, 0 = sonst (Menues, Lobby, Karte, Debriefing).
 * mission = loc_mouse_mission() (Lockstep aktiv, eigener VT, nicht Karte), replay = [0x306BA8]. Ohne Lockstep-
 * Mission bleibt die bisherige Regel (Replay-Viewer/Demo unveraendert). */
static __inline int lps_scene(int mission, unsigned end_ctr, unsigned end_len, unsigned replay)
{
    if (mission) { return lps_mission_over(end_ctr, end_len) ? 0 : 1; }
    return replay ? 2 : 0;
}

#endif
