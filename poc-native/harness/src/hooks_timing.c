/* hooks_timing.c - XDK-Hooks: Takt (TURBO-Modus, VBlank/BlockUntilVertical-
 * Blank, DisplayFieldStatus, GPU-Ready-Waits, TSC-Frequenz).
 * W68 SPLIT: 1:1 aus xdk_hooks.c verschoben. */
#include "hooks_int.h"
#include "hlock.h"
#include "audio_out.h"
#include "sb_perfhud.h" /* W122 PERF-HUD */

/* TURBO bis Spielmodus (A1): 1 solange u32 an 0x2EEA34 (Spielmodus: 1 Logos,
 * 2 Titel, 3 Menues, 4 Missions-Load, memscan M4d) noch unter
 * turbo_until_mode liegt. 0 = Schalter aus. Leseschutz per SEH wie bei
 * mem:-Triggern (Adresse ist XBE-.data, ggf. frueh noch nicht gesetzt). */
int turbo_mode_active(void)
{
    static int mode_logged;
    u32 mode = 0;
    if (g_cfg.turbo_until_mode <= 0) {
        return 0;
    }
    __try {
        mode = *(volatile u32 *)(uintptr_t)0x2EEA34u;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mode = 0;
    }
    if ((int)mode < g_cfg.turbo_until_mode) {
        return 1;
    }
    if (!mode_logged) {
        mode_logged = 1;
        HLOG_I("TURBO: Modus %u erreicht bei Frame %u (Ziel %d)",
               mode, g_frames, g_cfg.turbo_until_mode);
    }
    return 0;
}

/* VBLANK (A4): Echte Taktung statt No-Op. Das Spiel taktet jeden Frame per
 * BlockUntilVerticalBlank (0x20E520) + GetDisplayFieldStatus-Zaehler (Xbox:
 * 60 Hz VBlank). Bisher kehrte beides sofort zurueck (Fake-Event ungueltig
 * -> KeWait-SUCCESS; Zaehler +3/Call) -> Frames liefen mit Render-Speed
 * (~107 FPS), framegebundene Logik (Missions-Countdown) zu schnell. Mit
 * vblank_hz>0 laeuft der Zaehler mit vblank_hz Ticks/s aus GetTickCount
 * (ms, absolut: kein Drift, keine Schulden nach langsamen Frames) und
 * BlockUntilVerticalBlank wartet auf den naechsten Tick.
 * 0 = altes Verhalten (schnell, falsch getaktet).
 * HINWEIS: Absichtlich KEINE QPC-Basis mit gespeicherter Startzeit: ein
 * Spiel-Wild-Write traf BSS bei Frame ~22k (VBDBG: tick 28213->16308,
 * W5_vb60-Hang bei Frame 22514). GetTickCount ist zustandslos; einziger
 * State g_vb_last heilt per Resync-Guard (s.u.) -> kein Hang moeglich. */
static u32 g_vb_last;
/* TEST(POPIN,W91) rt_from_mission: Versatz beim Wechsel Testtakt -> Echtzeit
 * (Feldzaehler bleibt monoton; 0 = bisher). */
static u32 g_vb_off;
static u32 g_fs_counter; /* letzter Feldzaehler (GetDisplayFieldStatus) */

static u32 vblank_tick_raw(void);
static u32 vblank_tick_now(void)
{
    return vblank_tick_raw() + g_vb_off;
}

/* TEST(POPIN,W91): ab jetzt Echtzeit mit hz VBlanks/s (vorher vblank_hz=0). */
void sb_timing_go_realtime(int hz)
{
    u32 now;
    if (hz <= 0 || g_cfg.vblank_hz > 0) { return; }
    g_cfg.vblank_hz = hz;
    now = vblank_tick_raw();
    g_vb_off = g_fs_counter + 4u - now;
    g_vb_last = g_fs_counter + 4u;
    HLOG_I("RTFROM: Echtzeit ab Frame %u (vblank_hz=%d, Feldzaehler %u -> %u)", g_frames, hz,
           g_fs_counter, vblank_tick_now());
}

/* W188 GAME-MENU: Pause (sb_pause.c). Waehrend der Pause steht der Spiel-
 * Thread; beim Fortsetzen ruecken alle Folge-Ticks um die Pausendauer nach
 * hinten (g_vb_off wie rt_from_mission) - der Feldzaehler laeuft ohne Sprung
 * weiter, die Missions-Warteschleife holt nichts nach. vblank_hz=0
 * (Testtakt): Zaehler haengt nicht an der Zeit, nichts zu tun. */
static u32 g_pause_raw0;
static int g_pause_rt;
void sb_timing_pause_begin(void)
{
    g_pause_rt = g_cfg.vblank_hz > 0;
    if (g_pause_rt) { g_pause_raw0 = vblank_tick_raw(); }
}

void sb_timing_pause_end(void)
{
    u32 d;
    if (!g_pause_rt || g_cfg.vblank_hz <= 0) { return; }
    d = vblank_tick_raw() - g_pause_raw0;
    g_vb_off -= d;
    HLOG_I("PAUSE: VBlank-Zaehler um %u Ticks verschoben (Feldzaehler %u, jetzt %u)", d, g_fs_counter,
           vblank_tick_now());
}

unsigned sb_timing_field(void)
{
    return g_fs_counter;
}

static u32 vblank_tick_raw(void)
{
    if (g_cfg.vblank_qpc) {
        /* W69 vblank_qpc: gleiche Zeitbasis-Idee, aber QPC statt GetTickCount
         * (dessen 15,6-ms-Stufen machen aus 2-VBlank-Fenstern 31,25/46,9 ms).
         * Ebenfalls zustandslos (Frequenz je Aufruf neu, keine Startzeit). */
        LARGE_INTEGER q, f;
        QueryPerformanceCounter(&q);
        QueryPerformanceFrequency(&f);
        return (u32)((uint64_t)q.QuadPart * (uint64_t)g_cfg.vblank_hz /
                     (uint64_t)(f.QuadPart ? f.QuadPart : 1));
    }
    /* ms seit Boot * hz / 1000. U32-Wrap alle 49 Tage: alle Vergleiche
     * modulo-sicher (nur Differenzen im Sekundenbereich). */
    return (u32)((uint64_t)GetTickCount() * (uint64_t)g_cfg.vblank_hz / 1000u);
}

/* W69 vblank_qpc: ms bis zum Beginn von Tick target (QPC-Zeitbasis). */
static DWORD vblank_qpc_ms_until(u32 target)
{
    LARGE_INTEGER q, f;
    uint64_t now_t;
    u32 cur;
    target -= g_vb_off; /* TEST(POPIN,W91): Rohzeitbasis (0 = bisher) */
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    if (!f.QuadPart) { return 0; }
    now_t = (uint64_t)q.QuadPart * (uint64_t)g_cfg.vblank_hz;
    cur = (u32)(now_t / (uint64_t)f.QuadPart);
    if ((int32_t)(target - cur) <= 0) { return 0; }
    /* Rest bis zur Tick-Grenze in ms (abgerundet). */
    return (DWORD)((((uint64_t)(target - cur) * (uint64_t)f.QuadPart -
                     now_t % (uint64_t)f.QuadPart) * 1000u) /
                   ((uint64_t)f.QuadPart * (uint64_t)g_cfg.vblank_hz));
}

/* D3DDevice_BlockUntilVerticalBlank (0x20E520, keine Args, void): wartet auf
 * dem VBlank-Event (KeWaitForSingleObject, s. Disasm). Event ist Fake ->
 * bisher sofort SUCCESS. Mit vblank_hz>0: warten bis zum naechsten Tick. */
u32 __stdcall hook_BlockUntilVerticalBlank(void)
{
    static u32 calls, lastlog;
    u32 target, tick;
    if (g_cfg.vblank_hz <= 0) {
        return 0;
    }
    if (g_phud_meas) { sb_phud_wait_enter(); } /* W122 PERF-HUD: Warten */
    tick = vblank_tick_now();
    target = g_vb_last + 1;
    if ((int32_t)(target - tick) > 1) {
        /* last aus der Zukunft (korrumpiert/Race): resync statt Hang. */
        static u32 nrs;
        if (nrs < 8) {
            HLOG_W("VBLANK resync #%u: last=%u now=%u", nrs, g_vb_last, tick);
        }
        nrs++;
        g_vb_last = tick;
        target = tick + 1;
    }
    for (;;) {
        DWORD now_ms, end_ms, wait;
        tick = vblank_tick_now();
        if ((int32_t)(tick - target) >= 0) {
            break;
        }
        /* Restzeit bis target: grob schlafen, Rest yield-spinnen. */
        now_ms = GetTickCount();
        end_ms = (DWORD)((uint64_t)(u32)(target - g_vb_off) * 1000u / (uint64_t)g_cfg.vblank_hz);
        wait = end_ms - now_ms; /* modulo-sicher */
        if (g_cfg.vblank_qpc) {
            wait = vblank_qpc_ms_until(target);
        }
        if (g_cfg.interp_fps > 0 && sb_interp_pending()) {
            /* FPS-INTERP (W69): Zwischenbilder im VBlank-Warten zeigen. */
            sb_interp_idle((int32_t)wait > 0 ? wait : 0u);
            continue;
        }
        if ((int32_t)wait > 1) {
            Sleep(wait - 1);
        } else {
            Sleep(0);
        }
    }
    g_vb_last = tick;
    if (g_phud_meas) { sb_phud_wait_leave(); } /* W122 PERF-HUD: Warten zu Ende */
    /* DIAG(VBLANK): 1 Zeile pro 600 Ticks -> scripts/vblank_rate.py belegt
     * die reale Tick-Rate aus dem Boot-Log. */
    if (tick - lastlog >= 600) {
        lastlog = tick;
        HLOG_I("VBLANK tick=%u", tick);
    }
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D BlockUntilVerticalBlank -> tick=%u", tick);
    }
    return 0;
}

/* D3DDevice_GetDisplayFieldStatus (0x20EB80, 1 Arg, RET 0x4): Poll in
 * FUN_00045F60 wartet auf wachsenden Status ([ARG+4]). M1: steigender
 * Zaehler (+3 pro Aufruf, schneller als Poll-Zaehler +2). VBLANK (A4):
 * mit vblank_hz>0 echter Zeitbasis-Zaehler (GetTickCount), sonst alt. */
u32 __stdcall hook_GetDisplayFieldStatus(u32 out)
{
    static u32 counter;
    static u32 calls;
    if (g_cfg.vblank_hz > 0) {
        counter = vblank_tick_now();
    } else {
        counter += 3;
    }
    g_fs_counter = counter; /* TEST(POPIN,W91): Stand fuer rt_from_mission */
    if (g_phud_meas) { sb_phud_wait_enter(); } /* W122 PERF-HUD: Poll = Warten */
    *(u32 *)(uintptr_t)(out + 4) = counter;
    *(u32 *)(uintptr_t)out = 3;
    *(u32 *)(g_fake_d3d + 0x2580) = counter;
    if (g_cfg.interp_fps > 0) {
        /* FPS-INTERP (W69): Feldwechsel = Tick-Anker; das Missions-Warten
         * (0x466D0) pollt hier -> faellige Zwischenbilder zeigen. */
        sb_interp_field(counter);
    }
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D GetDisplayFieldStatus -> counter=%u", counter);
    }
    if (g_phud_meas) { sb_phud_wait_leave(); } /* W122 PERF-HUD */
    return 0;
}

/* FUN_00223E2D: GPU-Config-Methode (thiscall, 1 Arg, RET 4): pollt
 * [0xFE820010]&~3 >= 4 (0x223E41-4C), dann indizierter GPU-Write
 * [EAX*4+0xFE820200] (0x223E5C), gibt 0 zurueck. M1: 0 direkt
 * (kein NV2A-MMIO, Seiteneffekt irrelevant ohne echtes Rendering). */
u32 __stdcall hook_GPU_223E2D(u32 arg)
{
    static u32 calls;
    (void)arg;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D GPU-Config(0x223E2D) -> 0 (kein NV2A-MMIO)");
    }
    return 0;
}

/* FUN_00229CC4: GPU-Ready-Wait (0xFEC0012C setzen, Bit8@0xFEC00130 pollen,
 * Timeout 1000x20us). Rueckgabe 1=ready / 0=Timeout; Caller 0x22A3F7 bricht
 * bei 0 mit D3D-Fehler 0x88780078 ab. M1: 1 (ready, kein NV2A-MMIO). */
u32 __stdcall hook_GPU_readywait(void)
{
    static u32 calls;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D GPU-ReadyWait(0x229CC4) -> 1 (ready)");
    }
    return 1;
}

/* FUN_00229C97: GPU-Ready-Wait (Bit0@0xFEC00134 pollen, Timeout). Caller
 * 0x229D28 schreibt bei 1 indiziert (0xFEC00000-Tabelle), gibt EAX hoch;
 * 0x229D45 mappt auf S_OK/E_FAIL. M1: 1 (ready) + Write noppen. */
u32 __stdcall hook_GPU_readywait2(void)
{
    static u32 calls;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D GPU-ReadyWait(0x229C97) -> 1 (ready)");
    }
    return 1;
}

/* W22 FILMAV: Host-TSC-Frequenz (einmalig, QPC-vermessen). RDTSC laeuft mit
 * Host-Takt (~GHz), das Spiel teilt durch Xbox-733,5 MHz (0x191250) -> alle
 * RDTSC/733-Nutzer (Film-Player 0x046xxx, Bink, Frame-Loop 0x0E2xxx) liefen
 * ~4,8x zu schnell (45-s-Filmabbruch: Bink-Timer erreicht 182 s nach 45 s).
 * Mit tsc_freq=1 melden wir die echte Host-Frequenz (alle 1x, Xbox-treu). */
static uint64_t g_tsc_hz;

static void w22_tsc_calibrate(void)
{
    LARGE_INTEGER qf, q0, q1;
    uint64_t t0, t1;
    double secs;
    if (g_tsc_hz)
        return;
    QueryPerformanceFrequency(&qf);
    if (qf.QuadPart == 0)
        qf.QuadPart = 1;
    t0 = __rdtsc();
    QueryPerformanceCounter(&q0);
    Sleep(100);
    t1 = __rdtsc();
    QueryPerformanceCounter(&q1);
    secs = (double)(q1.QuadPart - q0.QuadPart) / (double)qf.QuadPart;
    if (secs > 0.001)
        g_tsc_hz = (uint64_t)((double)(t1 - t0) / secs);
    if (!g_tsc_hz)
        g_tsc_hz = 733500005ull; /* Fallback: alt */
    HLOG_I("TSC: Host-Frequenz %llu Hz (Kalibrierung %.0f ms)", g_tsc_hz,
           secs * 1000.0);
}

u32 __stdcall hook_PerfFrequency(u32 out)
{
    if (!g_cfg.tsc_freq) {
        /* alt: Xbox-733,5 MHz (RDTSC-Nutzer laufen ~4,8x schnell) */
        if (out) {
            *(u32 *)(uintptr_t)out = 733500005u;
            *(u32 *)(uintptr_t)(out + 4) = 0;
        }
        return 1;
    }
    w22_tsc_calibrate();
    if (out) {
        *(u32 *)(uintptr_t)out = (u32)(g_tsc_hz & 0xFFFFFFFFu);
        *(u32 *)(uintptr_t)(out + 4) = (u32)(g_tsc_hz >> 32);
    }
    return 1;
}

/* W192 BINK-PAUSE: XAPI QueryPerformanceCounter (0x19123F) war direkt
 * RDTSC. Binks Timer 0x2441D0 benutzt ihn UND KeTickCount (Korrektur ab
 * 200 ms). Beide Uhren muessen stehen. Unpausiert bleibt die Zeitbasis
 * der Host-TSC mit der W22-Frequenz, kein QPC-Neustart/Skalierungsfehler.
 * SUSPSAFE-registrierte hlock schuetzt die 64-Bit-Werte auch auf Win32 und gegen den Bink-Worker. */
hlock_t g_tsc_lock; /* krnl_sync.c: SUSPSAFE muss auch diese Sperre sehen */
static uint64_t g_tsc_offset, g_tsc_stopped;
static int g_tsc_held;

unsigned long long sb_tsc_now(void)
{
    uint64_t now;
    hlock_enter(&g_tsc_lock);
    now = (g_tsc_held ? g_tsc_stopped : __rdtsc()) - g_tsc_offset;
    hlock_leave(&g_tsc_lock);
    return now;
}

void sb_tsc_pause(int begin)
{
    if (!g_cfg.bink_pause) { return; }
    hlock_enter(&g_tsc_lock);
    if (begin && !g_tsc_held) {
        g_tsc_stopped = __rdtsc();
        g_tsc_held = 1;
    } else if (!begin && g_tsc_held) {
        g_tsc_offset += __rdtsc() - g_tsc_stopped;
        g_tsc_held = 0;
    }
    hlock_leave(&g_tsc_lock);
}

u32 __stdcall hook_PerfCounter(u32 out)
{
    uint64_t now = sb_tsc_now();
    *(uint64_t *)(uintptr_t)out = now; /* XAPI: gueltiger Ausgabezeiger */
    return 1;
}

/* W205: the original BinkWait uses a private millisecond start epoch at
 * +0x280, initialized by 0x2402C0. Align that epoch once with the measured
 * output queue. This delays the first frame interval only; neither guest
 * clocks nor tick rate nor the subsequent movie cadence are changed.
 * Keep the original wait/refill code and the W192 pause clock intact. */
u32 __stdcall hook_BinkWait(u32 movie)
{
    typedef u32 (__stdcall *wait_fn)(u32);
    static u32 previous, previous_frame;
    static int aligned;
    volatile u32 *b = (volatile u32 *)(uintptr_t)movie;
    u32 result = ((wait_fn)(uintptr_t)0x241430u)(movie);
    if (movie && b[0] && b[1] && b[3] <= b[2]) {
        if (movie != previous || (b[3] <= 1u && previous_frame > 1u)) aligned = 0;
        previous = movie;
        previous_frame = b[3];
        if (!aligned && b[0x280 / 4] && b[0x2F0 / 4]) {
            u32 delay = audio_output_latency_ms();
            b[0x280 / 4] += delay;
            aligned = 1;
            if (g_cfg.audio_sync_diag)
                HLOG_I("AVSYNC movie-start: frame=%u delay_ms=%u", b[3], delay);
            result = ((wait_fn)(uintptr_t)0x241430u)(movie);
        }
    }
    return result;
}
