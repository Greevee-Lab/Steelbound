/* render_winmode.c - W81 WINMODE: Fenster / randloses Vollbild / exklusives
 * Vollbild, Alt+Enter, Groessenwechsel per ResizeBuffers.
 *
 * Schalter ([display], hcfg.h): window_mode (0 Fenster = bisher, 1 randlos,
 * 2 exklusiv), fs_width/fs_height/fs_hz (0 = Desktop), fs_monitor (0 =
 * Monitor des Fensters, N = N-ter DXGI-Ausgang), alt_enter (1 = eigene
 * Umschaltung, 0 = alt: DXGI-Standard), alt_enter_mode (Vollbild-Art fuer
 * Alt+Enter bei window_mode=0), win_resize (Backbuffer folgt der Fenster-
 * groesse statt Strecken), dpi_aware (-1 auto), fs_interp_match (Interp-Takt
 * = Bildwiederholrate im Exklusiv-Modus), winmode_test (+ winmode_alt_*).
 *
 * Threads: Fenster-Nachrichten (UI-Thread) setzen nur Anforderungen
 * (wm_req, wm_size_pend, wm_active). Alle Fenster-/DXGI-/Resize-Schritte
 * laufen auf dem Render-Thread am Ende von sb_render_present
 * (sb_winmode_tick) - dort ist das Bild fertig, kein Draw offen, der
 * Context-Schluessel (ctx_lock) wird gehalten. Der UI-Thread wartet nie auf
 * den Render-Thread (DXGI/SetWindowPos schicken ihm synchron Nachrichten).
 *
 * Sicherheit (Nutzerregel 3.11.): window_hidden=1 oder winmode_test=1 =
 * Trockenlauf - Fenster bleibt ausserhalb des virtuellen Bildschirms
 * (geprueft vor jedem SetWindowPos), SetFullscreenState/Minimieren/
 * Wiederherstellen werden nur geloggt, nie ausgefuehrt. */
#include "render_int.h"
#include <dxgi.h>
#include <math.h>
#include <stdlib.h>

#define WM_LOG_MODES_MAX 96

static HWND wm_hwnd;
static IDXGISwapChain *wm_swap;
static volatile LONG wm_req;        /* gewuenschter Modus (UI-Thread / Test) */
static volatile LONG wm_size_pend;  /* 1 = Client-Groesse neu pruefen */
static volatile LONG wm_in_sizemove;
static volatile LONG wm_active = 1; /* letzter WM_ACTIVATEAPP-Stand */
static int wm_cur;                  /* aktueller Modus (Render-Thread) */
static int wm_kind;                 /* Vollbild-Art fuer Alt+Enter (1/2) */
static int wm_dry;                  /* 1 = Trockenlauf (unsichtbar) */
static int wm_dpi;                  /* 1 = Per-Monitor-DPI (PMv2) */
static int wm_ready;
static int wm_fs_on;                /* exklusiv aktiv (bzw. simuliert) */
static int wm_fs_susp;              /* exklusiv wegen Fokusverlust ausgesetzt */
static int wm_fs_retry;             /* Fehlversuche beim Wiederherstellen */
static unsigned wm_fs_next;         /* naechster Versuch (Present-Nr) */
static IDXGIOutput *wm_out;         /* Ausgang des Exklusiv-Modus */
static DXGI_MODE_DESC wm_mode;      /* gewaehlter Modus */
static int wm_saved;                /* Fensterlage gesichert */
static RECT wm_save_rc;
static LONG_PTR wm_save_style;
static unsigned wm_save_cw, wm_save_ch; /* Backbuffer = Client vor dem Vollbild */
static int wm_interp_saved = -1;    /* interp_fps vor fs_interp_match */
static unsigned wm_nswitch, wm_test_steps;
/* Zaehler (UI-Thread erhoeht, Render-Thread loggt: hlog verwirft Zeilen
 * bei Schloss-Konkurrenz, daher keine Logs auf dem UI-Thread + kumulative
 * Statistikzeile als Beleg). */
static volatile LONG wm_n_alt, wm_n_act0, wm_n_act1;
static LONG wm_seen_alt, wm_seen_act0, wm_seen_act1;
static unsigned wm_n_susp, wm_n_resume, wm_n_resize, wm_n_fail;
static int wm_modes_logged;
static int wm_stat_due;
unsigned g_swap_flags;              /* Swapchain-Flags (Erzeugung + ResizeBuffers) */

static const char *wm_name(int m)
{
    return m == 1 ? "randlos" : m == 2 ? "exklusiv" : "Fenster";
}

/* ---------- DPI (Per-Monitor v2, dynamisch: Win10 1607+) ---------- */
typedef HANDLE(WINAPI *wm_setctx_t)(HANDLE);
static wm_setctx_t wm_setctx(void)
{
    static wm_setctx_t f;
    static int done;
    if (!done) {
        HMODULE u = GetModuleHandleA("user32.dll");
        done = 1;
        if (u) { f = (wm_setctx_t)GetProcAddress(u, "SetThreadDpiAwarenessContext"); }
    }
    return f;
}

static int wm_dpi_wanted(void)
{
    if (g_cfg.dpi_aware >= 0) { return g_cfg.dpi_aware ? 1 : 0; }
    return g_cfg.window_mode > 0; /* auto: nur Vollbild-Start (Fenster = bisher) */
}

/* UI-Thread vor CreateWindow: Fenster bekommt die DPI-Art des Threads. */
void sb_winmode_ui_dpi(void)
{
    wm_setctx_t f;
    wm_dpi = 0;
    if (!wm_dpi_wanted()) { return; }
    f = wm_setctx();
    if (f && f((HANDLE)(INT_PTR)-4)) { /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
        wm_dpi = 1;
    }
}

static HANDLE wm_dpi_push(void)
{
    wm_setctx_t f = wm_setctx();
    return (wm_dpi && f) ? f((HANDLE)(INT_PTR)-4) : NULL;
}

static void wm_dpi_pop(HANDLE old)
{
    wm_setctx_t f = wm_setctx();
    if (old && f) { f(old); }
}

/* ---------- Ausgaenge / Monitore ---------- */
typedef struct {
    IDXGIOutput *out;
    DXGI_OUTPUT_DESC d;
    int on_dev; /* gehoert zum Adapter des Devices (Exklusiv moeglich) */
} wm_out_t;

#define WM_OUTS_MAX 8

static int wm_outputs(wm_out_t *o, int max)
{
    IDXGIDevice *dx = NULL;
    IDXGIAdapter *ad = NULL, *a2 = NULL;
    IDXGIFactory *fac = NULL;
    DXGI_ADAPTER_DESC add, ad2;
    int n = 0;
    UINT i, k;
    if (FAILED(ID3D11Device_QueryInterface(g_dev, &IID_IDXGIDevice, (void **)&dx)) || !dx) {
        return 0;
    }
    if (FAILED(IDXGIDevice_GetAdapter(dx, &ad)) || !ad) {
        IDXGIDevice_Release(dx);
        return 0;
    }
    IDXGIDevice_Release(dx);
    for (i = 0; n < max; i++) {
        IDXGIOutput *out = NULL;
        if (IDXGIAdapter_EnumOutputs(ad, i, &out) != S_OK || !out) { break; }
        o[n].out = out;
        IDXGIOutput_GetDesc(out, &o[n].d);
        o[n].on_dev = 1;
        n++;
    }
    /* weitere Adapter (nur randlos nutzbar) */
    IDXGIAdapter_GetDesc(ad, &add);
    if (SUCCEEDED(IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory, (void **)&fac)) && fac) {
        for (k = 0; n < max && IDXGIFactory_EnumAdapters(fac, k, &a2) == S_OK; k++) {
            IDXGIAdapter_GetDesc(a2, &ad2);
            if (memcmp(&ad2.AdapterLuid, &add.AdapterLuid, sizeof add.AdapterLuid) != 0) {
                for (i = 0; n < max; i++) {
                    IDXGIOutput *out = NULL;
                    if (IDXGIAdapter_EnumOutputs(a2, i, &out) != S_OK || !out) { break; }
                    o[n].out = out;
                    IDXGIOutput_GetDesc(out, &o[n].d);
                    o[n].on_dev = 0;
                    n++;
                }
            }
            IDXGIAdapter_Release(a2);
        }
        IDXGIFactory_Release(fac);
    }
    IDXGIAdapter_Release(ad);
    return n;
}

static void wm_outputs_free(wm_out_t *o, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (o[i].out) { IDXGIOutput_Release(o[i].out); }
    }
}

/* Index des Zielausgangs: fs_monitor (1-basiert) oder Monitor des Fensters. */
static int wm_pick_output(const wm_out_t *o, int n)
{
    HMONITOR hm;
    int i;
    if (n <= 0) { return -1; }
    if (g_cfg.fs_monitor > 0) {
        if (g_cfg.fs_monitor <= n) { return g_cfg.fs_monitor - 1; }
        HLOG_W("WINMODE: fs_monitor=%d gibt es nicht (%d Ausgaenge) -> Monitor des Fensters",
               g_cfg.fs_monitor, n);
    }
    hm = MonitorFromWindow(wm_hwnd, MONITOR_DEFAULTTONEAREST);
    for (i = 0; i < n; i++) {
        if (o[i].d.Monitor == hm) { return i; }
    }
    return 0;
}

/* Desktop-Modus eines Ausgangs (physische Pixel, Hz). */
static void wm_desktop_mode(const wm_out_t *o, unsigned *w, unsigned *h, unsigned *hz)
{
    DEVMODEW dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    *w = (unsigned)(o->d.DesktopCoordinates.right - o->d.DesktopCoordinates.left);
    *h = (unsigned)(o->d.DesktopCoordinates.bottom - o->d.DesktopCoordinates.top);
    *hz = 0;
    if (EnumDisplaySettingsW(o->d.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
        if (dm.dmPelsWidth && dm.dmPelsHeight) {
            *w = dm.dmPelsWidth;
            *h = dm.dmPelsHeight;
        }
        if (dm.dmDisplayFrequency > 1) { *hz = dm.dmDisplayFrequency; }
    }
}

static void wm_log_outputs(const wm_out_t *o, int n, int pick)
{
    static int done;
    int i;
    if (done) { return; }
    done = 1;
    for (i = 0; i < n; i++) {
        unsigned w, h, hz;
        wm_desktop_mode(&o[i], &w, &h, &hz);
        HLOG_I("WINMODE Ausgang #%d: %ls %ux%u@%u Hz%s%s", i + 1, o[i].d.DeviceName,
               w, h, hz, o[i].on_dev ? "" : " (anderer Adapter: nur randlos)",
               i == pick ? " <- Ziel" : "");
    }
}

/* ---------- Fensterlage ---------- */
static void wm_virt(RECT *v)
{
    v->left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    v->top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    v->right = v->left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    v->bottom = v->top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

/* Trockenlauf: Rechteck links neben allen Monitoren (wie window_hidden). */
static void wm_dry_rect(unsigned w, unsigned h, RECT *r)
{
    RECT v;
    wm_virt(&v);
    r->left = v.left - (LONG)w - 200;
    r->top = v.top;
    r->right = r->left + (LONG)w;
    r->bottom = r->top + (LONG)h;
}

static int wm_offscreen(const RECT *r)
{
    RECT v, x;
    wm_virt(&v);
    return !IntersectRect(&x, r, &v);
}

/* SetWindowPos mit Sicherheitspruefung (Trockenlauf: nie auf den Desktop). */
static int wm_place(const RECT *r, LONG_PTR style)
{
    if (wm_dry && !wm_offscreen(r)) {
        HLOG_E("WINMODE: Trockenlauf-Rechteck %ld,%ld-%ld,%ld laege auf dem Bildschirm -> abgebrochen",
               r->left, r->top, r->right, r->bottom);
        return 0;
    }
    SetWindowLongPtrA(wm_hwnd, GWL_STYLE, style);
    SetWindowPos(wm_hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    return 1;
}

static void wm_save_window(void)
{
    GetWindowRect(wm_hwnd, &wm_save_rc);
    wm_save_style = GetWindowLongPtrA(wm_hwnd, GWL_STYLE);
    wm_save_cw = g_bb_w;
    wm_save_ch = g_bb_h;
    wm_saved = 1;
}

static LONG_PTR wm_popup_style(void)
{
    LONG_PTR st = GetWindowLongPtrA(wm_hwnd, GWL_STYLE);
    st &= ~(LONG_PTR)(WS_OVERLAPPEDWINDOW);
    st |= WS_POPUP;
    return st;
}

/* Client-Groesse uebernehmen (Backbuffer folgt, nie strecken). */
static void wm_sync_client(const char *why)
{
    RECT rc;
    if (IsIconic(wm_hwnd) || !GetClientRect(wm_hwnd, &rc)) { return; }
    if (rc.right > 0 && rc.bottom > 0) {
        if (sb_render_resize((unsigned)rc.right, (unsigned)rc.bottom, why)) { wm_n_resize++; }
    }
}

/* ---------- Randlos ---------- */
static int wm_enter_borderless(void)
{
    wm_out_t o[WM_OUTS_MAX];
    int n = wm_outputs(o, WM_OUTS_MAX), pick = wm_pick_output(o, n);
    RECT r;
    unsigned w = 0, h = 0, hz = 0;
    wm_log_outputs(o, n, pick);
    if (pick >= 0) {
        MONITORINFO mi;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        wm_desktop_mode(&o[pick], &w, &h, &hz);
        if (GetMonitorInfoA(o[pick].d.Monitor, &mi)) {
            r = mi.rcMonitor; /* in der DPI-Art des Threads (wie SetWindowPos) */
            w = (unsigned)(r.right - r.left);
            h = (unsigned)(r.bottom - r.top);
        } else {
            r = o[pick].d.DesktopCoordinates;
        }
    } else {
        w = g_bb_w;
        h = g_bb_h;
        r.left = r.top = 0;
        r.right = (LONG)w;
        r.bottom = (LONG)h;
    }
    wm_outputs_free(o, n);
    if (wm_dry) {
        if (g_cfg.fs_width >= 64 && g_cfg.fs_height >= 64) { /* simulierter Monitor */
            w = g_cfg.fs_width;
            h = g_cfg.fs_height;
        }
        wm_dry_rect(w, h, &r);
    }
    if (!wm_place(&r, wm_popup_style())) { return 0; }
    HLOG_I("WINMODE randlos: Fenster %ux%u @%ld,%ld (WS_POPUP%s)", w, h, r.left, r.top,
           wm_dry ? ", Trockenlauf ausserhalb des Bildschirms" : ", Monitor des Fensters");
    wm_sync_client("randlos");
    return 1;
}

/* ---------- Exklusiv ---------- */
static unsigned wm_mode_hz(const DXGI_MODE_DESC *m)
{
    if (!m->RefreshRate.Denominator) { return 0; }
    return (m->RefreshRate.Numerator + m->RefreshRate.Denominator / 2) / m->RefreshRate.Denominator;
}

static void wm_log_modes(IDXGIOutput *out)
{
    UINT n = 0, i, nl = 0;
    DXGI_MODE_DESC *m;
    if (wm_modes_logged) { return; }
    wm_modes_logged = 1;
    if (FAILED(IDXGIOutput_GetDisplayModeList(out, DXGI_FORMAT_R8G8B8A8_UNORM, 0, &n, NULL)) || !n) {
        HLOG_W("WINMODE Modusliste: leer (n=%u)", n);
        return;
    }
    m = (DXGI_MODE_DESC *)calloc(n, sizeof *m);
    if (!m) { return; }
    if (SUCCEEDED(IDXGIOutput_GetDisplayModeList(out, DXGI_FORMAT_R8G8B8A8_UNORM, 0, &n, m))) {
        /* je Groesse eine Zeile mit allen Raten (gerundet, ohne Doppel) */
        HLOG_I("WINMODE Modusliste: %u Modi (R8G8B8A8_UNORM)", n);
        for (i = 0; i < n && nl < WM_LOG_MODES_MAX;) {
            char line[512];
            unsigned w = m[i].Width, h = m[i].Height, last[64], nlast = 0, j;
            int len = _snprintf_s(line, sizeof line, _TRUNCATE, "%ux%u:", w, h);
            for (; i < n && m[i].Width == w && m[i].Height == h; i++) {
                unsigned hz = wm_mode_hz(&m[i]), dup = 0;
                for (j = 0; j < nlast; j++) { if (last[j] == hz) { dup = 1; } }
                if (dup || nlast >= 64u) { continue; }
                last[nlast++] = hz;
                if (len > 0 && len < (int)sizeof line - 8) {
                    int k = _snprintf_s(line + len, sizeof line - (size_t)len, _TRUNCATE, " %u", hz);
                    if (k > 0) { len += k; }
                }
            }
            nl++;
            HLOG_I("WINMODE   Modus %s Hz", line);
        }
    }
    free(m);
}

/* Modus waehlen (nie strecken): exakt fs_width x fs_height, sonst die
 * groesste Groesse <= Wunsch mit dem Seitenverhaeltnis des Monitors
 * (Desktop-Aufloesung dw x dh), sonst die Desktop-Aufloesung. Ein Modus mit
 * anderem Seitenverhaeltnis als der Monitor wird nie genommen (die Treiber-
 * Skalierung koennte ihn strecken). Rate: exakt (gerundet) = Wunsch, sonst
 * die hoechste <= Wunsch, sonst die naechste. 1 = *got gefuellt. */
static int wm_pick_mode(IDXGIOutput *out, unsigned ww, unsigned wh, unsigned whz,
                        unsigned dw, unsigned dh, DXGI_MODE_DESC *got, const char **why)
{
    UINT n = 0, i;
    DXGI_MODE_DESC *m, best;
    unsigned tw = 0, th = 0;
    int have = 0;
    double da = dh ? (double)dw / (double)dh : 0.0;
    *why = "";
    if (FAILED(IDXGIOutput_GetDisplayModeList(out, DXGI_FORMAT_R8G8B8A8_UNORM, 0, &n, NULL)) || !n) {
        return 0;
    }
    m = (DXGI_MODE_DESC *)calloc(n, sizeof *m);
    if (!m) { return 0; }
    if (FAILED(IDXGIOutput_GetDisplayModeList(out, DXGI_FORMAT_R8G8B8A8_UNORM, 0, &n, m))) {
        free(m);
        return 0;
    }
    /* 1. Groesse */
    for (i = 0; i < n; i++) {
        if (m[i].Width == ww && m[i].Height == wh) { tw = ww; th = wh; break; }
    }
    if (tw && da > 0.0 && fabs((double)tw / (double)th - da) > da * 0.01) {
        tw = th = 0; /* exakt vorhanden, aber anderes Seitenverhaeltnis als der Monitor */
        *why = "Seitenverhaeltnis != Monitor";
    } else if (tw) {
        *why = "exakt";
    }
    if (!tw) {
        for (i = 0; i < n; i++) {
            double a = (double)m[i].Width / (double)m[i].Height;
            if (da > 0.0 && fabs(a - da) > da * 0.01) { continue; }
            if (m[i].Width > ww || m[i].Height > wh) { continue; }
            if ((unsigned long long)m[i].Width * m[i].Height > (unsigned long long)tw * th) {
                tw = m[i].Width;
                th = m[i].Height;
            }
        }
        if (tw) {
            if (!**why) { *why = "gleiches Seitenverhaeltnis wie der Monitor"; }
        } else {
            tw = dw;
            th = dh;
            *why = "Desktop-Aufloesung";
        }
    }
    /* 2. Rate */
    for (i = 0; i < n; i++) {
        unsigned hz;
        if (m[i].Width != tw || m[i].Height != th) { continue; }
        hz = (m[i].RefreshRate.Denominator)
                 ? (m[i].RefreshRate.Numerator + m[i].RefreshRate.Denominator / 2) /
                       m[i].RefreshRate.Denominator : 0;
        if (!have) { best = m[i]; have = 1; continue; }
        {
            unsigned bhz = (best.RefreshRate.Numerator + best.RefreshRate.Denominator / 2) /
                           (best.RefreshRate.Denominator ? best.RefreshRate.Denominator : 1);
            int b_ok = bhz <= whz, c_ok = hz <= whz;
            if (hz == whz && bhz != whz) { best = m[i]; }
            else if (bhz == whz) { /* behalten */ }
            else if (c_ok && (!b_ok || hz > bhz)) { best = m[i]; }
            else if (!c_ok && !b_ok && hz < bhz) { best = m[i]; }
        }
    }
    free(m);
    if (!have) { return 0; }
    *got = best;
    return 1;
}

/* fs_interp_match: Interp-Takt = Bildwiederholrate (nur bei interp_fps>0). */
static void wm_interp_match(int on)
{
    if (on) {
        unsigned hz = wm_mode_hz(&wm_mode);
        if (g_cfg.fs_interp_match && g_cfg.interp_fps > 0 && hz >= 60 && hz <= 240 &&
            (int)hz != g_cfg.interp_fps) {
            wm_interp_saved = g_cfg.interp_fps;
            g_cfg.interp_fps = (int)hz;
            HLOG_I("WINMODE exklusiv: interp_fps %d -> %u (= Bildwiederholrate, fs_interp_match=1)",
                   wm_interp_saved, hz);
        }
    } else if (wm_interp_saved >= 0) {
        HLOG_I("WINMODE: interp_fps %d -> %d (zurueck)", g_cfg.interp_fps, wm_interp_saved);
        g_cfg.interp_fps = wm_interp_saved;
        wm_interp_saved = -1;
    }
}

/* Vollbild-Zustand setzen (Trockenlauf: nur loggen). */
static HRESULT wm_set_fs(BOOL on, const char *why)
{
    HRESULT hr;
    if (wm_dry) {
        HLOG_I("WINMODE exklusiv: Trockenlauf - SetFullscreenState(%s) uebersprungen (%s)",
               on ? "TRUE" : "FALSE", why);
        return S_OK;
    }
    sb_ctx_enter();
    hr = IDXGISwapChain_SetFullscreenState(wm_swap, on, on ? wm_out : NULL);
    sb_ctx_leave();
    HLOG_I("WINMODE exklusiv: SetFullscreenState(%s) hr=0x%08lX (%s)", on ? "TRUE" : "FALSE",
           (unsigned long)hr, why);
    return hr;
}

/* Exklusiv betreten: Modus suchen, (Trockenlauf: simulieren), Backbuffer
 * auf Modusgroesse. 0 = gescheitert (Aufrufer faellt auf randlos zurueck). */
static int wm_enter_exclusive(void)
{
    wm_out_t o[WM_OUTS_MAX];
    int n = wm_outputs(o, WM_OUTS_MAX), pick = wm_pick_output(o, n);
    DXGI_MODE_DESC want;
    unsigned dw, dh, dhz;
    HRESULT hr;
    wm_log_outputs(o, n, pick);
    if (pick < 0 || !o[pick].on_dev) {
        HLOG_W("WINMODE exklusiv: kein Ausgang am Render-Adapter (pick=%d) -> randlos", pick);
        wm_outputs_free(o, n);
        return 0;
    }
    if (wm_out) { IDXGIOutput_Release(wm_out); }
    wm_out = o[pick].out;
    IDXGIOutput_AddRef(wm_out);
    wm_desktop_mode(&o[pick], &dw, &dh, &dhz);
    wm_outputs_free(o, n);
    memset(&want, 0, sizeof want);
    want.Width = g_cfg.fs_width ? g_cfg.fs_width : dw;
    want.Height = g_cfg.fs_height ? g_cfg.fs_height : dh;
    want.RefreshRate.Numerator = g_cfg.fs_hz ? (UINT)g_cfg.fs_hz : dhz;
    want.RefreshRate.Denominator = want.RefreshRate.Numerator ? 1 : 0;
    want.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    wm_log_modes(wm_out);
    {
        DXGI_MODE_DESC pick;
        const char *why = "";
        if (wm_pick_mode(wm_out, want.Width, want.Height, want.RefreshRate.Numerator, dw, dh,
                         &pick, &why)) {
            HLOG_I("WINMODE exklusiv: Auswahl %ux%u @ %u/%u (%s)", pick.Width, pick.Height,
                   pick.RefreshRate.Numerator, pick.RefreshRate.Denominator, why);
            want = pick; /* voll bestimmt: FindClosestMatchingMode liefert genau ihn */
        } else {
            HLOG_W("WINMODE exklusiv: keine Modusliste -> FindClosestMatchingMode mit Wunsch");
        }
    }
    hr = IDXGIOutput_FindClosestMatchingMode(wm_out, &want, &wm_mode, (IUnknown *)g_dev);
    HLOG_I("WINMODE exklusiv: Wunsch %ux%u@%u Hz (Desktop %ux%u@%u) -> FindClosestMatchingMode "
           "hr=0x%08lX %ux%u @ %u/%u (%u Hz)",
           g_cfg.fs_width ? g_cfg.fs_width : dw, g_cfg.fs_height ? g_cfg.fs_height : dh,
           g_cfg.fs_hz ? (unsigned)g_cfg.fs_hz : dhz, dw, dh, dhz, (unsigned long)hr,
           wm_mode.Width, wm_mode.Height, wm_mode.RefreshRate.Numerator,
           wm_mode.RefreshRate.Denominator, wm_mode_hz(&wm_mode));
    if (FAILED(hr) || wm_mode.Width < 64 || wm_mode.Height < 64) {
        return 0;
    }
    if (dh && fabs((double)wm_mode.Width / wm_mode.Height - (double)dw / dh) > (double)dw / dh * 0.01) {
        HLOG_W("WINMODE exklusiv: Modus %ux%u hat ein anderes Seitenverhaeltnis als der Monitor "
               "%ux%u -> Desktop-Aufloesung (nie strecken)", wm_mode.Width, wm_mode.Height, dw, dh);
        want.Width = dw;
        want.Height = dh;
        if (FAILED(IDXGIOutput_FindClosestMatchingMode(wm_out, &want, &wm_mode, (IUnknown *)g_dev))) {
            return 0;
        }
    }
    if (wm_dry) {
        /* Alles bis vor SetFullscreenState; Fenster/Backbuffer nehmen die
         * Modusgroesse an (unsichtbar), damit Resize/Viewport/Interp laufen. */
        RECT r;
        wm_dry_rect(wm_mode.Width, wm_mode.Height, &r);
        if (!wm_place(&r, wm_popup_style())) { return 0; }
        HLOG_I("WINMODE exklusiv: Trockenlauf - ResizeTarget(%ux%u) uebersprungen, Fenster "
               "ausserhalb des Bildschirms", wm_mode.Width, wm_mode.Height);
        wm_set_fs(TRUE, "Eintritt");
    } else {
        /* Fenster vorab randlos auf den Zielmonitor (sauberer Uebergang). */
        sb_ctx_enter();
        hr = IDXGISwapChain_ResizeTarget(wm_swap, &wm_mode);
        sb_ctx_leave();
        HLOG_I("WINMODE exklusiv: ResizeTarget hr=0x%08lX", (unsigned long)hr);
        hr = wm_set_fs(TRUE, "Eintritt");
        if (FAILED(hr)) {
            return 0;
        }
        {
            DXGI_MODE_DESC m2 = wm_mode; /* MS-Empfehlung: Rate 0 nach dem Wechsel */
            m2.RefreshRate.Numerator = 0;
            m2.RefreshRate.Denominator = 0;
            sb_ctx_enter();
            IDXGISwapChain_ResizeTarget(wm_swap, &m2);
            sb_ctx_leave();
        }
    }
    wm_fs_on = 1;
    wm_fs_susp = 0;
    wm_fs_retry = 0;
    if (sb_render_resize(wm_mode.Width, wm_mode.Height, "exklusiv")) { wm_n_resize++; }
    wm_interp_match(1);
    return 1;
}

static void wm_leave_exclusive(const char *why)
{
    if (wm_fs_on && !wm_fs_susp) {
        wm_set_fs(FALSE, why);
    }
    wm_fs_on = 0;
    wm_fs_susp = 0;
    wm_interp_match(0);
}

/* ---------- Wechsel ---------- */
static void wm_restore_window(void)
{
    RECT r;
    LONG_PTR st;
    if (wm_saved) {
        r = wm_save_rc;
        st = wm_save_style;
    } else { /* Start im Vollbild: Fenster in cfg-Groesse */
        r.left = 0;
        r.top = 0;
        r.right = (LONG)(g_cfg.width ? g_cfg.width : 640);
        r.bottom = (LONG)(g_cfg.height ? g_cfg.height : 480);
        st = (GetWindowLongPtrA(wm_hwnd, GWL_STYLE) & ~(LONG_PTR)WS_POPUP) | WS_OVERLAPPEDWINDOW;
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        if (wm_dry) {
            wm_dry_rect((unsigned)(r.right - r.left), (unsigned)(r.bottom - r.top), &r);
        } else {
            OffsetRect(&r, 64 - r.left, 64 - r.top);
        }
    }
    if (!wm_place(&r, st)) { return; }
    /* Client exakt auf die alte Backbuffer-Groesse (Rahmen haengen von
     * Stil/DPI/Toolfenster ab - AdjustWindowRect allein trifft nicht immer). */
    {
        unsigned cw = wm_saved ? wm_save_cw : (g_cfg.width ? g_cfg.width : 640);
        unsigned ch = wm_saved ? wm_save_ch : (g_cfg.height ? g_cfg.height : 480);
        RECT c;
        if (GetClientRect(wm_hwnd, &c) && (c.right != (LONG)cw || c.bottom != (LONG)ch)) {
            RECT r2 = r;
            r2.right += (LONG)cw - c.right;
            r2.bottom += (LONG)ch - c.bottom;
            wm_place(&r2, st);
        }
    }
}

static void wm_switch(int to)
{
    int from = wm_cur, ok = 1;
    unsigned ow = g_bb_w, oh = g_bb_h;
    if (to == from) { return; }
    if (from == 2) { wm_leave_exclusive("Wechsel"); }
    if (from == 0) { wm_save_window(); }
    if (to == 0) {
        wm_restore_window();
        wm_sync_client("Fenster");
    } else if (to == 1) {
        ok = wm_enter_borderless();
    } else {
        ok = wm_enter_exclusive();
        if (!ok) {
            HLOG_W("WINMODE: exklusiv gescheitert -> randlos");
            to = 1;
            ok = wm_enter_borderless();
        }
    }
    if (!ok) {
        wm_n_fail++;
        HLOG_W("WINMODE: Wechsel %s -> %s gescheitert, bleibe %s", wm_name(from), wm_name(to),
               wm_name(from));
        InterlockedExchange(&wm_req, from);
        return;
    }
    wm_cur = to;
    InterlockedExchange(&wm_req, to);
    wm_nswitch++;
    wm_stat_due = 1;
    HLOG_I("WINMODE Wechsel #%u: %s -> %s (BB %ux%u -> %ux%u, trocken=%d, f=%u)", wm_nswitch,
           wm_name(from), wm_name(to), ow, oh, g_bb_w, g_bb_h, wm_dry, g_present_no);
}

/* Exklusiv: Fokusverlust/-rueckkehr (Alt+Tab) und verlorenes Vollbild. */
static void wm_focus_tick(unsigned n)
{
    int act = (int)wm_active;
    if (wm_cur != 2 || !wm_fs_on) { return; }
    if (!act && !wm_fs_susp) {
        wm_set_fs(FALSE, "Fokus verloren");
        wm_fs_susp = 1;
        if (wm_dry) {
            HLOG_I("WINMODE exklusiv: Trockenlauf - Minimieren uebersprungen (f=%u)", n);
        } else if (!IsIconic(wm_hwnd)) {
            ShowWindow(wm_hwnd, SW_SHOWMINNOACTIVE);
        }
        wm_n_susp++;
        HLOG_I("WINMODE exklusiv: ausgesetzt (Fokus verloren, f=%u)", n);
        return;
    }
    if (act && wm_fs_susp && n >= wm_fs_next) {
        HRESULT hr;
        if (!wm_dry && IsIconic(wm_hwnd)) { return; } /* wartet auf Wiederherstellen */
        hr = wm_set_fs(TRUE, "Fokus zurueck");
        if (FAILED(hr)) {
            wm_fs_retry++;
            wm_fs_next = n + 30u;
            if (wm_fs_retry >= 20) {
                HLOG_W("WINMODE exklusiv: Wiederherstellen scheitert (%d Versuche) -> randlos",
                       wm_fs_retry);
                wm_fs_susp = 0;
                wm_fs_on = 0;
                wm_interp_match(0);
                InterlockedExchange(&wm_req, 1); /* wm_switch(2 -> 1) */
            }
            return;
        }
        wm_fs_susp = 0;
        wm_fs_retry = 0;
        if (!wm_dry) {
            DXGI_MODE_DESC m2 = wm_mode;
            m2.RefreshRate.Numerator = 0;
            m2.RefreshRate.Denominator = 0;
            sb_ctx_enter();
            IDXGISwapChain_ResizeTarget(wm_swap, &m2);
            sb_ctx_leave();
        }
        if (sb_render_resize(wm_mode.Width, wm_mode.Height, "exklusiv zurueck")) { wm_n_resize++; }
        wm_n_resume++;
        HLOG_I("WINMODE exklusiv: wiederhergestellt (f=%u, BB %ux%u)", n, g_bb_w, g_bb_h);
        return;
    }
    /* Vollbild ohne unser Zutun verloren (DXGI/anderes Programm)? */
    if (!wm_dry && !wm_fs_susp && (n % 60u) == 0u) {
        BOOL fs = FALSE;
        if (SUCCEEDED(IDXGISwapChain_GetFullscreenState(wm_swap, &fs, NULL)) && !fs) {
            HLOG_W("WINMODE exklusiv: Vollbild verloren (f=%u) -> ausgesetzt", n);
            wm_fs_susp = 1;
            wm_fs_next = n + 30u;
        }
    }
}

/* Test-Hook (nur Trockenlauf): Alt+Enter / Fokus per PostMessage an das
 * eigene unsichtbare Fenster - fokusunabhaengig, kein Vordergrund. */
static void wm_test_tick(unsigned n)
{
    unsigned every = (unsigned)g_cfg.winmode_alt_every, from = (unsigned)g_cfg.winmode_alt_from;
    unsigned s;
    static unsigned base; /* winmode_alt_from < 0: Start relativ zur Mission */
    if (!wm_dry || g_cfg.winmode_alt_every <= 0) { return; }
    if (g_cfg.winmode_alt_from < 0) {
        /* erst ab der ersten Interp-Aufzeichnung (= Mission, interp_fps>0)
         * + |from| Presents: Wechsel mitten in der Zwischenbild-Wiedergabe */
        if (!base) {
            if (sb_interp_pairs() < 30u) { return; } /* >= 1 s echte Interp-Paare */
            base = n + (unsigned)(-g_cfg.winmode_alt_from);
            HLOG_I("WINMODE test: Mission erkannt (f=%u) -> Schritte ab f=%u", n, base);
        }
        from = base;
    }
    if (!from) { from = every; }
    if (n < from || ((n - from) % every) != 0u || wm_test_steps >= (unsigned)g_cfg.winmode_alt_max) {
        return;
    }
    /* Folge je Zyklus: Fokus weg, Fokus da (im Start-Vollbild), Alt+Enter
     * (-> Fenster), Alt+Enter (-> Vollbild). */
    s = wm_test_steps++ % 4u;
    if (s >= 2u) {
        LPARAM lp = 1 | (0x1C << 16) | (1 << 29); /* Wdh 1, Scan ENTER, Kontext = ALT */
        PostMessageA(wm_hwnd, WM_SYSKEYDOWN, VK_RETURN, lp);
        PostMessageA(wm_hwnd, WM_SYSKEYUP, VK_RETURN, lp | (1 << 30) | (1u << 31));
    } else {
        PostMessageA(wm_hwnd, WM_ACTIVATEAPP, s == 1u ? TRUE : FALSE, 0);
    }
    HLOG_I("WINMODE test: Schritt %u (%s) gepostet f=%u", wm_test_steps,
           s == 0u ? "WM_ACTIVATEAPP 0" : s == 1u ? "WM_ACTIVATEAPP 1" : "Alt+Enter", n);
}

/* ---------- Schnittstellen ---------- */

/* Nach dem Erzeugen der Swapchain (Loader-Thread, vor dem ersten Present). */
void sb_winmode_init(HWND hwnd, IDXGISwapChain *swap)
{
    int m = g_cfg.window_mode;
    wm_hwnd = hwnd;
    wm_swap = swap;
    if (m < 0 || m > 2) { m = 0; }
    wm_kind = m ? m : (g_cfg.alt_enter_mode == 2 ? 2 : 1);
    wm_dry = (g_cfg.winmode_test || g_cfg.window_hidden) ? 1 : 0;
    if (g_cfg.alt_enter) {
        IDXGIFactory *fac = NULL;
        if (SUCCEEDED(IDXGISwapChain_GetParent(swap, &IID_IDXGIFactory, (void **)&fac)) && fac) {
            HRESULT hr = IDXGIFactory_MakeWindowAssociation(fac, hwnd, DXGI_MWA_NO_ALT_ENTER);
            if (FAILED(hr)) {
                HLOG_W("WINMODE: MakeWindowAssociation hr=0x%08lX", (unsigned long)hr);
            }
            IDXGIFactory_Release(fac);
        }
    }
    wm_cur = 0;
    InterlockedExchange(&wm_req, m);
    wm_ready = 1;
    HLOG_I("WINMODE: window_mode=%d (%s) alt_enter=%d -> %s fs=%ux%u@%d monitor=%d win_resize=%d "
           "dpi=%d fs_interp_match=%d trocken=%d%s flags=0x%X",
           m, wm_name(m), g_cfg.alt_enter, wm_name(wm_kind), g_cfg.fs_width, g_cfg.fs_height,
           g_cfg.fs_hz, g_cfg.fs_monitor, g_cfg.win_resize, wm_dpi, g_cfg.fs_interp_match, wm_dry,
           (wm_dry && !g_cfg.winmode_test) ? " (erzwungen: window_hidden=1)" : "", g_swap_flags);
}

/* Fenster-Nachrichten (UI-Thread, vor dem Standardpfad). 1 = erledigt. */
int sb_winmode_wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp, LRESULT *res)
{
    (void)w;
    if (!wm_ready) { return 0; }
    switch (msg) {
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29)) && g_cfg.alt_enter) {
            if (!(lp & (1 << 30))) { /* keine Autowiederholung */
                LONG cur = wm_req, nx = cur ? 0 : wm_kind;
                InterlockedExchange(&wm_req, nx);
                InterlockedIncrement(&wm_n_alt);
            }
            *res = 0;
            return 1;
        }
        break;
    case WM_SYSCHAR:
        if (wp == VK_RETURN && (lp & (1 << 29)) && g_cfg.alt_enter) {
            *res = 0; /* kein Warnton */
            return 1;
        }
        break;
    case WM_ENTERSIZEMOVE:
        InterlockedExchange(&wm_in_sizemove, 1);
        break;
    case WM_EXITSIZEMOVE:
        InterlockedExchange(&wm_in_sizemove, 0);
        InterlockedExchange(&wm_size_pend, 1);
        break;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && !wm_in_sizemove) {
            InterlockedExchange(&wm_size_pend, 1);
        }
        break;
    case WM_ACTIVATEAPP:
        InterlockedExchange(&wm_active, wp ? 1 : 0);
        InterlockedIncrement(wp ? &wm_n_act1 : &wm_n_act0);
        break;
    default:
        break;
    }
    return 0;
}

/* Programmende (WM_CLOSE, UI-Thread): Exklusiv-Modus verlassen. */
void sb_winmode_on_close(void)
{
    if (wm_ready && wm_fs_on && !wm_fs_susp && !wm_dry && wm_swap) {
        IDXGISwapChain_SetFullscreenState(wm_swap, FALSE, NULL);
    }
}

/* Render-Thread, Ende jedes Presents (Bild fertig, kein Draw offen). */
void sb_winmode_tick(unsigned n)
{
    HANDLE dctx;
    int req;
    if (!wm_ready) { return; }
    dctx = wm_dpi_push();
    wm_test_tick(n);
    {
        LONG a = wm_n_alt, d0 = wm_n_act0, d1 = wm_n_act1;
        if (a != wm_seen_alt || d0 != wm_seen_act0 || d1 != wm_seen_act1) {
            HLOG_I("WINMODE: Nachrichten Alt+Enter +%ld, WM_ACTIVATEAPP 0 +%ld / 1 +%ld -> "
                   "Ziel %s, aktiv=%ld (f=%u)", a - wm_seen_alt, d0 - wm_seen_act0,
                   d1 - wm_seen_act1, wm_name((int)wm_req), (long)wm_active, n);
            wm_seen_alt = a;
            wm_seen_act0 = d0;
            wm_seen_act1 = d1;
            wm_stat_due = 1;
        }
    }
    req = (int)wm_req;
    if (req < 0 || req > 2) { req = 0; }
    if (req != wm_cur) {
        wm_switch(req);
    }
    wm_focus_tick(n);
    if (wm_stat_due && (n % 64u) == 0u) {
        wm_stat_due = 0;
        HLOG_I("WINMODE stat: modus=%s wechsel=%u alt_enter=%ld fokus_weg=%ld fokus_da=%ld "
               "ausgesetzt=%u wiederhergestellt=%u resize=%u fehl=%u BB=%ux%u f=%u",
               wm_name(wm_cur), wm_nswitch, (long)wm_n_alt, (long)wm_n_act0, (long)wm_n_act1,
               wm_n_susp, wm_n_resume, wm_n_resize, wm_n_fail, g_bb_w, g_bb_h, n);
    }
    if (wm_size_pend) {
        InterlockedExchange(&wm_size_pend, 0);
        /* Exklusiv: Groesse = Modus; sonst Client (nur bei win_resize; Wechsel
         * selbst gleichen immer ab, nie strecken). */
        if (!(wm_cur == 2 && wm_fs_on) && g_cfg.win_resize) {
            wm_sync_client(wm_cur ? "randlos" : "Fenstergroesse");
        }
    }
    wm_dpi_pop(dctx);
}

int sb_winmode_current(void)
{
    return wm_cur;
}
