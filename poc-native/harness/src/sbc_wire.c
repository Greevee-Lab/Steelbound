/* sbc_wire.c - Verdrahtung Original-Steel-Battalion-Controller -> Spiel
 * (W75, CTRL-SBC; CONTROLLER.md §9 Schritte 1-5).
 *
 * Quelle ist entweder das echte Geraet (sbc_usb=1, WinUSB-Modul) oder eine
 * Fixture-Datei (sbc_fixture=<pfad> gesetzt). Der Hook sieht nur
 * sbc_wire_poll/sbc_wire_setstate; sbc_usb=0 laesst beide wirkungslos.
 */
#include "sbc_wire.h"
#include "sb_profile.h" /* W229 SBA() */

#include "hcfg.h"
#include "hlog.h"
#include "sbc_fixture.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

extern unsigned g_frames; /* Present-Zaehler (xdk_hooks.c) */

/* ---- USB-Geraet (§9 Schritte 1-2) ---- */

/* sbc_usb_poll blockiert bis zu 100 ms - der Spiel-Thread darf nie warten.
 * Deshalb liest ein Hintergrund-Thread und der Hook kopiert nur den Cache
 * (Prozess-Lebensdauer, kein Shutdown noetig). */
static sbc_usb_dev_t *g_sbc_dev;
static int g_sbc_tried; /* einmalig oeffnen (§9 Schritt 1) */
static sbc_state_t g_sbc_cache;
static int g_sbc_have;
static CRITICAL_SECTION g_sbc_cs;
static INIT_ONCE g_sbc_once = INIT_ONCE_STATIC_INIT;
static HANDLE g_sbc_thread;
static volatile LONG g_sbc_run;

static BOOL CALLBACK sbc_once_init(PINIT_ONCE once, PVOID arg, PVOID *ctx)
{
    (void)once;
    (void)arg;
    (void)ctx;
    InitializeCriticalSection(&g_sbc_cs);
    return TRUE;
}

static DWORD WINAPI sbc_reader_thread(void *arg)
{
    uint32_t err_n = 0;
    (void)arg;
    while (InterlockedCompareExchange(&g_sbc_run, 1, 1)) {
        sbc_state_t st;
        int rc = sbc_usb_poll(g_sbc_dev, &st);
        if (rc == SBC_OK) {
            EnterCriticalSection(&g_sbc_cs);
            g_sbc_cache = st;
            g_sbc_have = 1;
            LeaveCriticalSection(&g_sbc_cs);
        } else if (rc == SBC_ERR_TIMEOUT) {
            /* Idle (NAK) - kein Fehler, sofort weiter. */
        } else {
            /* Abgezogen/defekt: gedrosselt loggen, kein Busy-Spin. */
            if (hlog_throttle(&err_n)) {
                HLOG_W("SBC-USB: poll -> %s (letzter Stand eingefroren)",
                       sbc_usb_strerror(rc));
            }
            Sleep(200);
        }
    }
    return 0;
}

/* Geraet einmalig oeffnen + Lese-Thread starten. Rueckgabe 1 = offen. */
static int sbc_usb_ensure(void)
{
    int ok = 0;
    InitOnceExecuteOnce(&g_sbc_once, sbc_once_init, NULL, NULL);
    EnterCriticalSection(&g_sbc_cs);
    if (!g_sbc_tried) {
        int rc;
        g_sbc_tried = 1;
        rc = sbc_usb_open(&g_sbc_dev);
        if (rc == SBC_OK) {
            HLOG_I("SBC-USB: Original-Controller geoeffnet "
                   "(Tastatur/Maus bleiben parallel aktiv)");
            InterlockedExchange(&g_sbc_run, 1);
            g_sbc_thread = CreateThread(NULL, 0, sbc_reader_thread, NULL,
                                        0, NULL);
            if (!g_sbc_thread) {
                HLOG_E("SBC-USB: Lese-Thread startet nicht - Fallback Tastatur");
                sbc_usb_close(g_sbc_dev);
                g_sbc_dev = NULL;
            }
        } else {
            /* §9 Schritt 1: einmal loggen, Tastatur faehrt weiter. */
            HLOG_W("SBC-USB: kein Geraet (%s) - Fallback Tastatur/Maus",
                   sbc_usb_strerror(rc));
        }
    }
    ok = g_sbc_dev != NULL;
    LeaveCriticalSection(&g_sbc_cs);
    return ok;
}

/* ---- Fixture-Datei (§9-Testpfad, W75 Schritt 2) ---- */

static sbc_fixture_t g_fx;
static int g_fx_tried;
static ULONGLONG g_fx_t0;
static int g_fx_t0_ok;
static unsigned g_fx_last = 0xFFFFFFFFu;
static int g_fx_anchor_logged[SBC_FIX_ANCHORS];

static uint32_t sbc_fix_memread(uint32_t addr)
{
    uint32_t v = 0;
    __try {
        /* W230: Fixture-Anker sind EU-Adressen -> Profil */
        v = *(volatile uint32_t *)(uintptr_t)SBA(addr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return v;
}

/* Fixture-Reports ueber denselben Codepfad wie das Geraet
 * (sbc_usb_parse_report -> sbc_usb_state_to_block im Hook). */
static int sbc_fixture_poll(sbc_state_t *st)
{
    unsigned idx, i;
    uint64_t ms;
    if (!g_fx_tried) {
        int rc;
        g_fx_tried = 1;
        rc = sbc_fixture_load(g_cfg.sbc_fixture, &g_fx);
        if (rc != 0) {
            HLOG_W("SBC-Fixture: %s (%s) - Fallback Tastatur/Maus",
                   g_cfg.sbc_fixture, sbc_fixture_strerror(rc));
            return 0;
        }
        HLOG_I("SBC-Fixture: %u Eintraege, %u Anker aus %s (%u Zeilen uebersprungen)",
               g_fx.n, g_fx.na, g_cfg.sbc_fixture, g_fx.skipped);
    }
    if (!g_fx.e) {
        return 0;
    }
    if (!g_fx_t0_ok) {
        g_fx_t0 = GetTickCount64();
        g_fx_t0_ok = 1;
    }
    ms = GetTickCount64() - g_fx_t0;
    if (!sbc_fixture_at(&g_fx, g_frames, ms, sbc_fix_memread, &idx)) {
        return 0;
    }
    for (i = 0; i < g_fx.na && i < SBC_FIX_ANCHORS; i++) {
        if (g_fx.a[i].fired && !g_fx_anchor_logged[i]) {
            g_fx_anchor_logged[i] = 1;
            HLOG_I("SBC-Fixture: Anker %u feuerte bei f=%u", i,
                   g_fx.a[i].fire_frame);
        }
    }
    if (idx != g_fx_last) {
        g_fx_last = idx;
        if (g_cfg.input_diag) {
            HLOG_I("SBC-Fixture: f=%u ms=%llu -> Eintrag %u (%s%llu)",
                   g_frames, (unsigned long long)ms, idx,
                   g_fx.e[idx].is_ms ? "ms" : "f",
                   (unsigned long long)g_fx.e[idx].when);
        }
    }
    return sbc_usb_parse_report(g_fx.e[idx].raw, st) == SBC_OK;
}

int sbc_wire_poll(sbc_state_t *st)
{
    int have = 0;
    if (!st || !g_cfg.sbc_usb) {
        return 0;
    }
    /* Fixture gesetzt? Dann sie statt des Geraets (kein USB-Open). */
    if (g_cfg.sbc_fixture[0]) {
        have = sbc_fixture_poll(st);
    } else {
        if (!sbc_usb_ensure()) {
            return 0;
        }
        EnterCriticalSection(&g_sbc_cs);
        if (g_sbc_have) {
            *st = g_sbc_cache;
            have = 1;
        }
        LeaveCriticalSection(&g_sbc_cs);
    }
    /* W128: Raender/Totzonen/Gang-Zuordnung aus der cfg (ungesetzt = No-Op),
     * mit sbc_cal=1 ergaenzt um die beim Start erfasste Ruhelage. */
    if (have && g_cfg.sbc_autocal) {
        static sbc_autocal_t ac;
        static sbc_cal_t eff;
        static int eff_ok;
        if (!ac.done && sbc_usb_autocal_feed(&ac, st)) {
            sbc_usb_autocal_build(&ac, &g_cfg.sbc_cal, g_cfg.sbc_axis_dead,
                                  g_cfg.sbc_pedal_dead, &eff);
            eff_ok = 1;
            if (ac.stable) {
                HLOG_I("SBC: Auto-Kalibrierung Ruhe aim=%d,%d rot=%d sight=%d,%d "
                       "ped=%d,%d,%d (Totzone Achse %d, Pedal %d)",
                       ac.rest[0], ac.rest[1], ac.rest[2], ac.rest[3], ac.rest[4],
                       ac.rest[5], ac.rest[6], ac.rest[7], g_cfg.sbc_axis_dead,
                       g_cfg.sbc_pedal_dead);
            } else {
                HLOG_W("SBC: Auto-Kalibrierung: nie ruhig (Hebel/Pedal beim Start "
                       "bewegt?) - nur cfg-Werte");
            }
        }
        if (eff_ok) {
            sbc_usb_cal_apply(&eff, st);
        } else {
            sbc_usb_cal_apply(&g_cfg.sbc_cal, st);
        }
    } else if (have) {
        sbc_usb_cal_apply(&g_cfg.sbc_cal, st);
    }
    if (have) {
        static int cal_logged;
        if (!cal_logged) {
            int a, n = 0;
            cal_logged = 1;
            for (a = 0; a < SBC_AX_COUNT; a++) {
                n += g_cfg.sbc_cal.ax[a].set != 0;
            }
            HLOG_I("SBC: Kalibrierung %d/%d Achsen, Gang-Zuordnung %s",
                   n, SBC_AX_COUNT, g_cfg.sbc_cal.gear_map_set ? "an" : "aus");
        }
    }
    return have;
}

int sbc_wire_status(void)
{
    if (!g_cfg.sbc_usb) {
        return SBC_WIRE_OFF;
    }
    if (g_cfg.sbc_fixture[0]) {
        return SBC_WIRE_FIXTURE;
    }
    if (!g_sbc_tried) {
        return SBC_WIRE_PENDING;
    }
    return g_sbc_dev ? SBC_WIRE_USB_OK : SBC_WIRE_NODEV;
}

/* ---- LEDs (§9 Schritt 5) ----
 *
 * Belegt (eigene Disassembly): Das Spiel schreibt 20 LED-Bytes an fb+66
 * (Nibble k = LED k nach CONTROLLER.md §4, Byte = k/2, High-Nibble bei
 * ungeradem k - Setter 0x494B0, Manager 0x49520 mit Tabelle 0x2B3130,
 * deren Werte 1..30+32..38 exakt die 37 LEDs sind; Index 30 = reserviert
 * wird nie beschrieben). bReportId/bLength (fb+64/65) laesst das Spiel 0 -
 * der XID-Treiber stellt 00 16 voran (OUT-Report, s. Cxbx XidSBCOutput).
 * Einziger SetState-Aufrufer ist die Poll-Loop 0x49BDA, fb = 0x4AF0C0 +
 * Port*0x56; beschrieben wird nur der Mapper-Port ([0x352274], im Setter
 * wie im Manager). dwStatus (fb+0, 0x3E5 = ERROR_IO_PENDING bei Neuigkeit)
 * wird nur gelesen, nie geschrieben - der Hook gab bisher 0 zurueck und
 * das Spiel laeuft so; daran aendert sbc_usb=1 nichts. Es gibt keine
 * Out->LED-Umsetzung: 20 Bytes kopieren, Header 00 16 voranstellen. */

#define SBC_FB_BASE SBA(0x4AF0C0u)
#define SBC_FB_STRIDE 0x56u
#define SBC_FB_PORTS 4u
#define SBC_FB_LEDS_OFF 66u /* LED-Bytes im Feedback (20 B -> OUT 2..21) */
#define SBC_FB_LEDS_LEN 20u
#define SBC_LED_MIN_MS 50u /* Mindestabstand zweier LED-Sendungen */

static uint8_t g_led_last[SBC_FB_PORTS][SBC_USB_OUT_LEN];
static int g_led_have[SBC_FB_PORTS];
static ULONGLONG g_led_sent_ms;
static int g_led_rsv_logged;
static uint32_t g_led_err_n;

static uint32_t sbc_mapper_port(void)
{
    uint32_t mp = 0;
    __try {
        mp = *(volatile uint32_t *)(uintptr_t)SBA(0x352274u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return mp < SBC_FB_PORTS ? mp : 0;
}

void sbc_wire_setstate(uint32_t fb)
{
    uint8_t rep[SBC_USB_OUT_LEN];
    uint32_t port;
    unsigned i;
    /* Nur Feedback-Structs der Poll-Loop (falsche Adressen ignorieren). */
    if (fb < SBC_FB_BASE ||
        fb >= SBC_FB_BASE + SBC_FB_PORTS * SBC_FB_STRIDE ||
        (fb - SBC_FB_BASE) % SBC_FB_STRIDE != 0) {
        return;
    }
    port = (fb - SBC_FB_BASE) / SBC_FB_STRIDE;
    rep[0] = 0x00;
    rep[1] = SBC_USB_OUT_LEN;
    __try {
        memcpy(rep + 2, (const void *)(uintptr_t)(fb + SBC_FB_LEDS_OFF),
               SBC_FB_LEDS_LEN);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (g_led_have[port] && memcmp(g_led_last[port], rep, sizeof rep) == 0) {
        return; /* unveraendert: nichts tun (Drossel, kein Poll-Spam) */
    }
    memcpy(g_led_last[port], rep, sizeof rep);
    g_led_have[port] = 1;
    if (g_cfg.input_diag) {
        char hex[2 * SBC_USB_OUT_LEN + 1];
        for (i = 0; i < SBC_USB_OUT_LEN; i++) {
            sprintf_s(hex + 2 * i, 3, "%02X", rep[i]);
        }
        HLOG_I("SBC-LED: port=%u f=%u fb=%s", port, g_frames, hex);
    }
    /* LEDs folgen dem Port, den das Spiel gerade liest (Mapper-Port). */
    if (port != sbc_mapper_port()) {
        return;
    }
    if (!g_sbc_dev) {
        return; /* Fixture/kein Geraet: nur loggen (s. o.) */
    }
    if ((rep[17] & 0x0Fu) || rep[21]) {
        if (!g_led_rsv_logged) {
            g_led_rsv_logged = 1;
            HLOG_I("SBC-LED: reservierte Bits gesetzt "
                   "(Byte17low=%X, Byte21=%02X) - entfallen",
                   rep[17] & 0x0Fu, rep[21]);
        }
    }
    {
        ULONGLONG now = GetTickCount64();
        sbc_leds_t leds;
        if (g_led_sent_ms && now - g_led_sent_ms < SBC_LED_MIN_MS) {
            return; /* gedrosselt */
        }
        g_led_sent_ms = now;
        if (sbc_usb_parse_led_report(rep, &leds) != SBC_OK) {
            return;
        }
        if (sbc_usb_set_leds(g_sbc_dev, &leds) != SBC_OK &&
            hlog_throttle(&g_led_err_n)) {
            HLOG_W("SBC-LED: senden scheitert");
        }
    }
}
