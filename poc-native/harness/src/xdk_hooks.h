#pragma once
/* xdk_hooks.h - JMP-Patches auf statisch gelinkte XDK-Funktionen (M1-Satz). */
#ifdef __cplusplus
extern "C" {
#endif

void hooks_install(void);
unsigned hooks_frame_count(void);
/* Read-Hang-Detector: liefert 1 + Args, wenn hook_ReadFile seit >0ms im
 * Win32-Read haengt (0 = kein aktiver Read). */
int hooks_read_hang(unsigned *h, unsigned *len, unsigned *ov, unsigned *ms);
void hooks_activity(unsigned *reads, unsigned *xin);
unsigned hooks_activity_rbytes(void); /* W32: gelesene Bytes kumulativ */
void hooks_d3d_put(unsigned *used, unsigned *total);
/* MISSION3D (W25): VSConst-Aufrufzaehler (DIAG). */
void hooks_vsconst_stats(unsigned *c1, unsigned *c4, unsigned *cf);

#ifdef __cplusplus
}
#endif
