/* diag_input.c - DIAG(INPUT): Eingabezustand pro Swap ueberwachen (A8).
 * Abgetastet wird (aktiver Port po = [0x352274]&3, wie OUTWRITE):
 * - Eingabe-Zweigflags u32 [0x3B8CC0] (1 = 0x4A232-Zweig),
 *   [0x3B8CE0] (Analog-Tail), [0x670E9C] (Tasten-ODER vs. Dekodierung)
 * - Mapper-Out-Basis u32 [0x4AF9D0] (FUN_00049AE0-Arg, phasenvariant!)
 * - SBC-Block (24 B) an 0x4AEEFC+po*0x1C (Hook-Ausgabe: Buttons, Analog)
 * - Out-Kopf (24 B) an 0x4AF480+po*0x154 (OUTWRITE-Ziel, fest)
 * - Missions-Buttonflags (je 8 B) 0x3B8CB8 und 0x3B8CE8 (0x4A232-Dekoder)
 * - W44: Out+0x2A8-Cluster (16 B, 0x4AF728), G6-Latches (8 B, 0x4AF76C),
 * - SBC Port 0 (24 B) und Mapper-Out-Kopf (24 B, Basis+po*0x154) zum Vergleich
 * Loggt bei JEDER Aenderung und alle 600 Frames einen Heartbeat.
 * Schalter: [input] input_diag=1. Aufruf: eine Zeile im Swap-Hook. */
#include "diag_input.h"
#include "sb_profile.h" /* W229 SBA() */
#include "hlog.h"
#include "hcfg.h"
#include <stdint.h>
#include <string.h>
#include <windows.h>

typedef uint32_t u32;

#define IN_PORT_ADDR SBA(0x352274u)
#define IN_CC0_ADDR SBA(0x3B8CC0u)
#define IN_CE0_ADDR SBA(0x3B8CE0u)
#define IN_ANY_ADDR SBA(0x670E9Cu)
#define IN_MOUT_ADDR SBA(0x4AF9D0u)
#define IN_LATCH_ADDR SBA(0x351518u) /* Mapper-Gang-Latch (P3-Copy, Menue+Mission) */
#define IN_SBC_BASE SBA(0x4AEEFCu)
#define IN_OUT_BASE SBA(0x4AF480u)
#define IN_FLG1_ADDR SBA(0x3B8CB8u)
#define IN_FLG2_ADDR SBA(0x3B8CE8u)
#define IN_HEARTBEAT 600u

/* Blob: port,cc0,ce0,any,mout(je u32) + sbc,out,flg16,sbc0,mout(24/24/16/24/24) */
#define IN_BLOB (5 * 4 + 24 + 24 + 16 + 24 + 24 + 24 + 4 + 16 + 8 + 16 + 2 + 4 + 4 + 4 + 4)
#define IN_OFF_MOUT 16
#define IN_OFF_SBC 20
#define IN_OFF_OUT 44
#define IN_OFF_FLG 68
#define IN_OFF_SBC0 84
#define IN_OFF_MOUTH 108
#define IN_OFF_OUT0 132
#define IN_OFF_LATCH 156
/* W44 IGNITION: Zuendpfad-Bytes (fixe Adressen, port-unabhaengig) */
#define IN_OFF_OUT2 160   /* Out+0x2A8-Cluster, 16 B (Schalter/IGN/START/HATCH) */
#define IN_OFF_LAT 176    /* G6-Latches, 8 B ab 0x4AF76C (START/IGN/HATCH) */
#define IN_OFF_SH2 184    /* Shadow+0x2A8-Cluster, 16 B (Mapper-Ziel?) */
#define IN_OFF_ENG 200    /* Engine-Flag u16 0x3516DC */
#define IN_OFF_GATE 202   /* Gate u32 0x6A37E0 (Bit 2 = Engine-Toggle) */
#define IN_OFF_DSP 206    /* Dispatch u32 0x354F3C (3 = Engine-Latch) */
#define IN_OFF_CKP 210    /* Cockpit u32 0x3520DC (1 = Welt-Pfad) */
#define IN_OFF_LCK 214   /* W52: Lock u32 0x53831C (1 = Start verriegelt, Toggle-scharf) */
#define IN_GATE_ADDR SBA(0x6A37E0u)
#define IN_DSP_ADDR SBA(0x354F3Cu)
#define IN_CKP_ADDR SBA(0x3520DCu)
#define IN_LCK_ADDR SBA(0x53831Cu)
#define IN_OUT2_ADDR SBA(0x4AF728u)
#define IN_LAT_ADDR SBA(0x4AF76Cu)
#define IN_SH2_ADDR SBA(0x4AFC80u)
#define IN_ENG_ADDR SBA(0x3516DCu)

static uint8_t in_last[IN_BLOB];
static int in_have_last;

static u32 in_read_u32(u32 addr)
{
    u32 v = 0xFFFFFFFFu;
    __try {
        v = *(volatile u32 *)(uintptr_t)addr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v = 0xFFFFFFFEu; /* (noch) nicht lesbar */
    }
    return v;
}

static void in_read_blob(uint8_t *d, u32 addr, unsigned n)
{
    __try {
        memcpy(d, (const void *)(uintptr_t)addr, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(d, 0xEE, n);
    }
}

static void in_hex(char *d, const uint8_t *s, unsigned n)
{
    static const char h[] = "0123456789ABCDEF";
    unsigned i;
    for (i = 0; i < n; i++) {
        d[2 * i] = h[s[i] >> 4];
        d[2 * i + 1] = h[s[i] & 15];
    }
    d[2 * n] = '\0';
}

void diag_input_tick(unsigned frame)
{
    uint8_t cur[IN_BLOB];
    u32 port, mout;
    char sbc[49], out[49], flg[33], sbc0[49], mhead[49], out0[49];
    char out2[33], lat[17], sh2[33];
    if (!g_cfg.input_diag) {
        return;
    }
    port = in_read_u32(IN_PORT_ADDR) & 3u;
    mout = in_read_u32(IN_MOUT_ADDR);
    memcpy(cur, &port, 4);
    {
        u32 cc0 = in_read_u32(IN_CC0_ADDR);
        u32 ce0 = in_read_u32(IN_CE0_ADDR);
        u32 any = in_read_u32(IN_ANY_ADDR);
        memcpy(cur + 4, &cc0, 4);
        memcpy(cur + 8, &ce0, 4);
        memcpy(cur + 12, &any, 4);
    }
    memcpy(cur + IN_OFF_MOUT, &mout, 4);
    in_read_blob(cur + IN_OFF_SBC, IN_SBC_BASE + port * 0x1Cu, 24);
    in_read_blob(cur + IN_OFF_OUT, IN_OUT_BASE + port * 0x154u, 24);
    in_read_blob(cur + IN_OFF_FLG, IN_FLG1_ADDR, 8);
    in_read_blob(cur + IN_OFF_FLG + 8, IN_FLG2_ADDR, 8);
    in_read_blob(cur + IN_OFF_SBC0, IN_SBC_BASE, 24);
    if (mout >= 0x10000u && mout < 0x80000000u) {
        in_read_blob(cur + IN_OFF_MOUTH, mout + port * 0x154u, 24);
        in_read_blob(cur + IN_OFF_OUT0, mout, 24); /* Mapper schreibt ohne Port-Stride! */
    } else {
        memset(cur + IN_OFF_MOUTH, 0xEE, 24);
        memset(cur + IN_OFF_OUT0, 0xEE, 24);
    }
    {
        u32 latch = in_read_u32(IN_LATCH_ADDR);
        memcpy(cur + IN_OFF_LATCH, &latch, 4);
    }
    in_read_blob(cur + IN_OFF_OUT2, IN_OUT2_ADDR, 16);
    in_read_blob(cur + IN_OFF_LAT, IN_LAT_ADDR, 8);
    in_read_blob(cur + IN_OFF_SH2, IN_SH2_ADDR, 16);
    in_read_blob(cur + IN_OFF_ENG, IN_ENG_ADDR, 2);
    { u32 gate = in_read_u32(IN_GATE_ADDR); memcpy(cur + IN_OFF_GATE, &gate, 4); }
    { u32 dsp = in_read_u32(IN_DSP_ADDR); memcpy(cur + IN_OFF_DSP, &dsp, 4); }
    { u32 ckp = in_read_u32(IN_CKP_ADDR); memcpy(cur + IN_OFF_CKP, &ckp, 4); }
    { u32 lck = in_read_u32(IN_LCK_ADDR); memcpy(cur + IN_OFF_LCK, &lck, 4); }
    if (!in_have_last || memcmp(cur, in_last, sizeof cur) != 0) {
        u32 cc0, ce0, any;
        memcpy(in_last, cur, sizeof cur);
        in_have_last = 1;
        memcpy(&cc0, cur + 4, 4);
        memcpy(&ce0, cur + 8, 4);
        memcpy(&any, cur + 12, 4);
        in_hex(sbc, cur + IN_OFF_SBC, 24);
        in_hex(out, cur + IN_OFF_OUT, 24);
        in_hex(flg, cur + IN_OFF_FLG, 16);
        in_hex(sbc0, cur + IN_OFF_SBC0, 24);
        in_hex(mhead, cur + IN_OFF_MOUTH, 24);
        in_hex(out0, cur + IN_OFF_OUT0, 24);
        in_hex(out2, cur + IN_OFF_OUT2, 16);
        in_hex(lat, cur + IN_OFF_LAT, 8);
        in_hex(sh2, cur + IN_OFF_SH2, 16);
        {
            u32 latch, eng;
            memcpy(&latch, cur + IN_OFF_LATCH, 4);
            memcpy(&eng, cur + IN_OFF_ENG, 2);
            { u32 gate, dsp, ckp, lck;
              memcpy(&gate, cur + IN_OFF_GATE, 4);
              memcpy(&dsp, cur + IN_OFF_DSP, 4);
              memcpy(&ckp, cur + IN_OFF_CKP, 4);
              memcpy(&lck, cur + IN_OFF_LCK, 4);
            HLOG_I("INDIAG: f=%u po=%u cc0=%u ce0=%u any=%u mout=0x%X sbc=%s "
                   "out=%s flg=%s sbc0=%s mhead=%s out0=%s latch=%u (chg) "
                   "out2=%s lat=%s sh2=%s eng=%u gate=0x%X dsp=%u ckp=%u lck=%u",
                   frame, port, cc0, ce0, any, mout, sbc, out, flg, sbc0,
                   mhead, out0, latch, out2, lat, sh2, eng & 0xFFFFu,
                   gate, dsp, ckp, lck); }
        }
    } else if ((frame % IN_HEARTBEAT) == 0) {
        u32 cc0, ce0, any;
        memcpy(&cc0, cur + 4, 4);
        memcpy(&ce0, cur + 8, 4);
        memcpy(&any, cur + 12, 4);
        in_hex(sbc, cur + IN_OFF_SBC, 24);
        in_hex(out, cur + IN_OFF_OUT, 24);
        in_hex(flg, cur + IN_OFF_FLG, 16);
        in_hex(sbc0, cur + IN_OFF_SBC0, 24);
        in_hex(mhead, cur + IN_OFF_MOUTH, 24);
        in_hex(out0, cur + IN_OFF_OUT0, 24);
        in_hex(out2, cur + IN_OFF_OUT2, 16);
        in_hex(lat, cur + IN_OFF_LAT, 8);
        in_hex(sh2, cur + IN_OFF_SH2, 16);
        {
            u32 latch, eng;
            memcpy(&latch, cur + IN_OFF_LATCH, 4);
            memcpy(&eng, cur + IN_OFF_ENG, 2);
            { u32 gate, dsp, ckp, lck;
              memcpy(&gate, cur + IN_OFF_GATE, 4);
              memcpy(&dsp, cur + IN_OFF_DSP, 4);
              memcpy(&ckp, cur + IN_OFF_CKP, 4);
              memcpy(&lck, cur + IN_OFF_LCK, 4);
            HLOG_I("INDIAG: f=%u po=%u cc0=%u ce0=%u any=%u mout=0x%X sbc=%s "
                   "out=%s flg=%s sbc0=%s mhead=%s out0=%s latch=%u "
                   "out2=%s lat=%s sh2=%s eng=%u gate=0x%X dsp=%u ckp=%u lck=%u",
                   frame, port, cc0, ce0, any, mout, sbc, out, flg, sbc0,
                   mhead, out0, latch, out2, lat, sh2, eng & 0xFFFFu,
                   gate, dsp, ckp, lck); }
        }
    }
}
