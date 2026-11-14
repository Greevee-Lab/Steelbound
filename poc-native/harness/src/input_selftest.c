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
 * Teil 15 (W195 PHOTO-MODE): Aktion photo (Standard KEY:F12, Tasten nur ueber
 *         das Fenster, Pad-Flanke im Hook, imap_bind_keys, Profilvergleich).
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

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
    run_joy_cases(rc_kb == 0 ? &kb_file : &kb_in, rc_xbox == 0 ? &xbox_file : &xbox_in); /* Teil 17: W221 */

    printf("selftest: %d checks, %d failed\n", n_checks, n_fail);
    return n_fail ? 1 : 0;
}
