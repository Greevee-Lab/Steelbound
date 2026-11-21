/* loc_wma.c - W367 LOC-WMA: WMA-Soundtrack fuer Line of Contact (s. loc_wma.h).
 *
 * Belege (LoC-XBE, Ghidra nur gelesen, LOC_BOOT §43):
 *   - Die Playlist ist der "eigene Soundtrack" des Spielers (Optionsmenue, gespeichert 0x4A6 Bytes): bis 99 Eintraege
 *     {Soundtrack-ID, Lied-ID, Lautstaerke, Spiel-Lied-Index}. Spiel-Lieder: "z:\Patrol.wma", "z:\Turbulence.wma"
 *     (Tabelle 0x274960; beim Start aus media\sndeff des Xbox-Laufwerks D nach Z kopiert). Standard nach dem Zuruecksetzen: aus, leer.
 *   - Einsatzbeginn FUN_0008C3D0: FUN_0008D350 (Release, CreateWmaPlayList(Bank, ST_CUE, Zufall|2), Add je Eintrag,
 *     Next, bei Zufall rand()%n weitere Next) und FUN_0008D0B0 (nur wenn eingeschaltet und n > 0: PlayEx ST_CUE).
 *   - Meldung wType 0 fuer ST_CUE (je Liedstart): FUN_0008D2B0 setzt die Lautstaerke des Lieds (Kategorie 2),
 *     FUN_0008C540 zaehlt den Index weiter. Taste 0x22 = Next, 0x17 = Pause (GlobalPause Kategorie 2).
 *   - Menue-Vorhoeren FUN_0008D4D0/FUN_0008D6F0: Create(…, 2), ein Add, Next, PlayEx ST_CUE.
 * Musik veraendert keinen spielrelevanten Zustand: nur Kategorie-2-Lautstaerke und den Soundtrack-Index (eigener
 * Zustand je Xbox im Original). Die Dekodierung laeuft im eigenen Thread; der Spiel-Thread setzt nur Soll-Werte.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include "loc_wma.h"

#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LW_SELFTEST
#include "audio_out.h"
#include "hcfg.h"
#include "hlog.h"
#endif

/* ---------------- Playlist-Modell ---------------- */
static uint32_t lw_rand(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

void lw_pl_init(lw_pl_t *p, uint32_t sb, int cue, uint32_t flags)
{
    memset(p, 0, sizeof *p);
    p->kind = LW_K_PL;
    p->sb = sb;
    p->cue = cue;
    p->flags = flags;
    p->behavior = (flags & 1u) ? 3u : 2u;
    p->cur = -1;
    p->rnd = 0x2545F491u; /* fester Startwert: gleiche Reihenfolge in jedem Lauf (keine Zufallsquelle der Maschine) */
}

int lw_pl_add(lw_pl_t *p, uint32_t type, const char *path, uint32_t a, uint32_t b, uint32_t *out_id)
{
    lw_entry_t *e;
    if (!p || p->n >= LW_MAX_SONGS) {
        return -1;
    }
    e = &p->e[p->n];
    memset(e, 0, sizeof *e);
    e->type = type;
    if (type == LW_ENTRY_FILE && path) {
        strncpy_s(e->path, sizeof e->path, path, _TRUNCATE);
    }
    e->st_id = a;
    e->song_id = b;
    e->id = LW_SONG_ID_BASE + (uint32_t)p->n;
    if (out_id) {
        *out_id = e->id;
    }
    p->n++;
    return 0;
}

int lw_pl_shuffle(const lw_pl_t *p) { return p->behavior == 3u || (p->behavior != 2u && (p->flags & 1u)); }

int lw_pl_playable(const lw_pl_t *p, int i)
{
    return i >= 0 && i < p->n && p->e[i].type == LW_ENTRY_FILE && p->e[i].path[0];
}

int lw_pl_next(lw_pl_t *p)
{
    if (!p || p->n <= 0) {
        return -1;
    }
    p->cur = p->cur < 0 ? 0 : (p->cur + 1) % p->n;
    return p->cur;
}

int lw_pl_after(lw_pl_t *p, int i)
{
    if (p->n <= 0) {
        return -1;
    }
    if (lw_pl_shuffle(p) && p->n > 1) {
        int j = (int)(lw_rand(&p->rnd) % (uint32_t)(p->n - 1));
        return j >= i ? j + 1 : j; /* nie dasselbe Lied zweimal hintereinander */
    }
    if (i + 1 >= p->n && !(p->flags & 2u)) {
        return -1;
    }
    return (i + 1) % p->n;
}

int lw_is_asf(const uint8_t *data, uint32_t n)
{
    static const uint8_t asf_hdr[16] = { 0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                         0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C };
    return data && n >= 30 && memcmp(data, asf_hdr, 16) == 0;
}

/* ---------------- Media Foundation (dynamisch geladen) ---------------- */
typedef HRESULT(WINAPI *pMFStartup_t)(ULONG, DWORD);
typedef HRESULT(WINAPI *pMFShutdown_t)(void);
typedef HRESULT(WINAPI *pMFCreateMFByteStreamOnStream_t)(IStream *, IMFByteStream **);
typedef HRESULT(WINAPI *pMFCreateMediaType_t)(IMFMediaType **);
typedef HRESULT(WINAPI *pMFCreateSourceReaderFromByteStream_t)(IMFByteStream *, IMFAttributes *, IMFSourceReader **);
typedef IStream *(WINAPI *pSHCreateMemStream_t)(const BYTE *, UINT);

static pMFStartup_t p_MFStartup;
static pMFShutdown_t p_MFShutdown;
static pMFCreateMFByteStreamOnStream_t p_MFCreateMFByteStreamOnStream;
static pMFCreateMediaType_t p_MFCreateMediaType;
static pMFCreateSourceReaderFromByteStream_t p_MFCreateSourceReaderFromByteStream;
static pSHCreateMemStream_t p_SHCreateMemStream;
static __declspec(thread) int t_mf_state; /* 0 offen, 1 ok, -1 Fehler (je Thread) */
static __declspec(thread) int t_com;

static int lw_mf_load(char *err, int cap)
{
    static volatile LONG done; /* 1 = geladen, -1 = Fehler */
    HMODULE mp, mr, sh;
    if (done == 1) {
        return 0;
    }
    if (done == -1) {
        _snprintf_s(err, (size_t)cap, _TRUNCATE, "Media Foundation nicht verfuegbar");
        return -1;
    }
    mp = LoadLibraryW(L"mfplat.dll");
    mr = LoadLibraryW(L"mfreadwrite.dll");
    sh = LoadLibraryW(L"shlwapi.dll");
    if (mp) {
        p_MFStartup = (pMFStartup_t)(void *)GetProcAddress(mp, "MFStartup");
        p_MFShutdown = (pMFShutdown_t)(void *)GetProcAddress(mp, "MFShutdown");
        p_MFCreateMFByteStreamOnStream =
            (pMFCreateMFByteStreamOnStream_t)(void *)GetProcAddress(mp, "MFCreateMFByteStreamOnStream");
        p_MFCreateMediaType = (pMFCreateMediaType_t)(void *)GetProcAddress(mp, "MFCreateMediaType");
    }
    if (mr) {
        p_MFCreateSourceReaderFromByteStream =
            (pMFCreateSourceReaderFromByteStream_t)(void *)GetProcAddress(mr, "MFCreateSourceReaderFromByteStream");
    }
    if (sh) {
        p_SHCreateMemStream = (pSHCreateMemStream_t)(void *)GetProcAddress(sh, "SHCreateMemStream");
    }
    if (!p_MFStartup || !p_MFShutdown || !p_MFCreateMFByteStreamOnStream || !p_MFCreateMediaType ||
        !p_MFCreateSourceReaderFromByteStream || !p_SHCreateMemStream) {
        _snprintf_s(err, (size_t)cap, _TRUNCATE, "Media Foundation fehlt (mfplat=%d mfreadwrite=%d shlwapi=%d)",
                    mp != NULL, mr != NULL, sh != NULL);
        InterlockedExchange(&done, -1);
        return -1;
    }
    InterlockedExchange(&done, 1);
    return 0;
}

int lw_mf_thread_init(char *err, int cap)
{
    HRESULT hr;
    if (t_mf_state) {
        if (t_mf_state < 0) {
            _snprintf_s(err, (size_t)cap, _TRUNCATE, "Media Foundation nicht verfuegbar");
        }
        return t_mf_state > 0 ? 0 : -1;
    }
    t_mf_state = -1;
    if (lw_mf_load(err, cap) != 0) {
        return -1;
    }
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    t_com = SUCCEEDED(hr);
    hr = p_MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        _snprintf_s(err, (size_t)cap, _TRUNCATE, "MFStartup 0x%08lX", (unsigned long)hr);
        return -1;
    }
    t_mf_state = 1;
    return 0;
}

void lw_mf_thread_done(void)
{
    if (t_mf_state > 0) {
        p_MFShutdown();
    }
    if (t_com) {
        CoUninitialize();
    }
    t_mf_state = 0;
    t_com = 0;
}

struct lw_dec {
    IMFSourceReader *rd;
    int rate, ch;
    double secs;
    uint8_t *pend;
    uint32_t pend_n, pend_off, pend_cap;
    int eof;
};

static void lw_rel(IUnknown *u)
{
    if (u) {
        IUnknown_Release(u);
    }
}

lw_dec_t *lw_dec_open(const uint8_t *data, uint32_t n, char *err, int cap)
{
    lw_dec_t *d = NULL;
    IStream *st = NULL;
    IMFByteStream *bs = NULL;
    IMFSourceReader *rd = NULL;
    IMFMediaType *mt = NULL, *cur = NULL;
    HRESULT hr;
    UINT32 rate = 0, ch = 0, bits = 0;
    PROPVARIANT pv;
    if (t_mf_state <= 0 && lw_mf_thread_init(err, cap) != 0) {
        return NULL;
    }
    if (!lw_is_asf(data, n)) {
        _snprintf_s(err, (size_t)cap, _TRUNCATE, "kein ASF/WMA-Kopf (%u Bytes)", n);
        return NULL;
    }
    st = p_SHCreateMemStream(data, n);
    if (!st) {
        _snprintf_s(err, (size_t)cap, _TRUNCATE, "SHCreateMemStream");
        return NULL;
    }
    hr = p_MFCreateMFByteStreamOnStream(st, &bs);
    if (SUCCEEDED(hr)) {
        hr = p_MFCreateSourceReaderFromByteStream(bs, NULL, &rd);
    }
    if (FAILED(hr)) {
        _snprintf_s(err, (size_t)cap, _TRUNCATE, "Source Reader 0x%08lX (WMA-Decoder fehlt oder Datei kaputt)",
                    (unsigned long)hr);
        goto fail;
    }
    IMFSourceReader_SetStreamSelection(rd, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    hr = IMFSourceReader_SetStreamSelection(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    if (SUCCEEDED(hr)) {
        hr = p_MFCreateMediaType(&mt);
    }
    if (SUCCEEDED(hr)) {
        IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
        IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
        IMFMediaType_SetUINT32(mt, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        hr = IMFSourceReader_SetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, mt);
    }
    if (SUCCEEDED(hr)) {
        hr = IMFSourceReader_GetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur);
    }
    if (SUCCEEDED(hr)) {
        IMFMediaType_GetUINT32(cur, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        IMFMediaType_GetUINT32(cur, &MF_MT_AUDIO_NUM_CHANNELS, &ch);
        IMFMediaType_GetUINT32(cur, &MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
    }
    if (FAILED(hr) || rate < 8000 || rate > 96000 || ch < 1 || ch > 2 || bits != 16) {
        _snprintf_s(err, (size_t)cap, _TRUNCATE, "PCM-Ausgabe nicht moeglich (hr 0x%08lX, %u Hz, %u Kan., %u Bit)",
                    (unsigned long)hr, rate, ch, bits);
        goto fail;
    }
    d = (lw_dec_t *)calloc(1, sizeof *d);
    if (!d) {
        goto fail;
    }
    d->rd = rd;
    d->rate = (int)rate;
    d->ch = (int)ch;
    PropVariantInit(&pv);
    if (SUCCEEDED(IMFSourceReader_GetPresentationAttribute(rd, (DWORD)MF_SOURCE_READER_MEDIASOURCE, &MF_PD_DURATION,
                                                           &pv)) &&
        pv.vt == VT_UI8) {
        d->secs = (double)pv.uhVal.QuadPart / 1e7;
    }
    PropVariantClear(&pv);
    lw_rel((IUnknown *)cur);
    lw_rel((IUnknown *)mt);
    lw_rel((IUnknown *)bs);
    lw_rel((IUnknown *)st);
    return d;
fail:
    lw_rel((IUnknown *)cur);
    lw_rel((IUnknown *)mt);
    lw_rel((IUnknown *)rd);
    lw_rel((IUnknown *)bs);
    lw_rel((IUnknown *)st);
    return NULL;
}

void lw_dec_format(const lw_dec_t *d, int *rate, int *ch)
{
    *rate = d ? d->rate : 0;
    *ch = d ? d->ch : 0;
}

double lw_dec_secs(const lw_dec_t *d) { return d ? d->secs : 0; }

int lw_dec_read(lw_dec_t *d, uint8_t *dst, uint32_t cap, uint32_t *got)
{
    uint32_t align = (uint32_t)(2 * d->ch);
    *got = 0;
    cap -= cap % align;
    while (*got < cap) {
        if (d->pend_off < d->pend_n) {
            uint32_t k = d->pend_n - d->pend_off;
            if (k > cap - *got) {
                k = cap - *got;
            }
            memcpy(dst + *got, d->pend + d->pend_off, k);
            d->pend_off += k;
            *got += k;
            continue;
        }
        if (d->eof) {
            break;
        }
        {
            DWORD idx = 0, fl = 0;
            LONGLONG ts = 0;
            IMFSample *smp = NULL;
            IMFMediaBuffer *mb = NULL;
            BYTE *p = NULL;
            DWORD n = 0;
            HRESULT hr = IMFSourceReader_ReadSample(d->rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &idx, &fl,
                                                    &ts, &smp);
            if (FAILED(hr) || (fl & MF_SOURCE_READERF_ERROR)) {
                lw_rel((IUnknown *)smp);
                return *got ? 0 : -1;
            }
            if (fl & MF_SOURCE_READERF_ENDOFSTREAM) {
                d->eof = 1;
            }
            if (!smp) {
                continue;
            }
            hr = IMFSample_ConvertToContiguousBuffer(smp, &mb);
            if (SUCCEEDED(hr)) {
                hr = IMFMediaBuffer_Lock(mb, &p, NULL, &n);
            }
            if (SUCCEEDED(hr)) {
                n -= n % align;
                if (n > d->pend_cap) {
                    uint8_t *q = (uint8_t *)realloc(d->pend, n);
                    if (!q) {
                        IMFMediaBuffer_Unlock(mb);
                        lw_rel((IUnknown *)mb);
                        lw_rel((IUnknown *)smp);
                        return -1;
                    }
                    d->pend = q;
                    d->pend_cap = n;
                }
                memcpy(d->pend, p, n);
                d->pend_n = n;
                d->pend_off = 0;
                IMFMediaBuffer_Unlock(mb);
            }
            lw_rel((IUnknown *)mb);
            lw_rel((IUnknown *)smp);
            if (FAILED(hr)) {
                return *got ? 0 : -1;
            }
        }
    }
    return (*got == 0 && d->eof && d->pend_off >= d->pend_n) ? 1 : 0;
}

void lw_dec_close(lw_dec_t *d)
{
    if (!d) {
        return;
    }
    lw_rel((IUnknown *)d->rd);
    free(d->pend);
    free(d);
}

#ifndef LW_SELFTEST
/* ---------------- Player (Client) ---------------- */
extern int krnl_host_path(const char *xb, char *out, size_t cap);       /* krnl_file.c */
extern int krnl_read_d(const char *rel, unsigned char **buf, unsigned *len); /* krnl_file.c */

#define LW_PKTS 3
#define LW_CHUNK 65536u
#define LW_E_PENDING 0x8000000Au
#define LW_CACHE 4

static struct {
    SRWLOCK lk;
    HANDLE th, ev;
    /* Soll (Spiel-Thread, unter lk) */
    int run, auto_mode;
    long gen;
    int jump;
    int db100, ucat, paused;
    lw_pl_t pl;
    /* Ist (Player-Thread) */
    volatile LONG active, starts, song, fails, songs, pkts, mf_bad;
} s_p = { SRWLOCK_INIT, NULL, NULL, 0, 0, 0, 0, 0, 0, 0 };

static struct {
    char path[LW_PATH];
    uint8_t *data;
    unsigned n;
} s_cache[LW_CACHE];

/* Lieddatei laden: Z:-Kopie des Spiels, sonst media\sndeff\<Name> auf dem Xbox-Laufwerk D (Abbild/Ordner). Nur im Player-Thread. */
static const uint8_t *lw_load(const char *xb, unsigned *n)
{
    int i, slot = -1;
    char host[1024];
    unsigned char *buf = NULL;
    unsigned len = 0;
    if (g_cfg.loc_wma_fail_test) { /* DIAG: Fehlerfall wie fehlende/kaputte Datei */
        return NULL;
    }
    for (i = 0; i < LW_CACHE; i++) {
        if (s_cache[i].data && _stricmp(s_cache[i].path, xb) == 0) {
            *n = s_cache[i].n;
            return s_cache[i].data;
        }
        if (!s_cache[i].data && slot < 0) {
            slot = i;
        }
    }
    if (krnl_host_path(xb, host, sizeof host) == 0) {
        HANDLE h = CreateFileA(host, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD sz = GetFileSize(h, NULL), got = 0;
            if (sz != INVALID_FILE_SIZE && sz > 0 && sz < 64u * 1024u * 1024u && (buf = (unsigned char *)malloc(sz))) {
                if (!ReadFile(h, buf, sz, &got, NULL) || got != sz) {
                    free(buf);
                    buf = NULL;
                } else {
                    len = sz;
                }
            }
            CloseHandle(h);
        }
    }
    if (!buf) {
        const char *b = strrchr(xb, '\\');
        char rel[160];
        b = b ? b + 1 : (strchr(xb, ':') ? strchr(xb, ':') + 1 : xb);
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "media\\sndeff\\%s", b);
        if (krnl_read_d(rel, &buf, &len) != 0) {
            buf = NULL;
        }
    }
    if (!buf) {
        return NULL;
    }
    if (slot < 0) {
        slot = 0;
        free(s_cache[0].data);
    }
    strncpy_s(s_cache[slot].path, sizeof s_cache[slot].path, xb, _TRUNCATE);
    s_cache[slot].data = buf;
    s_cache[slot].n = len;
    *n = len;
    return buf;
}

typedef struct {
    uint32_t v;
    int rate, ch;
    uint32_t pkt[LW_PKTS][6];
    uint32_t status[LW_PKTS], compl_[LW_PKTS];
    int inuse[LW_PKTS];
    uint8_t *buf[LW_PKTS];
    int db, ucat, paused;
} lw_voice_t;

static void lw_voice_clear(lw_voice_t *vo)
{
    int k;
    if (vo->v) {
        audio_flush(vo->v);
        audio_stop(vo->v);
        audio_release(vo->v);
    }
    for (k = 0; k < LW_PKTS; k++) {
        free(vo->buf[k]);
    }
    memset(vo, 0, sizeof *vo);
}

static int lw_voice_make(lw_voice_t *vo, int rate, int ch, int db, int ucat, int paused)
{
    uint8_t wfx[20];
    uint32_t desc[9], a[16];
    uint16_t w16;
    uint32_t w32;
    float mn = 8.0f, mx = 1000.0f;
    memset(wfx, 0, sizeof wfx);
    w16 = 1; memcpy(wfx + 0, &w16, 2);                     /* PCM */
    w16 = (uint16_t)ch; memcpy(wfx + 2, &w16, 2);
    w32 = (uint32_t)rate; memcpy(wfx + 4, &w32, 4);
    w32 = (uint32_t)(rate * ch * 2); memcpy(wfx + 8, &w32, 4);
    w16 = (uint16_t)(ch * 2); memcpy(wfx + 12, &w16, 2);
    w16 = 16; memcpy(wfx + 14, &w16, 2);
    memset(desc, 0, sizeof desc);
    desc[0] = 36;
    desc[3] = (uint32_t)(uintptr_t)wfx;
    vo->v = audio_stream_create((uint32_t)(uintptr_t)desc, 0);
    if (!vo->v) {
        return -1;
    }
    vo->rate = rate;
    vo->ch = ch;
    audio_set_ucat(vo->v, ucat);
    audio_set_volume(vo->v, db);
    audio_set_pan(vo->v, 1.0f, 1.0f);
    memset(a, 0, sizeof a);
    memcpy(&a[13], &mn, 4);
    memcpy(&a[14], &mx, 4);
    a[15] = 2; /* 2D (3D aus) */
    audio_3d_voice_allparams(vo->v, (uint32_t)(uintptr_t)a);
    if (g_cfg.audio_xact_sends) {
        static const uint32_t none[9];
        audio_set_room_send(vo->v, -10000); /* trocken wie 2D-Cues */
        audio_set_i3dl2_vals(vo->v, none);
    }
    if (paused) {
        audio_pause(vo->v, 1);
    }
    vo->db = db;
    vo->ucat = ucat;
    vo->paused = paused;
    return 0;
}

/* Pakete nachspeisen; Rueckgabe 1 = Lied zu Ende dekodiert, -1 = Dekoderfehler */
static int lw_voice_feed(lw_voice_t *vo, lw_dec_t *dec, int *pending)
{
    int k, r = 0;
    *pending = 0;
    for (k = 0; k < LW_PKTS; k++) {
        if (vo->inuse[k] && vo->status[k] != LW_E_PENDING) {
            vo->inuse[k] = 0;
        }
        *pending += vo->inuse[k];
    }
    for (k = 0; k < LW_PKTS && dec; k++) {
        uint32_t got = 0;
        if (vo->inuse[k]) {
            continue;
        }
        if (!vo->buf[k] && !(vo->buf[k] = (uint8_t *)malloc(LW_CHUNK))) {
            return -1;
        }
        r = lw_dec_read(dec, vo->buf[k], LW_CHUNK, &got);
        if (got) {
            vo->pkt[k][0] = (uint32_t)(uintptr_t)vo->buf[k];
            vo->pkt[k][1] = got;
            vo->pkt[k][2] = (uint32_t)(uintptr_t)&vo->compl_[k];
            vo->pkt[k][3] = (uint32_t)(uintptr_t)&vo->status[k];
            vo->pkt[k][4] = 0;
            vo->pkt[k][5] = 0;
            vo->status[k] = LW_E_PENDING;
            vo->inuse[k] = 1;
            audio_process(vo->v, (uint32_t)(uintptr_t)vo->pkt[k], 0);
            (*pending)++;
            InterlockedIncrement(&s_p.pkts);
        }
        if (r != 0) {
            return r;
        }
    }
    return 0;
}

static DWORD WINAPI lw_thread(LPVOID arg)
{
    lw_voice_t vo;
    lw_dec_t *dec = NULL;
    lw_pl_t pl;
    long my_gen = -1;
    int cur = -1, need_open = 0, run = 0, failrow = 0;
    char err[256];
    (void)arg;
    memset(&vo, 0, sizeof vo);
    memset(&pl, 0, sizeof pl);
    for (;;) {
        int db, ucat, paused, pend = 0;
        long gen;
        WaitForSingleObject(s_p.ev, 15);
        AcquireSRWLockExclusive(&s_p.lk);
        gen = s_p.gen;
        db = s_p.db100;
        ucat = s_p.ucat;
        paused = s_p.paused;
        if (gen != my_gen) {
            run = s_p.run;
            pl = s_p.pl;
            cur = s_p.jump;
        }
        ReleaseSRWLockExclusive(&s_p.lk);
        if (gen != my_gen) {
            my_gen = gen;
            lw_dec_close(dec);
            dec = NULL;
            lw_voice_clear(&vo);
            need_open = run;
            failrow = 0;
            if (!run) {
                InterlockedExchange(&s_p.song, -1);
                InterlockedExchange(&s_p.active, 0);
            }
        }
        if (!run) {
            continue;
        }
        if (vo.v && db != vo.db) {
            audio_set_volume(vo.v, db);
            vo.db = db;
        }
        if (vo.v && paused != vo.paused) {
            audio_pause(vo.v, paused ? 1 : 0);
            vo.paused = paused;
        }
        if (need_open) {
            const uint8_t *data = NULL;
            unsigned n = 0;
            err[0] = '\0';
            if (s_p.mf_bad) {
                cur = -1;
            } else if (lw_pl_playable(&pl, cur) && (data = lw_load(pl.e[cur].path, &n)) != NULL) {
                dec = lw_dec_open(data, n, err, sizeof err);
            } else {
                _snprintf_s(err, sizeof err, _TRUNCATE, lw_pl_playable(&pl, cur) ? "Datei nicht lesbar" :
                            "Eintrag nicht abspielbar (Typ %u)", cur >= 0 && cur < pl.n ? pl.e[cur].type : 0u);
            }
            if (dec) {
                int rate, ch;
                lw_dec_format(dec, &rate, &ch);
                if (vo.v && (vo.rate != rate || vo.ch != ch)) {
                    /* anderes Format: alte Stimme laeuft nicht aus, neues Lied beginnt sofort */
                    lw_voice_clear(&vo);
                }
                if (!vo.v && lw_voice_make(&vo, rate, ch, db, ucat, paused) != 0) {
                    lw_dec_close(dec);
                    dec = NULL;
                    _snprintf_s(err, sizeof err, _TRUNCATE, "Mischer-Stimme nicht verfuegbar");
                }
            }
            if (dec) {
                int rate, ch;
                lw_dec_format(dec, &rate, &ch);
                InterlockedIncrement(&s_p.songs);
                InterlockedExchange(&s_p.song, cur);
                InterlockedIncrement(&s_p.starts);
                need_open = 0;
                failrow = 0;
                HLOG_I("LOCWMA: Lied %d/%d '%s' %d Hz %d Kan. %.1f s%s", cur + 1, pl.n, pl.e[cur].path, rate, ch,
                       lw_dec_secs(dec), vo.paused ? " (pausiert)" : "");
            } else {
                InterlockedIncrement(&s_p.fails);
                if (cur >= 0 && strstr(err, "Media Foundation")) {
                    InterlockedExchange(&s_p.mf_bad, 1);
                }
                if (cur >= 0) {
                    HLOG_I("LOCWMA: Lied %d nicht abspielbar (%s) - stumm/naechstes", cur + 1, err);
                }
                if (++failrow >= (pl.n > 0 ? pl.n : 1) || cur < 0 || s_p.mf_bad ||
                    (cur = lw_pl_after(&pl, cur)) < 0) {
                    /* nichts abspielbar: stumm wie bisher, Cue endet */
                    lw_voice_clear(&vo);
                    run = 0;
                    InterlockedExchange(&s_p.song, -1);
                    InterlockedExchange(&s_p.active, 0);
                }
                continue;
            }
        }
        if (vo.v) {
            int r = lw_voice_feed(&vo, dec, &pend);
            if (r != 0) {
                if (r < 0) {
                    InterlockedIncrement(&s_p.fails);
                    HLOG_I("LOCWMA: Dekoderfehler in Lied %d - weiter mit dem naechsten", cur + 1);
                }
                lw_dec_close(dec);
                dec = NULL;
                cur = lw_pl_after(&pl, cur);
                if (cur >= 0) {
                    need_open = 1; /* naechstes Lied direkt anschliessen (offene Pakete spielen weiter) */
                }
            }
            if (!dec && !need_open && pend == 0) {
                /* Liste zu Ende (ohne Wiederholung) */
                lw_voice_clear(&vo);
                run = 0;
                InterlockedExchange(&s_p.song, -1);
                InterlockedExchange(&s_p.active, 0);
            }
        }
    }
}

static int lw_ensure_thread(void)
{
    static volatile LONG once;
    if (InterlockedCompareExchange(&once, 1, 0) == 0) {
        s_p.ev = CreateEventW(NULL, FALSE, FALSE, NULL);
        s_p.th = s_p.ev ? CreateThread(NULL, 256 * 1024, lw_thread, NULL, 0, NULL) : NULL;
        if (s_p.th) {
            SetThreadPriority(s_p.th, THREAD_PRIORITY_ABOVE_NORMAL);
        } else {
            HLOG_I("LOCWMA: Player-Thread nicht startbar - stumm");
        }
    }
    return s_p.th != NULL;
}

int lw_player_start(const lw_pl_t *p, int db100, int ucat, int paused, int auto_mode)
{
    int start;
    if (!p || p->n <= 0 || s_p.mf_bad || !lw_ensure_thread()) {
        return 0;
    }
    start = p->cur >= 0 ? p->cur : 0;
    AcquireSRWLockExclusive(&s_p.lk);
    s_p.pl = *p;
    s_p.jump = start;
    s_p.run = 1;
    s_p.auto_mode = auto_mode;
    s_p.db100 = db100;
    s_p.ucat = ucat;
    s_p.paused = paused;
    s_p.gen++;
    InterlockedExchange(&s_p.active, 1);
    ReleaseSRWLockExclusive(&s_p.lk);
    SetEvent(s_p.ev);
    return 1;
}

void lw_player_jump(const lw_pl_t *p)
{
    if (!s_p.th || !s_p.run || !p || p->cur < 0) {
        return;
    }
    AcquireSRWLockExclusive(&s_p.lk);
    s_p.pl = *p;
    s_p.jump = p->cur;
    s_p.gen++;
    InterlockedExchange(&s_p.active, 1);
    ReleaseSRWLockExclusive(&s_p.lk);
    SetEvent(s_p.ev);
}

void lw_player_stop(void)
{
    if (!s_p.th) {
        return;
    }
    AcquireSRWLockExclusive(&s_p.lk);
    if (s_p.run) {
        s_p.run = 0;
        s_p.gen++;
    }
    InterlockedExchange(&s_p.active, 0);
    ReleaseSRWLockExclusive(&s_p.lk);
    SetEvent(s_p.ev);
}

void lw_player_volume(int db100)
{
    AcquireSRWLockExclusive(&s_p.lk);
    s_p.db100 = db100;
    ReleaseSRWLockExclusive(&s_p.lk);
}

void lw_player_pause(int on)
{
    AcquireSRWLockExclusive(&s_p.lk);
    s_p.paused = on ? 1 : 0;
    ReleaseSRWLockExclusive(&s_p.lk);
    if (s_p.ev) {
        SetEvent(s_p.ev);
    }
}

int lw_player_active(void) { return s_p.active != 0; }
long lw_player_starts(void) { return s_p.starts; }
int lw_player_song(void) { return s_p.th ? (int)s_p.song : -1; }

void lw_player_stats(char *buf, int cap)
{
    _snprintf_s(buf, (size_t)cap, _TRUNCATE, "Liedstarts=%ld Lieder=%ld Fehler=%ld Pakete=%ld aktiv=%ld%s",
                s_p.starts, s_p.songs, s_p.fails, s_p.pkts, s_p.active, s_p.mf_bad ? " (Media Foundation fehlt)" : "");
}

void lw_player_shutdown(void) { lw_player_stop(); }
#endif
