/* launcher_ctrl.c - Steuerungs-Helfer des Launchers (W78, CTRL-UI).
 * ASCII only. Alle Nutzertexte deutsch. Nutzt das unveraenderte
 * Client-Modul input_sbc_usb.c (nur einkompiliert, nicht geaendert). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include "launcher_ctrl.h"
#include "launcher_cfg.h"
#include "launcher_u8.h"
#include "sb_lang.h"
#include "input_sbc_usb.h"
#include "launcher_assist.h" /* W419: user.cfg-Zeilen lesen/schreiben */

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
                    strcmp(name, "xbox_pad_default") == 0 ||
                    strcmp(name, "default_full") == 0); /* W431 */
}

int lctrl_vk_is_gamepad(int vk)
{
    return vk >= 0xC3 && vk <= 0xDA; /* VK_GAMEPAD_A .. RIGHT_THUMBSTICK_LEFT */
}

int lctrl_vk_is_modifier(int vk)
{
    return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_CONTROL ||
           vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_MENU || vk == VK_LMENU ||
           vk == VK_RMENU;
}

int lctrl_bind_combo_row(int idx)
{
    /* W365 BIND-COMBO: jede digitale Zeile (Tasten, Achsen-Endlagen, Gang/Tuner, Client-Aktionen);
     * analoge Achsen bleiben Einzelatome. */
    return idx >= 0 && idx < LCTRL_ELEM_N && kCtrlElems[idx].kind != LCTRL_AXIS;
}

int lctrl_bind_key_combo_row(int idx)
{
    /* W365: pause/feedback_* wertet das Fenster nur als einfache Taste aus (imap_bind_keys),
     * Pad-/Controller-Kombis gehen dort ueber die Engine. */
    return lctrl_bind_combo_row(idx) && strcmp(kCtrlElems[idx].key, "pause") != 0 &&
           strcmp(kCtrlElems[idx].key, "feedback_shot") != 0 && strcmp(kCtrlElems[idx].key, "feedback_clip") != 0;
}

int lctrl_bind_mouse_combo_row(int idx)
{
    /* W365: Zusatztaste + Maustaste nur, wo die Engine mit echten Tasten auswertet (Spiel-Elemente,
     * vr_recenter, aim_center); photo/quick_shot/orig_view kennen im Fenster nur Zusatz+Taste,
     * replay_exit ignoriert die Maus. */
    if (!lctrl_bind_combo_row(idx)) {
        return 0;
    }
    return kCtrlElems[idx].kind != LCTRL_ACT || strcmp(kCtrlElems[idx].key, "vr_recenter") == 0 ||
           strcmp(kCtrlElems[idx].key, "aim_center") == 0;
}

int lctrl_bind_empty_line_row(int idx)
{
    return idx >= 0 && idx < LCTRL_ELEM_N &&
           (strcmp(kCtrlElems[idx].key, "orig_view") == 0 || strcmp(kCtrlElems[idx].key, "quick_shot") == 0 ||
            strcmp(kCtrlElems[idx].key, "replay_exit") == 0 || /* W299: leer = aus (sonst Standard) */
            strcmp(kCtrlElems[idx].key, "net_hud") == 0 ||     /* W374 */
            strcmp(kCtrlElems[idx].key, "menu_back") == 0 ||   /* W438 */
            strcmp(kCtrlElems[idx].key, "chat") == 0);         /* W437 */
}

void lctrl_key_combo(int mod_vk, int vk, char *out, size_t cap)
{
    char k[32];
    const char *m = NULL;
    lctrl_key_name(vk, k, sizeof k);
    if (mod_vk == VK_SHIFT || mod_vk == VK_LSHIFT || mod_vk == VK_RSHIFT) { m = "SHIFT"; }
    else if (mod_vk == VK_CONTROL || mod_vk == VK_LCONTROL || mod_vk == VK_RCONTROL) { m = "CTRL"; }
    else if (mod_vk == VK_MENU || mod_vk == VK_LMENU || mod_vk == VK_RMENU) { m = "ALT"; }
    if (m && !lctrl_vk_is_modifier(vk)) {
        _snprintf_s(out, cap, _TRUNCATE, "KEY:%s+KEY:%s", m, k);
    } else {
        _snprintf_s(out, cap, _TRUNCATE, "KEY:%s", k);
    }
}

int lctrl_user_rest_of(const char *profile, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!profile || strncmp(profile, "user_", 5) != 0 ||
        !lctrl_user_rest_ok(profile + 5) || strlen(profile + 5) >= cap) {
        return 0;
    }
    strcpy_s(out, cap, profile + 5);
    return 1;
}

/* --- SBC-USB-Status ---------------------------------------------------- */

int lctrl_sbc_status(char *out, size_t cap)
{
    sbc_usb_dev_t *dev = NULL;
    int rc = sbc_usb_open(&dev);
    if (rc == SBC_OK) {
        sbc_usb_close(dev);
        strncpy_s(out, cap, LT("gui.sbc_ok"), _TRUNCATE);
        return 0;
    }
    if (rc == SBC_ERR_DRIVER) {
        /* W128: Hinweis auf Zadig + gefuehrte Pruefung sbc_probe --test */
        strncpy_s(out, cap, LT("gui.sbc_driver"), _TRUNCATE);
        return rc;
    }
    if (rc == SBC_ERR_NODEVICE) {
        strncpy_s(out, cap, LT("gui.sbc_none"), _TRUNCATE);
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
/* W312 JOY-CALIB: Pad-Lerner mit Ruhestellung (s. launcher_ctrl.h). */
void lctrl_pad_learner_reset(lctrl_pad_learner_t *L)
{
    if (L) {
        memset(L, 0, sizeof *L);
    }
}

int lctrl_pad_learner_axis(lctrl_pad_learner_t *L, const lctrl_pad_t *cur,
                           char *name, size_t cap)
{
    double v[6];
    int hot = -1, i;
    if (!L || !cur || !name) {
        return 0;
    }
    if (!L->have_base) {
        int still = L->settle > 0 &&
                    abs(cur->lx - L->prev.lx) <= 2048 && abs(cur->ly - L->prev.ly) <= 2048 &&
                    abs(cur->rx - L->prev.rx) <= 2048 && abs(cur->ry - L->prev.ry) <= 2048 &&
                    abs((int)cur->lt - (int)L->prev.lt) <= 8 &&
                    abs((int)cur->rt - (int)L->prev.rt) <= 8;
        L->settle = still ? L->settle + 1 : 1;
        L->prev = *cur;
        if (L->settle >= 3) {
            L->base = *cur;
            L->have_base = 1;
        }
        return 0;
    }
    L->prev = *cur;
    /* W432: Auslenkung je Kanal als Anteil des vollen Bereichs (Stick
     * 65535, Trigger 255), staerkster Kanal muss >= 20 % sein und jeden
     * anderen um mindestens das Doppelte uebertreffen. */
    v[0] = (cur->lx - L->base.lx) / 65535.0;
    v[1] = (cur->ly - L->base.ly) / 65535.0;
    v[2] = (cur->rx - L->base.rx) / 65535.0;
    v[3] = (cur->ry - L->base.ry) / 65535.0;
    v[4] = ((int)cur->lt - (int)L->base.lt) / 255.0;
    v[5] = ((int)cur->rt - (int)L->base.rt) / 255.0;
    for (i = 0; i < 6; i++) {
        if (hot < 0 || dabs(v[i]) > dabs(v[hot])) {
            hot = i;
        }
    }
    if (hot < 0 || dabs(v[hot]) < 0.2) {
        return 0;
    }
    for (i = 0; i < 6; i++) {
        if (i != hot && dabs(v[i]) * 2.0 > dabs(v[hot])) {
            return 0; /* zwei Kanaele etwa gleich weit: uneindeutig */
        }
    }
    {
        static const char *nm[6] = { "LX", "LY", "RX", "RY", "LT", "RT" };
        strcpy_s(name, cap, nm[hot]);
    }
    return 1;
}

int lctrl_axis_partner(int idx)
{
    static const char *const pr[2][2] = { { "aim_x", "aim_y" }, { "sight_x", "sight_y" } };
    int p, k, j;
    if (idx < 0 || idx >= LCTRL_ELEM_N) {
        return -1;
    }
    for (p = 0; p < 2; p++) {
        for (k = 0; k < 2; k++) {
            if (strcmp(kCtrlElems[idx].key, pr[p][k]) == 0) {
                for (j = 0; j < LCTRL_ELEM_N; j++) {
                    if (strcmp(kCtrlElems[j].key, pr[p][1 - k]) == 0) {
                        return j;
                    }
                }
            }
        }
    }
    return -1;
}

/* "PAD:LX:dz=.." -> "PAD:LX", "JOY0:X:.." / "JOY:X" -> "JOY0:X"; "" sonst
 * (Achse als Taste "Z+"/Zone zaehlt nicht als Analogachse). */
static void alt_axis_key(const char *a, char *out, size_t cap)
{
    int slot = 0, axis = -1;
    out[0] = '\0';
    if (!a) {
        return;
    }
    while (*a == ' ' || *a == '\t') {
        a++;
    }
    if (_strnicmp(a, "PAD:", 4) == 0) {
        static const char *const st[4] = { "LX", "LY", "RX", "RY" };
        int k;
        for (k = 0; k < 4; k++) {
            if (_strnicmp(a + 4, st[k], 2) == 0 && (a[6] == '\0' || a[6] == ':')) {
                _snprintf_s(out, cap, _TRUNCATE, "PAD:%s", st[k]);
            }
        }
        return;
    }
    if (lctrl_joy_is_axis(a, &slot, &axis) && axis >= 0) {
        _snprintf_s(out, cap, _TRUNCATE, "JOY%d:%s", slot < 0 ? 0 : slot, ijoy_axis_name(axis));
    }
}

int lctrl_alt_same_axis(const char *a, const char *b)
{
    char ka[32], kb[32];
    alt_axis_key(a, ka, sizeof ka);
    alt_axis_key(b, kb, sizeof kb);
    return ka[0] && strcmp(ka, kb) == 0;
}

int lctrl_bind_axis_used_by(const lctrl_bind_t *b, int idx, const char *src)
{
    int a;
    if (!b || idx < 0 || idx >= LCTRL_ELEM_N || !src) {
        return 0;
    }
    for (a = 0; a < b->row[idx].n_alt; a++) {
        if (lctrl_alt_same_axis(b->row[idx].alt[a], src)) {
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
        { VK_SNAPSHOT, "SNAPSHOT" }, /* W290: Druck-Taste (Schnell-Screenshot) */
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

/* --- W365 BIND-KEYNAMES: Anzeigenamen (nur Anzeige, Profil bleibt kanonisch) --- */

int lctrl_key_vk(const char *name)
{
    static const struct { const char *n; int vk; } tab[] = {
        { "SPACE", VK_SPACE }, { "ENTER", VK_RETURN }, { "RETURN", VK_RETURN }, { "TAB", VK_TAB },
        { "SHIFT", VK_SHIFT }, { "CTRL", VK_CONTROL }, { "CONTROL", VK_CONTROL }, { "LCTRL", VK_LCONTROL },
        { "RCTRL", VK_RCONTROL }, { "ALT", VK_MENU }, { "ESC", VK_ESCAPE }, { "ESCAPE", VK_ESCAPE },
        { "LEFT", VK_LEFT }, { "RIGHT", VK_RIGHT }, { "UP", VK_UP }, { "DOWN", VK_DOWN },
        { "BACKSPACE", VK_BACK }, { "INSERT", VK_INSERT }, { "DELETE", VK_DELETE }, { "HOME", VK_HOME },
        { "END", VK_END }, { "PAGEUP", VK_PRIOR }, { "PAGEDOWN", VK_NEXT }, { "PAUSE", VK_PAUSE },
        { "SNAPSHOT", VK_SNAPSHOT }, { NULL, 0 }
    };
    char *end = NULL;
    long v;
    int i;
    if (!name || !name[0]) {
        return -1;
    }
    if (!name[1]) {
        char c = name[0];
        if (c >= 'a' && c <= 'z') { c = (char)(c - 32); }
        return ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : -1;
    }
    if ((name[0] == 'F' || name[0] == 'f') && name[1] >= '1' && name[1] <= '9') {
        v = strtol(name + 1, &end, 10);
        if (end && !*end && v >= 1 && v <= 12) {
            return VK_F1 + (int)v - 1;
        }
    }
    for (i = 0; tab[i].n; i++) {
        if (_stricmp(name, tab[i].n) == 0) {
            return tab[i].vk;
        }
    }
    if (name[0] >= '0' && name[0] <= '9') {
        v = strtol(name, &end, 0);
        if (end && !*end && v >= 0 && v < 512) {
            return (int)v;
        }
    }
    return -1;
}

void lctrl_key_disp(int vk, char *out, size_t cap)
{
    static const struct { int vk; const char *n; } tab[] = {
        { VK_SPACE, "Space" }, { VK_RETURN, "Enter" }, { VK_TAB, "Tab" }, { VK_ESCAPE, "Esc" },
        { VK_BACK, "Backspace" }, { VK_CAPITAL, "Caps Lock" }, { VK_NUMLOCK, "Num Lock" },
        { VK_SCROLL, "Scroll Lock" }, { VK_PAUSE, "Pause" }, { VK_SNAPSHOT, "Print Screen" },
        { VK_INSERT, "Insert" }, { VK_DELETE, "Delete" }, { VK_HOME, "Home" }, { VK_END, "End" },
        { VK_PRIOR, "Page Up" }, { VK_NEXT, "Page Down" }, { VK_CLEAR, "Clear" },
        { VK_LEFT, "Left" }, { VK_RIGHT, "Right" }, { VK_UP, "Up" }, { VK_DOWN, "Down" },
        { VK_SHIFT, "Shift" }, { VK_LSHIFT, "Left Shift" }, { VK_RSHIFT, "Right Shift" },
        { VK_CONTROL, "Ctrl" }, { VK_LCONTROL, "Left Ctrl" }, { VK_RCONTROL, "Right Ctrl" },
        { VK_MENU, "Alt" }, { VK_LMENU, "Left Alt" }, { VK_RMENU, "Right Alt" },
        { VK_LWIN, "Left Win" }, { VK_RWIN, "Right Win" }, { VK_APPS, "Menu" },
        { VK_MULTIPLY, "Num *" }, { VK_ADD, "Num +" }, { VK_SUBTRACT, "Num -" }, { VK_DIVIDE, "Num /" },
        { VK_DECIMAL, "Num Del" }, { VK_SEPARATOR, "Num Sep" },
        { VK_VOLUME_MUTE, "Volume Mute" }, { VK_VOLUME_DOWN, "Volume Down" }, { VK_VOLUME_UP, "Volume Up" },
        { VK_MEDIA_NEXT_TRACK, "Next Track" }, { VK_MEDIA_PREV_TRACK, "Previous Track" },
        { VK_MEDIA_STOP, "Media Stop" }, { VK_MEDIA_PLAY_PAUSE, "Play/Pause" },
        { VK_LAUNCH_MAIL, "Mail" }, { VK_LAUNCH_MEDIA_SELECT, "Media Select" },
        { VK_LAUNCH_APP1, "App 1" }, { VK_LAUNCH_APP2, "App 2" },
        { VK_BROWSER_BACK, "Browser Back" }, { VK_BROWSER_FORWARD, "Browser Forward" },
        { VK_BROWSER_REFRESH, "Browser Refresh" }, { VK_BROWSER_STOP, "Browser Stop" },
        { VK_BROWSER_SEARCH, "Browser Search" }, { VK_BROWSER_FAVORITES, "Browser Favorites" },
        { VK_BROWSER_HOME, "Browser Home" }, { VK_SLEEP, "Sleep" },
        { 0, NULL }
    };
    /* OEM-Tasten: Zeichen nach Tastaturlayout (GetKeyNameText), sonst diese Rueckfallnamen. */
    static const struct { int vk; const char *n; } oem[] = {
        { VK_OEM_1, "OEM ;" }, { VK_OEM_PLUS, "OEM +" }, { VK_OEM_COMMA, "OEM ," }, { VK_OEM_MINUS, "OEM -" },
        { VK_OEM_PERIOD, "OEM ." }, { VK_OEM_2, "OEM /" }, { VK_OEM_3, "OEM `" }, { VK_OEM_4, "OEM [" },
        { VK_OEM_5, "OEM \\" }, { VK_OEM_6, "OEM ]" }, { VK_OEM_7, "OEM '" }, { VK_OEM_8, "OEM 8" },
        { VK_OEM_102, "OEM <>" }, { 0, NULL }
    };
    int i;
    if (!out || cap == 0) {
        return;
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        _snprintf_s(out, cap, _TRUNCATE, "%c", (char)vk);
        return;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        _snprintf_s(out, cap, _TRUNCATE, "F%d", vk - VK_F1 + 1);
        return;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        _snprintf_s(out, cap, _TRUNCATE, "Num %d", vk - VK_NUMPAD0);
        return;
    }
    for (i = 0; tab[i].n; i++) {
        if (tab[i].vk == vk) {
            strcpy_s(out, cap, tab[i].n);
            return;
        }
    }
    for (i = 0; oem[i].n; i++) {
        if (oem[i].vk == vk) {
            wchar_t w[32];
            UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
            int n = sc ? GetKeyNameTextW((LONG)(sc << 16), w, 32) : 0;
            if (n > 0 && WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) > 0) {
                return;
            }
            strcpy_s(out, cap, oem[i].n);
            return;
        }
    }
    _snprintf_s(out, cap, _TRUNCATE, LT("gui.key_unknown"), vk);
}

/* Pad-Anzeige: A/B/X/Y, LB/RB, LT/RT bleiben, der Rest mit Klarnamen. */
static const char *pad_disp_key(const char *nm)
{
    static const struct { const char *n, *k; } tab[] = {
        { "BACK", "gui.pad_back" }, { "START", "gui.pad_start" }, { "L3", "gui.pad_l3" },
        { "R3", "gui.pad_r3" }, { "DU", "gui.pad_du" }, { "DUP", "gui.pad_du" }, { "DD", "gui.pad_dd" },
        { "DDOWN", "gui.pad_dd" }, { "DL", "gui.pad_dl" }, { "DLEFT", "gui.pad_dl" },
        { "DR", "gui.pad_dr" }, { "DRIGHT", "gui.pad_dr" }, { NULL, NULL }
    };
    int i;
    for (i = 0; tab[i].n; i++) {
        if (_stricmp(nm, tab[i].n) == 0) {
            return tab[i].k;
        }
    }
    return NULL;
}

/* Ein Atom ("KEY:20", "PAD:L3", "JOY0:B3") -> Anzeige. 1 = Anzeige weicht vom Rohnamen ab. */
static int atom_pretty(const char *atom, char *out, size_t cap)
{
    char ty[16], nm[48];
    const char *c1, *c2;
    size_t n;
    while (*atom == ' ' || *atom == '\t') { atom++; }
    c1 = strchr(atom, ':');
    strncpy_s(out, cap, atom, _TRUNCATE);
    if (!c1 || (size_t)(c1 - atom) >= sizeof ty) {
        return 0;
    }
    memcpy(ty, atom, (size_t)(c1 - atom));
    ty[c1 - atom] = '\0';
    c2 = strchr(c1 + 1, ':');
    n = c2 ? (size_t)(c2 - c1 - 1) : strlen(c1 + 1);
    if (n >= sizeof nm) { n = sizeof nm - 1; }
    memcpy(nm, c1 + 1, n);
    nm[n] = '\0';
    while (n > 0 && (nm[n - 1] == ' ' || nm[n - 1] == '\t')) { nm[--n] = '\0'; }
    if (_stricmp(ty, "KEY") == 0) {
        int vk = lctrl_key_vk(nm);
        if (vk < 0) {
            return 0;
        }
        lctrl_key_disp(vk, out, cap);
        return _stricmp(out, nm) != 0;
    }
    if (_stricmp(ty, "PAD") == 0) {
        const char *k = pad_disp_key(nm);
        strncpy_s(out, cap, k ? LT(k) : nm, _TRUNCATE);
        return k != NULL;
    }
    if (_strnicmp(ty, "JOY", 3) == 0 && lctrl_joy_atom_text(atom, out, cap)) {
        return 1;
    }
    strncpy_s(out, cap, nm, _TRUNCATE);
    if (_stricmp(ty, "SBC") == 0 || _stricmp(ty, "MOUSE") == 0) {
        _snprintf_s(out, cap, _TRUNCATE, "%s:%s", ty, nm);
    }
    return 0;
}

int lctrl_src_pretty(const char *alt, char *out, size_t cap)
{
    char work[128], t[96];
    char *p, *plus;
    int diff = 0, first = 1;
    if (!out || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!alt) {
        return 0;
    }
    strncpy_s(work, sizeof work, alt, _TRUNCATE);
    p = work;
    for (;;) {
        plus = (char *)lctrl_chord_plus(p);
        if (plus) {
            *plus = '\0';
            diff = 1; /* Kombination: immer lesbar mit " + " zeigen */
        }
        if (atom_pretty(p, t, sizeof t)) {
            diff = 1;
        }
        if (!first) {
            strncat_s(out, cap, " + ", _TRUNCATE);
        }
        strncat_s(out, cap, t, _TRUNCATE);
        first = 0;
        if (!plus) {
            break;
        }
        p = plus + 1;
    }
    return diff;
}

/* --- W365 BIND-COMBO: Kombi-Lernen (rein, testbar) ---------------------- */

void lctrl_combo_reset(lctrl_combo_t *c)
{
    if (c) {
        c->pend[0] = '\0';
    }
}

int lctrl_combo_feed(lctrl_combo_t *c, const char *press, int pend_held, char *out, size_t cap)
{
    if (!c || !out || cap == 0) {
        return 0;
    }
    if (press && press[0]) {
        if (c->pend[0] && pend_held && _stricmp(c->pend, press) != 0) {
            _snprintf_s(out, cap, _TRUNCATE, "%s+%s", c->pend, press);
            c->pend[0] = '\0';
            return 1;
        }
        if (!c->pend[0]) {
            strncpy_s(c->pend, sizeof c->pend, press, _TRUNCATE);
            return 0;
        }
    }
    if (c->pend[0] && !pend_held) {
        strncpy_s(out, cap, c->pend, _TRUNCATE);
        c->pend[0] = '\0';
        return 1;
    }
    return 0;
}

int lctrl_pad_held(const lctrl_pad_t *p, const char *name)
{
    static const unsigned bits[] = {
        LCTRL_PB_A, LCTRL_PB_B, LCTRL_PB_X, LCTRL_PB_Y, LCTRL_PB_LB, LCTRL_PB_RB, LCTRL_PB_START,
        LCTRL_PB_BACK, LCTRL_PB_L3, LCTRL_PB_R3, LCTRL_PB_DU, LCTRL_PB_DD, LCTRL_PB_DL, LCTRL_PB_DR
    };
    size_t i;
    if (!p || !name) {
        return 0;
    }
    if (_strnicmp(name, "PAD:", 4) == 0) {
        name += 4;
    }
    if (_stricmp(name, "LT") == 0) { return p->lt > 128; }
    if (_stricmp(name, "RT") == 0) { return p->rt > 128; }
    for (i = 0; i < sizeof bits / sizeof bits[0]; i++) {
        const char *nm = lctrl_pad_button_name(bits[i]);
        if (nm && _stricmp(nm, name) == 0) {
            return (p->buttons & bits[i]) != 0;
        }
    }
    return 0;
}

int lctrl_pad_combo_step(lctrl_combo_t *c, const lctrl_pad_t *prev, const lctrl_pad_t *cur, char *out, size_t cap)
{
    lctrl_pad_t pv;
    char nm[16], at[24];
    int guard;
    if (!c || !prev || !cur) {
        return 0;
    }
    pv = *prev;
    /* Alle frischen Tasten dieses Takts (zwei gleichzeitig = Kombi in Reihenfolge der Bits). */
    for (guard = 0; guard < 16 && lctrl_pad_new_press(&pv, cur, nm, sizeof nm); guard++) {
        if (strcmp(nm, "LT") == 0) { pv.lt = cur->lt; }
        else if (strcmp(nm, "RT") == 0) { pv.rt = cur->rt; }
        else {
            size_t i;
            for (i = 0; i < 16; i++) {
                const char *bn = lctrl_pad_button_name(1u << i);
                if (bn && strcmp(bn, nm) == 0) { pv.buttons |= (unsigned short)(1u << i); }
            }
        }
        _snprintf_s(at, sizeof at, _TRUNCATE, "PAD:%s", nm);
        if (lctrl_combo_feed(c, at, c->pend[0] ? lctrl_pad_held(cur, c->pend) : 1, out, cap)) {
            return 1;
        }
    }
    return lctrl_combo_feed(c, NULL, c->pend[0] ? lctrl_pad_held(cur, c->pend) : 0, out, cap);
}

/* Modifier-Familie fuer den Konfliktvergleich (CTRL ~ LCTRL/RCTRL, SHIFT ~ L/R). */
static int key_family(const char *atom)
{
    int vk;
    if (_strnicmp(atom, "KEY:", 4) != 0) {
        return -1;
    }
    vk = lctrl_key_vk(atom + 4);
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) { return VK_SHIFT; }
    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) { return VK_CONTROL; }
    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) { return VK_MENU; }
    return vk < 0 ? -1 : 0x1000 + vk;
}

/* Atom ohne Parameter ("KEY:LEFT:cond=!SHIFT" -> "KEY:LEFT"), getrimmt. */
static void atom_core(const char *a, char *out, size_t cap)
{
    const char *c1, *c2;
    size_t n;
    while (*a == ' ' || *a == '\t') { a++; }
    c1 = strchr(a, ':');
    c2 = c1 ? strchr(c1 + 1, ':') : NULL;
    n = c2 ? (size_t)(c2 - a) : strlen(a);
    if (n >= cap) { n = cap - 1; }
    memcpy(out, a, n);
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t')) { out[--n] = '\0'; }
}

int lctrl_bind_combo_single_use(const lctrl_bind_t *b, const char *combo, char *atom_out, size_t acap,
                                int *row_out)
{
    char work[128], core[64], other[64];
    char *p, *plus;
    int r, a;
    if (!b || !combo || !lctrl_chord_plus(combo)) {
        return 0;
    }
    strncpy_s(work, sizeof work, combo, _TRUNCATE);
    p = work;
    for (;;) {
        int fam;
        plus = (char *)lctrl_chord_plus(p);
        if (plus) {
            *plus = '\0';
        }
        atom_core(p, core, sizeof core);
        fam = key_family(core);
        for (r = 0; r < LCTRL_ELEM_N; r++) {
            for (a = 0; a < b->row[r].n_alt; a++) {
                const char *alt = b->row[r].alt[a];
                if (lctrl_chord_plus(alt)) {
                    continue;
                }
                atom_core(alt, other, sizeof other);
                if (_stricmp(other, core) == 0 || (fam >= 0 && fam < 0x1000 && key_family(other) == fam)) {
                    if (atom_out && acap) {
                        strncpy_s(atom_out, acap, core, _TRUNCATE);
                    }
                    if (row_out) {
                        *row_out = r;
                    }
                    return 1;
                }
            }
        }
        if (!plus) {
            break;
        }
        p = plus + 1;
    }
    return 0;
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
    /* W290 QUICK-SHOT: Schnell-Screenshot mit den Foto-Einstellungen (Abschnitt [client], Standard F12;
     * photo seit W290 Standard Shift+F12). */
    { "quick_shot", "-", "bind.quick_shot", LCTRL_ACT },
    /* W271 STICK-RELATIVE: Client-Aktion Waffenarm zur Mitte (Abschnitt [client]). */
    { "aim_center", "-", "bind.aim_center", LCTRL_ACT },
    /* W283 ORIG-KEY-BIND: Vergleichstaste Original <-> eigene Einstellungen
     * (Abschnitt [client], Standard KEY:SHIFT+KEY:F8, Rueckfall orig_key). */
    { "orig_view", "-", "bind.orig_view", LCTRL_ACT },
    /* W299 REPLAY: laufendes Spiel-Replay beenden (Abschnitt [client], Standard Ruecktaste; die Maus
     * beendet Replays nicht, Pad/Original-Controller/Joystick wie im Original). */
    { "replay_exit", "-", "bind.replay_exit", LCTRL_ACT },
    /* W374 LOC-NET-OVERLAY: Netz-Overlay an/aus (Abschnitt [client], Standard F3; Umschalt + Taste = Ecke). */
    { "net_hud", "-", "bind.net_hud", LCTRL_ACT },
    /* W438 CTRL-RESET-040: Zurueck in den LoC-Menues (Taste B des virtuellen Menue-Pads, nur Tastatur). */
    { "menu_back", "-", "bind.menu_back", LCTRL_ACT },
    /* W437 LOC-CHAT: Chatzeile oeffnen (nur Line of Contact online, nur Tastatur; Standard Enter, falls frei). */
    { "chat", "-", "bind.chat", LCTRL_ACT },
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
        if (strlen(one) >= sizeof b->row[idx].alt[0]) { continue; } /* W332 SEC: zu lang (Feld 96) verwerfen */
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
        strncpy_s(out, cap, LT("gui.bind_unset"), _TRUNCATE);
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
static int bind_parse_line(char *line, lctrl_bind_t *b)
{
    char *h, *eq, *val, *one, *ctx = NULL;
    int idx;
    h = strchr(line, '#');
    if (h) { *h = '\0'; } /* Kommentar ab '#' (wie Client) */
    trim(line);
    if (!line[0] || line[0] == ';' || line[0] == '[') { return -1; }
    eq = strchr(line, '=');
    if (!eq) { return -1; }
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
        return -1;
    }
    idx = lctrl_bind_elem_index(line);
    if (idx < 0) { return -1; } /* unbekannt: ignorieren */
    val = eq + 1;
    b->row[idx].n_alt = 0; /* letzte Zeile gewinnt */
    for (one = strtok_s(val, ",", &ctx); one;
         one = strtok_s(NULL, ",", &ctx)) {
        trim(one);
        if (!one[0]) { continue; }
        /* W332 SEC: Profile werden geteilt; zu lange Alternative (Zeile bis 511, Feld 96) verwerfen statt
         * strcpy_s-Abbruch (Absturz beim Laden des Profils / der Steuerungsseite). */
        if (strlen(one) >= sizeof b->row[idx].alt[0]) { continue; }
        if (b->row[idx].n_alt >= LCTRL_ALT_N) { break; }
        strcpy_s(b->row[idx].alt[b->row[idx].n_alt],
                 sizeof b->row[idx].alt[0], one);
        b->row[idx].n_alt++;
    }
    return idx;
}

/* W283: Profil ohne orig_view-Zeile (aeltere Benutzerprofile) -> der Client
 * nimmt den Rueckfall orig_key (Standard Shift+F8); der Dialog zeigt das
 * als Standardbelegung und schreibt die Zeile beim Speichern ausdruecklich. */
static void bind_orig_default(lctrl_bind_t *b, int seen)
{
    int ov = lctrl_bind_elem_index("orig_view");
    if (!seen && ov >= 0) {
        lctrl_bind_set(b, ov, LCTRL_ORIG_VIEW_DEFAULT);
    }
}

/* W290 QUICK-SHOT: Profil ohne quick_shot-Zeile -> Uebergang wie im Client (imap_quick_shot_migrate). */
static void bind_quick_default(lctrl_bind_t *b, int seen)
{
    int qs = lctrl_bind_elem_index("quick_shot"), ph = lctrl_bind_elem_index("photo"), a, f12 = 0;
    if (seen || qs < 0 || ph < 0) {
        return;
    }
    if (b->row[ph].n_alt == 1 && _stricmp(b->row[ph].alt[0], "KEY:F12") == 0) {
        lctrl_bind_set(b, ph, LCTRL_PHOTO_DEFAULT);
        lctrl_bind_set(b, qs, LCTRL_QUICK_SHOT_DEFAULT);
        return;
    }
    for (a = 0; a < b->row[ph].n_alt; a++) {
        if (_stricmp(b->row[ph].alt[a], "KEY:F12") == 0) {
            f12 = 1;
        }
    }
    if (!f12) {
        lctrl_bind_set(b, qs, LCTRL_QUICK_SHOT_DEFAULT);
    }
}

/* W299 REPLAY: Profil ohne replay_exit-Zeile -> Standard wie im Client (imap_profile_parse). */
static void bind_replay_exit_default(lctrl_bind_t *b, int seen)
{
    int re = lctrl_bind_elem_index("replay_exit");
    if (!seen && re >= 0) {
        lctrl_bind_set(b, re, LCTRL_REPLAY_EXIT_DEFAULT);
    }
}

/* W438: Profil ohne menu_back-Zeile -> BACKSPACE, INSERT wenn START (bi6) auf BACKSPACE liegt (wie der Client). */
static void bind_menu_back_default(lctrl_bind_t *b, int seen)
{
    int mb = lctrl_bind_elem_index("menu_back"), st = lctrl_bind_elem_index("bi6"), a, back = 0;
    if (seen || mb < 0) {
        return;
    }
    for (a = 0; st >= 0 && a < b->row[st].n_alt; a++) {
        if (_stricmp(b->row[st].alt[a], "KEY:BACKSPACE") == 0) {
            back = 1;
        }
    }
    lctrl_bind_set(b, mb, back ? LCTRL_MENU_BACK_ALT : LCTRL_MENU_BACK_DEFAULT);
}

/* W437: Profil ohne chat-Zeile -> ENTER, ausser eine Spieltaste (bi0..bi38) nutzt ENTER ohne Zusatztaste (wie der
 * Client, imap_profile_parse). */
static void bind_chat_default(lctrl_bind_t *b, int seen)
{
    int ch = lctrl_bind_elem_index("chat"), i, a, used = 0;
    if (seen || ch < 0) {
        return;
    }
    for (i = 0; i < 39 && !used; i++) {
        char k[8];
        int r;
        _snprintf_s(k, sizeof k, _TRUNCATE, "bi%d", i);
        r = lctrl_bind_elem_index(k);
        for (a = 0; r >= 0 && a < b->row[r].n_alt; a++) {
            if (_stricmp(b->row[r].alt[a], "KEY:ENTER") == 0 || _stricmp(b->row[r].alt[a], "KEY:RETURN") == 0) {
                used = 1;
            }
        }
    }
    if (!used) {
        lctrl_bind_set(b, ch, LCTRL_CHAT_DEFAULT);
    }
}

/* W374: Profil ohne net_hud-Zeile -> Standard F3 wie im Client. */
static void bind_net_hud_default(lctrl_bind_t *b, int seen)
{
    int nh = lctrl_bind_elem_index("net_hud");
    if (!seen && nh >= 0) {
        lctrl_bind_set(b, nh, LCTRL_NET_HUD_DEFAULT);
    }
}

int lctrl_bind_load_file(const char *path, lctrl_bind_t *b)
{
    FILE *f;
    char line[512];
    int ov = lctrl_bind_elem_index("orig_view"), seen = 0;
    int qs = lctrl_bind_elem_index("quick_shot"), qseen = 0; /* W290 */
    int re = lctrl_bind_elem_index("replay_exit"), rseen = 0; /* W299 */
    int nh = lctrl_bind_elem_index("net_hud"), nseen = 0;     /* W374 */
    int mbk = lctrl_bind_elem_index("menu_back"), mseen = 0;  /* W438 */
    int chk = lctrl_bind_elem_index("chat"), cseen = 0;       /* W437 */
    lctrl_bind_init(b);
    if (u8_fopen_s(&f, path, "r") != 0 || !f) { return 1; }
    while (fgets(line, sizeof line, f)) {
        int r = bind_parse_line(line, b);
        if (r == ov) { seen = 1; }
        if (r == qs) { qseen = 1; }
        if (r == re) { rseen = 1; }
        if (r == nh) { nseen = 1; }
        if (r == mbk) { mseen = 1; }
        if (r == chk) { cseen = 1; }
    }
    fclose(f);
    bind_orig_default(b, seen);
    bind_quick_default(b, qseen);
    bind_replay_exit_default(b, rseen);
    bind_net_hud_default(b, nseen);
    bind_menu_back_default(b, mseen);
    bind_chat_default(b, cseen);
    return 0;
}

/* W182 KEYS-SVG: gleiche Auswertung fuer einen Profiltext im Speicher
 * (Browser-Export: Profiltext ist schon gelesen). */
int lctrl_bind_load_text(const char *text, lctrl_bind_t *b)
{
    char line[512];
    const char *p = text;
    int ov = lctrl_bind_elem_index("orig_view"), seen = 0;
    int qs = lctrl_bind_elem_index("quick_shot"), qseen = 0; /* W290 */
    int re = lctrl_bind_elem_index("replay_exit"), rseen = 0; /* W299 */
    int nh = lctrl_bind_elem_index("net_hud"), nseen = 0;     /* W374 */
    int mbk = lctrl_bind_elem_index("menu_back"), mseen = 0;  /* W438 */
    int chk = lctrl_bind_elem_index("chat"), cseen = 0;       /* W437 */
    lctrl_bind_init(b);
    if (!text) { return 1; }
    while (*p) {
        size_t n = strcspn(p, "\r\n");
        size_t c = n < sizeof line - 1 ? n : sizeof line - 1;
        int r;
        memcpy(line, p, c);
        line[c] = '\0';
        r = bind_parse_line(line, b);
        if (r == ov) { seen = 1; }
        if (r == qs) { qseen = 1; }
        if (r == re) { rseen = 1; }
        if (r == nh) { nseen = 1; }
        if (r == mbk) { mseen = 1; }
        if (r == chk) { cseen = 1; }
        p += n;
        while (*p == '\r' || *p == '\n') { p++; }
    }
    bind_orig_default(b, seen);
    bind_quick_default(b, qseen);
    bind_replay_exit_default(b, rseen);
    bind_net_hud_default(b, nseen);
    bind_menu_back_default(b, mseen);
    bind_chat_default(b, cseen);
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

/* W438 CTRL-RESET-040: START (bi6) und "Replay beenden" duerfen dieselbe Taste teilen (default_full: Backspace).
 * START wirkt im Spiel-Replay nicht (W299), replay_exit nur waehrend der Wiedergabe -> kein Konflikt. */
static int conf_pair_ok(int i, int m)
{
    int st = lctrl_bind_elem_index("bi6"), re = lctrl_bind_elem_index("replay_exit");
    return st >= 0 && re >= 0 && ((i == st && m == re) || (i == re && m == st));
}

/* Konflikt-Suche: gleiche Alternative auf mehreren Elementen. */
int lctrl_bind_conflicts(const lctrl_bind_t *b, char out[][160], int max)
{
    int i, m, a, n = 0, st = lctrl_bind_elem_index("bi6");
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
                        int s2;
                        if (st >= 0 && m != st && conf_pair_ok(m, st)) { /* W438: replay_exit neben START */
                            for (s2 = 0; s2 < b->row[st].n_alt && !strieq(b->row[st].alt[s2], alt); s2++) {}
                            if (s2 < b->row[st].n_alt) { break; }
                        }
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
    if (lctrl_stick_mode_get(b, idx) >= 0) { v |= LCTRL_VIS_SMODE; } /* W271 */
    return v;
}

/* W271 STICK-RELATIVE (s. Header). */
int lctrl_stick_src(const char *alt)
{
    if (!alt) { return 0; }
    while (*alt == ' ' || *alt == '\t') { alt++; }
    if (lctrl_chord_plus(alt)) { return 0; }
    if (_strnicmp(alt, "PAD:", 4) == 0) {
        const char *n = alt + 4;
        if ((n[0] == 'L' || n[0] == 'R' || n[0] == 'l' || n[0] == 'r') &&
            (n[1] == 'X' || n[1] == 'Y' || n[1] == 'x' || n[1] == 'y') &&
            (n[2] == '\0' || n[2] == ':')) {
            return 1;
        }
        return 0;
    }
    return lctrl_joy_src_axis(alt, NULL, NULL) == 1;
}

static int stick_aim_row(int idx)
{
    return idx >= 0 && idx < LCTRL_ELEM_N && kCtrlElems[idx].kind == LCTRL_AXIS &&
           (strcmp(kCtrlElems[idx].key, "aim_x") == 0 ||
            strcmp(kCtrlElems[idx].key, "aim_y") == 0);
}

int lctrl_stick_mode_get(const lctrl_bind_t *b, int idx)
{
    int a;
    if (!b || !stick_aim_row(idx)) { return -1; }
    for (a = 0; a < b->row[idx].n_alt; a++) {
        const char *alt = b->row[idx].alt[a];
        char v[16];
        if (!lctrl_stick_src(alt)) { continue; }
        if (!lctrl_joy_param_get(alt, "mode", v, sizeof v)) { return 0; }
        if (_stricmp(v, "rate") == 0 || _stricmp(v, "relative") == 0 ||
            _stricmp(v, "rel") == 0) {
            return 2;
        }
        if (_stricmp(v, "position") == 0 || _stricmp(v, "pos") == 0) { return 1; }
        return 0;
    }
    return -1;
}

int lctrl_stick_mode_set(lctrl_bind_t *b, int idx, int mode)
{
    static const char *const kv[3] = { NULL, "position", "rate" };
    int a, any = 0;
    if (!b || !stick_aim_row(idx) || mode < 0 || mode > 2) { return 1; }
    for (a = 0; a < b->row[idx].n_alt; a++) {
        char *alt = b->row[idx].alt[a];
        if (!lctrl_stick_src(alt)) { continue; }
        if (lctrl_joy_param_set(alt, sizeof b->row[idx].alt[a], "mode", kv[mode]) != 0) {
            return 1;
        }
        any = 1;
    }
    return any ? 0 : 1;
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

int lctrl_bind_clear_game(lctrl_bind_t *b)
{
    unsigned used = 0;
    int i, a, n = 0;
    if (!b) {
        return 0;
    }
    for (i = 0; i < LCTRL_ELEM_N; i++) {
        if (kCtrlElems[i].kind == LCTRL_ACT) {
            continue; /* Client-Aktionen bleiben (ESC-Menue, Foto, Feedback) */
        }
        if (b->row[i].n_alt > 0) {
            n++;
        }
        lctrl_bind_clear(b, i);
    }
    /* joyN-Zeilen, die keine verbliebene Quelle (auch im Akkord) nutzt */
    for (i = 0; i < LCTRL_ELEM_N; i++) {
        for (a = 0; a < b->row[i].n_alt; a++) {
            const char *p;
            for (p = b->row[i].alt[a]; *p; p++) {
                if ((p[0] == 'J' || p[0] == 'j') && (p[1] == 'O' || p[1] == 'o') &&
                    (p[2] == 'Y' || p[2] == 'y') && p[3] >= '0' &&
                    p[3] < '0' + IMAP_JOY_MAX && p[4] == ':') {
                    used |= 1u << (p[3] - '0');
                }
            }
        }
    }
    for (i = 0; i < IMAP_JOY_MAX; i++) {
        if (!(used & (1u << i))) {
            b->joy[i][0] = '\0';
        }
    }
    return n;
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
            if (m == idx || conf_pair_ok(idx, m)) { continue; } /* W438: START + Replay beenden erlaubt */
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
        if (err) { strncpy_s(err, errcap, LT("gui.bind_nopath"), _TRUNCATE); }
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
        if (b->row[i].n_alt == 0 && !lctrl_bind_empty_line_row(i)) {
            continue; /* unbelegt: keine Zeile */
        } /* W283: orig_view leer -> "orig_view =" (aus, kein Rueckfall auf orig_key); W290: quick_shot ebenso */
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
    if (best < 0 || iabs_(bestd) < LCTRL_JOY_LEARN_MIN) { /* W432: 20 % des Bereichs (vorher 40 %) */
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
    int i, cand = -1, cd = 0, cex = -1, moving, released, r;
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
                iabs_((int)cur->axis[i] - (int)L->prev.axis[i]) > LCTRL_JOY_STILL) {
                still = 0;
            }
        }
        for (i = 0; i < IJOY_AXES; i++) { /* W432: Rauschen der laufenden Ruhephase */
            int dd = iabs_((int)cur->axis[i] - (int)L->prev.axis[i]);
            if (!still || !(axmask & (1u << i))) {
                L->jit[i] = 0;
            } else if (dd > L->jit[i]) {
                L->jit[i] = dd;
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
        /* W432: Auslenkung ueber dem Rauschen der Ruhephase zaehlt */
        if (iabs_(L->peakd[i]) - L->jit[i] > cex) {
            cex = iabs_(L->peakd[i]) - L->jit[i];
            cd = L->peakd[i];
            cand = i;
        }
    }
    if (cand < 0 || cex < LCTRL_JOY_LEARN_MIN) { /* W432: 20 % (vorher 40 %) */
        L->stable = 0;
        L->prev = *cur;
        return 0;
    }
    if (cand != L->cand) {
        L->cand = cand;
        L->stable = 0;
    }
    moving = iabs_((int)cur->axis[cand] - (int)L->prev.axis[cand]) > 1310 + L->jit[cand];
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
            strncpy_s(out, cap, LT("gui.bind_notmp"), _TRUNCATE);
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

/* ======================================================================
 * W419 CTRL-EXPORT: Steuerung exportieren / importieren (.sbcontrols)
 * ====================================================================== */

enum { CXK_INT = 0, CXK_MOUSE, CXK_STICK };
typedef struct {
    const char *key;
    int ui;   /* 1 = Launcher-Oberflaeche, 0 = nur user.cfg [input] */
    int kind; /* CXK_* */
    int lo, hi;
} cx_key_t;

/* Inventur der steuerungsnahen Werte (Doku: EINGABE.md S8a). Bereiche wie
 * der Client (hcfg.c); sbc_cal_/sbc_dead_ und joy_calN kommen dazu. */
static const cx_key_t kCxKeys[] = {
    { "mouse_mode", 1, CXK_MOUSE, 0, 1 },
    { "mouse_sens", 1, CXK_INT, 1, 16 },
    { "stick_aim", 1, CXK_STICK, 0, 1 },
    { "stick_aim_sens", 1, CXK_INT, 1, 16 },
    { "aim_assist", 1, CXK_INT, 0, 2 },
    { "auto_gear", 1, CXK_INT, 0, 1 },
    { "sbc_usb", 1, CXK_INT, 0, 1 },
    { "sbc_mouse_off", 1, CXK_INT, 0, 1 },
    { "mouse_invert_x", 0, CXK_INT, 0, 1 },
    { "mouse_invert_y", 0, CXK_INT, 0, 1 },
    { "stick_aim_accel", 0, CXK_INT, 0, 4 },
    { "aim_assist_edge", 0, CXK_INT, 30, 100 },
    { "pad_dz_stick", 0, CXK_INT, 0, 90 },
    { "pad_dz_trig", 0, CXK_INT, 0, 90 },
};
#define CX_NKEYS ((int)(sizeof kCxKeys / sizeof kCxKeys[0]))

/* Strenge Ganzzahl: optional '-', 1..6 Ziffern, sonst nichts. 1 = ok. */
static int cx_int(const char *v, int *out)
{
    const char *p = v;
    int n = 0;
    if (!v) { return 0; }
    if (*p == '-') { p++; }
    while (*p >= '0' && *p <= '9') { p++; n++; }
    if (*p || n < 1 || n > 6) { return 0; }
    *out = atoi(v);
    return 1;
}

/* 0 = unbekannt, 1 = Oberflaeche, 2 = nur user.cfg. */
static int cx_key_kind(const char *key)
{
    int i;
    if (!key || !*key) { return 0; }
    for (i = 0; i < CX_NKEYS; i++) {
        if (strcmp(key, kCxKeys[i].key) == 0) { return kCxKeys[i].ui ? 1 : 2; }
    }
    if ((strncmp(key, "sbc_cal_", 8) == 0 || strncmp(key, "sbc_dead_", 9) == 0) && assist_cfg_owns(key)) {
        return 2; /* Kalibrierung Original-Controller (Assistent) */
    }
    if (joycal_cfg_owns(key)) { return 2; } /* W312 Joystick-Kalibrierung je VID:PID + Achse */
    return 0;
}

int lcx_set_check(const char *key, const char *val)
{
    int i, kind = cx_key_kind(key), v;
    const char *p;
    if (!kind) { return -1; }
    if (!val || !*val || strlen(val) > 80) { return 0; }
    for (p = val; *p; p++) { /* Werte sind Zahlen/Namen/Listen: nichts anderes zulassen */
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ',' ||
              c == ':' || c == '-' || c == '.')) {
            return 0;
        }
    }
    for (i = 0; i < CX_NKEYS; i++) {
        if (strcmp(key, kCxKeys[i].key) != 0) { continue; }
        if (kCxKeys[i].kind == CXK_MOUSE) {
            return strcmp(val, "absolute") == 0 || strcmp(val, "relative") == 0;
        }
        if (kCxKeys[i].kind == CXK_STICK) {
            return strcmp(val, "position") == 0 || strcmp(val, "relative") == 0;
        }
        return cx_int(val, &v) && v >= kCxKeys[i].lo && v <= kCxKeys[i].hi;
    }
    if (joycal_cfg_owns(key)) {
        unsigned short vid, pid;
        int ax;
        ijoy_cal_t c;
        return ijoy_cal_parse(val, &vid, &pid, &ax, &c) ? 1 : 0;
    }
    {
        static sbc_cal_t cal; /* nur Pruefung (Struktur nicht klein) */
        int r;
        memset(&cal, 0, sizeof cal);
        r = sbc_usb_cal_set(&cal, key, val);
        return (r == 1 || r == SBC_CAL_NOTE) ? 1 : 0;
    }
}

int lcx_set_is_ui(const char *key)
{
    return cx_key_kind(key) == 1;
}

static void cx_set_put(lcx_data_t *d, const char *key, const char *val)
{
    int i;
    size_t kl = strlen(key);
    for (i = 0; i < d->n_set; i++) {
        if (strncmp(d->set[i], key, kl) == 0 && d->set[i][kl] == '=') {
            _snprintf_s(d->set[i], sizeof d->set[i], _TRUNCATE, "%s=%s", key, val);
            return;
        }
    }
    if (d->n_set < LCX_SET_MAX) {
        _snprintf_s(d->set[d->n_set], sizeof d->set[0], _TRUNCATE, "%s=%s", key, val);
        d->n_set++;
    }
}

static void cx_put_int(lcx_data_t *d, const char *key, int v)
{
    char t[16];
    _snprintf_s(t, sizeof t, _TRUNCATE, "%d", v);
    if (lcx_set_check(key, t) == 1) { cx_set_put(d, key, t); }
}

void lcx_put_settings(lcx_data_t *d, const launcher_settings_t *s)
{
    if (!d || !s) { return; }
    cx_set_put(d, "mouse_mode", s->mouse_mode ? "absolute" : "relative");
    cx_put_int(d, "mouse_sens", s->mouse_sens);
    cx_set_put(d, "stick_aim", s->stick_aim ? "relative" : "position");
    cx_put_int(d, "stick_aim_sens", s->stick_aim_sens);
    cx_put_int(d, "aim_assist", s->aim_assist);
    cx_put_int(d, "auto_gear", s->auto_gear);
    cx_put_int(d, "sbc_usb", s->sbc_usb ? 1 : 0);
    cx_put_int(d, "sbc_mouse_off", s->sbc_mouse_off ? 1 : 0);
}

static int cx_owns_raw(const char *key)
{
    return cx_key_kind(key) == 2;
}

int lcx_put_cfg(lcx_data_t *d, const char *user_cfg)
{
    static char lines[LCX_SET_MAX][96];
    int n, i, got = 0;
    if (!d || !user_cfg) { return 0; }
    n = assist_cfg_read_fn(user_cfg, lines, LCX_SET_MAX, cx_owns_raw);
    for (i = 0; i < n; i++) {
        char *eq = strchr(lines[i], '=');
        if (!eq) { continue; }
        *eq = '\0';
        if (lcx_set_check(lines[i], eq + 1) == 1) { /* ungueltige Zeilen nicht weitergeben */
            cx_set_put(d, lines[i], eq + 1);
            got++;
        }
    }
    return got;
}

int lcx_get_settings(const lcx_data_t *d, launcher_settings_t *s)
{
    int i, n = 0;
    if (!d || !s) { return 0; }
    for (i = 0; i < d->n_set; i++) {
        char k[48];
        const char *eq = strchr(d->set[i], '=');
        int v = 0;
        size_t kl;
        if (!eq) { continue; }
        kl = (size_t)(eq - d->set[i]);
        if (kl >= sizeof k) { continue; }
        memcpy(k, d->set[i], kl);
        k[kl] = '\0';
        if (!lcx_set_is_ui(k) || lcx_set_check(k, eq + 1) != 1) { continue; }
        (void)cx_int(eq + 1, &v);
        if (strcmp(k, "mouse_mode") == 0) { s->mouse_mode = strcmp(eq + 1, "absolute") == 0; }
        else if (strcmp(k, "stick_aim") == 0) { s->stick_aim = strcmp(eq + 1, "relative") == 0; }
        else if (strcmp(k, "mouse_sens") == 0) { s->mouse_sens = v; }
        else if (strcmp(k, "stick_aim_sens") == 0) { s->stick_aim_sens = v; }
        else if (strcmp(k, "aim_assist") == 0) { s->aim_assist = v; }
        else if (strcmp(k, "auto_gear") == 0) { s->auto_gear = v; }
        else if (strcmp(k, "sbc_usb") == 0) { s->sbc_usb = v; }
        else if (strcmp(k, "sbc_mouse_off") == 0) { s->sbc_mouse_off = v; }
        else { continue; }
        n++;
    }
    return n;
}

/* Import-Besitzer: genau die user.cfg-Keys (ohne joy_calN) aus der Datei. */
static const lcx_data_t *g_cx_own;
static int cx_owns_import(const char *key)
{
    int i;
    size_t kl;
    if (!g_cx_own || !key || cx_key_kind(key) != 2 || joycal_cfg_owns(key)) { return 0; }
    kl = strlen(key);
    for (i = 0; i < g_cx_own->n_set; i++) {
        if (strncmp(g_cx_own->set[i], key, kl) == 0 && g_cx_own->set[i][kl] == '=') { return 1; }
    }
    return 0;
}

int lcx_apply_cfg(const lcx_data_t *d, const char *user_cfg)
{
    static char lines[LCX_SET_MAX][96], ex[IJOY_CAL_MAX][96];
    typedef struct { unsigned short v, p; int ax; ijoy_cal_t c; } jc_t;
    static jc_t tab[IJOY_CAL_MAX];
    int i, n = 0, nj = 0, nimp = 0, rc = 0;
    if (!d || !user_cfg || !*user_cfg) { return 1; }
    for (i = 0; i < d->n_set; i++) {
        char k[48];
        const char *eq = strchr(d->set[i], '=');
        size_t kl;
        if (!eq) { continue; }
        kl = (size_t)(eq - d->set[i]);
        if (kl >= sizeof k) { continue; }
        memcpy(k, d->set[i], kl);
        k[kl] = '\0';
        if (cx_key_kind(k) != 2 || lcx_set_check(k, eq + 1) != 1) { continue; }
        if (joycal_cfg_owns(k)) { nimp++; continue; }
        strcpy_s(lines[n++], sizeof lines[0], d->set[i]);
    }
    if (n > 0) {
        g_cx_own = d;
        rc = assist_cfg_rewrite_fn(user_cfg, (const char (*)[96])lines, n, cx_owns_import);
        g_cx_own = NULL;
        if (rc != 0) { return 1; }
    }
    if (nimp == 0) { return 0; }
    /* joy_calN: vorhandene Eintraege behalten, gleiche VID:PID+Achse ersetzen, neue anhaengen
     * (Kalibrierung gehoert zum Geraet: nichts wird auf andere Geraete uebertragen). */
    {
        int m = joycal_cfg_read(user_cfg, ex, IJOY_CAL_MAX);
        for (i = 0; i < m && nj < IJOY_CAL_MAX; i++) {
            const char *eq = strchr(ex[i], '=');
            if (eq && ijoy_cal_parse(eq + 1, &tab[nj].v, &tab[nj].p, &tab[nj].ax, &tab[nj].c)) { nj++; }
        }
    }
    for (i = 0; i < d->n_set; i++) {
        char k[48];
        const char *eq = strchr(d->set[i], '=');
        jc_t e;
        int j;
        size_t kl;
        if (!eq) { continue; }
        kl = (size_t)(eq - d->set[i]);
        if (kl >= sizeof k) { continue; }
        memcpy(k, d->set[i], kl);
        k[kl] = '\0';
        if (!joycal_cfg_owns(k) || !ijoy_cal_parse(eq + 1, &e.v, &e.p, &e.ax, &e.c)) { continue; }
        for (j = 0; j < nj; j++) {
            if (tab[j].v == e.v && tab[j].p == e.p && tab[j].ax == e.ax) { break; }
        }
        if (j < nj) { tab[j] = e; }
        else if (nj < IJOY_CAL_MAX) { tab[nj++] = e; }
    }
    n = 0;
    for (i = 0; i < nj; i++) {
        char val[80];
        if (ijoy_cal_format(tab[i].v, tab[i].p, tab[i].ax, &tab[i].c, val, (int)sizeof val)) {
            _snprintf_s(lines[n], sizeof lines[0], _TRUNCATE, "joy_cal%d=%s", n, val);
            n++;
        }
    }
    return joycal_cfg_apply(user_cfg, (const char (*)[96])lines, n) == 0 ? 0 : 1;
}

/* Einfacher Anhaenger fuer lcx_build. */
typedef struct { char *p; size_t cap, len; int full; } cx_buf_t;
static void cx_add(cx_buf_t *b, const char *fmt, ...)
{
    va_list ap;
    int r;
    if (b->full || b->len >= b->cap) { b->full = 1; return; }
    va_start(ap, fmt);
    r = _vsnprintf_s(b->p + b->len, b->cap - b->len, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (r < 0) { b->full = 1; return; }
    b->len += (size_t)r;
}

int lcx_build(const lcx_data_t *d, char *out, size_t cap)
{
    cx_buf_t b;
    int i;
    if (!d || !out || cap < 64) { return 1; }
    b.p = out;
    b.cap = cap;
    b.len = 0;
    b.full = 0;
    out[0] = '\0';
    cx_add(&b, "# Steelbound controls: key bindings and control settings.\n");
    cx_add(&b, "# Import in the launcher: Controls > Import. No paths, accounts or other settings.\n");
    cx_add(&b, "[steelbound_controls]\nformat = %d\nlauncher = %s\ngame = %s\nprofile = %s\n", LCX_FORMAT,
           d->launcher[0] ? d->launcher : "dev", d->game[0] ? d->game : "sb", d->profile);
    cx_add(&b, "\n[settings]\n");
    for (i = 0; i < d->n_set; i++) {
        const char *eq = strchr(d->set[i], '=');
        if (eq) { cx_add(&b, "%.*s = %s\n", (int)(eq - d->set[i]), d->set[i], eq + 1); }
    }
    cx_add(&b, "\n[profile]\n");
    for (i = 0; i < IMAP_JOY_MAX; i++) {
        if (d->bind.joy[i][0]) { cx_add(&b, "joy%d = %s\n", i, d->bind.joy[i]); }
    }
    for (i = 0; i < LCTRL_ELEM_N; i++) { /* wie lctrl_bind_save (leere Aus-Zeilen bleiben) */
        int a;
        if (d->bind.row[i].n_alt == 0 && !lctrl_bind_empty_line_row(i)) { continue; }
        cx_add(&b, "%s =", kCtrlElems[i].key);
        for (a = 0; a < d->bind.row[i].n_alt; a++) {
            cx_add(&b, "%s%s", a ? ", " : " ", d->bind.row[i].alt[a]);
        }
        cx_add(&b, "\n");
    }
    cx_add(&b, "\n[end]\n");
    return b.full ? 1 : 0;
}

int lcx_write_file(const char *path, const lcx_data_t *d)
{
    static char buf[LCX_MAX_BYTES];
    FILE *f = NULL;
    size_t n;
    if (!path || !*path || lcx_build(d, buf, sizeof buf) != 0) { return 1; }
    if (u8_fopen_s(&f, path, "wb") != 0 || !f) { return 1; }
    n = strlen(buf);
    if (fwrite(buf, 1, n, f) != n) { fclose(f); return 1; }
    return fclose(f) == 0 ? 0 : 1;
}

/* Zahl (strtod, ganz verbraucht) in [lo, hi]. */
static int cx_num_in(const char *v, double lo, double hi)
{
    char *e = NULL;
    double x;
    if (!v || !*v || strlen(v) > 12) { return 0; }
    x = strtod(v, &e);
    return e && !*e && x >= lo && x <= hi;
}

/* Eine Quelle pruefen: 1 = ok, 0 = ungueltig (Zeichen, Form, Bereich), -1 = unbekannte Quellenart. */
static int cx_alt_check(const char *alt)
{
    const char *p, *q;
    int unknown = 0;
    size_t L = alt ? strlen(alt) : 0;
    if (L < 1 || L > 95) { return 0; }
    for (p = alt; *p; p++) {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ':' ||
              c == '+' || c == '=' || c == '.' || c == '_' || c == '!' || c == '-')) {
            return 0;
        }
    }
    for (p = alt; *p; p = q ? q + 1 : p + strlen(p)) {
        char atom[96], *seg, *ctx = NULL;
        size_t n;
        int si = 0;
        q = lctrl_chord_plus(p);
        n = q ? (size_t)(q - p) : strlen(p);
        if (n == 0 || n >= sizeof atom) { return 0; }
        memcpy(atom, p, n);
        atom[n] = '\0';
        if (!strchr(atom, ':')) { return 0; }
        for (seg = strtok_s(atom, ":", &ctx); seg; seg = strtok_s(NULL, ":", &ctx), si++) {
            if (si == 0) {
                if (_stricmp(seg, "KEY") && _stricmp(seg, "MOUSE") && _stricmp(seg, "PAD") &&
                    _stricmp(seg, "SBC") && _stricmp(seg, "JOY") && !(_strnicmp(seg, "JOY", 3) == 0 &&
                    seg[3] >= '0' && seg[3] < '0' + IMAP_JOY_MAX && seg[4] == '\0')) {
                    unknown = 1;
                }
                continue;
            }
            if (si == 1) {
                if (!seg[0]) { return 0; }
                continue;
            }
            { /* Parameter p=v */
                char *eq = strchr(seg, '=');
                const char *v;
                if (!eq) { continue; }
                *eq = '\0';
                v = eq + 1;
                if (_stricmp(seg, "dz") == 0 && !cx_num_in(v, 0.0, 0.9)) { return 0; }
                if (_stricmp(seg, "curve") == 0 && !cx_num_in(v, 0.5, 3.0)) { return 0; }
                if (_stricmp(seg, "sens") == 0 && !cx_num_in(v, 0.1, 4.0)) { return 0; }
                if ((_stricmp(seg, "inv") == 0 || _stricmp(seg, "half") == 0) && strcmp(v, "0") && strcmp(v, "1")) {
                    return 0;
                }
                if (_stricmp(seg, "zone") == 0) {
                    int lo = -1, hi = -1;
                    char tail = 0;
                    if (sscanf_s(v, "%d-%d%c", &lo, &hi, &tail, 1) != 2 || lo < 0 || hi > 100 || lo > hi) {
                        return 0;
                    }
                }
            }
        }
        if (!q) { break; }
    }
    return unknown ? -1 : 1;
}

/* Profilname aus der Datei: kein Pfad, keine Steuerzeichen, 1..63 Zeichen. */
static int cx_name_ok(const char *v)
{
    const unsigned char *p;
    size_t L = v ? strlen(v) : 0;
    if (L < 1 || L > 63 || strstr(v, "..")) { return 0; }
    for (p = (const unsigned char *)v; *p; p++) {
        if (*p < 0x20 || *p == 0x7F || *p == '/' || *p == '\\' || *p == ':' || *p == '*' || *p == '?' ||
            *p == '"' || *p == '<' || *p == '>' || *p == '|') {
            return 0;
        }
    }
    return 1;
}

static void cx_err(char *err, size_t cap, const char *key, const char *a, int line)
{
    if (!err || !cap) { return; }
    if (a) { _snprintf_s(err, cap, _TRUNCATE, LT(key), a, line); }
    else if (line > 0) { _snprintf_s(err, cap, _TRUNCATE, LT(key), line); }
    else { strncpy_s(err, cap, LT(key), _TRUNCATE); }
}

int lcx_parse(const char *buf, size_t len, lcx_data_t *d, char *err, size_t cap)
{
    enum { S_NONE, S_HEAD, S_SET, S_PROF, S_END, S_OTHER };
    static char text[LCX_MAX_BYTES + 1024]; /* gepruefte Profilzeilen fuer lctrl_bind_load_text */
    size_t i, p, tl = 0;
    int sec = S_NONE, lineno = 0, have_head = 0, have_end = 0, first = 1, rc = LCX_OK;
    if (err && cap) { err[0] = '\0'; }
    if (!d) { return LCX_E_IO; }
    memset(d, 0, sizeof *d);
    if (!buf) { cx_err(err, cap, "gui.cx_err_io", NULL, 0); return LCX_E_IO; }
    if (len > LCX_MAX_BYTES) { cx_err(err, cap, "gui.cx_err_big", NULL, 0); return LCX_E_TOOBIG; }
    if (len >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB &&
        (unsigned char)buf[2] == 0xBF) {
        buf += 3; /* UTF-8-BOM */
        len -= 3;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if ((c < 0x20 && c != '\t' && c != '\r' && c != '\n') || c == 0x7F) {
            cx_err(err, cap, "gui.cx_err_bin", NULL, 0);
            return LCX_E_BINARY;
        }
    }
    /* Vorlauf: Kopf zuerst, Endmarke vorhanden (abgeschnittene Dateien vor jeder Zeilenpruefung erkennen). */
    for (p = 0; p < len;) {
        char ln[520];
        size_t q = p, n;
        while (q < len && buf[q] != '\n') { q++; }
        n = q - p;
        if (n < sizeof ln) {
            char *t;
            memcpy(ln, buf + p, n);
            ln[n] = '\0';
            trim(ln);
            t = ln;
            if (t[0] && t[0] != '#' && t[0] != ';') {
                if (first) {
                    first = 0;
                    if (_stricmp(t, "[steelbound_controls]") != 0) {
                        cx_err(err, cap, "gui.cx_err_notcx", NULL, 0);
                        return LCX_E_NOTCX;
                    }
                }
                if (_stricmp(t, "[end]") == 0) { have_end = 1; }
            }
        }
        p = q + 1;
    }
    if (first) { cx_err(err, cap, "gui.cx_err_notcx", NULL, 0); return LCX_E_NOTCX; }
    if (!have_end) { cx_err(err, cap, "gui.cx_err_trunc", NULL, 0); return LCX_E_TRUNC; }
    text[0] = '\0';
    for (p = 0; p < len && rc == LCX_OK;) {
        char ln[520], *t, *eq, *key, *val, *h;
        size_t q = p, n;
        while (q < len && buf[q] != '\n') { q++; }
        n = q - p;
        lineno++;
        if (n >= 512) { cx_err(err, cap, "gui.cx_err_line", NULL, lineno); rc = LCX_E_LINE; break; }
        memcpy(ln, buf + p, n);
        ln[n] = '\0';
        p = q + 1;
        h = strchr(ln, '#');
        if (h) { *h = '\0'; } /* Kommentar ab '#' (wie Profile) */
        trim(ln);
        t = ln;
        if (!t[0] || t[0] == ';') { continue; }
        if (sec == S_END) { d->unknown++; continue; } /* nach [end] zaehlt nichts */
        if (t[0] == '[') {
            if (_stricmp(t, "[steelbound_controls]") == 0) { sec = S_HEAD; have_head = 1; }
            else if (_stricmp(t, "[settings]") == 0) { sec = S_SET; }
            else if (_stricmp(t, "[profile]") == 0) { sec = S_PROF; }
            else if (_stricmp(t, "[end]") == 0) { sec = S_END; }
            else { sec = S_OTHER; d->unknown++; }
            continue;
        }
        eq = strchr(t, '=');
        if (!eq || sec == S_OTHER) { d->unknown++; continue; }
        *eq = '\0';
        key = t;
        val = eq + 1;
        trim(key);
        trim(val);
        if (sec == S_HEAD) {
            if (_stricmp(key, "format") == 0) {
                int v = 0;
                if (!cx_int(val, &v) || v < 1) { cx_err(err, cap, "gui.cx_err_notcx", NULL, 0); rc = LCX_E_NOTCX; }
                else if (v > LCX_FORMAT) { cx_err(err, cap, "gui.cx_err_newer", NULL, 0); rc = LCX_E_NEWER; }
                else { d->format = v; }
            } else if (_stricmp(key, "profile") == 0) {
                if (!cx_name_ok(val)) { cx_err(err, cap, "gui.cx_err_name", NULL, 0); rc = LCX_E_NAME; }
                else { strcpy_s(d->profile, sizeof d->profile, val); }
            } else if (_stricmp(key, "game") == 0) {
                if (_stricmp(val, "sb") == 0 || _stricmp(val, "loc") == 0) {
                    strcpy_s(d->game, sizeof d->game, _stricmp(val, "sb") == 0 ? "sb" : "loc");
                } else {
                    d->unknown++;
                }
            } else if (_stricmp(key, "launcher") == 0) {
                const char *c;
                int ok = strlen(val) < sizeof d->launcher;
                for (c = val; ok && *c; c++) {
                    ok = (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') ||
                         *c == '.' || *c == '-' || *c == '+';
                }
                if (ok) { strcpy_s(d->launcher, sizeof d->launcher, val); }
            } else {
                d->unknown++;
            }
        } else if (sec == S_SET) {
            int r = strlen(key) < 48 ? lcx_set_check(key, val) : -1;
            if (r == 1) { cx_set_put(d, key, val); }
            else if (r == 0) { cx_err(err, cap, "gui.cx_err_value", key, lineno); rc = LCX_E_VALUE; }
            else { d->unknown++; }
        } else if (sec == S_PROF) {
            char row[520];
            if ((key[0] == 'j' || key[0] == 'J') && (key[1] == 'o' || key[1] == 'O') &&
                (key[2] == 'y' || key[2] == 'Y') && key[3] >= '0' && key[3] < '0' + IMAP_JOY_MAX && !key[4]) {
                const unsigned char *c;
                int ok = strlen(val) < sizeof d->bind.joy[0];
                for (c = (const unsigned char *)val; ok && *c; c++) {
                    ok = *c >= 0x20 && *c != 0x7F && *c != ',';
                }
                if (!ok) { cx_err(err, cap, "gui.cx_err_value", key, lineno); rc = LCX_E_VALUE; break; }
                _snprintf_s(row, sizeof row, _TRUNCATE, "joy%c = %s\n", key[3], val);
            } else if (strlen(key) < 32 && lctrl_bind_elem_index(key) >= 0) {
                char *one, *ctx = NULL;
                size_t rl;
                int na = 0;
                rl = (size_t)_snprintf_s(row, sizeof row, _TRUNCATE, "%s =", key);
                for (one = strtok_s(val, ",", &ctx); one && rc == LCX_OK; one = strtok_s(NULL, ",", &ctx)) {
                    int r;
                    trim(one);
                    if (!one[0]) { continue; }
                    r = cx_alt_check(one);
                    if (r < 0) { d->unknown++; continue; } /* unbekannte Quellenart: weglassen */
                    if (r == 0 || ++na > LCTRL_ALT_N) {
                        cx_err(err, cap, "gui.cx_err_value", key, lineno);
                        rc = LCX_E_VALUE;
                        break;
                    }
                    rl += (size_t)_snprintf_s(row + rl, sizeof row - rl, _TRUNCATE, "%s%s", na > 1 ? ", " : " ", one);
                }
                if (rc != LCX_OK) { break; }
                strcat_s(row, sizeof row, "\n");
            } else {
                d->unknown++;
                continue;
            }
            if (tl + strlen(row) + 1 >= sizeof text) { cx_err(err, cap, "gui.cx_err_big", NULL, 0); rc = LCX_E_TOOBIG; break; }
            memcpy(text + tl, row, strlen(row) + 1);
            tl += strlen(row);
        } else {
            d->unknown++; /* Wert vor dem ersten Abschnitt */
        }
    }
    if (rc == LCX_OK && (!have_head || d->format < 1)) { cx_err(err, cap, "gui.cx_err_notcx", NULL, 0); rc = LCX_E_NOTCX; }
    if (rc == LCX_OK && !d->profile[0]) { cx_err(err, cap, "gui.cx_err_name", NULL, 0); rc = LCX_E_NAME; }
    if (rc != LCX_OK) {
        memset(d, 0, sizeof *d); /* nichts davon darf angewendet werden */
        return rc;
    }
    lctrl_bind_load_text(text, &d->bind);
    return LCX_OK;
}

int lcx_read_file(const char *path, lcx_data_t *d, char *err, size_t cap)
{
    static char buf[LCX_MAX_BYTES + 1];
    FILE *f = NULL;
    long sz;
    size_t n;
    if (d) { memset(d, 0, sizeof *d); }
    if (!path || !*path || u8_fopen_s(&f, path, "rb") != 0 || !f) {
        cx_err(err, cap, "gui.cx_err_io", NULL, 0);
        return LCX_E_IO;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
        fclose(f);
        cx_err(err, cap, "gui.cx_err_io", NULL, 0);
        return LCX_E_IO;
    }
    if (sz > LCX_MAX_BYTES) { /* vor dem Lesen ablehnen */
        fclose(f);
        cx_err(err, cap, "gui.cx_err_big", NULL, 0);
        return LCX_E_TOOBIG;
    }
    fseek(f, 0, SEEK_SET);
    n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return lcx_parse(buf, n, d, err, cap);
}

int lcx_import_name(const char *client_dir, const char *raw, char *out, size_t cap)
{
    char rest[48], cand[72], path[1200];
    size_t i, n = 0;
    int k;
    if (!out || cap < 8) { return 1; }
    out[0] = '\0';
    if (!raw) { raw = ""; }
    if (_strnicmp(raw, "user_", 5) == 0) { raw += 5; }
    for (i = 0; raw[i] && n < 40; i++) {
        char c = raw[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                 c == '-' || c == '.';
        if ((unsigned char)c >= 0x80 && (unsigned char)c < 0xC0) { continue; } /* UTF-8-Folgebyte: nur ein '_' */
        rest[n++] = ok ? c : '_';
    }
    rest[n] = '\0';
    while (n > 0 && rest[n - 1] == '.') { rest[--n] = '\0'; } /* keine Punkte am Ende */
    if (!n || strspn(rest, "._") == n) { strcpy_s(rest, sizeof rest, "imported"); }
    for (k = 1; k < 100; k++) {
        char sfx[8] = "";
        size_t keep;
        if (k > 1) { _snprintf_s(sfx, sizeof sfx, _TRUNCATE, "-%d", k); }
        keep = strlen(rest);
        if (keep + strlen(sfx) > 40) { keep = 40 - strlen(sfx); }
        _snprintf_s(cand, sizeof cand, _TRUNCATE, "user_%.*s%s", (int)keep, rest, sfx);
        if (lctrl_input_profile_path(client_dir, cand, path, sizeof path) != 0) { return 1; }
        if (u8_GetFileAttributes(path) == INVALID_FILE_ATTRIBUTES) {
            strcpy_s(out, cap, cand);
            return 0;
        }
    }
    return 1;
}

static void cx_miss_add(char out[][96], int max, int *n, const char *name)
{
    int i;
    for (i = 0; i < *n; i++) {
        if (_stricmp(out[i], name) == 0) { return; }
    }
    if (*n < max) {
        strcpy_s(out[*n], 96, name);
        (*n)++;
    }
}

int lcx_missing(const lcx_data_t *d, const ijoy_info_t *list, int n, int sbc_present, char out[][96], int max)
{
    int s, i, a, cnt = 0, sbc = 0;
    if (!d || !out || max <= 0) { return 0; }
    for (s = 0; s < IMAP_JOY_MAX; s++) {
        if (!lctrl_bind_uses_joy_slot(&d->bind, s)) { continue; }
        if (d->bind.joy[s][0]) {
            if (ijoy_match_list(list, n, d->bind.joy[s], s) < 0) { cx_miss_add(out, max, &cnt, d->bind.joy[s]); }
        } else if (ijoy_match_list(list, n, "", s) < 0) {
            char t[96];
            _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.cx_joy_slot"), s + 1);
            cx_miss_add(out, max, &cnt, t);
        }
    }
    for (i = 0; i < LCTRL_ELEM_N && !sbc; i++) {
        for (a = 0; a < d->bind.row[i].n_alt; a++) {
            if (lctrl_bind_alt_is_sbc(d->bind.row[i].alt[a])) { sbc = 1; }
        }
    }
    for (i = 0; i < d->n_set; i++) {
        if (strcmp(d->set[i], "sbc_usb=1") == 0) { sbc = 1; }
    }
    if (sbc && !sbc_present) { cx_miss_add(out, max, &cnt, LT("gui.cx_sbc_name")); }
    return cnt;
}
