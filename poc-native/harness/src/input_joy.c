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
#include <stdarg.h>

#include "input_joy.h"
#include "input_despike.h" /* W439 */

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
    int dead;                          /* W305: abgezogen/tot -> freigeben */
    LONG lo[IJOY_AXES], hi[IJOY_AXES]; /* W282: tatsaechlicher Bereich */
    DIOBJECTDATAFORMAT *odf;           /* W282: eigenes Format (Rueckfall) */
    ijoy_obj_t *obj;                   /* W312: Objekt-Metadaten (malloc) */
    int nobj;
} ijoy_dev_t;

static ijoy_dev_t g_dev[IJOY_MAX_DEV];
static int g_ndev;
/* W282: uebersprungene XInput-Geraete der letzten Aufzaehlung */
static char g_skip_nm[8][96];
static int g_nskip;

/* ---- W305 (JOY-HOTPLUG): Test-Backend (nur ijoy_test_*, Selbsttest) ----
 * Ersetzt Aufzaehlen/Oeffnen/Lesen/Freigeben durch Fake-Geraete: "abziehen"
 * laesst das offene Objekt dauerhaft scheitern (GetDeviceState ->
 * DIERR_INPUTLOST, Acquire -> DIERR_UNPLUGGED), "einstecken" meldet das Geraet
 * mit DERSELBEN guidInstance in einer neuen Generation (altes Objekt bleibt
 * tot) - genau der Tester-Fall. Im Spieler-/Launcher-Pfad nie aktiv. */
#define IJOY_FAKE_MAX 4
#define IJOY_FAKE_MAGIC 0x4A305u
typedef struct {
    unsigned magic;
    int k;
    unsigned gen;
} ijoy_fake_h_t;
static int g_test;
static struct {
    int n;
    int present[IJOY_FAKE_MAX];
    unsigned gen[IJOY_FAKE_MAX];
    LONG ax[IJOY_FAKE_MAX];
    ijoy_test_stats_t st;
    /* W312: optionale Objektliste je Fake-Geraet + Werte je Objekt */
    ijoy_obj_t obj[IJOY_FAKE_MAX][IJOY_OBJ_MAX];
    long oval[IJOY_FAKE_MAX][IJOY_OBJ_MAX];
    int nobj[IJOY_FAKE_MAX];
} g_fk;

/* W305: Gesundheit. Schlaegt das Lesen IJOY_FAIL_DROP-mal in Folge fehl
 * (~100 ms im 4-ms-Takt) oder meldet DirectInput DIERR_UNPLUGGED, gilt das
 * Objekt als tot: der Slot-Takt gibt es frei und zaehlt neu auf. Ein
 * behaltenes Objekt uebernimmt ijoy_scan nur, wenn es zuletzt lesbar war. */
#define IJOY_FAIL_DROP 25
#define IJOY_RESCAN_MS 3000u      /* Slot ohne Geraet: neu aufzaehlen */
#define IJOY_RESCAN_DROP_MS 1000u /* nach Freigabe: Abstand ... */
#define IJOY_FAST_WINDOW_MS 10000u /* ... fuer 10 s (schnelles Umstecken) */

int16_t ijoy_norm_axis(long v, long lo, long hi)
{
    long long r;
    if (lo == -32768 && hi == 32767) { /* Normalfall: bitgleich wie bisher */
        if (v < -32768) v = -32768;
        if (v > 32767) v = 32767;
        return (int16_t)v;
    }
    if (hi <= lo) return 0;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    r = ((long long)(v - lo) * 65535 + (long long)(hi - lo) / 2) / (long long)(hi - lo) - 32768;
    if (r < -32768) r = -32768;
    if (r > 32767) r = 32767;
    return (int16_t)r;
}

unsigned ijoy_plan_axes(const int *kind, int n, int *slot)
{
    static const int kFree[IJOY_AXES] = {6, 7, 2, 3, 4, 5, 0, 1};
    unsigned used = 0;
    int i, k;
    for (i = 0; i < n; i++) slot[i] = -1;
    for (i = 0; i < n; i++) { /* 1. Standard-Achsen auf ihren Slot */
        if (kind[i] >= 0 && kind[i] <= 5 && !(used & (1u << kind[i]))) {
            slot[i] = kind[i];
            used |= 1u << kind[i];
        }
    }
    for (i = 0; i < n; i++) { /* 2. Slider auf S0/S1 */
        if (kind[i] == 6) {
            for (k = 6; k <= 7; k++) {
                if (!(used & (1u << k))) {
                    slot[i] = k;
                    used |= 1u << k;
                    break;
                }
            }
        }
    }
    for (i = 0; i < n; i++) { /* 3. Rest (unbekannt, doppelt) auf freie Slots */
        if (slot[i] >= 0) continue;
        for (k = 0; k < IJOY_AXES; k++) {
            if (!(used & (1u << kFree[k]))) {
                slot[i] = kFree[k];
                used |= 1u << kFree[k];
                break;
            }
        }
    }
    return used;
}

/* ---- W312 JOY-CALIB: Objektliste, Format-Entscheidung (rein) ---- */
static int ijoy_obj_is_axis(const ijoy_obj_t *o)
{
    return o->kind >= IJOY_K_X && o->kind <= IJOY_K_AXIS;
}

unsigned ijoy_std_capture(const ijoy_obj_t *o, int n, int *read)
{
    /* Abbild von g_df: Eintraege der Reihe nach, je "erstes noch freies
     * Objekt mit passender GUID und passendem Typ" (DIDFT_ANYINSTANCE). */
    static const int want[IJOY_AXES] = {IJOY_K_X,  IJOY_K_Y,  IJOY_K_Z,
                                        IJOY_K_RX, IJOY_K_RY, IJOY_K_RZ,
                                        IJOY_K_SLIDER, IJOY_K_SLIDER};
    unsigned m = 0;
    int s, i;
    if (n > IJOY_OBJ_MAX) n = IJOY_OBJ_MAX;
    for (i = 0; i < n; i++) read[i] = -1;
    for (s = 0; s < IJOY_AXES; s++) {
        for (i = 0; i < n; i++) {
            if (read[i] < 0 && o[i].kind == want[s]) {
                read[i] = s;
                m |= 1u << s;
                break;
            }
        }
    }
    for (s = 0; s < IJOY_POVS; s++) {
        for (i = 0; i < n; i++) {
            if (read[i] < 0 && o[i].kind == IJOY_K_POV) {
                read[i] = s;
                break;
            }
        }
    }
    for (s = 0; s < IJOY_BTNS; s++) {
        for (i = 0; i < n; i++) {
            if (read[i] < 0 && o[i].kind == IJOY_K_BTN) {
                read[i] = s;
                break;
            }
        }
        if (i == n) break;
    }
    return m;
}

/* W312: Achse mit unbekannter GUID, aber eindeutiger HID-Usage -> bevorzugter
 * Slot (Generic Desktop X..RZ/Slider/Dial, Simulation Aileron/Elevator/
 * Rudder/Throttle). Nur fuer den eigenen Plan; das Standard-Format sieht
 * weiterhin nur die GUID. -1 = keine Vorgabe. */
static int ijoy_usage_hint(unsigned short page, unsigned short usage)
{
    if (page == 0x01 && usage >= 0x30 && usage <= 0x35) return usage - 0x30;
    if (page == 0x01 && (usage == 0x36 || usage == 0x37)) return IJOY_K_SLIDER;
    if (page == 0x02) {
        if (usage == 0xB0) return IJOY_K_X;  /* Aileron  */
        if (usage == 0xB8) return IJOY_K_Y;  /* Elevator */
        if (usage == 0xBA) return IJOY_K_RZ; /* Rudder   */
        if (usage == 0xBB) return IJOY_K_SLIDER; /* Throttle */
    }
    return -1;
}

unsigned ijoy_plan_objects(const ijoy_obj_t *o, int n, int *read)
{
    int kind[IJOY_OBJ_MAX], idx[IJOY_OBJ_MAX], slot[IJOY_OBJ_MAX];
    int na = 0, np = 0, nb = 0, i;
    unsigned m;
    if (n > IJOY_OBJ_MAX) n = IJOY_OBJ_MAX;
    for (i = 0; i < n; i++) {
        read[i] = -1;
        if (ijoy_obj_is_axis(&o[i])) {
            int h = o[i].kind == IJOY_K_AXIS ? ijoy_usage_hint(o[i].upage, o[i].usage) : -1;
            kind[na] = h >= 0 ? h : o[i].kind;
            idx[na++] = i;
        } else if (o[i].kind == IJOY_K_POV && np < IJOY_POVS) {
            read[i] = np++;
        } else if (o[i].kind == IJOY_K_BTN && nb < IJOY_BTNS) {
            read[i] = nb++;
        }
    }
    m = ijoy_plan_axes(kind, na, slot);
    for (i = 0; i < na; i++) read[idx[i]] = slot[i];
    return m;
}

int ijoy_need_own_format(const ijoy_obj_t *o, int n)
{
    int a[IJOY_OBJ_MAX], b[IJOY_OBJ_MAX], i, ca = 0, cb = 0;
    if (n > IJOY_OBJ_MAX) n = IJOY_OBJ_MAX;
    if (n <= 0) return 0;
    ijoy_std_capture(o, n, a);
    ijoy_plan_objects(o, n, b);
    for (i = 0; i < n; i++) {
        ca += a[i] >= 0;
        cb += b[i] >= 0;
    }
    return cb > ca;
}

static const char *ijoy_kind_name(int k)
{
    static const char *const nm[] = {"X", "Y", "Z", "RX", "RY", "RZ", "Slider",
                                     "Axis(unknown GUID)", "POV", "Button", "Other"};
    return (k >= 0 && k <= IJOY_K_OTHER) ? nm[k] : "?";
}

static void ijoy_cat(char *out, int cap, int *len, const char *fmt, ...)
{
    va_list ap;
    int r;
    if (*len >= cap - 1) return;
    va_start(ap, fmt);
    r = _vsnprintf_s(out + *len, (size_t)(cap - *len), _TRUNCATE, fmt, ap);
    va_end(ap);
    *len = r < 0 ? cap - 1 : *len + r;
}

int ijoy_devinfo_text(const ijoy_info_t *in, const ijoy_obj_t *o, int n,
                      char *out, int cap)
{
    static const char *const fmts[3] = {"standard", "axis fallback", "per object"};
    int len = 0, i, na = 0, nr = 0;
    if (!out || cap <= 0) return 0;
    out[0] = '\0';
    if (!in) return 0;
    for (i = 0; i < IJOY_AXES; i++) na += (in->axmask >> i) & 1;
    ijoy_cat(out, cap, &len, "Device: %s\r\n", in->product[0] ? in->product : in->name);
    if (in->name[0] && strcmp(in->name, in->product) != 0)
        ijoy_cat(out, cap, &len, "Instance name: %s\r\n", in->name);
    ijoy_cat(out, cap, &len, "VID/PID: %04X:%04X\r\n", in->vid, in->pid);
    ijoy_cat(out, cap, &len,
             "Driver reports: %d axes, %d buttons, %d POV; devtype 0x%08X, flags 0x%08X%s\r\n",
             in->ncaps_axes, in->ncaps_btn, in->ncaps_pov, in->devtype, in->capflags,
             (in->capflags & 0x100u) ? " (force feedback)" : "");
    ijoy_cat(out, cap, &len, "Read: %d axes (", na);
    for (i = 0; i < IJOY_AXES; i++) {
        if ((in->axmask >> i) & 1)
            ijoy_cat(out, cap, &len, "%s%s", nr++ ? " " : "", ijoy_axis_name(i));
    }
    ijoy_cat(out, cap, &len, "), %d buttons, %d POV; format %s", in->nbtn, in->npov,
             fmts[in->fmt >= 0 && in->fmt <= 2 ? in->fmt : 0]);
    if (in->axnorm) ijoy_cat(out, cap, &len, ", range converted 0x%02X", in->axnorm);
    if (in->open_hr) ijoy_cat(out, cap, &len, ", acquire 0x%08lX", (unsigned long)in->open_hr);
    ijoy_cat(out, cap, &len, "\r\nObjects (%d):\r\n", n);
    ijoy_cat(out, cap, &len,
             " # | name | kind | usage page:usage | type | flags | range | logical | read as\r\n");
    for (i = 0; i < n && i < IJOY_OBJ_MAX; i++) {
        char rd[24], rg[40], lg[40];
        const ijoy_obj_t *b = &o[i];
        if (b->read < 0)
            strcpy_s(rd, sizeof rd, "-");
        else if (ijoy_obj_is_axis(b))
            strcpy_s(rd, sizeof rd, ijoy_axis_name(b->read));
        else if (b->kind == IJOY_K_POV)
            _snprintf_s(rd, sizeof rd, _TRUNCATE, "POV%d", b->read + 1);
        else
            _snprintf_s(rd, sizeof rd, _TRUNCATE, "B%d", b->read + 1);
        if (ijoy_obj_is_axis(b) && b->hi > b->lo)
            _snprintf_s(rg, sizeof rg, _TRUNCATE, "%ld..%ld", b->lo, b->hi);
        else
            strcpy_s(rg, sizeof rg, "-");
        if (b->lhi > b->llo)
            _snprintf_s(lg, sizeof lg, _TRUNCATE, "%ld..%ld", b->llo, b->lhi);
        else
            strcpy_s(lg, sizeof lg, "-");
        ijoy_cat(out, cap, &len, "%2d | %s | %s | %02X:%02X | 0x%08X | 0x%08X%s%s | %s | %s | %s\r\n",
                 i, b->name[0] ? b->name : "?", ijoy_kind_name(b->kind), b->upage, b->usage,
                 b->type, b->flags, (b->type & 0x01000000u) ? " FF" : "",
                 (ijoy_obj_is_axis(b) && !(b->flags & 0x0F00u)) ? " no-aspect" : "", rg, lg, rd);
    }
    if (n > IJOY_OBJ_MAX)
        ijoy_cat(out, cap, &len, "(%d more objects not listed)\r\n", n - IJOY_OBJ_MAX);
    return len;
}

/* ---- W312 JOY-CALIB: Kalibrierung ---- */
typedef struct {
    unsigned short vid, pid;
    int axis;
    ijoy_cal_t c;
} ijoy_cal_ent_t;
static ijoy_cal_ent_t g_cal[IJOY_CAL_MAX];
static int g_ncal;

int16_t ijoy_cal_apply(const ijoy_cal_t *c, int16_t v)
{
    long long r, x = v;
    int dz;
    if (!c || c->hi <= c->lo) return v;
    dz = c->dz < 0 ? 0 : c->dz > 50 ? 50 : c->dz;
    if (x < c->lo) x = c->lo;
    if (x > c->hi) x = c->hi;
    if (c->mid > c->lo && c->mid < c->hi) {
        /* mittig: -1..1 in 1/65536-Einheiten, Totzone um die Mitte */
        long long span = x >= c->mid ? (long long)c->hi - c->mid : (long long)c->mid - c->lo;
        long long f = ((x - c->mid) * 65536) / span; /* -65536..65536 */
        long long af = f < 0 ? -f : f, d = (long long)dz * 65536 / 100;
        if (af <= d)
            af = 0;
        else
            af = ((af - d) * 65536) / (65536 - d);
        r = f < 0 ? -((af * 32768) >> 16) : ((af * 32767) >> 16);
    } else {
        /* linear lo..hi (Schubhebel/Pedal), Totzone am lo-Ende */
        long long f = ((x - c->lo) * 65536) / ((long long)c->hi - c->lo); /* 0..65536 */
        long long d = (long long)dz * 65536 / 100;
        if (f <= d)
            f = 0;
        else
            f = ((f - d) * 65536) / (65536 - d);
        r = ((f * 65535) >> 16) - 32768;
    }
    if (r < -32768) r = -32768;
    if (r > 32767) r = 32767;
    if (c->inv) r = r == -32768 ? 32767 : -r;
    return (int16_t)r;
}

int ijoy_cal_parse(const char *val, unsigned short *vid, unsigned short *pid,
                   int *axis, ijoy_cal_t *c)
{
    char buf[128], an[8], *tok[8], *ctx = NULL, *p;
    unsigned v = 0, d = 0;
    int n = 0, ax, k;
    long lo, mid, hi, dz, inv;
    if (!val || !vid || !pid || !axis || !c) return 0;
    strncpy_s(buf, sizeof buf, val, _TRUNCATE);
    for (p = strtok_s(buf, ",", &ctx); p && n < 8; p = strtok_s(NULL, ",", &ctx)) {
        while (*p == ' ' || *p == '\t') p++;
        tok[n++] = p;
    }
    if (n != 7 || sscanf_s(tok[0], "%x:%x", &v, &d) != 2 || v > 0xFFFF || d > 0xFFFF) return 0;
    strncpy_s(an, sizeof an, tok[1], _TRUNCATE);
    for (k = (int)strlen(an) - 1; k >= 0 && (an[k] == ' ' || an[k] == '\t'); k--) an[k] = 0;
    ax = ijoy_axis_parse(an);
    if (ax < 0) return 0;
    lo = strtol(tok[2], NULL, 10);
    mid = (tok[3][0] == '-' && !(tok[3][1] >= '0' && tok[3][1] <= '9'))
              ? IJOY_CAL_NOMID
              : strtol(tok[3], NULL, 10);
    hi = strtol(tok[4], NULL, 10);
    dz = strtol(tok[5], NULL, 10);
    inv = strtol(tok[6], NULL, 10);
    if (lo < -32768 || hi > 32767 || hi <= lo || dz < 0 || dz > 50) return 0;
    if (mid != IJOY_CAL_NOMID && (mid <= lo || mid >= hi)) mid = IJOY_CAL_NOMID;
    *vid = (unsigned short)v;
    *pid = (unsigned short)d;
    *axis = ax;
    c->lo = (int)lo;
    c->mid = (int)mid;
    c->hi = (int)hi;
    c->dz = (int)dz;
    c->inv = inv ? 1 : 0;
    return 1;
}

int ijoy_cal_format(unsigned short vid, unsigned short pid, int axis,
                    const ijoy_cal_t *c, char *out, int cap)
{
    char m[16];
    if (!c || !out || cap <= 0 || axis < 0 || axis >= IJOY_AXES) return 0;
    if (c->mid == IJOY_CAL_NOMID)
        strcpy_s(m, sizeof m, "-");
    else
        _snprintf_s(m, sizeof m, _TRUNCATE, "%d", c->mid);
    return _snprintf_s(out, (size_t)cap, _TRUNCATE, "%04X:%04X,%s,%d,%s,%d,%d,%d", vid, pid,
                       ijoy_axis_name(axis), c->lo, m, c->hi, c->dz, c->inv ? 1 : 0) > 0;
}

static int ijoy_cal_find(unsigned short vid, unsigned short pid, int axis)
{
    int k;
    for (k = 0; k < g_ncal; k++) {
        if (g_cal[k].vid == vid && g_cal[k].pid == pid && g_cal[k].axis == axis) return k;
    }
    return -1;
}

int ijoy_cal_set(unsigned short vid, unsigned short pid, int axis, const ijoy_cal_t *c)
{
    int k;
    if (axis < 0 || axis >= IJOY_AXES) return -1;
    k = ijoy_cal_find(vid, pid, axis);
    if (!c) {
        if (k >= 0) {
            memmove(&g_cal[k], &g_cal[k + 1], sizeof g_cal[0] * (size_t)(g_ncal - k - 1));
            g_ncal--;
        }
        return 0;
    }
    if (c->hi <= c->lo) return -1;
    if (k < 0) {
        if (g_ncal >= IJOY_CAL_MAX) return -1;
        k = g_ncal++;
    }
    g_cal[k].vid = vid;
    g_cal[k].pid = pid;
    g_cal[k].axis = axis;
    g_cal[k].c = *c;
    return 0;
}

int ijoy_cal_get(unsigned short vid, unsigned short pid, int axis, ijoy_cal_t *c)
{
    int k = ijoy_cal_find(vid, pid, axis);
    if (k < 0) return 0;
    if (c) *c = g_cal[k].c;
    return 1;
}

int ijoy_cal_count(void) { return g_ncal; }

int ijoy_cal_entry(int k, unsigned short *vid, unsigned short *pid, int *axis, ijoy_cal_t *c)
{
    if (k < 0 || k >= g_ncal) return 0;
    if (vid) *vid = g_cal[k].vid;
    if (pid) *pid = g_cal[k].pid;
    if (axis) *axis = g_cal[k].axis;
    if (c) *c = g_cal[k].c;
    return 1;
}

void ijoy_cal_clear(void) { g_ncal = 0; }

int ijoy_skipped_xinput(char names[][96], int max)
{
    int i;
    for (i = 0; i < g_nskip && i < max; i++)
        strncpy_s(names[i], 96, g_skip_nm[i], _TRUNCATE);
    return g_nskip;
}

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
        /* W282 ISSUE6: ohne Aspekt-Zwang (wie c_dfDIJoystick). Mit
         * DIDOI_ASPECTPOSITION fielen Achsen von Treibern, die keinen
         * Aspekt melden (aeltere Geraete), aus dem Format - Tasten gingen,
         * Achsen nicht (Tester-Befund Wingman, Hypothese). */
        g_odf[n].dwFlags = 0;
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
    if (g_test) return 0; /* W305: Fake-Geraete */
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

/* W282 ISSUE6: Achsen-Objekte fuer den Rueckfall (eigenes Format). */
typedef struct {
    int n;
    int kind[IJOY_AXES * 2];
    DWORD type[IJOY_AXES * 2];
} ijoy_axenum_t;

static BOOL CALLBACK ijoy_axobj_cb(LPCDIDEVICEOBJECTINSTANCEW oi, LPVOID ctx)
{
    ijoy_axenum_t *a = (ijoy_axenum_t *)ctx;
    static const GUID *const g[6] = {&kG_X, &kG_Y, &kG_Z, &kG_RX, &kG_RY, &kG_RZ};
    int k, kind = 7;
    if (a->n >= IJOY_AXES * 2) return DIENUM_STOP;
    for (k = 0; k < 6; k++) {
        if (IsEqualGUID(&oi->guidType, g[k])) kind = k;
    }
    if (IsEqualGUID(&oi->guidType, &kG_SL)) kind = 6;
    a->kind[a->n] = kind;
    a->type[a->n] = oi->dwType;
    a->n++;
    return DIENUM_CONTINUE;
}

static unsigned ijoy_axmask_of(LPDIRECTINPUTDEVICE8W dev)
{
    unsigned m = 0;
    int i;
    for (i = 0; i < IJOY_AXES; i++) {
        DIDEVICEOBJECTINSTANCEW oi;
        memset(&oi, 0, sizeof oi);
        oi.dwSize = sizeof oi;
        if (SUCCEEDED(IDirectInputDevice8_GetObjectInfo(
                dev, &oi, (DWORD)(FIELD_OFFSET(ijoy_raw_t, ax) + i * sizeof(LONG)),
                DIPH_BYOFFSET)))
            m |= 1u << i;
    }
    return m;
}

static int ijoy_popcount(unsigned m)
{
    int n = 0;
    for (; m; m &= m - 1) n++;
    return n;
}

/* Rueckfall: meldet der Treiber mehr Achsen, als das Standard-Format
 * (GUID X..RZ, Slider) erfasst, werden die Achsen einzeln per Instanz
 * zugeordnet (ijoy_plan_axes). Tasten/POV bleiben wie im Standard. */
static void ijoy_try_remap(ijoy_dev_t *d, int ncaps)
{
    ijoy_axenum_t ae;
    DIOBJECTDATAFORMAT *odf;
    DIDATAFORMAT df;
    int slot[IJOY_AXES * 2], i, n = 0;
    /* W312: zusaetzlich ausloesen, wenn die Objektliste mehr lesbare Achsen
     * zeigt als das Standard-Format erfasst (DIDEVCAPS.dwAxes kann zu klein
     * sein). Plan dann aus derselben Objektliste (eine Aufzaehlung). */
    int own = d->obj && ijoy_need_own_format(d->obj, d->nobj);
    memset(&ae, 0, sizeof ae);
    if (!own && ncaps <= ijoy_popcount(d->info.axmask)) return;
    if (d->obj && d->nobj > 0) {
        for (i = 0; i < d->nobj && ae.n < IJOY_AXES * 2; i++) {
            if (d->obj[i].kind >= IJOY_K_X && d->obj[i].kind <= IJOY_K_AXIS) {
                int h = d->obj[i].kind == IJOY_K_AXIS
                            ? ijoy_usage_hint(d->obj[i].upage, d->obj[i].usage) : -1;
                ae.kind[ae.n] = h >= 0 ? h : d->obj[i].kind; /* wie ijoy_plan_objects */
                ae.type[ae.n] = d->obj[i].type;
                ae.n++;
            }
        }
    } else {
        IDirectInputDevice8_EnumObjects(d->dev, ijoy_axobj_cb, &ae, DIDFT_AXIS);
    }
    if (ae.n <= ijoy_popcount(d->info.axmask)) return;
    ijoy_plan_axes(ae.kind, ae.n, slot);
    odf = (DIOBJECTDATAFORMAT *)calloc(IJOY_AXES * 2 + IJOY_POVS + IJOY_BTNS,
                                       sizeof *odf);
    if (!odf) return;
    for (i = 0; i < ae.n; i++) {
        if (slot[i] < 0) continue;
        odf[n].pguid = NULL;
        odf[n].dwOfs = (DWORD)(FIELD_OFFSET(ijoy_raw_t, ax) + slot[i] * sizeof(LONG));
        odf[n].dwType = DIDFT_AXIS | DIDFT_MAKEINSTANCE(DIDFT_GETINSTANCE(ae.type[i])) |
                        DIDFT_OPTIONAL;
        odf[n].dwFlags = 0;
        n++;
    }
    for (i = IJOY_AXES; i < IJOY_AXES + IJOY_POVS + IJOY_BTNS; i++) {
        odf[n++] = g_odf[i]; /* POV + Tasten wie im Standard-Format */
    }
    df = g_df;
    df.dwNumObjs = (DWORD)n;
    df.rgodf = odf;
    if (SUCCEEDED(IDirectInputDevice8_SetDataFormat(d->dev, &df))) {
        unsigned m = ijoy_axmask_of(d->dev);
        if (ijoy_popcount(m) > ijoy_popcount(d->info.axmask)) {
            d->odf = odf;
            d->info.axmask = m;
            d->info.axremap = 1;
            d->info.fmt = own ? 2 : 1; /* W312 */
            return;
        }
    }
    IDirectInputDevice8_SetDataFormat(d->dev, &g_df); /* zurueck */
    free(odf);
}

/* Bereich je Achse: erst geraeteweit setzen (wie bisher), dann je Achse
 * setzen und tatsaechlich zuruecklesen. Bleibt ein anderer Bereich (Treiber
 * nimmt DIPROP_RANGE nicht an), rechnet ijoy_read per Software um - vorher
 * wurden solche Werte auf +-32767 abgeschnitten. */
static void ijoy_ranges(ijoy_dev_t *d)
{
    DIPROPRANGE rg;
    int i;
    memset(&rg, 0, sizeof rg);
    rg.diph.dwSize = sizeof rg;
    rg.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    rg.diph.dwHow = DIPH_DEVICE;
    rg.lMin = -32768;
    rg.lMax = 32767;
    IDirectInputDevice8_SetProperty(d->dev, DIPROP_RANGE, &rg.diph);
    for (i = 0; i < IJOY_AXES; i++) {
        d->lo[i] = -32768;
        d->hi[i] = 32767;
        if (!(d->info.axmask & (1u << i))) continue;
        memset(&rg, 0, sizeof rg);
        rg.diph.dwSize = sizeof rg;
        rg.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        rg.diph.dwObj = (DWORD)(FIELD_OFFSET(ijoy_raw_t, ax) + i * sizeof(LONG));
        rg.diph.dwHow = DIPH_BYOFFSET;
        rg.lMin = -32768;
        rg.lMax = 32767;
        IDirectInputDevice8_SetProperty(d->dev, DIPROP_RANGE, &rg.diph);
        rg.lMin = rg.lMax = 0;
        if (SUCCEEDED(IDirectInputDevice8_GetProperty(d->dev, DIPROP_RANGE, &rg.diph)) &&
            rg.lMax > rg.lMin && (rg.lMin != -32768 || rg.lMax != 32767)) {
            d->lo[i] = rg.lMin;
            d->hi[i] = rg.lMax;
            d->info.axnorm |= 1u << i;
        }
    }
}

/* ---- W312 JOY-CALIB: Objektliste des Geraets (nur Metadaten) ---- */
static int ijoy_kind_of(const GUID *g, DWORD type)
{
    static const GUID *const ax[6] = {&kG_X, &kG_Y, &kG_Z, &kG_RX, &kG_RY, &kG_RZ};
    int k;
    if (type & (DIDFT_NODATA | DIDFT_COLLECTION)) return IJOY_K_OTHER;
    if (type & DIDFT_AXIS) {
        for (k = 0; k < 6; k++) {
            if (IsEqualGUID(g, ax[k])) return k;
        }
        return IsEqualGUID(g, &kG_SL) ? IJOY_K_SLIDER : IJOY_K_AXIS;
    }
    if (type & DIDFT_POV) return IJOY_K_POV;
    if (type & DIDFT_BUTTON) return IJOY_K_BTN;
    return IJOY_K_OTHER;
}

typedef struct {
    ijoy_obj_t *o;
    int n, total;
} ijoy_objenum_t;

static BOOL CALLBACK ijoy_allobj_cb(LPCDIDEVICEOBJECTINSTANCEW oi, LPVOID ctx)
{
    ijoy_objenum_t *e = (ijoy_objenum_t *)ctx;
    ijoy_obj_t *b;
    e->total++;
    if (e->n >= IJOY_OBJ_MAX) return DIENUM_CONTINUE;
    b = &e->o[e->n++];
    memset(b, 0, sizeof *b);
    b->kind = ijoy_kind_of(&oi->guidType, oi->dwType);
    b->type = oi->dwType;
    b->flags = oi->dwFlags;
    if (oi->dwSize >= sizeof(DIDEVICEOBJECTINSTANCEW)) {
        b->upage = oi->wUsagePage;
        b->usage = oi->wUsage;
    }
    ijoy_w2u8(oi->tszName, b->name, sizeof b->name);
    b->read = -1;
    return DIENUM_CONTINUE;
}

static void ijoy_enum_objects(ijoy_dev_t *d)
{
    ijoy_objenum_t e;
    memset(&e, 0, sizeof e);
    e.o = (ijoy_obj_t *)calloc(IJOY_OBJ_MAX, sizeof *e.o);
    if (!e.o) return;
    IDirectInputDevice8_EnumObjects(d->dev, ijoy_allobj_cb, &e, DIDFT_ALL);
    if (e.n == 0) {
        free(e.o);
        return;
    }
    d->obj = e.o;
    d->nobj = e.n;
}

static long ijoy_prop_range(LPDIRECTINPUTDEVICE8W dev, REFGUID prop, DWORD how,
                            DWORD obj, long *lo, long *hi)
{
    DIPROPRANGE rg;
    memset(&rg, 0, sizeof rg);
    rg.diph.dwSize = sizeof rg;
    rg.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    rg.diph.dwObj = obj;
    rg.diph.dwHow = how;
    *lo = *hi = 0;
    if (FAILED(IDirectInputDevice8_GetProperty(dev, prop, &rg.diph))) return 0;
    *lo = rg.lMin;
    *hi = rg.lMax;
    return 1;
}

/* Nach dem endgueltigen Format: welches Objekt landet tatsaechlich wo
 * (GetObjectInfo je Datenfeld) und welche Bereiche gelten. */
static void ijoy_fill_reads(ijoy_dev_t *d)
{
    int i, k;
    if (!d->obj) return;
    for (i = 0; i < d->nobj; i++) {
        ijoy_obj_t *b = &d->obj[i];
        b->read = -1;
        if (b->kind <= IJOY_K_AXIS) {
            ijoy_prop_range(d->dev, DIPROP_RANGE, DIPH_BYID, b->type, &b->lo, &b->hi);
            ijoy_prop_range(d->dev, DIPROP_LOGICALRANGE, DIPH_BYID, b->type, &b->llo, &b->lhi);
        }
    }
    for (k = 0; k < IJOY_AXES + IJOY_POVS + IJOY_BTNS; k++) {
        DIDEVICEOBJECTINSTANCEW oi;
        DWORD ofs = k < IJOY_AXES ? (DWORD)(FIELD_OFFSET(ijoy_raw_t, ax) + k * sizeof(LONG))
                    : k < IJOY_AXES + IJOY_POVS
                        ? (DWORD)(FIELD_OFFSET(ijoy_raw_t, pov) + (k - IJOY_AXES) * sizeof(DWORD))
                        : (DWORD)(FIELD_OFFSET(ijoy_raw_t, btn) + (k - IJOY_AXES - IJOY_POVS));
        memset(&oi, 0, sizeof oi);
        oi.dwSize = sizeof oi;
        if (FAILED(IDirectInputDevice8_GetObjectInfo(d->dev, &oi, ofs, DIPH_BYOFFSET))) continue;
        for (i = 0; i < d->nobj; i++) {
            if (d->obj[i].type == oi.dwType) {
                d->obj[i].read = k < IJOY_AXES ? k
                                 : k < IJOY_AXES + IJOY_POVS ? k - IJOY_AXES
                                                             : k - IJOY_AXES - IJOY_POVS;
                break;
            }
        }
    }
}

/* W312: Fake-Geraet mit Objektliste - dieselbe Entscheidung wie echt. */
static void ijoy_fake_open_objs(ijoy_dev_t *d, int k)
{
    int rd[IJOY_OBJ_MAX], i, n = g_fk.nobj[k], nax = 0, own;
    unsigned m;
    d->obj = (ijoy_obj_t *)calloc(IJOY_OBJ_MAX, sizeof *d->obj);
    if (!d->obj) return;
    memcpy(d->obj, g_fk.obj[k], sizeof(ijoy_obj_t) * (size_t)n);
    d->nobj = n;
    for (i = 0; i < n; i++) nax += d->obj[i].kind <= IJOY_K_AXIS;
    m = ijoy_std_capture(d->obj, n, rd);
    own = ijoy_need_own_format(d->obj, n); /* = echter Weg (ncaps = Achsobjekte) */
    if (own) m = ijoy_plan_objects(d->obj, n, rd);
    d->info.fmt = own ? 2 : 0;
    d->info.axremap = own;
    d->info.axmask = m;
    d->info.ncaps_axes = nax;
    d->info.nbtn = d->info.npov = 0;
    for (i = 0; i < n; i++) {
        ijoy_obj_t *b = &d->obj[i];
        b->read = rd[i];
        if (b->kind == IJOY_K_BTN) {
            d->info.ncaps_btn++;
            if (rd[i] >= 0) d->info.nbtn++;
        } else if (b->kind == IJOY_K_POV) {
            d->info.ncaps_pov++;
            if (rd[i] >= 0) d->info.npov++;
        } else if (b->kind <= IJOY_K_AXIS && rd[i] >= 0) {
            /* Treiber behaelt seinen Bereich (lo/hi des Objekts) bzw. nimmt
             * -32768..32767 an (lo == hi: Standard) */
            if (b->hi > b->lo && (b->lo != -32768 || b->hi != 32767)) {
                d->lo[rd[i]] = b->lo;
                d->hi[rd[i]] = b->hi;
                d->info.axnorm |= 1u << rd[i];
            } else {
                b->lo = -32768;
                b->hi = 32767;
            }
        }
    }
}

/* 1 = geoeffnet, 2 = XInput (uebersprungen, Pad-Pfad), 0 = Fehler. */
static int ijoy_open_dev(const DIDEVICEINSTANCEW *di, ijoy_dev_t *d)
{
    LPDIRECTINPUTDEVICE8W dev = NULL;
    DIDEVCAPS caps;
    int ncaps = 0;
    memset(d, 0, sizeof *d);
    if (g_test) { /* W305: Fake-Geraet (k = guidInstance.Data4[7]) */
        ijoy_fake_h_t *h = (ijoy_fake_h_t *)calloc(1, sizeof *h);
        int i;
        if (!h) return 0;
        h->magic = IJOY_FAKE_MAGIC;
        h->k = di->guidInstance.Data4[7] % IJOY_FAKE_MAX;
        h->gen = g_fk.gen[h->k];
        d->dev = (LPDIRECTINPUTDEVICE8W)(void *)h;
        d->inst = di->guidInstance;
        ijoy_w2u8(di->tszInstanceName, d->info.name, sizeof d->info.name);
        ijoy_w2u8(di->tszProductName, d->info.product, sizeof d->info.product);
        ijoy_guid_str(&di->guidInstance, d->info.guid, sizeof d->info.guid);
        ijoy_guid_str(&di->guidProduct, d->info.pguid, sizeof d->info.pguid);
        d->info.nbtn = 8;
        d->info.axmask = 0x3u; /* X, Y */
        for (i = 0; i < IJOY_AXES; i++) {
            d->lo[i] = -32768;
            d->hi[i] = 32767;
        }
        d->info.vid = LOWORD(di->guidProduct.Data1);
        d->info.pid = HIWORD(di->guidProduct.Data1);
        if (g_fk.nobj[h->k] > 0) ijoy_fake_open_objs(d, h->k); /* W312 */
        g_fk.st.opens++;
        return 1;
    }
    if (FAILED(IDirectInput8_CreateDevice(g_di, &di->guidInstance, &dev, NULL)) ||
        !dev)
        return 0;
    if (ijoy_is_xinput(dev)) {
        IDirectInputDevice8_Release(dev);
        return 2;
    }
    if (FAILED(IDirectInputDevice8_SetDataFormat(dev, &g_df))) {
        IDirectInputDevice8_Release(dev);
        return 0;
    }
    /* Ohne SetCooperativeLevel: nicht-exklusiv + Hintergrund (Standard),
     * kein Fenster noetig, kein Fokus-Bezug. */
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
        ncaps = (int)caps.dwAxes;
        d->info.ncaps_btn = (int)caps.dwButtons; /* W312: ungekuerzt */
        d->info.ncaps_pov = (int)caps.dwPOVs;
        d->info.devtype = caps.dwDevType;
        d->info.capflags = caps.dwFlags;
    }
    d->info.vid = LOWORD(di->guidProduct.Data1); /* W312 */
    d->info.pid = HIWORD(di->guidProduct.Data1);
    d->info.ncaps_axes = ncaps;
    d->info.axmask = ijoy_axmask_of(dev);
    ijoy_enum_objects(d); /* W312: Objekt-Metadaten (Geraeteinfo, Plan) */
    ijoy_try_remap(d, ncaps);
    ijoy_ranges(d);
    ijoy_fill_reads(d); /* W312 */
    {
        HRESULT ha = IDirectInputDevice8_Acquire(dev);
        d->info.open_hr = FAILED(ha) ? (long)ha : 0; /* W312: Fehlercode fuer Geraeteinfo */
    }
    return 1;
}

static void ijoy_release(ijoy_dev_t *d)
{
    if (d->dev && g_test) { /* W305: Fake */
        ijoy_fake_h_t *h = (ijoy_fake_h_t *)(void *)d->dev;
        if (h->magic != IJOY_FAKE_MAGIC) {
            g_fk.st.bad_release++;
        } else {
            h->magic = 0;
            free(h);
        }
        g_fk.st.releases++;
        d->dev = NULL;
    } else if (d->dev) {
        IDirectInputDevice8_Unacquire(d->dev);
        IDirectInputDevice8_Release(d->dev);
        d->dev = NULL;
    }
    free(d->odf);
    d->odf = NULL;
    free(d->obj); /* W312 */
    d->obj = NULL;
    d->nobj = 0;
}

int ijoy_scan(void)
{
    ijoy_enum_t *e;
    ijoy_dev_t nd[IJOY_MAX_DEV];
    int nn = 0, i, j;
    if (ijoy_init() != 0) return 0;
    e = (ijoy_enum_t *)calloc(1, sizeof *e);
    if (!e) return g_ndev;
    g_nskip = 0;
    if (g_test) { /* W305: Fake-Geraete (nur eingesteckte) */
        int k;
        g_fk.st.scans++;
        for (k = 0; k < g_fk.n && e->n < IJOY_MAX_DEV; k++) {
            DIDEVICEINSTANCEW *di = &e->inst[e->n];
            if (!g_fk.present[k]) continue;
            memset(di, 0, sizeof *di);
            di->dwSize = sizeof *di;
            di->guidInstance.Data1 = 0xF00D0305u;
            di->guidInstance.Data4[7] = (BYTE)k;
            di->guidProduct.Data1 = 0x5678046Du + (DWORD)k;
            _snwprintf_s(di->tszInstanceName, MAX_PATH, _TRUNCATE, L"Test Stick %d", k);
            _snwprintf_s(di->tszProductName, MAX_PATH, _TRUNCATE, L"Test Stick %d", k);
            e->n++;
        }
    } else {
        IDirectInput8_EnumDevices(g_di, DI8DEVCLASS_GAMECTRL, ijoy_enum_cb, e,
                                  DIEDFL_ATTACHEDONLY);
    }
    for (i = 0; i < e->n && nn < IJOY_MAX_DEV; i++) {
        int kept = 0;
        for (j = 0; j < g_ndev; j++) {
            if (g_dev[j].dev && IsEqualGUID(&g_dev[j].inst, &e->inst[i].guidInstance)) {
                if (g_dev[j].dead || g_dev[j].fails > 0) {
                    /* W305: gleiche guidInstance, aber Objekt tot (wieder
                     * eingesteckt) -> freigeben und neu oeffnen statt behalten. */
                    ijoy_release(&g_dev[j]);
                    break;
                }
                nd[nn++] = g_dev[j]; /* offen lassen (kein Aussetzer) */
                g_dev[j].dev = NULL;
                g_dev[j].odf = NULL; /* gehoert jetzt nd[] */
                g_dev[j].obj = NULL; /* W312: ebenso */
                g_dev[j].nobj = 0;
                kept = 1;
                break;
            }
        }
        if (!kept) {
            int rc = ijoy_open_dev(&e->inst[i], &nd[nn]);
            if (rc == 1) {
                nn++;
            } else if (rc == 2 && g_nskip < 8) { /* W282: fuer die Geraeteliste */
                ijoy_w2u8(e->inst[i].tszProductName[0] ? e->inst[i].tszProductName
                                                       : e->inst[i].tszInstanceName,
                          g_skip_nm[g_nskip], sizeof g_skip_nm[g_nskip]);
                g_nskip++;
            }
        }
    }
    for (j = 0; j < g_ndev; j++) {
        if (g_dev[j].dev) ijoy_release(&g_dev[j]);
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

int ijoy_objects(int i, const ijoy_obj_t **out)
{
    if (out) *out = NULL;
    if (i < 0 || i >= g_ndev || !g_dev[i].obj) return 0;
    if (out) *out = g_dev[i].obj;
    return g_dev[i].nobj;
}

int ijoy_read(int i, imap_joy_state_t *st)
{
    return ijoy_read2(i, st, NULL);
}

int ijoy_read2(int i, imap_joy_state_t *st, imap_joy_state_t *raw)
{
    ijoy_raw_t r;
    HRESULT hr;
    int k;
    if (raw) {
        memset(raw, 0, sizeof *raw);
        for (k = 0; k < IJOY_POVS; k++) raw->pov[k] = -1;
    }
    memset(st, 0, sizeof *st);
    for (k = 0; k < IJOY_POVS; k++) st->pov[k] = -1;
    if (i < 0 || i >= g_ndev || !g_dev[i].dev) return 0;
    if (g_test) { /* W305: Fake (abgezogen bzw. alte Generation = tot) */
        ijoy_fake_h_t *h = (ijoy_fake_h_t *)(void *)g_dev[i].dev;
        memset(&r, 0, sizeof r);
        if (h->magic != IJOY_FAKE_MAGIC) {
            g_fk.st.bad_read++;
            hr = DIERR_INPUTLOST;
        } else if (!g_fk.present[h->k] || h->gen != g_fk.gen[h->k]) {
            hr = DIERR_UNPLUGGED; /* GetDeviceState INPUTLOST, Acquire UNPLUGGED */
        } else if (g_dev[i].obj && g_fk.nobj[h->k] > 0) { /* W312: ueber den Plan */
            int o;
            for (k = 0; k < IJOY_POVS; k++) r.pov[k] = 0xFFFFFFFFu;
            for (o = 0; o < g_dev[i].nobj; o++) {
                const ijoy_obj_t *b = &g_dev[i].obj[o];
                long v = g_fk.oval[h->k][o];
                if (b->read < 0) continue;
                if (b->kind <= IJOY_K_AXIS) r.ax[b->read] = v;
                else if (b->kind == IJOY_K_POV) r.pov[b->read] = v < 0 ? 0xFFFFFFFFu : (DWORD)v;
                else if (b->kind == IJOY_K_BTN) r.btn[b->read] = v ? 0x80 : 0;
            }
            hr = DI_OK;
        } else {
            r.ax[0] = g_fk.ax[h->k];
            r.ax[1] = -g_fk.ax[h->k];
            r.btn[0] = 0x80;
            hr = DI_OK;
        }
        if (FAILED(hr)) g_fk.st.read_fails++;
    } else {
        IDirectInputDevice8_Poll(g_dev[i].dev);
        hr = IDirectInputDevice8_GetDeviceState(g_dev[i].dev, sizeof r, &r);
        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
            HRESULT ha = IDirectInputDevice8_Acquire(g_dev[i].dev);
            if (ha == DIERR_UNPLUGGED) {
                hr = DIERR_UNPLUGGED; /* W305: abgezogen, nicht nur Fokus */
            } else {
                IDirectInputDevice8_Poll(g_dev[i].dev);
                hr = IDirectInputDevice8_GetDeviceState(g_dev[i].dev, sizeof r, &r);
            }
        }
    }
    if (FAILED(hr)) {
        g_dev[i].fails++;
        if (hr == DIERR_UNPLUGGED || g_dev[i].fails >= IJOY_FAIL_DROP)
            g_dev[i].dead = 1; /* W305: Slot-Takt gibt frei + zaehlt neu auf */
        return 0;
    }
    g_dev[i].fails = 0;
    for (k = 0; k < IJOY_AXES; k++) {
        if (!(g_dev[i].info.axmask & (1u << k))) {
            st->axis[k] = 0;
            continue;
        }
        /* W282: Geraetebereich -> -32768..32767 (Normalfall bitgleich) */
        st->axis[k] = ijoy_norm_axis(r.ax[k], g_dev[i].lo[k], g_dev[i].hi[k]);
        if (raw) raw->axis[k] = st->axis[k];
        if (g_ncal > 0) { /* W312: Kalibrierung (ohne Eintrag bitgleich) */
            ijoy_cal_t c;
            if (ijoy_cal_get(g_dev[i].info.vid, g_dev[i].info.pid, k, &c))
                st->axis[k] = ijoy_cal_apply(&c, st->axis[k]);
        }
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
    if (raw) {
        memcpy(raw->btn, st->btn, sizeof raw->btn);
        memcpy(raw->pov, st->pov, sizeof raw->pov);
    }
    return 1;
}

void ijoy_shutdown(void)
{
    int j;
    for (j = 0; j < g_ndev; j++) {
        ijoy_release(&g_dev[j]);
    }
    g_ndev = 0;
}

/* ---- Client: Slot-Leser im Hintergrund-Thread ---- */
static SRWLOCK g_slk = SRWLOCK_INIT;
static char g_spec[IMAP_JOY_MAX][96];
static unsigned g_used;
static imap_joy_state_t g_slot_st[IMAP_JOY_MAX];
static volatile LONG g_ds_on;            /* W439: Ausreisser-Filter an */
static despike_t g_ds[IMAP_JOY_MAX];     /* W439: nur Lese-Thread */
static volatile LONG g_ds_spikes;
void ijoy_set_despike(int on) { InterlockedExchange(&g_ds_on, on ? 1 : 0); }
unsigned ijoy_despike_spikes(void) { return (unsigned)g_ds_spikes; }
static unsigned g_slot_mask;
static char g_slot_nm[IMAP_JOY_MAX][96];
static volatile LONG g_thr_started;

/* W305: ein Takt des Slot-Lesers (Thread alle 4 ms, Selbsttest mit
 * simulierter Zeit). Fehlendes Geraet -> Slot-Bit 0 = neutral (Achsen 0,
 * Tasten los, POV mittig), nie der letzte Stand. Totes Objekt -> sofort
 * freigeben, danach 10 s lang alle IJOY_RESCAN_DROP_MS neu aufzaehlen,
 * sonst ohne Geraet alle IJOY_RESCAN_MS (kein Busy-Spin). */
typedef struct {
    int map[IMAP_JOY_MAX];
    DWORD last_scan, fast_until;
    int need_scan, fast;
} ijoy_sup_t;

static void ijoy_sup_init(ijoy_sup_t *sp)
{
    int s;
    memset(sp, 0, sizeof *sp);
    for (s = 0; s < IMAP_JOY_MAX; s++) sp->map[s] = -1;
    sp->need_scan = 1;
}

static void ijoy_tick(ijoy_sup_t *sp, DWORD now)
{
    imap_joy_state_t st[IMAP_JOY_MAX];
    unsigned mask = 0;
    int s;
    if (sp->fast && (LONG)(now - sp->fast_until) >= 0) sp->fast = 0;
    if (sp->need_scan &&
        (sp->last_scan == 0 ||
         now - sp->last_scan >= (sp->fast ? IJOY_RESCAN_DROP_MS : IJOY_RESCAN_MS))) {
        ijoy_info_t list[IJOY_MAX_DEV];
        int n, i;
        sp->last_scan = now ? now : 1;
        n = ijoy_scan();
        for (i = 0; i < n; i++) list[i] = g_dev[i].info;
        sp->need_scan = 0;
        AcquireSRWLockExclusive(&g_slk);
        for (s = 0; s < IMAP_JOY_MAX; s++) {
            sp->map[s] = (g_used & (1u << s)) ? ijoy_match_list(list, n, g_spec[s], s) : -1;
            if ((g_used & (1u << s)) && sp->map[s] < 0) sp->need_scan = 1;
            if (sp->map[s] >= 0)
                strncpy_s(g_slot_nm[s], sizeof g_slot_nm[s], list[sp->map[s]].product[0]
                          ? list[sp->map[s]].product : list[sp->map[s]].name, _TRUNCATE);
            else
                g_slot_nm[s][0] = '\0';
        }
        ReleaseSRWLockExclusive(&g_slk);
    }
    memset(st, 0, sizeof st);
    for (s = 0; s < IMAP_JOY_MAX; s++) {
        int k, d = sp->map[s];
        for (k = 0; k < IJOY_POVS; k++) st[s].pov[k] = -1;
        if (d < 0) continue;
        if (ijoy_read(d, &st[s])) {
            mask |= 1u << s;
        } else if (g_dev[d].dead && g_dev[d].dev) {
            ijoy_release(&g_dev[d]); /* W305: totes Objekt nie weiterbenutzen */
            sp->need_scan = 1;
            sp->fast = 1;
            sp->fast_until = now + IJOY_FAST_WINDOW_MS;
        } else if (!g_dev[d].dev) {
            sp->need_scan = 1; /* freigegeben, Aufzaehlung steht aus */
        }
    }
    if (g_ds_on) { /* W439: Ausreisser-Filter je Slot (Achsen-Strom alle ~4 ms) */
        LONG sp = 0;
        for (s = 0; s < IMAP_JOY_MAX; s++) {
            int k, v[IJOY_AXES], o[IJOY_AXES];
            if (!(mask & (1u << s))) { g_ds[s].init = 0; continue; }
            if (!g_ds[s].n) despike_init(&g_ds[s], IJOY_AXES, DESPIKE_THR_S16, DESPIKE_HOLD_MS);
            for (k = 0; k < IJOY_AXES; k++) v[k] = st[s].axis[k];
            despike_feed(&g_ds[s], v, (uint64_t)now, o);
            for (k = 0; k < IJOY_AXES; k++) st[s].axis[k] = (int16_t)o[k];
            sp += (LONG)g_ds[s].spikes;
        }
        InterlockedExchange(&g_ds_spikes, sp);
    }
    AcquireSRWLockExclusive(&g_slk);
    memcpy(g_slot_st, st, sizeof st);
    g_slot_mask = mask;
    ReleaseSRWLockExclusive(&g_slk);
}

static DWORD WINAPI ijoy_thread(LPVOID unused)
{
    ijoy_sup_t sp;
    (void)unused;
    ijoy_sup_init(&sp);
    for (;;) {
        ijoy_tick(&sp, GetTickCount());
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

/* ---- W305: Test-Schnittstelle (nur Selbsttest, s. input_joy.h) ---- */
static ijoy_sup_t g_tsup;
static DWORD g_tnow;

void ijoy_test_begin(int nfake, const char specs[IMAP_JOY_MAX][96], unsigned used)
{
    int s;
    ijoy_shutdown();
    memset(&g_fk, 0, sizeof g_fk);
    g_fk.n = nfake < 0 ? 0 : nfake > IJOY_FAKE_MAX ? IJOY_FAKE_MAX : nfake;
    for (s = 0; s < g_fk.n; s++) {
        g_fk.present[s] = 1;
        g_fk.ax[s] = 1000 * (s + 1);
    }
    g_test = 1;
    for (s = 0; s < IMAP_JOY_MAX; s++)
        strncpy_s(g_spec[s], sizeof g_spec[s], specs ? specs[s] : "", _TRUNCATE);
    g_used = used;
    ijoy_sup_init(&g_tsup);
    g_tnow = 100000;
}

void ijoy_test_plug(int k, int present)
{
    if (k < 0 || k >= g_fk.n) return;
    if (present && !g_fk.present[k]) g_fk.gen[k]++; /* neue Generation */
    g_fk.present[k] = present ? 1 : 0;
}

void ijoy_test_axis(int k, long v)
{
    if (k >= 0 && k < g_fk.n) g_fk.ax[k] = v;
}

void ijoy_test_run(unsigned ms)
{
    DWORD end = g_tnow + ms;
    while ((LONG)(end - g_tnow) > 0) {
        ijoy_tick(&g_tsup, g_tnow);
        g_tnow += 4;
    }
}

void ijoy_test_stats(ijoy_test_stats_t *out)
{
    if (out) *out = g_fk.st;
}

void ijoy_test_objects(int k, const ijoy_obj_t *o, int n)
{
    if (k < 0 || k >= IJOY_FAKE_MAX) return;
    if (n < 0) n = 0;
    if (n > IJOY_OBJ_MAX) n = IJOY_OBJ_MAX;
    if (o && n) memcpy(g_fk.obj[k], o, sizeof(ijoy_obj_t) * (size_t)n);
    memset(g_fk.oval[k], 0, sizeof g_fk.oval[k]);
    g_fk.nobj[k] = n;
}

void ijoy_test_obj_value(int k, int obj, long v)
{
    if (k >= 0 && k < IJOY_FAKE_MAX && obj >= 0 && obj < IJOY_OBJ_MAX) g_fk.oval[k][obj] = v;
}

void ijoy_test_end(void)
{
    ijoy_shutdown();
    g_test = 0;
    g_used = 0;
    memset(g_slot_st, 0, sizeof g_slot_st);
    g_slot_mask = 0;
}
