#pragma once
/* kernel_hle.h - Kernel-Thunk-Dispatch + HLE-Implementierungen. */
#include "xbe_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

void krnl_init(xbe_image_t *img);
void krnl_install_thunks(xbe_image_t *img);
const char *krnl_name(unsigned ordinal);
void krnl_dump_stats(void);
void krnl_lock_diag(void); /* DIAG(SUSPSAFE): Host-Sperren-Owner beim STALL */
struct xbe_image; /* fwd */
void *krnl_CreateThread(void *fn, void *param, unsigned long flags,
                        unsigned long *tid);
int krnl_xb_thread_count(void);
int krnl_xb_thread_get(int i, void **h, unsigned *routine);
void *krnl_main_thread(void); /* W22: Main-Thread-Handle (fuer CPU-DIAG) */
int krnl_seek_skip(unsigned xh, unsigned long long off); /* W22: Seek-Cache */
void krnl_seek_note(unsigned xh, unsigned long long off);
void krnl_seek_add(unsigned xh, unsigned long long n);
void krnl_seek_invalidate(unsigned xh);
void krnl_seek_stats(long *n, long *skip); /* W32: Fenster-Zaehler */
int krnl_seek_tids(unsigned long *tids, long *skips, int cap); /* W32 */
void *krnl_file_host(unsigned xh);
/* W150 ISO-DIRECT: D: aus dem Abbild (game_image). krnl_vfs_open: 0 = ok
 * (oder kein Abbild konfiguriert), <0 = Fehler (err gefuellt). */
int krnl_vfs_open(const char *image, char *err, unsigned cap);
int krnl_vfs_active(void);
/* W150: Selbsttest (iso_selftest=1): 0 = ok, sonst Anzahl Fehler. */
int krnl_vfs_selftest(void);
/* 1 = Xbox-Handle ist eine Datei aus dem Abbild. */
int krnl_vfs_handle(unsigned xh);
/* XBE (oder andere Datei) ganz aus dem Abbild lesen (malloc). 0 = ok. */
int krnl_vfs_read_all(const char *rel, unsigned char **out, unsigned *len);
/* XAPI-ReadFile auf einem Abbild-Handle: 1 = bedient (got gesetzt),
 * 0 = kein Abbild-Handle (Aufrufer nimmt den Host-Pfad). */
int krnl_vfs_read(unsigned xh, const long long *off, void *buf, unsigned len,
                  unsigned long *got, char *path, unsigned pathcap);
/* W190 SAVE-LOCK: Spielstaende schuetzen (Wegwerf-Arbeitskopie, sb_savecheck.h).
 * krnl_savelock_active: 1 = T:/U: laufen in dieser Sitzung in die Arbeitskopie.
 * krnl_savelock_begin: jetzt einschalten (frische Kopie der echten Staende),
 *   0 = aktiv, -1 = Fehler (Schutz bleibt aus, Log).
 * krnl_savelock_commit: Arbeitskopie in die echten Staende uebernehmen (mit
 *   Sicherung). 0 = ok, 1 = Spiel hat gerade einen Spielstand offen (nichts
 *   getan), -1 = Fehler / nicht aktiv. */
int krnl_savelock_active(void);
int krnl_savelock_begin(const char *why);
int krnl_savelock_commit(void);
/* W190: Selbsttest durch die echten Nt*File-Wege (savelock_selftest=1). 0 = ok. */
int krnl_savelock_selftest(void);
void *krnl_event_host(unsigned xh);
unsigned long __stdcall krnl_GetExitCodeThread(unsigned long xh,
                                               unsigned long *code);

#ifdef __cplusplus
}
#endif
