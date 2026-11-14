/* vr_mock_runtime.c - W134 VR-PROTO: OpenXR-Test-Runtime (nur Tests, nie fuer Spieler).
 *
 * Zweck: den OpenXR-Pfad des Clients (vr.c: Mini-Loader, Instanz, System,
 * Session, Swapchain, Frame-Schleife, Projection-Layer) ohne Brille und ohne
 * die echte Runtime des Nutzers pruefen (Fokus-Regel: die echte Runtime
 * koennte Fenster/Streams oeffnen). Auswahl per cfg vr_runtime_json=<pfad>
 * (Manifest vr_mock_runtime.json neben der DLL, library_path relativ).
 *
 * Verhalten:
 *  - xrNegotiateLoaderRuntimeInterface + xrGetInstanceProcAddr nach Loader-
 *    Spezifikation, Erweiterung XR_KHR_D3D11_enable.
 *  - Ein System (HMD), Stereo, je Auge 960x1056, Sichtfeld links/rechts
 *    -50/+45 bzw. +45/-50 Grad, oben +48, unten -52 (asymmetrisch wie echte
 *    Brillen).
 *  - Kopfposen per Skript je Bild (90 Hz): Drehen +-30 Grad (4 s), Neigen
 *    +-10 Grad (3 s), IPD 64 mm.
 *  - xrWaitFrame taktet mit SB_VR_MOCK_HZ (Standard 90) ueber einen hoch-
 *    aufloesenden Wartezeitgeber.
 *  - xrEndFrame prueft den Projection-Layer (2 Ansichten, Swapchain, Rechtecke)
 *    und schreibt alle SB_VR_MOCK_SHOTS Bilder (Standard 180, max 12) je Auge
 *    ein BMP nach %SB_OUT_DIR%\shots\vrmock_<bild>_<auge>.bmp; Zeilen in
 *    %SB_OUT_DIR%\vr_mock.log (Rate je Sekunde, Posen, Pruefergebnis).
 *  - SB_VR_MOCK_NOHMD=1: xrGetSystem -> XR_ERROR_FORM_FACTOR_UNAVAILABLE
 *    (Fehlerpfad "Brille nicht verbunden").
 *  - W145 SB_VR_MOCK_BLOCK=<Buchstaben> (Standard leer = nie blockieren, wie W134):
 *    VDXR-aehnliche Wartepunkte (echte Runtimes warten auf GPU-Zaeune und den
 *    Compositor; die W134-Mock-Runtime blockierte nie -> Sperr-Deadlock blieb
 *    unentdeckt):
 *      w  Swapchain: xrReleaseSwapchainImage setzt einen GPU-Zaun auf dem
 *         Immediate-Context des Session-Devices (ID3D11DeviceContext4::Signal,
 *         OHNE Flush - die Runtime verlaesst sich auf spaetere Arbeit der App);
 *         ein Compositor-Thread haelt das zuletzt abgegebene Bild ("Anzeige")
 *         und gibt aeltere erst frei, wenn ihr Zaun erreicht ist.
 *         xrWaitSwapchainImage wartet, bis das Bild frei ist (Timeout der App).
 *      e  xrEndFrame nutzt den Immediate-Context (Compositor-Kopie des Bildes,
 *         ohne Flush), wartet bis zur naechsten Taktgrenze und auf den Zaun
 *         des abgegebenen Bildes ("Encoder braucht das fertige Bild").
 *      g  wie VDXR vermutet: xrWaitSwapchainImage ignoriert den Timeout
 *         (wartet unbegrenzt auf den Compositor).
 *      n  "Compositor wartet auf das naechste Spielbild": xrWaitSwapchainImage
 *         kehrt erst zurueck, wenn das Spiel seit dem letzten Release einen
 *         neuen Swap gemacht hat (Test-Export sb_test_frame_count von
 *         sb_emu.dll). Mit g unbegrenzt -> haelt die App dabei die Sperre, die
 *         das Spiel fuer den naechsten Swap braucht: Deadlock (W145_pre2).
 *    Jede Wartezeit > 1 s: Zeile "MOCK WARTET ..." in vr_mock.log.
 *  - W164 SB_VR_MOCK_POSES=w164: Posen-Programm (vorlehnen 0..0,6 m, rollen
 *    +-22,5/+-45 Grad, gieren +-30/+-60 Grad, seitlich +-15 cm), je Pose
 *    SB_VR_MOCK_POSE_HOLD s (Standard 1,5) in Schleife; Zeile "MOCK pose ...".
 *  - W183 SB_VR_MOCK_EYE=WxH: empfohlene Augengroesse (Standard 960x1056; Quest 3
 *    ueber VDXR z. B. 2496x2688), Maximum 4096x4096.
 *  - W183 XR_FB_display_refresh_rate: Bildraten 72/80/90/120 Hz, Wechsel per
 *    xrRequestDisplayRefreshRateFB (Takt + Ereignis); SB_VR_MOCK_NORR=1 = ohne.
 *  - W183 SB_VR_MOCK_UNFOCUS=a,b: ab a s nach Sessionstart Zustand VISIBLE
 *    (Brille ab/Systemmenue), ab b s wieder FOCUSED.
 * Kein Fenster, keine Geraete, kein Netzwerk. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "vr_openxr.h"

#pragma comment(linker, "/EXPORT:xrNegotiateLoaderRuntimeInterface=_xrNegotiateLoaderRuntimeInterface@8")

#define MOCK_EW 960u
#define MOCK_EH 1056u
#define MOCK_PI 3.14159265358979

static XrInstance m_inst;
static XrSession m_sess;
static XrSpace m_space;
static ID3D11Device *m_dev;
static int m_hz = 90, m_shot_every = 180, m_shots, m_nohmd, m_running, m_req_done;
static XrSessionState m_state;
static LARGE_INTEGER m_qpf;
static LONGLONG m_t0, m_next;
static HANDLE m_timer;
static unsigned m_frames, m_sec_frames, m_bad_layers, m_layers_ok, m_waits;
static LONGLONG m_sec_t0;
static FILE *m_log;
static char m_out[MAX_PATH];
/* Ereignis-Schlange (Session-Zustaende) */
static XrSessionState m_evq[16];
/* W183 */
static unsigned m_ew = MOCK_EW, m_eh = MOCK_EH;
static int m_norr, m_rr_ev;
static float m_rr_from, m_rr_to;
static double m_unf_a = -1.0, m_unf_b = -1.0;
static int m_unf_st;
static LONGLONG m_begin_q;
static int m_evn;
/* Swapchains */
typedef struct {
    int used;
    ID3D11Texture2D *img[3];
    uint32_t n, w, h, acq, last;
    int64_t fmt;
    volatile LONG busy[3];   /* W145: 1 = beim Compositor (abgegeben, noch nicht frei) */
    UINT64 fval[3];          /* W145: Zaunwert der Abgabe */
} mock_sc_t;
static mock_sc_t m_sc[4];

/* W145 SB_VR_MOCK_BLOCK */
static int m_blk_w, m_blk_e, m_blk_g;
static ID3D11DeviceContext *m_ctx;
static ID3D11DeviceContext4 *m_ctx4;
static ID3D11Fence *m_fence;
static UINT64 m_fval;
static HANDLE m_comp_thr, m_free_ev, m_fence_ev;
static volatile LONG m_comp_quit;
static ID3D11Texture2D *m_comp_tex;
static unsigned m_long_waits;
static CRITICAL_SECTION m_cs;
static double m_wait_max_ms[3]; /* 0 Swapchain-Wait, 1 EndFrame, 2 Compositor */
static int m_blk_n;
static unsigned (*m_game_frames)(void);
static volatile unsigned m_rel_frame;

static void mlog(const char *fmt, ...)
{
    va_list ap;
    if (!m_log) {
        char p[MAX_PATH];
        const char *o = getenv("SB_OUT_DIR");
        strcpy_s(m_out, sizeof m_out, (o && o[0]) ? o : ".");
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\vr_mock.log", m_out);
        fopen_s(&m_log, p, "a");
        if (!m_log) { return; }
    }
    va_start(ap, fmt);
    vfprintf(m_log, fmt, ap);
    va_end(ap);
    fputc('\n', m_log);
    fflush(m_log);
}

static LONGLONG mnow(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}

static XrTime q2ns(LONGLONG q)
{
    return (XrTime)((double)q * 1e9 / (double)m_qpf.QuadPart);
}

static void ev_push(XrSessionState s)
{
    if (m_evn < 16) { m_evq[m_evn++] = s; }
    m_state = s;
}

/* ---------- Instanz ---------- */

static XrResult XRAPI_CALL m_EnumerateInstanceExtensionProperties(const char *layer, uint32_t cap, uint32_t *n, XrExtensionProperties *p)
{
    (void)layer;
    if (!n) { return XR_ERROR_VALIDATION_FAILURE; }
    *n = (getenv("SB_VR_MOCK_NORR") && atoi(getenv("SB_VR_MOCK_NORR")) == 1) ? 1u : 2u; /* W183 */
    if (cap == 0) { return XR_SUCCESS; }
    if (!p) { return XR_ERROR_VALIDATION_FAILURE; }
    strcpy_s(p[0].extensionName, sizeof p[0].extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
    p[0].extensionVersion = 9;
    if (*n > 1u && cap > 1u) {
        strcpy_s(p[1].extensionName, sizeof p[1].extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
        p[1].extensionVersion = 1;
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_CreateInstance(const XrInstanceCreateInfo *ci, XrInstance *out)
{
    uint32_t i;
    int d3d = 0;
    if (!ci || !out || ci->type != XR_TYPE_INSTANCE_CREATE_INFO) { return XR_ERROR_VALIDATION_FAILURE; }
    if (XR_VERSION_MAJOR(ci->applicationInfo.apiVersion) != 1) { return XR_ERROR_API_VERSION_UNSUPPORTED; }
    for (i = 0; i < ci->enabledExtensionCount; i++) {
        if (strcmp(ci->enabledExtensionNames[i], XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) { d3d = 1; }
        else if (strcmp(ci->enabledExtensionNames[i], XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) == 0 &&
                 !(getenv("SB_VR_MOCK_NORR") && atoi(getenv("SB_VR_MOCK_NORR")) == 1)) { /* W183 */ }
        else { return XR_ERROR_EXTENSION_NOT_PRESENT; }
    }
    QueryPerformanceFrequency(&m_qpf);
    m_t0 = mnow();
    if (getenv("SB_VR_MOCK_HZ")) { m_hz = atoi(getenv("SB_VR_MOCK_HZ")); if (m_hz < 30 || m_hz > 240) { m_hz = 90; } }
    if (getenv("SB_VR_MOCK_EYE")) { /* W183 */
        unsigned w = 0, h = 0;
        if (sscanf_s(getenv("SB_VR_MOCK_EYE"), "%ux%u", &w, &h) == 2 && w >= 64u && h >= 64u && w <= 4096u && h <= 4096u) { m_ew = w; m_eh = h; }
    }
    m_norr = getenv("SB_VR_MOCK_NORR") && atoi(getenv("SB_VR_MOCK_NORR")) == 1;
    if (getenv("SB_VR_MOCK_UNFOCUS")) {
        if (sscanf_s(getenv("SB_VR_MOCK_UNFOCUS"), "%lf,%lf", &m_unf_a, &m_unf_b) != 2) { m_unf_a = m_unf_b = -1.0; }
    }
    if (getenv("SB_VR_MOCK_SHOTS")) { m_shot_every = atoi(getenv("SB_VR_MOCK_SHOTS")); }
    m_nohmd = getenv("SB_VR_MOCK_NOHMD") && atoi(getenv("SB_VR_MOCK_NOHMD")) == 1;
    {
        const char *b = getenv("SB_VR_MOCK_BLOCK");
        m_blk_w = b && strchr(b, 'w') != NULL;
        m_blk_e = b && strchr(b, 'e') != NULL;
        m_blk_g = b && strchr(b, 'g') != NULL;
        m_blk_n = b && strchr(b, 'n') != NULL;
        if (m_blk_n) {
            HMODULE emu = GetModuleHandleA("sb_emu.dll");
            m_game_frames = emu ? (unsigned (*)(void))(void *)GetProcAddress(emu, "sb_test_frame_count") : NULL;
            if (!m_game_frames) { m_blk_n = 0; }
        }
        if (m_blk_g || m_blk_n) { m_blk_w = 1; }
    }
    m_inst = 0x1001;
    *out = m_inst;
    mlog("MOCK xrCreateInstance app='%s' engine='%s' api=%u.%u d3d11=%d hz=%d shots=%d nohmd=%d block=%s%s%s",
         ci->applicationInfo.applicationName, ci->applicationInfo.engineName,
         XR_VERSION_MAJOR(ci->applicationInfo.apiVersion), XR_VERSION_MINOR(ci->applicationInfo.apiVersion),
         d3d, m_hz, m_shot_every, m_nohmd, m_blk_w ? "w" : "", m_blk_e ? "e" : "", m_blk_g ? (m_blk_n ? "gn" : "g") : (m_blk_n ? "n" : ""));
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_DestroyInstance(XrInstance i)
{
    if (i != m_inst) { return XR_ERROR_HANDLE_INVALID; }
    m_inst = 0;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_GetInstanceProperties(XrInstance i, XrInstanceProperties *p)
{
    if (i != m_inst || !p) { return XR_ERROR_HANDLE_INVALID; }
    p->runtimeVersion = XR_MAKE_VERSION(0, 1, 134);
    strcpy_s(p->runtimeName, sizeof p->runtimeName, "SB VR-Mock (Test-Runtime W134)");
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_PollEvent(XrInstance i, XrEventDataBuffer *b)
{
    XrEventDataSessionStateChanged *e;
    if (i != m_inst || !b) { return XR_ERROR_HANDLE_INVALID; }
    if (m_rr_ev) { /* W183 */
        XrEventDataDisplayRefreshRateChangedFB *r = (XrEventDataDisplayRefreshRateChangedFB *)b;
        memset(b, 0, sizeof *b);
        r->type = XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB;
        r->fromDisplayRefreshRate = m_rr_from;
        r->toDisplayRefreshRate = m_rr_to;
        m_rr_ev = 0;
        return XR_SUCCESS;
    }
    if (!m_evn) { return XR_EVENT_UNAVAILABLE; }
    e = (XrEventDataSessionStateChanged *)b;
    memset(b, 0, sizeof *b);
    e->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    e->session = m_sess;
    e->state = m_evq[0];
    e->time = q2ns(mnow());
    memmove(m_evq, m_evq + 1, (size_t)(m_evn - 1) * sizeof m_evq[0]);
    m_evn--;
    return XR_SUCCESS;
}

/* ---------- System ---------- */

static XrResult XRAPI_CALL m_GetSystem(XrInstance i, const XrSystemGetInfo *gi, XrSystemId *id)
{
    if (i != m_inst || !gi || !id) { return XR_ERROR_HANDLE_INVALID; }
    if (gi->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY || m_nohmd) {
        mlog("MOCK xrGetSystem -> FORM_FACTOR_UNAVAILABLE");
        return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
    }
    *id = 7;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_GetSystemProperties(XrInstance i, XrSystemId s, XrSystemProperties *p)
{
    if (i != m_inst || s != 7 || !p) { return XR_ERROR_HANDLE_INVALID; }
    p->systemId = 7;
    p->vendorId = 0x5342;
    strcpy_s(p->systemName, sizeof p->systemName, "Mock-Brille (Skript-Posen)");
    p->graphicsProperties.maxSwapchainImageWidth = 4096;
    p->graphicsProperties.maxSwapchainImageHeight = 4096;
    p->graphicsProperties.maxLayerCount = 16;
    p->trackingProperties.orientationTracking = XR_TRUE;
    p->trackingProperties.positionTracking = XR_TRUE;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_EnumerateViewConfigurationViews(XrInstance i, XrSystemId s, XrViewConfigurationType t,
                                                             uint32_t cap, uint32_t *n, XrViewConfigurationView *v)
{
    uint32_t k;
    if (i != m_inst || s != 7 || !n) { return XR_ERROR_HANDLE_INVALID; }
    if (t != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) { return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED; }
    *n = 2;
    if (!cap) { return XR_SUCCESS; }
    if (cap < 2 || !v) { return XR_ERROR_SIZE_INSUFFICIENT; }
    for (k = 0; k < 2; k++) {
        v[k].recommendedImageRectWidth = m_ew; /* W183 SB_VR_MOCK_EYE */
        v[k].recommendedImageRectHeight = m_eh;
        v[k].maxImageRectWidth = 4096;
        v[k].maxImageRectHeight = 4096;
        v[k].recommendedSwapchainSampleCount = 1;
        v[k].maxSwapchainSampleCount = 4;
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_GetD3D11GraphicsRequirementsKHR(XrInstance i, XrSystemId s, XrGraphicsRequirementsD3D11KHR *r)
{
    IDXGIFactory1 *f = NULL;
    IDXGIAdapter1 *a = NULL;
    DXGI_ADAPTER_DESC1 d;
    if (i != m_inst || s != 7 || !r) { return XR_ERROR_HANDLE_INVALID; }
    memset(&r->adapterLuid, 0, sizeof r->adapterLuid);
    if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f)) && f) {
        if (SUCCEEDED(IDXGIFactory1_EnumAdapters1(f, 0, &a)) && a) {
            if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d))) {
                r->adapterLuid.LowPart = d.AdapterLuid.LowPart;
                r->adapterLuid.HighPart = d.AdapterLuid.HighPart;
            }
            IDXGIAdapter1_Release(a);
        }
        IDXGIFactory1_Release(f);
    }
    r->minFeatureLevel = D3D_FEATURE_LEVEL_10_0;
    m_req_done = 1;
    return XR_SUCCESS;
}

/* ---------- W145: blockierende Wartepunkte (SB_VR_MOCK_BLOCK) ---------- */

static double ms_since(LONGLONG q0)
{
    return (double)(mnow() - q0) * 1000.0 / (double)m_qpf.QuadPart;
}

static void wait_note(int kind, double ms)
{
    if (ms > m_wait_max_ms[kind]) { m_wait_max_ms[kind] = ms; }
}

/* Compositor: das juengste abgegebene Bild bleibt "angezeigt"; aeltere werden
 * frei, sobald ihr Zaun erreicht ist (GPU-Arbeit der App fertig). */
static DWORD WINAPI mock_comp_main(LPVOID unused)
{
    LONGLONG warn_q = 0;
    (void)unused;
    while (!m_comp_quit) {
        int k;
        UINT64 done = m_fence ? ID3D11Fence_GetCompletedValue(m_fence) : 0;
        int pending = 0;
        EnterCriticalSection(&m_cs);
        for (k = 0; k < 4; k++) {
            uint32_t j;
            mock_sc_t *sc = &m_sc[k];
            if (!sc->used) { continue; }
            for (j = 0; j < sc->n; j++) {
                if (!sc->busy[j] || j == sc->last) { continue; }
                if (done >= sc->fval[j]) { InterlockedExchange(&sc->busy[j], 0); SetEvent(m_free_ev); }
                else { pending = 1; }
            }
        }
        LeaveCriticalSection(&m_cs);
        if (pending && m_fval > done) {
            if (!warn_q) { warn_q = mnow(); }
            if (ms_since(warn_q) >= 1000.0) {
                wait_note(2, ms_since(warn_q));
                mlog("MOCK WARTET Compositor: Zaun %llu nicht erreicht (fertig %llu) seit %.0f ms - GPU-Arbeit der App nicht abgeschickt (kein Flush)",
                     (unsigned long long)m_fval, (unsigned long long)done, ms_since(warn_q));
                m_long_waits++;
                warn_q = mnow();
            }
        } else {
            warn_q = 0;
        }
        Sleep(1);
    }
    return 0;
}

static int mock_block_init(void)
{
    ID3D11Device5 *d5 = NULL;
    ID3D11Device_GetImmediateContext(m_dev, &m_ctx);
    if (!m_ctx) { return 0; }
    if (FAILED(ID3D11DeviceContext_QueryInterface(m_ctx, &IID_ID3D11DeviceContext4, (void **)&m_ctx4)) || !m_ctx4) { return 0; }
    if (FAILED(ID3D11Device_QueryInterface(m_dev, &IID_ID3D11Device5, (void **)&d5)) || !d5) { return 0; }
    if (FAILED(ID3D11Device5_CreateFence(d5, 0, D3D11_FENCE_FLAG_NONE, &IID_ID3D11Fence, (void **)&m_fence)) || !m_fence) {
        ID3D11Device5_Release(d5);
        return 0;
    }
    ID3D11Device5_Release(d5);
    InitializeCriticalSection(&m_cs);
    m_free_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    m_fence_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    m_comp_thr = CreateThread(NULL, 0, mock_comp_main, NULL, 0, NULL);
    mlog("MOCK blockierend: swapchain-wait=%d endframe=%d timeout-ignorieren=%d (Zaun ohne Flush auf dem Session-Device %p)",
         m_blk_w, m_blk_e, m_blk_g, (void *)m_dev);
    return m_free_ev && m_fence_ev && m_comp_thr;
}

/* ---------- Session ---------- */

static XrResult XRAPI_CALL m_CreateSession(XrInstance i, const XrSessionCreateInfo *ci, XrSession *out)
{
    const XrBaseInStructure *b;
    const XrGraphicsBindingD3D11KHR *gb = NULL;
    if (i != m_inst || !ci || !out || ci->systemId != 7) { return XR_ERROR_VALIDATION_FAILURE; }
    if (!m_req_done) { return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING; }
    for (b = (const XrBaseInStructure *)ci->next; b; b = (const XrBaseInStructure *)b->next) {
        if (b->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) { gb = (const XrGraphicsBindingD3D11KHR *)b; }
    }
    if (!gb || !gb->device) { return XR_ERROR_GRAPHICS_DEVICE_INVALID; }
    m_dev = (ID3D11Device *)gb->device;
    ID3D11Device_AddRef(m_dev);
    if ((m_blk_w || m_blk_e) && !mock_block_init()) {
        mlog("MOCK SB_VR_MOCK_BLOCK: Zaun/Context nicht verfuegbar -> blockiert nicht");
        m_blk_w = m_blk_e = m_blk_g = 0;
    }
    m_sess = 0x2001;
    *out = m_sess;
    ev_push(XR_SESSION_STATE_IDLE);
    ev_push(XR_SESSION_STATE_READY);
    mlog("MOCK xrCreateSession device=%p feature=0x%X -> IDLE, READY", (void *)m_dev, (unsigned)ID3D11Device_GetFeatureLevel(m_dev));
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_DestroySession(XrSession s)
{
    if (s != m_sess) { return XR_ERROR_HANDLE_INVALID; }
    m_sess = 0;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_BeginSession(XrSession s, const XrSessionBeginInfo *bi)
{
    if (s != m_sess || !bi) { return XR_ERROR_HANDLE_INVALID; }
    if (m_state != XR_SESSION_STATE_READY) { return XR_ERROR_SESSION_NOT_READY; }
    if (bi->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) { return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED; }
    m_running = 1;
    ev_push(XR_SESSION_STATE_SYNCHRONIZED);
    ev_push(XR_SESSION_STATE_VISIBLE);
    ev_push(XR_SESSION_STATE_FOCUSED);
    m_begin_q = mnow(); /* W183 SB_VR_MOCK_UNFOCUS */
    m_timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (!m_timer) { m_timer = CreateWaitableTimerW(NULL, FALSE, NULL); }
    m_next = mnow();
    m_sec_t0 = m_next;
    mlog("MOCK xrBeginSession -> SYNCHRONIZED, VISIBLE, FOCUSED (Takt %d Hz)", m_hz);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_EndSession(XrSession s)
{
    if (s != m_sess) { return XR_ERROR_HANDLE_INVALID; }
    m_running = 0;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_CreateReferenceSpace(XrSession s, const XrReferenceSpaceCreateInfo *ci, XrSpace *out)
{
    if (s != m_sess || !ci || !out) { return XR_ERROR_HANDLE_INVALID; }
    if (ci->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_LOCAL && ci->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_STAGE &&
        ci->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_VIEW) { return XR_ERROR_VALIDATION_FAILURE; }
    m_space = 0x3000 + (XrSpace)ci->referenceSpaceType;
    *out = m_space;
    mlog("MOCK xrCreateReferenceSpace typ=%d", (int)ci->referenceSpaceType);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_DestroySpace(XrSpace sp)
{
    (void)sp;
    return XR_SUCCESS;
}

/* ---------- Swapchain ---------- */

static const int64_t kFmts[3] = { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB };

static XrResult XRAPI_CALL m_EnumerateSwapchainFormats(XrSession s, uint32_t cap, uint32_t *n, int64_t *f)
{
    uint32_t k;
    if (s != m_sess || !n) { return XR_ERROR_HANDLE_INVALID; }
    *n = 3;
    if (!cap) { return XR_SUCCESS; }
    if (cap < 3 || !f) { return XR_ERROR_SIZE_INSUFFICIENT; }
    for (k = 0; k < 3; k++) { f[k] = kFmts[k]; }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_CreateSwapchain(XrSession s, const XrSwapchainCreateInfo *ci, XrSwapchain *out)
{
    int k;
    uint32_t j;
    D3D11_TEXTURE2D_DESC td;
    mock_sc_t *sc = NULL;
    if (s != m_sess || !ci || !out || !m_dev) { return XR_ERROR_HANDLE_INVALID; }
    for (k = 0; k < 4; k++) { if (!m_sc[k].used) { sc = &m_sc[k]; break; } }
    if (!sc) { return XR_ERROR_LIMIT_REACHED; }
    memset(&td, 0, sizeof td);
    td.Width = ci->width; td.Height = ci->height; td.MipLevels = 1; td.ArraySize = 1;
    /* wie echte Runtimes: sRGB-Wunsch -> typlose Textur (App waehlt die View) */
    td.Format = (ci->format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || ci->format == DXGI_FORMAT_R8G8B8A8_UNORM) ?
                DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_B8G8R8A8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (j = 0; j < 3; j++) {
        if (FAILED(ID3D11Device_CreateTexture2D(m_dev, &td, NULL, &sc->img[j]))) { return XR_ERROR_RUNTIME_FAILURE; }
    }
    if (m_blk_e && !m_comp_tex && FAILED(ID3D11Device_CreateTexture2D(m_dev, &td, NULL, &m_comp_tex))) { m_comp_tex = NULL; }
    sc->used = 1; sc->n = 3; sc->w = ci->width; sc->h = ci->height; sc->fmt = ci->format; sc->acq = 0xFFFFFFFFu; sc->last = 0;
    *out = 0x4000 + (XrSwapchain)k;
    mlog("MOCK xrCreateSwapchain %ux%u Format %d (typlos) Nutzung 0x%llX", ci->width, ci->height, (int)ci->format,
         (unsigned long long)ci->usageFlags);
    return XR_SUCCESS;
}

static mock_sc_t *sc_of(XrSwapchain h)
{
    if (h < 0x4000 || h >= 0x4004) { return NULL; }
    return m_sc[h - 0x4000].used ? &m_sc[h - 0x4000] : NULL;
}

static XrResult XRAPI_CALL m_DestroySwapchain(XrSwapchain h)
{
    uint32_t j;
    mock_sc_t *sc = sc_of(h);
    if (!sc) { return XR_ERROR_HANDLE_INVALID; }
    for (j = 0; j < sc->n; j++) { if (sc->img[j]) { ID3D11Texture2D_Release(sc->img[j]); } }
    memset(sc, 0, sizeof *sc);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_EnumerateSwapchainImages(XrSwapchain h, uint32_t cap, uint32_t *n, XrSwapchainImageBaseHeader *im)
{
    uint32_t j;
    mock_sc_t *sc = sc_of(h);
    if (!sc || !n) { return XR_ERROR_HANDLE_INVALID; }
    *n = sc->n;
    if (!cap) { return XR_SUCCESS; }
    if (cap < sc->n || !im) { return XR_ERROR_SIZE_INSUFFICIENT; }
    for (j = 0; j < sc->n; j++) {
        XrSwapchainImageD3D11KHR *d = &((XrSwapchainImageD3D11KHR *)im)[j];
        if (d->type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) { return XR_ERROR_VALIDATION_FAILURE; }
        d->texture = (struct ID3D11Texture2D *)sc->img[j];
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_AcquireSwapchainImage(XrSwapchain h, const XrSwapchainImageAcquireInfo *ai, uint32_t *idx)
{
    mock_sc_t *sc = sc_of(h);
    (void)ai;
    if (!sc || !idx) { return XR_ERROR_HANDLE_INVALID; }
    if (sc->acq != 0xFFFFFFFFu) { return XR_ERROR_CALL_ORDER_INVALID; }
    sc->acq = (sc->last + 1u) % sc->n;
    *idx = sc->acq;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_WaitSwapchainImage(XrSwapchain h, const XrSwapchainImageWaitInfo *wi)
{
    mock_sc_t *sc = sc_of(h);
    LONGLONG q0, warn_q;
    double tmo_ms;
    if (!sc || sc->acq == 0xFFFFFFFFu || !wi) { return XR_ERROR_CALL_ORDER_INVALID; }
    if (!m_blk_w) { return XR_SUCCESS; }
    /* W145: wartet, bis der Compositor das Bild freigibt (g: Timeout ignoriert) */
    q0 = warn_q = mnow();
    tmo_ms = wi->timeout >= 0 ? (double)wi->timeout * 1e-6 : 1e12;
    while (sc->busy[sc->acq] || (m_blk_n && m_game_frames() == m_rel_frame)) {
        double ms = ms_since(q0);
        if (!m_blk_g && ms >= tmo_ms) { wait_note(0, ms); return XR_TIMEOUT_EXPIRED; }
        if (ms_since(warn_q) >= 1000.0) {
            mlog("MOCK WARTET xrWaitSwapchainImage Bild %u seit %.0f ms (%s, Thread %lu)", sc->acq, ms,
                 sc->busy[sc->acq] ? "Compositor haelt es" : "kein neues Spielbild seit dem letzten Release", GetCurrentThreadId());
            m_long_waits++;
            warn_q = mnow();
        }
        WaitForSingleObject(m_free_ev, 1);
    }
    wait_note(0, ms_since(q0));
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_ReleaseSwapchainImage(XrSwapchain h, const XrSwapchainImageReleaseInfo *ri)
{
    mock_sc_t *sc = sc_of(h);
    (void)ri;
    if (!sc || sc->acq == 0xFFFFFFFFu) { return XR_ERROR_CALL_ORDER_INVALID; }
    if (m_blk_w || m_blk_e) {
        /* W145: Zaun auf dem Immediate-Context des Session-Devices, ohne Flush */
        EnterCriticalSection(&m_cs);
        m_fval++;
        ID3D11DeviceContext4_Signal(m_ctx4, m_fence, m_fval);
        sc->fval[sc->acq] = m_fval;
        if (m_blk_n) { m_rel_frame = m_game_frames(); }
        InterlockedExchange(&sc->busy[sc->acq], 1);
        sc->last = sc->acq;
        sc->acq = 0xFFFFFFFFu;
        LeaveCriticalSection(&m_cs);
        return XR_SUCCESS;
    }
    sc->last = sc->acq;
    sc->acq = 0xFFFFFFFFu;
    return XR_SUCCESS;
}

/* ---------- Frame-Schleife ---------- */

static XrResult XRAPI_CALL m_WaitFrame(XrSession s, const XrFrameWaitInfo *wi, XrFrameState *fs)
{
    LONGLONG per, now;
    (void)wi;
    if (s != m_sess || !fs) { return XR_ERROR_HANDLE_INVALID; }
    if (!m_running) { return XR_ERROR_SESSION_NOT_RUNNING; }
    per = m_qpf.QuadPart / m_hz;
    m_next += per;
    now = mnow();
    if (m_next < now - per) { m_next = now; } /* weit zurueck (App blockiert) -> neu ansetzen */
    if (m_next > now && m_timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((double)(m_next - now) * 1e7 / (double)m_qpf.QuadPart);
        if (SetWaitableTimer(m_timer, &due, 0, NULL, NULL, FALSE)) { WaitForSingleObject(m_timer, 100); }
    }
    while (mnow() < m_next) { YieldProcessor(); }
    fs->predictedDisplayPeriod = (XrDuration)(1e9 / m_hz);
    fs->predictedDisplayTime = q2ns(m_next + per);
    fs->shouldRender = XR_TRUE;
    m_waits++;
    if (m_unf_a >= 0.0 && m_begin_q) { /* W183: Brille ab (VISIBLE) und wieder auf (FOCUSED) */
        double ts = (double)(mnow() - m_begin_q) / (double)m_qpf.QuadPart;
        if (m_unf_st == 0 && ts >= m_unf_a) { m_unf_st = 1; ev_push(XR_SESSION_STATE_VISIBLE); mlog("MOCK Zustand VISIBLE (t=%.1fs, Brille ab)", ts); }
        else if (m_unf_st == 1 && ts >= m_unf_b) { m_unf_st = 2; ev_push(XR_SESSION_STATE_FOCUSED); mlog("MOCK Zustand FOCUSED (t=%.1fs, Brille auf)", ts); }
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_BeginFrame(XrSession s, const XrFrameBeginInfo *bi)
{
    (void)bi;
    if (s != m_sess) { return XR_ERROR_HANDLE_INVALID; }
    return m_running ? XR_SUCCESS : XR_ERROR_SESSION_NOT_RUNNING;
}

/* W164 SB_VR_MOCK_POSES=w164: Posen-Programm statt Sinus-Skript. Jede Pose wird
 * SB_VR_MOCK_POSE_HOLD Sekunden (Standard 1,5) gehalten, das Programm laeuft ab
 * t = 0 in Schleife (der Client sichert je neue, ruhige Pose ein Augenbild,
 * cfg vr_pose_shots). Vorlehnen = Kopf nach vorn (-z im LOCAL-Raum). */
typedef struct { const char *name; double lean, x, roll, yaw, pitch; } mock_pose_t; /* W242: + Nicken (Grad, + = hoch) */
static const mock_pose_t kPoses[] = {
    { "gerade", 0.0, 0.0, 0.0, 0.0 },
    { "vor10", 0.10, 0.0, 0.0, 0.0 }, { "vor20", 0.20, 0.0, 0.0, 0.0 }, { "vor30", 0.30, 0.0, 0.0, 0.0 },
    { "vor40", 0.40, 0.0, 0.0, 0.0 }, { "vor50", 0.50, 0.0, 0.0, 0.0 }, { "vor60", 0.60, 0.0, 0.0, 0.0 },
    { "roll-45", 0.0, 0.0, -45.0, 0.0 }, { "roll-22", 0.0, 0.0, -22.5, 0.0 },
    { "roll+22", 0.0, 0.0, 22.5, 0.0 }, { "roll+45", 0.0, 0.0, 45.0, 0.0 },
    { "vor30_roll-45", 0.30, 0.0, -45.0, 0.0 }, { "vor30_roll+45", 0.30, 0.0, 45.0, 0.0 },
    { "gier-60", 0.0, 0.0, 0.0, -60.0 }, { "gier-30", 0.0, 0.0, 0.0, -30.0 },
    { "gier+30", 0.0, 0.0, 0.0, 30.0 }, { "gier+60", 0.0, 0.0, 0.0, 60.0 },
    { "links15", 0.0, -0.15, 0.0, 0.0 }, { "rechts15", 0.0, 0.15, 0.0, 0.0 },
};
/* W242 SB_VR_MOCK_POSES=w242: Umschauen (Gieren bis 180 Grad, Nicken bis +-60 Grad) fuer Huelle/Vignette */
static const mock_pose_t kPoses242[] = {
    { "gerade", 0, 0, 0, 0, 0 },
    { "gier+30", 0, 0, 0, 30, 0 }, { "gier+45", 0, 0, 0, 45, 0 }, { "gier+60", 0, 0, 0, 60, 0 },
    { "gier+90", 0, 0, 0, 90, 0 }, { "gier+135", 0, 0, 0, 135, 0 }, { "gier180", 0, 0, 0, 180, 0 },
    { "gier-45", 0, 0, 0, -45, 0 }, { "gier-60", 0, 0, 0, -60, 0 }, { "gier-90", 0, 0, 0, -90, 0 },
    { "gier-135", 0, 0, 0, -135, 0 },
    { "hoch20", 0, 0, 0, 0, 20 }, { "hoch40", 0, 0, 0, 0, 40 }, { "hoch60", 0, 0, 0, 0, 60 },
    { "runter20", 0, 0, 0, 0, -20 }, { "runter40", 0, 0, 0, 0, -40 }, { "runter60", 0, 0, 0, 0, -60 },
    { "gier+60_hoch30", 0, 0, 0, 60, 30 }, { "vor30_gier+60", 0.30, 0, 0, 60, 0 },
};
static int m_poses = -1; /* -1 = noch nicht gelesen, 0 = Sinus-Skript, 1 = Posen-Programm w164, 2 = w242 */
static double m_pose_hold = 1.5;
static int m_pose_last = -1;

static int mock_pose_prog(double t, float q[4], float p[3], double *yaw_deg, double *pitch_deg)
{
    int n = (int)(sizeof kPoses / sizeof kPoses[0]), k;
    const mock_pose_t *ps, *tab = kPoses;
    double yaw, roll, pitch, cy, sy, cr, sr, cp, sp;
    if (m_poses < 0) {
        const char *e = getenv("SB_VR_MOCK_POSES");
        m_poses = (e && _stricmp(e, "w164") == 0) ? 1 : (e && _stricmp(e, "w242") == 0) ? 2 : 0;
        if (getenv("SB_VR_MOCK_POSE_HOLD")) { m_pose_hold = atof(getenv("SB_VR_MOCK_POSE_HOLD")); }
        if (m_pose_hold < 0.2) { m_pose_hold = 1.5; }
        if (m_poses == 2) { n = (int)(sizeof kPoses242 / sizeof kPoses242[0]); }
        if (m_poses) { mlog("MOCK Posen-Programm %s: %d Posen je %.1f s in Schleife", m_poses == 2 ? "w242" : "w164", n, m_pose_hold); }
    }
    if (!m_poses) { return 0; }
    if (m_poses == 2) { tab = kPoses242; n = (int)(sizeof kPoses242 / sizeof kPoses242[0]); }
    if (t < 0.0) { t = 0.0; }
    k = (int)(t / m_pose_hold) % n;
    ps = &tab[k];
    if (k != m_pose_last) {
        m_pose_last = k;
        mlog("MOCK pose %d '%s' t=%.1fs vor=%.2fm x=%.2fm roll=%.1f gier=%.1f nicken=%.1f", k, ps->name, t, ps->lean, ps->x,
             ps->roll, ps->yaw, ps->pitch);
    }
    yaw = ps->yaw * MOCK_PI / 180.0;
    roll = ps->roll * MOCK_PI / 180.0;
    pitch = ps->pitch * MOCK_PI / 180.0;
    cy = cos(yaw * 0.5); sy = sin(yaw * 0.5); cr = cos(roll * 0.5); sr = sin(roll * 0.5);
    cp = cos(pitch * 0.5); sp = sin(pitch * 0.5);
    if (ps->pitch != 0.0) {
        /* W242: q = q_yaw(y) * q_pitch(x), Rollen 0 (wie das Sinus-Skript) */
        q[0] = (float)(cy * sp); q[1] = (float)(sy * cp); q[2] = (float)(-sy * sp); q[3] = (float)(cy * cp);
    } else {
        /* q = q_yaw(y) * q_roll(z), Nicken 0: (0,sy,0,cy)*(0,0,sr,cr) = (sy*sr, sy*cr, cy*sr, cy*cr) */
        q[0] = (float)(sy * sr);
        q[1] = (float)(sy * cr);
        q[2] = (float)(cy * sr);
        q[3] = (float)(cy * cr);
    }
    p[0] = (float)ps->x; p[1] = 0.0f; p[2] = (float)(-ps->lean);
    *yaw_deg = ps->yaw;
    *pitch_deg = ps->pitch;
    return 1;
}

/* Skript-Kopfpose zur Zeit t (s): Drehen +-30 Grad (4 s), Neigen +-10 Grad (3 s). */
static void mock_head(double t, float q[4], float p[3], double *yaw_deg, double *pitch_deg)
{
    double yaw = 30.0 * sin(2.0 * MOCK_PI * t / 4.0) * MOCK_PI / 180.0;
    double pitch = 10.0 * sin(2.0 * MOCK_PI * t / 3.0) * MOCK_PI / 180.0;
    double cy = cos(yaw * 0.5), sy = sin(yaw * 0.5), cp = cos(pitch * 0.5), sp = sin(pitch * 0.5);
    if (mock_pose_prog(t, q, p, yaw_deg, pitch_deg)) { return; } /* W164 */
    /* q = q_yaw(y) * q_pitch(x) */
    q[0] = (float)(cy * sp); q[1] = (float)(sy * cp); q[2] = (float)(-sy * sp); q[3] = (float)(cy * cp);
    p[0] = (float)(0.02 * sin(2.0 * MOCK_PI * t / 4.0)); p[1] = 0.0f; p[2] = 0.0f;
    *yaw_deg = yaw * 180.0 / MOCK_PI;
    *pitch_deg = pitch * 180.0 / MOCK_PI;
}

static XrResult XRAPI_CALL m_LocateViews(XrSession s, const XrViewLocateInfo *li, XrViewState *vs, uint32_t cap, uint32_t *n, XrView *v)
{
    float q[4], p[3];
    double t, yd, pd;
    int e;
    if (s != m_sess || !li || !vs || !n) { return XR_ERROR_HANDLE_INVALID; }
    *n = 2;
    if (!cap) { return XR_SUCCESS; }
    if (cap < 2 || !v) { return XR_ERROR_SIZE_INSUFFICIENT; }
    t = (double)(li->displayTime - q2ns(m_t0)) * 1e-9;
    mock_head(t, q, p, &yd, &pd);
    vs->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    for (e = 0; e < 2; e++) {
        /* Augenversatz +-32 mm entlang der Kopf-x-Achse */
        float ox = (e == 0 ? -0.032f : 0.032f);
        float x = q[0], y = q[1], z = q[2], w = q[3];
        float rx = 1 - 2 * (y * y + z * z), ry = 2 * (x * y + z * w), rz = 2 * (x * z - y * w);
        v[e].pose.orientation.x = q[0]; v[e].pose.orientation.y = q[1];
        v[e].pose.orientation.z = q[2]; v[e].pose.orientation.w = q[3];
        v[e].pose.position.x = p[0] + ox * rx;
        v[e].pose.position.y = p[1] + ox * ry;
        v[e].pose.position.z = p[2] + ox * rz;
        v[e].fov.angleLeft = (float)((e == 0 ? -50.0 : -45.0) * MOCK_PI / 180.0);
        v[e].fov.angleRight = (float)((e == 0 ? 45.0 : 50.0) * MOCK_PI / 180.0);
        v[e].fov.angleUp = (float)(48.0 * MOCK_PI / 180.0);
        v[e].fov.angleDown = (float)(-52.0 * MOCK_PI / 180.0);
    }
    return XR_SUCCESS;
}

static void write_bmp(const char *path, const unsigned char *rgba, unsigned pitch, unsigned x0, unsigned w, unsigned h)
{
    FILE *f = NULL;
    unsigned rowb = (w * 3u + 3u) & ~3u, y, x;
    unsigned char hdr[54], *row;
    unsigned size = 54u + rowb * h;
    fopen_s(&f, path, "wb");
    if (!f) { return; }
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    *(unsigned *)(hdr + 2) = size;
    *(unsigned *)(hdr + 10) = 54;
    *(unsigned *)(hdr + 14) = 40;
    *(int *)(hdr + 18) = (int)w;
    *(int *)(hdr + 22) = (int)h;
    *(unsigned short *)(hdr + 26) = 1;
    *(unsigned short *)(hdr + 28) = 24;
    *(unsigned *)(hdr + 34) = rowb * h;
    fwrite(hdr, 1, 54, f);
    row = (unsigned char *)calloc(1, rowb);
    for (y = 0; row && y < h; y++) {
        const unsigned char *src = rgba + (size_t)(h - 1u - y) * pitch + (size_t)x0 * 4u;
        for (x = 0; x < w; x++) { row[x * 3] = src[x * 4 + 2]; row[x * 3 + 1] = src[x * 4 + 1]; row[x * 3 + 2] = src[x * 4]; }
        fwrite(row, 1, rowb, f);
    }
    free(row);
    fclose(f);
}

static void mock_shot(const XrCompositionLayerProjection *pl, double yd, double pd)
{
    mock_sc_t *sc = sc_of(pl->views[0].subImage.swapchain);
    ID3D11DeviceContext *ctx = NULL;
    ID3D11Texture2D *stg = NULL;
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE map;
    uint32_t e;
    if (!sc || !m_dev) { return; }
    ID3D11Texture2D_GetDesc(sc->img[sc->last], &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(m_dev, &td, NULL, &stg))) { return; }
    ID3D11Device_GetImmediateContext(m_dev, &ctx);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stg, (ID3D11Resource *)sc->img[sc->last]);
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &map))) {
        for (e = 0; e < pl->viewCount && e < 2; e++) {
            char p[MAX_PATH];
            const XrRect2Di *r = &pl->views[e].subImage.imageRect;
            _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\shots\\vrmock_%05u_%s.bmp", m_out, m_frames, e == 0 ? "links" : "rechts");
            write_bmp(p, (const unsigned char *)map.pData, map.RowPitch, (unsigned)r->offset.x, (unsigned)r->extent.width,
                      (unsigned)r->extent.height);
        }
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stg, 0);
        mlog("MOCK shot bild=%u -> vrmock_%05u_links/rechts.bmp (Kopf yaw=%.1f pitch=%.1f, Ansicht 0 pos=(%.3f,%.3f,%.3f))",
             m_frames, m_frames, yd, pd, pl->views[0].pose.position.x, pl->views[0].pose.position.y, pl->views[0].pose.position.z);
    }
    ID3D11Texture2D_Release(stg);
    ID3D11DeviceContext_Release(ctx);
}

static XrResult XRAPI_CALL m_EndFrame(XrSession s, const XrFrameEndInfo *fe)
{
    uint32_t k;
    LONGLONG now = mnow();
    if (s != m_sess || !fe) { return XR_ERROR_HANDLE_INVALID; }
    if (fe->environmentBlendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE) { return XR_ERROR_VALIDATION_FAILURE; }
    m_frames++;
    m_sec_frames++;
    for (k = 0; k < fe->layerCount; k++) {
        const XrCompositionLayerProjection *pl = (const XrCompositionLayerProjection *)fe->layers[k];
        int ok = pl && pl->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION && pl->viewCount == 2 && pl->views &&
                 pl->space == m_space;
        uint32_t e;
        for (e = 0; ok && e < 2; e++) {
            const XrCompositionLayerProjectionView *v = &pl->views[e];
            mock_sc_t *sc = sc_of(v->subImage.swapchain);
            ok = v->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW && sc && sc->acq == 0xFFFFFFFFu &&
                 v->subImage.imageRect.offset.x >= 0 && v->subImage.imageRect.offset.y >= 0 &&
                 (uint32_t)(v->subImage.imageRect.offset.x + v->subImage.imageRect.extent.width) <= sc->w &&
                 (uint32_t)(v->subImage.imageRect.offset.y + v->subImage.imageRect.extent.height) <= sc->h &&
                 v->fov.angleLeft < v->fov.angleRight && v->fov.angleDown < v->fov.angleUp;
        }
        if (ok) {
            m_layers_ok++;
            if (m_shot_every > 0 && m_shots < 12 && (m_frames % (unsigned)m_shot_every) == 0u) {
                float q[4], p[3];
                double yd, pd;
                mock_head((double)(fe->displayTime - q2ns(m_t0)) * 1e-9, q, p, &yd, &pd);
                m_shots++;
                mock_shot(pl, yd, pd);
            }
        } else {
            m_bad_layers++;
        }
    }
    if (m_blk_e && fe->layerCount > 0) {
        /* W145: Compositor-Kopie ueber den Immediate-Context (ohne Flush), dann
         * bis zur naechsten Taktgrenze und auf den Zaun des Bildes warten. */
        LONGLONG q0 = mnow(), per = m_qpf.QuadPart / m_hz;
        const XrCompositionLayerProjection *pl = (const XrCompositionLayerProjection *)fe->layers[0];
        mock_sc_t *sc = pl ? sc_of(pl->views[0].subImage.swapchain) : NULL;
        if (sc && m_comp_tex && m_ctx) {
            ID3D11DeviceContext_CopyResource(m_ctx, (ID3D11Resource *)m_comp_tex, (ID3D11Resource *)sc->img[sc->last]);
        }
        while (mnow() < m_next + per) { Sleep(0); }
        while (ID3D11Fence_GetCompletedValue(m_fence) < m_fval) {
            ID3D11Fence_SetEventOnCompletion(m_fence, m_fval, m_fence_ev);
            if (WaitForSingleObject(m_fence_ev, 1000) == WAIT_TIMEOUT) {
                mlog("MOCK WARTET xrEndFrame seit %.0f ms auf Zaun %llu (fertig %llu, Thread %lu) - Bild der App nicht auf der GPU",
                     ms_since(q0), (unsigned long long)m_fval, (unsigned long long)ID3D11Fence_GetCompletedValue(m_fence),
                     GetCurrentThreadId());
                m_long_waits++;
            }
        }
        wait_note(1, ms_since(q0));
        now = mnow();
    }
    if (now - m_sec_t0 >= m_qpf.QuadPart) {
        double sec = (double)(now - m_sec_t0) / (double)m_qpf.QuadPart;
        mlog("MOCK rate t=%.1fs bilder=%u (%.1f/s) layer_ok=%u layer_fehler=%u gesamt=%u",
             (double)(now - m_t0) / (double)m_qpf.QuadPart, m_sec_frames, m_sec_frames / sec, m_layers_ok, m_bad_layers, m_frames);
        if (m_blk_w || m_blk_e) {
            mlog("MOCK block max-wartezeit/s ms: swapchain-wait=%.1f endframe=%.1f compositor=%.1f lange=%u",
                 m_wait_max_ms[0], m_wait_max_ms[1], m_wait_max_ms[2], m_long_waits);
            m_wait_max_ms[0] = m_wait_max_ms[1] = m_wait_max_ms[2] = 0;
        }
        m_sec_frames = 0;
        m_sec_t0 = now;
    }
    return XR_SUCCESS;
}

/* ---------- W183: XR_FB_display_refresh_rate ---------- */
static const float kMockRates[4] = { 72.0f, 80.0f, 90.0f, 120.0f };

static XrResult XRAPI_CALL m_EnumerateDisplayRefreshRatesFB(XrSession s, uint32_t cap, uint32_t *n, float *r)
{
    uint32_t k;
    if (s != m_sess || !n || m_norr) { return XR_ERROR_HANDLE_INVALID; }
    *n = 4;
    if (!cap) { return XR_SUCCESS; }
    if (cap < 4 || !r) { return XR_ERROR_SIZE_INSUFFICIENT; }
    for (k = 0; k < 4; k++) { r[k] = kMockRates[k]; }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_GetDisplayRefreshRateFB(XrSession s, float *r)
{
    if (s != m_sess || !r || m_norr) { return XR_ERROR_HANDLE_INVALID; }
    *r = (float)m_hz;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_RequestDisplayRefreshRateFB(XrSession s, float r)
{
    int k, ok = 0;
    if (s != m_sess || m_norr) { return XR_ERROR_HANDLE_INVALID; }
    for (k = 0; k < 4; k++) { if (fabsf(kMockRates[k] - r) < 0.5f) { ok = 1; } }
    if (!ok) { return XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB; }
    m_rr_from = (float)m_hz;
    m_rr_to = r;
    m_hz = (int)(r + 0.5f);
    m_rr_ev = 1;
    mlog("MOCK Bildrate %.0f -> %.0f Hz (xrRequestDisplayRefreshRateFB)", (double)m_rr_from, (double)r);
    return XR_SUCCESS;
}

/* ---------- xrGetInstanceProcAddr / Aushandlung ---------- */

typedef struct { const char *n; PFN_xrVoidFunction f; } mock_fn_t;
#define MF(name) { "xr" #name, (PFN_xrVoidFunction)m_##name }
static const mock_fn_t kFns[] = {
    MF(EnumerateInstanceExtensionProperties), MF(CreateInstance), MF(DestroyInstance), MF(GetInstanceProperties),
    MF(PollEvent), MF(GetSystem), MF(GetSystemProperties), MF(EnumerateViewConfigurationViews),
    MF(GetD3D11GraphicsRequirementsKHR), MF(CreateSession), MF(DestroySession), MF(BeginSession), MF(EndSession),
    MF(CreateReferenceSpace), MF(DestroySpace), MF(EnumerateSwapchainFormats), MF(CreateSwapchain),
    MF(DestroySwapchain), MF(EnumerateSwapchainImages), MF(AcquireSwapchainImage), MF(WaitSwapchainImage),
    MF(ReleaseSwapchainImage), MF(WaitFrame), MF(BeginFrame), MF(EndFrame), MF(LocateViews),
    MF(EnumerateDisplayRefreshRatesFB), MF(GetDisplayRefreshRateFB), MF(RequestDisplayRefreshRateFB), /* W183 */
};

static XrResult XRAPI_CALL m_GetInstanceProcAddr(XrInstance inst, const char *name, PFN_xrVoidFunction *fn)
{
    size_t k;
    if (!name || !fn) { return XR_ERROR_VALIDATION_FAILURE; }
    *fn = NULL;
    if (strcmp(name, "xrGetInstanceProcAddr") == 0) { *fn = (PFN_xrVoidFunction)m_GetInstanceProcAddr; return XR_SUCCESS; }
    /* ohne Instanz nur die instanzfreien Funktionen (Spezifikation) */
    if (inst == XR_NULL_HANDLE && strcmp(name, "xrCreateInstance") != 0 &&
        strcmp(name, "xrEnumerateInstanceExtensionProperties") != 0 && strcmp(name, "xrEnumerateApiLayerProperties") != 0) {
        return XR_ERROR_HANDLE_INVALID;
    }
    for (k = 0; k < sizeof kFns / sizeof kFns[0]; k++) {
        if (strcmp(kFns[k].n, name) == 0) { *fn = kFns[k].f; return XR_SUCCESS; }
    }
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo *li, XrNegotiateRuntimeRequest *rr)
{
    if (!li || !rr || li->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO || li->structSize != sizeof *li ||
        rr->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST || rr->structSize != sizeof *rr ||
        li->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION || li->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION) {
        mlog("MOCK Aushandlung abgelehnt");
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    rr->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    rr->runtimeApiVersion = XR_MAKE_VERSION(1, 0, 34);
    rr->getInstanceProcAddr = m_GetInstanceProcAddr;
    mlog("MOCK Aushandlung ok (Schnittstelle 1, API 1.0)");
    return XR_SUCCESS;
}
