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
 * Teil 16 (W218 SIMPLE-CONTROLS): Kopplung Zielhebel -> Drehhebel und
 *   Gang-Automatik gegen ein einfaches Fahrzeugmodell.
 * Teil 15 (W195 PHOTO-MODE): Aktion photo (Standard KEY:F12, Tasten nur ueber
 *         das Fenster, Pad-Flanke im Hook, imap_bind_keys, Profilvergleich).
 */
#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "input_map.h"
#include "input_tap.h"
#include "input_sbc_usb.h"

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
    VK_LCONTROL, VK_RCONTROL, VK_CONTROL /* W110: Slide-Step (nur links) */
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
    CHECK(kb->photo.n == 1 && kb->photo.alt[0].a.kind == IMAP_A_KEY && kb->photo.alt[0].a.code == VK_F12,
          "photo: kb KEY:F12");
    CHECK(xbox->photo.n == 1 && xbox->photo.alt[0].a.code == VK_F12, "photo: xbox KEY:F12");
    n = imap_bind_keys(&kb->photo, vks, 4, &pm);
    CHECK(n == 1 && vks[0] == VK_F12 && pm == 0, "photo: bind_keys kb (n=%d)", n);
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

/* ---- Teil 16: W218 SIMPLE-CONTROLS (Kopplung Ziel->Drehhebel, Automatik) ---- */
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

    printf("Teil 16: W218 SIMPLE-CONTROLS\n");
    /* -- Kopplung Zielhebel -> Drehhebel -- */
    blk_neutral(&b);
    CHECK(imap_aim_turn(&b, 60, 100) == 0 && b.rot_lever == 0, "aim_turn: Mitte 0x7F -> 0");
    b.aiming_x = 0x80;
    CHECK(imap_aim_turn(&b, 60, 100) == 0 && b.rot_lever == 0, "aim_turn: Mitte 0x80 -> 0");
    b.aiming_x = 0x80 + 76; /* 59,8 % */
    CHECK(imap_aim_turn(&b, 60, 100) == 0 && b.rot_lever == 0, "aim_turn: unter Schwelle -> 0");
    b.aiming_x = 0xFF;
    CHECK(imap_aim_turn(&b, 60, 100) == 127 && b.rot_lever == 127, "aim_turn: Anschlag rechts -> +127");
    blk_neutral(&b);
    b.aiming_x = 0x00;
    CHECK(imap_aim_turn(&b, 60, 100) == -127 && b.rot_lever == -127, "aim_turn: Anschlag links -> -127");
    blk_neutral(&b);
    b.aiming_x = 0x80 + 102; /* 80,3 % -> (0,803-0,6)/0,4 = 0,51 */
    r = imap_aim_turn(&b, 60, 100);
    CHECK(r >= 60 && r <= 70 && b.rot_lever == r, "aim_turn: anteilig (r=%d)", r);
    blk_neutral(&b);
    b.aiming_x = 0xFF;
    CHECK(imap_aim_turn(&b, 60, 50) == 64, "aim_turn: Staerke 50 %% -> 64");
    blk_neutral(&b);
    b.aiming_x = 0xFF;
    CHECK(imap_aim_turn(&b, 60, 0) == 0 && b.rot_lever == 0, "aim_turn: Staerke 0 -> aus");
    blk_neutral(&b);
    b.aiming_x = 0xFF;
    b.rot_lever = -127; /* A gehalten: gleich starke Auslenkung bleibt */
    CHECK(imap_aim_turn(&b, 60, 100) == 127 && b.rot_lever == -127, "aim_turn: A/D gewinnt bei Gleichstand");
    blk_neutral(&b);
    b.aiming_x = 0xFF;
    b.rot_lever = 30;
    CHECK(imap_aim_turn(&b, 60, 100) == 127 && b.rot_lever == 127, "aim_turn: Kopplung staerker als Stick");
    blk_neutral(&b);
    b.aiming_x = 0xFF;
    CHECK(imap_aim_turn(&b, 0, 100) == 127, "aim_turn: Schwelle 0");
    blk_neutral(&b);
    b.aiming_x = 0x80 + 64;
    CHECK(imap_aim_turn(&b, 99, 100) == 0, "aim_turn: Schwelle geklemmt 95 %%");
    { /* Tastatur RIGHT (keyboard_default) -> Zielhebel 0xFF -> Drehhebel +127 */
        imap_state_t st;
        imap_snap_t sn;
        cfgvar_t cv = { 1, 1, 0, 1, 4, 0, 0 };
        int k = VK_RIGHT;
        imap_state_init(&st);
        set_keys(&k, 1);
        make_snap(&sn, 5, &cv);
        blk_neutral(&b);
        imap_profile_apply(&b, &st, kb, &sn);
        r = imap_aim_turn(&b, 60, 100);
        CHECK(b.aiming_x == 0xFF && r == 127 && b.rot_lever == 127,
              "aim_turn: RIGHT -> aim %02X rot %d", b.aiming_x, (int)b.rot_lever);
        set_keys(NULL, 0);
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

    printf("selftest: %d checks, %d failed\n", n_checks, n_fail);
    return n_fail ? 1 : 0;
}
