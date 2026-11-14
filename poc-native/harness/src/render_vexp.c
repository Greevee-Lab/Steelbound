/* render_vexp.c - W255 VIDEO-EXPORT (experimentell). Beschreibung s. render_vexp.h.
 * Quelltext ASCII. Nur Client-Bild (gezeigtes Spielbild vor ESC-Menue/F2, wie
 * W210-Clip), kein Desktop-/Fenster-Capture, kein Mikrofon/Loopback. */
#include "render_int.h"
#include "render_vexp.h"
#include "vexp_enc.h"
#include "sb_rauto.h"
#include "sb_photo.h"
#include "sb_pause.h" /* sb_timing_field */
#include <stdlib.h>

extern unsigned g_frames;
void emu_shutdown_for_exit(void);

#define VX_SLOTS 10

enum { VX_OFF = 0, VX_WAIT, VX_REC, VX_END };

static struct {
    int state;
    unsigned w, h, fps, R, kbps;
    wchar_t path[700], tmp[720];
    char name[200];                 /* nur Dateiname (Log) */
    /* virtueller Takt */
    volatile LONG clock, vc, polled;
    /* Zeitachse */
    LONG c_prev;
    vx_tl_t tl;
    int have_first;
    struct { ID3D11Texture2D *tex; } slot[VX_SLOTS];
    int nslots, batch[VX_SLOTS], nbatch, prev;
    unsigned drops, badsrc, images, swaps;
    unsigned hist[8];
    DXGI_FORMAT fmt;
    /* Ton */
    volatile LONG a_on;
    LONG a_vc0;
    volatile LONG a_deliv;          /* Ton-Frames seit Aufnahmestart (32 bit: atomar, 13 h) */
    volatile LONG a_pushed;
    unsigned a_timeouts;
    int a_dead;
    ULONGLONG wall0, a_wall0;
    double next_log;
    char info[200];
} vx;

/* ------------------------------------------------------------------ */
/* Ziel                                                                */
/* ------------------------------------------------------------------ */
static void vx_widen(const char *s, wchar_t *w, int cap)
{
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, cap) <= 0) {
        MultiByteToWideChar(CP_ACP, 0, s, -1, w, cap);
    }
}

static void vx_mkdirs(const wchar_t *dir)
{
    wchar_t t[700];
    wchar_t *s;
    wcsncpy_s(t, 700, dir, _TRUNCATE);
    for (s = t; *s; s++) {
        if ((*s == L'\\' || *s == L'/') && s > t && s[-1] != L':') {
            wchar_t c = *s;
            *s = 0;
            CreateDirectoryW(t, NULL);
            *s = c;
        }
    }
    CreateDirectoryW(t, NULL);
}

static void vx_resolve(void)
{
    const char *v = g_cfg.video_export;
    size_t n = strlen(v);
    char dir[600], nm[200];
    wchar_t wdir[700];
    SYSTEMTIME lt;
    DWORD a;
    int is_dir = 0;
    GetLocalTime(&lt);
    _snprintf_s(nm, sizeof nm, _TRUNCATE, "Steelbound_Replay_%s_%04u-%02u-%02u_%02u-%02u-%02u.mp4",
                g_cfg.replay_autoplay[0] ? g_cfg.replay_autoplay : "none", lt.wYear, lt.wMonth, lt.wDay, lt.wHour,
                lt.wMinute, lt.wSecond);
    if (_stricmp(v, "auto") == 0 || _stricmp(v, "1") == 0) {
        char ph[600];
        sb_photo_dir(ph, sizeof ph);
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\Videos", ph);
        is_dir = 1;
    } else {
        strcpy_s(dir, sizeof dir, v);
        if (n && (v[n - 1] == '\\' || v[n - 1] == '/')) {
            dir[n - 1] = 0;
            is_dir = 1;
        } else {
            vx_widen(v, wdir, 700);
            a = GetFileAttributesW(wdir);
            is_dir = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
        }
    }
    if (is_dir) {
        char p8[800];
        vx_widen(dir, wdir, 700);
        vx_mkdirs(wdir);
        _snprintf_s(p8, sizeof p8, _TRUNCATE, "%s\\%s", dir, nm);
        vx_widen(p8, vx.path, 700);
        strcpy_s(vx.name, sizeof vx.name, nm);
    } else {
        wchar_t *s, *last = NULL;
        vx_widen(v, vx.path, 700);
        wcsncpy_s(wdir, 700, vx.path, _TRUNCATE);
        for (s = wdir; *s; s++) { if (*s == L'\\' || *s == L'/') { last = s; } }
        if (last) { *last = 0; vx_mkdirs(wdir); }
        {
            const char *b = v, *p;
            for (p = v; *p; p++) { if (*p == '\\' || *p == '/') { b = p + 1; } }
            strcpy_s(vx.name, sizeof vx.name, b);
        }
    }
    _snwprintf_s(vx.tmp, 720, _TRUNCATE, L"%s.part", vx.path);
}

/* ------------------------------------------------------------------ */
/* Start / Ende                                                        */
/* ------------------------------------------------------------------ */
void sb_vexp_init(void)
{
    unsigned w = g_cfg.video_width, h = g_cfg.video_height;
    if (!g_cfg.video_export[0]) { return; }
    memset(&vx, 0, sizeof vx);
    vx.prev = -1;
    /* H.264-Grenzen (Software-Encoder: hoechstens 4096x2304), gerade Kanten */
    if (w < 320) { w = 320; }
    if (h < 240) { h = 240; }
    if (w > 4096) { w = 4096; }
    if (h > 2304) { h = 2304; }
    vx.w = w & ~1u;
    vx.h = h & ~1u;
    vx.fps = g_cfg.video_fps == 60 ? 60u : 30u;
    vx.R = g_cfg.game_tick == 25 ? 50u : 60u;
    vx.kbps = g_cfg.video_kbps > 0 ? (unsigned)g_cfg.video_kbps : vx_auto_kbps(vx.w, vx.h, vx.fps);
    vx_resolve();
    /* Export-Betrieb: unsichtbar, Zielgroesse, Testtakt, kein Live-Ton, nichts, das pausiert */
    g_cfg.width = vx.w;
    g_cfg.height = vx.h;
    g_cfg.window_hidden = 1;
    g_cfg.window_mode = 0;
    g_cfg.win_resize = 0;
    g_cfg.vblank_hz = 0;
    g_cfg.interp_fps = (vx.fps == 60 && g_cfg.video_interp) ? 60 : 0;
    g_cfg.interp_auto = 0;
    g_cfg.present_interval = 0;
    g_cfg.fps_limit = 0;
    g_cfg.rt_from_mission = 0;
    g_cfg.rt_from_film = 0;
    g_cfg.rt_scale_dyn = 0;
    g_cfg.vr_enable = 0;
    g_cfg.audio_live = 0;
    g_cfg.alpha_welcome = 0;
    vx.state = VX_WAIT;
    HLOG_I("VIDEO: Export aktiv (experimentell) Datei=%s %ux%u %u FPS %u kbit/s Ton=%d HW=%d interp_fps=%d Takt=%u Hz",
           vx.name, vx.w, vx.h, vx.fps, vx.kbps, g_cfg.video_audio ? 1 : 0, g_cfg.video_hw ? 1 : 0, g_cfg.interp_fps,
           vx.R);
    if (!g_cfg.replay_autoplay[0]) {
        HLOG_W("VIDEO: kein replay_autoplay gesetzt - Export bricht beim ersten Bild ab");
    }
}

int sb_vexp_on(void) { return vx.state != VX_OFF; }
int sb_vexp_rec(void) { return vx.state == VX_REC; }

static void vx_slots_free(void)
{
    int i;
    for (i = 0; i < vx.nslots; i++) {
        if (vx.slot[i].tex) { ID3D11Texture2D_Release(vx.slot[i].tex); vx.slot[i].tex = NULL; }
    }
    vx.nslots = 0;
}

static void vx_exit(int code)
{
    HLOG_I("VIDEO: Client beendet sich (Exit %d)", code);
    emu_shutdown_for_exit();
    ExitProcess((UINT)code);
}

static void vx_fail(int code, const char *why)
{
    InterlockedExchange(&vx.a_on, 0);
    InterlockedExchange(&vx.clock, 0);
    if (vx.state == VX_REC) { vxe_abort(); }
    vx.state = VX_END;
    sb_ctx_enter();
    vx_slots_free();
    sb_ctx_leave();
    DeleteFileW(vx.tmp);
    HLOG_E("VIDEO: FEHLER %s - keine Datei geschrieben (f=%u)", why, g_frames);
    vx_exit(code);
}

static void vx_start(void)
{
    char why[260];
    DeleteFileW(vx.tmp);
    if (!vxe_open(vx.tmp, vx.w, vx.h, vx.fps, vx.kbps, g_cfg.video_audio, g_cfg.video_hw, vx.info, sizeof vx.info)) {
        _snprintf_s(why, sizeof why, _TRUNCATE, "Encoder: %s", vx.info);
        vx.state = VX_END;
        DeleteFileW(vx.tmp);
        HLOG_E("VIDEO: FEHLER %s - keine Datei geschrieben (f=%u)", why, g_frames);
        vx_exit(3);
    }
    vx.vc = (LONG)sb_timing_field();
    vx.polled = 0;
    vx.c_prev = vx.vc;
    vx.a_vc0 = vx.vc;
    vx_tl_init(&vx.tl, (double)vx.R / (double)vx.fps);
    InterlockedExchange(&vx.clock, 1);
    vx.wall0 = GetTickCount64();
    vx.next_log = 10.0;
    if (g_cfg.video_audio) {
        vx.a_deliv = 0;
        vx.a_pushed = 0;
        vx.a_wall0 = vx.wall0;
        InterlockedExchange(&vx.a_on, 1);
    }
    vx.state = VX_REC;
    HLOG_I("VIDEO: Aufnahme startet (f=%u Feld=%ld) %s", g_frames, (long)vx.vc, vx.info);
}

/* Bild aus Slot s an den Encoder (rep Ausgabebilder). ctx_lock gehalten. */
static void vx_push(int s, unsigned rep)
{
    D3D11_MAPPED_SUBRESOURCE m;
    unsigned char *dst;
    unsigned y;
    int bgra = vx.fmt != DXGI_FORMAT_R8G8B8A8_UNORM;
    if (s < 0 || !rep || !vx.slot[s].tex) { return; }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)vx.slot[s].tex, 0, D3D11_MAP_READ, 0, &m))) {
        vx.drops += rep;
        return;
    }
    dst = vxe_frame_begin();
    if (dst) {
        for (y = 0; y < vx.h; y++) {
            const unsigned char *src = (const unsigned char *)m.pData + (size_t)y * m.RowPitch;
            unsigned char *d = dst + (size_t)y * vx.w * 4u;
            if (!bgra) {
                memcpy(d, src, (size_t)vx.w * 4u);
            } else {
                unsigned x;
                for (x = 0; x < vx.w; x++, d += 4, src += 4) { d[0] = src[2]; d[1] = src[1]; d[2] = src[0]; d[3] = 255; }
            }
        }
        vxe_frame_end(rep);
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)vx.slot[s].tex, 0);
}

static void vx_feed(double T, int s)
{
    unsigned c = vx_tl_feed(&vx.tl, T);
    if (!vx.have_first) {
        vx.have_first = 1;
        if (vx.a_on) { /* Ton beginnt am Aufnahmestart, Bild 0 bei T: Versatz verwerfen */
            double d = (T - (double)vx.a_vc0) * (double)VXE_RATE / (double)vx.R;
            vxe_audio_skip(d > 0.0 ? (unsigned)(d + 0.5) : 0u);
        }
    }
    if (c && vx.prev >= 0) { vx_push(vx.prev, c); }
    vx.prev = s;
}

static void vx_finish(double t_end)
{
    unsigned frames = 0, size = 0;
    double secs = 0.0, wall;
    unsigned long long afr = 0, adrop = 0;
    int ok;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    sb_ctx_enter();
    vx_push(vx.prev, vx_tl_end(&vx.tl, t_end)); /* letztes Bild bis zum Ende der Wiedergabe */
    vx_slots_free();
    sb_ctx_leave();
    InterlockedExchange(&vx.a_on, 0);
    InterlockedExchange(&vx.clock, 0);
    ok = vxe_close(&frames, &secs, &afr, &adrop);
    vx.state = VX_END;
    wall = (double)(GetTickCount64() - vx.wall0) / 1000.0;
    if (ok && !MoveFileExW(vx.tmp, vx.path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        HLOG_E("VIDEO: Umbenennen .part -> .mp4 fehlgeschlagen (GLE=%lu)", GetLastError());
        ok = 0;
    }
    if (ok && GetFileAttributesExW(vx.path, GetFileExInfoStandard, &fa)) { size = fa.nFileSizeLow; }
    HLOG_I("VIDEO: VBlanks je Swap 0:%u 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u >6:%u (Swaps=%u Bilder=%u verworfen=%u falsch=%u Ton-Wartezeit-Ueberlauf=%u)",
           vx.hist[0], vx.hist[1], vx.hist[2], vx.hist[3], vx.hist[4], vx.hist[5], vx.hist[6], vx.hist[7], vx.swaps,
           vx.images, vx.drops, vx.badsrc, vx.a_timeouts);
    if (!ok) {
        DeleteFileW(vx.tmp);
        HLOG_E("VIDEO: FEHLER Encoder/Abschluss - keine Datei (Bilder=%u)", frames);
        vx_exit(3);
    }
    HLOG_I("VIDEO: fertig Datei=%s %ux%u %u FPS Bilder=%u Dauer=%.3f s Ton=%s%llu Frames (verworfen %llu) Groesse=%u Bytes "
           "Laufzeit=%.1f s (%.2fx Echtzeit, Encoder %.1f ms/Bild) %s",
           vx.name, vx.w, vx.h, vx.fps, frames, secs, g_cfg.video_audio ? (vx.a_dead ? "stumm(Mischer fehlt) " : "AAC ") : "aus ",
           afr, adrop, size, wall, wall > 0.0 ? secs / wall : 0.0, vxe_ms_per_frame(), vx.info);
    vx_exit(0);
}

/* ------------------------------------------------------------------ */
/* Bilder                                                              */
/* ------------------------------------------------------------------ */
void sb_vexp_frame(ID3D11RenderTargetView *rtv)
{
    ID3D11Resource *src = NULL;
    D3D11_TEXTURE2D_DESC td;
    int s = -1, i, k;
    if (vx.state != VX_REC || !rtv) { return; }
    ID3D11RenderTargetView_GetResource(rtv, &src);
    if (!src) { return; }
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)src, &td);
    if (td.Width != vx.w || td.Height != vx.h || td.SampleDesc.Count != 1 ||
        (td.Format != DXGI_FORMAT_R8G8B8A8_UNORM && td.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
         td.Format != DXGI_FORMAT_B8G8R8X8_UNORM)) {
        if (!vx.badsrc++) {
            HLOG_W("VIDEO: Bildquelle %ux%u Format %d passt nicht zu %ux%u - Bild ausgelassen (nie gestreckt)",
                   td.Width, td.Height, (int)td.Format, vx.w, vx.h);
        }
        ID3D11Resource_Release(src);
        return;
    }
    if (!vx.nslots) {
        int K = g_cfg.interp_fps > 0 ? g_cfg.interp_fps / 30 : 1;
        if (K < 1) { K = 1; }
        vx.nslots = K + 2 > VX_SLOTS ? VX_SLOTS : K + 2;
        vx.fmt = td.Format;
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        for (i = 0; i < vx.nslots; i++) {
            if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &vx.slot[i].tex))) {
                vx.nslots = i;
                break;
            }
        }
        HLOG_I("VIDEO: %d Zwischenpuffer %ux%u (Format %d)", vx.nslots, vx.w, vx.h, (int)vx.fmt);
    }
    if (td.Format != vx.fmt) {
        vx.badsrc++;
        ID3D11Resource_Release(src);
        return;
    }
    for (i = 0; i < vx.nslots && s < 0; i++) {
        int used = (i == vx.prev);
        for (k = 0; k < vx.nbatch; k++) { if (vx.batch[k] == i) { used = 1; } }
        if (!used) { s = i; }
    }
    if (s < 0) {
        if (!vx.nbatch) { ID3D11Resource_Release(src); vx.drops++; return; }
        s = vx.batch[--vx.nbatch]; /* mehr Bilder als Puffer: letztes Zwischenbild ersetzen */
        vx.drops++;
    }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)vx.slot[s].tex, src);
    vx.batch[vx.nbatch++] = s;
    vx.images++;
    ID3D11Resource_Release(src);
}

/* ------------------------------------------------------------------ */
/* Swap (Spiel-Thread)                                                 */
/* ------------------------------------------------------------------ */
static void vx_audio_sync(void)
{
    LONGLONG target;
    ULONGLONG t0;
    if (!vx.a_on || vx.a_dead) { return; }
    target = (LONGLONG)(vx.vc - vx.a_vc0) * (LONGLONG)VXE_RATE / (LONGLONG)vx.R;
    if (!vx.a_pushed) {
        if (GetTickCount64() - vx.a_wall0 > 5000u) {
            vx.a_dead = 1; /* kein Mischer-Thread (kein Ton im Spiel?): stumme Spur, nicht warten */
            HLOG_W("VIDEO: Mischer liefert keinen Ton - Tonspur bleibt stumm");
        }
        return;
    }
    t0 = GetTickCount64();
    while (vx.a_on && vx.a_deliv <= target) {
        if (GetTickCount64() - t0 > 2000u) {
            if (!vx.a_timeouts++) { HLOG_W("VIDEO: Mischer holt nicht auf (Ton %lld/%lld Frames)", (long long)vx.a_deliv, (long long)target); }
            break;
        }
        Sleep(0);
        if (GetTickCount64() - t0 > 2u) { Sleep(1); }
    }
}

void sb_vexp_swap(void)
{
    int err = 0, phase, k;
    LONG C;
    double secs;
    if (vx.state != VX_WAIT && vx.state != VX_REC) { return; }
    phase = sb_rauto_phase(&err);
    if (vx.state == VX_WAIT) {
        if (phase == RA_PLAY) {
            vx_start();
        } else if (phase == RA_OFF || phase == RA_FAIL || phase == RA_DONE) {
            char why[96];
            _snprintf_s(why, sizeof why, _TRUNCATE, "Replay-Start (Phase %s, Fehler %d)", ra_phase_name(phase), err);
            vx_fail(2, why);
        }
        return;
    }
    C = vx.vc;
    vx.polled = 0; /* neuer Swap: erster Feld-Poll danach zaehlt keinen VBlank */
    vx.swaps++;
    k = (int)(C - vx.c_prev);
    vx.hist[k < 0 ? 7 : k > 6 ? 7 : k]++;
    if (phase != RA_PLAY) {
        /* Wiedergabe zu Ende: Bilder dieses Swaps gehoeren schon zum Menue */
        vx.nbatch = 0;
        HLOG_I("VIDEO: Wiedergabe beendet (Phase %s, f=%u) - Datei wird abgeschlossen", ra_phase_name(phase), g_frames);
        vx_finish((double)C);
        return;
    }
    if (vx.nbatch) {
        sb_ctx_enter();
        for (k = 0; k < vx.nbatch; k++) {
            vx_feed(vx_batch_time((double)vx.c_prev, (double)C, (unsigned)k + 1u, (unsigned)vx.nbatch), vx.batch[k]);
        }
        vx.nbatch = 0;
        sb_ctx_leave();
    }
    vx.c_prev = C;
    if (vxe_failed()) {
        vx_fail(3, "Encoder meldet Fehler");
        return;
    }
    vx_audio_sync();
    secs = (double)vx.tl.n / (double)vx.fps;
    if (secs >= vx.next_log) {
        double wall = (double)(GetTickCount64() - vx.wall0) / 1000.0;
        vx.next_log += 10.0;
        HLOG_I("VIDEO: %.0f s Video (%u Bilder) nach %.1f s (%.2fx Echtzeit, Ton %lld Frames)", secs, vx.tl.n, wall,
               wall > 0.0 ? secs / wall : 0.0, (long long)vx.a_deliv);
    }
}

/* ------------------------------------------------------------------ */
/* virtueller Takt / Ton                                               */
/* ------------------------------------------------------------------ */
int sb_vexp_clock(void) { return vx.clock != 0; }

unsigned sb_vexp_field_poll(void)
{
    if (vx.polled) { InterlockedIncrement(&vx.vc); }
    vx.polled = 1;
    return (unsigned)vx.vc;
}

void sb_vexp_vblank(void)
{
    InterlockedIncrement(&vx.vc);
    vx.polled = 0;
}

int sb_vexp_audio(void) { return vx.a_on != 0; }

void sb_vexp_audio_push(const short *pcm, unsigned frames)
{
    if (!vx.a_on) { return; }
    vxe_audio(pcm, frames);
    InterlockedExchangeAdd(&vx.a_deliv, (LONG)frames);
    InterlockedExchange(&vx.a_pushed, 1);
}

void sb_vexp_audio_pace(void)
{
    /* naechster Block erst, wenn die virtuelle Zeit den gelieferten Ton erreicht hat */
    while (vx.a_on) {
        LONGLONG target = (LONGLONG)(vx.vc - vx.a_vc0) * (LONGLONG)VXE_RATE / (LONGLONG)vx.R;
        if (vx.a_deliv <= target) { return; }
        Sleep(1);
    }
}
