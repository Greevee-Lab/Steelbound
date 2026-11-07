/* krnl_int.h - interner Header der Kernel-HLE (W68 SPLIT).
 * Gemeinsame Includes, Handle-Tabellen-Typen und die zwischen kernel_hle.c
 * (Dispatch/Stubs/Thunks/Misc), krnl_sync.c (Handles/Threads/Sync/Waits),
 * krnl_file.c (Pfade/Symlinks/Nt*File/Seek-Cache) und krnl_mem.c (Speicher)
 * geteilten Globals/Funktionen. Nur von diesen Modulen einzubinden. */
#ifndef KRNL_INT_H
#define KRNL_INT_H
#include "kernel_hle.h"
#include "xbox_types.h"
#include "hlog.h"
#include "hcfg.h"
#include "fs_emu.h"
#include "sctrace.h"
#include "watch_dr.h" /* W41 WATCHDR */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <intrin.h>
#include "hlock.h" /* W119 LOADLOCK */

typedef uint32_t u32;

/* ---------- Handle-Tabelle (Xbox-Handle = 0x100 + Slot) ---------- */
typedef enum { HT_FREE, HT_EVENT, HT_MUTANT, HT_FILE, HT_THREAD, HT_SYMLINK } ht_kind_t;

typedef struct {
    ht_kind_t kind;
    HANDLE host;
    char path[512]; /* Dateien: Host-Pfad; Symlinks: Ziel */
    int is_dir;
    int delete_on_close;
    uint64_t dir_state; /* FindFirst-Handle bei Verzeichnissen */
    volatile u32 seek_pos; /* W22: Seek-Cache (nur < 4 GB, atomar) */
    volatile int seek_valid;
    unsigned char *stale; /* W63 FATXSTALE: Inhalt vor dem Ueberschreiben */
    u32 stale_len;
    int vfs;           /* W150 ISO-DIRECT: 1 = D:-Datei aus dem Abbild (host=NULL) */
    int vfs_node;      /* W150: Knoten in iso_vfs */
    uint64_t vfs_pos;  /* W150: eigener Dateizeiger (ReadFile ohne Offset) */
    u32 vfs_dir_k;     /* W150: NtQueryDirectoryFile-Fortschritt */
    char vfs_mask[64]; /* W150: NtQueryDirectoryFile-Maske (erster Aufruf) */
} ht_entry_t;

#define XB_HANDLE_BASE 0x100u
#define XB_HANDLE_MAX 2048

/* Steel Battalion (Europe): Title-ID aus dem XBE-Zertifikat (fuer T:/U:). */
#define XB_TITLE_ID_STR "43430002"

/* ---- Geteilte Globals ---- */
extern xbe_image_t *g_img;
extern hlock_t g_unimpl_cs; /* W119: hlock (atomarer Owner) */
extern unsigned g_exreg_n;
extern unsigned long susp_n_suspend, susp_n_retry, susp_n_giveup;
extern LONG s_si_n;
extern LONG s_si_skip;
extern u32 s_si_xh, s_si_cls, s_si_caller;
extern uint64_t s_si_off;
extern unsigned long s_si_tid;
extern char s_si_path[512];
extern HANDLE g_main_thread_h;

/* ---- Modulgrenzen-Funktionen (vorher static) ---- */
void w32_seek_note_skip(void);
void ht_init_once(void);
u32 ht_alloc(ht_kind_t kind, HANDLE host);
ht_entry_t *ht_get(u32 xh);
void ht_free(u32 xh);
u32 __stdcall hle_RtlInitializeCriticalSection(u32 cs);
u32 __stdcall hle_RtlEnterCriticalSection(u32 cs);
u32 __stdcall hle_RtlLeaveCriticalSection(u32 cs);
u32 __stdcall hle_NtCreateEvent(u32 ph, u32 attr, u32 type, u32 initial);
u32 __stdcall hle_NtCreateMutant(u32 ph, u32 attr, u32 owner);
u32 __stdcall hle_NtSetEvent(u32 xh, u32 pPrev);
u32 __stdcall hle_NtClearEvent(u32 xh);
u32 __stdcall hle_NtReleaseMutant(u32 xh, u32 pPrev);
u32 __stdcall hle_NtWaitForSingleObject(u32 xh, u32 alertable, u32 pTimeout);
u32 __stdcall hle_NtWaitForSingleObjectEx(u32 xh, u32 mode, u32 alertable, u32 pTimeout);
u32 __stdcall hle_NtWaitForMultipleObjectsEx(u32 count, u32 pHandles, u32 waitType, u32 mode, u32 alertable, u32 pTimeout);
u32 __stdcall hle_NtSuspendThread(u32 xh, u32 pPrev);
u32 __stdcall hle_NtResumeThread(u32 xh, u32 pPrev);
u32 __stdcall hle_PsCreateSystemThreadEx(u32 phThread, u32 extSize, u32 kernStack, u32 tlsSize, u32 pTid, u32 start, u32 ctx, u32 suspended, u32 dbg, u32 sysRoutine);
u32 __stdcall hle_PsTerminateSystemThread(u32 status);
u32 __stdcall hle_KeSetBasePriorityThread(u32 a0, u32 prio);
u32 __stdcall hle_KeDelayExecutionThread(u32 mode, u32 alertable, u32 pInterval);
u32 __stdcall hle_KeQuerySystemTime(u32 pTime);
unsigned __int64 __stdcall hle_KeQueryInterruptTime(void);
u32 __stdcall hle_KeSetEvent(u32 ev, u32 inc, u32 wait);
u32 __stdcall hle_KeWaitForSingleObject(u32 obj, u32 reason, u32 mode, u32 alert, u32 timeout);
void xb_mkdir_p(const char *path);
void krnl_save_check(void); /* W187 KIA-LOAD: Spielstaende pruefen/reparieren (sb_savecheck.c) */
void krnl_savelock_init(void); /* W190 SAVE-LOCK: Arbeitskopie beim Start anlegen/behalten/verwerfen */
u32 __stdcall hle_NtCreateFile(u32 ph, u32 access, u32 attr, u32 iosb, u32 allocSize, u32 fileAttr, u32 share, u32 disp, u32 options);
u32 __stdcall hle_NtOpenFile(u32 ph, u32 access, u32 attr, u32 iosb, u32 share, u32 options);
u32 __stdcall hle_NtReadFile(u32 xh, u32 ev, u32 apc, u32 apcCtx, u32 iosb, u32 buf, u32 len, u32 pOff);
u32 __stdcall hle_NtWriteFile(u32 xh, u32 ev, u32 apc, u32 apcCtx, u32 iosb, u32 buf, u32 len, u32 pOff);
u32 __stdcall hle_NtClose(u32 xh);
u32 __stdcall hle_NtQueryFullAttributesFile(u32 pAttr, u32 pInfo);
u32 __stdcall hle_NtQueryInformationFile(u32 xh, u32 iosb, u32 info, u32 len, u32 cls);
u32 __stdcall hle_NtQueryDirectoryFile(u32 a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8, u32 a9);
u32 __stdcall hle_NtFlushBuffersFile(u32 xh, u32 iosb);
u32 __stdcall hle_NtQueryVolumeInformationFile(u32 xh, u32 iosb, u32 buf, u32 len, u32 cls);
u32 __stdcall hle_NtDeviceIoControlFile(u32 xh, u32 ev, u32 apc, u32 apcCtx, u32 iosb, u32 code, u32 inBuf, u32 inLen, u32 outBuf, u32 outLen);
u32 __stdcall hle_NtFsControlFile(u32 a0, u32 a1, u32 a2, u32 a3, u32 iosb, u32 code, u32 a6, u32 a7, u32 a8, u32 a9);
u32 __stdcall hle_IoCreateSymbolicLink(u32 pLink, u32 pTarget);
u32 __stdcall hle_NtOpenSymbolicLinkObject(u32 ph, u32 pAttr); /* W162: stdcall/2 (Spiel pusht 2) */

/* W162 ARITY-Zaehler (kernel_hle.c; nur mit hle_arity_check=1 aktiv) */
extern volatile LONG g_arity87_calls, g_arity203_calls;
extern u32 g_arity87_a0, g_arity87_a1;
u32 __stdcall hle_NtQuerySymbolicLinkObject(u32 xh, u32 pTarget, u32 pRetLen);
u32 __stdcall hle_NtSetInformationFile_counted(u32 xh, u32 iosb, u32 info, u32 len, u32 cls);
u32 __stdcall hle_NtAllocateVirtualMemory(u32 pBase, u32 zeroBits, u32 pSize, u32 type, u32 prot);
u32 __stdcall hle_NtFreeVirtualMemory(u32 pBase, u32 pSize, u32 type);
u32 __stdcall hle_NtQueryVirtualMemory(u32 base, u32 pInfo);
u32 __stdcall hle_MmAllocateContiguousMemory(u32 size);
u32 __stdcall hle_MmAllocateContiguousMemoryEx(u32 size, u32 low, u32 high, u32 align, u32 prot);
u32 __stdcall hle_MmFreeContiguousMemory(u32 va);
u32 __stdcall hle_MmAllocateSystemMemory(u32 size, u32 flags);
u32 __stdcall hle_MmFreeSystemMemory(u32 va, u32 flags);
u32 __stdcall hle_ExAllocatePool(u32 size);
u32 __stdcall hle_ExAllocatePoolWithTag(u32 size, u32 tag);
u32 __stdcall hle_ExFreePool(u32 va);
u32 __stdcall hle_ExQueryPoolBlockSize(u32 va);
u32 __stdcall hle_MmQueryAllocationSize(u32 va);
u32 __stdcall hle_MmQueryStatistics(u32 buf);
u32 __stdcall hle_MmQueryAddressProtect(u32 va);
u32 __stdcall hle_MmSetAddressProtect(u32 va, u32 size, u32 prot);
u32 __stdcall hle_MmPersistContiguousMemory(u32 a0, u32 a1, u32 a2);
u32 __stdcall hle_MmClaimGpuInstanceMemory(u32 size, u32 a1);
unsigned __int64 __stdcall hle_MmGetPhysicalAddress(u32 va);
u32 __stdcall hle_MmLockUnlockBufferPages(u32 a0, u32 a1, u32 a2);
u32 __stdcall hle_MmLockUnlockPhysicalPage(u32 a0, u32 a1);

#endif /* KRNL_INT_H */
