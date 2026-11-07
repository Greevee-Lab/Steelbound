/* sbc_test.c - Gemeinsamer Testkern: gefuehrte Controller-Pruefung (W143).
 *
 * Aus sbc_probe.c ausgelagert (Testschritte, Erkennung, Lampenfolge,
 * Ergebnisdatei); Verhalten wie W141, mit der W143-Neutralitaet
 * ("weitere Tasten gesehen" statt "unerwartet" bei mitgedrueckten Tasten).
 * Ein-/Ausgabe nur ueber sbc_test_io_t (Konsole oder Launcher-Assistent).
 * Keine Umlaute in Ausgabe/Ergebnisdatei (Codepage-sicher).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "sbc_test.h"

/* ---- Ausgabe ueber den Aufrufer ---- */

static void t_emitf(const sbc_test_ctx_t *c, const char *fmt, ...)
{
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(b, sizeof b, _TRUNCATE, fmt, ap);
    va_end(ap);
    c->io->emit(c->io->ctx, b);
}

static void t_prog(const sbc_test_ctx_t *c, int ev, int a, int b)
{
    if (c->io->progress) {
        c->io->progress(c->io->ctx, ev, a, b);
    }
}

/* ---- Namen/Tabellen ---- */

const char *const sbc_test_btn_name[39] = {
    "MainWeapon", "Fire", "LockOn", "Eject", "CockpitHatch", "Ignition",
    "Start", "OpenClose", "MapZoomInOut", "ModeSelect", "SubMonitor",
    "MainMonZoomIn", "MainMonZoomOut", "FSS", "Manipulator",
    "LineColorChange", "Washing", "Extinguisher", "Chaff", "TankDetach",
    "Override", "NightScope", "F1", "F2", "F3", "MainWeaponControl",
    "SubWeaponControl", "MagazineChange", "Comm1", "Comm2", "Comm3",
    "Comm4", "Comm5", "SightChange", "Tog FilterControl",
    "Tog OxygenSupply", "Tog FuelFlowRate", "Tog BufferMaterial",
    "Tog VTLocation"
};

const char *const sbc_test_btn_hint[39] = {
    "Zielhebel rechts: Abzug (Zeigefinger)",
    "Zielhebel rechts: Daumentaste oben (Fire)",
    "Zielhebel rechts: Daumentaste (Lock On)",
    "Mittelkonsole: Notausstieg (Eject, unter dem Buegel)",
    "rechtes Panel: Cockpit Hatch",
    "rechtes Panel: Ignition",
    "rechtes Panel: Start",
    "Multi-Monitor: Open/Close",
    "Multi-Monitor: Map Zoom In/Out",
    "Multi-Monitor: Mode Select",
    "Multi-Monitor: Sub Monitor Mode Select",
    "Main Monitor: Zoom In",
    "Main Monitor: Zoom Out",
    "FSS (Forecast Shooting System)",
    "Manipulator",
    "Line Color Change",
    "Washing",
    "Extinguisher",
    "Chaff",
    "Tank Detach",
    "Override",
    "Night Scope",
    "F1", "F2", "F3",
    "Main Weapon Control",
    "Sub Weapon Control",
    "Magazine Change",
    "Comm 1", "Comm 2", "Comm 3", "Comm 4", "Comm 5",
    "Drehhebel links: Daumenstick eindruecken (Sight Change)",
    "Kippschalter Filter Control",
    "Kippschalter Oxygen Supply",
    "Kippschalter Fuel Flow Rate",
    "Kippschalter Buffer Material",
    "Kippschalter VT-Location Measurement"
};

const char *const sbc_test_led_name[SBC_LED_COUNT] = {
    "EmergencyEject", "CockpitHatch", "Ignition", "Start", "OpenClose",
    "MapZoomInOut", "ModeSelect", "SubMonitor", "MainMonZoomIn",
    "MainMonZoomOut", "FSS", "Manipulator", "LineColorChange", "Washing",
    "Extinguisher", "Chaff", "TankDetach", "Override", "NightScope", "F1",
    "F2", "F3", "MainWeaponControl", "SubWeaponControl", "MagazineChange",
    "Comm1", "Comm2", "Comm3", "Comm4", "Comm5", "Gear R", "Gear N",
    "Gear 1", "Gear 2", "Gear 3", "Gear 4", "Gear 5"
};

const sbc_test_axisdef_t sbc_test_axes[8] = {
    { "Aiming X", 8, 0, SBC_AX_AIM_X },
    { "Aiming Y", 10, 0, SBC_AX_AIM_Y },
    { "Rotation", 12, 1, SBC_AX_ROT },
    { "SightChange X", 14, 1, SBC_AX_SIGHT_X },
    { "SightChange Y", 16, 1, SBC_AX_SIGHT_Y },
    { "Pedal links (Slide-Step)", 18, 2, SBC_AX_PEDAL_L },
    { "Pedal Mitte (Bremse)", 20, 2, SBC_AX_PEDAL_M },
    { "Pedal rechts (Gas)", 22, 2, SBC_AX_PEDAL_R },
};

static const char *const kResName[] = {
    "OK", "fehlt", "unerwartet", "teilweise", "-"
};

const char *sbc_test_resname(int status)
{
    if (status < 0 || status > 4) {
        return "?";
    }
    return kResName[status];
}

uint64_t sbc_test_raw_btns(const uint8_t *r)
{
    return ((uint64_t)r[2] | ((uint64_t)r[3] << 8) | ((uint64_t)r[4] << 16) |
            ((uint64_t)r[5] << 24) | ((uint64_t)(r[6] & 0x7F) << 32));
}

int sbc_test_raw_axis(const uint8_t *r, int a)
{
    unsigned v = (unsigned)r[sbc_test_axes[a].off] |
                 ((unsigned)r[sbc_test_axes[a].off + 1] << 8);
    if (sbc_test_axes[a].kind == 1) {
        return (int)(int16_t)v;
    }
    return (int)v;
}

int sbc_test_game_units_round(int a, int v)
{
    (void)a;
    return (v >= 0) ? (v + 128) / 256 : -((-v + 127) / 256);
}

int sbc_test_axis_nominal_rest(int a)
{
    return sbc_test_axes[a].kind == 0 ? 0x8000 : 0;
}

int sbc_test_game_units(int a, int v)
{
    if (sbc_test_axes[a].kind == 1) {
        return (v >= 0) ? v / 256 : -((-v + 255) / 256);
    }
    return v / 256;
}

const char *sbc_test_gear_name(int g)
{
    static const char *const n[] = { "R", "N", "1", "2", "3", "4", "5" };
    int e;
    if (g == 0) {
        return "dazwischen";
    }
    if (g >= 7 && g <= 13) {
        return n[g - 7];
    }
    /* W143: echte Geraete-Bytes (Nutzer 6.11.) ebenfalls benennen. */
    e = sbc_usb_gear_effective(g);
    if (e >= 0 && e < 7) {
        return n[e];
    }
    return "?";
}

/* ---- Unsichere Felder ---- */

void sbc_test_unk_add(sbc_test_unk_t *u, const uint8_t *r, unsigned len)
{
    int i;
    if (!u->n || len < u->len_min) u->len_min = len;
    if (len > u->len_max) u->len_max = len;
    u->n++;
    if (len != SBC_USB_IN_LEN) u->bad_len++;
    if (len < 2 || r[0] != 0x00 || r[1] != SBC_USB_IN_LEN) u->bad_hdr++;
    u->b7[r[7]] = 1;
    u->b6h[r[6] >> 7] = 1;
    u->b24h[r[24] >> 4] = 1;
    for (i = 0; i < 8; i++) {
        u->lo_seen[i][r[sbc_test_axes[i].off]] = 1;
    }
    for (i = SBC_USB_IN_LEN; i < (int)len && i < SBC_USB_RAW_MAX; i++) {
        if (r[i]) u->tail_nz = 1;
    }
}

static int set_count(const uint8_t *s, int n, char *list, size_t cap)
{
    int i, c = 0;
    size_t o = 0;
    list[0] = '\0';
    for (i = 0; i < n; i++) {
        if (s[i]) {
            if (c < 12 && o < cap) {
                int w = _snprintf_s(list + o, cap - o, _TRUNCATE, "%s%02X",
                                    c ? " " : "", i);
                if (w > 0) o += (size_t)w;
            }
            c++;
        }
    }
    if (c > 12 && o < cap) {
        _snprintf_s(list + o, cap - o, _TRUNCATE, " ... (%d Werte)", c);
    }
    return c;
}

void sbc_test_unk_report(const sbc_test_unk_t *u,
                         void (*pf)(void *opaque, const char *fmt, ...),
                         void *opaque)
{
    char l[160];
    int i, c;
    pf(opaque, "Unsichere Felder (CONTROLLER.md 10) - bitte diesen Block melden:\n");
    pf(opaque, "  Reports: %u, Laenge %u..%u, falsche Laenge %u, falscher Header %u%s\n",
       u->n, u->len_min, u->len_max, u->bad_len, u->bad_hdr,
       u->tail_nz ? ", Bytes 26+ NICHT 0!" : "");
    c = set_count(u->b7, 256, l, sizeof l);
    pf(opaque, "  Byte 7 (reserviert): %s%s\n", l, c == 1 && u->b7[0] ? "  (immer 00 = wie angenommen)" : c ? "  <- WERTE != 00: bitte melden" : "-");
    pf(opaque, "  Byte 6 Bit 7: %s%s\n",
       u->b6h[1] ? (u->b6h[0] ? "0 und 1" : "1") : (u->b6h[0] ? "0" : "-"),
       u->b6h[1] ? "  (gesetzt gesehen = bekannt, wird ignoriert)" :
       u->b6h[0] ? "  (immer 0)" : "");
    c = set_count(u->b24h, 16, l, sizeof l);
    pf(opaque, "  Byte 24 High-Nibble: %s%s\n", c ? l : "-",
       (c == 1 && u->b24h[0]) ? "  (immer 0)" : c ? "  <- != 0: bitte melden" : "");
    for (i = 0; i < 8; i++) {
        c = set_count(u->lo_seen[i], 256, l, sizeof l);
        pf(opaque, "  Low-Byte %-24s (Byte %2d): %d Wert(e) %s%s\n", sbc_test_axes[i].name,
           sbc_test_axes[i].off, c, l,
           c > 2 ? "  -> traegt Feinaufloesung (Spiel nutzt nur High-Byte)" : "");
    }
}

/* ---- Lampenfolge ---- */

void sbc_test_led_seq_slot(int k, sbc_test_led_slot_t *sl)
{
    if (k < SBC_LED_COUNT) {
        /* LED k: Nibble-Position k (Byte 2 + k/2) bis Comm5 (k=29),
         * danach ab Byte 17 high (Nibble 31). */
        int nib = k < 30 ? k : k + 1;
        sl->byte = 2 + nib / 2;
        sl->hi = nib & 1;
        sl->led = k;
        sl->name = sbc_test_led_name[k];
    } else if (k == SBC_LED_COUNT) {
        sl->byte = 17;
        sl->hi = 0;
        sl->led = -1;
        sl->name = "reserviert (OUT-Byte 17 low) - sollte DUNKEL bleiben";
    } else {
        sl->byte = 21;
        sl->hi = 0;
        sl->led = -1;
        sl->name = "ungenutzt (OUT-Byte 21 low) - sollte DUNKEL bleiben";
    }
}

void sbc_test_led_seq_report(int k, uint8_t out[SBC_USB_OUT_LEN])
{
    sbc_test_led_slot_t sl;
    memset(out, 0, SBC_USB_OUT_LEN);
    out[0] = 0x00;
    out[1] = SBC_USB_OUT_LEN;
    if (k < 0 || k >= SBC_TEST_LED_SEQ_N) {
        return; /* alles aus */
    }
    if (k < SBC_LED_COUNT) {
        /* ueber den Modul-Builder (gleicher Pfad wie das Spiel) */
        sbc_leds_t leds;
        memset(&leds, 0, sizeof leds);
        leds.level[k] = 0xF;
        sbc_usb_build_led_report(&leds, out);
        return;
    }
    sbc_test_led_seq_slot(k, &sl);
    out[sl.byte] = (uint8_t)(sl.hi ? 0xF0 : 0x0F);
}

int sbc_test_led_seq_run(const sbc_test_io_t *io, unsigned step_ms,
                         int with_reserved)
{
    uint8_t out[SBC_USB_OUT_LEN];
    char b[256];
    int k, rc = SBC_OK, n = with_reserved ? SBC_TEST_LED_SEQ_N : SBC_LED_COUNT;
    for (k = 0; k < n && !io->aborted(io->ctx); k++) {
        sbc_test_led_slot_t sl;
        sbc_test_led_seq_slot(k, &sl);
        sbc_test_led_seq_report(k, out);
        _snprintf_s(b, sizeof b, _TRUNCATE, "Lampe %2d/%d: %-18s (OUT-Byte %d %s)\n",
                    k + 1, n, sl.name, sl.byte, sl.hi ? "high" : "low");
        io->emit(io->ctx, b);
        if (io->progress) {
            io->progress(io->ctx, SBC_TP_LED, k, 0);
        }
        rc = io->send_out(io->ctx, out);
        if (rc != SBC_OK) {
            _snprintf_s(b, sizeof b, _TRUNCATE, "  senden scheitert: %s\n",
                        sbc_usb_strerror(rc));
            io->emit(io->ctx, b);
            break;
        }
        io->sleep_ms(io->ctx, step_ms);
    }
    sbc_test_led_seq_report(-1, out);
    if (io->send_out(io->ctx, out) == SBC_OK) {
        io->emit(io->ctx, "Alle Lampen aus.\n");
    }
    return rc;
}

int sbc_test_led_seq_verify_skip(const sbc_test_io_t *io, unsigned skip,
                                 int with_reserved, char *msg, size_t cap)
{
    int k, n = with_reserved ? SBC_TEST_LED_SEQ_N : SBC_LED_COUNT, bad = 0;
    unsigned nout = io->nout(io->ctx);
    msg[0] = '\0';
    if (nout < skip) {
        skip = nout;
    }
    if ((int)(nout - skip) != n + 1) {
        _snprintf_s(msg, cap, _TRUNCATE, "%u Reports statt %d", nout - skip, n + 1);
        return 1;
    }
    for (k = 0; k <= n; k++) {
        const uint8_t *o = io->out_at(io->ctx, skip + (unsigned)k);
        int j, lit = 0, lit_byte = -1, lit_hi = -1;
        sbc_test_led_slot_t sl;
        sbc_leds_t leds;
        if (!o) {
            bad++;
            if (!msg[0]) _snprintf_s(msg, cap, _TRUNCATE, "Schritt %d: kein Report", k);
            continue;
        }
        if (o[0] != 0x00 || o[1] != SBC_USB_OUT_LEN) {
            bad++;
            if (!msg[0]) _snprintf_s(msg, cap, _TRUNCATE, "Schritt %d: Header %02X %02X", k, o[0], o[1]);
            continue;
        }
        for (j = 2; j < SBC_USB_OUT_LEN; j++) {
            if (o[j] & 0x0F) { lit++; lit_byte = j; lit_hi = 0; if ((o[j] & 0x0F) != 0x0F) bad++; }
            if (o[j] & 0xF0) { lit++; lit_byte = j; lit_hi = 1; if ((o[j] & 0xF0) != 0xF0) bad++; }
        }
        if (k == n) { /* Abschluss: alles aus */
            if (lit) {
                bad++;
                if (!msg[0]) _snprintf_s(msg, cap, _TRUNCATE, "Abschluss nicht dunkel");
            }
            continue;
        }
        sbc_test_led_seq_slot(k, &sl);
        if (lit != 1 || lit_byte != sl.byte || lit_hi != sl.hi) {
            bad++;
            if (!msg[0]) _snprintf_s(msg, cap, _TRUNCATE, "Schritt %d (%s): %d Nibbles, Byte %d", k, sl.name, lit, lit_byte);
            continue;
        }
        /* Gegenprobe mit dem Modul-Parser: genau diese LED (bzw. keine). */
        if (sbc_usb_parse_led_report(o, &leds) != SBC_OK) {
            bad++;
            continue;
        }
        for (j = 0; j < SBC_LED_COUNT; j++) {
            int want = (j == sl.led) ? 0xF : 0;
            if (leds.level[j] != want) {
                bad++;
                if (!msg[0]) _snprintf_s(msg, cap, _TRUNCATE, "Schritt %d: Parser LED %d=%X", k, j, leds.level[j]);
                break;
            }
        }
    }
    return bad;
}

int sbc_test_led_seq_verify(const sbc_test_io_t *io, int with_reserved,
                            char *msg, size_t cap)
{
    return sbc_test_led_seq_verify_skip(io, 0, with_reserved, msg, cap);
}

/* ---- Gefuehrte Pruefung ---- */

int sbc_test_build_steps(sbc_test_step_t *st)
{
    int n = 0, i;
    for (i = 0; i < 34; i++) { st[n].kind = SBC_TK_BTN; st[n++].idx = i; }
    for (i = 0; i < 5; i++) { st[n].kind = SBC_TK_TOG; st[n++].idx = SBC_TEST_TOG_FIRST + i; }
    for (i = 0; i < 8; i++) { st[n].kind = SBC_TK_AXIS; st[n++].idx = i; }
    st[n].kind = SBC_TK_TUNER; st[n++].idx = 0;
    st[n].kind = SBC_TK_GEAR; st[n++].idx = 0;
    st[n].kind = SBC_TK_LED; st[n++].idx = 0;
    return n;
}

const char *sbc_test_step_name(const sbc_test_step_t *st, char *buf, size_t cap)
{
    switch (st->kind) {
    case SBC_TK_BTN: _snprintf_s(buf, cap, _TRUNCATE, "Taste %s (bi%d)", sbc_test_btn_name[st->idx], st->idx); break;
    case SBC_TK_TOG: _snprintf_s(buf, cap, _TRUNCATE, "Schalter %s (bi%d)", sbc_test_btn_name[st->idx], st->idx); break;
    case SBC_TK_AXIS: _snprintf_s(buf, cap, _TRUNCATE, "Achse %s", sbc_test_axes[st->idx].name); break;
    case SBC_TK_TUNER: strcpy_s(buf, cap, "Tuner-Rad"); break;
    case SBC_TK_GEAR: strcpy_s(buf, cap, "Schalthebel"); break;
    default: strcpy_s(buf, cap, "Lampentest (OUT)"); break;
    }
    return buf;
}

void sbc_test_ctx_init(sbc_test_ctx_t *c, const sbc_test_io_t *io)
{
    memset(c, 0, sizeof *c);
    c->io = io;
}

int sbc_test_first_report(sbc_test_ctx_t *c)
{
    sbc_test_ev_t e;
    uint64_t t_end = c->io->now(c->io->ctx) + 1500;
    c->used_getreport = 0;
    for (;;) {
        int k = c->io->next(c->io->ctx, t_end, &e);
        if (k == SBC_TEV_REPORT) {
            memcpy(c->cur, e.raw, SBC_USB_RAW_MAX);
            c->len = e.len;
            return 1;
        }
        if (k == SBC_TEV_ERR || (k == SBC_TEV_KEY && e.key == 0x1B)) {
            return 0;
        }
        if (k == SBC_TEV_TIMEOUT && c->io->now(c->io->ctx) >= t_end) {
            break;
        }
        if (!c->io->is_live(c->io->ctx) && c->io->has_ended(c->io->ctx)) {
            return 0;
        }
    }
    if (c->io->is_live(c->io->ctx)) {
        unsigned len = 0;
        if (c->io->get_report &&
            c->io->get_report(c->io->ctx, c->cur, &len) && len >= 2) {
            c->len = len;
            c->used_getreport = 1;
            t_emitf(c, "Hinweis: keine Interrupt-Reports im Leerlauf - GET_REPORT lieferte "
                       "%u Bytes (Geraet sendet vermutlich nur bei Aenderung).\n", len);
            return 1;
        }
        t_emitf(c, "Bitte eine beliebige Taste am Controller druecken ...\n");
        t_end = c->io->now(c->io->ctx) + 15000;
        for (;;) {
            int k = c->io->next(c->io->ctx, t_end, &e);
            if (k == SBC_TEV_REPORT) {
                memcpy(c->cur, e.raw, SBC_USB_RAW_MAX);
                c->len = e.len;
                return 1;
            }
            if (k == SBC_TEV_ERR || k == SBC_TEV_KEY || c->io->now(c->io->ctx) >= t_end) {
                return 0;
            }
        }
    }
    return 0;
}

/* Ein Ereignis holen; Reports aktualisieren cur. Rueckgabe SBC_TEV_* */
static int t_next(sbc_test_ctx_t *c, uint64_t deadline, int *key)
{
    sbc_test_ev_t e;
    int k = c->io->next(c->io->ctx, deadline, &e);
    *key = 0;
    if (k == SBC_TEV_REPORT) {
        memcpy(c->cur, e.raw, sizeof c->cur);
        c->len = e.len;
        sbc_test_unk_add(&c->unk, e.raw, e.len);
        c->n_rep++;
    } else if (k == SBC_TEV_KEY) {
        *key = e.key;
        if (e.key == 0x1B || c->io->aborted(c->io->ctx)) {
            c->aborted = 1;
        }
    } else if (k == SBC_TEV_ERR) {
        c->aborted = 1;
    }
    return k;
}

static uint64_t t_now(sbc_test_ctx_t *c)
{
    return c->io->now(c->io->ctx);
}

static void bit_list(uint64_t m, char *out, size_t cap)
{
    int i;
    size_t o = 0;
    out[0] = '\0';
    for (i = 0; i < 39 && o < cap; i++) {
        if (m & (1ULL << i)) {
            int w = _snprintf_s(out + o, cap - o, _TRUNCATE, "%s%s(bi%d)",
                                o ? ", " : "", sbc_test_btn_name[i], i);
            if (w > 0) o += (size_t)w;
        }
    }
}

/* Taste: Bit muss 0->1 gehen (und wieder los). Kippschalter: beide
 * Richtungen. W143: Mitgedrueckte Tasten (gleichzeitig mit dem erwarteten
 * Bit oben) sind Bedienung: neutral "weitere Tasten gesehen", kein Befund. */
void sbc_test_step_button(sbc_test_ctx_t *c, int bi, int toggle,
                          sbc_test_res_t *r)
{
    uint64_t m = 1ULL << bi;
    uint64_t prev = sbc_test_raw_btns(c->cur), other = 0, seen = 0, overlap = 0, nb = 0;
    uint64_t t0 = t_now(c), t_press = 0;
    int pressed = 0, released = 0, rise = 0, fall = 0, key;
    int start_state = (prev & m) != 0;
    if (!toggle) {
        t_emitf(c, "  -> Taste druecken und loslassen: %s\n", sbc_test_btn_hint[bi]);
        if (start_state) {
            t_emitf(c, "     (Bit ist schon AN - klemmt die Taste? Bitte loslassen.)\n");
        }
    } else {
        t_emitf(c, "  -> %s: umlegen und zurueck (AN und AUS, Stellung jetzt: %s)\n",
                sbc_test_btn_hint[bi], start_state ? "AN" : "AUS");
    }
    for (;;) {
        int k = t_next(c, t_now(c) + 100, &key);
        if (c->aborted) break;
        if (k == SBC_TEV_KEY && key == '\r') { c->skip_key = 1; break; }
        if (k == SBC_TEV_REPORT) {
            uint64_t b = sbc_test_raw_btns(c->cur);
            uint64_t up = b & ~prev, down = prev & ~b;
            if (up & m) { rise = 1; pressed = 1; t_press = t_now(c); }
            if (down & m) { fall = 1; if (pressed) released = 1; }
            /* W141: Fremdbits zaehlen nur bei Ueberlappung mit dem
             * erwarteten Bit (gleichzeitig oben); Nachbar-Kippschalter
             * gehen mechanisch mit und werden Hinweis, kein Befund. */
            seen |= up & ~m & ~SBC_TEST_TOG_MASK;
            if (b & m) overlap |= b & ~m & ~SBC_TEST_TOG_MASK;
            if (toggle) nb |= (up | down) & SBC_TEST_TOG_MASK & ~m;
            prev = b;
        }
        if (!toggle && pressed && released) break;
        if (!toggle && pressed && t_now(c) - t_press > 3000) break; /* gehalten */
        if (toggle && rise && fall) break;
        if (t_now(c) - t0 > SBC_TEST_STEP_TIMEOUT_MS) break;
    }
    if (toggle) {
        if (rise && fall) {
            r->status = SBC_TR_OK;
        } else if (rise || fall) {
            r->status = SBC_TR_PARTIAL;
            _snprintf_s(r->note, sizeof r->note, _TRUNCATE, "nur %s erkannt",
                        rise ? "AUS->AN" : "AN->AUS");
        } else {
            r->status = SBC_TR_MISSING;
        }
        _snprintf_s(r->note + strlen(r->note), sizeof r->note - strlen(r->note),
                    _TRUNCATE, "%sStellung am Ende: %s", r->note[0] ? "; " : "",
                    (sbc_test_raw_btns(c->cur) & m) ? "AN" : "AUS");
    } else {
        r->status = pressed ? SBC_TR_OK : SBC_TR_MISSING;
        if (pressed && !released) {
            _snprintf_s(r->note, sizeof r->note, _TRUNCATE, "Loslassen nicht gesehen");
        }
    }
    /* W143: Ueberlappung = neutraler Hinweis, kein "unerwartet" mehr. */
    other = (pressed || rise || fall) ? overlap : seen;
    if (other) {
        char l[160];
        bit_list(other, l, sizeof l);
        if (!(pressed || rise || fall)) {
            if (r->status == SBC_TR_MISSING || r->status == SBC_TR_OK) {
                r->status = SBC_TR_UNEXPECTED;
            }
            _snprintf_s(r->note + strlen(r->note), sizeof r->note - strlen(r->note),
                        _TRUNCATE, "%s%s: %s", r->note[0] ? "; " : "",
                        "kam stattdessen", l);
        } else {
            _snprintf_s(r->note + strlen(r->note), sizeof r->note - strlen(r->note),
                        _TRUNCATE, "%sweitere Tasten gesehen: %s",
                        r->note[0] ? "; " : "", l);
        }
    }
    if (nb) {
        char l[160];
        bit_list(nb, l, sizeof l);
        _snprintf_s(r->note + strlen(r->note), sizeof r->note - strlen(r->note),
                    _TRUNCATE, "%sHinweis: Nachbar-Schalter gingen mit: %s",
                    r->note[0] ? "; " : "", l);
    }
    if (r->status == SBC_TR_MISSING && !r->note[0]) {
        _snprintf_s(r->note, sizeof r->note, _TRUNCATE, "%s",
                    c->skip_key ? "uebersprungen (Enter)" :
                    (!c->io->is_live(c->io->ctx) && c->io->has_ended(c->io->ctx)) ? "keine Daten" : "Zeit abgelaufen (60 s)");
    }
}

/* Schwellen im Rohwert. */
#define AX_CROSSTALK 0x1A00 /* W141: Mitbewegen erst ab ~10 % Hub melden */
#define AX_MOVE 0x1800   /* Bewegung erkannt (~9 %) */
#define AX_FULL 0x6000   /* Rand erreicht: 75 % des Halbbereichs */
#define PED_FULL 0xC000  /* Pedal: 75 % von 0xFF00 */
#define AX_REST_TOL 0x0C00 /* zurueck in Ruhe */

void sbc_test_step_axis(sbc_test_ctx_t *c, int a, sbc_test_res_t *r)
{
    int key, k, i;
    int ped = sbc_test_axes[a].kind == 2;
    int base[8], devmax[8];
    uint64_t t0, t_settle = 0;
    int moved = 0, v;
    int rest_lo, rest_hi, rc;
    r->measured = 0;
    /* Phase A: Ruhe 0,8 s messen */
    t_emitf(c, "  -> %s: NICHT beruehren, Ruhe wird gemessen ...\n", sbc_test_axes[a].name);
    v = sbc_test_raw_axis(c->cur, a);
    rest_lo = rest_hi = v;
    t0 = t_now(c);
    while (t_now(c) - t0 < 800) {
        k = t_next(c, t0 + 800, &key);
        if (c->aborted) return;
        if (k == SBC_TEV_REPORT) {
            v = sbc_test_raw_axis(c->cur, a);
            if (v < rest_lo) rest_lo = v;
            if (v > rest_hi) rest_hi = v;
        }
    }
    for (i = 0; i < 8; i++) {
        base[i] = sbc_test_raw_axis(c->cur, i);
        devmax[i] = 0;
    }
    rc = (rest_lo + rest_hi) / 2;
    r->mn = rest_lo;
    r->mx = rest_hi;
    if (ped) {
        t_emitf(c, "  -> %s: ganz durchtreten, dann loslassen (Enter = fertig)\n", sbc_test_axes[a].name);
    } else {
        t_emitf(c, "  -> %s: bis zu BEIDEN Anschlaegen bewegen, dann loslassen (Enter = fertig)\n",
                sbc_test_axes[a].name);
    }
    t0 = t_now(c);
    for (;;) {
        k = t_next(c, t_now(c) + 100, &key);
        if (c->aborted) return;
        if (k == SBC_TEV_KEY && key == '\r') { c->skip_key = 1; break; }
        if (k == SBC_TEV_REPORT) {
            v = sbc_test_raw_axis(c->cur, a);
            if (v < r->mn) r->mn = v;
            if (v > r->mx) r->mx = v;
            if (abs(v - rc) > AX_MOVE) moved = 1;
            for (i = 0; i < 8; i++) {
                int d = abs(sbc_test_raw_axis(c->cur, i) - base[i]);
                if (d > devmax[i]) devmax[i] = d;
            }
            if (moved && abs(v - rc) <= AX_REST_TOL + (rest_hi - rest_lo)) {
                if (!t_settle) t_settle = t_now(c);
            } else {
                t_settle = 0;
            }
        }
        {
            int nom = sbc_test_axis_nominal_rest(a);
            int full_lo = ped ? 1 : (r->mn <= nom - AX_FULL);
            int full_hi = ped ? (r->mx >= PED_FULL) : (r->mx >= nom + AX_FULL);
            if (moved && full_lo && full_hi && t_settle &&
                t_now(c) - t_settle >= 600) {
                break;
            }
        }
        if (t_now(c) - t0 > SBC_TEST_STEP_TIMEOUT_MS) break;
    }
    /* Ruhe nach dem Loslassen in das Ruheband aufnehmen (nur wenn zurueck). */
    if (t_settle) {
        v = sbc_test_raw_axis(c->cur, a);
        if (v < rest_lo) rest_lo = v;
        if (v > rest_hi) rest_hi = v;
    }
    r->rest_lo = rest_lo;
    r->rest_hi = rest_hi;
    {
        int nom = sbc_test_axis_nominal_rest(a);
        r->full_lo = ped ? 1 : (r->mn <= nom - AX_FULL);
        r->full_hi = ped ? (r->mx >= PED_FULL) : (r->mx >= nom + AX_FULL);
    }
    r->measured = moved;
    if (moved && r->full_lo && r->full_hi) {
        r->status = SBC_TR_OK;
    } else if (moved) {
        r->status = SBC_TR_PARTIAL;
        _snprintf_s(r->note, sizeof r->note, _TRUNCATE, "Rand nicht erreicht:%s%s",
                    r->full_lo ? "" : (sbc_test_axes[a].kind == 1 || sbc_test_axes[a].kind == 0) ? " min" : "",
                    r->full_hi ? "" : " max");
    } else {
        r->status = SBC_TR_MISSING;
        _snprintf_s(r->note, sizeof r->note, _TRUNCATE, "%s",
                    c->skip_key ? "keine Bewegung (Enter)" :
                    (!c->io->is_live(c->io->ctx) && c->io->has_ended(c->io->ctx)) ? "keine Daten" : "keine Bewegung (Zeit)");
    }
    /* Andere Achse bewegt? (Aiming X/Y zusammen ist normal: diagonal)
     * W141: Hinweis statt Befund - die eigene Achse hat funktioniert. */
    for (i = 0; i < 8; i++) {
        if (i == a || devmax[i] <= AX_CROSSTALK) continue;
        if ((a <= 1 && i <= 1) || (a == 3 && i == 4) || (a == 4 && i == 3)) continue;
        _snprintf_s(r->note + strlen(r->note), sizeof r->note - strlen(r->note),
                    _TRUNCATE, "%sHinweis: %s bewegte %s (max %d)", r->note[0] ? "; " : "",
                    sbc_test_axes[i].name, moved ? "mit" : "stattdessen", devmax[i]);
    }
}

void sbc_test_step_tuner(sbc_test_ctx_t *c, sbc_test_res_t *r,
                         uint8_t seen[16])
{
    uint64_t t0 = t_now(c), t_all = 0;
    int key, i, n;
    memset(seen, 0, 16);
    seen[c->cur[24] & 0x0F] = 1;
    t_emitf(c, "  -> Tuner-Rad (Funk) langsam ueber ALLE Stellungen drehen (Enter = fertig)\n");
    for (;;) {
        int k = t_next(c, t_now(c) + 100, &key), all = 1;
        if (c->aborted) return;
        if (k == SBC_TEV_KEY && key == '\r') { c->skip_key = 1; break; }
        if (k == SBC_TEV_REPORT) seen[c->cur[24] & 0x0F] = 1;
        for (i = 0; i <= 12; i++) if (!seen[i]) all = 0;
        if (all && !t_all) t_all = t_now(c);
        if (t_all && t_now(c) - t_all > 400) break;
        if (t_now(c) - t0 > SBC_TEST_STEP_TIMEOUT_MS) break;
    }
    for (i = 0, n = 0; i < 16; i++) n += seen[i];
    {
        char miss[64] = "", ex[32] = "";
        size_t o = 0, oe = 0;
        for (i = 0; i <= 12; i++) {
            if (!seen[i] && o < sizeof miss) {
                int w = _snprintf_s(miss + o, sizeof miss - o, _TRUNCATE, " %d", i);
                if (w > 0) o += (size_t)w;
            }
        }
        for (i = 13; i < 16; i++) {
            if (seen[i] && oe < sizeof ex) {
                int w = _snprintf_s(ex + oe, sizeof ex - oe, _TRUNCATE, " %d", i);
                if (w > 0) oe += (size_t)w;
            }
        }
        r->status = !miss[0] ? SBC_TR_OK : n >= 2 ? SBC_TR_PARTIAL : SBC_TR_MISSING;
        _snprintf_s(r->note, sizeof r->note, _TRUNCATE, "%d Stellungen%s%s%s%s",
                    n, miss[0] ? "; fehlen:" : "", miss,
                    ex[0] ? "; Blende 13-15 gesehen:" : "", ex);
    }
}

/* Letzte 7 verschiedene stabile Stellungen (Reihenfolge R,N,1..5), 1 = ok. */
static int gear_last7(sbc_test_ctx_t *c, int v[7])
{
    int i, n = 0;
    for (i = c->gear_nseq - 1; i >= 1 && n < 7; i--) {
        int j, dup = 0;
        for (j = 0; j < n; j++) if (v[j] == c->gear_seq[i]) dup = 1;
        if (!dup) v[n++] = c->gear_seq[i];
    }
    if (n != 7) return 0;
    for (i = 0; i < 3; i++) { int t = v[i]; v[i] = v[6 - i]; v[6 - i] = t; }
    return 1;
}

void sbc_test_step_gear(sbc_test_ctx_t *c, sbc_test_res_t *r,
                        uint8_t seen[256])
{
    uint64_t t0 = t_now(c), t_all = 0, t_chg;
    int key, i, last_val;
    memset(seen, 0, 256);
    seen[c->cur[25]] = 1;
    c->gear_nseq = 0;
    last_val = c->cur[25];
    t_chg = t_now(c);
    c->gear_seq[c->gear_nseq++] = last_val; /* Startstellung */
    t_emitf(c, "  -> Schalthebel nacheinander: R, N, 1, 2, 3, 4, 5 - jede Stellung kurz "
               "halten (Enter = fertig)\n");
    for (;;) {
        int k = t_next(c, t_now(c) + 100, &key), all = 1, nd = 0;
        if (c->aborted) return;
        if (k == SBC_TEV_KEY && key == '\r') { c->skip_key = 1; break; }
        if (k == SBC_TEV_REPORT) {
            seen[c->cur[25]] = 1;
            if (c->cur[25] != last_val) {
                last_val = c->cur[25];
                t_chg = t_now(c);
            }
        }
        if (t_now(c) - t_chg >= 300 && c->gear_nseq < 32 &&
            c->gear_seq[c->gear_nseq - 1] != last_val) {
            c->gear_seq[c->gear_nseq++] = last_val;
        }
        for (i = 7; i <= 13; i++) if (!seen[i]) all = 0;
        /* abweichende Kodierung: 7 verschiedene stabile Stellungen nach dem Start */
        {
            uint8_t d[256];
            memset(d, 0, sizeof d);
            for (i = 1; i < c->gear_nseq; i++) {
                if (!d[c->gear_seq[i]]) { d[c->gear_seq[i]] = 1; nd++; }
            }
        }
        if ((all || nd >= 7) && !t_all) t_all = t_now(c);
        if (t_all && t_now(c) - t_all > 400) break;
        if (t_now(c) - t0 > SBC_TEST_STEP_TIMEOUT_MS) break;
    }
    {
        char miss[64] = "", odd[64] = "", sq[128] = "";
        size_t o = 0, oo = 0, os = 0;
        int n = 0;
        for (i = 7; i <= 13; i++) {
            if (seen[i]) { n++; continue; }
            if (o < sizeof miss) {
                int w = _snprintf_s(miss + o, sizeof miss - o, _TRUNCATE, " %s", sbc_test_gear_name(i));
                if (w > 0) o += (size_t)w;
            }
        }
        for (i = 1; i < 256; i++) {
            if (seen[i] && (i < 7 || i > 13) && oo < sizeof odd) {
                int w = _snprintf_s(odd + oo, sizeof odd - oo, _TRUNCATE, " %d", i);
                if (w > 0) oo += (size_t)w;
            }
        }
        for (i = 0; i < c->gear_nseq && os < sizeof sq; i++) {
            int w = _snprintf_s(sq + os, sizeof sq - os, _TRUNCATE, " %d", c->gear_seq[i]);
            if (w > 0) os += (size_t)w;
        }
        {
            int cov[7] = { 0, 0, 0, 0, 0, 0, 0 }, rep[7] = { 0, 0, 0, 0, 0, 0, 0 };
            int npos = 0, bad = 0, j, e;
            for (j = 1; j < 256; j++) {
                if (!seen[j]) continue;
                e = sbc_usb_gear_effective(j);
                if (e >= 0) {
                    if (!cov[e]) { cov[e] = 1; rep[e] = j; npos++; }
                } else if (e == -2) {
                    bad = 1;
                }
            }
            if (!miss[0] && !odd[0]) {
                r->status = SBC_TR_OK;
            } else if (npos == 7 && !bad) {
                r->status = SBC_TR_OK;
            } else if (bad || odd[0]) {
                r->status = SBC_TR_UNEXPECTED;
            } else {
                r->status = n >= 2 ? SBC_TR_PARTIAL : SBC_TR_MISSING;
            }
            if (npos == 7 && (miss[0] || odd[0])) {
                _snprintf_s(r->note, sizeof r->note, _TRUNCATE,
                            "Kodierung abweichend, fuers Spiel gleichwertig: "
                            "R=%d N=%d 1=%d 2=%d 3=%d 4=%d 5=%d%s; Reihenfolge:%s",
                            rep[0], rep[1], rep[2], rep[3], rep[4], rep[5], rep[6],
                            seen[0] ? "; 0 gesehen" : "", sq);
            } else {
                _snprintf_s(r->note, sizeof r->note, _TRUNCATE, "%d/7 Gaenge%s%s%s%s%s; Reihenfolge:%s",
                            n, miss[0] ? "; fehlen:" : "", miss,
                            odd[0] ? "; Werte ausser 7..13:" : "", odd,
                            seen[0] ? "; 0 gesehen" : "", sq);
            }
        }
    }
}

int sbc_test_gear_suggest(const sbc_test_ctx_t *c, char *out, size_t cap)
{
    int v[7], i, npos = 0, bad = 0;
    int cov[7] = { 0, 0, 0, 0, 0, 0, 0 };
    out[0] = '\0';
    if (!gear_last7((sbc_test_ctx_t *)c, v)) {
        return 0;
    }
    if (v[0] == 7 && v[1] == 8 && v[2] == 9 && v[3] == 10 && v[4] == 11 &&
        v[5] == 12 && v[6] == 13) {
        return 0; /* Doku-Kodierung - keine Zuordnung noetig */
    }
    for (i = 0; i < c->gear_nseq; i++) {
        int e = sbc_usb_gear_effective(c->gear_seq[i]);
        if (e >= 0 && !cov[e]) { cov[e] = 1; npos++; }
        else if (e == -2) bad = 1;
    }
    if (npos == 7 && !bad) {
        return 0; /* z. B. 254/255/1-5: Spiel versteht nativ */
    }
    _snprintf_s(out, cap, _TRUNCATE, "sbc_gear_map=%d,%d,%d,%d,%d,%d,%d",
                v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
    return 1;
}

void sbc_test_step_led(sbc_test_ctx_t *c, sbc_test_res_t *r)
{
    int key = 0, k;
    uint64_t t0;
    unsigned n0 = c->io->nout(c->io->ctx);
    t_emitf(c, "  -> Lampentest: alle Lampen leuchten nacheinander (je 0,6 s) - bitte zusehen.\n");
    if (sbc_test_led_seq_run(c->io, 600, 1) != SBC_OK) {
        r->status = SBC_TR_MISSING;
        _snprintf_s(r->note, sizeof r->note, _TRUNCATE, "OUT-Report senden scheitert");
        return;
    }
    if (c->io->aborted(c->io->ctx)) {
        /* W143: Abbruch waehrend der Folge -> kein Schein-Befund, Aufrufer
         * wertet NOTRUN ("ABGEBROCHEN"). */
        c->aborted = 1;
        return;
    }
    if (!c->io->is_live(c->io->ctx)) {
        /* Simulation: gesendete Folge gegen das OUT-Format pruefen */
        char msg[128];
        if (sbc_test_led_seq_verify_skip(c->io, n0, 1, msg, sizeof msg) != 0) {
            r->status = SBC_TR_UNEXPECTED;
            _snprintf_s(r->note, sizeof r->note, _TRUNCATE, "OUT-Format: %s", msg);
            return;
        }
    }
    if (c->io->discard_keys) {
        c->io->discard_keys(c->io->ctx); /* waehrend der Folge getippte Tasten verwerfen */
    }
    t_emitf(c, "  -> Haben alle 37 Lampen in der Reihenfolge geleuchtet und die 2 reservierten "
               "Stellen nicht? [j/n, Enter = ueberspringen]\n");
    t0 = t_now(c);
    for (;;) {
        k = t_next(c, t_now(c) + 100, &key);
        if (c->aborted) return;
        if (k == SBC_TEV_KEY && (key == 'j' || key == 'J' || key == 'y' || key == 'n' ||
                            key == 'N' || key == '\r')) break;
        if (t_now(c) - t0 > SBC_TEST_STEP_TIMEOUT_MS) { key = 0; break; }
    }
    if (key == 'j' || key == 'J' || key == 'y') {
        r->status = SBC_TR_OK;
        _snprintf_s(r->note, sizeof r->note, _TRUNCATE, "vom Nutzer bestaetigt%s",
                    c->io->is_live(c->io->ctx) ? "" : " (simuliert, OUT-Format geprueft)");
    } else if (key == 'n' || key == 'N') {
        r->status = SBC_TR_UNEXPECTED;
        _snprintf_s(r->note, sizeof r->note, _TRUNCATE,
                    "Nutzer: NEIN - bitte notieren, welche Lampe falsch/dunkel war "
                    "(--leds seq erneut)");
    } else {
        r->status = SBC_TR_MISSING;
        _snprintf_s(r->note, sizeof r->note, _TRUNCATE, "keine Antwort%s",
                    c->io->is_live(c->io->ctx) ? "" : " (OUT-Format geprueft)");
    }
}

/* cfg-Vorschlag je Achse (Spiel-Einheiten, Rand 2 nach innen). 1 = Zeilen. */
int sbc_test_axis_suggest(int a, const sbc_test_res_t *r, char *cal,
                          size_t ccap, char *dead, size_t dcap)
{
    int lo = sbc_test_game_units(a, r->mn), hi = sbc_test_game_units(a, r->mx);
    int rl = sbc_test_game_units(a, r->rest_lo), rh = sbc_test_game_units(a, r->rest_hi);
    int mid = sbc_test_game_units_round(a, (r->rest_lo + r->rest_hi) / 2);
    int d;
    const char *nm = sbc_usb_axis_name(sbc_test_axes[a].cal_axis);
    cal[0] = dead[0] = '\0';
    if (!r->measured) {
        return 0;
    }
    if (sbc_test_axes[a].kind == 2) {
        d = (rh - rl) + 2;
        _snprintf_s(dead, dcap, _TRUNCATE, "sbc_dead_%s=%d", nm, d);
        if (r->status == SBC_TR_OK && hi - 2 > rl + d) {
            _snprintf_s(cal, ccap, _TRUNCATE, "sbc_cal_%s=%d,%d", nm, rl, hi - 2);
        }
        return 1;
    }
    if (sbc_usb_axis_is_aim(sbc_test_axes[a].cal_axis)) {
        /* W141: Reibungshebel - keine Totzone; Raender (Zwei-Wert-Form)
         * nur, wenn der Hub den Bereich nicht ausschoepft. */
        if (r->status == SBC_TR_OK && (lo > 0 || hi < 255) && lo + 2 < hi - 2) {
            _snprintf_s(cal, ccap, _TRUNCATE, "sbc_cal_%s=%d,%d", nm, lo + 2, hi - 2);
        }
        return 1;
    }
    d = (rh - mid > mid - rl ? rh - mid : mid - rl) + 2;
    _snprintf_s(dead, dcap, _TRUNCATE, "sbc_dead_%s=%d", nm, d);
    if (r->status == SBC_TR_OK && lo + 2 < mid - d && hi - 2 > mid + d) {
        _snprintf_s(cal, ccap, _TRUNCATE, "sbc_cal_%s=%d,%d,%d", nm, lo + 2, mid, hi - 2);
    }
    return 1;
}

/* Kodierungs-Urteil: Aiming u16 (Hub entscheidet, W141: Reibungshebel -
 * die Ruhe ist eine beliebige Stellung und keine Warnung). */
const char *sbc_test_axis_verdict(int a, const sbc_test_res_t *r)
{
    int c = (r->rest_lo + r->rest_hi) / 2;
    if (sbc_test_axes[a].kind == 0) {
        if (!r->measured) return "nicht bewegt - keine Aussage (Ruhe waere beliebig, korrekt)";
        if (r->mn <= 0x1000 && r->mx >= 0xF000)
            return "u16 bestaetigt (Hub voll); Reibungshebel: Ruhe = beliebige Stellung (korrekt)";
        return "Hub schoepft Bereich nicht aus (s. Vorschlag); Reibungshebel: Ruhe beliebig (korrekt)";
    }
    if (sbc_test_axes[a].kind == 1) {
        if (abs(c) <= 0x1000) return "s16 bestaetigt (Ruhe nahe 0)";
        if (abs(c) >= 0x7000) return "Ruhe nahe +-0x8000 - doch u16? bitte melden";
        return "Ruhe weit neben 0 - bitte melden";
    }
    if (c <= 0x0400) {
        return r->mx >= 0xFE00 ? (r->mx > 0xFF00 ? "Pedal ok (max > 0xFF00!)" : "Pedal ok (0..0xFF00)") : "Ruhe ok";
    }
    return "Pedal-Ruhe nicht 0 - Totzone noetig";
}

void sbc_test_stamp(char *out, size_t cap, int file_fmt)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_s(&tm, &t);
    strftime(out, cap, file_fmt ? "%Y%m%d_%H%M%S" : "%Y-%m-%d %H:%M:%S", &tm);
}

static void rep_line(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (f) vfprintf(f, fmt, ap);
    va_end(ap);
}

static void rep_pf(void *opaque, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (opaque) vfprintf((FILE *)opaque, fmt, ap);
    va_end(ap);
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

int sbc_test_write_report(const sbc_test_ctx_t *c,
                          const sbc_test_step_t *steps,
                          const sbc_test_res_t *res, const char *out_path,
                          const char *src_desc, const char *tool_tag,
                          const char *version, int aborted,
                          sbc_test_sum_t *sum)
{
    char path[MAX_PATH], ts[32], nm[96];
    int i, ns = SBC_TEST_N_STEPS;
    FILE *f = NULL;
    if (out_path && out_path[0]) {
        strcpy_s(path, sizeof path, out_path);
    } else {
        char fn[64];
        sbc_test_stamp(ts, sizeof ts, 1);
        _snprintf_s(fn, sizeof fn, _TRUNCATE, "sbc_test_%s.txt", ts);
        path_next_to_exe(path, sizeof path, fn);
    }
    if (fopen_s(&f, path, "w") != 0 || !f) {
        t_emitf(c, "FAIL: Ergebnisdatei %s nicht schreibbar\n", path);
        return 1;
    }
    {
        int cnt[5] = { 0, 0, 0, 0, 0 };
        char dt[32];
        for (i = 0; i < ns; i++) cnt[res[i].status]++;
        sbc_test_stamp(dt, sizeof dt, 0);
        rep_line(f, "Steel-Battalion-Controller - Ergebnis %s (%s)\n", tool_tag, version);
        rep_line(f, "Datum: %s   Quelle: %s%s\n", dt, src_desc, aborted ? "   ABGEBROCHEN" : "");
        rep_line(f, "Summe: %d Schritte - OK %d, fehlt %d, unerwartet %d, teilweise %d, nicht gelaufen %d\n",
                 ns, cnt[SBC_TR_OK], cnt[SBC_TR_MISSING], cnt[SBC_TR_UNEXPECTED], cnt[SBC_TR_PARTIAL], cnt[SBC_TR_NOTRUN]);
        {
            double secs = (c->io->now(c->io->ctx) - c->t_first) / 1000.0;
            rep_line(f, "Reports: %llu in %.1f s (%.0f/s)%s\n",
                     (unsigned long long)c->n_rep, secs, secs > 0 ? c->n_rep / secs : 0.0,
                     c->used_getreport ? "; Start per GET_REPORT (keine Leerlauf-Reports)" : "");
        }
        rep_line(f, "\n== Elemente ==\n");
        for (i = 0; i < ns; i++) {
            rep_line(f, "%-34s %-10s %s\n", sbc_test_step_name(&steps[i], nm, sizeof nm),
                     sbc_test_resname(res[i].status), res[i].note);
        }
        rep_line(f, "\n== Achsen (Rohwerte u16/s16; Spiel nutzt das High-Byte) ==\n");
        for (i = 0; i < ns; i++) {
            int a;
            const sbc_test_res_t *r = &res[i];
            if (steps[i].kind != SBC_TK_AXIS || r->status == SBC_TR_NOTRUN) continue;
            a = steps[i].idx;
            if (sbc_test_axes[a].kind == 1) {
                rep_line(f, "%-26s Ruhe %d..%d  min %d  max %d  | Spiel lo=%d mitte=%d hi=%d  | %s\n",
                         sbc_test_axes[a].name, r->rest_lo, r->rest_hi, r->mn, r->mx,
                         sbc_test_game_units(a, r->mn), sbc_test_game_units_round(a, (r->rest_lo + r->rest_hi) / 2),
                         sbc_test_game_units(a, r->mx), sbc_test_axis_verdict(a, r));
            } else {
                rep_line(f, "%-26s Ruhe 0x%04X..0x%04X  min 0x%04X  max 0x%04X  | Spiel lo=%d mitte=%d hi=%d  | %s\n",
                         sbc_test_axes[a].name, r->rest_lo, r->rest_hi, r->mn, r->mx,
                         sbc_test_game_units(a, r->mn), sbc_test_game_units_round(a, (r->rest_lo + r->rest_hi) / 2),
                         sbc_test_game_units(a, r->mx), sbc_test_axis_verdict(a, r));
            }
        }
        rep_line(f, "\n");
        sbc_test_unk_report(&c->unk, rep_pf, f);
        rep_line(f, "\n== Vorschlag fuer user.cfg (Abschnitt [input], nur bei Bedarf eintragen) ==\n");
        rep_line(f, "# Totzone = Ruhe-Schwankung + 2; Raender 2 Einheiten nach innen, damit der\n"
                    "# volle Ausschlag sicher erreicht wird. Ohne diese Zeilen: Rohwerte (bitgleich).\n");
        rep_line(f, "[input]\n");
        rep_line(f, "sbc_usb=1\n");
        for (i = 0; i < ns; i++) {
            char cal[64], dead[64];
            if (steps[i].kind != SBC_TK_AXIS || res[i].status == SBC_TR_NOTRUN) continue;
            if (sbc_test_axis_suggest(steps[i].idx, &res[i], cal, sizeof cal, dead, sizeof dead)) {
                if (cal[0]) rep_line(f, "%s\n", cal);
                if (dead[0]) rep_line(f, "%s\n", dead);
            }
        }
        {
            char gm[96];
            if (sbc_test_gear_suggest(c, gm, sizeof gm)) {
                rep_line(f, "# Gang-Byte weicht von 7..13 ab: Zuordnung aus der geschalteten Reihenfolge\n"
                            "# R,N,1,2,3,4,5 (nur eintragen, wenn genau so geschaltet wurde)\n");
                rep_line(f, "%s\n", gm);
            }
        }
        if (sum) {
            memcpy(sum->n, cnt, sizeof cnt);
            sum->steps = ns;
            strcpy_s(sum->path, sizeof sum->path, path);
        }
    }
    fclose(f);
    return 0;
}

int sbc_test_run_all(const sbc_test_io_t *io, const char *out_path,
                     const char *src_desc, const char *tool_tag,
                     const char *version, sbc_test_sum_t *sum)
{
    static sbc_test_step_t steps[SBC_TEST_N_STEPS];
    static sbc_test_res_t res[SBC_TEST_N_STEPS];
    uint8_t tun_seen[16], gear_seen[256];
    sbc_test_ctx_t ctx;
    char nm[96];
    int ns, i, rc_exit;
    memset(res, 0, sizeof res);
    memset(tun_seen, 0, sizeof tun_seen);
    memset(gear_seen, 0, sizeof gear_seen);
    for (i = 0; i < SBC_TEST_N_STEPS; i++) res[i].status = SBC_TR_NOTRUN;
    ns = sbc_test_build_steps(steps);
    sbc_test_ctx_init(&ctx, io);
    t_emitf(&ctx, "=== Steel-Battalion-Controller: gefuehrte Pruefung (%d Schritte) ===\n", ns);
    t_emitf(&ctx, "Jeder Schritt erkennt das erwartete Feld selbst. Enter = Schritt ueberspringen/"
                  "fertig, Esc/Ctrl+C = Abbruch.\n");
    if (!sbc_test_first_report(&ctx)) {
        t_emitf(&ctx, "sbc_probe: kein Report vom Controller - Abbruch.\n");
        return 1;
    }
    sbc_test_unk_add(&ctx.unk, ctx.cur, ctx.len);
    ctx.t_first = io->now(io->ctx);
    for (i = 0; i < ns && !ctx.aborted; i++) {
        sbc_test_res_t *r = &res[i];
        memset(r, 0, sizeof *r);
        ctx.skip_key = 0;
        t_emitf(&ctx, "\n[%d/%d] %s\n", i + 1, ns, sbc_test_step_name(&steps[i], nm, sizeof nm));
        t_prog(&ctx, SBC_TP_STEP_START, i, steps[i].kind);
        switch (steps[i].kind) {
        case SBC_TK_BTN: sbc_test_step_button(&ctx, steps[i].idx, 0, r); break;
        case SBC_TK_TOG: sbc_test_step_button(&ctx, steps[i].idx, 1, r); break;
        case SBC_TK_AXIS: sbc_test_step_axis(&ctx, steps[i].idx, r); break;
        case SBC_TK_TUNER: sbc_test_step_tuner(&ctx, r, tun_seen); break;
        case SBC_TK_GEAR: sbc_test_step_gear(&ctx, r, gear_seen); break;
        default: sbc_test_step_led(&ctx, r); break;
        }
        t_prog(&ctx, SBC_TP_STEP_DONE, i, r->status);
        if (ctx.aborted) {
            r->status = SBC_TR_NOTRUN;
            break;
        }
        t_emitf(&ctx, "     = %s%s%s\n", sbc_test_resname(r->status), r->note[0] ? ": " : "", r->note);
    }
    /* Ergebnisdatei (Reihenfolge der Meldungen wie bisher:
     * Zusammenfassung, dann Pfad). */
    {
        sbc_test_sum_t local;
        sbc_test_sum_t *sp = sum ? sum : &local;
        int cnt[5] = { 0, 0, 0, 0, 0 };
        if (sbc_test_write_report(&ctx, steps, res, out_path, src_desc, tool_tag,
                                  version, ctx.aborted, sp) != 0) {
            return 1;
        }
        for (i = 0; i < ns; i++) cnt[res[i].status]++;
        rc_exit = (cnt[SBC_TR_OK] == ns) ? 0 : 1;
        t_emitf(&ctx, "\n=== Ergebnis: OK %d / %d (fehlt %d, unerwartet %d, teilweise %d) ===\n",
                cnt[SBC_TR_OK], ns, cnt[SBC_TR_MISSING], cnt[SBC_TR_UNEXPECTED], cnt[SBC_TR_PARTIAL]);
        t_emitf(&ctx, "Ergebnisdatei: %s\n", sp->path);
        t_emitf(&ctx, "Bitte diese Datei melden (Inhalt kopieren oder Datei schicken).\n");
    }
    return rc_exit;
}
