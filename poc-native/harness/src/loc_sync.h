/* loc_sync.h - W297 LOC-MISSION: Lockstep-Synchronitaet messen (loc_sync=1, nur LoC-Profil). */
#pragma once

int loc_sync_install(void);
void loc_sync_tick(int waited_s);
void loc_sync_frame(unsigned f); /* je Present (Testfolge loc_steps) */
int loc_pace_install(void); /* W320 LOC-PERF: Missionstakt messen (loc_pace_diag) / Gegenprobe (loc_fields) */
/* W338 LOC-ROLLBACK-R2 (loc_rb_input): Die Eingabeschicht liefert die endgueltigen Akkumulatoren in Slot-Reihenfolge;
 * loc_sync zaehlt dann je Rahmenbau nur Takt (LOCLAT) und Testtasten und rechnet seinen Hash aus diesen Werten
 * (LOCSYNC Rahmen = Slot + 1, gleich auf allen Clients). */
void loc_sync_ext_on(int on);
void loc_sync_ext_frame(unsigned slot, unsigned acc, unsigned tick);
