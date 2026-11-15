/* krnl_loc.c - W275 LOC-BOOT: Kernel-Ordinals, die nur Line of Contact
 * (XDK 5788) importiert und Steel Battalion nie nutzt (31 Stueck).
 *
 * Aktiv nur im LoC-Profil: krnl_loc_enable() setzt g_krnl_ext, das
 * krnl_install_thunks() fuer jedes Ordinal zuerst fragt. Fuer SB bleibt
 * g_krnl_ext NULL -> Thunk-Installation unveraendert.
 *
 * Semantik wie die SB-Kernel-HLE (M1-Prinzip): kein Netz (PHY ohne Link),
 * keine echte Krypto (Xc* wie XcSHA*: neutral, Null-Digest), keine
 * Treiber-IRPs (Fehlerstatus), Timer feuern nicht (wie KeSetTimer).
 * Kernel-Variablen zeigen auf eigene, bereinigte Daten (Nullschluessel,
 * neutrale Platten-Kennung) - nie echte Konsolen-Schluessel. */
#include "krnl_int.h"

#define LOC_ST_SUCCESS 0x00000000u
#define LOC_ST_UNSUCCESSFUL 0xC0000001u
#define LOC_ST_INVALID_DEVICE_REQUEST 0xC0000010u
#define LOC_ST_OBJECT_NAME_NOT_FOUND 0xC0000034u

int krnl_host_path(const char *xb, char *out, size_t cap); /* krnl_file.c */
u32 __stdcall hle_KeWaitForMultipleObjects(u32 count, u32 objs, u32 waitType, u32 reason,
                                           u32 mode, u32 alertable, u32 timeout, u32 blocks);

/* Erster Aufruf je Ordinal -> eine Logzeile (Boot-Protokoll). */
static volatile LONG s_seen[512];
static void loc_first(unsigned ord, const char *what)
{
    if (ord < 512 && InterlockedExchange(&s_seen[ord], 1) == 0) {
        HLOG_I("LOCKRNL: %s (Ordinal %u) erster Aufruf", what, ord);
    }
}

/* ---------- Variablen (bereinigt) ---------- */
typedef struct { uint16_t len, max; u32 buf; } xstr_t;
static uint16_t s_krnl_version[4] = { 1, 0, 5838, 1 };   /* Major, Minor, Build, Qfe */
static uint8_t s_pubkey[284];                             /* XePublicKeyData: Null */
static uint8_t s_lankey[16];                              /* XboxLANKey: Null (gleich fuer alle Peers) */
static char s_img_name[] = "\\Device\\CdRom0\\default.xbe";
static char s_disk_model[] = "STEELBOUND-HDD";
static char s_disk_serial[] = "0000000000";
static xstr_t s_xe_image_file_name, s_hal_disk_model, s_hal_disk_serial;

static void xstr_set(xstr_t *s, const char *t)
{
    s->len = (uint16_t)strlen(t);
    s->max = (uint16_t)(s->len + 1);
    s->buf = (u32)(uintptr_t)t;
}

/* ---------- Funktionen ---------- */
static u32 __stdcall loc_PhyGetLinkState(u32 verify)
{
    (void)verify;
    loc_first(252, "PhyGetLinkState -> 0 (kein Link)");
    return 0;
}

static u32 __stdcall loc_PhyInitialize(u32 force, u32 param)
{
    (void)force; (void)param;
    loc_first(253, "PhyInitialize -> SUCCESS");
    return LOC_ST_SUCCESS;
}

static u32 __stdcall loc_IoDeleteSymbolicLink(u32 name)
{
    (void)name;
    loc_first(69, "IoDeleteSymbolicLink");
    return LOC_ST_SUCCESS;
}

static u32 __stdcall loc_XcBlockCryptCBC(u32 cipher, u32 len, u32 out, u32 in, u32 ks, u32 op, u32 fb)
{
    (void)cipher; (void)ks; (void)op; (void)fb;
    loc_first(349, "XcBlockCryptCBC (neutral: Kopie)");
    if (out && in && out != in && len) {
        __try { memmove((void *)(uintptr_t)out, (const void *)(uintptr_t)in, len); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return 0;
}

static u32 __stdcall loc_XcKeyTable(u32 cipher, u32 table, u32 key)
{
    (void)cipher; (void)table; (void)key;
    loc_first(347, "XcKeyTable (neutral)");
    return 0;
}

static u32 __stdcall loc_XcDESKeyParity(u32 key, u32 len)
{
    (void)key; (void)len;
    loc_first(346, "XcDESKeyParity (neutral)");
    return 0;
}

static u32 __stdcall loc_XcHMAC(u32 k, u32 kl, u32 d1, u32 l1, u32 d2, u32 l2, u32 digest)
{
    (void)k; (void)kl; (void)d1; (void)l1; (void)d2; (void)l2;
    loc_first(340, "XcHMAC -> Null-Digest (wie XcSHA*)");
    if (digest) {
        __try { memset((void *)(uintptr_t)digest, 0, 20); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return 0;
}

static u32 __stdcall loc_XcRC4Crypt(u32 ks, u32 len, u32 data)
{
    (void)ks; (void)len; (void)data;
    loc_first(339, "XcRC4Crypt (neutral)");
    return 0;
}

static u32 __stdcall loc_XcRC4Key(u32 ks, u32 kl, u32 key)
{
    (void)ks; (void)kl; (void)key;
    loc_first(338, "XcRC4Key (neutral)");
    return 0;
}

static u32 __stdcall loc_XcModExp(u32 a, u32 b, u32 c, u32 d, u32 len)
{
    (void)a; (void)b; (void)c; (void)d; (void)len;
    loc_first(345, "XcModExp -> 0");
    return 0;
}

static u32 __stdcall loc_XcVerifyPKCS1Signature(u32 sig, u32 pub, u32 digest)
{
    (void)sig; (void)pub; (void)digest;
    loc_first(344, "XcVerifyPKCS1Signature -> TRUE");
    return 1;
}

static u32 __stdcall loc_NtDeleteFile(u32 attr)
{
    char path[512];
    u32 st = LOC_ST_OBJECT_NAME_NOT_FOUND;
    loc_first(195, "NtDeleteFile");
    path[0] = 0;
    __try {
        /* OBJECT_ATTRIBUTES (Xbox): +0 RootDirectory, +4 PSTRING ObjectName */
        const xstr_t *on = *(const xstr_t **)(uintptr_t)(attr + 4);
        if (on && on->buf && on->len) {
            char xb[512];
            size_t n = on->len < sizeof xb - 1 ? on->len : sizeof xb - 1;
            memcpy(xb, (const void *)(uintptr_t)on->buf, n);
            xb[n] = 0;
            if (_strnicmp(xb, "D:", 2) != 0 && _strnicmp(xb, "\\??\\D:", 6) != 0 &&
                _strnicmp(xb, "\\Device\\CdRom0", 14) != 0 &&
                krnl_host_path(xb, path, sizeof path) == 0) {
                st = DeleteFileA(path) ? LOC_ST_SUCCESS : LOC_ST_OBJECT_NAME_NOT_FOUND;
            }
            HLOG_I("LOCKRNL: NtDeleteFile '%s' -> 0x%X", xb, st);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        st = LOC_ST_UNSUCCESSFUL;
    }
    return st;
}

static u32 __fastcall loc_IofCallDriver(u32 dev, u32 irp)
{
    (void)dev; (void)irp;
    loc_first(86, "IofCallDriver -> INVALID_DEVICE_REQUEST");
    return LOC_ST_INVALID_DEVICE_REQUEST;
}

static u32 __stdcall loc_IoBuildSynchronousFsdRequest(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
    loc_first(62, "IoBuildSynchronousFsdRequest -> NULL");
    return 0;
}

static u32 __stdcall loc_ObReferenceObjectByName(u32 name, u32 attr, u32 type, u32 ctx, u32 pobj)
{
    (void)name; (void)attr; (void)type; (void)ctx;
    loc_first(247, "ObReferenceObjectByName -> NOT_FOUND");
    if (pobj) {
        __try { *(u32 *)(uintptr_t)pobj = 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return LOC_ST_OBJECT_NAME_NOT_FOUND;
}

static u32 __stdcall loc_KeSetTimerEx(u32 timer, u32 dueLo, u32 dueHi, u32 period, u32 dpc)
{
    (void)timer; (void)dueLo; (void)dueHi; (void)period; (void)dpc;
    loc_first(150, "KeSetTimerEx -> 0 (Timer feuern nicht, wie KeSetTimer)");
    return 0;
}

#define LOC_ACPI_HZ 3375000ull /* Xbox-ACPI-Zeitgeber */
static unsigned __int64 __stdcall loc_KeQueryPerformanceCounter(void)
{
    LARGE_INTEGER c, f;
    unsigned __int64 q, r;
    loc_first(126, "KeQueryPerformanceCounter");
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    q = (unsigned __int64)c.QuadPart / (unsigned __int64)f.QuadPart;
    r = (unsigned __int64)c.QuadPart % (unsigned __int64)f.QuadPart;
    return q * LOC_ACPI_HZ + r * LOC_ACPI_HZ / (unsigned __int64)f.QuadPart;
}

static unsigned __int64 __stdcall loc_KeQueryPerformanceFrequency(void)
{
    loc_first(127, "KeQueryPerformanceFrequency");
    return LOC_ACPI_HZ;
}

static u32 __stdcall loc_NtSetSystemTime(u32 nt, u32 old)
{
    (void)nt; (void)old;
    loc_first(228, "NtSetSystemTime (ignoriert)");
    return LOC_ST_SUCCESS;
}

static u32 __stdcall loc_NtYieldExecution(void)
{
    SwitchToThread();
    return LOC_ST_SUCCESS;
}

static u32 __stdcall loc_IoSynchronousFsdRequest(u32 a, u32 b, u32 c, u32 d, u32 e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    loc_first(85, "IoSynchronousFsdRequest -> INVALID_DEVICE_REQUEST");
    return LOC_ST_INVALID_DEVICE_REQUEST;
}

static u32 __stdcall loc_IoSynchronousDeviceIoControlRequest(u32 a, u32 b, u32 c, u32 d, u32 e,
                                                             u32 f, u32 g, u32 h)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h;
    loc_first(84, "IoSynchronousDeviceIoControlRequest -> INVALID_DEVICE_REQUEST");
    return LOC_ST_INVALID_DEVICE_REQUEST;
}

static u32 __stdcall loc_MmCreateKernelStack(u32 size, u32 dbg)
{
    uint8_t *p;
    (void)dbg;
    loc_first(169, "MmCreateKernelStack");
    if (!size) {
        size = 0x4000u;
    }
    p = (uint8_t *)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return p ? (u32)(uintptr_t)(p + size) : 0; /* Stack-Basis = oberes Ende */
}

static u32 __stdcall loc_MmDeleteKernelStack(u32 base, u32 limit)
{
    (void)base;
    loc_first(170, "MmDeleteKernelStack");
    if (limit) {
        VirtualFree((void *)(uintptr_t)limit, 0, MEM_RELEASE);
    }
    return 0;
}

static u32 __stdcall loc_KeWaitForMultipleObjects(u32 count, u32 objs, u32 wt, u32 reason, u32 mode,
                                                  u32 alertable, u32 timeout, u32 blocks)
{
    loc_first(158, "KeWaitForMultipleObjects");
    return hle_KeWaitForMultipleObjects(count, objs, wt, reason, mode, alertable, timeout, blocks);
}

/* ---------- Tabelle ---------- */
typedef struct { unsigned ord; const char *name; int is_var; void *p; } loc_ord_t;

static loc_ord_t s_tab[] = {
    { 252, "PhyGetLinkState", 0, (void *)loc_PhyGetLinkState },
    { 253, "PhyInitialize", 0, (void *)loc_PhyInitialize },
    { 158, "KeWaitForMultipleObjects", 0, (void *)loc_KeWaitForMultipleObjects },
    { 69, "IoDeleteSymbolicLink", 0, (void *)loc_IoDeleteSymbolicLink },
    { 349, "XcBlockCryptCBC", 0, (void *)loc_XcBlockCryptCBC },
    { 347, "XcKeyTable", 0, (void *)loc_XcKeyTable },
    { 346, "XcDESKeyParity", 0, (void *)loc_XcDESKeyParity },
    { 340, "XcHMAC", 0, (void *)loc_XcHMAC },
    { 339, "XcRC4Crypt", 0, (void *)loc_XcRC4Crypt },
    { 338, "XcRC4Key", 0, (void *)loc_XcRC4Key },
    { 345, "XcModExp", 0, (void *)loc_XcModExp },
    { 344, "XcVerifyPKCS1Signature", 0, (void *)loc_XcVerifyPKCS1Signature },
    { 195, "NtDeleteFile", 0, (void *)loc_NtDeleteFile },
    { 355, "XePublicKeyData", 1, (void *)s_pubkey },
    { 324, "XboxKrnlVersion", 1, (void *)s_krnl_version },
    { 353, "XboxLANKey", 1, (void *)s_lankey },
    { 86, "IofCallDriver", 0, (void *)loc_IofCallDriver },
    { 62, "IoBuildSynchronousFsdRequest", 0, (void *)loc_IoBuildSynchronousFsdRequest },
    { 247, "ObReferenceObjectByName", 0, (void *)loc_ObReferenceObjectByName },
    { 150, "KeSetTimerEx", 0, (void *)loc_KeSetTimerEx },
    { 126, "KeQueryPerformanceCounter", 0, (void *)loc_KeQueryPerformanceCounter },
    { 127, "KeQueryPerformanceFrequency", 0, (void *)loc_KeQueryPerformanceFrequency },
    { 42, "HalDiskSerialNumber", 1, (void *)&s_hal_disk_serial },
    { 41, "HalDiskModelNumber", 1, (void *)&s_hal_disk_model },
    { 228, "NtSetSystemTime", 0, (void *)loc_NtSetSystemTime },
    { 238, "NtYieldExecution", 0, (void *)loc_NtYieldExecution },
    { 326, "XeImageFileName", 1, (void *)&s_xe_image_file_name },
    { 85, "IoSynchronousFsdRequest", 0, (void *)loc_IoSynchronousFsdRequest },
    { 84, "IoSynchronousDeviceIoControlRequest", 0, (void *)loc_IoSynchronousDeviceIoControlRequest },
    { 170, "MmDeleteKernelStack", 0, (void *)loc_MmDeleteKernelStack },
    { 169, "MmCreateKernelStack", 0, (void *)loc_MmCreateKernelStack },
};

static unsigned s_used;

static void *loc_ext(unsigned ord, int *is_var)
{
    unsigned i;
    for (i = 0; i < sizeof s_tab / sizeof s_tab[0]; i++) {
        if (s_tab[i].ord == ord) {
            *is_var = s_tab[i].is_var;
            s_used++;
            HLOG_I("LOCKRNL: Ordinal %u %s -> %s", ord, s_tab[i].name,
                   s_tab[i].is_var ? "Variable (bereinigt)" : "LoC-HLE");
            return s_tab[i].p;
        }
    }
    return NULL;
}

void krnl_loc_enable(void)
{
    xstr_set(&s_xe_image_file_name, s_img_name);
    xstr_set(&s_hal_disk_model, s_disk_model);
    xstr_set(&s_hal_disk_serial, s_disk_serial);
    g_krnl_ext = loc_ext;
    strcpy_s(g_xb_title_id_str, sizeof g_xb_title_id_str, "43430009");
    HLOG_I("LOCKRNL: Erweiterung aktiv (%u LoC-Ordinals, T:/U: -> Title-ID %s)",
           (unsigned)(sizeof s_tab / sizeof s_tab[0]), g_xb_title_id_str);
}

unsigned krnl_loc_used(void)
{
    return s_used;
}
