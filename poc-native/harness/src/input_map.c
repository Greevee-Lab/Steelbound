/* input_map.c - Eingabe-Profile: Quelle -> Controller-Element (W76 CTRL-MAP).
 * Doku: poc-native/docs/EINGABE.md. Harness-unabhaengig (nur C + Win32). */
#include "input_map.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void imap_state_init(imap_state_t *st)
{
    memset(st, 0, sizeof *st);
    st->gear = 8; /* N (7=R, 8=N, 9..13 = 1..5), wie hooks_input.c */
    st->m_ax = 0x7F;
    st->m_ay = 0x7F;
    st->m_step_frame = 0xFFFFFFFFu;
}

static void imap_btn(imap_block_t *s, int idx)
{
    s->buttons[idx / 16] |= (uint16_t)(1u << (idx % 16));
}

/* ---- Maus (1:1 aus hooks_input.c:mouse_apply uebernommen) ----
 * Relativ -> Aiming-/Sight-Akku, Tasten -> bi0/bi1/bi2. Echte Maus nur
 * ohne Replay; synthetische VKs 320-326 sind Replay-Inhalt (BUTTONMAP.md).
 * Polaritaet: rechts/runter = aufwaerts (0xFF/+127), wie Pfeiltasten. */
static void imap_mouse_accu_add_raw(imap_state_t *st,
                                    const imap_snap_t *snap, int dx, int dy)
{
    if (snap->mouse_invert_x) dx = -dx;
    if (snap->mouse_invert_y) dy = -dy;
    if (snap->mouse_sight) {
        st->m_sx += dx;
        st->m_sy += dy;
        if (st->m_sx > 127) st->m_sx = 127;
        if (st->m_sx < -127) st->m_sx = -127;
        if (st->m_sy > 127) st->m_sy = 127;
        if (st->m_sy < -127) st->m_sy = -127;
    } else {
        st->m_ax += dx;
        st->m_ay += dy;
        if (st->m_ax > 0xFF) st->m_ax = 0xFF;
        if (st->m_ax < 0) st->m_ax = 0;
        if (st->m_ay > 0xFF) st->m_ay = 0xFF;
        if (st->m_ay < 0) st->m_ay = 0;
    }
}

/* W87 mouse_abs: Verstaerkung sens/64 Hebelstufen je Maus-Count. Eine
 * Stufe = 0,015 rad = ~9 px auf dem 512-px-Hauptmonitor, Mitte -> Rand
 * ~25 Stufen (W87_rel): sens 4 = 400 Counts, sens 1 = 1600, sens 16 = 100.
 * (Relativ bleibt sens/4 ganzzahlig, bitgenau.) Bei grossen Deltas wird
 * pro Poll weiter auf +-64 Counts begrenzt (wie relativ). */
#define IMAP_ABS_DIV 64.0f

/* W87 mouse_abs: Aiming-Akku feinstufig (Bruchteile bleiben erhalten,
 * langsame Mausbewegungen gehen nicht im /4 verloren). Sight: wie alt. */
static void imap_mouse_accu_add_f(imap_state_t *st, const imap_snap_t *snap,
                                  float dx, float dy)
{
    float vx, vy;
    if (snap->mouse_sight) {
        imap_mouse_accu_add_raw(st, snap,
                                (int)(dx < 0 ? dx - 0.5f : dx + 0.5f),
                                (int)(dy < 0 ? dy - 0.5f : dy + 0.5f));
        return;
    }
    if (snap->mouse_invert_x) dx = -dx;
    if (snap->mouse_invert_y) dy = -dy;
    vx = (float)st->m_ax + st->m_fx + dx;
    vy = (float)st->m_ay + st->m_fy + dy;
    if (vx > 255.0f) vx = 255.0f;
    if (vx < 0.0f) vx = 0.0f;
    if (vy > 255.0f) vy = 255.0f;
    if (vy < 0.0f) vy = 0.0f;
    st->m_ax = (int)(vx + 0.5f);
    st->m_ay = (int)(vy + 0.5f);
    st->m_fx = vx - (float)st->m_ax;
    st->m_fy = vy - (float)st->m_ay;
}

/* Echte Maustaste: W79 input_source=1 -> snap->real_vk (Fenster-
 * Nachrichten), sonst GetAsyncKeyState (bisher, bitgenau). */
static int imap_real_btn(const imap_snap_t *snap, int vk)
{
    if (snap->real_vk) return snap->real_vk(vk) != 0;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

/* Akku-Update (muss EINMAL pro Poll laufen, vor der Achsen-Auswertung).
 * Gibt die Maus-Tasten zurueck (L/R/M + W110-Seitentasten X1/X2). */
static void imap_mouse_update(imap_state_t *st, const imap_snap_t *snap,
                              int *lb, int *rb, int *mb, int *x1, int *x2)
{
    *lb = *rb = *mb = *x1 = *x2 = 0;
    if (snap->replay_active) {
        if (snap->mouse_aim && snap->frame != st->m_step_frame) {
            int sdx = 0, sdy = 0;
            st->m_step_frame = snap->frame;
            if (snap->key(320, snap->ctx)) sdx += 1;
            if (snap->key(321, snap->ctx)) sdx -= 1;
            if (snap->key(322, snap->ctx)) sdy += 1;
            if (snap->key(323, snap->ctx)) sdy -= 1;
            if (sdx || sdy)
                imap_mouse_accu_add_raw(st, snap, sdx * 4, sdy * 4);
        }
        if (snap->mouse_buttons) {
            if (snap->key(324, snap->ctx)) *lb = 1;
            if (snap->key(325, snap->ctx)) *rb = 1;
            if (snap->key(326, snap->ctx)) *mb = 1;
            if (snap->key(327, snap->ctx)) *x1 = 1; /* W110: X1/X2 */
            if (snap->key(328, snap->ctx)) *x2 = 1;
        }
    } else if (snap->real_blocked) {
        /* W79: kein Fokus / Maus freigegeben -> echte Maus traegt nichts
         * bei; Cursor-Bezug nachfuehren (kein Sprung bei Rueckkehr). */
        POINT pt;
        if (!snap->mouse_raw && GetCursorPos(&pt)) {
            st->m_last_x = (int)pt.x;
            st->m_last_y = (int)pt.y;
            st->m_have_last = 1;
        }
    } else if (snap->mouse_raw) {
        /* W79 mouse_capture: Raw-Input-Deltas (keine Randsaettigung). */
        if (snap->mouse_aim) {
            int sens = snap->mouse_sens;
            int dx = snap->raw_dx, dy = snap->raw_dy;
            if (sens < 0) sens = 0;
            if (sens > 16) sens = 16;
            if (dx > 64) dx = 64;
            if (dx < -64) dx = -64;
            if (dy > 64) dy = 64;
            if (dy < -64) dy = -64;
            if ((dx || dy) && snap->mouse_abs)
                imap_mouse_accu_add_f(st, snap,
                                      (float)(dx * sens) / IMAP_ABS_DIV,
                                      (float)(dy * sens) / IMAP_ABS_DIV);
            else if (dx || dy)
                imap_mouse_accu_add_raw(st, snap, dx * sens / 4,
                                        dy * sens / 4);
        }
        if (snap->mouse_buttons) {
            if (imap_real_btn(snap, VK_LBUTTON)) *lb = 1;
            if (imap_real_btn(snap, VK_RBUTTON)) *rb = 1;
            if (imap_real_btn(snap, VK_MBUTTON)) *mb = 1;
            if (imap_real_btn(snap, VK_XBUTTON1)) *x1 = 1;
            if (imap_real_btn(snap, VK_XBUTTON2)) *x2 = 1;
        }
    } else {
        if (snap->mouse_aim) {
            POINT pt;
            if (GetCursorPos(&pt)) {
                if (st->m_have_last) {
                    int sens = snap->mouse_sens;
                    int dx = (int)pt.x - st->m_last_x;
                    int dy = (int)pt.y - st->m_last_y;
                    if (sens < 0) sens = 0;
                    if (sens > 16) sens = 16;
                    if (dx > 64) dx = 64;
                    if (dx < -64) dx = -64;
                    if (dy > 64) dy = 64;
                    if (dy < -64) dy = -64;
                    if ((dx || dy) && snap->mouse_abs)
                        imap_mouse_accu_add_f(st, snap,
                                              (float)(dx * sens) / IMAP_ABS_DIV,
                                              (float)(dy * sens) / IMAP_ABS_DIV);
                    else if (dx || dy)
                        imap_mouse_accu_add_raw(st, snap, dx * sens / 4,
                                                dy * sens / 4);
                }
                st->m_last_x = (int)pt.x;
                st->m_last_y = (int)pt.y;
                st->m_have_last = 1;
            }
        }
        if (snap->mouse_buttons) {
            if (imap_real_btn(snap, VK_LBUTTON)) *lb = 1;
            if (imap_real_btn(snap, VK_RBUTTON)) *rb = 1;
            if (imap_real_btn(snap, VK_MBUTTON)) *mb = 1;
            if (imap_real_btn(snap, VK_XBUTTON1)) *x1 = 1;
            if (imap_real_btn(snap, VK_XBUTTON2)) *x2 = 1;
        }
    }
}

/* W79 INPUT-REAL: Kippschalter bi34..bi38 als Rastschalter (s. Header). */
unsigned imap_toggles_apply(imap_block_t *blk, imap_state_t *st,
                            const imap_snap_t *snap)
{
    unsigned changed = 0;
    int i;
    for (i = 0; i < 5; i++) {
        int bi = 34 + i;
        uint16_t bit = (uint16_t)(1u << (bi % 16));
        int sig = (blk->buttons[bi / 16] & bit) != 0;
        if (snap->key('1' + i, snap->ctx)) sig = 1;
        blk->buttons[bi / 16] &= (uint16_t)~bit;
        if (sig && !(st->tog_prev & (1u << i))) {
            st->tog_latch ^= (uint8_t)(1u << i);
            changed |= 1u << i;
        }
        if (sig) st->tog_prev |= (uint8_t)(1u << i);
        else st->tog_prev &= (uint8_t)~(1u << i);
        if (st->tog_latch & (1u << i)) blk->buttons[bi / 16] |= bit;
    }
    return changed;
}

/* ---- Legacy-Abbildung (1:1 aus hook_XInputGetState extrahiert) ----
 * Reihenfolge und Semantik unveraendert; nur Globals -> imap_state_t und
 * key_down/g_cfg -> Snapshot. */
void imap_legacy_apply(imap_block_t *blk, imap_state_t *st,
                       const imap_snap_t *snap)
{
    int ign, hatch, qup, qdn, tup, tdn;
    int lb, rb, mb, x1, x2;
    imap_key_fn key_down = snap->key;
    void *kctx = snap->ctx;

    memset(blk, 0, sizeof *blk);
    blk->aiming_x = 0x7F;
    blk->aiming_y = 0x7F;

    if (key_down(VK_RETURN, kctx)) imap_btn(blk, 6);
    if (key_down(VK_SPACE, kctx)) imap_btn(blk, 0);
    if (key_down('F', kctx)) imap_btn(blk, 1);
    if (key_down('L', kctx)) imap_btn(blk, 2);
    if (key_down(VK_TAB, kctx)) imap_btn(blk, 9);
    if (key_down('M', kctx)) imap_btn(blk, 8);
    if (key_down('Z', kctx)) imap_btn(blk, 11);
    if (key_down('X', kctx)) imap_btn(blk, 12);
    if (key_down('C', kctx)) imap_btn(blk, 18);
    if (key_down('O', kctx)) imap_btn(blk, 20);
    if (key_down('N', kctx)) imap_btn(blk, 21);
    if (key_down('R', kctx)) imap_btn(blk, 27);
    if (key_down('V', kctx)) imap_btn(blk, 13);
    {
        int bi;
        for (bi = 0; bi < 39; bi++) {
            if (key_down(256 + bi, kctx)) imap_btn(blk, bi);
        }
    }
    ign = key_down('I', kctx);
    hatch = key_down('H', kctx);
    if (snap->sbc_momentary) {
        if (ign) blk->buttons[0] |= (1u << 5);
        if (hatch) blk->buttons[0] |= (1u << 4);
    } else {
        if (ign && !st->prev_ign) st->toggle ^= (1u << 5);
        if (hatch && !st->prev_hatch) st->toggle ^= (1u << 4);
        blk->buttons[0] |= st->toggle;
    }
    st->prev_ign = ign;
    st->prev_hatch = hatch;
    if (!key_down(VK_SHIFT, kctx)) {
        if (key_down(VK_LEFT, kctx)) blk->sight_x = -127;
        else if (key_down(VK_RIGHT, kctx)) blk->sight_x = 127;
        if (key_down(VK_UP, kctx)) blk->sight_y = -127;
        else if (key_down(VK_DOWN, kctx)) blk->sight_y = 127;
    }
    if (key_down(VK_SHIFT, kctx)) {
        if (key_down(VK_LEFT, kctx)) blk->sight_x = -127;
        else if (key_down(VK_RIGHT, kctx)) blk->sight_x = 127;
        if (key_down(VK_UP, kctx)) blk->sight_y = -127;
        else if (key_down(VK_DOWN, kctx)) blk->sight_y = 127;
    } else {
        if (key_down(VK_LEFT, kctx)) blk->aiming_x = 0x00;
        else if (key_down(VK_RIGHT, kctx)) blk->aiming_x = 0xFF;
        if (key_down(VK_UP, kctx)) blk->aiming_y = 0x00;
        else if (key_down(VK_DOWN, kctx)) blk->aiming_y = 0xFF;
    }
    st->m_aim_src = 0;
    imap_mouse_update(st, snap, &lb, &rb, &mb, &x1, &x2);
    if (lb) imap_btn(blk, 0);
    if (rb) imap_btn(blk, 1);
    if (mb) imap_btn(blk, 2);
    if (x1 || x2) blk->pedal_l = 0xFF; /* W110: Slide-Step (Seitentasten) */
    if (snap->mouse_aim) {
        int arrow_x = key_down(VK_LEFT, kctx) || key_down(VK_RIGHT, kctx);
        int arrow_y = key_down(VK_UP, kctx) || key_down(VK_DOWN, kctx);
        if (snap->mouse_sight) {
            if (!arrow_x) blk->sight_x = (int8_t)st->m_sx;
            if (!arrow_y) blk->sight_y = (int8_t)st->m_sy;
        } else {
            if (!arrow_x) blk->aiming_x = (uint8_t)st->m_ax;
            if (!arrow_y) blk->aiming_y = (uint8_t)st->m_ay;
            st->m_aim_src = (uint8_t)((arrow_x ? 0 : 1) | (arrow_y ? 0 : 2));
        }
    }
    if (key_down('A', kctx)) blk->rot_lever = -127;
    else if (key_down('D', kctx)) blk->rot_lever = 127;
    if (key_down('W', kctx)) blk->pedal_r = 0xFF;
    if (key_down('S', kctx)) blk->pedal_m = 0xFF;
    /* W110: Slide-Step (linkes Pedal) auf linker Strg - mit A/D kombinierbar
     * (linke Hand: kleiner Finger Strg + Mittel-/Zeigefinger A/D). Replays
     * druecken die Taste nie, Testlaeufe bleiben bitgleich. */
    if (key_down(VK_LCONTROL, kctx)) blk->pedal_l = 0xFF;
    qup = key_down('E', kctx);
    if (qup && !st->prev_qup && st->gear < 13) st->gear++;
    st->prev_qup = qup;
    qdn = key_down('Q', kctx);
    if (qdn && !st->prev_qdn && st->gear > 7) st->gear--;
    st->prev_qdn = qdn;
    tup = key_down('T', kctx);
    if (tup && !st->prev_tup && st->tuner < 12) st->tuner++;
    st->prev_tup = tup;
    tdn = key_down('G', kctx);
    if (tdn && !st->prev_tdn && st->tuner > 0) st->tuner--;
    st->prev_tdn = tdn;
    blk->gear = st->gear;
    blk->tuner = st->tuner;
}

/* ---- Profil-Auswertung ---- */

static int imap_atom_active(const imap_atom_t *a, const imap_snap_t *snap)
{
    switch (a->kind) {
    case IMAP_A_KEY:
        return snap->key(a->code, snap->ctx) != 0;
    case IMAP_A_MOUSEBTN:
        if (!snap->mouse_buttons) return 0;
        if (snap->replay_active)
            return snap->key(324 + a->code, snap->ctx) != 0;
        if (snap->real_blocked) return 0; /* W79: kein Fokus/freigegeben */
        {
            int vk = a->code == 0 ? VK_LBUTTON :
                     a->code == 1 ? VK_RBUTTON :
                     a->code == 2 ? VK_MBUTTON :
                     a->code == 3 ? VK_XBUTTON1 : VK_XBUTTON2;
            return imap_real_btn(snap, vk);
        }
    case IMAP_A_PADBTN:
    case IMAP_A_PADDPAD:
        if (!snap->pad_present) return 0;
        return (snap->pad.buttons & (uint16_t)a->code) != 0;
    case IMAP_A_PADTRIG:
        if (!snap->pad_present) return 0;
        return (a->code == 0 ? snap->pad.lt : snap->pad.rt) > 127;
    case IMAP_A_SBC: /* W159: Original-Controller-Taste bi0..38 */
        if (!snap->sbc_present) return 0;
        if (a->code < 0 || a->code >= 39) return 0;
        return ((snap->sbc[a->code / 16] >> (a->code % 16)) & 1u) != 0;
    }
    return 0;
}

static int imap_chord_has_back(const imap_chord_t *c)
{
    if ((c->a.kind == IMAP_A_PADBTN && c->a.code == IMAP_PAD_BACK) ||
        (c->chord && c->b.kind == IMAP_A_PADBTN &&
         c->b.code == IMAP_PAD_BACK))
        return 1;
    return 0;
}

static int imap_chord_uses_pad(const imap_chord_t *c)
{
    if (c->a.kind == IMAP_A_PADBTN || c->a.kind == IMAP_A_PADDPAD ||
        c->a.kind == IMAP_A_PADTRIG)
        return 1;
    if (c->chord && (c->b.kind == IMAP_A_PADBTN ||
                     c->b.kind == IMAP_A_PADDPAD ||
                     c->b.kind == IMAP_A_PADTRIG))
        return 1;
    return 0;
}

/* Eine Alternative ist aktiv, wenn Atome (+ Akkord-Partner) aktiv sind und
 * die SHIFT-Bedingung gilt. Shift-Ebene: ist BACK gehalten, gelten nur
 * Pad-Alternativen mit BACK (Basis-Funktionen schweigen, z. B. feuert
 * BACK+RB als Gang-hebung nicht die Hauptwaffe). Tastatur/Maus unbeeinflusst,
 * Achsen (Sticks/Trigger) bleiben live. */
static int imap_chord_active(const imap_chord_t *c, const imap_snap_t *snap,
                             int ignore_cond, int only_nonvirtual)
{
    int back_held, i;
    if (!imap_atom_active(&c->a, snap)) return 0;
    if (c->chord && !imap_atom_active(&c->b, snap)) return 0;
    if (!c->chord) {
        /* Basis-Bindung schweigt, wenn ihr Atom in einem aktiven Akkord
         * steckt (Akkord gewinnt, kein Doppelfeuer). */
        for (i = 0; i < snap->nsupp; i++) {
            if (snap->supp[i].kind == (int)c->a.kind &&
                snap->supp[i].code == c->a.code)
                return 0;
        }
    }
    if (only_nonvirtual) {
        if (c->a.kind == IMAP_A_KEY && c->a.code >= 256) return 0;
        if (c->chord && c->b.kind == IMAP_A_KEY && c->b.code >= 256)
            return 0;
    }
    back_held = snap->pad_present &&
                (snap->pad.buttons & IMAP_PAD_BACK) != 0;
    if (back_held && imap_chord_uses_pad(c) && !imap_chord_has_back(c))
        return 0;
    if (!ignore_cond && c->cond) {
        int sh = snap->key(VK_SHIFT, snap->ctx) != 0;
        if (c->cond == 1 && !sh) return 0;
        if (c->cond == 2 && sh) return 0;
    }
    return 1;
}

static int imap_bind_active(const imap_bind_t *b, const imap_snap_t *snap)
{
    int i;
    for (i = 0; i < b->n; i++) {
        if (imap_chord_active(&b->alt[i], snap, 0, 0)) return 1;
    }
    return 0;
}

/* Binde-Druck ohne SHIFT-Bedingung (Maus-Unterdrueckung wie bisher: Pfeile
 * schlagen die Maus auch MIT Shift, s. mouse_apply arrow_x). */
static int imap_bind_active_raw(const imap_bind_t *b, const imap_snap_t *snap)
{
    int i;
    for (i = 0; i < b->n; i++) {
        if (imap_chord_active(&b->alt[i], snap, 1, 0)) return 1;
    }
    return 0;
}

/* NORM: Stick -32768..32767 -> -1..1 (XInput, + = rechts/oben). */
static float imap_stick_norm(int16_t v)
{
    float f = v >= 0 ? (float)v / 32767.0f : (float)v / 32768.0f;
    if (f > 1.0f) f = 1.0f;
    if (f < -1.0f) f = -1.0f;
    return f;
}

/* Analogquelle -> -1..1 (Trigger: 0..1) mit Invertierung, Totzone
 * (reskaliert) und Kurve. */
static float imap_analog_value(const imap_analog_t *a,
                               const imap_snap_t *snap)
{
    float v;
    if (a->src < 0 || !snap->pad_present) return 0.0f;
    switch (a->src) {
    case 0: v = imap_stick_norm(snap->pad.lx); break;
    case 1: v = imap_stick_norm(snap->pad.ly); break;
    case 2: v = imap_stick_norm(snap->pad.rx); break;
    case 3: v = imap_stick_norm(snap->pad.ry); break;
    case 4: v = (float)snap->pad.lt / 255.0f; break;
    case 5: v = (float)snap->pad.rt / 255.0f; break;
    default: return 0.0f;
    }
    if (a->inv) v = -v;
    /* W143: globale Totzone als Untergrenze (max mit Profilwert). */
    {
        float fl = a->src <= 3 ? snap->pad_dz_stick : snap->pad_dz_trig;
        float dz = a->dz < fl ? fl : a->dz;
        if (a->src <= 3) {
            float av = v < 0 ? -v : v;
            if (av < dz) return 0.0f;
            v = (v < 0 ? -1.0f : 1.0f) * (av - dz) / (1.0f - dz);
            if (a->curve != 1.0f && a->curve > 0.0f) {
                float s = v < 0 ? -1.0f : 1.0f;
                float m = v < 0 ? -v : v;
                v = s * powf(m, a->curve);
            }
        } else {
            /* Trigger einseitig: Totzone schneidet nur ab. */
            if (v < dz) return 0.0f;
        }
    }
    return v;
}

/* Maus-Kandidat einer Achse in Ziel-Einheiten (muss idle 0 liefern).
 * Rueckgabe 1 = Maus traegt bei (Gatter + Modus ok). */
static int imap_mouse_candidate(imap_axis_t ax, const imap_axis_map_t *m,
                                imap_state_t *st, const imap_snap_t *snap,
                                int *out)
{
    if (!snap->mouse_aim || !m->mouse) return 0;
    if (m->mouse_sight >= 0 && m->mouse_sight != snap->mouse_sight)
        return 0;
    switch (ax) {
    case IMAP_AX_AIM_X:
        if (snap->mouse_sight || m->mouse != 1) return 0;
        *out = st->m_ax - 0x7F;
        return 1;
    case IMAP_AX_AIM_Y:
        if (snap->mouse_sight || m->mouse != 2) return 0;
        *out = st->m_ay - 0x7F;
        return 1;
    case IMAP_AX_SIGHT_X:
        if (!snap->mouse_sight || m->mouse != 1) return 0;
        *out = st->m_sx;
        return 1;
    case IMAP_AX_SIGHT_Y:
        if (!snap->mouse_sight || m->mouse != 2) return 0;
        *out = st->m_sy;
        return 1;
    default:
        return 0;
    }
}

static int imap_clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* W132 PAD-ARB: einachsige Arbitrierung Pad gegen Maus - exakt die
 * Zustandstabelle von imap_arb_axes (W128, "zuletzt aktives Geraet
 * gewinnt"), einachsig, in Offset-Einheiten (0 = neutral). mouse =
 * Maus-Kandidat, pad = Pad-Kandidat, mpos = Mausstellung in Hebelstufen
 * (fuer die Wake-Schwelle). dead: Pad-Bewegung > dead holt die Achse;
 * wake: Mausbewegung >= wake bei ruhendem Pad holt sie zurueck. */
int imap_arb_pad1(imap_arb1_t *a, int mouse, int pad, float mpos,
                  int dead, float wake, int *wism)
{
    float dm;
    if (!a) {
        if (wism) *wism = 1;
        return mouse;
    }
    if (!a->init) {
        a->owns = 0;
        a->ref = pad;
        a->last = pad;
        a->m_last = mpos;
        a->m_move = 0.0f;
        a->init = 1;
    }
    dm = (float)fabs((double)mpos - (double)a->m_last);
    a->m_last = mpos;
    if (!a->owns) {
        if (abs(pad - a->ref) > dead) {
            a->owns = 1;
            a->last = pad;
            a->m_move = 0.0f;
        }
    } else {
        if (abs(pad - a->last) > dead) {
            a->last = pad; /* Pad aktiv: Maus-Zittern zaehlt nicht */
            a->m_move = 0.0f;
        } else {
            a->m_move += dm;
            if (a->m_move >= wake) {
                a->owns = 0;
                a->ref = pad;
                a->m_move = 0.0f;
            }
        }
    }
    if (wism) *wism = !a->owns;
    return a->owns ? pad : mouse;
}

/* Zentrierte Achse (Aiming/Rotation/Sight): digital voll, Maus-Akku,
 * Pad-Analog; groesste Auslenkung gewinnt (Gleichstand: digital > Maus
 * > Pad). raw_dig unterdrueckt die Maus auch bei verfehlter SHIFT-
 * Bedingung (Legacy: Pfeile + Shift lassen Aiming unberuehrt).
 * W132 (pad_axis_arb=1): Hat die Achse Pad-Quellen, ist ein Pad da und
 * liefert die Maus, entscheiden Pad gegen Maus per "zuletzt aktives
 * Geraet gewinnt" (imap_arb_pad1); Digitalachsen bleiben wie bisher. */
static int imap_axis_centered(imap_axis_t ax, const imap_axis_map_t *m,
                              imap_state_t *st, const imap_snap_t *snap,
                              int full_neg, int full_pos, int *from_mouse)
{
    int best = 0, have = 0, i, mc = 0, moff, mvalid = 0, mwon = 0;
    int neg_p = imap_bind_active(&m->neg, snap);
    int pos_p = imap_bind_active(&m->pos, snap);
    int raw = imap_bind_active_raw(&m->neg, snap) ||
              imap_bind_active_raw(&m->pos, snap);
    int dhave = 0;   /* W132: Digitalauslenkung liegt an */
    int mcon = 0;    /* W132: Maus liefert auf dieser Achse */
    int po = 0;      /* W132: beste Pad-Auslenkung (0 = keine) */
    int pbound = 0;  /* W132: Achse hat Pad-Quellen */
    if (neg_p) {
        best = -full_neg;
        have = dhave = 1;
    } else if (pos_p) {
        best = full_pos;
        have = dhave = 1;
    }
    if (!raw && imap_mouse_candidate(ax, m, st, snap, &mc)) {
        mcon = 1;
        mvalid = !have; /* W87: Maus ist Quelle, auch wenn der Akku mittig steht */
        if (mc != 0) {
            moff = mc < 0 ? -mc : mc;
            if (!have || moff > (best < 0 ? -best : best)) {
                best = mc;
                have = 1;
                mwon = 1;
            }
        }
    }
    for (i = 0; i < 2; i++) {
        if (m->pad[i].src >= 0 && m->pad[i].src <= 3) {
            pbound = 1;
            break;
        }
    }
    if (snap->pad_present && pbound) {
        for (i = 0; i < 2; i++) {
            if (m->pad[i].src < 0 || m->pad[i].src > 3) continue;
            {
                float v = imap_analog_value(&m->pad[i], snap);
                int o = v >= 0 ? (int)(v * (float)full_pos + 0.5f)
                               : (int)(v * (float)full_neg - 0.5f);
                int ao = o < 0 ? -o : o;
                int ab = po < 0 ? -po : po;
                if (o != 0 && ao > ab) {
                    po = o;
                }
            }
        }
    }
    if (snap->pad_axis_arb && pbound && snap->pad_present && mcon) {
        /* W132: Pad gegen Maus per Arbitrierung statt Max-Abs. */
        int slot, w, wism = 0, wb, bb;
        if (ax == IMAP_AX_AIM_X) slot = 0;
        else if (ax == IMAP_AX_AIM_Y) slot = 1;
        else if (ax == IMAP_AX_SIGHT_X) slot = 2;
        else slot = 3; /* mcon => nur diese 4 Achsen (s. imap_mouse_candidate) */
        w = imap_arb_pad1(&st->pad_arb[slot], mc, po, (float)mc,
                          snap->pad_arb_dead, (float)snap->pad_mouse_wake,
                          &wism);
        wb = w < 0 ? -w : w;
        bb = best < 0 ? -best : best;
        if (!dhave || wb > bb) {
            /* Kein Digital: Gewinner uebernehmen. Mit Digital: weiter die
             * Max-Abs-Regel (voll bzw. Gleichstand digital gewinnt - alle
             * Kandidaten klemmen auf +-voll, digital verliert nie). */
            best = w;
            mwon = wism;
            mvalid = 0;
        } else {
            mwon = 0;
            mvalid = 0;
        }
    } else if (po != 0) {
        /* Alte Max-Abs-Regel (pad_axis_arb=0, kein Pad, keine Pad-Quelle
         * oder keine Maus auf der Achse): po ist das strikte Maximum
         * beider Pad-Quellen (erste gewinnt Gleichstand) wie bisher. */
        int ao = po < 0 ? -po : po;
        if (!have || ao > (best < 0 ? -best : best)) {
            best = po;
            have = 1;
            mwon = mvalid = 0;
        }
    }
    (void)have;
    if (from_mouse) *from_mouse = mwon || mvalid;
    return best;
}

/* Einseitige Achse (Pedal 0..255): Maximum aller Quellen. */
static int imap_axis_pedal(const imap_axis_map_t *m, const imap_snap_t *snap)
{
    int best = 0, i;
    if (imap_bind_active(&m->neg, snap) ||
        imap_bind_active(&m->pos, snap))
        best = 0xFF;
    if (snap->pad_present) {
        for (i = 0; i < 2; i++) {
            if (m->pad[i].src < 0) continue;
            {
                float v = imap_analog_value(&m->pad[i], snap);
                int o;
                if (m->pad[i].src <= 3) {
                    /* Stick auf Pedal: nur positive Haelfte. */
                    if (v < 0) v = 0;
                }
                o = (int)(v * 255.0f + 0.5f);
                if (o > best) best = o;
            }
        }
    }
    return imap_clampi(best, 0, 0xFF);
}

/* Aktive Akkorde des Profils sammeln -> deren Atome unterdruecken
 * reine Basis-Bindungen (s. imap_chord_active). Akkorde selbst werden
 * nie unterdrueckt (keine Rekursion). */
static void imap_collect_suppression(const imap_profile_t *prof,
                                     imap_snap_t *snap)
{
    int bi, ax, i, k;
    snap->nsupp = 0;
    for (bi = 0; bi < 39; bi++) {
        for (i = 0; i < prof->btn[bi].n; i++) {
            const imap_chord_t *c = &prof->btn[bi].alt[i];
            const imap_atom_t *at[2];
            if (!c->chord) continue;
            if (!imap_chord_active(c, snap, 0, 0)) continue;
            at[0] = &c->a;
            at[1] = &c->b;
            for (k = 0; k < 2; k++) {
                int d, dup = 0;
                for (d = 0; d < snap->nsupp; d++) {
                    if (snap->supp[d].kind == (int)at[k]->kind &&
                        snap->supp[d].code == at[k]->code) {
                        dup = 1;
                        break;
                    }
                }
                if (!dup && snap->nsupp < 16) {
                    snap->supp[snap->nsupp].kind = (int)at[k]->kind;
                    snap->supp[snap->nsupp].code = at[k]->code;
                    snap->nsupp++;
                }
            }
        }
    }
    /* Akkorde auf Achsen-Endlagen und Gang/Tuner ebenso. */
    for (ax = 0; ax < IMAP_AX_COUNT; ax++) {
        const imap_bind_t *bb[2] = { &prof->axis[ax].neg,
                                     &prof->axis[ax].pos };
        for (k = 0; k < 2; k++) {
            for (i = 0; i < bb[k]->n; i++) {
                const imap_chord_t *c = &bb[k]->alt[i];
                if (c->chord && imap_chord_active(c, snap, 0, 0) &&
                    snap->nsupp + 2 <= 16) {
                    snap->supp[snap->nsupp].kind = (int)c->a.kind;
                    snap->supp[snap->nsupp].code = c->a.code;
                    snap->nsupp++;
                    snap->supp[snap->nsupp].kind = (int)c->b.kind;
                    snap->supp[snap->nsupp].code = c->b.code;
                    snap->nsupp++;
                }
            }
        }
    }
    {
        const imap_bind_t *bb[4] = { &prof->gear_up, &prof->gear_down,
                                     &prof->tuner_up, &prof->tuner_down };
        for (k = 0; k < 4; k++) {
            for (i = 0; i < bb[k]->n; i++) {
                const imap_chord_t *c = &bb[k]->alt[i];
                if (c->chord && imap_chord_active(c, snap, 0, 0) &&
                    snap->nsupp + 2 <= 16) {
                    snap->supp[snap->nsupp].kind = (int)c->a.kind;
                    snap->supp[snap->nsupp].code = c->a.code;
                    snap->nsupp++;
                    snap->supp[snap->nsupp].kind = (int)c->b.kind;
                    snap->supp[snap->nsupp].code = c->b.code;
                    snap->nsupp++;
                }
            }
        }
    }
}

void imap_profile_apply(imap_block_t *blk, imap_state_t *st,
                        const imap_profile_t *prof, const imap_snap_t *snap)
{
    int bi, lb, rb, mb, x1, x2, o;
    int p4_full, p5_full, p4_edge, p5_edge, i;
    imap_snap_t loc;

    loc = *snap;
    imap_collect_suppression(prof, &loc);
    snap = &loc;

    memset(blk, 0, sizeof *blk);
    blk->aiming_x = 0x7F;
    blk->aiming_y = 0x7F;

    /* Tasten (ODER aller Quellen). */
    for (bi = 0; bi < 39; bi++) {
        if (bi == 4 || bi == 5) continue; /* I/H unten (Taster/Toggle) */
        if (imap_bind_active(&prof->btn[bi], snap)) imap_btn(blk, bi);
    }
    /* Replay-Virtuals 256+bi (Test-Infrastruktur, wie Legacy direkt). */
    for (bi = 0; bi < 39; bi++) {
        if (snap->key(256 + bi, snap->ctx)) imap_btn(blk, bi);
    }
    /* Luke/Zuendung: Taster (Moment) oder Rastschalter (Flanke). Die Flanke
     * sieht nur echte Bindungen (keine 256+bi-Virtuals, wie Legacy). */
    p4_full = imap_bind_active(&prof->btn[4], snap);
    p5_full = imap_bind_active(&prof->btn[5], snap);
    p4_edge = p5_edge = 0;
    for (i = 0; i < prof->btn[4].n; i++) {
        if (imap_chord_active(&prof->btn[4].alt[i], snap, 0, 1)) {
            p4_edge = 1;
            break;
        }
    }
    for (i = 0; i < prof->btn[5].n; i++) {
        if (imap_chord_active(&prof->btn[5].alt[i], snap, 0, 1)) {
            p5_edge = 1;
            break;
        }
    }
    if (snap->sbc_momentary) {
        if (p5_full) blk->buttons[0] |= (1u << 5);
        if (p4_full) blk->buttons[0] |= (1u << 4);
    } else {
        if (p5_edge && !st->prev_ign) st->toggle ^= (1u << 5);
        if (p4_edge && !st->prev_hatch) st->toggle ^= (1u << 4);
        blk->buttons[0] |= st->toggle;
    }
    st->prev_ign = p5_edge;
    st->prev_hatch = p4_edge;

    /* Maus-Akku (gleicher Code wie Legacy). Maustasten kommen im
     * Profil-Pfad AUSSCHLIESSLICH aus MOUSE:LEFT/RIGHT/MIDDLE-Bindungen
     * (frei belegbar); keyboard_default bindet sie wie Legacy auf
     * bi0/bi1/bi2. lb/rb/mb hier ignorieren. */
    imap_mouse_update(st, snap, &lb, &rb, &mb, &x1, &x2);
    (void)lb;
    (void)rb;
    (void)mb;
    (void)x1;
    (void)x2;

    /* Achsen. */
    {
        int fmx = 0, fmy = 0;
        o = imap_axis_centered(IMAP_AX_AIM_X, &prof->axis[IMAP_AX_AIM_X], st,
                               snap, 127, 128, &fmx);
        blk->aiming_x = (uint8_t)imap_clampi(0x7F + o, 0, 0xFF);
        o = imap_axis_centered(IMAP_AX_AIM_Y, &prof->axis[IMAP_AX_AIM_Y], st,
                               snap, 127, 128, &fmy);
        blk->aiming_y = (uint8_t)imap_clampi(0x7F + o, 0, 0xFF);
        st->m_aim_src = (uint8_t)((fmx ? 1 : 0) | (fmy ? 2 : 0));
    }
    o = imap_axis_centered(IMAP_AX_ROT, &prof->axis[IMAP_AX_ROT], st, snap,
                           127, 127, NULL);
    blk->rot_lever = (int8_t)imap_clampi(o, -127, 127);
    o = imap_axis_centered(IMAP_AX_SIGHT_X, &prof->axis[IMAP_AX_SIGHT_X], st,
                           snap, 127, 127, NULL);
    blk->sight_x = (int8_t)imap_clampi(o, -127, 127);
    o = imap_axis_centered(IMAP_AX_SIGHT_Y, &prof->axis[IMAP_AX_SIGHT_Y], st,
                           snap, 127, 127, NULL);
    blk->sight_y = (int8_t)imap_clampi(o, -127, 127);
    blk->pedal_l =
        (uint8_t)imap_axis_pedal(&prof->axis[IMAP_AX_PEDAL_L], snap);
    blk->pedal_m =
        (uint8_t)imap_axis_pedal(&prof->axis[IMAP_AX_PEDAL_M], snap);
    blk->pedal_r =
        (uint8_t)imap_axis_pedal(&prof->axis[IMAP_AX_PEDAL_R], snap);

    /* Gang/Tuner (Flanken auf ODER-Signal, Direktwahl gewinnt). */
    {
        int up = imap_bind_active(&prof->gear_up, snap);
        int dn = imap_bind_active(&prof->gear_down, snap);
        int tu = imap_bind_active(&prof->tuner_up, snap);
        int td = imap_bind_active(&prof->tuner_down, snap);
        if (up && !st->prev_qup && st->gear < 13) st->gear++;
        st->prev_qup = up;
        if (dn && !st->prev_qdn && st->gear > 7) st->gear--;
        st->prev_qdn = dn;
        if (tu && !st->prev_tup && st->tuner < 12) st->tuner++;
        st->prev_tup = tu;
        if (td && !st->prev_tdn && st->tuner > 0) st->tuner--;
        st->prev_tdn = td;
        for (i = 0; i < 7; i++) {
            if (imap_bind_active(&prof->gear_direct[i], snap)) {
                st->gear = (uint8_t)(7 + i);
                break;
            }
        }
    }
    blk->gear = st->gear;
    blk->tuner = st->tuner;
}

/* W159: Client-Aktion "vr_recenter" (VR: Blick zentrieren). Steigende Flanke
 * des Bindungs-Pegels (gleiche Semantik wie Tasten: ODER, Akkord-
 * Unterdrueckung, BACK-Shift-Ebene, cond). */
int imap_action_vr_recenter(const imap_profile_t *prof, imap_state_t *st,
                            const imap_snap_t *snap)
{
    imap_snap_t loc;
    int lvl, hit;
    if (prof->vr_recenter.n == 0) {
        st->prev_vr_recenter = 0;
        return 0;
    }
    loc = *snap;
    imap_collect_suppression(prof, &loc);
    lvl = imap_bind_active(&prof->vr_recenter, &loc) ? 1 : 0;
    hit = (lvl && !st->prev_vr_recenter) ? 1 : 0;
    st->prev_vr_recenter = lvl;
    return hit;
}

/* W188 GAME-MENU: Client-Aktion "pause" (nur Pad-/Controller-Quellen; Tasten
 * wertet das Fenster aus, s. input_map.h). */
static int imap_key_off(int vk, void *ctx)
{
    (void)vk;
    (void)ctx;
    return 0;
}

int imap_action_pause(const imap_profile_t *prof, imap_state_t *st,
                      const imap_snap_t *snap)
{
    imap_snap_t loc;
    int lvl, hit;
    if (prof->pause.n == 0) {
        st->prev_pause = 0;
        return 0;
    }
    loc = *snap;
    loc.key = imap_key_off;
    imap_collect_suppression(prof, &loc);
    lvl = imap_bind_active(&prof->pause, &loc) ? 1 : 0;
    hit = (lvl && !st->prev_pause) ? 1 : 0;
    st->prev_pause = lvl;
    return hit;
}

int imap_bind_keys(const imap_bind_t *b, int *vks, int cap, unsigned *padmask)
{
    int i, n = 0;
    if (padmask) *padmask = 0;
    for (i = 0; b && i < b->n; i++) {
        const imap_chord_t *c = &b->alt[i];
        if (c->chord) continue;
        if (c->a.kind == IMAP_A_KEY && c->a.code > 0 && c->a.code < 256 && n < cap) {
            vks[n++] = c->a.code;
        } else if ((c->a.kind == IMAP_A_PADBTN || c->a.kind == IMAP_A_PADDPAD) && padmask) {
            *padmask |= (unsigned)c->a.code;
        }
    }
    return n;
}

/* ---- Profil-Parser ---- */

static int imap_strieq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
    }
    return *a == *b;
}

static int imap_isspace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void imap_trim(char *s)
{
    char *e;
    while (*s && imap_isspace(*s)) memmove(s, s + 1, strlen(s));
    e = s + strlen(s);
    while (e > s && imap_isspace(e[-1])) *--e = '\0';
}

/* Tastenname -> VK (Buchstaben/Ziffern, Namen, Dezimal/Hex). -1 = unbekannt. */
static int imap_key_code(const char *name)
{
    static const struct { const char *n; int vk; } tab[] = {
        {"SPACE", VK_SPACE}, {"ENTER", VK_RETURN}, {"RETURN", VK_RETURN},
        {"TAB", VK_TAB}, {"SHIFT", VK_SHIFT}, {"CTRL", VK_CONTROL},
        {"CONTROL", VK_CONTROL}, {"LCTRL", VK_LCONTROL}, {"RCTRL", VK_RCONTROL},
        {"ALT", VK_MENU}, {"ESC", VK_ESCAPE},
        {"ESCAPE", VK_ESCAPE}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
        {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"BACKSPACE", VK_BACK},
        {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE}, {"HOME", VK_HOME},
        {"END", VK_END}, {"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT},
        {"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4},
        {"F5", VK_F5}, {"F6", VK_F6}, {"F7", VK_F7}, {"F8", VK_F8},
        {"F9", VK_F9}, {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},
        {"PAUSE", VK_PAUSE}, /* W188 GAME-MENU */
        {NULL, 0}
    };
    int i;
    if (!name[0]) return -1;
    if (!name[1]) {
        char c = name[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
        return -1;
    }
    for (i = 0; tab[i].n; i++) {
        if (imap_strieq(name, tab[i].n)) return tab[i].vk;
    }
    if ((name[0] >= '0' && name[0] <= '9') ||
        (name[0] == '0' && (name[1] == 'x' || name[1] == 'X'))) {
        char *end = NULL;
        long v = strtol(name, &end, 0);
        if (end && !*end && v >= 0 && v < 512) return (int)v;
    }
    return -1;
}

/* W193 PAUSE-LIVE: Tastenname -> VK fuer Client-Tasten ausserhalb der Profile (menu_peek_key). */
int imap_key_name(const char *name)
{
    return name ? imap_key_code(name) : -1;
}

static int imap_padbtn_code(const char *name, int *is_dpad)
{
    *is_dpad = 0;
    if (imap_strieq(name, "A")) return IMAP_PAD_A;
    if (imap_strieq(name, "B")) return IMAP_PAD_B;
    if (imap_strieq(name, "X")) return IMAP_PAD_X;
    if (imap_strieq(name, "Y")) return IMAP_PAD_Y;
    if (imap_strieq(name, "LB")) return IMAP_PAD_LB;
    if (imap_strieq(name, "RB")) return IMAP_PAD_RB;
    if (imap_strieq(name, "BACK")) return IMAP_PAD_BACK;
    if (imap_strieq(name, "START")) return IMAP_PAD_START;
    if (imap_strieq(name, "L3")) return IMAP_PAD_L3;
    if (imap_strieq(name, "R3")) return IMAP_PAD_R3;
    *is_dpad = 1;
    if (imap_strieq(name, "DU") || imap_strieq(name, "DUP"))
        return IMAP_PAD_DU;
    if (imap_strieq(name, "DD") || imap_strieq(name, "DDOWN"))
        return IMAP_PAD_DD;
    if (imap_strieq(name, "DL") || imap_strieq(name, "DLEFT"))
        return IMAP_PAD_DL;
    if (imap_strieq(name, "DR") || imap_strieq(name, "DRIGHT"))
        return IMAP_PAD_DR;
    return -1;
}

static int imap_analog_src(const char *name)
{
    if (imap_strieq(name, "LX")) return 0;
    if (imap_strieq(name, "LY")) return 1;
    if (imap_strieq(name, "RX")) return 2;
    if (imap_strieq(name, "RY")) return 3;
    if (imap_strieq(name, "LT")) return 4;
    if (imap_strieq(name, "RT")) return 5;
    return -1;
}

/* Klarnamen der 39 Controller-Tasten (Element-Aliase und W159
 * SBC-Quellnamen teilen sich diese Tabelle). */
static const struct { const char *n; int bi; } kBiAlias[] = {
    {"mainweapon", 0}, {"subweapon", 1}, {"lockon", 2}, {"eject", 3},
    {"hatch", 4}, {"ignition", 5}, {"start", 6}, {"openclose", 7},
    {"mapzoom", 8}, {"mode", 9}, {"submon", 10}, {"zoomin", 11},
    {"zoomout", 12}, {"fss", 13}, {"manip", 14}, {"linecolor", 15},
    {"washing", 16}, {"exting", 17}, {"extinguisher", 17}, {"chaff", 18},
    {"tankdetach", 19}, {"override", 20}, {"night", 21},
    {"nightscope", 21}, {"f1", 22}, {"f2", 23}, {"f3", 24},
    {"mainwc", 25}, {"subwc", 26}, {"magazine", 27},
    {"comm1", 28}, {"comm2", 29}, {"comm3", 30}, {"comm4", 31},
    {"comm5", 32}, {"sightbtn", 33}, {"togfilter", 34},
    {"togoxygen", 35}, {"togfuel", 36}, {"togbuffer", 37},
    {"togloc", 38}, {"togvtloc", 38},
    {NULL, 0}
};

/* W159: Client-Aktionen (Elemente ausserhalb des Controller-Blocks). */
enum {
    IMAP_ACT_VR_RECENTER = 0,
    IMAP_ACT_PAUSE = 1 /* W188 GAME-MENU */
};

/* Elementname -> Ziel. btn_i >= 0: Taste; axis >= 0 (+negpos 0/1/2 =
 * analog/neg/pos); gear: 0=up 1=down 2+r/n/1..5; tuner: 0=up 1=down;
 * action >= 0: Client-Aktion (kein Controller-Element). */
typedef struct {
    int btn_i, axis, negpos, gear, tuner, action;
} imap_elem_t;

static int imap_elem_parse(const char *name, imap_elem_t *e)
{
    int i;
    memset(e, 0, sizeof *e);
    e->btn_i = -1;
    e->axis = -1;
    e->negpos = 0;
    e->gear = -1;
    e->tuner = -1;
    e->action = -1;
    if (imap_strieq(name, "vr_recenter")) { /* W159: VR: Blick zentrieren */
        e->action = IMAP_ACT_VR_RECENTER;
        return 0;
    }
    if (imap_strieq(name, "pause")) { /* W188: Spiel anhalten/fortsetzen */
        e->action = IMAP_ACT_PAUSE;
        return 0;
    }
    if ((name[0] == 'b' || name[0] == 'B') &&
        (name[1] == 'i' || name[1] == 'I')) {
        char *end = NULL;
        long v = strtol(name + 2, &end, 10);
        if (end && !*end && v >= 0 && v < 39) {
            e->btn_i = (int)v;
            return 0;
        }
        return -1;
    }
    for (i = 0; kBiAlias[i].n; i++) {
        if (imap_strieq(name, kBiAlias[i].n)) {
            e->btn_i = kBiAlias[i].bi;
            return 0;
        }
    }
    {
        /* Achsen, optional .neg/.pos (auch _neg/_pos). */
        static const struct { const char *n; int ax; } axes[] = {
            {"aim_x", IMAP_AX_AIM_X}, {"aim_y", IMAP_AX_AIM_Y},
            {"rot", IMAP_AX_ROT}, {"sight_x", IMAP_AX_SIGHT_X},
            {"sight_y", IMAP_AX_SIGHT_Y}, {"pedal_l", IMAP_AX_PEDAL_L},
            {"pedal_m", IMAP_AX_PEDAL_M}, {"pedal_r", IMAP_AX_PEDAL_R},
            {NULL, 0}
        };
        char base[32];
        const char *dot;
        size_t bl;
        strncpy_s(base, sizeof base, name, sizeof base - 1);
        dot = strchr(base, '.');
        if (!dot) dot = strchr(base, '_');
        /* Vorsicht: Namen enthalten '_' (aim_x) - Suffix nur am Ende. */
        dot = NULL;
        bl = strlen(base);
        if (bl > 4 && (imap_strieq(base + bl - 4, ".neg") ||
                       imap_strieq(base + bl - 4, "_neg"))) {
            e->negpos = 1;
            base[bl - 4] = '\0';
        } else if (bl > 4 && (imap_strieq(base + bl - 4, ".pos") ||
                              imap_strieq(base + bl - 4, "_pos"))) {
            e->negpos = 2;
            base[bl - 4] = '\0';
        }
        for (i = 0; axes[i].n; i++) {
            if (imap_strieq(base, axes[i].n)) {
                e->axis = axes[i].ax;
                return 0;
            }
        }
        (void)dot;
    }
    if (imap_strieq(name, "gear.up") || imap_strieq(name, "gear_up")) {
        e->gear = 0;
        return 0;
    }
    if (imap_strieq(name, "gear.down") || imap_strieq(name, "gear_down")) {
        e->gear = 1;
        return 0;
    }
    if (imap_strieq(name, "gear.r")) {
        e->gear = 2;
        return 0;
    }
    if (imap_strieq(name, "gear.n")) {
        e->gear = 3;
        return 0;
    }
    if (strlen(name) == 6 && (name[0] == 'g' || name[0] == 'G') &&
        (name[4] == '.' || name[4] == '_') && name[5] >= '1' &&
        name[5] <= '5' && imap_strieq("gear", "gear")) {
        char g4[5];
        memcpy(g4, name, 4);
        g4[4] = '\0';
        if (imap_strieq(g4, "gear")) {
            e->gear = 4 + (name[5] - '1');
            return 0;
        }
    }
    if (imap_strieq(name, "tuner.up") || imap_strieq(name, "tuner_up")) {
        e->tuner = 0;
        return 0;
    }
    if (imap_strieq(name, "tuner.down") || imap_strieq(name, "tuner_down")) {
        e->tuner = 1;
        return 0;
    }
    return -1;
}

/* W159: Original-Controller-Taste als Quelle ("bi<N>" 0..38 oder
 * Klarname wie "chaff"). Rueckgabe bi-Index oder -1. */
static int imap_sbc_btn_parse(const char *name)
{
    int i;
    if ((name[0] == 'b' || name[0] == 'B') &&
        (name[1] == 'i' || name[1] == 'I')) {
        char *end = NULL;
        long v = strtol(name + 2, &end, 10);
        if (end && !*end && v >= 0 && v < 39) {
            return (int)v;
        }
        return -1;
    }
    for (i = 0; kBiAlias[i].n; i++) {
        if (imap_strieq(name, kBiAlias[i].n)) {
            return kBiAlias[i].bi;
        }
    }
    return -1;
}

/* Ein Atom "KIND:NAME" parsen (digitaler Kontext: Tasten/Endlagen). */
static int imap_atom_parse_dig(const char *kind, const char *name,
                               imap_atom_t *a, int *has_pad, int *has_sbc)
{
    if (imap_strieq(kind, "KEY")) {
        int vk = imap_key_code(name);
        if (vk < 0) return -1;
        a->kind = IMAP_A_KEY;
        a->code = vk;
        return 0;
    }
    if (imap_strieq(kind, "MOUSE")) {
        a->kind = IMAP_A_MOUSEBTN;
        if (imap_strieq(name, "LEFT") || imap_strieq(name, "L")) {
            a->code = 0;
            return 0;
        }
        if (imap_strieq(name, "RIGHT") || imap_strieq(name, "R")) {
            a->code = 1;
            return 0;
        }
        if (imap_strieq(name, "MIDDLE") || imap_strieq(name, "M")) {
            a->code = 2;
            return 0;
        }
        if (imap_strieq(name, "X1")) { /* W110: Seitentasten */
            a->code = 3;
            return 0;
        }
        if (imap_strieq(name, "X2")) {
            a->code = 4;
            return 0;
        }
        return -1;
    }
    if (imap_strieq(kind, "PAD")) {
        int is_dpad = 0, bit = imap_padbtn_code(name, &is_dpad);
        if (bit >= 0) {
            a->kind = is_dpad ? IMAP_A_PADDPAD : IMAP_A_PADBTN;
            a->code = bit;
            *has_pad = 1;
            return 0;
        }
        if (imap_strieq(name, "LT") || imap_strieq(name, "RT")) {
            a->kind = IMAP_A_PADTRIG;
            a->code = (name[0] == 'L' || name[0] == 'l') ? 0 : 1;
            *has_pad = 1;
            return 0;
        }
        return -1;
    }
    if (imap_strieq(kind, "SBC")) { /* W159: Original-Controller-Taste */
        int bi = imap_sbc_btn_parse(name);
        if (bi < 0) return -1;
        a->kind = IMAP_A_SBC;
        a->code = bi;
        *has_sbc = 1;
        return 0;
    }
    return -1;
}

/* Eine Alternative "SRC[+SRC][:param...]" an eine Bindung haengen. */
static int imap_bind_add(imap_bind_t *b, const char *text, int *has_pad,
                         int *has_sbc)
{
    char work[128], *plus, *params;
    imap_chord_t c;
    memset(&c, 0, sizeof c);
    strncpy_s(work, sizeof work, text, sizeof work - 1);
    plus = strchr(work, '+');
    params = NULL;
    if (plus) {
        /* Akkord: Parameter nur am zweiten Atom (in der Praxis ungenutzt). */
        char *p2 = strchr(plus + 1, ':');
        if (p2 && strchr(p2, '=')) {
            /* "A+B:cond=.." gehoert zur Alternative, nicht zu B. */
            *p2 = '\0';
            params = p2 + 1;
        }
        *plus = '\0';
    } else {
        /* Erstes ':' nach KIND:NAME trennt Parameter ab. */
        char *c1 = strchr(work, ':');
        if (c1) {
            char *c2 = strchr(c1 + 1, ':');
            if (c2) {
                *c2 = '\0';
                params = c2 + 1;
            }
        }
    }
    {
        char *k1 = work, *n1, *k2 = NULL, *n2 = NULL;
        n1 = strchr(k1, ':');
        if (!n1) return -1;
        *n1++ = '\0';
        if (imap_atom_parse_dig(k1, n1, &c.a, has_pad, has_sbc) != 0) return -1;
        if (plus) {
            k2 = plus + 1;
            n2 = strchr(k2, ':');
            if (!n2) return -1;
            *n2++ = '\0';
            if (imap_atom_parse_dig(k2, n2, &c.b, has_pad, has_sbc) != 0) return -1;
            c.chord = 1;
        }
    }
    if (params) {
        /* "p=v:p=v" */
        char *p = params;
        while (*p) {
            char *nx = strchr(p, ':');
            char *eq;
            if (nx) *nx++ = '\0';
            eq = strchr(p, '=');
            if (eq) {
                *eq++ = '\0';
                if (imap_strieq(p, "cond")) {
                    if (imap_strieq(eq, "SHIFT")) c.cond = 1;
                    else if (imap_strieq(eq, "!SHIFT") ||
                             imap_strieq(eq, "NOSHIFT")) c.cond = 2;
                    else return -1;
                } else {
                    return -1; /* unbekannter Param */
                }
            } else {
                return -1;
            }
            p = nx ? nx : "";
        }
    }
    if (b->n >= IMAP_MAX_ALT) return -2; /* voll */
    b->alt[b->n++] = c;
    return 0;
}

/* Analog-Alternative "PAD:SRC[:dz=..:curve=..:inv=..]" oder "MOUSE:X/Y[:mode=..]". */
static int imap_axis_add_analog(imap_axis_map_t *m, const char *text,
                                int *has_pad)
{
    char work[128], *c1, *c2, *params;
    strncpy_s(work, sizeof work, text, sizeof work - 1);
    c1 = strchr(work, ':');
    if (!c1) return -1;
    *c1++ = '\0';
    c2 = strchr(c1, ':');
    params = NULL;
    if (c2) {
        *c2++ = '\0';
        params = c2;
    }
    if (imap_strieq(work, "MOUSE")) {
        int accu = imap_strieq(c1, "X") ? 1 : imap_strieq(c1, "Y") ? 2 : 0;
        int mode = -2; /* ungesetzt */
        char *p;
        if (!accu) return -1;
        p = params;
        while (p && *p) {
            char *nx = strchr(p, ':');
            char *eq;
            if (nx) *nx++ = '\0';
            eq = strchr(p, '=');
            if (!eq) return -1;
            *eq++ = '\0';
            if (!imap_strieq(p, "mode")) return -1;
            if (imap_strieq(eq, "aim")) mode = 0;
            else if (imap_strieq(eq, "sight")) mode = 1;
            else if (imap_strieq(eq, "any")) mode = -1;
            else return -1;
            p = nx ? nx : "";
        }
        m->mouse = accu;
        m->mouse_sight = mode == -2 ? -1 : mode;
        return 0;
    }
    if (imap_strieq(work, "PAD")) {
        int src = imap_analog_src(c1), slot;
        float dz = 0.0f, curve = 1.0f;
        int inv = 0;
        char *p;
        if (src < 0) return -1;
        p = params;
        while (p && *p) {
            char *nx = strchr(p, ':');
            char *eq;
            if (nx) *nx++ = '\0';
            eq = strchr(p, '=');
            if (!eq) return -1;
            *eq++ = '\0';
            if (imap_strieq(p, "dz")) {
                dz = (float)atof(eq);
                if (dz < 0 || dz >= 1) return -1;
            } else if (imap_strieq(p, "curve")) {
                curve = (float)atof(eq);
                if (curve < 0.5f || curve > 3.0f) return -1;
            } else if (imap_strieq(p, "inv")) {
                inv = atoi(eq) != 0;
            } else {
                return -1;
            }
            p = nx ? nx : "";
        }
        for (slot = 0; slot < 2; slot++) {
            if (m->pad[slot].src < 0) {
                m->pad[slot].src = src;
                m->pad[slot].dz = dz;
                m->pad[slot].curve = curve;
                m->pad[slot].inv = inv;
                *has_pad = 1;
                return 0;
            }
        }
        return -2;
    }
    return -1;
}

static void imap_profile_init(imap_profile_t *p, const char *name)
{
    int i, k;
    memset(p, 0, sizeof *p);
    strncpy_s(p->name, sizeof p->name, name ? name : "",
              sizeof p->name - 1);
    for (i = 0; i < IMAP_AX_COUNT; i++) {
        p->axis[i].mouse_sight = -1;
        for (k = 0; k < 2; k++) p->axis[i].pad[k].src = -1;
    }
}

int imap_profile_parse(const char *text, const char *name, imap_profile_t *out)
{
    char line[256];
    const char *r = text;
    imap_profile_init(out, name);
    while (*r) {
        const char *nl = strchr(r, '\n');
        size_t n = nl ? (size_t)(nl - r) : strlen(r);
        char *eq, *val, *alt, *ctx = NULL;
        imap_elem_t e;
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, r, n);
        line[n] = '\0';
        r = nl ? nl + 1 : r + n;
        /* Kommentar ab '#' (ganze Zeile oder inline). */
        {
            char *h = strchr(line, '#');
            if (h) *h = '\0';
        }
        imap_trim(line);
        if (!line[0] || line[0] == ';' || line[0] == '[') continue;
        eq = strchr(line, '=');
        if (!eq) continue; /* unbekannte Zeile: ignorieren */
        *eq = '\0';
        imap_trim(line);
        val = eq + 1;
        if (imap_elem_parse(line, &e) != 0) continue; /* unbekannt: weiter */
        for (alt = strtok_s(val, ",", &ctx); alt;
             alt = strtok_s(NULL, ",", &ctx)) {
            int rc;
            imap_trim(alt);
            if (!alt[0]) continue;
            if (e.action == IMAP_ACT_VR_RECENTER) { /* W159 */
                rc = imap_bind_add(&out->vr_recenter, alt, &out->has_pad,
                                   &out->has_sbc);
            } else if (e.action == IMAP_ACT_PAUSE) { /* W188 */
                rc = imap_bind_add(&out->pause, alt, &out->has_pad,
                                   &out->has_sbc);
            } else if (e.btn_i >= 0) {
                rc = imap_bind_add(&out->btn[e.btn_i], alt, &out->has_pad,
                                   &out->has_sbc);
            } else if (e.axis >= 0 && e.negpos == 0) {
                rc = imap_axis_add_analog(&out->axis[e.axis], alt,
                                          &out->has_pad);
                if (rc != 0) {
                    /* Kein Analog-Atom (z. B. KEY:S auf Pedal):
                     * digitale Vollauslenkung. */
                    rc = imap_bind_add(&out->axis[e.axis].pos, alt,
                                       &out->has_pad, &out->has_sbc);
                }
            } else if (e.axis >= 0 && e.negpos == 1) {
                rc = imap_bind_add(&out->axis[e.axis].neg, alt,
                                   &out->has_pad, &out->has_sbc);
            } else if (e.axis >= 0) {
                rc = imap_bind_add(&out->axis[e.axis].pos, alt,
                                   &out->has_pad, &out->has_sbc);
            } else if (e.gear == 0) {
                rc = imap_bind_add(&out->gear_up, alt, &out->has_pad,
                                   &out->has_sbc);
            } else if (e.gear == 1) {
                rc = imap_bind_add(&out->gear_down, alt, &out->has_pad,
                                   &out->has_sbc);
            } else if (e.gear >= 2) {
                rc = imap_bind_add(&out->gear_direct[e.gear - 2], alt,
                                   &out->has_pad, &out->has_sbc);
            } else if (e.tuner == 0) {
                rc = imap_bind_add(&out->tuner_up, alt, &out->has_pad,
                                   &out->has_sbc);
            } else {
                rc = imap_bind_add(&out->tuner_down, alt, &out->has_pad,
                                   &out->has_sbc);
            }
            (void)rc; /* fehlerhafte Alternative: ignorieren */
        }
    }
    return 0;
}

/* Eingebaute Profile = exakt die Inhalte von profiles/*.ini (Selbsttest
 * prueft Datei == eingebaut). */
static const char kBuiltinKeyboard[] =
    "# keyboard_default - heutige feste Belegung (hooks_input.c), exakt.\n"
    "bi0 = KEY:SPACE, MOUSE:LEFT\n"
    "bi1 = KEY:F, MOUSE:RIGHT\n"
    "bi2 = KEY:L, MOUSE:MIDDLE\n"
    "bi4 = KEY:H\n"
    "bi5 = KEY:I\n"
    "bi6 = KEY:ENTER\n"
    "bi8 = KEY:M\n"
    "bi9 = KEY:TAB\n"
    "bi11 = KEY:Z\n"
    "bi12 = KEY:X\n"
    "bi13 = KEY:V\n"
    "bi18 = KEY:C\n"
    "bi20 = KEY:O\n"
    "bi21 = KEY:N\n"
    "bi27 = KEY:R\n"
    "aim_x.neg = KEY:LEFT:cond=!SHIFT\n"
    "aim_x.pos = KEY:RIGHT:cond=!SHIFT\n"
    "aim_x = MOUSE:X:mode=aim\n"
    "aim_y.neg = KEY:UP:cond=!SHIFT\n"
    "aim_y.pos = KEY:DOWN:cond=!SHIFT\n"
    "aim_y = MOUSE:Y:mode=aim\n"
    "rot.neg = KEY:A\n"
    "rot.pos = KEY:D\n"
    "sight_x.neg = KEY:LEFT\n"
    "sight_x.pos = KEY:RIGHT\n"
    "sight_x = MOUSE:X:mode=sight\n"
    "sight_y.neg = KEY:UP\n"
    "sight_y.pos = KEY:DOWN\n"
    "sight_y = MOUSE:Y:mode=sight\n"
    "pedal_l = KEY:LCTRL, MOUSE:X1, MOUSE:X2\n"
    "pedal_m = KEY:S\n"
    "pedal_r = KEY:W\n"
    "gear.up = KEY:E\n"
    "gear.down = KEY:Q\n"
    "tuner.up = KEY:T\n"
    "tuner.down = KEY:G\n"
    "[vr]\n"
    "vr_recenter = KEY:F8\n"
    "[client]\n"
    "pause = KEY:PAUSE\n";

static const char kBuiltinXbox[] =
    "# xbox_pad_default - Xbox-Gamepad + Tastatur parallel (OR / Max-Abs).\n"
    "# Tasten: volle keyboard_default-Belegung, dazu Pad (s. EINGABE.md).\n"
    "bi0 = KEY:SPACE, MOUSE:LEFT, PAD:RB\n"
    "bi1 = KEY:F, MOUSE:RIGHT, PAD:B\n"
    "bi2 = KEY:L, MOUSE:MIDDLE, PAD:LB\n"
    "bi3 = PAD:BACK+PAD:Y\n"
    "bi4 = KEY:H, PAD:BACK+PAD:X\n"
    "bi5 = KEY:I, PAD:BACK+PAD:A\n"
    "bi6 = KEY:ENTER, PAD:START\n"
    "bi8 = KEY:M\n"
    "bi9 = KEY:TAB\n"
    "bi10 = PAD:DL, PAD:DR\n"
    "bi11 = KEY:Z\n"
    "bi12 = KEY:X\n"
    "bi13 = KEY:V\n"
    "bi16 = PAD:BACK+PAD:START\n"
    "bi17 = PAD:L3\n"
    "bi18 = KEY:C, PAD:A\n"
    "bi20 = KEY:O, PAD:BACK+PAD:B\n"
    "bi21 = KEY:N, PAD:BACK+PAD:L3\n"
    "bi25 = PAD:Y\n"
    "bi26 = PAD:X\n"
    "bi27 = KEY:R, PAD:LB+PAD:RB\n"
    "bi33 = PAD:R3\n"
    "bi34 = PAD:BACK+PAD:DL\n"
    "bi35 = PAD:BACK+PAD:DR\n"
    "bi36 = PAD:BACK+PAD:DU\n"
    "bi37 = PAD:BACK+PAD:DD\n"
    "bi38 = PAD:BACK+PAD:R3\n"
    "aim_x.neg = KEY:LEFT:cond=!SHIFT\n"
    "aim_x.pos = KEY:RIGHT:cond=!SHIFT\n"
    "aim_x = MOUSE:X:mode=aim, PAD:LX:dz=0.15:curve=1.5\n"
    "aim_y.neg = KEY:UP:cond=!SHIFT\n"
    "aim_y.pos = KEY:DOWN:cond=!SHIFT\n"
    "aim_y = MOUSE:Y:mode=aim, PAD:LY:dz=0.15:curve=1.5:inv=1\n"
    "rot.neg = KEY:A\n"
    "rot.pos = KEY:D\n"
    "rot = PAD:RX:dz=0.12:curve=1.3\n"
    "sight_x.neg = KEY:LEFT\n"
    "sight_x.pos = KEY:RIGHT\n"
    "sight_x = MOUSE:X:mode=sight, PAD:RX:dz=0.12:curve=1.3\n"
    "sight_y.neg = KEY:UP\n"
    "sight_y.pos = KEY:DOWN\n"
    "sight_y = MOUSE:Y:mode=sight, PAD:RY:dz=0.12:curve=1.3:inv=1\n"
    "pedal_l = KEY:LCTRL, MOUSE:X1, MOUSE:X2, PAD:BACK+PAD:LT\n"
    "pedal_m = KEY:S, PAD:LT:dz=0.05\n"
    "pedal_r = KEY:W, PAD:RT:dz=0.05\n"
    "gear.up = KEY:E, PAD:BACK+PAD:RB\n"
    "gear.down = KEY:Q, PAD:BACK+PAD:LB\n"
    "tuner.up = KEY:T, PAD:DU\n"
    "tuner.down = KEY:G, PAD:DD\n"
    "[vr]\n"
    "vr_recenter = KEY:F8\n"
    "[client]\n"
    "pause = KEY:PAUSE\n";

int imap_profile_builtin(const char *name, imap_profile_t *out)
{
    if (imap_strieq(name, "keyboard_default"))
        return imap_profile_parse(kBuiltinKeyboard, name, out);
    if (imap_strieq(name, "xbox_pad_default"))
        return imap_profile_parse(kBuiltinXbox, name, out);
    return -1;
}

/* Datei einlesen (ganz, max 64 KB). */
static char *imap_read_file(const char *path)
{
    FILE *f;
    long n;
    char *buf;
    size_t got;
    if (fopen_s(&f, path, "rb") != 0 || !f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 65536) {
        fclose(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

int imap_profile_load(const char *name_or_path, imap_profile_t *out)
{
    char path[512];
    char *text;
    int is_path;
    size_t i;
    /* Direkter Pfad? (Trennzeichen oder .ini-Endung) */
    is_path = 0;
    for (i = 0; name_or_path[i]; i++) {
        if (name_or_path[i] == '/' || name_or_path[i] == '\\')
            is_path = 1;
    }
    if (!is_path) {
        size_t n = strlen(name_or_path);
        if (n > 4 && imap_strieq(name_or_path + n - 4, ".ini"))
            is_path = 1;
    }
    if (is_path) {
        text = imap_read_file(name_or_path);
        if (!text) {
            /* Bekannter Name als Pfad (z. B. mit .ini-Suffix)?
             * -> eingebauter Fallback statt Fehler. */
            const char *b = name_or_path, *s;
            char base[64];
            size_t n;
            for (s = name_or_path; *s; s++) {
                if (*s == '/' || *s == '\\') b = s + 1;
            }
            strncpy_s(base, sizeof base, b, sizeof base - 1);
            n = strlen(base);
            if (n > 4 && imap_strieq(base + n - 4, ".ini"))
                base[n - 4] = '\0';
            return imap_profile_builtin(base, out) == 0 ? 1 : -1;
        }
        imap_profile_parse(text, name_or_path, out);
        free(text);
        return 0;
    }
    /* profiles/<name>.ini: erst CWD, dann Exe-Ordner. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "profiles/%s.ini", name_or_path);
    text = imap_read_file(path);
    if (!text) {
        char exe[384], *sep;
        if (GetModuleFileNameA(NULL, exe, sizeof exe) &&
            ((sep = strrchr(exe, '\\')) != NULL ||
             (sep = strrchr(exe, '/')) != NULL)) {
            *sep = '\0';
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s/profiles/%s.ini",
                        exe, name_or_path);
            text = imap_read_file(path);
        }
    }
    if (text) {
        imap_profile_parse(text, name_or_path, out);
        free(text);
        return 0;
    }
    /* Fallback: eingebaut (bitgenau, auch ohne Datei). */
    return imap_profile_builtin(name_or_path, out) == 0 ? 1 : -1;
}

static int imap_bind_equal(const imap_bind_t *a, const imap_bind_t *b)
{
    int i;
    if (a->n != b->n) return 1;
    for (i = 0; i < a->n; i++) {
        const imap_chord_t *x = &a->alt[i], *y = &b->alt[i];
        if (x->chord != y->chord || x->cond != y->cond) return 1;
        if (x->a.kind != y->a.kind || x->a.code != y->a.code) return 1;
        if (x->chord &&
            (x->b.kind != y->b.kind || x->b.code != y->b.code))
            return 1;
    }
    return 0;
}

static int imap_axis_equal(const imap_axis_map_t *a, const imap_axis_map_t *b)
{
    int i;
    if (imap_bind_equal(&a->neg, &b->neg)) return 1;
    if (imap_bind_equal(&a->pos, &b->pos)) return 1;
    if (a->mouse != b->mouse || a->mouse_sight != b->mouse_sight) return 1;
    for (i = 0; i < 2; i++) {
        if (a->pad[i].src != b->pad[i].src) return 1;
        if (a->pad[i].src < 0) continue;
        if (a->pad[i].dz != b->pad[i].dz) return 1;
        if (a->pad[i].curve != b->pad[i].curve) return 1;
        if (a->pad[i].inv != b->pad[i].inv) return 1;
    }
    return 0;
}

int imap_profile_equal(const imap_profile_t *a, const imap_profile_t *b)
{
    int i;
    for (i = 0; i < 39; i++) {
        if (imap_bind_equal(&a->btn[i], &b->btn[i])) return 1;
    }
    for (i = 0; i < IMAP_AX_COUNT; i++) {
        if (imap_axis_equal(&a->axis[i], &b->axis[i])) return 1;
    }
    if (imap_bind_equal(&a->gear_up, &b->gear_up)) return 1;
    if (imap_bind_equal(&a->gear_down, &b->gear_down)) return 1;
    if (imap_bind_equal(&a->tuner_up, &b->tuner_up)) return 1;
    if (imap_bind_equal(&a->tuner_down, &b->tuner_down)) return 1;
    for (i = 0; i < 7; i++) {
        if (imap_bind_equal(&a->gear_direct[i], &b->gear_direct[i]))
            return 1;
    }
    if (imap_bind_equal(&a->vr_recenter, &b->vr_recenter)) return 1; /* W159 */
    if (imap_bind_equal(&a->pause, &b->pause)) return 1; /* W188 */
    if (a->has_pad != b->has_pad) return 1;
    if (a->has_sbc != b->has_sbc) return 1; /* W159 */
    return 0;
}

/* ---- Gamepad-Poll (XInput dynamisch, kein Treiber noetig) ---- */

#pragma pack(push, 1)
typedef struct {
    uint16_t wButtons;
    uint8_t bLeftTrigger, bRightTrigger;
    int16_t sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} imap_xi_gamepad_t;

typedef struct {
    uint32_t dwPacketNumber;
    imap_xi_gamepad_t Gamepad;
} imap_xi_state_t;
#pragma pack(pop)

typedef uint32_t(WINAPI *imap_xinput_get_state_fn)(uint32_t, imap_xi_state_t *);

static imap_xinput_get_state_fn g_xgs;
static int g_xgs_tried;
static unsigned g_pad_frame = 0xFFFFFFFFu;
static imap_pad_state_t g_pad_cache;
static int g_pad_cached_present;

int imap_pad_poll(unsigned frame, imap_pad_state_t *out)
{
    uint32_t port;
    if (!g_xgs_tried) {
        static const char *dlls[] = {
            "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", NULL
        };
        int i;
        g_xgs_tried = 1;
        for (i = 0; dlls[i]; i++) {
            HMODULE m = LoadLibraryA(dlls[i]);
            if (m) {
                g_xgs = (imap_xinput_get_state_fn)(void *)GetProcAddress(
                    m, "XInputGetState");
                if (g_xgs) break;
                FreeLibrary(m);
            }
        }
    }
    if (frame == g_pad_frame) {
        if (g_pad_cached_present && out) *out = g_pad_cache;
        return g_pad_cached_present;
    }
    g_pad_frame = frame;
    g_pad_cached_present = 0;
    if (g_xgs) {
        for (port = 0; port < 4; port++) {
            imap_xi_state_t xis;
            memset(&xis, 0, sizeof xis);
            if (g_xgs(port, &xis) == 0 /* ERROR_SUCCESS */) {
                g_pad_cache.buttons = xis.Gamepad.wButtons;
                g_pad_cache.lt = xis.Gamepad.bLeftTrigger;
                g_pad_cache.rt = xis.Gamepad.bRightTrigger;
                g_pad_cache.lx = xis.Gamepad.sThumbLX;
                g_pad_cache.ly = xis.Gamepad.sThumbLY;
                g_pad_cache.rx = xis.Gamepad.sThumbRX;
                g_pad_cache.ry = xis.Gamepad.sThumbRY;
                g_pad_cached_present = 1;
                break;
            }
        }
    }
    if (g_pad_cached_present && out) *out = g_pad_cache;
    return g_pad_cached_present;
}

/* ---- W128: Achsen-Arbitrierung SBC gegen Maus (s. input_map.h) ---- */
void imap_arb_axes(imap_arb_t *a, const imap_arb_in_t *in, int out[IMAP_ARB_N])
{
    int i;
    if (!a || !in || !out) {
        return;
    }
    if (!a->init) {
        for (i = 0; i < IMAP_ARB_N; i++) {
            a->sbc_owns[i] = 0;
            a->sbc_ref[i] = in->sbc_center[i];
            a->sbc_last[i] = in->sbc[i];
            a->m_last[i] = in->mpos[i];
            a->m_move[i] = 0.0f;
        }
        a->init = 1;
    }
    for (i = 0; i < IMAP_ARB_N; i++) {
        float dm = fabsf(in->mpos[i] - a->m_last[i]);
        int sbc = in->sbc[i];
        a->m_last[i] = in->mpos[i];
        if (!(in->mouse_src & (1u << i))) {
            /* Tastatur/Pfeile/keine Maus: alte Regel (W75) */
            out[i] = (in->cur[i] == in->idle[i] && sbc != in->sbc_center[i])
                         ? sbc : in->cur[i];
            continue;
        }
        if (!a->sbc_owns[i]) {
            if (abs(sbc - a->sbc_ref[i]) > in->dead) {
                a->sbc_owns[i] = 1;
                a->sbc_last[i] = sbc;
                a->m_move[i] = 0.0f;
            }
        } else {
            if (abs(sbc - a->sbc_last[i]) > in->dead) {
                a->sbc_last[i] = sbc; /* Hebel aktiv: Maus-Zittern zaehlt nicht */
                a->m_move[i] = 0.0f;
            } else {
                a->m_move[i] += dm;
                if (a->m_move[i] >= in->wake) {
                    a->sbc_owns[i] = 0;
                    a->sbc_ref[i] = sbc;
                    a->m_move[i] = 0.0f;
                }
            }
        }
        out[i] = a->sbc_owns[i] ? sbc : in->cur[i];
    }
}
