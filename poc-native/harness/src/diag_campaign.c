/* diag_campaign.c - DIAG(CAMPAIGN) (W63): Missionsziel-/Kampagnen-Zustand.
 * Missionsablauf (Disasm W63, s. docs/SPIELZUSTAENDE.md §26):
 *   Init  [0x2B3270+mno*4] (M1: 0x4A820) zaehlt die Ziel-Entities
 *         (+0x4A Bit 0 gesetzt, Bit 10 frei) -> Zielzaehler [0x356E74];
 *   Tod   [0x2B3340+mno*4] (M1: 0x4A900, aus FUN_0001E190) -> Zaehler--;
 *   Frame [0x2B3410+mno*4] (M1: 0x4A880, aus FUN_0004CAF0) -> Zaehler 0
 *         und Phase [0x66ABF4]==5 -> FUN_0002C280(0) (Erfolg, [0x356DD0]=1)
 *         + FUN_0002C250(0xD6) (Missionsende, fc8=1).
 *   Tutorial-Regie M1: FUN_0005F4C0, Flagwort [0x4CA0D4] (Zaehler
 *         [0x4CA0D0..E0], Ereignisbits [0x538318]).
 * CAMP-Zeile im state_every-Takt (wall-clock), CAMPOBJ bei jeder Aenderung
 * der Ziel-Liste (Index/Typ/HP), CAMPSTEP bei jeder Aenderung von Phase/
 * Zaehler/Ergebnis/Szene/Modus (frame-genau, logdrop-tolerant: Werte stehen
 * auch in der naechsten CAMP-Zeile). Alle Reads SEH-geschuetzt.
 * Schalter: [log] campaign_diag=0 (DIAG, Default aus). */
#include "diag_campaign.h"
#include "hlog.h"
#include "hcfg.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <windows.h>

typedef uint32_t u32;

#define CPG_MNO_ADDR 0x3B89A0u   /* u16, Missionsnummer (0 = M1) */
#define CPG_OBJ_ADDR 0x356E74u   /* s32, Zielzaehler (M1: 0x4A820/0x4A900) */
#define CPG_PH_ADDR 0x66ABF4u    /* u16, Missionsphase (5 = laeuft, 0 = Erfolg, 3/4 = Fehlschlag) */
#define CPG_RES_ADDR 0x356DD0u   /* u32, Ergebnis (1 = Erfolg, 2 = Fehlschlag; 2C280) */
#define CPG_FC8_ADDR 0x356FC8u   /* u32, Missionsende-Flag (2C250) */
#define CPG_RSN_ADDR 0x35236Cu   /* u32, Ende-Grund (0xD5 VT tot/Zeit, 0xD6 Ziel) */
#define CPG_S88_ADDR 0x356F88u   /* u32, Story-Film-Index (M1-Erfolg setzt 1) */
#define CPG_CDC_ADDR 0x3B8CDCu   /* u32, Sonderweiche im M1-Frame-Check (==1 -> ohne Film) */
#define CPG_TUT_ADDR 0x4CA0D4u   /* u32, Tutorial-Flagwort (5F4C0) */
#define CPG_TC0_ADDR 0x4CA0D0u   /* u32, Tutorial-Framezaehler (Bit 7 bei 150) */
#define CPG_TD8_ADDR 0x4CA0D8u   /* u32, Zaehler nach [0x538318]&0x20 */
#define CPG_TDC_ADDR 0x4CA0DCu   /* u32, Zaehler nach [0x538318]&0x40 */
#define CPG_TE0_ADDR 0x4CA0E0u   /* u32, Zaehler nach Bewegung/Treffer (>450 -> 0x3000) */
#define CPG_F318_ADDR 0x538318u  /* u32, Ereignisbits (0x20/0x40 -> Tutorial-Stufen) */
#define CPG_KILL_ADDR 0x356CA0u  /* u32, VT-Abschuesse (1E190) */
#define CPG_KALL_ADDR 0x356E78u  /* u32, Abschuesse gesamt (1E190) */
#define CPG_SCENE_ADDR 0x6B96DCu /* u32, Szene */
#define CPG_MODE_ADDR 0x2EEA34u  /* u32, Grobmodus */
#define CPG_MLOOP_ADDR 0x356F58u /* u32, Missionsschleife */
#define CPG_GATE_ADDR 0x669E24u  /* u32, Folge-Gate (0x30-Handler) */
#define CPG_LOCK_ADDR 0x686D44u  /* u32, Eingabesperre (0x30-Handler) */
#define CPG_TMR_ADDR 0x3563F0u   /* u32, Missions-Countdown */
#define CPG_N_ADDR 0x352288u     /* u32, Entity-Anzahl */
#define CPG_TAB_ADDR 0x351828u   /* u32, Entity-Tabelle (Stride 0x5C) */
#define CPG_OURS_ADDR 0x356E2Cu  /* u16, unser VT-Index */
#define CPG_PROG_ADDR 0x69D5DCu  /* u32, Spielstand-Fortschritt (user+0x34) */
#define CPG_SEL_ADDR 0x69D5E0u   /* u16, Spielstand-Missionswahl (user+0x38) */

static u32 cp_rd(u32 a)
{
    u32 v = 0xFFFFFFFFu;
    __try {
        v = *(volatile u32 *)(uintptr_t)a;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v = 0xFFFFFFFEu;
    }
    return v;
}

static u32 cp_rd16(u32 a)
{
    u32 v = 0xFFFFu;
    __try {
        v = *(volatile uint16_t *)(uintptr_t)a;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v = 0xFFFEu;
    }
    return v;
}

/* Ziel-Liste wie 0x4A820: +0x4A Bit 0 gesetzt, Bit 10 frei (max 16). */
static void cp_objlist(unsigned frame, char *buf, size_t cap, u32 *hash)
{
    u32 n = cp_rd(CPG_N_ADDR), tab = cp_rd(CPG_TAB_ADDR), i, h = 2166136261u;
    size_t k = 0;
    int cnt = 0;
    buf[0] = 0;
    if (!tab || tab >= 0xFFFFFFF0u || n > 100000u) {
        *hash = 0;
        return;
    }
    __try {
        for (i = 0; i < n; i++) {
            u32 b = tab + i * 0x5Cu;
            uint16_t f4a = *(volatile uint16_t *)(uintptr_t)(b + 0x4A);
            uint16_t hp, ty;
            float x, z;
            if (!(f4a & 1u) || (f4a & 0x400u)) continue;
            hp = *(volatile uint16_t *)(uintptr_t)b;
            ty = *(volatile uint16_t *)(uintptr_t)(b + 2);
            x = *(volatile float *)(uintptr_t)(b + 0x0C);
            z = *(volatile float *)(uintptr_t)(b + 0x14);
            h = (h ^ i) * 16777619u;
            h = (h ^ hp) * 16777619u;
            if (cnt < 16 && k + 48 < cap) {
                k += (size_t)_snprintf_s(buf + k, cap - k, _TRUNCATE,
                                         " %u:%X:%u:%.0f,%.0f:%04X", i, ty, hp,
                                         (double)x, (double)z, f4a);
            }
            cnt++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    (void)frame;
    *hash = h ^ (u32)cnt;
}

void diag_campaign_tick(unsigned frame)
{
    static DWORD cp_last_ms, cp_t0;
    static int cp_have_t0;
    static u32 cp_last_hash, cp_last_key[8];
    DWORD now, iv;
    u32 key[8];
    char ol[1024];
    u32 h;
    int i, chg = 0;
    if (!g_cfg.campaign_diag) {
        return;
    }
    /* frame-genau: Phase/Zaehler/Ergebnis/Szene/Modus/mloop/fc8/Film */
    key[0] = cp_rd16(CPG_PH_ADDR);
    key[1] = cp_rd(CPG_OBJ_ADDR);
    key[2] = cp_rd(CPG_RES_ADDR);
    key[3] = cp_rd(CPG_SCENE_ADDR);
    key[4] = cp_rd(CPG_MODE_ADDR);
    key[5] = cp_rd(CPG_MLOOP_ADDR);
    key[6] = cp_rd(CPG_FC8_ADDR);
    key[7] = cp_rd(CPG_S88_ADDR);
    for (i = 0; i < 8; i++) {
        if (key[i] != cp_last_key[i]) chg = 1;
    }
    if (chg) {
        memcpy(cp_last_key, key, sizeof key);
        HLOG_I("CAMPSTEP: f=%u ph=%u obj=%d res=%u scene=0x%X mode=%u mloop=%u "
               "fc8=%u s88=%u rsn=0x%X mno=%u",
               frame, key[0], (int)key[1], key[2], key[3], key[4], key[5],
               key[6], key[7], cp_rd(CPG_RSN_ADDR), cp_rd16(CPG_MNO_ADDR));
    }
    now = GetTickCount();
    if (!cp_have_t0) {
        cp_have_t0 = 1;
        cp_t0 = now;
    }
    iv = g_cfg.state_every > 0 ? (DWORD)g_cfg.state_every : 1000u;
    if (cp_last_ms && now - cp_last_ms < iv) {
        return;
    }
    cp_last_ms = now;
    /* Ziel-Liste nur in der Mission (Szene 0x2C, mloop=1): ausserhalb ist
     * die Entity-Tabelle freigegeben/umgewidmet (Lesen liefert Muell). */
    if (key[3] == 0x2Cu && key[5] == 1u) {
        cp_objlist(frame, ol, sizeof ol, &h);
    } else {
        ol[0] = 0;
        h = 0;
    }
    if (h != cp_last_hash) {
        cp_last_hash = h;
        HLOG_I("CAMPOBJ: f=%u obj=%d liste(idx:typ:hp:x,z:f4a)=%s", frame,
               (int)cp_rd(CPG_OBJ_ADDR), ol[0] ? ol : " -");
    }
    HLOG_I("CAMP: f=%u t=%.1f mno=%u obj=%d ph=%u res=%u fc8=%u rsn=0x%X s88=%u "
           "cdc=%u tut=0x%X tc=%u t8=%u tdc=%u te0=%u f318=0x%X kvt=%u kall=%u "
           "scene=0x%X mode=%u mloop=%u gate=0x%X lock=%u tmr=%u ours=%u "
           "prog=%u sel=%u",
           frame, (double)(now - cp_t0) / 1000.0, cp_rd16(CPG_MNO_ADDR),
           (int)cp_rd(CPG_OBJ_ADDR), cp_rd16(CPG_PH_ADDR), cp_rd(CPG_RES_ADDR),
           cp_rd(CPG_FC8_ADDR), cp_rd(CPG_RSN_ADDR), cp_rd(CPG_S88_ADDR),
           cp_rd(CPG_CDC_ADDR), cp_rd(CPG_TUT_ADDR), cp_rd(CPG_TC0_ADDR),
           cp_rd(CPG_TD8_ADDR), cp_rd(CPG_TDC_ADDR), cp_rd(CPG_TE0_ADDR),
           cp_rd(CPG_F318_ADDR), cp_rd(CPG_KILL_ADDR), cp_rd(CPG_KALL_ADDR),
           cp_rd(CPG_SCENE_ADDR), cp_rd(CPG_MODE_ADDR), cp_rd(CPG_MLOOP_ADDR),
           cp_rd(CPG_GATE_ADDR), cp_rd(CPG_LOCK_ADDR), cp_rd(CPG_TMR_ADDR),
           cp_rd16(CPG_OURS_ADDR), cp_rd(CPG_PROG_ADDR), cp_rd16(CPG_SEL_ADDR));
}
