#pragma once
/* loc_boot.h - W275 LOC-BOOT: Line of Contact im Client starten (experimentell).
 *
 * Nur aktiv, wenn sb_gamever die LoC-World-Disc erkennt und [expert]
 * loc_boot=1 gesetzt ist. Steel Battalion (EU/US) laeuft nie hier durch. */
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Profil waehlen (vor xbe_map/krnl_init): LoC-Adressprofil, Kernel-
 * Erweiterung (LoC-only-Ordinals), eigener Spielstandordner. */
void loc_boot_select(void);
/* Nach krnl_install_thunks: Erstbytes pruefen (exit 8 bei Abweichung),
 * XDK-Hooks (D3D/XAPI/XInput) + LoC-Stubs (XACT/DSOUND-Init) setzen. */
void loc_boot_install(void);
/* VEH-Kurzmeldung (gedeckelt) fuer Ausnahmen im LoC-Code; 1 = gemeldet. */
int loc_boot_veh_note(EXCEPTION_POINTERS *ep);
/* Watchdog-Takt (alle 5 s): Boot-Fortschritt als LOCBOOT-Zeile. */
void loc_boot_tick(int waited_s);
/* W277 LOC-DISC: Xbox-SetLastError fuer XDK 5788 (XAPI-Block = slots[[_tls_index]],
 * LastError bei +8; SB 4627: [0x6E8A80], +4). slots = fs_emu_tls_slots(). */
void loc_set_last_error(unsigned int *slots, unsigned int code);

#ifdef __cplusplus
}
#endif
