/* input_selftest.c - Vergleichstest alt <-> neu + Gamepad-Logik (W76).
 *
 * Bau: input_selftest.exe (s. CMakeLists, nur input_map.c + dieses File).
 *   input_selftest                 alle Pruefungen (Exit 0 = gruen)
 * Keine Umlaute in der Konsolenausgabe (Codepage-sicher).
 *
 * Teil 1: profiles/*.ini == eingebaute Profile (Datei gefunden?).
 * Teil 2: Legacy (imap_legacy_apply = shipped alter Code) vs. Profil
 *         (imap_profile_apply + keyboard_default) ueber alle Tasten,
 *         Kombinationen, Folgen und cfg-Varianten: 0 Abweichungen.
 * Teil 3: Gamepad-Logik (xbox_pad_default) mit simulierten XInput-
 *         Zustaenden: Tabelle + Pruefung.
 * Teil 7 (W110 PEDAL-L): Slide-Step - LCtrl, Maus X1/X2, Pad BACK+LT.
 * Teil 8 (W128 SBC-LIVE): Achsen-Arbitrierung SBC gegen Maus
 *         (imap_arb_axes, "zuletzt aktives Geraet gewinnt").
 * Teil 9 (W132 PAD-ARB): Achsen-Arbitrierung Pad gegen Maus
 *         (imap_arb_pad1 im Profil-Pfad, simuliertes Pad + absolute Maus).
 * Teil 10 (W141 CTRL-CAL2): Zielhebel absolut (Reibungshebel ohne Mitte,
 *         Zwei-Wert-Form), federnde Achsen weiter mit Auto-Mitte, Gang
 *         254/255/0/1-5 nativ (sbc_usb_gear_effective).
 * Teil 11 (W143 CTRL-ASSIST): globale Pad-Totzonen (pad_dz_stick/_trig)
 *         als Untergrenze ueber der Profil-Totzone (0 = unveraendert).
 * Teil 12 (W159 VR-LAUNCH): Aktion vr_recenter (Flanke, KEY/PAD/SBC,
 *         Akkord-Unterdrueckung) + SBC-Quellnamen.
 * Teil 13 (W188 GAME-MENU): Aktion pause (Standard KEY:PAUSE, Tasten nur
 *         ueber das Fenster, Pad-Flanke im Hook, imap_bind_keys).
 * Teil 14 (W193 PAUSE-LIVE): imap_key_name fuer menu_peek_key.
 * Teil 16 (W218 SIMPLE-CONTROLS, W236 AIM-ASSIST): Sichtfuehrung (Rand dreht
 * die Sicht, Waffenarm fest, Anschlag, Rueckweg, Aus bitgleich) und
 *   Gang-Automatik gegen ein einfaches Fahrzeugmodell.
 * Teil 17 (W221 HOTAS, experimentell): simulierte DirectInput-Geraete
 *   (imap_snap_t.joy): Abbildung, Totzone, Invertierung, Kurve/sens,
 *   Tasten->Achse, Tasten->Gang, Achsen-Zonen->Gang, POV, mehrere Geraete,
 *   Geraete-Zuordnung (ijoy_match_list).
 * Teil 19 (W271 STICK-RELATIVE): relativer Stick auf dem Zielhebel (Bewegung,
 *   Loslassen haelt, Klemmung, Totzone, Invertierung, aim_center, Maus+Stick,
 *   Beschleunigung, :mode= je Quelle, Standard bitgleich).
 * Teil 15 (W195 PHOTO-MODE): Aktion photo (Standard KEY:F12, Tasten nur ueber
 *         das Fenster, Pad-Flanke im Hook, imap_bind_keys, Profilvergleich).
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h> /* W439: abs */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "input_map.h"
#include "input_tap.h"
#include "input_sbc_usb.h"
#include "sb_ally_core.h" /* W424 */
#include "input_despike.h" /* W439 */

static int n_checks, n_fail;
static long n_states;
static long n_mismatch;

#define CHECK(cond, ...) do { \
    n_checks++; \
    if (!(cond)) { n_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

static uint8_t g_keys[512];

static int stub_key(int vk, void *ctx)
{
    (void)ctx;
    if (vk < 0 || vk >= 512) return 0;
    return g_keys[vk] != 0;
}

static void set_keys(const int *vks, int n)
{
    int i;
    memset(g_keys, 0, sizeof g_keys);
    for (i = 0; i < n; i++) {
        if (vks[i] >= 0 && vks[i] < 512) g_keys[vks[i]] = 1;
    }
}

typedef struct {
    int momentary, aim, sight, buttons, sens, invx, invy;
} cfgvar_t;

static void make_snap(imap_snap_t *s, unsigned frame, const cfgvar_t *c)
{
    memset(s, 0, sizeof *s);
    s->key = stub_key;
    s->ctx = NULL;
    s->replay_active = 1; /* synthetische Maus-VKs, deterministisch */
    s->frame = frame;
    s->mouse_aim = c->aim;
    s->mouse_sight = c->sight;
    s->mouse_sens = c->sens;
    s->mouse_invert_x = c->invx;
    s->mouse_invert_y = c->invy;
    s->mouse_buttons = c->buttons;
    s->sbc_momentary = c->momentary;
    s->pad_present = 0;
}

static void fmt_blk(char *out, size_t n, const imap_block_t *b)
{
    _snprintf_s(out, n, _TRUNCATE,
                "btn=%04X%04X%04X aim=%02X%02X sight=%d,%d rot=%d "
                "ped=%02X%02X%02X tun=%u gear=%u",
                b->buttons[0], b->buttons[1], b->buttons[2],
                b->aiming_x, b->aiming_y,
                (int)b->sight_x, (int)b->sight_y, (int)b->rot_lever,
                b->pedal_l, b->pedal_m, b->pedal_r, b->tuner, b->gear);
}

/* Ein Poll beidseitig fahren + vergleichen. */
static void cmp_poll(imap_state_t *sl, imap_state_t *sp,
                     const imap_profile_t *prof, imap_snap_t *snap,
                     const char *tag)
{
    imap_block_t bl, bp;
    imap_legacy_apply(&bl, sl, snap);
    imap_profile_apply(&bp, sp, prof, snap);
    n_states++;
    if (memcmp(&bl, &bp, sizeof bl) != 0) {
        char a[160], b[160];
        n_mismatch++;
        if (n_mismatch <= 8) {
            fmt_blk(a, sizeof a, &bl);
            fmt_blk(b, sizeof b, &bp);
            printf("DIFF %s f=%u\n  alt: %s\n  neu: %s\n", tag, snap->frame,
                   a, b);
        }
    }
}

/* Tasten-Universum der Legacy-Belegung + Virtuals + Maus-Synth. */
static const int kCoreKeys[] = {
    VK_RETURN, VK_SPACE, 'F', 'L', VK_TAB, 'M', 'Z', 'X', 'C', 'O',
    'N', 'R', 'V', 'I', 'H', 'A', 'D', 'W', 'S', 'Q', 'E', 'T', 'G',
    VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_SHIFT,
    VK_LCONTROL, VK_RCONTROL, VK_CONTROL, /* W110: Slide-Step (nur links) */
    VK_DELETE /* W436: Eject-Akkord Umschalt+Entf (W403) - Legacy == keyboard_default */
};
#define KCORE_N ((int)(sizeof kCoreKeys / sizeof kCoreKeys[0]))

static int build_universe(int *out)
{
    int n = 0, i;
    for (i = 0; i < KCORE_N; i++)
        out[n++] = kCoreKeys[i];
    for (i = 0; i < 39; i++) out[n++] = 256 + i; /* bi-Virtuals */
    for (i = 320; i <= 328; i++) out[n++] = i;   /* Maus-Synth (+X1/X2) */
    return n;
}

static void run_compare(const imap_profile_t *prof)
{
    static const cfgvar_t vars[] = {
        {1, 1, 0, 1, 4, 0, 0}, /* Default-cfg */
        {0, 1, 0, 1, 4, 0, 0}, /* Toggle-Modus */
        {1, 0, 0, 1, 4, 0, 0}, /* Maus analog aus */
        {1, 1, 1, 1, 4, 0, 0}, /* Maus -> Sight */
        {1, 1, 0, 0, 4, 0, 0}, /* Maustasten aus */
        {0, 0, 1, 0, 4, 0, 0}, /* Toggle + Sight + Tasten aus */
        {1, 1, 0, 1, 8, 1, 1}, /* sens 8 + invertiert */
        {1, 1, 1, 1, 8, 1, 1}, /* dto. Sight */
    };
    int uni[128], nu, vi, i, j, k;
    unsigned frame = 100;
    nu = build_universe(uni);

    for (vi = 0; vi < (int)(sizeof vars / sizeof vars[0]); vi++) {
        const cfgvar_t *c = &vars[vi];
        char tag[64];
        _snprintf_s(tag, sizeof tag, _TRUNCATE, "v%d", vi);
        /* A. Einzelne Tasten: down,down,down,up,down,up (Flanken!). */
        for (i = 0; i < nu; i++) {
            imap_state_t sl, sp;
            imap_snap_t s;
            static const int pat[6] = {1, 1, 1, 0, 1, 0};
            imap_state_init(&sl);
            imap_state_init(&sp);
            for (k = 0; k < 6; k++) {
                make_snap(&s, frame++, c);
                if (pat[k]) set_keys(&uni[i], 1);
                else set_keys(NULL, 0);
                cmp_poll(&sl, &sp, prof, &s, tag);
            }
        }
        /* B. Paare (Kern-Tasten): beide 2 Polls, dann loslassen. */
        for (i = 0; i < KCORE_N; i++) {
            for (j = i + 1; j < KCORE_N; j++) {
                imap_state_t sl, sp;
                imap_snap_t s;
                int pair[2] = {uni[i], uni[j]};
                imap_state_init(&sl);
                imap_state_init(&sp);
                make_snap(&s, frame++, c);
                set_keys(pair, 2);
                cmp_poll(&sl, &sp, prof, &s, tag);
                make_snap(&s, frame++, c);
                cmp_poll(&sl, &sp, prof, &s, tag);
                make_snap(&s, frame++, c);
                set_keys(NULL, 0);
                cmp_poll(&sl, &sp, prof, &s, tag);
            }
        }
        /* C. Pfeile+Shift-Tripel: alle 32 Teilmengen. */
        {
            static const int arr[5] = {
                VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_SHIFT
            };
            for (i = 0; i < 32; i++) {
                imap_state_t sl, sp;
                imap_snap_t s;
                int ks[5], n = 0;
                for (k = 0; k < 5; k++) {
                    if (i & (1 << k)) ks[n++] = arr[k];
                }
                imap_state_init(&sl);
                imap_state_init(&sp);
                make_snap(&s, frame++, c);
                set_keys(ks, n);
                cmp_poll(&sl, &sp, prof, &s, tag);
                make_snap(&s, frame++, c);
                cmp_poll(&sl, &sp, prof, &s, tag);
            }
        }
        /* D. Gang/Tuner-Sweeps (Zustand laeuft weiter!). */
        {
            imap_state_t sl, sp;
            imap_snap_t s;
            static const int seq[] = {
                'E', 'E', 'E', 'E', 'E', 'E', 'E', 'E',
                'Q', 'Q', 'Q', 'Q', 'Q', 'Q', 'Q', 'Q',
                'T', 'T', 'T', 'T', 'T', 'T', 'T', 'T',
                'T', 'T', 'T', 'T', 'T', 'T',
                'G', 'G', 'G', 'G', 'G', 'G', 'G', 'G',
                'G', 'G', 'G', 'G', 'G', 'G'
            };
            imap_state_init(&sl);
            imap_state_init(&sp);
            for (i = 0; i < (int)(sizeof seq / sizeof seq[0]); i++) {
                make_snap(&s, frame++, c);
                set_keys(&seq[i], 1);
                cmp_poll(&sl, &sp, prof, &s, tag);
                make_snap(&s, frame++, c);
                set_keys(NULL, 0);
                cmp_poll(&sl, &sp, prof, &s, tag);
            }
        }
        /* E. I/H-Folgen (Taster + Toggle). */
        {
            imap_state_t sl, sp;
            imap_snap_t s;
            static const int seq[][2] = {
                {'I', -1}, {-1, -1}, {'I', -1}, {-1, -1},
                {'H', -1}, {-1, -1}, {'H', -1}, {-1, -1},
                {'I', 'H'}, {'I', 'H'}, {-1, -1}
            };
            imap_state_init(&sl);
            imap_state_init(&sp);
            for (i = 0; i < 11; i++) {
                int ks[2], n = 0;
                if (seq[i][0] >= 0) ks[n++] = seq[i][0];
                if (seq[i][1] >= 0) ks[n++] = seq[i][1];
                make_snap(&s, frame++, c);
                set_keys(ks, n);
                cmp_poll(&sl, &sp, prof, &s, tag);
            }
        }
        /* F. Maus-Synth: Akku rauf/runter, mit Pfeil-Unterdrueckung. */
        {
            imap_state_t sl, sp;
            imap_snap_t s;
            imap_state_init(&sl);
            imap_state_init(&sp);
            for (k = 0; k < 40; k++) { /* +X-Anschlag */
                int vk = 320;
                make_snap(&s, frame++, c);
                set_keys(&vk, 1);
                cmp_poll(&sl, &sp, prof, &s, tag);
            }
            for (k = 0; k < 40; k++) { /* -X-Anschlag */
                int vk = 321;
                make_snap(&s, frame++, c);
                set_keys(&vk, 1);
                cmp_poll(&sl, &sp, prof, &s, tag);
            }
            for (k = 0; k < 30; k++) {
                int vk = 322;
                make_snap(&s, frame++, c);
                set_keys(&vk, 1);
                cmp_poll(&sl, &sp, prof, &s, tag);
            }
            for (k = 0; k < 30; k++) {
                int vk = 323;
                make_snap(&s, frame++, c);
                set_keys(&vk, 1);
                cmp_poll(&sl, &sp, prof, &s, tag);
            }
            { /* Pfeil schlaegt Maus (auch mit Shift auf Aiming). */
                int ks1[2] = {320, VK_LEFT};
                int ks2[3] = {320, VK_LEFT, VK_SHIFT};
                int ks3[2] = {322, VK_UP};
                make_snap(&s, frame++, c);
                set_keys(ks1, 2);
                cmp_poll(&sl, &sp, prof, &s, tag);
                make_snap(&s, frame++, c);
                set_keys(ks2, 3);
                cmp_poll(&sl, &sp, prof, &s, tag);
                make_snap(&s, frame++, c);
                set_keys(ks3, 2);
                cmp_poll(&sl, &sp, prof, &s, tag);
            }
            { /* Maustasten-Synth. */
                int ks[3] = {324, 325, 326};
                make_snap(&s, frame++, c);
                set_keys(ks, 3);
                cmp_poll(&sl, &sp, prof, &s, tag);
            }
            { /* Virtuals + echte Tasten gemischt. */
                int ks[3] = {260, 261, 'I'};
                make_snap(&s, frame++, c);
                set_keys(ks, 3);
                cmp_poll(&sl, &sp, prof, &s, tag);
            }
        }
    }
}

/* ---- Teil 3: Gamepad-Logik mit simulierten Zustaenden ---- */

typedef struct {
    const char *name;
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
    int keys[4];
    int nkeys;
    int polls; /* Polls mit diesem Zustand (fuer Flanken) */
} padcase_t;

static void run_padcases(const imap_profile_t *prof)
{
    imap_snap_t s;
    imap_state_t st;
    imap_block_t b;
    cfgvar_t c = {1, 1, 0, 1, 4, 0, 0};
    unsigned fr = 50000;
    char fb[160];

#define PADCASE(nm, btn, _lt, _rt, _lx, _ly, _rx, _ry) \
    {nm, btn, _lt, _rt, _lx, _ly, _rx, _ry, {0, 0, 0, 0}, 0, 1}
    static const padcase_t cases[] = {
        PADCASE("LX links voll", 0, 0, 0, -32768, 0, 0, 0),
        PADCASE("LX rechts voll", 0, 0, 0, 32767, 0, 0, 0),
        PADCASE("LY hoch voll", 0, 0, 0, 0, 32767, 0, 0),
        PADCASE("LY runter voll", 0, 0, 0, 0, -32768, 0, 0),
        PADCASE("RX rechts voll", 0, 0, 0, 0, 0, 32767, 0),
        PADCASE("RY hoch voll", 0, 0, 0, 0, 0, 0, 32767),
        PADCASE("RT voll", 0, 0, 255, 0, 0, 0, 0),
        PADCASE("RT halb (128)", 0, 0, 128, 0, 0, 0, 0),
        PADCASE("LT voll", 0, 255, 0, 0, 0, 0, 0),
        PADCASE("RB", IMAP_PAD_RB, 0, 0, 0, 0, 0, 0),
        PADCASE("LB", IMAP_PAD_LB, 0, 0, 0, 0, 0, 0),
        PADCASE("A", IMAP_PAD_A, 0, 0, 0, 0, 0, 0),
        PADCASE("B", IMAP_PAD_B, 0, 0, 0, 0, 0, 0),
        PADCASE("X", IMAP_PAD_X, 0, 0, 0, 0, 0, 0),
        PADCASE("Y", IMAP_PAD_Y, 0, 0, 0, 0, 0, 0),
        PADCASE("START", IMAP_PAD_START, 0, 0, 0, 0, 0, 0),
        PADCASE("L3", IMAP_PAD_L3, 0, 0, 0, 0, 0, 0),
        PADCASE("R3", IMAP_PAD_R3, 0, 0, 0, 0, 0, 0),
        PADCASE("D-Pad links", IMAP_PAD_DL, 0, 0, 0, 0, 0, 0),
        PADCASE("BACK allein", IMAP_PAD_BACK, 0, 0, 0, 0, 0, 0),
        PADCASE("LB+RB", IMAP_PAD_LB | IMAP_PAD_RB, 0, 0, 0, 0, 0, 0),
        PADCASE("BACK+A", IMAP_PAD_BACK | IMAP_PAD_A, 0, 0, 0, 0, 0, 0),
        PADCASE("BACK+Y", IMAP_PAD_BACK | IMAP_PAD_Y, 0, 0, 0, 0, 0, 0),
        PADCASE("BACK+R3", IMAP_PAD_BACK | IMAP_PAD_R3, 0, 0, 0, 0, 0, 0),
        PADCASE("BACK+DL", IMAP_PAD_BACK | IMAP_PAD_DL, 0, 0, 0, 0, 0, 0),
        PADCASE("BACK+DU", IMAP_PAD_BACK | IMAP_PAD_DU, 0, 0, 0, 0, 0, 0),
        PADCASE("LX in Totzone (2000)", 0, 0, 0, 2000, 0, 0, 0),
        PADCASE("LX halb (16384)", 0, 0, 0, 16384, 0, 0, 0),
        PADCASE("LY halb hoch (16384)", 0, 0, 0, 0, 16384, 0, 0),
    };
    int i;
    printf("--- Pad-Faelle (xbox_pad_default, simuliert) ---\n");
    for (i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++) {
        const padcase_t *pc = &cases[i];
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(pc->keys, pc->nkeys);
        s.pad_present = 1;
        s.pad.buttons = pc->buttons;
        s.pad.lt = pc->lt;
        s.pad.rt = pc->rt;
        s.pad.lx = pc->lx;
        s.pad.ly = pc->ly;
        s.pad.rx = pc->rx;
        s.pad.ry = pc->ry;
        imap_profile_apply(&b, &st, prof, &s);
        fmt_blk(fb, sizeof fb, &b);
        printf("PAD %-22s -> %s\n", pc->name, fb);
    }
#undef PADCASE

    /* Gezielte Pruefungen (Erwartungswerte von Hand gerechnet). */
    {
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(NULL, 0);
        s.pad_present = 1;
        s.pad.buttons = 0;
        s.pad.lt = s.pad.rt = 0;
        s.pad.lx = -32768;
        s.pad.ly = s.pad.rx = s.pad.ry = 0;
        imap_profile_apply(&b, &st, prof, &s);
        CHECK(b.aiming_x == 0x00, "LX links -> aim_x 0x00, ist 0x%02X",
              b.aiming_x);
    }
    {
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(NULL, 0);
        s.pad_present = 1;
        s.pad.buttons = 0;
        s.pad.lt = s.pad.rt = 0;
        s.pad.lx = 32767;
        s.pad.ly = s.pad.rx = s.pad.ry = 0;
        imap_profile_apply(&b, &st, prof, &s);
        CHECK(b.aiming_x == 0xFF, "LX rechts -> aim_x 0xFF, ist 0x%02X",
              b.aiming_x);
    }
    {
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(NULL, 0);
        s.pad_present = 1;
        s.pad.buttons = 0;
        s.pad.lt = s.pad.rt = 0;
        s.pad.ly = 32767;
        s.pad.lx = s.pad.rx = s.pad.ry = 0;
        imap_profile_apply(&b, &st, prof, &s);
        CHECK(b.aiming_y == 0x00, "LY hoch -> aim_y 0x00, ist 0x%02X",
              b.aiming_y);
    }
    {
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(NULL, 0);
        s.pad_present = 1;
        s.pad.buttons = 0;
        s.pad.lt = s.pad.rt = 0;
        s.pad.rx = 32767;
        s.pad.lx = s.pad.ly = s.pad.ry = 0;
        imap_profile_apply(&b, &st, prof, &s);
        CHECK(b.rot_lever == 127 && b.sight_x == 127,
              "RX rechts -> rot/sight_x 127, ist %d/%d",
              (int)b.rot_lever, (int)b.sight_x);
    }
    {
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(NULL, 0);
        s.pad_present = 1;
        s.pad.buttons = 0;
        s.pad.lt = 0;
        s.pad.rt = 255;
        s.pad.lx = s.pad.ly = s.pad.rx = s.pad.ry = 0;
        imap_profile_apply(&b, &st, prof, &s);
        CHECK(b.pedal_r == 0xFF, "RT voll -> pedal_r 0xFF, ist 0x%02X",
              b.pedal_r);
    }
    {
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(NULL, 0);
        s.pad_present = 1;
        s.pad.buttons = 0;
        s.pad.lt = 0;
        s.pad.rt = 128;
        s.pad.lx = s.pad.ly = s.pad.rx = s.pad.ry = 0;
        imap_profile_apply(&b, &st, prof, &s);
        CHECK(b.pedal_r == 0x80, "RT halb -> pedal_r 0x80, ist 0x%02X",
              b.pedal_r);
    }
    { /* LB+RB = nur Magazin (kein Doppelfeuer). */
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(NULL, 0);
        s.pad_present = 1;
        s.pad.buttons = IMAP_PAD_LB | IMAP_PAD_RB;
        s.pad.lt = s.pad.rt = 0;
        s.pad.lx = s.pad.ly = s.pad.rx = s.pad.ry = 0;
        imap_profile_apply(&b, &st, prof, &s);
        CHECK(b.buttons[1] == (1u << (27 - 16)) && b.buttons[0] == 0,
              "LB+RB -> nur bi27, ist %04X %04X", b.buttons[0],
              b.buttons[1]);
    }
    { /* BACK+Y = nur Eject (MainWC schweigt). */
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(NULL, 0);
        s.pad_present = 1;
        s.pad.buttons = IMAP_PAD_BACK | IMAP_PAD_Y;
        s.pad.lt = s.pad.rt = 0;
        s.pad.lx = s.pad.ly = s.pad.rx = s.pad.ry = 0;
        imap_profile_apply(&b, &st, prof, &s);
        CHECK(b.buttons[0] == (1u << 3) && b.buttons[1] == 0,
              "BACK+Y -> nur bi3, ist %04X %04X", b.buttons[0],
              b.buttons[1]);
    }
    { /* BACK allein: neutral, kein Gangwechsel. */
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(NULL, 0);
        s.pad_present = 1;
        s.pad.buttons = IMAP_PAD_BACK;
        s.pad.lt = s.pad.rt = 0;
        s.pad.lx = s.pad.ly = s.pad.rx = s.pad.ry = 0;
        imap_profile_apply(&b, &st, prof, &s);
        CHECK(b.buttons[0] == 0 && b.buttons[1] == 0 &&
                  b.buttons[2] == 0 && b.gear == 8,
              "BACK allein -> neutral");
    }
    { /* BACK+RB: Gang 8->9, kein Feuer. */
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(NULL, 0);
        s.pad_present = 1;
        s.pad.buttons = IMAP_PAD_BACK | IMAP_PAD_RB;
        s.pad.lt = s.pad.rt = 0;
        s.pad.lx = s.pad.ly = s.pad.rx = s.pad.ry = 0;
        imap_profile_apply(&b, &st, prof, &s);
        CHECK(b.gear == 9 && b.buttons[0] == 0,
              "BACK+RB -> Gang 9 ohne Feuer, ist Gang %u btn %04X",
              b.gear, b.buttons[0]);
    }
    { /* Totzone: LX=2000 bleibt neutral. */
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(NULL, 0);
        s.pad_present = 1;
        s.pad.buttons = 0;
        s.pad.lt = s.pad.rt = 0;
        s.pad.lx = 2000;
        s.pad.ly = s.pad.rx = s.pad.ry = 0;
        imap_profile_apply(&b, &st, prof, &s);
        CHECK(b.aiming_x == 0x7F, "LX=2000 -> neutral 0x7F, ist 0x%02X",
              b.aiming_x);
    }
    { /* Kurve exakt: LX=16384 -> 0xA1 (Handrechnung). */
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(NULL, 0);
        s.pad_present = 1;
        s.pad.buttons = 0;
        s.pad.lt = s.pad.rt = 0;
        s.pad.lx = 16384;
        s.pad.ly = s.pad.rx = s.pad.ry = 0;
        imap_profile_apply(&b, &st, prof, &s);
        CHECK(b.aiming_x == 0xA1, "LX=16384 -> 0xA1, ist 0x%02X",
              b.aiming_x);
    }
    { /* Invertierung exakt: LY=+16384 -> 0x5D (Handrechnung). */
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(NULL, 0);
        s.pad_present = 1;
        s.pad.buttons = 0;
        s.pad.lt = s.pad.rt = 0;
        s.pad.ly = 16384;
        s.pad.lx = s.pad.rx = s.pad.ry = 0;
        imap_profile_apply(&b, &st, prof, &s);
        CHECK(b.aiming_y == 0x5D, "LY=+16384 -> 0x5D, ist 0x%02X",
              b.aiming_y);
    }
    { /* Zusammenfuehrung: W + RT128 -> voll (digital gewinnt). */
        int kys[1] = {'W'};
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(kys, 1);
        s.pad_present = 1;
        s.pad.buttons = 0;
        s.pad.lt = 0;
        s.pad.rt = 128;
        s.pad.lx = s.pad.ly = s.pad.rx = s.pad.ry = 0;
        imap_profile_apply(&b, &st, prof, &s);
        CHECK(b.pedal_r == 0xFF, "W+RT128 -> 0xFF, ist 0x%02X",
              b.pedal_r);
    }
    { /* Max-Abs: LEFT + LX halb rechts -> digital (0x00). */
        int kys[1] = {VK_LEFT};
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(kys, 1);
        s.pad_present = 1;
        s.pad.buttons = 0;
        s.pad.lt = s.pad.rt = 0;
        s.pad.lx = 16384;
        s.pad.ly = s.pad.rx = s.pad.ry = 0;
        imap_profile_apply(&b, &st, prof, &s);
        CHECK(b.aiming_x == 0x00, "LEFT+LXhalb -> 0x00, ist 0x%02X",
              b.aiming_x);
    }
    { /* Max-Abs: LEFT + LX voll rechts -> Pad (+128 > -127). */
        int kys[1] = {VK_LEFT};
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(kys, 1);
        s.pad_present = 1;
        s.pad.buttons = 0;
        s.pad.lt = s.pad.rt = 0;
        s.pad.lx = 32767;
        s.pad.ly = s.pad.rx = s.pad.ry = 0;
        imap_profile_apply(&b, &st, prof, &s);
        CHECK(b.aiming_x == 0xFF, "LEFT+LXvoll -> 0xFF, ist 0x%02X",
              b.aiming_x);
    }
    { /* Ohne Geraet: Pad-Eingaben wirkungslos (selbst voll belegt). */
        imap_block_t ref;
        imap_state_t st2;
        int kys[2] = {VK_SPACE, 'W'};
        imap_state_init(&st);
        imap_state_init(&st2);
        make_snap(&s, fr++, &c);
        set_keys(kys, 2);
        s.pad_present = 0;
        s.pad.buttons = 0xFFFF;
        s.pad.lt = s.pad.rt = 255;
        s.pad.lx = s.pad.ly = s.pad.rx = s.pad.ry = 32767;
        imap_profile_apply(&b, &st, prof, &s);
        make_snap(&s, fr++, &c);
        set_keys(kys, 2);
        s.pad_present = 0;
        memset(&s.pad, 0, sizeof s.pad);
        imap_profile_apply(&ref, &st2, prof, &s);
        CHECK(memcmp(&b, &ref, sizeof b) == 0 &&
                  b.buttons[0] == 1 && b.pedal_r == 0xFF,
              "ohne Geraet wirkungslos");
    }
    { /* Direktwahl + Parser-Robustheit (eigener Profiltext). */
        static const char *txt =
            "# Kommentar\n"
            "[buttons]\n"
            "gear.1 = PAD:A\n"
            "gear.r = KEY:0x52\n"
            "tuner.up = KEY:T\n"
            "unsinn = KEY:X\n"
            "bi0 = KEY:SPACE, UNSINN:X\n"
            "ohne-gleichheitszeichen\n";
        imap_profile_t p;
        CHECK(imap_profile_parse(txt, "t", &p) == 0, "Parser ok");
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(NULL, 0);
        s.pad_present = 1;
        s.pad.buttons = IMAP_PAD_A;
        s.pad.lt = s.pad.rt = 0;
        s.pad.lx = s.pad.ly = s.pad.rx = s.pad.ry = 0;
        imap_profile_apply(&b, &st, &p, &s);
        CHECK(b.gear == 9, "gear.1 per PAD:A -> 9, ist %u", b.gear);
        CHECK(p.has_pad == 1, "has_pad gesetzt");
    }
    {
        imap_profile_t p;
        CHECK(imap_profile_builtin("gibtesnicht", &p) != 0,
              "unbekanntes Profil -> Fehler");
        CHECK(imap_profile_load("gibtesnicht", &p) != 0,
              "unbekannte Datei -> Fehler");
    }
}

static const char *axname(int ax)
{
    static const char *n[] = {
        "aim_x", "aim_y", "rot", "sight_x", "sight_y",
        "pedal_l", "pedal_m", "pedal_r"
    };
    return n[ax];
}

static void dump_bind(const char *pre, const imap_bind_t *b)
{
    int i;
    printf("%s n=%d", pre, b->n);
    for (i = 0; i < b->n; i++) {
        const imap_chord_t *c = &b->alt[i];
        printf(" [%d:%d%s%d:%d c=%d]", (int)c->a.kind, c->a.code,
               c->chord ? "+" : "", c->chord ? (int)c->b.kind : 0,
               c->chord ? c->b.code : 0, c->cond);
    }
    printf("\n");
}

static void dump_profile(const imap_profile_t *p)
{
    int i, k;
    printf("Profil '%s' has_pad=%d\n", p->name, p->has_pad);
    for (i = 0; i < 39; i++) {
        if (p->btn[i].n) {
            char pre[16];
            _snprintf_s(pre, sizeof pre, _TRUNCATE, "bi%d", i);
            dump_bind(pre, &p->btn[i]);
        }
    }
    for (i = 0; i < IMAP_AX_COUNT; i++) {
        const imap_axis_map_t *m = &p->axis[i];
        char pre[32];
        _snprintf_s(pre, sizeof pre, _TRUNCATE, "%s.neg", axname(i));
        dump_bind(pre, &m->neg);
        _snprintf_s(pre, sizeof pre, _TRUNCATE, "%s.pos", axname(i));
        dump_bind(pre, &m->pos);
        printf("%s mouse=%d mode=%d", axname(i), m->mouse,
               m->mouse_sight);
        for (k = 0; k < 2; k++)
            printf(" pad%d=%d,dz=%.2f,c=%.1f,inv=%d", k,
                   m->pad[k].src, m->pad[k].dz, m->pad[k].curve,
                   m->pad[k].inv);
        printf("\n");
    }
    dump_bind("gear.up", &p->gear_up);
    dump_bind("gear.down", &p->gear_down);
    dump_bind("tuner.up", &p->tuner_up);
    dump_bind("tuner.down", &p->tuner_down);
    if (p->vr_recenter.n) /* W159 */
        dump_bind("vr_recenter", &p->vr_recenter);
    if (p->pause.n) /* W188 */
        dump_bind("pause", &p->pause);
    if (p->photo.n) /* W195 */
        dump_bind("photo", &p->photo);
    for (i = 0; i < 7; i++) { /* W221: Direktwahl (z. B. Schubhebel-Zonen) */
        static const char *const gn[7] = {"gear.r", "gear.n", "gear.1", "gear.2",
                                          "gear.3", "gear.4", "gear.5"};
        if (p->gear_direct[i].n) dump_bind(gn[i], &p->gear_direct[i]);
    }
    if (p->joy_used) { /* W221 HOTAS */
        printf("joy_used=0x%X\n", p->joy_used);
        for (i = 0; i < IMAP_JOY_MAX; i++) {
            if (p->joy_used & (1u << i))
                printf("joy%d = '%s'%s\n", i, p->joy_dev[i],
                       p->joy_dev[i][0] ? "" : " (Slot-Reihenfolge)");
        }
    }
}

/* W79 INPUT-REAL: Kippschalter-Rast (key_toggles), Raw-Maus, Sperre. */
static void run_realcases(const imap_profile_t *xbox)
{
    imap_block_t b;
    imap_state_t st;
    imap_snap_t sn;
    cfgvar_t c = { 1, 1, 0, 1, 4, 0, 0 };
    int k1[] = { '1' }, k15[] = { '1', '5' };
    unsigned chg;

    printf("Teil 4: W79 Kippschalter/Raw-Maus\n");
    imap_state_init(&st);
    make_snap(&sn, 1, &c);
    sn.replay_active = 0;
    set_keys(k1, 1);
    imap_legacy_apply(&b, &st, &sn);
    chg = imap_toggles_apply(&b, &st, &sn);
    CHECK(chg == 1 && (b.buttons[2] & 0x4), "Taste 1 -> bi34 AN");
    set_keys(NULL, 0);
    imap_legacy_apply(&b, &st, &sn);
    chg = imap_toggles_apply(&b, &st, &sn);
    CHECK(chg == 0 && (b.buttons[2] & 0x4), "bi34 rastet nach Loslassen");
    set_keys(k15, 2);
    imap_legacy_apply(&b, &st, &sn);
    chg = imap_toggles_apply(&b, &st, &sn);
    CHECK(chg == 0x11 && !(b.buttons[2] & 0x4) && (b.buttons[2] & 0x40),
          "1+5: bi34 AUS, bi38 AN");
    /* Profil-Signal (Pad BACK+DL = bi34) klappt ebenfalls per Flanke. */
    imap_state_init(&st);
    make_snap(&sn, 2, &c);
    sn.replay_active = 0;
    set_keys(NULL, 0);
    sn.pad_present = 1;
    sn.pad.buttons = IMAP_PAD_BACK | IMAP_PAD_DL;
    imap_profile_apply(&b, &st, xbox, &sn);
    chg = imap_toggles_apply(&b, &st, &sn);
    sn.pad.buttons = 0;
    imap_profile_apply(&b, &st, xbox, &sn);
    imap_toggles_apply(&b, &st, &sn);
    CHECK(chg == 1 && (b.buttons[2] & 0x4), "Pad BACK+DL rastet bi34");
    /* Raw-Maus: Deltas wirken, gesperrt nicht. */
    imap_state_init(&st);
    make_snap(&sn, 3, &c);
    sn.replay_active = 0;
    sn.mouse_raw = 1;
    sn.raw_dx = 10;
    sn.raw_dy = -6;
    imap_legacy_apply(&b, &st, &sn);
    CHECK(b.aiming_x == 0x7F + 10 && b.aiming_y == 0x7F - 6,
          "Raw-Delta -> aim (%02X%02X)", b.aiming_x, b.aiming_y);
    sn.real_blocked = 1;
    sn.frame = 4;
    imap_legacy_apply(&b, &st, &sn);
    CHECK(b.aiming_x == 0x7F + 10 && b.aiming_y == 0x7F - 6,
          "gesperrt -> Akku unveraendert");

    /* W87 mouse_abs: kleine Deltas gehen nicht verloren (sens=16:
     * 1 Count = 16/64 = 0,25 Hebelstufen; relativ: 16/4 = 4 je Count). */
    printf("Teil 5: W87 Maus absolut (Akku-Bruchteile, Quelle)\n");
    {
        int i;
        imap_state_t sa, sr;
        imap_snap_t na;
        cfgvar_t c1 = { 1, 1, 0, 1, 16, 0, 0 };
        imap_state_init(&sa);
        imap_state_init(&sr);
        make_snap(&na, 10, &c1);
        na.replay_active = 0;
        na.mouse_raw = 1;
        na.raw_dx = 1;
        na.raw_dy = -1;
        set_keys(NULL, 0);
        for (i = 0; i < 8; i++) {
            na.frame = 10 + (unsigned)i;
            na.mouse_abs = 1;
            imap_legacy_apply(&b, &sa, &na);
            na.mouse_abs = 0;
            na.mouse_sens = 1;
            imap_legacy_apply(&b, &sr, &na);
            na.mouse_sens = 16;
        }
        CHECK(sa.m_ax == 0x7F + 2 && sa.m_ay == 0x7F - 2 &&
              sa.m_fx == 0.0f && sa.m_fy == 0.0f,
              "absolut: 8x1 Count (sens 16) = +2 Stufen (%d,%d)",
              sa.m_ax, sa.m_ay);
        CHECK(sr.m_ax == 0x7F && sr.m_ay == 0x7F,
              "relativ unveraendert: 8x1 Count (sens 1) = 0 (alt, bitgenau)");
        CHECK(sa.m_aim_src == 3, "Legacy: Maus ist Quelle beider Achsen");
        /* Pfeiltaste schlaegt die Maus -> Achse nicht vorsteuern. */
        {
            int kl[] = { VK_LEFT };
            set_keys(kl, 1);
            na.mouse_abs = 1;
            na.frame = 30;
            imap_legacy_apply(&b, &sa, &na);
            CHECK(sa.m_aim_src == 2 && b.aiming_x == 0x00,
                  "LEFT: aiming_x digital, nur Y von der Maus");
            set_keys(NULL, 0);
        }
        /* Profil-Pfad: Maus Quelle, auch bei mittigem Akku; Pad gewinnt. */
        imap_state_init(&sa);
        make_snap(&na, 40, &c1);
        na.replay_active = 0;
        na.mouse_abs = 1;
        na.mouse_raw = 1; /* deterministisch (kein GetCursorPos) */
        imap_profile_apply(&b, &sa, xbox, &na);
        CHECK(sa.m_aim_src == 3, "Profil: Maus Quelle bei Akku Mitte");
        na.pad_present = 1;
        na.pad.lx = 30000;
        na.frame = 41;
        imap_profile_apply(&b, &sa, xbox, &na);
        CHECK((sa.m_aim_src & 1) == 0, "Profil: Pad-Stick schlaegt die Maus (X)");
    }
}

/* W110 PEDAL-L: Slide-Step (linkes Pedal) - LCtrl, Maus X1/X2,
 * Pad BACK+LT. Explizite Erwartungswerte (Teil 2 prueft zusaetzlich
 * Legacy<->Profil-Gleichheit ueber alle neuen Tasten). */
static void run_pedal_cases(const imap_profile_t *kb,
                            const imap_profile_t *xbox)
{
    imap_block_t b;
    imap_state_t st;
    imap_snap_t sn;
    cfgvar_t c = { 1, 1, 0, 1, 4, 0, 0 };
    unsigned fr = 90000;
    int kl[] = { VK_LCONTROL };
    int kr[] = { VK_RCONTROL };
    int kc[] = { VK_CONTROL };
    int kla[] = { VK_LCONTROL, 'A' };
    int kx1[] = { 327 }, kx2[] = { 328 };

    printf("Teil 7: W110 Pedal-L (Slide-Step)\n");
    /* LCtrl -> pedal_l voll (Legacy + Profil). */
    imap_state_init(&st);
    make_snap(&sn, fr++, &c);
    set_keys(kl, 1);
    imap_legacy_apply(&b, &st, &sn);
    CHECK(b.pedal_l == 0xFF && b.pedal_m == 0 && b.pedal_r == 0,
          "Legacy LCtrl -> pedal_l 0xFF (ist %02X)", b.pedal_l);
    imap_state_init(&st);
    make_snap(&sn, fr++, &c);
    set_keys(kl, 1);
    imap_profile_apply(&b, &st, kb, &sn);
    CHECK(b.pedal_l == 0xFF, "Profil LCtrl -> pedal_l 0xFF (ist %02X)",
          b.pedal_l);
    /* Nur links: RCtrl und generisches CTRL geben kein Slide. */
    imap_state_init(&st);
    make_snap(&sn, fr++, &c);
    set_keys(kr, 1);
    imap_legacy_apply(&b, &st, &sn);
    CHECK(b.pedal_l == 0, "Legacy RCtrl -> kein Slide (ist %02X)",
          b.pedal_l);
    imap_state_init(&st);
    make_snap(&sn, fr++, &c);
    set_keys(kc, 1);
    imap_profile_apply(&b, &st, kb, &sn);
    CHECK(b.pedal_l == 0, "Profil CTRL -> kein Slide (ist %02X)",
          b.pedal_l);
    /* Slide links: LCtrl + A -> Pedal voll + Drehhebel links. */
    imap_state_init(&st);
    make_snap(&sn, fr++, &c);
    set_keys(kla, 2);
    imap_profile_apply(&b, &st, kb, &sn);
    CHECK(b.pedal_l == 0xFF && b.rot_lever == -127,
          "LCtrl+A -> Slide links (ped=%02X rot=%d)", b.pedal_l,
          (int)b.rot_lever);
    /* Maus-Seitentasten (Replay-Synth 327/328 = X1/X2). */
    imap_state_init(&st);
    make_snap(&sn, fr++, &c);
    set_keys(kx1, 1);
    imap_legacy_apply(&b, &st, &sn);
    CHECK(b.pedal_l == 0xFF, "Legacy X1 -> pedal_l 0xFF (ist %02X)",
          b.pedal_l);
    imap_state_init(&st);
    make_snap(&sn, fr++, &c);
    set_keys(kx2, 1);
    imap_profile_apply(&b, &st, kb, &sn);
    CHECK(b.pedal_l == 0xFF, "Profil X2 -> pedal_l 0xFF (ist %02X)",
          b.pedal_l);
    { /* mouse_buttons=0: Seitentasten wirken nicht. */
        cfgvar_t c0 = { 1, 1, 0, 0, 4, 0, 0 };
        imap_state_init(&st);
        make_snap(&sn, fr++, &c0);
        set_keys(kx1, 1);
        imap_profile_apply(&b, &st, kb, &sn);
        CHECK(b.pedal_l == 0, "X1 ohne mouse_buttons -> 0 (ist %02X)",
              b.pedal_l);
    }
    /* Pad BACK+LT -> Slide (xbox_pad_default); LT allein = nur Bremse. */
    imap_state_init(&st);
    make_snap(&sn, fr++, &c);
    set_keys(NULL, 0);
    sn.pad_present = 1;
    sn.pad.buttons = IMAP_PAD_BACK;
    sn.pad.lt = 255;
    imap_profile_apply(&b, &st, xbox, &sn);
    CHECK(b.pedal_l == 0xFF, "BACK+LT -> pedal_l 0xFF (ist %02X)",
          b.pedal_l);
    imap_state_init(&st);
    make_snap(&sn, fr++, &c);
    set_keys(NULL, 0);
    sn.pad_present = 1;
    sn.pad.buttons = 0;
    sn.pad.lt = 255;
    imap_profile_apply(&b, &st, xbox, &sn);
    CHECK(b.pedal_l == 0 && b.pedal_m == 0xFF,
          "LT allein -> nur Bremse (l=%02X m=%02X)", b.pedal_l,
          b.pedal_m);
    imap_state_init(&st);
    make_snap(&sn, fr++, &c);
    set_keys(NULL, 0);
    sn.pad_present = 1;
    sn.pad.buttons = IMAP_PAD_BACK;
    sn.pad.rt = 255;
    imap_profile_apply(&b, &st, xbox, &sn);
    CHECK(b.pedal_l == 0, "BACK+RT -> kein Slide (ist %02X)", b.pedal_l);
}

/* W92 INPUT-TAPS: Tap-Warteschlange je Taste. */
static void run_tapcases(void)
{
    int i, got[8];
    printf("Teil 6: W92 Tap-Queue (3 Tipper, Halten, Repeat, Deckel)\n");
    /* A. 3 Tipper zwischen zwei Polls -> 3 Druck/Los-Paare. */
    itap_init();
    itap_poll_begin();
    CHECK(itap_eval(VK_RIGHT, 0) == 0, "tap: idle 0");
    itap_down_edge(VK_RIGHT); itap_up_edge(VK_RIGHT);
    itap_down_edge(VK_RIGHT); itap_up_edge(VK_RIGHT);
    itap_down_edge(VK_RIGHT); itap_up_edge(VK_RIGHT);
    CHECK(itap_pending(VK_RIGHT) == 3, "tap: 3 Tipper angestaut");
    for (i = 0; i < 6; i++) {
        itap_poll_begin();
        got[i] = itap_eval(VK_RIGHT, 0);
        /* Mehrfach-Abfrage im selben Poll: derselbe Wert, ein Schritt. */
        CHECK(itap_eval(VK_RIGHT, 0) == got[i], "tap: Poll %d idempotent", i);
    }
    CHECK(got[0]==1 && got[1]==0 && got[2]==1 && got[3]==0 && got[4]==1 && got[5]==0,
          "tap: 3x 1/0 (ist %d%d%d%d%d%d)",
          got[0], got[1], got[2], got[3], got[4], got[5]);
    itap_poll_begin();
    CHECK(itap_eval(VK_RIGHT, 0) == 0, "tap: danach idle 0");
    /* B. Gehaltene Taste: Dauer-an, kein Nachtrag nach Loslassen. */
    itap_init();
    itap_poll_begin();
    CHECK(itap_eval(VK_SPACE, 0) == 0, "tap: halten idle 0");
    itap_down_edge(VK_SPACE);
    itap_poll_begin();
    CHECK(itap_eval(VK_SPACE, 1) == 1, "tap: halten Poll 1");
    itap_poll_begin();
    CHECK(itap_eval(VK_SPACE, 1) == 1, "tap: halten Poll 2 (keine Luecke)");
    itap_poll_begin();
    CHECK(itap_eval(VK_SPACE, 1) == 1, "tap: halten Poll 3");
    itap_up_edge(VK_SPACE); /* live gesehen -> kein Phantom */
    CHECK(itap_pending(VK_SPACE) == 0, "tap: Halten staut nichts an");
    itap_poll_begin();
    CHECK(itap_eval(VK_SPACE, 0) == 0, "tap: nach Loslassen 0");
    /* C. Autowiederholung zaehlt nicht (doppelte down-Flanke -> 1 Tipp). */
    itap_init();
    itap_poll_begin();
    (void)itap_eval(0x43, 0);
    itap_down_edge(0x43); /* C runter */
    itap_down_edge(0x43); /* Repeat (lp 0x40000000): Aufrufer filtert, Modul ist idempotent */
    itap_down_edge(0x43);
    itap_up_edge(0x43);
    CHECK(itap_pending(0x43) == 1, "tap: Repeat = 1 Tipp");
    /* D. Deckel 8 + Maustaste links im selben Pfad. */
    itap_init();
    itap_poll_begin();
    (void)itap_eval(VK_LBUTTON, 0);
    for (i = 0; i < 10; i++) { itap_down_edge(VK_LBUTTON); itap_up_edge(VK_LBUTTON); }
    CHECK(itap_pending(VK_LBUTTON) == 8, "tap: Deckel 8 (10 Tipper)");
    for (i = 0; i < 16; i++) { itap_poll_begin(); got[i % 8] = itap_eval(VK_LBUTTON, 0); }
    CHECK(got[0]==1 && got[1]==0, "tap: Maus spielt 1/0 aus");
    itap_poll_begin();
    CHECK(itap_eval(VK_LBUTTON, 0) == 0, "tap: Maus danach 0");
    /* E. Live-Druck + angestauter Tipp -> erst Luecke, dann Tipp. */
    itap_init();
    itap_poll_begin();
    (void)itap_eval(0x45, 0);
    itap_down_edge(0x45);
    itap_poll_begin();
    CHECK(itap_eval(0x45, 1) == 1, "tap: live 1");
    itap_up_edge(0x45); /* gesehen -> kein Nachtrag */
    itap_down_edge(0x45); itap_up_edge(0x45); /* 1 Tipp zwischen Polls */
    itap_poll_begin();
    CHECK(itap_eval(0x45, 0) == 0, "tap: Luecke nach live");
    itap_poll_begin();
    CHECK(itap_eval(0x45, 0) == 1, "tap: angespielter Tipp 1");
    itap_poll_begin();
    CHECK(itap_eval(0x45, 0) == 0, "tap: danach 0");
    /* F. Fokusverlust verwirft (itap_clear). */
    itap_down_edge(VK_RIGHT); itap_up_edge(VK_RIGHT);
    itap_clear();
    CHECK(itap_pending(VK_RIGHT) == 0, "tap: clear verwirft");
}


/* ---- Teil 8: W128 Arbitrierung SBC gegen Maus ---- */

/* Ein Poll: Maus (absolut) liefert aim_x = mx (Akku mx), SBC liefert sx. */
static int arb_poll(imap_arb_t *a, int mx, int sx, unsigned src)
{
    imap_arb_in_t in;
    int out[IMAP_ARB_N];
    memset(&in, 0, sizeof in);
    in.cur[0] = mx; in.cur[1] = 0x7F; in.cur[2] = 0; in.cur[3] = 0;
    in.idle[0] = 0x7F; in.idle[1] = 0x7F;
    in.mouse_src = src;
    in.mpos[0] = (float)mx; in.mpos[1] = 127.0f;
    in.sbc[0] = sx; in.sbc[1] = 0x80;
    in.sbc_center[0] = 0x80; in.sbc_center[1] = 0x80;
    in.dead = 8;
    in.wake = 3.0f;
    imap_arb_axes(a, &in, out);
    return out[0];
}

static void run_arb_cases(void)
{
    imap_arb_t a;
    int v, i;
    printf("Teil 8: W128 Arbitrierung SBC gegen Maus\n");
    memset(&a, 0, sizeof a);
    /* Maus absolut haelt eine Stellung (0x60), SBC ruht mittig -> Maus. */
    v = arb_poll(&a, 0x60, 0x80, 3);
    CHECK(v == 0x60, "arb: SBC ruhig, Maus-Stellung bleibt (%02X)", v);
    /* SBC-Hebel bewegt (0xF0) -> SBC-Wert im Block, obwohl die Maus liefert. */
    v = arb_poll(&a, 0x60, 0xF0, 3);
    CHECK(v == 0xF0, "arb: Hebel bewegt -> SBC (%02X)", v);
    /* Maus ruhig, Hebel bleibt -> SBC behaelt. */
    for (i = 0; i < 5; i++) v = arb_poll(&a, 0x60, 0xF0, 3);
    CHECK(v == 0xF0, "arb: SBC behaelt bei ruhiger Maus (%02X)", v);
    /* Maus zittert 1 Stufe (< wake 3) -> SBC behaelt. */
    v = arb_poll(&a, 0x61, 0xF0, 3);
    CHECK(v == 0xF0, "arb: Maus-Zittern < wake -> SBC (%02X)", v);
    /* Maus bewegt sich deutlich (+10) -> Maus zurueck. */
    v = arb_poll(&a, 0x6B, 0xF0, 3);
    CHECK(v == 0x6B, "arb: Maus bewegt -> Maus (%02X)", v);
    /* SBC bleibt ausgelenkt (unveraendert) -> Maus behaelt (kein Zurueckspringen). */
    for (i = 0; i < 5; i++) v = arb_poll(&a, 0x6B, 0xF0, 3);
    CHECK(v == 0x6B, "arb: SBC unveraendert ausgelenkt -> Maus behaelt (%02X)", v);
    /* SBC-Rauschen innerhalb dead (0xF0 -> 0xF6) -> Maus behaelt. */
    v = arb_poll(&a, 0x6B, 0xF6, 3);
    CHECK(v == 0x6B, "arb: SBC-Rauschen <= dead -> Maus (%02X)", v);
    /* Erneut Hebel (0x20) -> SBC. */
    v = arb_poll(&a, 0x6B, 0x20, 3);
    CHECK(v == 0x20, "arb: erneut Hebel -> SBC (%02X)", v);
    /* Hebel aktiv + gleichzeitiges Maus-Zittern: Hebelbewegung setzt die
     * Maussumme zurueck (Hysterese) -> SBC. */
    v = arb_poll(&a, 0x6D, 0x40, 3);
    v = arb_poll(&a, 0x6F, 0x60, 3);
    CHECK(v == 0x60, "arb: Hebel aktiv schlaegt Maus-Zittern (%02X)", v);
    /* Tastatur/Pfeile (keine Maus-Quelle): alte Regel - Taste gewinnt. */
    v = arb_poll(&a, 0x00, 0x60, 0);
    CHECK(v == 0x00, "arb: Pfeiltaste gewinnt (%02X)", v);
    /* Keine Maus-Quelle + Block im Leerlauf -> SBC (alte Regel W75). */
    v = arb_poll(&a, 0x7F, 0x60, 0);
    CHECK(v == 0x60, "arb: Leerlauf -> SBC (%02X)", v);
    /* Achse Y (nicht ausgelenkt, Maus-Quelle) bleibt Maus 0x7F. */
    {
        imap_arb_in_t in;
        int out[IMAP_ARB_N];
        imap_arb_t b;
        memset(&b, 0, sizeof b);
        memset(&in, 0, sizeof in);
        in.cur[1] = 0x7F; in.idle[1] = 0x7F; in.mouse_src = 2;
        in.mpos[1] = 127.0f; in.sbc[1] = 0x80; in.sbc_center[1] = 0x80;
        in.dead = 8; in.wake = 3.0f;
        imap_arb_axes(&b, &in, out);
        CHECK(out[1] == 0x7F, "arb: Y ruhig -> Maus-Mitte (%02X)", out[1]);
        /* Ruhewerte am Blech (Nutzer 5.11.: aim 0x76/0x7C): Abweichung
         * 10 > dead 8 -> SBC uebernimmt beim Start (ohne Kalibrierung). */
        in.sbc[1] = 0x76;
        imap_arb_axes(&b, &in, out);
        CHECK(out[1] == 0x76, "arb: Ruhe 0x76 unkalibriert -> SBC (%02X)", out[1]);
        /* Maus bewegt -> Maus; Ruhe 0x76 bleibt danach ohne Wirkung. */
        in.mpos[1] = 140.0f; in.cur[1] = 140;
        imap_arb_axes(&b, &in, out);
        CHECK(out[1] == 140, "arb: Maus holt Y zurueck (%d)", out[1]);
        for (i = 0; i < 3; i++) imap_arb_axes(&b, &in, out);
        CHECK(out[1] == 140, "arb: Ruhe 0x76 danach ohne Wirkung (%d)", out[1]);
    }
    /* NULL-tolerant */
    imap_arb_axes(NULL, NULL, NULL);
}

/* ---- Teil 9: W132 Arbitrierung Pad gegen Maus (Profil-Pfad) ----
 * Simulierte XInput-Zustaende (wie Teil 3) + absolute Maus (Raw-Deltas,
 * deterministisch; sens 16: 64 Counts = 16 Stufen, 8 = 2, 12 = 3).
 * Vollausschlag LX: o = +-128/-127 (kurvenunabhaengig, exakt). */

static void padarb_poll(const imap_profile_t *prof, imap_state_t *st,
                        imap_snap_t *s, imap_block_t *b, unsigned *fr,
                        int16_t lx, int rdx)
{
    s->frame = (*fr)++;
    s->pad.lx = lx;
    s->raw_dx = rdx;
    s->raw_dy = 0;
    imap_profile_apply(b, st, prof, s);
}

static void run_padarb_cases(const imap_profile_t *prof)
{
    imap_state_t st;
    imap_snap_t s;
    imap_block_t b;
    cfgvar_t c = { 1, 1, 0, 0, 16, 0, 0 }; /* Maus an, Tasten aus, sens 16 */
    unsigned fr = 120000;
    int kr[] = { VK_RIGHT };

    printf("Teil 9: W132 Arbitrierung Pad gegen Maus\n");
    imap_state_init(&st);
    make_snap(&s, fr++, &c);
    set_keys(NULL, 0);
    s.replay_active = 0;
    s.mouse_raw = 1; /* deterministisch (kein GetCursorPos) */
    s.mouse_abs = 1; /* absolute Maus (nie neutral) */
    s.pad_axis_arb = 1;
    s.pad_arb_dead = 8;
    s.pad_mouse_wake = 3;
    s.pad_present = 1;
    memset(&s.pad, 0, sizeof s.pad);

    /* Ruhe: Maus-Mitte im Block, Maus ist Quelle. */
    padarb_poll(prof, &st, &s, &b, &fr, 0, 0);
    CHECK(b.aiming_x == 0x7F && (st.m_aim_src & 1),
          "padarb: Ruhe -> Maus-Mitte 0x7F (ist 0x%02X, src=%u)",
          b.aiming_x, st.m_aim_src & 1u);
    /* Maus bewegt (+16): Maus-Wert im Block ... */
    padarb_poll(prof, &st, &s, &b, &fr, 0, 64);
    CHECK(b.aiming_x == 0x8F, "padarb: Maus +16 -> 0x8F (ist 0x%02X)",
          b.aiming_x);
    /* ... und bleibt stehen (absolute Maus, nie neutral). */
    padarb_poll(prof, &st, &s, &b, &fr, 0, 0);
    padarb_poll(prof, &st, &s, &b, &fr, 0, 0);
    CHECK(b.aiming_x == 0x8F, "padarb: Maus-Stellung bleibt (ist 0x%02X)",
          b.aiming_x);
    /* Stick bewegt -> Pad-Wert im Block, Maus ist nicht mehr Quelle. */
    padarb_poll(prof, &st, &s, &b, &fr, 32767, 0);
    CHECK(b.aiming_x == 0xFF && !(st.m_aim_src & 1),
          "padarb: Stick -> Pad 0xFF (ist 0x%02X, src=%u)",
          b.aiming_x, st.m_aim_src & 1u);
    /* Stick gehalten, Maus ruhig -> Pad behaelt. */
    padarb_poll(prof, &st, &s, &b, &fr, 32767, 0);
    CHECK(b.aiming_x == 0xFF, "padarb: Pad behaelt (ist 0x%02X)",
          b.aiming_x);
    /* Maus-Zittern (+2 < wake 3) -> Pad behaelt (Hysterese). */
    padarb_poll(prof, &st, &s, &b, &fr, 32767, 8);
    CHECK(b.aiming_x == 0xFF, "padarb: Zittern < wake -> Pad (ist 0x%02X)",
          b.aiming_x);
    /* Maus bewegt (+3, Summe 5 >= wake) -> Maus-Wert im Block. */
    padarb_poll(prof, &st, &s, &b, &fr, 32767, 12);
    CHECK(b.aiming_x == 0x94 && (st.m_aim_src & 1),
          "padarb: Maus bewegt -> Maus 0x94 (ist 0x%02X, src=%u)",
          b.aiming_x, st.m_aim_src & 1u);
    /* Pad weiter ausgelenkt, aber unveraendert -> kein Zurueckspringen. */
    padarb_poll(prof, &st, &s, &b, &fr, 32767, 0);
    padarb_poll(prof, &st, &s, &b, &fr, 32767, 0);
    CHECK(b.aiming_x == 0x94, "padarb: Pad unveraendert -> Maus (ist 0x%02X)",
          b.aiming_x);
    /* Erneut Stick (andere Seite) -> Pad. */
    padarb_poll(prof, &st, &s, &b, &fr, -32768, 0);
    CHECK(b.aiming_x == 0x00, "padarb: erneut Stick -> Pad (ist 0x%02X)",
          b.aiming_x);
    /* Digitalachse wie bisher: Pfeil gewinnt, danach wieder Pad. */
    set_keys(kr, 1);
    padarb_poll(prof, &st, &s, &b, &fr, -32768, 0);
    CHECK(b.aiming_x == 0xFF, "padarb: Pfeil gewinnt (ist 0x%02X)",
          b.aiming_x);
    set_keys(NULL, 0);
    padarb_poll(prof, &st, &s, &b, &fr, -32768, 0);
    CHECK(b.aiming_x == 0x00, "padarb: nach Pfeil wieder Pad (ist 0x%02X)",
          b.aiming_x);
    /* Stick losgelassen -> Pad-Mitte im Block (Maus parkt, W128-Semantik). */
    padarb_poll(prof, &st, &s, &b, &fr, 0, 0);
    CHECK(b.aiming_x == 0x7F && !(st.m_aim_src & 1),
          "padarb: Stick los -> Pad-Mitte (ist 0x%02X, src=%u)",
          b.aiming_x, st.m_aim_src & 1u);
    /* Maus bewegt (+3 >= wake) -> Maus zurueck. */
    padarb_poll(prof, &st, &s, &b, &fr, 0, 12);
    CHECK(b.aiming_x == 0x97, "padarb: Maus holt zurueck (ist 0x%02X)",
          b.aiming_x);

    /* Alte Regel (pad_axis_arb=0): statisch ausgelenkter Stick gewinnt
     * per Max-Abs gegen die mittige Maus ... */
    imap_state_init(&st);
    make_snap(&s, fr++, &c);
    set_keys(NULL, 0);
    s.replay_active = 0;
    s.mouse_raw = 1;
    s.mouse_abs = 1;
    s.pad_axis_arb = 0;
    s.pad_present = 1;
    memset(&s.pad, 0, sizeof s.pad);
    padarb_poll(prof, &st, &s, &b, &fr, 32767, 0);
    CHECK(b.aiming_x == 0xFF, "padarb: alt, Stick statisch -> Pad (ist 0x%02X)",
          b.aiming_x);
    /* ... mit Arbitrierung gewinnt die ruhige Maus, bis sich der Stick
     * bewegt (gleiche Logik wie W128). */
    imap_state_init(&st);
    make_snap(&s, fr++, &c);
    set_keys(NULL, 0);
    s.replay_active = 0;
    s.mouse_raw = 1;
    s.mouse_abs = 1;
    s.pad_axis_arb = 1;
    s.pad_arb_dead = 8;
    s.pad_mouse_wake = 3;
    s.pad_present = 1;
    memset(&s.pad, 0, sizeof s.pad);
    padarb_poll(prof, &st, &s, &b, &fr, 32767, 0);
    CHECK(b.aiming_x == 0x7F, "padarb: neu, Stick statisch -> Maus (ist 0x%02X)",
          b.aiming_x);
    /* Ohne Pad: bitgleich wie bisher (Maus-Wert, alte Regel). */
    imap_state_init(&st);
    make_snap(&s, fr++, &c);
    set_keys(NULL, 0);
    s.replay_active = 0;
    s.mouse_raw = 1;
    s.mouse_abs = 1;
    s.pad_axis_arb = 1;
    s.pad_present = 0;
    padarb_poll(prof, &st, &s, &b, &fr, 0, 64);
    CHECK(b.aiming_x == 0x8F, "padarb: ohne Pad -> Maus (ist 0x%02X)",
          b.aiming_x);
    /* Maus-Achse aus (pad_mouse_off-Wirkung): Pad gewinnt per Max-Abs. */
    imap_state_init(&st);
    make_snap(&s, fr++, &c);
    set_keys(NULL, 0);
    s.replay_active = 0;
    s.mouse_raw = 1;
    s.mouse_abs = 1;
    s.mouse_aim = 0;
    s.pad_axis_arb = 1;
    s.pad_present = 1;
    memset(&s.pad, 0, sizeof s.pad);
    padarb_poll(prof, &st, &s, &b, &fr, 32767, 0);
    CHECK(b.aiming_x == 0xFF, "padarb: Maus aus -> Pad (ist 0x%02X)",
          b.aiming_x);

    /* Sight-Achse (Slots 2/3): Maus in den Sight-Modus, RX bewegt. */
    {
        cfgvar_t cs = { 1, 1, 1, 0, 16, 0, 0 };
        imap_state_init(&st);
        make_snap(&s, fr++, &cs);
        set_keys(NULL, 0);
        s.replay_active = 0;
        s.mouse_raw = 1;
        s.mouse_abs = 1;
        s.pad_axis_arb = 1;
        s.pad_arb_dead = 8;
        s.pad_mouse_wake = 3;
        s.pad_present = 1;
        memset(&s.pad, 0, sizeof s.pad);
        s.frame = fr++;
        s.raw_dx = 64;
        imap_profile_apply(&b, &st, prof, &s);
        CHECK(b.sight_x == 16, "padarb: Sight-Maus +16 (ist %d)",
              (int)b.sight_x);
        s.frame = fr++;
        s.raw_dx = 0;
        s.pad.rx = 32767;
        imap_profile_apply(&b, &st, prof, &s);
        CHECK(b.sight_x == 127, "padarb: RX -> Pad-Sight (ist %d)",
              (int)b.sight_x);
        s.frame = fr++;
        s.raw_dx = 12;
        imap_profile_apply(&b, &st, prof, &s);
        CHECK(b.sight_x == 19, "padarb: Sight-Maus zurueck (ist %d)",
              (int)b.sight_x);
    }
    /* NULL-tolerant (Maus faellt durch). */
    CHECK(imap_arb_pad1(NULL, 7, 9, 0.0f, 8, 3.0f, NULL) == 7,
          "padarb: NULL -> Maus");
}

/* ---- Teil 11: W143 globale Pad-Totzonen (Untergrenze ueber Profil) ----
 * xbox_pad_default: aim_x = PAD:LX dz=0.15, pedal_m = PAD:LT dz=0.05.
 * pad_dz_* = 0 laesst das Profil unveraendert; groessere Werte schneiden
 * mehr ab (max), kleinere aendern nichts. */
static void run_paddz_cases(const imap_profile_t *prof)
{
    imap_snap_t s;
    imap_state_t st;
    imap_block_t b;
    cfgvar_t c = { 1, 0, 0, 0, 4, 0, 0 }; /* Maus aus (nur Pad) */
    unsigned fr = 200000;
    int v0, v1;

    printf("Teil 11: W143 globale Pad-Totzonen\n");
    /* LX halb (0.5), ohne Floor: Profil-dz 0.15 -> rechts von Mitte. */
    imap_state_init(&st);
    make_snap(&s, fr++, &c);
    set_keys(NULL, 0);
    s.pad_present = 1;
    memset(&s.pad, 0, sizeof s.pad);
    s.pad.lx = 16384;
    imap_profile_apply(&b, &st, prof, &s);
    v0 = b.aiming_x;
    CHECK(v0 > 0x7F, "paddz: LX 0.5 ohne Floor rechts (ist 0x%02X)", v0);
    /* Gleicher Stick, Floor 0.6: in der Totzone -> Mitte. */
    imap_state_init(&st);
    make_snap(&s, fr++, &c);
    set_keys(NULL, 0);
    s.pad_present = 1;
    memset(&s.pad, 0, sizeof s.pad);
    s.pad.lx = 16384;
    s.pad_dz_stick = 0.6f;
    imap_profile_apply(&b, &st, prof, &s);
    CHECK(b.aiming_x == 0x7F, "paddz: LX 0.5 Floor 0.6 -> Mitte (ist 0x%02X)",
          b.aiming_x);
    /* Floor 0.4 (< 0.5): reskaliert, kleiner als ohne Floor. */
    imap_state_init(&st);
    make_snap(&s, fr++, &c);
    set_keys(NULL, 0);
    s.pad_present = 1;
    memset(&s.pad, 0, sizeof s.pad);
    s.pad.lx = 16384;
    s.pad_dz_stick = 0.4f;
    imap_profile_apply(&b, &st, prof, &s);
    v1 = b.aiming_x;
    CHECK(v1 > 0x7F && v1 < v0,
          "paddz: LX 0.5 Floor 0.4 -> 0x7F < 0x%02X < 0x%02X", v1, v0);
    /* Floor unter Profil-dz (0.1 < 0.15): Profil gewinnt (gleich v0). */
    imap_state_init(&st);
    make_snap(&s, fr++, &c);
    set_keys(NULL, 0);
    s.pad_present = 1;
    memset(&s.pad, 0, sizeof s.pad);
    s.pad.lx = 16384;
    s.pad_dz_stick = 0.1f;
    imap_profile_apply(&b, &st, prof, &s);
    CHECK(b.aiming_x == v0, "paddz: Floor 0.1 -> Profil (0x%02X == 0x%02X)",
          b.aiming_x, v0);
    /* Trigger: LT halb (0.5), Profil-dz 0.05 -> Pedal tritt. */
    imap_state_init(&st);
    make_snap(&s, fr++, &c);
    set_keys(NULL, 0);
    s.pad_present = 1;
    memset(&s.pad, 0, sizeof s.pad);
    s.pad.lt = 128;
    imap_profile_apply(&b, &st, prof, &s);
    CHECK(b.pedal_m > 0, "paddz: LT 0.5 ohne Floor -> Pedal %d", b.pedal_m);
    /* Gleicher Trigger, Floor 0.6: abgeschnitten. */
    imap_state_init(&st);
    make_snap(&s, fr++, &c);
    set_keys(NULL, 0);
    s.pad_present = 1;
    memset(&s.pad, 0, sizeof s.pad);
    s.pad.lt = 128;
    s.pad_dz_trig = 0.6f;
    imap_profile_apply(&b, &st, prof, &s);
    CHECK(b.pedal_m == 0, "paddz: LT 0.5 Floor 0.6 -> 0 (ist %d)", b.pedal_m);
}

/* ---- Teil 10: W141 CTRL-CAL2 - Zielhebel absolut, Gang nativ ----
 * Aiming-Ruhe 0x59/0x93 (Nutzer 6.11.) mit sbc_cal=1: lineare Abbildung
 * ohne Mitten-Verzerrung; Zwei-Wert-Form; federnde Achsen weiter mit
 * Auto-Mitte; Gang 254/255/0/1-5 laut Mapper-Befund (nativ). */
static void run_sbccal_cases(void)
{
    sbc_autocal_t ac;
    sbc_cal_t cfg, eff;
    sbc_state_t st, r;
    uint8_t raw[SBC_USB_IN_LEN];
    int k, fin = 0;
    static const uint8_t gears[] = { 254, 255, 0, 1, 2, 3, 4, 5, 7, 8, 9, 13 };
    printf("Teil 10: W141 Zielhebel absolut + Gang nativ\n");
    /* Reibungshebel-Ruhe: stabil, aber keine Mitte, keine Verzerrung. */
    memset(&ac, 0, sizeof ac);
    memset(&cfg, 0, sizeof cfg);
    memset(&st, 0, sizeof st);
    st.aiming_x = 0x59; st.aiming_y = 0x93; st.gear = 8;
    for (k = 0; k < SBC_AC_WIN + 5 && !fin; k++) {
        fin = sbc_usb_autocal_feed(&ac, &st);
    }
    CHECK(fin && ac.stable, "sbccal: ruhig trotz Reibungshebel-Ruhe");
    CHECK(ac.rest[SBC_AX_AIM_X] == 0x59 && ac.rest[SBC_AX_AIM_Y] == 0x93,
          "sbccal: Ruhe protokolliert (%d %d)", ac.rest[0], ac.rest[1]);
    sbc_usb_autocal_build(&ac, &cfg, 6, 16, &eff);
    CHECK(!eff.ax[SBC_AX_AIM_X].set && !eff.ax[SBC_AX_AIM_Y].set,
          "sbccal: Aiming ohne Mitte");
    CHECK(eff.ax[SBC_AX_ROT].set && eff.ax[SBC_AX_ROT].mid == 0,
          "sbccal: Rotation weiter mit Mitte");
    r = st;
    sbc_usb_cal_apply(&eff, &r);
    CHECK(r.aiming_x == 0x59 && r.aiming_y == 0x93,
          "sbccal: Ruhe unverzerrt (%02X %02X)", r.aiming_x, r.aiming_y);
    r = st; r.aiming_x = 0; r.aiming_y = 255;
    sbc_usb_cal_apply(&eff, &r);
    CHECK(r.aiming_x == 0 && r.aiming_y == 255, "sbccal: Anschlaege linear");
    /* Zwei-Wert-Form + Drei-Wert-Kompatibilitaet + Totzone ignoriert. */
    memset(&cfg, 0, sizeof cfg);
    CHECK(sbc_usb_cal_set(&cfg, "sbc_cal_aim_x", "4,251") == 1,
          "sbccal: zwei-wert ok");
    CHECK(sbc_usb_cal_set(&cfg, "sbc_cal_aim_y", "4,90,251") == SBC_CAL_NOTE,
          "sbccal: drei-wert note (mid ignoriert)");
    CHECK(sbc_usb_cal_set(&cfg, "sbc_dead_aim_x", "3") == SBC_CAL_NOTE,
          "sbccal: totzone ignoriert");
    CHECK(cfg.ax[SBC_AX_AIM_X].dead == 0,
          "sbccal: totzone ungesetzt");
    memset(&r, 0, sizeof r);
    r.aiming_x = 4; r.aiming_y = 251;
    sbc_usb_cal_apply(&cfg, &r);
    CHECK(r.aiming_x == 0 && r.aiming_y == 255, "sbccal: raender -> voll");
    r.aiming_x = 128; r.aiming_y = 128;
    sbc_usb_cal_apply(&cfg, &r);
    CHECK(r.aiming_x == 128 && r.aiming_y == 128, "sbccal: mitte linear (%u %u)",
          r.aiming_x, r.aiming_y);
    /* Federnde Achsen + Pedale weiter mit Auto-Mitte. */
    memset(&ac, 0, sizeof ac);
    memset(&cfg, 0, sizeof cfg);
    memset(&st, 0, sizeof st);
    st.rot_lever = 5; st.sight_x = -3; st.sight_y = 9; st.pedal_m = 28;
    st.aiming_x = 0x59; st.aiming_y = 0x93;
    fin = 0;
    for (k = 0; k < SBC_AC_WIN + 5 && !fin; k++) {
        fin = sbc_usb_autocal_feed(&ac, &st);
    }
    CHECK(fin && ac.stable, "sbccal: federnd ruhig");
    sbc_usb_autocal_build(&ac, &cfg, 6, 16, &eff);
    CHECK(eff.ax[SBC_AX_ROT].mid == 5 && eff.ax[SBC_AX_SIGHT_X].mid == -3 &&
          eff.ax[SBC_AX_SIGHT_Y].mid == 9, "sbccal: Mitte = Ruhe");
    r = st;
    sbc_usb_cal_apply(&eff, &r);
    CHECK(r.rot_lever == 0 && r.sight_x == 0 && r.sight_y == 0 &&
          r.pedal_m == 0, "sbccal: Ruhe -> neutral");
    r = st; r.rot_lever = 127; r.pedal_m = 255;
    sbc_usb_cal_apply(&eff, &r);
    CHECK(r.rot_lever == 127 && r.pedal_m == 255, "sbccal: voll bleibt voll");
    r = st; r.rot_lever = -128;
    sbc_usb_cal_apply(&eff, &r);
    CHECK(r.rot_lever == -128, "sbccal: min bleibt min");
    /* Gang nativ: ohne Zuordnung 1:1 (Spiel versteht 254/255/1-5). */
    memset(raw, 0, sizeof raw);
    raw[0] = 0x00; raw[1] = SBC_USB_IN_LEN;
    raw[9] = 0x80; raw[11] = 0x80;
    memset(&cfg, 0, sizeof cfg);
    for (k = 0; k < (int)sizeof gears; k++) {
        raw[25] = gears[k];
        CHECK(sbc_usb_parse_report(raw, &st) == SBC_OK, "sbccal: parse gang %u",
              gears[k]);
        r = st;
        sbc_usb_cal_apply(&cfg, &r);
        CHECK(r.gear == gears[k], "sbccal: gang %u nativ 1:1", gears[k]);
    }
    CHECK(sbc_usb_gear_effective(254) == 0 && sbc_usb_gear_effective(255) == 1,
          "sbccal: 254=R 255=N");
    CHECK(sbc_usb_gear_effective(0) == -1, "sbccal: 0 = dazwischen");
    CHECK(sbc_usb_gear_effective(1) == 2 && sbc_usb_gear_effective(5) == 6,
          "sbccal: 1-5 nativ");
    CHECK(sbc_usb_gear_effective(7) == 0 && sbc_usb_gear_effective(8) == 1 &&
          sbc_usb_gear_effective(13) == 6, "sbccal: 7-13 weiter ok");
    CHECK(sbc_usb_gear_effective(6) == -2 && sbc_usb_gear_effective(14) == -2,
          "sbccal: Klasse 6 ungueltig");
    CHECK(sbc_usb_cal_set(&cfg, "sbc_gear_map", "254,255,1,2,3,4,5") == 1,
          "sbccal: gear_map ok");
    memset(&r, 0, sizeof r);
    r.gear = 254;
    sbc_usb_cal_apply(&cfg, &r);
    CHECK(r.gear == 7, "sbccal: gear_map 254 -> 7 (%u)", r.gear);
}

/* ---- Teil 12: W159 Aktion vr_recenter + SBC-Quellen ----
 * "vr_recenter" feuert auf der steigenden Flanke (Hook ruft dann
 * sb_vr_recenter("Belegung")); unbelegt -> 0. Quellen wie Tasten
 * (KEY/MOUSE/PAD), dazu SBC:<taste> (bi0..38 oder Klarname) gegen
 * snap->sbc (nur mit sbc_present). */
static void run_vraction_cases(const imap_profile_t *kb,
                               const imap_profile_t *xbox)
{
    imap_profile_t p, q;
    imap_snap_t s;
    imap_state_t st;
    cfgvar_t c = { 1, 0, 0, 0, 4, 0, 0 }; /* Tasten an, Rest aus */
    unsigned fr = 300000;
    int kF8[] = { VK_F8 };

    printf("Teil 12: W159 Aktion vr_recenter + SBC-Quellen\n");
    /* Standardprofile: F8 gebunden, ohne SBC. */
    CHECK(kb->vr_recenter.n == 1, "vr: kb n=1 (ist %d)",
          kb->vr_recenter.n);
    CHECK(kb->vr_recenter.alt[0].a.kind == IMAP_A_KEY, "vr: kb KEY");
    CHECK(kb->vr_recenter.alt[0].a.code == VK_F8, "vr: kb F8 (ist %d)",
          kb->vr_recenter.alt[0].a.code);
    CHECK(kb->has_sbc == 0, "vr: kb ohne SBC");
    CHECK(xbox->vr_recenter.n == 1, "vr: xbox n=1 (ist %d)",
          xbox->vr_recenter.n);
    CHECK(xbox->vr_recenter.alt[0].a.code == VK_F8, "vr: xbox F8");
    CHECK(xbox->has_sbc == 0, "vr: xbox ohne SBC");
    /* Alle drei Quellarten parsen. */
    CHECK(imap_profile_parse("vr_recenter = KEY:F8, PAD:RB, SBC:CHAFF\n",
                             "t", &p) == 0, "vr: parse ok");
    CHECK(p.vr_recenter.n == 3, "vr: n=3 (ist %d)", p.vr_recenter.n);
    CHECK(p.vr_recenter.alt[0].a.kind == IMAP_A_KEY &&
          p.vr_recenter.alt[0].a.code == VK_F8, "vr: alt0 KEY:F8");
    CHECK(p.vr_recenter.alt[1].a.kind == IMAP_A_PADBTN &&
          p.vr_recenter.alt[1].a.code == IMAP_PAD_RB, "vr: alt1 PAD:RB");
    CHECK(p.vr_recenter.alt[2].a.kind == IMAP_A_SBC &&
          p.vr_recenter.alt[2].a.code == 18, "vr: alt2 SBC:CHAFF=bi18");
    CHECK(p.has_pad == 1, "vr: has_pad");
    CHECK(p.has_sbc == 1, "vr: has_sbc");
    /* Flanke ueber KEY. */
    imap_state_init(&st);
    make_snap(&s, fr++, &c);
    set_keys(NULL, 0);
    CHECK(imap_action_vr_recenter(&p, &st, &s) == 0, "vr: Ruhe -> 0");
    make_snap(&s, fr++, &c);
    set_keys(kF8, 1);
    CHECK(imap_action_vr_recenter(&p, &st, &s) == 1, "vr: F8 -> Flanke");
    make_snap(&s, fr++, &c);
    CHECK(imap_action_vr_recenter(&p, &st, &s) == 0, "vr: Halten -> 0");
    make_snap(&s, fr++, &c);
    set_keys(NULL, 0);
    CHECK(imap_action_vr_recenter(&p, &st, &s) == 0, "vr: Loslassen -> 0");
    /* PAD-Quelle: ohne Geraet still, mit Geraet Flanke. */
    imap_state_init(&st);
    make_snap(&s, fr++, &c);
    set_keys(NULL, 0);
    memset(&s.pad, 0, sizeof s.pad);
    s.pad.buttons = IMAP_PAD_RB;
    s.pad_present = 0;
    CHECK(imap_action_vr_recenter(&p, &st, &s) == 0, "vr: RB ohne Pad -> 0");
    make_snap(&s, fr++, &c);
    memset(&s.pad, 0, sizeof s.pad);
    s.pad.buttons = IMAP_PAD_RB;
    s.pad_present = 1;
    CHECK(imap_action_vr_recenter(&p, &st, &s) == 1, "vr: RB -> Flanke");
    make_snap(&s, fr++, &c);
    memset(&s.pad, 0, sizeof s.pad);
    s.pad.buttons = IMAP_PAD_RB;
    s.pad_present = 1;
    CHECK(imap_action_vr_recenter(&p, &st, &s) == 0, "vr: RB halten -> 0");
    /* SBC-Quelle: ohne present still, mit Bit Flanke (bi18 = Wort 1 Bit 2). */
    imap_state_init(&st);
    make_snap(&s, fr++, &c);
    s.sbc[0] = 0;
    s.sbc[1] = (uint16_t)(1u << 2);
    s.sbc[2] = 0;
    s.sbc_present = 0;
    CHECK(imap_action_vr_recenter(&p, &st, &s) == 0,
          "vr: Chaff ohne SBC -> 0");
    make_snap(&s, fr++, &c);
    s.sbc[0] = 0;
    s.sbc[1] = (uint16_t)(1u << 2);
    s.sbc[2] = 0;
    s.sbc_present = 1;
    CHECK(imap_action_vr_recenter(&p, &st, &s) == 1, "vr: Chaff -> Flanke");
    make_snap(&s, fr++, &c);
    s.sbc[0] = 0;
    s.sbc[1] = (uint16_t)(1u << 2);
    s.sbc[2] = 0;
    s.sbc_present = 1;
    CHECK(imap_action_vr_recenter(&p, &st, &s) == 0, "vr: Chaff halten -> 0");
    /* SBC-Namen: bi<N>, Klarnamen, Gross/Klein egal; Gegner still raus. */
    CHECK(imap_profile_parse("vr_recenter = SBC:bi0\n", "t", &p) == 0,
          "vr: bi0 ok");
    CHECK(p.vr_recenter.n == 1 && p.vr_recenter.alt[0].a.code == 0,
          "vr: bi0 = 0");
    CHECK(imap_profile_parse("vr_recenter = SBC:BI38, SBC:togvtloc\n", "t",
                             &p) == 0, "vr: BI38 ok");
    CHECK(p.vr_recenter.n == 2 && p.vr_recenter.alt[0].a.code == 38 &&
          p.vr_recenter.alt[1].a.code == 38, "vr: BI38 = togvtloc = 38");
    CHECK(imap_profile_parse("vr_recenter = SBC:QUATSCH\n", "t", &p) == 0,
          "vr: Unbekannt ok");
    CHECK(p.vr_recenter.n == 0, "vr: QUATSCH ignoriert (n=%d)",
          p.vr_recenter.n);
    CHECK(imap_profile_parse("vr_recenter = SBC:bi39, SBC:bi-1\n", "t", &p)
          == 0, "vr: Bereich ok");
    CHECK(p.vr_recenter.n == 0, "vr: bi39/bi-1 ignoriert (n=%d)",
          p.vr_recenter.n);
    CHECK(imap_profile_parse("vr_recenter2 = KEY:F8\n", "t", &p) == 0,
          "vr: aehnlich ok");
    CHECK(p.vr_recenter.n == 0, "vr: vr_recenter2 ignoriert");
    /* Akkord mit SBC: nur beides zusammen. */
    CHECK(imap_profile_parse("vr_recenter = SBC:CHAFF+KEY:SPACE\n", "t", &p)
          == 0, "vr: Akkord ok");
    CHECK(p.vr_recenter.n == 1 && p.vr_recenter.alt[0].chord == 1,
          "vr: Akkord erkannt");
    {
        int kSp[] = { VK_SPACE };
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(kSp, 1);
        s.sbc[1] = 0;
        s.sbc_present = 1;
        CHECK(imap_action_vr_recenter(&p, &st, &s) == 0,
              "vr: nur SPACE -> 0");
        make_snap(&s, fr++, &c);
        set_keys(kSp, 1);
        s.sbc[1] = (uint16_t)(1u << 2);
        s.sbc_present = 1;
        CHECK(imap_action_vr_recenter(&p, &st, &s) == 1,
              "vr: Chaff+SPACE -> Flanke");
    }
    /* Reine Aktions-Alternative schweigt im aktiven Tasten-Akkord
     * (gleiche Unterdrueckung wie Tasten, einseitig). */
    CHECK(imap_profile_parse("bi27 = PAD:LB+PAD:RB\n"
                             "vr_recenter = PAD:RB\n", "t", &p) == 0,
          "vr: Akkord-Unterdrueckung ok");
    {
        imap_block_t b;
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(NULL, 0);
        memset(&s.pad, 0, sizeof s.pad);
        s.pad.buttons = (uint16_t)(IMAP_PAD_LB | IMAP_PAD_RB);
        s.pad_present = 1;
        imap_profile_apply(&b, &st, &p, &s);
        CHECK((b.buttons[1] & (1u << 11)) != 0, "vr: Magazin feuert");
        CHECK(imap_action_vr_recenter(&p, &st, &s) == 0,
              "vr: RB im LB+RB-Akkord unterdrueckt");
        make_snap(&s, fr++, &c);
        memset(&s.pad, 0, sizeof s.pad);
        s.pad.buttons = IMAP_PAD_RB;
        s.pad_present = 1;
        CHECK(imap_action_vr_recenter(&p, &st, &s) == 1,
              "vr: RB allein -> Flanke");
    }
    /* Unbelegt: nie feuern, auch nicht mit Tasten. */
    CHECK(imap_profile_parse("bi0 = KEY:SPACE\n", "u", &p) == 0,
          "vr: unbelegt ok");
    {
        int kAll[] = { VK_SPACE, VK_F8 };
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(kAll, 2);
        s.pad_present = 1;
        memset(&s.pad, 0, sizeof s.pad);
        s.pad.buttons = 0xFFFF;
        s.sbc_present = 1;
        s.sbc[0] = s.sbc[1] = s.sbc[2] = 0xFFFF;
        CHECK(imap_action_vr_recenter(&p, &st, &s) == 0,
              "vr: unbelegt -> 0");
    }
    /* equal: Aktion zaehlt mit. */
    CHECK(imap_profile_parse("vr_recenter = KEY:F8\n", "t", &p) == 0 &&
          imap_profile_parse("vr_recenter = KEY:F8\n", "t", &q) == 0,
          "vr: equal-Basis ok");
    CHECK(imap_profile_equal(&p, &q) == 0, "vr: gleich -> 0");
    CHECK(imap_profile_parse("vr_recenter = KEY:F9\n", "t", &q) == 0,
          "vr: equal-Gegen ok");
    CHECK(imap_profile_equal(&p, &q) == 1, "vr: ungleich -> 1");
}

/* ---- Teil 13: W188 Aktion pause ----
 * Standardprofile: pause = KEY:PAUSE. Tasten zaehlen im Hook nicht (das
 * Fenster wertet sie aus, auch waehrend das Spiel steht), Pad-Quellen
 * feuern auf der Flanke; imap_bind_keys liefert Tasten + Pad-Maske. */
static void run_pause_cases(const imap_profile_t *kb, const imap_profile_t *xbox)
{
    imap_profile_t p;
    imap_snap_t s;
    imap_state_t st;
    cfgvar_t c = { 1, 0, 0, 0, 4, 0, 0 };
    unsigned fr = 310000, pm = 0;
    int kP[] = { VK_PAUSE }, vks[4], n;

    printf("Teil 13: W188 Aktion pause\n");
    CHECK(kb->pause.n == 1 && kb->pause.alt[0].a.kind == IMAP_A_KEY && kb->pause.alt[0].a.code == VK_PAUSE,
          "pause: kb KEY:PAUSE");
    CHECK(xbox->pause.n == 1 && xbox->pause.alt[0].a.code == VK_PAUSE, "pause: xbox KEY:PAUSE");
    n = imap_bind_keys(&kb->pause, vks, 4, &pm);
    CHECK(n == 1 && vks[0] == VK_PAUSE && pm == 0, "pause: bind_keys kb (n=%d)", n);
    CHECK(imap_profile_parse("pause = KEY:P, PAD:BACK+PAD:START, PAD:Y, SBC:CHAFF\n", "t", &p) == 0,
          "pause: parse");
    CHECK(p.pause.n == 4, "pause: n=4 (ist %d)", p.pause.n);
    n = imap_bind_keys(&p.pause, vks, 4, &pm);
    CHECK(n == 1 && vks[0] == 'P' && pm == IMAP_PAD_Y, "pause: bind_keys P + Pad-Maske Y (n=%d pm=0x%X)", n, pm);
    /* Tasten feuern im Hook nicht */
    imap_state_init(&st);
    make_snap(&s, fr++, &c);
    set_keys(NULL, 0);
    CHECK(imap_action_pause(&p, &st, &s) == 0, "pause: Ruhe -> 0");
    {
        int kp[] = { 'P' };
        make_snap(&s, fr++, &c);
        set_keys(kp, 1);
        CHECK(imap_action_pause(&p, &st, &s) == 0, "pause: Taste P im Hook -> 0");
    }
    /* Pad Y: Flanke, Halten still */
    make_snap(&s, fr++, &c);
    set_keys(NULL, 0);
    memset(&s.pad, 0, sizeof s.pad);
    s.pad.buttons = IMAP_PAD_Y;
    s.pad_present = 1;
    CHECK(imap_action_pause(&p, &st, &s) == 1, "pause: Pad Y -> Flanke");
    make_snap(&s, fr++, &c);
    memset(&s.pad, 0, sizeof s.pad);
    s.pad.buttons = IMAP_PAD_Y;
    s.pad_present = 1;
    CHECK(imap_action_pause(&p, &st, &s) == 0, "pause: Pad Y halten -> 0");
    /* W423 LOC-XSB-LOOP (#71): schon beim Start gehalten -> erst nach Loslassen eine Flanke */
    imap_state_init(&st);
    make_snap(&s, fr++, &c);
    memset(&s.pad, 0, sizeof s.pad);
    s.pad.buttons = IMAP_PAD_Y;
    s.pad_present = 1;
    CHECK(imap_action_pause(&p, &st, &s) == 0, "pause: Pad Y ab Start gehalten -> 0 (W423)");
    CHECK(imap_action_photo(&p, &st, &s) == 0 && imap_action_net_hud(&p, &st, &s) == 0, "W423: andere Aktionen still");
    make_snap(&s, fr++, &c);
    memset(&s.pad, 0, sizeof s.pad);
    s.pad.buttons = IMAP_PAD_Y;
    s.pad_present = 1;
    CHECK(imap_action_pause(&p, &st, &s) == 0, "pause: weiter gehalten -> 0 (W423)");
    s.pad.buttons = 0;
    CHECK(imap_action_pause(&p, &st, &s) == 0, "pause: losgelassen -> 0 (W423)");
    s.pad.buttons = IMAP_PAD_Y;
    CHECK(imap_action_pause(&p, &st, &s) == 1, "pause: neuer Druck nach dem Start -> Flanke (W423)");
    /* ohne Geraet still */
    imap_state_init(&st);
    make_snap(&s, fr++, &c);
    memset(&s.pad, 0, sizeof s.pad);
    s.pad.buttons = IMAP_PAD_Y;
    s.pad_present = 0;
    CHECK(imap_action_pause(&p, &st, &s) == 0, "pause: Pad Y ohne Geraet -> 0");
    /* Standardprofil: KEY:PAUSE allein loest im Hook nie aus */
    imap_state_init(&st);
    make_snap(&s, fr++, &c);
    set_keys(kP, 1);
    CHECK(imap_action_pause(kb, &st, &s) == 0, "pause: kb PAUSE im Hook -> 0");
    set_keys(NULL, 0);
    /* unbelegt -> 0, Profilvergleich erkennt Unterschied */
    CHECK(imap_profile_parse("bi0 = KEY:SPACE\n", "t", &p) == 0 && p.pause.n == 0, "pause: unbelegt n=0");
    CHECK(imap_action_pause(&p, &st, &s) == 0, "pause: unbelegt -> 0");
    {
        imap_profile_t q;
        CHECK(imap_profile_parse("pause = KEY:PAUSE\n", "t", &p) == 0 &&
              imap_profile_parse("pause = KEY:P\n", "t", &q) == 0 && imap_profile_equal(&p, &q) != 0,
              "pause: Vergleich erkennt Unterschied");
    }
}

/* ---- Teil 15: W195 Aktion photo (wie pause) ---- */
static void run_photo_cases(const imap_profile_t *kb, const imap_profile_t *xbox)
{
    imap_profile_t p;
    imap_state_t st;
    imap_snap_t s;
    cfgvar_t c = { 1, 0, 0, 0, 4, 0, 0 };
    unsigned fr = 320000, pm = 0;
    int vks[4], n;
    printf("Teil 15: W195 Aktion photo\n");
    /* W290: Standard ist jetzt KEY:SHIFT+KEY:F12 (F12 = Schnell-Screenshot, Teil 21) */
    CHECK(kb->photo.n == 1 && kb->photo.alt[0].chord && kb->photo.alt[0].a.code == VK_SHIFT &&
          kb->photo.alt[0].b.code == VK_F12, "photo: kb KEY:SHIFT+KEY:F12");
    CHECK(xbox->photo.n == 1 && xbox->photo.alt[0].chord && xbox->photo.alt[0].b.code == VK_F12,
          "photo: xbox KEY:SHIFT+KEY:F12");
    n = imap_bind_keys(&kb->photo, vks, 4, &pm);
    CHECK(n == 0 && pm == 0, "photo: bind_keys kb ohne einfache Taste (n=%d)", n);
    CHECK(imap_profile_parse("photo = KEY:F11, PAD:BACK+PAD:LB, PAD:X\n", "t", &p) == 0, "photo: parse");
    CHECK(p.photo.n == 3 && p.pause.n == 0, "photo: n=3 (ist %d), pause unberuehrt", p.photo.n);
    n = imap_bind_keys(&p.photo, vks, 4, &pm);
    CHECK(n == 1 && vks[0] == VK_F11 && pm == IMAP_PAD_X, "photo: bind_keys F11 + Pad-Maske X (n=%d pm=0x%X)", n, pm);
    imap_state_init(&st);
    make_snap(&s, fr++, &c);
    set_keys(NULL, 0);
    CHECK(imap_action_photo(&p, &st, &s) == 0, "photo: Ruhe -> 0");
    make_snap(&s, fr++, &c);
    memset(&s.pad, 0, sizeof s.pad);
    s.pad.buttons = IMAP_PAD_X;
    s.pad_present = 1;
    CHECK(imap_action_photo(&p, &st, &s) == 1, "photo: Pad X -> Flanke");
    make_snap(&s, fr++, &c);
    memset(&s.pad, 0, sizeof s.pad);
    s.pad.buttons = IMAP_PAD_X;
    s.pad_present = 1;
    CHECK(imap_action_photo(&p, &st, &s) == 0, "photo: Pad X halten -> 0");
    {
        int kf[] = { VK_F12 };
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(kf, 1);
        CHECK(imap_action_photo(kb, &st, &s) == 0, "photo: kb F12 im Hook -> 0 (Fenster)");
        set_keys(NULL, 0);
    }
    {
        imap_profile_t q;
        CHECK(imap_profile_parse("photo = KEY:F12\n", "t", &p) == 0 &&
              imap_profile_parse("photo = KEY:F11\n", "t", &q) == 0 && imap_profile_equal(&p, &q) != 0,
              "photo: Vergleich erkennt Unterschied");
    }
    CHECK(imap_key_name("F12") == VK_F12, "photo: Tastenname F12");
}

static void run_feedback_shot_cases(const imap_profile_t *kb, const imap_profile_t *xbox)
{
    imap_profile_t p;
    imap_state_t st;
    imap_snap_t s;
    cfgvar_t c = { 1, 0, 0, 0, 4, 0, 0 };
    unsigned fr = 320000, pm = 0;
    int vks[4], n;
    printf("Teil 15: W211 Aktion feedback_shot\n");
    CHECK(kb->feedback_shot.n == 1 && kb->feedback_shot.alt[0].a.kind == IMAP_A_KEY && kb->feedback_shot.alt[0].a.code == VK_F11,
          "feedback_shot: kb KEY:F11");
    CHECK(xbox->feedback_shot.n == 1 && xbox->feedback_shot.alt[0].a.code == VK_F11, "feedback_shot: xbox KEY:F11");
    n = imap_bind_keys(&kb->feedback_shot, vks, 4, &pm);
    CHECK(n == 1 && vks[0] == VK_F11 && pm == 0, "feedback_shot: bind_keys kb (n=%d)", n);
    CHECK(imap_profile_parse("feedback_shot = KEY:F7, PAD:BACK+PAD:LB, PAD:X\n", "t", &p) == 0, "feedback_shot: parse");
    CHECK(p.feedback_shot.n == 3 && p.pause.n == 0, "feedback_shot: n=3 (ist %d), pause unberuehrt", p.feedback_shot.n);
    n = imap_bind_keys(&p.feedback_shot, vks, 4, &pm);
    CHECK(n == 1 && vks[0] == VK_F7 && pm == IMAP_PAD_X, "feedback_shot: bind_keys F7 + Pad-Maske X (n=%d pm=0x%X)", n, pm);
    imap_state_init(&st);
    make_snap(&s, fr++, &c);
    set_keys(NULL, 0);
    CHECK(imap_action_feedback_shot(&p, &st, &s) == 0, "feedback_shot: Ruhe -> 0");
    make_snap(&s, fr++, &c);
    memset(&s.pad, 0, sizeof s.pad);
    s.pad.buttons = IMAP_PAD_X;
    s.pad_present = 1;
    CHECK(imap_action_feedback_shot(&p, &st, &s) == 1, "feedback_shot: Pad X -> Flanke");
    make_snap(&s, fr++, &c);
    memset(&s.pad, 0, sizeof s.pad);
    s.pad.buttons = IMAP_PAD_X;
    s.pad_present = 1;
    CHECK(imap_action_feedback_shot(&p, &st, &s) == 0, "feedback_shot: Pad X halten -> 0");
    s.pad.buttons = 0;
    CHECK(imap_action_feedback_shot(&p, &st, &s) == 0, "feedback_shot: release");
    s.pad.buttons = IMAP_PAD_BACK | IMAP_PAD_LB;
    CHECK(imap_action_feedback_shot(&p, &st, &s) == 1, "feedback_shot: chord edge");
    CHECK(imap_action_feedback_shot(&p, &st, &s) == 0, "feedback_shot: chord held");
    p.feedback_shot.n = 0;
    CHECK(imap_action_feedback_shot(&p, &st, &s) == 0 && st.prev_feedback_shot == 0,
          "feedback_shot: disabled resets edge");
    {
        int kf[] = { VK_F11 };
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(kf, 1);
        CHECK(imap_action_feedback_shot(kb, &st, &s) == 0, "feedback_shot: kb F11 im Hook -> 0 (Fenster)");
        set_keys(NULL, 0);
    }
    {
        imap_profile_t q;
        CHECK(imap_profile_parse("feedback_shot = KEY:F11\n", "t", &p) == 0 &&
              imap_profile_parse("feedback_shot = KEY:F7\n", "t", &q) == 0 && imap_profile_equal(&p, &q) != 0,
              "feedback_shot: Vergleich erkennt Unterschied");
    }
    CHECK(imap_key_name("F11") == VK_F11, "feedback_shot: Tastenname F11");
}

/* Teil 20: W283 ORIG-KEY-BIND - Aktion orig_view (Vergleichstaste), Standard,
 * Rueckfall orig_key, gelernte Quellen aus dem Launcher-Dialog. */
static void run_origview_cases(const imap_profile_t *kb, const imap_profile_t *xbox)
{
    imap_profile_t p, q;
    imap_state_t st;
    imap_snap_t s;
    cfgvar_t c = { 1, 0, 0, 0, 4, 0, 0 };
    unsigned fr = 330000;
    int vks[4], mods[4], n;
    printf("Teil 20: W283 Aktion orig_view\n");
    /* Standardbelegung in beiden mitgelieferten Profilen */
    CHECK(kb->orig_view_set == 1 && kb->orig_view.n == 1 && kb->orig_view.alt[0].chord &&
          kb->orig_view.alt[0].a.kind == IMAP_A_KEY && kb->orig_view.alt[0].a.code == VK_SHIFT &&
          kb->orig_view.alt[0].b.kind == IMAP_A_KEY && kb->orig_view.alt[0].b.code == VK_F8,
          "orig_view: kb Standard KEY:SHIFT+KEY:F8");
    CHECK(xbox->orig_view_set == 1 && imap_orig_view_is_default(&xbox->orig_view),
          "orig_view: xbox Standard KEY:SHIFT+KEY:F8");
    CHECK(imap_orig_view_is_default(&kb->orig_view), "orig_view: kb ist Standard");
    n = imap_bind_key_combos(&kb->orig_view, vks, mods, 4);
    CHECK(n == 1 && vks[0] == VK_F8 && mods[0] == 1, "orig_view: Fenster-Taste F8 + Shift (n=%d)", n);
    {
        int pk[4];
        unsigned pm = 0;
        CHECK(imap_bind_keys(&kb->orig_view, pk, 4, &pm) == 0 && pm == 0,
              "orig_view: Akkord ist keine einfache Taste (Pause/Foto-Weg unberuehrt)");
    }
    /* Tastatur wertet das Fenster aus: im Hook keine Flanke */
    {
        int kf[] = { VK_SHIFT, VK_F8 };
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(kf, 2);
        CHECK(imap_action_orig_view(kb, &st, &s) == 0, "orig_view: Shift+F8 im Hook -> 0 (Fenster)");
        set_keys(NULL, 0);
    }
    /* Rueckfall / Alias orig_key */
    CHECK(imap_orig_key_is_default("SHIFT+F8") && imap_orig_key_is_default(" shift + f8 ") &&
          !imap_orig_key_is_default("F8") && !imap_orig_key_is_default("CTRL+F8") &&
          !imap_orig_key_is_default("OFF") && !imap_orig_key_is_default("") && !imap_orig_key_is_default(NULL),
          "orig_view: orig_key Standard erkannt");
    CHECK(imap_profile_parse("pause = KEY:PAUSE\n", "alt", &p) == 0 && p.orig_view_set == 0 && p.orig_view.n == 0,
          "orig_view: altes Profil ohne Zeile -> nicht gesetzt");
    CHECK(imap_orig_view_resolve(&p, "SHIFT+F8") == 0 && p.orig_view.n == 0, "orig_view: ohne Zeile -> orig_key");
    p = *kb;
    CHECK(imap_orig_view_resolve(&p, "SHIFT+F8") == 1 && p.orig_view.n == 1, "orig_view: Standard + orig_key Standard -> Profil");
    p = *kb;
    CHECK(imap_orig_view_resolve(&p, "F6") == 0 && p.orig_view.n == 0,
          "orig_view: Standard im Profil + orig_key=F6 (bestehende cfg) -> orig_key, Hook leer");
    CHECK(imap_profile_parse("orig_view = KEY:F7\n", "t", &p) == 0 && imap_orig_view_resolve(&p, "F6") == 1 &&
          p.orig_view.n == 1 && p.orig_view.alt[0].a.code == VK_F7,
          "orig_view: eigene Belegung schlaegt orig_key");
    CHECK(imap_profile_parse("orig_view =\npause = KEY:PAUSE\n", "t", &p) == 0 && p.orig_view_set == 1 &&
          p.orig_view.n == 0 && p.pause.n == 1 && imap_orig_view_resolve(&p, "SHIFT+F8") == 1 &&
          imap_bind_key_combos(&p.orig_view, vks, mods, 4) == 0,
          "orig_view: leere Zeile = aus (kein Rueckfall)");
    CHECK(imap_profile_parse("orig_view = KEY:F6\norig_view = KEY:F7\n", "t", &p) == 0 && p.orig_view.n == 1 &&
          p.orig_view.alt[0].a.code == VK_F7, "orig_view: letzte Zeile gewinnt");
    /* Gelernte Quellen (Formate des Launcher-Lernmodus) */
    CHECK(imap_profile_parse("orig_view = KEY:CTRL+KEY:F7, KEY:F6, PAD:BACK+PAD:Y, SBC:CHAFF\n", "t", &p) == 0 &&
          p.orig_view.n == 4 && p.has_pad && p.has_sbc, "orig_view: gelernt 4 Quellen (n=%d)", p.orig_view.n);
    n = imap_bind_key_combos(&p.orig_view, vks, mods, 4);
    CHECK(n == 2 && vks[0] == VK_F7 && mods[0] == 2 && vks[1] == VK_F6 && mods[1] == 0,
          "orig_view: Fenster-Tasten Ctrl+F7, F6 (n=%d)", n);
    CHECK(imap_orig_view_resolve(&p, "SHIFT+F8") == 1 && !imap_orig_view_is_default(&p.orig_view),
          "orig_view: gelernt -> Profil");
    CHECK(imap_profile_parse("orig_view = KEY:ALT+KEY:O, KEY:F8+KEY:SHIFT, KEY:A+KEY:B, KEY:LCTRL\n", "t", &p) == 0 &&
          imap_bind_key_combos(&p.orig_view, vks, mods, 4) == 3 && vks[0] == 'O' && mods[0] == 4 &&
          vks[1] == VK_F8 && mods[1] == 1 && vks[2] == VK_LCONTROL && mods[2] == 0,
          "orig_view: Alt+O, F8+Shift, A+B zaehlt nicht, LCTRL allein");
    CHECK(imap_profile_parse("orig_view = KEY:SHIFT+KEY:F8, PAD:Y\n", "t", &p) == 0 &&
          !imap_orig_view_is_default(&p.orig_view), "orig_view: Standard + Pad ist eigene Belegung");
    imap_profile_parse("orig_view = KEY:CTRL+KEY:F7, PAD:BACK+PAD:Y, SBC:CHAFF\n", "t", &p);
    imap_state_init(&st);
    make_snap(&s, fr++, &c);
    set_keys(NULL, 0);
    CHECK(imap_action_orig_view(&p, &st, &s) == 0, "orig_view: Ruhe -> 0");
    memset(&s.pad, 0, sizeof s.pad);
    s.pad.buttons = IMAP_PAD_BACK | IMAP_PAD_Y;
    s.pad_present = 1;
    CHECK(imap_action_orig_view(&p, &st, &s) == 1, "orig_view: Pad BACK+Y -> Flanke");
    CHECK(imap_action_orig_view(&p, &st, &s) == 0, "orig_view: halten -> 0");
    s.pad.buttons = 0;
    CHECK(imap_action_orig_view(&p, &st, &s) == 0, "orig_view: los");
    s.pad.buttons = IMAP_PAD_Y;
    CHECK(imap_action_orig_view(&p, &st, &s) == 0, "orig_view: Y ohne BACK -> 0");
    s.pad.buttons = 0;
    {
        imap_profile_t r;
        imap_state_t st2;
        CHECK(imap_profile_parse("orig_view = SBC:CHAFF\n", "t", &r) == 0 && r.orig_view.n == 1 && r.has_sbc,
              "orig_view: SBC-Quelle");
        imap_state_init(&st2);
        make_snap(&s, fr++, &c);
        s.sbc_present = 1;
        s.sbc[0] = s.sbc[1] = s.sbc[2] = 0;
        (void)imap_action_orig_view(&r, &st2, &s); /* W423: Ruhe nach dem Start, dann erst eine Flanke */
        s.sbc[1] = (uint16_t)(1u << 2); /* bi18 CHAFF = Wort 1 Bit 2 (wie Teil 12) */
        n = imap_action_orig_view(&r, &st2, &s);
        s.sbc[1] = 0;
        s.sbc_present = 0;
        CHECK(n == 1, "orig_view: SBC CHAFF -> Flanke");
    }
    p.orig_view.n = 0;
    CHECK(imap_action_orig_view(&p, &st, &s) == 0 && st.prev_orig_view == 0, "orig_view: leer setzt Flanke zurueck");
    /* Joystick-Slot und Vergleich */
    CHECK(imap_profile_parse("orig_view = JOY1:B3\n", "t", &p) == 0 && (p.joy_used & 2u) != 0,
          "orig_view: JOY1 zaehlt fuer joy_used (0x%X)", p.joy_used);
    CHECK(imap_profile_parse("orig_view = KEY:F6\n", "t", &p) == 0 &&
          imap_profile_parse("orig_view = KEY:F7\n", "t", &q) == 0 && imap_profile_equal(&p, &q) != 0,
          "orig_view: Vergleich erkennt Unterschied");
    CHECK(imap_profile_parse("orig_view =\n", "t", &p) == 0 && imap_profile_parse("pause =\n", "t", &q) == 0 &&
          imap_profile_equal(&p, &q) != 0, "orig_view: Vergleich erkennt leere Zeile");
}

/* ---- Teil 21: W290 Aktion quick_shot (Schnell-Screenshot) ---- */
static void run_quickshot_cases(const imap_profile_t *kb, const imap_profile_t *xbox)
{
    imap_profile_t p, q;
    imap_state_t st;
    imap_snap_t s;
    cfgvar_t c = { 1, 0, 0, 0, 4, 0, 0 };
    unsigned fr = 340000, pm = 0;
    int vks[4], mods[4], n;
    printf("Teil 21: W290 Aktion quick_shot\n");
    /* Standardbelegung: F12 = Schnell-Screenshot, Shift+F12 = Fotomodus */
    CHECK(kb->quick_shot_set == 1 && kb->quick_shot.n == 1 && !kb->quick_shot.alt[0].chord &&
          kb->quick_shot.alt[0].a.kind == IMAP_A_KEY && kb->quick_shot.alt[0].a.code == VK_F12,
          "quick_shot: kb Standard KEY:F12");
    CHECK(xbox->quick_shot_set == 1 && xbox->quick_shot.n == 1 && xbox->quick_shot.alt[0].a.code == VK_F12,
          "quick_shot: xbox Standard KEY:F12");
    n = imap_bind_key_combos(&kb->quick_shot, vks, mods, 4);
    CHECK(n == 1 && vks[0] == VK_F12 && mods[0] == 0, "quick_shot: Fenster-Taste F12 ohne Zusatztaste (n=%d)", n);
    n = imap_bind_key_combos(&kb->photo, vks, mods, 4);
    CHECK(n == 1 && vks[0] == VK_F12 && mods[0] == 1, "quick_shot: photo Fenster-Taste F12 + Shift (n=%d)", n);
    CHECK(imap_bind_keys(&kb->quick_shot, vks, 4, &pm) == 1 && vks[0] == VK_F12 && pm == 0,
          "quick_shot: einfache Taste F12");
    {
        int kf[] = { VK_F12 };
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(kf, 1);
        CHECK(imap_action_quick_shot(kb, &st, &s) == 0, "quick_shot: F12 im Hook -> 0 (Fenster)");
        set_keys(NULL, 0);
    }
    /* Uebergang alter Profile (ohne quick_shot-Zeile) */
    CHECK(imap_profile_parse("pause = KEY:PAUSE\nphoto = KEY:F12\n", "alt", &p) == 0 && p.quick_shot_set == 0,
          "quick_shot: altes Profil ohne Zeile");
    n = imap_bind_key_combos(&p.photo, vks, mods, 4);
    CHECK(n == 1 && vks[0] == VK_F12 && mods[0] == 1, "quick_shot: alter Standard photo=F12 -> Shift+F12 (n=%d)", n);
    n = imap_bind_key_combos(&p.quick_shot, vks, mods, 4);
    CHECK(n == 1 && vks[0] == VK_F12 && mods[0] == 0, "quick_shot: alter Standard -> quick_shot=F12 (n=%d)", n);
    CHECK(imap_profile_parse("photo = KEY:F7\n", "t", &p) == 0 && p.photo.n == 1 && p.photo.alt[0].a.code == VK_F7 &&
          p.quick_shot.n == 1 && p.quick_shot.alt[0].a.code == VK_F12,
          "quick_shot: eigene photo-Taste bleibt, F12 frei -> quick_shot=F12");
    CHECK(imap_profile_parse("photo = KEY:F12, PAD:Y\n", "t", &p) == 0 && p.photo.n == 2 &&
          p.photo.alt[0].a.code == VK_F12 && p.quick_shot.n == 0,
          "quick_shot: eigene photo-Belegung mit F12 bleibt, quick_shot leer (kein Konflikt)");
    CHECK(imap_profile_parse("photo = KEY:F12\nquick_shot =\n", "t", &p) == 0 && p.quick_shot_set == 1 &&
          p.quick_shot.n == 0 && p.photo.n == 1 && !p.photo.alt[0].chord && p.photo.alt[0].a.code == VK_F12,
          "quick_shot: leere Zeile = aus, photo unveraendert");
    CHECK(imap_profile_parse("quick_shot = KEY:F6\nquick_shot = KEY:F7\n", "t", &p) == 0 && p.quick_shot.n == 1 &&
          p.quick_shot.alt[0].a.code == VK_F7, "quick_shot: letzte Zeile gewinnt");
    /* Gelernte Quellen (Formate des Launcher-Lernmodus, mit Zusatztaste wie W283) */
    CHECK(imap_profile_parse("quick_shot = KEY:CTRL+KEY:F12, KEY:SNAPSHOT, PAD:BACK+PAD:X, SBC:CHAFF\n", "t", &p) == 0 &&
          p.quick_shot.n == 4 && p.has_pad && p.has_sbc, "quick_shot: gelernt 4 Quellen (n=%d)", p.quick_shot.n);
    n = imap_bind_key_combos(&p.quick_shot, vks, mods, 4);
    CHECK(n == 2 && vks[0] == VK_F12 && mods[0] == 2 && vks[1] == VK_SNAPSHOT && mods[1] == 0,
          "quick_shot: Fenster-Tasten Ctrl+F12, Druck (n=%d)", n);
    CHECK(imap_profile_parse("photo = KEY:ALT+KEY:P\nquick_shot = KEY:F12+KEY:SHIFT\n", "t", &p) == 0 &&
          imap_bind_key_combos(&p.photo, vks, mods, 4) == 1 && vks[0] == 'P' && mods[0] == 4 &&
          imap_bind_key_combos(&p.quick_shot, vks, mods, 4) == 1 && vks[0] == VK_F12 && mods[0] == 1,
          "quick_shot: Alt+P / F12+Shift (umgekehrt belegt)");
    imap_profile_parse("quick_shot = KEY:CTRL+KEY:F12, PAD:BACK+PAD:X, SBC:CHAFF\n", "t", &p);
    imap_state_init(&st);
    make_snap(&s, fr++, &c);
    set_keys(NULL, 0);
    CHECK(imap_action_quick_shot(&p, &st, &s) == 0, "quick_shot: Ruhe -> 0");
    memset(&s.pad, 0, sizeof s.pad);
    s.pad.buttons = IMAP_PAD_BACK | IMAP_PAD_X;
    s.pad_present = 1;
    CHECK(imap_action_quick_shot(&p, &st, &s) == 1, "quick_shot: Pad BACK+X -> Flanke");
    CHECK(imap_action_quick_shot(&p, &st, &s) == 0, "quick_shot: halten -> 0");
    s.pad.buttons = 0;
    CHECK(imap_action_quick_shot(&p, &st, &s) == 0, "quick_shot: los");
    {
        imap_profile_t r;
        imap_state_t st2;
        CHECK(imap_profile_parse("quick_shot = SBC:CHAFF\n", "t", &r) == 0 && r.quick_shot.n == 1 && r.has_sbc,
              "quick_shot: SBC-Quelle");
        imap_state_init(&st2);
        make_snap(&s, fr++, &c);
        s.sbc_present = 1;
        s.sbc[0] = s.sbc[1] = s.sbc[2] = 0;
        (void)imap_action_quick_shot(&r, &st2, &s); /* W423: Ruhe nach dem Start, dann erst eine Flanke */
        s.sbc[1] = (uint16_t)(1u << 2); /* bi18 CHAFF */
        n = imap_action_quick_shot(&r, &st2, &s);
        s.sbc[1] = 0;
        s.sbc_present = 0;
        CHECK(n == 1, "quick_shot: SBC CHAFF -> Flanke");
    }
    p.quick_shot.n = 0;
    CHECK(imap_action_quick_shot(&p, &st, &s) == 0 && st.prev_quick_shot == 0, "quick_shot: leer setzt Flanke zurueck");
    CHECK(imap_profile_parse("quick_shot = JOY2:B1\n", "t", &p) == 0 && (p.joy_used & 4u) != 0,
          "quick_shot: JOY2 zaehlt fuer joy_used (0x%X)", p.joy_used);
    CHECK(imap_profile_parse("quick_shot = KEY:F6\n", "t", &p) == 0 &&
          imap_profile_parse("quick_shot = KEY:F7\n", "t", &q) == 0 && imap_profile_equal(&p, &q) != 0,
          "quick_shot: Vergleich erkennt Unterschied");
    CHECK(imap_profile_parse("quick_shot =\n", "t", &p) == 0 && imap_profile_parse("pause =\n", "t", &q) == 0 &&
          imap_profile_equal(&p, &q) != 0, "quick_shot: Vergleich erkennt leere Zeile");
    CHECK(imap_key_name("SNAPSHOT") == VK_SNAPSHOT, "quick_shot: Tastenname SNAPSHOT");
}

/* ---- Teil 23: W299 REPLAY - Aktion replay_exit, Maus beendet das Spiel-Replay nicht (bi0_nomouse) ---- */
static void run_replayexit_cases(const imap_profile_t *kb, const imap_profile_t *xbox)
{
    imap_profile_t p, q;
    imap_state_t st;
    imap_snap_t s;
    imap_block_t b;
    cfgvar_t c = { 1, 0, 0, 1, 4, 0, 0 }; /* Maustasten an (synthetische VKs 324.. im Replay-Pfad) */
    unsigned fr = 360000;
    printf("Teil 23: W299 Aktion replay_exit / Maus im Replay\n");
    CHECK(kb->replay_exit_set == 1 && kb->replay_exit.n == 1 && !kb->replay_exit.alt[0].chord &&
          kb->replay_exit.alt[0].a.kind == IMAP_A_KEY && kb->replay_exit.alt[0].a.code == VK_BACK,
          "replay_exit: kb Standard KEY:BACKSPACE");
    CHECK(xbox->replay_exit_set == 1 && xbox->replay_exit.n == 1 && xbox->replay_exit.alt[0].a.code == VK_BACK,
          "replay_exit: xbox Standard KEY:BACKSPACE");
    CHECK(imap_profile_parse("pause = KEY:PAUSE\n", "alt", &p) == 0 && p.replay_exit_set == 0 &&
          p.replay_exit.n == 1 && p.replay_exit.alt[0].a.code == VK_BACK,
          "replay_exit: altes Profil ohne Zeile -> Standard Ruecktaste");
    CHECK(imap_profile_parse("replay_exit =\n", "t", &p) == 0 && p.replay_exit_set == 1 && p.replay_exit.n == 0,
          "replay_exit: leere Zeile = aus");
    CHECK(imap_profile_parse("replay_exit = KEY:F6\nreplay_exit = KEY:END\n", "t", &p) == 0 && p.replay_exit.n == 1 &&
          p.replay_exit.alt[0].a.code == VK_END, "replay_exit: letzte Zeile gewinnt");
    CHECK(imap_profile_parse("replay_exit = KEY:CTRL+KEY:L, PAD:BACK+PAD:A, SBC:CHAFF, JOY1:B3\n", "t", &p) == 0 &&
          p.replay_exit.n == 4 && p.has_pad && p.has_sbc && (p.joy_used & 2u) != 0,
          "replay_exit: gelernt 4 Quellen (n=%d joy=0x%X)", p.replay_exit.n, p.joy_used);
    /* Pegel (alle Quellen inkl. Tastatur; die Wiedergabe prueft der Hook). */
    make_snap(&s, fr++, &c);
    set_keys(NULL, 0);
    CHECK(imap_action_replay_exit(kb, &s) == 0, "replay_exit: Ruhe -> 0");
    { int k[] = { VK_BACK }; set_keys(k, 1); }
    CHECK(imap_action_replay_exit(kb, &s) == 1, "replay_exit: Ruecktaste -> 1");
    CHECK(imap_action_replay_exit(kb, &s) == 1, "replay_exit: gehalten -> 1 (Pegel)");
    set_keys(NULL, 0);
    CHECK(imap_action_replay_exit(kb, &s) == 0, "replay_exit: los -> 0");
    memset(&s.pad, 0, sizeof s.pad);
    s.pad.buttons = IMAP_PAD_BACK | IMAP_PAD_A;
    s.pad_present = 1;
    CHECK(imap_action_replay_exit(&p, &s) == 1, "replay_exit: Pad BACK+A -> 1");
    s.pad.buttons = 0;
    s.pad_present = 0;
    s.sbc_present = 1;
    s.sbc[1] = (uint16_t)(1u << 2); /* bi18 CHAFF */
    CHECK(imap_action_replay_exit(&p, &s) == 1, "replay_exit: SBC CHAFF -> 1");
    s.sbc[1] = 0;
    s.sbc_present = 0;
    imap_profile_parse("replay_exit =\n", "t", &q);
    { int k[] = { VK_BACK }; set_keys(k, 1); }
    CHECK(imap_action_replay_exit(&q, &s) == 0, "replay_exit: aus -> Ruecktaste wirkungslos");
    set_keys(NULL, 0);
    CHECK(imap_profile_parse("replay_exit = KEY:F6\n", "t", &p) == 0 &&
          imap_profile_parse("replay_exit = KEY:F7\n", "t", &q) == 0 && imap_profile_equal(&p, &q) != 0,
          "replay_exit: Vergleich erkennt Unterschied");
    CHECK(imap_profile_parse("replay_exit =\n", "t", &p) == 0 && imap_profile_parse("pause =\n", "t", &q) == 0 &&
          imap_profile_equal(&p, &q) != 0, "replay_exit: Vergleich erkennt leere Zeile");
    /* bi0_nomouse: Maus (links) traegt im Spiel-Replay nicht zur Hauptwaffe bei. */
    imap_state_init(&st);
    make_snap(&s, fr++, &c);
    g_keys[324] = 1; /* MOUSE:LEFT */
    imap_profile_apply(&b, &st, kb, &s);
    CHECK((b.buttons[0] & 1u) != 0, "nomouse aus: Mausklick -> Hauptwaffe (bisher)");
    s.bi0_nomouse = 1;
    s.frame = fr++;
    imap_profile_apply(&b, &st, kb, &s);
    CHECK((b.buttons[0] & 1u) == 0, "nomouse an: Mausklick -> keine Hauptwaffe");
    g_keys[324] = 0;
    g_keys[325] = 1; /* MOUSE:RIGHT -> Subwaffe (Effekte im Replay) bleibt */
    s.frame = fr++;
    imap_profile_apply(&b, &st, kb, &s);
    CHECK((b.buttons[0] & 3u) == 2u, "nomouse an: rechte Maustaste -> Subwaffe bleibt");
    g_keys[325] = 0;
    g_keys[VK_SPACE] = 1;
    s.frame = fr++;
    imap_profile_apply(&b, &st, kb, &s);
    CHECK((b.buttons[0] & 1u) != 0, "nomouse an: Leertaste -> Hauptwaffe bleibt");
    g_keys[VK_SPACE] = 0;
    CHECK(imap_profile_parse("bi0 = MOUSE:LEFT, SBC:MAINWEAPON, PAD:A\n", "t", &p) == 0, "nomouse: Testprofil");
    s.sbc_present = 1;
    s.sbc[0] = 1u; /* Original-Controller Hauptwaffe */
    s.frame = fr++;
    imap_profile_apply(&b, &st, &p, &s);
    CHECK((b.buttons[0] & 1u) != 0, "nomouse an: Original-Controller -> Hauptwaffe (wie im Original)");
    s.sbc[0] = 0;
    s.sbc_present = 0;
    s.pad_present = 1;
    s.pad.buttons = IMAP_PAD_A;
    s.frame = fr++;
    imap_profile_apply(&b, &st, &p, &s);
    CHECK((b.buttons[0] & 1u) != 0, "nomouse an: Pad A -> Hauptwaffe (wie im Original)");
    s.pad.buttons = 0;
    s.pad_present = 0;
    g_keys[324] = 1;
    s.frame = fr++;
    imap_profile_apply(&b, &st, &p, &s);
    CHECK((b.buttons[0] & 1u) == 0, "nomouse an: Testprofil Mausklick -> keine Hauptwaffe");
    /* Legacy-Pfad (ohne Profil) */
    {
        imap_state_t lg;
        imap_state_init(&lg);
        s.bi0_nomouse = 0;
        s.frame = fr++;
        imap_legacy_apply(&b, &lg, &s);
        CHECK((b.buttons[0] & 1u) != 0, "Legacy nomouse aus: Mausklick -> Hauptwaffe");
        s.bi0_nomouse = 1;
        s.frame = fr++;
        imap_legacy_apply(&b, &lg, &s);
        CHECK((b.buttons[0] & 1u) == 0, "Legacy nomouse an: Mausklick -> keine Hauptwaffe");
    }
    set_keys(NULL, 0);
}

static void run_feedback_clip_cases(const imap_profile_t *kb, const imap_profile_t *xbox)
{
    imap_profile_t p;
    imap_state_t st;
    imap_snap_t s;
    cfgvar_t c = { 1, 0, 0, 0, 4, 0, 0 };
    unsigned fr = 320000, pm = 0;
    int vks[4], n;
    printf("Teil 15: W211 Aktion feedback_clip\n");
    CHECK(kb->feedback_clip.n == 1 && kb->feedback_clip.alt[0].a.kind == IMAP_A_KEY && kb->feedback_clip.alt[0].a.code == VK_F9,
          "feedback_clip: kb KEY:F9");
    CHECK(xbox->feedback_clip.n == 1 && xbox->feedback_clip.alt[0].a.code == VK_F9, "feedback_clip: xbox KEY:F9");
    n = imap_bind_keys(&kb->feedback_clip, vks, 4, &pm);
    CHECK(n == 1 && vks[0] == VK_F9 && pm == 0, "feedback_clip: bind_keys kb (n=%d)", n);
    CHECK(imap_profile_parse("feedback_clip = KEY:F7, PAD:BACK+PAD:LB, PAD:X\n", "t", &p) == 0, "feedback_clip: parse");
    CHECK(p.feedback_clip.n == 3 && p.pause.n == 0, "feedback_clip: n=3 (ist %d), pause unberuehrt", p.feedback_clip.n);
    n = imap_bind_keys(&p.feedback_clip, vks, 4, &pm);
    CHECK(n == 1 && vks[0] == VK_F7 && pm == IMAP_PAD_X, "feedback_clip: bind_keys F7 + Pad-Maske X (n=%d pm=0x%X)", n, pm);
    imap_state_init(&st);
    make_snap(&s, fr++, &c);
    set_keys(NULL, 0);
    CHECK(imap_action_feedback_clip(&p, &st, &s) == 0, "feedback_clip: Ruhe -> 0");
    make_snap(&s, fr++, &c);
    memset(&s.pad, 0, sizeof s.pad);
    s.pad.buttons = IMAP_PAD_X;
    s.pad_present = 1;
    CHECK(imap_action_feedback_clip(&p, &st, &s) == 1, "feedback_clip: Pad X -> Flanke");
    make_snap(&s, fr++, &c);
    memset(&s.pad, 0, sizeof s.pad);
    s.pad.buttons = IMAP_PAD_X;
    s.pad_present = 1;
    CHECK(imap_action_feedback_clip(&p, &st, &s) == 0, "feedback_clip: Pad X halten -> 0");
    s.pad.buttons = 0;
    CHECK(imap_action_feedback_clip(&p, &st, &s) == 0, "feedback_clip: release");
    s.pad.buttons = IMAP_PAD_BACK | IMAP_PAD_LB;
    CHECK(imap_action_feedback_clip(&p, &st, &s) == 1, "feedback_clip: chord edge");
    CHECK(imap_action_feedback_clip(&p, &st, &s) == 0, "feedback_clip: chord held");
    p.feedback_clip.n = 0;
    CHECK(imap_action_feedback_clip(&p, &st, &s) == 0 && st.prev_feedback_clip == 0,
          "feedback_clip: disabled resets edge");
    {
        int kf[] = { VK_F9 };
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(kf, 1);
        CHECK(imap_action_feedback_clip(kb, &st, &s) == 0, "feedback_clip: kb F9 im Hook -> 0 (Fenster)");
        set_keys(NULL, 0);
    }
    {
        imap_profile_t q;
        CHECK(imap_profile_parse("feedback_clip = KEY:F9\n", "t", &p) == 0 &&
              imap_profile_parse("feedback_clip = KEY:F7\n", "t", &q) == 0 && imap_profile_equal(&p, &q) != 0,
              "feedback_clip: Vergleich erkennt Unterschied");
    }
    CHECK(imap_key_name("F9") == VK_F9, "feedback_clip: Tastenname F9");
}

/* ---- Teil 16: W236 AIM-ASSIST (Sichtfuehrung) + W218 Automatik ---- */
static void blk_neutral(imap_block_t *b)
{
    memset(b, 0, sizeof *b);
    b->aiming_x = 0x7F;
    b->aiming_y = 0x7F;
    b->gear = 8;
}

/* Einfaches Fahrzeugmodell: je Gang Endgeschwindigkeit + Beschleunigung;
 * Gas zieht v gegen vmax[g] (R: -0,08), Bremse 0,92/Frame, sonst 0,97;
 * blocked = Hindernis (v bleibt 0). */
static float sim_v(float v, const imap_block_t *b, int blocked)
{
    static const float vmax[14] = { 0, 0, 0, 0, 0, 0, 0, -0.08f, 0, 0.10f, 0.18f, 0.26f, 0.33f, 0.40f };
    static const float acc[14] = { 0, 0, 0, 0, 0, 0, 0, 0.08f, 0, 0.08f, 0.06f, 0.045f, 0.035f, 0.03f };
    if (blocked) return 0.0f;
    if (b->pedal_m >= 0x40) return v * 0.92f;
    if (b->pedal_r >= 0x40 && b->gear != 8) return v + acc[b->gear] * (vmax[b->gear] - v);
    return v * 0.97f;
}

static void run_simple_cases(const imap_profile_t *kb)
{
    imap_block_t b;
    imap_autogear_t ag;
    imap_autogear_cfg_t c;
    uint8_t gst = 8;
    float v = 0.0f;
    unsigned f = 1000;
    int r, i, ups = 0, downs = 0, maxg = 0, g, prev;

    printf("Teil 16: W218 SIMPLE-CONTROLS (Automatik) + W236 AIM-ASSIST\n");
    /* -- W236 AIM-ASSIST: Sicht dem Waffenarm nachfuehren. Spielmodell wie
     * FUN_00037290/FUN_00034BE0 (BERICHT_M2 W236): Arm as += k*(t - as);
     * Sicht vyaw -= e_x*kv (+-pi/2), vpit += dir*e_y*kv (+-0.611), e_y =
     * sight_y + 1 (Out = ~sight_y), Totzone |e| <= 25; Fadenkreuz im Bild:
     * X = Arm - Sicht, Nick = Arm + Sicht-Nick (pit_rel -1). -- */
    {
        imap_assist_in_t in;
        imap_block_t b0;
        float vy = 0.0f, vp = 0.0f, as = 0.0f, ap = 0.0f, rel, tol, mx;
        int n, flips, last, ok, ex, ey;
        memset(&in, 0, sizeof in);
        in.kv = 45.0f * 0.0222222f * 0.0005f; /* FOV 45 */
        in.sx = -0.015f;
        in.sy = 0.0061359f;
        in.yaw_max = 1.5707964f;
        in.pit_max = 0.6108652f;
        in.edge_x = 0.30f;
        in.edge_y = 0.25f;
        in.pit_dir = 1;
        in.pit_rel = -1;
        in.pit_bias = -1;
        in.dz = 25;
        in.arm_kx = in.arm_ky = 1.0f;
        tol = 13.5f * in.kv + 1e-6f; /* halber kleinster Schritt (26 kv) */
#define AA_T(ax_) ((float)((ax_) >= 0x80 ? (ax_) - 0x80 : (ax_) - 0x7F) * in.sx)
#define AA_TP(ay_) ((float)((ay_) <= 0x7F ? 0x7F - (ay_) : 0x80 - (ay_)) * in.sy)
#define AA_STEP(mode_, ax_, ay_) do { \
            blk_neutral(&b); b.aiming_x = (uint8_t)(ax_); b.aiming_y = (uint8_t)(ay_); \
            in.vyaw = vy; in.vpit = vp; in.arm_x = as; in.arm_y = ap; \
            r = imap_aim_assist(&b, (mode_), &in); \
            ex = b.sight_x; if (ex <= 25 && ex >= -25) ex = 0; \
            ey = b.sight_y - in.pit_bias; if (ey <= 25 && ey >= -25) ey = 0; \
            vy -= (float)ex * in.kv; \
            if (vy > in.yaw_max) vy = in.yaw_max; \
            if (vy < -in.yaw_max) vy = -in.yaw_max; \
            vp += (float)in.pit_dir * (float)ey * in.kv; \
            if (vp > in.pit_max) vp = in.pit_max; \
            if (vp < -in.pit_max) vp = -in.pit_max; \
            as += in.arm_kx * (AA_T(ax_) - as); \
            ap += in.arm_ky * (AA_TP(ay_) - ap); } while (0)
        /* Aus/Original: Block bitgleich, auch mit Arm am Anschlag */
        ok = 1;
        for (i = 0; i < 256; i += 15) {
            blk_neutral(&b);
            b.aiming_x = (uint8_t)i;
            b.aiming_y = (uint8_t)(255 - i);
            b0 = b;
            in.vyaw = 0.3f;
            in.vpit = -0.2f;
            in.arm_x = 1.0f;
            in.arm_y = -0.5f;
            if (imap_aim_assist(&b, 0, &in) != 0 || memcmp(&b, &b0, sizeof b) != 0) ok = 0;
            if (imap_aim_assist(&b, 1, NULL) != 0 || memcmp(&b, &b0, sizeof b) != 0) ok = 0;
            if (imap_aim_assist(&b, 3, &in) != 0 || memcmp(&b, &b0, sizeof b) != 0) ok = 0;
        }
        CHECK(ok, "assist: Aus/Original (Stufe 0, ungueltig, NULL) -> Block bitgleich");
        /* Totzone: Rest 20 kv -> kleinster wirksamer Schritt 26, Rest 10 kv -> 0 */
        in.lock_axes = 1;
        blk_neutral(&b);
        b.aiming_x = 0x80 + 20;
        in.arm_x = AA_T(0x80 + 20);
        in.vyaw = in.arm_x + 20.0f * in.kv;
        imap_aim_assist(&b, 2, &in);
        CHECK(b.sight_x == 26, "assist: Rest 20 kv in der Totzone -> Schritt 26 (sx=%d)", (int)b.sight_x);
        blk_neutral(&b);
        b.aiming_x = 0x80 + 20;
        in.vyaw = in.arm_x - 10.0f * in.kv;
        imap_aim_assist(&b, 2, &in);
        CHECK(b.sight_x == 0, "assist: Rest 10 kv (< halber Schritt) -> 0 (sx=%d)", (int)b.sight_x);
        in.lock_axes = 0;
        /* Stufe 1, innerhalb des Rands: nichts */
        vy = vp = as = ap = 0.0f;
        AA_STEP(1, 0x80 + 19, 0x7F); /* Arm -0,285 rad < Rand 0,30 */
        CHECK(r == 0 && b.sight_x == 0 && b.sight_y == 0, "assist1: innerhalb des Rands -> Sicht unberuehrt (r=%d sx=%d sy=%d)",
              r, (int)b.sight_x, (int)b.sight_y);
        /* Stufe 1, geglaetteter Arm (relative Maus, k 0,0625): Rand -> Sicht folgt */
        in.arm_kx = in.arm_ky = 0.0625f;
        vy = vp = as = ap = 0.0f;
        flips = 0; last = 0; mx = 0.0f;
        for (n = 0; n < 150; n++) {
            AA_STEP(1, 0x80 + 40, 0x7F);
            if (b.sight_x && last && ((b.sight_x > 0) != (last > 0))) flips++;
            if (b.sight_x) last = b.sight_x;
            rel = as - vy;
            if (-rel > mx) mx = -rel;
        }
        rel = as - vy;
        CHECK(fabsf(rel + in.edge_x) <= tol && b.sight_x == 0 && flips == 0 && mx <= in.edge_x + tol,
              "assist1: Fadenkreuz bleibt am Rand (rel=%.4f, max=%.4f, Pendeln=%d)", rel, mx, flips);
        /* Rueckweg: Arm zur Mitte -> Sicht bleibt stehen */
        rel = vy;
        for (n = 0; n < 150; n++) AA_STEP(1, 0x7F, 0x7F);
        CHECK(fabsf(vy - rel) < 1e-6f, "assist1: Rueckweg zur Mitte -> Sicht steht (vy=%.3f)", vy);
        /* weiter links ueber den linken Rand -> Sicht dreht links */
        for (n = 0; n < 200; n++) AA_STEP(1, 0x7F - 40, 0x7F);
        rel = as - vy;
        CHECK(fabsf(rel - in.edge_x) <= tol && vy > 0.29f, "assist1: linker Rand -> Sicht links (vy=%.3f rel=%.4f)", vy, rel);
        /* Stufe 2 ohne Maus-Bit (Pad/Tasten) = Stufe 1 */
        in.arm_kx = in.arm_ky = 1.0f;
        vy = vp = as = ap = 0.0f;
        in.lock_axes = 0;
        AA_STEP(2, 0x80 + 19, 0x7F);
        CHECK(b.sight_x == 0, "assist2 ohne Maus-Achse: wie Stufe 1 (innerhalb Rand, sx=%d)", (int)b.sight_x);
        /* Stufe 2 (Maus, relativ k 0,0625): Fadenkreuz waehrend der ganzen
         * Bewegung in der Mitte (Sicht folgt dem geglaetteten Arm) */
        in.lock_axes = 3;
        in.arm_kx = in.arm_ky = 0.0625f;
        vy = vp = as = ap = 0.0f;
        flips = 0; last = 0; mx = 0.0f;
        for (n = 0; n < 150; n++) {
            AA_STEP(2, 0x80 + 20, 0x7F);
            if (b.sight_x && last && ((b.sight_x > 0) != (last > 0))) flips++;
            if (b.sight_x) last = b.sight_x;
            if (fabsf(as - vy) > mx) mx = fabsf(as - vy);
        }
        CHECK(mx <= tol && flips == 0, "assist2 relativ: Fadenkreuz in der Mitte (max |rel|=%.5f <= %.5f, Pendeln=%d)",
              mx, tol, flips);
        /* absolute Maus (k 1): Arm 20 -> 60 Stufen, je Frame +1 Stufe */
        in.arm_kx = in.arm_ky = 1.0f;
        mx = 0.0f;
        for (n = 20; n <= 60; n++) {
            AA_STEP(2, 0x80 + n, 0x7F);
            if (fabsf(as - vy) > mx) mx = fabsf(as - vy);
        }
        CHECK(mx <= tol, "assist2 absolut: Sicht folgt jede Stufe im selben Frame (max |rel|=%.5f)", mx);
        /* Anschlag: Arm ganz rechts -> Sicht am Anschlag, Rest nur der Arm */
        for (n = 0; n < 60; n++) AA_STEP(2, 0xFF, 0x7F);
        rel = as - vy;
        CHECK(fabsf(vy + in.yaw_max) < 1e-4f && rel < -0.30f && b.sight_x == 0,
              "assist2: Sicht am Anschlag (vy=%.4f), Arm allein weiter (rel=%.3f)", vy, rel);
        /* Rueckweg: zuerst der Arm (Sicht bleibt am Anschlag), dann die Sicht */
        AA_STEP(2, 0x80 + 106, 0x7F); /* -1,59 rad: noch jenseits des Anschlags */
        CHECK(b.sight_x == 0 && fabsf(vy + in.yaw_max) < 1e-4f,
              "assist2: Rueckweg erst Arm (Sicht bleibt %.4f)", vy);
        AA_STEP(2, 0x80 + 100, 0x7F); /* -1,50 rad: jetzt folgt die Sicht */
        last = b.sight_x;
        AA_STEP(2, 0x80 + 100, 0x7F); /* 0,07 rad > 127*kv: zweiter Frame */
        rel = as - vy;
        CHECK(last < 0 && fabsf(rel) <= tol, "assist2: dann Sicht zurueck (sx=%d rel=%.5f)", last, rel);
        /* Nick: Arm (Hebel 0x00-Seite) -> Sicht-Nick gegenlaeufig, Fadenkreuz Mitte */
        vy = vp = as = ap = 0.0f;
        for (n = 0; n < 40; n++) AA_STEP(2, 0x7F, 0x7F - 40);
        rel = ap + vp;
        CHECK(fabsf(rel) <= tol && vp < -0.2f && b.sight_y == 0,
              "assist2: Nick folgt dem Arm (Arm %.3f Sicht %.3f, Ruhe sy=%d)", ap, vp, (int)b.sight_y);
        for (n = 0; n < 80; n++) AA_STEP(2, 0x7F, 0x00); /* 127 Stufen = 0,78 rad > 0,611 */
        CHECK(fabsf(vp + in.pit_max) < 1e-4f && ap + vp > 0.1f,
              "assist2: Nick-Anschlag %.4f, Arm allein weiter (%.3f)", vp, ap);
        for (n = 0; n < 80; n++) AA_STEP(2, 0x7F, 0xFF); /* Gegenrichtung */
        CHECK(fabsf(vp - in.pit_max) <= tol, "assist2: Nick-Anschlag Gegenseite %.4f", vp);
        /* Nick, Stufe 1 innerhalb des Rands: sight_y unberuehrt */
        vp = ap = 0.0f;
        AA_STEP(1, 0x7F, 0x7F - 20);
        CHECK(b.sight_y == 0 && !(r & 2), "assist1: Nick innerhalb Rand unberuehrt (sy=%d)", (int)b.sight_y);
        /* eigene Sicht-Eingabe (Pfeile/Stick) gewinnt je Achse */
        blk_neutral(&b);
        b.aiming_x = 0xFF;
        b.sight_x = -50;
        in.vyaw = 0.0f;
        in.arm_x = 0.0f;
        r = imap_aim_assist(&b, 2, &in);
        CHECK(b.sight_x == -50 && !(r & 1), "assist: eigene Sicht-Eingabe gewinnt (sx=%d)", (int)b.sight_x);
        { /* Tastatur RIGHT (keyboard_default): Pfeil treibt Arm UND Sicht -> unveraendert */
            imap_state_t st;
            imap_snap_t sn;
            cfgvar_t cv = { 1, 1, 0, 1, 4, 0, 0 };
            int k = VK_RIGHT;
            imap_state_init(&st);
            set_keys(&k, 1);
            make_snap(&sn, 5, &cv);
            blk_neutral(&b);
            imap_profile_apply(&b, &st, kb, &sn);
            b0 = b;
            in.vyaw = 0.0f;
            r = imap_aim_assist(&b, 1, &in);
            CHECK(b.aiming_x == 0xFF && b.sight_x == 127 && memcmp(&b, &b0, sizeof b) == 0,
                  "assist: RIGHT -> aim %02X sight %d unveraendert", b.aiming_x, (int)b.sight_x);
            set_keys(NULL, 0);
        }
        /* Zoom: kleiner FOV -> kleinere Rate, gleiche Endlage */
        in.kv = 10.0f * 0.0222222f * 0.0005f;
        tol = 13.5f * in.kv + 1e-6f;
        in.lock_axes = 1;
        vy = as = 0.0f;
        for (n = 0; n < 300; n++) AA_STEP(2, 0x80 + 30, 0x7F);
        rel = as - vy;
        CHECK(fabsf(rel) <= tol, "assist2: Zoom (FOV 10) gleiche Endlage (rel=%.5f)", rel);
#undef AA_STEP
#undef AA_T
#undef AA_TP
    }

    /* -- Automatik: Anfahren und Hochschalten 1..5 -- */
    imap_autogear_defaults(&c);
    memset(&ag, 0, sizeof ag);
    prev = 8;
    for (i = 0; i < 1500; i++, f++) {
        blk_neutral(&b);
        b.gear = gst;
        b.pedal_r = 0xFF;
        g = imap_autogear_step(&ag, &c, &b, &gst, v, f);
        /* zweiter Poll im selben Frame darf nichts zusaetzlich schalten */
        b.gear = gst;
        CHECK(imap_autogear_step(&ag, &c, &b, &gst, v, f) == g, "autogear: 2. Poll gleicher Gang");
        if (g > prev) ups++;
        if (g < prev) downs++;
        if (g > maxg) maxg = g;
        prev = g;
        v = sim_v(v, &b, 0);
    }
    CHECK(gst == 13 && maxg == 13, "autogear: Gas lang -> 5. Gang (gst=%u)", gst);
    CHECK(ups == 5 && downs == 0, "autogear: 5x hoch, 0x runter (hoch=%d runter=%d)", ups, downs);
    CHECK(ag.vtop[9] > 0.09f && ag.vtop[12] > 0.30f, "autogear: Endgeschw. gelernt (1:%.3f 4:%.3f)",
          ag.vtop[9], ag.vtop[12]);

    /* -- Bremsen bis Stillstand: runter, dann R -- */
    downs = 0;
    for (i = 0; i < 400; i++, f++) {
        blk_neutral(&b);
        b.gear = gst;
        b.pedal_m = 0xFF;
        g = imap_autogear_step(&ag, &c, &b, &gst, v, f);
        if (g < prev && g != 7) downs++;
        prev = g;
        v = sim_v(v, &b, 0);
    }
    CHECK(gst == 7, "autogear: Bremse im Stand -> R (gst=%u)", gst);
    CHECK(downs >= 1, "autogear: beim Bremsen heruntergeschaltet (%d)", downs);
    blk_neutral(&b);
    b.gear = gst;
    b.pedal_m = 0xC0;
    imap_autogear_step(&ag, &c, &b, &gst, v, f++);
    CHECK(b.gear == 7 && b.pedal_r == 0xC0 && b.pedal_m == 0, "autogear: in R Bremse = Gas (r=%u m=%u)",
          b.pedal_r, b.pedal_m);
    /* rueckwaerts fahren */
    for (i = 0; i < 200; i++, f++) {
        blk_neutral(&b);
        b.gear = gst;
        b.pedal_m = 0xFF;
        imap_autogear_step(&ag, &c, &b, &gst, v, f);
        v = sim_v(v, &b, 0);
    }
    CHECK(gst == 7 && v < -0.05f, "autogear: rueckwaerts (v=%.3f)", v);
    /* Gas in R: erst bremsen, dann 1. Gang */
    blk_neutral(&b);
    b.gear = gst;
    b.pedal_r = 0xFF;
    imap_autogear_step(&ag, &c, &b, &gst, v, f++);
    CHECK(b.gear == 7 && b.pedal_m == 0xFF && b.pedal_r == 0, "autogear: Gas in Rueckwaertsfahrt bremst");
    for (i = 0; i < 300 && gst == 7; i++, f++) {
        blk_neutral(&b);
        b.gear = gst;
        b.pedal_r = 0xFF;
        imap_autogear_step(&ag, &c, &b, &gst, v, f);
        v = sim_v(v, &b, 0);
    }
    CHECK(gst == 9, "autogear: nach Stillstand Gas -> 1. Gang (gst=%u)", gst);

    /* -- Hindernis: Gas, v bleibt 0 -> kein Hochschalten -- */
    memset(&ag, 0, sizeof ag);
    gst = 9;
    v = 0.0f;
    for (i = 0; i < 600; i++, f++) {
        blk_neutral(&b);
        b.gear = gst;
        b.pedal_r = 0xFF;
        imap_autogear_step(&ag, &c, &b, &gst, v, f);
    }
    CHECK(gst == 9, "autogear: festgefahren bleibt 1. Gang (gst=%u)", gst);

    /* -- Steigung: gelernte Endgeschw., Plateau bei 42 % -> kein Hoch, runter -- */
    memset(&ag, 0, sizeof ag);
    ag.init = 1;
    ag.last_frame = f - 1;
    ag.shift_frame = f - 100;
    ag.vtop[9] = 0.10f;
    ag.vtop[10] = 0.18f;
    ag.vtop[11] = 0.26f;
    gst = 11; /* 3. Gang */
    for (i = 0; i < 200; i++, f++) {
        blk_neutral(&b);
        b.gear = gst;
        b.pedal_r = 0xFF;
        imap_autogear_step(&ag, &c, &b, &gst, 0.11f, f);
    }
    CHECK(gst == 10, "autogear: Steigung 3 -> 2, kein Pendeln (gst=%u)", gst);

    /* -- N + Gas -> 1; manuelle Flanke wird uebernommen -- */
    memset(&ag, 0, sizeof ag);
    blk_neutral(&b);
    gst = 8;
    b.pedal_r = 0xFF;
    imap_autogear_step(&ag, &c, &b, &gst, 0.0f, f++);
    CHECK(gst == 9 && b.gear == 9, "autogear: N + Gas -> 1");
    blk_neutral(&b);
    b.gear = 11; /* Spieler schaltet manuell (Q/E) */
    imap_autogear_step(&ag, &c, &b, &gst, 0.2f, f++);
    CHECK(gst == 11 && b.gear == 11, "autogear: manueller Gang bleibt Ausgangspunkt");
    blk_neutral(&b);
    b.gear = 9;
    imap_autogear_step(&ag, &c, &b, &gst, 0.0f, f++);
    CHECK(gst == 9 && b.pedal_r == 0 && b.pedal_m == 0, "autogear: ohne Pedal keine Aenderung");
}

/* ---- Teil 17: W221 HOTAS (experimentell) - simulierte DirectInput-Geraete ---- */
static void joy_snap(imap_snap_t *s, unsigned frame)
{
    static const cfgvar_t c = {1, 0, 0, 1, 3, 0, 0};
    int k, i;
    make_snap(s, frame, &c);
    for (k = 0; k < IMAP_JOY_MAX; k++) {
        for (i = 0; i < IJOY_POVS; i++) s->joy[k].pov[i] = -1;
    }
}

static void joy_btn(imap_snap_t *s, int slot, int b1, int on)
{
    int i = b1 - 1;
    if (on) s->joy[slot].btn[i >> 5] |= 1u << (i & 31);
    else s->joy[slot].btn[i >> 5] &= ~(1u << (i & 31));
}

static int16_t joy_pos(float f) /* -1..1 -> DirectInput-Bereich */
{
    float v = f >= 0 ? f * 32767.0f : f * 32768.0f;
    return (int16_t)(v >= 0 ? v + 0.5f : v - 0.5f);
}

static const char kJoyProf[] =
    "[joystick]\n"
    "joy0 = T.16000M\n"
    "joy1 = TWCS Throttle\n"
    "joy2 = {11111111-2222-3333-4444-555555555555}\n"
    "aim_x = JOY0:X:dz=0.1\n"
    "aim_x.neg = KEY:LEFT:cond=!SHIFT\n" /* digital + Stick */
    "aim_y = JOY0:Y:inv=1\n"
    "rot = JOY0:RZ:dz=0.05:curve=2\n"
    "rot.neg = JOY0:B5\n"
    "rot.pos = JOY0:B6\n"
    "sight_x = JOY0:S0:sens=2\n"
    "pedal_r = KEY:W, JOY1:Z:inv=1\n"
    "pedal_m = JOY2:RZ:dz=0.1\n"
    "pedal_l = JOY2:X:half=1\n"
    "bi0 = KEY:SPACE, JOY0:B1\n"
    "bi2 = JOY0:B2+JOY0:B3\n"
    "bi18 = JOY0:Y-\n"
    "bi27 = JOY1:RX:zone=40-60\n"
    "tuner.up = JOY0:POV1U\n"
    "tuner.down = JOY0:POV1D\n"
    "gear.up = JOY1:B1\n"
    "gear.down = JOY1:B2\n"
    "gear.r = JOY1:S0:zone=0-10\n"
    "gear.n = JOY1:S0:zone=10-25\n"
    "gear.1 = JOY1:S0:zone=25-40\n"
    "gear.2 = JOY1:S0:zone=40-55\n"
    "gear.3 = JOY1:S0:zone=55-70\n"
    "gear.4 = JOY1:S0:zone=70-85\n"
    "gear.5 = JOY1:S0:zone=85-100\n"
    "bi1 = JOY5:B1, JOY0:B0, JOY0:POV9U, JOY0:Q, JOY0:B4\n" /* ungueltig bis auf B4 */
    "sight_y = JOY0:Y:dz=2\n";                              /* ungueltig */

static void run_joy_cases(const imap_profile_t *kb, const imap_profile_t *xbox)
{
    imap_profile_t p, p2;
    imap_snap_t s;
    imap_block_t b;
    imap_state_t st;
    unsigned f = 1;
    int i;
    printf("Teil 17: W221 HOTAS (simulierte Geraete)\n");

    /* Profil: Slots, Geraete, Bitgleichheit der Standardprofile. */
    CHECK(imap_profile_parse(kJoyProf, "joytest", &p) == 0, "joy: parse");
    CHECK(p.joy_used == 7u, "joy: Slots 0..2 benutzt (%u)", p.joy_used);
    CHECK(strcmp(p.joy_dev[0], "T.16000M") == 0 && strcmp(p.joy_dev[1], "TWCS Throttle") == 0 &&
              p.joy_dev[2][0] == '{' && p.joy_dev[3][0] == 0,
          "joy: Geraete-Zeilen");
    CHECK(imap_profile_parse(kJoyProf, "joytest", &p2) == 0 && imap_profile_equal(&p, &p2) == 0,
          "joy: Profil stabil");
    p2.axis[IMAP_AX_AIM_X].pad[0].sens = 1.5f;
    CHECK(imap_profile_equal(&p, &p2) != 0, "joy: Vergleich sieht sens");
    CHECK(kb->joy_used == 0 && xbox->joy_used == 0, "joy: Standardprofile ohne JOY");
    CHECK(p.btn[1].n == 1 && p.btn[1].alt[0].a.kind == IMAP_A_JOY &&
              IMAP_JOY_IDX(p.btn[1].alt[0].a.code) == 3,
          "joy: ungueltige Quellen ignoriert (nur B4 bleibt)");
    CHECK(p.axis[IMAP_AX_SIGHT_Y].pad[0].src < 0, "joy: dz=2 abgelehnt");
    CHECK(p.axis[IMAP_AX_AIM_X].pad[0].src == IMAP_SRC_JOY + 0 &&
              p.axis[IMAP_AX_PEDAL_M].pad[0].src == IMAP_SRC_JOY + 2 * 8 + 5,
          "joy: Analog-Quellen kodiert");

    /* Ohne Geraet: Block wie ohne JOY-Zeilen (nur Tastatur wirkt). */
    imap_state_init(&st);
    joy_snap(&s, f++);
    imap_profile_apply(&b, &st, &p, &s);
    CHECK(b.aiming_x == 0x7F && b.aiming_y == 0x7F && b.rot_lever == 0 && b.sight_x == 0 &&
              b.pedal_l == 0 && b.pedal_m == 0 && b.pedal_r == 0 && b.buttons[0] == 0 &&
              b.buttons[1] == 0 && b.gear == 8 && b.tuner == 0,
          "joy: ohne Geraet neutral");
    {
        int k[1] = {'W'};
        set_keys(k, 1);
        joy_snap(&s, f++);
        imap_profile_apply(&b, &st, &p, &s);
        CHECK(b.pedal_r == 0xFF, "joy: Tastatur bleibt live (W)");
        set_keys(NULL, 0);
    }

    /* Abbildung, Deadzone, Invertierung, Kurve, Empfindlichkeit. */
    imap_state_init(&st);
    joy_snap(&s, f++);
    s.joy_mask = 7u;
    s.joy[0].axis[0] = 32767;        /* X voll rechts */
    s.joy[0].axis[1] = joy_pos(1.0f); /* Y voll (+) -> inv -> 0x00 */
    s.joy[0].axis[5] = joy_pos(0.525f); /* RZ: (0.525-0.05)/0.95 = 0.5 -> ^2 = 0.25 */
    s.joy[0].axis[6] = joy_pos(0.25f);  /* S0 * sens 2 = 0.5 */
    s.joy[1].axis[2] = -32768;        /* Schub voll vorn (Minimum) -> inv -> 255 */
    s.joy[2].axis[5] = joy_pos(0.0f); /* Pedal Mitte = halber Weg */
    s.joy[2].axis[0] = joy_pos(-0.5f); /* half: negative Haelfte = 0 */
    imap_profile_apply(&b, &st, &p, &s);
    CHECK(b.aiming_x == 0xFF, "joy: X voll -> 0xFF (%02X)", b.aiming_x);
    CHECK(b.aiming_y == 0x00, "joy: Y inv -> 0x00 (%02X)", b.aiming_y);
    CHECK(b.rot_lever >= 31 && b.rot_lever <= 33, "joy: Totzone+Kurve rot=%d (32)", b.rot_lever);
    CHECK(b.sight_x >= 63 && b.sight_x <= 64, "joy: sens 2 sight_x=%d (64)", b.sight_x);
    CHECK(b.pedal_r == 0xFF, "joy: Schub inv voll -> 255 (%u)", b.pedal_r);
    CHECK(b.pedal_m >= 112 && b.pedal_m <= 114, "joy: Pedal Mitte dz 0.1 -> %u (113)", b.pedal_m);
    CHECK(b.pedal_l == 0, "joy: half=1 negative Haelfte -> 0 (%u)", b.pedal_l);
    s.joy[0].axis[0] = joy_pos(0.08f); /* in der Totzone 0.1 */
    s.joy[0].axis[1] = joy_pos(-0.5f); /* inv -> +0.5 */
    s.joy[1].axis[2] = 32767;          /* Schub hinten -> 0 */
    s.joy[2].axis[0] = joy_pos(0.5f);  /* half: +0.5 -> 128 */
    s.joy[2].axis[5] = joy_pos(-0.85f); /* Pedal fast Ruhe: (0.075) < dz -> 0 */
    imap_profile_apply(&b, &st, &p, &s);
    CHECK(b.aiming_x == 0x7F, "joy: Totzone -> Mitte (%02X)", b.aiming_x);
    CHECK(b.aiming_y == 0x7F + 64, "joy: Y inv halb -> %02X (BF)", b.aiming_y);
    CHECK(b.pedal_r == 0, "joy: Schub hinten -> 0 (%u)", b.pedal_r);
    CHECK(b.pedal_l == 128, "joy: half +0.5 -> 128 (%u)", b.pedal_l);
    CHECK(b.pedal_m == 0, "joy: Pedal-Totzone am Ruhe-Ende (%u)", b.pedal_m);
    s.joy[0].axis[0] = -32768;
    imap_profile_apply(&b, &st, &p, &s);
    CHECK(b.aiming_x == 0x00, "joy: X voll links -> 0x00 (%02X)", b.aiming_x);
    { /* Digital (Pfeil, voll) schlaegt den halb ausgelenkten Stick. */
        int k[1] = {VK_LEFT};
        set_keys(k, 1);
        s.joy[0].axis[0] = joy_pos(0.5f);
        imap_profile_apply(&b, &st, &p, &s);
        CHECK(b.aiming_x == 0x00, "joy: Pfeil links vor Stick halb rechts (%02X)", b.aiming_x);
        set_keys(NULL, 0);
    }

    /* Mehrere Geraete: Slot 1 weg -> nur dessen Quellen schweigen. */
    s.joy_mask = 5u;
    s.joy[0].axis[0] = 32767;
    s.joy[1].axis[2] = -32768;
    s.joy[2].axis[0] = 32767;
    imap_profile_apply(&b, &st, &p, &s);
    CHECK(b.aiming_x == 0xFF && b.pedal_r == 0 && b.pedal_l == 0xFF,
          "joy: Slot 1 fehlt -> Schub 0, Stick+Pedale live");
    s.joy_mask = 7u;

    /* Tasten, Akkord, POV, Achse-als-Taste, Zone. */
    memset(s.joy[0].btn, 0, sizeof s.joy[0].btn);
    s.joy[0].axis[0] = s.joy[0].axis[1] = 0;
    joy_btn(&s, 0, 1, 1);
    imap_profile_apply(&b, &st, &p, &s);
    CHECK((b.buttons[0] & 1u) != 0, "joy: B1 -> Hauptwaffe");
    joy_btn(&s, 0, 2, 1);
    joy_btn(&s, 0, 3, 1);
    imap_profile_apply(&b, &st, &p, &s);
    CHECK((b.buttons[0] & 4u) != 0, "joy: Akkord B2+B3 -> Lock-On");
    joy_btn(&s, 0, 1, 0);
    joy_btn(&s, 0, 3, 0);
    imap_profile_apply(&b, &st, &p, &s);
    CHECK((b.buttons[0] & 5u) == 0, "joy: Tasten los");
    joy_btn(&s, 0, 2, 0);
    joy_btn(&s, 0, 4, 1);
    imap_profile_apply(&b, &st, &p, &s);
    CHECK((b.buttons[0] & 2u) != 0, "joy: B4 -> Subwaffe");
    joy_btn(&s, 0, 4, 0);
    s.joy[0].axis[1] = joy_pos(-0.9f); /* Y- (unteres Viertel) */
    imap_profile_apply(&b, &st, &p, &s);
    CHECK((b.buttons[1] & (1u << 2)) != 0, "joy: Achse Y- -> Chaff");
    s.joy[0].axis[1] = joy_pos(-0.3f);
    imap_profile_apply(&b, &st, &p, &s);
    CHECK((b.buttons[1] & (1u << 2)) == 0, "joy: Achse Y Mitte -> kein Chaff");
    s.joy[1].axis[3] = 0; /* RX Mitte = 50 % -> Zone 40-60 */
    imap_profile_apply(&b, &st, &p, &s);
    CHECK((b.buttons[1] & (1u << 11)) != 0, "joy: Zone 40-60 -> Magazin");
    s.joy[1].axis[3] = joy_pos(0.4f); /* 70 % */
    imap_profile_apply(&b, &st, &p, &s);
    CHECK((b.buttons[1] & (1u << 11)) == 0, "joy: ausserhalb Zone");
    /* Tasten -> Achse (Drehhebel per Tasten). */
    joy_btn(&s, 0, 5, 1);
    imap_profile_apply(&b, &st, &p, &s);
    CHECK(b.rot_lever == -127, "joy: B5 -> Drehhebel links (%d)", b.rot_lever);
    joy_btn(&s, 0, 5, 0);
    joy_btn(&s, 0, 6, 1);
    imap_profile_apply(&b, &st, &p, &s);
    CHECK(b.rot_lever == 127, "joy: B6 -> Drehhebel rechts (%d)", b.rot_lever);
    joy_btn(&s, 0, 6, 0);
    /* POV: Flanken auf den Tuner, Diagonale loest Nachbarn aus. */
    {
        int t0 = st.tuner;
        s.joy[0].pov[0] = 0;
        imap_profile_apply(&b, &st, &p, &s);
        imap_profile_apply(&b, &st, &p, &s); /* gehalten: eine Flanke */
        CHECK(b.tuner == t0 + 1, "joy: POV hoch -> Tuner +1 (%u)", b.tuner);
        s.joy[0].pov[0] = -1;
        imap_profile_apply(&b, &st, &p, &s);
        s.joy[0].pov[0] = 13500; /* rechts-unten -> zaehlt als runter */
        imap_profile_apply(&b, &st, &p, &s);
        CHECK(b.tuner == t0, "joy: POV Diagonale unten-rechts -> Tuner -1 (%u)", b.tuner);
        s.joy[0].pov[0] = 9000; /* rechts: weder hoch noch runter */
        imap_profile_apply(&b, &st, &p, &s);
        s.joy[0].pov[0] = -1;
        imap_profile_apply(&b, &st, &p, &s);
        CHECK(b.tuner == t0, "joy: POV rechts ohne Wirkung (%u)", b.tuner);
    }

    /* Tasten -> Gang (Schalthebel per Tasten), dann Achse -> Gang (Zonen). */
    imap_state_init(&st);
    joy_snap(&s, f++);
    s.joy_mask = 7u;
    s.joy[1].axis[6] = joy_pos(0.0f); /* S0 Mitte = 50 % -> Zone gear.2 */
    /* Erst ohne Zonen-Geraet pruefen: Zonen schweigen, wenn Slot fehlt. */
    s.joy_mask = 5u;
    imap_profile_apply(&b, &st, &p, &s);
    CHECK(b.gear == 8 && !st.gear_direct_hit, "joy: Slot fehlt -> N, keine Direktwahl");
    s.joy_mask = 7u;
    s.joy[1].axis[6] = -32768; /* 0 % -> R */
    imap_profile_apply(&b, &st, &p, &s);
    CHECK(b.gear == 7 && st.gear_direct_hit, "joy: Schub 0 %% -> R (%u)", b.gear);
    {
        static const struct { float pct; int gear; } z[] = {
            {5, 7}, {15, 8}, {30, 9}, {47, 10}, {60, 11}, {77, 12}, {92, 13}, {100, 13}, {0, 7}};
        for (i = 0; i < (int)(sizeof z / sizeof z[0]); i++) {
            float fr = z[i].pct / 100.0f * 2.0f - 1.0f;
            s.joy[1].axis[6] = joy_pos(fr);
            imap_profile_apply(&b, &st, &p, &s);
            CHECK(b.gear == z[i].gear, "joy: Zone %.0f%% -> Gang %u (soll %d)", z[i].pct, b.gear,
                  z[i].gear);
        }
    }
    /* Tasten-Gang ohne Zonen (eigenes Profil: Gangknueppel per Tasten). */
    {
        static const char kG[] = "joy0 = Wheel\n"
                                 "gear.up = JOY0:B1\n"
                                 "gear.down = JOY0:B2\n"
                                 "pedal_r = JOY0:Y:inv=1\n";
        imap_profile_t g;
        CHECK(imap_profile_parse(kG, "joygear", &g) == 0 && g.joy_used == 1u, "joy: Gang-Profil");
        imap_state_init(&st);
        joy_snap(&s, f++);
        s.joy_mask = 1u;
        s.joy[0].axis[1] = 32767; /* Pedal (inv) los -> 0 */
        joy_btn(&s, 0, 1, 1);
        imap_profile_apply(&b, &st, &g, &s);
        imap_profile_apply(&b, &st, &g, &s);
        CHECK(b.gear == 9 && b.pedal_r == 0, "joy: B1 gehalten -> eine Flanke N->1 (%u)", b.gear);
        joy_btn(&s, 0, 1, 0);
        imap_profile_apply(&b, &st, &g, &s);
        joy_btn(&s, 0, 1, 1);
        imap_profile_apply(&b, &st, &g, &s);
        CHECK(b.gear == 10, "joy: B1 erneut -> 2 (%u)", b.gear);
        joy_btn(&s, 0, 1, 0);
        joy_btn(&s, 0, 2, 1);
        imap_profile_apply(&b, &st, &g, &s);
        CHECK(b.gear == 9 && !st.gear_direct_hit, "joy: B2 -> 1, keine Direktwahl (%u)", b.gear);
    }

    /* Maus + Stick auf derselben Achse (W132-Regel gilt auch fuer JOY):
     * Maus ruht, Stick bewegt -> Stick. */
    {
        static const char kM[] = "aim_x = MOUSE:X:mode=aim, JOY0:X\n";
        static const cfgvar_t cm = {1, 1, 0, 1, 3, 0, 0};
        imap_profile_t m;
        CHECK(imap_profile_parse(kM, "joymouse", &m) == 0 && m.joy_used == 1u, "joy: Maus-Profil");
        imap_state_init(&st);
        for (i = 0; i < 3; i++) {
            make_snap(&s, f++, &cm);
            s.pad_axis_arb = 1;
            s.pad_arb_dead = 8;
            s.pad_mouse_wake = 3;
            s.joy_mask = 1u;
            s.joy[0].axis[0] = i == 0 ? 0 : joy_pos(0.5f);
            imap_profile_apply(&b, &st, &m, &s);
        }
        CHECK(b.aiming_x == 0x7F + 64, "joy: Stick schlaegt ruhende Maus (%02X)", b.aiming_x);
    }

    /* W226 JOY-AXIS-DIR: umgekehrter Schubhebel (Ruhe = Maximum 32767).
     * Analog per inv=1; Achse als Taste Z+ (vorher als leerer Akkord
     * verworfen); inv=1 an Z+/zone spiegelt den Weg (0 % = Maximum). */
    {
        static const char kI[] = "joy0 = Throttle\n"
                                 "pedal_r = JOY0:Z:inv=1\n"
                                 "bi18 = JOY0:Z+\n"
                                 "bi19 = JOY0:Z+:inv=1\n"
                                 "bi20 = JOY0:Z++JOY0:B1\n"
                                 "bi21 = KEY:A:inv=1\n"  /* ungueltig */
                                 "bi22 = JOY0:B2:inv=1\n" /* ungueltig */
                                 "gear.r = JOY0:Z:zone=0-10:inv=1\n"
                                 "gear.5 = JOY0:Z:inv=1:zone=90-100\n";
        static const char kI0[] = "joy0 = Throttle\n"
                                  "pedal_r = JOY0:Z:inv=1\n"
                                  "bi18 = JOY0:Z+\n"
                                  "bi19 = JOY0:Z+:inv=0\n"
                                  "bi20 = JOY0:Z++JOY0:B1\n"
                                  "gear.r = JOY0:Z:zone=0-10:inv=1\n"
                                  "gear.5 = JOY0:Z:inv=1:zone=90-100\n";
        imap_profile_t q, q2;
        CHECK(imap_profile_parse(kI, "joyinv", &q) == 0 && q.joy_used == 1u, "w226: parse");
        CHECK(q.btn[18].n == 1 && q.btn[18].alt[0].a.kind == IMAP_A_JOY &&
                  IMAP_JOY_LO(q.btn[18].alt[0].a.code) == 75 &&
                  IMAP_JOY_HI(q.btn[18].alt[0].a.code) == 100,
              "w226: JOY0:Z+ geparst (oberes Viertel)");
        CHECK(q.btn[19].n == 1 && (q.btn[19].alt[0].a.code & IMAP_JOY_INVF) != 0 &&
                  (q.btn[18].alt[0].a.code & IMAP_JOY_INVF) == 0,
              "w226: Z+:inv=1 traegt Spiegel-Bit");
        CHECK(q.btn[20].n == 1 && q.btn[20].alt[0].chord, "w226: Akkord Z++B1");
        CHECK(q.btn[21].n == 0 && q.btn[22].n == 0, "w226: inv an Taste abgelehnt");
        CHECK(q.gear_direct[0].n == 1 && q.gear_direct[6].n == 1 &&
                  (q.gear_direct[0].alt[0].a.code & IMAP_JOY_INVF) &&
                  (q.gear_direct[6].alt[0].a.code & IMAP_JOY_INVF) &&
                  IMAP_JOY_LO(q.gear_direct[6].alt[0].a.code) == 90,
              "w226: zone+inv in beiden Reihenfolgen");
        CHECK(imap_profile_parse(kI0, "joyinv0", &q2) == 0 && imap_profile_equal(&q, &q2) != 0,
              "w226: Vergleich sieht inv an Zone");
        imap_state_init(&st);
        joy_snap(&s, f++);
        s.joy_mask = 1u;
        s.joy[0].axis[2] = 32767; /* Schub in Ruhe (Maximum) */
        imap_profile_apply(&b, &st, &q, &s);
        CHECK(b.pedal_r == 0, "w226: Schub Ruhe 32767 + inv -> 0 (%u)", b.pedal_r);
        CHECK((b.buttons[1] & (1u << 2)) != 0 && (b.buttons[1] & (1u << 3)) == 0,
              "w226: Z+ roh oben an, Z+:inv aus (%04X)", b.buttons[1]);
        CHECK(b.gear == 7 && st.gear_direct_hit, "w226: zone 0-10 inv -> R bei Ruhe (%u)", b.gear);
        joy_btn(&s, 0, 1, 1);
        imap_profile_apply(&b, &st, &q, &s);
        CHECK((b.buttons[1] & (1u << 4)) != 0, "w226: Akkord Z++B1 -> an");
        joy_btn(&s, 0, 1, 0);
        s.joy[0].axis[2] = -32768; /* Vollgas (Minimum) */
        imap_profile_apply(&b, &st, &q, &s);
        CHECK(b.pedal_r == 0xFF, "w226: Vollgas -32768 + inv -> 255 (%u)", b.pedal_r);
        CHECK((b.buttons[1] & (1u << 2)) == 0 && (b.buttons[1] & (1u << 3)) != 0 &&
                  (b.buttons[1] & (1u << 4)) == 0,
              "w226: Z+ aus, Z+:inv an (%04X)", b.buttons[1]);
        CHECK(b.gear == 13, "w226: zone 90-100 inv -> Gang 5 bei Vollgas (%u)", b.gear);
        s.joy[0].axis[2] = joy_pos(-0.2f); /* roh 40 % = logisch 60 % */
        imap_profile_apply(&b, &st, &q, &s);
        CHECK((b.buttons[1] & (3u << 2)) == 0, "w226: Mitte-vorn weder Z+ noch Z+:inv");
    }

    /* Geraete-Zuordnung (rein, ohne DirectInput). */
    {
        ijoy_info_t L[4];
        memset(L, 0, sizeof L);
        strcpy_s(L[0].name, sizeof L[0].name, "T.16000M");
        strcpy_s(L[0].product, sizeof L[0].product, "T.16000M");
        strcpy_s(L[0].guid, sizeof L[0].guid, "{AAAAAAAA-0000-0000-0000-000000000001}");
        strcpy_s(L[1].name, sizeof L[1].name, "TWCS Throttle");
        strcpy_s(L[1].product, sizeof L[1].product, "TWCS Throttle");
        strcpy_s(L[1].guid, sizeof L[1].guid, "{AAAAAAAA-0000-0000-0000-000000000002}");
        strcpy_s(L[2].name, sizeof L[2].name, "T-Rudder");
        strcpy_s(L[2].product, sizeof L[2].product, "T-Rudder");
        strcpy_s(L[2].guid, sizeof L[2].guid, "{AAAAAAAA-0000-0000-0000-000000000003}");
        strcpy_s(L[2].pguid, sizeof L[2].pguid, "{B679044F-0000-0000-0000-504944564944}");
        strcpy_s(L[3].name, sizeof L[3].name, "T.16000M");
        strcpy_s(L[3].product, sizeof L[3].product, "T.16000M");
        CHECK(ijoy_match_list(L, 4, "throttle", 0) == 1, "joy: Name-Teilstring");
        CHECK(ijoy_match_list(L, 4, "{aaaaaaaa-0000-0000-0000-000000000003}", 0) == 2,
              "joy: Instanz-GUID");
        CHECK(ijoy_match_list(L, 4, "{B679044F-0000-0000-0000-504944564944}", 0) == 2,
              "joy: Produkt-GUID");
        CHECK(ijoy_match_list(L, 4, "T.16000M@2", 0) == 3, "joy: @2 = zweites gleiches Geraet");
        CHECK(ijoy_match_list(L, 4, "", 2) == 2 && ijoy_match_list(L, 4, NULL, 5) == -1,
              "joy: leer = Slot-Reihenfolge");
        CHECK(ijoy_match_list(L, 4, "Saitek", 0) == -1, "joy: unbekannt -> -1");
        CHECK(ijoy_axis_parse("rz") == 5 && ijoy_axis_parse("SLIDER") == 6 &&
                  ijoy_axis_parse("W") == -1,
              "joy: Achsennamen");
    }
}

/* W260 MOUSE-WHEEL: Rasten -> Impulse (Profil-Pfad, echte Maus). */
static void wheel_snap(imap_snap_t *s, unsigned frame, unsigned mask)
{
    static const cfgvar_t c = { 1, 0, 0, 1, 4, 0, 0 };
    make_snap(s, frame, &c);
    s->replay_active = 0;  /* echter Pfad */
    s->mouse_raw = 1;      /* keine Cursor-Abfrage */
    s->real_vk = NULL;
    s->wheel_mask = mask;
}
static int wheel_real_vk(int vk) { (void)vk; return 0; }
static void run_wheel_cases(const imap_profile_t *kb, const imap_profile_t *xbox)
{
    static const char kW[] = "tuner.up = KEY:T, MOUSE:WHEEL_UP\n"
                             "tuner.down = KEY:G, mouse:wheel_down\n"
                             "gear.up = MOUSE:WHEEL_RIGHT\n"
                             "gear.down = MOUSE:Wheel_Left\n"
                             "bi20 = MOUSE:WHEEL\n"          /* ungueltig */
                             "bi21 = MOUSE:WHEEL_UPX\n";     /* ungueltig */
    imap_profile_t p;
    imap_wheel_t w;
    imap_block_t b;
    imap_state_t st;
    imap_snap_t sn;
    unsigned f = 200000, m, seq = 0;
    int i, ok;

    printf("Teil 18: W260 Mausrad\n");
    set_keys(NULL, 0);
    CHECK(imap_profile_parse(kW, "wheel", &p) == 0, "wheel: Profil geparst");
    CHECK(p.tuner_up.n == 2 && p.tuner_up.alt[1].a.kind == IMAP_A_MOUSEBTN &&
          p.tuner_up.alt[1].a.code == 5, "wheel: MOUSE:WHEEL_UP = Maus-Code 5");
    CHECK(p.tuner_down.n == 2 && p.tuner_down.alt[1].a.code == 6, "wheel: klein geschrieben, WHEEL_DOWN = 6");
    CHECK(p.gear_down.n == 1 && p.gear_down.alt[0].a.code == 7 &&
          p.gear_up.n == 1 && p.gear_up.alt[0].a.code == 8, "wheel: WHEEL_LEFT/RIGHT = 7/8");
    CHECK(p.btn[20].n == 0 && p.btn[21].n == 0, "wheel: MOUSE:WHEEL / WHEEL_UPX verworfen");

    /* Pulsmaschine: eine Raste = 2 Frames gedrueckt, 2 Frames los. */
    memset(&w, 0, sizeof w);
    imap_wheel_add(&w, 0, 120);
    for (i = 0; i < 6; i++) seq |= (imap_wheel_step(&w, f + i, 0) & 1u) << i;
    CHECK(seq == 0x03u, "wheel: 1 Raste -> Impuls 2 Frames (Muster %02X)", seq);
    /* Mehrere Rasten -> mehrere Impulse, kein Dauerdruck. */
    f += 10; seq = 0;
    imap_wheel_add(&w, 0, 360);
    for (i = 0; i < 14; i++) seq |= (imap_wheel_step(&w, f + i, 0) & 1u) << i;
    CHECK(seq == 0x333u, "wheel: 3 Rasten -> 3 Impulse (Muster %04X)", seq);
    /* Feines Rad: Teil-Deltas summiert; Richtungswechsel verwirft Rest. */
    f += 20;
    for (i = 0; i < 3; i++) imap_wheel_add(&w, 0, 40);
    CHECK(w.pend[IMAP_WHEEL_UP] == 1 && w.acc[0] == 0, "wheel: 3x40 = 1 Raste");
    imap_wheel_add(&w, 0, 100);
    imap_wheel_add(&w, 0, -30);
    CHECK(w.acc[0] == -30 && w.pend[IMAP_WHEEL_DOWN] == 0, "wheel: Richtungswechsel verwirft Rest (%ld)", w.acc[0]);
    imap_wheel_clear(&w);
    /* Puffergrenze. */
    imap_wheel_add(&w, 0, 120 * 20);
    CHECK(w.pend[IMAP_WHEEL_UP] == IMAP_WHEEL_MAXQ, "wheel: hoechstens %d Rasten im Puffer", IMAP_WHEEL_MAXQ);
    /* Gesperrt: verwerfen, kein spaeteres Nachfeuern. */
    m = imap_wheel_step(&w, f, 1);
    CHECK(m == 0 && w.pend[IMAP_WHEEL_UP] == 0 && imap_wheel_step(&w, f + 1, 0) == 0,
          "wheel: gesperrt verwirft Rasten");
    /* Richtungen getrennt (hoch/runter, links/rechts). */
    f += 10;
    imap_wheel_add(&w, 0, -120);
    imap_wheel_add(&w, 1, 120);
    m = imap_wheel_step(&w, f, 0);
    CHECK(m == ((1u << IMAP_WHEEL_DOWN) | (1u << IMAP_WHEEL_RIGHT)), "wheel: runter + rechts getrennt (%X)", m);
    imap_wheel_clear(&w);
    memset(&w, 0, sizeof w);

    /* Profil: Raste -> genau eine Tuner-Stufe je Raste (Flanke). */
    imap_state_init(&st);
    imap_wheel_add(&w, 0, 120 * 3);
    f += 10;
    for (i = 0; i < 16; i++) {
        wheel_snap(&sn, f + i, imap_wheel_step(&w, f + i, 0));
        sn.real_vk = wheel_real_vk;
        sn.wheel_step = imap_wheel_steps(&w); /* W384: Zaehl-Spur */
        imap_profile_apply(&b, &st, &p, &sn);
    }
    CHECK(b.tuner == 3, "wheel: 3 Rasten hoch -> Tuner 3 (ist %u)", b.tuner);
    imap_wheel_add(&w, 0, -120);
    f += 20;
    for (i = 0; i < 6; i++) {
        wheel_snap(&sn, f + i, imap_wheel_step(&w, f + i, 0));
        sn.real_vk = wheel_real_vk;
        sn.wheel_step = imap_wheel_steps(&w);
        imap_profile_apply(&b, &st, &p, &sn);
    }
    CHECK(b.tuner == 2, "wheel: 1 Raste runter -> Tuner 2 (ist %u)", b.tuner);
    imap_wheel_add(&w, 1, 120);
    f += 20;
    for (i = 0; i < 6; i++) {
        wheel_snap(&sn, f + i, imap_wheel_step(&w, f + i, 0));
        sn.real_vk = wheel_real_vk;
        sn.wheel_step = imap_wheel_steps(&w);
        imap_profile_apply(&b, &st, &p, &sn);
    }
    CHECK(b.gear == 9 && b.tuner == 2, "wheel: Kipprad rechts -> Gang N->1 (gear=%u)", b.gear);
    /* Gatter: gesperrt / ohne mouse_buttons traegt das Rad nichts bei. */
    imap_state_init(&st);
    wheel_snap(&sn, f + 40, 1u << IMAP_WHEEL_UP);
    sn.real_blocked = 1;
    sn.wheel_step = 1u << IMAP_WHEEL_UP; /* W384 */
    imap_profile_apply(&b, &st, &p, &sn);
    CHECK(b.tuner == 0, "wheel: real_blocked -> kein Tuner (ist %u)", b.tuner);
    imap_state_init(&st);
    wheel_snap(&sn, f + 41, 1u << IMAP_WHEEL_UP);
    sn.mouse_buttons = 0;
    sn.wheel_step = 1u << IMAP_WHEEL_UP; /* W384 */
    imap_profile_apply(&b, &st, &p, &sn);
    CHECK(b.tuner == 0, "wheel: mouse_buttons=0 -> kein Tuner (ist %u)", b.tuner);
    /* Replay: synthetische VKs 329..332 (wie 324..328 fuer die Tasten). */
    {
        int kr[] = { 329 };
        cfgvar_t c = { 1, 0, 0, 1, 4, 0, 0 };
        imap_state_init(&st);
        make_snap(&sn, f + 50, &c);
        set_keys(kr, 1);
        imap_profile_apply(&b, &st, &p, &sn);
        set_keys(NULL, 0);
        CHECK(b.tuner == 1, "wheel: Replay-VK 329 -> WHEEL_UP (Tuner %u)", b.tuner);
    }
    /* Standardprofile: Rad nicht belegt -> Rad-Maske aendert nichts. */
    for (i = 0; i < 2; i++) {
        const imap_profile_t *dp = i ? xbox : kb;
        imap_block_t b0, b1;
        imap_state_t s0, s1;
        memset(&b0, 0, sizeof b0);
        memset(&b1, 0, sizeof b1);
        imap_state_init(&s0);
        imap_state_init(&s1);
        wheel_snap(&sn, f + 60, 0);
        sn.real_vk = wheel_real_vk;
        imap_profile_apply(&b0, &s0, dp, &sn);
        wheel_snap(&sn, f + 60, 0xFu);
        sn.real_vk = wheel_real_vk;
        sn.wheel_step = 0xFu; /* W384 */
        imap_profile_apply(&b1, &s1, dp, &sn);
        ok = memcmp(&b0, &b1, sizeof b0) == 0;
        CHECK(ok, "wheel: %s ohne Rad-Belegung (Block gleich)", i ? "xbox_pad_default" : "keyboard_default");
    }

    /* W384 (GH #63): Raste -> Tuner-Schritt im selben Frame wie die Raste ankommt
     * (kein Warten auf 2+2-Impulse); schnelles Drehen: 1 Schritt je Frame, ohne
     * Stufen zu ueberspringen; zweiter Poll im selben Frame zaehlt nicht doppelt. */
    {
        unsigned seqt[8];
        int bad = 0;
        imap_state_init(&st);
        memset(&w, 0, sizeof w);
        f += 100;
        imap_wheel_add(&w, 0, 120);
        wheel_snap(&sn, f, imap_wheel_step(&w, f, 0));
        sn.real_vk = wheel_real_vk;
        sn.wheel_step = imap_wheel_steps(&w);
        imap_profile_apply(&b, &st, &p, &sn);
        CHECK(b.tuner == 1, "W384 wheel: 1 Raste -> Tuner 1 im naechsten Poll (ist %u)", b.tuner);
        wheel_snap(&sn, f, imap_wheel_step(&w, f, 0)); /* zweiter Poll, gleicher Frame */
        sn.real_vk = wheel_real_vk;
        sn.wheel_step = imap_wheel_steps(&w);
        imap_profile_apply(&b, &st, &p, &sn);
        CHECK(b.tuner == 1 && sn.wheel_step == 0, "W384 wheel: 2. Poll im selben Frame zaehlt nicht (Tuner %u)", b.tuner);
        imap_state_init(&st);
        memset(&w, 0, sizeof w);
        f += 10;
        imap_wheel_add(&w, 0, 120 * 5); /* schnelles Drehen: 5 Rasten auf einmal */
        for (i = 0; i < 8; i++) {
            wheel_snap(&sn, f + i, imap_wheel_step(&w, f + i, 0));
            sn.real_vk = wheel_real_vk;
            sn.wheel_step = imap_wheel_steps(&w);
            imap_profile_apply(&b, &st, &p, &sn);
            seqt[i] = b.tuner;
        }
        for (i = 0; i < 8; i++) bad |= seqt[i] != (unsigned)(i < 5 ? i + 1 : 5);
        CHECK(!bad, "W384 wheel: 5 Rasten -> Tuner 1,2,3,4,5 in 5 Frames (ist %u,%u,%u,%u,%u,%u)",
              seqt[0], seqt[1], seqt[2], seqt[3], seqt[4], seqt[5]);
        imap_wheel_add(&w, 0, -120 * 2);
        for (i = 0; i < 3; i++) {
            wheel_snap(&sn, f + 20 + i, imap_wheel_step(&w, f + 20 + i, 0));
            sn.real_vk = wheel_real_vk;
            sn.wheel_step = imap_wheel_steps(&w);
            imap_profile_apply(&b, &st, &p, &sn);
            seqt[i] = b.tuner;
        }
        CHECK(seqt[0] == 4 && seqt[1] == 3 && seqt[2] == 3, "W384 wheel: 2 Rasten runter -> 4,3 (ist %u,%u,%u)",
              seqt[0], seqt[1], seqt[2]);
        imap_wheel_add(&w, 1, 120); /* Kipprad -> Gang im selben Frame */
        wheel_snap(&sn, f + 30, imap_wheel_step(&w, f + 30, 0));
        sn.real_vk = wheel_real_vk;
        sn.wheel_step = imap_wheel_steps(&w);
        imap_profile_apply(&b, &st, &p, &sn);
        CHECK(b.gear == 9, "W384 wheel: Kipprad -> Gang N->1 sofort (gear=%u)", b.gear);
        imap_wheel_add(&w, 0, 120 * 3); /* gesperrt verwirft auch die Zaehl-Spur */
        (void)imap_wheel_step(&w, f + 31, 1);
        CHECK(imap_wheel_steps(&w) == 0 && w.cnt[IMAP_WHEEL_UP] == 0 &&
              (imap_wheel_step(&w, f + 32, 0), imap_wheel_steps(&w)) == 0,
              "W384 wheel: gesperrt verwirft Zaehl-Spur");
    }
}

/* ---- Teil 19: W271 STICK-RELATIVE - Stick "wie eine Maus" auf dem Zielhebel ---- */
typedef struct {
    int stick_aim, accel, mouse_aim, pad_present;
    float rate;
} sr_cfg_t;

static void sr_poll(imap_block_t *b, imap_state_t *st, const imap_profile_t *p,
                    unsigned frame, int lx, int ly, const sr_cfg_t *c)
{
    imap_snap_t s;
    static const cfgvar_t cv = {1, 1, 0, 1, 4, 0, 0};
    make_snap(&s, frame, &cv);
    s.mouse_aim = c->mouse_aim;
    s.pad_axis_arb = 1;
    s.pad_arb_dead = 8;
    s.pad_mouse_wake = 3;
    s.pad_present = c->pad_present;
    s.pad.lx = (int16_t)lx;
    s.pad.ly = (int16_t)ly;
    s.stick_aim = c->stick_aim;
    s.stick_aim_rate = c->rate;
    s.stick_aim_accel = c->accel;
    imap_profile_apply(b, st, p, &s);
}

static const char kSrProf[] =
    "aim_x = MOUSE:X:mode=aim, PAD:LX:dz=0.15:curve=1.5\n"
    "aim_y = MOUSE:Y:mode=aim, PAD:LY:dz=0.15:curve=1.5:inv=1\n"
    "aim_x.neg = KEY:LEFT:cond=!SHIFT\n"
    "aim_x.pos = KEY:RIGHT:cond=!SHIFT\n"
    "rot = PAD:RX:dz=0.12:curve=1.3:mode=rate\n" /* auf rot ignoriert */
    "[client]\n"
    "aim_center = KEY:B, PAD:BACK+PAD:RT\n";

static void run_stickrel_cases(const imap_profile_t *xbox)
{
    imap_profile_t p, q;
    imap_state_t st, st2;
    imap_block_t b, b2;
    sr_cfg_t c = {1, 0, 1, 1, 4.0f * IMAP_STICK_RATE_PER_SENS};
    unsigned f = 70000;
    int i, ok;
    float exp;
    printf("Teil 19: W271 STICK-RELATIVE\n");

    CHECK(imap_profile_parse(kSrProf, "sr", &p) == 0, "sr: parse");
    CHECK(p.axis[IMAP_AX_AIM_X].pad[0].amode == IMAP_AMODE_GLOBAL &&
              p.axis[IMAP_AX_ROT].pad[0].amode == IMAP_AMODE_GLOBAL &&
              p.axis[IMAP_AX_ROT].pad[0].src == 2,
          "sr: mode=rate auf rot ignoriert (Quelle bleibt)");
    CHECK(p.aim_center.n == 2, "sr: aim_center 2 Quellen (%d)", p.aim_center.n);
    CHECK(xbox->aim_center.n == 0, "sr: Standardprofil ohne aim_center");

    /* 1. Auslenkung -> Bewegung: LX voll rechts 10 Frames, Rate 1,667/Frame. */
    imap_state_init(&st);
    for (i = 0; i < 10; i++) sr_poll(&b, &st, &p, f++, 32767, 0, &c);
    exp = 127.0f + 10.0f * c.rate;
    CHECK(fabsf((float)st.m_ax + st.m_fx - exp) < 0.01f && b.aiming_x == 0x90,
          "sr: 10 Frames voll rechts -> Hebel %.2f (soll %.2f), aim_x 0x%02X (soll 0x90)",
          (float)st.m_ax + st.m_fx, exp, b.aiming_x);
    CHECK(st.m_rate_src == 3 && (st.m_aim_src & 1), "sr: m_rate_src=3 (%u), X vom Hebel (%u)",
          st.m_rate_src, st.m_aim_src);
    /* gleicher Frame zweimal gepollt -> keine Doppel-Integration */
    sr_poll(&b, &st, &p, f - 1, 32767, 0, &c);
    CHECK(fabsf((float)st.m_ax + st.m_fx - exp) < 0.01f, "sr: gleicher Frame integriert nicht doppelt");

    /* 2. Loslassen haelt die Stellung. */
    for (i = 0; i < 20; i++) sr_poll(&b, &st, &p, f++, 0, 0, &c);
    CHECK(b.aiming_x == 0x90 && b.aiming_y == 0x7F, "sr: Loslassen haelt (aim=%02X%02X)",
          b.aiming_x, b.aiming_y);

    /* 3. Halbe Auslenkung: (0.5-0.15)/0.85 ^1.5 = 0.2643 * 1.667 = 0.4405/Frame */
    {
        float v0 = (float)st.m_ax + st.m_fx, m = powf((0.5f - 0.15f) / 0.85f, 1.5f);
        for (i = 0; i < 10; i++) sr_poll(&b, &st, &p, f++, 16384, 0, &c);
        CHECK(fabsf((float)st.m_ax + st.m_fx - (v0 + 10.0f * m * c.rate)) < 0.05f,
              "sr: halbe Auslenkung -> Geschwindigkeit nach Kurve (%.2f soll %.2f)",
              (float)st.m_ax + st.m_fx, v0 + 10.0f * m * c.rate);
    }

    /* 4. Klemmung an beiden Enden. */
    for (i = 0; i < 200; i++) sr_poll(&b, &st, &p, f++, 32767, 0, &c);
    CHECK(st.m_ax == 255 && b.aiming_x == 0xFF, "sr: Klemmung rechts (m_ax %d, aim 0x%02X)",
          st.m_ax, b.aiming_x);
    for (i = 0; i < 20; i++) sr_poll(&b, &st, &p, f++, -32768, 0, &c);
    CHECK(b.aiming_x < 0xFF && b.aiming_x > 0xD0, "sr: sofort zurueck nach Klemmung (0x%02X)", b.aiming_x);
    for (i = 0; i < 300; i++) sr_poll(&b, &st, &p, f++, -32768, 0, &c);
    CHECK(st.m_ax == 0 && b.aiming_x == 0x00, "sr: Klemmung links (m_ax %d)", st.m_ax);

    /* 5. Totzone: LX 2000 (6 %) < dz 0,15 -> keine Bewegung. */
    imap_state_init(&st);
    for (i = 0; i < 30; i++) sr_poll(&b, &st, &p, f++, 2000, -2500, &c);
    CHECK(st.m_ax == 0x7F && st.m_ay == 0x7F && st.m_fx == 0.0f && b.aiming_x == 0x7F,
          "sr: Totzone haelt still (m %d/%d)", st.m_ax, st.m_ay);

    /* 6. Invertierung wie Position: LY hoch (inv=1) -> aim_y sinkt (Position: 0x00). */
    for (i = 0; i < 10; i++) sr_poll(&b, &st, &p, f++, 0, 32767, &c);
    CHECK(b.aiming_y == 0x6E,
          "sr: LY hoch -> aim_y sinkt (0x%02X, soll 0x6E)", b.aiming_y);

    /* 7. Zentrieren-Taste (Flanke): B -> Mitte; gehalten zentriert nicht erneut. */
    g_keys['B'] = 1;
    sr_poll(&b, &st, &p, f++, 0, 0, &c);
    CHECK(b.aiming_x == 0x7F && b.aiming_y == 0x7F && st.aim_centered == 1 &&
              st.m_fx == 0.0f && st.m_fy == 0.0f,
          "sr: aim_center -> Mitte (aim=%02X%02X)", b.aiming_x, b.aiming_y);
    for (i = 0; i < 5; i++) sr_poll(&b, &st, &p, f++, 32767, 0, &c);
    CHECK(b.aiming_x > 0x7F && st.aim_centered == 0, "sr: gehaltene Taste zentriert nicht erneut");
    g_keys['B'] = 0;
    sr_poll(&b, &st, &p, f++, 0, 0, &c);
    {
        unsigned char held = b.aiming_x;
        g_keys['B'] = 1;
        sr_poll(&b, &st, &p, f++, 0, 0, &c);
        g_keys['B'] = 0;
        CHECK(held > 0x7F && b.aiming_x == 0x7F, "sr: zweite Flanke zentriert erneut");
    }

    /* 8. Maus und Stick addieren im selben Frame (gleicher virtueller Hebel). */
    imap_state_init(&st);
    g_keys[320] = 1; /* synthetische Maus +1 Count -> +4 Stufen (Replay-Pfad) */
    sr_poll(&b, &st, &p, f++, 32767, 0, &c);
    g_keys[320] = 0;
    exp = 127.0f + 4.0f + c.rate;
    CHECK(fabsf((float)st.m_ax + st.m_fx - exp) < 0.01f,
          "sr: Maus + Stick addieren (%.2f soll %.2f)", (float)st.m_ax + st.m_fx, exp);
    /* Maus allein danach bewegt weiter vom gehaltenen Punkt (kein Sprung). */
    g_keys[321] = 1;
    sr_poll(&b, &st, &p, f++, 0, 0, &c);
    g_keys[321] = 0;
    CHECK(fabsf((float)st.m_ax + st.m_fx - (exp - 4.0f)) < 0.01f && b.aiming_x == st.m_ax,
          "sr: Maus links danach ohne Sprung (%.2f)", (float)st.m_ax + st.m_fx);

    /* 9. Digital (Pfeil) gewinnt, Loslassen -> zurueck zur gehaltenen Stellung. */
    imap_state_init(&st);
    for (i = 0; i < 6; i++) sr_poll(&b, &st, &p, f++, 32767, 0, &c);
    ok = b.aiming_x;
    g_keys[VK_LEFT] = 1;
    sr_poll(&b, &st, &p, f++, 0, 0, &c);
    g_keys[VK_LEFT] = 0;
    CHECK(b.aiming_x == 0x00, "sr: Pfeil links gewinnt (0x%02X)", b.aiming_x);
    sr_poll(&b, &st, &p, f++, 0, 0, &c);
    CHECK(b.aiming_x == ok, "sr: nach Pfeil zurueck auf 0x%02X (ist 0x%02X)", ok, b.aiming_x);

    /* 10. Ohne Maus-Kandidat (mouse_aim=0): virtueller Hebel bleibt Quelle. */
    {
        sr_cfg_t c2 = c;
        c2.mouse_aim = 0;
        imap_state_init(&st);
        for (i = 0; i < 10; i++) sr_poll(&b, &st, &p, f++, 32767, 0, &c2);
        for (i = 0; i < 5; i++) sr_poll(&b, &st, &p, f++, 0, 0, &c2);
        CHECK(b.aiming_x == 0x90 && (st.m_aim_src & 1), "sr: ohne Maus haelt (0x%02X)", b.aiming_x);
    }

    /* 11. Kein Pad -> nichts, m_rate_src = 0. */
    {
        sr_cfg_t c3 = c;
        c3.pad_present = 0;
        imap_state_init(&st);
        for (i = 0; i < 5; i++) sr_poll(&b, &st, &p, f++, 32767, 0, &c3);
        CHECK(st.m_rate_src == 0 && b.aiming_x == 0x7F, "sr: ohne Pad keine Wirkung");
    }

    /* 12. Beschleunigung: 40 Frames Vollausschlag, accel 2 > accel 0. */
    {
        sr_cfg_t ca = c;
        float d0, d2;
        ca.accel = 2;
        imap_state_init(&st);
        imap_state_init(&st2);
        for (i = 0; i < 40; i++, f++) {
            sr_poll(&b, &st, &p, f, -32768, 0, &c);
            sr_poll(&b2, &st2, &p, f, -32768, 0, &ca);
        }
        d0 = 127.0f - ((float)st.m_ax + st.m_fx);
        d2 = 127.0f - ((float)st2.m_ax + st2.m_fx);
        /* accel 0: 40*1,667 = 66,7; accel 2: Rampe ab Frame 7 bis 2x ab Frame 21 */
        CHECK(fabsf(d0 - 40.0f * c.rate) < 0.05f && d2 > d0 * 1.5f && d2 < d0 * 2.0f,
              "sr: Beschleunigung (accel0 %.1f, accel2 %.1f)", d0, d2);
    }

    /* 13. Standard stick_aim=0: Position wie bisher (bitgleich mit leeren W271-Feldern). */
    {
        sr_cfg_t c0 = {0, 0, 1, 1, 0.0f}, cz = {0, 3, 1, 1, 6.0f};
        int same = 1;
        unsigned g = 90000;
        imap_state_init(&st);
        imap_state_init(&st2);
        for (i = 0; i < 400; i++, g++) {
            int lx = (int)((i * 7919) % 65535) - 32768, ly = (int)((i * 104729) % 65535) - 32768;
            if (i % 50 > 40) lx = ly = 0;
            g_keys[320 + (i % 4)] = (i % 3) == 0;
            sr_poll(&b, &st, xbox, g, lx, ly, &c0);
            sr_poll(&b2, &st2, xbox, g, lx, ly, &cz);
            if (memcmp(&b, &b2, sizeof b) != 0 || st.m_ax != st2.m_ax || st.m_ay != st2.m_ay) same = 0;
        }
        memset(g_keys, 0, sizeof g_keys);
        CHECK(same && st.m_rate_src == 0, "sr: stick_aim=0 bitgleich (Rate/accel ohne Wirkung)");
        /* Position: LX voll -> 0xFF, losgelassen -> 0x7F (federt zurueck). */
        imap_state_init(&st);
        sr_poll(&b, &st, xbox, g++, 0, 0, &c0); /* W132: Ruhe zuerst */
        sr_poll(&b, &st, xbox, g++, 32767, 0, &c0);
        sr_poll(&b2, &st, xbox, g++, 0, 0, &c0);
        CHECK(b.aiming_x == 0xFF && b2.aiming_x == 0x7F, "sr: Position federt zurueck (0x%02X/0x%02X)",
              b.aiming_x, b2.aiming_x);
    }

    /* 14. Je Quelle ueberschreiben: global Position, LX:mode=rate -> X relativ, Y Position. */
    CHECK(imap_profile_parse("aim_x = PAD:LX:dz=0.15:mode=rate\n"
                             "aim_y = PAD:LY:dz=0.15:inv=1\n", "q", &q) == 0 &&
              q.axis[IMAP_AX_AIM_X].pad[0].amode == IMAP_AMODE_RATE,
          "sr: parse mode=rate");
    {
        sr_cfg_t c0 = c;
        c0.stick_aim = 0;
        imap_state_init(&st);
        for (i = 0; i < 10; i++) sr_poll(&b, &st, &q, f++, 32767, 32767, &c0);
        CHECK(b.aiming_y == 0x00, "sr: Y Position voll (0x%02X)", b.aiming_y);
        sr_poll(&b, &st, &q, f++, 0, 0, &c0);
        CHECK(b.aiming_x > 0x7F + 15 && b.aiming_y == 0x7F && st.m_rate_src == 1,
              "sr: mode=rate haelt X, Y federt (aim=%02X%02X)", b.aiming_x, b.aiming_y);
    }
    /* global relativ, JOY-Hebel :mode=position bleibt Position. */
    CHECK(imap_profile_parse("aim_x = JOY0:X:mode=position, PAD:LX:dz=0.15\n", "q2", &q) == 0 &&
              q.axis[IMAP_AX_AIM_X].pad[0].amode == IMAP_AMODE_POSITION &&
              q.axis[IMAP_AX_AIM_X].pad[1].amode == IMAP_AMODE_GLOBAL,
          "sr: parse JOY mode=position");
    {
        imap_snap_t s;
        static const cfgvar_t cv = {1, 0, 0, 1, 4, 0, 0};
        imap_state_init(&st);
        for (i = 0; i < 3; i++) {
            make_snap(&s, f++, &cv);
            s.stick_aim = 1;
            s.pad_present = 1;
            s.joy_mask = 1;
            s.joy[0].axis[0] = joy_pos(0.5f);
            imap_profile_apply(&b, &st, &q, &s);
        }
        CHECK(b.aiming_x == 0x7F + 64, "sr: JOY mode=position = Position (0x%02X)", b.aiming_x);
        make_snap(&s, f++, &cv);
        s.stick_aim = 1;
        s.pad_present = 1;
        s.joy_mask = 1;
        imap_profile_apply(&b, &st, &q, &s);
        CHECK(b.aiming_x == 0x7F, "sr: JOY-Hebel losgelassen -> Mitte (0x%02X)", b.aiming_x);
        /* JOY mit dz 0: Mindest-Totzone 0,05 gegen Drift bei mode=rate */
        CHECK(imap_profile_parse("aim_x = JOY0:X:mode=rate\n", "q3", &q) == 0, "sr: parse JOY rate");
        imap_state_init(&st);
        for (i = 0; i < 30; i++) {
            make_snap(&s, f++, &cv);
            s.joy_mask = 1;
            s.joy[0].axis[0] = joy_pos(0.03f);
            imap_profile_apply(&b, &st, &q, &s);
        }
        CHECK(st.m_ax == 0x7F && st.m_fx == 0.0f && st.m_rate_src == 1,
              "sr: Drift 3 %% unter Mindest-Totzone (m_ax %d)", st.m_ax);
    }
    /* Profilvergleich erkennt den Modus; unbekannter Modus verwirft die Alternative. */
    {
        imap_profile_t a1, a2;
        imap_profile_parse("aim_x = PAD:LX:mode=rate\n", "a1", &a1);
        imap_profile_parse("aim_x = PAD:LX\n", "a2", &a2);
        CHECK(imap_profile_equal(&a1, &a2) != 0, "sr: equal unterscheidet mode");
        imap_profile_parse("aim_x = PAD:LX:mode=quatsch\n", "a3", &a1);
        CHECK(a1.axis[IMAP_AX_AIM_X].pad[0].src < 0, "sr: mode=quatsch verworfen");
        imap_profile_parse("pedal_r = PAD:RT:mode=rate\n", "a4", &a1);
        CHECK(a1.axis[IMAP_AX_PEDAL_R].pad[0].src == 5 &&
                  a1.axis[IMAP_AX_PEDAL_R].pad[0].amode == IMAP_AMODE_GLOBAL,
              "sr: mode auf Pedal ignoriert");
    }
    /* Pad-Akkord BACK+RT als aim_center (Pad-Quelle). */
    {
        imap_snap_t s;
        static const cfgvar_t cv = {1, 1, 0, 1, 4, 0, 0};
        imap_state_init(&st);
        st.m_ax = 0xA0;
        make_snap(&s, f++, &cv);
        s.pad_present = 1;
        s.pad.buttons = IMAP_PAD_BACK;
        s.pad.rt = 255;
        imap_profile_apply(&b, &st, &p, &s);
        CHECK(st.aim_centered == 1 && b.aiming_x == 0x7F, "sr: aim_center per BACK+RT");
    }
}

/* ---- Teil 22: W305 JOY-HOTPLUG - Joystick abziehen/wieder einstecken ----
 * Fake-DirectInput (ijoy_test_*): abgezogen -> GetDeviceState scheitert,
 * wieder eingesteckt -> gleiche guidInstance, altes Objekt bleibt tot. Bis
 * W304 behielt ijoy_scan das tote Objekt (gleiche GUID) - Slot blieb bis zum
 * Neustart ohne Geraet. Belegt: neutral sofort, Freigabe, neu aufzaehlen,
 * wieder lesbar, gedrosselte Suche, gesunde Geraete nie neu geoeffnet. */
static void run_joyhotplug_cases(void)
{
    char specs[IMAP_JOY_MAX][96];
    imap_joy_state_t js[IMAP_JOY_MAX];
    ijoy_test_stats_t t0, ts;
    char nm[96];
    unsigned m;
    int i, miss;
    printf("Teil 22: W305 Joystick-Hotplug (Fake-DirectInput)\n");
    memset(specs, 0, sizeof specs);
    ijoy_test_begin(1, (const char (*)[96])specs, 1u);
    ijoy_test_run(100);
    m = ijoy_slots_get(js);
    ijoy_slot_name(0, nm, sizeof nm);
    ijoy_test_stats(&t0);
    CHECK(m == 1u && js[0].axis[0] == 1000 && (js[0].btn[0] & 1u) &&
              strstr(nm, "Test Stick 0") != NULL && t0.scans == 1 && t0.opens == 1,
          "w305 joy: Start verbunden (m=%u ax=%d scans=%u opens=%u)", m, js[0].axis[0],
          t0.scans, t0.opens);
    /* Abziehen: im naechsten Takt neutral (kein eingefrorener Stand). */
    ijoy_test_plug(0, 0);
    ijoy_test_run(8);
    m = ijoy_slots_get(js);
    CHECK(m == 0u && js[0].axis[0] == 0 && js[0].axis[1] == 0 && js[0].btn[0] == 0 &&
              js[0].pov[0] == -1,
          "w305 joy: abgezogen -> sofort neutral (m=%u ax=%d)", m, js[0].axis[0]);
    ijoy_test_run(5000);
    ijoy_test_stats(&ts);
    m = ijoy_slots_get(js);
    CHECK(ts.releases == 1 && ts.bad_release == 0 && ts.bad_read == 0,
          "w305 joy: totes Objekt genau einmal freigegeben (rel=%u bad=%u/%u)",
          ts.releases, ts.bad_release, ts.bad_read);
    CHECK(ts.scans - t0.scans >= 3 && ts.scans - t0.scans <= 6 && m == 0u,
          "w305 joy: Suche gedrosselt, kein Busy-Spin (5 s -> %u Aufzaehlungen)",
          ts.scans - t0.scans);
    CHECK(ts.read_fails <= 30,
          "w305 joy: kein Dauerlesen am toten Objekt (%u Fehl-Lesungen)", ts.read_fails);
    /* Wieder einstecken (gleiche guidInstance, neue Generation). */
    ijoy_test_axis(0, -2000);
    ijoy_test_plug(0, 1);
    ijoy_test_run(3100);
    m = ijoy_slots_get(js);
    ijoy_test_stats(&ts);
    CHECK(m == 1u && js[0].axis[0] == -2000 && ts.opens == 2 && ts.bad_read == 0,
          "w305 joy: wieder eingesteckt -> neu geoeffnet + lesbar (m=%u ax=%d opens=%u)",
          m, js[0].axis[0], ts.opens);
    /* Schnelles Umstecken (vor jeder Aufzaehlung): alter Code behielt das
     * tote Objekt wegen gleicher GUID -> Endlosschleife. */
    ijoy_test_plug(0, 0);
    ijoy_test_run(40);
    ijoy_test_plug(0, 1);
    ijoy_test_run(1500);
    m = ijoy_slots_get(js);
    ijoy_test_stats(&ts);
    CHECK(m == 1u && ts.opens == 3 && ts.releases == 2 && ts.bad_release == 0,
          "w305 joy: schnell umgesteckt -> tot erkannt, neu geoeffnet (m=%u opens=%u rel=%u)",
          m, ts.opens, ts.releases);
    /* Gesundes Geraet 10 s: nie neu geoeffnet, keine Aufzaehlung. */
    t0 = ts;
    ijoy_test_run(10000);
    ijoy_test_stats(&ts);
    CHECK(ts.opens == t0.opens && ts.scans == t0.scans && ts.releases == t0.releases &&
              ijoy_slots_get(js) == 1u,
          "w305 joy: gesundes Geraet ohne Aussetzer (opens %u->%u, scans %u->%u)",
          t0.opens, ts.opens, t0.scans, ts.scans);
    ijoy_test_end();

    /* Zwei Geraete: Slot 1 abziehen, Slot 0 laeuft ohne Aussetzer weiter. */
    memset(specs, 0, sizeof specs);
    strcpy_s(specs[1], sizeof specs[1], "Test Stick 1");
    ijoy_test_begin(2, (const char (*)[96])specs, 3u);
    ijoy_test_run(100);
    CHECK(ijoy_slots_get(js) == 3u && js[0].axis[0] == 1000 && js[1].axis[0] == 2000,
          "w305 joy: zwei Slots verbunden");
    ijoy_test_plug(1, 0);
    miss = 0;
    for (i = 0; i < 1000; i++) { /* 4 s im Takt pruefen */
        ijoy_test_run(4);
        m = ijoy_slots_get(js);
        if (!(m & 1u) || js[0].axis[0] != 1000) miss++;
    }
    ijoy_test_stats(&ts);
    CHECK(miss == 0 && !(m & 2u) && js[1].axis[0] == 0 && ts.opens == 2,
          "w305 joy: Slot 0 unberuehrt (Aussetzer %d), Slot 1 neutral, kein Neu-Oeffnen "
          "(opens=%u)", miss, ts.opens);
    ijoy_test_plug(1, 1);
    ijoy_test_run(3100);
    ijoy_test_stats(&ts);
    CHECK(ijoy_slots_get(js) == 3u && js[1].axis[0] == 2000 && ts.opens == 3,
          "w305 joy: Slot 1 zurueck, Spec-Zuordnung stimmt (opens=%u)", ts.opens);
    ijoy_slot_name(1, nm, sizeof nm);
    CHECK(strstr(nm, "Test Stick 1") != NULL, "w305 joy: Slot-Name nach Wiederkehr (%s)", nm);
    ijoy_test_end();
}

/* ---- Teil 27: W380 LOC-DODGE - Ausweich-Hilfe (slide_assist): kurzer Druck/Flanke ---- */
static void sa_poll(int path, const imap_profile_t *prof, imap_block_t *b, imap_state_t *st, unsigned *fr,
                    const cfgvar_t *c, const int *keys, int n, int assist)
{
    imap_snap_t sn;
    make_snap(&sn, (*fr)++, c);
    sn.slide_assist = assist;
    set_keys(keys, n);
    if (path) imap_profile_apply(b, st, prof, &sn);
    else imap_legacy_apply(b, st, &sn);
}

static void run_slide_assist_cases(const imap_profile_t *kb)
{
    imap_block_t b;
    imap_state_t st;
    cfgvar_t c = { 1, 1, 0, 1, 4, 0, 0 };
    unsigned fr = 120000;
    int kla[] = { VK_LCONTROL, 'A' }, kl[] = { VK_LCONTROL }, ka[] = { 'A' }, kd[] = { 'D' }, kx1[] = { 327 };
    int path, i, ok;
    printf("Teil 27: W380 Ausweich-Hilfe (slide_assist)\n");
    for (path = 0; path < 2; path++) {
        const char *pn = path ? "Profil" : "Legacy";
        /* Tipp 1 Poll Strg+A -> Pedal + links 8 Polls, danach los. */
        imap_state_init(&st);
        sa_poll(path, kb, &b, &st, &fr, &c, kla, 2, 8);
        ok = b.pedal_l == 0xFF && b.rot_lever == -127;
        for (i = 1; i < 8; i++) {
            sa_poll(path, kb, &b, &st, &fr, &c, NULL, 0, 8);
            ok = ok && b.pedal_l == 0xFF && b.rot_lever == -127;
        }
        CHECK(ok, "%s: Tipp Strg+A haelt Pedal+links 8 Polls", pn);
        sa_poll(path, kb, &b, &st, &fr, &c, NULL, 0, 8);
        CHECK(b.pedal_l == 0 && b.rot_lever == 0, "%s: nach 8 Polls los (ped=%02X rot=%d)", pn, b.pedal_l,
              (int)b.rot_lever);
        /* Richtung einen Poll spaeter (A nach Strg) -> links ab dem A-Poll. */
        imap_state_init(&st);
        sa_poll(path, kb, &b, &st, &fr, &c, kl, 1, 8);
        CHECK(b.pedal_l == 0xFF && b.rot_lever == 0, "%s: Strg allein -> Pedal, Hebel 0", pn);
        sa_poll(path, kb, &b, &st, &fr, &c, ka, 1, 8);
        sa_poll(path, kb, &b, &st, &fr, &c, NULL, 0, 8);
        CHECK(b.pedal_l == 0xFF && b.rot_lever == -127, "%s: A nach Strg -> links gehalten (rot=%d)", pn,
              (int)b.rot_lever);
        /* A vor dem Druck (Rueckblick 2 Polls) -> links; 4 Polls vorher -> geradeaus. */
        imap_state_init(&st);
        sa_poll(path, kb, &b, &st, &fr, &c, ka, 1, 8);
        sa_poll(path, kb, &b, &st, &fr, &c, kl, 1, 8);
        sa_poll(path, kb, &b, &st, &fr, &c, NULL, 0, 8);
        CHECK(b.pedal_l == 0xFF && b.rot_lever == -127, "%s: A kurz vor Strg -> links (rot=%d)", pn,
              (int)b.rot_lever);
        imap_state_init(&st);
        sa_poll(path, kb, &b, &st, &fr, &c, ka, 1, 8);
        for (i = 0; i < 4; i++) sa_poll(path, kb, &b, &st, &fr, &c, NULL, 0, 8);
        sa_poll(path, kb, &b, &st, &fr, &c, kl, 1, 8);
        sa_poll(path, kb, &b, &st, &fr, &c, NULL, 0, 8);
        CHECK(b.pedal_l == 0xFF && b.rot_lever == 0, "%s: A 5 Polls vorher -> geradeaus (rot=%d)", pn,
              (int)b.rot_lever);
        /* Gegenrichtung im Fenster gewinnt. */
        imap_state_init(&st);
        sa_poll(path, kb, &b, &st, &fr, &c, kla, 2, 8);
        sa_poll(path, kb, &b, &st, &fr, &c, kd, 1, 8);
        sa_poll(path, kb, &b, &st, &fr, &c, NULL, 0, 8);
        CHECK(b.pedal_l == 0xFF && b.rot_lever == 127, "%s: D im Fenster -> rechts (rot=%d)", pn,
              (int)b.rot_lever);
        /* Laenger gehalten = Original: Pedal solange gedrueckt, Hebel wieder echt nach dem Fenster. */
        imap_state_init(&st);
        sa_poll(path, kb, &b, &st, &fr, &c, kla, 2, 4);
        for (i = 0; i < 9; i++) sa_poll(path, kb, &b, &st, &fr, &c, kl, 1, 4);
        CHECK(b.pedal_l == 0xFF && b.rot_lever == 0, "%s: gehalten ueber das Fenster -> Pedal, Hebel echt", pn);
        sa_poll(path, kb, &b, &st, &fr, &c, NULL, 0, 4);
        CHECK(b.pedal_l == 0, "%s: losgelassen nach dem Fenster -> sofort los", pn);
        /* Schalter aus = bisheriges Verhalten (Tipp nur 1 Poll). */
        imap_state_init(&st);
        sa_poll(path, kb, &b, &st, &fr, &c, kla, 2, 0);
        sa_poll(path, kb, &b, &st, &fr, &c, NULL, 0, 0);
        CHECK(b.pedal_l == 0 && b.rot_lever == 0, "%s: slide_assist=0 -> Tipp 1 Poll (Original)", pn);
        /* Ohne Druck keine Wirkung (Hebel/Pedal unveraendert). */
        imap_state_init(&st);
        sa_poll(path, kb, &b, &st, &fr, &c, ka, 1, 8);
        CHECK(b.pedal_l == 0 && b.rot_lever == -127, "%s: A ohne Strg -> kein Pedal", pn);
        sa_poll(path, kb, &b, &st, &fr, &c, NULL, 0, 8);
        CHECK(b.pedal_l == 0 && b.rot_lever == 0, "%s: ohne Druck kein Halten", pn);
        /* Maus X1 (Synth 327) als Tipp. */
        imap_state_init(&st);
        sa_poll(path, kb, &b, &st, &fr, &c, kx1, 1, 8);
        sa_poll(path, kb, &b, &st, &fr, &c, NULL, 0, 8);
        CHECK(b.pedal_l == 0xFF, "%s: X1-Tipp haelt Pedal", pn);
    }
}

/* ---- Teil 26: W374 LOC-NET-OVERLAY - Aktion net_hud (Netz-Overlay, Standard F3, nur Tasten) ---- */
static void run_nethud_cases(const imap_profile_t *kb, const imap_profile_t *xbox)
{
    imap_profile_t p, q;
    int vks[4], mods[4], n;
    printf("Teil 26: W374 Aktion net_hud\n");
    CHECK(kb->net_hud_set == 1 && kb->net_hud.n == 1 && !kb->net_hud.alt[0].chord &&
          kb->net_hud.alt[0].a.kind == IMAP_A_KEY && kb->net_hud.alt[0].a.code == VK_F3,
          "net_hud: kb Standard KEY:F3");
    CHECK(xbox->net_hud_set == 1 && xbox->net_hud.n == 1 && xbox->net_hud.alt[0].a.code == VK_F3,
          "net_hud: xbox Standard KEY:F3");
    n = imap_bind_key_combos(&kb->net_hud, vks, mods, 4);
    CHECK(n == 1 && vks[0] == VK_F3 && mods[0] == 0, "net_hud: Fenster-Taste F3 ohne Zusatz");
    CHECK(imap_profile_parse("pause = KEY:PAUSE\n", "alt", &p) == 0 && p.net_hud_set == 0 && p.net_hud.n == 1 &&
          p.net_hud.alt[0].a.code == VK_F3, "net_hud: altes Profil ohne Zeile -> Standard F3");
    CHECK(imap_profile_parse("net_hud =\n", "t", &p) == 0 && p.net_hud_set == 1 && p.net_hud.n == 0,
          "net_hud: leere Zeile = aus");
    CHECK(imap_profile_parse("net_hud = KEY:F6\nnet_hud = KEY:N\n", "t", &p) == 0 && p.net_hud.n == 1 &&
          p.net_hud.alt[0].a.code == 'N', "net_hud: letzte Zeile gewinnt");
    CHECK(imap_profile_parse("net_hud = KEY:CTRL+KEY:F3\n", "t", &p) == 0 &&
          (n = imap_bind_key_combos(&p.net_hud, vks, mods, 4)) == 1 && vks[0] == VK_F3 && mods[0] == 2,
          "net_hud: Strg+F3 -> Fenster-Taste mit Zusatz");
    CHECK(imap_profile_parse("net_hud = KEY:F3\n", "t", &p) == 0 &&
          imap_profile_parse("net_hud = KEY:F4\n", "t", &q) == 0 && imap_profile_equal(&p, &q) != 0,
          "net_hud: Belegung geht in den Profilvergleich ein");
    CHECK(imap_profile_parse("net_hud = KEY:F3\n", "t", &p) == 0 &&
          imap_profile_parse("pause = KEY:PAUSE\n", "t", &q) == 0 && imap_profile_equal(&p, &q) != 0,
          "net_hud: Zeile gesetzt vs. Standard unterscheidbar (net_hud_set)");
}

/* ---- Teil 24: W312 JOY-CALIB - simulierte DirectInput-Objektlisten ----
 * Wingman Force 3D (Tester: Schub geht, Stick X/Y nicht), Hori PS4-Modus,
 * Standard-HOTAS. Format-Entscheidung (Standard vs. eigenes Format je
 * Objekt), Lesen ueber den Plan (Fake-DirectInput), Geraeteinfo ohne Werte,
 * Kalibrierung, Diagonale/Totzone im Client (je Achse). */
#define W312_ABS 0x02u
#define W312_BTN 0x04u
#define W312_POV 0x10u
#define W312_COLL 0x40u
#define W312_NODATA 0x80u
#define W312_FFA 0x01000000u
#define W312_INST(n) ((unsigned)(n) << 8)
#define W312_ASP 0x100u

static int w312_add(ijoy_obj_t *o, int n, int kind, unsigned type, unsigned flags,
                    unsigned short up, unsigned short u, const char *nm, long lo, long hi)
{
    memset(&o[n], 0, sizeof o[n]);
    o[n].kind = kind;
    o[n].type = type;
    o[n].flags = flags;
    o[n].upage = up;
    o[n].usage = u;
    strncpy_s(o[n].name, sizeof o[n].name, nm, _TRUNCATE);
    o[n].lo = lo;
    o[n].hi = hi;
    o[n].read = -1;
    return n + 1;
}

static int w312_buttons(ijoy_obj_t *o, int n, int count, int inst0)
{
    int b;
    for (b = 0; b < count; b++) {
        char nm[24];
        _snprintf_s(nm, sizeof nm, _TRUNCATE, "Button %d", b + 1);
        n = w312_add(o, n, IJOY_K_BTN, W312_BTN | W312_INST(inst0 + b), 0, 0x09,
                     (unsigned short)(b + 1), nm, 0, 0);
    }
    return n;
}

/* Wingman-artig: Stick als Simulation Aileron/Elevator (unbekannte GUID),
 * Schub als Simulation Throttle (GUID_Slider), Bereiche 0..1023 / 0..255
 * (DIPROP_RANGE abgelehnt), Hat, 9 Tasten, FF-Ausgabe-Kollektion. */
static int w312_wingman(ijoy_obj_t *o)
{
    int n = 0;
    n = w312_add(o, n, IJOY_K_AXIS, W312_ABS | W312_INST(0), 0, 0x02, 0xB0, "Aileron", 0, 1023);
    n = w312_add(o, n, IJOY_K_AXIS, W312_ABS | W312_INST(1), 0, 0x02, 0xB8, "Elevator", 0, 1023);
    n = w312_add(o, n, IJOY_K_SLIDER, W312_ABS | W312_INST(2), 0, 0x02, 0xBB, "Throttle", 0, 255);
    n = w312_add(o, n, IJOY_K_POV, W312_POV | W312_INST(0), 0, 0x01, 0x39, "Hat Switch", 0, 0);
    n = w312_buttons(o, n, 9, 0);
    n = w312_add(o, n, IJOY_K_OTHER, W312_COLL | W312_NODATA | W312_INST(0), 0, 0x0F, 0x21,
                 "Force feedback", 0, 0);
    return n;
}

static int w312_hotas(ijoy_obj_t *o) /* Standard-HOTAS-Stick */
{
    int n = 0;
    n = w312_add(o, n, IJOY_K_X, W312_ABS | W312_INST(0), W312_ASP, 0x01, 0x30, "X Axis", 0, 0);
    n = w312_add(o, n, IJOY_K_Y, W312_ABS | W312_INST(1), W312_ASP, 0x01, 0x31, "Y Axis", 0, 0);
    n = w312_add(o, n, IJOY_K_RZ, W312_ABS | W312_INST(5), W312_ASP, 0x01, 0x35, "Z Rotation", 0, 0);
    n = w312_add(o, n, IJOY_K_SLIDER, W312_ABS | W312_INST(6), W312_ASP, 0x01, 0x36, "Slider", 0, 0);
    n = w312_add(o, n, IJOY_K_POV, W312_POV | W312_INST(0), 0, 0x01, 0x39, "Hat Switch", 0, 0);
    return w312_buttons(o, n, 16, 0);
}

static int w312_hori(ijoy_obj_t *o) /* PS4-Modus (DS4-Deskriptor) */
{
    int n = 0;
    n = w312_add(o, n, IJOY_K_X, W312_ABS | W312_INST(0), W312_ASP, 0x01, 0x30, "X Axis", 0, 0);
    n = w312_add(o, n, IJOY_K_Y, W312_ABS | W312_INST(1), W312_ASP, 0x01, 0x31, "Y Axis", 0, 0);
    n = w312_add(o, n, IJOY_K_Z, W312_ABS | W312_INST(2), W312_ASP, 0x01, 0x32, "Z Axis", 0, 0);
    n = w312_add(o, n, IJOY_K_RZ, W312_ABS | W312_INST(5), W312_ASP, 0x01, 0x35, "Z Rotation", 0, 0);
    n = w312_add(o, n, IJOY_K_RX, W312_ABS | W312_INST(3), W312_ASP, 0x01, 0x33, "X Rotation", 0, 0);
    n = w312_add(o, n, IJOY_K_RY, W312_ABS | W312_INST(4), W312_ASP, 0x01, 0x34, "Y Rotation", 0, 0);
    n = w312_add(o, n, IJOY_K_POV, W312_POV | W312_INST(0), 0, 0x01, 0x39, "Hat Switch", 0, 0);
    return w312_buttons(o, n, 14, 0); /* B14 = Touchpad-Taste */
}

static int w312_count_read(const int *rd, int n)
{
    int i, c = 0;
    for (i = 0; i < n; i++) c += rd[i] >= 0;
    return c;
}

static void run_joycalib_cases(void)
{
    ijoy_obj_t o[IJOY_OBJ_MAX];
    int rd[IJOY_OBJ_MAX], rp[IJOY_OBJ_MAX], n, i;
    unsigned m, mp;
    char specs[IMAP_JOY_MAX][96];
    imap_joy_state_t js[IMAP_JOY_MAX];
    const ijoy_info_t *in;
    printf("Teil 24: W312 JOY-CALIB (simulierte Objektlisten, Kalibrierung)\n");
    memset(specs, 0, sizeof specs);

    /* A: Standard-HOTAS -> Standard-Format genuegt (bitgleich zu bisher). */
    n = w312_hotas(o);
    m = ijoy_std_capture(o, n, rd);
    mp = ijoy_plan_objects(o, n, rp);
    CHECK(m == 0x63u && rd[0] == 0 && rd[1] == 1 && rd[2] == 5 && rd[3] == 6 && rd[4] == 0 &&
              rd[5] == 0 && rd[20] == 15,
          "w312 hotas: Standard liest X Y RZ S0, POV1, B1..B16 (m=%02X)", m);
    CHECK(!ijoy_need_own_format(o, n) && mp == m && memcmp(rd, rp, sizeof(int) * (size_t)n) == 0,
          "w312 hotas: Plan == Standard, kein eigenes Format");

    /* B: Wingman-artig -> Standard liest NUR den Schub (Tester-Befund). */
    n = w312_wingman(o);
    m = ijoy_std_capture(o, n, rd);
    CHECK(m == 0x40u && rd[0] < 0 && rd[1] < 0 && rd[2] == 6 && rd[3] == 0 && rd[12] == 8 && rd[13] < 0,
          "w312 wingman: Standard-Format liest nur Schub S0 (m=%02X) - Stick X/Y fehlen", m);
    CHECK(ijoy_need_own_format(o, n), "w312 wingman: eigenes Format noetig");
    mp = ijoy_plan_objects(o, n, rp);
    CHECK(mp == 0x43u && rp[0] == 0 && rp[1] == 1 && rp[2] == 6 && rp[3] == 0 &&
              w312_count_read(rp, n) == 13,
          "w312 wingman: Plan liest Aileron->X, Elevator->Y, Schub->S0, Hat, 9 Tasten (m=%02X)", mp);
    {
        /* W282-Ausloeser (DIDEVCAPS.dwAxes > erfasste Achsen) greift nicht,
         * wenn der Treiber nur die erfassten Achsen zaehlt; W312 schon. */
        int ncaps_under = 1;
        CHECK(!(ncaps_under > 1) && ijoy_need_own_format(o, n),
              "w312 wingman: Treiber zaehlt 1 Achse -> W282 greift nicht, W312 schon");
        /* W282-Plan ohne Usage-Hinweis: Stick landete auf S1/Z (lernbar, aber falsch benannt). */
        {
            int kind[3] = {IJOY_K_AXIS, IJOY_K_AXIS, IJOY_K_SLIDER}, sl[3];
            ijoy_plan_axes(kind, 3, sl);
            CHECK(sl[0] == 7 && sl[1] == 2 && sl[2] == 6,
                  "w312 wingman: ohne Usage-Hinweis S1/Z (alt), mit Hinweis X/Y (neu)");
        }
    }
    /* Fake-Geraet: Werte kommen am richtigen Slot an, Bereich umgerechnet. */
    ijoy_test_begin(1, (const char (*)[96])specs, 1u);
    ijoy_test_objects(0, o, n);
    ijoy_test_obj_value(0, 0, 1023); /* Aileron voll rechts */
    ijoy_test_obj_value(0, 1, 0);    /* Elevator voll vorn  */
    ijoy_test_obj_value(0, 2, 255);  /* Schub voll          */
    ijoy_test_obj_value(0, 3, 9000); /* Hat rechts          */
    ijoy_test_obj_value(0, 4 + 8, 1); /* Taste 9            */
    ijoy_test_run(40);
    m = ijoy_slots_get(js);
    in = ijoy_info(0);
    CHECK(m == 1u && in && in->fmt == 2 && in->axmask == 0x43u && in->axnorm == 0x43u &&
              js[0].axis[0] == 32767 && js[0].axis[1] == -32768 && js[0].axis[6] == 32767,
          "w312 wingman: X=%d Y=%d S0=%d (fmt=%d mask=%02X norm=%02X)", js[0].axis[0],
          js[0].axis[1], js[0].axis[6], in ? in->fmt : -1, in ? in->axmask : 0,
          in ? in->axnorm : 0);
    CHECK(js[0].pov[0] == 9000 && (js[0].btn[0] & (1u << 8)) && in && in->nbtn == 9 && in->npov == 1,
          "w312 wingman: Hat=%d, Taste 9 gelesen (nbtn=%d npov=%d)", js[0].pov[0],
          in ? in->nbtn : -1, in ? in->npov : -1);
    ijoy_test_obj_value(0, 0, 512);
    ijoy_test_obj_value(0, 1, 512);
    ijoy_test_run(8);
    ijoy_slots_get(js);
    CHECK(js[0].axis[0] >= -64 && js[0].axis[0] <= 64 && js[0].axis[1] >= -64 && js[0].axis[1] <= 64,
          "w312 wingman: Mitte 512 -> ~0 (%d/%d)", js[0].axis[0], js[0].axis[1]);
    {
        /* Geraeteinfo: Metadaten ja, Werte nie. */
        static char txt[8192];
        const ijoy_obj_t *ob = NULL;
        int no = ijoy_objects(0, &ob), len;
        ijoy_test_obj_value(0, 0, 777);
        ijoy_test_obj_value(0, 2, 199);
        ijoy_test_run(8);
        ijoy_slots_get(js);
        len = ijoy_devinfo_text(in, ob, no, txt, (int)sizeof txt);
        CHECK(no == n && len > 200 && strstr(txt, "Aileron") && strstr(txt, "02:B0") &&
                  strstr(txt, "0..1023") && strstr(txt, "per object") && strstr(txt, "| X\r\n") &&
                  strstr(txt, "Button 9") && strstr(txt, "POV1"),
              "w312 info: Objektliste mit Usage, Bereich, Zuordnung (%d Objekte, %d Zeichen)", no, len);
        CHECK(!strstr(txt, "777") && !strstr(txt, "199") && !strstr(txt, "9000") &&
                  !strstr(txt, "32767 |"),
              "w312 info: keine Eingabewerte im Text");
    }
    ijoy_test_end();

    /* C: Hori im PS4-Modus (DS4-Deskriptor) -> alles im Standard-Format,
     * Touchpad = B14 wird gelesen (fehlt sie, laeuft der Stick als XInput). */
    n = w312_hori(o);
    m = ijoy_std_capture(o, n, rd);
    CHECK(m == 0x3Fu && !ijoy_need_own_format(o, n) && rd[n - 1] == 13,
          "w312 hori: 6 Achsen, POV, Touchpad = B14 gelesen (m=%02X)", m);
    ijoy_test_begin(1, (const char (*)[96])specs, 1u);
    ijoy_test_objects(0, o, n);
    ijoy_test_obj_value(0, 0, 32767); /* nur X rechts */
    ijoy_test_obj_value(0, n - 1, 1); /* Touchpad */
    ijoy_test_run(40);
    ijoy_slots_get(js);
    in = ijoy_info(0);
    CHECK(in && in->fmt == 0 && js[0].axis[0] == 32767 && js[0].axis[1] == 0 &&
              (js[0].btn[0] & (1u << 13)),
          "w312 hori: X allein -> nur X (Y=%d), B14 gedrueckt", js[0].axis[1]);
    ijoy_test_end();

    /* D: Standard-HOTAS ueber das Fake-Geraet: bitgleich (Standard-Format). */
    n = w312_hotas(o);
    ijoy_test_begin(1, (const char (*)[96])specs, 1u);
    ijoy_test_objects(0, o, n);
    ijoy_test_obj_value(0, 0, -12345);
    ijoy_test_obj_value(0, 2, 23456);
    ijoy_test_run(40);
    ijoy_slots_get(js);
    in = ijoy_info(0);
    CHECK(in && in->fmt == 0 && in->axnorm == 0 && js[0].axis[0] == -12345 && js[0].axis[5] == 23456,
          "w312 hotas: Werte unveraendert durchgereicht (X=%d RZ=%d)", js[0].axis[0], js[0].axis[5]);

    /* E: Kalibrierung - ohne Eintrag bitgleich, mit Eintrag angewendet. */
    {
        ijoy_cal_t c = {-20000, 1000, 22000, 10, 0}, c2;
        unsigned short v = 0, p = 0;
        int ax = -1;
        char line[96];
        imap_joy_state_t st, raw;
        CHECK(ijoy_cal_apply(&c, 1000) == 0 && ijoy_cal_apply(&c, 22000) == 32767 &&
                  ijoy_cal_apply(&c, -20000) == -32768 && ijoy_cal_apply(&c, 3000) == 0 &&
                  ijoy_cal_apply(&c, 30000) == 32767,
              "w312 cal: Mitte/Enden/Totzone (%d %d %d %d)", ijoy_cal_apply(&c, 1000),
              ijoy_cal_apply(&c, 22000), ijoy_cal_apply(&c, -20000), ijoy_cal_apply(&c, 3000));
        {
            int16_t h = ijoy_cal_apply(&c, 11500 + 1050); /* halber Weg + halbe Totzone */
            CHECK(h > 15000 && h < 19000, "w312 cal: Totzone reskaliert (%d)", h);
        }
        c.inv = 1;
        CHECK(ijoy_cal_apply(&c, 22000) == -32767 && ijoy_cal_apply(&c, -20000) == 32767,
              "w312 cal: invertiert");
        c.inv = 0;
        c.mid = IJOY_CAL_NOMID;
        c.dz = 0;
        c.lo = -30000;
        c.hi = 30000;
        CHECK(ijoy_cal_apply(&c, -30000) == -32768 && ijoy_cal_apply(&c, 30000) == 32767 &&
                  ijoy_cal_apply(&c, 0) >= -1 && ijoy_cal_apply(&c, 0) <= 0,
              "w312 cal: linear (Schub) %d..%d", ijoy_cal_apply(&c, -30000), ijoy_cal_apply(&c, 30000));
        c.mid = 1000;
        c.dz = 10;
        c.lo = -20000;
        c.hi = 22000;
        CHECK(ijoy_cal_format(0x046D, 0xC283, 1, &c, line, sizeof line) &&
                  strcmp(line, "046D:C283,Y,-20000,1000,22000,10,0") == 0 &&
                  ijoy_cal_parse(line, &v, &p, &ax, &c2) && v == 0x046D && p == 0xC283 && ax == 1 &&
                  memcmp(&c, &c2, sizeof c) == 0,
              "w312 cal: Zeile hin und zurueck (%s)", line);
        CHECK(ijoy_cal_parse("046D:C283,S0,-32768,-,32767,5,1", &v, &p, &ax, &c2) && ax == 6 &&
                  c2.mid == IJOY_CAL_NOMID && c2.inv == 1 &&
                  !ijoy_cal_parse("046D:C283,Q,0,-,100,5,0", &v, &p, &ax, &c2) &&
                  !ijoy_cal_parse("046D:C283,X,100,-,50,5,0", &v, &p, &ax, &c2) &&
                  !ijoy_cal_parse("046D:C283,X,0,-,100,80,0", &v, &p, &ax, &c2),
              "w312 cal: Parser (Schub ohne Mitte, ungueltige abgelehnt)");
        /* am Geraet: Eintrag fuer VID/PID des Fake-Geraets 0 (046D:5678) */
        ijoy_cal_clear();
        CHECK(ijoy_read2(0, &st, &raw) == 1 && st.axis[0] == raw.axis[0] && st.axis[0] == -12345,
              "w312 cal: ohne Eintrag bitgleich (%d)", st.axis[0]);
        c.lo = -20000;
        c.mid = -12345;
        c.hi = 20000;
        c.dz = 5;
        CHECK(ijoy_cal_set(0x046D, 0x5678, 0, &c) == 0 && ijoy_cal_count() == 1, "w312 cal: Eintrag gesetzt");
        CHECK(ijoy_read2(0, &st, &raw) == 1 && st.axis[0] == 0 && raw.axis[0] == -12345 &&
                  st.axis[5] == 23456,
              "w312 cal: angewendet (Mitte -> %d, roh %d, andere Achse unberuehrt)", st.axis[0],
              raw.axis[0]);
        ijoy_test_run(8);
        ijoy_slots_get(js);
        CHECK(js[0].axis[0] == 0, "w312 cal: Client-Slot liest kalibriert (%d)", js[0].axis[0]);
        ijoy_cal_set(0x046D, 0x5678, 0, NULL);
        CHECK(ijoy_cal_count() == 0, "w312 cal: Eintrag entfernt");
        ijoy_cal_clear();
    }
    ijoy_test_end();

    /* F: Diagonale/Totzone im Client - je Achse, keine Achse verlangt die andere. */
    {
        static const char kProf[] = "joy0 = Hori\naim_x = JOY0:X:dz=0.1\naim_y = JOY0:Y:dz=0.1:inv=1\n";
        imap_profile_t p;
        imap_snap_t s;
        imap_block_t b;
        imap_state_t st;
        CHECK(imap_profile_parse(kProf, "w312", &p) == 0, "w312 diag: Profil");
        imap_state_init(&st);
        joy_snap(&s, 900001);
        s.joy_mask = 1u;
        s.joy[0].axis[0] = 32767; /* nur rechts */
        s.joy[0].axis[1] = 0;
        imap_profile_apply(&b, &st, &p, &s);
        CHECK(b.aiming_x == 0xFF && b.aiming_y == 0x7F, "w312 diag: nur X -> X voll, Y Mitte (%02X/%02X)",
              b.aiming_x, b.aiming_y);
        s.joy[0].axis[0] = 0;
        s.joy[0].axis[1] = -32768; /* nur vorn */
        imap_profile_apply(&b, &st, &p, &s);
        CHECK(b.aiming_x == 0x7F && b.aiming_y == 0xFF, "w312 diag: nur Y -> Y voll, X Mitte (%02X/%02X)",
              b.aiming_x, b.aiming_y);
        s.joy[0].axis[0] = joy_pos(0.15f); /* knapp ueber der Totzone, Y in Ruhe */
        s.joy[0].axis[1] = joy_pos(0.05f);
        imap_profile_apply(&b, &st, &p, &s);
        CHECK(b.aiming_x > 0x7F && b.aiming_y == 0x7F, "w312 diag: X knapp ueber Totzone wirkt allein (%02X/%02X)",
              b.aiming_x, b.aiming_y);
        s.joy[0].axis[0] = joy_pos(0.7f);
        s.joy[0].axis[1] = joy_pos(-0.7f);
        imap_profile_apply(&b, &st, &p, &s);
        CHECK(b.aiming_x > 0xC0 && b.aiming_y > 0xC0, "w312 diag: diagonal -> beide (%02X/%02X)",
              b.aiming_x, b.aiming_y);
    }
    (void)i;
}

/* Teil 25: W365 BIND-COMBO - Kombis aus dem Launcher (alle Zeilen): Akkord gewinnt, die Einzeltaste
 * feuert nicht zusaetzlich (auch Gang-Direktwahl, Client-Aktionen, Zusatztasten-Familie LCTRL ~ CTRL). */
static void w365_apply(const imap_profile_t *p, imap_block_t *b, imap_state_t *st, const int *vk, int n,
                       unsigned short padb, unsigned fr)
{
    cfgvar_t c = { 1, 1, 0, 1, 4, 0, 0 };
    imap_snap_t s;
    make_snap(&s, fr, &c);
    set_keys(vk, n);
    memset(&s.pad, 0, sizeof s.pad);
    s.pad.buttons = padb;
    s.pad_present = padb != 0;
    imap_profile_apply(b, st, p, &s);
}

static void run_w365_combo_cases(void)
{
    static const int k_ctrl_s[] = { VK_CONTROL, VK_LCONTROL, 'S' }, k_s[] = { 'S' },
                     k_ctrl[] = { VK_CONTROL, VK_LCONTROL }, k_shift_s[] = { VK_SHIFT, VK_LSHIFT, 'S' },
                     k_ctrl_a[] = { VK_CONTROL, VK_RCONTROL, 'A' }, k_a[] = { 'A' },
                     k_shift_f8[] = { VK_SHIFT, VK_LSHIFT, VK_F8 }, k_f8[] = { VK_F8 };
    imap_profile_t p;
    imap_state_t st;
    imap_block_t b;
    imap_snap_t s;
    cfgvar_t c = { 1, 1, 0, 1, 4, 0, 0 };
    unsigned fr = 1;
    printf("Teil 25: W365 BIND-COMBO\n");
    /* Tastatur Strg+S auf bi1, S allein auf bi0 */
    CHECK(imap_profile_parse("bi0 = KEY:S\nbi1 = KEY:CTRL+KEY:S\n", "w365", &p) == 0, "w365: parse Strg+S");
    imap_state_init(&st);
    w365_apply(&p, &b, &st, k_ctrl_s, 3, 0, fr++);
    CHECK((b.buttons[0] & 2u) && !(b.buttons[0] & 1u), "w365: Strg+S -> nur bi1 (btn=%04X)", b.buttons[0]);
    w365_apply(&p, &b, &st, k_s, 1, 0, fr++);
    CHECK((b.buttons[0] & 1u) && !(b.buttons[0] & 2u), "w365: S allein -> nur bi0 (btn=%04X)", b.buttons[0]);
    /* Familie: Akkord mit CTRL unterdrueckt Einzelbelegung LCTRL (Pedal) */
    CHECK(imap_profile_parse("pedal_l = KEY:LCTRL\nbi1 = KEY:CTRL+KEY:S\n", "w365", &p) == 0, "w365: parse LCTRL");
    imap_state_init(&st);
    w365_apply(&p, &b, &st, k_ctrl_s, 3, 0, fr++);
    CHECK((b.buttons[0] & 2u) && b.pedal_l == 0, "w365: Strg+S -> bi1, Pedal LCTRL schweigt (%u)", b.pedal_l);
    w365_apply(&p, &b, &st, k_ctrl, 2, 0, fr++);
    CHECK(!(b.buttons[0] & 2u) && b.pedal_l == 0xFF, "w365: LCTRL allein -> Pedal (%u)", b.pedal_l);
    /* Pad LB+X auf bi1, X allein auf bi0 */
    CHECK(imap_profile_parse("bi0 = PAD:X\nbi1 = PAD:LB+PAD:X\n", "w365", &p) == 0, "w365: parse LB+X");
    imap_state_init(&st);
    w365_apply(&p, &b, &st, NULL, 0, (unsigned short)(IMAP_PAD_LB | IMAP_PAD_X), fr++);
    CHECK((b.buttons[0] & 2u) && !(b.buttons[0] & 1u), "w365: LB+X -> nur bi1 (btn=%04X)", b.buttons[0]);
    w365_apply(&p, &b, &st, NULL, 0, (unsigned short)IMAP_PAD_X, fr++);
    CHECK((b.buttons[0] & 1u) && !(b.buttons[0] & 2u), "w365: X allein -> nur bi0 (btn=%04X)", b.buttons[0]);
    /* Gang-Direktwahl mit Akkord unterdrueckt die Einzeltaste (vorher nicht gesammelt) */
    CHECK(imap_profile_parse("bi0 = KEY:S\ngear.1 = KEY:SHIFT+KEY:S\n", "w365", &p) == 0, "w365: parse gear.1");
    imap_state_init(&st);
    w365_apply(&p, &b, &st, k_shift_s, 3, 0, fr++);
    CHECK(b.gear == 9 && !(b.buttons[0] & 1u), "w365: Umschalt+S -> Gang 1, bi0 schweigt (gear=%u btn=%04X)",
          b.gear, b.buttons[0]);
    /* Client-Aktion mit Akkord unterdrueckt Spiel-Taste */
    CHECK(imap_profile_parse("bi0 = KEY:A\naim_center = KEY:CTRL+KEY:A\n", "w365", &p) == 0, "w365: parse aim_center");
    imap_state_init(&st);
    w365_apply(&p, &b, &st, k_ctrl_a, 3, 0, fr++);
    CHECK(!(b.buttons[0] & 1u), "w365: Strg+A (aim_center) -> bi0 schweigt (btn=%04X)", b.buttons[0]);
    w365_apply(&p, &b, &st, k_a, 1, 0, fr++);
    CHECK((b.buttons[0] & 1u) != 0, "w365: A allein -> bi0");
    /* vr_recenter F8 vs. orig_view Umschalt+F8 (Standardprofile): Umschalt+F8 zentriert nicht mehr */
    CHECK(imap_profile_parse("vr_recenter = KEY:F8\norig_view = KEY:SHIFT+KEY:F8\n", "w365", &p) == 0,
          "w365: parse vr/orig");
    imap_state_init(&st);
    make_snap(&s, fr++, &c);
    set_keys(k_shift_f8, 3);
    CHECK(imap_action_vr_recenter(&p, &st, &s) == 0, "w365: Umschalt+F8 -> kein vr_recenter");
    make_snap(&s, fr++, &c);
    set_keys(NULL, 0);
    (void)imap_action_vr_recenter(&p, &st, &s);
    make_snap(&s, fr++, &c);
    set_keys(k_f8, 1);
    CHECK(imap_action_vr_recenter(&p, &st, &s) == 1, "w365: F8 allein -> vr_recenter");
    /* Vom Launcher erzeugte Formen parsen (numerische Taste, Maus, Joystick, Original-Controller) */
    CHECK(imap_profile_parse("bi0 = KEY:CTRL+KEY:20\nbi1 = KEY:SHIFT+MOUSE:LEFT\nbi2 = JOY0:B1+JOY0:B2\n"
                             "bi3 = SBC:CHAFF+SBC:EJECT\nbi4 = PAD:LT+PAD:A\n", "w365", &p) == 0 &&
          p.btn[0].alt[0].chord && p.btn[0].alt[0].b.code == 20 && p.btn[1].alt[0].chord &&
          p.btn[2].alt[0].chord && p.btn[3].alt[0].chord && p.btn[4].alt[0].chord,
          "w365: Launcher-Kombis (KEY:20, Maus, JOY, SBC, LT) parsen als Akkord");
}

/* Teil 28 (W394 LOC-VIEW-PERFRAME): Vorschau des Maus-Akkus ohne Doppelzaehlung. Zwischen zwei Polls liest die Anzeige
 * mehrmals den Akku-Stand plus die noch nicht abgeholten Deltas (imap_mouse_preview). Pruefung: (a) Vorschau = Ergebnis
 * eines Polls mit genau diesen Deltas (auf einer Kopie), (b) der Zustand der Simulation bleibt durch Vorschauen
 * unveraendert, (c) nach dem echten Poll ist der Zustand bitgleich mit einem Bezugslauf ganz ohne Vorschau. */
static int w394_realvk(int vk) { (void)vk; return 0; }
static void run_w394_preview_cases(void)
{
    static const int sens_v[] = {1, 4, 9, 16};
    int abs_m, si, sight, inv, poll, part, nprev = 0, nbad = 0;
    uint32_t rnd = 12345u;
    printf("Teil 28: W394 Maus-Akku-Vorschau ohne Doppelzaehlung ...\n");
    for (abs_m = 0; abs_m < 2; abs_m++)
        for (si = 0; si < 4; si++)
            for (sight = 0; sight < 2; sight++)
                for (inv = 0; inv < 2; inv++) {
                    imap_state_t sim, ref, before, prev, chk;
                    imap_snap_t sn;
                    imap_block_t b1, b2;
                    imap_state_init(&sim);
                    imap_state_init(&ref);
                    memset(&sn, 0, sizeof sn);
                    memset(g_keys, 0, sizeof g_keys);
                    sn.key = stub_key;
                    sn.real_vk = w394_realvk;
                    sn.mouse_aim = 1;
                    sn.mouse_raw = 1;
                    sn.mouse_live = 1;
                    sn.mouse_abs = abs_m;
                    sn.mouse_sight = sight;
                    sn.mouse_sens = sens_v[si];
                    sn.mouse_invert_x = inv;
                    sn.mouse_invert_y = !inv;
                    for (poll = 0; poll < 400; poll++) {
                        int pdx = 0, pdy = 0;
                        sn.frame = (unsigned)poll;
                        for (part = 0; part < 4; part++) { /* Deltas kommen in Teilen zwischen zwei Polls an */
                            rnd = rnd * 1664525u + 1013904223u;
                            pdx += (int)((rnd >> 8) % 41u) - 20;
                            rnd = rnd * 1664525u + 1013904223u;
                            pdy += (int)((rnd >> 8) % 41u) - 20;
                            before = sim;
                            imap_mouse_preview(&sim, &sn, pdx, pdy, &prev);
                            nprev++;
                            if (memcmp(&before, &sim, sizeof sim) != 0) nbad++; /* (b) */
                            chk = sim;
                            sn.raw_dx = pdx; sn.raw_dy = pdy;
                            imap_legacy_apply(&b1, &chk, &sn);
                            if (chk.m_ax != prev.m_ax || chk.m_ay != prev.m_ay || chk.m_sx != prev.m_sx ||
                                chk.m_sy != prev.m_sy || chk.m_fx != prev.m_fx || chk.m_fy != prev.m_fy) nbad++; /* (a) */
                        }
                        sn.raw_dx = pdx; sn.raw_dy = pdy; /* echter Poll holt alles genau einmal ab */
                        imap_legacy_apply(&b1, &sim, &sn);
                        imap_legacy_apply(&b2, &ref, &sn);
                        if (memcmp(&sim, &ref, sizeof sim) != 0 || memcmp(&b1, &b2, sizeof b1) != 0) nbad++; /* (c) */
                    }
                }
    { /* Gatter wie der Poll: Maus nicht wirksam -> Vorschau = Stand */
        imap_state_t st, pv;
        imap_snap_t sn;
        imap_state_init(&st);
        memset(&sn, 0, sizeof sn);
        sn.key = stub_key;
        sn.mouse_aim = 1; sn.mouse_raw = 1; sn.mouse_live = 1; sn.mouse_sens = 4; sn.real_blocked = 1;
        imap_mouse_preview(&st, &sn, 50, 50, &pv);
        CHECK(memcmp(&st, &pv, sizeof st) == 0, "W394: gesperrte Maus veraendert die Vorschau");
        sn.real_blocked = 0; sn.replay_active = 1; sn.mouse_live = 0;
        imap_mouse_preview(&st, &sn, 50, 50, &pv);
        CHECK(memcmp(&st, &pv, sizeof st) == 0, "W394: Replay ohne Live-Maus veraendert die Vorschau");
        sn.replay_active = 0; sn.mouse_live = 1;
        imap_mouse_preview(&st, &sn, 50, 0, &pv);
        CHECK(pv.m_ax == 0x7F + 50, "W394: Vorschau relativ sens 4: %d", pv.m_ax);
        imap_mouse_preview(&st, &sn, 500, 0, &pv);
        CHECK(pv.m_ax == 0x7F + 64, "W394: Vorschau begrenzt auf 64 Counts: %d", pv.m_ax);
    }
    CHECK(nbad == 0, "W394: %d Abweichungen in %d Vorschauen", nbad, nprev);
    printf("Teil 28: %d Vorschauen, %d Abweichungen\n", nprev, nbad);
}

/* Teil 30 (W403 LAUNCHER-R1-POLISH): Eject (bi3) hat in beiden Standardprofilen Umschalt+Entf (Akkord gegen
 * Versehen; Strg waere das Pedal). Nur der Akkord loest Eject aus, Entf bzw. Umschalt allein nichts, das Pedal
 * schweigt; Gamepad BACK+Y bleibt. */
static void run_w403_eject_cases(const imap_profile_t *kb, const imap_profile_t *xbox)
{
    static const int k_sd[] = { VK_SHIFT, VK_LSHIFT, VK_DELETE }, k_d[] = { VK_DELETE },
                     k_s[] = { VK_SHIFT, VK_LSHIFT };
    const imap_profile_t *pr[2];
    imap_state_t st;
    imap_block_t b;
    unsigned fr = 1;
    int i;
    pr[0] = kb;
    pr[1] = xbox;
    printf("Teil 30: W403 Eject-Standard\n");
    for (i = 0; i < 2; i++) {
        const imap_bind_t *e = &pr[i]->btn[3];
        int k, chord = 0, pad = 0;
        for (k = 0; k < e->n; k++) {
            const imap_chord_t *c = &e->alt[k];
            if (c->chord && c->a.kind == IMAP_A_KEY && c->a.code == VK_SHIFT && c->b.kind == IMAP_A_KEY &&
                c->b.code == VK_DELETE) chord = 1;
            if (c->chord && c->a.kind != IMAP_A_KEY && c->b.kind != IMAP_A_KEY) pad = 1;
        }
        CHECK(chord, "w403: %s bi3 = KEY:SHIFT+KEY:DELETE (n=%d)", i ? "xbox" : "kb", e->n);
        CHECK(i == 0 ? e->n == 1 : (e->n == 2 && pad), "w403: %s bi3 Quellen (n=%d, pad=%d)", i ? "xbox" : "kb",
              e->n, pad);
        imap_state_init(&st);
        w365_apply(pr[i], &b, &st, k_sd, 3, 0, fr++);
        CHECK(b.buttons[0] == (1u << 3) && b.buttons[1] == 0 && b.buttons[2] == 0 && b.pedal_l == 0,
              "w403: %s Umschalt+Entf -> nur Eject (btn=%04X %04X %04X pedal=%u)", i ? "xbox" : "kb", b.buttons[0],
              b.buttons[1], b.buttons[2], b.pedal_l);
        w365_apply(pr[i], &b, &st, k_d, 1, 0, fr++);
        CHECK(!(b.buttons[0] & (1u << 3)), "w403: %s Entf allein -> kein Eject (btn=%04X)", i ? "xbox" : "kb",
              b.buttons[0]);
        w365_apply(pr[i], &b, &st, k_s, 2, 0, fr++);
        CHECK(b.buttons[0] == 0 && b.buttons[1] == 0 && b.buttons[2] == 0,
              "w403: %s Umschalt allein -> keine Taste (btn=%04X)", i ? "xbox" : "kb", b.buttons[0]);
        if (i == 1) {
            w365_apply(pr[i], &b, &st, NULL, 0, (unsigned short)(IMAP_PAD_BACK | IMAP_PAD_Y), fr++);
            CHECK((b.buttons[0] & (1u << 3)) != 0, "w403: xbox BACK+Y -> Eject (btn=%04X)", b.buttons[0]);
        }
    }
    { /* W436 LOC-EJECT (#76): Standardbelegung ohne Profil (Legacy) - der Launcher zeigt dafuer keyboard_default */
        cfgvar_t c = { 1, 1, 0, 1, 4, 0, 0 };
        imap_snap_t s;
        imap_state_init(&st);
        make_snap(&s, fr++, &c);
        set_keys(k_sd, 3);
        imap_legacy_apply(&b, &st, &s);
        CHECK(b.buttons[0] == (1u << 3) && b.buttons[1] == 0 && b.buttons[2] == 0 && b.pedal_l == 0,
              "w436: Legacy Umschalt+Entf -> nur Eject (btn=%04X %04X %04X pedal=%u)", b.buttons[0], b.buttons[1],
              b.buttons[2], b.pedal_l);
        make_snap(&s, fr++, &c);
        set_keys(k_d, 1);
        imap_legacy_apply(&b, &st, &s);
        CHECK(!(b.buttons[0] & (1u << 3)), "w436: Legacy Entf allein -> kein Eject (btn=%04X)", b.buttons[0]);
        make_snap(&s, fr++, &c);
        set_keys(k_s, 2);
        imap_legacy_apply(&b, &st, &s);
        CHECK(b.buttons[0] == 0 && b.buttons[1] == 0 && b.buttons[2] == 0,
              "w436: Legacy Umschalt allein -> keine Taste (btn=%04X)", b.buttons[0]);
        set_keys(NULL, 0);
    }
}

/* Teil 32 (W436 LOC-EJECT, GitHub #76): Eject per Kombination bis ins Rollback-Eingabewort. Kette: Umschalt+Entf
 * (Legacy, keyboard_default, default_full) bzw. Pad BACK+Y -> SBC-Block bi3 -> Eingabesatz wie FUN_0001CAE0 (LoC,
 * Ghidra nur gelesen: Knopf 0..3 -> Byte 0 Bit 0..3, Eject = 0x08) -> eigener Rahmen der Eingabeschicht (loc_rbi.c)
 * -> Paket -> Entnahme beim Mitspieler. Druck genau einen Tick (eigener Rahmen q = 6 -> Slot q - P0 + D = 4).
 * a) Paket rechtzeitig: Slot 4 traegt 0x08, kein anderer Slot. b) Paket kommt erst nach Slot 6: der Mitspieler hat
 * Slots 4..6 vorhergesagt (ohne Eject, keine erfundene Flanke), verlangt einen Rollback, die Neurechnung entnimmt in
 * Slot 4 den Eject. */
#include "loc_rbi.h"
static void w436_pack(const imap_block_t *b, uint8_t in[RBI_IN])
{
    int k;
    memset(in, 0, RBI_IN);
    for (k = 0; k < 4; k++)
        if (b->buttons[0] & (1u << k)) in[0] |= (uint8_t)(1u << k);
}

/* Rueckgabe 0 = gruen; late = Paket mit dem Druck erst nach den Vorhersagen zustellen */
static int w436_chain(const imap_block_t *press, int late, char *why, size_t cap)
{
    rbi_t *a = rbi_new(), *b = rbi_new();
    uint8_t hdr[RBI_HDR] = { 1, 0, 0, 0, 3, 0, 0, 0 }, in[RBI_IN], out[RBI_FR], p[RBI_PKT_MAX];
    imap_block_t idle;
    int q, n, s, bad = 0, sent = 0, pl = 0;
    uint32_t ticks = 0, sback = 0;
    memset(&idle, 0, sizeof idle);
    idle.aiming_x = idle.aiming_y = 0x7F;
    why[0] = '\0';
    rbi_begin(a, 5, 0, 2, 8);
    rbi_begin(b, 5, 1, 2, 8);
    for (q = 0; q < 4; q++) { /* Vorlauf P0 = 4 (wie FUN_0001D3D0) */
        w436_pack(&idle, in);
        rbi_own(a, hdr, in);
    }
    for (q = 4; q < 12; q++) { /* je Tick: eigene Entnahme, dann eigener Rahmen q (Druck nur in q = 6) */
        (void)rbi_take(a, 0, 0, out);
        (void)rbi_tick_end(a, NULL);
        w436_pack(q == 6 ? press : &idle, in);
        rbi_own(a, hdr, in);
        if (late && q == 5) { /* nur Rahmen bis q = 5 (Slot 3) zustellen */
            n = rbi_tx(a, p, (int)sizeof p);
            if (n <= 0 || rbi_rx(b, p, n, 1) != 1) bad |= 1;
            sent = 1;
        }
    }
    if (!late) {
        n = rbi_tx(a, p, (int)sizeof p);
        if (n <= 0 || rbi_rx(b, p, n, 1) != 1) bad |= 1;
        for (s = 0; s < 9; s++) {
            if (rbi_take(b, 0, 0, out) != 1) { bad |= 2; break; }
            (void)rbi_tick_end(b, NULL);
            if (((out[RBI_HDR] & 0x08) != 0) != (s == 4)) bad |= 4;
        }
    } else {
        for (s = 0; s < 7; s++) { /* Slots 0..3 bestaetigt, 4..6 vorhergesagt */
            if (rbi_take(b, 0, 0, out) != 1) { bad |= 2; break; }
            (void)rbi_tick_end(b, NULL);
            if (out[RBI_HDR] & 0x08) bad |= 8; /* Vorhersage erfindet keinen Eject */
        }
        n = rbi_tx(a, p, (int)sizeof p); /* jetzt der Rest (mit dem Druck) */
        if (!sent || n <= 0 || rbi_rx(b, p, n, 1) != 1) bad |= 1;
        pl = rbi_plan(b, rbi_commits(b), &ticks, &sback);
        if (pl <= 0 || ticks < 3u) bad |= 16;
        for (s = 4; s < 9 && !bad; s++) { /* Neurechnung ab Slot 4, dann live weiter */
            if (rbi_take(b, 0, 0, out) != 1) { bad |= 2; break; }
            (void)rbi_tick_end(b, NULL);
            if (((out[RBI_HDR] & 0x08) != 0) != (s == 4)) bad |= 4;
        }
    }
    _snprintf_s(why, cap, _TRUNCATE, "bad=0x%X plan=%d ticks=%u", bad, pl, ticks);
    rbi_free(a);
    rbi_free(b);
    return bad;
}

static void run_w436_eject_chain(const imap_profile_t *kb, const imap_profile_t *xbox)
{
    static const int k_sd[] = { VK_SHIFT, VK_LSHIFT, VK_DELETE };
    cfgvar_t c = { 1, 1, 0, 1, 4, 0, 0 };
    imap_profile_t full;
    imap_state_t st;
    imap_snap_t s;
    imap_block_t b[4];
    static const char *const nm[4] = { "Legacy Umschalt+Entf", "keyboard_default Umschalt+Entf",
                                       "default_full Umschalt+Entf", "xbox_pad_default BACK+Y" };
    char why[96];
    int i, late, rc;
    printf("Teil 32: W436 Eject per Kombination im Rollback-Eingabewort\n");
    imap_state_init(&st);
    make_snap(&s, 1, &c);
    set_keys(k_sd, 3);
    imap_legacy_apply(&b[0], &st, &s);
    imap_state_init(&st);
    w365_apply(kb, &b[1], &st, k_sd, 3, 0, 2);
    rc = imap_profile_load("default_full", &full);
    CHECK(rc == 0, "w436: default_full geladen (rc=%d)", rc);
    imap_state_init(&st);
    if (rc == 0) w365_apply(&full, &b[2], &st, k_sd, 3, 0, 3);
    else memset(&b[2], 0, sizeof b[2]);
    imap_state_init(&st);
    w365_apply(xbox, &b[3], &st, NULL, 0, (unsigned short)(IMAP_PAD_BACK | IMAP_PAD_Y), 4);
    set_keys(NULL, 0);
    for (i = 0; i < 4; i++) {
        CHECK((b[i].buttons[0] & (1u << 3)) != 0, "w436: %s -> SBC bi3 (btn=%04X)", nm[i], b[i].buttons[0]);
        for (late = 0; late < 2; late++) {
            int bad = w436_chain(&b[i], late, why, sizeof why);
            CHECK(bad == 0, "w436: %s, %s: Eject nicht genau in Slot 4 (%s)", nm[i],
                  late ? "Paket spaet (Vorhersage + Rollback)" : "Paket rechtzeitig", why);
            if (i == 0) printf("  %s, %s: %s\n", nm[i], late ? "spaet" : "rechtzeitig", why);
        }
    }
}

/* Teil 29 (W396 LOC-POLISH): Maus-Akkus beim Missionswechsel zur Mitte, auch die Sicht (mouse_sight=1). */
static void run_w396_center_cases(void)
{
    imap_state_t st;
    imap_snap_t sn;
    imap_block_t b;
    printf("Teil 29: W396 Maus-Akkus zur Mitte (Ziel + Sicht) ...\n");
    imap_state_init(&st);
    memset(&sn, 0, sizeof sn);
    memset(g_keys, 0, sizeof g_keys);
    sn.key = stub_key;
    sn.real_vk = w394_realvk;
    sn.mouse_aim = 1;
    sn.mouse_raw = 1;
    sn.mouse_live = 1;
    sn.mouse_sight = 1;
    sn.mouse_sens = 16;
    sn.raw_dx = 40;
    sn.raw_dy = -30;
    imap_legacy_apply(&b, &st, &sn);
    CHECK(b.sight_x > 25 && b.sight_y < -25, "W396: Maus -> Sicht vor dem Zentrieren: %d,%d", b.sight_x, b.sight_y);
    st.m_ax = 0x20;
    st.m_fx = 0.5f;
    imap_mouse_center(&st);
    CHECK(st.m_sx == 0 && st.m_sy == 0 && st.m_ax == 0x7F && st.m_ay == 0x7F && st.m_fx == 0.0f && st.m_fy == 0.0f,
          "W396: Mitte: s %d,%d a %d,%d", st.m_sx, st.m_sy, st.m_ax, st.m_ay);
    sn.raw_dx = sn.raw_dy = 0;
    imap_legacy_apply(&b, &st, &sn);
    CHECK(b.sight_x == 0 && b.sight_y == 0, "W396: Sicht nach dem Zentrieren in Ruhe: %d,%d", b.sight_x, b.sight_y);
}

/* Teil 31: W431 DEFAULT-BINDINGS - default_full: jede Aktion hat eine Taste, keine Doppel-/Client-Tasten. */
static void run_w431_default_full_cases(void)
{
    /* W438: ENTER ist im Spiel fuer den LoC-Chat reserviert, ESC fuer das Client-Menue; BACKSPACE ist seit
     * 0.4.0 START (bi6) und keine reine Client-Taste mehr (replay_exit teilt sie bewusst, s. unten). */
    static const int reserved[] = { VK_ESCAPE, VK_F10, VK_F2, VK_F3, VK_F8, VK_F9, VK_F11, VK_F12, VK_PAUSE,
                                    VK_RETURN, VK_SNAPSHOT };
    imap_profile_t p;
    int bi, a, k, n, rc, used[256][2], nused = 0, bad_reserved = 0, dup = 0, unbound = 0, nopress = 0;
    printf("Teil 31: W431 default_full (jede Aktion belegt)\n");
    rc = imap_profile_load("default_full", &p);
    CHECK(rc == 0, "w431: default_full.ini gefunden (rc=%d)", rc);
    if (rc != 0) return;
    CHECK(p.has_pad == 0 && p.has_sbc == 0, "w431: nur Tastatur/Maus");
    for (bi = 0; bi < 39; bi++) { /* Tasten: belegt, einzeln gedrueckt -> genau dieses Bit */
        int vks[8], mods[8];
        n = imap_bind_key_combos(&p.btn[bi], vks, mods, 8);
        if (p.btn[bi].n == 0 || n == 0) { unbound++; printf("  bi%d unbelegt\n", bi); continue; }
        for (a = 0; a < n; a++) {
            for (k = 0; k < (int)(sizeof reserved / sizeof reserved[0]); k++)
                if (vks[a] == reserved[k] && mods[a] == 0) { bad_reserved++; printf("  bi%d nutzt Client-Taste %d\n", bi, vks[a]); }
            for (k = 0; k < nused; k++)
                if (used[k][0] == vks[a] && used[k][1] == mods[a]) { dup++; printf("  bi%d doppelt (VK %d)\n", bi, vks[a]); }
            if (nused < 256) { used[nused][0] = vks[a]; used[nused][1] = mods[a]; nused++; }
        }
        {
            imap_state_t st;
            imap_block_t b;
            int keys[8], nk = 0;
            unsigned long long want = 1ull << bi, got;
            imap_state_init(&st);
            for (a = 0; a < 1; a++) {
                keys[nk++] = vks[0];
                if (mods[0] & 1) { keys[nk++] = VK_SHIFT; keys[nk++] = VK_LSHIFT; }
            }
            w365_apply(&p, &b, &st, keys, nk, 0, 1000u + (unsigned)bi);
            got = (unsigned long long)b.buttons[0] | ((unsigned long long)b.buttons[1] << 16) |
                  ((unsigned long long)b.buttons[2] << 32);
            if (!(got & want)) { nopress++; printf("  bi%d wirkt nicht (btn=%llX)\n", bi, got); }
        }
    }
    CHECK(unbound == 0, "w431: alle 39 Tasten belegt (unbelegt=%d)", unbound);
    CHECK(nopress == 0, "w431: jede Taste setzt ihr Bit (Fehler=%d)", nopress);
    CHECK(dup == 0, "w431: keine Taste doppelt (%d)", dup);
    CHECK(bad_reserved == 0, "w431: keine Client-Taste belegt (%d)", bad_reserved);
    CHECK(p.gear_up.n && p.gear_down.n && p.tuner_up.n && p.tuner_down.n, "w431: Gang/Tuner hoch+runter");
    for (a = 0; a < IMAP_AX_COUNT; a++) {
        int has = p.axis[a].neg.n || p.axis[a].pos.n || p.axis[a].mouse;
        if (a == IMAP_AX_PEDAL_L || a == IMAP_AX_PEDAL_M || a == IMAP_AX_PEDAL_R) has = p.axis[a].pos.n || p.axis[a].neg.n;
        CHECK(has, "w431: Achse %d belegt", a);
    }
    CHECK(p.vr_recenter.n && p.pause.n && p.photo.n && p.feedback_shot.n && p.feedback_clip.n && p.quick_shot.n &&
          p.orig_view.n && p.replay_exit.n && p.net_hud.n && p.aim_center.n && p.menu_back.n,
          "w431: Client-Aktionen belegt");
    {   /* Client-Aktionen nicht mit Spiel-Tasten teilen (ausser den bekannten Client-Tasten selbst).
         * W438: einzige erlaubte Ausnahme replay_exit = START (bi6, BACKSPACE): START wirkt im Spiel-Replay nicht
         * (W299), replay_exit nur waehrend der Wiedergabe -> die START-Taste beendet das Replay. */
        const imap_bind_t *ca[] = { &p.aim_center, &p.pause, &p.replay_exit, &p.net_hud, &p.quick_shot, &p.photo,
                                    &p.feedback_shot, &p.feedback_clip, &p.orig_view, &p.vr_recenter, &p.menu_back };
        int cd = 0, cc = 0, i, j, svk[8], smod[8], sn = imap_bind_key_combos(&p.btn[6], svk, smod, 8);
        for (i = 0; i < (int)(sizeof ca / sizeof ca[0]); i++) {
            int vks[8], mods[8], m = imap_bind_key_combos(ca[i], vks, mods, 8);
            for (a = 0; a < m; a++) {
                int is_start = 0;
                for (j = 0; j < sn; j++)
                    if (svk[j] == vks[a] && smod[j] == mods[a]) is_start = 1;
                if (ca[i] == &p.replay_exit && is_start) continue; /* W438: erlaubt (s. oben) */
                for (k = 0; k < nused; k++)
                    if (used[k][0] == vks[a] && used[k][1] == mods[a]) { cd++; printf("  Client-Aktion %d: VK %d\n", i, vks[a]); }
                for (k = 0; k < (int)(sizeof reserved / sizeof reserved[0]); k++)
                    if (vks[a] == reserved[k] && mods[a] == 0 && vks[a] == VK_RETURN) cc++; /* Enter = Chat */
            }
            for (j = i + 1; j < (int)(sizeof ca / sizeof ca[0]); j++) { /* Client-Aktionen untereinander */
                int v2[8], m2[8], n2 = imap_bind_key_combos(ca[j], v2, m2, 8), x, y;
                for (x = 0; x < m; x++)
                    for (y = 0; y < n2; y++)
                        if (vks[x] == v2[y] && mods[x] == m2[y]) { cd++; printf("  Client-Aktionen %d/%d: VK %d\n", i, j, vks[x]); }
            }
        }
        CHECK(cd == 0, "w431: Client-Aktionen kollidieren nicht mit Spiel-Tasten (%d)", cd);
        CHECK(cc == 0, "w438: ENTER bleibt frei (Chat), auch fuer Client-Aktionen (%d)", cc);
        CHECK(sn == 1 && svk[0] == VK_BACK && smod[0] == 0, "w438: START (bi6) = BACKSPACE");
        CHECK(p.menu_back.n == 1 && !p.menu_back.alt[0].chord && p.menu_back.alt[0].a.code == VK_INSERT,
              "w438: menu_back (LoC-Pad B) = INSERT");
    }
}

/* Teil 33: W438 CTRL-RESET-040 - LoC-Menue-Pad folgt dem Profil (START = bi6, A = bi0, B = menu_back). */
static int w438_keys[8], w438_nkeys;
static int w438_key(int vk, void *ctx)
{
    int i;
    (void)ctx;
    for (i = 0; i < w438_nkeys; i++)
        if (w438_keys[i] == vk) return 1;
    return 0;
}
static unsigned w438_bits(const imap_profile_t *p, int k0, int k1)
{
    w438_nkeys = 0;
    if (k0) w438_keys[w438_nkeys++] = k0;
    if (k1) w438_keys[w438_nkeys++] = k1;
    return imap_loc_pad_bits(p, w438_key, NULL);
}
/* Teil 35: W439 LOC-RETICLE-JUMP (#78) - Ausreisser-Filter analoger Achsen (input_despike, Schalter ctrl_despike).
 * Synthetische Report-Folgen (keine Eingaben aufgezeichnet): einzelne Ausreisser verworfen, normale und schnelle echte
 * Bewegung unverzoegert bzw. hoechstens ein Report verzoegert, Haltezeit, Neutralstand, Joystick-Slots (Fake-DirectInput). */
static void run_w439_despike_cases(void)
{
    despike_t d;
    int v[8], o[8], k, ok, maxlag, bad;
    uint64_t t = 1000;
    uint32_t rnd = 12345u;
    printf("Teil 35: W439 Ausreisser-Filter analoger Achsen\n");
    despike_init(&d, 2, DESPIKE_THR_U8, DESPIKE_HOLD_MS);
    v[0] = 128; v[1] = -1;
    despike_feed(&d, v, t, o);
    CHECK(o[0] == 128 && o[1] == -1, "w439: erster Report direkt");
    /* einzelner Ausreisser (Poti) auf Achse 0 bzw. 1 -> nie ausgegeben */
    v[0] = 255; despike_feed(&d, v, t += 8, o);
    ok = o[0] == 128;
    v[0] = 130; despike_feed(&d, v, t += 8, o);
    ok = ok && o[0] == 130 && d.spikes == 1u;
    CHECK(ok, "w439: Einzel-Ausreisser 128 -> 255 -> 130 verworfen (out=%d spikes=%u)", o[0], d.spikes);
    v[1] = -128; despike_feed(&d, v, t += 8, o);
    ok = o[1] == -1;
    v[1] = 2; despike_feed(&d, v, t += 8, o);
    CHECK(ok && o[1] == 2 && d.spikes == 2u, "w439: Ausreisser auf vorzeichenbehafteter Achse verworfen");
    /* kleine Schritte (<= Schwelle) gehen ohne Verzoegerung durch, bitgleich */
    ok = 1;
    for (k = 0; k < 40; k++) { v[0] = 130 + k * 3; despike_feed(&d, v, t += 8, o); ok = ok && o[0] == v[0]; }
    CHECK(ok, "w439: normale Bewegung (Schritt 3) unverzoegert");
    /* echte schnelle Bewegung: Sprung, naechster Report bestaetigt -> genau ein Report Verzoegerung */
    despike_reset(&d, (v[0] = 128, v[1] = 0, v));
    v[0] = 200; despike_feed(&d, v, t += 8, o);
    ok = o[0] == 128;
    v[0] = 203; despike_feed(&d, v, t += 8, o);
    CHECK(ok && o[0] == 203, "w439: echter Sprung bestaetigt -> uebernommen (out=%d)", o[0]);
    /* Rampe mit grossen Schritten in eine Richtung: weiter in dieselbe Richtung -> uebernehmen */
    despike_reset(&d, (v[0] = 0, v[1] = 0, v));
    v[0] = 60; despike_feed(&d, v, t += 8, o);
    k = o[0];
    v[0] = 120; despike_feed(&d, v, t += 8, o);
    CHECK(k == 0 && o[0] == 120, "w439: Rampe 0/60/120 -> 120 nach einem Report (out=%d)", o[0]);
    /* Haltezeit: Sprung ohne weiteren Report (SBC sendet nur bei Aenderung) gilt nach hold_ms als echt */
    v[0] = 250; despike_feed(&d, v, t += 8, o);
    ok = o[0] == 120;
    despike_read(&d, t + DESPIKE_HOLD_MS - 1u, o);
    ok = ok && o[0] == 120;
    despike_read(&d, t + DESPIKE_HOLD_MS, o);
    CHECK(ok && o[0] == 250, "w439: ohne Folge-Report nach %u ms uebernommen (out=%d)", DESPIKE_HOLD_MS, o[0]);
    t += DESPIKE_HOLD_MS;
    /* Neutralstand (Trennung): sofort, ohne Kandidat */
    v[0] = 128; v[1] = 0; despike_reset(&d, v);
    v[0] = 128; despike_feed(&d, v, t += 8, o);
    CHECK(o[0] == 128 && !d.has_cand[0], "w439: Neutralstand sofort");
    /* Poti-Rauschen +-6 mit seltenen Ausreissern (0 bzw. 255), langsame Bewegung: Ausgabe bleibt nahe am Signal,
     * kein Ausreisser durch, Verzoegerung hoechstens 1 Report */
    despike_init(&d, 1, DESPIKE_THR_U8, DESPIKE_HOLD_MS);
    bad = 0; maxlag = 0;
    for (k = 0; k < 4000; k++) {
        int sig = 128 + (int)(90.0 * sin(k * 0.01)), x;
        rnd = rnd * 1664525u + 1013904223u;
        x = sig + (int)((rnd >> 8) % 13u) - 6;
        if ((rnd >> 20) % 97u == 0u) { x = ((rnd >> 4) & 1u) ? 255 : 0; }
        v[0] = x;
        despike_feed(&d, v, t += 8, o);
        if (abs(o[0] - sig) > DESPIKE_THR_U8 + 6) bad++; /* Ausreisser naeher als die Schwelle am Signal gehen durch */
        if (abs(o[0] - sig) > maxlag) maxlag = abs(o[0] - sig);
    }
    CHECK(bad == 0 && d.spikes >= 30u, "w439: Rauschen + Ausreisser: Abweichung max %d (Grenze Schwelle+Rauschen %d), verworfen %u",
          maxlag, DESPIKE_THR_U8 + 6, d.spikes);
    /* Zusatz-Latenz (Messung fuer den Bericht): Hebel-Bewegungen ohne Ausreisser, Report alle 4 ms (SBC: nur bei
     * Aenderung). Gezaehlt werden Reports, deren Ausgabe vom Eingang abweicht (= verzoegert), in ms. */
    {
        static const int dur_ms[5] = { 400, 200, 100, 40, 20 }; /* volle Auslenkung 0 -> 255 in ... ms */
        int pi;
        for (pi = 0; pi < 5; pi++) {
            int n = dur_ms[pi] / 4, late = 0, worst = 0, run = 0;
            despike_init(&d, 1, DESPIKE_THR_U8, DESPIKE_HOLD_MS);
            v[0] = 0; despike_feed(&d, v, t += 4, o);
            for (k = 1; k <= n + 5; k++) {
                v[0] = k >= n ? 255 : (255 * k) / n;
                despike_feed(&d, v, t += 4, o);
                if (o[0] != v[0]) { late++; run++; if (run > worst) worst = run; } else { run = 0; }
            }
            printf("  w439 Latenz: 0->255 in %3d ms (Schritt %3d): verzoegerte Reports %d, laengste Verzoegerung %d ms\n",
                   dur_ms[pi], 255 / (n ? n : 1), late, worst * 4);
            CHECK(worst <= 1 && (255 / (n ? n : 1) > DESPIKE_THR_U8 || late == 0),
                  "w439: Latenz %d ms-Bewegung: hoechstens 1 Report (4 ms), unter der Schwelle 0 (late=%d worst=%d)",
                  dur_ms[pi], late, worst);
        }
    }
    /* Joystick-Slots (Fake-DirectInput): aus = Original (Ausreisser kommt durch), an = verworfen */
    {
        char specs[IMAP_JOY_MAX][96];
        imap_joy_state_t js[IMAP_JOY_MAX];
        int seen_off = 0, seen_on = 0, r;
        memset(specs, 0, sizeof specs);
        for (r = 0; r < 2; r++) {
            int *seen = r ? &seen_on : &seen_off;
            ijoy_set_despike(r);
            ijoy_test_begin(1, (const char (*)[96])specs, 1u);
            ijoy_test_axis(0, 1000);
            ijoy_test_run(40);
            ijoy_test_axis(0, 30000); /* ein Takt (4 ms) Ausreisser */
            ijoy_test_run(4);
            ijoy_slots_get(js);
            if (js[0].axis[0] == 30000) (*seen)++;
            ijoy_test_axis(0, 1200);
            ijoy_test_run(4);
            ijoy_slots_get(js);
            if (js[0].axis[0] == 30000) (*seen)++;
            ijoy_test_run(40);
            ijoy_slots_get(js);
            if (js[0].axis[0] != 1200) (*seen) += 10;
            ijoy_test_end();
        }
        ijoy_set_despike(0);
        CHECK(seen_off == 1 && seen_on == 0 && ijoy_despike_spikes() >= 1u,
              "w439: Joystick aus = Original (Ausreisser sichtbar %d), an = verworfen (%d, Zaehler %u)", seen_off, seen_on,
              ijoy_despike_spikes());
    }
}

static void run_w438_loc_pad_cases(void)
{
    imap_profile_t full, kb, p;
    int rc;
    printf("Teil 33: W438 LoC-Menue-Pad aus dem Profil\n");
    /* Legacy (kein Profil, Testketten): unveraendert Enter = START, SPACE = A, Backspace = B */
    CHECK(w438_bits(NULL, VK_RETURN, 0) == IMAP_LPAD_START, "w438: Legacy Enter = START");
    CHECK(w438_bits(NULL, VK_SPACE, 0) == IMAP_LPAD_A, "w438: Legacy SPACE = A");
    CHECK(w438_bits(NULL, VK_BACK, 0) == IMAP_LPAD_B, "w438: Legacy Backspace = B");
    CHECK(imap_loc_pad_bits(NULL, NULL, NULL) == 0, "w438: ohne Tastenfunktion nichts");
    rc = imap_profile_load("default_full", &full);
    CHECK(rc == 0, "w438: default_full geladen (rc=%d)", rc);
    if (rc == 0) {
        CHECK(w438_bits(&full, VK_RETURN, 0) == 0, "w438: default_full Enter loest im Pad nichts aus");
        CHECK(w438_bits(&full, VK_BACK, 0) == IMAP_LPAD_START, "w438: default_full Backspace = START (nicht B)");
        CHECK(w438_bits(&full, VK_SPACE, 0) == IMAP_LPAD_A, "w438: default_full SPACE = A");
        CHECK(w438_bits(&full, VK_INSERT, 0) == IMAP_LPAD_B, "w438: default_full Insert = B");
        CHECK(w438_bits(&full, 'F', VK_LBUTTON) == 0, "w438: andere Tasten/Maus kein Pad-Knopf");
    }
    rc = imap_profile_load("keyboard_default", &kb);
    CHECK(rc >= 0, "w438: keyboard_default geladen (rc=%d)", rc);
    if (rc >= 0) { /* Standard-Belegung (START = Enter): menu_back fehlt -> wie bisher Backspace */
        CHECK(kb.menu_back_set == 0 && w438_bits(&kb, VK_BACK, 0) == IMAP_LPAD_B &&
              w438_bits(&kb, VK_RETURN, 0) == IMAP_LPAD_START, "w438: keyboard_default unveraendert (Enter/Backspace)");
    }
    /* Profil ohne menu_back-Zeile, START auf Backspace -> B weicht auf Insert aus */
    CHECK(imap_profile_parse("bi6 = KEY:BACKSPACE\nbi0 = KEY:SPACE\n", "t", &p) == 0 && p.menu_back_set == 0 &&
          w438_bits(&p, VK_INSERT, 0) == IMAP_LPAD_B && w438_bits(&p, VK_BACK, 0) == IMAP_LPAD_START,
          "w438: ohne Zeile + START=Backspace -> B = Insert");
    /* Umbelegt: START = Shift+P, B = leer (aus), A = Strg */
    CHECK(imap_profile_parse("bi6 = KEY:SHIFT+KEY:P\nbi0 = KEY:LCTRL\nmenu_back =\n", "t2", &p) == 0 &&
          p.menu_back_set == 1 && p.menu_back.n == 0, "w438: menu_back leer = aus");
    CHECK(w438_bits(&p, 'P', 0) == 0 && w438_bits(&p, 'P', VK_SHIFT) == IMAP_LPAD_START &&
          w438_bits(&p, VK_LCONTROL, 0) == IMAP_LPAD_A && w438_bits(&p, VK_BACK, 0) == 0 &&
          w438_bits(&p, VK_RETURN, 0) == 0, "w438: umbelegte Tasten (Shift+P / Strg)");
    CHECK(imap_profile_parse("menu_back = KEY:DECIMAL, KEY:B\n", "t3", &p) == 0 && w438_bits(&p, 'B', 0) == IMAP_LPAD_B,
          "w438: menu_back mehrere Tasten");
}

/* Teil 34: W437 LOC-CHAT - Chat-Taste im Profil und neutrale Eingabe bei offener Chatzeile. */
static int w437_chat_combo(const imap_profile_t *p, int *vk, int *md)
{
    int v[4], m[4], n = imap_bind_key_combos(&p->chat, v, m, 4);
    if (n > 0) {
        *vk = v[0];
        *md = m[0];
    }
    return n;
}
/* frames Polls mit gedrueckten Spieltasten, Maustaste/-bewegung (synthetisch) und Mausrad; block = Chatzeile offen.
 * Vergleich mit derselben Folge ganz ohne Eingabe. Rueckgabe: 1 = alle Bloecke gleich der Folge ohne Eingabe. */
static int w437_neutral_run(const imap_profile_t *prof, int block, unsigned *hash)
{
    static const cfgvar_t cv = {1, 1, 0, 1, 4, 0, 0};
    static const int keys[] = {'W', 'S', 'A', 'D', VK_SPACE, 'F', 'R', 'E', 'Q', VK_LCONTROL, VK_SHIFT, VK_DELETE,
                               VK_BACK, VK_UP, VK_LEFT, '1', '3', 320, 321, 322, 324, 325};
    imap_state_t sa, sb;
    imap_block_t ba, bb;
    imap_snap_t s, e;
    int f, same = 1;
    unsigned h = 2166136261u;
    imap_state_init(&sa);
    imap_state_init(&sb);
    for (f = 0; f < 90; f++) {
        size_t i;
        make_snap(&s, 5000u + (unsigned)f, &cv);
        s.wheel_mask = 1u;
        s.pad_present = 1;
        s.pad.buttons = 0xFFFF;
        s.pad.lx = 30000;
        s.pad.rt = 255;
        if (block) imap_snap_block_ui(&s);
        memset(&ba, 0, sizeof ba);
        set_keys(keys, (int)(sizeof keys / sizeof keys[0]));
        if (prof) imap_profile_apply(&ba, &sa, prof, &s);
        else imap_legacy_apply(&ba, &sa, &s);
        make_snap(&e, 5000u + (unsigned)f, &cv);
        memset(&bb, 0, sizeof bb);
        set_keys(NULL, 0);
        if (prof) imap_profile_apply(&bb, &sb, prof, &e);
        else imap_legacy_apply(&bb, &sb, &e);
        same &= memcmp(&ba, &bb, sizeof ba) == 0;
        for (i = 0; i < sizeof ba; i++) h = (h ^ ((const uint8_t *)&ba)[i]) * 16777619u;
    }
    set_keys(NULL, 0);
    if (hash) *hash = h;
    return same;
}
static void run_w437_chat_cases(void)
{
    imap_profile_t full, kb, p;
    int rc, vk = 0, md = -1;
    unsigned h1 = 0, h2 = 0;
    printf("Teil 34: W437 Chat-Taste und neutrale Eingabe bei offener Chatzeile\n");
    rc = imap_profile_load("default_full", &full);
    CHECK(rc == 0, "w437: default_full geladen (rc=%d)", rc);
    if (rc == 0) {
        CHECK(full.chat_set == 1 && w437_chat_combo(&full, &vk, &md) == 1 && vk == VK_RETURN && md == 0,
              "w437: default_full chat = Enter");
        CHECK(w437_neutral_run(&full, 1, &h1) && w437_neutral_run(&full, 1, &h2) && h1 == h2,
              "w437: default_full - offene Chatzeile = keine Eingabe (90 Polls, Tasten/Maus/Rad/Pad), deterministisch");
        CHECK(!w437_neutral_run(&full, 0, NULL), "w437: Gegenprobe ohne Chatzeile - dieselbe Eingabe wirkt");
    }
    CHECK(w437_neutral_run(NULL, 1, NULL) && !w437_neutral_run(NULL, 0, NULL),
          "w437: Legacy - offene Chatzeile = keine Eingabe, Gegenprobe wirkt");
    rc = imap_profile_load("keyboard_default", &kb);
    CHECK(rc >= 0 && kb.chat_set == 0 && kb.chat.n == 0,
          "w437: keyboard_default (Enter = START) ohne Zeile -> keine Chat-Taste (kein Konflikt)");
    CHECK(imap_profile_parse("bi0 = KEY:SPACE\nbi6 = KEY:BACKSPACE\n", "t", &p) == 0 && p.chat_set == 0 &&
          w437_chat_combo(&p, &vk, &md) == 1 && vk == VK_RETURN && md == 0, "w437: ohne Zeile, Enter frei -> Enter");
    CHECK(imap_profile_parse("bi6 = KEY:SHIFT+KEY:ENTER\n", "t2", &p) == 0 && w437_chat_combo(&p, &vk, &md) == 1 &&
          vk == VK_RETURN, "w437: Umschalt+Enter als START -> Enter allein bleibt Chat");
    CHECK(imap_profile_parse("chat =\n", "t3", &p) == 0 && p.chat_set == 1 && p.chat.n == 0, "w437: chat leer = aus");
    CHECK(imap_profile_parse("chat = KEY:T\n", "t4", &p) == 0 && w437_chat_combo(&p, &vk, &md) == 1 && vk == 'T' &&
          md == 0, "w437: chat umbelegt (T)");
    CHECK(imap_profile_parse("chat = KEY:CTRL+KEY:ENTER\n", "t5", &p) == 0 && w437_chat_combo(&p, &vk, &md) == 1 &&
          vk == VK_RETURN && md == 2, "w437: chat mit Zusatztaste (Strg+Enter)");
}

/* Teil W441 (LOC-AUTOGEAR): Gang-Automatik mit dem LoC-Gangmodell. VT-Modell nach der Messung W441: Mittel je Gang
 * 22/36/55/85/168 Einheiten je Tick (R -30), Schrittzyklus als Welligkeit (Gang 1 Periode 45, 2-4 Periode 25, Gang 5
 * gleichmaessig), Bremse haelt sofort an, ohne Pedal -1 je Tick. Eingaben nur synthetisch. */
typedef struct {
    float vm, x, z, hy;
    unsigned t;
} w441_vt_t;

static float w441_vt_tick(w441_vt_t *m, const imap_block_t *b)
{
    static const float tgt[14] = { 0, 0, 0, 0, 0, 0, 0, -30.f, 0, 22.f, 36.f, 55.f, 85.f, 168.f };
    int gas = b->pedal_r >= 0x40, brk = b->pedal_m >= 0x40, g = b->gear;
    float d, rip = 0.0f;
    if (brk) {
        m->vm = 0.0f;
    } else if (gas && g != 8 && g >= 7 && g <= 13) {
        if (g == 13) m->vm += m->vm < tgt[g] ? 1.2f : 0.0f;
        else m->vm += (tgt[g] - m->vm) / 20.0f;
    } else {
        m->vm = m->vm > 1.0f ? m->vm - 1.0f : (m->vm < -1.0f ? m->vm + 1.0f : 0.0f);
    }
    if (g == 9) rip = 0.9f * sinf(6.2831853f * (float)m->t / 45.0f);
    else if (g >= 10 && g <= 12) rip = 0.6f * sinf(6.2831853f * (float)m->t / 25.0f);
    d = m->vm * (1.0f + rip);
    if (m->vm > 0.0f && d < 0.0f) d = 0.0f;
    if (m->vm < 0.0f && d > 0.0f) d = 0.0f;
    m->x += d * sinf(m->hy);
    m->z += d * cosf(m->hy);
    m->t++;
    return d;
}

/* ein Tick wie loc_autogear_poll: Lage -> Verlauf -> v -> Schaltschritt -> VT-Modell. Rueckgabe Gang. */
static int w441_tick(imap_autogear_t *ag, const imap_autogear_cfg_t *c, imap_agvel_t *vl, w441_vt_t *m, uint8_t *gst,
                     int gas, int brk, unsigned *step, imap_block_t *out)
{
    imap_block_t b;
    int g;
    blk_neutral(&b);
    b.gear = *gst;
    b.pedal_r = gas ? 0xFF : 0;
    b.pedal_m = brk ? 0xFF : 0;
    (*step)++;
    imap_agvel_push(vl, m->x, m->z, m->hy, *step, LOC_AG_JUMP);
    {
        imap_autogear_cfg_t ce = *c; /* wie loc_autogear_poll: Hochschalten erst am Plateau */
        if (!imap_agvel_loc_plateau(vl, ag->init ? *step - ag->shift_frame : 0u)) ce.v_min = 1e30f;
        g = imap_autogear_step(ag, &ce, &b, gst, imap_agvel_loc_v(vl, &b, c->pedal_on), *step);
    }
    w441_vt_tick(m, &b);
    if (out) *out = b;
    return g;
}

static void run_w441_loc_autogear_cases(void)
{
    imap_autogear_t ag;
    imap_autogear_cfg_t c;
    imap_agvel_t vl;
    w441_vt_t m;
    imap_block_t b;
    uint8_t gst = 8;
    unsigned step = 100, i, t_stop = 0, t_rev = 0, h1 = 2166136261u, h2 = 2166136261u;
    int g, prev = 8, ups = 0, downs = 0, shifts1, run;
    printf("Teil W441: LoC-Gang-Automatik (Schaltlogik, Geschwindigkeit, Determinismus)\n");
    imap_autogear_defaults_loc(&c);
    CHECK(c.min_hold == 45 && c.win == 30 && c.rev_delay == 6 && c.v_min == LOC_AG_VMIN && c.v_stop == LOC_AG_VSTOP &&
          c.pedal_on == 0x40 && c.down_pct == 70.0f, "w441: LoC-Parameter (Haltezeit 2,25 s, Plateau 1,5 s, R nach 0,3 s)");

    /* Geschwindigkeit: Vorzeichen, gleicher Schritt, Sprung, Luecke */
    memset(&vl, 0, sizeof vl);
    imap_agvel_push(&vl, 0.0f, 0.0f, 0.0f, 1, LOC_AG_JUMP);
    for (i = 2; i <= 11; i++) imap_agvel_push(&vl, 0.0f, 10.0f * (float)(i - 1), 0.0f, i, LOC_AG_JUMP);
    CHECK(vl.n == 10 && fabsf(imap_agvel_avg(&vl, 10) - 10.0f) < 1e-3f && fabsf(imap_agvel_avg(&vl, 40) - 2.5f) < 1e-3f,
          "w441: vorwaerts 10/Tick, vor Beginn Stillstand (v10=%.3f v40=%.3f n=%u)", imap_agvel_avg(&vl, 10),
          imap_agvel_avg(&vl, 40), vl.n);
    imap_agvel_push(&vl, 0.0f, 999.0f, 0.0f, 11, LOC_AG_JUMP);
    CHECK(vl.n == 10, "w441: gleicher Schritt zweimal ohne Wirkung");
    imap_agvel_push(&vl, 0.0f, 80.0f, 0.0f, 12, LOC_AG_JUMP);
    CHECK(vl.n == 11 && imap_agvel_avg(&vl, 1) < -19.9f, "w441: rueckwaerts negativ (%.2f)", imap_agvel_avg(&vl, 1));
    imap_agvel_push(&vl, 0.0f, 60.0f, 3.14159265f, 14, LOC_AG_JUMP); /* Luecke 2, Blick gedreht: vorwaerts */
    CHECK(vl.n == 13 && fabsf(imap_agvel_avg(&vl, 2) - 10.0f) < 1e-3f, "w441: Luecke aufgefuellt (%.2f)",
          imap_agvel_avg(&vl, 2));
    imap_agvel_push(&vl, 50000.0f, 60.0f, 0.0f, 15, LOC_AG_JUMP);
    CHECK(vl.n == 0 && imap_agvel_avg(&vl, 40) == 0.0f, "w441: Sprung (Eject/Neustart) -> Verlauf neu");
    imap_agvel_push(&vl, 50000.0f, 60.0f, 0.0f, 60, LOC_AG_JUMP);
    CHECK(vl.n == 0, "w441: Luecke > 30 -> Verlauf neu");
    blk_neutral(&b);
    b.pedal_m = 0xFF;
    imap_agvel_push(&vl, 50000.0f, 70.0f, 0.0f, 61, LOC_AG_JUMP);
    for (i = 62; i < 120; i++) /* 30/Tick bis 110, danach steht der VT (Bremse) */
        imap_agvel_push(&vl, 50000.0f, 70.0f + 30.0f * (float)(i < 110 ? i - 61 : 49), 0.0f, i, LOC_AG_JUMP);
    CHECK(imap_agvel_loc_v(&vl, &b, 0x40) == 0.0f && imap_agvel_avg(&vl, LOC_AG_NLONG) > 5.0f,
          "w441: Bremse -> kurzes Mittel (0) statt Fahr-Mittel (%.1f)", imap_agvel_avg(&vl, LOC_AG_NLONG));
    b.gear = 7;
    CHECK(imap_agvel_loc_v(&vl, &b, 0x40) > 5.0f, "w441: in R ist die Bremse das Gas -> Fahr-Mittel");

    /* Zweimal dieselbe synthetische Folge: Anfahren bis 5, ausrollen, bremsen -> R, rueckwaerts, Gas -> 1 */
    for (run = 0; run < 2; run++) {
        memset(&ag, 0, sizeof ag);
        memset(&vl, 0, sizeof vl);
        memset(&m, 0, sizeof m);
        m.hy = 0.7f;
        gst = 8;
        step = 100;
        prev = 8;
        ups = downs = 0;
        t_stop = t_rev = 0;
        for (i = 0; i < 1600; i++) { /* 80 s Gas */
            g = w441_tick(&ag, &c, &vl, &m, &gst, 1, 0, &step, &b);
            if (g > prev) ups++;
            if (g < prev) downs++;
            prev = g;
            if (run == 0) h1 = (h1 ^ (unsigned)b.gear ^ ((unsigned)b.pedal_r << 8) ^ ((unsigned)b.pedal_m << 16)) * 16777619u;
            else h2 = (h2 ^ (unsigned)b.gear ^ ((unsigned)b.pedal_r << 8) ^ ((unsigned)b.pedal_m << 16)) * 16777619u;
        }
        if (run == 0) {
            CHECK(gst == 13, "w441: Gas lang -> 5. Gang trotz Schrittzyklus (gst=%u)", gst);
            CHECK(ups == 5 && downs == 0, "w441: 5x hoch, 0x runter, kein Pendeln (hoch=%d runter=%d)", ups, downs);
            CHECK(ag.vtop[9] > 19.0f && ag.vtop[9] < 26.0f && ag.vtop[10] > 31.0f && ag.vtop[11] > 48.0f &&
                      ag.vtop[12] > 75.0f, "w441: hoch erst nahe der Gang-Endgeschwindigkeit (1:%.1f 2:%.1f 3:%.1f 4:%.1f)",
                  ag.vtop[9], ag.vtop[10], ag.vtop[11], ag.vtop[12]);
            CHECK(ag.vtop[9] > 15.0f && ag.vtop[9] < 30.0f && ag.vtop[12] > 55.0f,
                  "w441: Endgeschw. gelernt (1:%.1f 4:%.1f)", ag.vtop[9], ag.vtop[12]);
        }
        shifts1 = ag.shifts;
        for (i = 0; i < 100; i++) { /* ausrollen ohne Pedal: kein Wechsel ausser runter */
            g = w441_tick(&ag, &c, &vl, &m, &gst, 0, 0, &step, &b);
            if (run == 0) h1 = (h1 ^ (unsigned)b.gear) * 16777619u;
            else h2 = (h2 ^ (unsigned)b.gear) * 16777619u;
        }
        for (i = 0; i < 80; i++) { /* bremsen bis Stillstand, gehalten -> R */
            g = w441_tick(&ag, &c, &vl, &m, &gst, 0, 1, &step, &b);
            if (!t_stop && m.vm == 0.0f) t_stop = i;
            if (!t_rev && g == 7) t_rev = i;
            if (run == 0) h1 = (h1 ^ (unsigned)b.gear ^ ((unsigned)b.pedal_r << 8)) * 16777619u;
            else h2 = (h2 ^ (unsigned)b.gear ^ ((unsigned)b.pedal_r << 8)) * 16777619u;
        }
        if (run == 0) {
            CHECK(ag.shifts >= shifts1 && gst == 7, "w441: Bremse im Stand -> R (gst=%u)", gst);
            CHECK(t_rev > t_stop && t_rev - t_stop <= 16, "w441: R nach %u Ticks Stillstand (<= 0,8 s)", t_rev - t_stop);
            CHECK(b.gear == 7 && b.pedal_r == 0xFF && b.pedal_m == 0, "w441: in R Bremse = Gas (r=%u m=%u)", b.pedal_r,
                  b.pedal_m);
            CHECK(m.vm < -5.0f, "w441: faehrt rueckwaerts (vm=%.1f)", m.vm);
        }
        for (i = 0; i < 40; i++) { /* Gas in R: Spiel sieht Bremse -> Stillstand -> 1. Gang */
            g = w441_tick(&ag, &c, &vl, &m, &gst, 1, 0, &step, &b);
            if (run == 0) h1 = (h1 ^ (unsigned)b.gear ^ ((unsigned)b.pedal_m << 16)) * 16777619u;
            else h2 = (h2 ^ (unsigned)b.gear ^ ((unsigned)b.pedal_m << 16)) * 16777619u;
        }
        if (run == 0) CHECK(gst == 9, "w441: Gas in R -> anhalten -> 1. Gang (gst=%u)", gst);
    }
    CHECK(h1 == h2, "w441: gleiche Eingabefolge -> gleiche Bloecke (deterministisch, %08X/%08X)", h1, h2);

    /* Gang 2 gehalten: Tal im Schrittzyklus ist kein Stillstand (kein Ruecksprung in den 1. Gang) */
    memset(&ag, 0, sizeof ag);
    memset(&vl, 0, sizeof vl);
    memset(&m, 0, sizeof m);
    gst = 10;
    step = 5000;
    m.vm = 36.0f;
    downs = 0;
    prev = 10;
    c.v_min = 1e9f; /* Hochschalten aus: nur Ruecksprung pruefen */
    for (i = 0; i < 400; i++) {
        g = w441_tick(&ag, &c, &vl, &m, &gst, 1, 0, &step, NULL);
        if (g < prev) downs++;
        prev = g;
    }
    CHECK(downs == 0 && gst == 10, "w441: Gang 2 mit Schrittzyklus bleibt (runter=%d gst=%u)", downs, gst);
    imap_autogear_defaults_loc(&c);
}

int main(int argc, char **argv)
{
    imap_profile_t kb_file, kb_in, xbox_file, xbox_in;
    int rc_kb, rc_xbox;

    if (argc > 1 && strcmp(argv[1], "--dump") == 0) {
        imap_profile_t p;
        const char *nm = argc > 2 ? argv[2] : "keyboard_default";
        int rc = imap_profile_load(nm, &p);

        printf("Quelle: %s\n",
               rc == 0 ? "Datei" : rc == 1 ? "eingebaut" : "FEHLT");
        if (rc >= 0) dump_profile(&p);
        return rc >= 0 ? 0 : 1;
    }

    { extern int imouse_selftest(void); CHECK(imouse_selftest() == 0, "W196 mouse/menu"); }
    { extern int sb_rauto_selftest(void); CHECK(sb_rauto_selftest() == 0, "W254 Replay-Start"); }
    { extern int sb_mauto_selftest(void); CHECK(sb_mauto_selftest() == 0, "W272 Teststart"); }
    { extern int sb_cchain_selftest(void); CHECK(sb_cchain_selftest() == 0, "W303 Kampagnen-Kette"); }
    { extern int avol_selftest(void); CHECK(avol_selftest() == 0, "W303 Lautstaerke"); }
    { /* W424 SB-ALLIED-SQUADS: Freigabe und Zusatzregel (reine Logik) */
        static const sb_ally_pt_t al[2] = { { 100, 50 }, { -20, 300 } };
        static const unsigned char tst[4] = { 1, 2, 3, 4 };
        static unsigned seen[8];
        CHECK(sb_ally_extra(0, 1, 1, 0, 0, al, 0, SB_ALLY_RANGE), "W426 verbuendet + gesehen -> aktiv");
        CHECK(!sb_ally_extra(0, 1, 0, 0, 0, al, 2, SB_ALLY_RANGE), "W426 verbuendet, nie gesehen -> eingefroren (Original)");
        CHECK(!sb_ally_extra(1, 1, 1, 100, 50, al, 2, SB_ALLY_RANGE), "W424 Trupp-Mitglied folgt dem Anfuehrer");
        CHECK(sb_ally_extra(0, 0, 0, 107, 43, al, 2, SB_ALLY_RANGE), "W424 Feind am Rand (+7/-7) aktiv");
        CHECK(!sb_ally_extra(0, 0, 0, 108, 50, al, 2, SB_ALLY_RANGE), "W424 Feind +8 bleibt eingefroren");
        CHECK(sb_ally_extra(0, 0, 0, -25, 295, al, 2, SB_ALLY_RANGE), "W424 Feind nahe zweitem Verbuendeten");
        CHECK(!sb_ally_extra(0, 0, 0, 100, 50, al, 0, SB_ALLY_RANGE), "W426 ohne gesehene Verbuendete nichts");
        /* W426 Merker "gesehen": Bild 0 zaehlt nicht, bleibt danach, Tod/Neubelegung loescht */
        CHECK(!sb_ally_mark(seen, 8, 3, sb_ally_key(17), 0) && !sb_ally_seen(seen, 8, 3, sb_ally_key(17)),
              "W426 Bild 0 merkt nicht");
        CHECK(sb_ally_mark(seen, 8, 3, sb_ally_key(17), 5) && sb_ally_seen(seen, 8, 3, sb_ally_key(17)),
              "W426 Original aktiv -> gesehen");
        CHECK(!sb_ally_mark(seen, 8, 3, sb_ally_key(17), 6), "W426 nur einmal neu gemerkt");
        CHECK(sb_ally_keep(seen, 8, 3, sb_ally_key(17), 1) && sb_ally_seen(seen, 8, 3, sb_ally_key(17)),
              "W426 bleibt gesehen, wenn der Spieler weiterzieht");
        CHECK(!sb_ally_keep(seen, 8, 3, sb_ally_key(40), 1) && !sb_ally_seen(seen, 8, 3, sb_ally_key(40)) &&
              seen[3] == 0, "W426 Slot neu belegt -> nicht gesehen");
        CHECK(sb_ally_mark(seen, 8, 4, sb_ally_key(0), 9) && !sb_ally_keep(seen, 8, 4, sb_ally_key(0), 0) &&
              seen[4] == 0, "W426 Tod loescht Merker");
        CHECK(!sb_ally_mark(seen, 8, 8, sb_ally_key(1), 9) && !sb_ally_seen(seen, 8, -1, sb_ally_key(1)),
              "W426 Index ausserhalb");
        CHECK(sb_ally_key(0) != 0 && sb_ally_key(0xFFFF) != 0, "W426 Schluessel nie 0");
        CHECK(sb_ally_fnv64(tst, 0) == 14695981039346656037ull && sb_ally_fnv64(tst, 4) != sb_ally_fnv64(tst, 3),
              "W424 fnv64");
    }

    printf("input_selftest: Vergleich alt<->neu + Gamepad-Logik\n");

    /* Teil 1: Datei == eingebaut (Datei muss gefunden werden). */
    rc_kb = imap_profile_load("keyboard_default", &kb_file);
    rc_xbox = imap_profile_load("xbox_pad_default", &xbox_file);
    printf("Teil 1: keyboard_default Quelle=%s, xbox_pad_default Quelle=%s\n",
           rc_kb == 0 ? "Datei" : rc_kb == 1 ? "EINGEBAUT (Datei fehlt!)"
                                             : "FEHLT",
           rc_xbox == 0 ? "Datei" : rc_xbox == 1 ? "EINGEBAUT (Datei fehlt!)"
                                                 : "FEHLT");
    CHECK(rc_kb == 0, "keyboard_default.ini gefunden");
    CHECK(rc_xbox == 0, "xbox_pad_default.ini gefunden");
    CHECK(imap_profile_builtin("keyboard_default", &kb_in) == 0,
          "builtin keyboard ok");
    CHECK(imap_profile_builtin("xbox_pad_default", &xbox_in) == 0,
          "builtin xbox ok");
    if (rc_kb == 0)
        CHECK(imap_profile_equal(&kb_file, &kb_in) == 0,
              "Datei == eingebaut (keyboard_default)");
    if (rc_xbox == 0)
        CHECK(imap_profile_equal(&xbox_file, &xbox_in) == 0,
              "Datei == eingebaut (xbox_pad_default)");
    CHECK(kb_in.has_pad == 0, "keyboard_default ohne Pad");
    CHECK(xbox_in.has_pad == 1, "xbox_pad_default mit Pad");

    /* Teil 2: bitgenau-Vergleich (mit Datei-Profil, falls da). */
    printf("Teil 2: Vergleich alt<->neu ...\n");
    run_compare(rc_kb == 0 ? &kb_file : &kb_in);
    printf("Teil 2: %ld Zustaende, %ld Abweichungen\n", n_states,
           n_mismatch);
    CHECK(n_mismatch == 0, "0 Abweichungen alt<->neu");

    /* Teil 3: Gamepad-Logik. */
    run_padcases(rc_xbox == 0 ? &xbox_file : &xbox_in);

    /* Teil 4: W79 INPUT-REAL. */
    run_realcases(rc_xbox == 0 ? &xbox_file : &xbox_in);

    /* Teil 6: W92 INPUT-TAPS. */
    run_tapcases();

    /* Teil 7: W110 PEDAL-L. */
    run_pedal_cases(rc_kb == 0 ? &kb_file : &kb_in,
                    rc_xbox == 0 ? &xbox_file : &xbox_in);

    /* Teil 8: W128 Arbitrierung SBC gegen Maus. */
    run_arb_cases();

    /* Teil 9: W132 Arbitrierung Pad gegen Maus. */
    run_padarb_cases(rc_xbox == 0 ? &xbox_file : &xbox_in);

    /* Teil 10: W141 CTRL-CAL2. */
    run_sbccal_cases();

    /* Teil 11: W143 globale Pad-Totzonen (Untergrenze). */
    run_paddz_cases(rc_xbox == 0 ? &xbox_file : &xbox_in);

    /* Teil 12: W159 Aktion vr_recenter + SBC-Quellen. */
    run_vraction_cases(rc_kb == 0 ? &kb_file : &kb_in,
                       rc_xbox == 0 ? &xbox_file : &xbox_in);

    /* Teil 13: W188 Aktion pause. */
    run_pause_cases(rc_kb == 0 ? &kb_file : &kb_in, rc_xbox == 0 ? &xbox_file : &xbox_in);
    printf("Teil 14: W193 menu_peek_key\n");
    CHECK(imap_key_name("H") == 'H' && imap_key_name("h") == 'H', "peek: H");
    CHECK(imap_key_name("F9") == VK_F9 && imap_key_name("pause") == VK_PAUSE, "peek: Namen");
    CHECK(imap_key_name("0x48") == 0x48, "peek: Hex");
    CHECK(imap_key_name("XYZ") == -1 && imap_key_name("") == -1 && imap_key_name(NULL) == -1, "peek: unbekannt");

    run_photo_cases(rc_kb == 0 ? &kb_file : &kb_in, rc_xbox == 0 ? &xbox_file : &xbox_in); /* Teil 15: W195 */
    run_feedback_shot_cases(rc_kb == 0 ? &kb_file : &kb_in, rc_xbox == 0 ? &xbox_file : &xbox_in);
    run_feedback_clip_cases(rc_kb == 0 ? &kb_file : &kb_in, rc_xbox == 0 ? &xbox_file : &xbox_in);

    run_simple_cases(rc_kb == 0 ? &kb_file : &kb_in); /* Teil 16: W218 */
    run_joy_cases(rc_kb == 0 ? &kb_file : &kb_in, rc_xbox == 0 ? &xbox_file : &xbox_in); /* Teil 17: W221 */
    run_wheel_cases(rc_kb == 0 ? &kb_file : &kb_in, rc_xbox == 0 ? &xbox_file : &xbox_in); /* Teil 18: W260 */
    run_stickrel_cases(rc_xbox == 0 ? &xbox_file : &xbox_in); /* Teil 19: W271 */
    run_origview_cases(rc_kb == 0 ? &kb_file : &kb_in, rc_xbox == 0 ? &xbox_file : &xbox_in); /* Teil 20: W283 */
    run_quickshot_cases(rc_kb == 0 ? &kb_file : &kb_in, rc_xbox == 0 ? &xbox_file : &xbox_in); /* Teil 21: W290 */
    run_joyhotplug_cases(); /* Teil 22: W305 */
    run_replayexit_cases(rc_kb == 0 ? &kb_file : &kb_in, rc_xbox == 0 ? &xbox_file : &xbox_in); /* Teil 23: W299 */
    run_joycalib_cases(); /* Teil 24: W312 */
    run_w365_combo_cases(); /* Teil 25: W365 */
    run_nethud_cases(rc_kb == 0 ? &kb_file : &kb_in, rc_xbox == 0 ? &xbox_file : &xbox_in); /* Teil 26: W374 */
    run_slide_assist_cases(rc_kb == 0 ? &kb_file : &kb_in); /* Teil 27: W380 */
    run_w394_preview_cases(); /* Teil 28: W394 */
    run_w396_center_cases(); /* Teil 29: W396 */
    run_w403_eject_cases(rc_kb == 0 ? &kb_file : &kb_in, rc_xbox == 0 ? &xbox_file : &xbox_in); /* Teil 30: W403 */

    run_w431_default_full_cases(); /* Teil 31: W431 */
    run_w436_eject_chain(rc_kb == 0 ? &kb_file : &kb_in, rc_xbox == 0 ? &xbox_file : &xbox_in); /* Teil 32: W436 */
    run_w438_loc_pad_cases(); /* Teil 33: W438 */
    run_w437_chat_cases(); /* Teil 34: W437 */
    run_w439_despike_cases(); /* Teil 35: W439 */
    run_w441_loc_autogear_cases(); /* Teil 36: W441 */

    printf("selftest: %d checks, %d failed\n", n_checks, n_fail);
    return n_fail ? 1 : 0;
}
