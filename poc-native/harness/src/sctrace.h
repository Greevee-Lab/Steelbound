#pragma once
/* sctrace.h - Syscall-Tracer: loggt jeden Kernel-Thunk-Call (Ordinal, Name,
 * Ruecksprungadresse). Nur zur Diagnose (cfg syscall_trace=1); pro Call eine
 * geflushte Zeile -> die letzte Zeile vor einem stillen Tod ist der Killer. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Baut den Trampolin-Pool (einmalig, vor krnl_install_thunks). */
void sctrace_init(void);

/* Liefert die Trace-Trampolin-Adresse fuer (ordinal -> impl). */
void *sctrace_wrap(unsigned ordinal, void *impl);

/* 1, wenn Tracing aktiv ist (cfg). */
int sctrace_active(void);

#ifdef __cplusplus
}
#endif
