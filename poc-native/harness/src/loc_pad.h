/* loc_pad.h - W286 LOC-SYSLINK: virtuelles Gamepad (Port 1) fuer die LoC-Netzmenues. */
#pragma once
#include <stdint.h>

int loc_pad_install(void); /* patcht XInputGetCapabilities (Bytevergleich), schaltet das Pad ein */
int loc_pad_active(void);
int loc_pad_is_gamepad_type(uint32_t type);
void loc_pad_device_changes(uint32_t pIns, uint32_t pRem);
uint32_t loc_pad_open(uint32_t type, uint32_t port); /* 0 = kein Gamepad-Typ */
int loc_pad_is_handle(uint32_t h);
uint32_t loc_pad_get_state(uint32_t pState);
