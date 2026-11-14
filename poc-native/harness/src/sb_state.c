/* sb_state.c - W232 STATE-LOADCOUNT: zentrale Missionserkennung (s. sb_state.h). */
#include <stdint.h>
#include "sb_state.h"

#ifdef SB_STATE_SELFTEST
extern unsigned imouse_test_state(unsigned addr); /* input_mouse_selftest.c */
static unsigned st_rd(unsigned addr) { return imouse_test_state(addr); }
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "sb_profile.h" /* W230: SB_ST_* sind EU-Adressen -> SBA() */
static unsigned st_rd(unsigned addr)
{
    addr = SBA(addr);
    __try { return *(volatile unsigned *)(uintptr_t)addr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
#endif

int sb_in_mission(void)
{
    return sb_state_mission_eval(st_rd(SB_ST_SCENE), st_rd(SB_ST_MLOOP), st_rd(SB_ST_DEMO));
}

int sb_state_mission_or_demo(void)
{
    return st_rd(SB_ST_SCENE) == 0x2Cu && st_rd(SB_ST_MLOOP) == 1u;
}

unsigned sb_state_loadcount(void)
{
    return st_rd(SB_ST_LOADCOUNT);
}
