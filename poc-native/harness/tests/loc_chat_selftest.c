/* loc_chat_selftest.c - W437 LOC-CHAT: Kern des Textchats (loc_chat.c) ohne Spiel/Fenster/Netz.
 * Codec (Rundreise, kaputte und zu lange Pakete), Bereinigung (Steuer-, Bidi-, Nullbreiten-Zeichen, UTF-8-Fehler,
 * Laenge), Ratenbremse (Senden/Empfangen), Stummschalten, Eingabezeile, Befehle, Mutations-Fuzzer (fester Seed),
 * Loeschen (Minidump enthaelt den Verlauf danach nicht mehr).
 * Aufruf: loc_chat_selftest [--fuzz N] [--seed S] [--dump-dir ORDNER]  (Minidump nur mit --dump-dir, Datei wird
 * danach geloescht). Alle Texte synthetisch, keine Eingaben eines Nutzers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <dbghelp.h>
#include "loc_chat.h"

static int s_ok, s_n;
#define CHECK(c, msg)                                                   \
    do {                                                                \
        s_n++;                                                          \
        if (c) s_ok++;                                                  \
        else printf("FAIL: %s (Zeile %d)\n", msg, __LINE__);            \
    } while (0)

static int san8(const char *s, int n, uint32_t *cps) { return lch_sanitize_u8((const uint8_t *)s, n, cps, LCH_MAX_CHARS); }
static int cps_eq_ascii(const uint32_t *c, int n, const char *a)
{
    int i, l = (int)strlen(a);
    if (n != l) return 0;
    for (i = 0; i < n; i++)
        if (c[i] != (unsigned char)a[i]) return 0;
    return 1;
}

/* Invarianten einer bereinigten Folge */
static int san_invariants(const uint32_t *c, int n)
{
    int i, comb = 0;
    if (n < 0 || n > LCH_MAX_CHARS) return 0;
    if (n > 0 && (c[0] == 0x20 || c[n - 1] == 0x20)) return 0;
    for (i = 0; i < n; i++) {
        if (c[i] == 0x20) {
            if (i > 0 && c[i - 1] == 0x20) return 0;
            comb = 0;
            continue;
        }
        if (!lch_cp_ok(c[i])) return 0;
        if (lch_cp_combining(c[i])) {
            if (++comb > 2) return 0;
        } else {
            comb = 0;
        }
    }
    return 1;
}

static void codec_tests(void)
{
    uint8_t b[LCH_PKT_MAX + 8], t[LCH_MAX_TEXT_BYTES + 1];
    lch_pkt p;
    int len, bad = 0;
    printf("Codec\n");
    len = lch_build(b, sizeof b, 0xA1B2C3D4u, 7u, (const uint8_t *)"Hallo Welt", 10);
    CHECK(len == LCH_HDR + 10, "Paket bauen (16 + Textlaenge)");
    CHECK(memcmp(b, "SBCH", 4) == 0 && b[4] == 1 && b[5] == 1 && b[6] == 10 && b[7] == 0, "Kopf: Kennung, Version, Art, Laenge LE");
    CHECK(lch_parse(b, len, &p) && p.nonce == 0xA1B2C3D4u && p.seq == 7u && p.len == 10 && memcmp(p.text, "Hallo Welt", 10) == 0,
          "Rundreise");
    memset(t, 'x', sizeof t);
    CHECK(lch_build(b, sizeof b, 1, 1, t, LCH_MAX_TEXT_BYTES) == LCH_PKT_MAX, "groesstes Paket (960 Byte Text, W452)");
    CHECK(lch_parse(b, LCH_PKT_MAX, &p) && p.len == LCH_MAX_TEXT_BYTES, "groesstes Paket lesbar");
    CHECK(lch_build(b, sizeof b, 1, 1, t, LCH_MAX_TEXT_BYTES + 1) == 0, "zu langer Text wird nicht gebaut");
    CHECK(lch_build(b, sizeof b, 1, 1, t, 0) == 0, "leerer Text wird nicht gebaut");
    CHECK(lch_build(b, 20, 1, 1, t, 10) == 0, "zu kleiner Puffer");
    CHECK(lch_build(b, sizeof b, 0, 1, t, 3) == 0 && lch_build(b, sizeof b, 1, 0, t, 3) == 0, "Kennung/Nummer 0 nicht gebaut");
    len = lch_build(b, sizeof b, 5, 9, (const uint8_t *)"abc", 3);
    { /* Verfaelschungen */
        uint8_t c[LCH_PKT_MAX + 8];
        int k;
        for (k = 0; k < 12; k++) {
            int cl = len;
            memcpy(c, b, (size_t)len);
            switch (k) {
            case 0: c[0] = 'X'; break;               /* Kennung */
            case 1: c[3] = 'h'; break;
            case 2: c[4] = 2; break;                 /* Version */
            case 3: c[5] = 0; break;                 /* Art */
            case 4: c[6] = 4; break;                 /* Laenge > Rest */
            case 5: c[6] = 2; break;                 /* Laenge < Rest */
            case 6: c[6] = 0; cl = LCH_HDR; break;   /* leer */
            case 7: memset(c + 8, 0, 4); break;      /* Kennung 0 */
            case 8: memset(c + 12, 0, 4); break;     /* Nummer 0 */
            case 9: cl = len - 1; break;             /* abgeschnitten */
            case 10: cl = len + 1; c[len] = 0; break; /* Ueberhang */
            default: c[6] = 0xC1; c[7] = 0x03; cl = LCH_HDR + 0x3C1; break; /* W452: 961 > Grenze */
            }
            bad += !lch_parse(c, cl, &p);
        }
        CHECK(bad == 12, "12 kaputte Pakete verworfen (Kennung, Version, Art, Laenge, leer, 0-Felder, Laengen)");
    }
    CHECK(!lch_parse(b, 0, &p) && !lch_parse(NULL, 20, &p) && !lch_parse(b, LCH_HDR, &p) && !lch_parse(b, LCH_PKT_MAX + 1, &p),
          "Laengen ausserhalb 17..496 verworfen");
}

static void sanitize_tests(void)
{
    uint32_t c[LCH_MAX_CHARS + 4];
    char big[2000];
    int n, i;
    uint8_t u8[LCH_MAX_TEXT_BYTES + 8];
    printf("Bereinigung\n");
    n = san8("Hallo Welt", 10, c);
    CHECK(cps_eq_ascii(c, n, "Hallo Welt"), "ASCII unveraendert");
    n = san8("Gr\xC3\xBC\xC3\x9F" "e \xC3\x84\xC3\x96\xC3\x9C", 14, c);
    CHECK(n == 9 && c[2] == 0xFC && c[3] == 0xDF && c[6] == 0xC4 && c[7] == 0xD6 && c[8] == 0xDC, "Umlaute und sz bleiben");
    n = san8("a\tb\nc\rd\x01" "e\x7F" "f", 11, c);
    CHECK(cps_eq_ascii(c, n, "a b c def") || cps_eq_ascii(c, n, "abcdef"), "Steuerzeichen entfernt");
    n = san8("a\xC2\x85" "b", 4, c);
    CHECK(cps_eq_ascii(c, n, "ab"), "C1 (U+0085) entfernt");
    n = san8("x\xE2\x80\xAEy\xE2\x81\xA6z\xE2\x81\xA9w\xE2\x80\x8F" "v\xD8\x9C" "u", 22, c);
    CHECK(cps_eq_ascii(c, n, "xyzwvu"), "Bidi-Steuerzeichen (RLO, LRI, PDI, RLM, ALM) entfernt");
    n = san8("a\xE2\x80\x8B" "b\xE2\x80\x8D" "c\xE2\x81\xA0" "d\xEF\xBB\xBF" "e\xC2\xAD" "f", 20, c);
    CHECK(cps_eq_ascii(c, n, "abcdef"), "Nullbreiten-Zeichen (ZWSP, ZWJ, WJ, BOM, SHY) entfernt");
    n = san8("a\xE2\x80\xA8" "b\xE2\x80\xA9" "c", 9, c);
    CHECK(cps_eq_ascii(c, n, "abc"), "Zeilen-/Absatztrenner entfernt");
    n = san8("   a    b\xC2\xA0\xE3\x80\x80" "c   ", 16, c);
    CHECK(cps_eq_ascii(c, n, "a b c"), "Leerraum zusammengefasst, Rand gekuerzt");
    n = san8("\xE2\x80\x8B \xE2\x80\xAE", 7, c);
    CHECK(n == 0, "nur Unsichtbares -> leer");
    {
        char z[64];
        int k = 0, j;
        z[k++] = 'a';
        for (j = 0; j < 10; j++) { z[k++] = (char)0xCC; z[k++] = (char)0x81; } /* U+0301 x10 */
        z[k++] = 'b';
        n = san8(z, k, c);
        CHECK(n == 4 && c[0] == 'a' && c[1] == 0x301 && c[2] == 0x301 && c[3] == 'b', "Kombinierende Zeichen hoechstens 2 in Folge");
        n = san8("\xCC\x81" "a", 3, c);
        CHECK(cps_eq_ascii(c, n, "a"), "Kombinierendes ohne Grundzeichen entfernt");
    }
    n = san8("\xC0\x80" "a\xE0\x80\x80" "b\xED\xA0\x80" "c\xF5\x80\x80\x80" "d\x80\xBF" "e\xE2\x82" "f\xF0\x9F\x98", 26, c);
    CHECK(cps_eq_ascii(c, n, "abcdef"), "UTF-8-Fehler (ueberlang, Surrogat, > U+10FFFF, lose Folgebytes, abgeschnitten) entfernt");
    n = san8("\xF0\x9F\x98\x80!", 5, c);
    CHECK(n == 2 && c[0] == 0x1F600 && c[1] == '!', "4-Byte-Zeichen erlaubt");
    n = san8("p\xEE\x80\x80q\xF3\xA0\x80\x81r\xEF\xBF\xBE" "s\xEF\xBF\xBC" "t", 18, c);
    CHECK(cps_eq_ascii(c, n, "pqrst"), "Private Nutzung, Tag-Zeichen, Nichtzeichen, Objekt-Ersatz entfernt");
    memset(big, 'A', sizeof big);
    n = san8(big, 500, c);
    CHECK(n == LCH_MAX_CHARS && LCH_MAX_CHARS == 240, "W452: ueber 240 Zeichen -> 240");
    for (i = 0; i < 960; i += 4) { big[i] = (char)0xF0; big[i + 1] = (char)0x9F; big[i + 2] = (char)0x98; big[i + 3] = (char)0x80; }
    n = san8(big, 960, c);
    CHECK(n == LCH_MAX_CHARS && lch_cps_to_u8(c, n, u8, sizeof u8) == 960, "W452: 240 4-Byte-Zeichen = 960 Byte (passt ins Paket)");
    {
        wchar_t w[LCH_LINE_W];
        CHECK(lch_cps_to_w(c, n, w, LCH_LINE_W) == 480 && w[0] == 0xD83D && w[1] == 0xDE00 && w[480] == 0,
              "UTF-16 mit Surrogatpaaren passt in die Zeile");
    }
    {
        char nm[LCH_NAME_MAX + 1];
        lch_name_clean("Pi\x01lot\x7F", nm, sizeof nm);
        CHECK(strcmp(nm, "Pi?lot?") == 0, "Pilotname: nicht druckbar -> '?'");
        lch_name_clean("", nm, sizeof nm);
        CHECK(strcmp(nm, "?") == 0, "leerer Pilotname -> '?'");
        lch_name_clean("ABCDEFGHIJKLMNOPQRSTUVWXYZ", nm, sizeof nm);
        CHECK(strlen(nm) == LCH_NAME_MAX, "Pilotname hoechstens 16 Zeichen");
    }
}

static void rate_tests(void)
{
    lch_rate r;
    int i, ok = 1;
    printf("Ratenbremse\n");
    memset(&r, 0, sizeof r);
    CHECK(lch_rate_take(&r, 7, 1000) && lch_rate_take(&r, 7, 1100) && lch_rate_take(&r, 7, 1200), "3 Nachrichten in 5 s erlaubt");
    CHECK(!lch_rate_take(&r, 7, 1300) && !lch_rate_take(&r, 7, 5999), "4. Nachricht innerhalb 5 s verworfen");
    CHECK(lch_rate_take(&r, 7, 6000) && !lch_rate_take(&r, 7, 6050), "nach 5 s wieder eine frei (gleitendes Fenster)");
    CHECK(lch_rate_take(&r, 8, 1300), "anderer Absender unabhaengig");
    memset(&r, 0, sizeof r);
    for (i = 0; i < 100; i++) ok &= lch_rate_take(&r, 1000u + (unsigned)i, 2000u + (unsigned)i); /* Tabelle voll */
    CHECK(ok, "neue Absender bei voller Tabelle angenommen (aeltester verdraengt)");
    memset(&r, 0, sizeof r);
    CHECK(lch_rate_take(&r, 1, 0xFFFFF000u) && lch_rate_take(&r, 1, 0xFFFFF100u) && lch_rate_take(&r, 1, 0x00000100u) &&
          !lch_rate_take(&r, 1, 0x00000200u) && lch_rate_take(&r, 1, 0x00001400u), "Zeitzaehler-Ueberlauf");
    { /* Dauerflut: in 60 s hoechstens 3 je 5 s = 36 */
        int got = 0;
        memset(&r, 0, sizeof r);
        for (i = 0; i < 6000; i++) got += lch_rate_take(&r, 3, (uint32_t)(i * 10));
        CHECK(got <= 36 && got >= 35, "Flut 100/s ueber 60 s -> hoechstens 36 durch");
    }
}

static void mute_tests(void)
{
    lch_mute m;
    int i;
    char nm[20];
    printf("Stummschalten\n");
    memset(&m, 0, sizeof m);
    CHECK(lch_mute_add(&m, "Spammer") == 1 && lch_mute_add(&m, "SPAMMER") == 0, "stumm, Gross/Klein egal");
    CHECK(lch_muted(&m, "spammer") && !lch_muted(&m, "Other"), "Abfrage");
    CHECK(lch_mute_del(&m, "sPaMmEr") == 1 && !lch_muted(&m, "Spammer") && lch_mute_del(&m, "x") == 0, "aufheben");
    CHECK(lch_mute_add(&m, "") == -1 && lch_mute_add(&m, "ABCDEFGHIJKLMNOPQ") == -1, "leer bzw. zu lang ungueltig");
    for (i = 0; i < LCH_MUTE_MAX; i++) {
        _snprintf_s(nm, sizeof nm, _TRUNCATE, "P%d", i);
        lch_mute_add(&m, nm);
    }
    CHECK(m.n == LCH_MUTE_MAX && lch_mute_add(&m, "Extra") == -1, "Liste begrenzt (16)");
}

static void pipeline_tests(void)
{
    lch_state s;
    lch_input in;
    uint8_t pkt[LCH_PKT_MAX], b[LCH_PKT_MAX];
    const lch_line *l;
    lch_pkt p;
    int len, i;
    printf("Ablauf (Senden/Empfangen)\n");
    lch_init(&s, 0x55u);
    len = lch_build(b, sizeof b, 0x77u, 1u, (const uint8_t *)"Hi \xE2\x80\xAE" "dlrow", 11);
    CHECK(lch_rx(&s, 1000, 0x7F000002u, "Alice", b, len) == 1 && s.rx_ok == 1, "Nachricht angenommen");
    l = lch_hist_get(&s.hist, 0);
    CHECK(l && l->kind == LCH_L_OTHER && wcscmp(l->name, L"Alice") == 0 && wcscmp(l->text, L"Hi dlrow") == 0,
          "Verlauf: Name aus der Sitzung, Text bereinigt (RLO entfernt)");
    CHECK(lch_rx(&s, 1001, 0x7F000002u, "Alice", b, len) == 0 && s.rx_dup == 1, "Doppel (gleiche Kennung/Nummer) verworfen");
    for (i = 2; i <= 5; i++) {
        len = lch_build(b, sizeof b, 0x77u, (uint32_t)i, (const uint8_t *)"x", 1);
        (void)lch_rx(&s, 1000u + (unsigned)i, 0x7F000002u, "Alice", b, len);
    }
    CHECK(s.rx_ok == 3 && s.rx_rate_drop == 2, "Empfang: 3 je 5 s je Absender, Rest verworfen");
    len = lch_build(b, sizeof b, 0x88u, 1u, (const uint8_t *)"y", 1);
    CHECK(lch_rx(&s, 1010, 0x7F000003u, "Bob", b, len) == 1, "anderer Absender unberuehrt");
    lch_mute_add(&s.mute, "bob");
    len = lch_build(b, sizeof b, 0x88u, 2u, (const uint8_t *)"z", 1);
    CHECK(lch_rx(&s, 1020, 0x7F000003u, "Bob", b, len) == 0 && s.rx_muted == 1, "stummer Absender verworfen");
    len = lch_build(b, sizeof b, 0x99u, 1u, (const uint8_t *)"\xE2\x80\x8B\xE2\x80\x8B", 6);
    CHECK(lch_rx(&s, 1030, 0x7F000004u, "Carl", b, len) == 0 && s.rx_empty == 1, "nur Unsichtbares -> nicht angezeigt");
    CHECK(lch_rx(&s, 1040, 0x7F000004u, "Carl", b, 5) == 0 && s.rx_bad == 1, "kaputtes Paket gezaehlt");
    /* Senden */
    lch_input_clear(&in);
    lch_input_wchar(&in, 'M'); lch_input_wchar(&in, 0xF6); lch_input_wchar(&in, 0x202E); lch_input_wchar(&in, 'p');
    CHECK(in.n == 3, "Eingabe: Bidi-Zeichen gar nicht erst aufgenommen");
    len = lch_tx(&s, 2000, &in, "Me", pkt, sizeof pkt);
    CHECK(len == LCH_HDR + 4 && lch_parse(pkt, len, &p) && p.seq == 1 && p.nonce == 0x55u &&
          memcmp(p.text, "M\xC3\xB6p", 4) == 0, "Senden: Paket mit bereinigtem UTF-8");
    l = lch_hist_get(&s.hist, 0);
    CHECK(l && l->kind == LCH_L_SELF && wcscmp(l->text, L"M\x00F6p") == 0, "eigene Zeile im Verlauf");
    CHECK(lch_tx(&s, 2001, &in, "Me", pkt, sizeof pkt) > 0 && lch_tx(&s, 2002, &in, "Me", pkt, sizeof pkt) > 0, "2. und 3.");
    CHECK(lch_tx(&s, 2003, &in, "Me", pkt, sizeof pkt) == -1 && s.tx_rate_drop == 1, "Senden: 4. in 5 s verweigert");
    CHECK(lch_tx(&s, 7100, &in, "Me", pkt, sizeof pkt) > 0, "Senden: nach 5 s wieder frei");
    lch_input_clear(&in);
    lch_input_wchar(&in, ' ');
    CHECK(lch_tx(&s, 9000, &in, "Me", pkt, sizeof pkt) == 0, "Senden: leere Zeile nicht gesendet");
}

static void input_tests(void)
{
    lch_input in;
    wchar_t w[LCH_LINE_W];
    int i;
    printf("Eingabezeile\n");
    lch_input_clear(&in);
    CHECK(lch_input_wchar(&in, 0xD83D) == 0 && lch_input_wchar(&in, 0xDE00) == 1 && in.n == 1 && in.cp[0] == 0x1F600,
          "Surrogatpaar -> ein Zeichen");
    CHECK(lch_input_wchar(&in, 0xDC00) == 0 && in.n == 1, "einzelnes niedriges Surrogat verworfen");
    CHECK(lch_input_wchar(&in, 0x0D) == 0 && lch_input_wchar(&in, 0x1B) == 0 && lch_input_wchar(&in, 0x7F) == 0,
          "Steuerzeichen nicht aufgenommen");
    CHECK(lch_input_wchar(&in, 0x200D) == 0 && lch_input_wchar(&in, 0xFEFF) == 0, "Nullbreiten-Zeichen nicht aufgenommen");
    lch_input_back(&in);
    CHECK(in.n == 0, "Backspace loescht ein ganzes Zeichen (auch 4-Byte)");
    for (i = 0; i < 300; i++) lch_input_wchar(&in, 'a');
    CHECK(in.n == LCH_MAX_CHARS, "W452: Eingabe hoechstens 240 Zeichen");
    CHECK(lch_input_w(&in, w, LCH_LINE_W) == 240, "Anzeige der Eingabe");
    lch_input_back(&in);
    lch_input_back(&in);
    CHECK(in.n == 238, "mehrfach Backspace");
}

static void set_line(lch_input *in, const char *a)
{
    lch_input_clear(in);
    while (*a) lch_input_wchar(in, (unsigned char)*a++);
}

static void cmd_tests(void)
{
    lch_input in;
    char arg[LCH_NAME_MAX + 1];
    printf("Befehle\n");
    set_line(&in, "/mute Spammer");
    CHECK(lch_cmd(&in, arg, sizeof arg) == LCH_CMD_MUTE && strcmp(arg, "Spammer") == 0, "/mute Name");
    set_line(&in, "/MUTE   Guest-ab12  ");
    CHECK(lch_cmd(&in, arg, sizeof arg) == LCH_CMD_MUTE && strcmp(arg, "Guest-ab12") == 0, "/MUTE mit Leerraum");
    set_line(&in, "/unmute Spammer");
    CHECK(lch_cmd(&in, arg, sizeof arg) == LCH_CMD_UNMUTE && strcmp(arg, "Spammer") == 0, "/unmute Name");
    set_line(&in, "/mute");
    CHECK(lch_cmd(&in, arg, sizeof arg) == LCH_CMD_MUTES, "/mute ohne Namen = Liste");
    set_line(&in, "/unmute");
    CHECK(lch_cmd(&in, arg, sizeof arg) == LCH_CMD_BAD, "/unmute ohne Namen = Hilfe");
    set_line(&in, "/help");
    CHECK(lch_cmd(&in, arg, sizeof arg) == LCH_CMD_HELP, "/help");
    set_line(&in, "/kick x");
    CHECK(lch_cmd(&in, arg, sizeof arg) == LCH_CMD_BAD, "unbekannter Befehl wird nicht gesendet");
    set_line(&in, "/mute ABCDEFGHIJKLMNOPQRS");
    CHECK(lch_cmd(&in, arg, sizeof arg) == LCH_CMD_BAD, "zu langer Name");
    set_line(&in, "hallo /mute x");
    CHECK(lch_cmd(&in, arg, sizeof arg) == LCH_CMD_NONE, "normale Nachricht");
}

/* ---------------- Mutations-Fuzzer (wie W430: fester Seed, gestapelte Mutationen, Invarianten) ---------------- */
static uint32_t s_rng = 0x437u;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static int fuzz_one(lch_state *st, uint8_t *buf, int len)
{
    lch_pkt p;
    uint32_t c[LCH_MAX_CHARS], c2[LCH_MAX_CHARS];
    uint8_t u8[LCH_MAX_TEXT_BYTES];
    int n, n2, ul, i, before = st->hist.n;
    uint32_t ok0 = st->rx_ok;
    /* Parser: Annahme nur bei exakt passender Laenge */
    if (lch_parse(buf, len, &p)) {
        if (p.len != len - LCH_HDR || p.len < 1 || p.len > LCH_MAX_TEXT_BYTES || !p.nonce || !p.seq) return 0;
        if (p.text != buf + LCH_HDR) return 0;
    } else if (len >= LCH_HDR + 1 && len <= LCH_PKT_MAX && memcmp(buf, "SBCH", 4) == 0 && buf[4] == 1 && buf[5] == 1 &&
               (int)(buf[6] | (buf[7] << 8)) == len - LCH_HDR && (buf[8] | buf[9] | buf[10] | buf[11]) &&
               (buf[12] | buf[13] | buf[14] | buf[15])) {
        return 0; /* gueltig, aber abgelehnt */
    }
    /* Bereinigung direkt auf beliebigen Bytes: Invarianten + Idempotenz */
    n = lch_sanitize_u8(buf, len, c, LCH_MAX_CHARS);
    if (!san_invariants(c, n)) return 0;
    ul = lch_cps_to_u8(c, n, u8, sizeof u8);
    if (ul < 0 || ul > LCH_MAX_TEXT_BYTES) return 0;
    n2 = lch_sanitize_u8(u8, ul, c2, LCH_MAX_CHARS);
    if (n2 != n || memcmp(c, c2, (size_t)n * sizeof c[0]) != 0) return 0;
    /* Empfang: nie mehr als eine Zeile, Zeile terminiert */
    (void)lch_rx(st, rnd(), rnd() % 4u, "Fuzz", buf, len);
    if (st->rx_ok - ok0 > 1) return 0;
    if (st->rx_ok != ok0) {
        const lch_line *l = lch_hist_get(&st->hist, 0);
        if (!l || wcsnlen(l->text, LCH_LINE_W) >= LCH_LINE_W || wcsnlen(l->name, LCH_NAME_MAX + 1) > LCH_NAME_MAX) return 0;
        for (i = 0; l->text[i]; i++)
            if (l->text[i] < 0x20) return 0;
    }
    (void)before;
    return 1;
}

static int fuzz_run(int count)
{
    static const char *const corpus[] = { "Hallo", "Gr\xC3\xBC\xC3\x9F" "e", "\xE2\x80\xAE" "evil", "a\xCC\x81\xCC\x81\xCC\x81",
                                          "\xF0\x9F\x98\x80 ok", "\xED\xA0\x80", "\xC0\xAF", "/mute x", "   ", "x" };
    uint8_t buf[LCH_PKT_MAX + 64];
    lch_state st;
    int i, fails = 0, valid = 0;
    lch_init(&st, 1);
    for (i = 0; i < count; i++) {
        const char *src = corpus[rnd() % (sizeof corpus / sizeof corpus[0])];
        int tl = (int)strlen(src), len, k, nm = 1 + (int)(rnd() % 4u);
        if (rnd() % 8u == 0) { /* langer Text */
            int j;
            tl = 1 + (int)(rnd() % LCH_MAX_TEXT_BYTES);
            for (j = 0; j < tl; j++) buf[LCH_HDR + j] = (uint8_t)rnd();
            len = lch_build(buf, sizeof buf, 1u + rnd() % 3u, 1u + rnd(), buf + LCH_HDR, tl);
            if (!len) len = LCH_HDR + tl;
        } else {
            len = lch_build(buf, sizeof buf, 1u + rnd() % 3u, 1u + rnd() % 64u, (const uint8_t *)src, tl);
        }
        for (k = 0; k < nm; k++) {
            uint32_t r = rnd() % 10u, pos = len > 0 ? rnd() % (uint32_t)len : 0;
            switch (r) {
            case 0: buf[pos] ^= (uint8_t)(1u << (rnd() % 8u)); break;               /* Bitflip */
            case 1: buf[pos] = (uint8_t)rnd(); break;                                /* Byte */
            case 2: len = (int)(rnd() % (uint32_t)(len + 1)); break;                 /* abschneiden */
            case 3: if (len < (int)sizeof buf - 8) { int j, a = 1 + (int)(rnd() % 8u); for (j = 0; j < a; j++) buf[len++] = (uint8_t)rnd(); } break;
            case 4: buf[6] = (uint8_t)rnd(); buf[7] = (uint8_t)(rnd() % 3u); break;  /* Laengenfeld */
            case 5: if (len > LCH_HDR) buf[LCH_HDR + rnd() % (uint32_t)(len - LCH_HDR)] = (uint8_t)(0x80 | rnd()); break; /* UTF-8-Bruch */
            case 6: if (len > LCH_HDR + 3) { buf[LCH_HDR] = 0xE2; buf[LCH_HDR + 1] = 0x80; buf[LCH_HDR + 2] = (uint8_t)(0x8B + rnd() % 0x25u); } break; /* Format-Zeichen */
            case 7: memset(buf + 8, 0, 4); break;                                   /* Kennung 0 */
            case 8: buf[4] = (uint8_t)(rnd() % 3u); buf[5] = (uint8_t)(rnd() % 3u); break;
            default: if (len > LCH_HDR) memset(buf + LCH_HDR, (int)(rnd() & 0x1F), (size_t)(len - LCH_HDR)); break; /* Steuerzeichen */
            }
        }
        if (len > (int)sizeof buf) len = (int)sizeof buf;
        {
            lch_pkt pp;
            valid += lch_parse(buf, len, &pp);
        }
        if (!fuzz_one(&st, buf, len)) {
            if (fails++ < 3) printf("Fuzz-Fund bei Mutation %d (Laenge %d)\n", i, len);
        }
    }
    printf("Fuzz: %d Mutationen (Seed fest), %d gueltige Pakete, %d Funde\n", count, valid, fails);
    return fails;
}

/* ---------------- Loeschen + Minidump ---------------- */
static lch_state *s_dump_st; /* Heap: statische Modulvariablen nimmt der Minidump ohnehin nicht mit (Messung) */

static int file_has_utf16(const char *path, const wchar_t *needle)
{
    FILE *f = NULL;
    long sz;
    unsigned char *b;
    size_t nl = wcslen(needle) * 2, i;
    int hit = 0;
    if (fopen_s(&f, path, "rb") || !f) return -1;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (unsigned char *)malloc((size_t)sz);
    if (!b) { fclose(f); return -1; }
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return -1; }
    fclose(f);
    for (i = 0; i + nl <= (size_t)sz && !hit; i++) hit = memcmp(b + i, needle, nl) == 0;
    free(b);
    return hit;
}

static DWORD WINAPI dump_thread(LPVOID arg)
{
    const char *path = (const char *)arg;
    HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    BOOL ok;
    if (f == INVALID_HANDLE_VALUE) return 0;
    /* gleiche Art wie sb_crash.c (dump_now, eigener Thread) */
    ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                           (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory), NULL, NULL, NULL);
    CloseHandle(f);
    return ok ? 1u : 0u;
}
/* Minidump aus einem zweiten Thread; dieser Thread wartet mit Zeigern auf den Verlauf auf seinem Stapel. */
static int write_dump(const char *path, const void *p)
{
    volatile const void *volatile refs[8];
    DWORD rc = 0;
    HANDLE h;
    int i;
    for (i = 0; i < 8; i++) refs[i] = (const char *)p + i * 16;
    h = CreateThread(NULL, 0, dump_thread, (LPVOID)path, 0, NULL);
    if (!h) return 0;
    WaitForSingleObject(h, 60000);
    GetExitCodeThread(h, &rc);
    CloseHandle(h);
    return rc == 1 && refs[0] == p;
}

static void wipe_tests(const char *dump_dir)
{
    static const wchar_t kSent[] = L"W437SENTINELqzx";
    volatile const lch_line *volatile ref;
    size_t i;
    int nz = 0;
    printf("Loeschen\n");
    s_dump_st = (lch_state *)malloc(sizeof *s_dump_st);
    if (!s_dump_st) return;
    lch_init(s_dump_st, 9);
    lch_hist_push(&s_dump_st->hist, LCH_L_OTHER, "Alice", kSent, 1);
    ref = lch_hist_get(&s_dump_st->hist, 0); /* Zeiger auf dem Stapel: Minidump nimmt den Bereich mit */
    if (dump_dir && dump_dir[0]) {
        char p1[MAX_PATH], p2[MAX_PATH];
        int h1, h2;
        _snprintf_s(p1, sizeof p1, _TRUNCATE, "%s\\w437_vorher.dmp", dump_dir);
        _snprintf_s(p2, sizeof p2, _TRUNCATE, "%s\\w437_nachher.dmp", dump_dir);
        CHECK(write_dump(p1, (const void *)ref->text), "Minidump vorher geschrieben");
        h1 = file_has_utf16(p1, kSent);
        lch_wipe(s_dump_st);
        CHECK(write_dump(p2, (const void *)ref->text), "Minidump nachher geschrieben");
        h2 = file_has_utf16(p2, kSent);
        printf("Minidump (wie sb_crash): Testtext vorher %s, nach dem Loeschen %s\n", h1 == 1 ? "enthalten" : "nicht gefunden",
               h2 == 1 ? "ENTHALTEN" : "nicht enthalten");
        CHECK(h1 == 1, "Gegenprobe: ohne Loeschen steht der Verlauf im Minidump");
        CHECK(h2 == 0, "nach dem Loeschen kein Chat-Text im Minidump");
        DeleteFileA(p1);
        DeleteFileA(p2);
    } else {
        lch_wipe(s_dump_st);
    }
    for (i = 0; i < sizeof s_dump_st->hist; i++) nz += ((const unsigned char *)&s_dump_st->hist)[i] != 0;
    CHECK(nz == 0 && ref->text[0] == 0, "Verlauf vollstaendig geloescht");
    free(s_dump_st);
    s_dump_st = NULL;
}

/* ------------------------------------------------------------------ W446 STEELNET-CHAT (JSON-Weg) */
static int js_dec(const char *json_str, uint8_t *out, int cap) /* json_str inkl. Anfuehrungszeichen */
{
    return lch_json_str_u8(json_str, json_str + strlen(json_str), out, cap);
}

static void sn_tests(void)
{
    uint8_t u[1100];
    char line[LCH_SN_LINE_MAX], body[LCH_SN_LINE_MAX];
    uint32_t cps[LCH_MAX_CHARS];
    lch_sn sn;
    lch_mute mu;
    lch_input in;
    const lch_line *l;
    int n, k;
    /* Decoder: rohes UTF-8, Escapes, Surrogatpaare, einzelne Surrogate verworfen, Fehler */
    n = js_dec("\"abc\"", u, sizeof u);
    CHECK(n == 3 && memcmp(u, "abc", 3) == 0, "W446 JSON: ASCII");
    n = js_dec("\"\\\"q\\\\ \\/ \\u00e4\"", u, sizeof u);
    CHECK(n == 8 && memcmp(u, "\"q\\ / \xC3\xA4", 8) == 0, "W446 JSON: Escapes und \\u00e4");
    n = js_dec("\"\\ud83d\\ude00\"", u, sizeof u);
    CHECK(n == 4 && memcmp(u, "\xF0\x9F\x98\x80", 4) == 0, "W446 JSON: Surrogatpaar -> 4 Byte");
    n = js_dec("\"a\\ud800b\\udc00c\"", u, sizeof u);
    CHECK(n == 3 && memcmp(u, "abc", 3) == 0, "W446 JSON: einzelne Surrogate verworfen");
    n = js_dec("\"\xC3\xA4\xF0\x9F\x98\x80\"", u, sizeof u);
    CHECK(n == 6, "W446 JSON: rohes UTF-8 unveraendert (Bereinigung prueft)");
    CHECK(js_dec("\"a\\x\"", u, sizeof u) < 0, "W446 JSON: unbekanntes Escape");
    CHECK(js_dec("\"a\\u12\"", u, sizeof u) < 0, "W446 JSON: kurzes \\u");
    CHECK(js_dec("\"a\x01" "b\"", u, sizeof u) < 0, "W446 JSON: Steuerbyte roh");
    CHECK(js_dec("abc", u, sizeof u) < 0 && js_dec("\"abc", u, sizeof u) < 0 && js_dec("\"", u, sizeof u) < 0,
          "W446 JSON: kein String");
    CHECK(js_dec("\"a\"b\"", u, sizeof u) < 0, "W446 JSON: Anfuehrungszeichen mitten im Wert");
    CHECK(js_dec("\"abcdef\"", u, 4) < 0, "W446 JSON: Puffer zu klein");
    CHECK(js_dec("\"\\", u, sizeof u) < 0 && js_dec("\"\\\"", u, sizeof u) < 0, "W446 JSON: Escape am Ende");
    /* Encoder: Rundreise, Escapes, 240 Emoji passen (W452) */
    n = san8("Gr\xC3\xBC\xC3\x9F" "e \"x\" \\ y", 15, cps);
    k = lch_json_text(cps, n, body, (int)sizeof body);
    CHECK(k > 0 && strstr(body, "\\\"x\\\"") && strstr(body, " \\\\ "), "W446 JSON-Text: Escapes");
    {
        char q[LCH_SN_LINE_MAX + 2];
        _snprintf_s(q, sizeof q, _TRUNCATE, "\"%s\"", body);
        n = js_dec(q, u, sizeof u);
        CHECK(n == 15 && memcmp(u, "Gr\xC3\xBC\xC3\x9F" "e \"x\" \\ y", 15) == 0, "W446 JSON: Rundreise");
    }
    for (k = 0; k < LCH_MAX_CHARS; k++) cps[k] = 0x1F600;
    n = lch_json_text(cps, LCH_MAX_CHARS, body, (int)sizeof body);
    CHECK(n == 960, "W452 JSON-Text: 240 Emoji = 960 Byte UTF-8");
    /* Senden: Zeile, Ratenbremse, leer */
    memset(&sn, 0, sizeof sn);
    set_line(&in, "  hallo   steelnet  ");
    n = lch_sn_tx(&sn, 1000, &in, line, (int)sizeof line);
    CHECK(n > 0 && strcmp(line, "{\"t\":\"chat\",\"text\":\"hallo steelnet\"}") == 0 && (int)strlen(line) == n,
          "W446 Senden: JSON-Zeile ohne Absender, bereinigt");
    CHECK(sn.hist.n == 0, "W446 Senden: eigene Zeile erst mit dem Echo");
    lch_sn_tx(&sn, 1100, &in, line, (int)sizeof line);
    lch_sn_tx(&sn, 1200, &in, line, (int)sizeof line);
    CHECK(lch_sn_tx(&sn, 1300, &in, line, (int)sizeof line) == -1 && sn.tx_rate_drop == 1, "W446 Senden: 4. in 5 s gebremst");
    CHECK(lch_sn_tx(&sn, 7000, &in, line, (int)sizeof line) > 0, "W446 Senden: nach 5 s wieder");
    set_line(&in, "   ");
    CHECK(lch_sn_tx(&sn, 20000, &in, line, (int)sizeof line) == 0 && sn.tx_empty == 1, "W446 Senden: leer");
    /* Empfang: Bereinigung, Name, stumm, eigene Zeile, Verlauf */
    memset(&sn, 0, sizeof sn);
    memset(&mu, 0, sizeof mu);
    CHECK(lch_sn_rx(&sn, &mu, 5, "Alpha", (const uint8_t *)"a\xE2\x80\xAE" "b\xE2\x80\x8B" "c", 9, 0, "Bravo") == 1,
          "W446 Empfang: angenommen");
    l = lch_hist_get(&sn.hist, 0);
    CHECK(l && wcscmp(l->text, L"abc") == 0 && wcscmp(l->name, L"Alpha") == 0 && l->kind == LCH_L_OTHER,
          "W446 Empfang: RLO/ZWSP entfernt, Name aus der Zeile");
    CHECK(lch_sn_rx(&sn, &mu, 6, "Bravo", (const uint8_t *)"x", 1, 0, "Bravo") == 1 &&
              lch_hist_get(&sn.hist, 0)->kind == LCH_L_SELF, "W446 Empfang: Echo = eigene Zeile");
    CHECK(lch_sn_rx(&sn, &mu, 7, "Ev\x01il\xC3\xA4", (const uint8_t *)"y", 1, 1, NULL) == 1 &&
              wcscmp(lch_hist_get(&sn.hist, 0)->name, L"Ev?il??") == 0 && sn.rx_hist == 1,
          "W446 Empfang: Name nur druckbares ASCII, Verlaufszeile gezaehlt");
    lch_mute_add(&mu, "alpha");
    CHECK(lch_sn_rx(&sn, &mu, 8, "Alpha", (const uint8_t *)"z", 1, 0, NULL) == 0 && sn.rx_muted == 1,
          "W446 Empfang: /mute gilt auch im steelnet-Tab");
    CHECK(lch_sn_rx(&sn, &mu, 9, "Charlie", (const uint8_t *)"\xE2\x80\x8B ", 4, 0, NULL) == 0 && sn.rx_empty == 1,
          "W446 Empfang: nach Bereinigung leer verworfen");
    CHECK(lch_sn_rx(&sn, &mu, 9, "", (const uint8_t *)"q", 1, 0, NULL) == 0 &&
              lch_sn_rx(&sn, &mu, 9, "X", (const uint8_t *)"q", 0, 0, NULL) == 0 && sn.rx_bad == 2,
          "W446 Empfang: ohne Name bzw. Text verworfen");
    lch_sn_wipe(&sn);
    CHECK(sn.hist.n == 0 && lch_hist_get(&sn.hist, 0) == NULL, "W446 Loeschen");
    /* Fuzz des JSON-Decoders + Empfang (Seed fest): nie ueber den Puffer, nie ungueltiges UTF-16 */
    {
        static const char alpha[] = "\"\\u0123456789abcdefABCDEFxyz/ \x01\x7F\xC3\xA4\xED\xA0\x80\xF0\x9F\x98\x80";
        int it, bad = 0;
        memset(&sn, 0, sizeof sn);
        for (it = 0; it < 200000; it++) {
            char s[96];
            int len = 2 + (int)(rnd() % 90u), j, r;
            s[0] = '"';
            for (j = 1; j < len - 1; j++) s[j] = alpha[rnd() % (sizeof alpha - 1)];
            s[len - 1] = '"';
            r = lch_json_str_u8(s, s + len, u, 64);
            if (r > 64) bad++;
            if (r > 0) lch_sn_rx(&sn, NULL, (uint32_t)it, "Fz", u, r, 0, NULL);
        }
        for (k = 0; k < sn.hist.n; k++) {
            const lch_line *x = lch_hist_get(&sn.hist, k);
            for (n = 0; x->text[n]; n++) {
                if (x->text[n] >= 0xD800 && x->text[n] <= 0xDBFF && !(x->text[n + 1] >= 0xDC00 && x->text[n + 1] <= 0xDFFF)) bad++;
            }
        }
        CHECK(bad == 0, "W446 Fuzz JSON-Decoder (200 000) ohne Fund");
    }
}

/* W452 CHATUX: 240 Zeichen (Codepunkte, nicht Byte), Japanisch (Kana/Kanji, IME-Ergebnisse), Kyrillisch durch
 * Bereinigung, Paket, Empfang, Eingabezeile und JSON (steelnet). Team-Tab entfaellt (BERICHT_M2 W452): kein
 * Team-Filter im Kern. */
static void w452_tests(void)
{
    static const char jp[] = "\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF"; /* こんにちは */
    static const char kanji[] = "\xE6\x88\xA6\xE9\x97\x98\xE9\x96\x8B\xE5\xA7\x8B";            /* 戦闘開始 */
    static const char ru[] = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";               /* Привет */
    static const char kata_hw[] = "\xEF\xBD\xB6\xEF\xBE\x80";                                   /* ｶﾀ (Halbbreite) */
    uint32_t c[LCH_MAX_CHARS], c2[LCH_MAX_CHARS];
    uint8_t u8[LCH_MAX_TEXT_BYTES + 8], pkt[LCH_PKT_MAX + 8];
    char big[LCH_MAX_TEXT_BYTES + 16], line[LCH_SN_LINE_MAX];
    wchar_t w[LCH_LINE_W];
    lch_state s;
    lch_input in;
    lch_sn sn;
    lch_mute mu;
    int n, i, len;
    printf("W452 (240 Zeichen, CJK, Kyrillisch)\n");
    n = san8(jp, (int)strlen(jp), c);
    CHECK(n == 5 && c[0] == 0x3053 && c[4] == 0x306F, "W452: Hiragana bleibt (5 Codepunkte)");
    n = san8(kanji, (int)strlen(kanji), c);
    CHECK(n == 4 && c[0] == 0x6226 && c[3] == 0x59CB, "W452: Kanji bleibt");
    n = san8(ru, (int)strlen(ru), c);
    CHECK(n == 6 && c[0] == 0x041F && c[5] == 0x0442, "W452: Kyrillisch bleibt");
    n = san8(kata_hw, (int)strlen(kata_hw), c);
    CHECK(n == 2 && c[0] == 0xFF76, "W452: Halbbreiten-Katakana bleibt");
    n = san8("a\xE3\x80\x80\xE3\x80\x80" "b", 8, c);
    CHECK(cps_eq_ascii(c, n, "a b"), "W452: ideografisches Leerzeichen (IME) -> ein Leerzeichen");
    /* Grenze zaehlt Codepunkte: 241 Kanji (723 Byte) -> 240, 240 Kanji = 720 Byte passen ins Paket */
    for (i = 0; i < 241; i++) { big[i * 3] = (char)0xE6; big[i * 3 + 1] = (char)0xBC; big[i * 3 + 2] = (char)0xA2; } /* 漢 */
    n = lch_sanitize_u8((const uint8_t *)big, 723, c, LCH_MAX_CHARS);
    CHECK(n == 240 && c[239] == 0x6F22, "W452: 241 Kanji -> 240 (Codepunkte, nicht Byte)");
    len = lch_cps_to_u8(c, n, u8, (int)sizeof u8);
    CHECK(len == 720, "W452: 240 Kanji = 720 Byte UTF-8");
    lch_init(&s, 0x77u);
    len = lch_build(pkt, (int)sizeof pkt, 9, 1, u8, len);
    CHECK(len == LCH_HDR + 720, "W452: Paket mit 240 Kanji gebaut");
    CHECK(lch_rx(&s, 1000, 0x7F000002u, "Aki", pkt, len) == 1, "W452: 240 Kanji empfangen");
    {
        const lch_line *l = lch_hist_get(&s.hist, 0);
        CHECK(l && wcslen(l->text) == 240 && l->text[0] == 0x6F22, "W452: Verlaufszeile 240 Kanji vollstaendig");
    }
    /* Kyrillisch + Japanisch gemischt durch Senden (Eingabezeile -> Paket -> Empfang) */
    lch_input_clear(&in);
    {
        static const unsigned seq[] = { 0x041F, 0x0440, 0x0438, 0x0432, 0x0435, 0x0442, 0x3000, 0x6226, 0x95D8, 0xFF01 };
        for (i = 0; i < (int)(sizeof seq / sizeof seq[0]); i++) lch_input_wchar(&in, seq[i]);
    }
    CHECK(in.n == 10, "W452: IME-Ergebnis (Kyrillisch, ideogr. Leerzeichen, Kanji) in der Eingabezeile");
    len = lch_tx(&s, 2000, &in, "Me", pkt, (int)sizeof pkt);
    CHECK(len > LCH_HDR, "W452: gemischte Zeile gesendet");
    {
        lch_pkt p;
        CHECK(lch_parse(pkt, len, &p) && p.len == 6 * 2 + 1 + 3 * 3, "W452: Netzformat UTF-8 (Leerzeichen bereinigt)");
        n = lch_sanitize_u8(p.text, p.len, c2, LCH_MAX_CHARS);
        CHECK(n == 10 && c2[6] == 0x20 && c2[7] == 0x6226 && c2[9] == 0xFF01, "W452: Rundreise Kyrillisch/Kanji");
    }
    /* Eingabezeile: 240 Kana, das 241. wird abgewiesen; Surrogatpaar (Emoji) zaehlt als ein Zeichen */
    lch_input_clear(&in);
    for (i = 0; i < 240; i++) lch_input_wchar(&in, 0x3042);
    CHECK(in.n == 240 && lch_input_wchar(&in, 0x3044) == 0, "W452: Eingabe 240 Kana, 241. abgewiesen");
    lch_input_clear(&in);
    lch_input_wchar(&in, 0xD83D);
    lch_input_wchar(&in, 0xDE00);
    CHECK(in.n == 1 && in.cp[0] == 0x1F600, "W452: Surrogatpaar = ein Zeichen");
    CHECK(lch_input_w(&in, w, LCH_LINE_W) == 2, "W452: Anzeige als Surrogatpaar");
    /* steelnet: 240 Kanji als JSON-Zeile (UTF-8 roh) passen in LCH_SN_LINE_MAX und kommen gleich an */
    memset(&sn, 0, sizeof sn);
    memset(&mu, 0, sizeof mu);
    lch_input_clear(&in);
    for (i = 0; i < 240; i++) lch_input_wchar(&in, 0x6F22);
    n = lch_sn_tx(&sn, 1000, &in, line, (int)sizeof line);
    CHECK(n == 20 + 720 + 2 && n < LCH_SN_LINE_MAX, "W452 steelnet: 240 Kanji = 742 Byte JSON-Zeile");
    lch_input_clear(&in);
    for (i = 0; i < 240; i++) lch_input_wchar(&in, '"');
    n = lch_sn_tx(&sn, 7000, &in, line, (int)sizeof line);
    CHECK(n == 20 + 480 + 2 && n < LCH_SN_LINE_MAX, "W452 steelnet: 240 Anfuehrungszeichen escaped passen");
    CHECK(lch_sn_rx(&sn, &mu, 8000, "Aki", u8, 720, 0, "Me") == 1, "W452 steelnet: 240 Kanji empfangen");
    {
        const lch_line *l = lch_hist_get(&sn.hist, 0);
        CHECK(l && wcslen(l->text) == 240, "W452 steelnet: Zeile vollstaendig");
    }
    lch_wipe(&s);
    lch_sn_wipe(&sn);
    SecureZeroMemory(big, sizeof big);
    SecureZeroMemory(u8, sizeof u8);
}


/* W474 CHATTABS: Lobby-Zustand, Reiter je Zustand, Reihenfolge, Tab-Taste, Match-Schluessel */
static unsigned tabs_for(int ses, int link, int sn, int msn, int mlb, int mtm)
{
    lch_tabin ti;
    memset(&ti, 0, sizeof ti);
    ti.ses = ses;
    ti.lobby_link = link;
    ti.sn_ready = sn;
    ti.team_known = 0;
    ti.mis_sn = msn;
    ti.mis_lobby = mlb;
    ti.mis_team = mtm;
    return lch_tabs_avail(&ti);
}

static void w474_tests(void)
{
    const unsigned SN = 1u << LCH_TAB_SN, LB = 1u << LCH_TAB_LOBBY, TM = 1u << LCH_TAB_TEAM;
    lch_tabin ti;
    lch_hist h;
    wchar_t t[8] = { 'h', 'i', 0 };
    /* Seiten (W460-Werte) */
    CHECK(lch_page(8, LCH_TID_MAIN, 7) == LCH_PAGE_MAIN && lch_page(8, LCH_TID_MATCH, 2) == LCH_PAGE_MATCH &&
              lch_page(8, LCH_TID_SEARCH, 1) == LCH_PAGE_SEARCH,
          "Seiten MAIN MENU / MatchMaking / Suche erkannt");
    CHECK(lch_page(8, 0x3A3, 13) == LCH_PAGE_OTHER && lch_page(8, 0x43B, 6) == LCH_PAGE_OTHER &&
              lch_page(8, LCH_TID_MAIN, 6) == LCH_PAGE_OTHER && lch_page(5, LCH_TID_MAIN, 7) == LCH_PAGE_OTHER,
          "Lobby-Seiten (SESSION) und Zufallstreffer sind keine Menueseite");
    /* Zustand */
    CHECK(lch_session_from(0, 8, LCH_PAGE_OTHER, 0, 0) == LCH_SES_NONE, "ohne Spielstrom keine Lobby (Raum des Transports zaehlt nicht)");
    CHECK(lch_session_from(0, 20, LCH_PAGE_OTHER, 1, 1) == LCH_SES_NONE, "ohne Spielstrom auch kein Match");
    CHECK(lch_session_from(1, 8, LCH_PAGE_OTHER, 0, 0) == LCH_SES_LOBBY, "Strom + Szene 8 Lobby-Seite = Lobby");
    CHECK(lch_session_from(2, 8, LCH_PAGE_MAIN, 0, 0) == LCH_SES_NONE, "Hauptmenue mit (altem) Strom = keine Lobby");
    CHECK(lch_session_from(2, 8, LCH_PAGE_MATCH, 0, 0) == LCH_SES_NONE, "MatchMaking nach dem Verlassen = keine Lobby");
    CHECK(lch_session_from(2, 8, LCH_PAGE_SEARCH, 0, 0) == LCH_SES_NONE, "Suche/Ergebnis = keine Lobby");
    CHECK(lch_session_from(1, 8, LCH_PAGE_OTHER, 1, 0) == LCH_SES_MATCH && lch_session_from(1, 8, LCH_PAGE_OTHER, 0, 1) == LCH_SES_MATCH,
          "Lockstep bzw. Mission der Eingabeschicht = Match");
    CHECK(lch_session_from(1, 12, LCH_PAGE_OTHER, 0, 0) == LCH_SES_MATCH, "andere Szene mit Strom (Laden, Ergebnis) = Match");
    CHECK(lch_session_from(1, 1, LCH_PAGE_OTHER, 0, 0) == LCH_SES_NONE && lch_session_from(1, 5, LCH_PAGE_OTHER, 0, 0) == LCH_SES_NONE &&
              lch_session_from(1, 3, LCH_PAGE_OTHER, 0, 0) == LCH_SES_NONE && lch_session_from(1, 6, LCH_PAGE_OTHER, 0, 0) == LCH_SES_NONE,
          "Titel/Hauptmenue/Benutzerdaten = keine Lobby");
    CHECK(lch_out_of_lobby(8, LCH_PAGE_MAIN) && lch_out_of_lobby(8, LCH_PAGE_MATCH) && lch_out_of_lobby(8, LCH_PAGE_SEARCH) &&
              lch_out_of_lobby(1, LCH_PAGE_OTHER) && lch_out_of_lobby(5, LCH_PAGE_OTHER),
          "selbst verlassen (Hauptmenue/MatchMaking/Suche/Titel): Verlauf wird geleert");
    CHECK(!lch_out_of_lobby(8, LCH_PAGE_OTHER) && !lch_out_of_lobby(12, LCH_PAGE_OTHER),
          "Ersteller allein in seiner Lobby (letzter Gast weg): Reiter weg, Verlauf bleibt");
    /* Verfuegbarkeit je Zustand (Standard: mission_sn 0, lobby 1, team 1) */
    CHECK(tabs_for(LCH_SES_NONE, 1, 1, 0, 1, 1) == SN, "Hauptmenue mit steelnet: nur steelnet (Lobby-Reiter fehlt trotz Transport)");
    CHECK(tabs_for(LCH_SES_NONE, 1, 0, 0, 1, 1) == 0u, "Hauptmenue ohne steelnet (Offline/LAN/aus): kein Reiter -> Enter ans Spiel");
    CHECK(tabs_for(LCH_SES_LOBBY, 1, 1, 0, 1, 1) == (SN | LB), "Lobby: steelnet | Lobby (Schalter gelten in der Lobby nicht)");
    CHECK(tabs_for(LCH_SES_LOBBY, 1, 1, 0, 0, 0) == (SN | LB), "Lobby: Match-Schluessel 0 wirken nicht");
    CHECK(tabs_for(LCH_SES_LOBBY, 0, 1, 0, 1, 1) == SN, "Lobby ohne verbundenen Mitspieler: nur steelnet");
    CHECK(tabs_for(LCH_SES_LOBBY, 1, 0, 0, 1, 1) == LB, "LAN-Lobby (kein steelnet): nur Lobby");
    CHECK(tabs_for(LCH_SES_MATCH, 1, 1, 0, 1, 1) == LB, "Match Standard: steelnet aus (loc_chat_mission_sn=0), Lobby an");
    CHECK(tabs_for(LCH_SES_MATCH, 1, 1, 1, 1, 1) == (SN | LB), "Match mit loc_chat_mission_sn=1: steelnet | Lobby");
    CHECK(tabs_for(LCH_SES_MATCH, 1, 1, 1, 0, 1) == SN, "Match mit loc_chat_mission_lobby=0: nur steelnet");
    CHECK(tabs_for(LCH_SES_MATCH, 1, 1, 0, 0, 1) == 0u, "Match alles aus: kein Reiter -> Enter ans Spiel");
    CHECK(tabs_for(LCH_SES_LOBBY, 1, 1, 0, 1, 1) == (SN | LB), "zurueck in der Lobby: Reiter wieder da");
    memset(&ti, 0, sizeof ti);
    ti.ses = LCH_SES_MATCH; ti.lobby_link = 1; ti.sn_ready = 1; ti.team_known = 1; ti.mis_sn = 1; ti.mis_lobby = 1; ti.mis_team = 0;
    CHECK(lch_tabs_avail(&ti) == (SN | LB), "Match mit loc_chat_mission_team=0: kein Team (falls Team bekannt)");
    ti.mis_team = 1;
    CHECK(lch_tabs_avail(&ti) == (SN | LB | TM), "Team nur mit sicherer Zuordnung (team_known)");
    ti.team_known = 0;
    CHECK(!(lch_tabs_avail(&ti) & TM), "ohne sichere Zuordnung nie ein Team-Reiter");
    ti.ses = LCH_SES_NONE; ti.team_known = 1;
    CHECK(!(lch_tabs_avail(&ti) & (LB | TM)), "ausserhalb der Lobby weder Lobby noch Team");
    CHECK(lch_tabs_avail(NULL) == 0u, "NULL: nichts verfuegbar");
    /* Reihenfolge und Tab-Taste */
    CHECK(LCH_TAB_SN == 0 && LCH_TAB_LOBBY == 1 && LCH_TAB_TEAM == 2 && LCH_TAB_N == 3, "Reihenfolge steelnet | Lobby | Team");
    CHECK(lch_tab_next(LCH_TAB_SN, SN | LB) == LCH_TAB_LOBBY && lch_tab_next(LCH_TAB_LOBBY, SN | LB) == LCH_TAB_SN,
          "Tab-Taste: steelnet -> Lobby -> steelnet");
    CHECK(lch_tab_next(LCH_TAB_SN, SN | LB | TM) == LCH_TAB_LOBBY && lch_tab_next(LCH_TAB_LOBBY, SN | LB | TM) == LCH_TAB_TEAM &&
              lch_tab_next(LCH_TAB_TEAM, SN | LB | TM) == LCH_TAB_SN,
          "Tab-Taste mit Team: steelnet -> Lobby -> Team -> steelnet");
    CHECK(lch_tab_next(LCH_TAB_SN, SN | TM) == LCH_TAB_TEAM && lch_tab_next(LCH_TAB_LOBBY, LB) == LCH_TAB_LOBBY,
          "Tab-Taste ueberspringt fehlende Reiter, allein bleibt er");
    /* automatischer Wechsel */
    CHECK(lch_tab_auto(LCH_TAB_SN, SN | LB, SN) == LCH_TAB_LOBBY, "Lobby-Beitritt: Sprung auf Lobby");
    CHECK(lch_tab_auto(LCH_TAB_SN, SN | LB, SN | LB) == LCH_TAB_SN, "in der Lobby: Wahl des Nutzers bleibt");
    CHECK(lch_tab_auto(LCH_TAB_LOBBY, SN, SN | LB) == LCH_TAB_SN, "Lobby verlassen: Wechsel auf steelnet");
    CHECK(lch_tab_auto(LCH_TAB_SN, LB, SN | LB) == LCH_TAB_LOBBY, "Match ohne steelnet: aktiver Reiter wird Lobby");
    CHECK(lch_tab_auto(LCH_TAB_LOBBY, 0u, LB) == LCH_TAB_LOBBY, "nichts verfuegbar: Reiter bleibt (Chat zu)");
    /* Verlauf leeren */
    memset(&h, 0, sizeof h);
    lch_hist_push(&h, LCH_L_OTHER, "Bob", t, 5u);
    lch_hist_push(&h, LCH_L_OTHER, "Bob", t, 6u);
    {
        uint32_t g = h.gen;
        lch_hist_clear(&h);
        CHECK(h.n == 0 && lch_hist_get(&h, 0) == NULL && h.gen == g + 1u && h.l[0].name[0] == 0 && h.l[0].text[0] == 0,
              "Lobby verlassen: Verlauf leer und geloescht, naechster Beitritt beginnt leer");
    }
}

int main(int argc, char **argv)
{
    int i, fuzz_n = 200000;
    const char *dump_dir = NULL;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fuzz") == 0 && i + 1 < argc) fuzz_n = atoi(argv[++i]);
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) s_rng = (uint32_t)strtoul(argv[++i], NULL, 0) | 1u;
        else if (strcmp(argv[i], "--dump-dir") == 0 && i + 1 < argc) dump_dir = argv[++i];
    }
    codec_tests();
    sanitize_tests();
    rate_tests();
    mute_tests();
    pipeline_tests();
    input_tests();
    cmd_tests();
    sn_tests(); /* W446 */
    w452_tests(); /* W452 */
    w474_tests(); /* W474 */
    CHECK(fuzz_run(fuzz_n) == 0, "Fuzz ohne Fund");
    wipe_tests(dump_dir);
    printf("loc_chat_selftest: %d/%d OK\n", s_ok, s_n);
    return s_ok == s_n ? 0 : 1;
}
