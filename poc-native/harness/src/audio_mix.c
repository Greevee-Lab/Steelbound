/* audio_mix.c - DirectSound-HLE: WAV-Capture, WinMM-Ausgabe, Loader-Header-
 * Fenster (A7B), WASAPI-Live-Ausgabe (Ring + Thread) und Mixer-Thread.
 * W68 SPLIT: 1:1 aus audio_out.c verschoben. */
#include "audio_int.h"
#include "sb_pause.h" /* W188 GAME-MENU: g_sb_paused */
#include <share.h> /* _SH_DENYNO (W102) */

/* ---------- WAV-Writer (kill-sicher: Header pro Chunk neu) ---------- */
static void au_wav_write_header(void)
{
    uint8_t h[44];
    au32 datasz = g_wav_bytes;
    au32 riff = 36 + datasz;
    if (!g_wav)
        return;
    memcpy(h, "RIFF", 4);
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16;
    h[17] = h[18] = h[19] = 0;
    h[20] = 1;
    h[21] = 0; /* PCM */
    h[22] = 2;
    h[23] = 0; /* stereo */
    {
        au32 r = MIXRATE;
        au32 b = MIXRATE * 4;
        memcpy(h + 24, &r, 4);
        memcpy(h + 28, &b, 4);
    }
    h[32] = 4;
    h[33] = 0; /* blockalign */
    h[34] = 16;
    h[35] = 0; /* bits */
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &datasz, 4);
    fseek(g_wav, 0, SEEK_SET);
    fwrite(h, 1, 44, g_wav);
    fseek(g_wav, 0, SEEK_END);
    fflush(g_wav);
}

static void au_wav_open(void)
{
    if (g_wav || !g_cfg.audio_capture)
        return;
    _snprintf_s(g_wav_path, sizeof g_wav_path, _TRUNCATE, "%s/audio.wav",
                hcfg_out_dir());
    if (fopen_s(&g_wav, g_wav_path, "wb") == 0 && g_wav) {
        g_wav_bytes = 0;
        au_wav_write_header();
        HLOG_I("AUDIO capture: %s (44100/16/stereo)", g_wav_path);
    } else {
        g_wav = NULL;
        HLOG_W("AUDIO capture: %s nicht schreibbar", g_wav_path);
    }
}

/* ---------- WinMM-Ausgabe (best-effort) ---------- */
/* A7B: Header-Fenster (Definition vor WASAPI-Abschnitt, WinMM nutzt mit) */
static int au_hdr_to_loader(void);
static void au_hdr_to_game(int token);
#define WO_NBUF 6
static WAVEHDR g_wo_hdr[WO_NBUF];
static int16_t *g_wo_buf[WO_NBUF];
static int g_wo_next;

static void CALLBACK au_wo_cb(HWAVEOUT hwo, UINT msg, DWORD_PTR inst,
                                       DWORD_PTR p1, DWORD_PTR p2)
{
    (void)hwo;
    (void)inst;
    (void)p1;
    (void)p2;
    if (msg == WOM_DONE)
        InterlockedDecrement(&g_wo_queued);
}

static void au_wo_open(void)
{
    WAVEFORMATEX w;
    int i;
    if (g_wo_ok != 0)
        return;
    memset(&w, 0, sizeof w);
    w.wFormatTag = WAVE_FORMAT_PCM;
    w.nChannels = 2;
    w.nSamplesPerSec = MIXRATE;
    w.nAvgBytesPerSec = MIXRATE * 4;
    w.nBlockAlign = 4;
    w.wBitsPerSample = 16;
    /* Best-effort: waveOutOpen kann auf kopflosen/defekten Stacks
     * faulten (0xC000000D, W4_a5) -> SEH-geschuetzt, WAV weiter. */
    __try {
        if (waveOutOpen(&g_wo, WAVE_MAPPER, &w, (DWORD_PTR)au_wo_cb, 0,
                        CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
            g_wo_ok = -1;
            HLOG_W("AUDIO waveOutOpen: kein Geraet (WAV weiter)");
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_wo_ok = -1;
        HLOG_W("AUDIO waveOutOpen fault (Subsystem defekt, WAV weiter)");
        return;
    }
    for (i = 0; i < WO_NBUF; i++) {
        g_wo_buf[i] =
            (int16_t *)malloc(MIXCHUNK * 2 * sizeof(int16_t));
        memset(&g_wo_hdr[i], 0, sizeof g_wo_hdr[i]);
        g_wo_hdr[i].lpData = (LPSTR)g_wo_buf[i];
        g_wo_hdr[i].dwBufferLength = MIXCHUNK * 4;
        waveOutPrepareHeader(g_wo, &g_wo_hdr[i], sizeof(WAVEHDR));
    }
    g_wo_ok = 1;
    HLOG_I("AUDIO live: WinMM waveOut 44100/16/stereo");
}

static void au_wo_write(const int16_t *s)
{
    if (g_wo_ok == 0)
        { /* A7B: Header-Fenster (W4-Faultursache: WinMM validiert Header) */
            int htok = au_hdr_to_loader();
            __try {
                au_wo_open();
            } __finally {
                au_hdr_to_game(htok);
            }
        }
    if (g_wo_ok != 1)
        return;
    if (g_wo_queued >= WO_NBUF)
        return; /* Ueberlauf: Chunk verwerfen (Live bleibt synchron) */
    memcpy(g_wo_buf[g_wo_next], s, MIXCHUNK * 4);
    waveOutWrite(g_wo, &g_wo_hdr[g_wo_next], sizeof(WAVEHDR));
    g_wo_next = (g_wo_next + 1) % WO_NBUF;
    InterlockedIncrement(&g_wo_queued);
}

/* ---------- Loader-Header-Fenster (A7B) ----------
 * WURZELURSACHE WinMM/WASAPI-Fault (0xC000000D): Das XBE liegt ueber dem
 * Loader-Image (Basis 0x10000); seit dem Mappen stehen dort XBE-Bytes statt
 * PE-Headern. Windows-APIs, die die Haupt-Image-Header validieren
 * (CreateThread, COM-Aktivierung, waveOutOpen u. a.), faulten. kernel_hle.c
 * loest das fuer CreateThread per xbe_swap_headers (Kernel-Datei, fremde
 * Baustelle); hier die lokale Variante fuer Audio-COM/WinMM: Loader-Header
 * einmalig aus der Exe-Datei lesen, pro Aufruf einblenden -> Aufruf
 * (SEH-gesichert) -> zurueck. Rest-Risiko wie beim Kernel-Swap (partielles
 * Ineinandergreifen hinterlaesst Loader-Bytes, mikrosekunden-klein, Setup
 * laeuft 1x pro Lauf) -> W10 (Swap unter Sperre stellen). */
static uint8_t *au_hdr_loader, *au_hdr_shadow;
static au32 au_hdr_n;
static uintptr_t au_hdr_base;
static int au_hdr_ready; /* -1 defekt, 0 unversucht, 1 bereit */
static CRITICAL_SECTION au_hdr_cs;
static int au_hdr_cs_init;

static au32 au_rd32le(const uint8_t *p)
{
    return (au32)p[0] | ((au32)p[1] << 8) | ((au32)p[2] << 16) |
        ((au32)p[3] << 24);
}

static void au_hdr_init(void)
{
    char path[512];
    FILE *f = NULL;
    uint8_t *file = NULL;
    long sz = 0;
    au32 e_lfanew, sizeh;
    if (au_hdr_ready)
        return;
    au_hdr_ready = -1;
    if (!au_hdr_cs_init) {
        InitializeCriticalSection(&au_hdr_cs);
        au_hdr_cs_init = 1;
    }
    au_hdr_base = (uintptr_t)GetModuleHandle(NULL);
    if (!au_hdr_base)
        return;
    if (!GetModuleFileNameA(NULL, path, sizeof path))
        return;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0x400 || sz > 64 * 1024 * 1024) {
        fclose(f);
        return;
    }
    file = (uint8_t *)malloc((size_t)sz);
    if (!file || fread(file, 1, (size_t)sz, f) != (size_t)sz) {
        free(file);
        fclose(f);
        return;
    }
    fclose(f);
    if (file[0] != 'M' || file[1] != 'Z') {
        free(file);
        return;
    }
    e_lfanew = au_rd32le(file + 0x3c);
    if (e_lfanew > (au32)sz - 6 || memcmp(file + e_lfanew, "PE\0\0", 4) != 0) {
        free(file);
        return;
    }
    sizeh = au_rd32le(file + e_lfanew + 4 + 20 + 60); /* SizeOfHeaders */
    if (sizeh < 0x200 || sizeh > 0x10000 || sizeh > (au32)sz) {
        free(file);
        return;
    }
    au_hdr_loader = (uint8_t *)malloc(sizeh);
    au_hdr_shadow = (uint8_t *)malloc(sizeh);
    if (!au_hdr_loader || !au_hdr_shadow) {
        free(au_hdr_loader);
        free(au_hdr_shadow);
        au_hdr_loader = au_hdr_shadow = NULL;
        free(file);
        return;
    }
    memcpy(au_hdr_loader, file, sizeh);
    au_hdr_n = sizeh;
    free(file);
    au_hdr_ready = 1;
    HLOG_I("audio: Loader-Header gerettet (%u Bytes @0x%p)", sizeh,
           (void *)au_hdr_base);
}

/* 1 = umgeschaltet (zurueckschalten noetig), 0 = schon Loader/defekt. */
static int au_hdr_to_loader(void)
{
    au_hdr_init();
    if (au_hdr_ready != 1)
        return 0;
    EnterCriticalSection(&au_hdr_cs);
    if (memcmp((void *)au_hdr_base, au_hdr_loader, 64) == 0)
        return 0; /* fremder Swap aktiv (Kernel): mitbenutzen */
    memcpy(au_hdr_shadow, (void *)au_hdr_base, au_hdr_n);
    memcpy((void *)au_hdr_base, au_hdr_loader, au_hdr_n);
    return 1;
}

static void au_hdr_to_game(int token)
{
    if (au_hdr_ready != 1)
        return;
    if (token)
        memcpy((void *)au_hdr_base, au_hdr_shadow, au_hdr_n);
    LeaveCriticalSection(&au_hdr_cs);
}

/* ---------- WASAPI-Live-Ausgabe (A7B, audio_live=1) ----------
 * Shared-Mode-Renderer auf eigenem Thread: der Mixer schreibt 20-ms-Chunks
 * in einen Ringpuffer (eigene Sperre, nie blockierend, bei Ueberlauf wird
 * Aeltestes verworfen = live-synchron), der WASAPI-Thread resampelt
 * 44100 -> Geraete-Rate (linear) und fuettert den Shared-Puffer per Polling
 * (s16/float/s32, Mono..7.1: L/R + Stille). Ohne Geraet: still weiter
 * (WAV laeuft), Reconnect alle 2 s. Kein Fehler ist je fatal (nur Log). */
#define RING_SEC 4
#define RING_FRAMES (MIXRATE * RING_SEC)
static int16_t *g_ring;
static LONG g_ring_rd, g_ring_wr, g_ring_n;
static CRITICAL_SECTION g_ring_cs;
static int g_ring_init;
static HANDLE g_wa_thread;

static void ring_init(void)
{
    if (!g_ring_init) {
        InitializeCriticalSection(&g_ring_cs);
        g_ring = (int16_t *)malloc((size_t)RING_FRAMES * 2 * sizeof(int16_t));
        g_ring_init = 1;
    }
}

/* MIXCHUNK Frames anhaengen; bei Ueberlauf aelteste verwerfen. */
static void ring_write(const int16_t *s)
{
    LONG i;
    ring_init();
    if (!g_ring)
        return;
    EnterCriticalSection(&g_ring_cs);
    for (i = 0; i < MIXCHUNK; i++) {
        if (g_ring_n >= RING_FRAMES) {
            if (++g_ring_rd >= RING_FRAMES)
                g_ring_rd = 0;
            g_ring_n--;
        }
        g_ring[g_ring_wr * 2] = s[i * 2];
        g_ring[g_ring_wr * 2 + 1] = s[i * 2 + 1];
        if (++g_ring_wr >= RING_FRAMES)
            g_ring_wr = 0;
        g_ring_n++;
    }
    LeaveCriticalSection(&g_ring_cs);
}

/* Bis zu n Frames lesen, Rueckgabe Anzahl. */
static LONG ring_read(int16_t *dst, LONG n)
{
    LONG got = 0;
    if (!g_ring_init || !g_ring)
        return 0;
    EnterCriticalSection(&g_ring_cs);
    while (got < n && g_ring_n > 0) {
        dst[got * 2] = g_ring[g_ring_rd * 2];
        dst[got * 2 + 1] = g_ring[g_ring_rd * 2 + 1];
        if (++g_ring_rd >= RING_FRAMES)
            g_ring_rd = 0;
        g_ring_n--;
        got++;
    }
    LeaveCriticalSection(&g_ring_cs);
    return got;
}

/* PKEY_Device_FriendlyName = {A45C254E-DF1C-4EFD-8020-67D146A850E0}, PID 14
 * (manuell definiert: kein Link gegen propsys-GUIDs noetig). */
static const PROPERTYKEY kPKEY_DevName = { { 0xa45c254e, 0xdf1c, 0x4efd,
                                             { 0x80, 0x20, 0x67, 0xd1, 0x46,
                                               0xa8, 0x50, 0xe0 } },
                                           14 };

/* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = 00000003-0000-0010-8000-00AA00389B71 */
/* WASAPI-Klassen/Interfaces (Werte aus SDK mmdeviceapi.h/audioclient.h;
 * die IID_*-Symbole liegen nicht in uuid.lib, daher manuell). */
static const GUID kCLSID_MMDE = { 0xbcde0395, 0xe52f, 0x467c,
                                  { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69,
                                    0x2e } };
static const GUID kIID_MMDE = { 0xa95664d2, 0x9614, 0x4f35,
                                { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17,
                                  0xe6 } };
static const GUID kIID_AudioCli = { 0x1cb9ad4c, 0xdbfa, 0x4c32,
                                    { 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03,
                                      0xb2 } };
static const GUID kIID_RenderCli = { 0xf294acfc, 0x3146, 0x4483,
                                     { 0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2,
                                       0x60, 0xe2 } };
static const GUID kSubFloat = { 0x00000003, 0x0000, 0x0010,
                                { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b,
                                  0x71 } };

static void wa_device_name(IMMDevice *dev, char *out, size_t cap)
{
    IPropertyStore *ps = NULL;
    PROPVARIANT pv;
    strncpy_s(out, cap, "Standardgeraet", cap - 1);
    if (!dev)
        return;
    if (FAILED(dev->lpVtbl->OpenPropertyStore(dev, STGM_READ, &ps)) || !ps)
        return;
    memset(&pv, 0, sizeof pv);
    if (SUCCEEDED(ps->lpVtbl->GetValue(ps, &kPKEY_DevName, &pv))) {
        if (pv.vt == VT_LPWSTR && pv.pwszVal) {
            WideCharToMultiByte(CP_ACP, 0, pv.pwszVal, -1, out, (int)cap,
                                NULL, NULL);
            CoTaskMemFree(pv.pwszVal); /* statt PropVariantClear */
        }
    }
    ps->lpVtbl->Release(ps);
}

/* Ein Ziel-Frame (Stereo float) ins Geraetepuffer-Format schreiben. */
static void wa_put_frame(BYTE **pp, float l, float r, WORD ch, WORD bits,
                         int isfloat)
{
    BYTE *p = *pp;
    WORD c;
    if (l > 1.0f)
        l = 1.0f;
    if (l < -1.0f)
        l = -1.0f;
    if (r > 1.0f)
        r = 1.0f;
    if (r < -1.0f)
        r = -1.0f;
    if (ch == 1) { /* Mono: Mitte */
        float m = (l + r) * 0.5f;
        if (isfloat) {
            *(float *)p = m;
            p += 4;
        } else if (bits == 32) {
            *(int32_t *)p = (int32_t)(m * 2147483647.0f);
            p += 4;
        } else {
            *(int16_t *)p = (int16_t)(m * 32767.0f);
            p += 2;
        }
    } else {
        for (c = 0; c < ch; c++) {
            float s = c == 0 ? l : (c == 1 ? r : 0.0f);
            if (isfloat) {
                *(float *)p = s;
                p += 4;
            } else if (bits == 32) {
                *(int32_t *)p = (int32_t)(s * 2147483647.0f);
                p += 4;
            } else {
                *(int16_t *)p = (int16_t)(s * 32767.0f);
                p += 2;
            }
        }
    }
    *pp = p;
}

static DWORD WINAPI au_wa_thread(LPVOID p)
{
    HRESULT hr;
    int coinited = 0;
    static au32 wano; /* Drossel fuer "kein Geraet" */
    (void)p;
    __try {
        { /* A7B: Header-Fenster (COM validiert Image-Header) */
            int htok0 = au_hdr_to_loader();
            __try {
                hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
            } __finally {
                au_hdr_to_game(htok0);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        HLOG_W("audio: WASAPI CoInitialize-Fault, still weiter");
        hr = E_FAIL;
    }
    coinited = SUCCEEDED(hr);
    for (;;) {
        IMMDeviceEnumerator *de = NULL;
        IMMDevice *dev = NULL;
        IAudioClient *cli = NULL;
        IAudioRenderClient *rc = NULL;
        WAVEFORMATEX *mix = NULL;
        UINT32 rate = 0, bufFrames = 0;
        WORD ch = 0, bits = 0;
        int isfloat = 0;
        char name[128];
        double step = 1.0, frac = 0.0;
        int16_t cur[2] = { 0, 0 }, nxt[2] = { 0, 0 };
        int primed = 0;

        { /* A7B: Header-Fenster fuers Setup (COM validiert Image-Header) */
            int htok = au_hdr_to_loader();
            __try {
                __try {
        hr = CoCreateInstance(&kCLSID_MMDE, NULL, CLSCTX_ALL,
                              &kIID_MMDE, (void **)&de);
        if (FAILED(hr) || !de) {
            if (hlog_throttle(&wano))
                HLOG_W("audio: WASAPI ohne COM-Enumerator (hr=0x%lX), still weiter",
                       hr);
            Sleep(5000);
            continue;
        }
        hr = de->lpVtbl->GetDefaultAudioEndpoint(de, eRender, eConsole, &dev);
        de->lpVtbl->Release(de);
        if (FAILED(hr) || !dev) {
            if (hlog_throttle(&wano))
                HLOG_W("audio: WASAPI kein Ausgabegeraet (hr=0x%lX), still weiter",
                       hr);
            Sleep(2000);
            continue;
        }
        wa_device_name(dev, name, sizeof name);
        hr = dev->lpVtbl->Activate(dev, &kIID_AudioCli, CLSCTX_ALL, NULL,
                                   (void **)&cli);
        dev->lpVtbl->Release(dev);
        if (FAILED(hr) || !cli) {
            HLOG_W("audio: WASAPI Activate fehlgeschlagen (hr=0x%lX)", hr);
            Sleep(2000);
            continue;
        }
        hr = cli->lpVtbl->GetMixFormat(cli, &mix);
        if (FAILED(hr) || !mix) {
            HLOG_W("audio: WASAPI GetMixFormat fehlgeschlagen (hr=0x%lX)", hr);
            cli->lpVtbl->Release(cli);
            Sleep(2000);
            continue;
        }
        rate = mix->nSamplesPerSec;
        ch = mix->nChannels;
        bits = mix->wBitsPerSample;
        if (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
            isfloat = 1;
        else if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            WAVEFORMATEXTENSIBLE *x = (WAVEFORMATEXTENSIBLE *)mix;
            if (IsEqualGUID(&x->SubFormat, &kSubFloat))
                isfloat = 1;
            if (x->Samples.wValidBitsPerSample)
                bits = x->Samples.wValidBitsPerSample;
        }
        if (rate < 8000 || rate > 192000 || ch < 1 || ch > 8 ||
            !(bits == 16 || bits == 32) ||
            (mix->wFormatTag != WAVE_FORMAT_PCM &&
             mix->wFormatTag != WAVE_FORMAT_IEEE_FLOAT &&
             mix->wFormatTag != WAVE_FORMAT_EXTENSIBLE)) {
            HLOG_W("audio: WASAPI Mix-Format %u Hz/%u Kanaele/%u Bit (tag 0x%X) nicht unterstuetzt, still weiter",
                   rate, ch, bits, mix->wFormatTag);
            CoTaskMemFree(mix);
            cli->lpVtbl->Release(cli);
            Sleep(5000);
            continue;
        }
        hr = cli->lpVtbl->Initialize(cli, AUDCLNT_SHAREMODE_SHARED, 0,
                                     2000000, 0, mix, NULL);
        if (FAILED(hr)) {
            HLOG_W("audio: WASAPI Initialize fehlgeschlagen (hr=0x%lX)", hr);
            CoTaskMemFree(mix);
            cli->lpVtbl->Release(cli);
            Sleep(2000);
            continue;
        }
        CoTaskMemFree(mix);
        if (FAILED(cli->lpVtbl->GetBufferSize(cli, &bufFrames)) || !bufFrames) {
            HLOG_W("audio: WASAPI GetBufferSize fehlgeschlagen");
            cli->lpVtbl->Release(cli);
            Sleep(2000);
            continue;
        }
        hr = cli->lpVtbl->GetService(cli, &kIID_RenderCli,
                                     (void **)&rc);
        if (FAILED(hr) || !rc) {
            HLOG_W("audio: WASAPI RenderClient fehlgeschlagen (hr=0x%lX)", hr);
            cli->lpVtbl->Release(cli);
            Sleep(2000);
            continue;
        }
        Sleep(200); /* Ring vorgluehen, damit der Start nicht knackt */
        if (FAILED(cli->lpVtbl->Start(cli))) {
            HLOG_W("audio: WASAPI Start fehlgeschlagen");
            rc->lpVtbl->Release(rc);
            cli->lpVtbl->Release(cli);
            Sleep(2000);
            continue;
        }
        HLOG_I("audio: WASAPI läuft (Gerät %s, %u Hz/%u Kanäle/%u Bit%s)",
               name, rate, ch, bits, isfloat ? " float" : "");
                } __finally {
                    au_hdr_to_game(htok);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                HLOG_W("audio: WASAPI Setup-Fault, still weiter (Retry)");
                if (de)
                    de->lpVtbl->Release(de);
                if (dev)
                    dev->lpVtbl->Release(dev);
                if (cli)
                    cli->lpVtbl->Release(cli);
                if (rc)
                    rc->lpVtbl->Release(rc);
                if (mix)
                    CoTaskMemFree(mix);
                Sleep(2000);
                continue;
            }
        }
        step = (double)MIXRATE / (double)rate;
        /* Render-Schleife bis Geraet weg */
        for (;;) {
            UINT32 pad = 0, avail, f;
            BYTE *dst = NULL;
            hr = cli->lpVtbl->GetCurrentPadding(cli, &pad);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED)
                break;
            if (FAILED(hr)) {
                Sleep(10);
                continue;
            }
            avail = (pad < bufFrames) ? bufFrames - pad : 0;
            if (!avail) {
                Sleep(8);
                continue;
            }
            if (avail > 4096)
                avail = 4096;
            hr = rc->lpVtbl->GetBuffer(rc, avail, &dst);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED)
                break;
            if (FAILED(hr) || !dst) {
                Sleep(10);
                continue;
            }
            if (!primed) { /* erste zwei Quell-Samples */
                if (ring_read(cur, 1) < 1)
                    cur[0] = cur[1] = 0;
                if (ring_read(nxt, 1) < 1) {
                    nxt[0] = cur[0];
                    nxt[1] = cur[1];
                }
                primed = 1;
            }
            for (f = 0; f < avail; f++) {
                float fl = (cur[0] + (nxt[0] - cur[0]) * (float)frac) / 32768.0f;
                float fr = (cur[1] + (nxt[1] - cur[1]) * (float)frac) / 32768.0f;
                wa_put_frame(&dst, fl, fr, ch, bits, isfloat);
                frac += step;
                while (frac >= 1.0) {
                    int16_t s[2];
                    frac -= 1.0;
                    cur[0] = nxt[0];
                    cur[1] = nxt[1];
                    if (ring_read(s, 1) < 1) {
                        g_stat_wa_under++;
                        s[0] = s[1] = 0;
                    }
                    nxt[0] = s[0];
                    nxt[1] = s[1];
                }
            }
            hr = rc->lpVtbl->ReleaseBuffer(rc, avail, 0);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED)
                break;
            Sleep(5);
        }
        cli->lpVtbl->Stop(cli);
        rc->lpVtbl->Release(rc);
        cli->lpVtbl->Release(cli);
        HLOG_W("audio: WASAPI Geraet weg (Reconnect in 2 s)");
        Sleep(2000);
    }
    if (coinited)
        CoUninitialize();
    return 0;
}

static void au_ensure_wasapi(void)
{
    static au32 wfail;
    unsigned long tid = 0;
    if (g_wa_thread)
        return;
    /* Wie Mixer-Thread: krnl_CreateThread (rohes CreateThread scheitert an
     * ueberschriebenen PE-Headern mit Fehler 193). */
    g_wa_thread = (HANDLE)krnl_CreateThread(au_wa_thread, NULL, 0, &tid);
    if (g_wa_thread)
        HLOG_I("audio: WASAPI-Thread startet (T%04x)", tid);
    else if (hlog_throttle(&wfail))
        HLOG_W("audio: WASAPI-Thread nicht erstellbar (GLE=%lu), still weiter",
               GetLastError());
}

/* ---------- Mixer ---------- */
static float g_mixbuf[MIXCHUNK * 2];
static int16_t g_outbuf[MIXCHUNK * 2];

/* W34: faellige In-Flight-Pakete vollziehen. Muss unter au_lock laufen;
 * wartefrei (nur Speicher-Schreibungen + SetEvent, keine Sperren). all=1:
 * alle (Flush/Release), sonst nur faellige (Kopf zuerst, geordnet). */
void au_complete_due(audio_voice_t *v, int all, au32 st)
{
    while (v->ninflight > 0) {
        audio_inflight_t *r = &v->inflight[0];
        au32 i;
        if (!all && v->pc_done < r->frames_end)
            break;
        au_probe_write(r->pdwCompleted, &r->done_size, 4);
        au_probe_write(r->pdwStatus, &st, 4);
        if (r->hev)
            SetEvent(r->hev);
        for (i = 1; i < v->ninflight; i++)
            v->inflight[i - 1] = v->inflight[i];
        v->ninflight--;
    }
}

/* Rendert eine Stimme in den Mix (additiv). Muss unter au_lock laufen. */
static void au_render_voice(audio_voice_t *v, au32 n)
{
    au32 i;
    float g = v->gain;
    int use_pan = g_cfg.audio_pan; /* A7B: MixBin-Pan + Headroom */
    int use_freq = g_cfg.audio_freq; /* A7B: Frequenz-Schritt */
    int lvl = g_cfg.audio_level_diag; /* W102 DIAG(LVL) */
    float rl = 0, rr = 0; /* W102: Pegel vor Filter (DIAG) */
    /* W102 (audio_filter): DLS2-Tiefpass; Koeffizient fuer 2x MIXRATE */
    int flt = g_cfg.audio_filter && v->flt_mode == 1 && v->flt_n;
    float ff = 0.0f, fq = 1.0f;
    if (flt) {
        float s2 = v->flt_f * 0.5f; /* f = 2 sin(pi fc/fs) -> sin-Wert */
        double w = asin(s2 > 1.0f ? 1.0 : (double)s2); /* pi fc/MIXRATE */
        ff = (float)(2.0 * sin(w * 0.5));
        fq = v->flt_qd;
    }
    if (use_pan)
        g *= v->hr_gain;
    if (v->paused)
        return;
    au3d_update(v); /* W89: DS3D-Daempfung/Pan (audio_3d; sonst 1/1/1) */
    g *= v->g3d;
    /* Puffer brauchen Play(); Streams laufen sobald aktiv (Xbox: Process
     * startet den Fluss implizit, Play gibt es fuer Streams nicht). */
    if (v->kind == 1 && !v->playing)
        return;
    if (v->kind == 2 && !v->active)
        return;
    for (i = 0; i < n; i++) {
        float l = 0, r = 0;
        int have = 0;
        if (v->fifo_n) {
            l = v->fifo[v->fifo_rd * 2] * g;
            r = v->fifo[v->fifo_rd * 2 + 1] * g;
            v->fifo_rd = (v->fifo_rd + 1) % v->fifo_cap;
            v->fifo_n--;
            v->pc_done++; /* W34: Vollzugs-Uhr (nur FIFO-Konsum) */
            have = 1;
        } else if (v->data && v->datalen) {
            /* A7B: fraktionaler Playhead, SetFrequency/SetPitch = Schritt */
            float step = use_freq ? v->freq_step : 1.0f;
            float fpos = (float)v->playhead + v->play_frac;
            au32 lo_end = v->loop_len ? v->loop_start + v->loop_len : v->datalen;
            if (g_cfg.audio_loop_fix && !v->looping)
                lo_end = v->datalen; /* W89: Einmal-Ton bis Datenende */
            if (g_cfg.audio_loop_fix && lo_end > v->datalen)
                lo_end = v->datalen; /* W89: sonst haengt er still in PLAYING */
            au32 smp0, smp1;
            float smpfr;
            if (step <= 0.0f)
                step = 1.0f;
            if ((au32)fpos >= lo_end) {
                if (v->looping && v->datalen) {
                    fpos = (float)(v->loop_len ? v->loop_start % v->datalen : 0);
                } else {
                    v->playing = 0; /* ehrlich STOPPED am Ende */
                    v->playhead = v->datalen;
                    v->play_frac = 0.0f;
                    if (v->kind == 1) { /* W89 */
                        g_stat_end++;
                        au_sfx_trace(v, "ENDE", 0, 0, 0, 0);
                    }
                    break;
                }
            }
            smp0 = (au32)fpos;
            smpfr = fpos - (float)smp0;
            smp1 = (smp0 + 1 < v->datalen) ? smp0 + 1 : (v->looping ? (v->loop_len ? v->loop_start % v->datalen : 0) : smp0);
            /* A7B: Loop-Wrap passiert nach Advance (fpos, s. u.) */
                /* (alte Integer-Wrap-Zeile ersetzt) */
            if (smp0 < v->datalen) {
                l = (v->data[smp0 * 2] + (v->data[smp1 * 2] - v->data[smp0 * 2]) * smpfr) * g;
                r = (v->data[smp0 * 2 + 1] + (v->data[smp1 * 2 + 1] - v->data[smp0 * 2 + 1]) * smpfr) * g;
                fpos += step;
                /* W95 (audio_oneshot_fix): Loop-Wrap nur bei Schleife. Das
                 * Spiel setzt die Loop-Region bei JEDEM Ton auf die volle
                 * Laenge (0x107C71) und spielt Einmal-Toene mit Play(0). Der
                 * Wrap hier sprang vor dem Ende-Check (lo_end) zurueck ->
                 * kein Einmal-Ton erreichte je das Datenende (W95_d1:
                 * ends=0, 45-64 Stimmen dauerhaft aktiv, Pool voll). */
                if (v->loop_len && (v->looping || !g_cfg.audio_oneshot_fix) &&
                    (au32)fpos >= v->loop_start + v->loop_len)
                    fpos = (float)v->loop_start + fmodf(fpos - (float)v->loop_start, (float)v->loop_len);
                v->playhead = (au32)fpos;
                v->play_frac = fpos - (float)v->playhead;
                have = 1;
            }
        }
        if (!have)
            break; /* Stream ohne Daten: Stille, bleibt PLAYING */
        if (lvl) {
            rl = l;
            rr = r;
        }
        if (flt) { /* W102: DLS2-Tiefpass (Chamberlin-SVF, 2x ueberabgetastet) */
            float in[2] = { l, r };
            int c, k;
            for (c = 0; c < 2; c++) {
                float lp = v->svf_lp[c], bp = v->svf_bp[c];
                for (k = 0; k < 2; k++) {
                    float hp;
                    lp += ff * bp;
                    hp = in[c] - lp - fq * bp;
                    bp += ff * hp;
                }
                if (!(lp == lp) || lp > 1e4f || lp < -1e4f) { /* Schutz */
                    lp = 0.0f;
                    bp = 0.0f;
                }
                v->svf_lp[c] = lp;
                v->svf_bp[c] = bp;
            }
            l = v->svf_lp[0];
            r = v->svf_lp[1];
        }
        if (use_pan) { /* A7B: Stimme links/rechts aus MixBins */
            l *= v->pan_l;
            r *= v->pan_r;
            rl *= v->pan_l;
            rr *= v->pan_r;
        }
        l *= v->pan3l; /* W89: DS3D-Balance (1 ohne audio_3d/3D) */
        r *= v->pan3r;
        if (lvl) { /* W102 DIAG(LVL) */
            rl *= v->pan3l;
            rr *= v->pan3r;
            v->lv_e += (double)l * l + (double)r * r;
            v->lv_eraw += (double)rl * rl + (double)rr * rr;
            v->lv_fr++;
        }
        g_mixbuf[i * 2] += l;
        g_mixbuf[i * 2 + 1] += r;
        if (g_cfg.audio_sfx_diag > 0) { /* W89 DIAG(SFX) */
            v->dbg_energy += (double)l * l + (double)r * r;
            v->dbg_eframes++;
        }
        v->played++;
        v->dbg_consumed++; /* W14: nie zurueckgesetzt (DIAG) */
    }
    if (v->ninflight) /* W34: Async-Vollzug (Echtzeit) */
        au_complete_due(v, 0, XMP_SUCCESS);
}

/* W95 DIAG(SFXN): einmal pro Sekunde aktive Puffer-Stimmen (ohne Bink) zaehlen.
 * Muss unter au_lock laufen. act = Play aktiv, loop = davon mit Schleife,
 * mute = davon mit wirksamer Lautstaerke < -60 dB, old = Einmal-Toene laenger
 * als 10 s aktiv; starts/ends/stops/cuts = Zuwachs seit letzter Zeile. */
static void au_voice_diag(void)
{
    static au32 last_play, last_end, last_stop, last_cut;
    audio_voice_t *v, *oldest[3] = { 0, 0, 0 };
    au32 tot = 0, act = 0, loop = 0, mute = 0, old = 0, act3d = 0, i;
    double sumg = 0.0;
    for (v = g_voices; v; v = v->next) {
        float ge;
        if (v->kind != 1 || v->create_ret == 0x2435FCu)
            continue;
        tot++;
        if (!v->playing || v->paused)
            continue;
        act++;
        if (v->looping)
            loop++;
        if (v->is3d)
            act3d++;
        ge = v->gain * (g_cfg.audio_pan ? v->hr_gain : 1.0f) * v->g3d;
        if (ge < 0.001f)
            mute++;
        sumg += ge;
        if (!v->looping && g_mixed_total - v->play_mix > (uint64_t)MIXRATE * 10)
            old++;
        for (i = 0; i < 3; i++) {
            if (!oldest[i] || v->play_mix < oldest[i]->play_mix) {
                au32 k;
                for (k = 2; k > i; k--)
                    oldest[k] = oldest[k - 1];
                oldest[i] = v;
                break;
            }
        }
    }
    HLOG_I("DIAG(SFXN) t=%.1f tot=%u act=%u loop=%u mute=%u act3d=%u old=%u sumg=%.2f "
           "starts=%u ends=%u stops=%u cuts=%u",
           (double)g_mixed_total / MIXRATE, tot, act, loop, mute, act3d, old, sumg,
           g_stat_bufplay - last_play, g_stat_end - last_end,
           g_stat_stop - last_stop, g_stat_cut - last_cut);
    last_play = g_stat_bufplay;
    last_end = g_stat_end;
    last_stop = g_stat_stop;
    last_cut = g_stat_cut;
    for (i = 0; i < 3 && oldest[i]; i++) {
        v = oldest[i];
        HLOG_I("DIAG(SFXN)   alt#%u v%u age=%.1f fl=0x%X loop=%d ph=%u/%u lr=%u+%u st=%.3f "
               "g=%.4f hr=%.3f g3d=%.3f d=%.0f nfy=%u",
               i, v->id, (double)(g_mixed_total - v->play_mix) / MIXRATE,
               v->play_flags, v->looping, v->playhead, v->datalen, v->loop_start,
               v->loop_len, v->freq_step, v->gain, v->hr_gain, v->g3d, v->dbg_d3,
               v->nnotify);
    }
}

/* W102 DIAG(LVL): einmal pro Sekunde Pegel je Stimme (alle Arten, auch Bink/
 * Streams) nach <out>/voice_levels.csv; alle 30 s Rangliste nach Gesamtenergie
 * (= Laufzeit x mittlerer Pegel) als DIAG(LVLT). Muss unter au_lock laufen. */
static FILE *g_lv_csv;
static void au_level_diag(void)
{
    audio_voice_t *v;
    double t = (double)g_mixed_total / MIXRATE;
    static int tick;
    if (!g_lv_csv) {
        char path[600];
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s/voice_levels.csv",
                    hcfg_out_dir());
        g_lv_csv = _fsopen(path, "w", _SH_DENYNO); /* waehrend des Laufs lesbar */
        if (g_lv_csv)
            fprintf(g_lv_csv, "t,id,kind,cr,datap,datalen,loop,playing,rms_db,rawrms_db,"
                              "fr,gain,hr,g3d,d3,panl,panr,step,fmode,fc0,fc1,fn,"
                              "i3d,i3ddir,i3ddirhf,i3droom,eg,egn,dbins,fmt,rate\n");
    }
    for (v = g_voices; v; v = v->next) {
        double rms, raw;
        if (v->lv_fr == 0) {
            v->lv_e = v->lv_eraw = 0.0;
            continue;
        }
        rms = sqrt(v->lv_e / (2.0 * MIXRATE));
        raw = sqrt(v->lv_eraw / (2.0 * MIXRATE));
        v->lv_etot += v->lv_e;
        if (rms > 0.001)
            v->lv_secs++;
        if (g_lv_csv)
            fprintf(g_lv_csv,
                    "%.1f,%u,%u,0x%X,0x%X,%u,%d,%d,%.1f,%.1f,%llu,%.4f,%.3f,%.3f,%.0f,%.2f,%.2f,"
                    "%.3f,%u,0x%X,0x%X,%u,%d,%d,%d,%d,0x%X,%u,0x%X,%u,%u\n",
                    t, v->id, v->kind, v->create_ret, v->dbg_setbuf_p, v->datalen,
                    v->looping, v->playing, rms > 1e-7 ? 20.0 * log10(rms) : -140.0,
                    raw > 1e-7 ? 20.0 * log10(raw) : -140.0,
                    (unsigned long long)v->lv_fr, v->gain, v->hr_gain, v->g3d, v->dbg_d3,
                    v->pan_l, v->pan_r, v->freq_step, v->flt_mode, v->flt_c[0], v->flt_c[1],
                    v->flt_n, v->has_i3dl2, v->i3d_direct, v->i3d_directhf, v->i3d_room,
                    v->eg_p, v->eg_n, v->desc_bins, v->have_fmt ? v->fmt.tag : 0, v->src_rate);
        v->lv_e = v->lv_eraw = 0.0;
        v->lv_fr = 0;
    }
    if (g_lv_csv)
        fflush(g_lv_csv);
    if (++tick % 30 == 0) {
        audio_voice_t *top[8];
        int nt = 0, k;
        double tot = 0.0;
        for (v = g_voices; v; v = v->next) {
            tot += v->lv_etot;
            if (v->lv_etot <= 0.0)
                continue;
            if (nt < 8)
                top[nt++] = v;
            else if (v->lv_etot > top[7]->lv_etot)
                top[7] = v;
            else
                continue;
            for (k = nt - 1; k > 0 && top[k - 1]->lv_etot < top[k]->lv_etot; k--) {
                audio_voice_t *x = top[k];
                top[k] = top[k - 1];
                top[k - 1] = x;
            }
        }
        for (k = 0; k < nt; k++) {
            v = top[k];
            HLOG_I("DIAG(LVLT) t=%.0f #%d v%u kind=%u cr=0x%X anteil=%.1f%% secs=%u "
                   "rms_mittel=%.0f loop=%d datap=0x%X len=%u fmode=%u fc=0x%X/0x%X fn=%u "
                   "i3d=%d dir=%d g=%.3f hr=%.3f g3d=%.3f",
                   t, k, v->id, v->kind, v->create_ret,
                   tot > 0 ? 100.0 * v->lv_etot / tot : 0.0, v->lv_secs,
                   v->lv_secs ? sqrt(v->lv_etot / (2.0 * MIXRATE * v->lv_secs)) * 32767.0 : 0.0,
                   v->looping, v->dbg_setbuf_p, v->datalen, v->flt_mode, v->flt_c[0],
                   v->flt_c[1], v->flt_n, v->has_i3dl2, v->i3d_direct, v->gain,
                   v->hr_gain, v->g3d);
        }
    }
}

/* Quell-Position (Bytes) aus gerenderten Frames ableiten. */
uint64_t au_src_pos(audio_voice_t *v)
{
    if (!v->have_fmt)
        return v->played * 4;
    {
        uint64_t frames =
            v->played * (uint64_t)v->src_rate / MIXRATE;
        if (v->fmt.tag == 105)
            return (frames / 64) * 36u * v->fmt.ch;
        return frames * v->fmt.align;
    }
}

static void au_pump_notifies(audio_voice_t *v)
{
    uint64_t newpos;
    au32 i;
    if (!v->nnotify)
        return;
    newpos = au_src_pos(v);
    for (i = 0; i < v->nnotify; i++) {
        au32 off = v->notifies[i].off;
        HANDLE hev;
        int fire = 0;
        if (off == DSBPN_OFFSETSTOP) {
            if (!v->playing && v->last_pump_pos)
                fire = 1; /* angehalten nach Lauf */
        } else if (newpos >= v->last_pump_pos) {
            if (off > v->last_pump_pos && (uint64_t)off <= newpos)
                fire = 1;
        } else {
            /* Wrap: Offset im umbrochenen Intervall */
            if ((uint64_t)off <= newpos || off > v->last_pump_pos)
                fire = 1;
        }
        if (!fire)
            continue;
        hev = (HANDLE)krnl_event_host(v->notifies[i].xhev);
        if (hev)
            SetEvent(hev);
    }
    v->last_pump_pos = newpos;
}

extern volatile LONG g_au_want; /* W168 PERF-TICK */
extern DWORD g_au_mix_tid;
extern volatile LONGLONG g_aul_wait_q, g_aul_wait_max_q, g_aul_hold_q, g_aul_hold_max_q;
extern volatile LONG g_aul_wait_n, g_aul_hold_n, g_aul_yield_n;
extern volatile LONGLONG g_aul_sb_q, g_aul_sb_max_q;
extern volatile LONG g_aul_sb_n;
static DWORD WINAPI au_mix_thread(LPVOID p)
{
    LARGE_INTEGER qpf, last, now;
    (void)p;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&last);
    g_au_mix_tid = GetCurrentThreadId(); /* W168 PERF-TICK */
    for (;;) {
        au32 i;
        static au32 mix_pass;
        static LONG au_was_paused; /* W188 */
        LARGE_INTEGER h0, h1;
        /* mischen */
        memset(g_mixbuf, 0, sizeof g_mixbuf);
        au_lock();
        QueryPerformanceCounter(&h0);
        if (g_sb_paused != au_was_paused) {
            /* W188: Pause-Wechsel mit Position im Ausgabestrom (audio.wav: Frame = 1/44100 s) */
            au_was_paused = g_sb_paused;
            HLOG_I("AUDIO: Pause %s bei Ausgabe-Frame %llu", au_was_paused ? "an (Mischer stumm)" : "aus",
                   (unsigned long long)g_mixed_total);
        }
        if (au_was_paused) {
            /* W188 GAME-MENU: Pause - Stille ausgeben, keine Stimme rueckt vor (Positionen, Notifies und
             * Stream-Pakete stehen), die Live-Ausgabe bleibt versorgt. */
        } else if (g_cfg.audio_mix_yield) {
            /* PERF(PERF-TICK,W168): jede Stimme genau einmal je Durchgang (mix_pass); wartet ein anderer
             * Thread auf au_lock, nach der laufenden Stimme abgeben und danach die Liste von vorn durchgehen
             * (gemischte Stimmen ueberspringen) - eingefuegte/freigegebene Stimmen bleiben so sicher. */
            audio_voice_t *v;
            mix_pass++;
            v = g_voices;
            while (v) {
                if (v->mix_pass != mix_pass) {
                    v->mix_pass = mix_pass;
                    au_render_voice(v, MIXCHUNK);
                    au_pump_notifies(v);
                }
                if (g_au_want) {
                    int k = 0;
                    QueryPerformanceCounter(&h1);
                    g_aul_hold_q += h1.QuadPart - h0.QuadPart;
                    if (h1.QuadPart - h0.QuadPart > g_aul_hold_max_q) { g_aul_hold_max_q = h1.QuadPart - h0.QuadPart; }
                    au_unlock();
                    while (g_au_want && k < 4000) { if (++k < 64) { YieldProcessor(); } else { SwitchToThread(); } }
                    g_aul_yield_n++;
                    au_lock();
                    QueryPerformanceCounter(&h0);
                    v = g_voices;
                    continue;
                }
                v = v->next;
            }
        } else
        {
            audio_voice_t *v = g_voices;
            while (v) {
                au_render_voice(v, MIXCHUNK);
                au_pump_notifies(v);
                v = v->next;
            }
        }
        if (g_cfg.audio_voice_diag && (g_mixed_total % (MIXCHUNK * 50)) == 0)
            au_voice_diag(); /* W95: 1x pro Sekunde */
        if (g_cfg.audio_level_diag && (g_mixed_total % (MIXCHUNK * 50)) == 0)
            au_level_diag(); /* W102: 1x pro Sekunde */
        QueryPerformanceCounter(&h1);
        g_aul_hold_q += h1.QuadPart - h0.QuadPart; /* DIAG(PERF-TICK,W168) */
        if (h1.QuadPart - h0.QuadPart > g_aul_hold_max_q) { g_aul_hold_max_q = h1.QuadPart - h0.QuadPart; }
        g_aul_hold_n++;
        au_unlock();
        if (g_cfg.audio_lock_diag && (g_aul_hold_n % 500u) == 0u) { /* DIAG(PERF-TICK,W168): alle ~10 s */
            double k = 1000.0 / (double)qpf.QuadPart;
            HLOG_I("AUDIOLOCK 10s: mischer_haelt=%.2fms mittel (max %.2fms) n=%ld abgegeben=%ld | andere_warten=%.1fms (n=%ld, max %.2fms) | setbuf=%.1fms (n=%ld, max %.2fms)",
                   g_aul_hold_n ? (double)g_aul_hold_q * k / 500.0 : 0.0, (double)g_aul_hold_max_q * k, (long)g_aul_hold_n,
                   (long)g_aul_yield_n, (double)g_aul_wait_q * k, (long)g_aul_wait_n, (double)g_aul_wait_max_q * k,
                   (double)g_aul_sb_q * k, (long)g_aul_sb_n, (double)g_aul_sb_max_q * k);
            g_aul_hold_q = g_aul_hold_max_q = g_aul_wait_q = g_aul_wait_max_q = 0;
            g_aul_wait_n = g_aul_yield_n = 0;
            g_aul_sb_q = g_aul_sb_max_q = 0; g_aul_sb_n = 0;
        }
        /* Clamp + s16 + Peak */
        for (i = 0; i < MIXCHUNK * 2; i++) {
            float s = g_mixbuf[i];
            float a;
            int v;
            if (s > 1.0f)
                s = 1.0f;
            if (s < -1.0f)
                s = -1.0f;
            a = s < 0 ? -s : s;
            if (a > g_peak)
                g_peak = a;
            v = (int)(s * 32767.0f);
            g_outbuf[i] = (int16_t)v;
        }
        if (g_cfg.audio_live == 1) { /* A7B: WASAPI (neu, Default) */
            ring_write(g_outbuf);
            au_ensure_wasapi();
        } else if (g_cfg.audio_live == 2) { /* WinMM (alt) */
            au_wo_write(g_outbuf);
        } /* 0 = nur Capture */
        if (g_cfg.audio_capture) {
            if (!g_wav)
                au_wav_open();
            if (g_wav) {
                fwrite(g_outbuf, 1, sizeof g_outbuf, g_wav);
                g_wav_bytes += sizeof g_outbuf;
                au_wav_write_header();
            }
        }
        g_mixed_total += MIXCHUNK;
        if ((g_mixed_total % (MIXCHUNK * 3000)) == 0) /* alle ~60 s */
            audio_log_stats();
        /* 20-ms-Takt */
        QueryPerformanceCounter(&now);
        {
            LONGLONG want =
                last.QuadPart + qpf.QuadPart * MIXCHUNK / MIXRATE;
            LONGLONG diff = want - now.QuadPart;
            LONG ms =
                (LONG)(diff * 1000 / (qpf.QuadPart ? qpf.QuadPart : 1));
            last.QuadPart = want;
            if (ms < 1)
                ms = 1;
            if (ms > 100)
                ms = 100;
            /* WinMM-Flusskontrolle: nicht schneller als Abspiel */
            if (g_wo_ok == 1 && g_wo_queued >= WO_NBUF)
                ms = 15;
            Sleep((DWORD)ms);
        }
    }
    return 0;
}

void au_ensure_thread(void)
{
    if (g_mix_thread)
        return;
    au_lock();
    if (!g_mix_thread) {
        unsigned long tid = 0;
        /* WICHTIG: krnl_CreateThread (nicht roh): CreateThread validiert per
         * RtlImageNtHeader die PE-Header, die das Spiel ueberschrieben hat
         * (-> Fehler 193). Der Wrapper blendet sie kurz ein. */
        g_mix_thread = (HANDLE)krnl_CreateThread(au_mix_thread, NULL, 0, &tid);
        if (g_mix_thread)
            HLOG_I("AUDIO Mixer-Thread startet (T%04x, 20-ms-Takt)", tid);
        else
            HLOG_E("AUDIO Mixer-CreateThread FEHLER %lu", GetLastError());
    }
    au_unlock();
}
