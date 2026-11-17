/* loc_rb_selftest.c - W330 LOC-ROLLBACK-R1: Selbsttest des Zustandsrings (src/loc_rb_ring.c) ohne Spiel.
 *
 * Synthetischer Speicher wie im LoC-Client: ein Write-Watch-Bereich (NtAlloc im LoC-Profil), ein
 * normaler Bereich (Schreibschutz-Verfolgung), eine Pagefile-Section mit zwei Ansichten (LoC-Contig mit
 * Spiegel), ein ausfuehrbarer Bereich (XBE-Abbild, RWX). Je Tick zufaellige Schreibzugriffe (auch ueber
 * den Spiegel, aus einem zweiten Thread, mit gleichem Wert, per ReadFile nach rbr_prewrite), dann
 * Sichern. Pruefungen: Schatten = Live nach jedem Sichern, Laden von k Ticks = exakt der Stand von
 * damals (Vollkopien als Referenz), Ring-Tiefe, Ueberlauf, Schutz nach rbr_stop aufgehoben.
 * Exit 0 = alles gut. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loc_rb_ring.h"

#define PG 4096u
#define NA 64u  /* Seiten Write-Watch */
#define NB 64u  /* Seiten Schreibschutz */
#define NC 32u  /* Seiten Section (2 Ansichten) */
#define ND 16u  /* Seiten RWX */
#define DEPTH 12u
#define HIST 64u

static int g_checks, g_fail;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FEHLER: " __VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *A, *B, *C1, *C2, *D;
static uint32_t g_rng = 12345u;
static uint32_t rnd(void)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

/* Schreiber-Thread: schreibt auf Anforderung in B (Fremd-Schreiber, verfolgt ueber den VEH) */
static volatile LONG g_req, g_done;
static volatile uint32_t g_req_off, g_req_val;
static DWORD WINAPI writer(LPVOID p)
{
    (void)p;
    for (;;) {
        LONG r = g_req;
        if (r < 0) return 0;
        if (r > g_done) {
            *(volatile uint32_t *)(B + g_req_off) = g_req_val;
            InterlockedExchange(&g_done, r);
        }
        SwitchToThread();
    }
}
static void foreign_write(uint32_t off, uint32_t val)
{
    LONG r;
    g_req_off = off;
    g_req_val = val;
    r = InterlockedIncrement(&g_req);
    while (g_done < r) SwitchToThread();
}

static void random_writes(unsigned tick, HANDLE fh, int big)
{
    unsigned n = 1u + rnd() % 40u, i;
    if (big) { /* alle Seiten aendern: groesser als der Ring */
        for (i = 0; i < NA; i++) A[i * PG + 9]++;
        for (i = 0; i < NB; i++) B[i * PG + 9]++;
        for (i = 0; i < NC; i++) C1[i * PG + 9]++;
        for (i = 0; i < ND; i++) D[i * PG + 9]++;
        return;
    }
    for (i = 0; i < n; i++) {
        uint32_t r = rnd() % 100u, v = rnd();
        if (r < 25) A[(rnd() % NA) * PG + (rnd() % PG)] = (uint8_t)v;
        else if (r < 45) *(uint32_t *)(B + (rnd() % NB) * PG + (rnd() % (PG / 4)) * 4) = v;
        else if (r < 55) C1[(rnd() % NC) * PG + (rnd() % PG)] = (uint8_t)v;
        else if (r < 65) C2[(rnd() % NC) * PG + (rnd() % PG)] = (uint8_t)v; /* ueber den Spiegel */
        else if (r < 72) D[(rnd() % ND) * PG + (rnd() % PG)] = (uint8_t)v;
        else if (r < 80) { /* gleicher Wert: Seite gemeldet, aber nicht geaendert */
            uint8_t *p = B + (rnd() % NB) * PG + (rnd() % PG);
            *p = *p;
        } else if (r < 86) {
            foreign_write((rnd() % NB) * PG + (rnd() % (PG / 4)) * 4, v);
        } else if (r < 90 && fh) { /* Kernel-Schreibzugriff: ReadFile in geschuetzte Seiten */
            DWORD got = 0;
            uint32_t off = (rnd() % (NB - 2)) * PG + 100u, len = 5000u;
            LARGE_INTEGER z;
            z.QuadPart = (LONGLONG)(rnd() % 1000u);
            SetFilePointerEx(fh, z, NULL, FILE_BEGIN);
            rbr_prewrite((uintptr_t)(B + off), len);
            CHECK(ReadFile(fh, B + off, len, &got, NULL) && got == len, "ReadFile nach rbr_prewrite (Tick %u, GLE %lu)",
                  tick, GetLastError());
        } else if (r < 95) { /* memset ueber mehrere Seiten (rep stos) */
            uint32_t pg = rnd() % (NA - 3);
            memset(A + pg * PG + 1000, (int)(v & 0xFF), 3 * PG);
        } else {
            uint32_t pg = rnd() % (NC - 2);
            memset(C1 + pg * PG + 2000, (int)(v & 0xFF), PG + 10);
        }
    }
    (void)tick;
}

int main(void)
{
    HANDLE sec, th, fh = NULL;
    uint32_t alias, tot, hist_t[HIST], loads = 0, hot_max = 0, cooled = 0, k_hist[9] = {0}, saved_max = 0, us_max = 0, lus_max = 0;
    uint8_t *ref[HIST];
    unsigned tick, nh = 0, i;
    char tmp[MAX_PATH], tmpf[MAX_PATH];
    rbr_commit_t c;
    rbr_stat_t st;

    A = (uint8_t *)VirtualAlloc(NULL, NA * PG, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    B = (uint8_t *)VirtualAlloc(NULL, NB * PG, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    sec = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_COMMIT, 0, NC * PG, NULL);
    C1 = sec ? (uint8_t *)MapViewOfFile(sec, FILE_MAP_ALL_ACCESS, 0, 0, NC * PG) : NULL;
    C2 = sec ? (uint8_t *)MapViewOfFile(sec, FILE_MAP_ALL_ACCESS, 0, 0, NC * PG) : NULL;
    D = (uint8_t *)VirtualAlloc(NULL, ND * PG, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!A || !B || !C1 || !C2 || !D) {
        printf("loc_rb_selftest: Speicher fehlt\n");
        return 2;
    }
    for (i = 0; i < NA * PG; i++) A[i] = (uint8_t)(i * 7);
    for (i = 0; i < NB * PG; i++) B[i] = (uint8_t)(i * 13);
    for (i = 0; i < NC * PG; i++) C1[i] = (uint8_t)(i * 3);
    memset(D, 0xC3, ND * PG);
    /* Datei fuer den Kernel-Schreibtest */
    GetTempPathA(sizeof tmp, tmp);
    GetTempFileNameA(tmp, "rbr", 0, tmpf);
    fh = CreateFileA(tmpf, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (fh != INVALID_HANDLE_VALUE) {
        static uint8_t buf[8192];
        DWORD w;
        for (i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i * 31 + 5);
        WriteFile(fh, buf, sizeof buf, &w, NULL);
    } else {
        fh = NULL;
    }
    th = CreateThread(NULL, 0, writer, NULL, 0, NULL);

    CHECK(rbr_init(DEPTH, 160) == 0, "rbr_init");
    rbr_set_hot(2, 8); /* heisse Seiten frueh abkuehlen lassen (Test) */
    CHECK(rbr_add((uint32_t)(uintptr_t)A, NA * PG, 1, NULL, 0, 2) == RBR_WW, "Bereich A soll Write-Watch nutzen");
    CHECK(rbr_add((uint32_t)(uintptr_t)B, NB * PG, 1, NULL, 0, 2) == RBR_WP, "Bereich B ohne MEM_WRITE_WATCH -> Schreibschutz");
    alias = (uint32_t)(uintptr_t)C2;
    CHECK(rbr_add((uint32_t)(uintptr_t)C1, NC * PG, 0, &alias, 1, 1) == RBR_WP, "Bereich C (Section + Spiegel)");
    CHECK(rbr_add((uint32_t)(uintptr_t)D, ND * PG, 0, NULL, 0, 0) == RBR_WP, "Bereich D (RWX)");
    tot = rbr_pages();
    CHECK(tot == NA + NB + NC + ND, "Seiten gesamt %u", tot);
    CHECK(rbr_start() == 0, "rbr_start");
    rbr_set_owner(GetCurrentThreadId());
    for (i = 0; i < HIST; i++) {
        ref[i] = (uint8_t *)malloc((size_t)tot * PG);
        if (!ref[i]) return 2;
    }
    rbr_full_copy(ref[0]);
    hist_t[0] = 0;
    nh = 1;

    for (tick = 1; tick <= 600; tick++) {
        int big = (tick == 300);
        random_writes(tick, fh, big);
        if (tick < 400) { /* heisse Seiten: B[1] aendert sich jeden Tick, C1[2] (ueber den Spiegel) gleicher Wert */
            B[1 * PG + 17]++;
            C2[2 * PG + 5] = C2[2 * PG + 5];
            if (tick > 250 && tick < 270) foreign_write(1 * PG + 40, tick); /* fremder Thread auf heisser Seite */
        }
        CHECK(rbr_commit(&c) == 0, "rbr_commit");
        if (c.hot > hot_max) hot_max = c.hot;
        cooled += c.cooled;
        if (c.saved > saved_max) saved_max = c.saved;
        if (c.us_total > us_max) us_max = c.us_total;
        CHECK(rbr_shadow_diff(NULL, 0) == 0, "Tick %u: Schatten != Live nach dem Sichern", tick);
        if (big) {
            CHECK(c.overflow == 1 && rbr_depth() == 0, "Tick %u: Ueberlauf erwartet (gesichert %u, Tiefe %u)", tick,
                  c.saved, rbr_depth());
            nh = 0; /* Geschichte ungueltig */
        } else {
            CHECK(rbr_depth() <= DEPTH, "Tiefe %u > %u", rbr_depth(), DEPTH);
        }
        /* Referenz merken (Ringpuffer der letzten HIST Staende) */
        if (nh == HIST) {
            uint8_t *t0 = ref[0];
            memmove(ref, ref + 1, (HIST - 1) * sizeof ref[0]);
            memmove(hist_t, hist_t + 1, (HIST - 1) * sizeof hist_t[0]);
            ref[HIST - 1] = t0;
            nh--;
        }
        rbr_full_copy(ref[nh]);
        hist_t[nh] = tick;
        nh++;
        /* gelegentlich laden */
        if (tick % 7 == 0 && rbr_depth() > 0) {
            unsigned maxk = rbr_depth() < 8 ? rbr_depth() : 8, k = 1 + rnd() % maxk, d0 = rbr_depth();
            uint32_t pages = 0, us = 0, nd;
            uint32_t addr[4] = {0};
            if (k >= nh) k = nh - 1;
            if (k == 0) continue;
            CHECK(rbr_load(k, &pages, &us) == 0, "rbr_load(%u)", k);
            if (us > lus_max) lus_max = us;
            nd = rbr_full_diff(ref[nh - 1 - k], addr, 4);
            CHECK(nd == 0, "Tick %u: Laden k=%u -> %u Seiten falsch (erste 0x%X), Stand von Tick %u erwartet", tick, k, nd,
                  addr[0], hist_t[nh - 1 - k]);
            CHECK(rbr_depth() == d0 - k, "Tiefe nach Laden %u (erwartet %u)", rbr_depth(), d0 - k);
            CHECK(rbr_shadow_diff(NULL, 0) == 0, "Schatten != Live nach dem Laden");
            nh -= k;
            loads++;
            k_hist[k]++;
            /* weiterrechnen ab dem geladenen Stand: naechstes Sichern darf nichts Altes zuruecklassen */
        }
        if (tick % 50 == 0) { /* ganzen Ring abbauen: tiefstes Laden */
            unsigned k = rbr_depth();
            if (k && k < nh) {
                uint32_t pages = 0, us = 0;
                CHECK(rbr_load(k, &pages, &us) == 0, "rbr_load(tief %u)", k);
                CHECK(rbr_full_diff(ref[nh - 1 - k], NULL, 0) == 0, "Tick %u: tiefes Laden k=%u falsch", tick, k);
                nh -= k;
                loads++;
            }
        }
    }
    CHECK(hot_max > 0 && cooled > 0, "heisse Seiten (max %u je Tick) und Abkuehlen (%u) erwartet", hot_max, cooled);
    /* Laden ohne genug Ticks */
    CHECK(rbr_load(rbr_depth() + 1, NULL, NULL) == -1, "Laden ueber die Tiefe muss scheitern");
    rbr_stats(&st);
    CHECK(st.faults > 0 && st.faults_alias > 0 && st.faults_foreign > 0, "Fehlerzaehler (gesamt %u, Spiegel %u, fremd %u)",
          st.faults, st.faults_alias, st.faults_foreign);
    CHECK(!fh || st.prewrites > 0, "rbr_prewrite gezaehlt");
    {
        rbr_writer_t w[4];
        unsigned nw = rbr_writers(w, 4);
        CHECK(nw == 1, "ein fremder Schreiber erwartet (%u)", nw);
    }
    rbr_stop();
    { /* Schutz aufgehoben: Schreiben ohne VEH darf nicht fehlschlagen */
        int ok = 1;
        __try {
            B[5] = 1;
            C2[7] = 2;
            D[9] = 3;
        } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
        CHECK(ok, "nach rbr_stop noch geschuetzt");
    }
    InterlockedExchange(&g_req, -1);
    WaitForSingleObject(th, 2000);
    if (fh) CloseHandle(fh);
    printf("loc_rb_selftest: Ladevorgaenge %u (k=1..8: %u %u %u %u %u %u %u %u), Fehler gesamt %u (Spiegel %u, fremd %u, "
           "prewrite %u), max %u Seiten je Tick, heiss max %u, abgekuehlt %u, Sichern max %u us, Laden max %u us\n",
           loads, k_hist[1], k_hist[2], k_hist[3], k_hist[4], k_hist[5], k_hist[6], k_hist[7], k_hist[8], st.faults,
           st.faults_alias, st.faults_foreign, st.prewrites, saved_max, hot_max, cooled, us_max, lus_max);
    printf("loc_rb_selftest: %d Pruefungen, %d Fehler -> %s\n", g_checks, g_fail, g_fail ? "FEHLER" : "OK");
    return g_fail ? 1 : 0;
}
