/* input_joy.c - W221 HOTAS (experimentell): DirectInput-8-Geraete
 * (Joystick, HOTAS, Lenkrad, Pedale) als Eingabequelle. Doku: input_joy.h,
 * EINGABE.md Abschnitt 15. dinput8.dll wird erst bei ijoy_init() geladen. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "input_joy.h"

/* Windows-SDK-dinput.h kennt DIDFT_OPTIONAL nicht (nur altes DX-SDK). */
#ifndef DIDFT_OPTIONAL
#define DIDFT_OPTIONAL 0x80000000
#endif

/* GUIDs lokal (kein dxguid.lib noetig, keine Doppel-Symbole im Client). */
static const GUID kIID_DI8W = {0xBF798031, 0x483A, 0x4DA2,
                               {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
#define IJOY_AXGUID(d1)                                                       \
    {(d1), 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}}
static const GUID kG_X = IJOY_AXGUID(0xA36D02E0);
static const GUID kG_Y = IJOY_AXGUID(0xA36D02E1);
static const GUID kG_Z = IJOY_AXGUID(0xA36D02E2);
static const GUID kG_RX = IJOY_AXGUID(0xA36D02F4);
static const GUID kG_RY = IJOY_AXGUID(0xA36D02F5);
static const GUID kG_RZ = IJOY_AXGUID(0xA36D02E3);
static const GUID kG_SL = IJOY_AXGUID(0xA36D02E4);
static const GUID kG_POV = IJOY_AXGUID(0xA36D02F2);

static const char *const kAxName[IJOY_AXES] = {"X",  "Y",  "Z",  "RX",
                                              "RY", "RZ", "S0", "S1"};

const char *ijoy_axis_name(int ax)
{
    return (ax >= 0 && ax < IJOY_AXES) ? kAxName[ax] : "?";
}

int ijoy_axis_parse(const char *name)
{
    int i;
    if (!name) return -1;
    for (i = 0; i < IJOY_AXES; i++) {
        if (_stricmp(name, kAxName[i]) == 0) return i;
    }
    if (_stricmp(name, "SLIDER") == 0 || _stricmp(name, "SL0") == 0) return 6;
    if (_stricmp(name, "SL1") == 0) return 7;
    return -1;
}

/* ---- Spec-Abgleich (rein) ---- */
static int ijoy_icontains(const char *hay, const char *needle)
{
    size_t n = strlen(needle), i;
    if (n == 0) return 1;
    for (; *hay; hay++) {
        for (i = 0; i < n; i++) {
            char a = hay[i], b = needle[i];
            if (!a) return 0;
            if (a >= 'a' && a <= 'z') a = (char)(a - 32);
            if (b >= 'a' && b <= 'z') b = (char)(b - 32);
            if (a != b) break;
        }
        if (i == n) return 1;
    }
    return 0;
}

int ijoy_match_list(const ijoy_info_t *list, int n, const char *spec,
                    int slot)
{
    char s[96];
    char *hash;
    int k = 1, i, hit = 0;
    size_t L;
    if (!list || n <= 0) return -1;
    if (!spec || !spec[0]) return (slot >= 0 && slot < n) ? slot : -1;
    strncpy_s(s, sizeof s, spec, _TRUNCATE);
    hash = strrchr(s, '@');
    if (hash && hash[1] >= '1' && hash[1] <= '9') {
        k = atoi(hash + 1);
        *hash = '\0';
    }
    /* Leerzeichen am Rand abschneiden. */
    while (s[0] == ' ') memmove(s, s + 1, strlen(s));
    L = strlen(s);
    while (L > 0 && s[L - 1] == ' ') s[--L] = '\0';
    if (!s[0]) return (slot >= 0 && slot < n) ? slot : -1;
    for (i = 0; i < n; i++) {
        int m;
        if (s[0] == '{')
            m = _stricmp(list[i].guid, s) == 0 || _stricmp(list[i].pguid, s) == 0;
        else
            m = ijoy_icontains(list[i].name, s) ||
                ijoy_icontains(list[i].product, s);
        if (m && ++hit == k) return i;
    }
    return -1;
}

/* ---- DirectInput ---- */
typedef HRESULT(WINAPI *di8create_fn)(HINSTANCE, DWORD, REFIID, LPVOID *,
                                      LPUNKNOWN);

typedef struct {
    LONG ax[IJOY_AXES];
    DWORD pov[IJOY_POVS];
    BYTE btn[IJOY_BTNS];
} ijoy_raw_t;

static HMODULE g_di_dll;
static LPDIRECTINPUT8W g_di;
static int g_di_tried, g_di_ok;
static DIOBJECTDATAFORMAT g_odf[IJOY_AXES + IJOY_POVS + IJOY_BTNS];
static DIDATAFORMAT g_df;

typedef struct {
    LPDIRECTINPUTDEVICE8W dev;
    GUID inst;
    ijoy_info_t info;
    int fails;
} ijoy_dev_t;

static ijoy_dev_t g_dev[IJOY_MAX_DEV];
static int g_ndev;

static void ijoy_build_format(void)
{
    static const GUID *const axg[IJOY_AXES] = {&kG_X,  &kG_Y,  &kG_Z,
                                               &kG_RX, &kG_RY, &kG_RZ,
                                               &kG_SL, &kG_SL};
    int i, n = 0;
    for (i = 0; i < IJOY_AXES; i++, n++) {
        g_odf[n].pguid = axg[i];
        g_odf[n].dwOfs = (DWORD)(FIELD_OFFSET(ijoy_raw_t, ax) + i * sizeof(LONG));
        g_odf[n].dwType = DIDFT_AXIS | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL;
        g_odf[n].dwFlags = DIDOI_ASPECTPOSITION;
    }
    for (i = 0; i < IJOY_POVS; i++, n++) {
        g_odf[n].pguid = &kG_POV;
        g_odf[n].dwOfs = (DWORD)(FIELD_OFFSET(ijoy_raw_t, pov) + i * sizeof(DWORD));
        g_odf[n].dwType = DIDFT_POV | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL;
        g_odf[n].dwFlags = 0;
    }
    for (i = 0; i < IJOY_BTNS; i++, n++) {
        g_odf[n].pguid = NULL;
        g_odf[n].dwOfs = (DWORD)(FIELD_OFFSET(ijoy_raw_t, btn) + i);
        g_odf[n].dwType = DIDFT_BUTTON | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL;
        g_odf[n].dwFlags = 0;
    }
    g_df.dwSize = sizeof g_df;
    g_df.dwObjSize = sizeof(DIOBJECTDATAFORMAT);
    g_df.dwFlags = DIDF_ABSAXIS;
    g_df.dwDataSize = sizeof(ijoy_raw_t);
    g_df.dwNumObjs = (DWORD)n;
    g_df.rgodf = g_odf;
}

int ijoy_init(void)
{
    di8create_fn create;
    if (g_di_tried) return g_di_ok ? 0 : -1;
    g_di_tried = 1;
    g_di_dll = LoadLibraryW(L"dinput8.dll");
    if (!g_di_dll) return -1;
    create = (di8create_fn)(void *)GetProcAddress(g_di_dll, "DirectInput8Create");
    if (!create) return -1;
    if (FAILED(create(GetModuleHandleW(NULL), DIRECTINPUT_VERSION, &kIID_DI8W,
                      (LPVOID *)&g_di, NULL)) || !g_di)
        return -1;
    ijoy_build_format();
    g_di_ok = 1;
    return 0;
}

static void ijoy_guid_str(const GUID *g, char *out, size_t cap)
{
    _snprintf_s(out, cap, _TRUNCATE,
                "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                (unsigned long)g->Data1, g->Data2, g->Data3, g->Data4[0],
                g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4],
                g->Data4[5], g->Data4[6], g->Data4[7]);
}

static void ijoy_w2u8(const WCHAR *w, char *out, int cap)
{
    int i, ok = 1;
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL)) {
        out[0] = '\0';
        ok = 0;
    }
    (void)ok;
    for (i = 0; out[i]; i++) {
        if (out[i] == '\r' || out[i] == '\n' || out[i] == ',' || out[i] == '=' ||
            out[i] == '#' || out[i] == '@')
            out[i] = ' '; /* profil-sicher */
    }
}

typedef struct {
    DIDEVICEINSTANCEW inst[IJOY_MAX_DEV];
    int n;
} ijoy_enum_t;

static BOOL CALLBACK ijoy_enum_cb(LPCDIDEVICEINSTANCEW di, LPVOID ctx)
{
    ijoy_enum_t *e = (ijoy_enum_t *)ctx;
    DWORD vidpid = di->guidProduct.Data1;
    /* Original-Controller (VID 0x0A7B PID 0xD000) nie doppelt erfassen. */
    if (LOWORD(vidpid) == 0x0A7B && HIWORD(vidpid) == 0xD000)
        return DIENUM_CONTINUE;
    if (e->n < IJOY_MAX_DEV) e->inst[e->n++] = *di;
    return DIENUM_CONTINUE;
}

static int ijoy_is_xinput(LPDIRECTINPUTDEVICE8W dev)
{
    DIPROPGUIDANDPATH gp;
    WCHAR *p;
    memset(&gp, 0, sizeof gp);
    gp.diph.dwSize = sizeof gp;
    gp.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    gp.diph.dwHow = DIPH_DEVICE;
    if (FAILED(IDirectInputDevice8_GetProperty(dev, DIPROP_GUIDANDPATH, &gp.diph)))
        return 0;
    for (p = gp.wszPath; p[0] && p[1] && p[2]; p++) {
        if ((p[0] == L'I' || p[0] == L'i') && (p[1] == L'G' || p[1] == L'g') &&
            p[2] == L'_')
            return 1;
    }
    return 0;
}

static int ijoy_open_dev(const DIDEVICEINSTANCEW *di, ijoy_dev_t *d)
{
    LPDIRECTINPUTDEVICE8W dev = NULL;
    DIPROPRANGE rg;
    DIDEVCAPS caps;
    int i;
    memset(d, 0, sizeof *d);
    if (FAILED(IDirectInput8_CreateDevice(g_di, &di->guidInstance, &dev, NULL)) ||
        !dev)
        return 0;
    if (ijoy_is_xinput(dev) ||
        FAILED(IDirectInputDevice8_SetDataFormat(dev, &g_df))) {
        IDirectInputDevice8_Release(dev);
        return 0;
    }
    /* Ohne SetCooperativeLevel: nicht-exklusiv + Hintergrund (Standard),
     * kein Fenster noetig, kein Fokus-Bezug. */
    memset(&rg, 0, sizeof rg);
    rg.diph.dwSize = sizeof rg;
    rg.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    rg.diph.dwHow = DIPH_DEVICE;
    rg.lMin = -32768;
    rg.lMax = 32767;
    IDirectInputDevice8_SetProperty(dev, DIPROP_RANGE, &rg.diph);
    d->dev = dev;
    d->inst = di->guidInstance;
    ijoy_w2u8(di->tszInstanceName, d->info.name, sizeof d->info.name);
    ijoy_w2u8(di->tszProductName, d->info.product, sizeof d->info.product);
    ijoy_guid_str(&di->guidInstance, d->info.guid, sizeof d->info.guid);
    ijoy_guid_str(&di->guidProduct, d->info.pguid, sizeof d->info.pguid);
    memset(&caps, 0, sizeof caps);
    caps.dwSize = sizeof caps;
    if (SUCCEEDED(IDirectInputDevice8_GetCapabilities(dev, &caps))) {
        d->info.nbtn = (int)(caps.dwButtons > IJOY_BTNS ? IJOY_BTNS : caps.dwButtons);
        d->info.npov = (int)(caps.dwPOVs > IJOY_POVS ? IJOY_POVS : caps.dwPOVs);
    }
    for (i = 0; i < IJOY_AXES; i++) {
        DIDEVICEOBJECTINSTANCEW oi;
        memset(&oi, 0, sizeof oi);
        oi.dwSize = sizeof oi;
        if (SUCCEEDED(IDirectInputDevice8_GetObjectInfo(
                dev, &oi, (DWORD)(FIELD_OFFSET(ijoy_raw_t, ax) + i * sizeof(LONG)),
                DIPH_BYOFFSET)))
            d->info.axmask |= 1u << i;
    }
    IDirectInputDevice8_Acquire(dev);
    return 1;
}

int ijoy_scan(void)
{
    ijoy_enum_t *e;
    ijoy_dev_t nd[IJOY_MAX_DEV];
    int nn = 0, i, j;
    if (ijoy_init() != 0) return 0;
    e = (ijoy_enum_t *)calloc(1, sizeof *e);
    if (!e) return g_ndev;
    IDirectInput8_EnumDevices(g_di, DI8DEVCLASS_GAMECTRL, ijoy_enum_cb, e,
                              DIEDFL_ATTACHEDONLY);
    for (i = 0; i < e->n && nn < IJOY_MAX_DEV; i++) {
        int kept = 0;
        for (j = 0; j < g_ndev; j++) {
            if (g_dev[j].dev && IsEqualGUID(&g_dev[j].inst, &e->inst[i].guidInstance)) {
                nd[nn++] = g_dev[j]; /* offen lassen (kein Aussetzer) */
                g_dev[j].dev = NULL;
                kept = 1;
                break;
            }
        }
        if (!kept && ijoy_open_dev(&e->inst[i], &nd[nn])) nn++;
    }
    for (j = 0; j < g_ndev; j++) {
        if (g_dev[j].dev) {
            IDirectInputDevice8_Unacquire(g_dev[j].dev);
            IDirectInputDevice8_Release(g_dev[j].dev);
        }
    }
    memcpy(g_dev, nd, sizeof(ijoy_dev_t) * (size_t)nn);
    g_ndev = nn;
    free(e);
    return g_ndev;
}

int ijoy_count(void) { return g_ndev; }

const ijoy_info_t *ijoy_info(int i)
{
    return (i >= 0 && i < g_ndev) ? &g_dev[i].info : NULL;
}

int ijoy_read(int i, imap_joy_state_t *st)
{
    ijoy_raw_t r;
    HRESULT hr;
    int k;
    memset(st, 0, sizeof *st);
    for (k = 0; k < IJOY_POVS; k++) st->pov[k] = -1;
    if (i < 0 || i >= g_ndev || !g_dev[i].dev) return 0;
    IDirectInputDevice8_Poll(g_dev[i].dev);
    hr = IDirectInputDevice8_GetDeviceState(g_dev[i].dev, sizeof r, &r);
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
        IDirectInputDevice8_Acquire(g_dev[i].dev);
        IDirectInputDevice8_Poll(g_dev[i].dev);
        hr = IDirectInputDevice8_GetDeviceState(g_dev[i].dev, sizeof r, &r);
    }
    if (FAILED(hr)) {
        g_dev[i].fails++;
        return 0;
    }
    g_dev[i].fails = 0;
    for (k = 0; k < IJOY_AXES; k++) {
        LONG v = r.ax[k];
        if (!(g_dev[i].info.axmask & (1u << k))) v = 0;
        if (v < -32768) v = -32768;
        if (v > 32767) v = 32767;
        st->axis[k] = (int16_t)v;
    }
    for (k = 0; k < IJOY_POVS; k++) {
        DWORD p = r.pov[k];
        st->pov[k] = (k >= g_dev[i].info.npov || LOWORD(p) == 0xFFFF || p >= 36000)
                         ? (int16_t)-1
                         : (int16_t)p;
    }
    for (k = 0; k < IJOY_BTNS; k++) {
        if (r.btn[k] & 0x80) st->btn[k >> 5] |= 1u << (k & 31);
    }
    return 1;
}

void ijoy_shutdown(void)
{
    int j;
    for (j = 0; j < g_ndev; j++) {
        if (g_dev[j].dev) {
            IDirectInputDevice8_Unacquire(g_dev[j].dev);
            IDirectInputDevice8_Release(g_dev[j].dev);
            g_dev[j].dev = NULL;
        }
    }
    g_ndev = 0;
}

/* ---- Client: Slot-Leser im Hintergrund-Thread ---- */
static SRWLOCK g_slk = SRWLOCK_INIT;
static char g_spec[IMAP_JOY_MAX][96];
static unsigned g_used;
static imap_joy_state_t g_slot_st[IMAP_JOY_MAX];
static unsigned g_slot_mask;
static char g_slot_nm[IMAP_JOY_MAX][96];
static volatile LONG g_thr_started;

static DWORD WINAPI ijoy_thread(LPVOID unused)
{
    int map[IMAP_JOY_MAX];
    DWORD last_scan = 0;
    int need_scan = 1, s;
    (void)unused;
    for (s = 0; s < IMAP_JOY_MAX; s++) map[s] = -1;
    for (;;) {
        imap_joy_state_t st[IMAP_JOY_MAX];
        unsigned mask = 0;
        DWORD now = GetTickCount();
        if (need_scan && (last_scan == 0 || now - last_scan >= 3000u)) {
            ijoy_info_t list[IJOY_MAX_DEV];
            int n, i;
            last_scan = now ? now : 1;
            n = ijoy_scan();
            for (i = 0; i < n; i++) list[i] = g_dev[i].info;
            need_scan = 0;
            AcquireSRWLockExclusive(&g_slk);
            for (s = 0; s < IMAP_JOY_MAX; s++) {
                map[s] = (g_used & (1u << s)) ? ijoy_match_list(list, n, g_spec[s], s) : -1;
                if ((g_used & (1u << s)) && map[s] < 0) need_scan = 1;
                if (map[s] >= 0)
                    strncpy_s(g_slot_nm[s], sizeof g_slot_nm[s], list[map[s]].product[0]
                              ? list[map[s]].product : list[map[s]].name, _TRUNCATE);
                else
                    g_slot_nm[s][0] = '\0';
            }
            ReleaseSRWLockExclusive(&g_slk);
        }
        memset(st, 0, sizeof st);
        for (s = 0; s < IMAP_JOY_MAX; s++) {
            if (map[s] < 0) continue;
            if (ijoy_read(map[s], &st[s])) {
                mask |= 1u << s;
            } else if (g_dev[map[s]].fails > 50) {
                need_scan = 1; /* abgezogen? neu aufzaehlen */
                last_scan = 0;
            }
        }
        AcquireSRWLockExclusive(&g_slk);
        memcpy(g_slot_st, st, sizeof st);
        g_slot_mask = mask;
        ReleaseSRWLockExclusive(&g_slk);
        Sleep(4);
    }

}

int ijoy_slots_start(const char specs[IMAP_JOY_MAX][96], unsigned used)
{
    HANDLE h;
    int s;
    if (InterlockedCompareExchange(&g_thr_started, 1, 0) != 0) return 0;
    if (ijoy_init() != 0) return -1;
    for (s = 0; s < IMAP_JOY_MAX; s++)
        strncpy_s(g_spec[s], sizeof g_spec[s], specs[s], _TRUNCATE);
    g_used = used;
    h = CreateThread(NULL, 0, ijoy_thread, NULL, 0, NULL);
    if (!h) return -1;
    SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
    CloseHandle(h);
    return 0;
}

unsigned ijoy_slots_get(imap_joy_state_t out[IMAP_JOY_MAX])
{
    unsigned m;
    AcquireSRWLockShared(&g_slk);
    memcpy(out, g_slot_st, sizeof g_slot_st);
    m = g_slot_mask;
    ReleaseSRWLockShared(&g_slk);
    return m;
}

void ijoy_slot_name(int i, char *out, int cap)
{
    if (cap <= 0) return;
    out[0] = '\0';
    if (i < 0 || i >= IMAP_JOY_MAX) return;
    AcquireSRWLockShared(&g_slk);
    strncpy_s(out, (size_t)cap, g_slot_nm[i], _TRUNCATE);
    ReleaseSRWLockShared(&g_slk);
}
