/* loc_toggle.c - W451 LOC-TOGGLE-SYNC: toggle_assist (W87) fuer Line of Contact (GitHub #87).
 *
 * Befund: Die Tasten 1-5 rasten die Kippschalter bi34..bi38 ein (key_toggles, W79). Nach einem Eject (oder Respawn,
 * Missionsneustart) steht die Rast noch auf AN. Die Schalterpruefung des neuen VT (FUN_00072FE0) setzt jeden schon
 * eingeschalteten Schalter auf Zustand 4: Er blinkt rot (Zeichner FUN_00073420, Blinkbyte Cockpit+0x274+i,
 * FUN_00073260) und zaehlt erst, wenn er AUS und wieder AN geht (4 -> 0 -> 1 -> 3). Der W87-Assistent liest
 * SB-Adressen ([0x51D8B4], [0x51D8B5..]) und wirkt in LoC nicht.
 *
 * LoC-Gegenstuecke (Ghidra, nur gelesen): Cockpit je Spieler [0x390CA0 + Spieler*0x374] (FUN_00072FB0 liest
 * [0x390EC4 + (Spieler*0xDD + i)*4] == 3), eigener Spieler [0x3071B0], VT-Zeiger Cockpit+0x370 (VT+0x92 = Spieler);
 * Cockpit+0x214 = 1 Startpruefung aktiv (FUN_000791A0/FUN_0007BE50 rufen FUN_000733E0), +0x220 Stufe (0 Init
 * FUN_00072FE0, 1 Schalter FUN_000732A0, 2 Abschluss FUN_00073390), +0x224+4i Zustand je Schalter. Die Schalter
 * kommen aus dem Eingabesatz (FUN_000B0940: Index 0x28..0x2B/0x36, Modus 0 -> VT+0xBC Bits 0x10..0x100).
 *
 * Wirkung (wie W87): Ein Druck auf einen Schalter in Zustand 4 legt ihn neu um - die Rast bleibt AN, das Bit geht
 * AUS, bis das Spiel den Schalter als "aus" fuehrt, dann wieder AN (imap_loctog_step). Geschrieben wird nur der
 * eigene SBC-Block, aus dem das Spiel den uebertragenen Eingabesatz bildet; Lockstep bzw. Rollback verteilen ihn
 * an alle Seiten, kein Spielzustand wird geschrieben. Beim Neurechnen (Rollback) bedient loc_rollback.c die
 * SBC-Abfrage aus der Aufzeichnung, dieser Weg laeuft dann nicht.
 *
 * DIAG: input_diag=1 mit Testfolge (loc_replay_tick/loc_steps) -> Zeile LOCTOG bei jeder Aenderung von Stufe oder
 * Schalterzustaenden (Spielwerte, keine Eingaben). */
#include "loc_toggle.h"
#include "hcfg.h"
#include "hlog.h"

#include <windows.h>
#include <stdint.h>
#include <string.h>

typedef uint32_t u32;

#define LTG_PLAYER 0x3071B0u
#define LTG_COCKPIT0 0x390CA0u
#define LTG_CSTRIDE 0x374u

static imap_loctog_t s_tog;
static imap_loctog_view_t s_last;
static int s_last_ok;

static int ltg_read(imap_loctog_view_t *v)
{
    u32 pl, cp, vt;
    int i;
    memset(v, 0, sizeof *v);
    __try {
        pl = *(volatile const u32 *)(uintptr_t)LTG_PLAYER;
        if (pl >= 8u) return 0;
        cp = LTG_COCKPIT0 + pl * LTG_CSTRIDE;
        vt = *(volatile const u32 *)(uintptr_t)(cp + 0x370u);
        if (vt < 0x10000u || vt >= 0x80000000u) return 0;
        if (*(volatile const uint16_t *)(uintptr_t)(vt + 0x92u) != (uint16_t)pl) return 0;
        v->phase = *(volatile const int *)(uintptr_t)(cp + 0x214u);
        v->sub = *(volatile const int *)(uintptr_t)(cp + 0x220u);
        for (i = 0; i < 5; i++) v->st[i] = *(volatile const int *)(uintptr_t)(cp + 0x224u + 4u * (u32)i);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(v, 0, sizeof *v);
        return 0;
    }
    v->valid = 1;
    return 1;
}

void loc_toggle_poll(imap_block_t *blk, uint8_t *latch, unsigned chg, unsigned frame)
{
    static const char *const tn[5] = { "FILT CONTROL", "OXYGEN SUPPLY", "FUEL FLOW", "BUFFER MATERIAL", "VT-LOCATION" };
    imap_loctog_view_t v;
    unsigned refl;
    int diag = g_cfg.input_diag && (g_cfg.loc_replay_tick[0] || g_cfg.loc_steps[0]); /* DIAG nur mit Testfolge */
    int i;
    if (!chg && !s_tog.pulse && !diag) return; /* nichts zu tun: kein Speicherzugriff */
    ltg_read(&v);
    if (diag && (!s_last_ok || memcmp(&v, &s_last, sizeof v) != 0)) {
        HLOG_I("LOCTOG: gueltig=%d pruefung=%d stufe=%d sw=%d%d%d%d%d rast=%u%u%u%u%u f=%u (DIAG W451)", v.valid, v.phase,
               v.sub, v.st[0], v.st[1], v.st[2], v.st[3], v.st[4], *latch & 1u, (*latch >> 1) & 1u, (*latch >> 2) & 1u,
               (*latch >> 3) & 1u, (*latch >> 4) & 1u, frame);
        s_last = v;
        s_last_ok = 1;
    }
    refl = imap_loctog_step(&s_tog, &v, latch, chg, &blk->buttons[2]);
    for (i = 0; i < 5; i++) {
        if (refl & (1u << i)) {
            HLOG_I("INPUT: Kippschalter %d (bi%d %s) war schon AN -> neu umgelegt (AUS bis das Spiel ihn als aus "
                   "fuehrt, dann AN; LoC-Schalterpruefung sw=%d) bei f=%u", i + 1, 34 + i, tn[i], v.st[i], frame);
        }
    }
}
