/* loc_assist.h - W440 LOC-LOBBY-MODES (GitHub #79): Spielmodus der LoC-Lobby und wirksame Hilfen - reine Logik.
 *
 * Nutzerentscheid 23.11.: Jede LoC-Lobby hat einen Modus, den der Host beim Anlegen waehlt (steelnet-Feld
 * play_mode, net/steelnet/PROTOKOLL.md "W440"):
 *  - Sim (Standard): keine Zielhilfe (aim_assist), keine Gang-Automatik (auto_gear) - auch wenn der Spieler sie
 *    eingestellt hat; seine Einstellung bleibt gespeichert und gilt nach dem Match wieder.
 *  - Arcade: jeder nutzt nur die Hilfen, die er selbst eingestellt hat. Nichts wird erzwungen.
 *  - Ausweich-Hilfe (slide_assist) und toggle_assist sind keine "Hilfen" in diesem Sinn und bleiben in beiden Modi.
 * Die Hilfen veraendern nur die eigene Eingabe vor dem Eingabewort (hooks_input.c), nie den Spielzustand; alle Seiten
 * bleiben deterministisch, weil das Eingabewort selbst uebertragen wird.
 * Grenzen (net/SECURITY.md §31): rein clientseitig, nicht manipulationssicher.
 *
 * Ohne Abhaengigkeiten (Selbsttest tests/loc_net_selftest.c, las_tests). */
#ifndef LOC_ASSIST_H
#define LOC_ASSIST_H

#include <string.h>

enum { LAS_FREE = 0, LAS_SIM = 1, LAS_ARCADE = 2 }; /* FREE = Steel Battalion: eigene Einstellung, keine Lobby */
enum { LAS_SRC_SB = 0, LAS_SRC_LOBBY = 1, LAS_SRC_NOLOBBY = 2, LAS_SRC_TEST = 3, LAS_SRC_OFFLINE = 4 };

/* Wert des Protokollfelds bzw. der Einstellung: "sim" -> LAS_SIM, "arcade" -> LAS_ARCADE, sonst 0 (ungueltig).
 * Exakt (klein geschrieben, ohne Leerzeichen) wie der Dienst prueft. */
static __inline int las_parse(const char *s)
{
    if (!s) { return 0; }
    if (strcmp(s, "sim") == 0) { return LAS_SIM; }
    if (strcmp(s, "arcade") == 0) { return LAS_ARCADE; }
    return 0;
}

static __inline const char *las_name(int m)
{
    return m == LAS_ARCADE ? "arcade" : m == LAS_SIM ? "sim" : "frei";
}

/* Wirksamer Modus des laufenden Spiels.
 *  loc        1 = Line of Contact (sonst Steel Battalion: LAS_FREE, nichts geaendert)
 *  net        1 = LoC-Netz an (loc_net); 0 = ohne Netz, keine Mitspieler moeglich: LAS_FREE (eigene Einstellung)
 *  srv        1 = Treffpunkt (steelnet) eingerichtet
 *  lobby_mode Modus der aktuellen steelnet-Lobby (LAS_SIM/LAS_ARCADE, 0 = in keiner Lobby)
 *  test_mode  TEST loc_play_mode_test (LAS_SIM/LAS_ARCADE, 0 = aus): gilt nur ohne Treffpunkt (System Link,
 *             Testweg loc_transport_peers), nie neben einer steelnet-Lobby
 * Ohne Lobby-Angabe gilt Sim ("fehlendes Feld = Sim"): ausserhalb einer Lobby, im System Link ohne Treffpunkt und
 * bei Lobbys aelterer Dienste. */
static __inline int las_effective(int loc, int net, int srv, int lobby_mode, int test_mode, int *src)
{
    int s, m;
    if (!loc) {
        s = LAS_SRC_SB;
        m = LAS_FREE;
    } else if (!net) {
        s = LAS_SRC_OFFLINE;
        m = LAS_FREE;
    } else if (lobby_mode == LAS_SIM || lobby_mode == LAS_ARCADE) {
        s = LAS_SRC_LOBBY;
        m = lobby_mode;
    } else if (!srv && (test_mode == LAS_SIM || test_mode == LAS_ARCADE)) {
        s = LAS_SRC_TEST;
        m = test_mode;
    } else {
        s = LAS_SRC_NOLOBBY;
        m = LAS_SIM;
    }
    if (src) { *src = s; }
    return m;
}

/* Wirksamer Wert einer Hilfe (aim_assist, auto_gear): in Sim 0, sonst die eigene Einstellung. */
static __inline int las_value(int eff, int cfg_value)
{
    return eff == LAS_SIM ? 0 : cfg_value;
}

static __inline const char *las_src_name(int s)
{
    return s == LAS_SRC_LOBBY ? "lobby" : s == LAS_SRC_TEST ? "test" : s == LAS_SRC_NOLOBBY ? "ohne-lobby" :
           s == LAS_SRC_OFFLINE ? "ohne-netz" : "sb";
}

#endif
