/* kernel_hle.c (1/3) - Dispatch, Handle-Tabelle, Threads, Pfade.
 * Thunk-Ziele: echte Impl wo noetig, sonst arity-exakte Logging-Stubs
 * (aus ordinal_table.inc generiert). Variablen -> Scratch-Puffer.
 * W68 SPLIT: hier Stubs/Dispatch-Metadaten, Variablen, Strings, Io-Device,
 * Ke/Hal/Av/Xe/Xc/Dbg, Init + Thunk-Installation; Handles/Threads/Sync in
 * krnl_sync.c, Pfade/Dateien in krnl_file.c, Speicher in krnl_mem.c. */
#include "krnl_int.h"

/* Vorwaerts: XBE-Bild (Definition unten bei den Implementierungen) */
xbe_image_t *g_img;

/* ---------- Unimplemented-Stub mit Zaehler ---------- */
static u32 g_unimpl_count[512];
hlock_t g_unimpl_cs; /* W119 LOADLOCK: hlock statt CRITICAL_SECTION */

/* W162 ARITY: Aufruf-Zaehler der HLE-Arity-Funde (Ord 87/203, W157). Nur mit
 * [log] hle_arity_check=1 aktiv (Selbsttest + Zaehlung, sonst kein Zusatzcode
 * im Aufrufpfad ausser einem int-Vergleich). */
volatile LONG g_arity87_calls;
volatile LONG g_arity203_calls;
u32 g_arity87_a0, g_arity87_a1; /* letzte Register-Args von Ord 87 (fastcall-Nachweis) */
static u32 *s_arity_slot87;    /* Thunk-Slot-Adressen fuer den Selbsttest */
static u32 *s_arity_slot203;

static u32 krnl_unimpl(unsigned ordinal)
{
    u32 c;
    hlock_enter(&g_unimpl_cs);
    c = g_unimpl_count[ordinal];
    g_unimpl_count[ordinal] = c + 1;
    hlock_leave(&g_unimpl_cs);
    if (c == 0) {
        HLOG_W("KRNL #%u %s: NICHT IMPLEMENTIERT -> return 0", ordinal,
               krnl_name(ordinal));
    }
    return 0;
}

unsigned long susp_n_suspend, susp_n_retry, susp_n_giveup; /* SUSPSAFE */
/* W22 FILMAV: NtSetInformationFile-Spin-Zaehler (t2-Seek-Loop). */
LONG s_si_n;
LONG s_si_skip;
u32 s_si_xh, s_si_cls, s_si_caller;
uint64_t s_si_off;
unsigned long s_si_tid; /* W32: Tid des Sample-Aufrufs */
char s_si_path[512];
/* W32 STALL20: pro-Tid Skip-Tabelle (wer schlaeft per Sleep(1)?).
 * Append-only, max 8 Tids; weitere Tids entfallen (selten). */
#define W32_SEEK_TIDS 8
static unsigned long s_sk_tid[W32_SEEK_TIDS];
static LONG s_sk_n[W32_SEEK_TIDS];

void krnl_seek_stats(long *n, long *skip)
{
    *n = s_si_n;
    *skip = s_si_skip;
}

int krnl_seek_tids(unsigned long *tids, long *skips, int cap)
{
    int i, o = 0;
    for (i = 0; i < W32_SEEK_TIDS && o < cap; i++) {
        if (!s_sk_tid[i])
            break;
        tids[o] = s_sk_tid[i];
        skips[o] = s_sk_n[i];
        o++;
    }
    return o;
}

void w32_seek_note_skip(void)
{
    unsigned long me = GetCurrentThreadId();
    int i;
    for (i = 0; i < W32_SEEK_TIDS; i++) {
        if (s_sk_tid[i] == me) {
            InterlockedIncrement(&s_sk_n[i]);
            return;
        }
        if (s_sk_tid[i] == 0) {
            s_sk_tid[i] = me; /* Erstschreiber gewinnt (Tid eindeutig) */
            InterlockedIncrement(&s_sk_n[i]);
            return;
        }
    }
}

void krnl_dump_stats(void)
{
    unsigned o;
    HLOG_I("KRNL MmContig: %u Allocs (Reg %u Slots)", g_exreg_n,
           8192);
    { extern void krnl_contig_stats(void); krnl_contig_stats(); } /* W177 contig_noalias */
    HLOG_I("KRNL SUSPSAFE: %lu suspends, %lu retries, %lu giveups (safe_suspend=%d)",
           susp_n_suspend, susp_n_retry, susp_n_giveup, g_cfg.safe_suspend);
    HLOG_I("KRNL LOADLOCK: Modell %d, inject=%d, injiziert=%ld, sync=%d, rueckzuege=%ld",
           g_cfg.diag_lock_model, g_cfg.diag_lock_inject,
           (long)g_hlock_injected, g_cfg.safe_suspend_sync,
           (long)g_hlock_backoff); /* W119 */
    { extern void krnl_lock_stats(void); krnl_lock_stats(); }
    HLOG_I("KRNL setinfo: %ld Aufrufe (%ld redundant entfallen, sample xh=0x%X cls=%u off=%llu caller=0x%X tid=0x%lX '%s')",
           s_si_n, s_si_skip, s_si_xh, s_si_cls, s_si_off, s_si_caller,
           s_si_tid, s_si_path);
    { /* W32: pro-Tid Skip-Verteilung (Sleep(1)-Opfer identifizieren) */
        int i;
        for (i = 0; i < W32_SEEK_TIDS && s_sk_tid[i]; i++)
            HLOG_I("KRNL setinfo-skip: tid=0x%lX skips=%ld",
                   s_sk_tid[i], s_sk_n[i]);
    }
    HLOG_I("KRNL unimplementierte Aufrufe:");
    for (o = 0; o < 512; o++) {
        if (g_unimpl_count[o]) {
            HLOG_I("  #%u %s: %u x", o, krnl_name(o), g_unimpl_count[o]);
        }
    }
    if (g_cfg.hle_arity_check) { /* W162 ARITY: Endstand = echte Spiel-Aufrufe */
        HLOG_I("KRNL arity: Spiel-Aufrufe ord87=%ld ord203=%ld (Selbsttest rausgerechnet)",
               (long)g_arity87_calls, (long)g_arity203_calls);
    }
}

/* ---------- Generierte Stubs (exaktes Stack-Cleanup) ---------- */
#define STUB0(o) static u32 __stdcall stub_##o(void) { return krnl_unimpl(o); }
#define STUB1(o) static u32 __stdcall stub_##o(u32 a0) { (void)a0; return krnl_unimpl(o); }
#define STUB2(o) static u32 __stdcall stub_##o(u32 a0, u32 a1) { (void)a0; (void)a1; return krnl_unimpl(o); }
#define STUB3(o) static u32 __stdcall stub_##o(u32 a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; return krnl_unimpl(o); }
#define STUB4(o) static u32 __stdcall stub_##o(u32 a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; return krnl_unimpl(o); }
#define STUB5(o) static u32 __stdcall stub_##o(u32 a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; return krnl_unimpl(o); }
#define STUB6(o) static u32 __stdcall stub_##o(u32 a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return krnl_unimpl(o); }
#define STUB7(o) static u32 __stdcall stub_##o(u32 a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; return krnl_unimpl(o); }
#define STUB8(o) static u32 __stdcall stub_##o(u32 a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; return krnl_unimpl(o); }
#define STUB9(o) static u32 __stdcall stub_##o(u32 a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; return krnl_unimpl(o); }
#define STUB10(o) static u32 __stdcall stub_##o(u32 a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8, u32 a9) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; return krnl_unimpl(o); }
#define STUBC(o) static u32 __cdecl stubc_##o(void) { return krnl_unimpl(o); }

#define PASTE2(a, b) a##b
#define GEN_STUB(o, a, c) PASTE2(GEN_STUB_, c)(o, a)
#define GEN_STUB_0(o, a) PASTE2(STUB, a)(o)
#define GEN_STUB_1(o, a) STUBC(o)

#define K(o, n, v, a, c) GEN_STUB(o, a, c)
#include "ordinal_table.inc"
#undef K
#undef GEN_STUB
#undef GEN_STUB_0
#undef GEN_STUB_1
#undef PASTE2

/* ---------- Dispatch-Metadaten ---------- */
typedef struct {
    const char *name;
    u32 is_var;
    u32 argc;
    void *stub;
    void *impl; /* gesetzt von krnl_init, sonst NULL (= Stub) */
} krnl_meta_t;

static krnl_meta_t g_meta[512];

#define K_STUB_0(o) stub_##o
#define K_STUB_1(o) stubc_##o
#define K_PICK(c, o) K_STUB_##c(o)

typedef struct {
    u32 ord;
    const char *name;
    u32 is_var;
    u32 argc;
    void *stub;
} krnl_row_t;

#define K(o, n, v, a, c) { o, #n, v, a, (void *)K_PICK(c, o) },
static const krnl_row_t g_rows[] = {
#include "ordinal_table.inc"
};
#undef K

static void krnl_build_meta(void)
{
    size_t i;
    for (i = 0; i < sizeof g_meta / sizeof g_meta[0]; i++) {
        g_meta[i].name = "?";
        g_meta[i].stub = (void *)stub_0;
    }
    for (i = 0; i < sizeof g_rows / sizeof g_rows[0]; i++) {
        g_meta[g_rows[i].ord].name = g_rows[i].name;
        g_meta[g_rows[i].ord].is_var = g_rows[i].is_var;
        g_meta[g_rows[i].ord].argc = g_rows[i].argc;
        g_meta[g_rows[i].ord].stub = g_rows[i].stub;
    }
}

const char *krnl_name(unsigned ordinal)
{
    if (ordinal < 512 && g_meta[ordinal].name) {
        return g_meta[ordinal].name;
    }
    return "?";
}

/* ---------- Variablen-Scratch ---------- */
/* XboxHardwareInfo (Ord 322, Cxbx types.h): Flags + Gpu/Mcp-Revision.
 * McpRevision=0xA1: XAPILIB-USB-Code (FUN_00255F85) ueberspringt damit den
 * OHCI-Remap auf 0xFED00000 (echtes HW-Register, im HLE-Harness sinnlos;
 * Input kommt spaeter aus SDL, nicht aus OHCI). */
static struct {
    uint32_t flags;
    uint8_t gpu_rev, mcp_rev, unk3, unk4;
} g_xbox_hwinfo = { 0, 0xA1, 0xA1, 0, 0 };

static uint8_t g_varscratch[512][512]; /* 256 KB; groesste Var: 256 B (+Reserve) */
static uint8_t g_launch_data_page[4096];
static uint32_t g_hal_boot_smc_video_mode;
static uint32_t g_ke_tick_count;
static uint8_t g_saved_data[65536];

static void *krnl_var_ptr(unsigned ordinal)
{
    switch (ordinal) {
    case 156:
        return &g_ke_tick_count; /* live! */
    case 164:
        return g_launch_data_page;
    case 356:
        return &g_hal_boot_smc_video_mode;
    case 322:
        return &g_xbox_hwinfo;
    default:
        return g_varscratch[ordinal & 511];
    }
}

/* W188 GAME-MENU: Pause (sb_pause.c) - KeTickCount steht waehrend der Pause
 * und laeuft danach um die Pausendauer versetzt weiter (0 = bisher). */
static volatile DWORD g_ke_tick_off;
static volatile LONG g_ke_tick_hold;
static DWORD g_ke_pause_t0;
uint64_t g_kit_off;          /* krnl_sync.c: KeQueryInterruptTime-Versatz (QPC-Einheiten) */
volatile LONG g_kit_hold;    /* 1 = Pause: KeQueryInterruptTime steht */
LARGE_INTEGER g_kit_q0;     /* Pausenbeginn (QPC) */

void krnl_time_pause(int begin)
{
    if (begin) {
        g_ke_pause_t0 = GetTickCount();
        QueryPerformanceCounter(&g_kit_q0);
        InterlockedExchange(&g_ke_tick_hold, 1);
        InterlockedExchange(&g_kit_hold, 1);
    } else {
        LARGE_INTEGER q1;
        QueryPerformanceCounter(&q1);
        g_kit_off += (uint64_t)(q1.QuadPart - g_kit_q0.QuadPart);
        g_ke_tick_off += GetTickCount() - g_ke_pause_t0;
        InterlockedExchange(&g_kit_hold, 0);
        InterlockedExchange(&g_ke_tick_hold, 0);
    }
}

static DWORD WINAPI tick_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        if (!g_ke_tick_hold) { g_ke_tick_count = GetTickCount() - g_ke_tick_off; }
        Sleep(5);
    }
    /* unreachable */
}

/* ================= Implementierungen ================= */

/* ----- Strings ----- */
static u32 __stdcall hle_RtlInitAnsiString(u32 dst, u32 src)
{
    xb_ansi_string *d = (xb_ansi_string *)(uintptr_t)dst;
    const char *s = (const char *)(uintptr_t)src;
    size_t n = s ? strlen(s) : 0;
    d->length = (xb_u16)n;
    d->max_length = (xb_u16)(n + 1);
    d->buffer = (char *)s;
    return 0;
}

static u32 __stdcall hle_RtlAnsiStringToUnicodeString(u32 dst, u32 src, u32 alloc)
{
    xb_unicode_string *d = (xb_unicode_string *)(uintptr_t)dst;
    xb_ansi_string *s = (xb_ansi_string *)(uintptr_t)src;
    unsigned i, n = s->length;
    if (alloc) {
        wchar_t *b = malloc((n + 1) * sizeof(wchar_t));
        if (!b) {
            return X_STATUS_NO_MEMORY;
        }
        d->buffer = b;
        d->max_length = (xb_u16)((n + 1) * 2);
    }
    for (i = 0; i < n; i++) {
        d->buffer[i] = (wchar_t)(unsigned char)s->buffer[i];
    }
    if (d->max_length > n * 2) {
        d->buffer[n] = 0;
    }
    d->length = (xb_u16)(n * 2);
    return X_STATUS_SUCCESS;
}

static u32 __stdcall hle_RtlUnicodeStringToAnsiString(u32 dst, u32 src, u32 alloc)
{
    xb_ansi_string *d = (xb_ansi_string *)(uintptr_t)dst;
    xb_unicode_string *s = (xb_unicode_string *)(uintptr_t)src;
    unsigned i, n = s->length / 2;
    if (alloc) {
        char *b = malloc(n + 1);
        if (!b) {
            return X_STATUS_NO_MEMORY;
        }
        d->buffer = b;
        d->max_length = (xb_u16)(n + 1);
    }
    for (i = 0; i < n; i++) {
        wchar_t c = s->buffer[i];
        d->buffer[i] = (char)(c < 128 ? c : '?');
    }
    if (d->max_length > n) {
        d->buffer[n] = '\0';
    }
    d->length = (xb_u16)n;
    return X_STATUS_SUCCESS;
}

static u32 __stdcall hle_RtlEqualString(u32 s1, u32 s2, u32 case_ins)
{
    xb_ansi_string *a = (xb_ansi_string *)(uintptr_t)s1;
    xb_ansi_string *b = (xb_ansi_string *)(uintptr_t)s2;
    unsigned i;
    if (a->length != b->length) {
        return 0;
    }
    for (i = 0; i < a->length; i++) {
        char ca = a->buffer[i], cb = b->buffer[i];
        if (case_ins) {
            if (ca >= 'A' && ca <= 'Z') {
                ca += 32;
            }
            if (cb >= 'A' && cb <= 'Z') {
                cb += 32;
            }
        }
        if (ca != cb) {
            return 0;
        }
    }
    return 1;
}

static u32 __stdcall hle_RtlCompareMemoryUlong(u32 buf, u32 len, u32 val)
{
    u32 n = 0;
    uint8_t *p = (uint8_t *)(uintptr_t)buf;
    while (n + 4 <= len && *(u32 *)(p + n) == val) {
        n += 4;
    }
    return n;
}

static u32 __stdcall hle_RtlNtStatusToDosError(u32 st)
{
    switch (st) {
    case X_STATUS_SUCCESS: return 0;
    case X_STATUS_NO_MEMORY: return 8;
    case X_STATUS_INVALID_HANDLE: return 6;
    case X_STATUS_INVALID_PARAMETER: return 87;
    case X_STATUS_ACCESS_DENIED: return 5;
    case X_STATUS_OBJECT_NAME_NOT_FOUND: return 2;
    case X_STATUS_OBJECT_NAME_COLLISION: return 183;
    case X_STATUS_END_OF_FILE: return 38;
    case X_STATUS_NO_MORE_FILES: return 18;
    default: return 31;
    }
}

/* TIME_FIELDS: {Jahr, Monat, Tag, Stunde, Minute, Sekunde, ms, Wochentag} u16 */
static u32 __stdcall hle_RtlTimeToTimeFields(u32 pTime, u32 pFields)
{
    FILETIME ft;
    SYSTEMTIME st;
    uint64_t t = *(uint64_t *)(uintptr_t)pTime;
    ft.dwLowDateTime = (DWORD)t;
    ft.dwHighDateTime = (DWORD)(t >> 32);
    if (!FileTimeToSystemTime(&ft, &st)) {
        return X_STATUS_INVALID_PARAMETER;
    }
    {
        xb_u16 *f = (xb_u16 *)(uintptr_t)pFields;
        f[0] = st.wYear;
        f[1] = st.wMonth;
        f[2] = st.wDay;
        f[3] = st.wHour;
        f[4] = st.wMinute;
        f[5] = st.wSecond;
        f[6] = st.wMilliseconds;
        f[7] = st.wDayOfWeek;
    }
    return X_STATUS_SUCCESS;
}

static u32 __stdcall hle_RtlTimeFieldsToTime(u32 pFields, u32 pTime)
{
    SYSTEMTIME st;
    FILETIME ft;
    xb_u16 *f = (xb_u16 *)(uintptr_t)pFields;
    st.wYear = f[0];
    st.wMonth = f[1];
    st.wDay = f[2];
    st.wHour = f[3];
    st.wMinute = f[4];
    st.wSecond = f[5];
    st.wMilliseconds = f[6];
    st.wDayOfWeek = 0;
    if (!SystemTimeToFileTime(&st, &ft)) {
        return 0;
    }
    *(uint64_t *)(uintptr_t)pTime =
        ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return 1;
}

/* RtlUnwind kommt aus winnt.h (x86-ABI-identisch zum Xbox-Kernel) */
static u32 __stdcall hle_RtlUnwind(u32 frame, u32 ip, u32 rec, u32 retval)
{
    HLOG_W("RtlUnwind(frame=0x%X ip=0x%X) -> Host", frame, ip);
    RtlUnwind((PVOID)(uintptr_t)frame, (PVOID)(uintptr_t)ip,
              (PVOID)(uintptr_t)rec, (PVOID)(uintptr_t)retval);
    return 0; /* unreachable */
}

static u32 __stdcall hle_RtlRaiseException(u32 rec)
{
    u32 code = 0xC0000005u;
    if (rec) {
        code = *(u32 *)(uintptr_t)rec; /* ExceptionCode @ +0 */
    }
    HLOG_W("RtlRaiseException(code=0x%X) -> Host", code);
    RaiseException(code, 0, 0, NULL);
    return 0; /* unreachable */
}

/* Xbox-DEVICE_OBJECT (Cxbx types.h): +0x18 = DeviceExtension (anders als NT,
 * das dort den Timer hat). Header 0x50 B, Extension folgt direkt. */
#define XB_DEVOBJ_SIZE 0x50u
#define XB_DEVOBJ_OFF_TYPE 0x00u
#define XB_DEVOBJ_OFF_SIZE 0x02u
#define XB_DEVOBJ_OFF_REFCOUNT 0x04u
#define XB_DEVOBJ_OFF_DRIVER 0x08u
#define XB_DEVOBJ_OFF_MOUNTED 0x0Cu
#define XB_DEVOBJ_OFF_FLAGS 0x14u
#define XB_DEVOBJ_OFF_EXT 0x18u
#define XB_DEVOBJ_OFF_DEVTYPE 0x1Cu
#define XB_DEVOBJ_OFF_STACKSIZE 0x1Eu

static u32 __stdcall hle_IoCreateDevice(u32 drvObj, u32 extSize, u32 devName,
                                        u32 devType, u32 exclusive,
                                        u32 ppDevObj)
{
    uint8_t *obj;
    u32 total;
    (void)devName;
    (void)exclusive;
    total = (XB_DEVOBJ_SIZE + extSize + 7) & ~7u;
    obj = calloc(1, total);
    if (!obj) {
        return X_STATUS_NO_MEMORY;
    }
    *(uint16_t *)(obj + XB_DEVOBJ_OFF_TYPE) = 3; /* IO_TYPE_DEVICE */
    *(uint16_t *)(obj + XB_DEVOBJ_OFF_SIZE) =
        (uint16_t)(XB_DEVOBJ_SIZE + extSize);
    *(u32 *)(obj + XB_DEVOBJ_OFF_REFCOUNT) = 1;
    *(u32 *)(obj + XB_DEVOBJ_OFF_DRIVER) = drvObj;
    /* MountedOrSelfDevice: NULL fuer Platte/CD/MU/MediaBoard (Cxbx). */
    *(u32 *)(obj + XB_DEVOBJ_OFF_MOUNTED) =
        (devType == 0x02 || devType == 0x07 || devType == 0x3A ||
         devType == 0x3B)
            ? 0
            : (u32)(uintptr_t)obj;
    *(u32 *)(obj + XB_DEVOBJ_OFF_FLAGS) = 0;
    if (extSize) {
        u32 ext = (u32)(uintptr_t)(obj + XB_DEVOBJ_SIZE);
        *(u32 *)(obj + XB_DEVOBJ_OFF_EXT) = ext;
        *(u32 *)(uintptr_t)ext = (u32)(uintptr_t)obj; /* Rueckzeiger */
    }
    *(obj + XB_DEVOBJ_OFF_DEVTYPE) = (uint8_t)devType;
    *(obj + XB_DEVOBJ_OFF_STACKSIZE) = 1;
    *(u32 *)(uintptr_t)ppDevObj = (u32)(uintptr_t)obj;
    HLOG_T("IoCreateDevice(type=0x%X ext=0x%X) -> 0x%X", devType, extSize,
           (u32)(uintptr_t)obj);
    return X_STATUS_SUCCESS;
}

static u32 __stdcall hle_IoStartPacket(u32 a0, u32 a1, u32 a2)
{
    (void)a0;
    (void)a1;
    (void)a2;
    return X_STATUS_SUCCESS;
}

static u32 __stdcall hle_IoStartNextPacket(u32 a0)
{
    (void)a0;
    return X_STATUS_SUCCESS;
}

/* Ord 87 IofCompleteRequest ist FASTCALL (Irp in ECX, Boost in EDX, RET 0).
 * Eigene XBE-Analyse (xbe_disasm.py kcall 87 + dis): 10/10 Aufrufstellen ohne
 * Push und ohne Caller-Cleanup, z.B. @0x256BF3 (mov ecx,[ebp+0xc] ... xor
 * dl,dl ... call [slot]) und @0x20AE05 (lea ecx,[esi-0x3c] ... xor dl,dl).
 * Das fruehere __stdcall/2 (RET 8) haette ESP pro Aufruf um 8 gedriftet. */
static u32 __fastcall hle_IofCompleteRequest(u32 irp, u32 boost)
{
    if (g_cfg.hle_arity_check) { /* W162: Register-Ankunft belegen */
        InterlockedIncrement(&g_arity87_calls);
        g_arity87_a0 = irp;
        g_arity87_a1 = boost;
    }
    return 0;
}

static u32 __stdcall hle_IoMarkIrpMustComplete(u32 a0)
{
    (void)a0;
    return 0;
}

static u32 __stdcall hle_IoInvalidDeviceRequest(u32 a0, u32 a1)
{
    (void)a0;
    (void)a1;
    return X_STATUS_INVALID_DEVICE_REQUEST;
}

static u32 __stdcall hle_ObReferenceObjectByHandle(u32 xh, u32 a1, u32 a2)
{
    (void)a1;
    (void)a2;
    return ht_get(xh) ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE;
}

/* Ord 250 ObfDereferenceObject ist FASTCALL (Object in ECX, RET 0, void).
 * Vgl. Cxbx EmuKrnlOb.cpp: "void_xt FASTCALL xbox::ObfDereferenceObject". */
static void __fastcall hle_ObfDereferenceObject(u32 obj)
{
    HLOG_T("ObfDereferenceObject(obj=0x%X)", obj);
}

static u32 __stdcall hle_ExQueryNonVolatileSetting(u32 a0, u32 a1, u32 a2, u32 a3,
                                                   u32 a4)
{
    /* W234 PAL50 (Befund W212): XAPI 0x191164 liest Byte 1 von Einstellung
     * 0x103 (Videostandard), 0x19118D Bits 16..22 von Einstellung 8
     * (Videoflags). PAL (3) ohne PAL60-Flag -> Spiel waehlt 50 Hz und die
     * PAL50-Musik (STRM10..18). Nur mit game_tick=25, sonst unveraendert. */
    if (g_cfg.game_tick == 25 && (a0 == 0x103u || a0 == 8u) && a2 && a3 >= 4u) {
        if (a1) { *(u32 *)(uintptr_t)a1 = 4u; }
        *(u32 *)(uintptr_t)a2 = a0 == 0x103u ? 0x300u : 0u;
        if (a4) { *(u32 *)(uintptr_t)a4 = 4u; }
        HLOG_I("ExQueryNonVolatileSetting(idx=0x%X) -> PAL50 (game_tick=25)", a0);
        return X_STATUS_SUCCESS;
    }
    HLOG_T("ExQueryNonVolatileSetting(idx=0x%X) -> NOT_FOUND", a0);
    (void)a1; (void)a2; (void)a3; (void)a4;
    return X_STATUS_OBJECT_NAME_NOT_FOUND; /* -> XDK nutzt Defaults */
}

/* ----- Ke ----- */
static u32 __stdcall hle_KeRaiseIrqlToDpcLevel(void)
{
    return 2; /* DISPATCH_LEVEL; symmetrisch zu Lower */
}

/* Xbox/NT: KfRaiseIrql/KfLowerIrql sind __fastcall (CL=Irql, AL=alt).
 * __stdcall wuerde hier den Caller-Stack driften (RET 4 ohne PUSH)! */
static u32 __fastcall hle_KfRaiseIrql(u32 newIrql)
{
    (void)newIrql;
    return 0;
}

static u32 __fastcall hle_KfLowerIrql(u32 old)
{
    (void)old;
    return 0;
}

static u32 __stdcall hle_KeInitializeTimerEx(u32 a0, u32 a1)
{
    (void)a0;
    (void)a1;
    return 0;
}

static u32 __stdcall hle_KeInitializeDpc(u32 a0, u32 a1, u32 a2)
{
    (void)a0;
    (void)a1;
    (void)a2;
    return 0;
}

static u32 __stdcall hle_KeInitializeInterrupt(u32 a0, u32 a1, u32 a2, u32 a3, u32 a4,
                                               u32 a5, u32 a6)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return 0;
}

static u32 __stdcall hle_KeSetTimer(u32 timer, u32 dueLo, u32 dueHi, u32 dpc)
{
    (void)timer;
    (void)dueLo;
    (void)dueHi;
    (void)dpc;
    HLOG_T("KeSetTimer -> 0 (M1: Timer feuern nicht)");
    return 0;
}

static u32 __stdcall hle_KeCancelTimer(u32 a0)
{
    (void)a0;
    return 0;
}

static u32 __stdcall hle_KeRemoveQueueDpc(u32 a0)
{
    (void)a0;
    return 0;
}

static u32 __stdcall hle_KeInsertQueueDpc(u32 a0, u32 a1, u32 a2)
{
    (void)a0;
    (void)a1;
    (void)a2;
    return 0;
}

static u32 __stdcall hle_KeConnectInterrupt(u32 a0)
{
    (void)a0;
    return X_STATUS_SUCCESS;
}

static u32 __stdcall hle_KeDisconnectInterrupt(u32 a0)
{
    (void)a0;
    return 0;
}

static u32 __stdcall hle_KeSynchronizeExecution(u32 intr, u32 routine, u32 ctx)
{
    typedef u32 (__stdcall *sync_fn)(u32);
    (void)intr;
    if (!routine) {
        return 0;
    }
    return ((sync_fn)(uintptr_t)routine)(ctx);
}

static u32 __stdcall hle_KeStallExecutionProcessor(u32 us)
{
    uint64_t freq, start, now;
    if (us > 100000) {
        HLOG_W("KeStallExecutionProcessor(%u us) -> Sleep", us);
        Sleep(us / 1000);
        return 0;
    }
    QueryPerformanceFrequency((LARGE_INTEGER *)&freq);
    QueryPerformanceCounter((LARGE_INTEGER *)&start);
    do {
        QueryPerformanceCounter((LARGE_INTEGER *)&now);
    } while ((now - start) * 1000000ull / freq < us);
    return 0;
}

static u32 __stdcall hle_KeSaveFloatingPointState(u32 a0)
{
    (void)a0;
    return X_STATUS_SUCCESS;
}

static u32 __stdcall hle_KeRestoreFloatingPointState(u32 a0)
{
    (void)a0;
    return X_STATUS_SUCCESS;
}

static u32 __stdcall hle_KeBugCheck(u32 code)
{
    void *frames[10];
    USHORT n = RtlCaptureStackBackTrace(0, 10, frames, NULL);
    HLOG_E("KeBugCheck(0x%X) - Xbox-Panik! stack:", code);
    for (USHORT i = 0; i < n; i++) {
        HLOG_E("  BC[%u] %p", i, frames[i]);
    }
    krnl_dump_stats();
    ExitProcess(3);
    return 0; /* unreachable */
}

/* ----- Hal ----- */
static u32 __stdcall hle_HalGetInterruptVector(u32 a0, u32 a1)
{
    (void)a0;
    (void)a1;
    HLOG_T("HalGetInterruptVector -> 0 (M1: keine HW-Interrupts)");
    return 0;
}

static u32 __stdcall hle_HalRegisterShutdownNotification(u32 a0, u32 a1)
{
    (void)a0;
    (void)a1;
    return X_STATUS_SUCCESS;
}

static u32 __stdcall hle_HalReadWritePCISpace(u32 a0, u32 a1, u32 a2, u32 a3, u32 a4,
                                              u32 a5)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    HLOG_T("HalReadWritePCISpace -> 0 (M1: kein PCI)");
    return 0;
}

static u32 __stdcall hle_HalIsResetOrShutdownPending(void)
{
    return 0;
}

static u32 __stdcall hle_HalInitiateShutdown(void)
{
    HLOG_I("HalInitiateShutdown -> Exit(0)");
    ExitProcess(0);
    return 0; /* unreachable */
}

static u32 __stdcall hle_HalReturnToFirmware(u32 a0)
{
    void *frames[16];
    int n, i;
    (void)a0;
    n = RtlCaptureStackBackTrace(0, 16, frames, NULL);
    HLOG_W("HalReturnToFirmware(%u) - Aufrufkette:", a0);
    for (i = 0; i < n; i++) {
        u32 v = (u32)(uintptr_t)frames[i];
        HLOG_W("  #%d %c0x%08X", i, (v >= 0x10000 && v < 0xB00000) ? '*' : ' ', v);
    }
    /* (Reboot-Pfad steht fest: XapiInitProcess-Epilog, Pfad A war Heap-Commit.
     * Kein EBP-Walk: Inline-ASM + FPO = unzuverlaessig.) */
    HLOG_I("HalReturnToFirmware -> Exit(0) (M1: Reboot = Exit)");
    krnl_dump_stats();
    ExitProcess(0);
    return 0; /* unreachable */
}

/* ----- AV ----- */
static u32 __stdcall hle_AvGetSavedDataAddress(void)
{
    return (u32)(uintptr_t)g_saved_data;
}

static u32 __stdcall hle_AvSetSavedDataAddress(u32 va)
{
    HLOG_I("AvSetSavedDataAddress(0x%X)", va);
    return 0;
}

static u32 __stdcall hle_AvSendTVEncoderOption(u32 a0, u32 a1, u32 a2, u32 a3)
{
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    return X_STATUS_SUCCESS;
}

static u32 __stdcall hle_AvSetDisplayMode(u32 a0, u32 a1, u32 a2, u32 a3, u32 a4,
                                          u32 a5)
{
    HLOG_I("AvSetDisplayMode(%u,%u,%u,%u,%u,%u) (M1: nur Log)", a0, a1, a2, a3, a4,
           a5);
    return X_STATUS_SUCCESS;
}

/* ----- Xe (Sections laden/entladen) ----- */
static u32 __stdcall hle_XeLoadSection(u32 pSec)
{
    xb_section_header *s = (xb_section_header *)(uintptr_t)pSec;
    u32 i;
    if (!pSec) {
        return X_STATUS_INVALID_PARAMETER;
    }
    for (i = 0; i < g_img->n_sections; i++) {
        if (g_img->sections[i].vaddr == (u32)(uintptr_t)s->vaddr) {
            break;
        }
    }
    if (i >= g_img->n_sections) {
        HLOG_W("XeLoadSection: unbekannte Section VA=0x%X",
               (u32)(uintptr_t)s->vaddr);
        return X_STATUS_INVALID_PARAMETER;
    }
    if (s->refcount == 0 && g_img->file) {
        /* Bytes (erneut) bereitstellen (Loader-Region: protect + copy) */
        section_info_t *si = &g_img->sections[i];
        void *mem = (void *)(uintptr_t)si->vaddr;
        DWORD old;
        VirtualProtect(mem, si->vsize, PAGE_EXECUTE_READWRITE, &old);
        if (si->rawsize && si->rawaddr + si->rawsize <= g_img->file_size) {
            memcpy(mem, g_img->file + si->rawaddr, si->rawsize);
        }
        HLOG_I("XeLoadSection '%s' (0x%X, 0x%X Bytes) geladen", si->name, si->vaddr,
               si->rawsize);
    }
    s->refcount++;
    return X_STATUS_SUCCESS;
}

static u32 __stdcall hle_XeUnloadSection(u32 pSec)
{
    xb_section_header *s = (xb_section_header *)(uintptr_t)pSec;
    if (!pSec) {
        return X_STATUS_INVALID_PARAMETER;
    }
    if (s->refcount > 0) {
        s->refcount--;
    }
    HLOG_T("XeUnloadSection (ref=%u)", s->refcount);
    return X_STATUS_SUCCESS;
}

/* ----- Xc (SHA; M1: Stub - echte Hashes nur falls Boot sie braucht) ----- */
static u32 __stdcall hle_XcSHAInit(u32 ctx)
{
    if (ctx) {
        memset((void *)(uintptr_t)ctx, 0, 96);
    }
    HLOG_T("XcSHAInit (M1-Stub)");
    return 0;
}

static u32 __stdcall hle_XcSHAUpdate(u32 ctx, u32 data, u32 len)
{
    (void)ctx;
    (void)data;
    (void)len;
    return 0;
}

static u32 __stdcall hle_XcSHAFinal(u32 ctx, u32 digest)
{
    (void)ctx;
    if (digest) {
        memset((void *)(uintptr_t)digest, 0, 20);
    }
    HLOG_T("XcSHAFinal -> Null-Digest (M1-Stub)");
    return 0;
}

/* ----- Dbg ----- */
static u32 __cdecl hle_DbgPrint(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    if (!fmt) {
        return 0;
    }
    va_start(ap, fmt);
    vsnprintf_s(buf, sizeof buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    HLOG_I("[XB] %s", buf);
    return (u32)strlen(buf);
}

static u32 __stdcall hle_DbgBreakPoint(void)
{
    HLOG_W("DbgBreakPoint (ignoriert)");
    return 0;
}

/* ================= Init + Thunk-Installation ================= */
#define REG(o, fn) g_meta[o].impl = (void *)fn
static void krnl_register_all(void)
{
    /* Speicher */
    REG(184, hle_NtAllocateVirtualMemory);
    REG(199, hle_NtFreeVirtualMemory);
    REG(217, hle_NtQueryVirtualMemory);
    REG(165, hle_MmAllocateContiguousMemory);
    REG(166, hle_MmAllocateContiguousMemoryEx);
    REG(171, hle_MmFreeContiguousMemory);
    REG(167, hle_MmAllocateSystemMemory);
    REG(172, hle_MmFreeSystemMemory);
    REG(180, hle_MmQueryAllocationSize);
    REG(181, hle_MmQueryStatistics);
    REG(179, hle_MmQueryAddressProtect);
    REG(182, hle_MmSetAddressProtect);
    REG(178, hle_MmPersistContiguousMemory);
    REG(168, hle_MmClaimGpuInstanceMemory);
    REG(173, hle_MmGetPhysicalAddress);
    REG(175, hle_MmLockUnlockBufferPages);
    REG(176, hle_MmLockUnlockPhysicalPage);
    REG(14, hle_ExAllocatePool);
    REG(15, hle_ExAllocatePoolWithTag);
    REG(17, hle_ExFreePool);
    REG(23, hle_ExQueryPoolBlockSize);
    REG(24, hle_ExQueryNonVolatileSetting);
    /* Strings */
    REG(289, hle_RtlInitAnsiString);
    REG(260, hle_RtlAnsiStringToUnicodeString);
    REG(308, hle_RtlUnicodeStringToAnsiString);
    REG(279, hle_RtlEqualString);
    REG(269, hle_RtlCompareMemoryUlong);
    REG(301, hle_RtlNtStatusToDosError);
    REG(305, hle_RtlTimeToTimeFields);
    REG(304, hle_RtlTimeFieldsToTime);
    REG(312, hle_RtlUnwind);
    REG(302, hle_RtlRaiseException);
    REG(291, hle_RtlInitializeCriticalSection);
    REG(277, hle_RtlEnterCriticalSection);
    REG(294, hle_RtlLeaveCriticalSection);
    /* Sync */
    REG(189, hle_NtCreateEvent);
    REG(192, hle_NtCreateMutant);
    REG(225, hle_NtSetEvent);
    REG(186, hle_NtClearEvent);
    REG(221, hle_NtReleaseMutant);
    REG(233, hle_NtWaitForSingleObject);
    REG(234, hle_NtWaitForSingleObjectEx);
    REG(235, hle_NtWaitForMultipleObjectsEx);
    REG(231, hle_NtSuspendThread);
    REG(224, hle_NtResumeThread);
    /* Threads */
    REG(255, hle_PsCreateSystemThreadEx);
    REG(258, hle_PsTerminateSystemThread);
    REG(143, hle_KeSetBasePriorityThread);
    REG(99, hle_KeDelayExecutionThread);
    /* Zeit */
    REG(128, hle_KeQuerySystemTime);
    REG(125, hle_KeQueryInterruptTime);
    /* Dateien */
    REG(190, hle_NtCreateFile);
    REG(202, hle_NtOpenFile);
    REG(219, hle_NtReadFile);
    REG(236, hle_NtWriteFile);
    REG(187, hle_NtClose);
    REG(210, hle_NtQueryFullAttributesFile);
    REG(211, hle_NtQueryInformationFile);
    REG(226, hle_NtSetInformationFile_counted);
    REG(207, hle_NtQueryDirectoryFile);
    REG(198, hle_NtFlushBuffersFile);
    REG(218, hle_NtQueryVolumeInformationFile);
    REG(196, hle_NtDeviceIoControlFile);
    REG(200, hle_NtFsControlFile);
    /* Objekte/Io */
    REG(67, hle_IoCreateSymbolicLink);
    REG(203, hle_NtOpenSymbolicLinkObject);
    REG(215, hle_NtQuerySymbolicLinkObject);
    REG(65, hle_IoCreateDevice);
    REG(83, hle_IoStartPacket);
    REG(81, hle_IoStartNextPacket);
    REG(87, hle_IofCompleteRequest);
    REG(359, hle_IoMarkIrpMustComplete);
    REG(74, hle_IoInvalidDeviceRequest);
    REG(246, hle_ObReferenceObjectByHandle);
    REG(250, hle_ObfDereferenceObject);
    /* Ke */
    REG(129, hle_KeRaiseIrqlToDpcLevel);
    REG(160, hle_KfRaiseIrql);
    REG(161, hle_KfLowerIrql);
    REG(145, hle_KeSetEvent);
    REG(159, hle_KeWaitForSingleObject);
    REG(113, hle_KeInitializeTimerEx);
    REG(107, hle_KeInitializeDpc);
    REG(109, hle_KeInitializeInterrupt);
    REG(149, hle_KeSetTimer);
    REG(97, hle_KeCancelTimer);
    REG(137, hle_KeRemoveQueueDpc);
    REG(119, hle_KeInsertQueueDpc);
    REG(98, hle_KeConnectInterrupt);
    REG(100, hle_KeDisconnectInterrupt);
    REG(153, hle_KeSynchronizeExecution);
    REG(151, hle_KeStallExecutionProcessor);
    REG(142, hle_KeSaveFloatingPointState);
    REG(139, hle_KeRestoreFloatingPointState);
    REG(95, hle_KeBugCheck);
    /* Hal */
    REG(44, hle_HalGetInterruptVector);
    REG(47, hle_HalRegisterShutdownNotification);
    REG(46, hle_HalReadWritePCISpace);
    REG(358, hle_HalIsResetOrShutdownPending);
    REG(360, hle_HalInitiateShutdown);
    REG(49, hle_HalReturnToFirmware);
    /* AV */
    REG(1, hle_AvGetSavedDataAddress);
    REG(4, hle_AvSetSavedDataAddress);
    REG(2, hle_AvSendTVEncoderOption);
    REG(3, hle_AvSetDisplayMode);
    /* Xe */
    REG(327, hle_XeLoadSection);
    REG(328, hle_XeUnloadSection);
    /* Xc */
    REG(335, hle_XcSHAInit);
    REG(336, hle_XcSHAUpdate);
    REG(337, hle_XcSHAFinal);
    /* Dbg */
    REG(8, hle_DbgPrint);
    REG(5, hle_DbgBreakPoint);
}

void krnl_init(xbe_image_t *img)
{
    HANDLE th;
    g_img = img;
    /* Xbox = Uniprozessor: Haupt-Thread auf Core 0 (s. PsCreate). */
    SetThreadAffinityMask(GetCurrentThread(), 1);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                    GetCurrentProcess(), &g_main_thread_h, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    HLOG_I("KRNL init: meta...");
    krnl_build_meta();
    HLOG_I("KRNL init: register...");
    krnl_register_all();
    HLOG_I("KRNL init: handles...");
    ht_init_once();
    HLOG_I("KRNL init: nach handles");

    /* FS-Emulation + Xbox-TLS (Loader-Pflicht): Index-VA merken (Entry
     * errechnet daraus den signed Slot-Index, z.B. -5); Startwert 0. */
    HLOG_I("KRNL init: tls_index_addr=0x%X", img->tls_index_addr);
    fs_emu_init();
    fs_emu_set_tls_index_va(img->tls_index_addr);
    fs_emu_alloc_thread(); /* Haupt-Thread */
    if (img->tls_index_addr) {
        *(u32 *)(uintptr_t)img->tls_index_addr = 0;
        HLOG_I("KRNL: TLS-Index 0 (@0x%X)", img->tls_index_addr);
    }

    /* HDD-Struktur (Dashboard legt Titel-Dirs an; Fehler ignorieren) */
    {
        char d[640];
        xb_mkdir_p(g_cfg.hdd_root);
        _snprintf_s(d, sizeof d, _TRUNCATE, "%s/TDATA/%s", g_cfg.hdd_root,
                    XB_TITLE_ID_STR);
        xb_mkdir_p(d);
        _snprintf_s(d, sizeof d, _TRUNCATE, "%s/UDATA/%s", g_cfg.hdd_root,
                    XB_TITLE_ID_STR);
        xb_mkdir_p(d);
        /* Z: (Cache-Partition) existiert auf der Xbox immer; frischer
         * Datenordner (Community-Erststart) -> sonst FEHLER 3 auf ZCACHE */
        _snprintf_s(d, sizeof d, _TRUNCATE, "%s/ZCACHE", g_cfg.hdd_root);
        xb_mkdir_p(d);
        HLOG_I("KRNL: hdd_root='%s' fair_cs=%d", g_cfg.hdd_root, g_cfg.fair_cs);
        if (g_cfg.save_check) {
            krnl_save_check();
        }
        krnl_savelock_init(); /* W190 SAVE-LOCK (save_lock=0: nur Rest einer frueheren Sitzung verwerfen) */
    }

    /* KeTickCount-Heartbeat */
    th = krnl_CreateThread(tick_thread, NULL, 0, NULL);
    if (th) {
        CloseHandle(th);
    } else {
        HLOG_E("KRNL: Tick-Thread nicht erstellbar (GLE=%lu)", GetLastError());
    }
    HLOG_I("KRNL: bereit (109 impl, Rest = Logging-Stubs)");
}

/* W162 ARITY: Selbsttest durch die echten Thunk-Slots (nur hle_arity_check=1).
 * Ruft Ord 87 als fastcall (ecx/edx) und Ord 203 mit 2 Stack-Args exakt so,
 * wie das Spiel sie ruft, und prueft Ein-/Austritts-ESP (Drift = 0) sowie die
 * Argument-Ankunft. Danach Zaehler ruecksetzen (Endstand = echte Spiel-Aufrufe).
 * Nur Win32 (__asm); der Harness baut ausschliesslich Win32 (CMake-FATAL sonst). */
static void krnl_arity_selfcheck(void)
{
    u32 esp0 = 0, esp1 = 0, rv = 0;
    if (s_arity_slot87 && *s_arity_slot87) {
        u32 slot = *s_arity_slot87;
        __asm {
            mov esp0, esp
            mov ecx, 087A1B2C3h
            mov edx, 087D4E5F6h
            call dword ptr [slot]
            mov esp1, esp
            mov esp, esp0 /* Selbstschutz: eigenen Frame nie sprengen */
            mov rv, eax
        }
        HLOG_I("KRNL arity87: Selbsttest rv=0x%X aufrufe=%ld args=(0x%X,0x%X) espdrift=%d",
               rv, (long)g_arity87_calls, g_arity87_a0, g_arity87_a1,
               (int)(esp1 - esp0));
        g_arity87_calls = 0;
    } else {
        HLOG_W("KRNL arity87: kein Thunk-Slot (Selbsttest entfaellt)");
    }
    esp0 = esp1 = rv = 0;
    if (s_arity_slot203 && *s_arity_slot203) {
        u32 slot = *s_arity_slot203;
        __asm {
            mov esp0, esp
            push 0 /* pAttr = NULL -> HLE kehrt sofort zurueck */
            push 0 /* ph (wird nicht angefasst) */
            call dword ptr [slot]
            mov esp1, esp
            mov esp, esp0 /* Selbstschutz: eigenen Frame nie sprengen */
            mov rv, eax
        }
        HLOG_I("KRNL arity203: Selbsttest rv=0x%X (soll 0x%X) aufrufe=%ld espdrift=%d",
               rv, X_STATUS_OBJECT_NAME_INVALID, (long)g_arity203_calls,
               (int)(esp1 - esp0));
        g_arity203_calls = 0;
    } else {
        HLOG_W("KRNL arity203: kein Thunk-Slot (Selbsttest entfaellt)");
    }
}

void krnl_install_thunks(xbe_image_t *img)
{
    u32 *kt = (u32 *)(uintptr_t)img->kt_addr;
    unsigned i, n = 0;
    for (i = 0; i < 4096; i++) {
        u32 raw = kt[i];
        u32 ord;
        void *target;
        if (raw == 0) {
            break;
        }
        if (raw & 0x80000000u) {
            HLOG_T("KRNL: Thunk %u High-Bit maskiert (0x%X)", i, raw);
        }
        ord = raw & 0x7FFFFFFFu;
        if (ord == 87) { /* W162 ARITY: Slot-Adressen fuer den Selbsttest */
            s_arity_slot87 = &kt[i];
        } else if (ord == 203) {
            s_arity_slot203 = &kt[i];
        }
        if (ord >= 512) {
            HLOG_E("KRNL: Ordinal %u ausserhalb - ABBRUCH", ord);
            ExitProcess(9);
        }
        if (g_meta[ord].is_var) {
            target = krnl_var_ptr(ord);
        } else if (g_meta[ord].impl) {
            target = g_meta[ord].impl;
        } else {
            target = g_meta[ord].stub;
        }
        if (!g_meta[ord].is_var && sctrace_active()) {
            target = sctrace_wrap(ord, target);
        }
        kt[i] = (u32)(uintptr_t)target;
        n++;
    }
    HLOG_I("KRNL: %u Thunks installiert", n);
    /* Rueckverifikation (nur ohne Tracing: sonst stehen Trampoline drin) */
    if (!sctrace_active()) {
        u32 want = (u32)(uintptr_t)hle_PsCreateSystemThreadEx;
        u32 got = kt[47];
        HLOG_I("KRNL verify: slot47 want=0x%X got=0x%X %s", want, got,
               want == got ? "OK" : "MISMATCH!!");
    }
    /* Slot 57 (ExQueryNonVolatileSetting) ebenso pruefen (Boot-Diagnose) */
    if (!sctrace_active()) {
        u32 want = (u32)(uintptr_t)hle_ExQueryNonVolatileSetting;
        u32 got = kt[57];
        HLOG_I("KRNL verify: slot57 want=0x%X got=0x%X %s", want, got,
               want == got ? "OK" : "MISMATCH!!");
        HLOG_I("KRNL verify: g_meta[24].impl=0x%X is_var=%u",
               (u32)(uintptr_t)g_meta[24].impl, g_meta[24].is_var);
    }
    if (g_cfg.hle_arity_check) { /* W162 ARITY: Selbsttest durch echte Slots */
        krnl_arity_selfcheck();
    }
}
