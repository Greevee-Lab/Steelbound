/* d3d11_render.c - Echter Present-Pfad (M2a).
 * Fenster + D3D11-Device + Swapchain in cfg-Groesse, Clear mit Spiel-Farbe,
 * Kamera/Projektion getrennt erfasst, Hor+-Aspectkorrektur, IM-Statistik.
 * W68 SPLIT: hier Fenster/UI-Thread, Device, Context-Lock, Clear/Viewport/
 * Kamera, Present, Aspect-Balken, Screenshot; Draw-Aufzeichnung/Submit in
 * render_im.c, Pipeline/VSH in render_shader.c, States in render_state.c,
 * RTT in render_rtt.c, Zensus in render_diag.c (gemeinsam: render_int.h).
 */
#include "render_int.h"
#include "render_fx_rem.h" /* W133 REM-FX */
#include "sb_remaster.h" /* W103 REM-FRAME */
#include "render_qa.h"
#include "render_clip.h"
#include "sb_overlay.h" /* W118 REM-OVERLAY */
#include "sb_perfhud.h" /* W122 PERF-HUD */
#include "hlock.h" /* W119 LOADLOCK */
#include <intrin.h> /* W138 DIAG pd_diag: _ReturnAddress */

int imouse_subclassed(void); /* input_mouse.c (W118) */

static HWND g_hwnd;
ID3D11Device *g_dev;
ID3D11DeviceContext *g_ctx;
unsigned g_im_vb_pos;      /* IMRING: naechster freier Vertex */
unsigned g_im_frame_draws; /* DRAWSTORM: Draws seit Present */
static IDXGISwapChain *g_swap;
static ID3D11Resource *g_swap_res; /* FPS-INTERP (W69): Swapchain-BB, wenn das Spiel offscreen zeichnet */
static ID3D11RenderTargetView *g_swap_rtv; /* W107 EDGE-MODE: RTV auf g_swap_res (Maske nach der Kopie), bei Bedarf */
ID3D11RenderTargetView *g_rtv;
ID3D11DepthStencilView *g_dsv;
ID3D11ShaderResourceView *g_bb_dsrv; /* W131 SHADER-OPT2: BB-Tiefe als SRV (nur ssao/bb_depth_srv) */
ID3D11RasterizerState *g_rs_norm; /* SIDESMEAR: Standard-RS (Restore) */
ID3D11RasterizerState *g_rs_scis; /* SIDESMEAR: RS + Scissor (Box-Clip) */
unsigned g_bb_w, g_bb_h;
static int g_open;

/* Kamera-Zustand: Spiel liefert View + Projektion getrennt (SetTransform),
 * Viewport in Spiel-Pixeln (SetViewport). Renderer leitet daraus pro Auge
 * aspect-korrigierte Projektion + echten Pixel-Viewport ab. */
sb_mat4 g_view, g_proj_game, g_world[4];
sb_viewport g_vp_game = { 0, 0, 640, 480, 0.0f, 1.0f };
sb_eye g_rig[2];
static float g_last_game_aspect, g_last_out_aspect;

#include "vr.h" /* W134 VR-PROTO */
#include "sb_relaunch.h" /* W186 RESET-TITLE */
#include "sb_pause.h" /* W188 GAME-MENU */
#include "sb_photo.h" /* W195 PHOTO-MODE */
#include "sb_watermark.h" /* W219 WATERMARK */
int g_vr_blit; /* W134 VR-PROTO: sb_render_blit zeigt ein Augenbild (ohne Nachbearbeitung des Monobilds) */

static LRESULT CALLBACK sb_wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT wmres = 0;
    if (sb_winmode_wndproc(w, msg, wp, lp, &wmres)) { /* W81 WINMODE: Alt+Enter */
        return wmres;
    }
    /* W118 REM-OVERLAY: ESC/Overlay-Tasten (ohne W79-Subclass; mit Subclass
     * fragt input_mouse.c zuerst) */
    if (!imouse_subclassed() && sb_phud_wndproc(w, msg, wp, lp)) { /* W122 PERF-HUD: F2 */
        return 0;
    }
    if (g_cfg.vr_enable && !imouse_subclassed() && sb_vr_wndproc(msg, (unsigned)wp, (long)lp)) { /* W134 VR-PROTO */
        return 0;
    }
    if (g_cfg.overlay && !imouse_subclassed() && sb_ovl_wndproc(w, msg, wp, lp)) {
        return 0;
    }
    switch (msg) {
    case WM_CLOSE:
        sb_winmode_on_close(); /* W81: Exklusiv-Vollbild verlassen */
        HLOG_I("render: Fenster geschlossen -> Exit(0)");
        hlog_close();
        ExitProcess(0);
        return 0;
    case WM_DESTROY:
        g_open = 0;
        return 0;
    default:
        return DefWindowProcW(w, msg, wp, lp);
    }
}

static void sb_create_views(void)
{
    ID3D11Texture2D *bb = NULL, *ds = NULL;
    D3D11_TEXTURE2D_DESC dd;
    HRESULT hr = IDXGISwapChain_GetBuffer(g_swap, 0, &IID_ID3D11Texture2D,
                                          (void **)&bb);
    if (FAILED(hr) || !bb) {
        HLOG_E("render: GetBuffer fehlgeschlagen (hr=0x%lX)", (long)hr);
        ExitProcess(7);
    }
    if (g_cfg.interp_fps > 0) {
        /* FPS-INTERP (W69): Spiel zeichnet in eine Offscreen-Textur gleicher
         * Art; gezeigt wird per Kopie (Spiel-Bild oder Zwischenbild). So kann
         * der Interp-Takt praesentieren, waehrend das Spiel den naechsten
         * Tick zeichnet. Ohne interp_fps: exakt der bisherige Pfad. */
        ID3D11Texture2D *gbb = NULL;
        D3D11_TEXTURE2D_DESC bd;
        ID3D11Texture2D_GetDesc(bb, &bd);
        bd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.CPUAccessFlags = 0;
        bd.MiscFlags = 0;
        hr = ID3D11Device_CreateTexture2D(g_dev, &bd, NULL, &gbb);
        if (SUCCEEDED(hr) && gbb) {
            g_swap_res = (ID3D11Resource *)bb; /* behaelt die GetBuffer-Referenz */
            bb = gbb;
            HLOG_I("render: FPS-INTERP Offscreen-Backbuffer %ux%u", bd.Width, bd.Height);
        }
    }
    hr = ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)bb,
                                             NULL, &g_rtv);
    ID3D11Texture2D_Release(bb);
    if (FAILED(hr)) {
        HLOG_E("render: RTV fehlgeschlagen (hr=0x%lX)", (long)hr);
        ExitProcess(7);
    }
    memset(&dd, 0, sizeof dd);
    dd.Width = g_bb_w;
    dd.Height = g_bb_h;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    /* W131 SHADER-OPT2 (ssao): Tiefe typlos mit SRV (DSV-Format unveraendert
     * D24S8 -> Tiefentest/-werte bitgleich); nur mit ssao>0 bzw. bb_depth_srv=1. */
    if (g_cfg.ssao > 0 || g_cfg.bb_depth_srv) {
        dd.Format = DXGI_FORMAT_R24G8_TYPELESS;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    }
    hr = ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &ds);
    if (FAILED(hr)) {
        HLOG_E("render: DS-Textur fehlgeschlagen (hr=0x%lX)", (long)hr);
        ExitProcess(7);
    }
    if (g_bb_dsrv) { ID3D11ShaderResourceView_Release(g_bb_dsrv); g_bb_dsrv = NULL; }
    if (dd.Format == DXGI_FORMAT_R24G8_TYPELESS) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dvd;
        D3D11_SHADER_RESOURCE_VIEW_DESC svd;
        memset(&dvd, 0, sizeof dvd);
        dvd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        hr = ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource *)ds, &dvd, &g_dsv);
        memset(&svd, 0, sizeof svd);
        svd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        svd.Texture2D.MipLevels = 1;
        if (SUCCEEDED(hr) &&
            FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)ds, &svd, &g_bb_dsrv))) {
            g_bb_dsrv = NULL;
        }
        HLOG_I("render: BB-Tiefe typlos mit SRV (W131 ssao) %ux%u srv=%d", g_bb_w, g_bb_h, g_bb_dsrv != NULL);
    } else {
        hr = ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource *)ds,
                                                 NULL, &g_dsv);
    }
    ID3D11Texture2D_Release(ds);
    if (FAILED(hr)) {
        HLOG_E("render: DSV fehlgeschlagen (hr=0x%lX)", (long)hr);
        ExitProcess(7);
    }
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, g_dsv);
}

/* A6 CTXTHREAD/ctx_lock: Der D3D11-Immediate-Context (g_ctx) ist NICHT
 * threadsicher (nur ID3D11Device_* ist es). Alle Context-Aufrufe laufen
 * ueber die sb_render_*-Eintritte (clear/blend/im_end/vb_draw/Present-
 * Screenshot); jeder nimmt sb_ctx_enter/leave. DIAG(CTXTHREAD):
 * g_ctx_foreign_n zaehlt Eintritte von anderen Threads als dem
 * Render-Thread (Tid des ersten Presents); pro fremder Tid eine Zeile,
 * sonst still. ctx_lock=1 serialisiert per Critical-Section (reentrant). */
static DWORD g_render_tid;
static hlock_t g_ctx_cs; /* W119 LOADLOCK: hlock (atomarer Owner, reentrant) */
static int g_ctx_cs_init;
static volatile LONG g_ctx_foreign_n;
static DWORD g_ctx_foreign_tids[8];
static int g_ctx_foreign_ntids;

static void sb_ctx_note_foreign(DWORD tid)
{
    int i;
    for (i = 0; i < g_ctx_foreign_ntids && i < 8; i++) {
        if (g_ctx_foreign_tids[i] == tid) {
            return;
        }
    }
    if (g_ctx_foreign_ntids < 8) {
        g_ctx_foreign_tids[g_ctx_foreign_ntids++] = tid;
        HLOG_W("DIAG(CTXTHREAD): Context-Eintritt von Fremd-Thread T%04x "
               "(Render-T%04x, n=%ld)", tid, g_render_tid,
               (long)g_ctx_foreign_n);
    }
}

/* PERF(PERF-FIX,W138) present_decouple: Der Present-Takt-Thread meldet vor
 * seinem Erwerb g_pd_want. Andere Threads lassen ihm an ihrer naechsten
 * aeussersten Erwerbsstelle den Vortritt (hlock ist nicht fair: der
 * Spiel-Thread nimmt die Sperre je Draw neu und wuerde den Takt sonst
 * zufaellig verzoegern). Der Takt-Thread haelt ausser ctx keine Sperre und
 * wartet auf niemanden -> kein Zyklus; das Warten hier haelt keine ctx. */
volatile LONG g_pd_want;
volatile DWORD g_pd_tid;
LONGLONG g_pd_last_q; /* letztes echtes Present (QPC, unter ctx_lock geschrieben) */
int g_phud_src;       /* Quelle des laufenden Presents fuer perf_trace: 0 Spiel-Thread, 1 Takt-Thread, 2 Wiederholung */

static void sb_ctx_pd_yield_wait(void)
{
    int k = 0;
    while (g_pd_want) {
        if (++k < 64) { YieldProcessor(); }
        else { SwitchToThread(); }
    }
}

/* DIAG(PERF-FIX,W138) pd_diag=1: aeussere ctx_lock-Regionen anderer Threads
 * > pd_diag_ms (Standard 4) mit Eintritts-/Austritts-Ruecksprungadresse
 * (sb_emu.map) -> wer den Present-Takt sperrt. */
extern unsigned g_present_no; /* weiter unten definiert */
static LONGLONG g_ctxd_q0;
static void *g_ctxd_ret;

/* DIAG(PERF-TICK,W168) ctx_wait_diag=1: Wartezeit des Spiel-Threads auf ctx_lock (Vortritt +
 * Erwerb, QPC-Ticks) - Zeile CTXWAIT im Present-Takt-Thread alle 10 s. */
volatile LONGLONG g_ctxw_q, g_ctxw_tick_q, g_ctxw_tick_max;
volatile LONG g_ctxw_n;
__declspec(noinline) void sb_ctx_enter(void)
{
    DWORD tid;
    if (!g_ctx_cs_init) {
        return; /* vor sb_render_init: kein Device, keine Context-Aufrufe */
    }
    tid = GetCurrentThreadId();
    if (g_render_tid != 0 && tid != g_render_tid) {
        InterlockedIncrement(&g_ctx_foreign_n);
        sb_ctx_note_foreign(tid);
    }
    if (g_cfg.ctx_lock) {
        int outer = hlock_owner(&g_ctx_cs) != tid;
        LARGE_INTEGER wq0 = {0};
        int wd = g_cfg.ctx_wait_diag && outer && tid == g_render_tid &&
                 (g_pd_want || hlock_owner(&g_ctx_cs) != 0); /* DIAG(PERF-TICK,W168): nur wenn belegt/gewuenscht */
        if (wd) { QueryPerformanceCounter(&wq0); }
        if (g_pd_want && tid != g_pd_tid && outer) {
            sb_ctx_pd_yield_wait(); /* W138: Present-Takt zuerst */
        }
        hlock_enter(&g_ctx_cs);
        if (wd) {
            LARGE_INTEGER wq1;
            QueryPerformanceCounter(&wq1);
            g_ctxw_q += wq1.QuadPart - wq0.QuadPart;
            g_ctxw_tick_q += wq1.QuadPart - wq0.QuadPart;
            g_ctxw_n++;
        }
        if (outer && g_cfg.pd_diag && tid != g_pd_tid) { /* DIAG(PERF-FIX,W138) */
            LARGE_INTEGER q;
            QueryPerformanceCounter(&q);
            g_ctxd_q0 = q.QuadPart;
            g_ctxd_ret = _ReturnAddress();
        }
    }
}

/* W138 present_decouple: ctx_lock fuer reine Device-Arbeit (frei-threaded:
 * Textur-/Shader-/Puffer-Erstellung, Datei-Lesen) ganz abgeben, damit der
 * Present-Takt nicht auf das Anlegen beim Missionsstart wartet (pd_diag:
 * Regionen 5-25 ms in sb_render_im_end/vb_draw/sb_interp_swap). Der
 * Takt-Thread aendert keinen Spielzustand am Context: Wiedergaben sichern
 * und setzen den Pipeline-Zustand zurueck (ip_state_save/-restore bzw.
 * ExecuteCommandList(..., TRUE)), wie bei den Zwischenbildern aus rec_draw.
 * Rueckgabe = gesicherte Rekursionstiefe (0 = nichts abgegeben). Ohne
 * Takt-Thread wirkungslos (alter Pfad unveraendert). */
int sb_ctx_release_all(void)
{
    DWORD tid;
    LONG r;
    if (!g_ctx_cs_init || !g_cfg.ctx_lock || !g_pd_tid || !sb_pd_on()) { return 0; }
    tid = GetCurrentThreadId();
    if (tid == g_pd_tid || hlock_owner(&g_ctx_cs) != tid) { return 0; }
    r = g_ctx_cs.recursion;
    g_ctx_cs.recursion = 1;
    sb_ctx_leave();
    return (int)r;
}

void sb_ctx_reacquire(int r)
{
    if (r <= 0) { return; }
    sb_ctx_enter();
    g_ctx_cs.recursion = r;
}

/* W138 present_decouple: in langen Warteschleifen unter ctx_lock (Sonnentest
 * blockierend) die Sperre kurz abgeben, wenn der Present-Takt sie will.
 * Nur auf aeusserster Ebene (recursion 1), sonst wirkungslos. */
void sb_ctx_yield(void)
{
    DWORD tid;
    if (!g_pd_want || !g_ctx_cs_init || !g_cfg.ctx_lock) { return; }
    tid = GetCurrentThreadId();
    if (hlock_owner(&g_ctx_cs) != tid || tid == g_pd_tid || g_ctx_cs.recursion != 1) { return; }
    hlock_leave(&g_ctx_cs);
    sb_ctx_pd_yield_wait();
    hlock_enter(&g_ctx_cs);
}

/* SUSPSAFE (W10): Owner-Tid von g_ctx_cs (0 = frei/uninit). Regionen sind
 * kurz (einzelne Context-Aufrufe); NtSuspendThread suspendiert kein Ziel,
 * das eine Host-Sperre haelt. */
unsigned long sb_ctx_owner_tid(void)
{
    if (!g_ctx_cs_init || !g_cfg.ctx_lock) {
        return 0;
    }
    return (unsigned long)hlock_owner(&g_ctx_cs);
}

/* W119 DIAG(LOADLOCK): voller Sperr-Zustand fuer krnl_lock_diag. */
const hlock_t *sb_ctx_hlock(void)
{
    return &g_ctx_cs;
}

__declspec(noinline) void sb_ctx_leave(void)
{
    if (g_ctx_cs_init && g_cfg.ctx_lock) {
        if (g_cfg.pd_diag && g_ctxd_q0 && g_ctx_cs.recursion == 1 &&
            hlock_owner(&g_ctx_cs) == GetCurrentThreadId()) { /* DIAG(PERF-FIX,W138): aeussere Region endet */
            static LARGE_INTEGER f;
            static unsigned n;
            LARGE_INTEGER q;
            double ms;
            if (!f.QuadPart) { QueryPerformanceFrequency(&f); }
            QueryPerformanceCounter(&q);
            ms = (double)(q.QuadPart - g_ctxd_q0) * 1000.0 / (double)f.QuadPart;
            if (ms > (g_cfg.pd_diag_ms > 0 ? (double)g_cfg.pd_diag_ms : 4.0) && n < 300u) {
                n++;
                HLOG_I("PRESENT-TAKT sperre lang: %.2fms T%04lx ein=%p aus=%p f=%u", ms, GetCurrentThreadId(),
                       g_ctxd_ret, _ReturnAddress(), g_present_no);
            }
            g_ctxd_q0 = 0;
        }
        hlock_leave(&g_ctx_cs);
    }
}

/* ASPECT (W18): Vorab-Deklaration (Definition unten bei sb_im_submit). */
static void sb_aspect_bars(void);

/* A6 ui_thread: eigener Fenster-Thread. Er erzeugt das Fenster (Besitz)
 * und pumpt es per GetMessage-Loop (D3D-Device/Swapchain bleiben auf
 * Loader-/Spiel-Threads nutzbar: nur CreateWindow braucht den Besitzer).
 * NICHT auf Xbox-Spiel-Threads verlegen (dort hing CreateWindowExA). */
static HANDLE g_ui_ready; /* gesetzt, wenn das Fenster steht (oder scheiterte) */
static DWORD g_ui_tid;

static DWORD WINAPI sb_ui_thread(LPVOID unused)
{
    HINSTANCE hinst;
    MSG msg;
    (void)unused;
    /* Klasse registriert sb_render_init (Loader) vor dem Spawn. */
    hinst = GetModuleHandleA("sb_emu.dll");
    { /* Zwilling des Erzeugungsblocks in sb_render_init (alter Pfad). */
        RECT rc;
        DWORD ex = 0;
        int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
        rc.left = 0;
        rc.top = 0;
        rc.right = (LONG)g_bb_w;
        rc.bottom = (LONG)g_bb_h;
        sb_winmode_ui_dpi(); /* W81: DPI-Art des Fensters (nur Vollbild-Start, sonst wie bisher) */
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        if (g_cfg.window_hidden) {
            ex = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
            x = GetSystemMetrics(SM_XVIRTUALSCREEN) - (rc.right - rc.left) - 200;
            y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        } else {
            sb_relaunch_winpos(&x, &y); /* W186: Neustart-Kind an der Stelle des alten Fensters */
        }
        g_hwnd = CreateWindowExW(ex, L"SB_NATIVE", L"Steel Battalion - Game Preservation Project",
                                 WS_OVERLAPPEDWINDOW, x, y,
                                 rc.right - rc.left, rc.bottom - rc.top,
                                 NULL, NULL, hinst, NULL);
        if (g_hwnd) {
            ShowWindow(g_hwnd, g_cfg.window_hidden ? SW_SHOWNOACTIVATE : SW_SHOW);
            UpdateWindow(g_hwnd);
            HLOG_I("render: UI-Thread T%04x erzeugt Fenster=0x%p (hidden=%d)",
                   GetCurrentThreadId(), g_hwnd, g_cfg.window_hidden);
        }
    }
    SetEvent(g_ui_ready); /* NACH dem Log (hlog-Reopen des Loaders folgt). */
    if (!g_hwnd) {
        return 1;
    }
    for (;;) {
        BOOL r = GetMessageW(&msg, NULL, 0, 0);
        if (r <= 0) { /* WM_QUIT (0) oder Fehler (-1) */
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    hlog_close();
    ExitProcess(0);
    return 0;
}

void sb_render_init(void)
{
    WNDCLASSW wc;
    DXGI_SWAP_CHAIN_DESC sd;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0,
                                   D3D_FEATURE_LEVEL_10_1,
                                   D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got = 0;
    RECT rc;
    HRESULT hr;
    if (g_dev) {
        return;
    }
    g_bb_w = g_cfg.width ? g_cfg.width : 640;
    g_bb_h = g_cfg.height ? g_cfg.height : 480;
    /* 1-ms-Timergranularitaet, sonst taktet Sleep() nur 64 FPS (15,6 ms). */
    timeBeginPeriod(1);
    /* A6 ctx_lock: Context-Schloss VOR allen Spiel-Threads anlegen. */
    if (!g_ctx_cs_init) {
        g_ctx_cs_init = 1; /* W119: hlock braucht keine Init */
    }

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = sb_wndproc;
    /* NICHT GetModuleHandleA(NULL): sb_ldr liegt auf 0x10000 im Header-Swap
     * mit dem XBE — user32 wuerde dort Muell lesen. DLL-Handle ist sauber. */
    wc.hInstance = GetModuleHandleA("sb_emu.dll");
    wc.lpszClassName = L"SB_NATIVE";
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    /* A6 ui_thread: Fenster gehoert dem UI-Thread (erzeugt+pumpt dort).
     * Alter Pfad (0): Loader erzeugt, Present + Pump-Thread pumpen. */
    if (g_cfg.ui_thread) {
        HANDLE hui;
        DWORD owner;
        DWORD_PTR smres = 0;
        LRESULT smok;
        g_ui_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!g_ui_ready) {
            HLOG_E("render: UI-Event fehlgeschlagen (GLE=%lu)", GetLastError());
            ExitProcess(7);
        }
        hui = CreateThread(NULL, 0, sb_ui_thread, NULL, 0, &g_ui_tid);
        if (!hui) {
            HLOG_E("render: UI-Thread fehlgeschlagen (GLE=%lu)", GetLastError());
            ExitProcess(7);
        }
        CloseHandle(hui);
        WaitForSingleObject(g_ui_ready, INFINITE);
        if (!g_hwnd) {
            HLOG_E("render: CreateWindow (UI-Thread) fehlgeschlagen");
            ExitProcess(7);
        }
        /* Pump-Beweis: WM_NULL wird erst zugestellt, wenn die UI-Loop laeuft. */
        owner = GetWindowThreadProcessId(g_hwnd, NULL);
        smok = SendMessageTimeoutA(g_hwnd, WM_NULL, 0, 0, SMTO_BLOCK, 5000,
                                   &smres);
        HLOG_I("render: UI-Fenster=0x%p owner-tid=0x%x ui-tid=0x%x pump=%s",
               g_hwnd, owner, g_ui_tid, smok ? "ok" : "TIMEOUT");
    } else {
    rc.left = 0;
    rc.top = 0;
    rc.right = (LONG)g_bb_w;
    rc.bottom = (LONG)g_bb_h;
    sb_winmode_ui_dpi(); /* W81 (alter Pfad: Loader-Thread besitzt das Fenster) */
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    {
        /* HIDDENWIN (31.10.): Testlaeufe stoeren nicht - Fenster links
         * neben allen Monitoren, ohne Taskleisten-Eintrag, ohne Fokus.
         * Fuer Windows bleibt es sichtbar -> Rendern/Present/Screenshots
         * (aus dem Render-Target) laufen unveraendert. */
        DWORD ex = 0;
        int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
        if (g_cfg.window_hidden) {
            ex = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
            x = GetSystemMetrics(SM_XVIRTUALSCREEN) - (rc.right - rc.left) - 200;
            y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        } else {
            sb_relaunch_winpos(&x, &y); /* W186: Neustart-Kind an der Stelle des alten Fensters */
        }
        g_hwnd = CreateWindowExW(ex, L"SB_NATIVE", L"Steel Battalion - Game Preservation Project",
                                 WS_OVERLAPPEDWINDOW, x, y,
                                 rc.right - rc.left, rc.bottom - rc.top,
                                 NULL, NULL, wc.hInstance, NULL);
    }
    } /* else ui_thread: Fenster steht (alter Block oben unveraendert) */
    if (!g_hwnd) {
        HLOG_E("render: CreateWindow fehlgeschlagen (GLE=%lu)", GetLastError());
        ExitProcess(7);
    }
    memset(&sd, 0, sizeof sd);
    sd.BufferCount = 1; /* M2b: SEQUENTIAL (RTV bleibt gueltig) */
    sd.BufferDesc.Width = g_bb_w;
    sd.BufferDesc.Height = g_bb_h;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 120;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;
    /* W81: Moduswechsel nur erlauben, wenn exklusiv erreichbar ist (sonst 0 = bisher). */
    g_swap_flags = (g_cfg.alt_enter && (g_cfg.window_mode == 2 ||
                    (g_cfg.window_mode == 0 && g_cfg.alt_enter_mode == 2)))
                       ? DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH : 0u;
    sd.Flags = g_swap_flags;
    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                       levels, 3, D3D11_SDK_VERSION, &sd,
                                       &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) {
        HLOG_E("render: D3D11CreateDeviceAndSwapChain (hr=0x%lX)", (long)hr);
        ExitProcess(7);
    }
    sb_create_views();
    sb_winmode_init(g_hwnd, g_swap); /* W81 WINMODE (Wechsel erst am ersten Present) */
    if (g_cfg.vr_enable) { sb_vr_init(); } /* W134 VR-PROTO (Standard aus) */
    /* A6: UI-Thread zeigt sein Fenster selbst (nur alter Pfad hier). */
    if (!g_cfg.ui_thread) { ShowWindow(g_hwnd, g_cfg.window_hidden ? SW_SHOWNOACTIVATE : SW_SHOW); }
    if (!g_cfg.ui_thread) { UpdateWindow(g_hwnd); }
    /* M2c: Eigener Pump-Thread (Fenster bleibt bedienbar, auch wenn das */
    /* Spiel nicht presentet; interaktiver NUDE-MAKER-Hang). */
    {
        extern DWORD WINAPI sb_pump_thread(LPVOID);
        /* A6: Pump-Thread nur im alten Pfad (ui_thread=0). */
        HANDLE hp = g_cfg.ui_thread ? NULL : CreateThread(NULL, 0, sb_pump_thread, NULL, 0, NULL);
        if (hp) { CloseHandle(hp); }
    }
    g_open = 1;
    HLOG_I("render: D3D11 bereit (%ux%u, FL=0x%X, Fenster=0x%p)", g_bb_w, g_bb_h,
           (unsigned)got, g_hwnd);
    sb_warm_start(); /* W139 PERF-WARM: warm_shaders (Standard 0) */
    sb_warm_gpu_start(); /* W139 PERF-WARM: warm_gpu */
}

DWORD WINAPI sb_pump_thread(LPVOID unused)
{
    MSG msg;
    (void)unused;
    for (;;) {
        while (!g_cfg.ui_thread && PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { hlog_close(); ExitProcess(0); }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(33);
    }
}

void sb_render_shutdown(void)
{
    if (g_rtv) {
        ID3D11RenderTargetView_Release(g_rtv);
        g_rtv = NULL;
    }
    if (g_dsv) {
        ID3D11DepthStencilView_Release(g_dsv);
        g_dsv = NULL;
    }
    if (g_swap_rtv) { /* W107 */
        ID3D11RenderTargetView_Release(g_swap_rtv);
        g_swap_rtv = NULL;
    }
    if (g_swap_res) {
        ID3D11Resource_Release(g_swap_res);
        g_swap_res = NULL;
    }
    if (g_swap) {
        IDXGISwapChain_Release(g_swap);
        g_swap = NULL;
    }
    if (g_ctx) {
        ID3D11DeviceContext_Release(g_ctx);
        g_ctx = NULL;
    }
    if (g_dev) {
        ID3D11Device_Release(g_dev);
        g_dev = NULL;
    }
    if (g_hwnd) {
        /* A6: UI-Fenster gehoert dem UI-Thread -> per Message schliessen. */
        if (g_cfg.ui_thread) { PostMessageA(g_hwnd, WM_CLOSE, 0, 0); }
        else { DestroyWindow(g_hwnd); }
        g_hwnd = NULL;
    }
    g_open = 0;
}

int sb_render_is_open(void)
{
    return g_open && g_dev != NULL;
}

/* Hor+-Aspectkorrektur: vertikales Sichtfeld bleibt (kein Stretch, kein
 * Crop), horizontal oeffnet sich das Bild bei 21:9/32:9. Dazu wird nur
 * proj[0] (X-Skalierung) mit SpielAspect/ZielAspect skaliert — die
 * Spiel-Projektion selbst bleibt unangetastet gespeichert (VR-Mods und
 * Debug koennen jederzeit beide einsehen). */
static void sb_update_rig(void)
{
    float game_aspect = 640.0f / 480.0f, out_aspect, k;
    unsigned i;
    if (g_vp_game.w && g_vp_game.h) {
        game_aspect = (float)g_vp_game.w / (float)g_vp_game.h;
    }
    out_aspect = (float)g_bb_w / (float)(g_bb_h ? g_bb_h : 1);
    k = game_aspect / out_aspect;
    /* Auge 0 = Vollbild (Mono). Auge 1 reserviert (Stereo/VR-Mod). */
    for (i = 0; i < 2; i++) {
        memcpy(g_rig[i].view.m, g_view.m, sizeof g_view.m);
        memcpy(g_rig[i].proj.m, g_proj_game.m, sizeof g_proj_game.m);
        g_rig[i].proj.m[0] *= k;
        g_rig[i].vp.x = 0;
        g_rig[i].vp.y = 0;
        g_rig[i].vp.w = g_bb_w;
        g_rig[i].vp.h = g_bb_h;
        g_rig[i].vp.minz = g_vp_game.minz;
        g_rig[i].vp.maxz = g_vp_game.maxz;
    }
    if (game_aspect != g_last_game_aspect || out_aspect != g_last_out_aspect) {
        static unsigned nchg;
        g_last_game_aspect = game_aspect;
        g_last_out_aspect = out_aspect;
        nchg++;
        /* DIAGNOSE: Mission wechselt Viewport pro Objekt (1.0/1.33) ->
         * nur jedes 512. loggen (Spam), Rest still uebernehmen. */
        if (nchg <= 4 || (nchg % 512) == 0) {
            HLOG_I("render: aspect Spiel=%.3f Ziel=%.3f k=%.3f (Hor+, #%u)",
                   game_aspect, out_aspect, k, nchg);
        }
    }
}

void sb_render_on_create_device(unsigned want_w, unsigned want_h)
{
    sb_render_init();
    /* Spiel wuenscht 640x480 — Backbuffer bleibt cfg-Groesse (Hor+ statt
     * Stretch). Echte Resize-Anforderung (Menue-Option o.ae.) spaeter (M2b). */
    (void)want_w;
    (void)want_h;
    sb_update_rig();
}

/* W177 CLEAR-FLAGS: Spiel-Flags (Xbox D3DDevice_Clear) -> interne D3D11-Bits.
 * clear_xbflags=1: Xbox-Semantik (XBX_CLEAR_*, render_int.h), 0 = alt
 * (Spiel-Flags direkt als PC-Bits: 0xF0 loescht nichts, 0x3 Farbe+Z). */
unsigned sb_clear_eff(unsigned xflags)
{
    if (!g_cfg.clear_xbflags) { return xflags; }
    return ((xflags & XBX_CLEAR_TARGET) ? XB_CLEAR_TARGET : 0u)
         | ((xflags & XBX_CLEAR_ZBUFFER) ? XB_CLEAR_ZBUFFER : 0u)
         | ((xflags & XBX_CLEAR_STENCIL) ? XB_CLEAR_STENCIL : 0u);
}

unsigned g_last_clear, g_last_clear_flags; /* DIAG(MOVIE): letztes Clear */
int g_submit_vb; /* DIAG(MOVIE): 1 = Submit via sb_render_vb_draw */

void sb_render_clear(unsigned flags, unsigned color, float z, unsigned stencil)
{
    float c[4];
    unsigned long ds = 0;
    if (!g_dev) {
        return;
    }
    /* Xbox-Farbe ist ARGB (A@24 R@16 G@8 B@0). */
    c[0] = (float)((color >> 16) & 0xFF) / 255.0f;
    c[1] = (float)((color >> 8) & 0xFF) / 255.0f;
    c[2] = (float)(color & 0xFF) / 255.0f;
    c[3] = (float)((color >> 24) & 0xFF) / 255.0f;
    g_last_clear = color; g_last_clear_flags = flags; /* DIAG(MOVIE) */
    if (g_cfg.fx_diag) { /* DIAG(FRONT-HANGAR,W101): Clear-Flags je Ziel (je Kombination einmal) */
        static unsigned ck[64], cn;
        unsigned key = (flags & 0xFFu) | ((unsigned)(rtt_active() ? g_rtt_bound + 1 : 0) << 8), q;
        for (q = 0; q < cn; q++) { if (ck[q] == key) { break; } }
        if (q == cn && cn < 64u) {
            ck[cn++] = key;
            HLOG_W("CLRDIAG f=%u flags=0x%X color=0x%08X z=%.3f st=%u tgt=%s%d",
                   g_present_no, flags, color, z, stencil, rtt_active() ? "RT" : "BB",
                   rtt_active() ? g_rtt_bound : 0);
        }
    }
    rtt_note_clear(flags, color); /* RTT-Session (beide Modi) */
    if (g_cfg.bloom_diag) { bloom_diag_clear(flags, color); } /* DIAG(SHADE-BLOOM,W112) */
    if (g_cfg.dlg_text_upload && rtt_active()) { rq_text_on_clear(g_rtt[g_rtt_bound].surf, flags, color); } /* FIX(DLGTEXT2,W97) */
    /* FIX(CLEAR-XB,W101/W177): Xbox-Clear-Flags (TARGET = 0xF0 je Kanal
     * R/G/B/A 0x10..0x80, ZBUFFER = 0x1, STENCIL = 0x2; Beleg W177: eigene
     * Disassembly D3DDevice_Clear 0x20DF80, s. render_int.h) statt PC-Werten
     * (1/2/4) fuer die D3D11-Operationen. Das Spiel nutzt 0xF3 (alles), 0xF0
     * (nur Farbe: alt = nichts) und 0x3 (Z+Stencil: alt = Farbe+Z) - W101_c1
     * CLRDIAG. Die Textebene (rq_text_on_clear) erfaehrt ueber sb_clear_eff,
     * ob ihr D3D11-Inhalt geloescht wird (W177: sonst fehlte der Hilfetext). */
    if (g_cfg.clear_xbflags) {
        unsigned ch = flags & XBX_CLEAR_TARGET;
        if (ch && ch != XBX_CLEAR_TARGET) { /* Teilmaske (einzelne Farbkanaele): D3D11-Clear kann das nicht */
            static unsigned npart;
            if (npart++ < 4u) { HLOG_W("CLEAR Teilmaske flags=0x%X (nur einige Farbkanaele) -> ganzes Ziel", flags); }
        }
        flags = sb_clear_eff(flags);
    }
    if (flags & XB_CLEAR_TARGET) {
        sb_ctx_enter(); /* A6 ctx_lock */
        if (g_ip_recording) { WD_CALL("rec_clear", NULL, sb_interp_rec_clear(rtt_cur_rtv(), c, NULL, 0, 0.0f, 0)); } /* FPS-INTERP */
        { ID3D11RenderTargetView *wrtv; WD_CALL("rtt_cur_rtv", NULL, wrtv = rtt_cur_rtv()); /* DIAG(PERF-WARM,W139) */
          WD_CALL("clear_rtv", NULL, ID3D11DeviceContext_ClearRenderTargetView(g_ctx, wrtv, c)); }
        /* ASPECT (W18): Balken sofort nach Spiel-Clear (2D); 3D-Draws der
         * Mission uebermalen sie danach korrekt (Hor+). */
        if (g_cfg.aspect_mode && !rtt_active()) {
            sb_aspect_bars();
        }
        if (g_cfg.sbar_probe[0] && m3d_on) { sbar_probe_after(-1, 0); } /* DIAG(W98): Clear */
        sb_ctx_leave();
    }
    if (flags & XB_CLEAR_ZBUFFER) {
        ds |= D3D11_CLEAR_DEPTH;
    }
    if (flags & XB_CLEAR_STENCIL) {
        ds |= D3D11_CLEAR_STENCIL;
    }
    if (ds) {
        sb_ctx_enter(); /* A6 ctx_lock */
        if (g_ip_recording) { WD_CALL("rec_clear_ds", NULL, sb_interp_rec_clear(NULL, NULL, rtt_cur_dsv(), ds, z, stencil)); } /* FPS-INTERP */
        WD_CALL("clear_dsv", NULL, ID3D11DeviceContext_ClearDepthStencilView(g_ctx, rtt_cur_dsv(), ds, z,
                                                  (unsigned char)stencil)); /* DIAG(PERF-WARM,W139) */
        sb_ctx_leave();
    }
}

void sb_render_set_viewport(const sb_viewport *vp)
{
    if (!vp) {
        return;
    }
    /* M2c: Absurde Viewports (INT_MAX: Laden/Deaktiviert) ignorieren. */
    if (vp->w == 0 || vp->h == 0 || vp->w > 8192 || vp->h > 8192) {
        static unsigned nig;
        if (nig < 4) {
            nig++;
            HLOG_I("render: Viewport %ux%u ignoriert (letzter gueltig)",
                   vp->w, vp->h);
        }
        return;
    }
    if (memcmp(vp, &g_vp_game, sizeof g_vp_game) != 0) {
        g_vp_game = *vp;
        sb_update_rig();
    }
}

void sb_render_set_view(const float *m16)
{
    if (m16) {
        if (memcmp(m16, g_view.m, sizeof g_view.m) != 0) {
            memcpy(g_view.m, m16, sizeof g_view.m);
            sb_update_rig(); /* CAMFIX (retry1): View pro Frame neu! */
        }
    }
}

void sb_render_set_proj(const float *m16)
{
    if (!m16) {
        return;
    }
    if (memcmp(m16, g_proj_game.m, sizeof g_proj_game.m) != 0) {
        memcpy(g_proj_game.m, m16, sizeof g_proj_game.m);
        sb_update_rig();
    }
}

void sb_render_set_world(int idx, const float *m16)
{
    if (m16 && idx >= 0 && idx < 4) {
        memcpy(g_world[idx].m, m16, sizeof g_world[idx].m);
    }
}

static void sb_render_screenshot(unsigned frame);
extern unsigned g_present_no; /* weiter unten definiert */

unsigned g_present_no; /* DIAG: letztes Present (global f. TEX-Throttle) */
int g_fix_blend; /* W2B-FIX: scoped Blend-Off aktiv (Save/Restore) */

/* DIAG(POPIN,W91): pf_ring=N - jedes praesentierte Bild (Spiel-Present und
 * Interp-Takt) per CopyResource in einen GPU-Ring (kein CPU-Stall, der
 * Echtzeit-Takt bleibt ungestoert); nach N Bildern alle auf einmal als
 * shots/pf<s>_<iiii>_<art>.(jpg|bmp) schreiben + Zeile PF je Bild (Zeit,
 * Art g = Spiel-Present, x = Interp exakt, i = Interp-Zwischenbild t,
 * Spiel-Frame der Quelle). Start pf_from Spiel-Frames nach Missionsbeginn. */
#define PFR_MAX 1200
static ID3D11Texture2D *pf_tex[PFR_MAX];
static struct { unsigned f, src; float t; char kind; double ms; } pf_meta[PFR_MAX];
static unsigned pf_n, pf_seq, pf_next, pf_m0;
static int pf_on;
static LONGLONG pf_q0;
static float pf_note_t = -1.0f; /* vom Interp-Takt vor present_raw gesetzt */
static unsigned pf_note_src;
static void sb_render_screenshot_ex(unsigned frame, const char *name, ID3D11Resource *src);

int sb_pf_active(void)
{
    return pf_on;
}

void sb_pf_note(float t, unsigned src_frame)
{
    pf_note_t = t;
    pf_note_src = src_frame;
}

/* Spiel-Present (Render-Thread): Missionsbeginn merken, Folge scharf schalten. */
static void pf_present_tick(unsigned n)
{
    unsigned sc = 0, ml = 0, md = 0;
    if (g_cfg.pf_ring <= 0 || pf_on) { return; }
    if (!pf_m0) {
        __try {
            sc = *(volatile unsigned *)(uintptr_t)M3D_SCENE_ADDR;
            ml = *(volatile unsigned *)(uintptr_t)M3D_MLOOP_ADDR;
            md = *(volatile unsigned *)(uintptr_t)0x2EEA34u; /* Modus 4 = Mission (Titel-Demo: 2) */
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (sc != 0x2Cu || ml != 1u || md != (unsigned)(g_cfg.pf_mode > 0 ? g_cfg.pf_mode : 4)) { return; } /* W215: pf_mode=2 = Titel-Demo/Replay */
        pf_m0 = n;
        pf_next = n + (unsigned)(g_cfg.pf_from > 0 ? g_cfg.pf_from : 0);
        HLOG_I("PF: Missionsbeginn f=%u, Bildfolge ab f=%u (%d Bilder)", n, pf_next, g_cfg.pf_ring);
    }
    if (pf_seq >= (unsigned)(g_cfg.pf_max > 0 ? g_cfg.pf_max : 1) || n < pf_next) { return; }
    pf_on = 1;
    pf_n = 0;
    pf_seq++;
    pf_q0 = 0;
}

/* Vor jedem Present: Swapchain-BB (bzw. Spiel-BB) in den Ring kopieren. */
static void pf_capture(char kind)
{
    ID3D11Resource *res = NULL;
    unsigned cap = (unsigned)g_cfg.pf_ring;
    LARGE_INTEGER q;
    if (!pf_on) { return; }
    if (cap > PFR_MAX) { cap = PFR_MAX; }
    if (g_swap_res) { res = g_swap_res; ID3D11Resource_AddRef(res); }
    else { ID3D11RenderTargetView_GetResource(g_rtv, &res); }
    if (!res) { return; }
    if (!pf_tex[pf_n]) {
        D3D11_TEXTURE2D_DESC dd;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &dd);
        dd.BindFlags = 0;
        dd.CPUAccessFlags = 0;
        dd.MiscFlags = 0;
        dd.Usage = D3D11_USAGE_DEFAULT;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &pf_tex[pf_n]))) {
            HLOG_W("PF: Ring-Textur %u fehlgeschlagen -> Folge endet", pf_n);
            pf_tex[pf_n] = NULL;
            ID3D11Resource_Release(res);
            cap = pf_n;
            goto dump;
        }
    }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)pf_tex[pf_n], res);
    ID3D11Resource_Release(res);
    QueryPerformanceCounter(&q);
    if (!pf_q0) { pf_q0 = q.QuadPart; }
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        pf_meta[pf_n].ms = (double)(q.QuadPart - pf_q0) * 1000.0 / (double)f.QuadPart;
    }
    pf_meta[pf_n].kind = kind;
    pf_meta[pf_n].f = g_present_no;
    pf_meta[pf_n].t = (kind == 'g') ? 1.0f : pf_note_t;
    pf_meta[pf_n].src = (kind == 'g') ? g_present_no : pf_note_src;
    pf_n++;
    pf_note_t = -1.0f;
    if (pf_n < cap) { return; }
dump:
    {
        unsigned i;
        HLOG_I("PF: Folge %u fertig (%u Bilder), schreibe", pf_seq, pf_n);
        for (i = 0; i < pf_n; i++) {
            char nm[64];
            char k = pf_meta[i].kind;
            if (k == 'i') {
                _snprintf_s(nm, sizeof nm, _TRUNCATE, "pf%u_%04u_i%03u", pf_seq, i,
                            (unsigned)(pf_meta[i].t * 1000.0f + 0.5f));
            } else {
                _snprintf_s(nm, sizeof nm, _TRUNCATE, "pf%u_%04u_%c", pf_seq, i, k);
            }
            sb_render_screenshot_ex(pf_meta[i].f, nm, (ID3D11Resource *)pf_tex[i]);
            if (g_cfg.edge_ab && k == 'g') { sb_edge_ab_shots(nm, (ID3D11Resource *)pf_tex[i]); } /* DIAG(EDGE-MODE,W107) */
            HLOG_I("PF s=%u i=%u ms=%.2f art=%c t=%.3f f=%u src=%u", pf_seq, i, pf_meta[i].ms, k,
                   (double)pf_meta[i].t, pf_meta[i].f, pf_meta[i].src);
        }
        for (i = 0; i < PFR_MAX; i++) {
            if (pf_tex[i]) { ID3D11Texture2D_Release(pf_tex[i]); pf_tex[i] = NULL; }
        }
        pf_on = 0;
        pf_n = 0;
        pf_next = g_present_no + (unsigned)(g_cfg.pf_every > 0 ? g_cfg.pf_every : 0x7FFFFFFF);
    }
}

static void sb_render_present_body(unsigned flags);
void sb_render_present(unsigned flags)
{
    int z0 = g_ph_zone; /* DIAG(PERF-TICK,W168) */
    g_ph_zone = 2;
    sb_render_present_body(flags);
    g_ph_zone = z0;
    sb_pause_point(); /* W188 GAME-MENU: bei Pause-Wunsch steht der Spiel-Thread hier (sonst sofort zurueck) */
}
static void sb_render_present_body(unsigned flags)
{
    MSG msg;
    HRESULT hr;
    int interval;
    static unsigned n_present;
    (void)flags;
    if (!g_dev) {
        return;
    }
    n_present++;
    g_present_no = n_present;
    sb_phud_tick_begin(g_im_frame_draws); /* W122 PERF-HUD: Taste/Tick (aus: nur Tests) */
    sb_rt_dyn_apply(); /* W146 rt_scale_dyn: gewuenschten RT-Faktor am Swap-Anfang umsetzen */
    if (g_cfg.present_decouple > 0 && g_swap_res) { sb_pd_start(); } /* W138 PERF-FIX: Present-Takt-Thread (startet einmal, prueft selbst Echtzeit) */
    if (g_swap_res) { sb_interp_prealloc(); } /* W139 PERF-WARM warm_vbpre: Tick-Puffer beim Start anlegen (einmal) */
    if (g_cfg.overlay) { /* W118 REM-OVERLAY: Tasten/Werte; W138: Takt-Thread zeichnet das Overlay mit -> unter ctx_lock */
        int pdl = sb_pd_on();
        if (pdl) { sb_ctx_enter(); }
        sb_ovl_present_begin(n_present);
        if (pdl) { sb_ctx_leave(); }
    }
    if (g_cfg.vis_query_live && g_cfg.shading) { extern void xdk_visq_live(void); xdk_visq_live(); } /* FIX(SHADE-FX,W121): Shading per ESC-Menue */
    sb_shfx_shading_watch(); /* FIX(SHADE-FX,W121) shade_live_reset: Bloom-Akkumulatoren beim Pfadwechsel leeren */
    if (g_cfg.pf_ring > 0) { pf_present_tick(n_present); } /* DIAG(POPIN,W91) */
    if (g_cfg.tex_dump || g_cfg.remaster) { sb_rem_present(n_present); } /* W103 REM-FRAME */
    if (g_cfg.m3d_census > 0) { m3d_present_tick(n_present); }
    if (g_cfg.rq_diag) { rq_census_present(n_present); } /* DIAG(RENDER-QA,W77) */
    if (g_cfg.rq_perf) { rq_perf_present(n_present); } /* DIAG(SHADE-PERF,W80) */
    if (g_cfg.im_fast == 2) { im_fast_check_present(n_present); } /* DIAG(IM-PERF,W99) */
    if (g_cfg.fx_diag) { sb_ctx_enter(); fx_present(); sb_ctx_leave(); } /* DIAG(FX-ORIG,W90) */
    if (g_cfg.mfx_diag || g_cfg.mfx_shots) { mfx_present(); } /* DIAG(MONITOR-FX,W105) */
    if (g_cfg.shot_mission || g_cfg.shfx_diag || g_cfg.shfx_ab) { sb_shfx_present(n_present); } /* DIAG(SHADE-FX,W121) */
    if (g_cfg.fxr_diag) { fxr_present(n_present); } /* DIAG(REM-FX,W133) */
    if (g_cfg.dlg_text_upload) { rq_text_flush(); } /* FIX(DLGTEXT,W77) */
    if (g_cfg.drawdrop_diag && ((n_present % 1000u) == 0 || m3d_on)) { dd_sum(n_present); } /* DIAG(COCKPIT,W60) */
    rtt_present_restore(); /* RTT: nie mit Offscreen-RT presenten/screenshottieren */
    /* A6 CTXTHREAD: Render-Thread = Thread des ersten Presents. */
    InterlockedCompareExchange((LONG *)&g_render_tid,
                               (LONG)GetCurrentThreadId(), 0);
    g_im_frame_draws = 0; /* DRAWSTORM */
    /* ASPECT (W18): Frames ohne 3D-Draws bekommen schwarze Balken (der
     * Film loescht das Target nie!); Missions-Frames mit 3D (Hor+)
     * bleiben Vollbild. */
    if (g_cfg.aspect_mode) {
        static unsigned last_3d;
        unsigned cur3d = g_im_3d;
        int had3d = (cur3d != last_3d);
        last_3d = cur3d;
        if (!had3d) {
            sb_ctx_enter();
            sb_aspect_bars();
            sb_ctx_leave();
        }
    }
    /* W107 EDGE-MODE: Bildraender ausserhalb der 4:3-Box (vor dem
     * Screenshot). Mit FPS-INTERP (g_swap_res) erst nach der Kopie in den
     * Swapchain-BB (sb_render_blit) - Aufzeichnung/Verify bleiben unmaskiert. */
    /* W121 SHADE-FX: Cockpit-Licht-Optionen (Standard aus) vor der Randmaske;
     * mit FPS-INTERP in sb_render_blit (nach Aufzeichnung/Verify). */
    if (!g_swap_res && shfx_post_on()) { /* W131: + Farbe/FXAA/SSAO */
        ID3D11Resource *pres = NULL;
        sb_ctx_enter();
        ID3D11RenderTargetView_GetResource(g_rtv, &pres);
        if (pres) { sb_shfx_post(g_rtv, pres); ID3D11Resource_Release(pres); }
        sb_ctx_leave();
    }
    if (!g_swap_res && fxr_ckp_active()) { /* W133 REM-FX: Explosions-Licht Cockpit (wie Cockpit-Licht W121) */
        ID3D11Resource *pres = NULL;
        sb_ctx_enter();
        ID3D11RenderTargetView_GetResource(g_rtv, &pres);
        if (pres) { fxr_post(g_rtv, pres); ID3D11Resource_Release(pres); }
        sb_ctx_leave();
    }
    if (!g_swap_res && (g_cfg.cockpit_panini || g_cfg.cockpit_persp_cycle[0])) { /* W191 UW-PERSP: Panini der Cockpit-Ansicht vor der Randmaske */
        float pd = sb_cp_panini_d();
        if (pd > 0.0f) {
            ID3D11Resource *pres = NULL;
            sb_ctx_enter();
            ID3D11RenderTargetView_GetResource(g_rtv, &pres);
            if (pres) { sb_cp_panini_apply(g_rtv, pres, pd); ID3D11Resource_Release(pres); }
            sb_ctx_leave();
        }
    }
    if (g_cfg.edge_mode > 0 && !g_swap_res) {
        sb_ctx_enter();
        sb_edge_apply(g_rtv);
        sb_ctx_leave();
    }
    if (sb_wm_on() && !g_swap_res) { /* W219 WATERMARK: vor dem Clip-Abgriff (im Clip sichtbar), Bereich gesichert */
        sb_ctx_enter(); sb_wm_draw(g_rtv, 1); sb_ctx_leave();
    }
    if (sb_clip_active() && !g_swap_res) {
        sb_ctx_enter(); sb_clip_frame(g_rtv); sb_ctx_leave();
    }
    /* W118 REM-OVERLAY: offenes Overlay auf den Spiel-BB (Bereich gesichert,
     * nach dem Present zurueck); mit FPS-INTERP in sb_render_blit. */
    if (g_cfg.overlay && (sb_ovl_open() || sb_clip_active()) && !g_swap_res) {
        sb_ctx_enter();
        sb_ovl_draw(g_rtv, 1);
        sb_ctx_leave();
    }
    if (sb_phud_visible() && !g_swap_res) { /* W122 PERF-HUD: wie das Overlay (Bereich gesichert) */
        sb_ctx_enter();
        sb_phud_draw(g_rtv, 1);
        sb_ctx_leave();
    }
    /* Screenshot VOR Present (Backbuffer enthaelt das fertige Bild). */
    sb_ctx_enter(); /* A6 ctx_lock (Screenshot liest per Context zurueck) */
    if (g_cfg.fxr_ab) { fxr_shot_tick(n_present); } /* DIAG(REM-FX,W133): Bildpaare an Explosionen */
    if (g_cfg.cockpit_persp_shots[0]) { sb_cp_present(n_present); } /* DIAG(UW-PERSP,W191) */
    if (g_cfg.overlay_test[0] && !g_swap_res) { /* TEST(W118): Overlay-Bild */
        char onm[48];
        if (sb_ovl_shot_due(onm, sizeof onm)) { sb_render_shot_name(onm, NULL); }
    }
    if (g_cfg.screenshot_frame > 0 &&
        n_present >= (unsigned)g_cfg.screenshot_frame) {
        if (g_cfg.screenshot_every > 0) {
            if ((n_present - (unsigned)g_cfg.screenshot_frame) %
                    (unsigned)g_cfg.screenshot_every == 0 &&
                n_present <= (unsigned)g_cfg.screenshot_frame +
                    (unsigned)g_cfg.screenshot_max * (unsigned)g_cfg.screenshot_every) {
                sb_render_screenshot(n_present);
            } else if (((g_cfg.shade_ab && g_cfg.shading) || g_cfg.mfx_ab || g_cfg.shfx_ab || g_cfg.shadow_ab) && /* W127 shadow_ab */ /* DIAG(RENDER-QA3,W100 / MONITOR-FX,W105 / SHADE-FX,W121): Partnerbild */
                       ((n_present - (unsigned)g_cfg.screenshot_frame) % (unsigned)g_cfg.screenshot_every == 1u ||
                        (g_cfg.shfx_ab == 5 && (n_present - (unsigned)g_cfg.screenshot_frame) % (unsigned)g_cfg.screenshot_every == 2u)) && /* W131 Preset-Tripel: +2 */
                       n_present <= (unsigned)g_cfg.screenshot_frame + 1u +
                           (unsigned)g_cfg.screenshot_max * (unsigned)g_cfg.screenshot_every) {
                sb_render_screenshot(n_present);
            }
        } else if ((n_present - (unsigned)g_cfg.screenshot_frame) %
                       500 == 0 &&
                   n_present <=
                       (unsigned)g_cfg.screenshot_frame + 1000) {
            sb_render_screenshot(n_present);
        }
    }
    if (g_shot_census) { g_shot_census = 0; sb_render_screenshot(n_present); }
    if (g_cfg.bloom_diag) { bloom_diag_present(n_present); } /* DIAG(SHADE-BLOOM,W112) */
    sb_ctx_leave();
    /* M2c: alle 100 Presents (fps120_probe nutzt "IM draws="). */
    if ((n_present % 100) == 0) {
        HLOG_I("render: IM draws=%u vdata=%u prims=[1]%u [2]%u [3]%u [4]%u "
               "[5]%u [6]%u [7]%u [8]%u tris=%u rhw=%u 3d=%u sub=%u texd=%u notex=%u",
               g_im_draws, g_im_vdata, g_im_prim_hist[1], g_im_prim_hist[2],
               g_im_prim_hist[3], g_im_prim_hist[4], g_im_prim_hist[5],
               g_im_prim_hist[6], g_im_prim_hist[7], g_im_prim_hist[8],
               g_im_tris, g_im_rhw, g_im_3d, g_im_submits,
               g_im_texd, g_im_notex);
    }
    /* A6 CTXTHREAD: Stand des Fremd-Thread-Zaehlers (alle 1000 Presents). */
    if ((n_present % 1000) == 0) {
        HLOG_I("render: DIAG(CTXTHREAD) fremde Context-Eintritte=%ld (Render-T%04x)",
               (long)g_ctx_foreign_n, g_render_tid);
    }
    /* A6 ui_thread: nur der alte Pfad pumpt hier (sonst der UI-Thread). */
    while (!g_cfg.ui_thread && PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            hlog_close();
            ExitProcess(0);
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    interval = g_cfg.present_interval < 0 ? 0 : g_cfg.present_interval;
    if (interval > 4) {
        interval = 4;
    }
    /* A5: THREADPRIO-Block entfernt (W3_phA/B gleich; ersatzlos, da
     * thread_priorities=1 die Xbox-Prioritaeten sauber abbildet). */
    { /* OUTWRITE-MAP-AN: Mission-HUD braucht Out-Bytes (altes OUTWRITE
         * war aktiv bei 58061/67699!). Threshold 60k (Load-Schutz). */
        /* OUTWRITE-MAP: Buttons -> Out via Capstone-Mapping. */
        static const struct { unsigned short off; unsigned char is_or, bit; }
        outmap[39] = {
            {3,0,0}, {2,0,0}, {0,1,0x02}, {0,1,0x80},       /* bi0-3 */
            {0x2AD,0,0}, {0x2AB,0,0}, {0x2AA,0,0}, {0x158,0,0}, /* bi4-7 */
            {0x159,0,0}, {0x156,0,0}, {0x157,0,0}, {0,1,0x08}, /* bi8-11 */
            {0,1,0x04}, {0xFFFF,0,0}, {0x15B,0,0}, {0x2AC,0,0}, /* bi12-15 */
            {0x15D,0,0}, {0x15A,0,0}, {7,0,0}, {0x2B1,0,0}, /* bi16-19 */
            {0x154,1,0x20}, {0x2A8,1,0x40}, {0x2AF,0,0}, {0x2AE,0,0}, /* bi20-23 */
            {0x2A8,1,0x80}, {5,0,0}, {4,0,0}, {0,1,0x01}, /* bi24-27 */
            {0x154,1,0x01}, {0x154,1,0x02}, {0x154,1,0x04}, {0x154,1,0x08}, /* bi28-31 */
            {0x154,1,0x40}, {0x154,1,0x40}, {0x2A8,1,0x01}, {0x2A8,1,0x02}, /* bi32-35 */
            {0x2A8,1,0x04}, {0x2A8,1,0x08}, {0x2B0,0,0},   /* bi36-38 */
        };
        extern uint8_t g_last_sbc[24];
        __try {
            unsigned short b0 = *(unsigned short *)g_last_sbc;
            unsigned short b1 = *(unsigned short *)(g_last_sbc + 2);
            unsigned short b2 = *(unsigned short *)(g_last_sbc + 4);
            /* A8: per Schalter abschaltbar (outwrite=0: nur Mapper), Schwelle
             * per outwrite_frame (Default 80000 = bisher). */
            if (!g_cfg.outwrite ||
                g_present_no < (unsigned)g_cfg.outwrite_frame) { b0 = 0; b1 = 0; b2 = 0; }
            /* EJECT (bi3) raus: haengt Animation. */
            b0 &= (unsigned short)~(1u << 3);
            if (b0 | b1 | b2) {
                unsigned bw[3] = { b0, b1, b2 };
                int bi;
                /* Nur aktiver Port ([0x352274]): Out-Struct (0x2B0+) ist
                 * groesser als Stride (0x154), Ports ueberlappen! */
                unsigned po = *(volatile unsigned *)(uintptr_t)0x352274u & 3u;
                {
                    unsigned outp = 0x4AF480u + po * 0x154u;
                    for (bi = 0; bi < 39; bi++) {
                        if ((bw[bi / 16] >> (bi % 16)) & 1u) {
                            unsigned off = outmap[bi].off;
                            if (off == 0xFFFFu) { continue; }
                            if (outmap[bi].is_or) {
                                *(unsigned char *)(uintptr_t)(outp + off)
                                    |= outmap[bi].bit;
                            } else {
                                *(unsigned char *)(uintptr_t)(outp + off) = 0xFF;
                            }
                        }
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (sb_interp_swap()) { /* FPS-INTERP (W69): Bilder zeigt der Interp-Takt */
        if (g_cfg.overlay) { sb_ovl_present_end(); } /* W118: Bildraten-Wechsel zwischen den Bildern */
        if (g_cfg.shfx_ab >= 4) { sb_shfx_present_end(n_present); } /* DIAG(SHADER-OPT2,W131) */
        sb_winmode_tick(n_present); /* W81: Moduswechsel/Resize zwischen den Bildern */
        if (g_phud_meas) { sb_phud_tick_end(); } /* W122 PERF-HUD */
        return;
    }
    /* W134 VR-PROTO: der VR-Thread (OpenXR) nutzt den Immediate-Context unter
     * ctx_lock -> Blit + Present des Spiel-Threads dann ebenfalls darunter
     * (sonst Treiber-Haenger in Present, Lauf W134_x1). vr_enable=0: unveraendert. */
    { int pdl = sb_pd_on(); /* W138 PERF-FIX: Takt-Thread wiederholt den Swapchain-BB -> Blit + Effekte + Present am Stueck */
    if (g_cfg.vr_enable || pdl) { sb_ctx_enter(); }
    if (g_swap_res) { sb_render_blit(NULL); } /* FPS-INTERP: Spiel-Bild zeigen */
    if (pf_on) { sb_ctx_enter(); pf_capture('g'); sb_ctx_leave(); } /* DIAG(POPIN,W91) */
    { long long q0 = (g_cfg.rq_perf || g_phud_meas) ? rq_qpc() : 0;
    if (g_phud_meas) { sb_ctx_enter(); sb_phud_present_pre(); sb_ctx_leave(); } /* W122 PERF-HUD: GPU-Zeit */
    hr = IDXGISwapChain_Present(g_swap, (unsigned)interval, 0);
    g_pd_last_q = rq_qpc(); /* W138 */
    sb_warm_note_game_present(g_pd_last_q); /* W139 warm_gpu: Ladephase erkennen */
    if (g_cfg.vr_enable || pdl) { sb_ctx_leave(); }
    if (g_phud_meas) { sb_ctx_enter(); sb_phud_present_post(q0, 1); sb_ctx_leave(); } /* W122 PERF-HUD */
    if (g_cfg.rq_perf) { g_rqp_q[RQP_PRESENT] += rq_qpc() - q0; } } } /* DIAG(SHADE-PERF,W80); W138: Ende pdl-Block */
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        HLOG_E("render: Device verloren (hr=0x%lX)", (long)hr);
        ExitProcess(7);
    }
    if (sb_phud_visible()) { /* W122 PERF-HUD: Spiel-BB-Bereich zurueck (vor dem Overlay: umgekehrte Reihenfolge) */
        sb_ctx_enter();
        sb_phud_restore();
        sb_ctx_leave();
    }
    if (g_cfg.overlay) { /* W118 REM-OVERLAY: Spiel-BB-Bereich zurueck, Bildraten-Wechsel */
        sb_ctx_enter();
        sb_ovl_restore();
        sb_ctx_leave();
        sb_ovl_present_end();
    }
    if (g_cfg.watermark) { /* W219 WATERMARK: zuletzt zurueck (zuerst gesichert) */
        sb_ctx_enter();
        sb_wm_restore();
        sb_ctx_leave();
    }
    if (g_cfg.shfx_ab >= 4) { sb_shfx_present_end(n_present); } /* DIAG(SHADER-OPT2,W131) */
    sb_winmode_tick(n_present); /* W81: Moduswechsel/Resize zwischen den Bildern */
    if (g_phud_meas) { sb_phud_tick_end(); } /* W122 PERF-HUD */
}

/* W118 REM-OVERLAY: Laufzeit-Bildratenwechsel 30 -> 60/120 ohne Offscreen-BB:
 * Views neu (sb_create_views legt mit interp_fps>0 den Offscreen-BB an),
 * gleiche Schritte wie sb_render_resize ohne ResizeBuffers. Render-Thread,
 * zwischen den Bildern. 1 = ok. */
int sb_render_views_rebuild(const char *why)
{
    D3D11_VIEWPORT dvp;
    if (!g_swap || !g_dev) { return 0; }
    sb_ctx_enter();
    sb_interp_reset(why);
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 0, NULL, NULL);
    if (g_rtv) { ID3D11RenderTargetView_Release(g_rtv); g_rtv = NULL; }
    if (g_dsv) { ID3D11DepthStencilView_Release(g_dsv); g_dsv = NULL; }
    if (g_bb_dsrv) { ID3D11ShaderResourceView_Release(g_bb_dsrv); g_bb_dsrv = NULL; }
    if (g_swap_rtv) { ID3D11RenderTargetView_Release(g_swap_rtv); g_swap_rtv = NULL; }
    if (g_swap_res) { ID3D11Resource_Release(g_swap_res); g_swap_res = NULL; }
    ID3D11DeviceContext_Flush(g_ctx);
    sb_create_views();
    dvp.TopLeftX = 0.0f;
    dvp.TopLeftY = 0.0f;
    dvp.Width = (float)g_bb_w;
    dvp.Height = (float)g_bb_h;
    dvp.MinDepth = 0.0f;
    dvp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &dvp);
    sb_update_rig();
    sb_ctx_leave();
    HLOG_I("render: Views neu (%s): interp_fps=%d offscreen=%d f=%u", why ? why : "", g_cfg.interp_fps,
           g_swap_res != NULL, g_present_no);
    return 1;
}

/* W131 SHADER-OPT2: SSAO braucht die BB-Tiefe als SRV (typlos, sb_create_views).
 * Sie wird nur beim Anlegen der Views typlos (ssao>0 bzw. bb_depth_srv=1 beim
 * Start). Ein Neubau der Views zur Laufzeit (erster Ansatz) stoerte in W131_t1
 * die Hangar-Komposition (Fenster grau/weiss bis Laufende, W131_t3 ohne Neubau
 * korrekt) - daher kein Neubau: live eingeschaltet wirkt SSAO erst nach Neustart. */
int sb_render_offscreen(void)
{
    return g_swap_res != NULL;
}

HWND sb_render_hwnd(void)
{
    return g_hwnd;
}

/* W81 WINMODE: Backbuffer auf w x h (Render-Thread, Ende des Presents).
 * Gibt vorher alle Backbuffer-Referenzen frei: Interp-Listen/Schatten/
 * Staging (sb_interp_reset), gebundenes RT, RTV, DSV, Swapchain-BB
 * (g_swap_res) - sonst scheitert ResizeBuffers. Danach Views, Viewport und
 * Hor+-Rig neu; Aspect-Box/Balken rechnen ohnehin je Aufruf aus g_bb_w/h.
 * 1 = neue Groesse gilt, 0 = unveraendert/abgelehnt. */
int sb_render_resize(unsigned w, unsigned h, const char *why)
{
    unsigned ow = g_bb_w, oh = g_bb_h;
    HRESULT hr;
    D3D11_VIEWPORT dvp;
    if (!g_swap || !g_dev) { return 0; }
    if (w < 64 || h < 64 || w > 8192 || h > 8192) {
        HLOG_W("WINMODE resize %ux%u abgelehnt (%s)", w, h, why ? why : "");
        return 0;
    }
    if (w == g_bb_w && h == g_bb_h) { return 0; }
    sb_ctx_enter();
    sb_interp_reset(why);
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 0, NULL, NULL);
    if (g_rtv) { ID3D11RenderTargetView_Release(g_rtv); g_rtv = NULL; }
    if (g_dsv) { ID3D11DepthStencilView_Release(g_dsv); g_dsv = NULL; }
    if (g_bb_dsrv) { ID3D11ShaderResourceView_Release(g_bb_dsrv); g_bb_dsrv = NULL; }
    if (g_swap_rtv) { ID3D11RenderTargetView_Release(g_swap_rtv); g_swap_rtv = NULL; } /* W107 */
    if (g_swap_res) { ID3D11Resource_Release(g_swap_res); g_swap_res = NULL; }
    ID3D11DeviceContext_Flush(g_ctx);
    hr = IDXGISwapChain_ResizeBuffers(g_swap, 0, w, h, DXGI_FORMAT_UNKNOWN, g_swap_flags);
    if (FAILED(hr)) {
        HLOG_E("WINMODE ResizeBuffers %ux%u hr=0x%08lX -> zurueck auf %ux%u", w, h,
               (unsigned long)hr, ow, oh);
        w = ow;
        h = oh;
        hr = IDXGISwapChain_ResizeBuffers(g_swap, 0, w, h, DXGI_FORMAT_UNKNOWN, g_swap_flags);
        if (FAILED(hr)) {
            HLOG_E("WINMODE ResizeBuffers (alt) hr=0x%08lX", (unsigned long)hr);
            ExitProcess(7);
        }
    }
    g_bb_w = w;
    g_bb_h = h;
    sb_create_views(); /* bindet RTV+DSV, legt den Interp-Offscreen-BB neu an */
    dvp.TopLeftX = 0.0f;
    dvp.TopLeftY = 0.0f;
    dvp.Width = (float)g_bb_w;
    dvp.Height = (float)g_bb_h;
    dvp.MinDepth = 0.0f;
    dvp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &dvp);
    sb_update_rig();
    sb_ctx_leave();
    HLOG_I("WINMODE resize %ux%u -> %ux%u (%s, f=%u, Seitenverhaeltnis %.3f)", ow, oh, g_bb_w,
           g_bb_h, why ? why : "", g_present_no, (double)g_bb_w / (double)g_bb_h);
    return (w != ow || h != oh) ? 1 : 0;
}

/* FPS-INTERP (W69): Bild (src, NULL = Spiel-Backbuffer) in den Swapchain-BB
 * kopieren. Nur mit Offscreen-Backbuffer (interp_fps>0) wirksam. */
void sb_render_blit(ID3D11Resource *src)
{
    ID3D11Resource *res = NULL;
    if (!g_swap_res) {
        return;
    }
    if (g_cfg.vr_enable && !g_vr_blit && sb_vr_window_mirrors()) {
        /* W183 VR-FIX3: Fenster zeigt in der Mission nur den VR-Spiegel (bzw. schwarz) - keine Kopie und
         * keine Nachbearbeitung des Monobilds; ESC-Menue (und F2 ohne vr_preview_clean) darueber. */
        sb_ctx_enter();
        if (!g_swap_rtv &&
            FAILED(ID3D11Device_CreateRenderTargetView(g_dev, g_swap_res, NULL, &g_swap_rtv))) {
            g_swap_rtv = NULL;
        }
        if (sb_vr_after_blit(g_swap_res, g_swap_rtv)) {
            if (sb_clip_active()) { sb_clip_frame(g_swap_rtv); } /* W210: before flat client overlays */
            if (g_cfg.overlay && (sb_ovl_open() || sb_clip_active())) { sb_ovl_draw(g_swap_rtv, 0); }
            if (!g_cfg.vr_preview_clean && sb_phud_visible()) { sb_phud_draw(g_swap_rtv, 0); }
        }
        if (g_cfg.overlay_test[0]) { /* TEST(W118): Overlay-Bild (gezeigtes Bild) */
            char onm[48];
            if (sb_ovl_shot_due(onm, sizeof onm)) { sb_render_shot_name(onm, g_swap_res); }
        }
        sb_ctx_leave();
        return;
    }
    if (!src) {
        ID3D11RenderTargetView_GetResource(g_rtv, &res);
        src = res;
    }
    sb_gp_mark(GP_BLIT); /* DIAG(GPU-COST,W146) */
    if (src) {
        ID3D11DeviceContext_CopyResource(g_ctx, g_swap_res, src);
    }
    if (res) {
        ID3D11Resource_Release(res);
    }
    sb_gp_mark(GP_POST); /* DIAG(GPU-COST,W146) */
    if (!g_vr_blit && shfx_post_on()) { /* W121 SHADE-FX (+W131): Cockpit-Licht/Farbe/FXAA/SSAO auf das gezeigte Bild (nach Verify/Wiedergabe) */
        sb_ctx_enter();
        if (!g_swap_rtv &&
            FAILED(ID3D11Device_CreateRenderTargetView(g_dev, g_swap_res, NULL, &g_swap_rtv))) {
            g_swap_rtv = NULL;
        }
        sb_shfx_post(g_swap_rtv, g_swap_res);
        sb_ctx_leave();
    }
    sb_gp_mark(GP_FXPOST); /* DIAG(GPU-COST,W146) */
    if (!g_vr_blit && fxr_ckp_active()) { /* W133 REM-FX: Explosions-Licht Cockpit auf jedes gezeigte Bild */
        sb_ctx_enter();
        if (!g_swap_rtv &&
            FAILED(ID3D11Device_CreateRenderTargetView(g_dev, g_swap_res, NULL, &g_swap_rtv))) {
            g_swap_rtv = NULL;
        }
        fxr_post(g_swap_rtv, g_swap_res);
        sb_ctx_leave();
    }
    sb_gp_mark(GP_HUD); /* DIAG(GPU-COST,W146) */
    if (!g_vr_blit && (g_cfg.cockpit_panini || g_cfg.cockpit_persp_cycle[0]) && sb_cp_panini_d() > 0.0f) { /* W191 UW-PERSP: Panini auf jedes gezeigte Bild (auch Pause-Standbild) */
        sb_ctx_enter();
        if (!g_swap_rtv &&
            FAILED(ID3D11Device_CreateRenderTargetView(g_dev, g_swap_res, NULL, &g_swap_rtv))) {
            g_swap_rtv = NULL;
        }
        sb_cp_panini_apply(g_swap_rtv, g_swap_res, sb_cp_panini_d());
        sb_ctx_leave();
    }
    if (!g_vr_blit && g_cfg.edge_mode > 0) { /* W107 EDGE-MODE: Maske auf das gezeigte Bild (nach Verify/Wiedergabe) */
        sb_ctx_enter();
        if (!g_swap_rtv &&
            FAILED(ID3D11Device_CreateRenderTargetView(g_dev, g_swap_res, NULL, &g_swap_rtv))) {
            g_swap_rtv = NULL;
        }
        sb_edge_apply(g_swap_rtv);
        sb_ctx_leave();
    }
    if (g_cfg.vr_enable && !g_vr_blit && sb_vr_panels_on()) { /* W197 VR-MENU: Leinwand-Quelle ohne ESC-Menue/F2 */
        sb_ctx_enter();
        sb_vr_canvas_source(g_swap_res);
        sb_ctx_leave();
    }
    if (sb_wm_on()) { /* W219 WATERMARK: auf jedes gezeigte Bild, vor dem Clip-Abgriff */
        sb_ctx_enter();
        if (!g_swap_rtv) { ID3D11Device_CreateRenderTargetView(g_dev, g_swap_res, NULL, &g_swap_rtv); }
        sb_wm_draw(g_swap_rtv, 0);
        sb_ctx_leave();
    }
    if (sb_clip_active()) {
        sb_ctx_enter();
        if (!g_swap_rtv) { ID3D11Device_CreateRenderTargetView(g_dev, g_swap_res, NULL, &g_swap_rtv); }
        sb_clip_frame(g_swap_rtv);
        sb_ctx_leave();
    }
    if (g_cfg.overlay && (sb_ovl_open() || sb_clip_active())) { /* W118 REM-OVERLAY: auf jedes gezeigte Bild (Kopie ueberschreibt ganz) */
        sb_ctx_enter();
        if (!g_swap_rtv &&
            FAILED(ID3D11Device_CreateRenderTargetView(g_dev, g_swap_res, NULL, &g_swap_rtv))) {
            g_swap_rtv = NULL;
        }
        sb_ovl_draw(g_swap_rtv, 0);
        sb_ctx_leave();
    }
    if (sb_phud_visible()) { /* W122 PERF-HUD: auf jedes gezeigte Bild */
        sb_ctx_enter();
        if (!g_swap_rtv &&
            FAILED(ID3D11Device_CreateRenderTargetView(g_dev, g_swap_res, NULL, &g_swap_rtv))) {
            g_swap_rtv = NULL;
        }
        sb_phud_draw(g_swap_rtv, 0);
        sb_ctx_leave();
    }
    if (g_cfg.vr_enable && !g_vr_blit) { /* W164 VR-FIX2: Leinwand-Quelle; OpenXR-Vorschau = eingereichtes Augenbild */
        sb_ctx_enter();
        if (!g_swap_rtv &&
            FAILED(ID3D11Device_CreateRenderTargetView(g_dev, g_swap_res, NULL, &g_swap_rtv))) {
            g_swap_rtv = NULL;
        }
        if (sb_vr_after_blit(g_swap_res, g_swap_rtv)) {
            if (g_cfg.overlay && (sb_ovl_open() || sb_clip_active())) { sb_ovl_draw(g_swap_rtv, 0); } /* ESC-Menue bleibt bedienbar */
            if (!g_cfg.vr_preview_clean && sb_phud_visible()) { sb_phud_draw(g_swap_rtv, 0); }
        }
        sb_ctx_leave();
    }
    if (g_cfg.overlay_test[0]) { /* TEST(W118): Overlay-Bild (gezeigtes Bild) */
        char onm[48];
        sb_ctx_enter();
        if (sb_ovl_shot_due(onm, sizeof onm)) { sb_render_shot_name(onm, g_swap_res); }
        sb_ctx_leave();
    }
}

/* FPS-INTERP (W69): nacktes Present eines Zwischen-/Tick-Bildes (ohne
 * Zaehler/Pump/Screenshot). Intervall 0: der Interp-Takt praesentiert auch
 * mitten im Spiel-Tick, ein VSync-Block wuerde das Spiel bremsen. */
void sb_render_present_raw(void)
{
    HRESULT hr;
    long long q0 = g_phud_meas ? rq_qpc() : 0;
    if (pf_on) { pf_capture(pf_note_t >= 1.0f ? 'x' : 'i'); } /* DIAG(POPIN,W91), unter ctx_lock */
    if (g_phud_meas) { sb_phud_present_pre(); } /* W122 PERF-HUD */
    hr = IDXGISwapChain_Present(g_swap, 0, 0);
    g_pd_last_q = rq_qpc(); /* W138 */
    if (g_phud_meas) { sb_phud_present_post(q0, 0); } /* W122 PERF-HUD */
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        HLOG_E("render: Device verloren (hr=0x%lX)", (long)hr);
        ExitProcess(7);
    }
}

/* PERF(PERF-FIX,W138) present_decouple: letztes gezeigtes Bild erneut
 * praesentieren (Takt-Thread, unter ctx_lock). Der Swapchain-BB
 * (Offscreen-Pfad, SEQUENTIAL) haelt das zuletzt gezeigte Bild samt
 * Effekten/Overlays unveraendert: Blit + Nachbearbeitung + Present des
 * Spiel-Threads laufen mit present_decouple am Stueck unter ctx_lock.
 * 1 = praesentiert. */
int sb_render_present_hold(void)
{
    if (!g_swap || !g_swap_res || !g_pd_last_q) { return 0; }
    g_phud_src = 2;
    sb_render_present_raw();
    g_phud_src = 0;
    return 1;
}

/* ---- W188 GAME-MENU: Pause-Bilder (sb_pause.c, Spiel-Thread) ----
 * Beginn: aktuelles Spielbild (g_rtv-Resource: mit FPS-INTERP der Offscreen-
 * BB = letztes Tick-Bild ohne Nachbearbeitung, sonst der Swapchain-BB mit
 * fertiger Nachbearbeitung) kopieren. Je Pausenbild: Kopie zeigen, Menue
 * darueber, Present. Ende: Swapchain-BB wieder sauber (ohne FPS-INTERP),
 * Zwischenbild-Aufzeichnung neu (keine Paare ueber die Pause hinweg). */
static ID3D11Texture2D *g_pause_snap;
static unsigned g_pause_sw, g_pause_sh;
/* W193 PAUSE-LIVE: Standbild aus der FPS-INTERP-Aufzeichnung mit den aktuellen Einstellungen.
 * Bei Pausenbeginn einmal neu zeichnen und gegen das Spiel-Bild abgleichen (Korrektheitsanker),
 * danach bei jeder Einstellungsaenderung (Fingerabdruck ueber g_cfg + Vergleichsschalter bzw.
 * sb_render_pause_dirty) hoechstens alle 50 ms neu. Gezeigt wird die Neuzeichnung nur, solange
 * die Einstellungen vom Pausenbeginn abweichen (sonst die Kopie = bitgleich zu W188). Ohne
 * Aufzeichnung (ausserhalb der Mission, interp_fps=0) bzw. mit VR: Kopie wie bisher. */
extern int g_shfx_bypass; /* render_shfx.c */
static ID3D11Texture2D *g_pause_live;
static int g_pl_state;            /* 1 = Neuzeichnen moeglich */
static int g_pl_use;              /* 1 = g_pause_live zeigen */
static unsigned g_pl_h0, g_pl_h;  /* Fingerabdruck bei Pausenbeginn / der letzten Neuzeichnung */
static volatile LONG g_pl_dirty;  /* sb_render_pause_dirty */
static long long g_pl_last;       /* QPC der letzten Neuzeichnung */
static unsigned g_pl_n, g_pl_nlog;

static unsigned pl_hash(void)
{
    const unsigned char *b = (const unsigned char *)&g_cfg;
    unsigned h = 2166136261u;
    size_t i;
    for (i = 0; i < sizeof g_cfg; i++) { h = (h ^ b[i]) * 16777619u; }
    h = (h ^ (unsigned)g_shfx_bypass) * 16777619u;
    h = (h ^ (unsigned)g_fxr_bypass) * 16777619u;
    return h;
}

void sb_render_pause_dirty(void)
{
    InterlockedExchange(&g_pl_dirty, 1);
}

/* Neu zeichnen und nach g_pause_live kopieren (unter ctx_lock). first = Pausenbeginn (Abgleich). */
static int pl_render(int first)
{
    sb_pl_info_t inf;
    ID3D11Resource *img;
    long long q0 = rq_qpc(), q1;
    unsigned nd = 0, mx = 0, tup = 0, tfail = 0;
    int cmp = 0, tleft;
    tleft = sb_tex_pause_relive(24, &tup, &tfail); /* HD live an/aus: Texturen selbst nachladen (24 je Pausenbild: Menue bleibt bedienbar) */
    if (tleft > 0) { InterlockedExchange(&g_pl_dirty, 1); } /* Rest beim naechsten Pausenbild */
    img = sb_interp_pause_render(&inf);
    if (!img) { return 0; }
    if (!g_pause_live && g_pause_snap) {
        D3D11_TEXTURE2D_DESC dd;
        ID3D11Texture2D_GetDesc(g_pause_snap, &dd);
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &g_pause_live))) { g_pause_live = NULL; }
    }
    if (!g_pause_live) { return 0; }
    {
        D3D11_TEXTURE2D_DESC a, b;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)img, &a);
        ID3D11Texture2D_GetDesc(g_pause_live, &b);
        if (a.Width != b.Width || a.Height != b.Height || a.Format != b.Format) {
            HLOG_W("PAUSE live: Bildgroesse %ux%u statt %ux%u - Kopie bleibt", a.Width, a.Height, b.Width, b.Height);
            return 0;
        }
    }
    if (first) { cmp = sb_interp_pause_cmp(img, &nd, &mx); }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_pause_live, img);
    q1 = rq_qpc();
    g_pl_n++;
    if (first || g_pl_nlog < 40u) {
        LARGE_INTEGER qf;
        QueryPerformanceFrequency(&qf);
        if (!first) { g_pl_nlog++; }
        HLOG_I("PAUSE live: %s #%u (Tick f=%u ser=%u ops=%u shading-draws=%u konst=%d fenster=%u sfx-neu=%u schatten-aus=%u "
               "ohne-shading=%u mfx-aus=%u mvp-haken=%u texturen=%u/%u offen=%d, shading=%d monitor_fx=%d edge=%d hd=%d, %.2f ms)",
               first ? "Neuzeichnung bei Pausenbeginn" : "Neuzeichnung", g_pl_n, inf.frame, inf.ser, inf.nops, inf.nrq,
               inf.cb_state, inf.ncb, inf.nsfx, inf.nshadow, inf.noshade, inf.nmfx, inf.nhook, tup, tup + tfail, tleft,
               g_cfg.shading, g_cfg.monitor_fx, g_cfg.edge_mode, g_cfg.remaster && g_cfg.hd_textures,
               (double)(q1 - q0) * 1000.0 / (double)qf.QuadPart);
        if (first) {
            if (cmp) {
                HLOG_I("PAUSE live: Abgleich Neuzeichnung vs. Spiel-Bild diff=%u px max=%u (%s)", nd, mx,
                       nd ? "ABWEICHUNG" : "bitgleich");
            } else {
                HLOG_W("PAUSE live: Abgleich nicht moeglich (keine Sicherung des Ticks)");
            }
        }
    }
    return 1;
}

int sb_render_pause_begin(void)
{
    ID3D11Resource *res = NULL;
    D3D11_TEXTURE2D_DESC dd;
    int ok = 0;
    if (!g_dev || !g_ctx || !g_rtv) { return 0; }
    sb_ctx_enter();
    ID3D11RenderTargetView_GetResource(g_rtv, &res);
    if (res) {
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &dd);
        if (g_pause_snap && (g_pause_sw != dd.Width || g_pause_sh != dd.Height)) {
            ID3D11Texture2D_Release(g_pause_snap);
            g_pause_snap = NULL;
        }
        if (!g_pause_snap) {
            dd.MipLevels = 1;
            dd.ArraySize = 1;
            dd.SampleDesc.Count = 1;
            dd.SampleDesc.Quality = 0;
            dd.Usage = D3D11_USAGE_DEFAULT;
            dd.BindFlags = 0;
            dd.CPUAccessFlags = 0;
            dd.MiscFlags = 0;
            if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &g_pause_snap))) { g_pause_snap = NULL; }
            g_pause_sw = dd.Width;
            g_pause_sh = dd.Height;
        }
        if (g_pause_snap) {
            ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_pause_snap, res);
            ok = 1;
        }
        ID3D11Resource_Release(res);
    }
    /* W193 PAUSE-LIVE */
    g_pl_state = 0;
    g_pl_use = 0;
    InterlockedExchange(&g_pl_dirty, 0);
    if (g_pause_live) {
        D3D11_TEXTURE2D_DESC a;
        ID3D11Texture2D_GetDesc(g_pause_live, &a);
        if (!g_pause_snap || a.Width != g_pause_sw || a.Height != g_pause_sh) {
            ID3D11Texture2D_Release(g_pause_live);
            g_pause_live = NULL;
        }
    }
    if (ok && g_swap_res && g_cfg.pause_live) {
        if (g_cfg.vr_enable) {
            HLOG_I("PAUSE live: aus im VR-Betrieb (VR-Thread zeigt seine Wiedergabe, Session unberuehrt)");
        } else if (pl_render(1)) {
            g_pl_state = 1;
            g_pl_h0 = g_pl_h = pl_hash();
            g_pl_last = rq_qpc();
        } else {
            HLOG_I("PAUSE live: keine Aufzeichnung des Ticks (ausserhalb der Mission) - Standbild als Kopie, "
                   "Nachbearbeitung/Bildraender wirken live");
        }
    } else if (ok && g_cfg.pause_live && !g_swap_res) {
        HLOG_I("PAUSE live: ohne FPS-INTERP (interp_fps=0) keine Aufzeichnung - Standbild als Kopie");
    }
    sb_ctx_leave();
    return ok;
}

void sb_render_pause_frame(void)
{
    if (!g_dev || !g_swap || !g_pause_snap) { return; }
    sb_ctx_enter();
    if (g_swap_res) { /* FPS-INTERP: wie ein gezeigtes Bild (Nachbearbeitung, Menue, VR-Leinwand, Test-Bild) */
        if (g_pl_state) { /* W193 PAUSE-LIVE: Einstellungen geaendert -> neu zeichnen (gedrosselt) */
            unsigned h = pl_hash();
            long long now = rq_qpc();
            LARGE_INTEGER qf;
            QueryPerformanceFrequency(&qf);
            if ((h != g_pl_h || g_pl_dirty) && now - g_pl_last >= qf.QuadPart / 20) {
                int dirty = InterlockedExchange(&g_pl_dirty, 0) != 0;
                g_pl_last = now;
                g_pl_h = h;
                if (h == g_pl_h0 && !dirty) {
                    g_pl_use = 0; /* zurueck auf die Einstellungen vom Pausenbeginn: Kopie (bitgleich) */
                } else if (pl_render(0)) {
                    g_pl_use = 1;
                }
            }
        }
        {
            ID3D11Resource *show = (ID3D11Resource *)(g_pl_use && g_pause_live ? g_pause_live : g_pause_snap);
            int raw = 0, vb0 = g_vr_blit;
            if (sb_photo_active()) { show = sb_photo_pause_image(show, &raw); } /* W195 PHOTO-MODE: Vorschau/Aufnahme */
            if (raw) { g_vr_blit = 1; } /* Aussenwelt ohne Endbild-Nachbearbeitung (Cockpit-Licht, Bildraender ...) */
            sb_render_blit(show);
            g_vr_blit = vb0;
        }
        sb_render_present_raw();
        sb_ctx_leave();
        return;
    }
    if (sb_photo_active()) { (void)sb_photo_pause_image((ID3D11Resource *)g_pause_snap, NULL); } /* W195: x1 aus der Kopie */
    {
        ID3D11Resource *bb = NULL;
        ID3D11RenderTargetView_GetResource(g_rtv, &bb);
        if (bb) {
            ID3D11DeviceContext_CopyResource(g_ctx, bb, (ID3D11Resource *)g_pause_snap);
            ID3D11Resource_Release(bb);
        }
    }
    sb_wm_draw(g_rtv, 0); /* W219 WATERMARK (Pause; nicht im Fotomodus) */
    if (g_cfg.overlay && (sb_ovl_open() || sb_clip_active())) { sb_ovl_draw(g_rtv, 0); }
    if (sb_phud_visible()) { sb_phud_draw(g_rtv, 0); }
    if (g_cfg.overlay_test[0]) { /* TEST(W118): Overlay-Bild */
        char onm[48];
        if (sb_ovl_shot_due(onm, sizeof onm)) { sb_render_shot_name(onm, NULL); }
    }
    /* W200: measure pause presents also without an offscreen buffer. */
    sb_render_present_raw();
    sb_ctx_leave();
}

void sb_render_pause_end(void)
{
    if (!g_dev || !g_ctx) { return; }
    sb_ctx_enter();
    if (!g_swap_res && g_pause_snap && g_rtv) { /* BB ohne Menue zurueck (Spiel zeichnet darauf weiter) */
        ID3D11Resource *bb = NULL;
        ID3D11RenderTargetView_GetResource(g_rtv, &bb);
        if (bb) {
            ID3D11DeviceContext_CopyResource(g_ctx, bb, (ID3D11Resource *)g_pause_snap);
            ID3D11Resource_Release(bb);
        }
    }
    if (g_swap_res) { sb_interp_reset("Pause"); }
    sb_tex_pause_clear(); /* W193: Tausch-Tabelle (Cache haelt die neuen Texturen) */
    if (g_pl_n) {
        HLOG_I("PAUSE live: Ende (%u Neuzeichnungen, zuletzt %s)", g_pl_n, g_pl_use ? "Neuzeichnung" : "Kopie");
    }
    g_pl_state = 0;
    g_pl_use = 0;
    g_pl_n = 0;
    g_pl_nlog = 0;
    sb_ctx_leave();
}

static int sb_write_jpg(const char *path, const unsigned char *bgr, unsigned w, unsigned h);
/* W188 GAME-MENU (Feedback): Spielbild der Pause (nie der Desktop) als JPG
 * nach path. Unter ctx_lock aufrufen. 1 = geschrieben. */
int sb_render_pause_jpg(const char *path, unsigned *ow, unsigned *oh)
{
    ID3D11Texture2D *stg = NULL;
    D3D11_TEXTURE2D_DESC dd;
    D3D11_MAPPED_SUBRESOURCE map;
    unsigned char *px;
    unsigned x, y;
    int ok;
    if (!g_pause_snap || !path) { return 0; }
    ID3D11Texture2D_GetDesc(g_pause_snap, &dd);
    if (dd.Format != DXGI_FORMAT_R8G8B8A8_UNORM && dd.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        HLOG_W("FEEDBACK: Bildformat %d nicht unterstuetzt", (int)dd.Format);
        return 0;
    }
    dd.Usage = D3D11_USAGE_STAGING;
    dd.BindFlags = 0;
    dd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    dd.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &stg))) { return 0; }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)stg, (ID3D11Resource *)g_pause_snap);
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &map))) {
        ID3D11Texture2D_Release(stg);
        return 0;
    }
    px = (unsigned char *)malloc((size_t)dd.Width * dd.Height * 3u);
    if (!px) {
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)stg, 0);
        ID3D11Texture2D_Release(stg);
        return 0;
    }
    for (y = 0; y < dd.Height; y++) {
        const unsigned char *s = (const unsigned char *)map.pData + (size_t)y * map.RowPitch;
        unsigned char *d = px + (size_t)y * dd.Width * 3u;
        int bgra = dd.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
        for (x = 0; x < dd.Width; x++) {
            d[x * 3 + 0] = s[x * 4 + (bgra ? 0 : 2)];
            d[x * 3 + 1] = s[x * 4 + 1];
            d[x * 3 + 2] = s[x * 4 + (bgra ? 2 : 0)];
        }
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)stg, 0);
    ID3D11Texture2D_Release(stg);
    (void)sb_wm_blend_bgr(px, dd.Width, dd.Height); /* W219 WATERMARK: Version im Feedback-Screenshot */
    ok = sb_write_jpg(path, px, dd.Width, dd.Height);
    free(px);
    if (ow) { *ow = dd.Width; }
    if (oh) { *oh = dd.Height; }
    return ok;
}

/* M2c: Device-Health (GPU-Fault? S_OK=gesund). */
long sb_render_device_reason(void)
{
    if (!g_dev) { return -1; }
    return (long)ID3D11Device_GetDeviceRemovedReason(g_dev);
}

/* ASPECT (W18, Regel S3.10 "nie strecken"): zentriertes Rechteck im
 * Backbuffer, das das Spiel-Seitenverhaeltnis (fest 640x480 = 4:3, z.B.
 * 1440x1080 auf 2560x1080) unverzerrt traegt. 2D (RHW) wird dorthin
 * abgebildet (s. sb_im_submit), die Raender bleiben schwarz
 * (sb_aspect_bars). 3D bleibt Hor+ (Vollbild). Fest 4:3 (kein
 * Viewport-Folgen): das Spiel schaltet den Viewport pro Draw um
 * (1.0/1.213/1.333, s. aspect-Log) - Balken daraus wuerden flackern
 * und Inhalt fressen; 2D lebt immer im 640x480-Framebuffer. */
void sb_aspect_box(unsigned *bx, unsigned *by, unsigned *bw, unsigned *bh)
{
    float ga = 640.0f / 480.0f, ba;
    ba = (float)g_bb_w / (float)(g_bb_h ? g_bb_h : 1);
    if (ga >= ba) { /* Letterbox (oder Vollbild bei Gleichheit) */
        *bw = g_bb_w;
        *bh = (unsigned)((float)g_bb_w / ga + 0.5f);
        if (*bh < 1) { *bh = 1; }
        if (*bh > g_bb_h) { *bh = g_bb_h; }
        *bx = 0;
        *by = (g_bb_h - *bh) / 2;
    } else { /* Pillarbox */
        *bh = g_bb_h;
        *bw = (unsigned)((float)g_bb_h * ga + 0.5f);
        if (*bw < 1) { *bw = 1; }
        if (*bw > g_bb_w) { *bw = g_bb_w; }
        *bx = (g_bb_w - *bw) / 2;
        *by = 0;
    }
    { /* Box-Wechsel einmal loggen (Phase/Seitenverhaeltnis im Blick). */
        static unsigned lbx, lby, lbw, lbh;
        static int have;
        if (!have || lbx != *bx || lby != *by || lbw != *bw || lbh != *bh) {
            have = 1; lbx = *bx; lby = *by; lbw = *bw; lbh = *bh;
            HLOG_I("render: aspect2d box %ux%u@%u,%u (Spiel=%.3f, %s)",
                   *bw, *bh, *bx, *by, ga,
                   (ga < ba) ? "Pillarbox" : ((ga > ba) ? "Letterbox" : "Vollbild"));
        }
    }
}

/* ASPECT (W18): schwarze Balken ausserhalb der Spiel-Aspect-Box, direkt
 * per IM-Pipeline (Identity-MVP, Schwarz, Blend aus, kein Tiefentest).
 * Hinterlaesst keinen Zustand: Blend wird restauriert, der VB-Ring
 * fortgesetzt. Zwei Malstellen: nach TARGET-Clear (danach uebermalen
 * 3D-Draws der Mission die Balken korrekt) und vor Present/Screenshot
 * bei Frames ohne 3D-Draws (der Film hat kein Target-Clear). */
static void sb_aspect_bars(void)
{
    unsigned bx, by, bw, bh;
    im_vertex_t v[24];
    unsigned nv = 0, i;
    D3D11_MAPPED_SUBRESOURCE map;
    unsigned start = 0, stride = sizeof(im_vertex_t), offset = 0;
    ID3D11ShaderResourceView *nullsrv = NULL;
    unsigned sbon = 0, sbs = 5, sbd = 6;
    float *m;
    unsigned k;
    HRESULT hr;
    if (!g_im_ready || !g_dev) {
        return;
    }
    sb_aspect_box(&bx, &by, &bw, &bh);
    if (bw >= g_bb_w && bh >= g_bb_h) {
        return; /* Vollbild: keine Balken */
    }
    for (i = 0; i < 4; i++) {
        unsigned x0, y0, x1, y1; /* Balken-Rechteck in BB-Pixeln */
        float nx0, ny0, nx1, ny1; /* dasselbe in NDC */
        unsigned r;
        static const float cx[6] = { 0, 1, 1, 0, 1, 0 };
        static const float cy[6] = { 0, 0, 1, 0, 1, 1 };
        if (i == 0) { x0 = 0; x1 = bx; y0 = 0; y1 = g_bb_h; }
        else if (i == 1) { x0 = bx + bw; x1 = g_bb_w; y0 = 0; y1 = g_bb_h; }
        else if (i == 2) { x0 = bx; x1 = bx + bw; y0 = 0; y1 = by; }
        else { x0 = bx; x1 = bx + bw; y0 = by + bh; y1 = g_bb_h; }
        if (x1 <= x0 || y1 <= y0) {
            continue;
        }
        nx0 = (float)x0 / (float)g_bb_w * 2.0f - 1.0f;
        nx1 = (float)x1 / (float)g_bb_w * 2.0f - 1.0f;
        ny0 = 1.0f - (float)y0 / (float)g_bb_h * 2.0f;
        ny1 = 1.0f - (float)y1 / (float)g_bb_h * 2.0f;
        for (r = 0; r < 6; r++) {
            im_vertex_t *p = &v[nv++];
            p->pos[0] = cx[r] ? nx1 : nx0;
            p->pos[1] = cy[r] ? ny1 : ny0;
            p->pos[2] = 0.0f;
            p->pos[3] = 1.0f;
            p->col[0] = p->col[1] = p->col[2] = 0.0f;
            p->col[3] = 1.0f;
            p->uv[0] = p->uv[1] = 0.0f;
        }
    }
    if (nv == 0) {
        return;
    }
    if (g_cfg.im_ring && g_im_vb_pos + nv <= IM_VB_MAX) {
        hr = ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_im_vb, 0,
                                     g_im_vb_pos ? D3D11_MAP_WRITE_NO_OVERWRITE
                                                 : D3D11_MAP_WRITE_DISCARD,
                                     0, &map);
    } else {
        g_im_vb_pos = 0;
        hr = ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_im_vb, 0,
                                     D3D11_MAP_WRITE_DISCARD, 0, &map);
    }
    if (FAILED(hr)) {
        return;
    }
    start = g_im_vb_pos;
    memcpy((im_vertex_t *)map.pData + start, v, nv * sizeof v[0]);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_im_vb, 0);
    g_im_vb_pos = g_cfg.im_ring ? start + nv : 0;
    hr = ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_im_cb, 0,
                                 D3D11_MAP_WRITE_DISCARD, 0, &map);
    if (FAILED(hr)) {
        return;
    }
    /* Identity-MVP (Positionen sind bereits NDC), keine Textur. */
    m = (float *)map.pData;
    for (k = 0; k < 16; k++) { m[k] = (k % 5 == 0) ? 1.0f : 0.0f; }
    *(unsigned *)((unsigned char *)map.pData + 64) = 0;
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_im_cb, 0);
    sb_render_blend_state(&sbon, &sbs, &sbd);
    rs_bars = 1; /* W33: Balken immer deckend (Spiel-Maske gilt nicht) */
    sb_render_blend(27, 0);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, g_im_ds_off, 0);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, g_im_layout);
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &g_im_vb, &stride,
                                           &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx,
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_im_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 0, 1, &g_im_cb);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_im_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_im_cb);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &nullsrv);
    if (g_ip_recording) { /* FPS-INTERP (W69): Balken-Draw aufzeichnen */
        float idm[16];
        for (k = 0; k < 16; k++) { idm[k] = (k % 5 == 0) ? 1.0f : 0.0f; }
        sb_interp_rec_draw(v, nv, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, idm, 0, 0, 0xBA125u, 0, 0xFFFFFFFFu);
    }
    WD_CALL("draw_bars", g_ctx, ID3D11DeviceContext_Draw(g_ctx, nv, start));
    rs_bars = 0; /* W33 */
    sb_render_blend(27, (int)sbon);
    sb_render_blend(19, (int)sbs);
    sb_render_blend(20, (int)sbd);
}

const sb_eye *sb_render_eye(int idx)
{
    if (idx < 0 || idx > 1) {
        return NULL;
    }
    return &g_rig[idx];
}

unsigned sb_render_backbuffer_w(void)
{
    return g_bb_w;
}

unsigned sb_render_backbuffer_h(void)
{
    return g_bb_h;
}

/* Screenshot: Backbuffer -> Staging -> 32-bit-BMP (BGRA, R<->B-Tausch).
 * Pfad: shot_<frame>.bmp im Arbeitsverzeichnis. */
/* JPG via WIC (in Windows enthalten, keine Fremd-Lib): ~0,4 MB statt
 * ~11 MB BMP bei 2560x1080. bgr = 24-bit BGR, top-down. */
static int sb_write_jpg(const char *path, const unsigned char *bgr,
                        unsigned w, unsigned h)
{
    IWICImagingFactory *fac = NULL;
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    IPropertyBag2 *props = NULL;
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    WCHAR wpath[MAX_PATH];
    HRESULT hr;
    int ok = 0;
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);
    CoInitializeEx(NULL, COINIT_MULTITHREADED); /* S_FALSE/CHANGED_MODE ok */
    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL,
                          CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                          (void **)&fac);
    if (FAILED(hr)) goto done;
    hr = IWICImagingFactory_CreateStream(fac, &st);
    if (FAILED(hr)) goto done;
    hr = IWICStream_InitializeFromFilename(st, wpath, GENERIC_WRITE);
    if (FAILED(hr)) goto done;
    hr = IWICImagingFactory_CreateEncoder(fac, &GUID_ContainerFormatJpeg,
                                          NULL, &enc);
    if (FAILED(hr)) goto done;
    hr = IWICBitmapEncoder_Initialize(enc, (IStream *)st,
                                      WICBitmapEncoderNoCache);
    if (FAILED(hr)) goto done;
    hr = IWICBitmapEncoder_CreateNewFrame(enc, &fr, &props);
    if (FAILED(hr)) goto done;
    if (props) {
        PROPBAG2 opt;
        VARIANT v;
        memset(&opt, 0, sizeof opt);
        opt.pstrName = L"ImageQuality";
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.85f;
        IPropertyBag2_Write(props, 1, &opt, &v);
    }
    hr = IWICBitmapFrameEncode_Initialize(fr, props);
    if (FAILED(hr)) goto done;
    hr = IWICBitmapFrameEncode_SetSize(fr, w, h);
    if (FAILED(hr)) goto done;
    hr = IWICBitmapFrameEncode_SetPixelFormat(fr, &pf);
    if (FAILED(hr)) goto done;
    hr = IWICBitmapFrameEncode_WritePixels(fr, h, w * 3, w * 3 * h,
                                           (BYTE *)bgr);
    if (FAILED(hr)) goto done;
    hr = IWICBitmapFrameEncode_Commit(fr);
    if (FAILED(hr)) goto done;
    hr = IWICBitmapEncoder_Commit(enc);
    ok = SUCCEEDED(hr);
done:
    if (!ok) {
        HLOG_W("shot: JPG-Encode fehlgeschlagen (hr=0x%lX)", (long)hr);
    }
    if (props) IPropertyBag2_Release(props);
    if (fr) IWICBitmapFrameEncode_Release(fr);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (st) IWICStream_Release(st);
    if (fac) IWICImagingFactory_Release(fac);
    return ok;
}

int sb_render_write_jpg(const char *path, const unsigned char *bgr,
                        unsigned w, unsigned h)
{
    return sb_write_jpg(path, bgr, w, h);
}

static void sb_render_screenshot_ex(unsigned frame, const char *name, ID3D11Resource *src);
static void sb_render_screenshot(unsigned frame)
{
    sb_render_screenshot_ex(frame, NULL, NULL);
    if (g_cfg.shfx_ab) { /* DIAG(SHADE-FX,W121): dasselbe Bild + Cockpit-Nachbearbeitung */
        char nm[32];
        _snprintf_s(nm, sizeof nm, _TRUNCATE, "shot_%06u", frame);
        sb_shfx_ab_shot(nm);
    }
    if (g_cfg.edge_ab) { /* DIAG(EDGE-MODE,W107) */
        char nm[32];
        _snprintf_s(nm, sizeof nm, _TRUNCATE, "shot_%06u", frame);
        sb_edge_ab_shots(nm, NULL);
    }
}

/* FPS-INTERP (W69): Bild (src, NULL = Spiel-Backbuffer) als
 * shots/<name>.<jpg|bmp> (Zwischenbilder, Bildfolgen). */
void sb_render_shot_name(const char *name, ID3D11Resource *src)
{
    sb_render_screenshot_ex(g_present_no, name, src);
}

static void sb_render_screenshot_ex(unsigned frame, const char *name, ID3D11Resource *src)
{
    ID3D11Resource *res = NULL;
    ID3D11Texture2D *stg = NULL;
    D3D11_TEXTURE2D_DESC dd;
    D3D11_MAPPED_SUBRESOURCE map;
    HRESULT hr;
    int jpg = g_cfg.screenshot_jpg;
    /* RTV-Resource (GetBuffer(0) rotiert nach Present und zeigt evtl.
     * den falschen Buffer). */
    if (src) {
        res = src;
        ID3D11Resource_AddRef(res);
    } else {
        ID3D11RenderTargetView_GetResource(g_rtv, &res);
    }
    if (!res) {
        HLOG_W("shot: RTV-Resource fehlt");
        return;
    }
    memset(&dd, 0, sizeof dd);
    dd.Width = g_bb_w;
    dd.Height = g_bb_h;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_STAGING;
    dd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &stg);
    if (FAILED(hr)) {
        HLOG_W("shot: Staging fehlgeschlagen");
        ID3D11Resource_Release(res);
        return;
    }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)stg, res);
    ID3D11Resource_Release(res);
    hr = ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)stg, 0,
                                 D3D11_MAP_READ, 0, &map);
    if (FAILED(hr)) {
        HLOG_W("shot: Map fehlgeschlagen");
        ID3D11Texture2D_Release(stg);
        return;
    }
    {
        unsigned bpp = jpg ? 3u : 4u;
        unsigned row_bytes = dd.Width * bpp;
        unsigned img_bytes = row_bytes * dd.Height;
        unsigned char *px;
        unsigned y;
        char path[600];
        int ok = 0;
        if (name) {
            sprintf_s(path, sizeof path, "%s/shots/%s.%s", hcfg_out_dir(),
                      name, jpg ? "jpg" : "bmp");
        } else {
            sprintf_s(path, sizeof path, "%s/shots/shot_%06u.%s", hcfg_out_dir(),
                      frame, jpg ? "jpg" : "bmp");
        }
        px = (unsigned char *)malloc(img_bytes);
        if (!px) {
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)stg, 0);
            ID3D11Texture2D_Release(stg);
            return;
        }
        /* RGBA -> BGR(A); JPG top-down, BMP bottom-up. */
        for (y = 0; y < dd.Height; y++) {
            unsigned sy = jpg ? y : (dd.Height - 1 - y);
            unsigned char *src = (unsigned char *)map.pData + sy * map.RowPitch;
            unsigned char *dst = px + y * row_bytes;
            unsigned x;
            for (x = 0; x < dd.Width; x++) {
                dst[x * bpp + 0] = src[x * 4 + 2];
                dst[x * bpp + 1] = src[x * 4 + 1];
                dst[x * bpp + 2] = src[x * 4 + 0];
                if (!jpg) {
                    dst[x * 4 + 3] = 0xFF;
                }
            }
        }
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)stg, 0);
        ID3D11Texture2D_Release(stg);
        if (jpg) {
            ok = sb_write_jpg(path, px, dd.Width, dd.Height);
        } else {
            unsigned char hdr[54];
            FILE *f;
            memset(hdr, 0, sizeof hdr);
            hdr[0] = 'B';
            hdr[1] = 'M';
            *(unsigned *)(hdr + 2) = 54 + img_bytes;
            *(unsigned *)(hdr + 10) = 54;
            *(unsigned *)(hdr + 14) = 40;
            *(unsigned *)(hdr + 18) = dd.Width;
            *(unsigned *)(hdr + 22) = dd.Height;
            *(unsigned short *)(hdr + 26) = 1;
            *(unsigned short *)(hdr + 28) = 32;
            if (fopen_s(&f, path, "wb") == 0 && f) {
                fwrite(hdr, 1, sizeof hdr, f);
                fwrite(px, 1, img_bytes, f);
                fclose(f);
                ok = 1;
            }
        }
        if (ok) {
            HLOG_I("shot: %s (%ux%u) texd=%u notex=%u stage0=0x%X", path,
                   dd.Width, dd.Height, g_im_texd, g_im_notex, sb_tex_xptr());
        } else {
            HLOG_W("shot: Schreiben fehlgeschlagen (%s)", path);
        }
        free(px);
    }
}
