/* loc_mem_selftest.c - W493 LOC-MEMLEAK: LOCMEM-Allokator synthetisch pruefen (ohne Spiel, ohne Spiegel).
 * Faelle: alter Modus (nur gleiche Groesse), VB/IB-Kopf wie XDK, Release/Refcount, Quarantaene (Inhalt bleibt,
 * keine Wiederverwendung vorher), Groessenklassen, Abbau des obersten Blocks, Vorlauf (nichts veraendert), doppelte
 * Freigabe, interner Refcount, Missionszyklen (Fuellstand flach) gegen den alten Weg (laeuft voll), vorzeitiges Leeren
 * der Quarantaene, Ausrichtung, Meldung "voll" nur einmal mit Aufrufer-Statistik. Exit 0 = alles gut. */
#include "loc_mem.h"
#include "hcfg.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

hcfg_t g_cfg; /* hlock liest diag_lock_model/diag_lock_inject/hlock_spin (alles 0) */
static int g_fail, g_pass;
static unsigned g_nlog_stelle, g_nlog_voll, g_nlog_auf;
static uint32_t g_now;

void hlog_msg(int level, const char *fmt, ...)
{
    char b[1024];
    va_list ap;
    (void)level;
    va_start(ap, fmt);
    _vsnprintf_s(b, sizeof b, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (strstr(b, "LOCMEM-STELLE")) g_nlog_stelle++;
    if (strstr(b, "Bereich voll bei Anforderung")) g_nlog_voll++;
    if (strstr(b, "LOCMEM-AUF")) g_nlog_auf++;
}

static uint32_t clk(void) { return g_now; }

#define CHECK(c, ...)                                                                                                 \
    do {                                                                                                              \
        if (c) {                                                                                                      \
            g_pass++;                                                                                                 \
        } else {                                                                                                      \
            g_fail++;                                                                                                 \
            printf("FEHLER Zeile %d: ", __LINE__);                                                                    \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
        }                                                                                                             \
    } while (0)

static uint32_t g_base, g_size;

static void reset(uint32_t size, int reclaim, uint32_t quar)
{
    memset((void *)(uintptr_t)g_base, 0xCD, g_size);
    loc_mem_init_test(g_base, size);
    loc_mem_config(reclaim, quar, clk);
    g_now = 1000;
}

static uint32_t used(void)
{
    loc_mem_info_t in;
    loc_mem_info(&in);
    return in.used;
}

static uint32_t rel(uint32_t res)
{
    uint32_t rc = 0xFFFFFFFFu;
    int r = loc_mem_res2_release(res, 0, &rc);
    if (r != 1) { /* nur Fehler zaehlen (Zyklen rufen das 180 000 Mal) */
        g_fail++;
        printf("FEHLER: Release 0x%X nicht als eigener Kopf erkannt\n", res);
    }
    return rc;
}

static unsigned s_rng = 12345u;
static unsigned rnd(void)
{
    s_rng = s_rng * 1103515245u + 12345u;
    return (s_rng >> 8) & 0xFFFFFFu;
}

int main(void)
{
    loc_mem_info_t in;
    uint32_t a, b, c, r1, r2, *h, d;
    int i, k;
    /* Puffer unter 256 MB (phys == VA wie im Client) */
    g_size = 64u << 20;
    for (a = 0x02000000u; a < 0x0C000000u && !g_base; a += 0x00400000u) {
        void *p = VirtualAlloc((void *)(uintptr_t)a, g_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p) g_base = (uint32_t)(uintptr_t)p;
    }
    if (!g_base) {
        printf("loc_mem_selftest: kein Puffer unter 256 MB - ABBRUCH\n");
        return 2;
    }

    /* 1. Alter Modus: nur gleiche Groesse, sofort wieder verwendbar */
    reset(16u << 20, 0, 300);
    a = loc_mem_alloc(0x3000, 0x1000, 0);
    b = loc_mem_alloc(0x2000, 0x1000, 0);
    CHECK(a == g_base && b == g_base + 0x3000, "alt: Bump 0x%X 0x%X", a, b);
    CHECK(loc_mem_free(a) == 1, "alt: Freigabe");
    c = loc_mem_alloc(0x2000, 0x1000, 0);
    CHECK(c == g_base + 0x5000, "alt: andere Groesse nicht wiederverwendet (0x%X)", c);
    c = loc_mem_alloc(0x3000, 0x1000, 0);
    CHECK(c == a, "alt: gleiche Groesse sofort wiederverwendet (0x%X)", c);

    /* 2. Kopf wie XDK, Release, Quarantaene */
    reset(16u << 20, 1, 10);
    r1 = loc_mem_res2_create(1000, 0, 0x84ED8u);
    h = (uint32_t *)(uintptr_t)r1;
    CHECK(r1 && h[0] == 0x01000001u, "VB-Kopf Common 0x%X", r1 ? h[0] : 0);
    CHECK(r1 && loc_mem_owns(h[1]) && h[2] == 0, "VB-Daten im Bereich, Lock 0");
    d = r1 ? h[1] : 0;
    CHECK(d && ((uint8_t *)(uintptr_t)d)[0] == 0 && ((uint8_t *)(uintptr_t)d)[999] == 0, "VB-Daten genullt");
    CHECK(loc_mem_size(d) == 1024u, "VB auf Klassengroesse 1024 gerundet (%u)", loc_mem_size(d));
    r2 = loc_mem_res2_create(64, 1, 0x84C40u);
    CHECK(r2 && ((uint32_t *)(uintptr_t)r2)[0] == 0x01010001u, "IB-Kopf Common");
    CHECK(loc_mem_res2_owns(r1) && loc_mem_res2_owns(r2) && !loc_mem_res2_owns(r1 + 4) && !loc_mem_res2_owns(g_base),
          "Kopf-Erkennung");
    memset((void *)(uintptr_t)d, 0x5A, 1000);
    /* AddRef wie das Spiel (Common++), dann zweimal Release */
    h[0]++;
    CHECK(rel(r1) == 1, "Release nach AddRef -> 1");
    loc_mem_info(&in);
    CHECK(in.q_n == 0 && in.h_rel == 0, "noch nicht frei");
    /* Vorlauf: nichts veraendern */
    {
        uint32_t rc = 99;
        CHECK(loc_mem_res2_release(r1, 1, &rc) == 1 && rc == 0, "Vorlauf: Rueckgabe 0 (%u)", rc);
        CHECK(h[0] == 0x01000001u, "Vorlauf: Kopf unveraendert (0x%X)", h[0]);
        loc_mem_info(&in);
        CHECK(in.q_n == 0 && in.h_spec == 1, "Vorlauf: nichts freigegeben");
    }
    CHECK(rel(r1) == 0, "Release -> 0");
    loc_mem_info(&in);
    CHECK(in.q_n == 1 && in.h_rel == 1 && in.t_live_n[LOCMEM_T_VB] == 0, "VB in Quarantaene (q=%u)", in.q_n);
    CHECK(h[1] == d && ((uint8_t *)(uintptr_t)d)[500] == 0x5A, "Inhalt und Kopf bleiben in der Quarantaene stehen");
    CHECK(rel(r1) == 0, "doppelte Freigabe -> 0");
    loc_mem_info(&in);
    CHECK(in.h_odd == 1 && in.q_n == 1, "doppelte Freigabe ohne Wirkung");
    /* waehrend der Quarantaene: gleiche Groesse bekommt einen neuen Block */
    g_now += 5;
    a = loc_mem_alloc_t(1000, 16, 1, LOCMEM_T_VB, 0);
    CHECK(a && a != d, "Quarantaene: kein Wiederverwenden vor Ablauf (0x%X)", a);
    CHECK(((uint8_t *)(uintptr_t)d)[500] == 0x5A, "Quarantaene: Inhalt unberuehrt");
    /* nach Ablauf: 900 Byte (Klasse 1024) nehmen den alten Block */
    g_now += 10;
    loc_mem_tick();
    b = loc_mem_alloc_t(900, 16, 1, LOCMEM_T_VB, 0);
    CHECK(b == d, "nach der Quarantaene: Block der Klasse wiederverwendet (0x%X / 0x%X)", b, d);
    loc_mem_info(&in);
    CHECK(in.nreuse >= 1, "Wiederverwendung gezaehlt");
    /* Kopf wird nach der Quarantaene wiederverwendet */
    c = loc_mem_res2_create(2000, 0, 0);
    CHECK(c == r1, "Kopf nach der Quarantaene wiederverwendet (0x%X / 0x%X)", c, r1);

    /* 3. interner Refcount (0x00780000): wie XDK nicht zerstoeren */
    reset(16u << 20, 1, 0);
    r1 = loc_mem_res2_create(256, 0, 0);
    h = (uint32_t *)(uintptr_t)r1;
    h[0] |= 0x00080000u;
    CHECK(rel(r1) == 0, "interner Refcount: Rueckgabe 0");
    loc_mem_info(&in);
    CHECK(in.h_relint == 1 && in.t_live_n[LOCMEM_T_VB] == 1, "interner Refcount: Daten behalten");

    /* 4. Abbau des obersten Blocks (Quarantaene 0) */
    reset(16u << 20, 1, 0);
    a = loc_mem_alloc(0x4000, 0x1000, 0);
    b = loc_mem_alloc(0x4000, 0x1000, 0);
    CHECK(used() == 0x8000u, "Fuellstand 0x8000 (0x%X)", used());
    loc_mem_free(b);
    CHECK(used() == 0x4000u, "oberster Block abgebaut (0x%X)", used());
    loc_mem_free(a);
    CHECK(used() == 0u, "alles abgebaut (0x%X)", used());

    /* 5. Ausrichtung: 16er-VB-Block nicht fuer eine Seitenanforderung */
    reset(16u << 20, 1, 0);
    a = loc_mem_alloc_t(16, 16, 0, LOCMEM_T_VB, 0);
    b = loc_mem_alloc_t(0x2000, 16, 0, LOCMEM_T_VB, 0); /* 8 KB, 16er Ausrichtung, liegt bei +16 */
    c = loc_mem_alloc_t(16, 16, 0, LOCMEM_T_VB, 0);    /* haelt den Fuellstand oben */
    loc_mem_free(b);
    d = loc_mem_alloc(0x2000, 0x1000, 0);
    CHECK(d != b && !(d & 0xFFFu), "Contig nimmt keinen falsch ausgerichteten Block (0x%X)", d);
    (void)a;
    (void)c;

    /* 6. Missionszyklen: je Runde 3000 VB/IB zufaelliger Groesse anlegen, danach die der Vorrunde freigeben (die neue
     * Mission laedt, bevor die alte weg ist -> Luecken), Bilder weiter, 60 Runden */
    {
        static uint32_t res[2][3000];
        uint32_t u3 = 0, u60 = 0, umax = 0;
        reset(64u << 20, 1, 300);
        s_rng = 4711u;
        memset(res, 0, sizeof res);
        for (k = 0; k < 60; k++) {
            uint32_t *cur = res[k & 1], *prv = res[(k + 1) & 1];
            for (i = 0; i < 3000; i++) {
                cur[i] = loc_mem_res2_create(16u + rnd() % 8000u, (i & 7) == 0, 0x84ED8u);
                if (!cur[i]) break;
            }
            CHECK(i == 3000, "Zyklus %d: Anlegen gescheitert bei %d (Fuellstand %u MB)", k, i, used() >> 20);
            if (used() > umax) umax = used();
            for (i = 0; i < 3000; i++) {
                if (prv[i]) rel(prv[i]);
                prv[i] = 0;
            }
            g_now += 400; /* Missionsende -> naechste Mission (> Quarantaene) */
            loc_mem_tick();
            if (k == 2) u3 = umax;
            if (k == 59) u60 = umax;
        }
        for (i = 0; i < 3000; i++) {
            if (res[0][i]) rel(res[0][i]);
            if (res[1][i]) rel(res[1][i]);
        }
        g_now += 400;
        loc_mem_tick();
        CHECK(used() == 0, "nach dem letzten Zyklus alles abgebaut (%u KB)", used() >> 10);
        loc_mem_info(&in);
        CHECK(u60 <= u3 + (u3 / 4u), "Fuellstand flach: Hoechststand nach 3 Runden %u KB, nach 60 Runden %u KB", u3 >> 10,
              u60 >> 10);
        CHECK(in.nfull == 0 && in.nforce == 0, "kein Voll, kein vorzeitiges Leeren (voll=%u vorzeitig=%u)", in.nfull,
              in.nforce);
        printf("Zyklen: Hoechststand nach 3 Runden %u KB, nach 60 Runden %u KB, %u neu, %u wiederverwendet\n", u3 >> 10,
               u60 >> 10, in.nalloc, in.nreuse);
        /* derselbe Ablauf auf dem alten Weg (Kopf ohne Refcount, nie frei) laeuft voll */
        reset(64u << 20, 0, 300);
        s_rng = 4711u;
        for (k = 0; k < 60; k++) {
            for (i = 0; i < 3000; i++) {
                if (!loc_mem_alloc_t(16u + rnd() % 8000u, 16, 1, LOCMEM_T_VB, 0x84ED8u)) break;
            }
            if (i < 3000) break;
        }
        CHECK(k < 60, "alter Weg laeuft voll (Runde %d)", k);
        CHECK(g_nlog_voll == 1 && g_nlog_stelle >= 1 && g_nlog_auf >= 1,
              "voll: einmal gemeldet mit Aufrufer-Statistik (voll=%u Stellen=%u AUF=%u)", g_nlog_voll, g_nlog_stelle,
              g_nlog_auf);
        printf("alter Weg: voll in Runde %d\n", k + 1);
        loc_mem_alloc(0x100000, 0x1000, 0);
        CHECK(g_nlog_voll == 1, "voll: zweite Meldung unterdrueckt (%u)", g_nlog_voll);
    }

    /* 7. Bereich sonst voll: Quarantaene vorzeitig leeren */
    reset(1u << 20, 1, 100000);
    for (i = 0; i < 64; i++) {
        a = loc_mem_alloc(0x8000, 0x1000, 0);
        CHECK(a != 0, "vorzeitig: Anlegen %d", i);
        loc_mem_free(a);
    }
    loc_mem_info(&in);
    CHECK(in.nforce >= 1 && in.nfull == 0, "vorzeitig geleert (%u), nie voll (%u)", in.nforce, in.nfull);

    /* 8. Contig gleicher Groesse nach der Quarantaene wieder an derselben Stelle */
    reset(16u << 20, 1, 10);
    a = loc_mem_alloc(0x3000, 0x1000, 0);
    b = loc_mem_alloc(0x1000, 0x1000, 0);
    loc_mem_free(a);
    g_now += 20;
    c = loc_mem_alloc(0x3000, 0x1000, 0);
    CHECK(c == a, "Contig gleiche Groesse wiederverwendet (0x%X / 0x%X)", c, a);
    CHECK(loc_mem_size(a) == 0x3000u, "Contig exakte Groesse (0x%X)", loc_mem_size(a));
    (void)b;

    loc_mem_init_test(0, 0);
    printf("loc_mem_selftest: %d OK, %d FEHLER\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
