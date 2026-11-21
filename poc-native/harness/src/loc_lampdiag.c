/* DIAG(W405 LOC-LAMPS) loc_lampdiag: Cockpit-Lampen in Line of Contact beobachten (nur lesend).
 *
 * LoC schaltet die Cockpit-Lampen per CPU im Linear-Atlas (512x256, A8R8G8B8): Die Spielfunktionen
 * FUN_0006FE20 (Alpha eines Rechtecks: 0x40 dunkel, 0xFF an, 0x18/0x20 Grundhelligkeit) und FUN_0006FB80
 * (Rechteck aus einem Lampenbild kopieren) schreiben ueber den bei Missionsbeginn gesperrten Zeiger
 * [0x3906F4] (Pitch [0x3906F0], FUN_0006D850). Die Rechtecke stehen je Cockpit in einer Tabelle
 * ([0x390CD8 + Spieler*0x374], 16 Byte je Platz: x0,y0,x1,y1); Spieler = [0x3071B0].
 *
 * Schluessel `loc_lampdiag` (Standard 0 = aus): 1 = je praesentiertem Bild das Texel in der Mitte jedes
 * Platzes lesen und Aenderungen als `LAMPD` loggen (einmal die Rechtecktabelle `LAMPD rect`);
 * 2 = zusaetzlich das Flagwort (+0) der ersten 128 Cockpit-Knoten ([0x2F70AC], Basis [0x2F75D0], 0x60 Byte je
 * Knoten) und die Spieler-VT-Woerter +0xBC/+0x4DC/+0x5E8. Liest nur Gastspeicher, schreibt nichts,
 * aendert weder Spielzustand noch Bild; Lauf wird nicht langsamer (ein paar hundert Lesezugriffe je Bild). */
#include <windows.h>
#include <stdio.h>
#include "hcfg.h"
#include "hlog.h"
#include "sb_profile.h"

#define LD_SLOTS 0x94u
#define LD_NODES 128u
#define LD_MAXLOG 6000u

static unsigned s_px[LD_SLOTS];
static unsigned char s_have[LD_SLOTS];
static unsigned short s_node[LD_NODES];
static unsigned char s_nhave[LD_NODES];
static unsigned s_vt[3];
static int s_vthave;
static unsigned s_tbl, s_bits, s_nlog;

static int ld_rd(unsigned a, unsigned *v)
{
    if (a < 0x10000u) { return 0; }
    __try { *v = *(const volatile unsigned *)(uintptr_t)a; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}
static int ld_rd16(unsigned a, unsigned short *v)
{
    if (a < 0x10000u) { return 0; }
    __try { *v = *(const volatile unsigned short *)(uintptr_t)a; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}

static void ld_log_rects(unsigned tbl)
{
    unsigned k, j;
    char line[512];
    for (k = 0; k < LD_SLOTS; k += 8u) {
        int n = _snprintf_s(line, sizeof line, _TRUNCATE, "LAMPD rect %02X:", k);
        for (j = k; j < k + 8u && j < LD_SLOTS; j++) {
            unsigned r[4] = { 0, 0, 0, 0 }, q;
            for (q = 0; q < 4u; q++) { ld_rd(tbl + j * 16u + q * 4u, &r[q]); }
            if (n > 0 && n < (int)sizeof line) {
                n += _snprintf_s(line + n, sizeof line - (size_t)n, _TRUNCATE, " %u,%u-%u,%u", r[0], r[1], r[2], r[3]);
            }
        }
        HLOG_I("%s", line);
    }
}

void loc_lampdiag_frame(unsigned frame)
{
    unsigned p = 0, tbl = 0, bits = 0, pitch = 0, k;
    if (g_cfg.loc_lampdiag <= 0 || !g_sba_loc || s_nlog >= LD_MAXLOG) { return; }
    if (!ld_rd(0x3071B0u, &p) || p > 7u || !ld_rd(0x390CD8u + p * 0x374u, &tbl) || !ld_rd(0x3906F4u, &bits) ||
        !ld_rd(0x3906F0u, &pitch) || !tbl || !bits || pitch < 2048u || pitch > 8192u) {
        return;
    }
    if (tbl != s_tbl || bits != s_bits) {
        HLOG_I("LAMPD: Tabelle 0x%X Atlas 0x%X Pitch %u Spieler %u f=%u (DIAG W405)", tbl, bits, pitch, p, frame);
        ld_log_rects(tbl);
        s_tbl = tbl; s_bits = bits;
        memset(s_have, 0, sizeof s_have);
    }
    for (k = 0; k < LD_SLOTS; k++) {
        unsigned r[4], q, ok = 1, v;
        for (q = 0; q < 4u; q++) { ok &= (unsigned)ld_rd(tbl + k * 16u + q * 4u, &r[q]); }
        if (!ok || r[2] <= r[0] || r[3] <= r[1] || r[2] > 512u || r[3] > 256u) { continue; }
        if (!ld_rd(bits + ((r[1] + r[3]) / 2u) * pitch + ((r[0] + r[2]) / 2u) * 4u, &v)) { continue; }
        if (!s_have[k]) { s_have[k] = 1; s_px[k] = v; continue; }
        if (v != s_px[k]) {
            if (s_nlog++ < LD_MAXLOG) {
                HLOG_I("LAMPD slot %02X f=%u a=%02X->%02X px=%08X->%08X rect=%u,%u-%u,%u", k, frame, s_px[k] >> 24,
                       v >> 24, s_px[k], v, r[0], r[1], r[2], r[3]);
            }
            s_px[k] = v;
        }
    }
    if (g_cfg.loc_lampdiag >= 2) {
        unsigned nb = 0, vt = 0, w[3];
        unsigned short base = 0, f;
        if (ld_rd(0x2F70ACu, &nb) && ld_rd16(0x2F75D0u, &base) && nb) {
            for (k = 0; k < LD_NODES; k++) {
                if (!ld_rd16(nb + ((unsigned)base + k) * 0x60u, &f)) { continue; }
                if (!s_nhave[k]) { s_nhave[k] = 1; s_node[k] = f; continue; }
                if (f != s_node[k]) {
                    if (s_nlog++ < LD_MAXLOG) { HLOG_I("LAMPD node %u f=%u %04X->%04X", k, frame, s_node[k], f); }
                    s_node[k] = f;
                }
            }
        }
        if (ld_rd(0x5C47D0u, &vt) && vt && ld_rd(vt + 0xBCu, &w[0]) && ld_rd(vt + 0x4DCu, &w[1]) && ld_rd(vt + 0x5E8u, &w[2])) {
            if (!s_vthave || w[0] != s_vt[0] || w[2] != s_vt[2]) {
                if (s_nlog++ < LD_MAXLOG) {
                    HLOG_I("LAMPD vt f=%u bc=%08X 4dc=%08X 5e8=%08X", frame, w[0], w[1], w[2]);
                }
                s_vt[0] = w[0]; s_vt[1] = w[1]; s_vt[2] = w[2]; s_vthave = 1;
            }
        }
    }
}
