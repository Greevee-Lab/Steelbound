/* vexp_enc.c - W255 VIDEO-EXPORT (experimentell): MP4-Encoder und Zeitachse.
 * Beschreibung s. vexp_enc.h. Nur CPU-Pixel, keine D3D-/Spielzugriffe.
 * Media Foundation wird erst im Encoder-Thread aus System32 geladen (wie W210
 * render_clip.c): keine statische MF-Abhaengigkeit des Clients. */
#ifndef COBJMACROS
#define COBJMACROS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include "vexp_enc.h"
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

/* seltene GUIDs lokal (Werte aus dem Windows-SDK), damit keine Zusatz-Lib noetig ist */
static const GUID vx_HW_MFT = { 0xa634a91c, 0x822b, 0x41b9, { 0xa4, 0x94, 0x4d, 0xe4, 0x64, 0x36, 0x12, 0xb0 } };
static const GUID vx_CONTAINER = { 0x150ff23f, 0x4abc, 0x478b, { 0xac, 0x4f, 0xe1, 0x91, 0x6f, 0xba, 0x1c, 0xca } };
static const GUID vx_MP4 = { 0xdc6cd05d, 0xb9d0, 0x40ef, { 0xbd, 0x35, 0xfa, 0x62, 0x2c, 0x1a, 0xb2, 0x8a } };
static const GUID vx_IID_SWEX = { 0x588d72ab, 0x5bc1, 0x496a, { 0x87, 0x14, 0xb7, 0x06, 0x17, 0x14, 0x1b, 0x25 } };
static const GUID vx_CAT_VENC = { 0xf79eac7d, 0xe545, 0x4387, { 0xbd, 0xee, 0xd6, 0x47, 0xd7, 0xbd, 0xe4, 0x2a } };
static const GUID vx_HW_URL = { 0x2fb866ac, 0xb078, 0x4942, { 0xab, 0x6c, 0x00, 0x3d, 0x05, 0xcd, 0xa6, 0x74 } };

static HRESULT g_vx_hr_last; /* letztes HRESULT (bleibt nach dem Schliessen) */
#define NQ 3
#define ACHUNK 1024u                 /* Ton je Sample (AAC-Rahmen) */
#define RCAP (VXE_RATE * 10u)        /* Tonpuffer 10 s */

static struct {
    volatile LONG fail, stop, active;
    HANDLE thr, ready, sem_free, sem_full;
    unsigned w, h, fps, kbps;
    int audio, hw, open_ok, hw_used;
    unsigned char *q[NQ];
    unsigned rep[NQ];
    unsigned qp, qc;                 /* Erzeuger-/Verbraucher-Index */
    CRITICAL_SECTION acs;
    int acs_init;
    short *ring;
    unsigned long long a_in, a_rd, a_out, a_drop; /* Frames: rein, gelesen, geschrieben, verworfen */
    volatile LONG skip;
    unsigned vframes;
    double conv_ms;
    LONGLONG qpf;
    wchar_t path[700];
    char info[200];
    HRESULT hr;
    IMFSinkWriter *wr;
    DWORD vs, as;
} e;

static HRESULT (WINAPI *mf_start)(ULONG, DWORD);
static HRESULT (WINAPI *mf_stop)(void);
static HRESULT (WINAPI *mf_type)(IMFMediaType **);
static HRESULT (WINAPI *mf_sample)(IMFSample **);
static HRESULT (WINAPI *mf_buffer)(DWORD, IMFMediaBuffer **);
static HRESULT (WINAPI *mf_attr)(IMFAttributes **, UINT32);
static HRESULT (WINAPI *mf_writer)(LPCWSTR, IMFByteStream *, IMFAttributes *, IMFSinkWriter **);

/* ------------------------------------------------------------------ */
/* rein: Zeitachse, Farbwandlung, Bitrate                              */
/* ------------------------------------------------------------------ */
void vx_tl_init(vx_tl_t *t, double step)
{
    memset(t, 0, sizeof *t);
    t->step = step > 0.0 ? step : 1.0;
}

unsigned vx_tl_feed(vx_tl_t *t, double T)
{
    unsigned c = 0;
    if (!t->started) {
        t->started = 1;
        t->t0 = T;
        t->n = 0;
        return 0;
    }
    while (t->t0 + (double)t->n * t->step < T - 1e-6) {
        c++;
        t->n++;
    }
    return c;
}

unsigned vx_tl_end(vx_tl_t *t, double T_end)
{
    return t->started ? vx_tl_feed(t, T_end) : 0u;
}

double vx_batch_time(double c0, double c1, unsigned k, unsigned nimg)
{
    if (!nimg || k >= nimg) { return c1; }
    return c0 + (c1 - c0) * (double)k / (double)nimg;
}

static unsigned char cl8(int v) { return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); }

void vx_rgba_to_nv12(const unsigned char *rgba, unsigned w, unsigned h, unsigned char *nv12)
{
    unsigned x, y;
    unsigned char *uv0 = nv12 + (size_t)w * h;
    for (y = 0; y + 1 < h; y += 2) {
        const unsigned char *r0 = rgba + (size_t)y * w * 4, *r1 = r0 + (size_t)w * 4;
        unsigned char *y0 = nv12 + (size_t)y * w, *y1 = y0 + w, *uv = uv0 + (size_t)(y / 2) * w;
        for (x = 0; x + 1 < w; x += 2) {
            const unsigned char *a = r0 + x * 4, *b = a + 4, *c = r1 + x * 4, *d = c + 4;
            int R = a[0] + b[0] + c[0] + d[0], G = a[1] + b[1] + c[1] + d[1], B = a[2] + b[2] + c[2] + d[2];
            /* BT.709, 16..235 (Koeffizienten x256, Chroma-Summen 0 -> Grau bleibt 128) */
            y0[x] = (unsigned char)(((47 * a[0] + 157 * a[1] + 16 * a[2] + 128) >> 8) + 16);
            y0[x + 1] = (unsigned char)(((47 * b[0] + 157 * b[1] + 16 * b[2] + 128) >> 8) + 16);
            y1[x] = (unsigned char)(((47 * c[0] + 157 * c[1] + 16 * c[2] + 128) >> 8) + 16);
            y1[x + 1] = (unsigned char)(((47 * d[0] + 157 * d[1] + 16 * d[2] + 128) >> 8) + 16);
            uv[x] = cl8(((-26 * R - 87 * G + 113 * B + 512) >> 10) + 128);
            uv[x + 1] = cl8(((112 * R - 102 * G - 10 * B + 512) >> 10) + 128);
        }
    }
}

unsigned vx_auto_kbps(unsigned w, unsigned h, unsigned fps)
{
    double k = (double)w * (double)h * (double)fps * 0.15 / 1000.0;
    if (k < 2000.0) { k = 2000.0; }
    if (k > 100000.0) { k = 100000.0; }
    return (unsigned)k;
}

/* ------------------------------------------------------------------ */
/* Media Foundation                                                    */
/* ------------------------------------------------------------------ */
#define TRY(call) if (SUCCEEDED(hr)) { hr = (call); }

static HRESULT vx_color(IMFMediaType *t)
{
    HRESULT hr = S_OK;
    TRY(IMFMediaType_SetUINT32(t, &MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
    TRY(IMFMediaType_SetUINT32(t, &MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));
    TRY(IMFMediaType_SetUINT32(t, &MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
    TRY(IMFMediaType_SetUINT32(t, &MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
    return hr;
}

static HRESULT vx_vtype(IMFMediaType **t, int out, int profile)
{
    HRESULT hr = mf_type(t);
    TRY(IMFMediaType_SetGUID(*t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video));
    TRY(IMFMediaType_SetGUID(*t, &MF_MT_SUBTYPE, out ? &MFVideoFormat_H264 : &MFVideoFormat_NV12));
    TRY(IMFMediaType_SetUINT32(*t, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    TRY(IMFMediaType_SetUINT64(*t, &MF_MT_FRAME_SIZE, ((UINT64)e.w << 32) | e.h));
    TRY(IMFMediaType_SetUINT64(*t, &MF_MT_FRAME_RATE, ((UINT64)e.fps << 32) | 1u));
    TRY(IMFMediaType_SetUINT64(*t, &MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1u));
    TRY(vx_color(*t));
    if (out) {
        TRY(IMFMediaType_SetUINT32(*t, &MF_MT_AVG_BITRATE, e.kbps * 1000u));
        if (profile) { TRY(IMFMediaType_SetUINT32(*t, &MF_MT_MPEG2_PROFILE, (UINT32)profile)); }
    } else {
        TRY(IMFMediaType_SetUINT32(*t, &MF_MT_DEFAULT_STRIDE, e.w));
    }
    return hr;
}

static HRESULT vx_atype(IMFMediaType **t, int out)
{
    HRESULT hr = mf_type(t);
    TRY(IMFMediaType_SetGUID(*t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio));
    TRY(IMFMediaType_SetGUID(*t, &MF_MT_SUBTYPE, out ? &MFAudioFormat_AAC : &MFAudioFormat_PCM));
    TRY(IMFMediaType_SetUINT32(*t, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
    TRY(IMFMediaType_SetUINT32(*t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, VXE_RATE));
    TRY(IMFMediaType_SetUINT32(*t, &MF_MT_AUDIO_NUM_CHANNELS, 2));
    if (out) {
        TRY(IMFMediaType_SetUINT32(*t, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000)); /* 192 kbit/s */
        TRY(IMFMediaType_SetUINT32(*t, &MF_MT_AAC_PAYLOAD_TYPE, 0));
    } else {
        TRY(IMFMediaType_SetUINT32(*t, &MF_MT_AUDIO_BLOCK_ALIGNMENT, 4));
        TRY(IMFMediaType_SetUINT32(*t, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, VXE_RATE * 4u));
    }
    return hr;
}

/* Wurde ein Hardware-Encoder eingesetzt? (IMFSinkWriterEx, ab Windows 8) */
static int vx_probe_hw(void)
{
    IMFSinkWriterEx *ex = NULL;
    DWORD i;
    int hw = 0;
    if (FAILED(IMFSinkWriter_QueryInterface(e.wr, &vx_IID_SWEX, (void **)&ex)) || !ex) { return -1; }
    for (i = 0; i < 8; i++) {
        GUID cat;
        IMFTransform *mft = NULL;
        IMFAttributes *at = NULL;
        if (FAILED(IMFSinkWriterEx_GetTransformForStream(ex, e.vs, i, &cat, &mft)) || !mft) { break; }
        if (IsEqualGUID(&cat, &vx_CAT_VENC) && SUCCEEDED(IMFTransform_GetAttributes(mft, &at)) && at) {
            UINT32 len = 0;
            if (SUCCEEDED(IMFAttributes_GetStringLength(at, &vx_HW_URL, &len)) && len) { hw = 1; }
            IMFAttributes_Release(at);
        }
        IMFTransform_Release(mft);
    }
    IMFSinkWriterEx_Release(ex);
    return hw;
}

static void vx_release_writer(void)
{
    if (e.wr) { IMFSinkWriter_Release(e.wr); e.wr = NULL; }
}

static HRESULT vx_setup(int hw, int profile)
{
    IMFAttributes *at = NULL;
    IMFMediaType *vo = NULL, *vi = NULL, *ao = NULL, *ai = NULL;
    HRESULT hr = mf_attr(&at, 3);
    TRY(IMFAttributes_SetUINT32(at, &vx_HW_MFT, hw ? 1u : 0u));
    TRY(IMFAttributes_SetGUID(at, &vx_CONTAINER, &vx_MP4));
    TRY(mf_writer(e.path, NULL, at, &e.wr));
    TRY(vx_vtype(&vo, 1, profile));
    TRY(IMFSinkWriter_AddStream(e.wr, vo, &e.vs));
    TRY(vx_vtype(&vi, 0, 0));
    TRY(IMFSinkWriter_SetInputMediaType(e.wr, e.vs, vi, NULL));
    if (e.audio) {
        TRY(vx_atype(&ao, 1));
        TRY(IMFSinkWriter_AddStream(e.wr, ao, &e.as));
        TRY(vx_atype(&ai, 0));
        TRY(IMFSinkWriter_SetInputMediaType(e.wr, e.as, ai, NULL));
    }
    TRY(IMFSinkWriter_BeginWriting(e.wr));
    if (at) { IMFAttributes_Release(at); }
    if (vo) { IMFMediaType_Release(vo); }
    if (vi) { IMFMediaType_Release(vi); }
    if (ao) { IMFMediaType_Release(ao); }
    if (ai) { IMFMediaType_Release(ai); }
    if (FAILED(hr)) {
        vx_release_writer();
        DeleteFileW(e.path);
    }
    return hr;
}

static LONGLONG vx_t(unsigned long long n, unsigned rate) { return (LONGLONG)(n * 10000000ull / rate); }

static HRESULT vx_write(DWORD stream, IMFMediaBuffer *buf, LONGLONG t0, LONGLONG t1)
{
    IMFSample *s = NULL;
    HRESULT hr = mf_sample(&s);
    TRY(IMFSample_AddBuffer(s, buf));
    TRY(IMFSample_SetSampleTime(s, t0));
    TRY(IMFSample_SetSampleDuration(s, t1 - t0));
    TRY(IMFSinkWriter_WriteSample(e.wr, stream, s));
    if (s) { IMFSample_Release(s); }
    return hr;
}

static HRESULT vx_video(const unsigned char *rgba, unsigned rep)
{
    IMFMediaBuffer *buf = NULL;
    BYTE *dst = NULL;
    DWORD size = e.w * e.h * 3u / 2u;
    LARGE_INTEGER a, b;
    unsigned i;
    HRESULT hr = mf_buffer(size, &buf);
    QueryPerformanceCounter(&a);
    TRY(IMFMediaBuffer_Lock(buf, &dst, NULL, NULL));
    if (SUCCEEDED(hr)) {
        vx_rgba_to_nv12(rgba, e.w, e.h, dst);
        IMFMediaBuffer_Unlock(buf);
        hr = IMFMediaBuffer_SetCurrentLength(buf, size);
    }
    /* Dasselbe (unveraenderliche) Bild repeat-mal mit fester Bildrate */
    for (i = 0; i < rep && SUCCEEDED(hr); i++, e.vframes++) {
        hr = vx_write(e.vs, buf, vx_t(e.vframes, e.fps), vx_t(e.vframes + 1ull, e.fps));
    }
    QueryPerformanceCounter(&b);
    e.conv_ms += (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)e.qpf;
    if (buf) { IMFMediaBuffer_Release(buf); }
    return hr;
}

/* n Ton-Frames (aus dem Ring, NULL = Stille) schreiben */
static HRESULT vx_audio_chunk(unsigned n, int silence)
{
    IMFMediaBuffer *buf = NULL;
    BYTE *dst = NULL;
    HRESULT hr = mf_buffer(n * 4u, &buf);
    TRY(IMFMediaBuffer_Lock(buf, &dst, NULL, NULL));
    if (SUCCEEDED(hr)) {
        if (silence) {
            memset(dst, 0, n * 4u);
        } else {
            unsigned i;
            EnterCriticalSection(&e.acs);
            for (i = 0; i < n; i++) {
                const short *s = e.ring + (size_t)((e.a_rd + i) % RCAP) * 2u;
                memcpy(dst + i * 4u, s, 4);
            }
            e.a_rd += n;
            LeaveCriticalSection(&e.acs);
        }
        IMFMediaBuffer_Unlock(buf);
        hr = IMFMediaBuffer_SetCurrentLength(buf, n * 4u);
    }
    TRY(vx_write(e.as, buf, vx_t(e.a_out, VXE_RATE), vx_t(e.a_out + n, VXE_RATE)));
    if (SUCCEEDED(hr)) { e.a_out += n; }
    if (buf) { IMFMediaBuffer_Release(buf); }
    return hr;
}

static unsigned long long vx_avail(void)
{
    unsigned long long n;
    EnterCriticalSection(&e.acs);
    /* fuehrende Frames verwerfen (Versatz Tonstart -> erstes Bild) */
    while (e.skip > 0 && e.a_in > e.a_rd) {
        unsigned long long k = e.a_in - e.a_rd;
        if (k > (unsigned long long)e.skip) { k = (unsigned long long)e.skip; }
        e.a_rd += k;
        InterlockedExchangeAdd(&e.skip, -(LONG)k);
    }
    n = e.skip > 0 ? 0 : e.a_in - e.a_rd;
    LeaveCriticalSection(&e.acs);
    return n;
}

/* Ton bis zur Videozeit (final: genau bis dahin, Rest Stille) */
static HRESULT vx_audio_until(int final)
{
    unsigned long long target = (unsigned long long)e.vframes * VXE_RATE / e.fps;
    HRESULT hr = S_OK;
    if (!e.audio) { return S_OK; }
    while (SUCCEEDED(hr) && e.a_out < target) {
        unsigned long long want = target - e.a_out, have = vx_avail();
        unsigned n;
        if (!final && (want < ACHUNK || have < ACHUNK)) { break; }
        n = (unsigned)(want < ACHUNK ? want : ACHUNK);
        if (have >= n) { hr = vx_audio_chunk(n, 0); }
        else if (have) { hr = vx_audio_chunk((unsigned)have, 0); }
        else { hr = vx_audio_chunk(n, 1); }
    }
    return hr;
}

static unsigned __stdcall vx_thread(void *unused)
{
    HMODULE plat, rw;
    HRESULT hr = E_FAIL, co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int started = 0, profile = 100; /* eAVEncH264VProfile_High */
    (void)unused;
    plat = LoadLibraryExW(L"mfplat.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    rw = LoadLibraryExW(L"mfreadwrite.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (plat && rw) {
#define FN(dst, mod, name) *(FARPROC *)&dst = GetProcAddress(mod, name)
        FN(mf_start, plat, "MFStartup"); FN(mf_stop, plat, "MFShutdown"); FN(mf_type, plat, "MFCreateMediaType");
        FN(mf_sample, plat, "MFCreateSample"); FN(mf_buffer, plat, "MFCreateMemoryBuffer");
        FN(mf_attr, plat, "MFCreateAttributes"); FN(mf_writer, rw, "MFCreateSinkWriterFromURL");
#undef FN
        if (mf_start && mf_stop && mf_type && mf_sample && mf_buffer && mf_attr && mf_writer) {
            hr = mf_start(MF_VERSION, MFSTARTUP_FULL);
            started = SUCCEEDED(hr);
        }
    }
    if (started) {
        hr = vx_setup(e.hw, profile);
        if (FAILED(hr) && e.hw) { e.hw = 0; hr = vx_setup(0, profile); }
        if (FAILED(hr)) { profile = 0; hr = vx_setup(0, 0); } /* ohne Profilwunsch */
    }
    e.hr = hr;
    if (SUCCEEDED(hr)) {
        e.hw_used = vx_probe_hw();
        _snprintf_s(e.info, sizeof e.info, _TRUNCATE, "H264 %s %ux%u %ufps %ukbit/s %s%s",
                    e.hw_used > 0 ? "HW" : e.hw_used == 0 ? "SW" : "?", e.w, e.h, e.fps, e.kbps,
                    e.audio ? "AAC 44100/2 192kbit/s" : "ohne Ton", profile ? " High" : "");
        e.open_ok = 1;
    } else {
        _snprintf_s(e.info, sizeof e.info, _TRUNCATE, "Media Foundation/H.264 nicht verfuegbar (hr=0x%08lX)",
                    (unsigned long)hr);
        InterlockedExchange(&e.fail, 1);
    }
    SetEvent(e.ready);
    while (e.open_ok) {
        DWORD r = WaitForSingleObject(e.sem_full, 50);
        if (r == WAIT_TIMEOUT) {
            if (e.stop) { break; }
            continue;
        }
        if (r != WAIT_OBJECT_0) { break; }
        if (SUCCEEDED(hr)) { hr = vx_video(e.q[e.qc], e.rep[e.qc]); }
        if (SUCCEEDED(hr)) { hr = vx_audio_until(0); }
        e.qc = (e.qc + 1u) % NQ;
        ReleaseSemaphore(e.sem_free, 1, NULL);
        if (FAILED(hr) && !e.fail) {
            e.hr = hr;
            InterlockedExchange(&e.fail, 1);
        }
    }
    if (e.open_ok) {
        if (SUCCEEDED(hr) && e.stop != 2) { hr = vx_audio_until(1); }
        if (SUCCEEDED(hr) && e.stop != 2 && e.vframes) { hr = IMFSinkWriter_Finalize(e.wr); }
        if (FAILED(hr) && !e.fail) { e.hr = hr; InterlockedExchange(&e.fail, 1); }
        vx_release_writer();
    }
    if (started) { mf_stop(); }
    if (rw) { FreeLibrary(rw); }
    if (plat) { FreeLibrary(plat); }
    if (SUCCEEDED(co)) { CoUninitialize(); }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Schnittstelle                                                       */
/* ------------------------------------------------------------------ */
static void vx_free(void)
{
    unsigned i;
    for (i = 0; i < NQ; i++) { free(e.q[i]); e.q[i] = NULL; }
    free(e.ring); e.ring = NULL;
    if (e.thr) { CloseHandle(e.thr); e.thr = NULL; }
    if (e.ready) { CloseHandle(e.ready); e.ready = NULL; }
    if (e.sem_free) { CloseHandle(e.sem_free); e.sem_free = NULL; }
    if (e.sem_full) { CloseHandle(e.sem_full); e.sem_full = NULL; }
    if (e.acs_init) { DeleteCriticalSection(&e.acs); e.acs_init = 0; }
}

int vxe_open(const wchar_t *path, unsigned w, unsigned h, unsigned fps, unsigned kbps, int audio, int hw,
             char *info, size_t cap)
{
    unsigned i;
    LARGE_INTEGER f;
    if (e.active || !path || w < 2 || h < 2 || (w & 1u) || (h & 1u) || !fps) { return 0; }
    memset(&e, 0, sizeof e);
    wcsncpy_s(e.path, 700, path, _TRUNCATE);
    e.w = w; e.h = h; e.fps = fps; e.kbps = kbps ? kbps : vx_auto_kbps(w, h, fps);
    e.audio = audio ? 1 : 0; e.hw = hw ? 1 : 0;
    QueryPerformanceFrequency(&f);
    e.qpf = f.QuadPart ? f.QuadPart : 1;
    InitializeCriticalSection(&e.acs);
    e.acs_init = 1;
    for (i = 0; i < NQ; i++) {
        e.q[i] = (unsigned char *)malloc((size_t)w * h * 4u);
        if (!e.q[i]) { goto fail; }
    }
    if (e.audio && !(e.ring = (short *)malloc((size_t)RCAP * 4u))) { goto fail; }
    e.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    e.sem_free = CreateSemaphoreW(NULL, NQ, NQ, NULL);
    e.sem_full = CreateSemaphoreW(NULL, 0, NQ, NULL);
    if (!e.ready || !e.sem_free || !e.sem_full) { goto fail; }
    e.thr = (HANDLE)_beginthreadex(NULL, 0, vx_thread, NULL, 0, NULL);
    if (!e.thr) { goto fail; }
    WaitForSingleObject(e.ready, INFINITE);
    if (info && cap) { strcpy_s(info, cap, e.info); }
    if (!e.open_ok) {
        WaitForSingleObject(e.thr, INFINITE);
        vx_free();
        return 0;
    }
    InterlockedExchange(&e.active, 1);
    return 1;
fail:
    if (info && cap) { strcpy_s(info, cap, "kein Speicher fuer Bildpuffer"); }
    vx_free();
    return 0;
}

unsigned char *vxe_frame_begin(void)
{
    if (!e.active || e.fail) { return NULL; }
    for (;;) {
        DWORD r = WaitForSingleObject(e.sem_free, 1000);
        if (r == WAIT_OBJECT_0) { return e.q[e.qp]; }
        if (e.fail || WaitForSingleObject(e.thr, 0) == WAIT_OBJECT_0) { return NULL; }
    }
}

void vxe_frame_end(unsigned repeat)
{
    if (!e.active) { return; }
    e.rep[e.qp] = repeat ? repeat : 1u;
    e.qp = (e.qp + 1u) % NQ;
    ReleaseSemaphore(e.sem_full, 1, NULL);
}

void vxe_audio(const short *pcm, unsigned frames)
{
    unsigned waited = 0;
    if (!e.active || !e.audio || !pcm || !frames) { return; }
    for (;;) {
        unsigned long long used;
        EnterCriticalSection(&e.acs);
        used = e.a_in - e.a_rd;
        if (used + frames <= RCAP) {
            unsigned i;
            for (i = 0; i < frames; i++) {
                memcpy(e.ring + (size_t)((e.a_in + i) % RCAP) * 2u, pcm + i * 2u, 4);
            }
            e.a_in += frames;
            LeaveCriticalSection(&e.acs);
            return;
        }
        LeaveCriticalSection(&e.acs);
        if (waited >= 1000u || e.fail) {
            EnterCriticalSection(&e.acs);
            e.a_drop += frames;
            LeaveCriticalSection(&e.acs);
            return;
        }
        Sleep(1);
        waited++;
    }
}

void vxe_audio_skip(unsigned frames)
{
    InterlockedExchange(&e.skip, (LONG)frames);
}

int vxe_close(unsigned *frames, double *seconds, unsigned long long *afr, unsigned long long *adrop)
{
    int ok;
    if (!e.active) { return 0; }
    InterlockedExchange(&e.stop, 1);
    WaitForSingleObject(e.thr, INFINITE);
    ok = !e.fail && e.vframes > 0;
    g_vx_hr_last = e.hr;
    if (frames) { *frames = e.vframes; }
    if (seconds) { *seconds = (double)e.vframes / (double)e.fps; }
    if (afr) { *afr = e.a_out; }
    if (adrop) { *adrop = e.a_drop; }
    InterlockedExchange(&e.active, 0);
    vx_free();
    return ok;
}

void vxe_abort(void)
{
    if (!e.active) { return; }
    InterlockedExchange(&e.stop, 2);
    WaitForSingleObject(e.thr, INFINITE);
    g_vx_hr_last = e.hr;
    InterlockedExchange(&e.active, 0);
    vx_free();
}

int vxe_failed(void) { return e.fail != 0; }
unsigned long vxe_hr(void) { return (unsigned long)(e.active ? e.hr : g_vx_hr_last); }

double vxe_ms_per_frame(void)
{
    return e.vframes ? e.conv_ms / (double)e.vframes : 0.0;
}
