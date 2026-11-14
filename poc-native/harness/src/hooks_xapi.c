/* hooks_xapi.c - XDK-Hooks: XAPI/Datei (Cache-Partition, XLaunch, Utility-
 * Drive, ReadFile, Aktivitaets-Zaehler, Null-Guards/No-Ops 106xxx-109xxx,
 * MapLetter, MediaCheck, XapiNotify). W68 SPLIT: 1:1 aus xdk_hooks.c. */
#include "hooks_int.h"
#include "sb_remaster.h" /* W103 REM-FRAME */
#include "sb_relaunch.h" /* W186 RESET-TITLE */

/* XapiSelectCachePartition (0x18CFC0, 3 Args, RET 0xC): waehlt Cache-
 * Partition (liest partition0-Tabelle, komplexe Logik). M1: Cache=1 +
 * ERFOLG (T:/U:-Symlinks existieren bereits, Tabelle irrelevant). */
u32 __stdcall hook_XapiSelectCachePartition(u32 a1, u32 a2, u32 pCache)
{
    (void)a1;
    (void)a2;
    if (pCache) {
        *(u32 *)(uintptr_t)pCache = 1;
    }
    HLOG_I("XAPI SelectCachePartition -> SUCCESS (Cache=1)");
    return 0; /* STATUS_SUCCESS */
}

/* XLaunchNewImageA (0x192363, 2 Args, RET 8): laedt neues XBE + rebootet.
 * Arg 2 ist ein PLAUNCH_DATA (bisher als "flags" geloggt; Zeile unveraendert).
 * Aufrufer (W186, Disasm): 0xE066D = BACK TO TITLE -> YES ("Reset the game and
 * return to the title screen?", Szenen 6/0x1F) mit "D:\default.xbe" und
 * genulltem LAUNCH_DATA; 0xE0271 = Szene-0-Dialog (Speicherplatz) mit NULL
 * = Dashboard (LD_LAUNCH_DASHBOARD, Grund 2 = Speicherverwaltung);
 * 0x192B1A = XapiBootToDash (XapiInitProcess-Fehlerpfade).
 * W186 RESET-TITLE: eigenes Image -> Client-Neustart (sb_relaunch.c,
 * xlaunch_self=1; 2 = sauberes Ende; 0 = alt). Alles andere wie bisher:
 * IGNORIEREN (kein Reboot, weiter mit aktuellem Image), loggen. */
static int xlaunch_is_self(const char *p)
{
    if (!p) {
        return 0;
    }
    if (_strnicmp(p, "D:", 2) == 0) {
        p += 2;
    } else if (_strnicmp(p, "\\Device\\CdRom0", 14) == 0) {
        p += 14;
    } else {
        return 0;
    }
    return _stricmp(p, "\\default.xbe") == 0;
}

u32 __stdcall hook_XLaunchNewImageA(u32 pPath, u32 flags)
{
    const char *path = pPath ? (const char *)(uintptr_t)pPath : "(null)";
    void *ret = _ReturnAddress(); /* Aufrufer (seltenes Echt-Ereignis) */
    if (flags) { /* W186: LAUNCH_DATA-Kopf (Typ/Grund + 3 Parameter) */
        const u32 *ld = (const u32 *)(uintptr_t)flags;
        HLOG_W("XLAUNCH LAUNCH_DATA: %08X %08X %08X %08X", ld[0], ld[1], ld[2], ld[3]);
    }
    if (pPath && xlaunch_is_self(path) && g_cfg.xlaunch_self) {
        HLOG_W("XLAUNCH XLaunchNewImageA('%s', flags=0x%X) <- 0x%p -> %s", path, flags, ret,
               g_cfg.xlaunch_self == 2 ? "ENDE" : "NEUSTART");
        sb_relaunch_exec(path); /* kehrt nur bei Fehlschlag zurueck */
    }
    HLOG_W("XLAUNCH XLaunchNewImageA('%s', flags=0x%X) <- 0x%p -> IGNORIERT",
           path, flags, ret);
    if (!pPath) {
        HLOG_W("XLAUNCH: Ziel Dashboard (kein Dashboard im Client) -> wie bisher ignoriert");
    }
    return 0; /* ERFOLG (kein Reboot, weiter!) */
}

/* XMountUtilityDrive (0x18D1EB, 1 Arg, RET 4): mountet Z: (Cache).
 * M1: TRUE (Z: -> hdd_root, beschreibbar, siehe xb_path_to_host). */
u32 __stdcall hook_XMountUtilityDrive(u32 fFormat)
{
    (void)fFormat;
    HLOG_I("XAPI MountUtilityDrive -> TRUE (Z: verfuegbar)");
    return 1; /* TRUE */
}

/* ReadFile (0x18D97F, 5 Args, RET 0x14): Xbox-ReadFile mit OVERLAPPED
 * (Status@0, Offset@8, Event@0x10). M1: synchron lesen; Return nach
 * Overlapped: ov==NULL (echt synchron, 0x13D00/BGM) -> TRUE; ov!=NULL
 * (Async-Handle: 0xC7C90/capcom UND 0xC7E10!) -> FALSE+997 (PENDING).
 * 0xC7E10 erwartet IMMER PENDING: sein TRUE-Pfad setzt [0x66A29C] nicht
 * und endet in der Sleep(10)-Endlosschleife (STATUS!=0x103-Wait). Die
 * alte len<0x1000-Heuristik war falsch (kleine ov-Reads -> TRUE -> Hang). */
/* Read-Hang-Detector: Args + Tick des laufenden Win32-Reads (Watchdog). */
static volatile LONG g_rd_active;
static volatile u32 g_rd_h, g_rd_len, g_rd_ov;
static volatile DWORD g_rd_tick;
/* Aktivitaets-Zaehler (Watchdog-Deltas: Drehmuster erkennen). */
u32 g_act_reads, g_act_xin;
static u32 g_act_rbytes; /* W32: gelesene Bytes (Watchdog-Delta) */
void hooks_activity(unsigned *reads, unsigned *xin)
{
    *reads = g_act_reads;
    *xin = g_act_xin;
}
unsigned hooks_activity_rbytes(void)
{
    return g_act_rbytes;
}

/* D3D-put-Diagnose: verbrauchte/gesamt Bytes des Command-Puffers. */
void hooks_d3d_put(unsigned *used, unsigned *total)
{
    u32 put = *(u32 *)(g_fake_d3d + 0x0);
    u32 lim = *(u32 *)(g_fake_d3d + 0x4);
    u32 base = (u32)(uintptr_t)g_d3d_pool;
    *used = (put >= base) ? (put - base) : 0;
    /* total = Limit-Offset (zeigt Ueberschreibung: != 8MB -> genullt!). */
    *total = (lim >= base) ? (lim - base) : 0xFFFFFFFFu;
}

int hooks_read_hang(unsigned *h, unsigned *len, unsigned *ov, unsigned *ms)
{
    if (!g_rd_active)
        return 0;
    *h = g_rd_h;
    *len = g_rd_len;
    *ov = g_rd_ov;
    *ms = GetTickCount() - g_rd_tick;
    return 1;
}

u32 __stdcall hook_ReadFile(u32 hFile, u32 buf, u32 len, u32 pRead,
                                   u32 ov)
{
    void *host = krnl_file_host(hFile);
    int vfs = !host && krnl_vfs_handle(hFile); /* W150 ISO-DIRECT: D: aus dem Abbild */
    DWORD got = 0;
    static u32 calls;
    g_act_reads++;
    if ((host || vfs) && buf && len) {
        g_rd_h = hFile;
        g_rd_len = len;
        g_rd_ov = ov;
        g_rd_tick = GetTickCount();
        g_rd_active = 1;
        if (vfs) {
            char rp[400];
            unsigned long g = 0;
            krnl_vfs_read(hFile, ov ? (const long long *)(uintptr_t)(ov + 8) : NULL,
                          (void *)(uintptr_t)buf, len, &g, rp, sizeof rp);
            got = g;
            g_act_rbytes += got; /* W32: nur Zaehler */
            if (g_cfg.tex_dump && got) { sb_rem_note_read(buf, got, rp); } /* DIAG(REM-FRAME,W103) */
        } else {
        if (ov) {
            LARGE_INTEGER li;
            li.QuadPart = *(int64_t *)(uintptr_t)(ov + 8);
            SetFilePointerEx(host, li, NULL, FILE_BEGIN);
        }
        ReadFile(host, (void *)(uintptr_t)buf, len, &got, NULL);
        g_act_rbytes += got; /* W32: nur Zaehler */
        if (g_cfg.tex_dump && got) { /* DIAG(REM-FRAME,W103): Textur-Quelle (Datei je Lese-Puffer) */
            char rp[400];
            if (GetFinalPathNameByHandleA((HANDLE)host, rp, sizeof rp, 0)) { sb_rem_note_read(buf, got, rp); }
        }
        }
        /* W22: Seek-Cache nachfuehren (ov-Seek laeuft unbedingt, auch got=0!) */
        if (ov)
            krnl_seek_note(hFile, (unsigned long long)*(int64_t *)(uintptr_t)(ov + 8) + got);
        else
            krnl_seek_add(hFile, got);
        g_rd_active = 0;
    }
    if (pRead) {
        *(u32 *)(uintptr_t)pRead = got;
    }
    if (ov) {
        *(u32 *)(uintptr_t)(ov + 0) = 0;    /* STATUS = ERFOLG */
        *(u32 *)(uintptr_t)(ov + 4) = got;  /* INFORMATION = Bytes */
        /* EVTFIX (boot506): hEvent signalisieren (BIN-Loader wartet drauf!). */
        { u32 hev = *(u32 *)(uintptr_t)(ov + 0x10);
          if (hev) { void *host_ev = krnl_event_host(hev);
              if (host_ev) SetEvent((HANDLE)host_ev); } }
    }
    if (hlog_throttle(&calls)) {
        HLOG_T("XAPI ReadFile(h=0x%X len=%u ov=0x%X) -> %lu",
               hFile, len, ov, got);
    }
    if (!ov) {
        xbox_set_last_error(0);
        if (hlog_throttle(&calls)) {
            HLOG_T("XAPI ReadFile TRUE-Pfad (ov=NULL)");
        }
        return 1; /* TRUE (nur echt-synchrone Reads, 0x13D00/BGM) */
    }
    xbox_set_last_error(997); /* ERROR_IO_PENDING (Overlapped = async) */
    return 0; /* FALSE (Pending-Tarnung, Flag=1-Pfad) */
}

/* FUN_00108720 (thiscall, ECX=this, RET): prueft [this+0x184/0x188],
 * gibt [ptr+0x64] zurueck (oder 0). Faultet bei NULL-ptr ([0+0x64]).
 * M1: gleiche Logik + NULL-Check (Caller kann mit 0 umgehen). */
u32 __fastcall hook_108720(u32 ecx_this)
{
    u32 base = ecx_this + 0x184;
    int i;
    static u32 calls;
    for (i = 0; i < 2; i++) {
        u32 ptr = *(u32 *)(uintptr_t)(base + (u32)i * 4);
        if (ptr) {
            u32 v = *(u32 *)(uintptr_t)(ptr + 0x64);
            if (v) {
                return v;
            }
        }
    }
    if (hlog_throttle(&calls)) {
        HLOG_T("108720(NULL-Check) -> 0 (kein Ptr)");
    }
    return 0;
}

/* FUN_00107E30 (thiscall, ECX=this, RET): Loop ueber 96 Slots
 * (this+4...+0x184), ruft 0x107CD0 pro Slot (crasht bei NULL-Slot:
 * [ECX+0x58] mit ECX=0). Slots bleiben NULL (kein echtes Audio).
 * M1: NULL-Slots mit Fake-Objekt fuellen (0x58!=2 -> harmlos), echte
 * Slots ans Original (0x107CD0) durchreichen. */
typedef u32 (__fastcall *fn107CD0_t)(u32 ecx_this);
u32 __fastcall hook_107E30(u32 ecx_this)
{
    static u32 calls;
    int i;
    u32 *slots;
    if (!ecx_this) {
        return 0;
    }
    slots = (u32 *)(uintptr_t)(ecx_this + 4);
    __try {
        for (i = 0; i < 0x60; i++) {
            if (!slots[i]) {
                slots[i] = (u32)(uintptr_t)g_fake_obj;
            } else {
                ((fn107CD0_t)(uintptr_t)SBA(0x107CD0u))(slots[i]);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (hlog_throttle(&calls)) {
        HLOG_T("107E30(Slot-Fake) this=0x%X", ecx_this);
    }
    return 0;
}

/* FUN_00109770 (thiscall+1, ECX=this, RET 4): 7er-Loop CALL 0x21F659
 * (Audio-Stimmen). Caller 0x108051 ruft mit NULL-this (Slots 0x60/0x61
 * leer, kein echtes Audio) -> [0+0x64]-Crash. M1: NULL -> 0 (Original
 * gibt bei [ECX+0x64]==0 ebenfalls 0 zurueck); non-NULL auch 0 + WARN
 * (bisher nicht beobachtet). */
u32 __fastcall hook_109770(u32 ecx_this, u32 edx_ign, u32 arg)
{
    static u32 calls, warned;
    (void)edx_ign;
    (void)arg;
    if (ecx_this && !warned) {
        warned = 1;
        HLOG_W("109770: non-NULL this=0x%X (unerwartet, -> 0)", ecx_this);
    }
    if (hlog_throttle(&calls)) {
        HLOG_T("109770(this=0x%X) -> 0", ecx_this);
    }
    return 0;
}

/* FUN_001086C0 (thiscall, ECX=this, RET): Loop ueber [this+0x184],
 * ruft 0x1072A0 mit NULL-ptr (Fault). Seiteneffekt nur NULLEN-schreiben.
 * M1: No-Op (0 zurueck). */
u32 __fastcall hook_1086C0(u32 ecx_this)
{
    static u32 calls;
    (void)ecx_this;
    if (hlog_throttle(&calls)) {
        HLOG_T("1086C0(No-Op) -> 0");
    }
    return 0;
}

/* FUN_00108740 (thiscall, ECX=this, RET): liest [this+0x184] ohne
 * NULL-Check (Fault). M1: No-Op (0 zurueck). */
u32 __fastcall hook_108740(u32 ecx_this)
{
    static u32 calls;
    (void)ecx_this;
    if (hlog_throttle(&calls)) {
        HLOG_T("108740(No-Op) -> 0");
    }
    return 0;
}

/* FUN_00106560 (thiscall, ECX=this, RET): COM/VTABLE-Init-Loop (10x),
 * faultet bei NULL-[this+4] (BSS, nie gesetzt). M1: No-Op. */
u32 __fastcall hook_106560(u32 ecx_this)
{
    static u32 calls;
    (void)ecx_this;
    if (hlog_throttle(&calls)) {
        HLOG_T("106560(No-Op) -> 0");
    }
    return 0;
}

/* XapiMapLetterToDirectory (0x19273F, 6 Args, RET 0x18): mappt
 * Laufwerksbuchstaben auf Verzeichnisse. Original loopt intern (53K+
 * NtCreateFile auf capcom.xpr, unergruendet). M1: ERFOLG ohne Aktion
 * (xb_path_to_host mappt statisch: D:->media, T:/U:/Z:->hdd). */
u32 __stdcall hook_XapiMapLetterToDirectory(u32 a1, u32 a2, u32 a3,
                                                  u32 a4, u32 a5, u32 a6)
{
    static u32 calls;
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    calls++;
    HLOG_W("MAPLETTER-HIT #%u (Hook betreten!)", calls);
    return 0; /* STATUS_SUCCESS */
}

/* FUN_00192B2D: XAPILIB Disc-/Media-Check (CdRom0 + Security-IOCTL 0x4D014).
 * M1-Bypass: Medium (XISO) liegt vor -> Erfolg. Ein echter IOCTL mit
 * Canned-Response kann das spaeter ersetzen. */
u32 __stdcall hook_XapiMediaCheck(void)
{
    HLOG_I("XAPI MediaCheck @0x192B2D -> 0 (Bypass: XISO-Medium gueltig)");
    return 0;
}

/* Wrapper um XapiCallThreadNotifyRoutines (ATTACH-Call in XapiThreadStartup):
 * Startup hat hier die TLS-Slots genullt -> XAPI-Block in den Slot des
 * Entry-Index (signed, z.B. -5) praefillen, damit spaetere Get/SetLastError
 * nicht auf Slot=0 laufen. */
u32 __stdcall hook_XapiNotifyWrap(u32 reason)
{
    typedef u32(__stdcall * orig_fn)(u32);
    fs_emu_prefill_xapi();
    return ((orig_fn)(uintptr_t)SBA(0x190BC6u))(reason);
}
