#pragma once
/* sb_state.h - W232 STATE-LOADCOUNT: zentrale Spielzustands-Erkennung.
 *
 * u32 [0x2EEA34] ist KEIN Spielmodus, sondern ein Ladezaehler: FUN_000F5AF0
 * zaehlt ihn bei jedem Ladebildschirm hoch (1..23, Wrap), auch bei jeder
 * Titel-Demo. Testkette mit zwei Demos: Mission 1 = 4, Mission 2 = 5;
 * Spieler ohne Demo: Mission 1 = 2 (= Wert der ersten Titel-Demo).
 * "==4" trifft also nur zufaellig die erste Mission der Testkette.
 *
 * Spielermission (W225-Logik, input_mouse.c):
 *   Szene [0x6B96DC] == 0x2C (Briefing-Film + Mission)
 *   + Missionsschleife [0x356F58] == 1 (FUN_00045F60 laeuft)
 *   + [0x3B8CDC] != 1 (1 = Titel-Demo/Replay, 0 = Kampagne).
 * Der Briefing-Film zaehlt dazu (gleiche Szene/Schleife), das Missionsende
 * (mloop 1 -> 0) nicht mehr. */

#define SB_ST_LOADCOUNT 0x2EEA34u /* u32 Ladezaehler 1..23 (nur Diagnose/Ketten) */
#define SB_ST_SCENE     0x6B96DCu /* u32 Szene; 0x2C = Film + Mission */
#define SB_ST_MLOOP     0x356F58u /* u32 Missionsschleife FUN_00045F60 */
#define SB_ST_DEMO      0x3B8CDCu /* u32 1 = Titel-Demo/Replay, 0 = Kampagne */

/* Reine Auswertung (Selbsttest): 1 = Spielermission. */
static __inline int sb_state_mission_eval(unsigned scene, unsigned mloop, unsigned demo)
{
    return scene == 0x2Cu && mloop == 1u && demo != 1u;
}

/* 1 = Spieler ist in einer Mission (Film oder Spiel), Titel-Demo
 * ausgenommen; unabhaengig vom Ladezaehler. Liest den Spielspeicher mit
 * SEH-Schutz (0 bei Lesefehler), aus jedem Thread aufrufbar. */
int sb_in_mission(void);

/* 1 = Szene 0x2C + Missionsschleife, Titel-Demo eingeschlossen (wie
 * rq_in_mission; fuer Stellen, die bewusst auch die Demo meinen). */
int sb_state_mission_or_demo(void);

/* Ladezaehler [0x2EEA34] (nur Diagnose/Log und Testketten). */
unsigned sb_state_loadcount(void);
