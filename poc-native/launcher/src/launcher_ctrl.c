/* launcher_ctrl.c - Steuerungs-Helfer des Launchers (W78, CTRL-UI).
 * ASCII only. Alle Nutzertexte deutsch. Nutzt das unveraenderte
 * Client-Modul input_sbc_usb.c (nur einkompiliert, nicht geaendert). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include "launcher_ctrl.h"
#include "launcher_cfg.h"
#include "launcher_u8.h"
#include "sb_lang.h"
#include "input_sbc_usb.h"

/* --- Eingabe-Profile -------------------------------------------------- */

int lctrl_input_profiles(const char *client_dir, char names[][64], int max)
{
    char dir[1024], pat[1100];
    u8_find_t fd;
    int n = 0, i, j;
    size_t dl;

    if (!client_dir || !*client_dir || max <= 0) {
        return 0;
    }
    strcpy_s(dir, sizeof dir, client_dir);
    dl = strlen(dir);
    if (dl > 0 && dir[dl - 1] != '/' && dir[dl - 1] != '\\') {
        strcat_s(dir, sizeof dir, "/");
    }
    strcat_s(dir, sizeof dir, "profiles");
    strcpy_s(pat, sizeof pat, dir);
    strcat_s(pat, sizeof pat, "/*.ini");
    if (!u8_find_first(pat, &fd)) {
        return 0; /* kein Ordner / keine Profile: nur Legacy */
    }
    do {
        size_t len = strlen(fd.name);
        if (fd.attr & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        if (len <= 4 || len - 4 >= 64 || n >= max) {
            continue;
        }
        memcpy(names[n], fd.name, len - 4);
        names[n][len - 4] = '\0';
        n++;
    } while (u8_find_next(&fd));
    u8_find_close(&fd);
    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            if (strcmp(names[i], names[j]) > 0) {
                char t[64];
                strcpy_s(t, sizeof t, names[i]);
                strcpy_s(names[i], 64, names[j]);
                strcpy_s(names[j], 64, t);
            }
        }
    }
    return n;
}

int lctrl_input_profile_path(const char *client_dir, const char *name,
                             char *out, size_t cap)
{
    size_t i, n;
    if (!client_dir || !*client_dir || !name || !*name) {
        return 1;
    }
    n = strlen(name);
    if (n > 63 || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 1;
    }
    for (i = 0; i < n; i++) {
        char c = name[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) {
            return 1; /* kein Pfad-Voodoo: nur Namen */
        }
    }
    strcpy_s(out, cap, client_dir);
    n = strlen(out);
    if (n > 0 && out[n - 1] != '/' && out[n - 1] != '\\') {
        strcat_s(out, cap, "/");
    }
    strcat_s(out, cap, "profiles/");
    strcat_s(out, cap, name);
    strcat_s(out, cap, ".ini");
    return 0;
}

int lctrl_user_rest_ok(const char *rest)
{
    size_t i, n;
    if (!rest || !*rest) {
        return 0;
    }
    n = strlen(rest);
    if (n > 40) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        char c = rest[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) {
            return 0;
        }
    }
    return 1;
}

int lctrl_user_filename(const char *rest, char *out, size_t cap)
{
    if (!lctrl_user_rest_ok(rest)) {
        return 1;
    }
    strcpy_s(out, cap, "user_");
    strcat_s(out, cap, rest);
    strcat_s(out, cap, ".ini");
    return 0;
}

int lctrl_name_shipped(const char *name)
{
    return name && (strcmp(name, "keyboard_default") == 0 ||
                    strcmp(name, "xbox_pad_default") == 0);
}

/* --- SBC-USB-Status ---------------------------------------------------- */

int lctrl_sbc_status(char *out, size_t cap)
{
    sbc_usb_dev_t *dev = NULL;
    int rc = sbc_usb_open(&dev);
    if (rc == SBC_OK) {
        sbc_usb_close(dev);
        strcpy_s(out, cap, LT("gui.sbc_ok"));
        return 0;
    }
    if (rc == SBC_ERR_DRIVER) {
        /* W128: Hinweis auf Zadig + gefuehrte Pruefung sbc_probe --test */
        strcpy_s(out, cap, LT("gui.sbc_driver"));
        return rc;
    }
    if (rc == SBC_ERR_NODEVICE) {
        strcpy_s(out, cap, LT("gui.sbc_none"));
        return rc;
    }
    _snprintf_s(out, cap, _TRUNCATE, LT("gui.sbc_error"),
                sbc_usb_strerror(rc));
    return rc;
}

/* --- Gamepad (XInput, dynamisch) --------------------------------------- */

#pragma pack(push, 1)
typedef struct {
    uint16_t wButtons;
    uint8_t bLeftTrigger, bRightTrigger;
    int16_t sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} lctrl_xi_gamepad_t;

typedef struct {
    uint32_t dwPacketNumber;
    lctrl_xi_gamepad_t Gamepad;
} lctrl_xi_state_t;
#pragma pack(pop)

typedef uint32_t(WINAPI *lctrl_xgs_fn)(uint32_t, lctrl_xi_state_t *);

static lctrl_xgs_fn g_xgs;
static int g_xgs_tried;

/* XINPUT_GAMEPAD_*-Bits (lokal, ohne xinput.h, wie input_map.c). */
#define LCTRL_PB_DU 0x0001
#define LCTRL_PB_DD 0x0002
#define LCTRL_PB_DL 0x0004
#define LCTRL_PB_DR 0x0008
#define LCTRL_PB_START 0x0010
#define LCTRL_PB_BACK 0x0020
#define LCTRL_PB_L3 0x0040
#define LCTRL_PB_R3 0x0080
#define LCTRL_PB_LB 0x0100
#define LCTRL_PB_RB 0x0200
#define LCTRL_PB_A 0x1000
#define LCTRL_PB_B 0x2000
#define LCTRL_PB_X 0x4000
#define LCTRL_PB_Y 0x8000

static void xgs_ensure(void)
{
    static const char *dlls[] = {
        "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", NULL
    };
    int i;
    if (g_xgs_tried) {
        return;
    }
    g_xgs_tried = 1;
    for (i = 0; dlls[i]; i++) {
        HMODULE m = LoadLibraryA(dlls[i]);
        if (m) {
            g_xgs = (lctrl_xgs_fn)(void *)GetProcAddress(m, "XInputGetState");
            if (g_xgs) {
                break;
            }
            FreeLibrary(m);
        }
    }
}

int lctrl_pad_poll(int port, lctrl_pad_t *out)
{
    lctrl_xi_state_t xs;
    xgs_ensure();
    if (!g_xgs || port < 0 || port > 3) {
        return 0;
    }
    memset(&xs, 0, sizeof xs);
    if (g_xgs((uint32_t)port, &xs) != 0 /* ERROR_SUCCESS */) {
        return 0;
    }
    if (out) {
        out->buttons = xs.Gamepad.wButtons;
        out->lt = xs.Gamepad.bLeftTrigger;
        out->rt = xs.Gamepad.bRightTrigger;
        out->lx = xs.Gamepad.sThumbLX;
        out->ly = xs.Gamepad.sThumbLY;
        out->rx = xs.Gamepad.sThumbRX;
        out->ry = xs.Gamepad.sThumbRY;
    }
    return 1;
}

int lctrl_pad_first(lctrl_pad_t *out)
{
    int p;
    for (p = 0; p < 4; p++) {
        if (lctrl_pad_poll(p, out)) {
            return p;
        }
    }
    return -1;
}

const char *lctrl_pad_button_name(unsigned bit)
{
    switch (bit) {
    case LCTRL_PB_A: return "A";
    case LCTRL_PB_B: return "B";
    case LCTRL_PB_X: return "X";
    case LCTRL_PB_Y: return "Y";
    case LCTRL_PB_LB: return "LB";
    case LCTRL_PB_RB: return "RB";
    case LCTRL_PB_BACK: return "BACK";
    case LCTRL_PB_START: return "START";
    case LCTRL_PB_L3: return "L3";
    case LCTRL_PB_R3: return "R3";
    case LCTRL_PB_DU: return "DU";
    case LCTRL_PB_DD: return "DD";
    case LCTRL_PB_DL: return "DL";
    case LCTRL_PB_DR: return "DR";
    default: return NULL;
    }
}

void lctrl_pad_format(const lctrl_pad_t *p, char *out, size_t cap)
{
    static const unsigned bits[] = {
        LCTRL_PB_A, LCTRL_PB_B, LCTRL_PB_X, LCTRL_PB_Y, LCTRL_PB_LB,
        LCTRL_PB_RB, LCTRL_PB_BACK, LCTRL_PB_START, LCTRL_PB_L3,
        LCTRL_PB_R3, LCTRL_PB_DU, LCTRL_PB_DD, LCTRL_PB_DL, LCTRL_PB_DR
    };
    char btns[128] = { 0 };
    size_t i, bl = 0;
    for (i = 0; i < sizeof bits / sizeof bits[0]; i++) {
        if (p->buttons & bits[i]) {
            const char *nm = lctrl_pad_button_name(bits[i]);
            bl += (size_t)_snprintf_s(btns + bl, sizeof btns - bl, _TRUNCATE,
                                      "%s%s", bl ? " " : "", nm ? nm : "?");
        }
    }
    if (p->lt > 20) {
        bl += (size_t)_snprintf_s(btns + bl, sizeof btns - bl, _TRUNCATE,
                                  "%sLT", bl ? " " : "");
    }
    if (p->rt > 20) {
        bl += (size_t)_snprintf_s(btns + bl, sizeof btns - bl, _TRUNCATE,
                                  "%sRT", bl ? " " : "");
    }
    _snprintf_s(out, cap, _TRUNCATE,
                "LX %+0.2f LY %+0.2f RX %+0.2f RY %+0.2f LT %.2f RT %.2f   %s%s%s",
                p->lx / 32768.0, p->ly / 32768.0, p->rx / 32768.0,
                p->ry / 32768.0, p->lt / 255.0, p->rt / 255.0,
                bl ? LT("gui.pad_keys") : LT("gui.pad_nokey"), bl ? " " : "",
                bl ? btns : "");
}

int lctrl_pad_new_press(const lctrl_pad_t *prev, const lctrl_pad_t *cur,
                        char *name, size_t cap)
{
    static const unsigned bits[] = {
        LCTRL_PB_A, LCTRL_PB_B, LCTRL_PB_X, LCTRL_PB_Y, LCTRL_PB_LB,
        LCTRL_PB_RB, LCTRL_PB_START, LCTRL_PB_BACK, LCTRL_PB_L3,
        LCTRL_PB_R3, LCTRL_PB_DU, LCTRL_PB_DD, LCTRL_PB_DL, LCTRL_PB_DR
    };
    unsigned fresh;
    size_t i;
    if (!prev || !cur) {
        return 0;
    }
    fresh = (unsigned)(cur->buttons & ~prev->buttons);
    for (i = 0; i < sizeof bits / sizeof bits[0]; i++) {
        if (fresh & bits[i]) {
            const char *nm = lctrl_pad_button_name(bits[i]);
            strcpy_s(name, cap, nm ? nm : "?");
            return 1;
        }
    }
    /* Trigger als digitale Taste (Schwelle 50 %, wie der Client). */
    if (prev->lt <= 128 && cur->lt > 128) {
        strcpy_s(name, cap, "LT");
        return 1;
    }
    if (prev->rt <= 128 && cur->rt > 128) {
        strcpy_s(name, cap, "RT");
        return 1;
    }
    return 0;
}

static double dabs(double v)
{
    return v < 0 ? -v : v;
}

int lctrl_pad_deflected(const lctrl_pad_t *p, char *name, size_t cap)
{
    double v[4];
    int hot = -1, i, n_hot = 0;
    if (!p) {
        return 0;
    }
    v[0] = p->lx / 32768.0;
    v[1] = p->ly / 32768.0;
    v[2] = p->rx / 32768.0;
    v[3] = p->ry / 32768.0;
    for (i = 0; i < 4; i++) {
        if (dabs(v[i]) > 0.7) {
            hot = i;
            n_hot++;
        } else if (dabs(v[i]) > 0.3) {
            return 0; /* mehrere Achsen: uneindeutig */
        }
    }
    if (n_hot == 1) {
        static const char *nm[4] = { "LX", "LY", "RX", "RY" };
        strcpy_s(name, cap, nm[hot]);
        return 1;
    }
    /* Trigger (analog): einer voll, keiner halb. */
    {
        int lt = p->lt > 200, rt = p->rt > 200;
        int lt_half = p->lt > 40 && p->lt <= 200;
        int rt_half = p->rt > 40 && p->rt <= 200;
        if (n_hot == 0 && !lt_half && !rt_half && (lt ^ rt)) {
            strcpy_s(name, cap, lt ? "LT" : "RT");
            return 1;
        }
    }
    return 0;
}
/* --- Tasten-Namen ------------------------------------------------------ */

void lctrl_key_name(int vk, char *out, size_t cap)
{
    static const struct { int vk; const char *n; } tab[] = {
        { VK_SPACE, "SPACE" }, { VK_RETURN, "ENTER" }, { VK_TAB, "TAB" },
        { VK_SHIFT, "SHIFT" }, { VK_CONTROL, "CTRL" }, { VK_MENU, "ALT" },
        { VK_LCONTROL, "LCTRL" }, { VK_RCONTROL, "RCTRL" },
        { VK_ESCAPE, "ESC" }, { VK_LEFT, "LEFT" }, { VK_RIGHT, "RIGHT" },
        { VK_UP, "UP" }, { VK_DOWN, "DOWN" }, { VK_BACK, "BACKSPACE" },
        { VK_INSERT, "INSERT" }, { VK_DELETE, "DELETE" },
        { VK_HOME, "HOME" }, { VK_END, "END" },
        { VK_PRIOR, "PAGEUP" }, { VK_NEXT, "PAGEDOWN" },
        { VK_F1, "F1" }, { VK_F2, "F2" }, { VK_F3, "F3" },
        { VK_F4, "F4" }, { VK_F5, "F5" }, { VK_F6, "F6" },
        { VK_F7, "F7" }, { VK_F8, "F8" }, { VK_F9, "F9" },
        { VK_F10, "F10" }, { VK_F11, "F11" }, { VK_F12, "F12" },
        { VK_PAUSE, "PAUSE" }, /* W188 */
        { 0, NULL }
    };
    int i;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        out[0] = (char)vk;
        out[1] = '\0';
        return;
    }
    /* Links-/Rechts-Varianten auf die Basisnamen falten (W110: Strg
     * bleibt getrennt - LCTRL/RCTRL wie im Client-Profil). */
    if (vk == VK_LSHIFT || vk == VK_RSHIFT) { vk = VK_SHIFT; }
    if (vk == VK_LMENU || vk == VK_RMENU) { vk = VK_MENU; }
    for (i = 0; tab[i].n; i++) {
        if (tab[i].vk == vk) {
            strcpy_s(out, cap, tab[i].n);
            return;
        }
    }
    _snprintf_s(out, cap, _TRUNCATE, "%d", vk); /* numerisch (Parser kann das) */
}

/* --- Belegungs-Modell -------------------------------------------------- */
/* Reihenfolge wie keyboard_default.ini (Tasten, Achsen, Gang/Tuner),
 * Labels aus CONTROLLER.md Abschnitt 8. */

const lctrl_elem_t kCtrlElems[LCTRL_ELEM_N] = {
    { "bi0", "mainweapon", "bind.bi0", LCTRL_DIG },
    { "bi1", "subweapon", "bind.bi1", LCTRL_DIG },
    { "bi2", "lockon", "bind.bi2", LCTRL_DIG },
    { "bi3", "eject", "bind.bi3", LCTRL_DIG },
    { "bi4", "hatch", "bind.bi4", LCTRL_DIG },
    { "bi5", "ignition", "bind.bi5", LCTRL_DIG },
    { "bi6", "start", "bind.bi6", LCTRL_DIG },
    { "bi7", "openclose", "bind.bi7", LCTRL_DIG },
    { "bi8", "mapzoom", "bind.bi8", LCTRL_DIG },
    { "bi9", "mode", "bind.bi9", LCTRL_DIG },
    { "bi10", "submon", "bind.bi10", LCTRL_DIG },
    { "bi11", "zoomin", "bind.bi11", LCTRL_DIG },
    { "bi12", "zoomout", "bind.bi12", LCTRL_DIG },
    { "bi13", "fss", "bind.bi13", LCTRL_DIG },
    { "bi14", "manip", "bind.bi14", LCTRL_DIG },
    { "bi15", "linecolor", "bind.bi15", LCTRL_DIG },
    { "bi16", "washing", "bind.bi16", LCTRL_DIG },
    { "bi17", "exting", "bind.bi17", LCTRL_DIG },
    { "bi18", "chaff", "bind.bi18", LCTRL_DIG },
    { "bi19", "tankdetach", "bind.bi19", LCTRL_DIG },
    { "bi20", "override", "bind.bi20", LCTRL_DIG },
    { "bi21", "night", "bind.bi21", LCTRL_DIG },
    { "bi22", "f1", "bind.bi22", LCTRL_DIG },
    { "bi23", "f2", "bind.bi23", LCTRL_DIG },
    { "bi24", "f3", "bind.bi24", LCTRL_DIG },
    { "bi25", "mainwc", "bind.bi25", LCTRL_DIG },
    { "bi26", "subwc", "bind.bi26", LCTRL_DIG },
    { "bi27", "magazine", "bind.bi27", LCTRL_DIG },
    { "bi28", "comm1", "bind.bi28", LCTRL_DIG },
    { "bi29", "comm2", "bind.bi29", LCTRL_DIG },
    { "bi30", "comm3", "bind.bi30", LCTRL_DIG },
    { "bi31", "comm4", "bind.bi31", LCTRL_DIG },
    { "bi32", "comm5", "bind.bi32", LCTRL_DIG },
    { "bi33", "sightbtn", "bind.bi33", LCTRL_DIG },
    { "bi34", "togfilter", "bind.bi34", LCTRL_DIG },
    { "bi35", "togoxygen", "bind.bi35", LCTRL_DIG },
    { "bi36", "togfuel", "bind.bi36", LCTRL_DIG },
    { "bi37", "togbuffer", "bind.bi37", LCTRL_DIG },
    { "bi38", "togvtloc", "bind.bi38", LCTRL_DIG },
    { "aim_x", "-", "bind.aim_x", LCTRL_AXIS },
    { "aim_y", "-", "bind.aim_y", LCTRL_AXIS },
    { "rot", "-", "bind.rot", LCTRL_AXIS },
    { "sight_x", "-", "bind.sight_x", LCTRL_AXIS },
    { "sight_y", "-", "bind.sight_y", LCTRL_AXIS },
    { "pedal_l", "-", "bind.pedal_l", LCTRL_AXIS },
    { "pedal_m", "-", "bind.pedal_m", LCTRL_AXIS },
    { "pedal_r", "-", "bind.pedal_r", LCTRL_AXIS },
    { "aim_x.neg", "-", "bind.aim_x.neg", LCTRL_NEG },
    { "aim_x.pos", "-", "bind.aim_x.pos", LCTRL_POS },
    { "aim_y.neg", "-", "bind.aim_y.neg", LCTRL_NEG },
    { "aim_y.pos", "-", "bind.aim_y.pos", LCTRL_POS },
    { "rot.neg", "-", "bind.rot.neg", LCTRL_NEG },
    { "rot.pos", "-", "bind.rot.pos", LCTRL_POS },
    { "sight_x.neg", "-", "bind.sight_x.neg", LCTRL_NEG },
    { "sight_x.pos", "-", "bind.sight_x.pos", LCTRL_POS },
    { "sight_y.neg", "-", "bind.sight_y.neg", LCTRL_NEG },
    { "sight_y.pos", "-", "bind.sight_y.pos", LCTRL_POS },
    { "gear.up", "-", "bind.gear.up", LCTRL_DIG },
    { "gear.down", "-", "bind.gear.down", LCTRL_DIG },
    { "gear.r", "-", "bind.gear.r", LCTRL_DIG },
    { "gear.n", "-", "bind.gear.n", LCTRL_DIG },
    { "gear.1", "-", "bind.gear.1", LCTRL_DIG },
    { "gear.2", "-", "bind.gear.2", LCTRL_DIG },
    { "gear.3", "-", "bind.gear.3", LCTRL_DIG },
    { "gear.4", "-", "bind.gear.4", LCTRL_DIG },
    { "gear.5", "-", "bind.gear.5", LCTRL_DIG },
    { "tuner.up", "-", "bind.tuner.up", LCTRL_DIG },
    { "tuner.down", "-", "bind.tuner.down", LCTRL_DIG },
    /* W159: Client-Aktion (Gruppe VR im Dialog, Abschnitt [vr] im Profil). */
    { "vr_recenter", "-", "bind.vr_recenter", LCTRL_ACT },
    /* W188 GAME-MENU: Client-Aktion Pause (Abschnitt [client]); Konflikte
     * mit Spiel-Elementen meldet lctrl_bind_conflicts wie bei allen Zeilen. */
    { "pause", "-", "bind.pause", LCTRL_ACT },
    /* W195 PHOTO-MODE: Client-Aktion Fotomodus (Abschnitt [client], Standard F12). */
    { "feedback_shot", "-", "bind.feedback_shot", LCTRL_ACT },
    { "feedback_clip", "-", "bind.feedback_clip", LCTRL_ACT },
    { "photo", "-", "bind.photo", LCTRL_ACT },
};

/* W159: kanonische SBC-Quellnamen bi0..38 (parserstabil, s. kBiAlias). */
static const char *const kSbcBtnNames[39] = {
    "MAINWEAPON", "SUBWEAPON", "LOCKON", "EJECT", "HATCH", "IGNITION",
    "START", "OPENCLOSE", "MAPZOOM", "MODE", "SUBMON", "ZOOMIN",
    "ZOOMOUT", "FSS", "MANIP", "LINECOLOR", "WASHING", "EXTING",
    "CHAFF", "TANKDETACH", "OVERRIDE", "NIGHT", "F1", "F2", "F3",
    "MAINWC", "SUBWC", "MAGAZINE", "COMM1", "COMM2", "COMM3", "COMM4",
    "COMM5", "SIGHTBTN", "TOGFILTER", "TOGOXYGEN", "TOGFUEL",
    "TOGBUFFER", "TOGVTLOC"
};

const char *lctrl_sbc_btn_name(int bi)
{
    if (bi < 0 || bi >= 39) {
        return NULL;
    }
    return kSbcBtnNames[bi];
}

/* Element-Index: bi0/bi+N, Klarname, achse[.neg/.pos], gear.*, tuner.*.
 * Case-insensitiv, "_neg"/"_pos" wie der Client. -1 = unbekannt. */
static int strieq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') { ca = (char)(ca + 32); }
        if (cb >= 'A' && cb <= 'Z') { cb = (char)(cb + 32); }
        if (ca != cb) { return 0; }
        a++;
        b++;
    }
    return *a == *b;
}

int lctrl_bind_elem_index(const char *name)
{
    int i;
    if (!name || !*name) { return -1; }
    /* Direkt: Profil-Schluessel. */
    for (i = 0; i < LCTRL_ELEM_N; i++) {
        if (strieq(name, kCtrlElems[i].key)) { return i; }
    }
    /* Klarnamen (bi-Aliase wie im Client). */
    for (i = 0; i < LCTRL_ELEM_N; i++) {
        if (kCtrlElems[i].klar[0] != '-' &&
            strieq(name, kCtrlElems[i].klar)) {
            return i;
        }
    }
    /* "_neg"/"_pos"-Schreibweise auf ".neg"/".pos" falten. */
    {
        char tmp[32];
        size_t n = strlen(name);
        if (n < sizeof tmp - 1 && n > 4) {
            strcpy_s(tmp, sizeof tmp, name);
            if (strieq(tmp + n - 4, "_neg") ||
                strieq(tmp + n - 4, "_pos")) {
                tmp[n - 4] = '.';
                for (i = 0; i < LCTRL_ELEM_N; i++) {
                    if (strieq(tmp, kCtrlElems[i].key)) { return i; }
                }
            }
        }
    }
    return -1;
}

void lctrl_bind_init(lctrl_bind_t *b)
{
    memset(b, 0, sizeof *b);
}

static void trim(char *s)
{
    char *e;
    size_t n;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        memmove(s, s + 1, strlen(s + 1) + 1);
    }
    n = strlen(s);
    e = s + n;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
                     e[-1] == '\n')) {
        e--;
    }
    *e = '\0';
}

int lctrl_bind_set(lctrl_bind_t *b, int idx, const char *alts)
{
    char work[512], *one, *ctx = NULL;
    if (!b || idx < 0 || idx >= LCTRL_ELEM_N) { return 1; }
    b->row[idx].n_alt = 0;
    if (!alts || !*alts) { return 0; }
    strcpy_s(work, sizeof work, alts);
    for (one = strtok_s(work, ",", &ctx); one;
         one = strtok_s(NULL, ",", &ctx)) {
        trim(one);
        if (!one[0]) { continue; }
        if (b->row[idx].n_alt >= LCTRL_ALT_N) { break; } /* max. 4 */
        strcpy_s(b->row[idx].alt[b->row[idx].n_alt],
                 sizeof b->row[idx].alt[0], one);
        b->row[idx].n_alt++;
    }
    return 0;
}

int lctrl_bind_add(lctrl_bind_t *b, int idx, const char *alt)
{
    char tmp[96];
    int i;
    if (!b || idx < 0 || idx >= LCTRL_ELEM_N || !alt || !*alt) { return 1; }
    strcpy_s(tmp, sizeof tmp, alt);
    trim(tmp);
    if (!tmp[0] || strchr(tmp, ',')) { return 1; }
    for (i = 0; i < b->row[idx].n_alt; i++) {
        if (strieq(b->row[idx].alt[i], tmp)) { return 1; } /* doppelt */
    }
    if (b->row[idx].n_alt >= LCTRL_ALT_N) { return 1; }
    strcpy_s(b->row[idx].alt[b->row[idx].n_alt], sizeof tmp, tmp);
    b->row[idx].n_alt++;
    return 0;
}

void lctrl_bind_clear(lctrl_bind_t *b, int idx)
{
    if (!b || idx < 0 || idx >= LCTRL_ELEM_N) { return; }
    b->row[idx].n_alt = 0;
}

void lctrl_bind_row_text(const lctrl_bind_t *b, int idx, char *out, size_t cap)
{
    size_t len = 0;
    int i;
    out[0] = '\0';
    if (!b || idx < 0 || idx >= LCTRL_ELEM_N) { return; }
    if (b->row[idx].n_alt == 0) {
        strcpy_s(out, cap, LT("gui.bind_unset"));
        return;
    }
    for (i = 0; i < b->row[idx].n_alt; i++) {
        len += (size_t)_snprintf_s(out + len, cap - len, _TRUNCATE, "%s%s",
                                   i ? ", " : "", b->row[idx].alt[i]);
    }
}

/* Eine Profilzeile auswerten (Semantik wie imap_profile_parse: '#'
 * inline, ';'/'[' ueberspringen, element = alt, alt; unbekannt = still
 * ignorieren, letzte Zeile gewinnt). line wird veraendert. */
static void bind_parse_line(char *line, lctrl_bind_t *b)
{
    char *h, *eq, *val, *one, *ctx = NULL;
    int idx;
    h = strchr(line, '#');
    if (h) { *h = '\0'; } /* Kommentar ab '#' (wie Client) */
    trim(line);
    if (!line[0] || line[0] == ';' || line[0] == '[') { return; }
    eq = strchr(line, '=');
    if (!eq) { return; }
    *eq = '\0';
    trim(line);
    /* W221 HOTAS: Geraete-Zuordnung joy0..joy3 = <spec> erhalten */
    if ((line[0] == 'j' || line[0] == 'J') && (line[1] == 'o' || line[1] == 'O') &&
        (line[2] == 'y' || line[2] == 'Y') && line[3] >= '0' &&
        line[3] < '0' + IMAP_JOY_MAX && line[4] == '\0') {
        char spec[96];
        strncpy_s(spec, sizeof spec, eq + 1, _TRUNCATE);
        trim(spec);
        strcpy_s(b->joy[line[3] - '0'], sizeof b->joy[0], spec);
        return;
    }
    idx = lctrl_bind_elem_index(line);
    if (idx < 0) { return; } /* unbekannt: ignorieren */
    val = eq + 1;
    b->row[idx].n_alt = 0; /* letzte Zeile gewinnt */
    for (one = strtok_s(val, ",", &ctx); one;
         one = strtok_s(NULL, ",", &ctx)) {
        trim(one);
        if (!one[0]) { continue; }
        if (b->row[idx].n_alt >= LCTRL_ALT_N) { break; }
        strcpy_s(b->row[idx].alt[b->row[idx].n_alt],
                 sizeof b->row[idx].alt[0], one);
        b->row[idx].n_alt++;
    }
}

int lctrl_bind_load_file(const char *path, lctrl_bind_t *b)
{
    FILE *f;
    char line[512];
    lctrl_bind_init(b);
    if (u8_fopen_s(&f, path, "r") != 0 || !f) { return 1; }
    while (fgets(line, sizeof line, f)) {
        bind_parse_line(line, b);
    }
    fclose(f);
    return 0;
}

/* W182 KEYS-SVG: gleiche Auswertung fuer einen Profiltext im Speicher
 * (Browser-Export: Profiltext ist schon gelesen). */
int lctrl_bind_load_text(const char *text, lctrl_bind_t *b)
{
    char line[512];
    const char *p = text;
    lctrl_bind_init(b);
    if (!text) { return 1; }
    while (*p) {
        size_t n = strcspn(p, "\r\n");
        size_t c = n < sizeof line - 1 ? n : sizeof line - 1;
        memcpy(line, p, c);
        line[c] = '\0';
        bind_parse_line(line, b);
        p += n;
        while (*p == '\r' || *p == '\n') { p++; }
    }
    return 0;
}

int lctrl_bind_alt_is_sbc(const char *alt)
{
    const char *p;
    if (!alt) {
        return 0;
    }
    while (*alt == ' ' || *alt == '\t') {
        alt++;
    }
    /* "SBC:" am Anfang oder nach '+' (Akkord), Gross/Klein egal. */
    if ((alt[0] == 'S' || alt[0] == 's') &&
        (alt[1] == 'B' || alt[1] == 'b') &&
        (alt[2] == 'C' || alt[2] == 'c') && alt[3] == ':') {
        return 1;
    }
    for (p = alt; *p; p++) {
        if (p[0] == '+' && (p[1] == 'S' || p[1] == 's') &&
            (p[2] == 'B' || p[2] == 'b') &&
            (p[3] == 'C' || p[3] == 'c') && p[4] == ':') {
            return 1;
        }
    }
    return 0;
}

/* Konflikt-Suche: gleiche Alternative auf mehreren Elementen. */
int lctrl_bind_conflicts(const lctrl_bind_t *b, char out[][160], int max)
{
    int i, m, a, n = 0;
    if (!b || max <= 0) { return 0; }
    for (i = 0; i < LCTRL_ELEM_N; i++) {
        int j;
        for (j = 0; j < b->row[i].n_alt; j++) {
            const char *alt = b->row[i].alt[j];
            char who[128] = { 0 };
            size_t wl = 0;
            int cnt = 0, seen_before = 0;
            /* Schon als Folge-Eintrag gemeldet? */
            for (m = 0; m < i && !seen_before; m++) {
                for (a = 0; a < b->row[m].n_alt; a++) {
                    if (strieq(b->row[m].alt[a], alt)) {
                        seen_before = 1;
                        break;
                    }
                }
            }
            if (seen_before) { continue; }
            for (m = 0; m < LCTRL_ELEM_N; m++) {
                for (a = 0; a < b->row[m].n_alt; a++) {
                    if (strieq(b->row[m].alt[a], alt)) {
                        wl += (size_t)_snprintf_s(who + wl, sizeof who - wl,
                                                 _TRUNCATE, "%s%s",
                                                 cnt ? ", " : "",
                                                 kCtrlElems[m].key);
                        cnt++;
                        break;
                    }
                }
            }
            if (cnt > 1 && n < max) {
                _snprintf_s(out[n], 160, _TRUNCATE, "%s: %s", alt, who);
                n++;
            }
        }
    }
    return n;
}

/* W228 BIND-DIALOG: Darstellungs-Helfer (Liste gegliedert, Detailbereich). */
int lctrl_elem_group(int idx)
{
    const char *k;
    if (idx < 0 || idx >= LCTRL_ELEM_N) { return -1; }
    k = kCtrlElems[idx].key;
    switch (kCtrlElems[idx].kind) {
    case LCTRL_AXIS: return 1;
    case LCTRL_NEG:
    case LCTRL_POS: return 2;
    case LCTRL_ACT: return 5;
    default: break;
    }
    if (strncmp(k, "gear.", 5) == 0) { return 3; }
    if (strncmp(k, "tuner.", 6) == 0) { return 4; }
    return 0;
}

int lctrl_helper_axis(int idx)
{
    char nm[32];
    char *dot;
    if (idx < 0 || idx >= LCTRL_ELEM_N ||
        (kCtrlElems[idx].kind != LCTRL_NEG && kCtrlElems[idx].kind != LCTRL_POS)) {
        return -1;
    }
    strcpy_s(nm, sizeof nm, kCtrlElems[idx].key);
    dot = strchr(nm, '.');
    if (dot) { *dot = '\0'; }
    return lctrl_bind_elem_index(nm);
}

unsigned lctrl_bind_detail_vis(const lctrl_bind_t *b, int idx)
{
    unsigned v = 0;
    int a, axis;
    if (!b || idx < 0 || idx >= LCTRL_ELEM_N) { return 0; }
    axis = kCtrlElems[idx].kind == LCTRL_AXIS;
    if (axis && (strncmp(kCtrlElems[idx].key, "aim_", 4) == 0 ||
                 strncmp(kCtrlElems[idx].key, "sight_", 6) == 0)) {
        v |= LCTRL_VIS_MOUSE;
    }
    for (a = 0; a < b->row[idx].n_alt; a++) {
        int k = lctrl_joy_src_axis(b->row[idx].alt[a], NULL, NULL);
        if (k) { v |= LCTRL_VIS_JOY; }
        if (k == 1) { v |= LCTRL_VIS_JOYDZ; }
    }
    if (axis && !(v & LCTRL_VIS_JOY)) { v |= LCTRL_VIS_JOYHINT; }
    return v;
}

int lctrl_bind_remove_alt(lctrl_bind_t *b, int idx, int ai)
{
    int i;
    if (!b || idx < 0 || idx >= LCTRL_ELEM_N || ai < 0 || ai >= b->row[idx].n_alt) {
        return 1;
    }
    for (i = ai; i + 1 < b->row[idx].n_alt; i++) {
        strcpy_s(b->row[idx].alt[i], sizeof b->row[idx].alt[i], b->row[idx].alt[i + 1]);
    }
    b->row[idx].n_alt--;
    b->row[idx].alt[b->row[idx].n_alt][0] = '\0';
    return 0;
}

int lctrl_bind_conflicts_for(const lctrl_bind_t *b, int idx,
                             lctrl_conf1_t *out, int max)
{
    int j, m, a, n = 0;
    if (!b || !out || max <= 0 || idx < 0 || idx >= LCTRL_ELEM_N) { return 0; }
    for (j = 0; j < b->row[idx].n_alt && n < max; j++) {
        lctrl_conf1_t c;
        memset(&c, 0, sizeof c);
        c.alt = j;
        for (m = 0; m < LCTRL_ELEM_N; m++) {
            if (m == idx) { continue; }
            for (a = 0; a < b->row[m].n_alt; a++) {
                if (strieq(b->row[m].alt[a], b->row[idx].alt[j])) {
                    if (c.n < 8) { c.other[c.n++] = m; }
                    break;
                }
            }
        }
        if (c.n > 0) { out[n++] = c; }
    }
    return n;
}

int lctrl_bind_save(const char *path, const lctrl_bind_t *b, const char *base,
                    char *err, size_t errcap)
{
    FILE *f;
    int i;
    const char *sec = NULL;
    if (!b || !path || !*path) {
        if (err) { strcpy_s(err, errcap, LT("gui.bind_nopath")); }
        return 1;
    }
    if (u8_fopen_s(&f, path, "w") != 0 || !f) {
        if (err) {
            _snprintf_s(err, errcap, _TRUNCATE, LT("gui.bind_nowrite"), path);
        }
        return 1;
    }
    fprintf(f, "# Benutzerprofil, per Launcher belegt (Basis: %s).\n",
            base && *base ? base : "(leer)");
    fprintf(f, "# Format: docs/EINGABE.md. Aktivieren: input_profile=<name>.\n");
    /* W221 HOTAS: Geraete-Zuordnung nur, wenn vorhanden (ohne Joystick
     * bleibt die Datei bitgleich wie bisher). */
    for (i = 0; i < IMAP_JOY_MAX; i++) {
        if (b->joy[i][0]) {
            int j;
            fprintf(f, "\n[joystick]\n");
            for (j = 0; j < IMAP_JOY_MAX; j++) {
                if (b->joy[j][0]) {
                    fprintf(f, "joy%d = %s\n", j, b->joy[j]);
                }
            }
            break;
        }
    }
    for (i = 0; i < LCTRL_ELEM_N; i++) {
        const char *want =
            kCtrlElems[i].kind == LCTRL_ACT ?
                (strcmp(kCtrlElems[i].key, "vr_recenter") == 0 ? "vr" : "client") : /* W159, W188 */
            kCtrlElems[i].kind == LCTRL_DIG && i < 39 ? "buttons" :
            kCtrlElems[i].kind == LCTRL_DIG ? "gear" : "axes";
        int a;
        if (b->row[i].n_alt == 0) { continue; } /* unbelegt: keine Zeile */
        if (!sec || strcmp(sec, want) != 0) {
            sec = want;
            fprintf(f, "\n[%s]\n", sec);
        }
        fprintf(f, "%s = ", kCtrlElems[i].key);
        for (a = 0; a < b->row[i].n_alt; a++) {
            fprintf(f, "%s%s", a ? ", " : "", b->row[i].alt[a]);
        }
        fprintf(f, "\n");
    }
    fclose(f);
    if (err && errcap) { err[0] = '\0'; }
    return 0;
}

/* --- W221 HOTAS (experimentell): Joystick-Quellen ---------------------- */

/* "JOYn:" bzw. "JOY:" am Anfang von a (Gross/Klein egal). Rueckgabe
 * Laenge des Praefixes (inkl. ':'), 0 = keine JOY-Quelle; *slot = n. */
static int joy_prefix(const char *a, int *slot)
{
    if (!((a[0] == 'J' || a[0] == 'j') && (a[1] == 'O' || a[1] == 'o') &&
          (a[2] == 'Y' || a[2] == 'y'))) {
        return 0;
    }
    if (a[3] == ':') {
        if (slot) { *slot = 0; }
        return 4;
    }
    if (a[3] >= '0' && a[3] < '0' + IMAP_JOY_MAX && a[4] == ':') {
        if (slot) { *slot = a[3] - '0'; }
        return 5;
    }
    return 0;
}

int lctrl_bind_uses_joy_slot(const lctrl_bind_t *b, int slot)
{
    int i, j;
    if (!b) { return 0; }
    for (i = 0; i < LCTRL_ELEM_N; i++) {
        for (j = 0; j < b->row[i].n_alt; j++) {
            const char *p = b->row[i].alt[j];
            for (;;) {
                int s = -1;
                while (*p == ' ' || *p == '\t') { p++; }
                if (joy_prefix(p, &s) && s == slot) { return 1; }
                p = (char *)lctrl_chord_plus(p); /* Akkord (W226) */
                if (!p) { break; }
                p++;
            }
        }
    }
    return 0;
}

static int joy_btn(const imap_joy_state_t *s, int b)
{
    return (int)((s->btn[b >> 5] >> (b & 31)) & 1u);
}

static int iabs_(int v)
{
    return v < 0 ? -v : v;
}

/* W226 JOY-AXIS-DIR: verlangte Bewegungsrichtung je Achsen-Element (wie
 * die Tastatur-Endlagen der Standardprofile: .pos = rechts bzw. unten).
 * Zielhebel/Sicht hoch = DirectInput-Minimum (Stick nach vorn). */
int lctrl_joy_dir_expect(int idx)
{
    const char *k;
    if (idx < 0 || idx >= LCTRL_ELEM_N || kCtrlElems[idx].kind != LCTRL_AXIS) {
        return 0;
    }
    k = kCtrlElems[idx].key;
    if (strcmp(k, "aim_y") == 0 || strcmp(k, "sight_y") == 0) {
        return -1;
    }
    return 1;
}

/* Gang-Direktwahl gear.r/n/1..5 (einzige Elemente mit Lern-Zone). */
static int joy_is_gear_direct(int idx)
{
    const char *k = kCtrlElems[idx].key;
    return strncmp(k, "gear.", 5) == 0 && strcmp(k, "gear.up") != 0 &&
           strcmp(k, "gear.down") != 0;
}

const char *lctrl_joy_dir_hint(int idx)
{
    int want;
    if (idx < 0 || idx >= LCTRL_ELEM_N) {
        return NULL;
    }
    if (kCtrlElems[idx].kind == LCTRL_POS || kCtrlElems[idx].kind == LCTRL_NEG) {
        return LT("gui.joy_hint_dig"); /* analoge Aktion empfehlen */
    }
    if (kCtrlElems[idx].kind == LCTRL_DIG && joy_is_gear_direct(idx)) {
        return LT("gui.joy_hint_gear");
    }
    want = lctrl_joy_dir_expect(idx);
    if (!want) {
        return NULL;
    }
    if (strncmp(kCtrlElems[idx].key, "pedal", 5) == 0) {
        return LT("gui.joy_hint_pedal");
    }
    return LT(want < 0 ? "gui.joy_hint_fwd" : "gui.joy_hint_right");
}

const char *lctrl_chord_plus(const char *s)
{
    const char *p;
    if (!s) {
        return NULL;
    }
    for (p = strchr(s, '+'); p; p = strchr(p + 1, '+')) {
        if (p[1] && p[1] != ':' && p[1] != '+') {
            return p;
        }
    }
    return NULL;
}

static int joy_pct(int v)
{
    return (v + 32768) * 100 / 65535;
}

int lctrl_joy_learn(const imap_joy_state_t *base, const imap_joy_state_t *prev,
                    const imap_joy_state_t *cur, unsigned axmask, int npov,
                    int idx, char *out, size_t cap)
{
    int is_axis, i, best = -1, bestd = 0;
    if (!base || !prev || !cur || !out || idx < 0 || idx >= LCTRL_ELEM_N) {
        return 0;
    }
    out[0] = '\0';
    is_axis = kCtrlElems[idx].kind == LCTRL_AXIS;
    if (!is_axis) {
        for (i = 0; i < IJOY_BTNS; i++) { /* neue Taste (Flanke) */
            if (joy_btn(cur, i) && !joy_btn(prev, i)) {
                _snprintf_s(out, cap, _TRUNCATE, "B%d", i + 1);
                return 1;
            }
        }
        if (npov > IJOY_POVS) { npov = IJOY_POVS; }
        for (i = 0; i < npov; i++) { /* neue POV-Richtung (8 -> 4 Wege) */
            int c = cur->pov[i], p = prev->pov[i];
            if (c >= 0 && c < 36000) {
                int d = ((c + 4500) / 9000) % 4;
                int pd = (p >= 0 && p < 36000) ? ((p + 4500) / 9000) % 4 : -1;
                if (d != pd) {
                    _snprintf_s(out, cap, _TRUNCATE, "POV%d%c", i + 1,
                                "URDL"[d]);
                    return 2;
                }
            }
        }
    }
    for (i = 0; i < IJOY_AXES; i++) { /* groesste Auslenkung ab Ruhestellung */
        int d;
        if (!(axmask & (1u << i))) { continue; }
        d = (int)cur->axis[i] - (int)base->axis[i];
        if (iabs_(d) > iabs_(bestd)) {
            bestd = d;
            best = i;
        }
    }
    if (best < 0 || iabs_(bestd) < 26214) { /* 40 % des Wegs */
        return 0;
    }
    {
        const char *an = ijoy_axis_name(best);
        int pct = joy_pct(cur->axis[best]);
        int rest = joy_pct(base->axis[best]);
        int lo = -1, hi = -1;
        if (is_axis) {
            /* W226: Bewegung gegen die verlangte Richtung -> inv=1 (alle
             * Achsen-Elemente). Pedal aus Mittelstellung (kombinierte
             * Ruderachse, Stick als Pedal) -> nur diese Haelfte: half=1. */
            int pedal = strncmp(kCtrlElems[idx].key, "pedal", 5) == 0;
            int want = lctrl_joy_dir_expect(idx);
            int centered = iabs_((int)base->axis[best]) < 8192;
            _snprintf_s(out, cap, _TRUNCATE, "%s:dz=0.05%s%s", an,
                        (bestd < 0 ? -1 : 1) != want ? ":inv=1" : "",
                        pedal && centered ? ":half=1" : "");
            return 3;
        }
        if (joy_is_gear_direct(idx)) {
            /* Gang-Direktwahl: Zone um die gehaltene Stellung (Schubhebel) */
            lo = pct - 12 < 0 ? 0 : pct - 12;
            hi = pct + 12 > 100 ? 100 : pct + 12;
        } else if (bestd > 0) {
            /* W226: Taste/.pos/.neg = Richtung, nie Zone um die Ruhe */
            if (pct >= 75 && rest < 75) {
                _snprintf_s(out, cap, _TRUNCATE, "%s+", an);
                return 3;
            }
            lo = (rest + pct) / 2; /* halber Weg Ruhe -> Spitze */
            hi = 100;
        } else {
            if (pct <= 25 && rest > 25) {
                _snprintf_s(out, cap, _TRUNCATE, "%s-", an);
                return 3;
            }
            lo = 0;
            hi = (rest + pct + 1) / 2;
        }
        if (lo >= hi || (rest >= lo && (rest < hi || hi >= 100))) {
            return 4; /* Zone enthielte die Ruhestellung: abgelehnt */
        }
        _snprintf_s(out, cap, _TRUNCATE, "%s:zone=%d-%d", an, lo, hi);
    }
    return 3;
}

void lctrl_joy_learner_reset(lctrl_joy_learner_t *L)
{
    if (L) {
        memset(L, 0, sizeof *L);
        L->cand = -1;
    }
}

int lctrl_joy_learner_step(lctrl_joy_learner_t *L, int ok,
                           const imap_joy_state_t *cur, unsigned axmask,
                           int npov, int idx, char *out, size_t cap)
{
    int i, cand = -1, cd = 0, moving, released, r;
    imap_joy_state_t pos;
    if (!L || !cur || !out || idx < 0 || idx >= LCTRL_ELEM_N) {
        return 0;
    }
    out[0] = '\0';
    if (!ok) { /* kein gueltiger Stand: nie lernen, Basis neu einschwingen */
        if (!L->have_base) { L->settle = 0; }
        return 0;
    }
    if (!L->have_base) {
        int still = L->settle > 0;
        for (i = 0; i < IJOY_AXES && still; i++) {
            if ((axmask & (1u << i)) &&
                iabs_((int)cur->axis[i] - (int)L->prev.axis[i]) > 655) {
                still = 0;
            }
        }
        L->settle = still ? L->settle + 1 : 1;
        L->prev = *cur;
        if (L->settle >= 3) { /* 3 ruhige Abtastungen (~150 ms) */
            int n = 0, same = 1, v0 = 0;
            L->base = *cur;
            L->have_base = 1;
            for (i = 0; i < IJOY_AXES; i++) {
                L->peak[i] = cur->axis[i];
                L->peakd[i] = 0;
                if (axmask & (1u << i)) {
                    if (n++ == 0) { v0 = cur->axis[i]; }
                    else if (cur->axis[i] != v0) { same = 0; }
                }
            }
            /* Alle Achsen exakt gleich (z. B. Startwert vor dem ersten
             * Geraete-Bericht): Basis je Achse erst mit der ersten
             * Aenderung bestaetigen (diese zaehlt nicht als Bewegung). */
            L->unconf = (n >= 2 && same) ? axmask : 0u;
            L->unext = 0u;
            for (i = 0; i < IJOY_AXES; i++) {
                if ((axmask & ~L->unconf & (1u << i)) &&
                    (cur->axis[i] == -32768 || cur->axis[i] == 32767)) {
                    L->unext |= 1u << i;
                }
            }
        }
        return 0;
    }
    if (kCtrlElems[idx].kind != LCTRL_AXIS) { /* Taste/POV (Flanke) */
        r = lctrl_joy_learn(&L->base, &L->prev, cur, 0, npov, idx, out, cap);
        if (r == 1 || r == 2) {
            L->prev = *cur;
            return r;
        }
    }
    for (i = 0; i < IJOY_AXES; i++) {
        int d;
        if (!(axmask & (1u << i))) { continue; }
        if (L->unconf & (1u << i)) {
            if (cur->axis[i] != L->base.axis[i]) {
                L->base.axis[i] = cur->axis[i];
                L->peak[i] = cur->axis[i];
                L->unconf &= ~(1u << i);
            }
            continue;
        }
        if ((L->unext & (1u << i)) && cur->axis[i] != L->base.axis[i]) {
            L->unext &= ~(1u << i); /* erste Aenderung ab Endwert */
            if (iabs_((int)cur->axis[i] - (int)L->prev.axis[i]) > 16384) {
                L->base.axis[i] = cur->axis[i];
                L->peak[i] = cur->axis[i];
                continue;
            }
        }
        d = (int)cur->axis[i] - (int)L->base.axis[i];
        if (iabs_(d) > iabs_(L->peakd[i])) {
            L->peakd[i] = d;
            L->peak[i] = cur->axis[i];
        }
        if (iabs_(L->peakd[i]) > iabs_(cd)) {
            cd = L->peakd[i];
            cand = i;
        }
    }
    if (cand < 0 || iabs_(cd) < 26214) {
        L->stable = 0;
        L->prev = *cur;
        return 0;
    }
    if (cand != L->cand) {
        L->cand = cand;
        L->stable = 0;
    }
    moving = iabs_((int)cur->axis[cand] - (int)L->prev.axis[cand]) > 1310;
    released = iabs_((int)cur->axis[cand] - (int)L->base.axis[cand]) * 2 <
               iabs_(cd);
    L->stable = moving ? 0 : L->stable + 1;
    L->prev = *cur;
    if (!released && L->stable < 2) {
        return 0; /* Achse noch in Bewegung */
    }
    pos = L->base; /* nur die Kandidaten-Achse zaehlt */
    pos.axis[cand] = released ? L->peak[cand] : cur->axis[cand];
    r = lctrl_joy_learn(&L->base, &pos, &pos, 1u << cand, 0, idx, out, cap);
    if (r == 4) {
        L->peakd[cand] = 0; /* abgelehnt: neu ansetzen */
        L->stable = 0;
    }
    return r;
}

/* Spec-Text darf weder Kommentar ('#') noch Trenner enthalten. */
static int joy_spec_ok(const char *s)
{
    return s && *s && !strpbrk(s, "#;,=@{}\r\n") && strlen(s) < 80;
}

int lctrl_joy_slot_for(lctrl_bind_t *b, const ijoy_info_t *list, int n,
                       int dev)
{
    int s;
    if (!b || !list || dev < 0 || dev >= n) { return -1; }
    for (s = 0; s < IMAP_JOY_MAX; s++) { /* 1. passende joyN-Zeile */
        if (b->joy[s][0] && ijoy_match_list(list, n, b->joy[s], s) == dev) {
            return s;
        }
    }
    for (s = 0; s < IMAP_JOY_MAX; s++) { /* 2. implizit (N-tes Geraet) */
        if (!b->joy[s][0] && lctrl_bind_uses_joy_slot(b, s) &&
            ijoy_match_list(list, n, "", s) == dev) {
            return s;
        }
    }
    for (s = 0; s < IMAP_JOY_MAX; s++) { /* 3. freier Slot + neue Zeile */
        if (!b->joy[s][0] && !lctrl_bind_uses_joy_slot(b, s)) {
            const char *nm = list[dev].product[0] ? list[dev].product
                                                  : list[dev].name;
            char spec[96];
            int k;
            spec[0] = '\0';
            if (joy_spec_ok(nm)) {
                for (k = 1; k <= n && !spec[0]; k++) {
                    char t[96];
                    if (k == 1) {
                        strcpy_s(t, sizeof t, nm);
                    } else {
                        _snprintf_s(t, sizeof t, _TRUNCATE, "%s@%d", nm, k);
                    }
                    if (ijoy_match_list(list, n, t, s) == dev) {
                        strcpy_s(spec, sizeof spec, t);
                    }
                }
            }
            if (!spec[0]) { /* Name unbrauchbar/mehrdeutig: Instanz-GUID */
                strcpy_s(spec, sizeof spec, list[dev].guid);
            }
            strcpy_s(b->joy[s], sizeof b->joy[0], spec);
            return s;
        }
    }
    return -1;
}

int lctrl_joy_is_axis(const char *alt, int *slot, int *axis)
{
    char nm[16];
    const char *p, *c;
    int pl, s = 0, ax;
    size_t n;
    if (!alt) { return 0; }
    while (*alt == ' ' || *alt == '\t') { alt++; }
    pl = joy_prefix(alt, &s);
    if (!pl || strchr(alt, '+') || strstr(alt, "zone=")) { return 0; }
    p = alt + pl;
    c = strchr(p, ':');
    n = c ? (size_t)(c - p) : strlen(p);
    if (n == 0 || n >= sizeof nm) { return 0; }
    memcpy(nm, p, n);
    nm[n] = '\0';
    trim(nm);
    if (nm[0] && nm[strlen(nm) - 1] == '-') { return 0; }
    ax = ijoy_axis_parse(nm);
    if (ax < 0) { return 0; }
    if (slot) { *slot = s; }
    if (axis) { *axis = ax; }
    return 1;
}

int lctrl_joy_src_axis(const char *alt, int *slot, int *axis)
{
    char nm[16];
    const char *p, *c;
    int pl, s = 0, ax;
    size_t n;
    if (!alt) { return 0; }
    while (*alt == ' ' || *alt == '\t') { alt++; }
    pl = joy_prefix(alt, &s);
    if (!pl || lctrl_chord_plus(alt)) { return 0; }
    p = alt + pl;
    c = strchr(p, ':');
    n = c ? (size_t)(c - p) : strlen(p);
    if (n == 0 || n >= sizeof nm) { return 0; }
    memcpy(nm, p, n);
    nm[n] = '\0';
    trim(nm);
    n = strlen(nm);
    if (n > 1 && (nm[n - 1] == '+' || nm[n - 1] == '-')) {
        nm[n - 1] = '\0';
        ax = ijoy_axis_parse(nm);
        if (ax < 0) { return 0; }
        if (slot) { *slot = s; }
        if (axis) { *axis = ax; }
        return 2;
    }
    ax = ijoy_axis_parse(nm);
    if (ax < 0) { return 0; }
    if (slot) { *slot = s; }
    if (axis) { *axis = ax; }
    return strstr(alt, "zone=") ? 2 : 1;
}

int lctrl_joy_eval(const char *alt, int pedal, int raw, float *res)
{
    char v[24];
    int kind = lctrl_joy_src_axis(alt, NULL, NULL);
    int inv = lctrl_joy_param_get(alt, "inv", v, sizeof v) && atoi(v) != 0;
    float x, m, sg, dz = 0.0f, curve = 1.0f, sens = 1.0f;
    if (raw < -32768) { raw = -32768; }
    if (raw > 32767) { raw = 32767; }
    if (res) { *res = 0.0f; }
    if (kind == 2) { /* Achse als Taste: wie imap_joy_atom_active */
        int lo = 75, hi = 100;
        float pct = ((float)raw + 32768.0f) * 100.0f / 65535.0f;
        if (lctrl_joy_param_get(alt, "zone", v, sizeof v)) {
            const char *mi = strchr(v, '-');
            lo = atoi(v);
            hi = mi ? atoi(mi + 1) : 100;
        } else {
            const char *p = alt, *c, *e;
            while (*p == ' ' || *p == '\t') { p++; }
            p += joy_prefix(p, NULL);
            c = strchr(p, ':');
            e = c ? c : p + strlen(p);
            if (e > p && e[-1] == '-') {
                lo = 0;
                hi = 25;
            }
        }
        if (inv) { pct = 100.0f - pct; }
        if (res) {
            *res = (pct >= (float)lo &&
                    (pct < (float)hi || (hi >= 100 && pct <= 100.0f)))
                       ? 1.0f : 0.0f;
        }
        return 2;
    }
    if (kind != 1) { return 0; }
    if (lctrl_joy_param_get(alt, "dz", v, sizeof v)) { dz = (float)atof(v); }
    if (lctrl_joy_param_get(alt, "curve", v, sizeof v)) { curve = (float)atof(v); }
    if (lctrl_joy_param_get(alt, "sens", v, sizeof v)) { sens = (float)atof(v); }
    /* wie imap_joy_value (input_map.c): inv, Totzone, Kurve, sens */
    x = raw < 0 ? (float)raw / 32768.0f : (float)raw / 32767.0f;
    if (inv) { x = -x; }
    if (pedal) {
        int half = lctrl_joy_param_get(alt, "half", v, sizeof v) && atoi(v);
        m = half ? (x < 0 ? 0.0f : x) : (x + 1.0f) * 0.5f;
        sg = 1.0f;
    } else {
        sg = x < 0 ? -1.0f : 1.0f;
        m = x < 0 ? -x : x;
    }
    if (m < dz) { return 1; }
    if (dz > 0.0f && dz < 1.0f) { m = (m - dz) / (1.0f - dz); }
    if (curve != 1.0f && curve > 0.0f) { m = powf(m, curve); }
    if (sens > 0.0f && sens != 1.0f) { m *= sens; }
    if (m > 1.0f) { m = 1.0f; }
    if (res) { *res = sg * m; }
    return 1;
}

int lctrl_joy_param_get(const char *alt, const char *key, char *out,
                        size_t cap)
{
    const char *p = alt;
    size_t kl = strlen(key);
    if (out && cap) { out[0] = '\0'; }
    while (p && (p = strchr(p, ':')) != NULL) {
        p++;
        if (_strnicmp(p, key, kl) == 0 && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t n = strcspn(v, ":+");
            if (out && cap) {
                if (n >= cap) { n = cap - 1; }
                memcpy(out, v, n);
                out[n] = '\0';
            }
            return 1;
        }
    }
    return 0;
}

int lctrl_joy_param_set(char *alt, size_t cap, const char *key,
                        const char *val)
{
    char res[160], seg[96];
    const char *p = alt;
    size_t kl = strlen(key), o = 0;
    int first = 1, done = 0;
    res[0] = '\0';
    while (*p) { /* Segmente ':'-getrennt; Typ/Name bleiben */
        size_t n = strcspn(p, ":");
        if (n >= sizeof seg) { n = sizeof seg - 1; }
        memcpy(seg, p, n);
        seg[n] = '\0';
        if (first || !(_strnicmp(seg, key, kl) == 0 && seg[kl] == '=')) {
            o += (size_t)_snprintf_s(res + o, sizeof res - o, _TRUNCATE,
                                     "%s%s", first ? "" : ":", seg);
            first = 0;
        } else if (val && *val && !done) { /* an alter Stelle ersetzen */
            o += (size_t)_snprintf_s(res + o, sizeof res - o, _TRUNCATE,
                                     ":%s=%s", key, val);
            done = 1;
        }
        p += n;
        if (*p == ':') { p++; }
    }
    if (val && *val && !done) {
        o += (size_t)_snprintf_s(res + o, sizeof res - o, _TRUNCATE, ":%s=%s",
                                 key, val);
    }
    if (strlen(res) >= cap) { return 1; }
    strcpy_s(alt, cap, res);
    return 0;
}

static int joy_atom_text_raw(const char *atom, char *out, size_t cap);

/* W226: wie bisher, invertierte Achsen-Quellen mit Zusatz "(invertiert)". */
int lctrl_joy_atom_text(const char *atom, char *out, size_t cap)
{
    char v[8];
    int r = joy_atom_text_raw(atom, out, cap);
    if (r && lctrl_joy_src_axis(atom, NULL, NULL) &&
        lctrl_joy_param_get(atom, "inv", v, sizeof v) && atoi(v) != 0) {
        size_t n = strlen(out);
        _snprintf_s(out + n, cap - n, _TRUNCATE, " %s", LT("gui.joy_src_inv"));
    }
    return r;
}

static int joy_atom_text_raw(const char *atom, char *out, size_t cap)
{
    static const char *const dk[4] = { "gui.joy_dir_u", "gui.joy_dir_r",
                                       "gui.joy_dir_d", "gui.joy_dir_l" };
    char nm[32], zone[24];
    const char *p, *c;
    int s = 0, pl, ax, n;
    size_t l;
    while (*atom == ' ' || *atom == '\t') { atom++; }
    pl = joy_prefix(atom, &s);
    if (!pl) { return 0; }
    p = atom + pl;
    c = strchr(p, ':');
    l = c ? (size_t)(c - p) : strlen(p);
    if (l >= sizeof nm) { l = sizeof nm - 1; }
    memcpy(nm, p, l);
    nm[l] = '\0';
    if ((nm[0] == 'B' || nm[0] == 'b') && nm[1] >= '0' && nm[1] <= '9') {
        n = atoi(nm + 1);
        _snprintf_s(out, cap, _TRUNCATE, LT("gui.joy_src_btn"), s + 1, n);
        return 1;
    }
    if (_strnicmp(nm, "POV", 3) == 0 && nm[3] >= '1' && nm[3] <= '4' &&
        nm[4] && strchr("URDLurdl", nm[4])) {
        int d = (int)(strchr("URDLurdl", nm[4]) - "URDLurdl") % 4;
        _snprintf_s(out, cap, _TRUNCATE, LT("gui.joy_src_pov"), s + 1,
                    nm[3] - '0', LT(dk[d]));
        return 1;
    }
    l = strlen(nm);
    if (l > 1 && (nm[l - 1] == '+' || nm[l - 1] == '-')) {
        char sign = nm[l - 1];
        nm[l - 1] = '\0';
        ax = ijoy_axis_parse(nm);
        if (ax >= 0) {
            _snprintf_s(out, cap, _TRUNCATE, LT("gui.joy_src_axdir"), s + 1,
                        ijoy_axis_name(ax), sign == '+' ? "+" : "-");
            return 1;
        }
        nm[l - 1] = sign;
    }
    ax = ijoy_axis_parse(nm);
    if (ax >= 0 && lctrl_joy_param_get(atom, "zone", zone, sizeof zone)) {
        _snprintf_s(out, cap, _TRUNCATE, LT("gui.joy_src_zone"), s + 1,
                    ijoy_axis_name(ax), zone);
        return 1;
    }
    if (ax >= 0) {
        _snprintf_s(out, cap, _TRUNCATE, LT("gui.joy_src_axis"), s + 1,
                    ijoy_axis_name(ax));
        return 1;
    }
    _snprintf_s(out, cap, _TRUNCATE, "%s", atom); /* unbekannt: roh */
    return 1;
}

/* --- input_selftest --dump pruefen ------------------------------------- */

int lctrl_run_dump(const char *client_dir, const char *profile, char *out,
                   size_t cap)
{
    char exe[1200], cmd[1400], tmp[1024];
    void *proc = NULL;
    DWORD code = 1, attr;
    FILE *f = NULL;
    char line[512];
    int lines = 0;

    _snprintf_s(exe, sizeof exe, _TRUNCATE, "%s/input_selftest.exe",
                client_dir);
    attr = u8_GetFileAttributes(exe);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        _snprintf_s(out, cap, _TRUNCATE, LT("gui.bind_nodump"), exe);
        return 2;
    }
    u8_GetTempPath(tmp, sizeof tmp);
    strcat_s(tmp, sizeof tmp, "w78_dump_check.txt");
    _snprintf_s(cmd, sizeof cmd, _TRUNCATE, "\"%s\" --dump \"%s\"", exe,
                profile);
    /* Ausgabe in Temp-Datei umleiten (Handle vererbt). W82: W-API. */
    {
        SECURITY_ATTRIBUTES sa;
        HANDLE h;
        wchar_t wtmp[1024];
        memset(&sa, 0, sizeof sa);
        sa.nLength = sizeof sa;
        sa.bInheritHandle = TRUE;
        u8_to_w(tmp, wtmp, 1024);
        h = CreateFileW(wtmp, GENERIC_WRITE, FILE_SHARE_READ, &sa,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            strcpy_s(out, cap, LT("gui.bind_notmp"));
            return 2;
        }
        if (!u8_CreateProcess(cmd, client_dir, CREATE_NO_WINDOW, 1, h, 1,
                              NULL, &proc)) {
            DWORD e = GetLastError();
            CloseHandle(h);
            _snprintf_s(out, cap, _TRUNCATE, LT("gui.bind_norun"), e);
            return 2;
        }
        CloseHandle(h);
    }
    WaitForSingleObject((HANDLE)proc, 15000);
    GetExitCodeProcess((HANDLE)proc, &code);
    CloseHandle((HANDLE)proc);
    if (u8_fopen_s(&f, tmp, "r") == 0 && f) {
        while (fgets(line, sizeof line, f)) {
            lines++;
        }
        fclose(f);
        u8_DeleteFile(tmp);
    }
    if (code == 0) {
        _snprintf_s(out, cap, _TRUNCATE, LT("gui.bind_dumpok"), profile,
                    lines);
        return 0;
    }
    _snprintf_s(out, cap, _TRUNCATE, LT("gui.bind_dumperr"), code, lines);
    return 1;
}
