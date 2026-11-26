/* loc_live_selftest.c - W479 LIVE-LOGON: Kern des Xbox-Live-Modus (loc_live.c), nur synthetische Daten.
 * Prueft Logon-/Task-Rueckgaben, Dienststatus je Dienst, Gamertag-Regel (Grenzfaelle), Pseudo-XUID
 * (fest, Bereich, keine Kollision mit echten Formen), XONLINE_USER-Aufbau und die Merkdatei (Festhalten,
 * Kollision, Umbenennung). Ausgabe: "loc_live_selftest: N/N OK". */
#include "loc_live.h"
#include "loc_camp.h" /* W491 CAMP-CLIENT */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int g_n, g_ok;

static void check(int cond, const char *what)
{
    g_n++;
    if (cond) {
        g_ok++;
    } else {
        printf("FAIL: %s\n", what);
    }
}

static void t_services(void)
{
    check(loclive_service_hr(LOCLIVE_SVC_MATCH) == 0, "Dienst 6 (Matchmaking) -> S_OK");
    check(loclive_service_hr(LOCLIVE_SVC_FEEDBACK) == 0, "Dienst 8 (Feedback) -> S_OK");
    check(loclive_service_hr(LOCLIVE_SVC_ACCOUNT) == 0, "Dienst 9 (Konto) -> S_OK");
    check(loclive_service_hr(LOCLIVE_SVC_CAMPAIGN) == 0x80151102u, "Dienst 0x43430009 (Kampagne) -> 0x80151102");
    check(loclive_service_hr(7) == 0x80151102u, "unbekannter Dienst -> nicht verfuegbar");
    { /* nie "nicht berechtigt" (0x80151101, TEKKI-Automat) oder Update-Codes (0x8015100x ausser Netz/Dienst) */
        uint32_t s[] = {0, 1, 5, 6, 7, 8, 9, 10, 0x43430009u, 0xFFFFFFFFu};
        int i, bad = 0;
        for (i = 0; i < (int)(sizeof s / sizeof s[0]); i++) {
            uint32_t hr = loclive_service_hr(s[i]);
            if (hr != 0 && hr != 0x80151102u) bad++;
        }
        check(!bad, "Dienste melden nur S_OK oder 0x80151102 (nie Update/nicht berechtigt)");
    }
}

static void t_logon(void)
{
    loclive_task t;
    uint32_t r;
    memset(&t, 0, sizeof t);
    t.magic = LOCLIVE_TASK_MAGIC;
    t.kind = LLT_LOGON;
    t.start_ms = 1000;
    r = loclive_task_step(&t, 1, 1500, LOCLIVE_LOGON_TIMEOUT_MS);
    check(r == LOCLIVE_TASK_RUNNING, "Logon laeuft, solange die steelnet-Anmeldung laeuft (0)");
    r = loclive_task_step(&t, 2, 2000, LOCLIVE_LOGON_TIMEOUT_MS);
    check(r == 0x001510F0u && t.result == 0x001510F0u, "Logon angenommen -> S_LOGON_CONNECTION_ESTABLISHED 0x001510F0");
    r = loclive_task_step(&t, 1, 90000, LOCLIVE_LOGON_TIMEOUT_MS);
    check(r == 0x001510F0u, "verbundener Logon bleibt verbunden (Pumpe Zustand 2, kein Abbruch bei kurzem Neuverbinden)");

    memset(&t, 0, sizeof t);
    t.magic = LOCLIVE_TASK_MAGIC;
    t.kind = LLT_LOGON;
    t.start_ms = 1000;
    r = loclive_task_step(&t, -1, 1100, LOCLIVE_LOGON_TIMEOUT_MS);
    check(r == LOCLIVE_E_NO_SERVICE && (int32_t)r < 0, "Konto abgelehnt -> Logon-Fehler (negativ)");
    check(loclive_task_step(&t, 2, 1200, LOCLIVE_LOGON_TIMEOUT_MS) == LOCLIVE_E_NO_SERVICE, "Fehler bleibt stehen");

    memset(&t, 0, sizeof t);
    t.magic = LOCLIVE_TASK_MAGIC;
    t.kind = LLT_LOGON;
    t.start_ms = 1000;
    check(loclive_task_step(&t, 0, 1001, LOCLIVE_LOGON_TIMEOUT_MS) == LOCLIVE_E_NO_SERVICE, "ohne Konto (Gast) -> Logon-Fehler sofort");

    memset(&t, 0, sizeof t);
    t.magic = LOCLIVE_TASK_MAGIC;
    t.kind = LLT_LOGON;
    t.start_ms = 0xFFFFFF00u; /* GetTickCount-Ueberlauf */
    check(loclive_task_step(&t, 1, 0x00000100u, LOCLIVE_LOGON_TIMEOUT_MS) == 0, "Zeitvergleich ueber den 32-Bit-Ueberlauf");
    check(loclive_task_step(&t, 1, 0xFFFFFF00u + LOCLIVE_LOGON_TIMEOUT_MS, LOCLIVE_LOGON_TIMEOUT_MS) == LOCLIVE_E_NO_SERVICE,
          "Zeit abgelaufen (30 s) -> Logon-Fehler");
    check(LOCLIVE_E_NO_SERVICE != 0x80151002u && LOCLIVE_E_NO_SERVICE != 0x80151101u, "Logon-Fehler ist nie \"Update noetig\"");
}

static void t_tasks(void)
{
    loclive_task t;
    memset(&t, 0, sizeof t);
    t.magic = LOCLIVE_TASK_MAGIC;
    t.kind = LLT_FRIENDS_ENUM;
    check(loclive_task_step(&t, 0, 0, 0) == 0x001500F1u, "Friends-Liste: erst RESULTS_AVAIL (leer)");
    check(loclive_task_step(&t, 0, 0, 0) == 0, "Friends-Liste: dann laeuft");
    t.finished = 1;
    check(loclive_task_step(&t, 0, 0, 0) == 0x001500F0u, "Friends-Liste nach Finish: SUCCESS");
    t.kind = LLT_FRIENDS;
    check(loclive_task_step(&t, 0, 0, 0) == 0, "Friends-Start laeuft dauerhaft");
    t.kind = LLT_MUTE;
    check(loclive_task_step(&t, 0, 0, 0) == 0, "Mutelist-Start laeuft dauerhaft");
    t.kind = LLT_DONE;
    check(loclive_task_step(&t, 0, 0, 0) == 0x001500F0u, "Namenspruefung/Feedback/Suche: SUCCESS (Name gueltig 0x001500F0)");
    t.magic = 0;
    check(loclive_task_step(&t, 0, 0, 0) == LOCLIVE_E_INVALIDARG, "geschlossene Task -> E_INVALIDARG");
    check(loclive_task_step(NULL, 0, 0, 0) == LOCLIVE_E_INVALIDARG, "NULL-Task -> E_INVALIDARG");
}

/* W487 LIVE-GAMERTAG: Gamertag kommt aus dem Konto (welcome); hier nur die Regel (wie steelcore) */
static void t_gamertag(void)
{
    static const char *const ok[] = {"A", "Ace", "ABCDEFGHIJKLMNO", "Live Test 01", "a b c", "Zz9"};
    static const char *const bad[] = {"", "ABCDEFGHIJKLMNOP", "1Ace", " Ace", "Ace ", "Ac  e", "Ac_e", "Ac-e",
                                      "Pi\xC3\xA4lot", "Ace!", "_a12", "Ac\te"};
    int i, nok = 0, nbad = 0;
    for (i = 0; i < (int)(sizeof ok / sizeof ok[0]); i++) nok += loclive_tag_valid(ok[i]);
    for (i = 0; i < (int)(sizeof bad / sizeof bad[0]); i++) nbad += !loclive_tag_valid(bad[i]);
    check(nok == (int)(sizeof ok / sizeof ok[0]), "Gamertag-Regel: gueltige Tags (1-15, Buchstabe vorn, einzelne Leerzeichen)");
    check(nbad == (int)(sizeof bad / sizeof bad[0]), "Gamertag-Regel: ungueltige Tags (Laenge, Ziffer vorn, Rand-/Doppel-Leerzeichen, _ - Umlaut)");
    check(!loclive_tag_valid(NULL), "Gamertag NULL ungueltig");
    {
        char g[16];
        loclive_park_tag(4294967295u, g);
        check(strcmp(g, "_a4294967295") == 0 && !loclive_tag_valid(g), "Parkname _a<Konto> ist nie ein Gamertag");
    }
}

static void t_xuid(void)
{
    uint64_t a = loclive_xuid(1), b = loclive_xuid(2), m = loclive_xuid(0xFFFFFFFFu);
    check(loclive_xuid(0) == 0, "Konto 0 -> keine XUID");
    check(a == 0x53424C0100000001ull, "Pseudo-XUID Konto 1 = 0x53424C0100000001");
    check(m == 0x53424C01FFFFFFFFull, "Pseudo-XUID hoechste Konto-ID");
    check(loclive_xuid(1) == a && loclive_xuid(123456) == loclive_xuid(123456), "Pseudo-XUID fest/reproduzierbar");
    check(a != b, "verschiedene Konten -> verschiedene XUIDs");
    check(loclive_xuid_account(loclive_xuid(987654)) == 987654, "Ableitung umkehrbar (Konto-ID zurueck)");
    check(loclive_xuid_is_pseudo(a) && loclive_xuid_is_pseudo(m), "Bereich erkannt");
    check(!loclive_xuid_is_pseudo(0x0009000001234567ull), "echte Live-XUID (0x0009...) ist keine Pseudo-XUID");
    check(!loclive_xuid_is_pseudo(0xE000000012345678ull), "Offline-XUID (0xE000...) ist keine Pseudo-XUID");
    check(!loclive_xuid_is_pseudo(0x53424C0100000000ull), "Bereich ohne Konto-ID (0) ungueltig");
    check(!loclive_xuid_is_pseudo(0x53424C0200000001ull), "andere Version (spaeter M3) nicht verwechselt");
    check((int64_t)a > 0, "Pseudo-XUID positiv (oberstes Bit 0)");
    {
        int i, bad = 0;
        for (i = 1; i < 5000; i++)
            if (loclive_xuid((uint32_t)i) == loclive_xuid((uint32_t)i + 1) || (loclive_xuid((uint32_t)i) >> 48) == 0x0009 ||
                (loclive_xuid((uint32_t)i) >> 60) == 0xE)
                bad++;
        check(!bad, "5000 Konten: keine Kollision, nie echte Form");
    }
}

static void t_user(void)
{
    uint8_t u[LOCLIVE_USER_SIZE];
    uint64_t x;
    uint32_t hr;
    memset(u, 0xCC, sizeof u);
    loclive_fill_user(u, loclive_xuid(77), "ABCDEFGHIJKLMNOXYZ", 0);
    memcpy(&x, u, 8);
    memcpy(&hr, u + 0x6C, 4);
    check(LOCLIVE_USER_SIZE == 0x70, "XONLINE_USER 0x70 Byte (Spielcode Schritt 0x70)");
    check(x == loclive_xuid(77), "XUID bei +0x00");
    check(memcmp(u + 0x0C, "ABCDEFGHIJKLMNO", 15) == 0 && u[0x0C + 15] == 0, "Gamertag bei +0x0C, hoechstens 15 + NUL");
    check(hr == 0, "hr bei +0x6C = S_OK");
    check(u[8] == 0 && u[9] == 0 && u[0x1C] == 0 && u[0x6B] == 0, "Rest genullt (keine Gast-Bits, kein Passcode)");
}

/* W487: Merkdatei (Konto -> zuletzt genutzter Pilotordner) und Gamertag-Wechsel mit Ordnerumzug */
static int dir_has(const char *base, const char *name, const char *file)
{
    char p[MAX_PATH];
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\%s%s%s", base, name, file ? "\\" : "", file ? file : "");
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}
static void mk_pilot(const char *base, const char *folder)
{
    char p[MAX_PATH];
    FILE *f = NULL;
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\%s", base, folder);
    CreateDirectoryA(p, NULL);
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\%s\\CAMPUSER", base, folder);
    if (fopen_s(&f, p, "wb") == 0 && f) {
        fputs("pilot", f);
        fclose(f);
    }
}
static void rm_tree(const char *base)
{
    char pat[MAX_PATH], p[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    _snprintf_s(pat, sizeof pat, _TRUNCATE, "%s\\*", base);
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\%s", base, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rm_tree(p);
            else DeleteFileA(p);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(base);
}

static void t_pin(void)
{
    char out[16], txt[512], txt2[512], dir[MAX_PATH], tdir[MAX_PATH], path[MAX_PATH];
    loclive_tag_plan_t pl;
    int r;
    FILE *f = NULL;
    const char *t0 = "a5 Old Tag\r\nmuell\r\na0 Null\r\na77\r\na78 ABCDEFGHIJKLMNOPQRS\r\na9 Bad!Tag\r\na12 _a12\r\n";

    check(!loclive_pin_get("", 5, out) && !loclive_pin_get(NULL, 5, out), "leere Merkdatei -> kein Eintrag");
    check(loclive_pin_get(t0, 5, out) && strcmp(out, "Old Tag") == 0, "Eintrag gefunden");
    check(!loclive_pin_get(t0, 77, out) && !loclive_pin_get(t0, 78, out) && !loclive_pin_get(t0, 9, out),
          "kaputte Zeilen uebergangen");
    check(loclive_pin_get(t0, 12, out) && strcmp(out, "_a12") == 0, "geparkter Ordner lesbar");
    check(loclive_pin_owner(t0, "old tag", 7) == 5 && loclive_pin_owner(t0, "old tag", 5) == 0,
          "Besitzer eines Tags ohne Gross/Klein, eigenes Konto ausgenommen");
    r = loclive_pin_put(t0, 5, "New Tag", txt, sizeof txt);
    check(r == 0 && strcmp(txt, "a12 _a12\r\na5 New Tag\r\n") == 0, "Zeile ersetzt, kaputte Zeilen entfernt");
    r = loclive_pin_put(txt, 8, "Other", txt2, sizeof txt2);
    check(r == 0 && loclive_pin_get(txt2, 8, out) && strcmp(out, "Other") == 0 && loclive_pin_get(txt2, 5, out),
          "neue Zeile angehaengt");
    check(loclive_pin_put(txt, 8, "Bad!", txt2, sizeof txt2) == -1 && loclive_pin_put(txt, 0, "X", txt2, sizeof txt2) == -1 &&
              loclive_pin_put(txt, 8, "Other", txt2, 8) == -1,
          "ungueltiger Tag / Konto 0 / zu kleiner Puffer -> Fehler");

    /* Plan */
    check(loclive_tag_plan("", 5, "Night Hawk", &pl) == 0 && !pl.park_acct && !pl.own_from[0] && !strcmp(pl.own_to, "Night Hawk"),
          "erster Logon: nichts umzuziehen");
    check(loclive_tag_plan("a5 Night Hawk\r\n", 5, "Night Hawk", &pl) == 0 && !pl.own_from[0], "gleicher Tag: nichts zu tun");
    check(loclive_tag_plan("a5 Live Test 01\r\n", 5, "Night Hawk", &pl) == 1 && !strcmp(pl.own_from, "Live Test 01"),
          "Wechsel (auch von der W479-Ableitung): alter Ordner -> neuer Tag");
    check(loclive_tag_plan("a5 night hawk\r\n", 5, "Night Hawk", &pl) == 1 && !strcmp(pl.own_from, "night hawk"),
          "nur Gross/Klein anders: umbenennen");
    r = loclive_tag_plan("a5 Old\r\na6 NIGHT HAWK\r\n", 5, "Night Hawk", &pl);
    check(r == 2 && pl.park_acct == 6 && !strcmp(pl.park_from, "NIGHT HAWK") && !strcmp(pl.park_to, "_a6") &&
              !strcmp(pl.own_from, "Old"),
          "veralteter Eintrag eines anderen Kontos: dessen Ordner parken, dann eigener Umzug");
    check(loclive_tag_plan("", 5, "Bad_Tag", &pl) == -1 && loclive_tag_plan("", 0, "Ok", &pl) == -1, "ungueltig -> -1");

    /* Ordnerumzug im Temp-Ordner (synthetisch, wie T:) */
    GetTempPathA(sizeof dir, dir);
    _snprintf_s(tdir, sizeof tdir, _TRUNCATE, "%sloc_live_selftest_%lu", dir, GetCurrentProcessId());
    rm_tree(tdir);
    CreateDirectoryA(tdir, NULL);
    mk_pilot(tdir, "Live Test 01CAMP");
    mk_pilot(tdir, "Live Test 01FREE");
    r = loclive_move_folders(tdir, "Live Test 01", "Night Hawk");
    check(r == 1 && dir_has(tdir, "Night HawkCAMP", "CAMPUSER") && dir_has(tdir, "Night HawkFREE", "CAMPUSER") &&
              !dir_has(tdir, "Live Test 01CAMP", NULL),
          "Wechsel: CAMP und FREE umgezogen, Piloten erhalten");
    check(loclive_move_folders(tdir, "Nobody", "Someone") == 1, "Quelle fehlt: nichts zu tun");
    mk_pilot(tdir, "Red FoxCAMP");
    r = loclive_move_folders(tdir, "Night Hawk", "Red Fox");
    check(r == 0 && dir_has(tdir, "Night HawkCAMP", "CAMPUSER") && dir_has(tdir, "Night HawkFREE", NULL),
          "Ziel belegt: nichts verschoben (beide Ordner bleiben)");
    r = loclive_move_folders(tdir, "Night Hawk", "night hawk");
    {
        WIN32_FIND_DATAA fd;
        char pat[MAX_PATH];
        HANDLE h;
        _snprintf_s(pat, sizeof pat, _TRUNCATE, "%s\\night hawkCAMP", tdir);
        h = FindFirstFileA(pat, &fd);
        check(r == 1 && h != INVALID_HANDLE_VALUE && strcmp(fd.cFileName, "night hawkCAMP") == 0,
              "nur Gross/Klein: Ordner umbenannt");
        if (h != INVALID_HANDLE_VALUE) FindClose(h);
    }
    check(loclive_move_folders(tdir, "..\\x", "y") == -1 && loclive_move_folders(tdir, "a", "b\\c") == -1,
          "Pfadteile im Tag -> Fehler (kein Ausbruch aus T:)");

    /* Merkdatei-Datei */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\steelbound_live.txt", tdir);
    check(!loclive_pin_lookup(path, 5, out), "Merkdatei fehlt -> kein Eintrag");
    if (fopen_s(&f, path, "wb") == 0 && f) {
        fputs(t0, f);
        fclose(f);
    }
    check(loclive_pin_lookup(path, 5, out) && !strcmp(out, "Old Tag"), "Merkdatei lesen");
    rm_tree(tdir);
}

/* ---------- W482 LIVE-MATCH: Formate (Attribut-JSON, Suchergebnis, Parse) ---------- */
static void w482_str(loclive_attr *a, uint32_t id, const wchar_t *s)
{
    int i;
    memset(a, 0, sizeof *a);
    a->id = id;
    a->is_str = 1;
    for (i = 0; s[i] && i < LOCLIVE_STR_MAX; i++) a->w[i] = (uint16_t)s[i];
}
static void w482_int(loclive_attr *a, uint32_t id, int64_t v)
{
    memset(a, 0, sizeof *a);
    a->id = id;
    a->v = v;
}

static void t_match(void)
{
    const uint32_t *ids;
    loclive_attr a[4];
    loclive_sess s;
    char js[512];
    static uint8_t blk[2048], out[512];
    uint8_t xn[36], key[16], sid[8] = {0x81, 1, 2, 3, 4, 5, 6, 7};
    uint32_t spec[8], used = 0, va = 0x00500000u, sz, v32;
    int64_t v64;
    uint16_t w;
    loclive_task t;
    int i, n;
    check(loclive_proc_ids(1, &ids) == 19 && ids[14] == 0x0010000Fu && ids[18] == 0x0010001Fu,
          "Prozedur 1: 19 Match-Attribute, Gamertag an 15, Pilot an 19");
    check(loclive_proc_ids(2, &ids) == 13 && ids[0] == 0x0010000Fu && ids[9] == 9 && ids[10] == 10,
          "Prozedur 2: 13 Chatraum-Attribute (9/10 wie Match)");
    check(loclive_proc_ids(3, &ids) == 0 && ids == NULL, "unbekannte Prozedur -> 0");
    /* JSON */
    w482_int(&a[0], 1, -(int64_t)0x7FFFFFFFFFFFFFFFll - 1);
    w482_str(&a[1], 0x0010000Fu, L"A\"b\\c\x00E4\x20AC");
    w482_int(&a[2], 0x10, 4);
    a[2].is_null = 1;
    w482_int(&a[3], 0x18, 0);
    n = loclive_attrs_json(a, 4, js, (int)sizeof js);
    check(n > 0 && strcmp(js, "[[1,-9223372036854775808],[1048591,\"A\\\"b\\\\c\xC3\xA4\xE2\x82\xAC\"],[16,null],[24,0]]") == 0,
          "Attribut-JSON: int64-Grenze, String UTF-8 mit Maskierung, null, Reihenfolge");
    check(loclive_attrs_json(a, 4, js, 20) == -1, "Attribut-JSON: zu kleiner Puffer -> -1");
    check(loclive_attrs_json(a, 0, js, 8) == 2 && strcmp(js, "[]") == 0, "Attribut-JSON: leer -> []");
    w482_str(&a[0], 0x0010000Fu, L"x\x0007y");
    a[0].w[1] = 0xD800; /* Surrogat im Spielspeicher */
    loclive_attrs_json(a, 1, js, (int)sizeof js);
    check(strcmp(js, "[[1048591,\"x?y\"]]") == 0, "Attribut-JSON: Surrogat/Steuerzeichen -> ?");
    check(loclive_kind(a, 1) == 1, "Art: ohne 0x13/0x1A -> Match");
    w482_int(&a[1], 0x13, 10);
    check(loclive_kind(a, 2) == 2, "Art: mit 0x13 -> Chatraum");
    /* Suchergebnis + Parse */
    memset(&s, 0, sizeof s);
    memcpy(s.sid, sid, 8);
    s.slots[0] = 9; s.slots[1] = 1; s.slots[2] = 0; s.slots[3] = 0;
    s.nattr = 3;
    w482_int(&s.attr[0], 1, 3);
    w482_str(&s.attr[1], 0x0010000Fu, L"Live Test 01");
    w482_int(&s.attr[2], 0x10, 0x123456789ll);
    for (i = 0; i < 36; i++) xn[i] = (uint8_t)(0xA0 + i);
    loclive_key_placeholder(sid, key);
    check(memcmp(key, "SBLIVEK", 7) == 0 && key[7] == 0 && memcmp(key + 8, sid, 8) == 0, "XNKEY-Platzhalter = SBLIVEK+0+XNKID");
    sz = loclive_result_size(&s);
    check(sz == 0x54 + 3 * 16 + 32, "Ergebnisgroesse: Kopf + 3 Attribute + 1 String");
    memset(blk, 0xCC, sizeof blk);
    loclive_result_build(blk, va, &s, xn, key);
    memcpy(&v32, blk + 0x50, 4);
    check(memcmp(blk + 4, sid, 8) == 0 && memcmp(blk + 0x0C, xn, 36) == 0 && memcmp(blk + 0x30, key, 16) == 0 && v32 == 3,
          "XMATCH_SEARCHRESULT: SessionID, HostAddress, Key, 3 Attribute");
    memcpy(&v32, blk + 0x40, 4);
    check(v32 == 9, "dwPublicOpen +0x40");
    memcpy(&v32, blk + 0x48, 4);
    check(v32 == 1, "dwPublicFilled +0x48");
    memcpy(&v32, blk + 0x54 + 16 + 8, 4);
    check(v32 == va + 0x54 + 48, "String-Zeiger zeigt in den Block (Spieladresse)");
    check(blk[sz] == 0xCC, "nichts hinter dem Block geschrieben");
    spec[0] = 0; spec[1] = 8; spec[2] = 0x00100000u; spec[3] = 32; spec[4] = 0; spec[5] = 8;
    check(loclive_parse(blk, va, sz, 3, spec, out, sizeof out, &used) == LOCLIVE_S_OK && used == 48,
          "Parse: int + str32 + int = 48 B");
    memcpy(&v64, out, 8);
    check(v64 == 3, "Parse: Attribut 1");
    memcpy(&w, out + 8, 2);
    check(w == 'L' && out[8 + 24] == 0 && out[8 + 30] == 0 && out[8 + 31] == 0, "Parse: String inline, NUL, Rest 0");
    memcpy(&v64, out + 40, 8);
    check(v64 == 0x123456789ll, "Parse: int64 voll");
    spec[2] = 0;
    check(loclive_parse(blk, va, sz, 3, spec, out, sizeof out, &used) == LOCLIVE_E_FAIL, "Parse: Typ passt nicht -> E_FAIL");
    spec[2] = 0x00100000u;
    check(loclive_parse(blk, va, sz, 4, spec, out, sizeof out, &used) == LOCLIVE_E_FAIL, "Parse: mehr Specs als Attribute -> E_FAIL");
    check(loclive_parse(blk, va, sz, 3, spec, out, 40, &used) == LOCLIVE_E_FAIL, "Parse: Ausgabe zu klein -> E_FAIL");
    check(loclive_parse(blk, va, sz - 1, 3, spec, out, sizeof out, &used) == LOCLIVE_E_FAIL, "Parse: Block kuerzer als Groesse -> E_FAIL");
    memcpy(&v32, blk + 0x54 + 16 + 8, 4);
    v32 += 0x1000;
    memcpy(blk + 0x54 + 16 + 8, &v32, 4);
    check(loclive_parse(blk, va, sz, 3, spec, out, sizeof out, &used) == LOCLIVE_E_FAIL, "Parse: String-Zeiger ausserhalb -> E_FAIL");
    /* Netz-Task */
    memset(&t, 0, sizeof t);
    t.magic = LOCLIVE_TASK_MAGIC;
    t.kind = LLT_NET;
    check(loclive_task_step(&t, 0, 0, 0) == LOCLIVE_TASK_RUNNING, "Netz-Task: laeuft ohne Ergebnis");
    t.result = LOCLIVE_TASK_SUCCESS;
    check(loclive_task_step(&t, 0, 0, 0) == LOCLIVE_TASK_SUCCESS, "Netz-Task: Ergebnis gemeldet");
    t.result = LOCLIVE_E_FAIL;
    check(loclive_task_step(&t, 0, 0, 0) == LOCLIVE_E_FAIL, "Netz-Task: Fehler gemeldet (Spiel: Live-Init neu)");
}

/* W485 LIVE-QOS: XNQOS-Block im Live-Modus (Werte aus dem Transport, Bitrate >= 205 200, COMPLETE). */
static uint16_t q16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t q32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void t_qos(void)
{
    uint8_t q[8 + 24 * 64], e[24];
    loclive_qos_val v[4] = {{LOCLIVE_QOS_SRC_DIRECT, 18, 140},
                            {LOCLIVE_QOS_SRC_RELAY, 95, 310},
                            {LOCLIVE_QOS_SRC_EST, 60, 160},
                            {LOCLIVE_QOS_SRC_SERVICE, 30, 30}};
    loclive_qos_val big = {LOCLIVE_QOS_SRC_RELAY, 9000, 70000}, inv = {LOCLIVE_QOS_SRC_DIRECT, 200, 50};
    loclive_qos_val zero = {LOCLIVE_QOS_SRC_NONE, 0, 0};
    uint32_t n, i;
    int src;
    /* Bitrate je Herkunft: immer >= Tabelle 0x2518D4 (10 Spieler) */
    for (src = 0; src <= 6; src++) check(loclive_qos_bps(src) >= LOCLIVE_QOS_MIN_BPS, "QoS Bitrate >= 205200 je Herkunft");
    check(loclive_qos_bps(LOCLIVE_QOS_SRC_DIRECT) == 10000000u, "QoS direkt 10 Mbit/s (wie System Link)");
    check(loclive_qos_bps(LOCLIVE_QOS_SRC_RELAY) < loclive_qos_bps(LOCLIVE_QOS_SRC_DIRECT), "QoS Relay < direkt");
    n = loclive_qos_block(q, sizeof q, 8, v, 4);
    check(n == 8 + 24 * 4, "QoS Block Groesse 8 + 24 n");
    check(q32(q) == 4 && q32(q + 4) == 0, "QoS cxnqos = 4, cxnqosPending = 0");
    for (i = 0; i < 4; i++) {
        const uint8_t *x = q + 8 + 24 * i;
        check((x[0] & 3) == 3, "QoS Eintrag COMPLETE|CONTACTED");
        check(q16(x + 2) == 8 && q16(x + 4) == 8, "QoS Proben gesendet = empfangen = 8");
        check(q16(x + 6) == 0 && q32(x + 8) == 0, "QoS kein Listen-Datenblock");
        check(q16(x + 12) == (uint16_t)v[i].rtt_min && q16(x + 14) == (uint16_t)v[i].rtt_med, "QoS RTT min/med uebernommen");
        check(q32(x + 16) == loclive_qos_bps(v[i].src) && q32(x + 20) == q32(x + 16), "QoS Up = Dn = Bitrate");
    }
    /* Klasse wie im Spiel (RttMed/2): 140 -> 70 -> 4, 310 -> 155 -> 3, 160 -> 80 -> 4 */
    check(loclive_qos_class(q + 8) == 4 && loclive_qos_class(q + 32) == 3 && loclive_qos_class(q + 56) == 4,
          "QoS Klasse aus RttMed/2 (4/3/4)");
    loclive_qos_entry(e, 8, &big);
    check(q16(e + 14) == 4999 && q16(e + 12) == 4999 && loclive_qos_class(e) == 1, "QoS RTT auf 4999 begrenzt, Klasse 1");
    loclive_qos_entry(e, 8, &inv);
    check(q16(e + 12) == 50 && q16(e + 14) == 50, "QoS min <= med erzwungen");
    loclive_qos_entry(e, 0, &zero);
    check(q16(e + 2) == 1 && q16(e + 12) >= 1 && q16(e + 14) >= 1 && (e[0] & 1), "QoS 0 Proben/0 ms -> 1, COMPLETE");
    loclive_qos_entry(e, 8, NULL);
    check(q16(e + 14) == LOCLIVE_QOS_RTT_DEFAULT && loclive_qos_class(e) == 4 && q32(e + 16) >= LOCLIVE_QOS_MIN_BPS,
          "QoS ohne Messung: Vorgabe 100 ms, Klasse 4, Bitrate ok");
    n = loclive_qos_block(q, sizeof q, 8, NULL, 0);
    check(n == 32 && q32(q) == 1 && (q[8] & 1), "QoS 0 Ziele -> 1 Eintrag (Create bricht nicht ab)");
    n = loclive_qos_block(q, sizeof q, 8, NULL, 100);
    check(n == 8 + 24 * 64 && q32(q) == 64, "QoS hoechstens 64 Ziele");
    check(loclive_qos_block(q, 31, 8, v, 1) == 0, "QoS Puffer zu klein -> 0");
    check(loclive_qos_block(NULL, 100, 8, v, 1) == 0, "QoS ohne Puffer -> 0");
}

/* ---------- W491 CAMP-CLIENT: Kampagne im Client (loc_camp.c) ---------- */
static void be(uint8_t *p, uint64_t v, int n)
{
    int i;
    for (i = n - 1; i >= 0; i--, v >>= 8) p[i] = (uint8_t)v;
}
static int mk_ticket(uint8_t *t, uint32_t acct, uint64_t issued, uint64_t until)
{
    static const char *strs[4] = {"steelnet-eu", "steelbound", "steelbound-test", "Camp Tag"};
    int n = 57, i;
    memset(t, 0x5A, 300);
    memcpy(t, "SBCT", 4);
    t[4] = 1;
    be(t + 13, issued, 8);
    be(t + 21, until, 8);
    be(t + 29, acct, 4);
    be(t + 33, ((uint64_t)0x53424C01u << 32) | acct, 8);
    for (i = 0; i < 4; i++) {
        size_t l = strlen(strs[i]);
        t[n++] = (uint8_t)l;
        memcpy(t + n, strs[i], l);
        n += (int)l;
    }
    return n + 64; /* Signatur (hier Fuellbytes; prueft nur der Server) */
}
static void t_camp(void)
{
    uint8_t tk[300], pre[LOCCAMP_PRE_MAX + 8], b[16], info[12], fr[0x1410];
    long long exp = 0;
    int n, pl;
    uint16_t port = 0;
    uint32_t ip_net;
    uint16_t port_net;
    /* Vorspann */
    n = mk_ticket(tk, 4242, 1000, 2800);
    check(n >= LOCCAMP_TICKET_MIN && n <= LOCCAMP_TICKET_MAX, "Kampagne: Testticket im Groessenbereich");
    pl = loccamp_preamble(tk, n, pre, sizeof pre);
    check(pl == 8 + n && memcmp(pre, "SBGW", 4) == 0 && pre[4] == 1 && pre[5] == 0 && pre[6] == (uint8_t)(n >> 8) &&
              pre[7] == (uint8_t)n && memcmp(pre + 8, tk, (size_t)n) == 0,
          "Vorspann: SBGW, Version 1, Flags 0, u16 BE Laenge, Ticket");
    check(loccamp_preamble(tk, LOCCAMP_TICKET_MIN - 1, pre, sizeof pre) == -1 &&
              loccamp_preamble(tk, LOCCAMP_TICKET_MAX + 1, pre, sizeof pre) == -1 &&
              loccamp_preamble(tk, n, pre, n + 7) == -1 && loccamp_preamble(NULL, n, pre, sizeof pre) == -1,
          "Vorspann: Ticket zu kurz/zu lang, Puffer zu klein, NULL -> -1");
    /* Antwort des Zugangs */
    memcpy(b, "SBGW\x01\x00", 6);
    check(loccamp_gw_status(b, 6) == LOCCAMP_GW_OK, "Zugang: Status 0 = ok");
    b[5] = 1;
    check(loccamp_gw_status(b, 6) == LOCCAMP_GW_REFUSED, "Zugang: Status 1 = abgewiesen (kein Orakel)");
    b[5] = 2;
    check(loccamp_gw_status(b, 6) == LOCCAMP_GW_BUSY, "Zugang: Status 2 = beschaeftigt");
    b[5] = 3;
    check(loccamp_gw_status(b, 6) == LOCCAMP_GW_UNSUPPORTED, "Zugang: Status 3 = nicht unterstuetzt");
    b[5] = 4;
    check(loccamp_gw_status(b, 6) == -1, "Zugang: unbekannter Status -> kaputt");
    b[5] = 0;
    b[4] = 2;
    check(loccamp_gw_status(b, 6) == -1, "Zugang: falsche Version -> kaputt");
    b[4] = 1;
    b[0] = 'X';
    check(loccamp_gw_status(b, 6) == -1, "Zugang: falsche Magie -> kaputt");
    check(loccamp_gw_status(b, 5) == -2 && loccamp_gw_status(NULL, 6) == -2, "Zugang: unvollstaendig -> -2");
    /* Rahmen Server -> Spiel */
    memset(fr, 0, sizeof fr);
    fr[0] = 0x00; fr[1] = 0x00; fr[2] = 0x10; fr[3] = 0x10; /* 0x1010 ohne Nutzlast */
    check(loccamp_frame(fr, 4) == 4 && loccamp_frame(fr, 3) == 0 && loccamp_frame(fr, 1) == 0, "Rahmen: Kopf allein / unvollstaendig");
    fr[0] = 0x05;
    check(loccamp_frame(fr, 8) == 0 && loccamp_frame(fr, 9) == 9 && loccamp_frame(fr, 20) == 9,
          "Rahmen: Nutzlast 5 -> 9 Byte, Rest gehoert zum naechsten Rahmen");
    fr[0] = 0x00; fr[1] = 0x14; /* 0x1400 = Grenze */
    check(loccamp_frame(fr, 4 + 0x1400) == 4 + 0x1400, "Rahmen: 0x1400 Nutzlast erlaubt");
    fr[0] = 0x01;
    check(loccamp_frame(fr, 4) == -1, "Rahmen: 0x1401 Nutzlast -> trennen");
    /* base64url */
    {
        uint8_t o[8];
        check(loccamp_b64url_dec("U0JDVA", o, sizeof o) == 4 && memcmp(o, "SBCT", 4) == 0, "base64url ohne '='");
        check(loccamp_b64url_dec("-_8", o, sizeof o) == 2 && o[0] == 0xFB && o[1] == 0xFF, "base64url: - und _");
        check(loccamp_b64url_dec("U0JDVA==", o, sizeof o) == -1 && loccamp_b64url_dec("U0J+", o, sizeof o) == -1,
              "base64url: '=' und '+' abgelehnt");
        check(loccamp_b64url_dec("U", o, sizeof o) == -1 && loccamp_b64url_dec("U1", o, sizeof o) == -1,
              "base64url: ungueltige Laenge / Restbits");
        check(loccamp_b64url_dec("U0JDVEFC", o, 4) == -1, "base64url: Puffer zu klein");
    }
    /* Ticketpruefung im Client */
    check(loccamp_ticket_ok(tk, n, 4242, &exp) == 1 && exp == 2800, "Ticket: plausibel, Ablauf gelesen");
    check(loccamp_ticket_ok(tk, n, 4243, &exp) == 0 && exp == 0, "Ticket: anderes Konto -> abgelehnt");
    check(loccamp_ticket_ok(tk, n, 0, NULL) == 0, "Ticket: ohne Konto -> abgelehnt");
    tk[40] ^= 1;
    check(loccamp_ticket_ok(tk, n, 4242, NULL) == 0, "Ticket: Pseudo-XUID passt nicht -> abgelehnt");
    tk[40] ^= 1;
    tk[0] = 'X';
    check(loccamp_ticket_ok(tk, n, 4242, NULL) == 0, "Ticket: falsche Magie -> abgelehnt");
    tk[0] = 'S';
    tk[4] = 2;
    check(loccamp_ticket_ok(tk, n, 4242, NULL) == 0, "Ticket: Version 2 -> abgelehnt");
    tk[4] = 1;
    check(loccamp_ticket_ok(tk, 128, 4242, NULL) == 0 && loccamp_ticket_ok(tk, 286, 4242, NULL) == 0,
          "Ticket: Groesse ausserhalb 129..285 -> abgelehnt");
    n = mk_ticket(tk, 4242, 1000, 1000 + 3601);
    check(loccamp_ticket_ok(tk, n, 4242, NULL) == 0, "Ticket: Laufzeit > 3600 s -> abgelehnt");
    n = mk_ticket(tk, 4242, 1000, 1000);
    check(loccamp_ticket_ok(tk, n, 4242, NULL) == 0, "Ticket: Laufzeit 0 -> abgelehnt");
    /* Erneuerung */
    check(loccamp_renew_due(1000, 0, 0) == 1, "Erneuern: kein Ticket -> holen");
    check(loccamp_renew_due(1000, 1000 + 1800, 1) == 0, "Erneuern: frisch (30 min) -> nicht");
    check(loccamp_renew_due(1000, 1000 + LOCCAMP_RENEW_S, 1) == 0, "Erneuern: genau 5 min -> noch nicht");
    check(loccamp_renew_due(1000, 1000 + LOCCAMP_RENEW_S - 1, 1) == 1, "Erneuern: < 5 min vor Ablauf -> holen");
    check(loccamp_renew_due(5000, 1000, 1) == 1, "Erneuern: abgelaufen -> holen");
    /* Dienst 0x43430009 */
    check(loccamp_service_hr(LOCCAMP_ST_READY) == LOCLIVE_S_OK, "Dienst: bereit -> S_OK");
    check(loccamp_service_hr(LOCCAMP_ST_PENDING) == LOCLIVE_E_SVC_UNAVAILABLE &&
              loccamp_service_hr(LOCCAMP_ST_DOWN) == LOCLIVE_E_SVC_UNAVAILABLE &&
              loccamp_service_hr(LOCCAMP_ST_OFF) == LOCLIVE_E_SVC_UNAVAILABLE,
          "Dienst: geprueft/nicht verfuegbar/aus -> 0x80151102 (Campaign grau)");
    check(loclive_service_hr(LOCLIVE_SVC_CAMPAIGN) == LOCLIVE_E_SVC_UNAVAILABLE,
          "Dienst: Kern ohne Bruecke bleibt 0x80151102");
    loccamp_service_info(info);
    memcpy(&ip_net, info + 4, 4);
    memcpy(&port_net, info + 8, 2);
    check(info[0] == 0x09 && info[1] == 0x00 && info[2] == 0x43 && info[3] == 0x43, "Dienstinfo: dwServiceID 0x43430009");
    check(info[4] == 127 && info[5] == 250 && info[6] == 67 && info[7] == 9, "Dienstinfo: IN_ADDR 127.250.67.9 (Netz-Reihenfolge)");
    check(port_net == LOCCAMP_GAME_PORT && info[10] == 0 && info[11] == 0, "Dienstinfo: Port in Host-Reihenfolge (Spiel ruft htons)");
    {
        uint16_t pn = (uint16_t)(((LOCCAMP_GAME_PORT & 0xFF) << 8) | (LOCCAMP_GAME_PORT >> 8)); /* htons */
        uint16_t po = (uint16_t)((((LOCCAMP_GAME_PORT + 1) & 0xFF) << 8) | ((LOCCAMP_GAME_PORT + 1) >> 8));
        uint32_t other = ip_net ^ 0x01000000u;
        check(loccamp_is_vip(ip_net, pn) == 1, "Umleitung: virtuelle Adresse + Port erkannt");
        check(loccamp_is_vip(ip_net, po) == 0 && loccamp_is_vip(other, pn) == 0 && loccamp_is_vip(0x0100007Fu, pn) == 0,
              "Umleitung: anderer Port/andere Adresse/127.0.0.1 nicht");
    }
    check(strcmp(loccamp_campaign_id("test", &port), LOCCAMP_ID_TEST) == 0 && port == LOCCAMP_PORT_TEST,
          "Kampagne: test -> steelbound-test, 47103");
    check(strcmp(loccamp_campaign_id("", &port), LOCCAMP_ID_SEASON) == 0 && port == LOCCAMP_PORT_SEASON &&
              strcmp(loccamp_campaign_id(NULL, NULL), LOCCAMP_ID_SEASON) == 0 &&
              strcmp(loccamp_campaign_id("TEST", &port), LOCCAMP_ID_SEASON) == 0,
          "Kampagne: sonst Saison steelbound-s1, 47100");
    check(loccamp_logon_ready(LOCCAMP_ST_PENDING, 100, LOCCAMP_LOGON_WAIT_MS) == 0, "Logon wartet, solange die Kampagne geprueft wird");
    check(loccamp_logon_ready(LOCCAMP_ST_PENDING, LOCCAMP_LOGON_WAIT_MS, LOCCAMP_LOGON_WAIT_MS) == 1, "Logon nach Wartezeit ohne Kampagne");
    check(loccamp_logon_ready(LOCCAMP_ST_READY, 0, LOCCAMP_LOGON_WAIT_MS) == 1 &&
              loccamp_logon_ready(LOCCAMP_ST_DOWN, 0, LOCCAMP_LOGON_WAIT_MS) == 1 &&
              loccamp_logon_ready(LOCCAMP_ST_OFF, 0, LOCCAMP_LOGON_WAIT_MS) == 1,
          "Logon sofort, wenn die Kampagne entschieden bzw. aus ist");
    { /* W492: Reinforcements-Auswahl (FUN_00118E50) */
        uint16_t o = 99;
        uint32_t m;
        int bad = 0, k;
        check(loccamp_reinf_blocked(0) == 1 && loccamp_reinf_next(0, 0, 1, &o) == -1 && o == 99 &&
                  loccamp_reinf_next(5, 0, -1, &o) == -1,
              "Reinforcements: leere Maske -> Original kaeme nicht zurueck, Schutz greift");
        for (k = 0, m = 1; k < 4000 && !bad; k++, m = m * 2654435761u + 0x9E37u) { /* viele Masken != 0, beide Richtungen */
            uint16_t c;
            if (!m) continue;
            for (c = 0; c < 32 && !bad; c++) {
                bad |= loccamp_reinf_blocked(m) != 0 || loccamp_reinf_next(c, m, 1, &o) < 0 || !(m & (1u << o)) ||
                       loccamp_reinf_next(c, m, -1, &o) < 0 || !(m & (1u << o));
            }
        }
        check(!bad && loccamp_reinf_blocked(0x80000000u) == 0, "Reinforcements: jede Maske != 0 endet, Schutz greift nicht");
        check(loccamp_reinf_next(0, 0x5u, 1, &o) == 1 && o == 2 && loccamp_reinf_next(2, 0x5u, 1, &o) == 1 && o == 0 &&
                  loccamp_reinf_next(0, 0x5u, -1, &o) == 1 && o == 2 && loccamp_reinf_next(0, 0x1u, 1, &o) == 0 && o == 0,
              "Reinforcements: naechstes Bit mit Umlauf, einzelnes Bit = keine Aenderung (wie Original)");
        /* Shop-Eintrag 9 B wie FUN_001201F0: u16 Typ, u32 Limit, u16 Kosten, Fuellbyte; Limit 0 -> nicht in der Maske */
        {
            static const uint8_t ans[] = { 0x00, 0x02, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x0C, 0x00, 0x00,
                                           0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x16, 0x00, 0x00 };
            uint32_t mk = 0;
            unsigned i, cnt = (unsigned)ans[1] | ((unsigned)ans[2] << 8);
            for (i = 0; i < cnt; i++) {
                const uint8_t *e = ans + 3 + 9 * i;
                uint32_t lim = (uint32_t)e[2] | ((uint32_t)e[3] << 8) | ((uint32_t)e[4] << 16) | ((uint32_t)e[5] << 24);
                if (lim) mk |= 1u << (e[0] & 31);
            }
            check(mk == 1u && !loccamp_reinf_blocked(mk), "Reinforcements: Shop-Antwort (Vitzh unbegrenzt, Falchion ausverkauft) -> Maske Bit 0");
        }
    }
    { /* W515 FACTIONSWAP (#110): Enlistment Office im HQ (FUN_000EF6B0 @0xEF737) */
        const uint8_t *o = loccamp_swap_orig, *nw = loccamp_swap_new;
        uint32_t va = LOCCAMP_SWAP_VA;
        uint32_t jb_to = va + 6 + o[5], jbe_to = va + 12 + o[11], jmp_to = va + 6 + nw[5];
        int t, f, bad = 0, diff = 0, i;
        for (i = 0; i < 12; i++) diff += o[i] != nw[i];
        check(o[4] == 0x72 && o[10] == 0x76 && nw[4] == 0xEB && diff == 2,
              "Seitenwechsel: Patch ersetzt nur jb (Zug < 3) durch jmp, 2 Byte");
        check(jb_to == 0xEF743u && jbe_to == 0xEF754u && jmp_to == jbe_to,
              "Seitenwechsel: jmp springt wie 'Zug 3..6' hinter die Sperre (0xEF754), jb ging auf die Sperre (0xEF743)");
        for (t = 0; t <= 9; t++) {
            int orig_open = !loccamp_enlist_locked(1, 0, t, 0);
            bad |= orig_open != (t >= 3 && t <= 6);   /* Original: nur Zug 3..6 */
            bad |= loccamp_enlist_locked(1, 1, t, 1);  /* Patch: jeder Zug */
            for (f = 0; f <= 4; f++) bad |= loccamp_enlist_locked(0, f, t, 1) != 1 || loccamp_enlist_locked(0, f, t, 0) != 1;
            bad |= loccamp_enlist_locked(1, 4, t, 0) || loccamp_enlist_locked(1, 4, t, 1);
        }
        check(!bad, "Seitenwechsel: Original offen nur Zug 3..6, mit Patch jeder Zug; neutral immer offen, ohne Kampagne immer grau");
    }
    { /* W515: nach dem Wechsel ins CAMPAIGN-Menue (Pilot + Garage neu) statt ins MAIN MENU (FUN_00112730 @0x112738) */
        const uint8_t *o = loccamp_swap_reload_orig, *nw = loccamp_swap_reload_new;
        int i, diff = 0;
        for (i = 0; i < 6; i++) diff += o[i] != nw[i];
        check(o[2] == 0x74 && o[3] == 0x0B && LOCCAMP_SWAP_RELOAD_VA + 4 + o[3] == 0x112747u,
              "Seitenwechsel: je (Flag 0x14 nicht gesetzt) bleibt auf dem HQ-Zweig (0x112747)");
        check(o[4] == 0x6A && o[5] == LOCCAMP_SCREEN_MAIN && nw[5] == LOCCAMP_SCREEN_CAMPAIGN && diff == 1,
              "Seitenwechsel: nur das push-Argument 8 (MAIN MENU) -> 3 (CAMPAIGN, laedt Pilot und Garage) aendert sich");
    }
}

int main(void)
{
    t_camp(); /* W491 */
    t_qos();
    t_match();
    t_services();
    t_logon();
    t_tasks();
    t_gamertag();
    t_xuid();
    t_user();
    t_pin();
    printf("loc_live_selftest: %d/%d OK\n", g_ok, g_n);
    return g_ok == g_n ? 0 : 1;
}
