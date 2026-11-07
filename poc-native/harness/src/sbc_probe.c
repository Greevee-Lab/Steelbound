/* sbc_probe.c - Testprogramm zum Steel-Battalion-USB-Modul (W49, CTRL-SBC;
 * W128 SBC-LIVE: gefuehrter Live-Test, Lampenfolge, Roh-Dump mit Markierung;
 * W143: Testschritte/Erkennung/Ergebnisdatei im gemeinsamen Kern sbc_test.c).
 *
 * Bau: sbc_probe.exe (VS2022 Win32, s. CMakeLists). Nutzung:
 *   sbc_probe                Geraet suchen, 3 s pollen, Zustand bei Aenderung zeigen
 *   sbc_probe --test         gefuehrte Pruefung aller Elemente -> sbc_test_<datum>.txt
 *   sbc_probe --leds seq     alle Lampen nacheinander (je 0,6 s, mit Namen), dann aus
 *   sbc_probe --leds [H]     alle LEDs auf Helligkeit H (0..15, Default 15)
 *   sbc_probe --dump [N]     N Roh-Reports (Default 5), unsichere Bytes markiert
 *   sbc_probe --watch        dauerhaft, nur geaenderte Felder (Enter = Ende)
 *   sbc_probe --selftest     Fixture-Test ohne Geraet (Abnahme!)
 *   sbc_probe --check-fixture <datei>  Fixture parsen + Eintraege zeigen
 * Ohne Geraet (Fixture/Simulation, W128):
 *   --test/--dump/--watch --fixture <datei> [--out <datei>]
 *   --leds seq --mock        OUT-Reports pruefen statt senden
 *   --gen-fixture ok|faulty|gearmap|blech|badgear <datei>   simulierten --test-Ablauf schreiben
 * Exit: 0 = ok, 1 = Fehler/Selbsttest rot (--test: nicht alles OK),
 *       2 = kein Geraet (sauber), 3 = Geraet da, aber WinUSB-Zugriff
 *       scheitert (Zadig fehlt?).
 * Keine Umlaute in der Konsolenausgabe (Codepage-sicher).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <conio.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "input_sbc_usb.h"
#include "sbc_fixture.h"
#include "sbc_test.h"

#define PROBE_VERSION "W143"

static void print_state(const sbc_state_t *st)
{
    printf("btn=%04X %04X %04X aim=%02X %02X rot=%d sight=%d,%d "
           "ped=%02X %02X %02X tun=%u gear=%u rsv7=%02X\n",
           st->buttons[0], st->buttons[1], st->buttons[2],
           st->aiming_x, st->aiming_y, (int)st->rot_lever,
           (int)st->sight_x, (int)st->sight_y,
           st->pedal_l, st->pedal_m, st->pedal_r,
           st->tuner, st->gear, st->reserved7);
}

static void print_hex(const uint8_t *p, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        printf("%02X%s", p[i], i + 1 < n ? " " : "\n");
    }
}

/* ======================================================================
 * W128: Ausgabe (Konsole + optional Datei), Abbruch, Namen
 * ====================================================================== */

static FILE *g_tee;          /* Mitschrift (--watch/--dump), NULL = aus */
static int g_quiet;          /* Selbsttest: Konsolenausgabe der Modi stumm */
static volatile LONG g_abort; /* Ctrl+C */

static void outf(const char *fmt, ...)
{
    va_list ap;
    if (!g_quiet) {
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
    }
    if (g_tee) {
        va_start(ap, fmt);
        vfprintf(g_tee, fmt, ap);
        va_end(ap);
    }
}

static BOOL WINAPI on_ctrl(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        InterlockedExchange(&g_abort, 1);
        return TRUE;
    }
    return FALSE;
}

/* W143: Tasten-/Achsen-Tabellen, Rohzugriff und Gang-Name stehen im
 * gemeinsamen Kern (sbc_test.h: sbc_test_btn_name/_hint/_axes,
 * sbc_test_raw_btns/_axis/_game_units, sbc_test_gear_name). */

/* ---- Unbekannte/unsichere Report-Bytes (CONTROLLER.md §10) ----
 * Byte 6 Bit 7 (ungenutzt), Byte 7 (reserviert), die Low-Bytes aller
 * Achsen/Pedale (8,10,..,22: das Spiel nutzt nur das High-Byte), Byte 24
 * High-Nibble (Tuner nutzt nur das Low-Nibble). Aiming 8..11: Kodierung
 * u16 (Mitte 0x8000) laut §3.2/§5 - am Geraet zu bestaetigen. */
static uint8_t unsure_mask(int i)
{
    switch (i) {
    case 6: return 0x80;
    case 7: return 0xFF;
    case 8: case 10: case 12: case 14: case 16: case 18: case 20: case 22:
        return 0xFF;
    case 24: return 0xF0;
    default: return i >= SBC_USB_IN_LEN ? 0xFF : 0x00;
    }
}

/* Marker-Zeile unter der Hexzeile: "^^" geaendert, "??" unsicher,
 * "!!" unsicher UND geaendert, "  " sonst. buf >= 3*len+1. */
static void mark_row(const uint8_t *prev, const uint8_t *cur, unsigned len,
                     int have_prev, char *buf)
{
    unsigned i;
    for (i = 0; i < len; i++) {
        int ch = have_prev && prev[i] != cur[i];
        int un = unsure_mask((int)i) != 0;
        const char *m = ch && un ? "!!" : ch ? "^^" : un ? "??" : "  ";
        buf[3 * i] = m[0];
        buf[3 * i + 1] = m[1];
        buf[3 * i + 2] = ' ';
    }
    buf[len ? 3 * len - 1 : 0] = '\0';
}

/* Feld-Aenderungen prev -> cur dekodiert in buf (Zeilen "  <feld>: ...").
 * Rueckgabe = Anzahl geaenderter Felder. unk_only: nur unsichere Felder. */
static int diff_fields(const uint8_t *prev, const uint8_t *cur, char *buf,
                       size_t cap)
{
    int n = 0, i;
    size_t o = 0;
    uint64_t bp = sbc_test_raw_btns(prev), bc = sbc_test_raw_btns(cur);
    buf[0] = '\0';
#define ADD(...) do { if (o < cap) { int w_ = _snprintf_s(buf + o, cap - o, _TRUNCATE, __VA_ARGS__); if (w_ > 0) o += (size_t)w_; else o = cap; } n++; } while (0)
    for (i = 0; i < 39; i++) {
        uint64_t m = 1ULL << i;
        if ((bp ^ bc) & m) {
            ADD("  %s (bi%d): %s\n", sbc_test_btn_name[i], i, (bc & m) ? "AN" : "aus");
        }
    }
    if ((prev[6] ^ cur[6]) & 0x80) {
        ADD("  Byte6.Bit7: %d -> %d (bekannt, wird ignoriert)\n",
            prev[6] >> 7, cur[6] >> 7);
    }
    if (prev[7] != cur[7]) {
        ADD("  Byte7 (RESERVIERT, Inhalt unbekannt): %02X -> %02X  <- bitte melden\n",
            prev[7], cur[7]);
    }
    for (i = 0; i < 8; i++) {
        int o0 = sbc_test_axes[i].off;
        if (prev[o0] != cur[o0] || prev[o0 + 1] != cur[o0 + 1]) {
            int vp = sbc_test_raw_axis(prev, i), vc = sbc_test_raw_axis(cur, i);
            if (sbc_test_axes[i].kind == 1) {
                ADD("  %s: %d -> %d (hi %d -> %d, lo %02X -> %02X%s)\n",
                    sbc_test_axes[i].name, vp, vc, sbc_test_game_units(i, vp), sbc_test_game_units(i, vc),
                    prev[o0], cur[o0],
                    prev[o0 + 1] == cur[o0 + 1] ? ", NUR Low-Byte?" : "");
            } else {
                ADD("  %s: 0x%04X -> 0x%04X (hi %02X -> %02X, lo %02X -> %02X%s)\n",
                    sbc_test_axes[i].name, (unsigned)vp, (unsigned)vc,
                    prev[o0 + 1], cur[o0 + 1], prev[o0], cur[o0],
                    prev[o0 + 1] == cur[o0 + 1] ? ", NUR Low-Byte?" : "");
            }
        }
    }
    if ((prev[24] ^ cur[24]) & 0x0F) {
        ADD("  Tuner: %u -> %u\n", prev[24] & 0x0F, cur[24] & 0x0F);
    }
    if ((prev[24] ^ cur[24]) & 0xF0) {
        ADD("  Byte24 High-Nibble (unbekannt): %X -> %X  <- bitte melden\n",
            prev[24] >> 4, cur[24] >> 4);
    }
    if (prev[25] != cur[25]) {
        ADD("  Gang: %u (%s) -> %u (%s)\n", prev[25], sbc_test_gear_name(prev[25]),
            cur[25], sbc_test_gear_name(cur[25]));
    }
#undef ADD
    return n;
}

/* W143: Mitschnitt unsicherer Felder im gemeinsamen Kern
 * (sbc_test_unk_t, sbc_test_unk_add/_report). */

/* ======================================================================
 * W128: Report-Quelle (echtes Geraet | Simulation aus Fixture/Speicher)
 * ====================================================================== */

typedef struct {
    uint64_t t;       /* ms (virtuell) */
    int key;          /* 0 = Report, sonst Taste ('\r' = Enter, 'j', 'n') */
    unsigned len;
    uint8_t raw[SBC_USB_RAW_MAX];
} sim_ev_t;

enum { SEV_REPORT = 1, SEV_KEY, SEV_TIMEOUT, SEV_ERR };

typedef struct {
    int live;
    sbc_usb_dev_t *dev;
    ULONGLONG t0;
    /* Simulation */
    sim_ev_t *ev;
    unsigned nev, iev;
    uint64_t vt;
    int ended;        /* Simulation: keine Ereignisse mehr */
    /* Mock/Simulation: gesendete OUT-Reports */
    uint8_t (*outs)[SBC_USB_OUT_LEN];
    unsigned nout, capout;
    int err;          /* letzter Fehler (SEV_ERR) */
} src_t;

static uint64_t src_now(src_t *s)
{
    return s->live ? (uint64_t)(GetTickCount64() - s->t0) : s->vt;
}

static void src_sleep(src_t *s, unsigned ms)
{
    if (s->live) {
        Sleep(ms);
    } else {
        s->vt += ms;
    }
}

/* Naechstes Ereignis bis spaetestens deadline (ms). Live: Taste aus der
 * Konsole oder ein Report (blockiert <= 100 ms). Simulation: Ereignis aus
 * der Liste, falls bis deadline faellig, sonst Zeit vorspulen + TIMEOUT. */
static int src_next(src_t *s, uint64_t deadline, sim_ev_t *out)
{
    if (s->live) {
        int rc;
        if (g_abort) {
            out->key = 0x1B;
            return SEV_KEY;
        }
        if (_kbhit()) {
            int c = _getch();
            if (c == 0 || c == 0xE0) { /* Sondertaste: 2. Byte verwerfen */
                (void)_getch();
                return SEV_TIMEOUT;
            }
            out->key = c;
            return SEV_KEY;
        }
        if (src_now(s) >= deadline) {
            return SEV_TIMEOUT;
        }
        rc = sbc_usb_poll_raw(s->dev, out->raw, &out->len);
        if (rc == SBC_OK) {
            out->key = 0;
            out->t = src_now(s);
            return SEV_REPORT;
        }
        if (rc == SBC_ERR_TIMEOUT) {
            return SEV_TIMEOUT;
        }
        s->err = rc;
        return SEV_ERR;
    }
    if (s->iev < s->nev && s->ev[s->iev].t <= deadline) {
        *out = s->ev[s->iev++];
        if (out->t > s->vt) {
            s->vt = out->t;
        }
        return out->key ? SEV_KEY : SEV_REPORT;
    }
    if (s->iev >= s->nev) {
        s->ended = 1;
    }
    if (deadline > s->vt) {
        s->vt = deadline;
    }
    return SEV_TIMEOUT;
}

static int src_send_out(src_t *s, const uint8_t out[SBC_USB_OUT_LEN])
{
    if (s->live && s->dev) {
        return sbc_usb_send_out_raw(s->dev, out);
    }
    if (s->nout >= s->capout) {
        unsigned nc = s->capout ? s->capout * 2 : 64;
        void *p = realloc(s->outs, (size_t)nc * SBC_USB_OUT_LEN);
        if (!p) {
            return SBC_ERR_ARG;
        }
        s->outs = (uint8_t (*)[SBC_USB_OUT_LEN])p;
        s->capout = nc;
    }
    memcpy(s->outs[s->nout++], out, SBC_USB_OUT_LEN);
    return SBC_OK;
}

static void src_free(src_t *s)
{
    if (s->dev) {
        sbc_usb_close(s->dev);
    }
    free(s->ev);
    free(s->outs);
    memset(s, 0, sizeof *s);
}

/* Simulation aus Fixture-Datei: Reports (fN = N/60 s, Nms) + Zeilen
 * "#key <Nms> ENTER|j|n" (fuer den Loader Kommentare). */
static int src_open_fixture(src_t *s, const char *path)
{
    sbc_fixture_t fx;
    FILE *f = NULL;
    char line[512];
    unsigned i, cap;
    int rc = sbc_fixture_load(path, &fx);
    memset(s, 0, sizeof *s);
    if (rc != 0) {
        printf("FAIL: %s: %s\n", path, sbc_fixture_strerror(rc));
        return 1;
    }
    cap = fx.n + 256;
    s->ev = (sim_ev_t *)calloc(cap, sizeof *s->ev);
    if (!s->ev) {
        sbc_fixture_free(&fx);
        return 1;
    }
    for (i = 0; i < fx.n; i++) {
        sim_ev_t *e = &s->ev[s->nev];
        e->t = fx.e[i].is_ms ? fx.e[i].when : fx.e[i].when * 1000 / 60;
        e->len = SBC_USB_IN_LEN;
        memcpy(e->raw, fx.e[i].raw, SBC_USB_IN_LEN);
        s->nev++;
    }
    sbc_fixture_free(&fx);
    if (fopen_s(&f, path, "r") == 0 && f) {
        while (fgets(line, sizeof line, f) && s->nev < cap) {
            unsigned long long ms;
            char what[32];
            if (sscanf_s(line, "#key %llums %31s", &ms, what, (unsigned)sizeof what) == 2) {
                sim_ev_t *e = &s->ev[s->nev++];
                memset(e, 0, sizeof *e);
                e->t = ms;
                e->key = _stricmp(what, "ENTER") == 0 ? '\r' : (unsigned char)what[0];
            }
        }
        fclose(f);
    }
    /* Stabil nach Zeit sortieren (Einfuegesortierung, wenige hundert
     * Eintraege; gleiche Zeit = Dateireihenfolge, Tasten nach Reports). */
    {
        unsigned j;
        for (i = 1; i < s->nev; i++) {
            sim_ev_t tmp = s->ev[i];
            j = i;
            while (j > 0 && s->ev[j - 1].t > tmp.t) {
                s->ev[j] = s->ev[j - 1];
                j--;
            }
            s->ev[j] = tmp;
        }
    }
    return 0;
}

/* Echtes Geraet oeffnen. Rueckgabe 0 oder Exit-Code 2/3 (mit Meldung). */
static int src_open_live(src_t *s)
{
    int rc;
    memset(s, 0, sizeof *s);
    s->live = 1;
    s->t0 = GetTickCount64();
    rc = sbc_usb_open(&s->dev);
    if (rc == SBC_ERR_NODEVICE) {
        printf("sbc_probe: kein Controller gefunden (0A7B:D000 nicht angesteckt "
               "oder keine WinUSB-Bindung).\n");
        printf("  1. Controller per Xbox->USB-Adapter anstecken (Direktport, kein Hub).\n");
        printf("  2. Zadig: Options -> List All Devices, 0A7B:D000 waehlen, WinUSB "
               "installieren (CONTROLLER.md 7).\n");
        printf("  3. Danach erneut: sbc_probe --test\n");
        return 2;
    }
    if (rc != SBC_OK) {
        printf("sbc_probe: Controller gefunden, Zugriff scheitert: %s\n",
               sbc_usb_strerror(rc));
        printf("  Zadig erneut: Treiber WinUSB (nicht libusbK/libusb0), anderen "
               "USB-Port probieren (CONTROLLER.md 7/10).\n");
        return 3;
    }
    return 0;
}

/* ======================================================================
 * W143: sbc_test_io_t ueber src_t (Konsole)
 * ====================================================================== */

static uint64_t pio_now(void *v) { return src_now((src_t *)v); }

static int pio_next(void *v, uint64_t deadline, sbc_test_ev_t *out)
{
    sim_ev_t e;
    int k = src_next((src_t *)v, deadline, &e);
    out->t = e.t;
    out->key = e.key;
    out->len = e.len;
    memcpy(out->raw, e.raw, sizeof e.raw);
    switch (k) {
    case SEV_REPORT: return SBC_TEV_REPORT;
    case SEV_KEY: return SBC_TEV_KEY;
    case SEV_ERR: return SBC_TEV_ERR;
    default: return SBC_TEV_TIMEOUT;
    }
}

static void pio_sleep(void *v, unsigned ms) { src_sleep((src_t *)v, ms); }

static int pio_send_out(void *v, const uint8_t out[SBC_USB_OUT_LEN])
{
    return src_send_out((src_t *)v, out);
}

static unsigned pio_nout(void *v) { return ((src_t *)v)->nout; }

static const uint8_t *pio_out_at(void *v, unsigned i)
{
    src_t *s = (src_t *)v;
    return i < s->nout ? s->outs[i] : NULL;
}

static int pio_is_live(void *v) { return ((src_t *)v)->live; }
static int pio_has_ended(void *v) { return ((src_t *)v)->ended; }
static int pio_err(void *v) { return ((src_t *)v)->err; }
static int pio_aborted(void *v) { (void)v; return g_abort != 0; }

static void pio_emit(void *v, const char *text)
{
    (void)v;
    outf("%s", text);
    if (!g_quiet) {
        fflush(stdout);
    }
}

static void pio_discard_keys(void *v)
{
    src_t *s = (src_t *)v;
    if (s->live) {
        while (_kbhit()) {
            (void)_getch();
        }
    }
}

static int pio_get_report(void *v, uint8_t raw[SBC_USB_RAW_MAX], unsigned *len)
{
    src_t *s = (src_t *)v;
    if (!s->live || !s->dev) {
        return 0;
    }
    *len = 0;
    return sbc_usb_get_report(s->dev, raw, len) == SBC_OK;
}

static void pio_make(src_t *s, sbc_test_io_t *io)
{
    io->ctx = s;
    io->now = pio_now;
    io->next = pio_next;
    io->sleep_ms = pio_sleep;
    io->send_out = pio_send_out;
    io->nout = pio_nout;
    io->out_at = pio_out_at;
    io->is_live = pio_is_live;
    io->has_ended = pio_has_ended;
    io->err = pio_err;
    io->aborted = pio_aborted;
    io->emit = pio_emit;
    io->progress = NULL;
    io->discard_keys = pio_discard_keys;
    io->get_report = pio_get_report;
}

/* Ersten Report holen (fuer --dump/--watch, ueber den Kern). */
static int src_first_report(src_t *s, uint8_t cur[SBC_USB_RAW_MAX], unsigned *len,
                            int *used_getreport)
{
    sbc_test_io_t io;
    sbc_test_ctx_t c;
    pio_make(s, &io);
    sbc_test_ctx_init(&c, &io);
    if (!sbc_test_first_report(&c)) {
        return 0;
    }
    memcpy(cur, c.cur, SBC_USB_RAW_MAX);
    *len = c.len;
    *used_getreport = c.used_getreport;
    return 1;
}

/* Pfad neben der exe (name = Dateiname). */
static void path_next_to_exe(char *out, size_t cap, const char *name)
{
    char *p;
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (n == 0 || n >= cap) {
        strcpy_s(out, cap, name);
        return;
    }
    p = strrchr(out, '\\');
    if (p) {
        p[1] = '\0';
    } else {
        out[0] = '\0';
    }
    strcat_s(out, cap, name);
}

/* W143: Zeitstempel und Lampenfolge im gemeinsamen Kern (sbc_test_stamp,
 * sbc_test_led_seq_*). */

/* ======================================================================
 * W128: --dump / --watch
 * ====================================================================== */

static int g_color; /* ANSI-Farben auf der Konsole (nicht in Dateien) */

static void enable_color(void)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD m = 0;
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &m) &&
        SetConsoleMode(h, m | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */)) {
        g_color = 1;
    }
}

/* Hexzeile + Markerzeile; geaenderte Bytes farbig (nur Konsole). */
static void dump_report(const uint8_t *prev, const uint8_t *cur, unsigned len,
                        int have_prev)
{
    char mk[3 * SBC_USB_RAW_MAX + 1];
    unsigned i, n = len > SBC_USB_RAW_MAX ? SBC_USB_RAW_MAX : len;
    if (!g_quiet) {
        printf("  IN : ");
        for (i = 0; i < n; i++) {
            int ch = have_prev && prev[i] != cur[i];
            if (g_color && ch) {
                printf("\x1b[1;33;7m%02X\x1b[0m%s", cur[i], i + 1 < n ? " " : "");
            } else if (g_color && unsure_mask((int)i)) {
                printf("\x1b[36m%02X\x1b[0m%s", cur[i], i + 1 < n ? " " : "");
            } else {
                printf("%02X%s", cur[i], i + 1 < n ? " " : "");
            }
        }
        printf("\n");
    }
    if (g_tee) {
        fprintf(g_tee, "  IN : ");
        for (i = 0; i < n; i++) {
            fprintf(g_tee, "%02X%s", cur[i], i + 1 < n ? " " : "");
        }
        fprintf(g_tee, "\n");
    }
    mark_row(prev, cur, n, have_prev, mk);
    outf("       %s\n", mk);
}

static void dump_legend(void)
{
    outf("Legende: ^^ = geaendert, ?? = unsicheres Byte (Byte 6.7, 7, Achsen-Low-"
         "Bytes 8/10/../22, Byte 24 high), !! = unsicher UND geaendert.\n");
    outf("         Aiming (Bytes 8..11) laut Doku u16 mit Mitte 0x8000 - in Ruhe "
         "pruefen.\n");
}

static void tee_pf(void *opaque, const char *fmt, ...)
{
    char b[1024];
    va_list ap;
    (void)opaque;
    va_start(ap, fmt);
    _vsnprintf_s(b, sizeof b, _TRUNCATE, fmt, ap);
    va_end(ap);
    outf("%s", b);
}

/* --dump N (watch = 0) oder --watch (dauerhaft, nur Aenderungen). */
static int cmd_dump(src_t *s, int n_max, int watch)
{
    uint8_t prev[SBC_USB_RAW_MAX], cur[SBC_USB_RAW_MAX];
    char buf[4096];
    unsigned len = 0;
    int have = 0, shown = 0, gr = 0;
    sbc_test_unk_t *u = (sbc_test_unk_t *)calloc(1, sizeof *u);
    sim_ev_t e;
    if (!u) {
        return 1;
    }
    memset(prev, 0, sizeof prev);
    dump_legend();
    if (watch) {
        outf("Watch: zeigt nur geaenderte Felder. Ende mit Enter (oder Ctrl+C).\n");
    }
    if (!src_first_report(s, cur, &len, &gr)) {
        outf("sbc_probe: kein Report empfangen%s.\n",
             s->err ? " (USB-Fehler)" : "");
        free(u);
        return 1;
    }
    for (;;) {
        unsigned ln = len;
        sbc_test_unk_add(u, cur, ln);
        if (ln < SBC_USB_IN_LEN || cur[0] != 0x00 || cur[1] != SBC_USB_IN_LEN) {
            outf("WARN: Report Laenge %u, Header %02X %02X (erwartet 26, 00 1A) - "
                 "bitte melden\n", ln, cur[0], cur[1]);
        }
        if (!watch) {
            sbc_state_t st;
            outf("[%d] t=%.3fs len=%u%s\n", shown, src_now(s) / 1000.0, ln,
                 gr && shown == 0 ? " (GET_REPORT)" : "");
            dump_report(prev, cur, ln, have);
            if (ln >= SBC_USB_IN_LEN && sbc_usb_parse_report(cur, &st) == SBC_OK) {
                outf("  dek: btn=%04X %04X %04X aim=%02X %02X rot=%d sight=%d,%d "
                     "ped=%02X %02X %02X tun=%u gear=%u(%s) rsv7=%02X\n",
                     st.buttons[0], st.buttons[1], st.buttons[2], st.aiming_x,
                     st.aiming_y, (int)st.rot_lever, (int)st.sight_x,
                     (int)st.sight_y, st.pedal_l, st.pedal_m, st.pedal_r,
                     st.tuner, st.gear, sbc_test_gear_name(st.gear), st.reserved7);
            }
            if (have && diff_fields(prev, cur, buf, sizeof buf) > 0) {
                outf("%s", buf);
            }
            shown++;
            if (shown >= n_max) {
                break;
            }
        } else {
            if (!have) {
                outf("t=%.3fs Startzustand:\n", src_now(s) / 1000.0);
                dump_report(prev, cur, ln, 0);
            } else if (diff_fields(prev, cur, buf, sizeof buf) > 0) {
                outf("t=%.3fs\n%s", src_now(s) / 1000.0, buf);
                if (g_tee) {
                    fflush(g_tee);
                }
            }
        }
        memcpy(prev, cur, sizeof cur);
        have = 1;
        /* naechster Report */
        for (;;) {
            uint64_t dl = src_now(s) + (watch ? 200 : 100);
            int k;
            if (!watch && s->live && src_now(s) > 10000) {
                goto done; /* --dump: 10 s Frist wie bisher */
            }
            k = src_next(s, dl, &e);
            if (k == SEV_REPORT) {
                memcpy(cur, e.raw, sizeof cur);
                len = e.len;
                break;
            }
            if (k == SEV_KEY && (e.key == '\r' || e.key == 0x1B || e.key == 'q')) {
                goto done;
            }
            if (k == SEV_ERR) {
                outf("sbc_probe: poll -> %s\n", sbc_usb_strerror(s->err));
                goto done;
            }
            if (!s->live && s->ended) {
                goto done;
            }
        }
    }
done:
    outf("\n");
    sbc_test_unk_report(u, tee_pf, NULL);
    free(u);
    return 0;
}

/* W143: gefuehrte Pruefung (--test) im gemeinsamen Kern sbc_test.c
 * (Schritte, Erkennung, Ergebnisdatei). Aufruf s. main/--test und Selbsttest. */

/* (Schritte sbc_test_step_button/_axis im Kern) */

/* (Schritte sbc_test_step_tuner/_gear + sbc_test_gear_suggest im Kern) */

/* (Lampenschritt, Vorschlaege, Bericht: sbc_test_step_led/_axis_suggest/_gear_suggest,
 * sbc_test_write_report/_run_all im Kern) */

/* ---- Fixture-Test (reine Funktionen, ohne Geraet) ---- */

static int n_checks, n_fail;

#define CHECK(cond, ...) do { \
    n_checks++; \
    if (!(cond)) { n_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

/* Fixture N: Neutralstellung (Doku-Werte). */
static const uint8_t kNeutral[SBC_USB_IN_LEN] = {
    0x00, 0x1A,
    0x00, 0x00, 0x00, 0x00, 0x00, /* Tasten: keine */
    0x00,                         /* Byte 7 reserviert */
    0x00, 0x80, 0x00, 0x80,       /* Aiming neutral */
    0x00, 0x00,                   /* Rotation neutral */
    0x00, 0x00, 0x00, 0x00,       /* Sight neutral */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* Pedale los */
    0x00,                         /* Tuner 0 */
    0x08                          /* Gang N */
};

/* Fixture S: Startsequenz (Luke+Zuendung+5 Toggles, Gas halb, 1. Gang). */
static const uint8_t kStart[SBC_USB_IN_LEN] = {
    0x00, 0x1A,
    0x30, 0x00, 0x00, 0x00, 0x7C, /* bi4+bi5, bi34..38 */
    0xFF,                         /* reserviert (wird maskiert!) */
    0x00, 0x80, 0x00, 0x80,
    0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x80, /* Gas halb */
    0x00,
    0x09                          /* 1. Gang */
};

/* Fixture K: Kampf (Feuer+LockOn+Chaff, voll rechts/unten, Sicht links/oben). */
static const uint8_t kCombat[SBC_USB_IN_LEN] = {
    0x00, 0x1A,
    0x07, 0x00, 0x04, 0x00, 0x00, /* bi0,bi1,bi2, bi18 */
    0x00,
    0xFF, 0xFF, 0xFF, 0xFF,       /* Aiming rechts/unten */
    0xFF, 0x7F,                   /* Rotation voll rechts (+127) */
    0x01, 0x80, 0xFF, 0x7F,       /* Sight links (-128), unten (+127) */
    0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, /* links voll, rechts voll */
    0x0C,                         /* Tuner 12 */
    0x07                          /* R-Gang */
};

static void selftest_w128(void); /* W128, s. unten */

static int run_selftest(void)
{
    sbc_state_t st;
    uint8_t blk[24];
    sbc_leds_t leds;
    uint8_t out[SBC_USB_OUT_LEN];
    int i, rc;

    /* 1. Neutral parsen. */
    memset(&st, 0xCC, sizeof st);
    rc = sbc_usb_parse_report(kNeutral, &st);
    CHECK(rc == SBC_OK, "neutral rc=%d", rc);
    CHECK(st.buttons[0] == 0 && st.buttons[1] == 0 && st.buttons[2] == 0,
          "neutral buttons %04X %04X %04X", st.buttons[0], st.buttons[1], st.buttons[2]);
    CHECK(st.aiming_x == 0x80 && st.aiming_y == 0x80, "neutral aim %02X %02X",
          st.aiming_x, st.aiming_y);
    CHECK(st.rot_lever == 0 && st.sight_x == 0 && st.sight_y == 0, "neutral sticks");
    CHECK(st.pedal_l == 0 && st.pedal_m == 0 && st.pedal_r == 0, "neutral pedals");
    CHECK(st.tuner == 0 && st.gear == 8, "neutral tun=%u gear=%u", st.tuner, st.gear);

    /* 2. Startsequenz: Tasten + Maskierung + Gas + Gang. */
    memset(&st, 0, sizeof st);
    rc = sbc_usb_parse_report(kStart, &st);
    CHECK(rc == SBC_OK, "start rc=%d", rc);
    CHECK(st.buttons[0] == 0x0030, "start b0=%04X", st.buttons[0]);
    CHECK(st.buttons[1] == 0x0000, "start b1=%04X", st.buttons[1]);
    CHECK(st.buttons[2] == 0x007C, "start b2=%04X", st.buttons[2]);
    CHECK(st.pedal_r == 0x80, "start gas=%02X", st.pedal_r);
    CHECK(st.gear == 9, "start gear=%u", st.gear);
    CHECK(st.reserved7 == 0xFF, "start rsv7=%02X", st.reserved7);

    /* 3. Kampf: alle Achsen-Endlagen + Tuner + R-Gang. */
    memset(&st, 0, sizeof st);
    rc = sbc_usb_parse_report(kCombat, &st);
    CHECK(rc == SBC_OK, "combat rc=%d", rc);
    CHECK(st.buttons[0] == 0x0007, "combat b0=%04X", st.buttons[0]);
    CHECK(st.buttons[1] == 0x0004, "combat b1=%04X", st.buttons[1]);
    CHECK(st.aiming_x == 0xFF && st.aiming_y == 0xFF, "combat aim");
    CHECK(st.rot_lever == 127, "combat rot=%d", (int)st.rot_lever);
    CHECK(st.sight_x == -128 && st.sight_y == 127, "combat sight=%d,%d",
          (int)st.sight_x, (int)st.sight_y);
    CHECK(st.pedal_l == 0xFF && st.pedal_m == 0 && st.pedal_r == 0xFF, "combat ped");
    CHECK(st.tuner == 12 && st.gear == 7, "combat tun=%u gear=%u", st.tuner, st.gear);

    /* 4. Fehlerfaelle: Header + NULL. */
    {
        uint8_t bad[SBC_USB_IN_LEN];
        memcpy(bad, kNeutral, sizeof bad);
        bad[1] = 0x10;
        CHECK(sbc_usb_parse_report(bad, &st) == SBC_ERR_BADREPORT, "bad len ok?");
        bad[1] = 0x1A; bad[0] = 0x01;
        CHECK(sbc_usb_parse_report(bad, &st) == SBC_ERR_BADREPORT, "bad id ok?");
        CHECK(sbc_usb_parse_report(NULL, &st) == SBC_ERR_ARG, "null raw ok?");
        CHECK(sbc_usb_parse_report(kNeutral, NULL) == SBC_ERR_ARG, "null st ok?");
        CHECK(sbc_usb_state_to_block(NULL, blk) == SBC_ERR_ARG, "null to_block?");
        CHECK(sbc_usb_state_to_block(&st, NULL) == SBC_ERR_ARG, "null blk?");
    }

    /* 5. Zustand -> Spiel-Block (Start-Fixture, inkl. Byte-7-Maske). */
    sbc_usb_parse_report(kStart, &st);
    memset(blk, 0xCC, sizeof blk);
    rc = sbc_usb_state_to_block(&st, blk);
    {
        static const uint8_t kWant[24] = {
            0x30, 0x00, 0x00, 0x00, 0x7C, 0x00, /* Byte 5 = 0 trotz rsv7=FF */
            0x00, 0x80, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x09
        };
        CHECK(rc == SBC_OK, "to_block rc=%d", rc);
        CHECK(memcmp(blk, kWant, sizeof blk) == 0, "to_block bytes");
        if (memcmp(blk, kWant, sizeof blk) != 0) {
            printf("  got: "); print_hex(blk, sizeof blk);
            printf("  exp: "); print_hex(kWant, sizeof kWant);
        }
    }

    /* 6. LED-Builder: einzeln + alle + NULL. */
    memset(&leds, 0, sizeof leds);
    leds.level[SBC_LED_START] = 0xF;
    leds.level[SBC_LED_GEAR_3] = 0xA;
    leds.level[SBC_LED_EJECT] = 0x5;
    sbc_usb_build_led_report(&leds, out);
    CHECK(out[0] == 0x00 && out[1] == 0x16, "led header %02X %02X", out[0], out[1]);
    CHECK(out[2] == 0x05, "led eject byte2=%02X", out[2]);
    CHECK(out[3] == 0xF0, "led start byte3=%02X", out[3]);
    CHECK(out[19] == 0xA0, "led gear3 byte19=%02X", out[19]);
    CHECK(out[21] == 0x00, "led byte21=%02X", out[21]);
    {
        int zeros = 1, j;
        for (j = 2; j <= 20; j++) {
            if (j == 2 || j == 3 || j == 19) {
                continue; /* oben einzeln belegt */
            }
            if (out[j] != 0) {
                zeros = 0;
            }
        }
        CHECK(zeros, "led rest 0?");
    }
    sbc_usb_leds_all(&leds, 0xF);
    sbc_usb_build_led_report(&leds, out);
    {
        int allf = 1;
        for (i = 2; i <= 16; i++) {
            if (out[i] != 0xFF) {
                allf = 0;
            }
        }
        if (out[17] != 0xF0 || out[18] != 0xFF || out[19] != 0xFF ||
            out[20] != 0xFF || out[21] != 0x00) {
            allf = 0;
        }
        CHECK(allf, "led all-on bytes (17=F0, 21=00)?");
    }
    sbc_usb_build_led_report(NULL, out);
    {
        int all0 = 1;
        for (i = 2; i < SBC_USB_OUT_LEN; i++) {
            if (out[i] != 0) {
                all0 = 0;
            }
        }
        CHECK(out[0] == 0x00 && out[1] == 0x16 && all0, "led null->off?");
    }

    /* 7. LED-Parser (W75): Roundtrip + Fehlerfaelle. */
    {
        sbc_leds_t rt;
        uint8_t back[SBC_USB_OUT_LEN];
        memset(&leds, 0, sizeof leds);
        leds.level[SBC_LED_START] = 0xF;
        leds.level[SBC_LED_GEAR_3] = 0xA;
        leds.level[SBC_LED_EJECT] = 0x5;
        sbc_usb_build_led_report(&leds, out);
        memset(&rt, 0xCC, sizeof rt);
        CHECK(sbc_usb_parse_led_report(out, &rt) == SBC_OK, "ledparse rc");
        CHECK(rt.level[SBC_LED_START] == 0xF &&
              rt.level[SBC_LED_GEAR_3] == 0xA &&
              rt.level[SBC_LED_EJECT] == 0x5, "ledparse werte");
        sbc_usb_build_led_report(&rt, back);
        CHECK(memcmp(out, back, sizeof out) == 0, "ledparse roundtrip");
        out[1] = 0x10;
        CHECK(sbc_usb_parse_led_report(out, &rt) == SBC_ERR_BADREPORT,
              "ledparse bad len?");
        out[1] = 0x16;
        CHECK(sbc_usb_parse_led_report(NULL, &rt) == SBC_ERR_ARG,
              "ledparse null raw?");
        CHECK(sbc_usb_parse_led_report(out, NULL) == SBC_ERR_ARG,
              "ledparse null leds?");
    }

    selftest_w128(); /* W128: Kalibrierung, Lampenfolge, Dump/Watch, --test */

    printf("selftest: %d checks, %d failed\n", n_checks, n_fail);
    return n_fail ? 1 : 0;
}

/* ======================================================================
 * W128: simulierter --test-Ablauf (Selbsttest + --gen-fixture)
 * ====================================================================== */

typedef struct {
    sim_ev_t *ev;
    unsigned n, cap;
    uint64_t t;
    uint8_t r[SBC_USB_IN_LEN];
} gen_t;

static void gen_push(gen_t *g, int key)
{
    sim_ev_t *e;
    if (g->n >= g->cap) {
        unsigned nc = g->cap ? g->cap * 2 : 256;
        void *p = realloc(g->ev, (size_t)nc * sizeof *g->ev);
        if (!p) return;
        g->ev = (sim_ev_t *)p;
        g->cap = nc;
    }
    e = &g->ev[g->n++];
    memset(e, 0, sizeof *e);
    e->t = g->t;
    e->key = key;
    if (!key) {
        e->len = SBC_USB_IN_LEN;
        memcpy(e->raw, g->r, SBC_USB_IN_LEN);
    }
}

static void gen_emit(gen_t *g, unsigned dt) { g->t += dt; gen_push(g, 0); }
static void gen_key(gen_t *g, unsigned dt, int key) { g->t += dt; gen_push(g, key); }

static void gen_btn(gen_t *g, int bi, int on)
{
    int byte = 2 + bi / 8, bit = bi % 8;
    if (on) g->r[byte] |= (uint8_t)(1u << bit);
    else g->r[byte] &= (uint8_t)~(1u << bit);
}

static void gen_axis(gen_t *g, int a, int v)
{
    unsigned u = (unsigned)v & 0xFFFFu;
    g->r[sbc_test_axes[a].off] = (uint8_t)(u & 0xFF);
    g->r[sbc_test_axes[a].off + 1] = (uint8_t)(u >> 8);
}

/* faulty = 2: alles korrekt, aber Gang-Byte 254/255/1-5 (echte
 * Geraete-Kodierung, Nutzer 6.11. -> Schalthebel OK, kein Vorschlag).
 * faulty = 3 (blech): Nutzer-Befund 6.11. nachgebaut - Reibungshebel-Ruhe
 * 0x59/0x93 bei Vollhub, Pedal-Ruhe, MainWeapon+Fire / TankDetach+WC
 * gleichzeitig, Nachbar-Schalter bei bi36, Rotation bei Aiming, Tuner mit
 * Blende, Gang-Reihenfolge 1 254 5 254 255 1 2 3 4, Byte6-Bit7 gesetzt.
 * faulty = 4 (badgear): Gang 6 statt 5 (Klasse mod 8 == 6, ungueltig)
 * -> Schalthebel unerwartet + Vorschlag sbc_gear_map.
 * faulty = 0: alles korrekt; 1: definierte Fehler (Selbsttest erwartet sie):
 *  bi3 per Enter uebersprungen, bi5 -> kommt bi6, Schalter bi36 nur AN,
 *  Aiming X Teilhub (Vorschlag Zwei-Wert-Form), Aiming Y nur halber Weg,
 *  Tuner nur 0..5, Gang 4 fehlt, Byte 7 = 0x04 waehrend des Rotationstests,
 *  Low-Bytes von Aiming X variieren. */
static void gen_script(gen_t *g, int faulty)
{
    int i, a;
    uint64_t t_end;
    memset(g, 0, sizeof *g);
    g->r[0] = 0x00; g->r[1] = SBC_USB_IN_LEN;
    gen_axis(g, 0, 0x8000);
    gen_axis(g, 1, 0x8000);
    if (faulty == 3) {
        /* alle Achsen auf Szenario-Ruhe (Schrittbeginn = Ruhe, wie live) */
        gen_axis(g, 0, 0x5940); gen_axis(g, 1, 0x9380);
        gen_axis(g, 2, 128); gen_axis(g, 3, -256); gen_axis(g, 4, 2464);
        gen_axis(g, 5, 0x0400); gen_axis(g, 6, 0x1C00); gen_axis(g, 7, 0x0020);
    }
    g->r[25] = (uint8_t)(faulty == 2 ? 0 : faulty == 3 ? 1 : 8);
    if (faulty == 3) g->r[6] |= 0x80; /* Byte6-Bit7 gesetzt (Nutzer 6.11.) */
    gen_emit(g, 0); /* Startzustand */
    for (i = 0; i < 34; i++) {
        if (faulty == 1 && i == 3) { gen_key(g, 300, '\r'); continue; }
        if (faulty == 1 && i == 5) {
            gen_btn(g, 6, 1); gen_emit(g, 300);
            gen_btn(g, 6, 0); gen_emit(g, 150);
            gen_key(g, 300, '\r');
            continue;
        }
        if (faulty == 3 && (i == 0 || i == 19)) {
            /* gleichzeitig Fremdbits (Nutzer 6.11.) -> W143: OK + neutraler Hinweis */
            gen_btn(g, i, 1);
            if (i == 0) gen_btn(g, 1, 1);
            else { gen_btn(g, 25, 1); gen_btn(g, 26, 1); }
            gen_emit(g, 300);
            gen_btn(g, i, 0);
            if (i == 0) gen_btn(g, 1, 0);
            else { gen_btn(g, 25, 0); gen_btn(g, 26, 0); }
            gen_emit(g, 150);
            continue;
        }
        gen_btn(g, i, 1); gen_emit(g, 300);
        gen_btn(g, i, 0); gen_emit(g, 150);
    }
    for (i = SBC_TEST_TOG_FIRST; i < SBC_TEST_TOG_FIRST + 5; i++) {
        gen_btn(g, i, 1); gen_emit(g, 300);
        if (faulty == 1 && i == 36) { gen_key(g, 300, '\r'); continue; }
        if (faulty == 3 && i == 36) {
            /* Nachbarn gehen kurz mit -> Hinweis, Schritt bleibt OK */
            gen_btn(g, 35, 1); gen_btn(g, 37, 1); gen_emit(g, 100);
            gen_btn(g, 35, 0); gen_btn(g, 37, 0); gen_emit(g, 100);
        }
        gen_btn(g, i, 0); gen_emit(g, 300);
    }
    t_end = g->t;
    for (a = 0; a < 8; a++) {
        int ped = sbc_test_axes[a].kind == 2;
        int rest = sbc_test_axis_nominal_rest(a);
        int lo = sbc_test_axes[a].kind == 0 ? 0x0000 : -0x7F00;
        int hi = sbc_test_axes[a].kind == 0 ? 0xFFC0 : 0x7F00;
        if (faulty == 3) {
            /* Nutzer 6.11.: Reibungshebel-Ruhe, Vollhub, Pedal-Ruhe */
            static const int b3rest[8] =
                { 0x5940, 0x9380, 128, -256, 2464, 0x0400, 0x1C00, 0x0020 };
            rest = b3rest[a];
        }
        if (faulty == 1 && a == 0) { lo = 0x0800; hi = 0xF700; } /* Teilhub */
        static const int jit[3] = { -0x60, 0x40, 0x10 };
        if (faulty == 1 && a == 1) { lo = 0x5000; hi = 0xB000; }
        if (faulty == 1 && a == 2) g->r[7] = 0x04;
        /* Ruhe-Schwankung im Messfenster [t_end, t_end+800] */
        g->t = t_end;
        for (i = 0; i < 3; i++) {
            int rv = ped ? (faulty == 3 ? rest + (i ? 0x40 : 0) : (i ? 0x0080 : 0))
                         : rest + jit[i];
            gen_axis(g, a, rv);
            gen_emit(g, 200);
        }
        g->t = t_end + 1100;
        if (ped) {
            static const int p[] = { 0x4000, 0x9000, 0xFF00, 0x6000, 0x0000 };
            for (i = 0; i < 5; i++) {
                gen_axis(g, a, faulty == 3 && i == 4 ? rest : p[i]);
                gen_emit(g, 100);
            }
        } else {
            int seq[7];
            seq[0] = (rest + lo) / 2; seq[1] = lo; seq[2] = rest;
            seq[3] = (rest + hi) / 2; seq[4] = hi; seq[5] = (rest + hi) / 2;
            seq[6] = rest + 0x20;
            for (i = 0; i < 7; i++) {
                int v = seq[i];
                if (faulty == 1 && a == 0) v = (v & ~0xFF) | (0x11 * (i + 1)); /* Low-Bytes */
                if (faulty == 3 && a <= 1 && i == 4) gen_axis(g, 2, a ? 30000 : -30000);
                if (faulty == 3 && a <= 1 && i == 6) gen_axis(g, 2, 128);
                gen_axis(g, a, v);
                gen_emit(g, 100);
            }
        }
        if (faulty == 1 && a == 1) {
            gen_key(g, 300, '\r');
            t_end = g->t;
        } else {
            t_end = g->t + 700; /* Ruhe 600 ms -> Schrittende */
        }
        if (faulty == 1 && a == 2) { g->r[7] = 0x00; }
    }
    /* Tuner */
    g->t = t_end;
    for (i = 1; i <= (faulty == 1 ? 5 : faulty == 3 ? 15 : 12); i++) {
        g->r[24] = (uint8_t)i;
        gen_emit(g, 100);
    }
    if (faulty == 1) { gen_key(g, 300, '\r'); t_end = g->t; }
    else t_end = g->t + 700;
    /* Gang */
    g->t = t_end;
    {
        static const int gs[] = { 7, 8, 9, 10, 11, 12, 13 };
        static const int g2[] = { 254, 255, 1, 2, 3, 4, 5 }; /* echt */
        static const int g3[] = { 254, 5, 254, 255, 1, 2, 3, 4 }; /* Nutzer */
        static const int g4[] = { 7, 8, 9, 10, 11, 12, 6 }; /* ungueltig */
        const int *seq = gs;
        int nseq = 7;
        if (faulty == 2) seq = g2;
        else if (faulty == 3) { seq = g3; nseq = 8; }
        else if (faulty == 4) seq = g4;
        for (i = 0; i < nseq; i++) {
            if (faulty == 1 && gs[i] == 12) continue;
            g->r[25] = 0; gen_emit(g, 100); /* zwischen den Gaengen */
            g->r[25] = (uint8_t)seq[i];
            gen_emit(g, 100);
            g->t += 400; /* Stellung halten (stabil >= 300 ms) */
        }
    }
    if (faulty == 1) { gen_key(g, 300, '\r'); t_end = g->t; }
    else t_end = g->t + 700;
    /* Lampentest: Antwort nach der Folge */
    g->t = t_end + 1000;
    gen_push(g, 'j');
}

static int gen_write_fixture(const char *path, int faulty)
{
    gen_t g;
    FILE *f = NULL;
    unsigned i, j;
    gen_script(&g, faulty);
    if (fopen_s(&f, path, "w") != 0 || !f) {
        printf("FAIL: %s nicht schreibbar\n", path);
        free(g.ev);
        return 1;
    }
    fprintf(f, "# simulierter Ablauf fuer sbc_probe --test --fixture (%s)\n"
               "# erzeugt mit: sbc_probe --gen-fixture %s <datei>\n"
               "# Reports: <Nms> <26 Bytes>; Tasten: '#key <Nms> ENTER|j|n'\n",
            faulty == 1 ? "mit Fehlern" : faulty == 2 ? "echte Gang-Kodierung 254/255/1-5" :
            faulty == 3 ? "Nutzer-Befund 6.11. (blech)" : faulty == 4 ? "ungueltiger Gang-Wert" : "alles korrekt",
            faulty == 1 ? "faulty" : faulty == 2 ? "gearmap" :
            faulty == 3 ? "blech" : faulty == 4 ? "badgear" : "ok");
    for (i = 0; i < g.n; i++) {
        if (g.ev[i].key) {
            fprintf(f, "#key %llums %s\n", (unsigned long long)g.ev[i].t,
                    g.ev[i].key == '\r' ? "ENTER" : g.ev[i].key == 'j' ? "j" : "n");
            continue;
        }
        fprintf(f, "%llums", (unsigned long long)g.ev[i].t);
        for (j = 0; j < SBC_USB_IN_LEN; j++) fprintf(f, " %02X", g.ev[i].raw[j]);
        fprintf(f, "\n");
    }
    fclose(f);
    printf("%s: %u Ereignisse geschrieben\n", path, g.n);
    free(g.ev);
    return 0;
}

static void src_from_gen(src_t *s, gen_t *g)
{
    memset(s, 0, sizeof *s);
    s->ev = g->ev;   /* geht in src_free */
    s->nev = g->n;
    g->ev = NULL;
}

static void temp_path(char *out, size_t cap, const char *name)
{
    DWORD n = GetTempPathA((DWORD)cap, out);
    if (n == 0 || n >= cap) out[0] = '\0';
    strcat_s(out, cap, name);
}

/* Datei komplett lesen (Selbsttest), NULL bei Fehler. */
static char *slurp(const char *path)
{
    FILE *f = NULL;
    long n;
    char *b;
    if (fopen_s(&f, path, "rb") != 0 || !f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (char *)malloc((size_t)n + 1);
    if (b) {
        size_t r = fread(b, 1, (size_t)n, f);
        b[r] = '\0';
    }
    fclose(f);
    return b;
}

/* Zeile zum Schritt-Namen im Ergebnis finden und Status pruefen. */
static int rep_has(const char *rep, const char *step, const char *status)
{
    const char *p = strstr(rep, step);
    char want[16];
    if (!p) return 0;
    _snprintf_s(want, sizeof want, _TRUNCATE, " %s", status);
    {
        const char *eol = strchr(p, '\n');
        const char *q = strstr(p, want);
        return q && (!eol || q < eol);
    }
}

static void selftest_w128(void)
{
    char tmp[MAX_PATH], msg[160], buf[4096];
    int i, rc;

    /* 8. Kalibrierung: Keys, Werte, Umrechnung, Fehler. */
    {
        sbc_cal_t cal;
        sbc_state_t st;
        memset(&cal, 0, sizeof cal);
        CHECK(sbc_usb_cal_set(&cal, "sbc_usb", "1") == 0, "cal fremder key");
        CHECK(sbc_usb_cal_set(&cal, "sbc_cal_aim_x", "3,253") == 1, "cal aim_x zwei-wert");
        CHECK(sbc_usb_cal_set(&cal, "sbc_cal_aim_x", "3,127,253") == SBC_CAL_NOTE, "cal aim_x drei-wert note");
        CHECK(cal.ax[SBC_AX_AIM_X].lo == 3 && cal.ax[SBC_AX_AIM_X].hi == 253, "cal aim_x raender");
        CHECK(sbc_usb_cal_set(&cal, "sbc_dead_aim_x", "3") == SBC_CAL_NOTE, "dead aim_x ignoriert");
        CHECK(cal.ax[SBC_AX_AIM_X].dead == 0, "dead aim_x setzt nichts");
        CHECK(sbc_usb_cal_set(&cal, "sbc_dead_rot", "4") == 1, "dead rot");
        CHECK(sbc_usb_cal_set(&cal, "sbc_cal_pedal_r", "2,250") == 1, "cal pedal_r");
        CHECK(sbc_usb_cal_set(&cal, "sbc_dead_pedal_r", "3") == 1, "dead pedal_r");
        CHECK(sbc_usb_cal_set(&cal, "sbc_cal_aim_y", "200,50") == -1, "cal aim_y lo>hi");
        CHECK(sbc_usb_cal_set(&cal, "sbc_cal_aim_y", "200,100,50") == -1, "cal aim_y unsortiert");
        CHECK(sbc_usb_cal_set(&cal, "sbc_cal_aim_y", "0,300") == -1, "cal aim_y zu gross");
        CHECK(sbc_usb_cal_set(&cal, "sbc_cal_rot", "-200,0,100") == -1, "cal rot zu klein");
        CHECK(sbc_usb_cal_set(&cal, "sbc_cal_foo", "1,2,3") == -1, "cal achse unbekannt");
        CHECK(sbc_usb_cal_set(&cal, "sbc_dead_sight_x", "500") == -1, "dead zu gross");
        CHECK(!cal.ax[SBC_AX_AIM_Y].set && !cal.ax[SBC_AX_SIGHT_X].set, "cal fehler setzt nichts");
        memset(&st, 0, sizeof st);
        st.aiming_x = 129; st.aiming_y = 0x91; st.rot_lever = -3;
        st.pedal_r = 4; st.pedal_m = 77;
        sbc_usb_cal_apply(&cal, &st);
        CHECK(st.aiming_x == 129, "cal aim_x linear ohne Mitte (%u)", st.aiming_x);
        CHECK(st.aiming_y == 0x91, "cal aim_y unberuehrt");
        CHECK(st.rot_lever == 0, "cal rot totzone -> 0 (%d)", st.rot_lever);
        CHECK(st.pedal_r == 0, "cal pedal_r totzone -> 0 (%u)", st.pedal_r);
        CHECK(st.pedal_m == 77, "cal pedal_m unberuehrt");
        st.aiming_x = 253; st.rot_lever = 127; st.pedal_r = 250;
        sbc_usb_cal_apply(&cal, &st);
        CHECK(st.aiming_x == 255 && st.rot_lever == 127 && st.pedal_r == 255,
              "cal raender -> voll (%u %d %u)", st.aiming_x, st.rot_lever, st.pedal_r);
        st.aiming_x = 3; st.rot_lever = -128;
        sbc_usb_cal_apply(&cal, &st);
        CHECK(st.aiming_x == 0 && st.rot_lever == -128, "cal min (%u %d)",
              st.aiming_x, st.rot_lever);
        st.aiming_x = 190; /* linear 3..253 -> 0..255: 191 */
        sbc_usb_cal_apply(&cal, &st);
        CHECK(st.aiming_x == 191, "cal linear (%u)", st.aiming_x);
        sbc_usb_cal_apply(NULL, &st);
        sbc_usb_cal_apply(&cal, NULL);
    }

    /* 9. Lampenfolge gegen das OUT-Format (Mock statt Geraet). */
    {
        src_t s;
        sbc_test_io_t io;
        memset(&s, 0, sizeof s);
        pio_make(&s, &io);
        g_quiet = 1;
        rc = sbc_test_led_seq_run(&io, 600, 1);
        g_quiet = 0;
        CHECK(rc == SBC_OK, "ledseq rc");
        CHECK(s.nout == SBC_TEST_LED_SEQ_N + 1, "ledseq %u Reports", s.nout);
        CHECK(s.vt == (uint64_t)SBC_TEST_LED_SEQ_N * 600, "ledseq Dauer %llu", (unsigned long long)s.vt);
        CHECK(sbc_test_led_seq_verify(&io, 1, msg, sizeof msg) == 0, "ledseq verify: %s", msg);
        if (s.nout > 7) {
            /* Lampe 7 (Index 6) = ModeSelect, Byte 5 low; Gear R = Byte 17 high */
            CHECK(s.outs[6][5] == 0x0F, "ledseq Lampe 7 Byte5=%02X", s.outs[6][5]);
            CHECK(s.outs[SBC_LED_GEAR_R][17] == 0xF0, "ledseq GearR Byte17");
            CHECK(s.outs[SBC_LED_COUNT][17] == 0x0F, "ledseq reserviert 17low");
            CHECK(s.outs[SBC_LED_COUNT + 1][21] == 0x0F, "ledseq Byte21");
            s.outs[3][3] = 0xFF; /* Fehler einbauen: zwei Lampen */
            CHECK(sbc_test_led_seq_verify(&io, 1, msg, sizeof msg) > 0, "ledseq verify erkennt Fehler");
        }
        src_free(&s);
    }

    /* 10. Dump-Markierung + Feld-Diff. */
    {
        uint8_t a[SBC_USB_RAW_MAX], b[SBC_USB_RAW_MAX];
        char mk[3 * SBC_USB_RAW_MAX + 1];
        memset(a, 0, sizeof a);
        memcpy(a, kNeutral, SBC_USB_IN_LEN);
        memcpy(b, a, sizeof b);
        b[2] = 0x02;     /* Fire */
        b[7] = 0x04;     /* reserviert */
        b[8] = 0x33;     /* nur Low-Byte Aiming X */
        b[25] = 0x09;    /* Gang 1 */
        mark_row(a, b, SBC_USB_IN_LEN, 1, mk);
        CHECK(strncmp(mk + 3 * 2, "^^", 2) == 0, "mark Byte2 ^^");
        CHECK(strncmp(mk + 3 * 7, "!!", 2) == 0, "mark Byte7 !!");
        CHECK(strncmp(mk + 3 * 8, "!!", 2) == 0, "mark Byte8 !!");
        CHECK(strncmp(mk + 3 * 10, "??", 2) == 0, "mark Byte10 ??");
        CHECK(strncmp(mk + 3 * 9, "  ", 2) == 0, "mark Byte9 leer");
        CHECK(strncmp(mk + 3 * 25, "^^", 2) == 0, "mark Byte25 ^^");
        i = diff_fields(a, b, buf, sizeof buf);
        CHECK(i == 4, "diff %d Felder", i);
        CHECK(strstr(buf, "Fire (bi1): AN") != NULL, "diff Fire");
        CHECK(strstr(buf, "Byte7 (RESERVIERT") != NULL, "diff Byte7");
        CHECK(strstr(buf, "NUR Low-Byte") != NULL, "diff Low-Byte");
        CHECK(strstr(buf, "Gang: 8 (N) -> 9 (1)") != NULL, "diff Gang");
        CHECK(diff_fields(a, a, buf, sizeof buf) == 0, "diff gleich = 0");
    }

    /* 11. --watch/--dump aus simulierten Reports (Mitschrift pruefen). */
    {
        gen_t g;
        src_t s;
        char *txt;
        memset(&g, 0, sizeof g);
        g.r[1] = SBC_USB_IN_LEN; g.r[9] = 0x80; g.r[11] = 0x80; g.r[25] = 8;
        gen_emit(&g, 0);
        gen_emit(&g, 10);                       /* unveraendert */
        gen_btn(&g, 18, 1); gen_emit(&g, 10);   /* Chaff */
        g.r[7] = 0x80; gen_emit(&g, 10);        /* reserviert */
        g.r[22] = 0x10; gen_emit(&g, 10);       /* Low-Byte Gas */
        g.r[24] = 0x35; gen_emit(&g, 10);       /* Tuner 5 + High-Nibble 3 */
        src_from_gen(&s, &g);
        temp_path(tmp, sizeof tmp, "w128_watch_selftest.txt");
        g_tee = NULL;
        if (fopen_s(&g_tee, tmp, "w") == 0 && g_tee) {
            g_quiet = 1;
            rc = cmd_dump(&s, 0, 1);
            g_quiet = 0;
            fclose(g_tee);
            g_tee = NULL;
            txt = slurp(tmp);
            CHECK(rc == 0, "watch rc");
            CHECK(txt && strstr(txt, "Chaff (bi18): AN"), "watch Chaff");
            CHECK(txt && strstr(txt, "Byte7 (RESERVIERT, Inhalt unbekannt): 00 -> 80"), "watch Byte7");
            CHECK(txt && strstr(txt, "Pedal rechts (Gas): 0x0000 -> 0x0010") && strstr(txt, "NUR Low-Byte"), "watch Low-Byte");
            CHECK(txt && strstr(txt, "Tuner: 0 -> 5") && strstr(txt, "Byte24 High-Nibble"), "watch Tuner/Nibble");
            CHECK(txt && strstr(txt, "Byte 7 (reserviert): 00 80") && strstr(txt, "bitte melden"), "watch Summe Byte7");
            /* nur Aenderungen: 4 Zeitstempel-Bloecke + Startzustand */
            {
                int nt = 0;
                const char *p = txt;
                while (p && (p = strstr(p, "t=")) != NULL) { nt++; p += 2; }
                CHECK(nt == 5, "watch %d Bloecke (5 erwartet)", nt);
            }
            free(txt);
        } else {
            CHECK(0, "watch temp-Datei");
        }
        src_free(&s);
        DeleteFileA(tmp);
        /* --dump 3: genau 3 Reports, Markerzeile */
        memset(&g, 0, sizeof g);
        g.r[1] = SBC_USB_IN_LEN; g.r[9] = 0x80; g.r[11] = 0x80; g.r[25] = 8;
        gen_emit(&g, 0);
        gen_btn(&g, 0, 1); gen_emit(&g, 10);
        gen_emit(&g, 10);
        gen_emit(&g, 10);
        src_from_gen(&s, &g);
        if (fopen_s(&g_tee, tmp, "w") == 0 && g_tee) {
            g_quiet = 1;
            rc = cmd_dump(&s, 3, 0);
            g_quiet = 0;
            fclose(g_tee);
            g_tee = NULL;
            txt = slurp(tmp);
            CHECK(txt && strstr(txt, "[2] t=") && !strstr(txt, "[3] t="), "dump 3 Reports");
            CHECK(txt && strstr(txt, "^^") && strstr(txt, "MainWeapon (bi0): AN"), "dump Markierung");
            free(txt);
        }
        src_free(&s);
        DeleteFileA(tmp);
    }

    /* 12. --test: simulierter Ablauf, alles korrekt -> 50/50 OK + cfg-Vorschlag. */
    {
        gen_t g;
        src_t s;
        sbc_test_sum_t sum;
        sbc_test_io_t io;
        char *txt;
        gen_script(&g, 0);
        src_from_gen(&s, &g);
        pio_make(&s, &io);
        temp_path(tmp, sizeof tmp, "w128_test_ok.txt");
        memset(&sum, 0, sizeof sum);
        g_quiet = 1;
        rc = sbc_test_run_all(&io, tmp, "Simulation (Selbsttest ok)", "sbc_probe --test", PROBE_VERSION, &sum);
        g_quiet = 0;
        CHECK(sum.steps == SBC_TEST_N_STEPS, "test %d Schritte", sum.steps);
        CHECK(rc == 0 && sum.n[SBC_TR_OK] == SBC_TEST_N_STEPS, "test ok: OK %d/%d (fehlt %d, unerw %d, teilw %d)",
              sum.n[SBC_TR_OK], sum.steps, sum.n[SBC_TR_MISSING], sum.n[SBC_TR_UNEXPECTED], sum.n[SBC_TR_PARTIAL]);
        CHECK(sbc_test_led_seq_verify(&io, 1, msg, sizeof msg) == 0, "test led-OUT: %s", msg);
        txt = slurp(tmp);
        CHECK(txt != NULL, "test Ergebnisdatei");
        if (txt) {
            char *p = strstr(txt, "[input]");
            int nl = 0, bad = 0;
            CHECK(strstr(txt, "u16 bestaetigt") != NULL, "test Aiming-Urteil u16");
            CHECK(strstr(txt, "Reibungshebel: Ruhe = beliebige Stellung (korrekt)") != NULL, "test Reibungshebel");
            CHECK(strstr(txt, "sbc_cal_aim_") == NULL && strstr(txt, "sbc_dead_aim_") == NULL, "test kein Aiming-Vorschlag (Vollhub)");
            CHECK(strstr(txt, "sbc_cal_rot=-125,0,125") != NULL, "test Vorschlag rot (Mitte gerundet)");
            CHECK(strstr(txt, "sbc_cal_pedal_r=0,253") != NULL, "test Vorschlag pedal_r");
            CHECK(strstr(txt, "Byte 7 (reserviert): 00  (immer 00") != NULL, "test Byte7 immer 00");
            /* jede Vorschlagszeile muss das Modul fehlerfrei annehmen */
            while (p && (p = strstr(p, "\nsbc_")) != NULL) {
                char k[64], v[64];
                sbc_cal_t cal;
                p++;
                memset(&cal, 0, sizeof cal);
                if (sscanf_s(p, "%63[^=]=%63s", k, (unsigned)sizeof k, v, (unsigned)sizeof v) == 2) {
                    if (strcmp(k, "sbc_usb") == 0) continue;
                    nl++;
                    {
                        int scr = sbc_usb_cal_set(&cal, k, v);
                        if (scr != 1 && scr != SBC_CAL_NOTE) bad++;
                    }
                }
            }
            CHECK(nl == 12 && bad == 0, "test Vorschlaege %d Zeilen, %d ungueltig", nl, bad);
            free(txt);
        }
        src_free(&s);
        DeleteFileA(tmp);
    }

    /* 13. --test: definierte Fehler werden richtig eingeordnet. */
    {
        gen_t g;
        src_t s;
        sbc_test_sum_t sum;
        sbc_test_io_t io;
        char *txt;
        gen_script(&g, 1);
        src_from_gen(&s, &g);
        pio_make(&s, &io);
        temp_path(tmp, sizeof tmp, "w128_test_faulty.txt");
        memset(&sum, 0, sizeof sum);
        g_quiet = 1;
        rc = sbc_test_run_all(&io, tmp, "Simulation (Selbsttest Fehler)", "sbc_probe --test", PROBE_VERSION, &sum);
        g_quiet = 0;
        CHECK(rc == 1, "faulty rc=%d", rc);
        txt = slurp(tmp);
        CHECK(txt != NULL, "faulty Ergebnisdatei");
        if (txt) {
            CHECK(rep_has(txt, "Taste Eject (bi3)", "fehlt") && strstr(txt, "uebersprungen (Enter)"), "faulty bi3 fehlt");
            CHECK(rep_has(txt, "Taste Ignition (bi5)", "unerwartet") && strstr(txt, "kam stattdessen: Start(bi6)"), "faulty bi5 unerwartet");
            CHECK(rep_has(txt, "Taste Start (bi6)", "OK"), "faulty bi6 ok");
            CHECK(rep_has(txt, "Schalter Tog FuelFlowRate (bi36)", "teilweise"), "faulty bi36 teilweise");
            CHECK(rep_has(txt, "Achse Aiming Y", "teilweise"), "faulty aim_y teilweise");
            CHECK(rep_has(txt, "Achse Aiming X", "OK"), "faulty aim_x ok");
            CHECK(rep_has(txt, "Tuner-Rad", "teilweise"), "faulty tuner teilweise");
            CHECK(rep_has(txt, "Schalthebel", "teilweise") && strstr(txt, "fehlen: 4"), "faulty gang 4 fehlt");
            CHECK(rep_has(txt, "Lampentest (OUT)", "OK"), "faulty led ok");
            CHECK(strstr(txt, "Byte 7 (reserviert): 00 04") != NULL, "faulty Byte7 gesehen");
            CHECK(strstr(txt, "Low-Byte Aiming X") && strstr(txt, "traegt Feinaufloesung"), "faulty Low-Bytes");
            CHECK(strstr(txt, "sbc_cal_aim_y=") == NULL && strstr(txt, "sbc_dead_aim_y=") == NULL,
                  "faulty aim_y: keine Aiming-Zeilen (teilweise)");
            CHECK(strstr(txt, "sbc_cal_aim_x=10,245") != NULL && strstr(txt, "sbc_dead_aim_x=") == NULL,
                  "faulty aim_x Teilhub-Vorschlag (Zwei-Wert, ohne Totzone)");
            CHECK(sum.n[SBC_TR_OK] == SBC_TEST_N_STEPS - 6, "faulty OK %d (erwartet %d)", sum.n[SBC_TR_OK], SBC_TEST_N_STEPS - 6);
            free(txt);
        }
        src_free(&s);
        DeleteFileA(tmp);
    }

    /* 15. Abweichende Gang-Kodierung -> Vorschlag sbc_gear_map + Umrechnung. */
    {
        gen_t g;
        src_t s;
        sbc_test_sum_t sum;
        sbc_test_io_t io;
        char *txt;
        sbc_cal_t cal;
        sbc_state_t st;
        gen_script(&g, 2);
        src_from_gen(&s, &g);
        pio_make(&s, &io);
        temp_path(tmp, sizeof tmp, "w128_test_gearmap.txt");
        memset(&sum, 0, sizeof sum);
        g_quiet = 1;
        rc = sbc_test_run_all(&io, tmp, "Simulation (Gang-Kodierung)", "sbc_probe --test", PROBE_VERSION, &sum);
        g_quiet = 0;
        txt = slurp(tmp);
        CHECK(txt && rep_has(txt, "Schalthebel", "OK") && strstr(txt, "gleichwertig"), "gearmap Schalthebel OK (nativ)");
        CHECK(txt && strstr(txt, "R=254 N=255 1=1 2=2 3=3 4=4 5=5") != NULL, "gearmap Stellungen");
        CHECK(txt && strstr(txt, "sbc_gear_map=") == NULL, "gearmap kein Vorschlag (Spiel versteht nativ)");
        CHECK(sum.n[SBC_TR_OK] == SBC_TEST_N_STEPS, "gearmap alles OK (%d)", sum.n[SBC_TR_OK]);
        free(txt);
        src_free(&s);
        DeleteFileA(tmp);
        memset(&cal, 0, sizeof cal);
        CHECK(sbc_usb_cal_set(&cal, "sbc_gear_map", "254,255,1,2,3,4,5") == 1, "gearmap set");
        CHECK(sbc_usb_cal_set(&cal, "sbc_gear_map", "1,1,2,3,4,5,6") == -1, "gearmap doppelt");
        CHECK(sbc_usb_cal_set(&cal, "sbc_gear_map", "1,2,3") == -1, "gearmap zu kurz");
        memset(&st, 0, sizeof st);
        st.gear = 254; sbc_usb_cal_apply(&cal, &st);
        CHECK(st.gear == 7, "gearmap R (%u)", st.gear);
        st.gear = 255; sbc_usb_cal_apply(&cal, &st);
        CHECK(st.gear == 8, "gearmap N (%u)", st.gear);
        st.gear = 5; sbc_usb_cal_apply(&cal, &st);
        CHECK(st.gear == 13, "gearmap 5 (%u)", st.gear);
        st.gear = 9; sbc_usb_cal_apply(&cal, &st);
        CHECK(st.gear == 0, "gearmap unbekannt -> dazwischen (%u)", st.gear);
    }

    /* 16. Auto-Kalibrierung mit den Ruhewerten vom Blech (Nutzer 5.11.:
     * aim=76 7C rot=0 sight=0,0 ped=03/02 17 00). */
    {
        sbc_autocal_t ac;
        sbc_cal_t cfg, eff;
        sbc_state_t st, r;
        int k, fin = 0;
        memset(&ac, 0, sizeof ac);
        memset(&cfg, 0, sizeof cfg);
        memset(&st, 0, sizeof st);
        st.aiming_x = 0x76; st.aiming_y = 0x7C; st.pedal_m = 0x17; st.gear = 3;
        for (k = 0; k < SBC_AC_WIN + 5 && !fin; k++) {
            st.pedal_l = (uint8_t)(k & 1 ? 3 : 2); /* Rauschen 02/03 */
            fin = sbc_usb_autocal_feed(&ac, &st);
        }
        CHECK(fin && ac.stable, "autocal fertig+ruhig (%d %d)", fin, ac.stable);
        CHECK(ac.rest[SBC_AX_AIM_X] == 0x76 && ac.rest[SBC_AX_AIM_Y] == 0x7C &&
              ac.rest[SBC_AX_PEDAL_M] == 0x17, "autocal Ruhe %d %d %d",
              ac.rest[0], ac.rest[1], ac.rest[6]);
        CHECK(sbc_usb_cal_set(&cfg, "sbc_cal_aim_y", "10,128,250") == SBC_CAL_NOTE, "autocal cfg aim_y");
        sbc_usb_autocal_build(&ac, &cfg, 6, 16, &eff);
        r = st;
        sbc_usb_cal_apply(&eff, &r);
        CHECK(!eff.ax[SBC_AX_AIM_X].set && r.aiming_x == 0x76, "autocal aim_x unberuehrt (%02X)", r.aiming_x);
        CHECK(eff.ax[SBC_AX_AIM_Y].lo == 10 && eff.ax[SBC_AX_AIM_Y].hi == 250,
              "autocal cfg aim_y gewinnt (mid ignoriert)");
        CHECK(r.pedal_m == 0 && r.pedal_l == 0 && r.pedal_r == 0,
              "autocal Pedale Ruhe -> 0 (%u %u %u)", r.pedal_l, r.pedal_m, r.pedal_r);
        r = st; r.pedal_m = 0xFF; r.aiming_x = 0xFF;
        sbc_usb_cal_apply(&eff, &r);
        CHECK(r.pedal_m == 0xFF && r.aiming_x == 0xFF, "autocal voll (%u %02X)", r.pedal_m, r.aiming_x);
        r = st; r.aiming_x = 0x00;
        sbc_usb_cal_apply(&eff, &r);
        CHECK(r.aiming_x == 0x00, "autocal min (%02X)", r.aiming_x);
        r = st; r.aiming_x = 0x76 + 5; /* keine Totzone mehr */
        sbc_usb_cal_apply(&eff, &r);
        CHECK(r.aiming_x == 0x7B, "autocal keine Totzone (%02X)", r.aiming_x);
        CHECK(r.gear == 3, "autocal Gang unberuehrt");
        /* unruhig (federnde Achse waehrend Start bewegt): gibt nach
         * SBC_AC_TRIES auf, keine Achse gesetzt. Aiming allein stoert
         * nicht mehr (Reibungshebel) - darum Rotation bewegen. */
        memset(&ac, 0, sizeof ac);
        fin = 0;
        for (k = 0; k < 400 && !fin; k++) {
            st.rot_lever = (int8_t)(k & 1 ? 0x20 : 0x60);
            fin = sbc_usb_autocal_feed(&ac, &st);
        }
        memset(&cfg, 0, sizeof cfg);
        sbc_usb_autocal_build(&ac, &cfg, 6, 16, &eff);
        CHECK(fin && !ac.stable && !eff.ax[SBC_AX_AIM_X].set, "autocal unruhig -> aufgegeben");
        /* Reibungshebel-Ruhe (0x20) erzeugt nie eine Mitte, Pedal schon */
        memset(&ac, 0, sizeof ac);
        st.aiming_x = 0x20;
        for (k = 0; k < SBC_AC_WIN; k++) sbc_usb_autocal_feed(&ac, &st);
        sbc_usb_autocal_build(&ac, &cfg, 6, 16, &eff);
        CHECK(ac.stable && !eff.ax[SBC_AX_AIM_X].set && eff.ax[SBC_AX_PEDAL_M].set,
              "autocal Reibungshebel ohne Mitte, Pedal uebernommen");
    }

    /* 17. Nutzer-Befund 6.11. nachgebaut (blech): Reibungshebel-Ruhe 0x59/0x93
     * bei Vollhub, Gang-Reihenfolge 1 254 5 254 255 1 2 3 4, Byte6-Bit7. */
    {
        gen_t g;
        src_t s;
        sbc_test_sum_t sum;
        sbc_test_io_t io;
        char *txt;
        gen_script(&g, 3);
        src_from_gen(&s, &g);
        pio_make(&s, &io);
        temp_path(tmp, sizeof tmp, "w141_test_blech.txt");
        memset(&sum, 0, sizeof sum);
        g_quiet = 1;
        rc = sbc_test_run_all(&io, tmp, "Simulation (Nutzer 6.11.)", "sbc_probe --test", PROBE_VERSION, &sum);
        g_quiet = 0;
        /* W143: mitgedrueckte Tasten sind neutral (OK + Hinweis), kein "unerwartet". */
        CHECK(rc == 0 && sum.n[SBC_TR_OK] == SBC_TEST_N_STEPS && sum.n[SBC_TR_UNEXPECTED] == 0,
              "blech OK %d/%d (unerw %d)", sum.n[SBC_TR_OK], sum.steps, sum.n[SBC_TR_UNEXPECTED]);
        txt = slurp(tmp);
        CHECK(txt != NULL, "blech Ergebnisdatei");
        if (txt) {
            CHECK(rep_has(txt, "Taste MainWeapon (bi0)", "OK") &&
                  strstr(txt, "weitere Tasten gesehen: Fire(bi1)"), "blech bi0+bi1 gleichzeitig");
            CHECK(rep_has(txt, "Taste TankDetach (bi19)", "OK") &&
                  strstr(txt, "weitere Tasten gesehen: MainWeaponControl(bi25), SubWeaponControl(bi26)"),
                  "blech bi19+WC gleichzeitig");
            CHECK(rep_has(txt, "Schalter Tog FuelFlowRate (bi36)", "OK") &&
                  strstr(txt, "Hinweis: Nachbar-Schalter gingen mit") &&
                  strstr(txt, "Tog OxygenSupply(bi35)") && strstr(txt, "Tog BufferMaterial(bi37)"),
                  "blech Nachbar-Schalter Hinweis");
            CHECK(rep_has(txt, "Achse Aiming X", "OK") &&
                  strstr(txt, "Hinweis: Rotation bewegte mit"), "blech aim_x Hinweis");
            CHECK(rep_has(txt, "Achse Aiming Y", "OK"), "blech aim_y OK");
            CHECK(strstr(txt, "Reibungshebel: Ruhe = beliebige Stellung (korrekt)") != NULL,
                  "blech Reibungshebel-Urteil");
            CHECK(strstr(txt, "Ruhe weit neben") == NULL, "blech keine Ruhe-Warnung");
            CHECK(strstr(txt, "sbc_cal_aim_") == NULL && strstr(txt, "sbc_dead_aim_") == NULL,
                  "blech kein Aiming-Vorschlag (Vollhub)");
            CHECK(rep_has(txt, "Schalthebel", "OK") &&
                  strstr(txt, "R=254 N=255 1=1 2=2 3=3 4=4 5=5") &&
                  strstr(txt, "Reihenfolge: 1 254 5 254 255 1 2 3 4"),
                  "blech Gang nativ OK");
            CHECK(strstr(txt, "sbc_gear_map=") == NULL, "blech kein Gang-Vorschlag");
            CHECK(strstr(txt, "bekannt, wird ignoriert") != NULL, "blech Byte6-Bit7 bekannt");
            CHECK(strstr(txt, "16 Stellungen") != NULL && strstr(txt, "Blende 13-15 gesehen: 13 14 15") != NULL,
                  "blech Tuner mit Blende");
            CHECK(strstr(txt, "sbc_cal_pedal_m=28,253") != NULL &&
                  strstr(txt, "sbc_dead_pedal_m=2") != NULL, "blech Pedal-Vorschlag");
            free(txt);
        }
        src_free(&s);
        DeleteFileA(tmp);
    }

    /* 18. Ungueltiger Gang-Wert (badgear): 6 statt 5 -> Vorschlag. */
    {
        gen_t g;
        src_t s;
        sbc_test_sum_t sum;
        sbc_test_io_t io;
        char *txt;
        gen_script(&g, 4);
        src_from_gen(&s, &g);
        pio_make(&s, &io);
        temp_path(tmp, sizeof tmp, "w141_test_badgear.txt");
        memset(&sum, 0, sizeof sum);
        g_quiet = 1;
        rc = sbc_test_run_all(&io, tmp, "Simulation (Gang ungueltig)", "sbc_probe --test", PROBE_VERSION, &sum);
        g_quiet = 0;
        CHECK(sum.n[SBC_TR_OK] == SBC_TEST_N_STEPS - 1, "badgear OK %d (erwartet %d)", sum.n[SBC_TR_OK], SBC_TEST_N_STEPS - 1);
        txt = slurp(tmp);
        CHECK(txt && rep_has(txt, "Schalthebel", "unerwartet") &&
              strstr(txt, "Werte ausser 7..13: 6"), "badgear Schalthebel unerwartet");
        CHECK(txt && strstr(txt, "sbc_gear_map=7,8,9,10,11,12,6") != NULL, "badgear Vorschlag");
        free(txt);
        src_free(&s);
        DeleteFileA(tmp);
    }

    /* 19. W143: Gang-Anzeigenamen (Doku 7..13 + echte Bytes 254/255/1-5). */
    {
        CHECK(strcmp(sbc_test_gear_name(7), "R") == 0, "gear_name 7 = R");
        CHECK(strcmp(sbc_test_gear_name(13), "5") == 0, "gear_name 13 = 5");
        CHECK(strcmp(sbc_test_gear_name(0), "dazwischen") == 0, "gear_name 0");
        CHECK(strcmp(sbc_test_gear_name(254), "R") == 0, "gear_name 254 = R");
        CHECK(strcmp(sbc_test_gear_name(255), "N") == 0, "gear_name 255 = N");
        CHECK(strcmp(sbc_test_gear_name(1), "1") == 0, "gear_name 1");
        CHECK(strcmp(sbc_test_gear_name(5), "5") == 0, "gear_name 5");
        CHECK(strcmp(sbc_test_gear_name(6), "?") == 0, "gear_name 6 = ?");
    }

    /* 20. W143: Abbruch waehrend der Lampenfolge -> c->aborted, kein Schein-Befund. */
    {
        src_t s;
        sbc_test_io_t io;
        sbc_test_ctx_t ctx;
        sbc_test_res_t r;
        memset(&s, 0, sizeof s);
        memset(&r, 0, sizeof r);
        r.status = SBC_TR_NOTRUN;
        pio_make(&s, &io);
        sbc_test_ctx_init(&ctx, &io);
        g_quiet = 1;
        g_abort = 1; /* Folge bricht sofort ab */
        sbc_test_step_led(&ctx, &r);
        g_abort = 0;
        g_quiet = 0;
        CHECK(ctx.aborted == 1, "led-abort ctx.aborted");
        CHECK(r.status == SBC_TR_NOTRUN, "led-abort kein Befund (%d)", r.status);
        src_free(&s);
    }

    /* 14. --test ohne Daten: endet sauber, alles "fehlt"/Abbruch, keine Haenger. */
    {
        src_t s;
        sbc_test_sum_t sum;
        sbc_test_io_t io;
        memset(&s, 0, sizeof s);
        memset(&sum, 0, sizeof sum);
        pio_make(&s, &io);
        g_quiet = 1;
        rc = sbc_test_run_all(&io, NULL, "leer", "sbc_probe --test", PROBE_VERSION, &sum);
        g_quiet = 0;
        CHECK(rc == 1 && sum.steps == 0, "test leer rc=%d", rc);
        src_free(&s);
    }
}

/* ---- Fixture-Pruefung (ohne Geraet, W75) ---- */

/* Datei laden, jeden Eintrag ueber denselben Pfad wie im Spiel schicken
 * (parse -> to_block) und zeigen. Exit 0 = Datei ok (>= 1 Eintrag). */
static int cmd_check(const char *path)
{
    sbc_fixture_t fx;
    unsigned i;
    int rc = sbc_fixture_load(path, &fx);
    if (rc != 0) {
        printf("FAIL: %s: %s\n", path, sbc_fixture_strerror(rc));
        return 1;
    }
    printf("%s: %u Eintraege, %u Anker, %u Zeilen uebersprungen\n",
           path, fx.n, fx.na, fx.skipped);
    for (i = 0; i < fx.na; i++) {
        printf("  anker %u: %s addrs=%u\n", i,
               fx.a[i].is_memall ? "memall" : "mem", fx.a[i].naddr);
    }
    for (i = 0; i < fx.n; i++) {
        sbc_state_t st;
        uint8_t blk[24];
        if (sbc_usb_parse_report(fx.e[i].raw, &st) != SBC_OK) {
            printf("FAIL: eintrag %u: parse-Fehler\n", i);
            sbc_fixture_free(&fx);
            return 1;
        }
        sbc_usb_state_to_block(&st, blk);
        printf("[%u] %s%llu anker=%d ", i, fx.e[i].is_ms ? "ms" : "f",
               (unsigned long long)fx.e[i].when, fx.e[i].anchor);
        print_state(&st);
        printf("     blk: ");
        print_hex(blk, sizeof blk);
    }
    sbc_fixture_free(&fx);
    return 0;
}

/* ---- Geraete-Modi (W49): Standard-Poll 3 s und --leds <0..15> ---- */

static int cmd_live(int led_level /* -1 = aus */)
{
    sbc_usb_dev_t *dev = NULL;
    int rc = sbc_usb_open(&dev);
    if (rc == SBC_ERR_NODEVICE) {
        printf("sbc_probe: no device (0A7B:D000 not found or no WinUSB binding).\n");
        printf("See CONTROLLER.md section 7 (Zadig) and 10 (checks); "
               "guided check: sbc_probe --test\n");
        return 2;
    }
    if (rc != SBC_OK) {
        printf("sbc_probe: open failed: %s\n", sbc_usb_strerror(rc));
        return 3;
    }
    printf("sbc_probe: device open.\n");
    if (led_level >= 0) {
        sbc_leds_t leds;
        sbc_usb_leds_all(&leds, (uint8_t)led_level);
        rc = sbc_usb_set_leds(dev, &leds);
        printf("sbc_probe: set_leds(%d) -> %s\n", led_level, sbc_usb_strerror(rc));
        sbc_usb_close(dev);
        return rc == SBC_OK ? 0 : 1;
    }
    {
        ULONGLONG t0 = GetTickCount64();
        sbc_state_t last;
        int have = 0, n = 0;
        printf("sbc_probe: polling 3 s (press buttons, move sticks)...\n");
        while (GetTickCount64() - t0 < 3000) {
            sbc_state_t st;
            rc = sbc_usb_poll(dev, &st);
            if (rc == SBC_ERR_TIMEOUT) {
                continue;
            }
            if (rc != SBC_OK) {
                printf("sbc_probe: poll -> %s\n", sbc_usb_strerror(rc));
                sbc_usb_close(dev);
                return 1;
            }
            n++;
            if (!have || memcmp(&last, &st, sizeof st) != 0) {
                last = st;
                have = 1;
                printf("[%d] ", n);
                print_state(&st);
            }
        }
        printf("sbc_probe: %d reports in 3 s.\n", n);
    }
    sbc_usb_close(dev);
    return 0;
}

static void usage(void)
{
    printf("sbc_probe (%s) - Original-Steel-Battalion-Controller am PC pruefen\n\n"
           "  sbc_probe                 Geraet suchen, 3 s Tasten/Achsen zeigen\n"
           "  sbc_probe --test          gefuehrte Pruefung aller Elemente (ca. 5 min),\n"
           "                            Ergebnis: sbc_test_<datum>.txt neben der exe\n"
           "  sbc_probe --leds seq      alle Lampen nacheinander (je 0,6 s, mit Namen)\n"
           "  sbc_probe --leds [0..15]  alle Lampen gleich hell (0 = aus)\n"
           "  sbc_probe --dump [N]      N Roh-Reports, unsichere Bytes markiert (Default 5)\n"
           "  sbc_probe --watch         dauerhaft nur Aenderungen (Enter = Ende),\n"
           "                            Mitschrift: sbc_watch_<datum>.txt neben der exe\n"
           "  sbc_probe --selftest      Selbsttest ohne Geraet\n"
           "  sbc_probe --check-fixture <datei>\n"
           "Ohne Geraet: --test/--dump/--watch --fixture <datei> [--out <datei>],\n"
           "             --leds seq --mock, --gen-fixture ok|faulty|gearmap|blech|badgear <datei>\n"
           "Exit: 0 ok, 1 Fehler/nicht alles OK, 2 kein Geraet, 3 WinUSB fehlt (Zadig)\n",
           PROBE_VERSION);
}

static const char *arg_val(int argc, char **argv, const char *flag)
{
    int i;
    for (i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], flag) == 0) {
            return argv[i + 1];
        }
    }
    return NULL;
}

static int arg_has(int argc, char **argv, const char *flag)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], flag) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Quelle fuer --test/--dump/--watch: Fixture (Simulation) oder Geraet.
 * Rueckgabe 0 = ok, sonst Exit-Code. */
static int open_source(src_t *s, const char *fixture)
{
    if (fixture) {
        return src_open_fixture(s, fixture) ? 1 : 0;
    }
    return src_open_live(s);
}

int main(int argc, char **argv)
{
    const char *fixture = arg_val(argc, argv, "--fixture");
    const char *outp = arg_val(argc, argv, "--out");
    int rc;
    src_t s;

    SetConsoleCtrlHandler(on_ctrl, TRUE);
    if (argc >= 2 && strcmp(argv[1], "--check-fixture") == 0) {
        if (argc < 3) {
            printf("usage: sbc_probe --check-fixture <datei>\n");
            return 1;
        }
        return cmd_check(argv[2]);
    }
    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0) {
        return run_selftest();
    }
    if (argc >= 2 && strcmp(argv[1], "--gen-fixture") == 0) {
        if (argc < 4 || (strcmp(argv[2], "ok") != 0 && strcmp(argv[2], "faulty") != 0 &&
                          strcmp(argv[2], "gearmap") != 0 && strcmp(argv[2], "blech") != 0 &&
                          strcmp(argv[2], "badgear") != 0)) {
            printf("usage: sbc_probe --gen-fixture ok|faulty|gearmap|blech|badgear <datei>\n");
            return 1;
        }
        return gen_write_fixture(argv[3], strcmp(argv[2], "faulty") == 0 ? 1 :
                                          strcmp(argv[2], "gearmap") == 0 ? 2 :
                                          strcmp(argv[2], "blech") == 0 ? 3 :
                                          strcmp(argv[2], "badgear") == 0 ? 4 : 0);
    }
    if (argc >= 2 && strcmp(argv[1], "--test") == 0) {
        char desc[600];
        if (fixture) {
            _snprintf_s(desc, sizeof desc, _TRUNCATE, "Simulation aus %s", fixture);
        } else {
            strcpy_s(desc, sizeof desc, "Geraet 0A7B:D000 (WinUSB)");
        }
        rc = open_source(&s, fixture);
        if (rc) {
            return rc;
        }
        {
            sbc_test_io_t io;
            pio_make(&s, &io);
            rc = sbc_test_run_all(&io, outp, desc, "sbc_probe --test", PROBE_VERSION, NULL);
        }
        src_free(&s);
        return rc;
    }
    if (argc >= 2 && (strcmp(argv[1], "--dump") == 0 || strcmp(argv[1], "--watch") == 0)) {
        int watch = strcmp(argv[1], "--watch") == 0 || arg_has(argc, argv, "--watch");
        int n = (argc >= 3 && argv[2][0] != '-') ? atoi(argv[2]) : 5;
        char path[MAX_PATH];
        if (n <= 0) {
            n = 5;
        }
        if (!arg_has(argc, argv, "--no-color")) {
            enable_color();
        }
        rc = open_source(&s, fixture);
        if (rc) {
            return rc;
        }
        path[0] = '\0';
        if (outp) {
            strcpy_s(path, sizeof path, outp);
        } else if (watch && !fixture) {
            char ts[32], fn[64];
            sbc_test_stamp(ts, sizeof ts, 1);
            _snprintf_s(fn, sizeof fn, _TRUNCATE, "sbc_watch_%s.txt", ts);
            path_next_to_exe(path, sizeof path, fn);
        }
        if (path[0] && fopen_s(&g_tee, path, "w") != 0) {
            g_tee = NULL;
        }
        rc = cmd_dump(&s, n, watch);
        if (g_tee) {
            fclose(g_tee);
            g_tee = NULL;
            printf("Mitschrift: %s\n", path);
        }
        src_free(&s);
        return rc;
    }
    if (argc >= 2 && strcmp(argv[1], "--leds") == 0) {
        if (argc >= 3 && strcmp(argv[2], "seq") == 0) {
            int mock = arg_has(argc, argv, "--mock");
            if (mock) {
                char msg[160];
                int bad;
                sbc_test_io_t io;
                memset(&s, 0, sizeof s);
                pio_make(&s, &io);
                sbc_test_led_seq_run(&io, 600, 1);
                bad = sbc_test_led_seq_verify(&io, 1, msg, sizeof msg);
                printf("leds seq --mock: %u OUT-Reports, %s%s\n", s.nout,
                       bad ? "FEHLER: " : "Format ok (je genau 1 Nibble = F, Abschluss dunkel)",
                       bad ? msg : "");
                src_free(&s);
                return bad ? 1 : 0;
            }
            rc = src_open_live(&s);
            if (rc) {
                return rc;
            }
            printf("Lampenfolge: 37 Lampen + 2 reservierte Stellen, je 0,6 s. "
                   "Ctrl+C = Abbruch (danach alles aus).\n");
            {
                sbc_test_io_t io;
                pio_make(&s, &io);
                rc = sbc_test_led_seq_run(&io, 600, 1);
            }
            src_free(&s);
            return rc == SBC_OK ? 0 : 1;
        }
        {
            int v = argc >= 3 ? atoi(argv[2]) : 15;
            if (v < 0) {
                v = 0;
            }
            if (v > 15) {
                v = 15;
            }
            return cmd_live(v);
        }
    }
    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "/?") == 0 ||
                      strcmp(argv[1], "-h") == 0)) {
        usage();
        return 0;
    }
    if (argc >= 2) {
        printf("sbc_probe: unbekannte Option %s\n\n", argv[1]);
        usage();
        return 1;
    }
    return cmd_live(-1);
}
