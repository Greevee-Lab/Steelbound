#pragma once
/* fs_emu.h - Xbox-FS-Emulation (KPCR/KPRCB/KTHREAD + TLS-Slots).
 * Auf Xbox zeigt FS auf den KPCR, auf Windows auf den TEB. Die 28
 * Nicht-SEH-FS-Stellen (siehe analyse/../work/fs_report.txt) werden zur
 * Ladezeit gepatcht; FS:[0x0] (SEH, 55 Stellen) bleibt nativ.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Einmal-Init: Windows-TLS-Slot, Fake-KPRCB, Trampolin-Pool. */
void fs_emu_init(void);

/* Pro Xbox-Thread (und Haupt-Thread) einmal rufen: legt Fake-KTHREAD +
 * TLS-Puffer an und hinterlegt sie im Windows-TLS-Slot. */
void fs_emu_alloc_thread(void);

/* Patcht die 28 FS-Stellen im gemappten Spiel-Code. Bei Byte-Mismatch:
 * harter Abbruch (Exit-Code 7), da ein uebersehener Patch spaeter crasht. */
void fs_emu_patch_all(void);

/* Slot[0]-Prefill, siehe fs_emu.c (nach Startup, vor Spiel-Routine). */
void fs_emu_prefill_xapi(void);

/* TLS-Index-VA aus dem XBE-TLS-Verzeichnis (fuer Prefill-Slotwahl). */
void fs_emu_set_tls_index_va(unsigned va);

/* TLS-Slotbasis des aktuellen Xbox-Threads (NULL ohne Thread-State).
 * Negative Indizes sind gueltig (Headroom), wie bei den FS-Trampolinen. */
uint32_t *fs_emu_tls_slots(void);

#ifdef __cplusplus
}
#endif
