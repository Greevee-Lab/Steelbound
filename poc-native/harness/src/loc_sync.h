/* loc_sync.h - W297 LOC-MISSION: Lockstep-Synchronitaet messen (loc_sync=1, nur LoC-Profil). */
#pragma once

int loc_sync_install(void);
void loc_sync_tick(int waited_s);
void loc_sync_frame(unsigned f); /* je Present (Testfolge loc_steps) */
