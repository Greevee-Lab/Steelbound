/* hooks_input.c - XDK-Hooks: Eingabe (SBC-Layout, Tastatur/Replay-Slots,
 * Maus, POLLCENSUS, XInputOpen/Close/SetState/GetState, XGetDeviceChanges).
 * W68 SPLIT: 1:1 aus xdk_hooks.c verschoben. */
#include "hooks_int.h"
#include "sbc_wire.h" /* W75 CTRL-SBC: Original-Controller (sbc_usb=1) */
#include "input_map.h" /* W76 CTRL-MAP: Legacy-Abbildung + Profile */
#include "input_mouse.h" /* W79 INPUT-REAL: Maus fangen + Fokus-Gatter */
#include "sb_overlay.h" /* W118 REM-OVERLAY: Eingabesperre */
#include "vr.h" /* W159: sb_vr_recenter("Belegung") per Aktions-Bindung */

static uint8_t g_fake_input[512];
unsigned g_last_pstate;
uint8_t g_last_sbc[24]; /* OUTWRITE: letzter SBC-Block (f. Present-Hook) */
static uint32_t g_packet;

/* W76: Eingabe-Zustand (Gang/Tuner/Toggles/Flanken/Maus) je Pfad.
 * g_leg = Legacy (input_profile leer), g_prof_st = Profil-Pfad. */
static imap_state_t g_leg;
static int g_leg_init;
static imap_state_t g_prof_st;
static imap_profile_t g_prof;
static int g_prof_init, g_prof_load_rc, g_pad_logged;

/* Layout-Gleichheit imap_block_t <-> sbc_in_t (Hook castet). */
_Static_assert(sizeof(imap_block_t) == sizeof(sbc_in_t), "imap_block 24");
_Static_assert((unsigned)(uintptr_t)&((imap_block_t *)0)->aiming_x == 7,
               "imap aiming_x");
_Static_assert((unsigned)(uintptr_t)&((imap_block_t *)0)->gear == 23,
               "imap gear");

/* Aktiver Gang (f. sbc_merge-Init + DIAG): Profil- oder Legacy-Pfad. */
static uint8_t active_gear(void)
{
    if (g_cfg.input_profile[0] && g_prof_init && g_prof_load_rc >= 0)
        return g_prof_st.gear;
    return g_leg.gear;
}

static int hook_key_down(int vk, void *ctx)
{
    (void)ctx;
    return key_down(vk);
}

/* W59 POLLCENSUS (DIAG, [log] poll_census=1): Laufzeit-Zensus aller
 * FUN_00048CF0-Aufrufe (port, gruppe, button) je Ruecksprungadresse.
 * Hook = cdecl-Ersatz (jmp, daher _ReturnAddress() = Spiel-Aufrufer), ruft
 * das Original per Trampolin (9 Originalbytes "mov ax,[esp+4]; cmp ax,1" +
 * jmp 0x48CF9) und zaehlt Aufrufe + Rueckgaben != 0. Log: "PCENS NEW" bei
 * Erstsicht, "PCENS NZ" je Eintrag mit neuen Treffern (!=0) alle
 * ~1000 Swaps. Klaert, wer Feuer/Lock/Aim im Missionscode wirklich abfragt
 * (statische push-imm-Scans verfehlen Register-Argumente). */
#define PCENS_MAX 1024
typedef struct {
    u32 ra;
    u16 port, grp, btn, used;
    u32 calls, nz, nz_logged;
} pcens_t;
static pcens_t g_pcens[PCENS_MAX];
static u32 g_pcens_n, g_pcens_over;
static u32 (__cdecl *g_tramp_48cf0)(u32, u32, u32);
static unsigned g_pcens_last_dump;

static u32 __cdecl hook_48CF0_census(u32 port, u32 grp, u32 btn)
{
    u32 r = g_tramp_48cf0(port, grp, btn);
    u32 ra = (u32)(uintptr_t)_ReturnAddress();
    u16 p16 = (u16)port, g16 = (u16)grp, b16 = (u16)btn;
    u32 h = ((ra * 2654435761u) ^ (b16 * 40503u) ^ (g16 << 7) ^ p16) & (PCENS_MAX - 1);
    u32 i;
    for (i = 0; i < PCENS_MAX; i++) {
        pcens_t *e = &g_pcens[(h + i) & (PCENS_MAX - 1)];
        if (!e->used) {
            if (g_pcens_n >= PCENS_MAX * 3 / 4) {
                g_pcens_over++;
                return r;
            }
            e->used = 1;
            e->ra = ra;
            e->port = p16;
            e->grp = g16;
            e->btn = b16;
            g_pcens_n++;
            HLOG_I("PCENS NEW ra=0x%X port=%u grp=%u btn=%u (slot%u rest%u) f=%u r=0x%X",
                   ra, p16, g16, b16, b16 / 20u, b16 % 20u, g_frames, r & 0xFFFFu);
        }
        if (e->ra == ra && e->port == p16 && e->grp == g16 && e->btn == b16) {
            e->calls++;
            if (r & 0xFFFFu) {
                e->nz++;
            }
            break;
        }
    }
    if (g_frames - g_pcens_last_dump >= 1000u) {
        g_pcens_last_dump = g_frames;
        for (i = 0; i < PCENS_MAX; i++) {
            pcens_t *e = &g_pcens[i];
            if (e->used && e->nz != e->nz_logged) {
                HLOG_I("PCENS NZ ra=0x%X port=%u grp=%u btn=%u calls=%u nz=%u (+%u) f=%u",
                       e->ra, e->port, e->grp, e->btn, e->calls, e->nz,
                       e->nz - e->nz_logged, g_frames);
                e->nz_logged = e->nz;
            }
        }
        if (g_pcens_over) {
            HLOG_W("PCENS voll: %u Aufrufe ohne Eintrag", g_pcens_over);
        }
    }
    return r;
}

void poll_census_install(void)
{
    static const uint8_t orig[9] = {0x66, 0x8B, 0x44, 0x24, 0x04, 0x66, 0x3D, 0x01, 0x00};
    uint8_t *t;
    intptr_t rel;
    if (memcmp((void *)(uintptr_t)0x48CF0u, orig, 9) != 0) {
        HLOG_E("PCENS: 0x48CF0 Byte-Mismatch - Zensus aus");
        return;
    }
    t = (uint8_t *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) {
        HLOG_E("PCENS: VirtualAlloc fehlgeschlagen - Zensus aus");
        return;
    }
    memcpy(t, orig, 9);
    t[9] = 0xE9;
    rel = (intptr_t)0x48CF9 - (intptr_t)(t + 14);
    memcpy(t + 10, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), t, 14);
    g_tramp_48cf0 = (u32 (__cdecl *)(u32, u32, u32))(void *)t;
    patch_jmp(0x48CF0u, hook_48CF0_census, "48CF0-PollCensus(DIAG)");
}

/* XInputOpen (0x256784, 4 Args, RET 0x10): Fake-Handle (lesbar, Nullen). */
u32 __stdcall hook_XInputOpen(u32 type, u32 port, u32 flags, u32 poll)
{
    static u32 calls;
    (void)flags;
    (void)poll;
    if (hlog_throttle(&calls)) {
        HLOG_I("XIN Open(type=0x%X port=%u) -> fake", type, port);
    }
    SetLastError(0);
    return (u32)(uintptr_t)g_fake_input;
}

/* XInputClose (0x2567DA, 1 Arg, RET 4): No-Op. */
u32 __stdcall hook_XInputClose(u32 h)
{
    (void)h;
    return 0;
}

/* XInputSetState (0x256A32, 2 Args, RET 8): Rumble/LEDs ignorieren. */
u32 __stdcall hook_XInputSetState(u32 h, u32 fb)
{
    (void)h;
    if (g_cfg.sbc_usb) {
        sbc_wire_setstate(fb); /* W75: LEDs (sbc_usb=0: No-Op wie bisher) */
    }
    return 0;
}

/* XGetDeviceChanges (0x25649E, 3 Args, RET 0xC): Slot 0 meldet angesteckt.
 * Hinweis: "type" ist ein Zeiger auf XDEVICE_PREALLOC_TYPE (z.B. SBC-Typ
 * bei 0x2557A4), kein 0x80-Byte — deshalb typunabhaengig melden (das Spiel
 * nutzt nur den SBC). */
u32 __stdcall hook_XGetDeviceChanges(u32 type, u32 pIns, u32 pRem)
{
    static u32 calls;
    if (pIns) {
        *(u32 *)(uintptr_t)pIns |= 1u;
    }
    if (pRem) {
        *(u32 *)(uintptr_t)pRem = 0;
    }
    if (hlog_throttle(&calls)) {
        HLOG_T("XIN GetDeviceChanges(type=0x%X) -> ins=1", type);
    }
    return 0;
}

/* W75 CTRL-SBC: Original-Controller als Zweitquelle (CONTROLLER.md §9).
 * sbc_usb=0: ein int-Vergleich, Tastatur-Pfad bitgenau wie bisher.
 * sbc_usb=1: Buttons (inkl. Toggles bi34-38 als Pegel, Schritt 3) werden
 * geODERt (Tastatur/Maus/Replay bleiben parallel nutzbar); Analogachsen
 * uebernimmt USB nur, wenn Tastatur/Maus sie neutral liessen (explizite
 * Auslenkung gewinnt, USB-Ruhe 0x80/0x00 verstellt nichts); Gang 0 haelt
 * den letzten Gang (Schritt 4, Init = Tastatur-Gang); Tuner 13-15 faellt
 * auf Tastatur zurueck. Bots (aimat/drive) laufen danach und behalten auf
 * ihren Achsen das letzte Wort (Replay-/Bot-Tests unveraendert). */
/* W128: 1 = Maus liefert in diesem Poll Achsen (0 = per sbc_mouse_off
 * abgeschaltet, weil der Original-Controller aktiv ist). */
static int g_sbc_mouse_live = 1;
static uint8_t g_sbc_gear = 8; /* letzter USB-Gang != 0 */
static int g_sbc_gear_init;

static void sbc_merge(sbc_in_t *s)
{
    sbc_state_t u;
    if (!g_cfg.sbc_usb) {
        return;
    }
    if (!sbc_wire_poll(&u)) {
        return; /* kein Geraet/Fixture: Tastatur pur */
    }
    s->buttons[0] |= u.buttons[0];
    s->buttons[1] |= u.buttons[1];
    s->buttons[2] |= (uint16_t)(u.buttons[2] & 0x007Fu); /* bi32-38, Rest 0 */
    if (g_cfg.sbc_axis_arb) {
        /* W128: "zuletzt aktives Geraet gewinnt" je Achse (input_map.h,
         * imap_arb_axes). Die Maus liefert auf ihren Achsen jeden Poll eine
         * Stellung (absolut: Vorsteuerung) - die alte Regel "USB nur bei
         * neutraler Achse" liess den Zielhebel nie durch (Nutzer 5.11.). */
        static imap_arb_t arb;
        imap_arb_in_t in;
        int out[IMAP_ARB_N];
        const imap_state_t *as = (g_cfg.input_profile[0] &&
                                  g_prof_load_rc >= 0) ? &g_prof_st : &g_leg;
        memset(&in, 0, sizeof in);
        in.cur[0] = s->aiming_x; in.cur[1] = s->aiming_y;
        in.cur[2] = s->sight_x;  in.cur[3] = s->sight_y;
        in.idle[0] = 0x7F; in.idle[1] = 0x7F; in.idle[2] = 0; in.idle[3] = 0;
        in.sbc[0] = u.aiming_x; in.sbc[1] = u.aiming_y;
        in.sbc[2] = u.sight_x;  in.sbc[3] = u.sight_y;
        in.sbc_center[0] = 0x80; in.sbc_center[1] = 0x80;
        in.sbc_center[2] = 0; in.sbc_center[3] = 0;
        if (g_sbc_mouse_live && g_cfg.mouse_aim) {
            if (!g_cfg.mouse_sight) {
                in.mouse_src = as->m_aim_src & 3u;
            } else {
                in.mouse_src = 0xCu; /* Sight per Maus (Pfeile: Naeherung) */
            }
        }
        in.mpos[0] = (float)as->m_ax + as->m_fx;
        in.mpos[1] = (float)as->m_ay + as->m_fy;
        in.mpos[2] = (float)as->m_sx;
        in.mpos[3] = (float)as->m_sy;
        in.dead = g_cfg.sbc_arb_dead;
        in.wake = (float)g_cfg.sbc_mouse_wake;
        imap_arb_axes(&arb, &in, out);
        s->aiming_x = (uint8_t)out[0];
        s->aiming_y = (uint8_t)out[1];
        s->sight_x = (int8_t)out[2];
        s->sight_y = (int8_t)out[3];
    } else {
        if (s->aiming_x == 0x7F && u.aiming_x != 0x80) {
            s->aiming_x = u.aiming_x;
        }
        if (s->aiming_y == 0x7F && u.aiming_y != 0x80) {
            s->aiming_y = u.aiming_y;
        }
        if (s->sight_x == 0 && u.sight_x != 0) {
            s->sight_x = u.sight_x;
        }
        if (s->sight_y == 0 && u.sight_y != 0) {
            s->sight_y = u.sight_y;
        }
    }
    if (s->rot_lever == 0 && u.rot_lever != 0) {
        s->rot_lever = u.rot_lever;
    }
    if (s->pedal_l == 0 && u.pedal_l != 0) {
        s->pedal_l = u.pedal_l;
    }
    if (s->pedal_m == 0 && u.pedal_m != 0) {
        s->pedal_m = u.pedal_m;
    }
    if (s->pedal_r == 0 && u.pedal_r != 0) {
        s->pedal_r = u.pedal_r;
    }
    if (!g_sbc_gear_init) {
        g_sbc_gear = active_gear();
        g_sbc_gear_init = 1;
    }
    if (u.gear != 0) {
        g_sbc_gear = u.gear;
    }
    s->gear = g_sbc_gear;
    if (u.tuner <= 12) {
        s->tuner = u.tuner;
    }
}

/* W87 DIAG(STARTSEQ) (input_diag=1): Startsequenz-Zustand bei jeder
 * Aenderung - Missionsschleife [0x356F58], Schalterphase-Stufe [0x51D8B4]
 * (SPIELZUSTAENDE 12.1: 3 = Schalter pollen), Schalter-Zustaende
 * [0x51D8B5..B9] (0 = nicht erkannt, 1..4 = erkannt/animiert), Zaehler
 * [0x51D92C], Minispiel-Status [0x55D168] (12.2), VT online [0x53831C]
 * und die Rastbits bi34-38 des gelieferten Blocks. Nur Lesen. */
extern unsigned g_shot_census; /* render_diag.c: Bild beim naechsten Present */
static void w87_seq_diag(const sbc_in_t *s)
{
    static uint8_t last[16];
    static int have;
    uint8_t cur[16];
    unsigned tog = ((unsigned)s->buttons[2] >> 2) & 0x1Fu;
    memset(cur, 0, sizeof cur);
    __try {
        cur[0] = (uint8_t)*(volatile u32 *)(uintptr_t)0x356F58u;
        cur[1] = *(volatile uint8_t *)(uintptr_t)0x51D8B4u;
        memcpy(cur + 2, (const void *)(uintptr_t)0x51D8B5u, 5);
        cur[7] = (uint8_t)*(volatile u32 *)(uintptr_t)0x51D92Cu;
        cur[8] = (uint8_t)*(volatile u32 *)(uintptr_t)0x55D168u;
        cur[9] = (uint8_t)*(volatile u32 *)(uintptr_t)0x53831Cu;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    cur[10] = (uint8_t)tog;
    if (g_cfg.startseq_shots && cur[0] == 1 && !cur[9]) {
        /* Belegbilder vor VT online: alle 300 Frames (eins pro Frame). */
        static unsigned shotf;
        if (g_frames % 300u == 0 && shotf != g_frames) {
            shotf = g_frames;
            g_shot_census = 1;
        }
    }
    if (have && memcmp(cur, last, sizeof cur) == 0) return;
    if (g_cfg.startseq_shots && have &&
        (cur[0] != last[0] || cur[1] != last[1] || cur[8] != last[8] ||
         cur[9] != last[9]))
        g_shot_census = 1; /* Bild zum Zustandswechsel (naechstes Present) */
    {
        float b[5] = { 0, 0, 0, 0, 0 };
        int i;
        __try {
            for (i = 0; i < 5; i++)
                b[i] = *(volatile float *)(uintptr_t)(0x55D19Cu + 4u * (u32)i);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        HLOG_I("STARTSEQ: f=%u mloop=%u stufe=%u sw=%u%u%u%u%u n=%u "
               "s168=%u online=%u raste=%u%u%u%u%u balken=%.2f/%.2f/%.2f/%.2f/%.2f",
               g_frames, cur[0], cur[1], cur[2], cur[3], cur[4], cur[5],
               cur[6], cur[7], cur[8], cur[9], tog & 1u, (tog >> 1) & 1u,
               (tog >> 2) & 1u, (tog >> 3) & 1u, (tog >> 4) & 1u,
               b[0], b[1], b[2], b[3], b[4]);
    }
    memcpy(last, cur, sizeof last);
    have = 1;
}

/* W87 DIAG(AIMSTEP) (aim_diag=1): Zielkette einmal pro Frame, solange der
 * Zielhebel sich in den letzten 120 Frames geaendert hat (nur Mission):
 * Hebel ax/ay -> Soll-Offset [0x356E1C]/[0x356D38] (FUN_00037290) ->
 * geglaettet [0x356D08]/[0x3521BC] (FUN_00034BE0, k=[0x260284]) ->
 * Fadenkreuz [0x358560/64]. Belegt Sprungantwort/Ueberschwingen. */
static void w87_aim_diag(const sbc_in_t *s)
{
    static unsigned lastf = 0xFFFFFFFFu, chgf;
    static uint8_t lax = 0x7F, lay = 0x7F;
    float t[2] = { 0, 0 }, m[2] = { 0, 0 }, c[2] = { 0, 0 };
    if (g_frames == lastf) return;
    lastf = g_frames;
    __try {
        if (*(volatile u32 *)(uintptr_t)0x356F58u != 1u) return;
        t[0] = *(volatile float *)(uintptr_t)0x356E1Cu;
        t[1] = *(volatile float *)(uintptr_t)0x356D38u;
        m[0] = *(volatile float *)(uintptr_t)0x356D08u;
        m[1] = *(volatile float *)(uintptr_t)0x3521BCu;
        c[0] = *(volatile float *)(uintptr_t)0x358560u;
        c[1] = *(volatile float *)(uintptr_t)0x358564u;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (s->aiming_x != lax || s->aiming_y != lay) {
        lax = s->aiming_x;
        lay = s->aiming_y;
        chgf = g_frames;
    }
    if (g_frames - chgf > 120u) return;
    HLOG_I("AIMSTEP: f=%u ax=%02X ay=%02X soll=%.4f,%.4f glatt=%.4f,%.4f "
           "kreuz=%.1f,%.1f", g_frames, s->aiming_x, s->aiming_y, t[0], t[1],
           m[0], m[1], c[0], c[1]);
}

static float w87_rdf(u32 a, float dflt)
{
    float f = dflt;
    __try { f = *(volatile float *)(uintptr_t)a; }
    __except (EXCEPTION_EXECUTE_HANDLER) { f = dflt; }
    if (!(f == f) || f > 1e6f || f < -1e6f) f = dflt;
    return f;
}

/* W87 mouse_mode=absolute: Das Spiel glaettet den Zielhebel selbst
 * (FUN_00034BE0: s += k*(t - s), k = [0x260284] = 0,0625/Frame, t = Soll-
 * Offset aus dem Hebel [0x356E1C]/[0x356D38], s = [0x356D08]/[0x3521BC]
 * bestimmt das Fadenkreuz). Der virtuelle Mauscursor (Akku m_ax+m_fx) gibt
 * das Ziel T vor; der Hebel wird so gesetzt, dass s im naechsten Frame T
 * erreicht: u = s + (T - s)/k (Vorsteuerung auf den gemessenen Glaettungs-
 * zustand, kein Integrator -> kein Windup, kein Ueberschwingen). Hebel ->
 * Offset gemessen (W87_rel, AIMSTEP): Stufen n = ax-0x80 (ax >= 0x80) bzw.
 * ax-0x7F (ax <= 0x7F; 0x7F/0x80 = 0, Totzone), t_x = n*kx*sx; Y: n =
 * 0x80-ay bzw. 0x7F-ay, t_y = n*ky*sy. Die Wirkung kommt im selben Frame
 * (Poll -> Mapper -> FUN_00037290 -> FUN_00034BE0), keine Zusatzverzoegerung.
 * Nur Missionsschleife und nur Achsen, die in diesem Poll von der Maus
 * kamen (Pfeiltasten/Pad behalten ihr Verhalten). Einmal je Frame
 * gerechnet; weitere Polls desselben Frames bekommen dieselben Werte. */
static void w87_aim_abs(sbc_in_t *s, const imap_state_t *st)
{
    static unsigned lastf = 0xFFFFFFFFu;
    static uint8_t oax = 0x7F, oay = 0x7F, osrc;
    static int logged;
    float k, sx, sy, kx, ky, smx, smy, vx, vy, tx, ty, ux, uy;
    long ax, ay;
    if (!(st->m_aim_src & 3u)) return;
    if (g_frames == lastf) {
        if (osrc & st->m_aim_src & 1u) s->aiming_x = oax;
        if (osrc & st->m_aim_src & 2u) s->aiming_y = oay;
        return;
    }
    lastf = g_frames;
    osrc = 0;
    __try {
        if (*(volatile u32 *)(uintptr_t)0x356F58u != 1u) return;
        /* nur mit VT online (nach START; Titel-Demo/Startsequenz bleiben
         * beim gelieferten Hebel) */
        if (*(volatile u32 *)(uintptr_t)0x53831Cu != 1u) return;
        sy = (*(volatile uint16_t *)(uintptr_t)0x356E20u) ?
             w87_rdf(0x260380u, -0.0061359f) : w87_rdf(0x26037Cu, 0.0061359f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    k = w87_rdf(0x260284u, 0.0625f);
    sx = w87_rdf(0x260384u, -0.015f);
    kx = w87_rdf(0x25FCD4u, 1.0f) / w87_rdf(0x354F90u, 1.0f);
    ky = w87_rdf(0x25FCD4u, 1.0f) / w87_rdf(0x347830u, 1.0f);
    if (!(k > 0.001f && k <= 1.0f)) return;
    if (!(kx > 0.05f && kx < 50.0f)) kx = 1.0f;
    if (!(ky > 0.05f && ky < 50.0f)) ky = 1.0f;
    if (fabsf(sx) < 1e-5f || fabsf(sy) < 1e-5f) return;
    smx = w87_rdf(0x356D08u, 0.0f);
    smy = w87_rdf(0x3521BCu, 0.0f);
    vx = (float)st->m_ax + st->m_fx;
    vy = (float)st->m_ay + st->m_fy;
    tx = (vx - 127.0f) * kx * sx;
    ty = (127.0f - vy) * ky * sy;
    ux = smx + (tx - smx) / k;
    uy = smy + (ty - smy) / k;
    {
        /* Umkehrung der Hebel-Kennlinie (Totzone 0x7F/0x80 = 0 Stufen). */
        long nx = lroundf(ux / (sx * kx)), ny = lroundf(uy / (sy * ky));
        ax = nx > 0 ? 0x80 + nx : 0x7F + nx;
        ay = ny > 0 ? 0x7F - ny : 0x80 - ny;
        if (ny == 0) ay = 0x7F;
    }
    if (ax < 0) ax = 0;
    if (ax > 0xFF) ax = 0xFF;
    if (ay < 0) ay = 0;
    if (ay > 0xFF) ay = 0xFF;
    oax = (uint8_t)ax;
    oay = (uint8_t)ay;
    osrc = st->m_aim_src;
    if (osrc & 1u) s->aiming_x = oax;
    if (osrc & 2u) s->aiming_y = oay;
    if (!logged) {
        logged = 1;
        HLOG_I("INPUT: Maus absolut aktiv (Vorsteuerung auf Spielglaettung "
               "k=%.4f, sx=%.5f sy=%.5f kx=%.3f ky=%.3f) bei f=%u",
               k, sx, sy, kx, ky, g_frames);
    }
}

/* W87 toggle_assist (nur key_toggles=1, Spielerbetrieb): Die Schalterphase
 * (Stufe [0x51D8B4]=3, FUN_000888E0) zaehlt nur Flanken AUS->AN (48CF0
 * Gruppe 6). Stehen Schalter beim Eintritt schon auf AN (z. B. 1-5 vor
 * Luke/Zuendung gedrueckt), setzt das Spiel sie auf Zustand 3 ("erst AUS",
 * W87_ri2: sw=33333) - der naechste Druck klappt nur AUS (3->0, im Bild
 * unsichtbar), erst der zweite zaehlt. Mit der Hilfe gilt ein Druck in der
 * Schalterphase auf einen eingerasteten, noch nicht gezaehlten Schalter
 * ([0x51D8B5+i] = 0 oder 3) als "neu umlegen": er bleibt AN, das Bit geht
 * 6 Frames auf AUS und dann wieder AN (Flanke) -> eine Zeile je Druck. */
static unsigned g_tog_pulse_end[5];
static void w87_toggle_assist(sbc_in_t *s, imap_state_t *ts, unsigned chg)
{
    static const char *const tn[5] = {
        "FILT CONTROL", "OXYGEN SUPPLY", "FUEL FLOW",
        "BUFFER MATERIAL", "VT-LOCATION" };
    uint8_t stage = 0, sw[5] = { 0, 0, 0, 0, 0 };
    int i;
    if (chg) {
        __try {
            stage = *(volatile uint8_t *)(uintptr_t)0x51D8B4u;
            memcpy(sw, (const void *)(uintptr_t)0x51D8B5u, 5);
            if (*(volatile u32 *)(uintptr_t)0x356F58u != 1u) stage = 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            stage = 0;
        }
    }
    for (i = 0; i < 5; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        if ((chg & bit) && stage == 3 && (sw[i] == 0 || sw[i] == 3) &&
            !(ts->tog_latch & bit)) {
            ts->tog_latch |= bit; /* bleibt eingerastet */
            g_tog_pulse_end[i] = g_frames + 6u;
            HLOG_I("INPUT: Kippschalter %d (bi%d %s) war schon AN -> neu "
                   "umgelegt (AUS 6 Frames, dann AN; Schalterphase sw=%u) "
                   "bei f=%u", i + 1, 34 + i, tn[i], sw[i], g_frames);
        }
        if (g_tog_pulse_end[i] && (int)(g_tog_pulse_end[i] - g_frames) > 0)
            s->buttons[2] &= (uint16_t)~(1u << (2 + i));
        else if (g_tog_pulse_end[i]) {
            g_tog_pulse_end[i] = 0;
            if (ts->tog_latch & bit) s->buttons[2] |= (uint16_t)(1u << (2 + i));
        }
    }
}

/* XInputGetState (0x2569C3, 2 Args, RET 8): Tastatur -> SBC-State, 0=OK. */
u32 __stdcall hook_XInputGetState(u32 h, u32 pState)
{
    sbc_in_t *s;
    static u32 calls;
    (void)h;
    g_act_xin++;
    if (!pState) {
        return 0x48Fu; /* ERROR_DEVICE_NOT_CONNECTED */
    }
    *(u32 *)(uintptr_t)pState = ++g_packet;
    g_last_pstate = pState;
    { /* XINPORTLOG: wer pollt welchen Port? */
        static unsigned nxp; unsigned *sp0 = NULL;
        __try { sp0 = (unsigned *)_AddressOfReturnAddress(); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        nxp++;
        if ((nxp & 0xFFF) == 1) {
            unsigned ra = 0;
            __try { ra = sp0 ? *sp0 : 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            { unsigned pt = (pState >= 0x4AEEF8u && pState < 0x4AEF80u) ? (pState - 0x4AEEF8u) / 0x1Cu : 99u;
              HLOG_W("XINPORT #%u: port=%u pState=0x%X RA=0x%X", nxp, pt, pState, ra); }
        }
    }
    /* PORTLOG-AUS (Mission-Port bekannt: rotiert 0-3!): nur noch alle 30k. */
    { static unsigned lastlog;
      if (g_frames - lastlog >= 30000u) {
          unsigned mp = 0xEEEEu;
          lastlog = g_frames;
          __try { mp = *(u32 *)(uintptr_t)0x352274u; }
          __except (EXCEPTION_EXECUTE_HANDLER) {}
          HLOG_W("PORTLOG: frame=%u mapper_port=%u", g_frames, mp);
      } }
    s = (sbc_in_t *)(uintptr_t)(pState + 4);
    { /* W76: Snapshot bauen, Legacy- oder Profil-Abbildung fahren. */
        imap_snap_t snap;
        memset(&snap, 0, sizeof snap);
        snap.key = hook_key_down;
        snap.ctx = NULL;
        snap.replay_active = input_replay_active();
        snap.frame = g_frames;
        snap.mouse_aim = g_cfg.mouse_aim;
        snap.mouse_sight = g_cfg.mouse_sight;
        snap.mouse_sens = g_cfg.mouse_sens;
        snap.mouse_invert_x = g_cfg.mouse_invert_x;
        snap.mouse_invert_y = g_cfg.mouse_invert_y;
        snap.mouse_buttons = g_cfg.mouse_buttons;
        snap.sbc_momentary = g_cfg.sbc_momentary;
        snap.mouse_abs = g_cfg.mouse_mode == 1; /* W87 */
        snap.pad_axis_arb = g_cfg.pad_axis_arb; /* W132 */
        snap.pad_arb_dead = g_cfg.pad_arb_dead;
        snap.pad_mouse_wake = g_cfg.pad_mouse_wake;
        snap.pad_dz_stick = g_cfg.pad_dz_stick / 100.0f; /* W143 */
        snap.pad_dz_trig = g_cfg.pad_dz_trig / 100.0f;
        /* W128 sbc_mouse_off: Original-Controller aktiv (Geraet offen bzw.
         * Fixture) -> Maus-Achsen aus, Maustasten nach sbc_mouse_buttons. */
        g_sbc_mouse_live = 1;
        if (g_cfg.sbc_usb && g_cfg.sbc_mouse_off) {
            int sst = sbc_wire_status();
            if (sst == SBC_WIRE_USB_OK || sst == SBC_WIRE_FIXTURE) {
                static int mo_logged;
                g_sbc_mouse_live = 0;
                snap.mouse_aim = 0;
                if (!g_cfg.sbc_mouse_buttons) {
                    snap.mouse_buttons = 0;
                }
                if (!mo_logged) {
                    mo_logged = 1;
                    HLOG_I("SBC: Maus-Achsen aus (sbc_mouse_off=1, Original-Controller "
                           "aktiv), Maustasten %s", g_cfg.sbc_mouse_buttons ? "an" : "aus");
                }
            }
        }
        { /* W79 INPUT-REAL: Spielerbetrieb-Hinweise (je einmal) + Maus. */
            static int w79_logged;
            if (!w79_logged) {
                w79_logged = 1;
                if (snap.replay_active)
                    HLOG_I("INPUT: Replay aktiv -> echte Tastatur/Maus "
                           "ignoriert (replay_steps=%s, replay=%s, "
                           "movie_skip=%d; Spielerbetrieb: Start ueber den "
                           "Launcher)",
                           g_cfg.replay_steps[0] ? "gesetzt" : "leer",
                           g_cfg.replay[0] ? "gesetzt" : "leer",
                           g_cfg.movie_skip);
                else
                    HLOG_I("INPUT: echte Tastatur/Maus aktiv "
                           "(input_source=%d mouse_capture=%d "
                           "input_focus_only=%d key_toggles=%d profil=%s)",
                           g_cfg.input_source, g_cfg.mouse_capture,
                           g_cfg.input_focus_only,
                           g_cfg.key_toggles,
                           g_cfg.input_profile[0] ? g_cfg.input_profile
                                                  : "Legacy");
            }
            if (!snap.replay_active) {
                snap.mouse_raw = imouse_poll(&snap.raw_dx, &snap.raw_dy,
                                             &snap.real_blocked);
                if (g_cfg.input_source == 1) snap.real_vk = imsg_real_vk;
                if (g_cfg.overlay && sb_ovl_open()) {
                    /* W118 REM-OVERLAY: Maus (Tasten + Deltas) gesperrt */
                    snap.real_blocked = 1;
                    snap.raw_dx = snap.raw_dy = 0;
                }
            } else if (g_cfg.input_source == 1) {
                /* Fenster-Eingabe auch bei Replay installieren (Nachrichten
                 * kommen an, Abbildung ignoriert sie - Gegenprobe/Hinweis). */
                int rdx, rdy, rbl;
                (void)imouse_poll(&rdx, &rdy, &rbl);
            }
        }
        if (!g_leg_init) {
            imap_state_init(&g_leg);
            g_leg_init = 1;
        }
        if (g_cfg.input_profile[0]) {
            if (!g_prof_init) {
                imap_state_init(&g_prof_st);
                g_prof_load_rc =
                    imap_profile_load(g_cfg.input_profile, &g_prof);
                g_prof_init = 1;
                if (g_prof_load_rc == 0)
                    HLOG_I("INPUT: Profil '%s' aus Datei",
                           g_cfg.input_profile);
                else if (g_prof_load_rc == 1)
                    HLOG_I("INPUT: Profil '%s' eingebaut (Datei fehlt)",
                           g_cfg.input_profile);
                else
                    HLOG_W("INPUT: Profil '%s' unbekannt -> Legacy",
                           g_cfg.input_profile);
                if (g_prof_load_rc >= 0 && g_cfg.overlay) {
                    /* W188 GAME-MENU: Tasten der Aktion "pause" ans
                     * Fenster (wirkt auch, waehrend das Spiel steht). */
                    int pvk[4];
                    unsigned pm = 0;
                    int pn = imap_bind_keys(&g_prof.pause, pvk, 4, &pm);
                    sb_ovl_set_pause_binding(pvk, pn, pm);
                    /* W195 PHOTO-MODE: Tasten der Aktion "photo" (Standard F12) */
                    pn = imap_bind_keys(&g_prof.photo, pvk, 4, &pm);
                    sb_ovl_set_photo_binding(pvk, pn, pm);
                }
            }
            if (g_prof_load_rc >= 0 && g_prof.has_pad &&
                g_cfg.pad_replay_gate && snap.replay_active) {
                /* W83: Replay aktiv -> echtes Pad ignorieren wie Tastatur/
                 * Maus (M2b): Tests duerfen nicht von einem angeschlossenen
                 * Pad abhaengen. */
                if (!g_pad_logged) {
                    g_pad_logged = 1;
                    HLOG_I("INPUT: gamepad ignoriert (Replay aktiv, pad_replay_gate=1)");
                }
            } else if (g_prof_load_rc >= 0 && g_prof.has_pad) {
                snap.pad_present = imap_pad_poll(g_frames, &snap.pad);
                if (g_cfg.overlay && sb_ovl_open())
                    snap.pad_present = 0; /* W118: Overlay offen -> Pad nicht ans Spiel */
                if (!g_pad_logged) {
                    g_pad_logged = 1;
                    HLOG_I("INPUT: gamepad %s",
                           snap.pad_present ? "aktiv (XInput)"
                                            : "inaktiv (kein XInput-Geraet)");
                }
            }
            if (g_prof_load_rc >= 0 && g_prof.has_sbc && g_cfg.sbc_usb) {
                /* W159: Original-Controller-Tasten als Belegungs-Quellen
                 * (SBC:<taste>): Cache lesen (nie blockierend), nur wenn
                 * das Profil sie nutzt. sbc_merge() liest spaeter denselben
                 * Stand als Zweitquelle ins Spiel. */
                sbc_state_t su;
                if (sbc_wire_poll(&su)) {
                    snap.sbc[0] = su.buttons[0];
                    snap.sbc[1] = su.buttons[1];
                    snap.sbc[2] = su.buttons[2];
                    snap.sbc_present = 1;
                }
            }
            if (g_cfg.pad_mouse_off && snap.pad_present) {
                /* W132 PAD-ARB: Gamepad aktiv -> Maus-Achsen aus (wie
                 * sbc_mouse_off); Maustasten bleiben immer an. */
                static int pm_logged;
                snap.mouse_aim = 0;
                if (!pm_logged) {
                    pm_logged = 1;
                    HLOG_I("INPUT: Maus-Achsen aus (pad_mouse_off=1, Gamepad "
                           "aktiv), Maustasten an");
                }
            }
            if (g_prof_load_rc >= 0) {
                imap_profile_apply((imap_block_t *)s, &g_prof_st, &g_prof,
                                   &snap);
                /* W159: Aktions-Bindung "vr_recenter" (Flanke) -> Blick
                 * zentrieren. Legacy-Pfad: nur die cfg-Taste (vr.c). */
                if (imap_action_vr_recenter(&g_prof, &g_prof_st, &snap)) {
                    sb_vr_recenter("Belegung");
                }
                /* W188 GAME-MENU: Aktion "pause" aus Pad/Controller (Flanke) */
                if (g_cfg.overlay && imap_action_pause(&g_prof, &g_prof_st, &snap)) {
                    sb_ovl_request_pause("Belegung");
                }
                /* W195 PHOTO-MODE: Aktion "photo" aus Pad/Controller (Flanke) */
                if (g_cfg.overlay && imap_action_photo(&g_prof, &g_prof_st, &snap)) {
                    sb_ovl_request_photo("Belegung");
                }
            } else
                imap_legacy_apply((imap_block_t *)s, &g_leg, &snap);
        } else {
            imap_legacy_apply((imap_block_t *)s, &g_leg, &snap);
        }
        if (g_cfg.key_toggles && !snap.replay_active) {
            /* W79: Kippschalter als Rastschalter (Tasten 1-5 / Profil). */
            static const char *const tn[5] = {
                "FILT CONTROL", "OXYGEN SUPPLY", "FUEL FLOW",
                "BUFFER MATERIAL", "VT-LOCATION" };
            imap_state_t *ts = (g_cfg.input_profile[0] &&
                                g_prof_load_rc >= 0) ? &g_prof_st : &g_leg;
            unsigned chg = imap_toggles_apply((imap_block_t *)s, ts, &snap);
            int i;
            for (i = 0; i < 5; i++) {
                if (chg & (1u << i))
                    HLOG_I("INPUT: Kippschalter %d (bi%d %s) %s bei f=%u",
                           i + 1, 34 + i, tn[i],
                           (ts->tog_latch & (1u << i)) ? "AN" : "AUS",
                           g_frames);
            }
            if (g_cfg.toggle_assist)
                w87_toggle_assist(s, ts, chg); /* W87 */
        }
        if (g_cfg.mouse_mode == 1 && g_cfg.mouse_aim && !g_cfg.mouse_sight &&
            g_sbc_mouse_live) {
            /* W87: absolute Maus (Vorsteuerung auf die Spielglaettung). */
            imap_state_t *as = (g_cfg.input_profile[0] &&
                                g_prof_load_rc >= 0) ? &g_prof_st : &g_leg;
            w87_aim_abs(s, as);
        }
    }
    sbc_merge(s); /* W75: USB/Fixture als Zweitquelle (sbc_usb=1) */
    if (g_aimat_idx && input_replay_active()) {
        aimat_apply(s); /* W59: Ziel-Bot (Testpfad, nur mit Replay) */
    }
    if (g_drive_idx && input_replay_active()) {
        drive_apply(s); /* W63: Fahr-Bot (Testpfad, nur mit Replay) */
    }
    if (hlog_throttle(&calls)) {
        HLOG_T("XIN GetState -> pkt=%u gear=%u", g_packet, active_gear());
    }
    /* DIAG(INPUT, A8): SBC-Block bei jeder Aenderung (Unit-Log fuer
     * Maus-Analog + Buttons; nur Deltas, kein Spam). */
    if (g_cfg.input_diag) {
        static uint8_t last[24];
        static int have_last;
        if (!have_last || memcmp(last, s, sizeof last) != 0) {
            memcpy(last, s, sizeof last);
            have_last = 1;
            HLOG_I("XIN sbc: f=%u btn=%04X%04X%04X aim=%02X%02X "
                   "sight=%d,%d rot=%d ped=%02X%02X%02X tun=%u gear=%u",
                   g_frames, s->buttons[0], s->buttons[1], s->buttons[2],
                   s->aiming_x, s->aiming_y,
                   (int)s->sight_x, (int)s->sight_y, (int)s->rot_lever,
                   s->pedal_l, s->pedal_m, s->pedal_r, s->tuner, s->gear);
        }
    }
    if (g_cfg.input_diag) w87_seq_diag(s);   /* W87 DIAG(STARTSEQ) */
    if (g_cfg.aim_diag) w87_aim_diag(s);     /* W87 DIAG(AIMSTEP) */
    /* OUTWRITE-Vorbereitung: letzten SBC-Block global merken (Present-Hook
     * schreibt ihn direkt nach Out (Mapper umgehen!). */
    __try { memcpy(g_last_sbc, s, sizeof g_last_sbc); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}
