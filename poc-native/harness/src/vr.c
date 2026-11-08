/* vr.c - W134 VR-PROTO: Stereo-Cockpit mit Kopf-Tracking aus der Interp-Aufzeichnung.
 *
 * vr_enable=0 (Standard): sb_vr_init() kehrt sofort zurueck, alle anderen
 * Eintritte pruefen vr_mode (0) -> bitgleich zum bisherigen Pfad.
 *
 * Modi (vr_mode):
 *  1 = Desktop-Vorschau (ohne Brille): ip_show (render_interp.c) zeigt statt
 *      des Interp-Bildes beide Augen nebeneinander (vr_preview=sbs) bzw. ein
 *      Auge (left/right). Kopfpose simuliert (Ziffernblock 4/6/8/2, 5 = Mitte, bzw.
 *      vr_sim=script). Je Auge eigenes Seitenverhaeltnis (Projektion aus dem
 *      Augen-Viewport, nie gestreckt).
 *  2 = OpenXR: eigener Mini-Loader (ActiveRuntime-Manifest -> Runtime-DLL ->
 *      xrNegotiateLoaderRuntimeInterface -> xrGetInstanceProcAddr), Session
 *      mit XR_KHR_D3D11_enable, LOCAL-Raum, PRIMARY_STEREO, eine Swapchain
 *      (beide Augen nebeneinander, imageRect je Auge), Projection-Layer.
 *      Eigener VR-Thread: xrWaitFrame bestimmt den Takt; je Bild wird das
 *      Tick-Paar fuer den Anzeigezeitpunkt unter ctx_lock wiedergegeben.
 *      Fenster zeigt das normale Spielbild.
 *      W145 VR-DEADLOCK: Die Runtime bekommt ein EIGENES D3D11-Device (gleicher
 *      Adapter) - sie sieht nie das Spiel-Device/den Spiel-Context. Das
 *      Augenziel ist eine geteilte Textur (Keyed Mutex): Spiel-Device rendert
 *      unter ctx_lock (Schluessel 0 -> 1), VR-Device kopiert ohne ctx_lock in
 *      das Swapchain-Bild (1 -> 0). Kein OpenXR-Aufruf (WaitFrame, BeginFrame,
 *      Acquire/Wait/Release, EndFrame) laeuft unter ctx_lock: blockiert die
 *      Runtime (VDXR wartet auf GPU-Zaeune/Compositor), steht nur der
 *      VR-Thread, das Spiel laeuft auf dem Monitor weiter.
 * Fehlt die Runtime oder scheitert ein Schritt: eine Log-Zeile
 * "VR: OpenXR nicht verfuegbar (...)", dann Vorschau (falls vr_preview
 * sbs/left/right) bzw. VR aus - das Spiel laeuft normal weiter.
 *
 * Koordinaten: Sitz-Sicht = Sichtraum der Spielkamera (RH, -z vorne, Spiel-
 * Einheiten). Augenpose im Sitz = Augenpose (Tracking) x Recentering^-1,
 * Verschiebung x world_scale + Sitzhoehe. Matrizen row-major, Zeilenvektor
 * (p' = p * M) wie im restlichen Renderer. */
#include "render_int.h"
#include "sb_lang.h" /* W142 L10N */
#include "vr.h"
#include "vr_openxr.h"
#include "sb_overlay.h" /* W197 VR-MENU: Menue-Panel */
#include "sb_perfhud.h" /* W197 VR-MENU: F2-Panel */
#include <math.h>
#include <tlhelp32.h>

#define VR_PI 3.14159265358979f

static int vr_mode;          /* 0 aus, 1 Vorschau, 2 OpenXR */
static int vr_prev_kind;     /* 0 sbs, 1 left, 2 right, 3 aus (nur Fenster normal) */
static int vr_sim_kind;      /* 0 keys, 1 script, 2 still */
static LARGE_INTEGER vr_qpf;
static LONGLONG vr_qt0;

/* Augenziel (beide Augen nebeneinander bzw. eins) */
static ID3D11Texture2D *vr_tex, *vr_dtex;
static ID3D11RenderTargetView *vr_rtv;
static ID3D11DepthStencilView *vr_dsv;
static unsigned vr_tw, vr_th;
static int vr_tex_shared;          /* W145: vr_tex mit Keyed Mutex (OpenXR-Modus) */
static IDXGIKeyedMutex *vr_km;     /* W145: Keyed Mutex des Augenziels (Spiel-Device) */
static ID3D11Texture2D *xr_shtex;  /* W145: dasselbe Augenziel, geoeffnet auf dem VR-Device */
static IDXGIKeyedMutex *xr_km;     /* W145: Keyed Mutex (VR-Device) */
static int xr_km_pending;          /* W145: Spiel gab Schluessel 1 ab, VR-Device hat ihn noch nicht genommen */
static ID3D11Device *xr_dev;       /* W145: eigenes D3D11-Device fuer die Runtime */
static ID3D11DeviceContext *xr_ctx;

/* Kopf-Simulation (Vorschau) und Recentering */
static float vr_yaw, vr_pitch, vr_roll, vr_hpos[3];
static volatile LONG vr_rc_req;
static volatile LONG vr_rc_gen;      /* W197: zaehlt ausgefuehrte Recenterings (Menue-Flaeche neu vor den Blick) */
static float vr_rc_yaw, vr_rc_pos[3];
static char vr_rc_why[24];
static int vr_keys; /* Bit 0 links, 1 rechts, 2 hoch, 3 runter */
static unsigned vr_frames, vr_shots_n;
static LONGLONG vr_last_q;
static float vr_auto_d2d;

/* Statistik je Fenster */
static unsigned st_fr, st_n3d, st_n2d, st_np2, st_nrq, st_nrqfb;
static double st_ms, st_ms_max, st_t;
static LONGLONG st_q0;

static LONGLONG vr_now(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}

/* ---------- Matrizen (row-major, Zeilenvektor) ---------- */

static void m4_mul(float r[16], const float a[16], const float b[16])
{
    float t[16];
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            t[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
        }
    }
    memcpy(r, t, sizeof t);
}

/* Pose (Quaternion + Position) -> Matrix lokal -> Eltern (Zeilenvektor). */
static void m4_pose(float m[16], const float q[4], const float p[3])
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    /* Spaltenvektor-Rotation R; Zeilenvektor-Matrix = R^T */
    float r00 = 1 - 2 * (y * y + z * z), r01 = 2 * (x * y - z * w), r02 = 2 * (x * z + y * w);
    float r10 = 2 * (x * y + z * w), r11 = 1 - 2 * (x * x + z * z), r12 = 2 * (y * z - x * w);
    float r20 = 2 * (x * z - y * w), r21 = 2 * (y * z + x * w), r22 = 1 - 2 * (x * x + y * y);
    m[0] = r00; m[1] = r10; m[2] = r20; m[3] = 0;
    m[4] = r01; m[5] = r11; m[6] = r21; m[7] = 0;
    m[8] = r02; m[9] = r12; m[10] = r22; m[11] = 0;
    m[12] = p[0]; m[13] = p[1]; m[14] = p[2]; m[15] = 1;
}

/* Inverse einer starren Matrix (Rotation + Verschiebung, Zeilenvektor). */
static void m4_rigid_inv(float r[16], const float m[16])
{
    float t[16];
    int i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) { t[i * 4 + j] = m[j * 4 + i]; }
        t[i * 4 + 3] = 0;
    }
    for (j = 0; j < 3; j++) {
        t[12 + j] = -(m[12] * t[j] + m[13] * t[4 + j] + m[14] * t[8 + j]);
    }
    t[15] = 1;
    memcpy(r, t, sizeof t);
}

static void q_from_euler(float q[4], float yaw, float pitch, float roll)
{
    /* RH, y oben: erst Gieren (um y), dann Nicken (um x), dann Rollen (um z) */
    float cy = cosf(yaw * 0.5f), sy = sinf(yaw * 0.5f);
    float cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);
    float cr = cosf(roll * 0.5f), sr = sinf(roll * 0.5f);
    /* q = qy * qx * qz */
    float ax = cy * sp, ay = sy * cp, az = -sy * sp, aw = cy * cp; /* qy*qx */
    q[0] = ax * cr + ay * sr;
    q[1] = ay * cr - ax * sr;
    q[2] = aw * sr + az * cr;
    q[3] = aw * cr - az * sr;
}

static float q_yaw(const float q[4])
{
    /* Blickrichtung -z rotieren, Gierwinkel in der xz-Ebene */
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float fx = -(2 * (x * z + y * w)), fz = -(1 - 2 * (x * x + y * y));
    return atan2f(-fx, -fz);
}

static float q_pitch(const float q[4])
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float fy = -(2 * (y * z - x * w));
    if (fy > 1) { fy = 1; } else if (fy < -1) { fy = -1; }
    return asinf(fy);
}

/* Projektion aus Sichtwinkeln (RH, D3D-Tiefe 0..1, Zeilenvektor). */
static void m4_proj_fov(float m[16], float tl, float tr, float tu, float td, float n, float f)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = 2.0f / (tr - tl);
    m[8] = (tr + tl) / (tr - tl);
    m[5] = 2.0f / (tu - td);
    m[9] = (tu + td) / (tu - td);
    m[10] = f / (n - f);
    m[14] = n * f / (n - f);
    m[11] = -1.0f;
}

/* Spiel-Projektion: b = g_proj_game.m[5] (Mission: 2,414 = 45 Grad vertikal),
 * a = b * 3/4 (VS-Projektion 4:3), Hor+ vk = (4/3)/(BB-Seitenverhaeltnis). */
static void vr_game_proj(float *ia, float *ib, float *fov_v)
{
    float b = g_proj_game.m[5];
    float oa = (float)g_bb_w / (float)(g_bb_h ? g_bb_h : 1);
    float vk = (4.0f / 3.0f) / (oa > 0.0f ? oa : 1.0f);
    float a;
    if (!(b > 0.3f && b < 20.0f)) { b = 2.41421356f; }
    a = b * 0.75f;
    *ia = 1.0f / (a * vk);
    *ib = 1.0f / b;
    *fov_v = 2.0f * atanf(1.0f / b);
}

/* Augenpose (Tracking-Raum, Quaternion + Position in Metern) -> E = Sicht x Projektion. */
static void vr_eye_matrix(float E[16], const float q[4], const float pos[3], float tl, float tr, float tu, float td)
{
    float M[16], RC[16], RCI[16], V[16], P[16], rq[4], rp[3];
    float s = (float)g_cfg.vr_world_scale / 100.0f;
    m4_pose(M, q, pos);
    q_from_euler(rq, vr_rc_yaw, 0, 0);
    rp[0] = vr_rc_pos[0]; rp[1] = vr_rc_pos[1]; rp[2] = vr_rc_pos[2];
    m4_pose(RC, rq, rp);
    m4_rigid_inv(RCI, RC);
    m4_mul(M, M, RCI);
    /* Verschiebung -> Spiel-Einheiten, Sitzhoehe */
    if (s <= 0.0f) { s = 1.0f; }
    M[12] *= s;
    M[13] = (M[13] + (float)g_cfg.vr_seat_height / 1000.0f) * s;
    M[14] = (M[14] - (float)g_cfg.vr_seat_forward / 1000.0f) * s; /* W183: Sitz nach vorne (-z) */
    m4_rigid_inv(V, M);
    /* Near/Far wie die Spielkamera (g_proj_game 0,1/100): Tiefe der linear
     * projizierten Shading-Draws = Original bei geradem Kopf (W134_e3) */
    m4_proj_fov(P, tl, tr, tu, td, 0.1f, 100.0f);
    m4_mul(E, V, P);
}

/* W183: Kopfposition im Sitz-Raum (Meter, vor Massstab; Sitz-Versatz eingerechnet) bei Mitte (yaw, rp) */
static void vr_seat_head(const float pos[3], float yaw, const float rp[3], float out[3])
{
    float dx = pos[0] - rp[0], dy = pos[1] - rp[1], dz = pos[2] - rp[2];
    float c = cosf(yaw), sn = sinf(yaw);
    /* Rueckdrehung um -yaw (RH, y oben): x' = c*x - s*z, z' = s*x + c*z */
    out[0] = c * dx - sn * dz;
    out[1] = dy + (float)g_cfg.vr_seat_height / 1000.0f;
    out[2] = sn * dx + c * dz - (float)g_cfg.vr_seat_forward / 1000.0f;
}

static void vr_recenter_apply(const float q[4], const float pos[3])
{
    float before[3], after[3];
    vr_seat_head(pos, vr_rc_yaw, vr_rc_pos, before);
    vr_rc_yaw = q_yaw(q);
    vr_rc_pos[0] = pos[0]; vr_rc_pos[1] = pos[1]; vr_rc_pos[2] = pos[2];
    InterlockedIncrement(&vr_rc_gen); /* W197 */
    vr_seat_head(pos, vr_rc_yaw, vr_rc_pos, after);
    HLOG_I("VR recenter (%s): Kopf yaw=%.1f pitch=%.1f pos=(%.3f,%.3f,%.3f) -> neue Mitte",
           vr_rc_why[0] ? vr_rc_why : "?", vr_rc_yaw * 180.0f / VR_PI, q_pitch(q) * 180.0f / VR_PI,
           pos[0], pos[1], pos[2]);
    /* W183 VR-FIX3: Reposition = Richtung UND Position zurueck auf die Sitzposition */
    HLOG_I("VR reposition (%s): Kopf im Sitz vorher (%.3f,%.3f,%.3f) -> nachher (%.3f,%.3f,%.3f) m = Sitzposition (vor %d mm, hoch %d mm)",
           vr_rc_why[0] ? vr_rc_why : "?", before[0], before[1], before[2], after[0], after[1], after[2],
           g_cfg.vr_seat_forward, g_cfg.vr_seat_height);
}

void sb_vr_recenter(const char *why)
{
    if (!vr_mode) { return; }
    strncpy_s(vr_rc_why, sizeof vr_rc_why, why ? why : "?", _TRUNCATE);
    InterlockedExchange(&vr_rc_req, 1);
}

/* ---------- Augenziel ---------- */

static int vr_ensure_target(unsigned w, unsigned h)
{
    D3D11_TEXTURE2D_DESC td;
    int shared = (vr_mode == 2) && xr_dev != NULL; /* W145: OpenXR -> geteilt mit dem VR-Device */
    if (vr_tex && vr_tw == w && vr_th == h && vr_tex_shared == shared) { return 1; }
    if (xr_km) { IDXGIKeyedMutex_Release(xr_km); xr_km = NULL; }
    xr_km_pending = 0;
    if (xr_shtex) { ID3D11Texture2D_Release(xr_shtex); xr_shtex = NULL; }
    if (vr_km) { IDXGIKeyedMutex_Release(vr_km); vr_km = NULL; }
    vr_tex_shared = 0;
    if (vr_rtv) { ID3D11RenderTargetView_Release(vr_rtv); vr_rtv = NULL; }
    if (vr_dsv) { ID3D11DepthStencilView_Release(vr_dsv); vr_dsv = NULL; }
    if (vr_tex) { ID3D11Texture2D_Release(vr_tex); vr_tex = NULL; }
    if (vr_dtex) { ID3D11Texture2D_Release(vr_dtex); vr_dtex = NULL; }
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = shared ? D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX : 0;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &vr_tex))) { vr_tex = NULL; return 0; }
    if (shared) {
        IDXGIResource *res = NULL;
        HANDLE hs = NULL;
        if (FAILED(ID3D11Texture2D_QueryInterface(vr_tex, &IID_IDXGIKeyedMutex, (void **)&vr_km)) || !vr_km ||
            FAILED(ID3D11Texture2D_QueryInterface(vr_tex, &IID_IDXGIResource, (void **)&res)) || !res ||
            FAILED(IDXGIResource_GetSharedHandle(res, &hs)) || !hs ||
            FAILED(ID3D11Device_OpenSharedResource(xr_dev, hs, &IID_ID3D11Texture2D, (void **)&xr_shtex)) || !xr_shtex ||
            FAILED(ID3D11Texture2D_QueryInterface(xr_shtex, &IID_IDXGIKeyedMutex, (void **)&xr_km)) || !xr_km) {
            if (res) { IDXGIResource_Release(res); }
            { static unsigned nsh; if (nsh++ < 2u) { HLOG_W("VR: geteiltes Augenziel %ux%u nicht moeglich (Keyed Mutex) -> kein VR-Bild", w, h); } }
            ID3D11Texture2D_Release(vr_tex); vr_tex = NULL;
            return 0;
        }
        IDXGIResource_Release(res);
        vr_tex_shared = 1;
    }
    td.MiscFlags = 0;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &vr_dtex))) { vr_dtex = NULL; return 0; }
    if (FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)vr_tex, NULL, &vr_rtv))) { return 0; }
    if (FAILED(ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource *)vr_dtex, NULL, &vr_dsv))) { return 0; }
    vr_tw = w; vr_th = h;
    HLOG_I("VR: Augenziel %ux%u (%s)", w, h, vr_mode == 2 ? "OpenXR, beide Augen nebeneinander, geteilt mit dem VR-Device" :
           (vr_prev_kind == 0 ? "Vorschau beide Augen" : "Vorschau ein Auge"));
    return 1;
}

static void vr_stat_add(const sb_vr_pass_t *p, double ms, float t)
{
    double win;
    LONGLONG now = vr_now();
    if (!st_q0) { st_q0 = now; }
    st_fr++;
    st_n3d += p->n3d; st_n2d += p->n2d; st_np2 += p->np2; st_nrq += p->nrq; st_nrqfb += p->nrqfb;
    st_ms += ms; if (ms > st_ms_max) { st_ms_max = ms; }
    st_t += t;
    win = (double)(now - st_q0) / (double)vr_qpf.QuadPart;
    if (win >= 5.0 && g_cfg.vr_diag >= 1) {
        HLOG_I("VR stat: modus=%s bilder=%u rate=%.1f/s (je Auge, %d Augen) wiedergabe=%.2fms(max %.2f) t=%.2f "
               "draws/Bild 3d=%u 2d=%u p2=%u shading=%u(ersatz %u) D=%.3f kopf yaw=%.1f pitch=%.1f mitte yaw=%.1f",
               vr_mode == 2 ? "openxr" : (vr_prev_kind == 0 ? "vorschau-sbs" : "vorschau-auge"),
               st_fr, (double)st_fr / win, vr_prev_kind == 0 || vr_mode == 2 ? 2 : 1,
               st_ms / st_fr, st_ms_max, st_t / st_fr,
               st_n3d / st_fr, st_n2d / st_fr, st_np2 / st_fr, st_nrq / st_fr, st_nrqfb / st_fr,
               p->d2d, vr_yaw * 180.0f / VR_PI, vr_pitch * 180.0f / VR_PI, vr_rc_yaw * 180.0f / VR_PI);
    }
    if (win >= 5.0) {
        st_fr = 0; st_n3d = st_n2d = st_np2 = st_nrq = st_nrqfb = 0;
        st_ms = st_ms_max = st_t = 0;
        st_q0 = now;
    }
}

/* Ebene D der 2D-Draws (HUD, Monitor-Komposition) und Basis des P2-Cockpit-
 * Reliefs: vr_monitor_depth (mm), Standard 800 mm (Spiel-Einheit ~ 1 m). */
static float vr_d2d(void)
{
    return (float)(g_cfg.vr_monitor_depth > 0 ? g_cfg.vr_monitor_depth : 800) / 1000.0f;
}

/* P2-Cockpit-Relief (W134-Befund: die Cockpit-Draws sind VSH-P2 mit Clip-w ~ 1e5,
 * also im Original praktisch orthografisch - keine echte Tiefe). Tiefe je
 * Vertex d = D + rl * (z/w - z0): z0 = Mittel, rl so, dass die halbe z/w-Spanne
 * der ersten Wiedergabe vr_relief mm ergibt. Einmal bestimmt, Log. */
static float vr_rz0, vr_rl;
static int vr_relief_done;
static int vr_fcmp(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void vr_auto_update(sb_vr_pass_t *p)
{
    float z0, half, lo, hi;
    if (vr_relief_done || p->zs_n < 16u) { return; }
    vr_relief_done = 1;
    /* Perzentile statt Min/Max: einzelne P2-Draws liegen bei z/w ~ 1 (Monitor-Hintergrund) */
    qsort(p->zs, p->zs_n, sizeof p->zs[0], vr_fcmp);
    lo = p->zs[p->zs_n * 5u / 100u];
    z0 = p->zs[p->zs_n / 2u];
    hi = p->zs[p->zs_n * 95u / 100u];
    half = hi - z0;
    if (z0 - lo > half) { half = z0 - lo; }
    vr_rz0 = z0;
    vr_rl = half > 1e-6f ? ((float)g_cfg.vr_relief / 1000.0f) / half : 0.0f;
    vr_auto_d2d = p->p2w_sum / (float)p->p2w_n;
    if (p->ws_n >= 8u) { /* W164: Tiefe des 3D-Pfads (= Cockpit-Mesh, metrisch) */
        qsort(p->ws, p->ws_n, sizeof p->ws[0], vr_fcmp);
        HLOG_I("VR: 3D-Pfad (Cockpit-Mesh, echte Perspektive): Clip-w P5=%.3f Median=%.3f min=%.3f (%u Stichproben) - "
               "Tiefe %s, P2-Instrumente %s", p->ws[p->ws_n * 5u / 100u], p->ws[p->ws_n / 2u], p->w3_min, p->ws_n,
               g_cfg.vr_world_depth ? "affin aus der Augenprojektion" : "Original-z/w (W134)",
               g_cfg.vr_inst_depth ? "metrisch aus z/w (z/w > 0,2)" : "im Relief (W134)");
    }
    HLOG_I("VR: Cockpit-Ebene D=%.3f, Relief %d mm: z/w Median=%.5f P5..P95=[%.5f,%.5f] (min %.5f max %.5f) -> %.2f Einheiten je z/w "
           "(%u Stichproben, Clip-w Mittel %.0f = fast orthografisch; naechste 3D-Geometrie w=%.3f)",
           vr_d2d(), g_cfg.vr_relief, z0, lo, hi, p->p2z_min, p->p2z_max, vr_rl, p->zs_n, vr_auto_d2d, p->w3_min);
}

/* ---------- Desktop-Vorschau ---------- */

static void vr_sim_update(void)
{
    LONGLONG now = vr_now();
    float dt = vr_last_q ? (float)((double)(now - vr_last_q) / (double)vr_qpf.QuadPart) : 0.0f;
    vr_last_q = now;
    if (dt > 0.1f) { dt = 0.1f; }
    if (vr_sim_kind == 1) {
        /* Skript (deterministisch je VR-Bild, 90 Bilder = 1 s): Drehen +-35 Grad
         * (Periode 4 s), Neigen +-12 Grad (Periode 2,7 s), leichtes Seitwaerts-
         * Schieben; Recentering bei Bild 300. */
        float f = (float)vr_frames;
        vr_yaw = 35.0f * VR_PI / 180.0f * sinf(2.0f * VR_PI * f / 360.0f);
        vr_pitch = 12.0f * VR_PI / 180.0f * sinf(2.0f * VR_PI * f / 240.0f);
        vr_roll = 0.0f;
        vr_hpos[0] = 0.03f * sinf(2.0f * VR_PI * f / 360.0f);
        vr_hpos[1] = 0.0f;
        vr_hpos[2] = 0.0f;
        if (vr_frames == 300u) { sb_vr_recenter("skript"); }
    } else if (vr_sim_kind == 0) {
        float rate = 60.0f * VR_PI / 180.0f;
        if (vr_keys & 1) { vr_yaw += rate * dt; }
        if (vr_keys & 2) { vr_yaw -= rate * dt; }
        if (vr_keys & 4) { vr_pitch += rate * dt; }
        if (vr_keys & 8) { vr_pitch -= rate * dt; }
        if (vr_pitch > 1.4f) { vr_pitch = 1.4f; } else if (vr_pitch < -1.4f) { vr_pitch = -1.4f; }
    }
}

/* Pass fuer die Vorschau: Kopfpose simuliert, Augen +-IPD/2, symmetrisches
 * Sichtfeld aus dem Augen-Viewport (nie gestreckt). */
static void vr_preview_pass(sb_vr_pass_t *p)
{
    float q[4], fov_v, tu, tr, aspect, ipd = (float)g_cfg.vr_ipd / 10000.0f;
    int e;
    memset(p, 0, sizeof *p);
    vr_game_proj(&p->ia, &p->ib, &fov_v);
    if (g_cfg.vr_fov > 0) { fov_v = (float)g_cfg.vr_fov * VR_PI / 180.0f; }
    p->d2d = vr_d2d();
    p->rz0 = vr_rz0;
    p->rl = vr_rl;
    p->rmax = (float)g_cfg.vr_relief / 1000.0f;
    p->wantz = !vr_relief_done;
    p->rtv = vr_rtv;
    p->dsv = vr_dsv;
    p->neyes = (vr_prev_kind == 0) ? 2 : 1;
    p->wfix = g_cfg.vr_world_depth ? 1 : 0; /* W164 */
    p->ifix = g_cfg.vr_inst_depth ? 1 : 0;
    { static int dumped; if (g_cfg.vr_diag >= 2 && !dumped) { dumped = 1; p->dump = 120; } }
    q_from_euler(q, vr_yaw, vr_pitch, vr_roll);
    if (InterlockedExchange(&vr_rc_req, 0)) { vr_recenter_apply(q, vr_hpos); }
    for (e = 0; e < p->neyes; e++) {
        float pos[3], off[3], R[16];
        int eye = (p->neyes == 2) ? e : (vr_prev_kind == 2 ? 1 : 0);
        p->vp[e][0] = (p->neyes == 2) ? (float)(e * (vr_tw / 2u)) : 0.0f;
        p->vp[e][1] = 0.0f;
        p->vp[e][2] = (p->neyes == 2) ? (float)(vr_tw / 2u) : (float)vr_tw;
        p->vp[e][3] = (float)vr_th;
        aspect = p->vp[e][2] / p->vp[e][3];
        tu = tanf(fov_v * 0.5f);
        tr = tu * aspect;
        /* Augenversatz im Kopf-Raum */
        off[0] = (eye == 0 ? -0.5f : 0.5f) * ipd; off[1] = 0.0f; off[2] = 0.0f;
        m4_pose(R, q, vr_hpos);
        pos[0] = off[0] * R[0] + off[1] * R[4] + off[2] * R[8] + R[12];
        pos[1] = off[0] * R[1] + off[1] * R[5] + off[2] * R[9] + R[13];
        pos[2] = off[0] * R[2] + off[1] * R[6] + off[2] * R[10] + R[14];
        vr_eye_matrix(p->E[e], q, pos, -tr, tr, tu, -tu);
    }
}

int sb_vr_preview_active(void)
{
    return vr_mode == 1;
}

int sb_vr_preview_show(void *A, void *B, float t)
{
    sb_vr_pass_t p;
    LONGLONG q0 = vr_now();
    float tout = t;
    extern int g_vr_blit;
    if (vr_mode != 1 || !g_dev) { return 0; }
    if (!vr_ensure_target(g_bb_w, g_bb_h)) { return 0; }
    vr_sim_update();
    vr_preview_pass(&p);
    if (!sb_interp_vr_replay(&p, A, B, t, 0, &tout)) { return 0; }
    vr_frames++;
    vr_auto_update(&p);
    g_vr_blit = 1;
    sb_render_blit((ID3D11Resource *)vr_tex);
    g_vr_blit = 0;
    if (g_cfg.vr_shots > 0 && vr_shots_n < 24u &&
        ((vr_frames % (unsigned)g_cfg.vr_shots) == 0u || (vr_sim_kind == 1 && (vr_frames == 299u || vr_frames == 302u)))) {
        char nm[64];
        vr_shots_n++;
        _snprintf_s(nm, sizeof nm, _TRUNCATE, "vr_%05u_%s", vr_frames, vr_prev_kind == 0 ? "sbs" : "auge");
        sb_render_shot_name(nm, (ID3D11Resource *)vr_tex);
        if (g_cfg.vr_diag >= 2 && tout >= 1.0f) { /* DIAG: Mono-Bild desselben Ticks (Korrektheitsanker) */
            char mn[64];
            _snprintf_s(mn, sizeof mn, _TRUNCATE, "vr_%05u_mono", vr_frames);
            sb_interp_vr_mono_shot(B, mn);
        }
        HLOG_I("VR shot %s: kopf yaw=%.1f pitch=%.1f pos=(%.3f,%.3f,%.3f) mitte yaw=%.1f t=%.2f draws 3d=%u 2d=%u p2=%u",
               nm, vr_yaw * 180.0f / VR_PI, vr_pitch * 180.0f / VR_PI, vr_hpos[0], vr_hpos[1], vr_hpos[2],
               vr_rc_yaw * 180.0f / VR_PI, tout, p.n3d, p.n2d, p.np2);
    }
    vr_stat_add(&p, (double)(vr_now() - q0) * 1000.0 / (double)vr_qpf.QuadPart, tout);
    return 1;
}

/* ---------- W164 VR-FIX2: Vorschau im OpenXR-Modus, Leinwand ----------
 * Vorschau (vr_mode 2, vr_preview sbs/left/right): der VR-Thread kopiert unter
 * ctx_lock das eingereichte Augenbild (gleiche Posen, gleiche Projektion wie
 * die Swapchain) nach vr_pv_tex; sb_vr_after_blit zeichnet es ins Fenster,
 * Seitenverhaeltnis erhalten (schwarze Balken, nie gestreckt).
 * Leinwand (vr_canvas=1): ausserhalb der Mission (keine Interp-Aufzeichnung)
 * kopiert sb_vr_after_blit das gezeigte Monitorbild (inkl. ESC-Menue) nach
 * vr_cv_tex; der VR-Thread zeichnet es je Auge als Rechteck im Sitz-Raum
 * (Abstand vr_canvas_dist, Hoehe vr_canvas_size, Breite = Hoehe x
 * Bild-Seitenverhaeltnis), ausgerichtet per Recentering. */
static ID3D11Texture2D *vr_pv_tex, *vr_cv_tex;
static ID3D11ShaderResourceView *vr_pv_srv, *vr_cv_srv;
static ID3D11RenderTargetView *vr_pv_rtv; /* W183: Spiegel wird skaliert hineingezeichnet */
static ID3D11Texture2D *vr_pvs_tex;        /* W183: Kopie des gespiegelten Auges (ungeteilt) als Quelle der Skalierung */
static ID3D11ShaderResourceView *vr_pvs_srv;
static unsigned vr_pvs_w, vr_pvs_h;
static volatile LONGLONG vr_last_xr_q;    /* W183: letztes eingereichtes VR-Bild (QPC, 0 = keins) */
static char vr_mir_desc[200];             /* W183: zuletzt gemeldeter Spiegel-Modus */
static unsigned vr_pv_w, vr_pv_h, vr_cv_w, vr_cv_h;
static volatile LONG vr_pv_valid, vr_cv_valid;
static unsigned vr_pv_frame, vr_pv_logged;
static float vr_pv_pose[7], vr_pv_lpose[7];
static int vr_cv_on = -1;
static ID3D11VertexShader *vq_vs;
static ID3D11PixelShader *vq_ps;
static ID3D11InputLayout *vq_lay;
static ID3D11Buffer *vq_vb;
static ID3D11SamplerState *vq_smp;
static ID3D11RasterizerState *vq_rs;
static int vq_done;

static int vq_init(void)
{
    static const char *vs =
        "struct VI { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
        "struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VO main(VI i) { VO o; o.pos = i.pos; o.uv = i.uv; return o; }\n";
    static const char *ps =
        "Texture2D t0 : register(t0); SamplerState s0 : register(s0);\n"
        "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return float4(t0.Sample(s0, uv).rgb, 1); }\n";
    ID3DBlob *b = NULL;
    D3D11_INPUT_ELEMENT_DESC ie[2];
    D3D11_BUFFER_DESC bd;
    D3D11_SAMPLER_DESC sd;
    D3D11_RASTERIZER_DESC rd;
    if (vq_done) { return vq_vs && vq_ps && vq_lay && vq_vb && vq_smp && vq_rs; }
    vq_done = 1;
    memset(ie, 0, sizeof ie);
    ie[0].SemanticName = "POSITION"; ie[0].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; ie[0].AlignedByteOffset = 0;
    ie[1].SemanticName = "TEXCOORD"; ie[1].Format = DXGI_FORMAT_R32G32_FLOAT; ie[1].AlignedByteOffset = 16;
    if (sb_im_compile(vs, "vs_4_0", &b)) {
        ID3D11Device_CreateVertexShader(g_dev, b->lpVtbl->GetBufferPointer(b), b->lpVtbl->GetBufferSize(b), NULL, &vq_vs);
        ID3D11Device_CreateInputLayout(g_dev, ie, 2, b->lpVtbl->GetBufferPointer(b), b->lpVtbl->GetBufferSize(b), &vq_lay);
        b->lpVtbl->Release(b);
        b = NULL;
    }
    if (sb_im_compile(ps, "ps_4_0", &b)) {
        ID3D11Device_CreatePixelShader(g_dev, b->lpVtbl->GetBufferPointer(b), b->lpVtbl->GetBufferSize(b), NULL, &vq_ps);
        b->lpVtbl->Release(b);
    }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 6u * 6u * sizeof(float);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &vq_vb))) { vq_vb = NULL; }
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(ID3D11Device_CreateSamplerState(g_dev, &sd, &vq_smp))) { vq_smp = NULL; }
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(ID3D11Device_CreateRasterizerState(g_dev, &rd, &vq_rs))) { vq_rs = NULL; }
    HLOG_I("VR: Vorschau/Leinwand-Shader %s", (vq_vs && vq_ps && vq_lay && vq_vb && vq_smp && vq_rs) ? "bereit" : "FEHLER");
    return vq_vs && vq_ps && vq_lay && vq_vb && vq_smp && vq_rs;
}

/* 2 Dreiecke (6 Ecken: x y z w u v, Clip-Raum) mit srv auf rtv im Viewport vp
 * zeichnen; Kontextzustand gesichert und zurueck. Unter ctx_lock. */
static void vq_draw_ex(ID3D11RenderTargetView *rtv, const float vp[4], const float v[36], ID3D11ShaderResourceView *srv,
                       ID3D11PixelShader *ps, ID3D11BlendState *bs);
static void vq_draw(ID3D11RenderTargetView *rtv, const float vp[4], const float v[36], ID3D11ShaderResourceView *srv)
{
    vq_draw_ex(rtv, vp, v, srv, NULL, NULL);
}

/* W197: ps/bs = NULL -> deckender Leinwand-PS ohne Mischen (W164); sonst eigener PS + Mischzustand. */
static void vq_draw_ex(ID3D11RenderTargetView *rtv, const float vp[4], const float v[36], ID3D11ShaderResourceView *srv,
                       ID3D11PixelShader *ps, ID3D11BlendState *bs)
{
    D3D11_MAPPED_SUBRESOURCE map;
    D3D11_VIEWPORT nvp, svp[1];
    UINT stride = 6u * sizeof(float), off = 0, s_nvp = 1, s_vbs = 0, s_vbo = 0, s_mask = 0, s_sref = 0;
    ID3D11RenderTargetView *s_rtv = NULL;
    ID3D11DepthStencilView *s_dsv = NULL;
    ID3D11BlendState *s_bs = NULL;
    FLOAT s_bf[4];
    ID3D11DepthStencilState *s_ds = NULL;
    ID3D11RasterizerState *s_rs = NULL;
    ID3D11InputLayout *s_lay = NULL;
    ID3D11Buffer *s_vb = NULL;
    D3D11_PRIMITIVE_TOPOLOGY s_topo;
    ID3D11VertexShader *s_vs = NULL;
    ID3D11PixelShader *s_ps = NULL;
    ID3D11ShaderResourceView *s_srv = NULL;
    ID3D11SamplerState *s_smp = NULL;
    if (!rtv || !srv || !vq_init()) { return; }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)vq_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) { return; }
    memcpy(map.pData, v, 36u * sizeof(float));
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)vq_vb, 0);
    ID3D11DeviceContext_OMGetRenderTargets(g_ctx, 1, &s_rtv, &s_dsv);
    ID3D11DeviceContext_OMGetBlendState(g_ctx, &s_bs, s_bf, &s_mask);
    ID3D11DeviceContext_OMGetDepthStencilState(g_ctx, &s_ds, &s_sref);
    ID3D11DeviceContext_RSGetState(g_ctx, &s_rs);
    ID3D11DeviceContext_RSGetViewports(g_ctx, &s_nvp, svp);
    ID3D11DeviceContext_IAGetInputLayout(g_ctx, &s_lay);
    ID3D11DeviceContext_IAGetVertexBuffers(g_ctx, 0, 1, &s_vb, &s_vbs, &s_vbo);
    ID3D11DeviceContext_IAGetPrimitiveTopology(g_ctx, &s_topo);
    ID3D11DeviceContext_VSGetShader(g_ctx, &s_vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(g_ctx, &s_ps, NULL, NULL);
    ID3D11DeviceContext_PSGetShaderResources(g_ctx, 0, 1, &s_srv);
    ID3D11DeviceContext_PSGetSamplers(g_ctx, 0, 1, &s_smp);
    nvp.TopLeftX = vp[0]; nvp.TopLeftY = vp[1]; nvp.Width = vp[2]; nvp.Height = vp[3];
    nvp.MinDepth = 0.0f; nvp.MaxDepth = 1.0f;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, bs, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, NULL, 0);
    ID3D11DeviceContext_RSSetState(g_ctx, vq_rs);
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &nvp);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, vq_lay);
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &vq_vb, &stride, &off);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_ctx, vq_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, ps ? ps : vq_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &vq_smp);
    ID3D11DeviceContext_Draw(g_ctx, 6, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &s_srv);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &s_smp);
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &s_rtv, s_dsv);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, s_bs, s_bf, s_mask);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, s_ds, s_sref);
    ID3D11DeviceContext_RSSetState(g_ctx, s_rs);
    if (s_nvp) { ID3D11DeviceContext_RSSetViewports(g_ctx, 1, svp); }
    ID3D11DeviceContext_IASetInputLayout(g_ctx, s_lay);
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &s_vb, &s_vbs, &s_vbo);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, s_topo);
    ID3D11DeviceContext_VSSetShader(g_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, s_ps, NULL, 0);
    if (s_srv) { ID3D11ShaderResourceView_Release(s_srv); }
    if (s_smp) { ID3D11SamplerState_Release(s_smp); }
    if (s_rtv) { ID3D11RenderTargetView_Release(s_rtv); }
    if (s_dsv) { ID3D11DepthStencilView_Release(s_dsv); }
    if (s_bs) { ID3D11BlendState_Release(s_bs); }
    if (s_ds) { ID3D11DepthStencilState_Release(s_ds); }
    if (s_rs) { ID3D11RasterizerState_Release(s_rs); }
    if (s_lay) { ID3D11InputLayout_Release(s_lay); }
    if (s_vb) { ID3D11Buffer_Release(s_vb); }
    if (s_vs) { ID3D11VertexShader_Release(s_vs); }
    if (s_ps) { ID3D11PixelShader_Release(s_ps); }
}

/* Textur (w x h, SRV, optional RTV) passend anlegen; 1 = vorhanden. */
static int vq_tex_ex(ID3D11Texture2D **tex, ID3D11ShaderResourceView **srv, ID3D11RenderTargetView **rtv,
                     unsigned *tw, unsigned *th, unsigned w, unsigned h)
{
    D3D11_TEXTURE2D_DESC td;
    if (*tex && *tw == w && *th == h && (!rtv || *rtv)) { return 1; }
    if (rtv && *rtv) { ID3D11RenderTargetView_Release(*rtv); *rtv = NULL; }
    if (*srv) { ID3D11ShaderResourceView_Release(*srv); *srv = NULL; }
    if (*tex) { ID3D11Texture2D_Release(*tex); *tex = NULL; }
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (rtv ? D3D11_BIND_RENDER_TARGET : 0);
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, tex))) { *tex = NULL; return 0; }
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)*tex, NULL, srv)) ||
        (rtv && FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)*tex, NULL, rtv)))) {
        if (*srv) { ID3D11ShaderResourceView_Release(*srv); *srv = NULL; }
        if (rtv && *rtv) { ID3D11RenderTargetView_Release(*rtv); *rtv = NULL; }
        ID3D11Texture2D_Release(*tex); *tex = NULL; return 0;
    }
    *tw = w; *th = h;
    return 1;
}

static int vq_tex(ID3D11Texture2D **tex, ID3D11ShaderResourceView **srv, unsigned *tw, unsigned *th, unsigned w, unsigned h)
{
    return vq_tex_ex(tex, srv, NULL, tw, th, w, h);
}

/* Rechteck (vp) mit Seitenverhaeltnis a, groesstmoeglich und mittig in bw x bh (nie gestreckt) */
static void vq_fit(float vp[4], float a, float bw, float bh)
{
    if (bw / bh > a) { vp[3] = bh; vp[2] = floorf(bh * a + 0.5f); } else { vp[2] = bw; vp[3] = floorf(bw / a + 0.5f); }
    vp[0] = floorf((bw - vp[2]) * 0.5f); vp[1] = floorf((bh - vp[3]) * 0.5f);
}

/* Quad-Ecken (Clip) mit UV-Rechteck u0..u1, v0..v1 */
static void vq_quad(float v[36], float u0, float u1, float v0, float v1)
{
    static const float cx[6] = { -1, 1, 1, -1, 1, -1 }, cy[6] = { 1, 1, -1, 1, -1, -1 };
    int k;
    for (k = 0; k < 6; k++) {
        v[k * 6 + 0] = cx[k]; v[k * 6 + 1] = cy[k]; v[k * 6 + 2] = 0.5f; v[k * 6 + 3] = 1.0f;
        v[k * 6 + 4] = cx[k] < 0 ? u0 : u1;
        v[k * 6 + 5] = cy[k] > 0 ? v0 : v1;
    }
}

static int vr_preview_xr_on(void)
{
    return vr_mode == 2 && vr_prev_kind <= 2;
}

/* VR-Thread unter ctx_lock, nach der Wiedergabe: eingereichtes Augenbild als
 * Fenster-Spiegel sichern. W183 VR-FIX3: eine skalierte Kopie direkt in
 * Fenstergroesse (x vr_mirror_res %): vr_mirror_fit=0 fuellt das Fenster durch
 * Zuschnitt (Ausschnitt mit Fenster-Seitenverhaeltnis um die Blickachse des
 * Auges), 1 = ganzes Augenbild mit Balken. Nie gestreckt. fov = tan(links,
 * rechts, oben, unten) des gespiegelten Auges (Blickachse im Bild). */
static void vr_preview_capture(unsigned frame, const float q[4], const float pos[3], const float fov[4])
{
    unsigned sw = vr_prev_kind == 0 ? vr_tw : vr_tw / 2u, mw, mh;
    float ae, aw, vp[4], v[36], black[4] = { 0, 0, 0, 1 }, u0 = 0, u1 = 1, v0 = 0, v1 = 1;
    int res = g_cfg.vr_mirror_res, crop = g_cfg.vr_mirror_fit == 0;
    char desc[200];
    D3D11_BOX box;
    if (!vr_preview_xr_on() || !vr_tex || !g_cfg.vr_mirror || !vr_th || !g_bb_h) { return; }
    if (res < 25) { res = 25; } else if (res > 100) { res = 100; }
    mw = (unsigned)((g_bb_w * (unsigned)res + 50u) / 100u); mh = (unsigned)((g_bb_h * (unsigned)res + 50u) / 100u);
    if (mw < 64u) { mw = 64u; }
    if (mh < 64u) { mh = 64u; }
    if (!vq_tex_ex(&vr_pv_tex, &vr_pv_srv, &vr_pv_rtv, &vr_pv_w, &vr_pv_h, mw, mh)) { return; }
    if (!vq_tex(&vr_pvs_tex, &vr_pvs_srv, &vr_pvs_w, &vr_pvs_h, sw, vr_th)) { return; }
    box.left = vr_prev_kind == 2 ? vr_tw / 2u : 0u; box.right = box.left + sw;
    box.top = 0; box.bottom = vr_th; box.front = 0; box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)vr_pvs_tex, 0, 0, 0, 0, (ID3D11Resource *)vr_tex, 0, &box);
    ae = (float)sw / (float)vr_th;
    aw = (float)mw / (float)mh;
    if (crop) {
        float cw = 1.0f, ch = 1.0f, uc = 0.5f, vc = 0.5f;
        if (aw > ae) { ch = ae / aw; } else { cw = aw / ae; }
        if (vr_prev_kind != 0 && fov && fov[1] - fov[0] > 1e-4f && fov[2] - fov[3] > 1e-4f) {
            uc = -fov[0] / (fov[1] - fov[0]); /* Blickachse: x = 0 im Augenbild */
            vc = fov[2] / (fov[2] - fov[3]);
        }
        u0 = uc - 0.5f * cw; if (u0 < 0.0f) { u0 = 0.0f; } if (u0 > 1.0f - cw) { u0 = 1.0f - cw; }
        v0 = vc - 0.5f * ch; if (v0 < 0.0f) { v0 = 0.0f; } if (v0 > 1.0f - ch) { v0 = 1.0f - ch; }
        u1 = u0 + cw; v1 = v0 + ch;
        vp[0] = 0; vp[1] = 0; vp[2] = (float)mw; vp[3] = (float)mh;
    } else {
        vq_fit(vp, ae, (float)mw, (float)mh);
    }
    vq_quad(v, u0, u1, v0, v1);
    ID3D11DeviceContext_ClearRenderTargetView(g_ctx, vr_pv_rtv, black);
    vq_draw(vr_pv_rtv, vp, v, vr_pvs_srv);
    vr_pv_frame = frame;
    memcpy(vr_pv_pose, q, 4 * sizeof(float));
    memcpy(vr_pv_pose + 4, pos, 3 * sizeof(float));
    InterlockedExchange(&vr_pv_valid, 1);
    _snprintf_s(desc, sizeof desc, _TRUNCATE, "%s, %s %ux%u -> Spiegel %ux%u (%d %% des Fensters %ux%u), Ausschnitt u=%.3f..%.3f v=%.3f..%.3f",
                crop ? "fensterfuellend (Zuschnitt)" : "ganzes Bild (Balken)",
                vr_prev_kind == 0 ? "beide Augen" : (vr_prev_kind == 1 ? "linkes Auge" : "rechtes Auge"), sw, vr_th, mw, mh, res,
                g_bb_w, g_bb_h, u0, u1, v0, v1);
    if (strcmp(desc, vr_mir_desc) != 0) {
        static unsigned nlog;
        strcpy_s(vr_mir_desc, sizeof vr_mir_desc, desc);
        if (nlog++ < 20u) { HLOG_I("VR: Spiegel im Fenster: %s", desc); }
    }
}

/* W183 VR-FIX3: zeigt das Fenster in der Mission den VR-Spiegel (bzw. schwarz)?
 * Nur mit laufender OpenXR-Session, die gerade Bilder einreicht (< 0,5 s), und
 * in der Mission (Interp-Aufzeichnung; Menues laufen ueber die Leinwand). */
int sb_vr_window_mirrors(void)
{
    LONGLONG lq = vr_last_xr_q;
    if (vr_mode != 2 || !lq || !sb_interp_vr_available()) { return 0; }
    if ((double)(vr_now() - lq) > 0.5 * (double)vr_qpf.QuadPart) { return 0; }
    if (!g_cfg.vr_mirror) { return 1; }
    return vr_preview_xr_on() && vr_pv_valid && vr_pv_srv;
}

/* VR-Thread unter ctx_lock: Leinwand je Auge ins Augenziel (Hintergrund schwarz). */
static int vr_canvas_draw(const sb_vr_pass_t *p)
{
    float s = (float)g_cfg.vr_world_scale / 100.0f, h, w, d, black[4] = { 0, 0, 0, 1 }, v[36];
    static const float cx[6] = { -1, 1, 1, -1, 1, -1 }, cy[6] = { 1, 1, -1, 1, -1, -1 };
    int e, k;
    if (!g_cfg.vr_canvas || !vr_cv_valid || !vr_cv_srv || !vr_cv_h) { return 0; }
    if (s <= 0.0f) { s = 1.0f; }
    h = (float)(g_cfg.vr_canvas_size > 0 ? g_cfg.vr_canvas_size : 1500) / 1000.0f * s;
    d = (float)(g_cfg.vr_canvas_dist > 0 ? g_cfg.vr_canvas_dist : 2500) / 1000.0f * s;
    w = h * (float)vr_cv_w / (float)vr_cv_h;
    ID3D11DeviceContext_ClearRenderTargetView(g_ctx, p->rtv, black);
    for (e = 0; e < p->neyes; e++) {
        for (k = 0; k < 6; k++) {
            float x = cx[k] * 0.5f * w, y = cy[k] * 0.5f * h, z = -d - (float)g_cfg.vr_seat_forward / 1000.0f * s; /* W183: Abstand ab Sitzposition */
            const float *E = p->E[e];
            v[k * 6 + 0] = x * E[0] + y * E[4] + z * E[8] + E[12];
            v[k * 6 + 1] = x * E[1] + y * E[5] + z * E[9] + E[13];
            v[k * 6 + 2] = x * E[2] + y * E[6] + z * E[10] + E[14];
            v[k * 6 + 3] = x * E[3] + y * E[7] + z * E[11] + E[15];
            v[k * 6 + 4] = (cx[k] + 1.0f) * 0.5f;
            v[k * 6 + 5] = (1.0f - cy[k]) * 0.5f;
        }
        vq_draw(p->rtv, p->vp[e], v, vr_cv_srv);
    }
    return 1;
}

/* W197 VR-MENU: d3d11_render.c sb_render_blit (unter ctx_lock, vr_enable=1, ausserhalb des Spiegel-Pfads)
 * VOR ESC-Menue und F2: Leinwand-Quelle ohne die eigenen Overlays sichern - beide erscheinen in der
 * Brille als eigene Flaechen (sonst doppelt: auf der Leinwand und als Flaeche). */
void sb_vr_canvas_source(void *swap_res)
{
    ID3D11Resource *sres = (ID3D11Resource *)swap_res;
    if (!sb_vr_panels_on() || !sres || !g_dev || !g_cfg.vr_canvas || sb_interp_vr_available()) { return; }
    if (vq_tex(&vr_cv_tex, &vr_cv_srv, &vr_cv_w, &vr_cv_h, g_bb_w, g_bb_h)) {
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)vr_cv_tex, sres);
        InterlockedExchange(&vr_cv_valid, 1);
    }
}

/* d3d11_render.c sb_render_blit (unter ctx_lock, vr_enable=1, nach Kopie,
 * Nachbearbeitung und Overlays des Monitorbilds): Leinwand-Quelle sichern,
 * dann ggf. das Fenster durch das eingereichte Augenbild ersetzen.
 * 1 = Fenster ersetzt (Aufrufer zeichnet das ESC-Menue erneut darueber). */
int sb_vr_after_blit(void *swap_res, void *swap_rtv)
{
    ID3D11Resource *sres = (ID3D11Resource *)swap_res;
    ID3D11RenderTargetView *srtv = (ID3D11RenderTargetView *)swap_rtv;
    int want_cv;
    if (vr_mode != 2 || !sres || !g_dev) { return 0; }
    want_cv = g_cfg.vr_canvas && !sb_interp_vr_available();
    if (want_cv != vr_cv_on) {
        static unsigned nlog;
        vr_cv_on = want_cv;
        if (!want_cv) { InterlockedExchange(&vr_cv_valid, 0); }
        if (g_cfg.vr_canvas && nlog++ < 40u) {
            HLOG_I("VR: Leinwand %s (Bild %ux%u, Abstand %d mm, Hoehe %d mm)", want_cv ? "an (Menue/Film/2D)" : "aus (Mission, Cockpit)",
                   g_bb_w, g_bb_h, g_cfg.vr_canvas_dist, g_cfg.vr_canvas_size);
        }
    }
    if (want_cv && !sb_vr_panels_on() && vq_tex(&vr_cv_tex, &vr_cv_srv, &vr_cv_w, &vr_cv_h, g_bb_w, g_bb_h)) {
        /* W197: mit Menue-Flaechen kommt die Quelle aus sb_vr_canvas_source (vor ESC-Menue/F2) */
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)vr_cv_tex, sres);
        InterlockedExchange(&vr_cv_valid, 1);
    }
    if (!want_cv && !g_cfg.vr_mirror && srtv && sb_vr_window_mirrors()) {
        /* W183 VR-FIX3: Spiegel aus (spart Leistung) - Fenster in der Mission schwarz, ESC-Menue bleibt */
        float black[4] = { 0, 0, 0, 1 };
        ID3D11DeviceContext_ClearRenderTargetView(g_ctx, srtv, black);
        return 1;
    }
    if (vr_preview_xr_on() && g_cfg.vr_mirror && vr_pv_valid && vr_pv_srv && srtv && vr_pv_h) {
        float black[4] = { 0, 0, 0, 1 }, vp[4], v[36], a = (float)vr_pv_w / (float)vr_pv_h;
        float bw = (float)g_bb_w, bh = (float)g_bb_h;
        if (vr_pv_w == g_bb_w && vr_pv_h == g_bb_h) {
            /* W183: Spiegel liegt schon in Fenstergroesse vor (Zuschnitt/Balken im VR-Thread) -> eine Kopie */
            ID3D11DeviceContext_CopyResource(g_ctx, sres, (ID3D11Resource *)vr_pv_tex);
            vp[0] = 0; vp[1] = 0; vp[2] = bw; vp[3] = bh;
        } else {
            /* halbe Aufloesung bzw. Fenster seit dem Spiegelbild geaendert: hochskalieren, nie strecken */
            vq_fit(vp, a, bw, bh);
            vq_quad(v, 0.0f, 1.0f, 0.0f, 1.0f);
            ID3D11DeviceContext_ClearRenderTargetView(g_ctx, srtv, black);
            vq_draw(srtv, vp, v, vr_pv_srv);
        }
        {
            int changed = 0, i;
            for (i = 0; i < 7; i++) { if (fabsf(vr_pv_pose[i] - vr_pv_lpose[i]) > 1e-3f) { changed = 1; } }
            if (vr_pv_frame != vr_pv_logged && (changed || vr_pv_logged == 0u) && g_cfg.vr_diag >= 1) {
                static unsigned nlog;
                if (nlog++ < 200u) {
                    HLOG_I("VR vorschau: zeigt xr-bild %u (%s, %ux%u -> Fenster %ux%u Rechteck %.0f,%.0f %.0fx%.0f) ansicht%d pos=(%.3f,%.3f,%.3f) q=(%.4f,%.4f,%.4f,%.4f)",
                           vr_pv_frame, vr_prev_kind == 0 ? "beide Augen" : (vr_prev_kind == 1 ? "linkes Auge" : "rechtes Auge"),
                           vr_pv_w, vr_pv_h, g_bb_w, g_bb_h, vp[0], vp[1], vp[2], vp[3], vr_prev_kind == 2 ? 1 : 0,
                           vr_pv_pose[4], vr_pv_pose[5], vr_pv_pose[6], vr_pv_pose[0], vr_pv_pose[1], vr_pv_pose[2], vr_pv_pose[3]);
                }
                memcpy(vr_pv_lpose, vr_pv_pose, sizeof vr_pv_lpose);
                vr_pv_logged = vr_pv_frame;
            }
        }
        if (g_cfg.vr_shots > 0) { /* DIAG(W164): Fensterbild der Vorschau (gezeigtes Bild), max 12 */
            static unsigned nwin, lastf;
            if (nwin < 12u && vr_pv_frame >= lastf + (unsigned)g_cfg.vr_shots) {
                char nm[64];
                lastf = vr_pv_frame;
                nwin++;
                _snprintf_s(nm, sizeof nm, _TRUNCATE, "vr_%05u_fenster", vr_pv_frame);
                sb_render_shot_name(nm, sres);
                HLOG_I("VR vorschau-shot %s: xr-bild %u ansicht pos=(%.3f,%.3f,%.3f) q=(%.4f,%.4f,%.4f,%.4f)", nm, vr_pv_frame,
                       vr_pv_pose[4], vr_pv_pose[5], vr_pv_pose[6], vr_pv_pose[0], vr_pv_pose[1], vr_pv_pose[2], vr_pv_pose[3]);
            }
        }
        return 1;
    }
    return 0;
}

/* Fensternachricht: Recentering-Taste, Kopf-Simulation per Ziffernblock (nur im Fokus:
 * Fensternachrichten kommen nur beim fokussierten Fenster an bzw. per
 * PostMessage im Test). */
static unsigned vr_key_vk(void)
{
    const char *k = g_cfg.vr_recenter_key;
    if ((k[0] == 'F' || k[0] == 'f') && atoi(k + 1) >= 1 && atoi(k + 1) <= 24) { return 0x70u + (unsigned)(atoi(k + 1) - 1); }
    if ((k[0] == 'V' || k[0] == 'v') && (k[1] == 'K' || k[1] == 'k')) { return (unsigned)strtoul(k + 2, NULL, 16); }
    return 0x77u; /* F8 */
}

int sb_vr_wndproc(unsigned msg, unsigned wp, long lp)
{
    int bit = 0;
    (void)lp;
    if (!vr_mode) { return 0; }
    if (msg != WM_KEYDOWN && msg != WM_KEYUP && msg != WM_SYSKEYDOWN && msg != WM_SYSKEYUP) { return 0; }
    if (wp == vr_key_vk()) {
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) { sb_vr_recenter("Taste"); }
        return 1;
    }
    if (vr_mode != 1 || vr_sim_kind != 0) { return 0; }
    /* Ziffernblock (NumLock an; die Legacy-Belegung nutzt ihn nicht): 4/6 drehen, 8/2 neigen, 5 zentrieren */
    if (wp == VK_NUMPAD5) {
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) { sb_vr_recenter("Ziffernblock 5"); }
        return 1;
    }
    if (wp == VK_NUMPAD4) { bit = 1; } else if (wp == VK_NUMPAD6) { bit = 2; }
    else if (wp == VK_NUMPAD8) { bit = 4; } else if (wp == VK_NUMPAD2) { bit = 8; }
    if (!bit) { return 0; }
    if (msg == WM_KEYUP || msg == WM_SYSKEYUP) { vr_keys &= ~bit; }
    else { vr_keys |= bit; }
    return 1;
}

/* ---------- W183 VR-FIX3: Leistungs-Presets ----------
 * Tabelle (eine Quelle fuer ESC-Menue und Doku; der Launcher spiegelt sie):
 *                 Aufloesung  Bildrate  Spiegel  Aussenwelt   Shading  Monitor-RT
 *  1 Leistung       65 %       72 Hz     50 %    Spieltakt    aus      1x
 *  2 Ausgewogen     80 %       Runtime  100 %    Spieltakt    an       2x   (Standard)
 *  3 Qualitaet     100 %       Runtime  100 %    je VR-Bild   an       wie Bildschirm */
static const int kVrPreset[4][6] = {
    { 0, 0, 0, 0, 0, 0 },
    { 65, 72, 50, 1, 0, 1 },
    { 80, 0, 100, 1, 1, 2 },
    { 100, 0, 100, 0, 1, 0 },
};

void sb_vr_preset_apply(int pr)
{
    if (pr < 1 || pr > 3) { g_cfg.vr_preset = 0; return; }
    g_cfg.vr_render_scale = kVrPreset[pr][0];
    g_cfg.vr_refresh_hz = kVrPreset[pr][1];
    g_cfg.vr_mirror_res = kVrPreset[pr][2];
    g_cfg.vr_world_tick = kVrPreset[pr][3];
    g_cfg.vr_shading = kVrPreset[pr][4];
    g_cfg.vr_rt_scale = kVrPreset[pr][5];
    g_cfg.vr_preset = pr;
}

int sb_vr_preset_match(void)
{
    int pr;
    for (pr = 1; pr <= 3; pr++) {
        if (g_cfg.vr_render_scale == kVrPreset[pr][0] && g_cfg.vr_refresh_hz == kVrPreset[pr][1] &&
            g_cfg.vr_mirror_res == kVrPreset[pr][2] && (g_cfg.vr_world_tick ? 1 : 0) == kVrPreset[pr][3] &&
            (g_cfg.vr_shading ? 1 : 0) == kVrPreset[pr][4] && g_cfg.vr_rt_scale == kVrPreset[pr][5]) {
            return pr;
        }
    }
    return 0;
}

const char *sb_vr_state_text(void)
{
    static char buf[96];
    _snprintf_s(buf, sizeof buf, _TRUNCATE, "%s", vr_mode == 2 ? LT("vr.st_openxr") :
                (vr_mode == 1 ? LT("vr.st_preview") : (g_cfg.vr_enable ? LT("vr.st_unavail") : LT("vr.st_off"))));
    return buf;
}

/* ---------- OpenXR: Mini-Loader ---------- */

static HMODULE xr_dll;
static PFN_xrGetInstanceProcAddr xr_gipa;
static XrInstance xr_inst;
static XrSystemId xr_sys;
static XrSession xr_sess;
static XrSpace xr_space;
static XrSwapchain xr_sc;
static ID3D11Texture2D *xr_img[8];
static uint32_t xr_nimg;
static uint32_t xr_ew, xr_eh;
static int xr_running, xr_quit;
static XrSessionState xr_state;
/* W145: Swapchain-Bild erworben, aber noch nicht abgegeben (Wait-Timeout ->
 * im naechsten Bild weiter warten, nicht neu erwerben: Spezifikation) */
static int xr_sc_held;
static uint32_t xr_sc_idx;
/* W145: laufender OpenXR-Aufruf (Haenger-Erkennung ohne Sperre) */
static const char *volatile xr_call;
static volatile LONGLONG xr_call_q;
static volatile LONG xr_hang_logged;
/* W183: Fortschritt des VR-Threads ausserhalb der OpenXR-Aufrufe (Watchdog: Thread steht?) */
static const char *volatile xr_stage;
static volatile LONGLONG xr_loop_q;
static volatile LONG xr_stage_logged;
/* W145: Statistik Sperrzeit / Wartezeiten je Fenster (vr_diag>=1) */
static double st2_lock, st2_lock_max, st2_wsc_max, st2_ef_max, st2_wf_max;
static unsigned st2_n, st2_skip_tmo, st2_skip_km;
static HANDLE xr_thread;
static volatile DWORD xr_tid; /* W145 */
static unsigned xr_fno;       /* W164: eingereichte VR-Bilder (Abgleich Vorschau/Pose) */
/* W183 VR-FIX3: Render-Skalierung (Empfehlung, Maximum, angelegte Swapchain je Auge), Bildrate */
static uint32_t xr_rec_w, xr_rec_h, xr_max_w, xr_max_h, xr_aw, xr_ah;
static int xr_scale_cur = -1, xr_have_rr, xr_rr_req = -1, xr_focus_log = -1, xr_rc_started;
static XrTime xr_last_disp;
/* W183: Takt-Statistik je Fenster (vr_diag>=1, Zeile "VR takt") */
static unsigned tk_n, tk_miss, tk_hold, tk_wtick, tk_newpose, tk_nskip;
static double tk_lw, tk_lw_max, tk_rep, tk_rep_max, tk_dt_max;
static unsigned tk_flog;
static float tk_lastpose[7];
#define XRF(n) static PFN_##n p##n
XRF(xrEnumerateInstanceExtensionProperties); XRF(xrCreateInstance); XRF(xrDestroyInstance);
XRF(xrGetInstanceProperties); XRF(xrPollEvent); XRF(xrGetSystem); XRF(xrGetSystemProperties);
XRF(xrEnumerateViewConfigurationViews); XRF(xrCreateSession); XRF(xrDestroySession);
XRF(xrBeginSession); XRF(xrEndSession); XRF(xrCreateReferenceSpace); XRF(xrEnumerateSwapchainFormats);
XRF(xrCreateSwapchain); XRF(xrEnumerateSwapchainImages); XRF(xrAcquireSwapchainImage);
XRF(xrWaitSwapchainImage); XRF(xrReleaseSwapchainImage); XRF(xrWaitFrame); XRF(xrBeginFrame);
XRF(xrEndFrame); XRF(xrLocateViews); XRF(xrGetD3D11GraphicsRequirementsKHR);
XRF(xrEnumerateDisplayRefreshRatesFB); XRF(xrGetDisplayRefreshRateFB); XRF(xrRequestDisplayRefreshRateFB); /* W183 */

static char xr_fail[256];

static void xr_failf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(xr_fail, sizeof xr_fail, _TRUNCATE, fmt, ap);
    va_end(ap);
}

/* Minimaler JSON-Leser: Zeichenkette zum Schluessel key (erstes Vorkommen),
 * Escapes \\ \/ \" werden aufgeloest. */
static int json_str(const char *js, const char *key, char *out, size_t cap)
{
    char pat[64];
    const char *p;
    size_t o = 0;
    _snprintf_s(pat, sizeof pat, _TRUNCATE, "\"%s\"", key);
    p = strstr(js, pat);
    if (!p) { return 0; }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { p++; }
    if (*p != ':') { return 0; }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { p++; }
    if (*p != '"') { return 0; }
    p++;
    while (*p && *p != '"' && o + 1 < cap) {
        if (*p == '\\' && p[1]) { p++; }
        out[o++] = *p++;
    }
    out[o] = '\0';
    return *p == '"';
}

/* PE-Maschinentyp lesen (ohne Laden): 0x14C = x86, 0x8664 = x64. */
static unsigned pe_machine(const char *path)
{
    unsigned char hdr[512];
    DWORD got = 0, pe;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { return 0; }
    ReadFile(f, hdr, sizeof hdr, &got, NULL);
    CloseHandle(f);
    if (got < 64 || hdr[0] != 'M' || hdr[1] != 'Z') { return 1; }
    pe = *(DWORD *)(hdr + 0x3C);
    if (pe + 6 > got || memcmp(hdr + pe, "PE\0\0", 4) != 0) { return 1; }
    return *(unsigned short *)(hdr + pe + 4);
}

static int xr_load_runtime(char *name_out, size_t cap)
{
    char json_path[MAX_PATH], lib[MAX_PATH], full[MAX_PATH], *js = NULL, *slash;
    const char *src = "cfg";
    DWORD sz = 0, got = 0;
    HANDLE f;
    unsigned mach;
    PFN_xrNegotiateLoaderRuntimeInterface neg;
    XrNegotiateLoaderInfo li;
    XrNegotiateRuntimeRequest rr;
    XrResult r;
    json_path[0] = '\0';
    if (g_cfg.vr_runtime_json[0]) {
        strcpy_s(json_path, sizeof json_path, g_cfg.vr_runtime_json);
    } else if (getenv("XR_RUNTIME_JSON") && getenv("XR_RUNTIME_JSON")[0]) {
        strcpy_s(json_path, sizeof json_path, getenv("XR_RUNTIME_JSON"));
        src = "XR_RUNTIME_JSON";
    } else {
        /* 32-bit-Prozess: Registry-Umleitung liefert die 32-bit-Runtime (WOW6432Node) */
        HKEY k;
        DWORD type = 0, n = sizeof json_path;
        src = "Registry ActiveRuntime";
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", 0, KEY_READ, &k) == ERROR_SUCCESS) {
            if (RegQueryValueExA(k, "ActiveRuntime", NULL, &type, (LPBYTE)json_path, &n) != ERROR_SUCCESS || type != REG_SZ) {
                json_path[0] = '\0';
            }
            RegCloseKey(k);
        }
        if (!json_path[0]) { xr_failf("keine aktive OpenXR-Runtime (Registry ActiveRuntime fehlt)"); return 0; }
    }
    f = CreateFileA(json_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { xr_failf("Runtime-Manifest fehlt: %s (%s)", json_path, src); return 0; }
    sz = GetFileSize(f, NULL);
    if (sz == INVALID_FILE_SIZE || sz > 65536u) { CloseHandle(f); xr_failf("Runtime-Manifest ungueltig: %s", json_path); return 0; }
    js = (char *)calloc(1, sz + 1);
    if (!js) { CloseHandle(f); xr_failf("kein Speicher"); return 0; }
    ReadFile(f, js, sz, &got, NULL);
    CloseHandle(f);
    if (!json_str(js, "library_path", lib, sizeof lib)) { free(js); xr_failf("Manifest ohne runtime.library_path: %s", json_path); return 0; }
    if (!json_str(js, "name", name_out, cap)) { strcpy_s(name_out, cap, "?"); }
    free(js);
    /* relativer Pfad -> relativ zum Manifest */
    if (!(lib[0] && lib[1] == ':') && !(lib[0] == '\\' && lib[1] == '\\')) {
        strcpy_s(full, sizeof full, json_path);
        slash = strrchr(full, '\\');
        if (!slash || (strrchr(full, '/') && strrchr(full, '/') > slash)) { slash = strrchr(full, '/'); }
        if (slash) { slash[1] = '\0'; } else { full[0] = '\0'; }
        strcat_s(full, sizeof full, lib);
    } else {
        strcpy_s(full, sizeof full, lib);
    }
    mach = pe_machine(full);
    if (mach == 0) { xr_failf("Runtime-DLL fehlt: %s", full); return 0; }
    if (mach == 1) { xr_failf("Runtime-DLL ungueltig (kein PE-Modul): %s", full); return 0; }
    if (mach != 0x14Cu) {
        xr_failf("Runtime-DLL ist kein 32-bit-Modul (Maschine 0x%X): %s - dieser Prozess ist 32-bit", mach, full);
        return 0;
    }
    xr_dll = LoadLibraryExA(full, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!xr_dll) { xr_failf("Runtime-DLL laedt nicht (Fehler %lu): %s", GetLastError(), full); return 0; }
    neg = (PFN_xrNegotiateLoaderRuntimeInterface)(void *)GetProcAddress(xr_dll, "xrNegotiateLoaderRuntimeInterface");
    if (!neg) { xr_failf("Runtime ohne xrNegotiateLoaderRuntimeInterface: %s", full); return 0; }
    memset(&li, 0, sizeof li);
    li.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    li.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    li.structSize = sizeof li;
    li.minInterfaceVersion = 1;
    li.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    li.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    li.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
    memset(&rr, 0, sizeof rr);
    rr.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
    rr.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
    rr.structSize = sizeof rr;
    r = neg(&li, &rr);
    if (XR_FAILED(r) || !rr.getInstanceProcAddr) { xr_failf("Aushandlung mit der Runtime gescheitert (%d)", (int)r); return 0; }
    xr_gipa = rr.getInstanceProcAddr;
    HLOG_I("VR: OpenXR-Runtime geladen: '%s' (%s, %s), Schnittstelle %u, API %u.%u", name_out, full, src,
           rr.runtimeInterfaceVersion, XR_VERSION_MAJOR(rr.runtimeApiVersion), XR_VERSION_MINOR(rr.runtimeApiVersion));
    return 1;
}

static int xr_get(XrInstance inst, const char *n, PFN_xrVoidFunction *f)
{
    *f = NULL;
    return XR_SUCCEEDED(xr_gipa(inst, n, f)) && *f;
}
#define XR_GET(inst, n) (xr_get(inst, #n, (PFN_xrVoidFunction *)&p##n))

/* W183 VR-FIX3: eingestellte Render-Skalierung (30..150 %) */
static int vr_scale_pct(void)
{
    int v = g_cfg.vr_render_scale;
    if (v <= 0) { v = 100; }
    if (v < 30) { v = 30; } else if (v > 150) { v = 150; }
    return v;
}

/* Augengroesse fuer die aktuelle Skalierung, begrenzt auf die angelegte Swapchain (gerade Zahlen) */
static void xr_eye_size(uint32_t *w, uint32_t *h)
{
    int sc = vr_scale_pct();
    uint32_t ew = (uint32_t)((xr_rec_w * (unsigned)sc + 50u) / 100u), eh = (uint32_t)((xr_rec_h * (unsigned)sc + 50u) / 100u);
    if (ew > xr_aw) { ew = xr_aw; }
    if (eh > xr_ah) { eh = xr_ah; }
    ew &= ~1u; eh &= ~1u;
    if (ew < 64u) { ew = 64u; }
    if (eh < 64u) { eh = 64u; }
    *w = ew; *h = eh;
}

static int xr_init(void)
{
    char rname[128];
    XrExtensionProperties ext[64];
    uint32_t next = 0, i, nfmt = 0;
    int have_d3d = 0;
    XrInstanceCreateInfo ici;
    const char *enabled[2] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME };
    XrInstanceProperties ip;
    XrSystemGetInfo sgi;
    XrSystemProperties sp;
    XrViewConfigurationView vcv[2];
    XrGraphicsRequirementsD3D11KHR req;
    XrGraphicsBindingD3D11KHR gb;
    XrSessionCreateInfo sci;
    XrReferenceSpaceCreateInfo rsi;
    XrSwapchainCreateInfo swi;
    XrSwapchainImageD3D11KHR imgs[8];
    int64_t fmts[64], fmt = 0;
    XrResult r;
    IDXGIDevice *dxd = NULL;
    IDXGIAdapter *ad = NULL;
    DXGI_ADAPTER_DESC adesc;

    if (!xr_load_runtime(rname, sizeof rname)) { return 0; }
    if (!XR_GET(XR_NULL_HANDLE, xrEnumerateInstanceExtensionProperties) || !XR_GET(XR_NULL_HANDLE, xrCreateInstance)) {
        xr_failf("Runtime liefert xrCreateInstance nicht");
        return 0;
    }
    for (i = 0; i < 64; i++) { memset(&ext[i], 0, sizeof ext[i]); ext[i].type = XR_TYPE_EXTENSION_PROPERTIES; }
    r = pxrEnumerateInstanceExtensionProperties(NULL, 0, &next, NULL);
    if (XR_FAILED(r)) { xr_failf("xrEnumerateInstanceExtensionProperties %d", (int)r); return 0; }
    if (next > 64) { next = 64; }
    r = pxrEnumerateInstanceExtensionProperties(NULL, next, &next, ext);
    for (i = 0; XR_SUCCEEDED(r) && i < next && i < 64; i++) {
        if (strcmp(ext[i].extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) { have_d3d = 1; }
        if (strcmp(ext[i].extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) == 0) { xr_have_rr = 1; } /* W183 */
    }
    if (!have_d3d) { xr_failf("Runtime ohne %s", XR_KHR_D3D11_ENABLE_EXTENSION_NAME); return 0; }
    memset(&ici, 0, sizeof ici);
    ici.type = XR_TYPE_INSTANCE_CREATE_INFO;
    strcpy_s(ici.applicationInfo.applicationName, sizeof ici.applicationInfo.applicationName, "Steel Battalion Preservation");
    ici.applicationInfo.applicationVersion = 1;
    strcpy_s(ici.applicationInfo.engineName, sizeof ici.applicationInfo.engineName, "sb_emu");
    ici.applicationInfo.engineVersion = 1;
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = xr_have_rr ? 2u : 1u; /* W183: Bildrate der Brille, falls angeboten */
    ici.enabledExtensionNames = enabled;
    r = pxrCreateInstance(&ici, &xr_inst);
    if (XR_FAILED(r)) { xr_failf("xrCreateInstance %d", (int)r); return 0; }
    if (!XR_GET(xr_inst, xrDestroyInstance) || !XR_GET(xr_inst, xrGetInstanceProperties) || !XR_GET(xr_inst, xrPollEvent) ||
        !XR_GET(xr_inst, xrGetSystem) || !XR_GET(xr_inst, xrGetSystemProperties) ||
        !XR_GET(xr_inst, xrEnumerateViewConfigurationViews) || !XR_GET(xr_inst, xrCreateSession) ||
        !XR_GET(xr_inst, xrDestroySession) || !XR_GET(xr_inst, xrBeginSession) || !XR_GET(xr_inst, xrEndSession) ||
        !XR_GET(xr_inst, xrCreateReferenceSpace) || !XR_GET(xr_inst, xrEnumerateSwapchainFormats) ||
        !XR_GET(xr_inst, xrCreateSwapchain) || !XR_GET(xr_inst, xrEnumerateSwapchainImages) ||
        !XR_GET(xr_inst, xrAcquireSwapchainImage) || !XR_GET(xr_inst, xrWaitSwapchainImage) ||
        !XR_GET(xr_inst, xrReleaseSwapchainImage) || !XR_GET(xr_inst, xrWaitFrame) || !XR_GET(xr_inst, xrBeginFrame) ||
        !XR_GET(xr_inst, xrEndFrame) || !XR_GET(xr_inst, xrLocateViews) ||
        !XR_GET(xr_inst, xrGetD3D11GraphicsRequirementsKHR)) {
        xr_failf("Runtime liefert benoetigte Funktionen nicht");
        return 0;
    }
    if (xr_have_rr && (!XR_GET(xr_inst, xrEnumerateDisplayRefreshRatesFB) || !XR_GET(xr_inst, xrGetDisplayRefreshRateFB) ||
                       !XR_GET(xr_inst, xrRequestDisplayRefreshRateFB))) {
        xr_have_rr = 0; /* W183: Erweiterung gemeldet, Funktionen fehlen -> Bildrate nicht einstellbar */
    }
    memset(&ip, 0, sizeof ip);
    ip.type = XR_TYPE_INSTANCE_PROPERTIES;
    if (XR_SUCCEEDED(pxrGetInstanceProperties(xr_inst, &ip))) {
        HLOG_I("VR: Instanz '%s' %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
               XR_VERSION_MINOR(ip.runtimeVersion), (unsigned)(ip.runtimeVersion & 0xffffffffu));
    }
    memset(&sgi, 0, sizeof sgi);
    sgi.type = XR_TYPE_SYSTEM_GET_INFO;
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = pxrGetSystem(xr_inst, &sgi, &xr_sys);
    if (XR_FAILED(r)) {
        xr_failf(r == XR_ERROR_FORM_FACTOR_UNAVAILABLE ? "Brille nicht verbunden (xrGetSystem %d)" : "xrGetSystem %d", (int)r);
        return 0;
    }
    memset(&sp, 0, sizeof sp);
    sp.type = XR_TYPE_SYSTEM_PROPERTIES;
    pxrGetSystemProperties(xr_inst, xr_sys, &sp);
    memset(vcv, 0, sizeof vcv);
    vcv[0].type = vcv[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    i = 0;
    r = pxrEnumerateViewConfigurationViews(xr_inst, xr_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &i, vcv);
    if (XR_FAILED(r) || i != 2) { xr_failf("keine Stereo-Ansicht (%d, n=%u)", (int)r, i); return 0; }
    xr_ew = vcv[0].recommendedImageRectWidth;
    xr_eh = vcv[0].recommendedImageRectHeight;
    if (xr_ew < 64 || xr_eh < 64 || xr_ew > 8192 || xr_eh > 8192) { xr_failf("Augengroesse unplausibel %ux%u", xr_ew, xr_eh); return 0; }
    HLOG_I("VR: System '%s' Tracking Richtung=%u Position=%u, Auge %ux%u (max %ux%u)", sp.systemName,
           sp.trackingProperties.orientationTracking, sp.trackingProperties.positionTracking, xr_ew, xr_eh,
           vcv[0].maxImageRectWidth, vcv[0].maxImageRectHeight);
    /* W183 VR-FIX3: Render-Skalierung. Swapchain fuer max(Einstellung, 100 %) anlegen (live per ESC bis
     * dahin umschaltbar: imageRect kleiner als die Swapchain), gerendert wird die eingestellte Groesse. */
    xr_rec_w = xr_ew; xr_rec_h = xr_eh;
    xr_max_w = vcv[0].maxImageRectWidth >= xr_ew ? vcv[0].maxImageRectWidth : xr_ew;
    xr_max_h = vcv[0].maxImageRectHeight >= xr_eh ? vcv[0].maxImageRectHeight : xr_eh;
    {
        int al = vr_scale_pct();
        if (al < 100) { al = 100; }
        xr_aw = (uint32_t)((xr_rec_w * (unsigned)al + 50u) / 100u); if (xr_aw > xr_max_w) { xr_aw = xr_max_w; }
        xr_ah = (uint32_t)((xr_rec_h * (unsigned)al + 50u) / 100u); if (xr_ah > xr_max_h) { xr_ah = xr_max_h; }
        if (xr_aw > 8192u) { xr_aw = 8192u; }
        if (xr_ah > 8192u) { xr_ah = 8192u; }
        xr_eye_size(&xr_ew, &xr_eh);
        xr_scale_cur = vr_scale_pct();
        HLOG_I("VR: Render-Skalierung %d %% -> Auge %ux%u (Empfehlung der Runtime %ux%u, max %ux%u); Swapchain angelegt fuer bis %d %% (%ux%u je Auge)",
               xr_scale_cur, xr_ew, xr_eh, xr_rec_w, xr_rec_h, xr_max_w, xr_max_h, al, xr_aw, xr_ah);
    }
    memset(&req, 0, sizeof req);
    req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR;
    r = pxrGetD3D11GraphicsRequirementsKHR(xr_inst, xr_sys, &req);
    if (XR_FAILED(r)) { xr_failf("xrGetD3D11GraphicsRequirementsKHR %d", (int)r); return 0; }
    if (SUCCEEDED(ID3D11Device_QueryInterface(g_dev, &IID_IDXGIDevice, (void **)&dxd)) && dxd) {
        if (SUCCEEDED(IDXGIDevice_GetAdapter(dxd, &ad)) && ad) {
            if (SUCCEEDED(IDXGIAdapter_GetDesc(ad, &adesc)) &&
                (adesc.AdapterLuid.LowPart != req.adapterLuid.LowPart || adesc.AdapterLuid.HighPart != req.adapterLuid.HighPart)) {
                IDXGIAdapter_Release(ad);
                IDXGIDevice_Release(dxd);
                xr_failf("Grafikkarte passt nicht zur Runtime (LUID %08lX:%08lX statt %08X:%08X)",
                         (unsigned long)adesc.AdapterLuid.HighPart, (unsigned long)adesc.AdapterLuid.LowPart,
                         (unsigned)req.adapterLuid.HighPart, req.adapterLuid.LowPart);
                return 0;
            }
            IDXGIAdapter_Release(ad);
        }
        IDXGIDevice_Release(dxd);
    }
    if ((int)ID3D11Device_GetFeatureLevel(g_dev) < req.minFeatureLevel) {
        xr_failf("Feature-Level 0x%X < 0x%X", (unsigned)ID3D11Device_GetFeatureLevel(g_dev), (unsigned)req.minFeatureLevel);
        return 0;
    }
    /* W145: eigenes Device fuer die Runtime (gleicher Adapter wie das Spiel,
     * damit das Augenziel geteilt werden kann). Die Runtime darf dessen
     * Immediate-Context in xrEndFrame/Release frei nutzen - nur der VR-Thread
     * ruft sie auf, das Spiel-Device sieht sie nie. */
    {
        D3D_FEATURE_LEVEL fl[2] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 }, got = 0;
        /* W206 VR-HANG: this device only submits the eye copy and runtime work.
         * Avoid the driver's internal submission worker: with two devices the
         * first AcquireSync (or even Flush before it) can wait forever inside
         * that worker path. This does NOT make the D3D11 device single-threaded;
         * runtime-created threads may still use it. The game device is unchanged. */
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS;
        IDXGIDevice *gd = NULL;
        IDXGIAdapter *ga = NULL;
        HRESULT hr = E_FAIL;
        if (SUCCEEDED(ID3D11Device_QueryInterface(g_dev, &IID_IDXGIDevice, (void **)&gd)) && gd) {
            if (SUCCEEDED(IDXGIDevice_GetAdapter(gd, &ga)) && ga) {
                hr = D3D11CreateDevice(ga, D3D_DRIVER_TYPE_UNKNOWN, NULL, flags, fl, 2,
                                       D3D11_SDK_VERSION, &xr_dev, &got, &xr_ctx);
                if (FAILED(hr)) {
                    hr = D3D11CreateDevice(ga, D3D_DRIVER_TYPE_UNKNOWN, NULL, flags, fl + 1, 1,
                                           D3D11_SDK_VERSION, &xr_dev, &got, &xr_ctx);
                }
                IDXGIAdapter_Release(ga);
            }
            IDXGIDevice_Release(gd);
        }
        if (FAILED(hr) || !xr_dev || !xr_ctx) { xr_failf("eigenes D3D11-Device fuer OpenXR scheitert (0x%lX)", (long)hr); return 0; }
        if ((int)got < req.minFeatureLevel) {
            xr_failf("VR-Device Feature-Level 0x%X < 0x%X", (unsigned)got, (unsigned)req.minFeatureLevel);
            return 0;
        }
        HLOG_I("VR: eigenes D3D11-Device fuer die Runtime (Feature-Level 0x%X, Adapter wie das Spiel); Spiel-Device bleibt privat",
               (unsigned)got);
        HLOG_I("VR: Bilduebergabe ohne interne D3D11-Threading-Optimierungen (Device-Flags 0x%X)", (unsigned)flags);
    }
    memset(&gb, 0, sizeof gb);
    gb.type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR;
    gb.device = (struct ID3D11Device *)xr_dev;
    memset(&sci, 0, sizeof sci);
    sci.type = XR_TYPE_SESSION_CREATE_INFO;
    sci.next = &gb;
    sci.systemId = xr_sys;
    r = pxrCreateSession(xr_inst, &sci, &xr_sess);
    if (XR_FAILED(r)) { xr_failf("xrCreateSession %d", (int)r); return 0; }
    memset(&rsi, 0, sizeof rsi);
    rsi.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsi.poseInReferenceSpace.orientation.w = 1.0f;
    r = pxrCreateReferenceSpace(xr_sess, &rsi, &xr_space);
    if (XR_FAILED(r)) {
        rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        r = pxrCreateReferenceSpace(xr_sess, &rsi, &xr_space);
    }
    if (XR_FAILED(r)) { xr_failf("xrCreateReferenceSpace %d", (int)r); return 0; }
    r = pxrEnumerateSwapchainFormats(xr_sess, 0, &nfmt, NULL);
    if (nfmt > 64) { nfmt = 64; }
    if (XR_SUCCEEDED(r)) { r = pxrEnumerateSwapchainFormats(xr_sess, nfmt, &nfmt, fmts); }
    /* Augenziel ist R8G8B8A8_UNORM mit Gamma-Werten; Kopie in _SRGB (gleiche
     * Typgruppe, Bitkopie) -> Runtime dekodiert sRGB richtig. Sonst UNORM. */
    for (i = 0; XR_SUCCEEDED(r) && i < nfmt; i++) { if (fmts[i] == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) { fmt = fmts[i]; } }
    for (i = 0; XR_SUCCEEDED(r) && !fmt && i < nfmt; i++) { if (fmts[i] == DXGI_FORMAT_R8G8B8A8_UNORM) { fmt = fmts[i]; } }
    if (!fmt) { xr_failf("kein passendes Swapchain-Format (R8G8B8A8)"); return 0; }
    memset(&swi, 0, sizeof swi);
    swi.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    swi.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    swi.format = fmt;
    swi.sampleCount = 1;
    swi.width = xr_aw * 2u; /* W183: angelegte Groesse (>= gerenderte) */
    swi.height = xr_ah;
    swi.faceCount = 1;
    swi.arraySize = 1;
    swi.mipCount = 1;
    r = pxrCreateSwapchain(xr_sess, &swi, &xr_sc);
    if (XR_FAILED(r)) { xr_failf("xrCreateSwapchain %d", (int)r); return 0; }
    for (i = 0; i < 8; i++) { memset(&imgs[i], 0, sizeof imgs[i]); imgs[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR; }
    r = pxrEnumerateSwapchainImages(xr_sc, 0, &xr_nimg, NULL);
    if (xr_nimg > 8) { xr_nimg = 8; }
    if (XR_SUCCEEDED(r)) { r = pxrEnumerateSwapchainImages(xr_sc, xr_nimg, &xr_nimg, (XrSwapchainImageBaseHeader *)imgs); }
    if (XR_FAILED(r) || !xr_nimg) { xr_failf("xrEnumerateSwapchainImages %d", (int)r); return 0; }
    for (i = 0; i < xr_nimg; i++) { xr_img[i] = (ID3D11Texture2D *)imgs[i].texture; }
    HLOG_I("VR: Session bereit, Raum %s, Swapchain %ux%u Format %d (%u Bilder)",
           rsi.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL ? "LOCAL" : "STAGE",
           swi.width, swi.height, (int)fmt, xr_nimg);
    if (xr_have_rr) { /* W183: Bildraten der Brille */
        float rates[16], cur = 0.0f;
        uint32_t nr = 0, k;
        char lst[128];
        size_t o = 0;
        lst[0] = '\0';
        if (XR_SUCCEEDED(pxrEnumerateDisplayRefreshRatesFB(xr_sess, 16, &nr, rates))) {
            for (k = 0; k < nr && k < 16u; k++) {
                int n = _snprintf_s(lst + o, sizeof lst - o, _TRUNCATE, "%s%.0f", k ? "/" : "", (double)rates[k]);
                if (n > 0) { o += (size_t)n; }
            }
        }
        pxrGetDisplayRefreshRateFB(xr_sess, &cur);
        HLOG_I("VR: Bildrate der Brille einstellbar (XR_FB_display_refresh_rate): %s Hz, aktuell %.0f Hz, Einstellung vr_refresh_hz=%d",
               lst[0] ? lst : "?", (double)cur, g_cfg.vr_refresh_hz);
    } else {
        HLOG_I("VR: Bildrate der Brille hier nicht einstellbar (Runtime ohne XR_FB_display_refresh_rate) - in der Runtime "
               "(z. B. Virtual Desktop) waehlen; vr_refresh_hz=%d wirkt nicht", g_cfg.vr_refresh_hz);
    }
    return 1;
}

static void xr_events(void)
{
    XrEventDataBuffer ev;
    int guard = 0;
    for (;;) {
        XrResult r;
        memset(&ev, 0, sizeof ev);
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        r = pxrPollEvent(xr_inst, &ev);
        if (r != XR_SUCCESS || ++guard > 32) { break; }
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged *sc = (const XrEventDataSessionStateChanged *)&ev;
            XrSessionState prev = xr_state;
            xr_state = sc->state;
            HLOG_I("VR: Session-Zustand %d", (int)xr_state);
            if (xr_state == XR_SESSION_STATE_READY && !xr_running) {
                XrSessionBeginInfo bi;
                memset(&bi, 0, sizeof bi);
                bi.type = XR_TYPE_SESSION_BEGIN_INFO;
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                r = pxrBeginSession(xr_sess, &bi);
                xr_running = XR_SUCCEEDED(r);
                HLOG_I("VR: xrBeginSession %d -> %s", (int)r, xr_running ? "laeuft" : "FEHLER");
            } else if (xr_state == XR_SESSION_STATE_STOPPING && xr_running) {
                pxrEndSession(xr_sess);
                xr_running = 0;
                HLOG_I("VR: Session beendet (STOPPING)");
            } else if (xr_state == XR_SESSION_STATE_EXITING || xr_state == XR_SESSION_STATE_LOSS_PENDING) {
                xr_quit = 1;
                HLOG_W("VR: Session %s -> VR aus, Spiel laeuft auf dem Monitor weiter",
                       xr_state == XR_SESSION_STATE_EXITING ? "beendet (EXITING)" : "verloren (LOSS_PENDING)");
            } else if (prev == XR_SESSION_STATE_FOCUSED &&
                       (xr_state == XR_SESSION_STATE_VISIBLE || xr_state == XR_SESSION_STATE_SYNCHRONIZED)) {
                HLOG_I("VR: Brille nicht im Fokus (Zustand %d, z. B. abgesetzt/Systemmenue) - Monitor laeuft weiter", (int)xr_state);
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            xr_quit = 1;
            HLOG_W("VR: Runtime beendet sich (INSTANCE_LOSS_PENDING) -> VR aus, Spiel laeuft auf dem Monitor weiter");
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            HLOG_I("VR: Raum-Wechsel der Runtime (System-Recentering)");
        } else if (ev.type == XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB) { /* W183 */
            const XrEventDataDisplayRefreshRateChangedFB *rc = (const XrEventDataDisplayRefreshRateChangedFB *)&ev;
            HLOG_I("VR: Bildrate der Brille %.0f -> %.0f Hz", (double)rc->fromDisplayRefreshRate, (double)rc->toDisplayRefreshRate);
        }
    }
}

/* DIAG(VR-PROTO,W134): Augenziel (beliebige Groesse) als JPG nach shots/
 * (sb_render_shot_name kann nur BB-Groesse). Unter ctx_lock. */
static void vr_shot_tex(const char *name)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *stg = NULL;
    D3D11_MAPPED_SUBRESOURCE map;
    unsigned char *bgr;
    unsigned x, y;
    char path[MAX_PATH];
    if (!vr_tex) { return; }
    ID3D11Texture2D_GetDesc(vr_tex, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &stg))) { return; }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)stg, (ID3D11Resource *)vr_tex);
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &map))) {
        bgr = (unsigned char *)malloc((size_t)td.Width * td.Height * 3u);
        if (bgr) {
            for (y = 0; y < td.Height; y++) {
                const unsigned char *src = (const unsigned char *)map.pData + (size_t)y * map.RowPitch;
                unsigned char *dst = bgr + (size_t)y * td.Width * 3u;
                for (x = 0; x < td.Width; x++) { dst[x * 3] = src[x * 4 + 2]; dst[x * 3 + 1] = src[x * 4 + 1]; dst[x * 3 + 2] = src[x * 4]; }
            }
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s/shots/%s.jpg", hcfg_out_dir(), name);
            sb_render_write_jpg(path, bgr, td.Width, td.Height);
            free(bgr);
        }
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)stg, 0);
    }
    ID3D11Texture2D_Release(stg);
}

/* W145: OpenXR-Aufruf merken (Haenger-Erkennung durch den Watchdog, ohne Sperre) */
static void xr_mark(const char *n)
{
    xr_call_q = vr_now();
    xr_call = n;
}

static void xr_unmark(void)
{
    if (xr_call && InterlockedExchange(&xr_hang_logged, 0)) {
        HLOG_I("VR: Runtime antwortet wieder (%s nach %.1f s)", xr_call,
               (double)(vr_now() - xr_call_q) / (double)vr_qpf.QuadPart);
    }
    xr_call = NULL;
}

static double xr_ms(LONGLONG q0)
{
    return (double)(vr_now() - q0) * 1000.0 / (double)vr_qpf.QuadPart;
}

static void xr_err(const char *what, XrResult r)
{
    static unsigned n;
    if (n++ < 8u) { HLOG_W("VR: %s %d (Spiel laeuft weiter)", what, (int)r); }
    if (r == XR_ERROR_SESSION_LOST || r == XR_ERROR_INSTANCE_LOST) {
        if (!xr_quit) { HLOG_W("VR: Session verloren (%d) -> VR aus, Spiel laeuft auf dem Monitor weiter", (int)r); }
        xr_quit = 1;
    }
}

static void xr_stat2(void)
{
    static LONGLONG q0;
    LONGLONG now = vr_now();
    if (!q0) { q0 = now; }
    if ((double)(now - q0) / (double)vr_qpf.QuadPart < 5.0) { return; }
    if (g_cfg.vr_diag >= 1 && st2_n) {
        HLOG_I("VR stat2: ctx_lock je Bild %.2fms (max %.2f), Warten max: xrWaitFrame %.1fms xrWaitSwapchainImage %.1fms xrEndFrame %.1fms, "
               "uebersprungen: Swapchain-Timeout %u Keyed-Mutex %u (%u Bilder)",
               st2_lock / st2_n, st2_lock_max, st2_wf_max, st2_wsc_max, st2_ef_max, st2_skip_tmo, st2_skip_km, st2_n);
    }
    st2_lock = st2_lock_max = st2_wsc_max = st2_ef_max = st2_wf_max = 0;
    st2_n = st2_skip_tmo = st2_skip_km = 0;
    q0 = now;
}

/* DIAG(VR-FIX2,W164) vr_pose_shots: je neue, ruhige Kopfpose (20 VR-Bilder
 * unveraendert, noch nicht gesichert) ein Augenbild. Fuer geskriptete Posen
 * der Mock-Runtime (SB_VR_MOCK_POSES=w164); Name traegt Vorlehnen (cm),
 * Seitversatz (cm), Rollen und Gieren (Grad). */
static float vr_q_roll(const float q[4])
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float ry = 2 * (x * y + z * w); /* y-Anteil der gedrehten Kopf-x-Achse */
    if (ry > 1) { ry = 1; } else if (ry < -1) { ry = -1; }
    return asinf(ry);
}

static int vr_pose_shot_due(const float hq[4], const float hp[3], char *nm, size_t cap)
{
    static float last[7], done[64][7];
    static unsigned stable, ndone;
    float cur[7];
    unsigned i, k;
    int same = 1;
    if (g_cfg.vr_pose_shots <= 0 || ndone >= 64u || ndone >= (unsigned)g_cfg.vr_pose_shots) { return 0; }
    cur[0] = hq[0]; cur[1] = hq[1]; cur[2] = hq[2]; cur[3] = hq[3]; cur[4] = hp[0]; cur[5] = hp[1]; cur[6] = hp[2];
    for (i = 0; i < 7; i++) { if (fabsf(cur[i] - last[i]) > 1e-4f) { same = 0; } }
    memcpy(last, cur, sizeof last);
    stable = same ? stable + 1u : 0u;
    if (stable != 20u) { return 0; }
    for (k = 0; k < ndone; k++) {
        int eq = 1;
        for (i = 0; i < 7; i++) { if (fabsf(cur[i] - done[k][i]) > 1e-3f) { eq = 0; } }
        if (eq) { return 0; }
    }
    memcpy(done[ndone++], cur, sizeof cur);
    _snprintf_s(nm, cap, _TRUNCATE, "vr_%05u_pose_v%02d_x%+03d_r%+03d_g%+03d", vr_frames + 1u,
                (int)floorf(-hp[2] * 100.0f + 0.5f), (int)floorf(hp[0] * 100.0f + 0.5f),
                (int)floorf(vr_q_roll(hq) * 180.0f / VR_PI + 0.5f), (int)floorf(q_yaw(hq) * 180.0f / VR_PI + 0.5f));
    return 1;
}

/* W183 VR-FIX3: vor jedem Bild (VR-Thread, ohne Sperre): Render-Skalierung und
 * Bildrate der Brille aus der Einstellung (ESC-Menue wirkt sofort). */
static void xr_frame_prep(void)
{
    int sc = vr_scale_pct();
    if (sc != xr_scale_cur) {
        uint32_t w, h;
        xr_eye_size(&w, &h);
        HLOG_I("VR: Render-Skalierung %d %% -> Auge %ux%u (Empfehlung %ux%u)%s", sc, w, h, xr_rec_w, xr_rec_h,
               ((unsigned)w + 2u < (xr_rec_w * (unsigned)sc) / 100u) ? " - begrenzt auf die angelegte Swapchain, mehr ab Neustart" : "");
        xr_ew = w; xr_eh = h;
        xr_scale_cur = sc;
    }
    if (xr_have_rr && g_cfg.vr_refresh_hz != xr_rr_req) {
        xr_rr_req = g_cfg.vr_refresh_hz;
        if (xr_rr_req > 0) {
            float rates[16], best = 0.0f;
            uint32_t nr = 0, k;
            XrResult r = pxrEnumerateDisplayRefreshRatesFB(xr_sess, 16, &nr, rates);
            for (k = 0; XR_SUCCEEDED(r) && k < nr && k < 16u; k++) {
                if (best <= 0.0f || fabsf(rates[k] - (float)xr_rr_req) < fabsf(best - (float)xr_rr_req)) { best = rates[k]; }
            }
            if (best > 0.0f) {
                r = pxrRequestDisplayRefreshRateFB(xr_sess, best);
                HLOG_I("VR: Bildrate der Brille %d Hz gewuenscht -> %.0f Hz angefordert (%d)", xr_rr_req, (double)best, (int)r);
            }
        }
    }
}

/* W183 VR-FIX3: Takt-Diagnose je eingereichtem Cockpit-Bild: Anzeige-Abstand
 * (verpasste Bilder), Pose je Bild (Cockpit mit neuer Kopfpose?), Paar/t/Halten
 * der Spielzeit, Sperr-Warten, Bildzeit. vr_frame_log=N: die ersten N Bilder der
 * Mission einzeln; vr_diag>=1: Summe alle 5 s ("VR takt"). */
static void xr_takt(const sb_vr_pass_t *p, const XrView *views, const float hq[4], const float hp[3], XrTime disp,
                    XrDuration per, double lw, LONGLONG q0, float tout)
{
    static LONGLONG w0;
    double dt = xr_last_disp ? (double)(disp - xr_last_disp) * 1e-6 : 0.0;
    double rep = (double)(vr_now() - q0) * 1000.0 / (double)vr_qpf.QuadPart, win;
    float cp[7];
    int i, newpose = 0;
    LONGLONG now = vr_now();
    cp[0] = views[0].pose.orientation.x; cp[1] = views[0].pose.orientation.y;
    cp[2] = views[0].pose.orientation.z; cp[3] = views[0].pose.orientation.w;
    cp[4] = views[0].pose.position.x; cp[5] = views[0].pose.position.y; cp[6] = views[0].pose.position.z;
    for (i = 0; i < 7; i++) { if (fabsf(cp[i] - tk_lastpose[i]) > 1e-6f) { newpose = 1; } }
    memcpy(tk_lastpose, cp, sizeof tk_lastpose);
    xr_last_disp = disp;
    if (!w0) { w0 = now; }
    tk_n++;
    if (per > 0 && dt > 1.5 * (double)per * 1e-6) { tk_miss++; }
    if (dt > tk_dt_max) { tk_dt_max = dt; }
    if (p->hold) { tk_hold++; }
    if (p->wtick_used) { tk_wtick++; }
    tk_nskip += p->nskip;
    if (newpose) { tk_newpose++; }
    tk_lw += lw; if (lw > tk_lw_max) { tk_lw_max = lw; }
    tk_rep += rep; if (rep > tk_rep_max) { tk_rep_max = rep; }
    if (g_cfg.vr_frame_log > 0 && tk_flog < (unsigned)g_cfg.vr_frame_log && sb_interp_vr_available()) {
        tk_flog++;
        HLOG_I("VR bild %u: anzeige +%.2fms kopf pos=(%.4f,%.4f,%.4f) gier=%.2f nicken=%.2f neue-pose=%d paar=%u/%u t=%.3f halten=%d "
               "welt=%s (rt-ops %u uebersprungen) cockpit-draws 3d=%u p2=%u 2d=%u sperre-warten=%.2fms bild=%.2fms",
               xr_fno, dt, hp[0], hp[1], hp[2], q_yaw(hq) * 180.0f / VR_PI, q_pitch(hq) * 180.0f / VR_PI, newpose, p->aser, p->bser,
               tout, p->hold, p->wtick_used ? "spieltakt" : "je-bild", p->nskip, p->n3d, p->np2, p->n2d, lw, rep);
    }
    win = (double)(now - w0) / (double)vr_qpf.QuadPart;
    if (win >= 5.0) {
        if (g_cfg.vr_diag >= 1 && tk_n) {
            HLOG_I("VR takt: bilder=%u (%.1f/s) verpasst=%u (max Abstand %.1fms, Periode %.1fms) halten=%u neue-pose=%u/%u "
                   "welt-spieltakt=%u/%u (rt-ops/bild %u) sperre-warten %.2fms (max %.2f) bild %.2fms (max %.2f) auge %ux%u (%d %%)",
                   tk_n, (double)tk_n / win, tk_miss, tk_dt_max, per > 0 ? (double)per * 1e-6 : 0.0, tk_hold, tk_newpose, tk_n,
                   tk_wtick, tk_n, tk_nskip / tk_n, tk_lw / tk_n, tk_lw_max, tk_rep / tk_n, tk_rep_max, xr_ew, xr_eh, xr_scale_cur);
        }
        tk_n = tk_miss = tk_hold = tk_wtick = tk_newpose = tk_nskip = 0;
        tk_lw = tk_lw_max = tk_rep = tk_rep_max = tk_dt_max = 0.0;
        w0 = now;
    }
}

/* ---------- W197 VR-MENU: ESC-Menue und F2 als Flaechen in der Brille ----------
 * Umsetzung im eigenen Projection-Layer (Augenziel), nicht als XrCompositionLayerQuad:
 *  - keine weiteren Swapchains/geteilten Texturen zwischen Spiel- und VR-Device (W145-Regel bleibt einfach),
 *  - keine Abhaengigkeit von der Quad-Layer-Umsetzung der Runtime (VDXR/SteamVR/Mock),
 *  - Augenbilder (Belege, Mock-Shots) zeigen genau, was die Brille zeigt.
 * Menue: beim Oeffnen vr_menu_dist vor der aktuellen Blickrichtung (Gieren + Nicken, +-30 Grad begrenzt), Flaeche
 * zum Kopf gedreht, danach weltfest im Sitz-Raum; ein Recentering (F8, ESC "Blick zentrieren") stellt sie neu vor
 * den Blick. Panel in eigener Textur (vr_menu_px, Mip-Kette), Welt dahinter abgedunkelt (vr_menu_dim), Mauszeiger
 * an der Stelle, die ein Klick im Fenster trifft. F2: kopffest oben rechts (vr_hud_*). Spiegel-Fenster: das
 * Augenbild wird VOR den Flaechen gesichert, das Fenster zeigt Menue/F2 flach wie am Desktop. */
static ID3D11PixelShader *vm_ps;      /* vormultipliziertes RGBA */
static ID3D11BlendState *vm_bs;       /* ONE / INV_SRC_ALPHA, nur RGB */
static ID3D11Texture2D *vm_cur_tex, *vm_dim_tex;
static ID3D11ShaderResourceView *vm_cur_srv, *vm_dim_srv;
static int vm_done, vm_dim_val = -1;
static int vm_open, vm_vis_log = -1, vm_hud_log = -1;
static float vm_c[3], vm_r[3], vm_u[3], vm_f[3]; /* Menue: Mitte (Sitz-Raum, m), Achsen rechts/oben, Blickrichtung */
static float vm_pyaw, vm_ppitch;
static float vm_hc[3];                /* F2: Mitte (Sitz-Raum, m) im letzten Bild */
static LONG vm_rc_seen;
static unsigned vm_rate_n;
static LONGLONG vm_rate_q0;
static float vm_rate;
#define VM_CUR_W 32u
#define VM_CUR_H 48u

static float vm_seg_d(float px, float py, float ax, float ay, float bx, float by)
{
    float dx = bx - ax, dy = by - ay, t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy + 1e-9f);
    if (t < 0.0f) { t = 0.0f; } else if (t > 1.0f) { t = 1.0f; }
    dx = ax + t * dx - px; dy = ay + t * dy - py;
    return sqrtf(dx * dx + dy * dy);
}

/* Pfeil-Zeiger (weiss, schwarzer Rand, 4x4-Abtastung, vormultipliziert), Spitze bei Texel (2,2) */
static void vm_cursor_bits(unsigned *px)
{
    static const float P[7][2] = { { 0, 0 }, { 0, 17 }, { 4, 13.2f }, { 7, 19.8f }, { 9.6f, 18.7f }, { 6.6f, 12.4f }, { 12, 12.4f } };
    const float sc = 2.15f, o = 2.0f;
    unsigned x, y;
    int i, j, k;
    for (y = 0; y < VM_CUR_H; y++) {
        for (x = 0; x < VM_CUR_W; x++) {
            float r = 0, a = 0;
            for (j = 0; j < 4; j++) {
                for (i = 0; i < 4; i++) {
                    float X = ((float)x + ((float)i + 0.5f) / 4.0f - o) / sc, Y = ((float)y + ((float)j + 0.5f) / 4.0f - o) / sc, d = 1e9f;
                    int in = 0;
                    for (k = 0; k < 7; k++) {
                        const float *A = P[k], *B = P[(k + 1) % 7];
                        float dd = vm_seg_d(X, Y, A[0], A[1], B[0], B[1]);
                        if (dd < d) { d = dd; }
                        if ((A[1] > Y) != (B[1] > Y) && X < (B[0] - A[0]) * (Y - A[1]) / (B[1] - A[1]) + A[0]) { in = !in; }
                    }
                    if (in) { r += 1.0f; a += 1.0f; }
                    else if (d * sc <= 1.6f) { a += 1.0f; }
                }
            }
            r /= 16.0f; a /= 16.0f;
            {
                unsigned c = (unsigned)(r * 255.0f + 0.5f), al = (unsigned)(a * 255.0f + 0.5f);
                px[y * VM_CUR_W + x] = (al << 24) | (c << 16) | (c << 8) | c; /* RGBA8 (R im niedrigsten Byte) */
            }
        }
    }
}

static int vm_tex_rgba(ID3D11Texture2D **tex, ID3D11ShaderResourceView **srv, const unsigned *px, unsigned w, unsigned h)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    if (*srv) { ID3D11ShaderResourceView_Release(*srv); *srv = NULL; }
    if (*tex) { ID3D11Texture2D_Release(*tex); *tex = NULL; }
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    memset(&sd, 0, sizeof sd);
    sd.pSysMem = px;
    sd.SysMemPitch = w * 4u;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, &sd, tex))) { *tex = NULL; return 0; }
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)*tex, NULL, srv))) {
        ID3D11Texture2D_Release(*tex); *tex = NULL; *srv = NULL; return 0;
    }
    return 1;
}

static int vm_init(void)
{
    static const char *ps =
        "Texture2D t0 : register(t0); SamplerState s0 : register(s0);\n"
        "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return t0.Sample(s0, uv); }\n";
    int dim = g_cfg.vr_menu_dim < 0 ? 0 : (g_cfg.vr_menu_dim > 90 ? 90 : g_cfg.vr_menu_dim);
    if (!vm_done) {
        ID3DBlob *b = NULL;
        D3D11_BLEND_DESC bd;
        unsigned *cur;
        vm_done = 1;
        if (!vq_init()) { return 0; }
        if (sb_im_compile(ps, "ps_4_0", &b)) {
            ID3D11Device_CreatePixelShader(g_dev, b->lpVtbl->GetBufferPointer(b), b->lpVtbl->GetBufferSize(b), NULL, &vm_ps);
            b->lpVtbl->Release(b);
        }
        memset(&bd, 0, sizeof bd);
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                                                   D3D11_COLOR_WRITE_ENABLE_BLUE;
        if (FAILED(ID3D11Device_CreateBlendState(g_dev, &bd, &vm_bs))) { vm_bs = NULL; }
        cur = (unsigned *)malloc(VM_CUR_W * VM_CUR_H * 4u);
        if (cur) {
            vm_cursor_bits(cur);
            vm_tex_rgba(&vm_cur_tex, &vm_cur_srv, cur, VM_CUR_W, VM_CUR_H);
            free(cur);
        }
        HLOG_I("VR-MENU: Flaechen-Shader %s (Menue %d mm vor dem Blick, Bezugsbox %d mm, Textur %d px, abdunkeln %d %%; "
               "F2 %d mm breit in %d mm, %d/%d Grad)", (vm_ps && vm_bs && vm_cur_srv) ? "bereit" : "FEHLER", g_cfg.vr_menu_dist,
               g_cfg.vr_menu_size, g_cfg.vr_menu_px, dim, g_cfg.vr_hud_size, g_cfg.vr_hud_dist, g_cfg.vr_hud_yaw, g_cfg.vr_hud_pitch);
    }
    if (dim != vm_dim_val) {
        unsigned px = (unsigned)(dim * 255 / 100) << 24; /* schwarz, Deckung dim % (vormultipliziert) */
        vm_dim_val = dim;
        vm_tex_rgba(&vm_dim_tex, &vm_dim_srv, &px, 1, 1);
    }
    return vm_ps && vm_bs && vm_cur_srv;
}

int sb_vr_panels_on(void)
{
    return vr_mode == 2 && g_cfg.vr_menu;
}

const char *sb_vr_hud_text(void)
{
    static char b[96];
    if (vr_mode != 2) { _snprintf_s(b, sizeof b, _TRUNCATE, "VR %s", vr_mode == 1 ? "Vorschau" : "aus"); }
    else { _snprintf_s(b, sizeof b, _TRUNCATE, "VR %5.1f B/s Auge %ux%u %d%%", (double)vm_rate, xr_ew, xr_eh, xr_scale_cur); }
    return b;
}

/* VR-Bildrate fuer die F2-Flaeche (eingereichte Bilder je Sekunde) */
static void vm_rate_tick(void)
{
    LONGLONG now = vr_now();
    if (!vm_rate_q0) { vm_rate_q0 = now; }
    vm_rate_n++;
    if ((double)(now - vm_rate_q0) >= (double)vr_qpf.QuadPart) {
        vm_rate = (float)((double)vm_rate_n * (double)vr_qpf.QuadPart / (double)(now - vm_rate_q0));
        vm_rate_n = 0;
        vm_rate_q0 = now;
    }
}

static void vm_v3(float o[3], float x, float y, float z) { o[0] = x; o[1] = y; o[2] = z; }

/* Kopfpose im Sitz-Raum: Rotation R (lokal -> Sitz, Zeilenvektor) und Kopfposition t (m, Sitz-Versatz eingerechnet) */
static void vm_head_seat(const float hq[4], const float hp[3], float R[16], float t[3])
{
    float M[16], RC[16], RCI[16], rq[4], z[3] = { 0, 0, 0 };
    m4_pose(M, hq, z);
    q_from_euler(rq, vr_rc_yaw, 0, 0);
    m4_pose(RC, rq, z);
    m4_rigid_inv(RCI, RC);
    m4_mul(R, M, RCI);
    vr_seat_head(hp, vr_rc_yaw, vr_rc_pos, t);
}

/* Punkt (Sitz-Raum, m) -> Clip des Auges e */
static void vm_clip(const sb_vr_pass_t *p, int e, const float pt[3], float o[4])
{
    float s = (float)g_cfg.vr_world_scale / 100.0f, x, y, z;
    const float *E = p->E[e];
    if (s <= 0.0f) { s = 1.0f; }
    x = pt[0] * s; y = pt[1] * s; z = pt[2] * s;
    o[0] = x * E[0] + y * E[4] + z * E[8] + E[12];
    o[1] = x * E[1] + y * E[5] + z * E[9] + E[13];
    o[2] = x * E[2] + y * E[6] + z * E[10] + E[14];
    o[3] = x * E[3] + y * E[7] + z * E[11] + E[15];
}

/* Rechteck (Ecken TL, TR, BR, BL im Sitz-Raum, m) mit srv je Auge zeichnen (vormultipliziert gemischt) */
static void vm_quad(const sb_vr_pass_t *p, float c[4][3], ID3D11ShaderResourceView *srv)
{
    static const int ix[6] = { 0, 1, 2, 0, 2, 3 };
    static const float uu[4] = { 0, 1, 1, 0 }, vv[4] = { 0, 0, 1, 1 };
    float v[36], cl[4];
    int e, k;
    for (e = 0; e < p->neyes; e++) {
        for (k = 0; k < 6; k++) {
            vm_clip(p, e, c[ix[k]], cl);
            v[k * 6 + 0] = cl[0]; v[k * 6 + 1] = cl[1]; v[k * 6 + 2] = cl[2]; v[k * 6 + 3] = cl[3];
            v[k * 6 + 4] = uu[ix[k]]; v[k * 6 + 5] = vv[ix[k]];
        }
        vq_draw_ex(p->rtv, p->vp[e], v, srv, vm_ps, vm_bs);
    }
}

/* Rechteck aus Mitte m, Achsen r/u (Einheitsvektoren) und Breite/Hoehe (m) */
static void vm_rect(float c[4][3], const float m[3], const float r[3], const float u[3], float w, float h)
{
    int k;
    for (k = 0; k < 3; k++) {
        c[0][k] = m[k] - 0.5f * w * r[k] + 0.5f * h * u[k];
        c[1][k] = m[k] + 0.5f * w * r[k] + 0.5f * h * u[k];
        c[2][k] = m[k] + 0.5f * w * r[k] - 0.5f * h * u[k];
        c[3][k] = m[k] - 0.5f * w * r[k] - 0.5f * h * u[k];
    }
}

/* Menue-Flaeche vor die aktuelle Blickrichtung (Sitz-Raum) legen */
static void vm_place(const float hq[4], const float hp[3], const char *why)
{
    float hs[3], yaw = q_yaw(hq) - vr_rc_yaw, pitch = q_pitch(hq), lim = 30.0f * VR_PI / 180.0f, d;
    float cy, sy, cp, sp;
    int k;
    while (yaw > VR_PI) { yaw -= 2.0f * VR_PI; }
    while (yaw < -VR_PI) { yaw += 2.0f * VR_PI; }
    if (pitch > lim) { pitch = lim; } else if (pitch < -lim) { pitch = -lim; }
    d = (float)(g_cfg.vr_menu_dist > 100 ? g_cfg.vr_menu_dist : 1750) / 1000.0f;
    vr_seat_head(hp, vr_rc_yaw, vr_rc_pos, hs);
    cy = cosf(yaw); sy = sinf(yaw); cp = cosf(pitch); sp = sinf(pitch);
    vm_v3(vm_f, -sy * cp, sp, -cy * cp);   /* Blickrichtung (RH, -z vorne, Gieren links positiv) */
    vm_v3(vm_r, cy, 0.0f, -sy);            /* rechts */
    vm_v3(vm_u, sy * sp, cp, cy * sp);     /* oben (zum Kopf geneigt) */
    for (k = 0; k < 3; k++) { vm_c[k] = hs[k] + d * vm_f[k]; }
    vm_pyaw = yaw * 180.0f / VR_PI;
    vm_ppitch = pitch * 180.0f / VR_PI;
    {
        static unsigned nlog;
        if (nlog++ < 60u) {
            HLOG_I("VR-MENU: Menue-Flaeche platziert (%s): vor dem Blick gier=%.1f nicken=%.1f (relativ zur Mitte), Abstand %.2f m, "
                   "Mitte im Sitz (%.3f,%.3f,%.3f) m, Kopf (%.3f,%.3f,%.3f) m - danach weltfest", why, (double)vm_pyaw, (double)vm_ppitch,
                   (double)d, (double)vm_c[0], (double)vm_c[1], (double)vm_c[2], (double)hs[0], (double)hs[1], (double)hs[2]);
        }
    }
}

/* VR-Thread, unter ctx_lock, nach Wiedergabe/Leinwand und nach der Spiegel-Sicherung:
 * Menue (weltfest) + Zeiger, F2 (kopffest). Rueckgabe Bit 0 = Menue gezeichnet, Bit 1 = F2. */
static int vm_draw(const sb_vr_pass_t *p, const float hq[4], const float hp[3], int *screen)
{
    sb_ovl_vr_t o;
    ID3D11ShaderResourceView *hsrv = NULL;
    unsigned htw = 0, hth = 0;
    float huw = 0, huh = 0, c[4][3];
    int vis, hud, res = 0, e;
    LONG g = vr_rc_gen;
    if (!g_cfg.vr_menu || !vm_init()) { return 0; }
    vis = sb_ovl_vr_panel(&o);
    hud = sb_phud_vr_panel(&hsrv, &htw, &hth, &huw, &huh);
    *screen = o.screen;
    if (o.open && (!vm_open || g != vm_rc_seen)) { vm_place(hq, hp, !vm_open ? "geoeffnet" : "Recentering"); }
    vm_rc_seen = g;
    if (o.open != vm_open) {
        static unsigned nlog;
        if (nlog++ < 60u) { HLOG_I("VR-MENU: ESC-Menue %s (Bildschirm %d)", o.open ? "offen -> Flaeche in der Brille" : "zu", o.screen); }
        vm_open = o.open;
    }
    if (vis != vm_vis_log) {
        static unsigned nlog;
        if (vis && nlog++ < 60u) {
            float um = (float)(g_cfg.vr_menu_size > 100 ? g_cfg.vr_menu_size : 1800) / 1000.0f / 540.0f;
            HLOG_I("VR-MENU: Flaeche sichtbar: Panel %ux%u Texel = %.2f x %.2f m (%.1f x %.1f Grad), Bildschirm %d", o.tw, o.th,
                   (double)(o.uw * um), (double)(o.uh * um),
                   2.0 * atan(0.5 * o.uw * um / ((g_cfg.vr_menu_dist > 100 ? g_cfg.vr_menu_dist : 1750) / 1000.0)) * 180.0 / VR_PI,
                   2.0 * atan(0.5 * o.uh * um / ((g_cfg.vr_menu_dist > 100 ? g_cfg.vr_menu_dist : 1750) / 1000.0)) * 180.0 / VR_PI, o.screen);
        }
        vm_vis_log = vis;
    }
    if (hud != vm_hud_log) {
        static unsigned nlog;
        if (nlog++ < 40u) {
            HLOG_I("VR-MENU: F2-Flaeche %s (kopffest, %d mm breit in %d mm, rechts %d / oben %d Grad, Panel %ux%u Texel)",
                   hud ? "an" : "aus", g_cfg.vr_hud_size, g_cfg.vr_hud_dist, g_cfg.vr_hud_yaw, g_cfg.vr_hud_pitch, htw, hth);
        }
        vm_hud_log = hud;
    }
    if (vis) {
        float um = (float)(g_cfg.vr_menu_size > 100 ? g_cfg.vr_menu_size : 1800) / 1000.0f / 540.0f;
        float w = o.uw * um, h = o.uh * um;
        if (vm_dim_srv && vm_dim_val > 0) { /* Welt dahinter abdunkeln (Pause sichtbar, Menue hebt sich ab) */
            float v[36];
            vq_quad(v, 0.0f, 1.0f, 0.0f, 1.0f);
            for (e = 0; e < p->neyes; e++) { vq_draw_ex(p->rtv, p->vp[e], v, vm_dim_srv, vm_ps, vm_bs); }
        }
        vm_rect(c, vm_c, vm_r, vm_u, w, h);
        vm_quad(p, c, o.srv);
        if (o.cursor && o.cu > -0.5f && o.cu < 1.5f && o.cv > -0.5f && o.cv < 1.5f) {
            float tip[3], ch = 16.0f * um, cw = ch * (float)VM_CUR_W / (float)VM_CUR_H, tl[3];
            int k;
            for (k = 0; k < 3; k++) {
                tip[k] = vm_c[k] + (o.cu - 0.5f) * w * vm_r[k] + (0.5f - o.cv) * h * vm_u[k] - 0.003f * vm_f[k]; /* 3 mm vor dem Panel */
                tl[k] = tip[k] - (2.0f / (float)VM_CUR_W) * cw * vm_r[k] + (2.0f / (float)VM_CUR_H) * ch * vm_u[k];
            }
            for (k = 0; k < 3; k++) {
                c[0][k] = tl[k];
                c[1][k] = tl[k] + cw * vm_r[k];
                c[2][k] = tl[k] + cw * vm_r[k] - ch * vm_u[k];
                c[3][k] = tl[k] - ch * vm_u[k];
            }
            vm_quad(p, c, vm_cur_srv);
        }
        res |= 1;
    }
    if (hud && hsrv && huw > 0.0f) {
        float R[16], t[3], d = (float)(g_cfg.vr_hud_dist > 100 ? g_cfg.vr_hud_dist : 1000) / 1000.0f;
        float w = (float)(g_cfg.vr_hud_size > 20 ? g_cfg.vr_hud_size : 300) / 1000.0f, h = w * huh / huw;
        float dir[3], rr[3], uu[3], m[3], ln, lc[4][3];
        int k, j;
        vm_v3(dir, tanf((float)g_cfg.vr_hud_yaw * VR_PI / 180.0f), tanf((float)g_cfg.vr_hud_pitch * VR_PI / 180.0f), -1.0f);
        ln = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        for (k = 0; k < 3; k++) { dir[k] /= ln; }
        vm_v3(rr, -dir[2], 0.0f, dir[0]);
        ln = sqrtf(rr[0] * rr[0] + rr[2] * rr[2]);
        rr[0] /= ln; rr[2] /= ln;
        vm_v3(uu, rr[1] * dir[2] - rr[2] * dir[1], rr[2] * dir[0] - rr[0] * dir[2], rr[0] * dir[1] - rr[1] * dir[0]);
        for (k = 0; k < 3; k++) { m[k] = d * dir[k]; }
        vm_rect(lc, m, rr, uu, w, h);
        vm_head_seat(hq, hp, R, t);
        for (j = 0; j < 4; j++) { /* Kopf-Raum -> Sitz-Raum */
            float x = lc[j][0], y = lc[j][1], z = lc[j][2];
            for (k = 0; k < 3; k++) { c[j][k] = x * R[k] + y * R[4 + k] + z * R[8 + k] + t[k]; }
        }
        for (k = 0; k < 3; k++) { vm_hc[k] = m[0] * R[k] + m[1] * R[4 + k] + m[2] * R[8 + k] + t[k]; }
        vm_quad(p, c, hsrv);
        res |= 2;
    }
    return res;
}

/* DIAG(VR-MENU,W197) vr_menu_shots: je neue ruhige Kopfpose bzw. neuer Menue-Zustand (offen, Bildschirm, F2) ein
 * Augenbild + Zeile mit der Panel-Mitte im linken Auge (NDC): weltfest = Mitte wandert gegen die Kopfdrehung,
 * kopffest = F2-Mitte bleibt gleich. */
static void vm_shot(const sb_vr_pass_t *p, int flags, int screen, const float hq[4], const float hp[3])
{
    static float last[9], done[64][9];
    static unsigned stable, ndone;
    float cur[9], cl[4], mx = 0, my = 0, hx = 0, hy = 0;
    unsigned i, k;
    int same = 1;
    char nm[96];
    if (g_cfg.vr_menu_shots <= 0 || ndone >= 64u || ndone >= (unsigned)g_cfg.vr_menu_shots) { return; }
    cur[0] = hq[0]; cur[1] = hq[1]; cur[2] = hq[2]; cur[3] = hq[3]; cur[4] = hp[0]; cur[5] = hp[1]; cur[6] = hp[2];
    cur[7] = (float)flags; cur[8] = (float)screen;
    for (i = 0; i < 9; i++) { if (fabsf(cur[i] - last[i]) > 1e-4f) { same = 0; } }
    memcpy(last, cur, sizeof last);
    stable = same ? stable + 1u : 0u;
    if (stable != 20u) { return; }
    for (k = 0; k < ndone; k++) {
        int eq = 1;
        for (i = 0; i < 9; i++) { if (fabsf(cur[i] - done[k][i]) > 1e-3f) { eq = 0; } }
        if (eq) { return; }
    }
    memcpy(done[ndone++], cur, sizeof cur);
    if (flags & 1) { vm_clip(p, 0, vm_c, cl); if (cl[3] > 1e-6f) { mx = cl[0] / cl[3]; my = cl[1] / cl[3]; } }
    if (flags & 2) { vm_clip(p, 0, vm_hc, cl); if (cl[3] > 1e-6f) { hx = cl[0] / cl[3]; hy = cl[1] / cl[3]; } }
    _snprintf_s(nm, sizeof nm, _TRUNCATE, "vr_%05u_menue_%s_b%d_g%+03d_v%02d", vr_frames + 1u,
                flags == 3 ? "menue_f2" : (flags & 1) ? "menue" : "f2", screen,
                (int)floorf((q_yaw(hq) - vr_rc_yaw) * 180.0f / VR_PI + 0.5f), (int)floorf(-hp[2] * 100.0f + 0.5f));
    vr_shot_tex(nm);
    HLOG_I("VR menue-shot %s: kopf gier=%.1f nicken=%.1f (relativ zur Mitte %.1f) vor=%.2fm menue=%d bildschirm=%d "
           "mitte-links-auge ndc=(%.3f,%.3f) platziert gier=%.1f nicken=%.1f f2=%d f2-links-auge ndc=(%.3f,%.3f)", nm,
           q_yaw(hq) * 180.0f / VR_PI, q_pitch(hq) * 180.0f / VR_PI, (q_yaw(hq) - vr_rc_yaw) * 180.0f / VR_PI, -hp[2],
           flags & 1, screen, mx, my, vm_pyaw, vm_ppitch, (flags >> 1) & 1, hx, hy);
}

/* Ein OpenXR-Bild (W145-Reihenfolge, nie ein OpenXR-Aufruf unter ctx_lock):
 *  1 xrWaitFrame, 2 xrBeginFrame, 3 xrAcquire-/xrWaitSwapchainImage,
 *  4 kurz ctx_lock: Wiedergabe ins geteilte Augenziel (Schluessel 0 -> 1),
 *  5 VR-Device: Kopie ins Swapchain-Bild (Schluessel 1 -> 0), xrReleaseSwapchainImage,
 *  6 xrEndFrame. */
static void xr_frame(void)
{
    XrFrameWaitInfo fwi;
    XrFrameState fs;
    XrFrameBeginInfo fbi;
    XrFrameEndInfo fei;
    XrViewLocateInfo vli;
    XrViewState vs;
    XrView views[2];
    XrCompositionLayerProjectionView pv[2];
    XrCompositionLayerProjection layer;
    const XrCompositionLayerBaseHeader *layers[1];
    uint32_t nv = 0;
    XrResult r;
    int e, drawn = 0, have_img = 0, cv = 0, vmf = 0, vmscr = 0;
    LONGLONG q0, qw, qe = 0;
    float tout = 1.0f, efov[4] = { 0, 0, 0, 0 };
    int rc_shot = 0;
    double lw = 0.0;
    sb_vr_pass_t p;
    xr_loop_q = vr_now();
    xr_stage = "Bildbeginn";
    if (InterlockedExchange(&xr_stage_logged, 0)) { HLOG_I("VR: VR-Thread laeuft wieder"); }
    memset(&fwi, 0, sizeof fwi);
    fwi.type = XR_TYPE_FRAME_WAIT_INFO;
    memset(&fs, 0, sizeof fs);
    fs.type = XR_TYPE_FRAME_STATE;
    qw = vr_now();
    xr_mark("xrWaitFrame");
    r = pxrWaitFrame(xr_sess, &fwi, &fs);
    xr_unmark();
    { double ms = xr_ms(qw); if (ms > st2_wf_max) { st2_wf_max = ms; } }
    if (XR_FAILED(r)) { xr_err("xrWaitFrame", r); Sleep(5); return; }
    memset(&fbi, 0, sizeof fbi);
    fbi.type = XR_TYPE_FRAME_BEGIN_INFO;
    xr_mark("xrBeginFrame");
    r = pxrBeginFrame(xr_sess, &fbi);
    xr_unmark();
    if (XR_FAILED(r)) { xr_err("xrBeginFrame", r); return; }
    q0 = vr_now();
    memset(&fei, 0, sizeof fei);
    fei.type = XR_TYPE_FRAME_END_INFO;
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    xr_frame_prep();
    {   /* W183 VR-FIX3 vr_idle_unfocused: Brille ab/Systemmenue (nicht FOCUSED) -> keine Wiedergabe, leeres Bild */
        int idle = g_cfg.vr_idle_unfocused && xr_state != XR_SESSION_STATE_FOCUSED;
        if (idle != xr_focus_log) {
            if (xr_focus_log >= 0 || idle) {
                HLOG_I("VR: %s", idle ? "Brille nicht im Fokus -> keine VR-Bilder (spart Leistung), Fenster zeigt das Spielbild"
                                      : "Brille im Fokus -> VR-Bilder an");
            }
            xr_focus_log = idle;
        }
        if (idle) {
            fs.shouldRender = 0;
            InterlockedExchange(&vr_pv_valid, 0);
            vr_last_xr_q = 0;
        }
    }
    if (fs.shouldRender) {
        memset(views, 0, sizeof views);
        views[0].type = views[1].type = XR_TYPE_VIEW;
        memset(&vs, 0, sizeof vs);
        vs.type = XR_TYPE_VIEW_STATE;
        memset(&vli, 0, sizeof vli);
        vli.type = XR_TYPE_VIEW_LOCATE_INFO;
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = xr_space;
        r = pxrLocateViews(xr_sess, &vli, &vs, 2, &nv, views);
        if (XR_SUCCEEDED(r) && nv == 2 && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
            float hq[4], hp[3];
            /* 3. Swapchain-Bild ohne Sperre erwerben und abwarten (Runtime darf hier blockieren) */
            if (!xr_sc_held) {
                XrSwapchainImageAcquireInfo ai;
                memset(&ai, 0, sizeof ai); ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
                xr_mark("xrAcquireSwapchainImage");
                r = pxrAcquireSwapchainImage(xr_sc, &ai, &xr_sc_idx);
                xr_unmark();
                if (XR_SUCCEEDED(r) && xr_sc_idx < xr_nimg) { xr_sc_held = 1; }
                else if (XR_FAILED(r)) { xr_err("xrAcquireSwapchainImage", r); }
            }
            if (xr_sc_held) {
                XrSwapchainImageWaitInfo wi;
                memset(&wi, 0, sizeof wi); wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO; wi.timeout = 100000000LL;
                qw = vr_now();
                xr_mark("xrWaitSwapchainImage");
                r = pxrWaitSwapchainImage(xr_sc, &wi);
                xr_unmark();
                { double ms = xr_ms(qw); if (ms > st2_wsc_max) { st2_wsc_max = ms; } }
                if (r == XR_SUCCESS) { have_img = 1; }
                else if (r == XR_TIMEOUT_EXPIRED) {
                    static unsigned ntmo;
                    st2_skip_tmo++;
                    if (ntmo++ < 4u) { HLOG_W("VR: xrWaitSwapchainImage Timeout (Runtime haelt das Bild) -> Bild ausgelassen, Spiel laeuft weiter"); }
                } else {
                    xr_err("xrWaitSwapchainImage", r);
                }
            }
            /* Kopf = Mitte der Augen, Richtung von Auge 0 */
            hq[0] = views[0].pose.orientation.x; hq[1] = views[0].pose.orientation.y;
            hq[2] = views[0].pose.orientation.z; hq[3] = views[0].pose.orientation.w;
            hp[0] = 0.5f * (views[0].pose.position.x + views[1].pose.position.x);
            hp[1] = 0.5f * (views[0].pose.position.y + views[1].pose.position.y);
            hp[2] = 0.5f * (views[0].pose.position.z + views[1].pose.position.z);
            vr_yaw = q_yaw(hq); vr_pitch = q_pitch(hq);
            if (g_cfg.vr_recenter_start && !xr_rc_started && have_img && xr_state == XR_SESSION_STATE_FOCUSED) {
                /* W183 VR-FIX3: Startposition = Sitzposition (Richtung und Position), einmal je Session */
                xr_rc_started = 1;
                strcpy_s(vr_rc_why, sizeof vr_rc_why, "Start");
                vr_recenter_apply(hq, hp);
            }
            /* Recentering in zwei Bildern: 1 = dieses Bild noch alte Mitte (Shot "vor"),
             * 2 = naechstes Bild neue Mitte (Shot "nach") */
            if (have_img) {
                if (vr_rc_req == 2) { InterlockedExchange(&vr_rc_req, 0); vr_recenter_apply(hq, hp); rc_shot = 2; }
                else if (vr_rc_req == 1) { InterlockedExchange(&vr_rc_req, 2); rc_shot = 1; }
            }
            memset(&p, 0, sizeof p);
            {
                float fov_v;
                vr_game_proj(&p.ia, &p.ib, &fov_v);
            }
            p.d2d = vr_d2d();
            p.rz0 = vr_rz0;
            p.rl = vr_rl;
            p.rmax = (float)g_cfg.vr_relief / 1000.0f;
            p.wantz = !vr_relief_done;
            p.neyes = 2;
            p.wfix = g_cfg.vr_world_depth ? 1 : 0; /* W164 */
            p.ifix = g_cfg.vr_inst_depth ? 1 : 0;
            p.wtick = g_cfg.vr_world_tick ? 1 : 0; /* W183 */
            p.noshade = g_cfg.vr_shading ? 0 : 1;
            { /* DIAG(VR-FIX2,W164): alle BB-Draws einer Mission-Wiedergabe (600. Cockpit-Bild) */
                static unsigned nck;
                if (g_cfg.vr_diag >= 2 && sb_interp_vr_available() && ++nck == 600u) { p.dump = 800; }
            }
            for (e = 0; e < 2; e++) {
                float q[4], pos[3];
                q[0] = views[e].pose.orientation.x; q[1] = views[e].pose.orientation.y;
                q[2] = views[e].pose.orientation.z; q[3] = views[e].pose.orientation.w;
                pos[0] = views[e].pose.position.x; pos[1] = views[e].pose.position.y; pos[2] = views[e].pose.position.z;
                vr_eye_matrix(p.E[e], q, pos, tanf(views[e].fov.angleLeft), tanf(views[e].fov.angleRight),
                              tanf(views[e].fov.angleUp), tanf(views[e].fov.angleDown));
                p.vp[e][0] = (float)(e * xr_ew); p.vp[e][1] = 0; p.vp[e][2] = (float)xr_ew; p.vp[e][3] = (float)xr_eh;
            }
            if (have_img) {
                int km_ok = 0;
                LONGLONG ql;
                /* Schluessel liegt noch bei 1 (VR-Seite scheiterte im letzten Bild):
                 * erst ohne ctx_lock zuruecksetzen, nie das Spiel darauf warten lassen */
                if (xr_km_pending && xr_km && IDXGIKeyedMutex_AcquireSync(xr_km, 1, 100) == S_OK) {
                    IDXGIKeyedMutex_ReleaseSync(xr_km, 0);
                    xr_km_pending = 0;
                }
                /* 4. kurze Sperre: nur Wiedergabe ins geteilte Augenziel (Schluessel 0
                 * ist nach Schritt 5 des letzten Bildes sofort frei: Timeout 1 ms) */
                qe = vr_now();
                xr_stage = "ctx_lock";
                sb_ctx_enter();
                xr_stage = "Wiedergabe (unter ctx_lock)";
                ql = vr_now();
                lw = (double)(ql - qe) * 1000.0 / (double)vr_qpf.QuadPart; /* W183: Warten auf ctx_lock */
                if (!xr_km_pending && vr_ensure_target(xr_ew * 2u, xr_eh) &&
                    IDXGIKeyedMutex_AcquireSync(vr_km, 0, 1) == S_OK) {
                    /* W183 VR-FIX3: Spielzeit der Wiedergabe = jetzt (- Latenz L in ip_pair_at) wie das Fenster.
                     * W134 rechnete jetzt + Bildperiode: dann lag der Zeitpunkt in den letzten ~P ms jedes Ticks
                     * hinter dem juengsten Tick -> "halten" (Bild steht) und Sprung beim naechsten Tick = 30-Hz-Ruckeln */
                    LONGLONG due = vr_now() + (g_cfg.vr_due_ahead ?
                        (LONGLONG)((double)fs.predictedDisplayPeriod * 1e-9 * (double)vr_qpf.QuadPart) : 0);
                    p.rtv = vr_rtv;
                    p.dsv = vr_dsv;
                    drawn = sb_interp_vr_replay(&p, NULL, NULL, 1.0f, due, &tout);
                    if (!drawn) {
                        if (vr_canvas_draw(&p)) { cv = 1; } /* W164: Menue/Film/2D auf der Leinwand */
                        else {
                            float black[4] = { 0, 0, 0, 1 };
                            ID3D11DeviceContext_ClearRenderTargetView(g_ctx, vr_rtv, black);
                        }
                    }
                    xr_fno++;
                    if (vr_preview_xr_on()) { /* W164: Fenster-Vorschau = eingereichtes Augenbild (W197: vor den Menue-Flaechen) */
                        int pe = vr_prev_kind == 2 ? 1 : 0;
                        float pq[4], pp[3];
                        pq[0] = views[pe].pose.orientation.x; pq[1] = views[pe].pose.orientation.y;
                        pq[2] = views[pe].pose.orientation.z; pq[3] = views[pe].pose.orientation.w;
                        pp[0] = views[pe].pose.position.x; pp[1] = views[pe].pose.position.y; pp[2] = views[pe].pose.position.z;
                        efov[0] = tanf(views[pe].fov.angleLeft); efov[1] = tanf(views[pe].fov.angleRight);
                        efov[2] = tanf(views[pe].fov.angleUp); efov[3] = tanf(views[pe].fov.angleDown);
                        vr_preview_capture(xr_fno, pq, pp, efov);
                    }
                    vmf = vm_draw(&p, hq, hp, &vmscr); /* W197 VR-MENU: ESC-Menue (weltfest) + F2 (kopffest) */
                    if ((drawn || cv) && g_cfg.vr_shots > 0 && vr_shots_n < 24u &&
                        (rc_shot || ((vr_frames + 1u) % (unsigned)g_cfg.vr_shots) == 0u)) {
                        char nm[64];
                        vr_shots_n++;
                        _snprintf_s(nm, sizeof nm, _TRUNCATE, "vr_%05u_xr%s%s", vr_frames + 1u,
                                    rc_shot == 1 ? "_vor_recenter" : rc_shot == 2 ? "_nach_recenter" : "", cv ? "_leinwand" : "");
                        vr_shot_tex(nm);
                        HLOG_I("VR shot %s: kopf yaw=%.1f pitch=%.1f mitte yaw=%.1f (relativ %.1f) t=%.2f draws 3d=%u 2d=%u p2=%u",
                               nm, vr_yaw * 180.0f / VR_PI, vr_pitch * 180.0f / VR_PI, vr_rc_yaw * 180.0f / VR_PI,
                               (vr_yaw - vr_rc_yaw) * 180.0f / VR_PI, tout, p.n3d, p.n2d, p.np2);
                    }
                    if (drawn && g_cfg.vr_pose_shots > 0) { /* DIAG(VR-FIX2,W164) */
                        char nm[96];
                        if (vr_pose_shot_due(hq, hp, nm, sizeof nm)) {
                            vr_shot_tex(nm);
                            HLOG_I("VR pose-shot %s: kopf pos=(%.3f,%.3f,%.3f) gier=%.1f nicken=%.1f rollen=%.1f t=%.2f draws 3d=%u 2d=%u p2=%u",
                                   nm, hp[0], hp[1], hp[2], vr_yaw * 180.0f / VR_PI, vr_pitch * 180.0f / VR_PI,
                                   vr_q_roll(hq) * 180.0f / VR_PI, tout, p.n3d, p.n2d, p.np2);
                        }
                    }
                    if (vmf) { vm_shot(&p, vmf, vmscr, hq, hp); } /* DIAG(VR-MENU,W197) */
                    if (g_cfg.vr_diag >= 1) { /* W164: eingereichte Pose bei Wechsel (Abgleich mit Vorschau/Mock) */
                        static float lp[7];
                        static unsigned nlog;
                        float cp[7];
                        int i, ch = 0;
                        cp[0] = views[0].pose.orientation.x; cp[1] = views[0].pose.orientation.y;
                        cp[2] = views[0].pose.orientation.z; cp[3] = views[0].pose.orientation.w;
                        cp[4] = views[0].pose.position.x; cp[5] = views[0].pose.position.y; cp[6] = views[0].pose.position.z;
                        for (i = 0; i < 7; i++) { if (fabsf(cp[i] - lp[i]) > 1e-3f) { ch = 1; } }
                        if (ch && nlog++ < 200u) {
                            HLOG_I("VR xr-bild %u eingereicht (%s): ansicht0 pos=(%.3f,%.3f,%.3f) q=(%.4f,%.4f,%.4f,%.4f)", xr_fno,
                                   drawn ? "cockpit" : (cv ? "leinwand" : "schwarz"), cp[4], cp[5], cp[6], cp[0], cp[1], cp[2], cp[3]);
                            memcpy(lp, cp, sizeof lp);
                        }
                    }
                    {
                        HRESULT kh = IDXGIKeyedMutex_ReleaseSync(vr_km, 1);
                        if (g_cfg.vr_diag >= 3 && xr_fno <= 3u) {
                            HLOG_I("VR handoff %u: producer ReleaseSync(1)=0x%08lX", xr_fno, (unsigned long)kh);
                        }
                    }
                    ID3D11DeviceContext_Flush(g_ctx); /* Augenbild zur GPU, bevor das VR-Device darauf wartet */
                    km_ok = 1;
                    xr_km_pending = 1;
                }
                {
                    double ms = xr_ms(ql);
                    st2_lock += ms; if (ms > st2_lock_max) { st2_lock_max = ms; }
                    st2_n++;
                }
                sb_ctx_leave();
                xr_stage = "AcquireSync (Keyed Mutex VR-Device)";
                /* 5. VR-Device (ohne ctx_lock): Kopie ins Swapchain-Bild */
                if (km_ok && IDXGIKeyedMutex_AcquireSync(xr_km, 1, 1000) == S_OK) {
                    xr_stage = "Kopie: CopySubresourceRegion (VR-Device)";
                    ID3D11DeviceContext_CopySubresourceRegion(xr_ctx, (ID3D11Resource *)xr_img[xr_sc_idx], 0, 0, 0, 0,
                                                              (ID3D11Resource *)xr_shtex, 0, NULL);
                    xr_stage = "Kopie: ReleaseSync (VR-Device)";
                    IDXGIKeyedMutex_ReleaseSync(xr_km, 0);
                    xr_stage = "Kopie fertig";
                    xr_km_pending = 0;
                } else {
                    static unsigned nkm;
                    st2_skip_km++;
                    if (nkm++ < 4u) { HLOG_W("VR: Augenziel nicht verfuegbar (Keyed Mutex/Ziel, km=%d) -> Bild ausgelassen", km_ok); }
                    if (km_ok) { drawn = 0; cv = 0; }
                }
            }
            if (xr_sc_held && have_img) {
                XrSwapchainImageReleaseInfo ri;
                memset(&ri, 0, sizeof ri); ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
                xr_mark("xrReleaseSwapchainImage");
                r = pxrReleaseSwapchainImage(xr_sc, &ri);
                xr_unmark();
                xr_sc_held = 0;
                /* Runtime-Zaun aus Release sicher zur GPU (VDXR-Vermutung: Signal ohne Flush) */
                xr_stage = "Flush VR-Device";
                ID3D11DeviceContext_Flush(xr_ctx);
                xr_stage = "nach Release";
                if (XR_FAILED(r)) { xr_err("xrReleaseSwapchainImage", r); }
                else if (have_img) {
                    for (e = 0; e < 2; e++) {
                        memset(&pv[e], 0, sizeof pv[e]);
                        pv[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                        pv[e].pose = views[e].pose;
                        pv[e].fov = views[e].fov;
                        pv[e].subImage.swapchain = xr_sc;
                        pv[e].subImage.imageRect.offset.x = (int32_t)(e * xr_ew);
                        pv[e].subImage.imageRect.extent.width = (int32_t)xr_ew;
                        pv[e].subImage.imageRect.extent.height = (int32_t)xr_eh;
                    }
                    memset(&layer, 0, sizeof layer);
                    layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
                    layer.space = xr_space;
                    layer.viewCount = 2;
                    layer.views = pv;
                    layers[0] = (const XrCompositionLayerBaseHeader *)&layer;
                    fei.layerCount = 1;
                    fei.layers = layers;
                    if (drawn || cv) { vr_last_xr_q = vr_now(); } /* W183: Fenster-Spiegel aktiv */
                    vm_rate_tick(); /* W197: VR-Bildrate fuer die F2-Flaeche */
                }
            }
            if (drawn) { xr_takt(&p, views, hq, hp, fs.predictedDisplayTime, fs.predictedDisplayPeriod, lw, q0, tout); }
            if (cv && !drawn) { vr_frames++; } /* W164: Leinwand-Bilder zaehlen fuer vr_shots mit */
            if (drawn) {
                vr_frames++;
                vr_auto_update(&p);
                vr_stat_add(&p, (double)(vr_now() - q0) * 1000.0 / (double)vr_qpf.QuadPart, tout);
            }
        }
    }
    xr_stage = "vor xrEndFrame";
    /* 6. xrEndFrame ohne Sperre (die Runtime nutzt hoechstens den Context des VR-Devices) */
    qw = vr_now();
    xr_mark("xrEndFrame");
    r = pxrEndFrame(xr_sess, &fei);
    xr_unmark();
    { double ms = xr_ms(qw); if (ms > st2_ef_max) { st2_ef_max = ms; } }
    if (XR_FAILED(r)) {
        static unsigned nerr;
        if (nerr++ < 4u) { HLOG_W("VR: xrEndFrame %d", (int)r); }
        if (r == XR_ERROR_SESSION_LOST || r == XR_ERROR_INSTANCE_LOST) { xr_err("xrEndFrame", r); }
    }
    xr_stat2();
}

static DWORD WINAPI xr_thread_main(LPVOID unused)
{
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    (void)unused;
    xr_tid = GetCurrentThreadId();
    if (!xr_init()) {
        vr_mode = (vr_prev_kind <= 2) ? 1 : 0;
        HLOG_W("VR: OpenXR nicht verfuegbar (%s) -> %s", xr_fail,
               vr_mode == 1 ? "Desktop-Vorschau" : "VR aus, Spiel normal");
        if (SUCCEEDED(co)) { CoUninitialize(); }
        return 0;
    }
    vr_mode = 2;
    sb_ctx_enter(); /* W183: Quad-Shader (Leinwand/Spiegel) vor dem ersten VR-Bild anlegen */
    vq_init();
    sb_ctx_leave();
    while (!xr_quit) {
        xr_events();
        if (!xr_running) { Sleep(10); continue; }
        xr_frame();
    }
    HLOG_I("VR: OpenXR-Schleife beendet (Zustand %d)", (int)xr_state);
    vr_mode = 0;
    if (SUCCEEDED(co)) { CoUninitialize(); }
    return 0;
}

unsigned long sb_vr_thread_tid(void)
{
    return (unsigned long)xr_tid;
}

/* W145: vom Watchdog (alle 5 s): haengt der VR-Thread > 3 s in einem
 * OpenXR-Aufruf, eine Zeile (je Haenger). Das Spiel ist davon nicht
 * betroffen (keine Sperre ueber OpenXR-Aufrufe). vr_enable=0: sofort zurueck. */
void sb_vr_watch(void)
{
    const char *c = xr_call;
    LONGLONG q = xr_call_q;
    double s;
    if (!xr_tid || !vr_qpf.QuadPart) { return; }
    if (xr_loop_q && !c && xr_running && !xr_quit) { /* W183: Thread steht ausserhalb eines OpenXR-Aufrufs */
        double ls = (double)(vr_now() - xr_loop_q) / (double)vr_qpf.QuadPart;
        if (ls > 3.0 && InterlockedCompareExchange(&xr_stage_logged, 1, 0) == 0) {
            const char *st = xr_stage;
            extern void sb_wd_thread_stack(unsigned long tid, const char *label);
            HLOG_W("VR: VR-Thread steht seit %.1f s bei '%s' (kein OpenXR-Aufruf)", ls, st ? st : "?");
            HLOG_W("VR: Bilduebergabe blockiert. Bleibt die Brille dunkel, Client neu starten. Kein Session-Neubau parallel zum blockierten Treiberaufruf.");
            sb_wd_thread_stack(xr_tid, "VR-Thread"); /* DIAG(W183): wo steht er? */
            if (g_cfg.vr_diag >= 3) {
                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                THREADENTRY32 te;
                te.dwSize = sizeof te;
                if (snap != INVALID_HANDLE_VALUE) {
                    if (Thread32First(snap, &te)) {
                        do {
                            if (te.th32OwnerProcessID == GetCurrentProcessId() &&
                                te.th32ThreadID != GetCurrentThreadId() && te.th32ThreadID != xr_tid) {
                                sb_wd_thread_stack(te.th32ThreadID, "VR-Peer");
                            }
                        } while (Thread32Next(snap, &te));
                    }
                    CloseHandle(snap);
                }
            }
        }
    }
    if (!c) { return; }
    s = (double)(vr_now() - q) / (double)vr_qpf.QuadPart;
    if (s > 3.0 && InterlockedCompareExchange(&xr_hang_logged, 1, 0) == 0) {
        HLOG_W("VR: Runtime antwortet nicht: %s seit %.1f s (VR-Thread T%04x haelt keine Spiel-Sperre, Spiel laeuft auf dem Monitor weiter)",
               c, s, (unsigned)(xr_tid & 0xFFFFu));
    }
}

/* ---------- Start ---------- */

void sb_vr_init(void)
{
    static int done;
    const char *pv = g_cfg.vr_preview;
    if (!g_cfg.vr_enable || done) { return; }
    done = 1;
    QueryPerformanceFrequency(&vr_qpf);
    vr_qt0 = vr_now();
    vr_prev_kind = (_stricmp(pv, "left") == 0) ? 1 : (_stricmp(pv, "right") == 0) ? 2 :
                   (_stricmp(pv, "off") == 0 || _stricmp(pv, "xr") == 0 || _stricmp(pv, "aus") == 0) ? 3 : 0;
    vr_sim_kind = (_stricmp(g_cfg.vr_sim, "script") == 0) ? 1 : (_stricmp(g_cfg.vr_sim, "still") == 0) ? 2 : 0;
    if (g_cfg.interp_fps <= 0) {
        HLOG_W("VR: nicht verfuegbar (braucht interp_fps>0 fuer die Aufzeichnung) -> VR aus, Spiel normal");
        return;
    }
    HLOG_I("VR: Start (vorschau=%s sim=%s sitzhoehe=%dmm massstab=%d%% ipd=%.1fmm fov=%d D=%dmm taste=%s)",
           pv, g_cfg.vr_sim, g_cfg.vr_seat_height, g_cfg.vr_world_scale, (double)g_cfg.vr_ipd / 10.0, g_cfg.vr_fov,
           g_cfg.vr_monitor_depth, g_cfg.vr_recenter_key);
    /* W183 VR-FIX3: Einstellungen (eine Zeile) */
    HLOG_I("VR: Einstellungen preset=%d aufloesung=%d%% bildrate=%d spiegel=%d (%s, %d%%) aussenwelt=%s shading=%d sitz vor=%dmm hoch=%dmm "
           "zentrieren-beim-start=%d ohne-fokus-aus=%d monitor-rt=%d",
           g_cfg.vr_preset, g_cfg.vr_render_scale, g_cfg.vr_refresh_hz, g_cfg.vr_mirror, g_cfg.vr_mirror_fit ? "balken" : "zuschnitt",
           g_cfg.vr_mirror_res, g_cfg.vr_world_tick ? "spieltakt" : "je-vr-bild", g_cfg.vr_shading, g_cfg.vr_seat_forward,
           g_cfg.vr_seat_height, g_cfg.vr_recenter_start, g_cfg.vr_idle_unfocused, g_cfg.vr_rt_scale);
    if (g_cfg.vr_rt_scale > 0 && g_cfg.vr_rt_scale <= 4 && g_cfg.vr_rt_scale != g_cfg.rt_scale) {
        /* W183: Aufloesung der Aussenwelt-/Monitor-RTs im VR-Betrieb getrennt (vor dem Anlegen der RTs) */
        HLOG_I("VR: Welt-/Monitor-RTs mit Faktor %d (vr_rt_scale) statt rt_scale=%d%s", g_cfg.vr_rt_scale, g_cfg.rt_scale,
               g_cfg.rt_scale_dyn ? ", rt_scale_dyn aus" : "");
        g_cfg.rt_scale = g_cfg.vr_rt_scale;
        g_cfg.rt_scale_dyn = 0;
    }
    if (_stricmp(g_cfg.vr_runtime_json, "off") == 0) {
        vr_mode = (vr_prev_kind <= 2) ? 1 : 0;
        HLOG_I("VR: OpenXR aus (vr_runtime_json=off) -> %s", vr_mode == 1 ? "Desktop-Vorschau" : "VR aus");
        return;
    }
    xr_thread = CreateThread(NULL, 0, xr_thread_main, NULL, 0, NULL);
    if (!xr_thread) {
        vr_mode = (vr_prev_kind <= 2) ? 1 : 0;
        HLOG_W("VR: OpenXR nicht verfuegbar (Thread) -> %s", vr_mode == 1 ? "Desktop-Vorschau" : "VR aus, Spiel normal");
    }
}
