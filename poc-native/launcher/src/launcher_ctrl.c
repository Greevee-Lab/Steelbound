/* launcher_ctrl.c - Steuerungs-Helfer des Launchers (W78, CTRL-UI).
 * ASCII only. Alle Nutzertexte deutsch. Nutzt das unveraenderte
 * Client-Modul input_sbc_usb.c (nur einkompiliert, nicht geaendert). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
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
    for (i = 0; i < LCTRL_ELEM_N; i++) {
        const char *want =
            kCtrlElems[i].kind == LCTRL_ACT ? "vr" : /* W159 */
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
