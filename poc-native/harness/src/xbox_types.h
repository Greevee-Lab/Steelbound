#pragma once
/* xbox_types.h - Xbox-Kernel-Typen (Layout aus Cxbx: kernel/common/types.h).
 * 32-bit only: Xbox-Adressen sind native Pointer. */
#include <stdint.h>

#ifndef _M_IX86
#error "Der Harness braucht 32-bit x86 (-A Win32)"
#endif

typedef uint32_t xb_u32;
typedef int32_t xb_s32;
typedef uint16_t xb_u16;
typedef uint8_t xb_u8;
typedef xb_u32 xb_ntstatus;
typedef xb_u32 xb_handle; /* Xbox-Handle: Index in unsere Handle-Tabelle */

typedef struct {
    xb_u16 length;
    xb_u16 max_length;
    wchar_t *buffer;
} xb_unicode_string;

typedef struct {
    xb_u16 length;
    xb_u16 max_length;
    char *buffer;
} xb_ansi_string;

typedef struct {
    xb_u32 root_directory; /* HANDLE */
    xb_ansi_string *object_name; /* PSTRING = ANSI auf Xbox! */
    xb_u32 attributes;
} xb_object_attributes; /* 12 Bytes, kein Length-Feld auf Xbox */

typedef struct {
    xb_u32 status; /* oder Pointer (Union) */
    xb_u32 information;
} xb_io_status_block;

typedef struct {
    xb_u32 creation_time_lo, creation_time_hi;
    xb_u32 last_access_lo, last_access_hi;
    xb_u32 last_write_lo, last_write_hi;
    xb_u32 change_lo, change_hi;
    xb_u32 alloc_lo, alloc_hi;
    xb_u32 eof_lo, eof_hi;
    xb_u32 attributes;
} xb_file_network_open_info; /* 56 Bytes */

typedef struct {
    xb_u32 flags;
    void *vaddr;
    xb_u32 vsize;
    xb_u32 rawaddr;
    xb_u32 rawsize;
    const char *name;
    xb_u32 refcount;
    void *head_ref;
    void *tail_ref;
    xb_u8 sha[20];
} xb_section_header; /* 56 Bytes */

/* NTSTATUS (Auswahl, identisch zu Win32-NT) */
#define X_STATUS_SUCCESS 0x00000000u
#define X_STATUS_WAIT_0 0x00000000u
#define X_STATUS_ABANDONED 0x00000080u
#define X_STATUS_TIMEOUT 0x00000102u
#define X_STATUS_PENDING 0x00000103u
#define X_STATUS_NO_MORE_FILES 0x80000006u
#define X_STATUS_INVALID_HANDLE 0xC0000008u
#define X_STATUS_INVALID_PARAMETER 0xC000000Du
#define X_STATUS_NO_SUCH_DEVICE 0xC000000Eu
#define X_STATUS_INVALID_DEVICE_REQUEST 0xC0000010u
#define X_STATUS_END_OF_FILE 0xC0000011u
#define X_STATUS_ACCESS_DENIED 0xC0000022u
#define X_STATUS_OBJECT_NAME_INVALID 0xC0000033u
#define X_STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034u
#define X_STATUS_OBJECT_NAME_COLLISION 0xC0000035u
#define X_STATUS_FILE_IS_A_DIRECTORY 0xC00000BAu
#define X_STATUS_INFO_LENGTH_MISMATCH 0xC0000004u
#define X_STATUS_INVALID_INFO_CLASS 0xC0000003u
#define X_STATUS_NOT_SUPPORTED 0xC00000BBu
#define X_STATUS_NO_MEMORY 0xC0000017u

#define X_NT_SUCCESS(s) ((xb_s32)(s) >= 0)

/* File-Dispositions (NT) */
#define X_FILE_SUPERSEDE 0u
#define X_FILE_OPEN 1u
#define X_FILE_CREATE 2u
#define X_FILE_OPEN_IF 3u
#define X_FILE_OVERWRITE 4u
#define X_FILE_OVERWRITE_IF 5u

/* File-CreateOptions (Auswahl) */
#define X_FILE_DIRECTORY_FILE 0x00000001u
#define X_FILE_SYNCHRONOUS_IO_NONALERT 0x00000020u

/* FileInformationClass (Auswahl, NT-Reihenfolge) */
#define X_FileBasicInformation 4u
#define X_FileStandardInformation 5u
#define X_FilePositionInformation 14u
#define X_FileEndOfFileInformation 20u
#define X_FileNetworkOpenInformation 34u

/* Event-Typ */
#define X_NotificationEvent 0u
#define X_SynchronizationEvent 1u
