/* loc_guard.c - W334 LOC-SEC-CLIENT: Gastfilter (siehe loc_guard.h, SECURITY.md §6). */
#include "loc_guard.h"
#include <string.h>

/* Laengentabelle des Spiels FUN_0004EFE0 (Sprungtabelle 0x4F058, Typ 1..0x38, aus der LoC-XBE gelesen, W331,
 * SECURITY.md §6.2). 0 = Zerleger kaeme nicht weiter (09 15 16 17 18 19 1D 28 29) -> nie zustellen.
 * Typ 0x34 rechnet die Laenge selbst (12 + 14 * (int8)Rahmen[4]) und steht hier mit 0. */
static const uint16_t k_len[0x39] = {
    /* 00 */ 0,      0x14,  100,   0x2C4, 0x2C4, 4,     0x308, 4,
    /* 08 */ 4,      0,     100,   0x308, 0x34,  0x308, 0x1A0, 4,
    /* 10 */ 4,      4,     4,     0x16C, 0x10,  0,     0,     0,
    /* 18 */ 0,      0,     8,     0x4F8, 8,     0,     4,     4,
    /* 20 */ 0x1008, 8,     0x100C, 8,    8,     8,     4,     0x2A8,
    /* 28 */ 0,      0,     8,     8,     0x10,  0x10,  0x1008, 8,
    /* 30 */ 0x1A0,  4,     4,     4,     0,     8,     8,     4,
    /* 38 */ 8,
};

int lguard_frame_len(const uint8_t *f, int avail)
{
    unsigned typ;
    if (avail < 2) return -1;
    typ = (unsigned)f[0] | ((unsigned)f[1] << 8); /* u16 LE wie movzx im Spiel */
    if (typ == 0x34) {
        if (avail < 5) return -1;
        if (f[4] >= 0x80) return 0; /* Spiel: movsx -> negative Laenge -> rep movsd mit riesigem Zaehler */
        /* Stufe 2 (W334, B, LOC_RE1 §6.5): der Handler FUN_0004E990 kopiert n*14 Byte ohne Pruefung in einen
         * Ringschlitz mit 16 Byte Nutzlast (0x4E9BE..0x4E9D1) - n >= 2 ueberschreibt Nachbarschlitze. Legitim n = 1. */
        if (f[4] > 1) return 0;
        return 12 + 14 * (int)f[4];
    }
    if (typ >= sizeof k_len / sizeof k_len[0]) return 0;
    return (int)k_len[typ];
}

/* ---------------------------------------------------------------- Inhaltsregeln Stufe 2 (§6.6 Punkt 1)
 * W339 SEC-A3. Jede Regel mit Disassembly-Beleg (docs/loc/LOC_RE1.md §6.6, BERICHT_M2 "W339: SEC-A3"); Grenzwerte aus
 * dem Original (Tabellen-/Puffergroessen, Sender-Code), nie enger als das, was das Spiel selbst erzeugt.
 *
 * Spielertabellen-Eintrag (0x300 B, T[i] = G+0xA8+i*0x300, G = *(0x361708); P = *(0x3116B0) = T): der Host schickt in
 * 0x0B je einen ganzen Eintrag ab Rahmen+8 (Bauer FUN_00054530, leere Plaetze genullt), ebenso 0x0D (FUN_000574D0)
 * und 0x06 (FUN_00057080, UDP 1202). Der Beitretende schickt in 0x03/0x04 (FUN_00053F60/00054030) Gamertag
 * (Rahmen+0x10) und Benutzerdatensatz (Rahmen+0x20, 0x2A4 B = Anfang seines Pilotblocks), der Host uebernimmt sie
 * als Eintrag+0x34 / Eintrag+0x4C (FUN_00055B10/00055CE0 -> FUN_0005A590). 0x27 traegt den Datensatz ab Rahmen+4
 * (FUN_00056C70 -> P[conn]+0x4C).
 *  - Gamertag Eintrag+0x34 (16 B): FUN_000D9890 kopiert ohne Grenze (do{}while(c)) in ein 16-B-Feld -> NUL in 16 B.
 *  - Pilotname Datensatz+0x28: mit Leerzeichen auf 16 B aufgefuellt (FUN_0010E1B0 kopiert die Eingabezellen roh),
 *    ohne NUL; als C-String laeuft er in die Optionsbytes +0x38/+0x39 (FUN_000EB7C0), Nation +0x3A, Lager +0x3B und
 *    die Garage +0x3C (8-B-Eintraege, FUN_00126E30 nullt alles und setzt nur [0] belegt/[1] VT-Id) -> spaetestens
 *    Name+0x16 ist im Original 0, ein echter Name-String hat hoechstens 22 Zeichen. Leser: _swprintf(L"%S") in
 *    32-wchar-Stapelpuffer (FUN_000F9ED0, FUN_000F87A0, FUN_001070A0), strcpy in 24-B-Feld (FUN_000D9890),
 *    sprintf("%s") nach G+0xDB9C (FUN_00055E40/0005B1A0/0005CAF0, 19 B bis zum Flag +0xDBAF - das Original
 *    selbst schreibt dort bis zu 4 Byte darueber). Regel: NUL innerhalb LGUARD_NAME_NUL = 23 Byte.
 *  - Portrait Datensatz+0x26 (u16) / +0x05: FUN_000F8730 -> FUN_000ED250 -> FUN_000ED140 laedt den Zeiger
 *    (&DAT_00726508)[0x0E + u16 + (Byte != 0 ? 0x54 : 0)] und dereferenziert ihn; 0x96 Eintraege (FUN_00121DE0,
 *    0x121E28; DAT_0027E098 ebenso lang) -> Index < 0x96.
 *  - VT-Id: 0x0E/0x30 Rahmen+6 (Ausstattung L = Rahmen+4, FUN_00056520/00056800), 0x2A/0x2B Rahmen+4
 *    (FUN_000566B0 -> L+2). FUN_00127620 liest (&DAT_005BE4DC)[vt*0x30] und nutzt den Wert ueber FUN_0006ABB0 als
 *    Index; Tabelle 35 Eintraege (FUN_00127B80: Schleife bis 0x1A40, Schritt 0xC0), Garage-VT-Id 0..0x22 -> vt <= 0x22.
 *  - Blocklaenge 0x20/0x2E Rahmen+4 (Daten ab +8) und 0x22 Rahmen+4 (Daten ab +0xC): FUN_000569D0/00056770/00056AD0
 *    kopieren n Byte ohne Pruefung (0x22 nach G+0xEF34, Platz bis zum Flag G+0xFF34 = 0x1000) -> n <= 0x1000 (= Nutzlast
 *    des Rahmens). Die Summe der Bloecke (0x20/0x2E in 0x5040-B-Puffer) prueft zusaetzlich der Aufruf-Hook mit dem
 *    Spielzustand (loc_net.c, "Blockpuffer").
 *  - 0x25 Rahmen[4] (int8): FUN_00056BF0 schreibt mit diesem Index in die 10-Eintraege-Tabelle DAT_003116AC
 *    (FUN_0004F7F0); Sender FUN_00055A00 schickt den eigenen Platz G+0xD128 -> 0..9.
 * Ohne Index-/Stringverwendung (nur Wert, keine Regel): 0x0A (Anzahl G+0xA4 nur Vergleich, Zeitwerte, Status je
 * Platz mit internem Index), 0x13 (XNADDR/Schluessel nur an XNetQos*, HLE), 0x1B, 0x14, 0x0C, die 4-/8-B-Typen
 * ausser 0x25/0x2A/0x2B, Platz in der Tabelle (immer interner Zaehler), Lager/Nation/Garage im Datensatz, Waffen-Ids
 * der Ausstattung (FUN_00127360 liest hoechstens 1 MB hinter DAT_002860A8, im Abbild, nur Wert). UDP 0x38 haengt am
 * Spielzustand (Absender muss einen Platz haben) und wird im HLE geprueft (loc_net.c). */
#define LGUARD_NAME_NUL 23

static int bad_reason(const uint8_t *f)
{
    if (f[0] == 0x34 && f[1] == 0) return f[4] >= 0x80 ? LGUARD_V_NEG : LGUARD_V_CONTENT;
    return LGUARD_V_TYPE;
}

static int nul_within(const uint8_t *p, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (p[i] == 0) return 1;
    }
    return 0;
}

static unsigned rd16(const uint8_t *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Benutzerdatensatz (0x2A4 B); r zeigt auf mindestens 0x28 + LGUARD_NAME_NUL Byte */
static int rec_ok(const uint8_t *r)
{
    unsigned idx = 0x0Eu + rd16(r + 0x26) + (r[0x05] ? 0x54u : 0u);
    if (idx >= 0x96u) return 0;
    return nul_within(r + 0x28, LGUARD_NAME_NUL);
}

/* Spielertabellen-Eintrag (0x300 B) */
static int entry_ok(const uint8_t *e)
{
    return nul_within(e + 0x34, 16) && rec_ok(e + 0x4C);
}

/* 1 = Typ hat Inhaltsregeln auf TCP (der Hook-Pfad muss dann den ganzen Rahmen sehen, bevor er freigibt) */
static int has_content_rule(unsigned typ)
{
    switch (typ) {
    case 0x0B: case 0x0D: case 0x0E: case 0x30: case 0x27: case 0x2A: case 0x2B: case 0x25:
    case 0x20: case 0x2E: case 0x22:
        return 1;
    default:
        return 0;
    }
}

/* port 1000 = TCP-Spielstrom, sonst UDP-Spielport. f = ganzer Rahmen mit Laenge n (bereits nach Tabelle geprueft). */
/* W373 LOC-PUBLIC-LOBBY (LOC_BOOT §49.2, B): Host-Info Typ 2 (UDP 1201, 100 B). Der Empfaenger kopiert Rahmen+0xC..0x63
 * in die Hostliste; spaetere Leser: Pilotname +0x50 per swprintf("%S") ohne Grenze in wchar[16] (FUN_000F2020) ->
 * NUL in 16 B; Sitze je Seite +0x4A/+0x4B (int8, movsx) -> Summe als Index in eine 11er-Tabelle (0x2518D4) und als
 * Laenge -> 0..10, Summe <= 10; Mission +0x4D -> Index in 0x27CCB8 (27 Eintraege, 0x18/0x1A = -1) -> < 0x1B ohne 0x18/0x1A.
 * Lager/Modus/Optionen/Zaehler sind nur Werte (begrenzter switch bzw. Anzeige). */
static int hostinfo_ok(const uint8_t *f)
{
    int a = (int8_t)f[0x4A], b = (int8_t)f[0x4B];
    unsigned ms = f[0x4D];
    return nul_within(f + 0x50, 16) && a >= 0 && b >= 0 && a + b <= 10 && ms < 0x1Bu && ms != 0x18u && ms != 0x1Au;
}

static int content_ok(uint16_t port, const uint8_t *f, int n)
{
    unsigned typ = rd16(f);
    if (port == 1000) {
        switch (typ) {
        case 0x0B: case 0x0D: return n == 0x308 && entry_ok(f + 8);
        case 0x27: return n == 0x2A8 && rec_ok(f + 4);
        case 0x0E: case 0x30: return n == 0x1A0 && rd16(f + 6) <= 0x22u;
        case 0x2A: case 0x2B: return n == 8 && rd16(f + 4) <= 0x22u;
        case 0x20: case 0x2E: return n == 0x1008 && rd32(f + 4) <= 0x1000u;
        case 0x22: return n == 0x100C && rd32(f + 4) <= 0x1000u;
        case 0x25: return n == 8 && f[4] <= 9; /* int8 0..9 (>= 0x80 negativ) */
        default: return 1;
        }
    }
    if (port == 1201 && typ == 0x02 && n == 100) return hostinfo_ok(f); /* W373 */
    if (port == 1202) {
        if ((typ == 0x03 || typ == 0x04) && n == 0x2C4) return nul_within(f + 0x10, 16) && rec_ok(f + 0x20);
        if (typ == 0x06 && n == 0x308) return entry_ok(f + 8);
    }
    return 1;
}

void lguard_tcp_init(lguard_tcp *g)
{
    g->len = 0;
    g->bad = 0;
}

int lguard_tcp_feed(lguard_tcp *g, const uint8_t *in, int n, uint8_t *out, int cap)
{
    int off = 0, o = 0;
    if (g->bad) return -1;
    if (n < 0 || (n > 0 && !in) || cap < 0) {
        g->bad = LGUARD_V_STATE;
        return -1;
    }
    if (g->len + n > LGUARD_TCP_MAX) {
        g->bad = LGUARD_V_BUFFER;
        g->len = 0;
        return -1;
    }
    if (n > 0) {
        memcpy(g->buf + g->len, in, (size_t)n);
        g->len += n;
    }
    while (g->len - off >= 2) {
        int fl = lguard_frame_len(g->buf + off, g->len - off);
        if (fl == 0) {
            g->bad = bad_reason(g->buf + off);
            break;
        }
        if (fl < 0 || g->len - off < fl) break; /* Rahmen noch unvollstaendig */
        if (!content_ok(1000, g->buf + off, fl)) {
            g->bad = LGUARD_V_CONTENT;
            break;
        }
        if (o + fl > cap) break; /* Ausgabe voll: Rest beim naechsten Aufruf */
        memcpy(out + o, g->buf + off, (size_t)fl);
        o += fl;
        off += fl;
    }
    if (g->bad) {
        /* fail closed: auch die schon geprueften Rahmen dieses Aufrufs nicht zustellen */
        memset(g->buf, 0, sizeof g->buf);
        g->len = 0;
        return -1;
    }
    if (off > 0) {
        memmove(g->buf, g->buf + off, (size_t)(g->len - off));
        g->len -= off;
    }
    return o;
}

int lguard_walk_tcp(lguard_walk *w, const uint8_t *p, int n, int commit, int *why)
{
    int pos = 0, rem = w->rem;
    if (why) *why = LGUARD_OK;
    if (n < 0 || rem < 0) {
        if (why) *why = LGUARD_V_STATE;
        return -1;
    }
    while (pos < n) {
        int fl;
        if (rem > 0) {
            int k = rem < n - pos ? rem : n - pos;
            rem -= k;
            pos += k;
            continue;
        }
        fl = lguard_frame_len(p + pos, n - pos);
        if (fl == 0) {
            if (why) *why = bad_reason(p + pos);
            return -1;
        }
        if (fl < 0) break; /* Kopf noch nicht entscheidbar: zurueckhalten */
        if (has_content_rule((unsigned)p[pos] | ((unsigned)p[pos + 1] << 8))) {
            /* W339: Inhaltsregel - erst freigeben, wenn der ganze Rahmen vorliegt und gueltig ist */
            if (n - pos < fl) break;
            if (!content_ok(1000, p + pos, fl)) {
                if (why) *why = LGUARD_V_CONTENT;
                return -1;
            }
        }
        rem = fl;
    }
    if (commit) w->rem = rem;
    return pos;
}

void lguard_walk_advance(lguard_walk *w, const uint8_t *p, int avail, int take)
{
    int pos = 0;
    while (pos < take) {
        if (w->rem > 0) {
            int k = w->rem < take - pos ? w->rem : take - pos;
            w->rem -= k;
            pos += k;
            continue;
        }
        {
            int fl = lguard_frame_len(p + pos, avail - pos);
            if (fl <= 0) return; /* nicht freigegeben - darf nicht vorkommen (Aufrufer nimmt nur Freigegebenes) */
            w->rem = fl;
        }
    }
}

int lguard_udp_check(uint16_t game_port, const uint8_t *d, int n, int *why)
{
    unsigned typ;
    int need = 0;
    if (why) *why = LGUARD_V_TYPE;
    if (!d || n < 2) return 0;
    typ = (unsigned)d[0] | ((unsigned)d[1] << 8);
    /* Positivliste Stufe 1 (SECURITY.md §6.3); gesperrt: 0x09, 0x15-0x19, 0x28/0x29, 0x39-0x3B, Port 1203 (VDP) */
    if (game_port == 1201) {
        if (typ == 0x01) need = 0x14;
        else if (typ == 0x02) need = 100;
    } else if (game_port == 1202) {
        switch (typ) {
        case 0x03: case 0x04: need = 0x2C4; break;
        case 0x05: case 0x08: need = 4; break;
        case 0x07: need = 6; break; /* W373 (B): Sender FUN_00054210 schickt 6 B, Byte 4 = Grund (vorher 4 -> Trennung) */
        case 0x06: need = 0x308; break;
        case 0x38: need = 8; break;
        default: break;
        }
    }
    if (need == 0 || n != need) return 0;
    if (!content_ok(game_port, d, n)) {
        if (why) *why = LGUARD_V_CONTENT;
        return 0;
    }
    if (why) *why = LGUARD_OK;
    return 1;
}

int lguard_udp_ok(uint16_t game_port, const uint8_t *d, int n)
{
    return lguard_udp_check(game_port, d, n, NULL);
}

int lguard_bucket_take(lguard_bucket *b, double now_ms, double rate_per_s, double burst)
{
    if (!b->init) {
        b->init = 1;
        b->tokens = burst;
        b->last = now_ms;
    }
    if (now_ms > b->last) {
        b->tokens += (now_ms - b->last) * rate_per_s / 1000.0;
        if (b->tokens > burst) b->tokens = burst;
        b->last = now_ms;
    }
    if (b->tokens < 1.0) return 0;
    b->tokens -= 1.0;
    return 1;
}

const char *lguard_reason(int why)
{
    switch (why) {
    case LGUARD_V_TYPE: return "Rahmentyp";
    case LGUARD_V_NEG: return "0x34-Anzahl";
    case LGUARD_V_BUFFER: return "Strompuffer";
    case LGUARD_V_CONTENT: return "Rahmeninhalt";
    case LGUARD_V_STATE: return "Zustand";
    default: return "-";
    }
}
