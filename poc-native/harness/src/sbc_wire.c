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
 * (Prozess-Lebensdauer, kein Shutdown noetig).
 *
 * W305 (JOY-HOTPLUG): Der Lese-Thread laeuft ueber den Verbindungs-Automaten
 * sbc_link_* (input_sbc_usb.h). Abziehen -> sofort neutraler Stand fuers
 * Spiel (bis W304 "letzter Stand eingefroren": Gas/Drehhebel liefen weiter),
 * nach anhaltendem SBC_ERR_IO Geraet schliessen und alle 1,5 s neu oeffnen -
 * auch wenn beim Start keins steckte. Den Geraete-Zeiger fuer den LED-Pfad
 * (Spiel-Thread) schuetzt g_sbc_devlk: geschlossen wird erst, nachdem er
 * exklusiv auf NULL gesetzt ist (keine Nutzung nach Freigabe); der LED-Pfad
 * wartet nie (TryAcquire, sonst spaeter nachsenden). */
static sbc_usb_dev_t *g_sbc_dev;   /* fuer LEDs veroeffentlicht (g_sbc_devlk) */
static SRWLOCK g_sbc_devlk = SRWLOCK_INIT;
static int g_sbc_tried; /* erster open-Versuch erfolgt (bis dahin PENDING) */
static sbc_state_t g_sbc_cache;
static int g_sbc_have;
static volatile LONG g_sbc_conn;   /* 0 = kein Geraet, 1 = verbunden, 2 = getrennt */
static volatile LONG g_sbc_event;  /* W305: 1 = getrennt, 2 = verbunden (Hinweis) */
static volatile LONG g_led_resync; /* W305: LED-Stand nach (Neu-)Oeffnen erneut senden */
static sbc_link_t g_link;          /* nur Lese-Thread (nach dem Start) */
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

/* Geraete-Funktionen des Automaten: Zeiger fuer den LED-Pfad pflegen. */
static int wire_open(void *ctx, sbc_usb_dev_t **out)
{
    int rc;
    (void)ctx;
    rc = sbc_usb_open(out);
    if (rc == SBC_OK && *out) {
        AcquireSRWLockExclusive(&g_sbc_devlk);
        g_sbc_dev = *out;
        ReleaseSRWLockExclusive(&g_sbc_devlk);
        InterlockedExchange(&g_led_resync, 1);
    }
    return rc;
}

static void wire_close(void *ctx, sbc_usb_dev_t *dev)
{
    (void)ctx;
    AcquireSRWLockExclusive(&g_sbc_devlk); /* wartet auf laufendes LED-Senden */
    if (g_sbc_dev == dev) {
        g_sbc_dev = NULL;
    }
    ReleaseSRWLockExclusive(&g_sbc_devlk);
    sbc_usb_close(dev); /* WinUsb_Free + CloseHandle */
}

static int wire_poll(void *ctx, sbc_usb_dev_t *dev, sbc_state_t *st)
{
    (void)ctx;
    return sbc_usb_poll(dev, st);
}

static const sbc_link_ops_t kWireOps = {wire_open, wire_close, wire_poll, NULL};

static void sbc_cache_put(const sbc_state_t *st)
{
    EnterCriticalSection(&g_sbc_cs);
    g_sbc_cache = *st;
    g_sbc_have = 1;
    LeaveCriticalSection(&g_sbc_cs);
}

static DWORD WINAPI sbc_reader_thread(void *arg)
{
    uint32_t err_n = 0, fail_n = 0;
    (void)arg;
    while (InterlockedCompareExchange(&g_sbc_run, 1, 1)) {
        sbc_state_t st;
        unsigned sl = 0, ev;
        ev = sbc_link_step(&g_link, &kWireOps, GetTickCount64(), &st, &sl);
        if (ev & SBC_LEV_OPENED) {
            if (g_link.opens > 1) {
                HLOG_I("CTRL: SBC wieder geoeffnet (Versuch %u)",
                       g_link.open_tries);
            } else {
                HLOG_I("CTRL: SBC geoeffnet (spaet eingesteckt, "
                       "Tastatur/Maus bleiben parallel aktiv)");
            }
            fail_n = 0;
        }
        if (ev & SBC_LEV_STATE) {
            sbc_cache_put(&st);
            if (ev & SBC_LEV_BACK) { /* auch: erstmals spaet eingesteckt */
                InterlockedExchange(&g_sbc_event, 2);
                HLOG_I("CTRL: SBC %sverbunden - gedrueckte Taster "
                       "erst nach Loslassen aktiv, LEDs neu gesendet",
                       g_link.opens > 1 ? "wieder " : "");
            }
            InterlockedExchange(&g_sbc_conn, 1);
        }
        if (ev & SBC_LEV_LOST) {
            /* Sofort neutral: Achsen Mitte, Pedale 0, Taster los;
             * Kippschalter/Tuner gehalten, Gang 0 = Hook haelt den Gang. */
            sbc_state_t nz;
            sbc_usb_neutral_hold(&g_link.last, g_link.have_last, &nz);
            sbc_cache_put(&nz);
            InterlockedExchange(&g_sbc_conn, 2);
            InterlockedExchange(&g_sbc_event, 1);
            HLOG_W("CTRL: SBC getrennt (%s) - neutral (Achsen Mitte, "
                   "Pedale 0, Taster los; Kippschalter/Gang/Tuner gehalten)",
                   sbc_usb_strerror(g_link.last_poll_rc));
        }
        if (ev & SBC_LEV_CLOSED) {
            HLOG_W("CTRL: SBC geschlossen nach %d USB-Fehlern - neu oeffnen "
                   "alle %u ms", SBC_LINK_IO_MAX, SBC_LINK_RETRY_MS);
        }
        if ((ev & SBC_LEV_OPENFAIL) && g_link.last_open_rc != SBC_ERR_NODEVICE &&
            hlog_throttle(&fail_n)) {
            HLOG_W("CTRL: SBC oeffnen scheitert (%s)",
                   sbc_usb_strerror(g_link.last_open_rc));
        }
        if (!ev && sl && g_link.dev && hlog_throttle(&err_n)) {
            HLOG_W("SBC-USB: poll -> %s", sbc_usb_strerror(g_link.last_poll_rc));
        }
        if (sl) {
            Sleep(sl);
        }
    }
    return 0;
}

/* Erster open-Versuch synchron (Status/Log wie bisher), danach uebernimmt
 * der Lese-Thread - W305: auch ohne Geraet (spaeter einstecken). Rueckgabe
 * 1 = Thread laeuft (Cache: verbunden, oder getrennt mit neutralem Stand). */
static int sbc_usb_ensure(void)
{
    int ok;
    InitOnceExecuteOnce(&g_sbc_once, sbc_once_init, NULL, NULL);
    EnterCriticalSection(&g_sbc_cs);
    if (!g_sbc_tried) {
        int rc;
        sbc_usb_dev_t *dev = NULL;
        g_sbc_tried = 1;
        rc = wire_open(NULL, &dev);
        if (rc == SBC_OK) {
            HLOG_I("SBC-USB: Original-Controller geoeffnet "
                   "(Tastatur/Maus bleiben parallel aktiv)");
        } else {
            /* §9 Schritt 1: einmal loggen, Tastatur faehrt weiter. */
            HLOG_W("SBC-USB: kein Geraet (%s) - Fallback Tastatur/Maus",
                   sbc_usb_strerror(rc));
            dev = NULL;
        }
        sbc_link_init(&g_link, dev);
        if (dev) {
            InterlockedExchange(&g_sbc_conn, 1);
        } else {
            /* W305: weitere Versuche im Lese-Thread, alle SBC_LINK_RETRY_MS */
            g_link.open_due = 0;
            g_link.open_tries = 1;
            g_link.last_open_rc = rc;
            g_link.next_open_ms = GetTickCount64() + SBC_LINK_RETRY_MS;
        }
        InterlockedExchange(&g_sbc_run, 1);
        g_sbc_thread = CreateThread(NULL, 0, sbc_reader_thread, NULL, 0, NULL);
        if (!g_sbc_thread) {
            HLOG_E("SBC-USB: Lese-Thread startet nicht - Fallback Tastatur");
            InterlockedExchange(&g_sbc_run, 0);
            if (dev) {
                wire_close(NULL, dev);
            }
            g_link.dev = NULL;
            InterlockedExchange(&g_sbc_conn, 0);
        }
    }
    ok = g_sbc_thread != NULL;
    LeaveCriticalSection(&g_sbc_cs);
    {
        static LONG start_logged;
        if (!InterlockedExchange(&start_logged, 1)) {
            sbc_wire_log_summary("Start"); /* W305: Pfad/LED-Ausgabe auf einen Blick */
        }
    }
    return ok;
}

int sbc_wire_take_event(void)
{
    return (int)InterlockedExchange(&g_sbc_event, 0);
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
        if (have && InterlockedCompareExchange(&g_sbc_conn, 0, 0) == 2) {
            /* W305: getrennt -> neutraler Stand, schon in Spiel-Einheiten
             * (ohne Kalibrierung, sonst laege der Zielhebel nicht mittig). */
            return 1;
        }
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
    switch (InterlockedCompareExchange(&g_sbc_conn, 0, 0)) {
    case 1: return SBC_WIRE_USB_OK;
    case 2: return SBC_WIRE_LOST; /* W305: getrennt, wird gesucht */
    default: return SBC_WIRE_NODEV;
    }
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
 * Out->LED-Umsetzung: 20 Bytes kopieren, Header 00 16 voranstellen.
 *
 * W267 (COMM-LED): Bis W266 verglich die 50-ms-Drossel mit dem zuletzt
 * GESEHENEN Report - ein gedrosselter Stand galt danach als "unveraendert"
 * und ging nie raus. Das Spiel blendet LEDs in Tick-Schritten (~33 ms) aus
 * (Comm nach dem Funkruf: 0xA1E30 setzt Comm1-5 auf Modus 2, Manager
 * 0x49520 zieht je Tick 2 ab: ..3 -> 1 -> 0), also fiel oft genau der
 * letzte Schritt in die Drossel: Lampe blieb auf Stufe 1 statt aus.
 * Jetzt Sendetor sbc_led_gate_* (Vergleich mit dem GESENDETEN Stand, faellig
 * bleibt faellig; SetState kommt jeden Poll, also spaetestens nach 50 ms +
 * 1 Poll nachgesendet). Log-Zeilen unveraendert (jede Spiel-Aenderung). */

#define SBC_FB_BASE SBA(0x4AF0C0u)
#define SBC_FB_STRIDE 0x56u
#define SBC_FB_PORTS 4u
#define SBC_FB_LEDS_OFF 66u /* LED-Bytes im Feedback (20 B -> OUT 2..21) */
#define SBC_FB_LEDS_LEN 20u
#define SBC_LED_MIN_MS 50u /* Mindestabstand zweier LED-Sendungen */

static uint8_t g_led_last[SBC_FB_PORTS][SBC_USB_OUT_LEN]; /* gesehen (Log) */
static int g_led_have[SBC_FB_PORTS];
static sbc_led_gate_t g_led_gate; /* W267: gesendet (Geraet, Mapper-Port) */
static int g_led_rsv_logged;
static uint32_t g_led_err_n;
/* W305: Zaehler fuer die Diagnosezeile (nur Anzahlen, nie Werte). */
static volatile LONG g_led_calls, g_led_chg, g_led_sent, g_led_fail, g_led_nodev,
    g_led_otherport;

static uint32_t sbc_mapper_port(void)
{
    uint32_t mp = 0;
    __try {
        mp = *(volatile uint32_t *)(uintptr_t)SBA(0x352274u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return mp < SBC_FB_PORTS ? mp : 0;
}

static void sbc_led_log(uint32_t port, const uint8_t rep[SBC_USB_OUT_LEN])
{
    char hex[2 * SBC_USB_OUT_LEN + 1];
    unsigned i;
    if (!g_cfg.input_diag) {
        return;
    }
    for (i = 0; i < SBC_USB_OUT_LEN; i++) {
        sprintf_s(hex + 2 * i, 3, "%02X", rep[i]);
    }
    HLOG_I("SBC-LED: port=%u f=%u fb=%s", port, g_frames, hex);
}

void sbc_wire_setstate(uint32_t fb)
{
    uint8_t rep[SBC_USB_OUT_LEN];
    uint32_t port;
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
    InterlockedIncrement(&g_led_calls);
    if (!g_led_have[port] || memcmp(g_led_last[port], rep, sizeof rep) != 0) {
        memcpy(g_led_last[port], rep, sizeof rep);
        g_led_have[port] = 1;
        InterlockedIncrement(&g_led_chg);
        sbc_led_log(port, rep); /* jede Spiel-Aenderung (input_diag) */
    }
    /* LEDs folgen dem Port, den das Spiel gerade liest (Mapper-Port). */
    if (port != sbc_mapper_port()) {
        InterlockedIncrement(&g_led_otherport);
        return;
    }
    if (!g_sbc_dev || InterlockedCompareExchange(&g_sbc_conn, 0, 0) != 1) {
        InterlockedIncrement(&g_led_nodev);
        return; /* Fixture/kein Geraet/getrennt: nur loggen (s. o.) */
    }
    if (InterlockedExchange(&g_led_resync, 0)) {
        /* W305: nach (Neu-)Oeffnen den aktuellen Spielstand erneut senden. */
        memset(&g_led_gate, 0, sizeof g_led_gate);
    }
    if (!TryAcquireSRWLockShared(&g_sbc_devlk)) {
        return; /* Lese-Thread schliesst gerade: faellig bleibt faellig */
    }
    if (!g_sbc_dev) {
        ReleaseSRWLockShared(&g_sbc_devlk);
        return;
    }
    {
        ULONGLONG now = GetTickCount64();
        sbc_leds_t leds;
        int ok;
        /* W267: Vergleich mit dem gesendeten Stand, Drossel mit Nachsenden. */
        if (!sbc_led_gate_due(&g_led_gate, rep, now, SBC_LED_MIN_MS)) {
            return;
        }
        if ((rep[17] & 0x0Fu) || rep[21]) {
            if (!g_led_rsv_logged) {
                g_led_rsv_logged = 1;
                HLOG_I("SBC-LED: reservierte Bits gesetzt "
                       "(Byte17low=%X, Byte21=%02X) - entfallen",
                       rep[17] & 0x0Fu, rep[21]);
            }
        }
        if (sbc_usb_parse_led_report(rep, &leds) != SBC_OK) {
            sbc_led_gate_mark(&g_led_gate, rep, now, 1); /* nie gueltig: nicht wiederholen */
            ReleaseSRWLockShared(&g_sbc_devlk);
            return;
        }
        ok = sbc_usb_set_leds(g_sbc_dev, &leds) == SBC_OK;
        sbc_led_gate_mark(&g_led_gate, rep, now, ok);
        InterlockedIncrement(ok ? &g_led_sent : &g_led_fail);
        if (!ok && hlog_throttle(&g_led_err_n)) {
            HLOG_W("SBC-LED: senden scheitert");
        }
    }
    ReleaseSRWLockShared(&g_sbc_devlk);
}

/* ---- W305: Diagnosezeile (Log + Feedback-Paket, nur Zustaende/Anzahlen) ---- */
void sbc_wire_summary(char *out, size_t cap)
{
    const char *path, *dev, *led;
    int st;
    if (!out || !cap) {
        return;
    }
    st = sbc_wire_status();
    path = !g_cfg.sbc_usb ? "off" : g_cfg.sbc_fixture[0] ? "fixture" : "winusb";
    switch (st) {
    case SBC_WIRE_USB_OK: dev = "connected"; break;
    case SBC_WIRE_LOST: dev = "disconnected (searching)"; break;
    case SBC_WIRE_PENDING: dev = "not opened yet"; break;
    case SBC_WIRE_FIXTURE: dev = "fixture"; break;
    case SBC_WIRE_NODEV:
        dev = g_link.last_open_rc == SBC_ERR_DRIVER ? "found, no WinUSB access (Zadig?)"
                                                   : "not found";
        break;
    default: dev = "-"; break;
    }
    led = st == SBC_WIRE_USB_OK ? "active" : "off (LEDs need the WinUSB path)";
    _snprintf_s(out, cap, _TRUNCATE,
                "sbc_usb=%d path=%s device=%s led_out=%s led_calls=%ld led_changes=%ld "
                "led_sent=%ld led_fail=%ld led_nodev=%ld led_otherport=%ld opens=%u closes=%u",
                g_cfg.sbc_usb, path, dev, led, (long)g_led_calls, (long)g_led_chg,
                (long)g_led_sent, (long)g_led_fail, (long)g_led_nodev, (long)g_led_otherport,
                g_link.opens, g_link.closes);
}

void sbc_wire_log_summary(const char *why)
{
    char b[400];
    if (!g_cfg.sbc_usb) {
        return; /* Standardpfad: keine Zeile (Logs unveraendert) */
    }
    sbc_wire_summary(b, sizeof b);
    HLOG_I("CTRL: SBC-Diagnose (%s) %s", why ? why : "-", b);
}
