/* input_sbc_usb.c - Original-Steel-Battalion-Controller per WinUSB lesen.
 *
 * W49-Vorarbeit (CTRL-SBC). Protokoll: poc-native/docs/CONTROLLER.md.
 * Nur C + Win32 (setupapi, winusb) - keine Harness-Abhaengigkeit.
 */
#include "input_sbc_usb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <initguid.h> /* GUID_DEVINTERFACE_USB_DEVICE instanziieren */
#include <usbiodef.h>
#include <winusb.h>

struct sbc_usb_dev {
    HANDLE file;
    WINUSB_INTERFACE_HANDLE wh;
    UCHAR pipe_in;   /* Interrupt-IN (Report lesen) */
    UCHAR pipe_out;  /* Interrupt-OUT (LEDs) */
};

const char *sbc_usb_strerror(int err)
{
    switch (err) {
    case SBC_OK: return "ok";
    case SBC_ERR_NODEVICE: return "kein Steel-Battalion-Controller (0A7B:D000) gefunden";
    case SBC_ERR_DRIVER: return "Controller gefunden, aber WinUSB-Zugriff scheitert (Zadig-Bindung fehlt?)";
    case SBC_ERR_IO: return "USB-Transferfehler am offenen Controller";
    case SBC_ERR_TIMEOUT: return "kein neuer Report (Timeout)";
    case SBC_ERR_BADREPORT: return "Report-Header passt nicht (Laenge/ID)";
    case SBC_ERR_ARG: return "ungueltiges Argument";
    default: return "unbekannter Fehler";
    }
}

/* ---- reine Funktionen (ohne Geraet) ---- */

int sbc_usb_parse_report(const uint8_t raw[SBC_USB_IN_LEN], sbc_state_t *st)
{
    if (!raw || !st) {
        return SBC_ERR_ARG;
    }
    if (raw[0] != 0x00 || raw[1] != SBC_USB_IN_LEN) {
        return SBC_ERR_BADREPORT;
    }
    /* Tasten Bytes 2-6 -> bi0..38 (CONTROLLER.md §3.1/§5, 1:1). */
    st->buttons[0] = (uint16_t)(raw[2] | ((uint16_t)raw[3] << 8));
    st->buttons[1] = (uint16_t)(raw[4] | ((uint16_t)raw[5] << 8));
    st->buttons[2] = (uint16_t)(raw[6] & 0x7Fu); /* Bit 7: ungenutzt */
    st->reserved7 = raw[7];
    /* Analog: HIGH-Byte je u16/s16 (Low = Pad im Spiel-Block). */
    st->aiming_x = raw[9];
    st->aiming_y = raw[11];
    st->rot_lever = (int8_t)raw[13];
    st->sight_x = (int8_t)raw[15];
    st->sight_y = (int8_t)raw[17];
    st->pedal_l = raw[19];
    st->pedal_m = raw[21];
    st->pedal_r = raw[23];
    st->tuner = (uint8_t)(raw[24] & 0x0Fu);
    st->gear = raw[25];
    return SBC_OK;
}

/* OUT-Nibbles je Byte 2..20: {low-LED, high-LED}, -1 = reserviert. */
static const int8_t kLedNib[19][2] = {
    { SBC_LED_EJECT, SBC_LED_HATCH },         /* Byte 2 */
    { SBC_LED_IGNITION, SBC_LED_START },      /* Byte 3 */
    { SBC_LED_OPENCLOSE, SBC_LED_MAPZOOM },   /* Byte 4 */
    { SBC_LED_MODE, SBC_LED_SUBMON },         /* Byte 5 */
    { SBC_LED_ZOOMIN, SBC_LED_ZOOMOUT },      /* Byte 6 */
    { SBC_LED_FSS, SBC_LED_MANIP },           /* Byte 7 */
    { SBC_LED_LINECOLOR, SBC_LED_WASHING },   /* Byte 8 */
    { SBC_LED_EXTING, SBC_LED_CHAFF },        /* Byte 9 */
    { SBC_LED_TANKDETACH, SBC_LED_OVERRIDE }, /* Byte 10 */
    { SBC_LED_NIGHTSCOPE, SBC_LED_F1 },       /* Byte 11 */
    { SBC_LED_F2, SBC_LED_F3 },               /* Byte 12 */
    { SBC_LED_MAINWC, SBC_LED_SUBWC },        /* Byte 13 */
    { SBC_LED_MAGAZINE, SBC_LED_COMM1 },      /* Byte 14 */
    { SBC_LED_COMM2, SBC_LED_COMM3 },         /* Byte 15 */
    { SBC_LED_COMM4, SBC_LED_COMM5 },         /* Byte 16 */
    { -1, SBC_LED_GEAR_R },                   /* Byte 17 */
    { SBC_LED_GEAR_N, SBC_LED_GEAR_1 },       /* Byte 18 */
    { SBC_LED_GEAR_2, SBC_LED_GEAR_3 },       /* Byte 19 */
    { SBC_LED_GEAR_4, SBC_LED_GEAR_5 },       /* Byte 20 */
};

void sbc_usb_build_led_report(const sbc_leds_t *leds, uint8_t out[SBC_USB_OUT_LEN])
{
    int i;
    static const sbc_leds_t kOff; /* alles 0 */
    if (!out) {
        return;
    }
    if (!leds) {
        leds = &kOff;
    }
    memset(out, 0, SBC_USB_OUT_LEN);
    out[0] = 0x00;
    out[1] = SBC_USB_OUT_LEN;
    for (i = 0; i < 19; i++) {
        uint8_t lo = 0, hi = 0;
        if (kLedNib[i][0] >= 0) {
            lo = (uint8_t)(leds->level[kLedNib[i][0]] & 0x0Fu);
        }
        if (kLedNib[i][1] >= 0) {
            hi = (uint8_t)(leds->level[kLedNib[i][1]] & 0x0Fu);
        }
        out[2 + i] = (uint8_t)(lo | (uint8_t)(hi << 4));
    }
    /* out[21] bleibt 0 (ungenutzt). */
}

int sbc_usb_parse_led_report(const uint8_t raw[SBC_USB_OUT_LEN], sbc_leds_t *leds)
{
    int i;
    if (!raw || !leds) {
        return SBC_ERR_ARG;
    }
    if (raw[0] != 0x00 || raw[1] != SBC_USB_OUT_LEN) {
        return SBC_ERR_BADREPORT;
    }
    memset(leds, 0, sizeof *leds);
    for (i = 0; i < 19; i++) {
        uint8_t b = raw[2 + i];
        if (kLedNib[i][0] >= 0) {
            leds->level[kLedNib[i][0]] = (uint8_t)(b & 0x0Fu);
        }
        if (kLedNib[i][1] >= 0) {
            leds->level[kLedNib[i][1]] = (uint8_t)((b >> 4) & 0x0Fu);
        }
    }
    return SBC_OK;
}

int sbc_usb_state_to_block(const sbc_state_t *st, uint8_t blk[24])
{
    if (!st || !blk) {
        return SBC_ERR_ARG;
    }
    /* Exakt sbc_in_t-Layout (pState+4): buttons, je Pad+Wert, Tuner, Gang. */
    blk[0] = (uint8_t)(st->buttons[0] & 0xFFu);
    blk[1] = (uint8_t)(st->buttons[0] >> 8);
    blk[2] = (uint8_t)(st->buttons[1] & 0xFFu);
    blk[3] = (uint8_t)(st->buttons[1] >> 8);
    blk[4] = (uint8_t)(st->buttons[2] & 0xFFu);
    blk[5] = (uint8_t)(st->buttons[2] >> 8);
    blk[6] = 0; blk[7] = st->aiming_x;
    blk[8] = 0; blk[9] = st->aiming_y;
    blk[10] = 0; blk[11] = (uint8_t)st->rot_lever;
    blk[12] = 0; blk[13] = (uint8_t)st->sight_x;
    blk[14] = 0; blk[15] = (uint8_t)st->sight_y;
    blk[16] = 0; blk[17] = st->pedal_l;
    blk[18] = 0; blk[19] = st->pedal_m;
    blk[20] = 0; blk[21] = st->pedal_r;
    blk[22] = st->tuner;
    blk[23] = st->gear;
    return SBC_OK;
}

void sbc_usb_leds_all(sbc_leds_t *leds, uint8_t level)
{
    int i;
    if (!leds) {
        return;
    }
    level &= 0x0Fu;
    for (i = 0; i < SBC_LED_COUNT; i++) {
        leds->level[i] = level;
    }
}

/* ---- WinUSB-Geraetezugriff ---- */

/* 4 Hexziffern ab p lesen (gross/klein), -1 bei Fehler. */
static int hex4(const char *p)
{
    int i, v = 0;
    for (i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v |= c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v |= c - 'A' + 10;
        } else {
            return -1;
        }
    }
    return v;
}

/* Device-Path enthaelt "vid_0a7b&pid_d000" (Standardformat, klein)? */
static int path_matches_sbc(const char *path)
{
    /* Gross/klein-tolerant suchen (SetupAPI liefert klein, sicher ist sicher). */
    const char *p = path;
    int vid = 0, pid = 0;
    while (*p) {
        if ((p[0] == 'v' || p[0] == 'V') && (p[1] == 'i' || p[1] == 'I') &&
            (p[2] == 'd' || p[2] == 'D') && p[3] == '_') {
            if (hex4(p + 4) == SBC_USB_VID) {
                vid = 1;
            }
        }
        if ((p[0] == 'p' || p[0] == 'P') && (p[1] == 'i' || p[1] == 'I') &&
            (p[2] == 'd' || p[2] == 'D') && p[3] == '_') {
            if (hex4(p + 4) == SBC_USB_PID) {
                pid = 1;
            }
        }
        p++;
    }
    return vid && pid;
}

int sbc_usb_open(sbc_usb_dev_t **out)
{
    HDEVINFO set = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA ifd;
    DWORD idx = 0;
    int seen_but_unusable = 0;
    sbc_usb_dev_t *dev = NULL;

    if (!out) {
        return SBC_ERR_ARG;
    }
    *out = NULL;

    set = SetupDiGetClassDevsA(&GUID_DEVINTERFACE_USB_DEVICE, NULL, NULL,
                               DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        return SBC_ERR_DRIVER;
    }
    memset(&ifd, 0, sizeof ifd);
    ifd.cbSize = sizeof ifd;
    while (SetupDiEnumDeviceInterfaces(set, NULL, &GUID_DEVINTERFACE_USB_DEVICE,
                                       idx++, &ifd)) {
        DWORD need = 0;
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *det = NULL;
        HANDLE fh = INVALID_HANDLE_VALUE;
        WINUSB_INTERFACE_HANDLE wh = NULL;
        USB_INTERFACE_DESCRIPTOR idesc;
        UCHAR pin = 0, pout = 0;
        UCHAR i;

        SetupDiGetDeviceInterfaceDetailA(set, &ifd, NULL, 0, &need, NULL);
        if (need < sizeof(*det)) {
            continue;
        }
        det = (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)malloc(need);
        if (!det) {
            continue;
        }
        det->cbSize = sizeof(*det);
        if (!SetupDiGetDeviceInterfaceDetailA(set, &ifd, det, need, NULL, NULL)) {
            free(det);
            continue;
        }
        if (!path_matches_sbc(det->DevicePath)) {
            free(det);
            continue;
        }
        /* Pfad passt: oeffnen + WinUSB binden. */
        fh = CreateFileA(det->DevicePath, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        free(det);
        det = NULL;
        if (fh == INVALID_HANDLE_VALUE) {
            seen_but_unusable = 1;
            continue;
        }
        if (!WinUsb_Initialize(fh, &wh)) {
            CloseHandle(fh);
            seen_but_unusable = 1;
            continue;
        }
        if (!WinUsb_QueryInterfaceSettings(wh, 0, &idesc)) {
            WinUsb_Free(wh);
            CloseHandle(fh);
            seen_but_unusable = 1;
            continue;
        }
        for (i = 0; i < idesc.bNumEndpoints; i++) {
            WINUSB_PIPE_INFORMATION pipe;
            if (!WinUsb_QueryPipe(wh, 0, i, &pipe)) {
                continue;
            }
            if (pipe.PipeType != UsbdPipeTypeInterrupt) {
                continue;
            }
            if (USB_ENDPOINT_DIRECTION_IN(pipe.PipeId)) {
                if (pipe.MaximumPacketSize >= SBC_USB_IN_LEN && !pin) {
                    pin = pipe.PipeId;
                }
            } else {
                if (pipe.MaximumPacketSize >= SBC_USB_OUT_LEN && !pout) {
                    pout = pipe.PipeId;
                }
            }
        }
        if (!pin || !pout) {
            WinUsb_Free(wh);
            CloseHandle(fh);
            seen_but_unusable = 1;
            continue;
        }
        dev = (sbc_usb_dev_t *)calloc(1, sizeof *dev);
        if (!dev) {
            WinUsb_Free(wh);
            CloseHandle(fh);
            SetupDiDestroyDeviceInfoList(set);
            return SBC_ERR_DRIVER;
        }
        dev->file = fh;
        dev->wh = wh;
        dev->pipe_in = pin;
        dev->pipe_out = pout;
        {
            /* Timeouts: IN kurz (Idle = NAK -> TIMEOUT), OUT grosszuegig. */
            ULONG ms = 100;
            WinUsb_SetPipePolicy(wh, pin, PIPE_TRANSFER_TIMEOUT, sizeof ms, &ms);
            ms = 500;
            WinUsb_SetPipePolicy(wh, pout, PIPE_TRANSFER_TIMEOUT, sizeof ms, &ms);
        }
        *out = dev;
        SetupDiDestroyDeviceInfoList(set);
        return SBC_OK;
    }
    SetupDiDestroyDeviceInfoList(set);
    return seen_but_unusable ? SBC_ERR_DRIVER : SBC_ERR_NODEVICE;
}

void sbc_usb_close(sbc_usb_dev_t *dev)
{
    if (!dev) {
        return;
    }
    if (dev->wh) {
        WinUsb_Free(dev->wh);
    }
    if (dev->file && dev->file != INVALID_HANDLE_VALUE) {
        CloseHandle(dev->file);
    }
    free(dev);
}

int sbc_usb_poll_raw(sbc_usb_dev_t *dev, uint8_t raw[SBC_USB_RAW_MAX], unsigned *len)
{
    ULONG got = 0;
    if (!dev || !raw || !len) {
        return SBC_ERR_ARG;
    }
    *len = 0;
    memset(raw, 0, SBC_USB_RAW_MAX);
    /* W128: Puffer = volle Paketgroesse (32), damit ein laengeres Paket
     * keinen Ueberlauf-Fehler erzeugt; Header prueft der Aufrufer. */
    if (!WinUsb_ReadPipe(dev->wh, dev->pipe_in, raw, SBC_USB_RAW_MAX, &got, NULL)) {
        DWORD e = GetLastError();
        if (e == ERROR_SEM_TIMEOUT) {
            return SBC_ERR_TIMEOUT;
        }
        return SBC_ERR_IO;
    }
    *len = (unsigned)got;
    return got ? SBC_OK : SBC_ERR_TIMEOUT;
}

int sbc_usb_get_report(sbc_usb_dev_t *dev, uint8_t raw[SBC_USB_RAW_MAX], unsigned *len)
{
    WINUSB_SETUP_PACKET sp;
    ULONG got = 0;
    if (!dev || !raw || !len) {
        return SBC_ERR_ARG;
    }
    *len = 0;
    memset(raw, 0, SBC_USB_RAW_MAX);
    sp.RequestType = 0xA1; /* IN | Klasse | Interface */
    sp.Request = 0x01;     /* GET_REPORT */
    sp.Value = 0x0100;     /* Input-Report, ID 0 */
    sp.Index = 0;
    sp.Length = SBC_USB_IN_LEN;
    if (!WinUsb_ControlTransfer(dev->wh, sp, raw, SBC_USB_IN_LEN, &got, NULL)) {
        return SBC_ERR_IO;
    }
    *len = (unsigned)got;
    return SBC_OK;
}

int sbc_usb_poll(sbc_usb_dev_t *dev, sbc_state_t *st)
{
    uint8_t raw[SBC_USB_RAW_MAX];
    unsigned got = 0;
    int rc;
    if (!dev || !st) {
        return SBC_ERR_ARG;
    }
    rc = sbc_usb_poll_raw(dev, raw, &got);
    if (rc != SBC_OK) {
        return rc;
    }
    if (got < SBC_USB_IN_LEN) {
        return SBC_ERR_BADREPORT;
    }
    return sbc_usb_parse_report(raw, st);
}

int sbc_usb_set_leds(sbc_usb_dev_t *dev, const sbc_leds_t *leds)
{
    uint8_t out[SBC_USB_OUT_LEN];
    ULONG sent = 0;
    if (!dev || !leds) {
        return SBC_ERR_ARG;
    }
    sbc_usb_build_led_report(leds, out);
    if (!WinUsb_WritePipe(dev->wh, dev->pipe_out, out, sizeof out, &sent, NULL)) {
        return SBC_ERR_IO;
    }
    return sent == sizeof out ? SBC_OK : SBC_ERR_IO;
}

int sbc_usb_send_out_raw(sbc_usb_dev_t *dev, const uint8_t out[SBC_USB_OUT_LEN])
{
    ULONG sent = 0;
    uint8_t buf[SBC_USB_OUT_LEN];
    if (!dev || !out) {
        return SBC_ERR_ARG;
    }
    memcpy(buf, out, sizeof buf);
    if (!WinUsb_WritePipe(dev->wh, dev->pipe_out, buf, sizeof buf, &sent, NULL)) {
        return SBC_ERR_IO;
    }
    return sent == sizeof buf ? SBC_OK : SBC_ERR_IO;
}

/* ---- W128: Kalibrierung (rein, ohne Geraet) ---- */

static const char *const kAxName[SBC_AX_COUNT] = {
    "aim_x", "aim_y", "rot", "sight_x", "sight_y",
    "pedal_l", "pedal_m", "pedal_r"
};

const char *sbc_usb_axis_name(int axis)
{
    return (axis >= 0 && axis < SBC_AX_COUNT) ? kAxName[axis] : "?";
}

int sbc_usb_axis_find(const char *name)
{
    int i;
    if (!name) {
        return -1;
    }
    for (i = 0; i < SBC_AX_COUNT; i++) {
        if (strcmp(name, kAxName[i]) == 0) {
            return i;
        }
    }
    return -1;
}

int sbc_usb_axis_is_aim(int axis)
{
    return axis == SBC_AX_AIM_X || axis == SBC_AX_AIM_Y;
}

static int ax_is_aim(int a) { return a == SBC_AX_AIM_X || a == SBC_AX_AIM_Y; }
static int ax_is_pedal(int a) { return a >= SBC_AX_PEDAL_L; }
static int ax_is_signed(int a) { return a == SBC_AX_ROT || a == SBC_AX_SIGHT_X || a == SBC_AX_SIGHT_Y; }

/* Nennwerte (Spiel-Einheiten) je Achse: min, Mitte, max. */
static void ax_nominal(int a, int *mn, int *md, int *mx)
{
    if (ax_is_signed(a)) {
        *mn = -128; *md = 0; *mx = 127;
    } else if (ax_is_pedal(a)) {
        *mn = 0; *md = 0; *mx = 255;
    } else {
        *mn = 0; *md = 0x80; *mx = 255;
    }
}

int sbc_usb_cal_set(sbc_cal_t *cal, const char *key, const char *val)
{
    int a, mn, md, mx;
    sbc_axis_cal_t *c;
    if (!cal || !key || !val) {
        return 0;
    }
    if (strcmp(key, "sbc_gear_map") == 0) {
        int v[7], n, i, j;
        if (!val[0]) {
            cal->gear_map_set = 0;
            return 1;
        }
        n = sscanf_s(val, "%d , %d , %d , %d , %d , %d , %d",
                     &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]);
        if (n != 7) {
            return -1;
        }
        for (i = 0; i < 7; i++) {
            if (v[i] < -128 || v[i] > 255) {
                return -1;
            }
            for (j = 0; j < i; j++) {
                if ((v[j] & 0xFF) == (v[i] & 0xFF)) {
                    return -1; /* doppelt */
                }
            }
        }
        for (i = 0; i < 7; i++) {
            cal->gear_raw[i] = (uint8_t)(v[i] & 0xFF);
        }
        cal->gear_map_set = 1;
        return 1;
    }
    if (strncmp(key, "sbc_cal_", 8) == 0) {
        int v[3], n;
        a = sbc_usb_axis_find(key + 8);
        if (a < 0) {
            return -1;
        }
        c = &cal->ax[a];
        ax_nominal(a, &mn, &md, &mx);
        if (!val[0]) { /* leer = Raender aus (Totzone bleibt) */
            c->lo = mn; c->mid = md; c->hi = mx;
            c->set = c->dead > 0;
            return 1;
        }
        n = sscanf_s(val, "%d , %d , %d", &v[0], &v[1], &v[2]);
        if (ax_is_aim(a)) {
            /* W141: Zielhebel absolut (Reibungshebel, keine Mitte, keine
             * Totzone): linear lo..hi -> 0..255. Drei-Wert-Form lesen,
             * mid ignorieren (Aufrufer loggt, SBC_CAL_NOTE). */
            if (n == 2) {
                if (v[0] < mn || v[1] > mx || v[1] <= v[0]) {
                    return -1;
                }
                c->lo = v[0]; c->mid = v[0]; c->hi = v[1];
            } else if (n == 3) {
                if (v[0] < mn || v[2] > mx || v[2] <= v[0]) {
                    return -1;
                }
                c->lo = v[0]; c->mid = v[0]; c->hi = v[2];
                c->set = 1;
                return SBC_CAL_NOTE;
            } else {
                return -1;
            }
        } else if (ax_is_pedal(a)) {
            if (n < 2 || v[0] < mn || v[1] > mx || v[1] <= v[0]) {
                return -1;
            }
            c->lo = v[0]; c->mid = v[0]; c->hi = v[1];
        } else {
            if (n != 3 || v[0] < mn || v[2] > mx || !(v[0] < v[1] && v[1] < v[2])) {
                return -1;
            }
            c->lo = v[0]; c->mid = v[1]; c->hi = v[2];
        }
        c->set = 1;
        return 1;
    }
    if (strncmp(key, "sbc_dead_", 9) == 0) {
        int d;
        a = sbc_usb_axis_find(key + 9);
        if (a < 0) {
            return -1;
        }
        c = &cal->ax[a];
        d = atoi(val);
        if (d < 0 || d > 100) {
            return -1;
        }
        if (ax_is_aim(a)) {
            /* W141: keine Totzone am Reibungshebel - ignorieren
             * (Aufrufer loggt, SBC_CAL_NOTE). */
            return SBC_CAL_NOTE;
        }
        if (!c->set && c->hi == 0 && c->lo == 0) { /* Raender = Nennwerte */
            ax_nominal(a, &mn, &md, &mx);
            c->lo = mn; c->mid = md; c->hi = mx;
        }
        c->dead = d;
        c->set = 1;
        return 1;
    }
    return 0;
}

/* Linear: v in [from_lo..from_hi] -> [to_lo..to_hi], gerundet + geklemmt. */
static int ax_lerp(int v, int from_lo, int from_hi, int to_lo, int to_hi)
{
    int r;
    if (from_hi <= from_lo) {
        return to_hi;
    }
    if (v <= from_lo) {
        return to_lo;
    }
    if (v >= from_hi) {
        return to_hi;
    }
    r = to_lo + ((v - from_lo) * (to_hi - to_lo) + (from_hi - from_lo) / 2) /
                (from_hi - from_lo);
    return r;
}

static int ax_map(const sbc_axis_cal_t *c, int a, int v)
{
    int mn, md, mx;
    ax_nominal(a, &mn, &md, &mx);
    if (ax_is_aim(a)) {
        return ax_lerp(v, c->lo, c->hi, mn, mx);
    }
    if (ax_is_pedal(a)) {
        int base = c->lo + c->dead;
        if (v <= base) {
            return 0;
        }
        return ax_lerp(v, base, c->hi, 0, mx);
    }
    if (v >= c->mid - c->dead && v <= c->mid + c->dead) {
        return md;
    }
    if (v > c->mid) {
        return ax_lerp(v, c->mid + c->dead, c->hi, md, mx);
    }
    return ax_lerp(v, c->lo, c->mid - c->dead, mn, md);
}

int sbc_usb_gear_effective(int raw)
{
    if (raw == 254) return 0; /* R (Mapper-Spezialfall 0x4A638) */
    if (raw == 255) return 1; /* N (Mapper-Spezialfall 0x4A621) */
    if (raw == 0) return -1;  /* dazwischen: halten */
    switch (raw & 7) {
    case 7: return 0; /* R */
    case 0: return 1; /* N */
    case 1: return 2; /* 1. */
    case 2: return 3;
    case 3: return 4;
    case 4: return 5;
    case 5: return 6; /* 5. */
    default: return -2; /* Klasse 6 (6,14,..): Out 0x80 = ungueltig */
    }
}

void sbc_usb_cal_apply(const sbc_cal_t *cal, sbc_state_t *st)
{
    const sbc_axis_cal_t *c;
    if (!cal || !st) {
        return;
    }
    c = cal->ax;
    if (c[SBC_AX_AIM_X].set) st->aiming_x = (uint8_t)ax_map(&c[SBC_AX_AIM_X], SBC_AX_AIM_X, st->aiming_x);
    if (c[SBC_AX_AIM_Y].set) st->aiming_y = (uint8_t)ax_map(&c[SBC_AX_AIM_Y], SBC_AX_AIM_Y, st->aiming_y);
    if (c[SBC_AX_ROT].set) st->rot_lever = (int8_t)ax_map(&c[SBC_AX_ROT], SBC_AX_ROT, st->rot_lever);
    if (c[SBC_AX_SIGHT_X].set) st->sight_x = (int8_t)ax_map(&c[SBC_AX_SIGHT_X], SBC_AX_SIGHT_X, st->sight_x);
    if (c[SBC_AX_SIGHT_Y].set) st->sight_y = (int8_t)ax_map(&c[SBC_AX_SIGHT_Y], SBC_AX_SIGHT_Y, st->sight_y);
    if (c[SBC_AX_PEDAL_L].set) st->pedal_l = (uint8_t)ax_map(&c[SBC_AX_PEDAL_L], SBC_AX_PEDAL_L, st->pedal_l);
    if (c[SBC_AX_PEDAL_M].set) st->pedal_m = (uint8_t)ax_map(&c[SBC_AX_PEDAL_M], SBC_AX_PEDAL_M, st->pedal_m);
    if (c[SBC_AX_PEDAL_R].set) st->pedal_r = (uint8_t)ax_map(&c[SBC_AX_PEDAL_R], SBC_AX_PEDAL_R, st->pedal_r);
    if (cal->gear_map_set) {
        int k;
        uint8_t g = 0;
        for (k = 0; k < 7; k++) {
            if (st->gear == cal->gear_raw[k]) {
                g = (uint8_t)(7 + k);
                break;
            }
        }
        st->gear = g;
    }
}

/* ---- W128: Auto-Kalibrierung (rein, ohne Geraet) ---- */

int sbc_usb_axis_get(const sbc_state_t *st, int axis)
{
    if (!st) {
        return 0;
    }
    switch (axis) {
    case SBC_AX_AIM_X: return st->aiming_x;
    case SBC_AX_AIM_Y: return st->aiming_y;
    case SBC_AX_ROT: return st->rot_lever;
    case SBC_AX_SIGHT_X: return st->sight_x;
    case SBC_AX_SIGHT_Y: return st->sight_y;
    case SBC_AX_PEDAL_L: return st->pedal_l;
    case SBC_AX_PEDAL_M: return st->pedal_m;
    case SBC_AX_PEDAL_R: return st->pedal_r;
    default: return 0;
    }
}

int sbc_usb_autocal_feed(sbc_autocal_t *ac, const sbc_state_t *st)
{
    int a, calm = 1;
    if (!ac || !st || ac->done) {
        return 0;
    }
    for (a = 0; a < SBC_AX_COUNT; a++) {
        int v = sbc_usb_axis_get(st, a);
        if (ac->n == 0) {
            ac->mn[a] = ac->mx[a] = v;
            ac->sum[a] = 0;
        }
        if (v < ac->mn[a]) ac->mn[a] = v;
        if (v > ac->mx[a]) ac->mx[a] = v;
        ac->sum[a] += v;
        if (!ax_is_aim(a) && ac->mx[a] - ac->mn[a] > 3) {
            calm = 0;
        }
    }
    ac->n++;
    if (!calm) {
        ac->n = 0; /* unruhig: Fenster neu beginnen */
        if (++ac->tries >= SBC_AC_TRIES) {
            ac->done = 1;
            ac->stable = 0;
            return 1;
        }
        return 0;
    }
    if (ac->n >= SBC_AC_WIN) {
        for (a = 0; a < SBC_AX_COUNT; a++) {
            long q = ac->sum[a];
            ac->rest[a] = (int)(q >= 0 ? (q + ac->n / 2) / ac->n : -((-q + ac->n / 2) / ac->n));
        }
        ac->done = 1;
        ac->stable = 1;
        return 1;
    }
    return 0;
}

void sbc_usb_autocal_build(const sbc_autocal_t *ac, const sbc_cal_t *cfg,
                           int axis_dead, int pedal_dead, sbc_cal_t *out)
{
    int a;
    if (!out) {
        return;
    }
    if (cfg) {
        *out = *cfg;
    } else {
        memset(out, 0, sizeof *out);
    }
    if (!ac || !ac->done || !ac->stable) {
        return;
    }
    for (a = 0; a < SBC_AX_COUNT; a++) {
        int mn, md, mx, r = ac->rest[a];
        sbc_axis_cal_t *c = &out->ax[a];
        if (ax_is_aim(a)) {
            continue; /* W141: Reibungshebel ohne Mitte (cfg-Raender gelten) */
        }
        if (cfg && cfg->ax[a].set && (cfg->ax[a].lo != 0 || cfg->ax[a].hi != 0) &&
            !(cfg->ax[a].dead > 0 && cfg->ax[a].lo == (ax_is_signed(a) ? -128 : 0) &&
              cfg->ax[a].hi == (ax_is_signed(a) ? 127 : 255) &&
              cfg->ax[a].mid == (ax_is_signed(a) || ax_is_pedal(a) ? 0 : 0x80))) {
            continue; /* explizite Raender aus der cfg gewinnen */
        }
        ax_nominal(a, &mn, &md, &mx);
        if (ax_is_pedal(a)) {
            int d = (cfg && cfg->ax[a].set && cfg->ax[a].dead > 0) ? cfg->ax[a].dead : pedal_dead;
            if (r < 0 || r > 48 || mx - r - d < 16) {
                continue;
            }
            c->lo = r; c->mid = r; c->hi = mx; c->dead = d; c->set = 1;
        } else {
            int d = (cfg && cfg->ax[a].set && cfg->ax[a].dead > 0) ? cfg->ax[a].dead : axis_dead;
            if (r - md > 48 || md - r > 48 || r - d <= mn || r + d >= mx) {
                continue;
            }
            c->lo = mn; c->mid = r; c->hi = mx; c->dead = d; c->set = 1;
        }
    }
}
