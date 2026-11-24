/* loc_lobbywidget.h - W460 LOC-LOBBY-WIDGET (GitHub #95): Widget "offene Lobbys" im LoC-Hauptmenue und MatchMaking.
 *
 * Solange das Spiel MAIN MENU oder das MatchMaking-Menue (Search Match / Create Match) zeigt, steht rechts in der
 * Bildschirmmitte (Backbuffer, Slot OVS_SLOT_LOBBYWIDGET) ein kleines Panel im gemeinsamen Overlay-Look: Kopfzeile mit
 * der Zahl offener Lobbys, darunter hoechstens 5 Zeilen Host-Pilot, SIM/ARCADE (Farben wie die Suchliste W443),
 * Spieler x/y und Ping-Schaetzung farbig (gruen bis 80 ms, gelb bis 150 ms, rot darueber). Leer: "No open lobbies -
 * host one!", ohne oeffentlichen Treffpunkt (System Link, Raumcode, offline) ein dezenter Hinweis.
 * Daten: dieselbe steelnet-Liste wie die Suche (lobby_list, Transport loc_tun_pub.c); nur solange das Widget sichtbar
 * ist, fragt der Transport hoechstens alle 12 s nach. Die Quelle ist austauschbar (lw_source, fuer den LAN-Modus W454).
 * Aus in Lobby/Mission/anderen Menues, bei ESC-Menue und VR (W474: F3 blendet nicht aus); Schalter loc_lobby_widget (Standard 1,
 * Launcher und ESC-Menue). Nur lesend: aendert nichts am Spielzustand. Log LOCWIDGET nur Zahlen (keine Namen/IPs). */
#pragma once

#include <wchar.h>

struct ID3D11RenderTargetView;

#define LW_MAX_ROWS 5   /* sichtbare Zeilen */
#define LW_SRC_OFF (-1) /* Quelle nicht verfuegbar (kein oeffentlicher Treffpunkt / offline) */
#define LW_SRC_WAIT (-2) /* Quelle verbindet bzw. Liste noch nicht da */
#define LW_SRC_CODE (-3) /* privat mit Raumcode: keine oeffentliche Liste */

typedef struct lw_entry {
    int mode;           /* 1 = SIM, 2 = ARCADE (LAS_*), 0 = unbekannt */
    int est;            /* Ping-Schaetzung ms, -1 = unbekannt */
    int cur, max;       /* Spieler belegt / Sitze */
    wchar_t name[24];   /* Host-Pilot bzw. Lobbyname */
} lw_entry;

/* Datenquelle (Spiel-Thread). watch(1/0): Widget sichtbar/aus (Quelle darf nur dann abfragen). fetch: hoechstens cap
 * Eintraege, *total = alle; Rueckgabe Zahl in out oder LW_SRC_OFF/LW_SRC_WAIT/LW_SRC_CODE. */
typedef struct lw_source {
    const char *name;
    void (*watch)(int on);
    int (*fetch)(lw_entry *out, int cap, int *total);
} lw_source;

/* Quelle setzen (NULL = Standard steelnet). Spiel-Thread, vor bzw. zwischen loc_lobbywidget_frame. */
void loc_lobbywidget_set_source(const lw_source *src);

/* Erkennung der Seite (reine Funktion, auch Selbsttest): 1 = MAIN MENU, 2 = MatchMaking, 0 = sonst.
 * Werte aus dem Spielspeicher (LoC-XBE, Abzuege W460): Szene u16 [0x727988] = 8 (Hauptmenue-Hub); Text-Id der ersten
 * Menuezeile u32 [0x6DC68C]: 0x1B4 MISSION SELECT (MAIN MENU), 0x3A0 Search Match (MatchMaking); andere Seiten der
 * Szene 8: 0x1B5 Missionswahl (Create Match), 0x432 Suchergebnis und "Cancel?"-Dialog. Zahl der Zeilen u8 [0x6DD2B0]:
 * 7 im MAIN MENU, 2 im MatchMaking (zweite Bedingung gegen Zufallstreffer). Unabhaengig von der Cursorzeile. */
#define LW_TID_MAIN 0x1B4
#define LW_TID_MATCH 0x3A0
static __inline int lw_screen_from(int scene, int text_id, int nitems)
{
    if (scene != 8) return 0;
    if (text_id == LW_TID_MAIN && nitems == 7) return 1;
    if (text_id == LW_TID_MATCH && nitems == 2) return 2;
    return 0;
}

/* Spiel-Thread (loc_render.c, je Bild, nur LoC-Profil). */
void loc_lobbywidget_frame(unsigned f);
/* Render-Thread (unter ctx_lock): sichtbar? / zeichnen (save = Bereich sichern) / Bereich zurueck. */
int loc_lobbywidget_visible(void);
void loc_lobbywidget_draw(struct ID3D11RenderTargetView *rtv, int save);
void loc_lobbywidget_restore(void);
/* TEST (loc_lobby_widget_test): 1 = jetzt ein Bild aufnehmen (Name in name). Render-Thread. */
int loc_lobbywidget_shot_due(char *name, unsigned cap);
