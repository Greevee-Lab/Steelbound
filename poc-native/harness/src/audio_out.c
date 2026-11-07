/* audio_out.c - DirectSound-HLE: Stimmen, Mixer, Live-Ausgabe, WAV-Capture.
 * W4/A7. Spiel-Puffer/Streams/XWaveFile-Pakete werden hier als HLE-Stimmen
 * gefuehrt; der Mixer-Thread mischt nach Stereo/44100 (float), gibt per WinMM
 * aus und schreibt bei audio_capture=1 <out>/audio.wav (Header pro Chunk
 * aktualisiert, damit auch abgebrochene Laeufe ein gueltiges WAV haben).
 * Formate: PCM 8/16 Bit (mono/stereo/5.1), Xbox-ADPCM (Tag 105, IMA).
 * GetStatus ist ehrlich (STOPPED bis wirklich gespielt wird): DS-Init wartet
 * auf STOPPED (BERICHT M2 "Verworfen"), PLAYING-Vortaeuschung brickt Boot.
 * W68 SPLIT: hier Stimmen-Verwaltung + DS-API; Formate/Dekoder/FIFO in
 * audio_fmt.c, Mixer/Ausgabe/WAV in audio_mix.c (gemeinsam: audio_int.h). */
#include "audio_int.h"

/* ---------- Globals ---------- */
static hlock_t g_au_cs; /* W119 LOADLOCK: hlock (atomarer Owner, reentrant) */
audio_voice_t *g_voices;
static au32 g_next_id = 1;
HANDLE g_mix_thread;
HWAVEOUT g_wo;
int g_wo_ok; /* -1=fehlgeschlagen, 0=offen, 1=ok */
LONG g_wo_queued;
FILE *g_wav;
au32 g_wav_bytes;
char g_wav_path[600];
uint64_t g_mixed_total;
au32 g_stat_packets, g_stat_bytes, g_stat_drops, g_stat_voices;
au32 g_stat_bufplay, g_stat_mixbin, g_stat_freq, g_stat_wa_under;
static LONG g_stat_getpos; /* W22: GetCurrentPosition-Aufrufe (Main-Spin?) */
static LONG g_stat_unlock_mm_buf, g_stat_unlock_mm_str; /* W22: Unlock<>Lock */
static LONG g_stat_unlock_ptr_out; /* W22: Unlock-p1 ausserhalb lockbuf */
static uint64_t g_stat_clip; /* W22: Ring-Clip-Summe (Ueberlapp-Bytes) */
static au32 g_stat_buf, g_stat_str; /* erzeugte Puffer-/Stream-Stimmen */
float g_peak;
au32 g_stat_cut, g_stat_end; /* W89 */
au32 g_stat_stop; /* W95 */

/* PERF(PERF-TICK,W168) audio_mix_yield / DIAG audio_lock_diag: Der Mischer haelt au_lock fuer einen ganzen
 * 20-ms-Durchgang (alle Stimmen); Audio-Aufrufe des Spiels (Notify, SetBufferData, Volume ...) warteten darauf
 * im Spiel-Thread (W168_n1m1: 8 Aufrufe in 17 ms, Tick-Last 39,6 ms). Mit audio_mix_yield meldet ein wartender
 * Nicht-Mischer-Thread g_au_want; der Mischer gibt die Sperre nach der laufenden Stimme kurz ab. */
volatile LONG g_au_want;
DWORD g_au_mix_tid;
volatile LONGLONG g_aul_wait_q, g_aul_wait_max_q, g_aul_hold_q, g_aul_hold_max_q;
volatile LONG g_aul_wait_n, g_aul_hold_n, g_aul_yield_n;
void au_lock(void)
{
    if (g_cfg.audio_mix_yield || g_cfg.audio_lock_diag) {
        DWORD tid = GetCurrentThreadId();
        DWORD own = hlock_owner(&g_au_cs);
        if (tid != g_au_mix_tid && own != 0 && own != tid) {
            LARGE_INTEGER a, b;
            QueryPerformanceCounter(&a);
            if (g_cfg.audio_mix_yield) { InterlockedIncrement(&g_au_want); }
            hlock_enter(&g_au_cs);
            if (g_cfg.audio_mix_yield) { InterlockedDecrement(&g_au_want); }
            QueryPerformanceCounter(&b);
            g_aul_wait_q += b.QuadPart - a.QuadPart;
            if (b.QuadPart - a.QuadPart > g_aul_wait_max_q) { g_aul_wait_max_q = b.QuadPart - a.QuadPart; }
            g_aul_wait_n++;
            return;
        }
    }
    hlock_enter(&g_au_cs);
}

/* SUSPSAFE (W10): Owner-Tid von g_au_cs (0 = frei/uninit). Regionen sind
 * kurz und wartefrei (Stimmen-Listen; Sleep/Wait/WinMM stehen ausserhalb);
 * NtSuspendThread suspendiert kein Ziel, das eine Host-Sperre haelt. */
unsigned long audio_lock_owner_tid(void)
{
    return (unsigned long)hlock_owner(&g_au_cs);
}

/* W119 DIAG(LOADLOCK): voller Sperr-Zustand fuer krnl_lock_diag. */
const hlock_t *audio_hlock(void)
{
    return &g_au_cs;
}

void au_unlock(void)
{
    hlock_leave(&g_au_cs);
}

/* ---------- Stimmen-Verwaltung ---------- */
void *g_audio_stream_vtable[16];
void *g_audio_buffer_vtable[16];

/* Vorwaerts: Slot-Funktionen (unten definiert) */
static au32 __stdcall auv_addref(au32 v);
static au32 __stdcall auv_release(au32 v);
static au32 __stdcall auv_getinfo(au32 v, au32 pInfo);
static au32 __stdcall auv_getstatus(au32 v, au32 pStatus);
static au32 __stdcall auv_process(au32 v, au32 pIn, au32 pOut);
static au32 __stdcall auv_1arg(au32 v);
static au32 __stdcall auv_2arg(au32 v, au32 a);
static au32 __stdcall auv_3arg(au32 v, au32 a, au32 b);
static au32 __stdcall auv_4arg(au32 v, au32 a, au32 b, au32 c);

static void au_vtable_init(void)
{
    static int done;
    int i;
    if (done)
        return;
    done = 1;
    /* Stream-Layout wie Original @0x26F4A4 (argc aus RET-N vermessen):
     * 0 AddRef(1) 1 Release(1) 2 GetInfo(2) 3 GetStatus(2) 4 Process(3)
     * 5 Discontinuity(1) 6 Flush(1) 7..15 generisch. */
    g_audio_stream_vtable[0] = auv_addref;
    g_audio_stream_vtable[1] = auv_release;
    g_audio_stream_vtable[2] = auv_getinfo;
    g_audio_stream_vtable[3] = auv_getstatus;
    g_audio_stream_vtable[4] = auv_process;
    g_audio_stream_vtable[5] = auv_1arg;
    g_audio_stream_vtable[6] = auv_1arg;
    g_audio_stream_vtable[7] = auv_1arg;
    for (i = 8; i < 16; i++)
        g_audio_stream_vtable[i] = auv_1arg;
    for (i = 0; i < 16; i++)
        g_audio_buffer_vtable[i] = auv_1arg;
    g_audio_buffer_vtable[0] = auv_addref;
    g_audio_buffer_vtable[1] = auv_release;
}

int audio_is_voice(au32 p)
{
    au32 hdr[2];
    if (!p)
        return 0;
    if (!au_probe_read(p, hdr, sizeof hdr))
        return 0;
    if (hdr[1] != AUDIO_MAGIC)
        return 0;
    return hdr[0] == (au32)(uintptr_t)g_audio_stream_vtable ||
        hdr[0] == (au32)(uintptr_t)g_audio_buffer_vtable;
}

int audio_is_ds(au32 p)
{
    au32 hdr[2];
    if (!p)
        return 0;
    if (!au_probe_read(p, hdr, sizeof hdr))
        return 0;
    return hdr[1] == AUDIO_DS_MAGIC;
}

static audio_voice_t *au_lookup(au32 p)
{
    if (!audio_is_voice(p))
        return NULL;
    return (audio_voice_t *)(uintptr_t)p;
}

static au32 au_create(au32 kind, au32 desc, au32 retaddr)
{
    audio_voice_t *v;
    wfx_t wfx;
    au32 buflen = 0, flags = 0;
    au_vtable_init();
    au_parse_desc(kind, desc, &wfx, &buflen, &flags);
    v = (audio_voice_t *)calloc(1, sizeof *v);
    if (!v)
        return 0;
    v->vtable = kind == 2 ? (void *)g_audio_stream_vtable
                          : (void *)g_audio_buffer_vtable;
    v->magic = AUDIO_MAGIC;
    v->kind = kind;
    v->refcount = 1;
    v->gain = 1.0f;
    v->pan_l = v->pan_r = 1.0f;
    v->hr_gain = 1.0f;
    v->bin_gain[0] = v->bin_gain[1] = 1.0f; /* Stereo-Default: FL+FR */
    v->freq_step = 1.0f;
    v->create_ret = retaddr; /* W89 */
    v->min3d = 1.0f; /* W89 DS3D-Defaults (DS3D_DEFAULTMIN/MAXDISTANCE) */
    v->max3d = 1e9f;
    v->g3d = v->pan3l = v->pan3r = 1.0f;
    au_lock();
    v->id = g_next_id++;
    v->next = g_voices;
    g_voices = v;
    g_stat_voices++;
    if (kind == 1)
        g_stat_buf++;
    else
        g_stat_str++;
    au_unlock();
    { /* W102 DIAG: Mixbin-Zeiger im Desc (DSBUFFERDESC +0x10, DSSTREAMDESC +0x14) */
        au32 dd[6];
        if (au_probe_read(desc, dd, sizeof dd))
            v->desc_bins = kind == 2 ? dd[5] : dd[4];
    }
    if (wfx.rate) {
        v->fmt = wfx;
        v->have_fmt = 1;
        v->src_rate = wfx.rate;
    }
    v->buflen = buflen;
    if (kind == 1 && buflen) {
        v->lockbuf = (uint8_t *)calloc(1, buflen ? buflen : 65536);
        if (!v->lockbuf)
            v->buflen = 0; /* A7B: faul 64K in Lock (statt mismatch) */
    }
    HLOG_I("AUDIO v%u %s: fmt=%s ch=%u rate=%u bits=%u align=%u buflen=%u "
           "flags=0x%X ret=0x%X",
           v->id, kind == 2 ? "stream" : "buffer",
           v->have_fmt ? (wfx.tag == 105 ? "ADPCM" : "PCM") : "?",
           wfx.ch, wfx.rate, wfx.bits, wfx.align, buflen, flags, retaddr);
    return (au32)(uintptr_t)v;
}

au32 audio_buffer_create(au32 desc, au32 retaddr)
{
    return au_create(1, desc, retaddr);
}

au32 audio_stream_create(au32 desc, au32 retaddr)
{
    return au_create(2, desc, retaddr);
}

au32 audio_ds_create(void)
{
    audio_ds_t *o = (audio_ds_t *)calloc(1, sizeof *o);
    if (!o)
        return 0;
    o->magic = AUDIO_DS_MAGIC;
    o->refcount = 1;
    HLOG_I("AUDIO DirectSound-Objekt erzeugt (HLE)");
    return (au32)(uintptr_t)o;
}

void audio_addref(au32 v)
{
    audio_voice_t *vp = au_lookup(v);
    if (vp)
        InterlockedIncrement(&vp->refcount);
}

void audio_release(au32 v)
{
    audio_voice_t *vp = au_lookup(v);
    audio_voice_t **pp;
    if (!vp)
        return;
    if (InterlockedDecrement(&vp->refcount) > 0)
        return;
    au_lock();
    pp = &g_voices;
    while (*pp && *pp != vp)
        pp = &(*pp)->next;
    if (*pp)
        *pp = vp->next;
    vp->magic = 0; /* Poison: Spaetaufrufe -> fremd -> S_OK statt UAF */
    vp->vtable = NULL;
    if (vp->ninflight) /* W34: kein PENDING-Strand (Slots wuerden ewig ruhen) */
        au_complete_due(vp, 1, E_ABORT);
    au_unlock();
    HLOG_I("AUDIO v%u freigegeben (played=%llu)", vp->id,
           (unsigned long long)vp->played);
    free(vp->data);
    free(vp->fifo);
    free(vp->lockbuf);
    free(vp->dbg_shadow); /* W83 DIAG(BINKWRAP) */
    free(vp);
}

void audio_ds_release(au32 o)
{
    if (audio_is_ds(o)) {
        au32 z = 0;
        au_probe_write(o + 4, &z, 4); /* Magic poisonen (UAF-Schutz) */
        free((void *)(uintptr_t)o);
    }
}

static au32 au_src_to_data_frames(const audio_voice_t *vp, uint64_t src);

/* PERF(PERF-TICK,W168) audio_decode_par=N: grosse SetBufferData-Puffer (>= 512 ADPCM-Bloecke) auf N Teile
 * verteilen (Threadpool, der Aufrufer rechnet Teil 0 selbst und wartet). ADPCM-Bloecke sind unabhaengig (je Block
 * eigener Praediktor/Index), das Resampling haengt je Ausgabe-Frame nur von o ab -> bitgleiches Ergebnis. */
typedef struct {
    int kind;                 /* 0 = ADPCM, 1 = Resampling linear, 2 = Resampling naechster Nachbar */
    const uint8_t *src; au32 frame, b0, nb; int ch; float *dst; au32 got_part;  /* ADPCM */
    const float *tmp; float *rs; au32 o0, o1, got, want, end; double stp;       /* Resampling */
    volatile LONG *left; HANDLE ev;
} au_par_t;
static void au_par_run(au_par_t *t)
{
    if (t->kind == 0) {
        t->got_part = au_adpcm_decode(t->src + (size_t)t->b0 * t->frame, t->nb * t->frame, t->ch,
                                      t->dst + (size_t)t->b0 * 64u * 2u, t->nb * 64u);
    } else {
        au32 o;
        t->end = t->o1;
        for (o = t->o0; o < t->o1; o++) {
            au32 i0;
            if (t->kind == 1) {
                double p = (double)o * t->stp;
                au32 i1;
                float fr;
                i0 = (au32)p;
                if (i0 >= t->got) { t->end = o; break; }
                i1 = i0 + 1 < t->got ? i0 + 1 : i0;
                fr = (float)(p - (double)i0);
                t->rs[o * 2] = t->tmp[i0 * 2] + (t->tmp[i1 * 2] - t->tmp[i0 * 2]) * fr;
                t->rs[o * 2 + 1] = t->tmp[i0 * 2 + 1] + (t->tmp[i1 * 2 + 1] - t->tmp[i0 * 2 + 1]) * fr;
            } else {
                uint64_t p = (uint64_t)o * t->got / t->want;
                i0 = (au32)p;
                if (i0 >= t->got) { t->end = o; break; }
                t->rs[o * 2] = t->tmp[i0 * 2];
                t->rs[o * 2 + 1] = t->tmp[i0 * 2 + 1];
            }
        }
    }
}
static VOID CALLBACK au_par_cb(PTP_CALLBACK_INSTANCE inst, PVOID ctx)
{
    au_par_t *t = (au_par_t *)ctx;
    (void)inst;
    au_par_run(t);
    if (InterlockedDecrement(t->left) == 0) { SetEvent(t->ev); }
}
/* t[0..n-1] ausfuehren (t[0] im Aufrufer). 0 = Threadpool nicht verfuegbar -> alles seriell gerechnet. */
static void au_par_exec(au_par_t *t, int n)
{
    HANDLE ev = n > 1 ? CreateEventA(NULL, TRUE, FALSE, NULL) : NULL; /* je Aufruf (mehrere Xbox-Threads moeglich) */
    volatile LONG left;
    int k;
    left = n - 1;
    for (k = 1; k < n; k++) {
        t[k].left = &left; t[k].ev = ev;
        if (!ev || !TrySubmitThreadpoolCallback(au_par_cb, &t[k], NULL)) {
            au_par_run(&t[k]);
            if (InterlockedDecrement(&left) == 0 && ev) { SetEvent(ev); }
        }
    }
    au_par_run(&t[0]);
    if (ev) {
        if (n > 1) { WaitForSingleObject(ev, INFINITE); }
        CloseHandle(ev);
    }
}

/* Quell-Bytes -> Stereo-float (ADPCM oder PCM) als eigener Puffer (malloc, *n Frames, Aufrufer gibt frei).
 * PERF(PERF-TICK,W168): Kern von au_decode_append, damit SetBufferData ohne FIFO-Umweg (Modulo je Sample,
 * Verdopplungs-Kopien, Rueckkopie) dieselben Werte bekommt. *got = Rueckgabewert wie bisher (ADPCM: Frames
 * vor dem Resampling), *used = verbrauchte Quell-Bytes. NULL = nichts dekodiert. */
static float *au_decode_buf(const audio_voice_t *v, const uint8_t *src, au32 nbytes, au32 *n, au32 *got_out,
                            au32 *used, int par)
{
    au32 got = 0;
    *n = 0; *got_out = 0; *used = 0;
    if (!v->have_fmt || !src || !nbytes)
        return NULL;
    if (v->fmt.tag == 105) {
        au32 nblocks = nbytes / (36u * v->fmt.ch);
        float *tmp;
        if (!nblocks)
            return NULL;
        tmp = (float *)malloc((size_t)nblocks * 64 * 2 * sizeof(float));
        if (!tmp)
            return NULL;
        if (par > 1 && nblocks >= 512u) { /* PERF(PERF-TICK,W168) audio_decode_par */
            au_par_t t[8];
            int k, np = par > 8 ? 8 : par;
            au32 per = (nblocks + (au32)np - 1u) / (au32)np, b = 0;
            memset(t, 0, sizeof t);
            for (k = 0; k < np; k++) {
                t[k].kind = 0; t[k].src = src; t[k].frame = 36u * v->fmt.ch; t[k].ch = v->fmt.ch; t[k].dst = tmp;
                t[k].b0 = b; t[k].nb = (b + per <= nblocks) ? per : nblocks - b; b += t[k].nb;
            }
            au_par_exec(t, np);
            got = 0;
            for (k = 0; k < np; k++) { got += t[k].got_part; }
        } else
        got = au_adpcm_decode(src, nblocks * 36u * v->fmt.ch, v->fmt.ch, tmp,
                              nblocks * 64);
        *used = nblocks * 36u * v->fmt.ch;
        *got_out = got;
        /* ADPCM-Rate fast immer 44100; Resampling bei Bedarf */
        if (v->src_rate != MIXRATE && got) {
            au32 want = got * MIXRATE / v->src_rate + 16;
            float *rs = (float *)malloc((size_t)want * 2 * sizeof(float));
            if (rs && par > 1 && got >= 512u * 64u) { /* PERF(PERF-TICK,W168) audio_decode_par: Ausgabebereiche teilen */
                au_par_t t[8];
                int k, np = par > 8 ? 8 : par;
                au32 per = (want + (au32)np - 1u) / (au32)np, o = 0, end = want;
                memset(t, 0, sizeof t);
                for (k = 0; k < np; k++) {
                    t[k].kind = g_cfg.audio_adpcm_lerp ? 1 : 2; t[k].tmp = tmp; t[k].rs = rs; t[k].got = got; t[k].want = want;
                    t[k].stp = (double)v->src_rate / (double)MIXRATE;
                    t[k].o0 = o; t[k].o1 = (o + per <= want) ? o + per : want; o = t[k].o1;
                }
                au_par_exec(t, np);
                for (k = 0; k < np; k++) { if (t[k].end < t[k].o1) { end = t[k].end; break; } } /* erster Abbruch (i0 monoton) */
                free(tmp);
                *n = end;
                return rs;
            }
            if (rs) {
                au32 o;
                if (g_cfg.audio_adpcm_lerp) {
                    /* W89: linear interpolieren (wie au_pcm_convert). Alt:
                     * naechster Nachbar -> 11025->44100 = 4-fach-Treppe
                     * (Spiegelfrequenzen, metallischer Klang). Schritt =
                     * Quellrate/MIXRATE (gleiche Laenge wie bisher). */
                    double stp = (double)v->src_rate / (double)MIXRATE;
                    for (o = 0; o < want; o++) {
                        double p = (double)o * stp;
                        au32 i0 = (au32)p;
                        au32 i1;
                        float fr;
                        if (i0 >= got)
                            break;
                        i1 = i0 + 1 < got ? i0 + 1 : i0;
                        fr = (float)(p - (double)i0);
                        rs[o * 2] = tmp[i0 * 2] + (tmp[i1 * 2] - tmp[i0 * 2]) * fr;
                        rs[o * 2 + 1] = tmp[i0 * 2 + 1] +
                            (tmp[i1 * 2 + 1] - tmp[i0 * 2 + 1]) * fr;
                    }
                } else {
                for (o = 0; o < want; o++) {
                    uint64_t p = (uint64_t)o * got / want;
                    au32 i0 = (au32)p;
                    if (i0 >= got)
                        break;
                    rs[o * 2] = tmp[i0 * 2];
                    rs[o * 2 + 1] = tmp[i0 * 2 + 1];
                }
                }
                free(tmp);
                *n = o;
                return rs;
            }
        }
        *n = got;
        return tmp;
    } else {
        au32 sframe = v->fmt.align;
        au32 nsrc = sframe ? nbytes / sframe : 0;
        au32 want;
        float *tmp;
        if (!nsrc)
            return NULL;
        want = nsrc * MIXRATE / (v->src_rate ? v->src_rate : MIXRATE) + 16;
        tmp = (float *)malloc((size_t)want * 2 * sizeof(float));
        if (!tmp)
            return NULL;
        got = au_pcm_convert(src, nsrc * sframe, v->fmt.ch, v->fmt.bits,
                             v->src_rate ? v->src_rate : v->fmt.rate, tmp,
                             want);
        *used = nsrc * sframe;
        *got_out = got;
        *n = got;
        return tmp;
    }
}

/* Quell-Bytes -> Stereo-float (ADPCM oder PCM), an FIFO anhaengen. */
static au32 au_decode_append(audio_voice_t *v, const uint8_t *src, au32 nbytes)
{
    au32 n = 0, got = 0, used = 0;
    float *buf = au_decode_buf(v, src, nbytes, &n, &got, &used, 0);
    if (!buf)
        return 0;
    au_fifo_append(v, buf, n);
    free(buf);
    v->src_consumed += used;
    return got;
}

/* W22-DIAG: Feed-Seite pro Stimme zaehlen (muss unter au_lock laufen). */
static void au_feed_diag(audio_voice_t *v, const uint8_t *tmp, au32 b)
{
    au32 sil = 0, k;
    if (v->fmt.bits == 8) {
        for (k = 0; k < b; k++)
            if (tmp[k] == 0x80)
                sil++;
    } else if (v->fmt.bits == 16) {
        for (k = 0; k + 1 < b; k += 2)
            if (tmp[k] == 0 && tmp[k + 1] == 0)
                sil += 2;
    }
    v->dbg_unlock_n++;
    v->dbg_unlock_bytes += b;
    v->dbg_sil_bytes += sil;
}

/* ---------- W89 DIAG(SFX) ---------- */
static LONG g_sfx_lines;
static LARGE_INTEGER g_sfx_qpf, g_sfx_qpc0;
/* Muss NICHT unter au_lock laufen (liest nur, Werte koennen um einen Frame
 * veraltet sein); unter au_lock ebenfalls erlaubt (CS ist rekursiv). */
void au_sfx_trace(audio_voice_t *v, const char *what, au32 a, au32 b, au32 c,
                  au32 ret)
{
    LARGE_INTEGER now;
    LONG n;
    if (g_cfg.audio_sfx_diag <= 0 || !v || v->kind != 1 ||
        v->create_ret == 0x2435FCu)
        return;
    n = InterlockedIncrement(&g_sfx_lines);
    if (n > g_cfg.audio_sfx_diag)
        return;
    if (!g_sfx_qpf.QuadPart) {
        QueryPerformanceFrequency(&g_sfx_qpf);
        QueryPerformanceCounter(&g_sfx_qpc0);
    }
    QueryPerformanceCounter(&now);
    HLOG_I("DIAG(SFX) t=%.4f v%u %s a=0x%X b=0x%X c=0x%X ret=0x%X | pl=%d ph=%u/%u "
           "g=%.3f hr=%.3f pan=%.2f/%.2f st=%.3f rate=%u g3d=%.3f d=%.0f",
           (double)(now.QuadPart - g_sfx_qpc0.QuadPart) /
               (double)(g_sfx_qpf.QuadPart ? g_sfx_qpf.QuadPart : 1),
           v->id, what, a, b, c, ret, v->playing, v->playhead, v->datalen,
           v->gain, v->hr_gain, v->pan_l, v->pan_r, v->freq_step, v->src_rate,
           v->g3d, v->dbg_d3);
}

void audio_sfx_evt(au32 v, const char *what, au32 a, au32 b, au32 c, au32 ret)
{
    audio_voice_t *vp;
    if (g_cfg.audio_sfx_diag <= 0)
        return;
    vp = au_lookup(v);
    if (!vp)
        return;
    if (strcmp(what, "GetStatus") == 0) { /* Spin-Schleifen: nur Wechsel */
        if (vp->dbg_last_status == a + 1)
            return;
        vp->dbg_last_status = a + 1;
    } else if (strncmp(what, "Set", 3) == 0 && strcmp(what, "SetBufferData") &&
               strcmp(what, "SetCurPos")) {
        /* Setter pro Frame (Filter/Headroom/Position): nur Wertwechsel */
        au32 i, slot = 8;
        for (i = 0; i < 8; i++) {
            if (vp->dbg_set_what[i] == what) {
                slot = i;
                break;
            }
            if (!vp->dbg_set_what[i] && slot == 8)
                slot = i;
        }
        if (slot < 8) {
            if (vp->dbg_set_what[slot] == what && vp->dbg_set_val[slot] == a &&
                vp->dbg_set_val2[slot] == b)
                return;
            vp->dbg_set_what[slot] = what;
            vp->dbg_set_val[slot] = a;
            vp->dbg_set_val2[slot] = b;
        }
    }
    au_sfx_trace(vp, what, a, b, c, ret);
}

/* ---------- W89 DS3D-Fassade (Logik in audio_3d.c) ---------- */
void audio_3d_voice_pos(au32 v, au32 x, au32 y, au32 z)
{
    audio_voice_t *vp = au_lookup(v);
    if (vp)
        au3d_voice_pos(vp, x, y, z);
}
void audio_3d_voice_allparams(au32 v, au32 pParams)
{
    audio_voice_t *vp = au_lookup(v);
    au32 d[16];
    if (vp && au_probe_read(pParams, d, sizeof d))
        au3d_voice_allparams(vp, d);
}
void audio_3d_voice_dist(au32 v, int which, au32 f)
{
    audio_voice_t *vp = au_lookup(v);
    if (vp)
        au3d_voice_dist(vp, which ? 0xFFFFFFFFu : f, which ? f : 0xFFFFFFFFu);
}
void audio_3d_listener_pos(au32 x, au32 y, au32 z)
{
    au3d_listener_pos(x, y, z);
}
void audio_3d_listener_orient(au32 fx, au32 fy, au32 fz, au32 tx, au32 ty, au32 tz)
{
    au3d_listener_orient(fx, fy, fz, tx, ty, tz);
}
void audio_3d_rolloff(au32 r)
{
    au3d_rolloff(r);
}

/* ---------- Datenpfad ---------- */
volatile LONGLONG g_aul_sb_q, g_aul_sb_max_q; /* DIAG(PERF-TICK,W168) audio_lock_diag: Dauer SetBufferData */
volatile LONG g_aul_sb_n;
static void audio_set_buffer_data_body(au32 v, au32 pdata, au32 bytes);
void audio_set_buffer_data(au32 v, au32 pdata, au32 bytes)
{
    LARGE_INTEGER a, b;
    if (!g_cfg.audio_lock_diag) { audio_set_buffer_data_body(v, pdata, bytes); return; }
    QueryPerformanceCounter(&a);
    audio_set_buffer_data_body(v, pdata, bytes);
    QueryPerformanceCounter(&b);
    g_aul_sb_q += b.QuadPart - a.QuadPart;
    if (b.QuadPart - a.QuadPart > g_aul_sb_max_q) { g_aul_sb_max_q = b.QuadPart - a.QuadPart; }
    g_aul_sb_n++;
}
static void audio_set_buffer_data_body(au32 v, au32 pdata, au32 bytes)
{
    audio_voice_t *vp = au_lookup(v);
    uint8_t *tmp;
    static au32 logged;
    if (!vp)
        return;
    if (!pdata || !bytes || bytes > 64 * 1024 * 1024) {
        HLOG_W("AUDIO v%u SetBufferData: ungueltig p=0x%X n=%u", vp->id, pdata,
               bytes);
        return;
    }
    tmp = (uint8_t *)malloc(bytes);
    if (!tmp)
        return;
    if (!au_probe_read(pdata, tmp, bytes)) {
        HLOG_W("AUDIO v%u SetBufferData: Speicher 0x%X nicht lesbar", vp->id,
               pdata);
        free(tmp);
        return;
    }
    /* dekodieren in Scratch-Stimme (eigene FIFO, kein Eingriff in vp) */
    {
        audio_voice_t scratch;
        au32 i;
        float *flat = NULL; /* PERF(PERF-TICK,W168) audio_setbuf_fast: fertiger Puffer statt FIFO */
        au32 flat_n = 0;
        memset(&scratch, 0, sizeof scratch);
        scratch.fmt = vp->fmt;
        scratch.have_fmt = vp->have_fmt;
        scratch.src_rate = vp->src_rate ? vp->src_rate : vp->fmt.rate;
        if (g_cfg.audio_setbuf_fast) {
            au32 got = 0, used = 0;
            flat = au_decode_buf(&scratch, tmp, bytes, &flat_n, &got, &used, g_cfg.audio_decode_par);
            if (flat && flat_n) {
                /* wie au_fifo_append in eine leere FIFO: Kapazitaet 16384 Frames, verdoppelt; ueber
                 * FIFO_HARD_CAP wird alles verworfen (ein Drop gezaehlt) */
                au32 ncap = 16384;
                while (ncap < flat_n) { ncap *= 2; }
                if ((uint64_t)ncap * 8 > FIFO_HARD_CAP) { g_stat_drops++; flat_n = 0; }
            }
            if (flat && !flat_n) { free(flat); flat = NULL; }
            if (g_cfg.audio_setbuf_fast == 2) { /* DIAG(PERF-TICK,W168): Pruefmodus - alter Weg zaehlt, Vergleich bitgenau */
                static au32 nchk, ndiff, nframes;
                au32 k, bad = 0;
                au_decode_append(&scratch, tmp, bytes);
                if ((flat ? flat_n : 0u) != scratch.fifo_n) { bad = 1; }
                for (k = 0; !bad && k < scratch.fifo_n; k++) {
                    au32 sidx = (scratch.fifo_rd + k) % scratch.fifo_cap;
                    if (memcmp(&flat[k * 2], &scratch.fifo[sidx * 2], 2 * sizeof(float)) != 0) { bad = 1; }
                }
                nchk++; nframes += scratch.fifo_n; ndiff += bad;
                if (bad || (nchk % 200u) == 1u) {
                    HLOG_I("AUDIO setbuf_fast Pruefung: puffer=%u frames=%u abweichend=%u (zuletzt n=%u/%u)", nchk, nframes, ndiff,
                           flat ? flat_n : 0u, scratch.fifo_n);
                }
                free(flat); flat = NULL; flat_n = 0;
            }
        } else {
            au_decode_append(&scratch, tmp, bytes);
        }
        au_lock();
        if (vp->kind == 1 && vp->playing && vp->data &&
            vp->playhead < vp->datalen)
            g_stat_cut++; /* W89: laufender Ton durch neue Daten ersetzt */
        free(vp->data);
        vp->data = NULL;
        vp->datalen = 0;
        vp->play_frac = 0.0f;
        vp->freq_step = 1.0f; /* neu dekodiert @src_rate */
        if (g_cfg.audio_setbuf_len && vp->kind == 1 && bytes > 0 &&
            bytes < 64 * 1024 * 1024 && vp->buflen != bytes) {
            /* W22: Xbox bindet SetBufferData-Speicher als Puffer (mit Groesse).
             * Bink rechnet Ringe mit dieser Groesse (53K); Lazy-64K brach den
             * Cursor-Wrap (falsche Deltas -> Silence-Fills + Spin). Lock-Allok
             * folgt faul in audio_lock. */
            free(vp->lockbuf);
            vp->lockbuf = NULL;
            free(vp->dbg_shadow); /* W83 DIAG(BINKWRAP) */
            vp->dbg_shadow = NULL;
            vp->buflen = bytes;
            vp->ring_front = 0; /* W22: Ring-Clip neu (Puffertausch) */
            vp->ring_contig = 0; /* W83 */
        }
        vp->dec_rate = scratch.src_rate ? scratch.src_rate : MIXRATE;
        vp->playhead = 0;
        if (flat) { /* PERF(PERF-TICK,W168): Puffer uebernehmen (gleiche Werte wie die FIFO-Kopie) */
            vp->data = flat;
            vp->datalen = flat_n;
            flat = NULL;
        } else
        if (scratch.fifo_n) {
            vp->data =
                (float *)malloc((size_t)scratch.fifo_n * 2 * sizeof(float));
            if (vp->data) {
                for (i = 0; i < scratch.fifo_n; i++) {
                    au32 s = (scratch.fifo_rd + i) % scratch.fifo_cap;
                    vp->data[i * 2] = scratch.fifo[s * 2];
                    vp->data[i * 2 + 1] = scratch.fifo[s * 2 + 1];
                }
                vp->datalen = scratch.fifo_n;
            }
        }
        g_stat_bytes += bytes;
        vp->dbg_setbuf_p = pdata; /* W83 DIAG(BINKWRAP) */
        au_unlock();
        free(scratch.fifo);
    }
    if (hlog_throttle(&logged) || vp->datalen == 0)
        HLOG_I("AUDIO v%u SetBufferData: %u Bytes -> %u Frames (fmt %s)",
               vp->id, bytes, vp->datalen,
               vp->have_fmt ? (vp->fmt.tag == 105 ? "ADPCM" : "PCM") : "?");
    free(tmp);
}

void audio_play(au32 v, au32 flags)
{
    audio_voice_t *vp = au_lookup(v);
    static au32 logged;
    if (!vp)
        return;
    au_lock();
    if (vp->kind == 1)
        g_stat_bufplay++;
    if (g_cfg.audio_loop_fix) /* W89: Xbox: nur DSBPLAY_LOOPING loopt */
        vp->looping = (flags & DSBPLAY_LOOPING) ? 1 : 0;
    else if (flags & DSBPLAY_LOOPING)
        vp->looping = 1;
    if (vp->data && vp->playhead >= vp->datalen && !vp->fifo_n)
        vp->playhead = 0; /* Re-Play nach Ende startet neu */
    if (!vp->playing) { /* W102: Filterzustand bei Neustart leeren */
        vp->svf_lp[0] = vp->svf_lp[1] = 0.0f;
        vp->svf_bp[0] = vp->svf_bp[1] = 0.0f;
    }
    vp->playing = 1;
    vp->paused = 0;
    vp->active = 1;
    vp->play_mix = g_mixed_total; /* W95 DIAG(SFXN) */
    vp->play_flags = flags;
    au_unlock();
    au_ensure_thread();
    if (hlog_throttle(&logged))
        HLOG_I("AUDIO v%u Play (flags=0x%X)%s", vp->id, flags,
               vp->looping ? " LOOP" : "");
}

void audio_stop(au32 v)
{
    audio_voice_t *vp = au_lookup(v);
    if (!vp)
        return;
    au_lock();
    if (vp->kind == 1 && vp->playing)
        g_stat_stop++; /* W95 */
    vp->playing = 0;
    au_unlock();
}

au32 audio_get_status(au32 v)
{
    audio_voice_t *vp = au_lookup(v);
    au32 st = 0;
    if (!vp)
        return 0;
    au_lock();
    if (vp->kind == 2) {
        /* Stream: PLAYING sobald aktiv (Datenfluss), bis Stop/Pause/Flush */
        if (vp->active && !vp->paused)
            st |= DSBSTATUS_PLAYING;
        if (vp->looping)
            st |= DSBSTATUS_LOOPING;
    } else {
        if (vp->playing && !vp->paused)
            st |= DSBSTATUS_PLAYING;
        if ((st & DSBSTATUS_PLAYING) && vp->looping)
            st |= DSBSTATUS_LOOPING;
    }
    au_unlock();
    return st;
}

void audio_get_pos(au32 v, au32 pPlay, au32 pWrite)
{
    audio_voice_t *vp = au_lookup(v);
    au32 play = 0, wr = 0;
    InterlockedIncrement(&g_stat_getpos);
    if (vp) {
        uint64_t pos;
        au_lock();
        pos = au_src_pos(vp);
        /* Puffer: Position im Ring (buflen bzw. statische Laenge) */
        if (vp->kind == 1) {
            uint64_t len = vp->buflen;
            if (!len && vp->have_fmt && vp->datalen) {
                len = vp->fmt.tag == 105
                    ? (uint64_t)(vp->datalen / 64) * 36u * vp->fmt.ch
                    : (uint64_t)vp->datalen * MIXRATE / MIXRATE *
                        vp->fmt.align;
            }
            if (len)
                pos %= len;
        }
        play = (au32)pos;
        wr = (au32)(pos + 8192);
        if (vp->kind == 1 && vp->buflen)
            wr %= vp->buflen;
        vp->dbg_last_play = play;
        vp->dbg_last_wr = wr;
        au_unlock();
    }
    au_probe_write(pPlay, &play, 4);
    au_probe_write(pWrite, &wr, 4);
}

void audio_set_pos(au32 v, au32 byteOff)
{
    audio_voice_t *vp = au_lookup(v);
    if (!vp)
        return;
    au_lock();
    /* Playhead aus Byte-Offset ableiten (statische Daten) */
    if (vp->have_fmt && vp->data && vp->datalen) {
        uint64_t f;
        if (vp->fmt.tag == 105) {
            f = (uint64_t)byteOff / (36u * vp->fmt.ch) * 64;
            if (g_cfg.audio_loop_fix) /* W89: Quell- -> Daten-Frames */
                f = au_src_to_data_frames(vp, f);
        } else if (vp->fmt.align)
            f = (uint64_t)byteOff / vp->fmt.align * MIXRATE /
                (vp->src_rate ? vp->src_rate : MIXRATE);
        else
            f = 0;
        vp->playhead = f > vp->datalen ? vp->datalen : (au32)f;
    vp->play_frac = 0.0f;
    }
    vp->played = 0;
    vp->ring_front = 0; /* W22: Ring-Clip neu aufsetzen (Cursor-Reset) */
    vp->ring_contig = 0; /* W83 */
    vp->last_pump_pos = 0;
    au_unlock();
}

au32 audio_lock(au32 v, au32 off, au32 bytes, au32 pp1, au32 pb1, au32 pp2,
                au32 pb2, au32 flags)
{
    audio_voice_t *vp = au_lookup(v);
    au32 z = 0;
    (void)flags;
    if (!vp)
        return E_FAIL;
    au_lock();
    if (!vp->lockbuf) {
        /* A7B: Allokation MATCHT buflen (vorher fix 64K bei beliebigem
         * buflen -> Lock log ueber das Ende -> Bink-Heap-Tod). */
        vp->lockbuf = (uint8_t *)calloc(1, vp->buflen ? vp->buflen : 65536);
        if (!vp->lockbuf)
            vp->buflen = 0; /* A7B: calloc fehlgeschlagen -> Nullen melden */
        else if (!vp->buflen)
            vp->buflen = 65536;
    }
    if (vp->buflen && off < vp->buflen) {
        au32 n1 = bytes;
        au32 p1 = (au32)(uintptr_t)(vp->lockbuf + off);
        au32 n2 = 0, p2 = 0;
        if (off + bytes > vp->buflen) {
            n1 = vp->buflen - off;
            /* A7B: n1+n2 nie groesser als buflen (DSound-Vertrag) */
            n2 = off + bytes - vp->buflen;
            if (n2 > vp->buflen - n1)
                n2 = vp->buflen - n1;
            p2 = (au32)(uintptr_t)vp->lockbuf;
        }
        au_probe_write(pp1, &p1, 4);
        au_probe_write(pb1, &n1, 4);
        au_probe_write(pp2, &p2, 4);
        au_probe_write(pb2, &n2, 4);
        vp->lock_n1 = n1; /* W22-DIAG: Unlock-Vergleich */
        vp->lock_n2 = n2;
        vp->lock_off = off; /* W22: Ring-Frontier-Basis */
    } else {
        au_probe_write(pb1, &z, 4);
        au_probe_write(pb2, &z, 4);
        vp->lock_n1 = vp->lock_n2 = 0; /* W22-DIAG: kein Bereich gewaehrt */
    }
    { /* W22-DIAG: Lock-Kadenz (QPC/TID/off/Bytes/Cursor-Snapshot) */
        LARGE_INTEGER lt;
        QueryPerformanceCounter(&lt);
        if (!vp->dbg_lock_n) {
            vp->dbg_lock_qpc0 = (uint64_t)lt.QuadPart;
            vp->dbg_lock_tid0 = GetCurrentThreadId();
        }
        vp->dbg_lock_qpc1 = (uint64_t)lt.QuadPart;
        vp->dbg_lock_bytes += bytes;
        if (vp->dbg_lock_hist_n < 8 ||
            ((vp->dbg_lock_n % 128) == 0 &&
             vp->dbg_lock_hist_n < 40)) {
            au32 h = vp->dbg_lock_hist_n++;
            vp->dbg_lock_hist[h].qpc = (uint64_t)lt.QuadPart;
            vp->dbg_lock_hist[h].tid = GetCurrentThreadId();
            vp->dbg_lock_hist[h].off = off;
            vp->dbg_lock_hist[h].bytes = bytes;
            vp->dbg_lock_hist[h].grant = vp->lock_n1 + vp->lock_n2;
            vp->dbg_lock_hist[h].play = vp->dbg_last_play;
            vp->dbg_lock_hist[h].wr = vp->dbg_last_wr;
        }
        vp->dbg_lock_n++;
    }
    au_unlock();
    return S_OK;
}

/* W83 RING-OVERWRITE: Ueberlappt ein Unlock bereits angehaengte Ring-Bytes,
 * gewinnt wie im Xbox-Ring die spaetere Schreibung - fuer alle Frames, die
 * der Mixer noch nicht gespielt hat (FIFO-Ende). Ursache (DIAG(BINKSEQ),
 * W83_d2): Bink lockt am Ringende ein umbrechendes Fenster, beschreibt nur
 * Teil 1 und gibt Teil 2 (noch Inhalt der Vorrunde) mit frei; den Rest
 * schreibt es erst im naechsten Lock ab Offset 0. Der Clip (W22) behielt
 * die alten Bytes -> alle 0,6 s 10-60 ms Ton einer Ringrunde zuvor.
 * Nur PCM 1:1 (Quellrate = Mixrate, Bink 44100/16): Byte <-> FIFO-Frame.
 * src = ueberlappende Bytes ab Ring-Position ao, n = ihre Anzahl,
 * ov = ring_front - ao (Abstand zum Append-Ende). Muss unter au_lock laufen. */
static uint64_t g_stat_ovw; /* ersetzte Frames (Summe) */
static void au_ring_overwrite(audio_voice_t *vp, const uint8_t *src, au32 n,
                              uint64_t ov)
{
    static au32 logged;
    au32 al = vp->fmt.align, nf, back, j, got, done = 0;
    float *buf;
    if (vp->fmt.tag != 1 || !al || vp->fmt.bits != 16 ||
        al != (au32)vp->fmt.ch * 2u ||
        (vp->src_rate ? vp->src_rate : vp->fmt.rate) != MIXRATE ||
        !vp->fifo || !vp->fifo_cap)
        return;
    nf = n / al;
    if (ov / al > 0xFFFFFFFFu)
        return;
    back = (au32)(ov / al); /* Frames vom FIFO-Ende bis zur Position ao */
    if (!nf || back > vp->fifo_cap)
        return;
    buf = (float *)malloc((size_t)(nf + 16) * 2 * sizeof(float));
    if (!buf)
        return;
    got = au_pcm_convert(src, nf * al, vp->fmt.ch, 16, MIXRATE, buf, nf + 16);
    for (j = 0; j < got && j < nf; j++) {
        au32 dist = back - j; /* Abstand vom Schreibende (>= 1) */
        au32 pos;
        if (dist == 0 || dist > vp->fifo_n)
            continue; /* schon gespielt (oder nie angehaengt) */
        pos = (vp->fifo_wr + vp->fifo_cap - dist) % vp->fifo_cap;
        vp->fifo[pos * 2] = buf[j * 2];
        vp->fifo[pos * 2 + 1] = buf[j * 2 + 1];
        done++;
    }
    free(buf);
    g_stat_ovw += done;
    if (done && hlog_throttle(&logged))
        HLOG_I("AUDIO v%u Ring-Ueberschreiben: %u/%u Frames ersetzt (Abstand %u, W83)",
               vp->id, done, nf, back);
}

/* W83 DIAG(BINKWRAP): Hat das Spiel die per Lock gewaehrten Bereiche
 * wirklich beschrieben? Vergleicht Lock-Puffer mit dem Schatten nach dem
 * letzten Unlock (geaendert = beschrieben) und mit dem SetBufferData-
 * Spielspeicher (Xbox: Lock-Zeiger zeigen dort hinein). Gedeckelt: die
 * ersten 24 Wrap-Unlocks (b2>0) + Summenzeile alle 256 Unlocks. */
static void au_wrap_diag(audio_voice_t *vp, au32 p1, au32 b1, au32 p2, au32 b2)
{
    static LONG nwrap, nall, w_un2, w_game2, n_un1, n_game1;
    uint8_t *g = NULL;
    au32 lo = vp->lock_off;
    int ch1, ch2 = -1, gm1 = -1, gm2 = -1;
    LONG k;
    if (vp->kind != 1 || !vp->lockbuf || !vp->buflen || !b1 ||
        lo + b1 > vp->buflen || b2 > vp->buflen)
        return;
    if (!vp->dbg_shadow) {
        vp->dbg_shadow = (uint8_t *)malloc(vp->buflen);
        if (!vp->dbg_shadow)
            return;
        memcpy(vp->dbg_shadow, vp->lockbuf, vp->buflen);
        return;
    }
    ch1 = memcmp(vp->lockbuf + lo, vp->dbg_shadow + lo, b1) != 0;
    if (b2)
        ch2 = memcmp(vp->lockbuf, vp->dbg_shadow, b2) != 0;
    if (vp->dbg_setbuf_p) {
        g = (uint8_t *)malloc(vp->buflen);
        if (g && au_probe_read(vp->dbg_setbuf_p, g, vp->buflen)) {
            gm1 = memcmp(g + lo, vp->lockbuf + lo, b1) == 0;
            if (b2)
                gm2 = memcmp(g, vp->lockbuf, b2) == 0;
        }
    }
    k = InterlockedIncrement(&nall);
    { /* DIAG(BINKSEQ): wo hat das Spiel seit dem letzten Unlock geschrieben?
       * (Bereiche geaenderter Bytes im ganzen Ring), erste Stimme, Unlocks
       * 120..200 dieser Stimme. */
        static au32 seq_id, seq_n;
        if (!seq_id)
            seq_id = vp->id;
        if (vp->id == seq_id && ++seq_n >= 120 && seq_n < 200) {
            char rs[200];
            int rn = 0, nr = 0;
            au32 i = 0;
            rs[0] = 0;
            while (i < vp->buflen && nr < 4) {
                au32 a;
                while (i < vp->buflen && vp->lockbuf[i] == vp->dbg_shadow[i])
                    i++;
                if (i >= vp->buflen)
                    break;
                a = i;
                while (i < vp->buflen && (vp->lockbuf[i] != vp->dbg_shadow[i] ||
                       (i + 8 < vp->buflen && memcmp(vp->lockbuf + i, vp->dbg_shadow + i, 8))))
                    i++;
                rn += _snprintf_s(rs + rn, sizeof rs - rn, _TRUNCATE, " [%u,%u)", a, i);
                nr++;
            }
            HLOG_I("DIAG(BINKSEQ) v%u u#%u off=%u b1=%u b2=%u neu:%s",
                   vp->id, seq_n, lo, b1, b2, rs);
        }
    }
    if (!ch1)
        n_un1++;
    if (gm1 == 0)
        n_game1++;
    if (b2) {
        LONG w = InterlockedIncrement(&nwrap);
        if (ch2 == 0)
            w_un2++;
        if (gm2 == 0)
            w_game2++;
        if (w <= 24) {
            int gch2 = -1;
            if (g && gm2 >= 0)
                gch2 = memcmp(g, vp->dbg_shadow, b2) != 0;
            HLOG_I("DIAG(BINKWRAP) v%u #%ld off=%u b1=%u b2=%u p1=0x%X p2=0x%X "
                   "lock2_neu=%d spiel2==lock2=%d spiel2_neu=%d lock1_neu=%d spiel1==lock1=%d "
                   "setbuf=0x%X lockbuf=0x%X tid=%lu",
                   vp->id, w, lo, b1, b2, p1, p2, ch2, gm2, gch2, ch1, gm1,
                   vp->dbg_setbuf_p, (au32)(uintptr_t)vp->lockbuf,
                   GetCurrentThreadId());
        }
    }
    if ((k % 256) == 0)
        HLOG_I("DIAG(BINKWRAP) summe: unlocks=%ld wraps=%ld wrap_lock2_unveraendert=%ld "
               "wrap_spiel2!=lock2=%ld lock1_unveraendert=%ld spiel1!=lock1=%ld",
               k, nwrap, w_un2, w_game2, n_un1, n_game1);
    memcpy(vp->dbg_shadow, vp->lockbuf, vp->buflen);
    free(g);
}

/* Unlock: geschriebene Bereiche als PCM dekodieren + an FIFO. Spielt das
 * Spiel (Bink) per Lock/Unlock-Streaming, landet hier echtes PCM. */
au32 audio_unlock(au32 v, au32 p1, au32 b1, au32 p2, au32 b2)
{
    audio_voice_t *vp = au_lookup(v);
    static au32 logged;
    if (!vp)
        return S_OK;
    /* Bereiche SEH-gesichert kopieren (Spiel kann Fremd-Zeiger liefern) */
    if (vp->have_fmt) {
        au32 i;
        /* W22-DIAG: Unlock-Args vs. Lock (Partial-Write-Verdacht). Uebergibt
         * Bink Lock-(p1,b1) (dann evtl. M<N stale) oder eigene (Over-Read?)? */
        if (b1 != vp->lock_n1 || b2 != vp->lock_n2) {
            LONG *cnt = vp->kind == 1 ? &g_stat_unlock_mm_buf
                                      : &g_stat_unlock_mm_str;
            if (InterlockedIncrement(cnt) == 1)
                HLOG_I("DIAG(UNLOCKMM) v%u %s: lock=(%u,%u) unlock=(%u,%u) p1=0x%X",
                       vp->id, vp->kind == 1 ? "buf" : "str", vp->lock_n1,
                       vp->lock_n2, b1, b2, p1);
        }
        if (p1 && vp->lockbuf &&
            (p1 < (au32)(uintptr_t)vp->lockbuf ||
             p1 >= (au32)(uintptr_t)vp->lockbuf + vp->buflen)) {
            if (InterlockedIncrement(&g_stat_unlock_ptr_out) == 1)
                HLOG_I("DIAG(UNLOCKPTR) v%u: p1=0x%X ausserhalb lockbuf=0x%X+%u",
                       vp->id, p1, (au32)(uintptr_t)vp->lockbuf, vp->buflen);
        }
        for (i = 0; i < 2; i++) {
            au32 p = i ? p2 : p1;
            au32 b = i ? b2 : b1;
            uint8_t *tmp;
            if (!p || !b || b > 64 * 1024 * 1024)
                continue;
            tmp = (uint8_t *)malloc(b);
            if (!tmp)
                continue;
            if (au_probe_read(p, tmp, b)) {
                au32 skip = 0;
                int tracked = 0; /* W83: Ring-Byte <-> FIFO-Frame bekannt */
                au_lock();
                if (g_cfg.audio_ring_clip && vp->kind == 1 && vp->buflen &&
                    b1 == vp->lock_n1 && b2 == vp->lock_n2 &&
                    vp->lock_n1 + vp->lock_n2 > 0) {
                    au32 roff = i ? 0 : vp->lock_off;
                    au32 al = vp->fmt.align ? vp->fmt.align : 1;
                    uint64_t ao = vp->ring_front -
                        (vp->ring_front % vp->buflen) + roff;
                    while (ao + vp->buflen <= vp->ring_front)
                        ao += vp->buflen;
                    tracked = 1;
                    if (ao > vp->ring_front)
                        vp->ring_contig = 0; /* W83: Luecke im Ring */
                    if (ao < vp->ring_front) {
                        uint64_t ov = vp->ring_front - ao;
                        skip = ov > b ? b : (au32)ov;
                        skip &= (au32) ~(uint64_t)(al - 1);
                        if (skip && g_cfg.audio_ring_overwrite &&
                            ov <= vp->ring_contig)
                            au_ring_overwrite(vp, tmp, skip, ov);
                    }
                    if (skip && !vp->dbg_clip_bytes)
                        HLOG_I("DIAG(RINGCLIP) v%u: erster Clip +%u/%u",
                               vp->id, skip, b);
                    if (ao + b > vp->ring_front)
                        vp->ring_front = ao + b;
                    vp->dbg_clip_bytes += skip;
                    g_stat_clip += skip;
                }
                au_decode_append(vp, tmp + skip, b - skip);
                if (tracked)
                    vp->ring_contig += b - skip;
                else
                    vp->ring_contig = 0;
                au_feed_diag(vp, tmp + skip, b - skip);
                vp->active = 1;
                g_stat_bytes += b - skip;
                au_unlock();
            }
            free(tmp);
        }
    }
    if (g_cfg.audio_wrap_diag)
        au_wrap_diag(vp, p1, b1, p2, b2);
    if (hlog_throttle(&logged))
        HLOG_I("AUDIO v%u Unlock: +%u+%u Bytes (fifo=%u)", vp->id, b1, b2,
               vp->fifo_n);
    return S_OK;
}

/* XMEDIAINFO: { flags, inputSize, outputSize, maxLookahead } */
void audio_get_info(au32 v, au32 pInfo)
{
    audio_voice_t *vp = au_lookup(v);
    au32 info[4] = { 0, 16384, 0, 0 };
    if (vp && vp->have_fmt && vp->fmt.tag == 105)
        info[1] = 36u * vp->fmt.ch * 32; /* 32 Bloecke */
    au_probe_write(pInfo, info, sizeof info);
}

/* XMEDIAPACKET: { pvBuffer, dwMaxSize, pdwCompleted, pdwStatus, hEvent,
 * pContext }. Verbraucht In- und Out-Paket, markiert SUCCESS + Event. */
static void au_consume_packet(audio_voice_t *v, au32 pkt, const char *tag)
{
    au32 f[6];
    au32 done, st;
    if (!pkt)
        return;
    if (!au_probe_read(pkt, f, sizeof f))
        return;
    /* W34 (audio_process_async, nur Streams): Xbox-Vollzug - Daten sofort an
     * die FIFO (nie abweisen: 3 Spiel-Slots <= MaxAttachedPackets), Status
     * PENDING, Vollzug (SUCCESS + Event) erst bei Echtzeit-Konsum durch den
     * Mixer. So findet die Spiel-Paketpumpe (0x109BF0) ruhende Slots
     * (alle PENDING -> return) statt endlos zu pumpen; der Audio-Thread
     * erreicht 0x109CC0, das die Drain-Flags +0x60 loescht. */
    if (g_cfg.audio_process_async && v->kind == 2 && f[0] && f[1] &&
        v->have_fmt) {
        uint8_t *tmp = (uint8_t *)malloc(f[1]);
        HANDLE hev = f[4] ? (HANDLE)krnl_event_host(f[4]) : NULL;
        if (tmp) {
            if (au_probe_read(f[0], tmp, f[1])) {
                au_lock();
                au_decode_append(v, tmp, f[1]);
                v->active = 1;
                g_stat_packets++;
                g_stat_bytes += f[1];
                if (v->ninflight < 8) {
                    audio_inflight_t *r = &v->inflight[v->ninflight++];
                    r->pdwCompleted = f[2];
                    r->pdwStatus = f[3];
                    r->hev = hev;
                    r->done_size = f[1];
                    r->frames_end = v->pc_done + v->fifo_n;
                    au_unlock();
                    st = E_PENDING;
                    au_probe_write(f[3], &st, 4);
                } else {
                    /* Ueberlauf (Spiel: <=3, darf nicht passieren): sync. */
                    au_unlock();
                    done = f[1];
                    st = XMP_SUCCESS;
                    au_probe_write(f[2], &done, 4);
                    au_probe_write(f[3], &st, 4);
                    if (hev)
                        SetEvent(hev);
                }
            } else {
                /* Bytes unlesbar: sync wie bisher (kein PENDING). */
                done = f[1];
                st = XMP_SUCCESS;
                au_probe_write(f[2], &done, 4);
                au_probe_write(f[3], &st, 4);
                if (hev)
                    SetEvent(hev);
            }
            free(tmp);
            return;
        }
        /* malloc-Fehler: unten sync wie bisher. */
    }
    if (f[0] && f[1] && v->have_fmt) {
        uint8_t *tmp = (uint8_t *)malloc(f[1]);
        if (tmp) {
            if (au_probe_read(f[0], tmp, f[1])) {
                au_lock();
                au_decode_append(v, tmp, f[1]);
                v->active = 1;
                g_stat_packets++;
                g_stat_bytes += f[1];
                au_unlock();
            }
            free(tmp);
        }
    }
    done = f[1];
    st = XMP_SUCCESS;
    au_probe_write(f[2], &done, 4);
    au_probe_write(f[3], &st, 4);
    if (f[4]) {
        HANDLE hev = (HANDLE)krnl_event_host(f[4]);
        if (hev)
            SetEvent(hev);
        /* Sonst: kein gemapptes Event -> nicht signalisieren (rohe Xbox-
         * Handles sind Nummern und duerfen nie an SetEvent). */
    }
    (void)tag;
}

void audio_process(au32 v, au32 pInPkt, au32 pOutPkt)
{
    audio_voice_t *vp = au_lookup(v);
    static au32 logged;
    int spin = 0;
    /* (audio_pause_cap revertiert: t2 ruft Process nie, 93M->75M = Rauschen) */
    if (!vp)
        return;
    /* W34: Mit Async-Vollzug nimmt Process immer an (Xbox: 3 Slots <=
     * MaxAttachedPackets; Pumpe ruht bei 3x PENDING). Die alte
     * Sleep(10)x50-Backpressure (~0,5 s/Paket -> Audio-Thread erreicht
     * 0x109CC0 nie -> 5,1-s-Ladepause) nur noch mit Schalter aus. */
    if (!g_cfg.audio_process_async) {
        /* Backpressure: Spiel nicht unbegrenzt puffern lassen (1,5 s), aber nie
         * haengen (max ~0,5 s warten, dann trotzdem annehmen). */
        while (spin < 50) {
            au32 n;
            au_lock();
            n = vp->fifo_n;
            au_unlock();
            if (n < FIFO_CAP_FRAMES)
                break;
            Sleep(10);
            spin++;
        }
    }
    au_consume_packet(vp, pInPkt, "in");
    au_consume_packet(vp, pOutPkt, "out");
    au_ensure_thread();
    if (hlog_throttle(&logged))
        HLOG_T("AUDIO v%u Process (fifo=%u)", vp->id, vp->fifo_n);
}

/* ---------- Setter ---------- */
void audio_set_volume(au32 v, int db100)
{
    audio_voice_t *vp = au_lookup(v);
    static au32 logged;
    float g;
    if (!vp)
        return;
    if (db100 > 0)
        db100 = 0;
    if (db100 < -10000)
        db100 = -10000;
    g = (float)pow(10.0, (db100 / 100.0) / 20.0);
    au_lock();
    vp->gain = g;
    au_unlock();
    if (hlog_throttle(&logged))
        HLOG_I("AUDIO v%u Volume: %d/100 dB (gain=%.3f)", vp->id, db100, g);
}

void audio_set_format(au32 v, au32 pwfx)
{
    audio_voice_t *vp = au_lookup(v);
    wfx_t w;
    if (!vp)
        return;
    if (!au_probe_read(pwfx, &w, sizeof w) || !au_wfx_ok(&w)) {
        HLOG_W("AUDIO v%u SetFormat: ungueltig (pwfx=0x%X)", vp->id, pwfx);
        return;
    }
    au_lock();
    vp->fmt = w;
    vp->have_fmt = 1;
    vp->src_rate = w.rate;
    au_unlock();
    HLOG_I("AUDIO v%u SetFormat: %s ch=%u rate=%u bits=%u", vp->id,
           w.tag == 105 ? "ADPCM" : "PCM", w.ch, w.rate, w.bits);
}

void audio_set_frequency(au32 v, au32 hz)
{
    audio_voice_t *vp = au_lookup(v);
    if (!vp)
        return;
    if (hz >= 4000 && hz <= 192000) {
        au_lock();
        vp->src_rate = hz;
        if (vp->dec_rate) { /* A7B: Schritt relativ zur Dekodier-Rate */
            float st = (float)hz / (float)vp->dec_rate;
            if (st < 0.0625f)
                st = 0.0625f;
            if (st > 16.0f)
                st = 16.0f;
            vp->freq_step = st;
        }
        g_stat_freq++;
        au_unlock();
    }
}

void audio_set_notify(au32 v, au32 count, au32 pNotifies)
{
    audio_voice_t *vp = au_lookup(v);
    au32 i;
    if (!vp)
        return;
    au_lock();
    vp->nnotify = 0;
    vp->last_pump_pos = 0;
    for (i = 0; i < count && i < 8; i++) {
        au32 nn[2];
        if (!au_probe_read(pNotifies + i * 8, nn, sizeof nn))
            break;
        vp->notifies[vp->nnotify].off = nn[0];
        vp->notifies[vp->nnotify].xhev = nn[1];
        vp->nnotify++;
    }
    au_unlock();
    HLOG_I("AUDIO v%u Notify: %u Positionen", vp->id, vp->nnotify);
}

/* W89 (audio_loop_fix): Quell-Samples -> Frames der dekodierten Daten. Die
 * Daten liegen auf MIXRATE resampelt vor (Dekodier-Rate dec_rate), Loop-/
 * Positionsangaben des Spiels sind Quell-Bytes -> ohne Umrechnung spielte
 * ein 11025-Hz-Ton nur das erste Viertel (W89_a2: Loop 5056 von 20240). */
static au32 au_src_to_data_frames(const audio_voice_t *vp, uint64_t src)
{
    au32 r = vp->dec_rate ? vp->dec_rate
                          : (vp->src_rate ? vp->src_rate : MIXRATE);
    uint64_t f = src * MIXRATE / r;
    return f > 0xFFFFFFFFu ? 0xFFFFFFFFu : (au32)f;
}

void audio_set_loop(au32 v, au32 start, au32 len)
{
    audio_voice_t *vp = au_lookup(v);
    if (!vp)
        return;
    au_lock();
    if (vp->have_fmt && vp->fmt.align) {
        vp->loop_start = start / vp->fmt.align;
        vp->loop_len = len / vp->fmt.align;
        if (vp->fmt.tag == 105) {
            vp->loop_start = start / (36u * vp->fmt.ch) * 64;
            vp->loop_len = len / (36u * vp->fmt.ch) * 64;
        }
        if (g_cfg.audio_loop_fix) {
            /* Xbox: SetLoopRegion legt nur die Region fest, geloopt wird
             * erst mit Play(DSBPLAY_LOOPING). Alt: jede Region = Schleife ->
             * das Spiel setzt sie bei JEDEM Ton (0x107C71) -> Einmal-Toene
             * (Schuesse, Treffer) loopten ewig (sfx_zuende=0, Clipping). */
            vp->loop_start = au_src_to_data_frames(vp, vp->loop_start);
            vp->loop_len = au_src_to_data_frames(vp, vp->loop_len);
        } else {
            vp->looping = vp->loop_len > 0;
        }
    }
    au_unlock();
}

void audio_set_play_region(au32 v, au32 start, au32 len)
{
    (void)start;
    (void)len;
    audio_set_pos(v, start);
}

void audio_pause(au32 v, au32 pause)
{
    audio_voice_t *vp = au_lookup(v);
    if (!vp)
        return;
    au_lock();
    vp->paused = pause ? 1 : 0;
    if (!vp->paused)
        vp->playing = 1;
    au_unlock();
}

void audio_flush(au32 v)
{
    audio_voice_t *vp = au_lookup(v);
    if (!vp)
        return;
    au_lock();
    vp->fifo_rd = vp->fifo_wr = vp->fifo_n = 0;
    vp->active = 0;
    if (vp->ninflight) /* W34: wartende Pakete als FLUSHED (Xbox) loesen */
        au_complete_due(vp, 1, E_ABORT);
    au_unlock();
}

void audio_duplicate(au32 v, au32 ppOut)
{
    au_probe_write(ppOut, &v, 4);
    audio_addref(v);
}

/* DSOUNDCAPS: erstes DWORD = Groesse; Rest defensiv (nur innerhalb Groesse). */
void audio_get_caps(au32 o, au32 pCaps)
{
    au32 size = 0;
    (void)o;
    if (!au_probe_read(pCaps, &size, 4) || size > 1024)
        return;
    {
        uint8_t *z = (uint8_t *)calloc(1, size);
        if (!z)
            return;
        memcpy(z, &size, 4);
        /* DSOUNDCAPS: +4 flags, +8 maxSecondarySampleRate, ... */
        if (size >= 12) {
            au32 r = 44100;
            memcpy(z + 8, &r, 4);
        }
        au_probe_write(pCaps, z, size);
        free(z);
    }
}

void audio_dowork(void)
{
    /* Notification-Pumpe laeuft im Mixer-Thread; hier nur sicherstellen,
     * dass er existiert (falls DoWork vor erstem Play kommt: nein, erst bei
     * Bedarf — kein Thread ohne Audio). */
    (void)0;
}

/* ---------- A7B: Pan (MixBins), Headroom, Pitch ---------- */
/* Xbox routet Stimmen per MixBins auf Lautsprecher (kein SetPan):
 * X_DSMIXBINS { dwCount, lpPairs }, Paar { dwMixBin, lVolume }.
 * Bins: 0 FL, 1 FR, 2 C, 3 LFE, 4 BL, 5 BR (Cxbx XbDSoundTypes.h).
 * lVolume = DSBVOLUME (Hundertstel-dB, 0 = max), wie SetVolume.
 * Wir falten auf Stereo-Pan: L = FL + .707*C + .5*LFE + .5*BL etc. */
#define AUBIN_FL 0
#define AUBIN_FR 1
#define AUBIN_C 2
#define AUBIN_LFE 3
#define AUBIN_BL 4
#define AUBIN_BR 5

static float au_db100_to_gain(int db100)
{
    if (db100 > 0)
        db100 = 0;
    if (db100 < -10000)
        return 0.0f;
    return (float)pow(10.0, (db100 / 100.0) / 20.0);
}

static void au_mixbins_apply(audio_voice_t *vp, au32 pMixBins, int full)
{
    au32 hdr[2];
    au32 i, n;
    static au32 logged;
    if (!au_probe_read(pMixBins, hdr, sizeof hdr))
        return;
    n = hdr[0];
    if (n > 8 || !hdr[1])
        return;
    au_lock();
    if (full) {
        for (i = 0; i < 8; i++)
            vp->bin_gain[i] = 0.0f;
    }
    for (i = 0; i < n; i++) {
        au32 pair[2];
        au32 bin;
        if (!au_probe_read(hdr[1] + i * 8, pair, sizeof pair))
            break;
        bin = pair[0];
        if (bin < 8)
            vp->bin_gain[bin] = au_db100_to_gain((int)pair[1]);
    }
    vp->pan_l = vp->bin_gain[AUBIN_FL] + 0.7071f * vp->bin_gain[AUBIN_C] +
        0.5f * vp->bin_gain[AUBIN_LFE] + 0.5f * vp->bin_gain[AUBIN_BL];
    vp->pan_r = vp->bin_gain[AUBIN_FR] + 0.7071f * vp->bin_gain[AUBIN_C] +
        0.5f * vp->bin_gain[AUBIN_LFE] + 0.5f * vp->bin_gain[AUBIN_BR];
    g_stat_mixbin++;
    { static au32 nmb; if (vp->id >= 130 || ++nmb <= 6) HLOG_I("DIAG(MIXBINS) v%u: %u Paare pan L=%.2f R=%.2f", vp->id, n, vp->pan_l, vp->pan_r); }
    au_unlock();
    if (hlog_throttle(&logged))
        HLOG_I("AUDIO v%u MixBins: %u Paare -> pan L=%.2f R=%.2f", vp->id, n,
               vp->pan_l, vp->pan_r);
}

void audio_set_mixbins(au32 v, au32 pMixBins)
{
    audio_voice_t *vp = au_lookup(v);
    if (vp)
        au_mixbins_apply(vp, pMixBins, 1);
}

void audio_set_mixbin_volumes(au32 v, au32 pMixBins)
{
    audio_voice_t *vp = au_lookup(v);
    if (vp)
        au_mixbins_apply(vp, pMixBins, 0);
}

/* Headroom: zusaetzliche Daempfung 0..10000 (Hundertstel-dB, Cxbx-Skala),
 * addiert sich zur Lautstaerke (wird mit gain multipliziert). */
void audio_set_headroom(au32 v, au32 headroom)
{
    audio_voice_t *vp = au_lookup(v);
    static au32 logged;
    if (!vp)
        return;
    if (headroom > 10000)
        return; /* DSERR_INVALIDPARAM-Semantik: ignorieren */
    au_lock();
    vp->hr_gain = au_db100_to_gain(-(int)headroom);
    au_unlock();
    if (hlog_throttle(&logged))
        HLOG_I("AUDIO v%u Headroom: %u/100 dB (gain=%.3f)", vp->id, headroom,
               vp->hr_gain);
}

/* Xbox-Pitch -> Frequenz: f = 2^(pitch/4096) * 48000 (Cxbx converter.hpp). */
void audio_set_pitch(au32 v, int pitch)
{
    audio_voice_t *vp = au_lookup(v);
    double f;
    static au32 logged;
    if (!vp)
        return;
    if (g_cfg.audio_pitch_range) {
        /* W89: Das Spiel nutzt fuer 11025-Hz-Toene ~-8576..-9216 (11,3/10,1
         * kHz); die alte Klemme -8192 (12 kHz) spielte sie bis 19 % zu hoch.
         * Weite Klemme, die Frequenz wird unten auf 4..192 kHz begrenzt. */
        if (pitch < -98304)
            pitch = -98304;
        if (pitch > 32767)
            pitch = 32767;
    } else {
        if (pitch < -8192)
            pitch = -8192;
        if (pitch > 8192)
            pitch = 8192;
    }
    f = exp((pitch / 4096.0) * log(2.0)) * 48000.0;
    if (hlog_throttle(&logged))
        HLOG_I("AUDIO v%u Pitch: %d -> %.0f Hz", vp->id, pitch, f);
    if (f >= 4000.0 && f <= 192000.0)
        audio_set_frequency(v, (au32)f);
}

/* ---------- W102 AUDIO-TUT: Voice-Filter / I3DL2 / EG ----------
 * Das Spiel (0x108530 -> Wrapper 0x107AB0 -> SetFilter 0x21F4A8, nur wenn
 * [obj+0x66]==1) setzt DSFILTERDESC {Mode=1 (DLS2), Q=0, c0, c1, 0, 0}:
 *   c0 = 4096*log2(2*sin(pi*fc/48000)) & 0xFFFF   (fc < 30 Hz -> 0x8000,
 *        fc > 8000 Hz -> 0), also f = 2^(s16(c0)/4096) einer Chamberlin-SVF
 *        bei 48 kHz (MCPX-Voice-Processor);
 *   c1 = 32768 * 10^(-res_dB/20) (res 0..22,5 dB, DLS2) = Daempfung q = 1/Q
 *        in 1.15 (Klemme 0xFFFF).
 * Hier: fc zurueckrechnen und f fuer MIXRATE neu bestimmen. */
void au_filter_coeffs(audio_voice_t *v)
{
    int c0 = (int)(int16_t)(v->flt_c[0] & 0xFFFFu);
    double f48 = pow(2.0, c0 / 4096.0), fc, f;
    double q = (double)(v->flt_c[1] & 0xFFFFu) / 32768.0;
    if (f48 > 1.999)
        f48 = 1.999;
    fc = 48000.0 / 3.14159265358979 * asin(f48 / 2.0);
    if (fc > 0.45 * MIXRATE)
        fc = 0.45 * MIXRATE;
    f = 2.0 * sin(3.14159265358979 * fc / MIXRATE);
    if (q < 0.05)
        q = 0.05; /* sehr hohe Resonanz begrenzen (Stabilitaet) */
    if (q > 2.0)
        q = 2.0;
    v->flt_f = (float)f;
    v->flt_qd = (float)q;
}

void audio_set_filter(au32 v, au32 pDesc)
{
    audio_voice_t *vp = au_lookup(v);
    au32 d[6];
    static au32 logged;
    if (!vp || !au_probe_read(pDesc, d, sizeof d))
        return;
    au_lock();
    if (vp->flt_mode != d[0] || vp->flt_c[0] != d[2] || vp->flt_c[1] != d[3] ||
        !vp->flt_n) {
        vp->flt_mode = d[0];
        vp->flt_q = d[1];
        memcpy(vp->flt_c, d + 2, sizeof vp->flt_c);
        au_filter_coeffs(vp);
    }
    vp->flt_n++;
    au_unlock();
    if (g_cfg.audio_level_diag && hlog_throttle(&logged))
        HLOG_I("DIAG(FLT) v%u mode=%u q=%u c=0x%X/0x%X/0x%X/0x%X -> f=%.4f qd=%.3f",
               vp->id, d[0], d[1], d[2], d[3], d[4], d[5], vp->flt_f, vp->flt_qd);
}

/* DSI3DL2BUFFER: lDirect, lDirectHF, lRoom, lRoomHF, flRoomRolloff,
 * Obstruction{lHFLevel, flLFRatio}, Occlusion{lHFLevel, flLFRatio}. */
void audio_set_i3dl2(au32 v, au32 pParams)
{
    audio_voice_t *vp = au_lookup(v);
    au32 d[4];
    if (!vp || !au_probe_read(pParams, d, sizeof d))
        return;
    vp->i3d_direct = (int)d[0];
    vp->i3d_directhf = (int)d[1];
    vp->i3d_room = (int)d[2];
    vp->i3d_roomhf = (int)d[3];
    vp->has_i3dl2 = 1;
}

void audio_note_eg(au32 v, au32 pDesc)
{
    audio_voice_t *vp = au_lookup(v);
    if (!vp)
        return;
    vp->eg_p = pDesc;
    vp->eg_n++;
}

void audio_log_stats(void)
{
    HLOG_I("AUDIO stats: stimmen=%u pakete=%u bytes=%u drops=%u "
           "gemischt=%llu frames peak=%.2f getpos=%ld unlockmm=%ld/%ld ptrout=%ld clip=%llu",
           g_stat_voices, g_stat_packets, g_stat_bytes, g_stat_drops,
           (unsigned long long)g_mixed_total, g_peak, g_stat_getpos,
           g_stat_unlock_mm_buf, g_stat_unlock_mm_str, g_stat_unlock_ptr_out,
           (unsigned long long)g_stat_clip);
    HLOG_I("AUDIO stats2: buf=%u str=%u bufplay=%u mixbin=%u freq=%u wa_under=%u",
           g_stat_buf, g_stat_str, g_stat_bufplay, g_stat_mixbin, g_stat_freq,
           g_stat_wa_under);
    HLOG_I("AUDIO stats3 (W89): sfx_abgeschnitten=%u sfx_zuende=%u ithis_fix=%d",
           g_stat_cut, g_stat_end, g_cfg.audio_ithis_fix);
    au3d_log_stats();
    if (g_cfg.audio_sfx_diag > 0) { /* W89 DIAG(SFX): Energie-Rangliste */
        audio_voice_t *top[8];
        int nt = 0, k;
        audio_voice_t *v;
        double tot = 0;
        au_lock();
        for (v = g_voices; v; v = v->next) {
            tot += v->dbg_energy;
            if (v->dbg_energy <= 0)
                continue;
            if (nt < 8)
                top[nt++] = v;
            else if (v->dbg_energy > top[7]->dbg_energy)
                top[7] = v;
            else
                continue;
            for (k = nt - 1; k > 0 && top[k - 1]->dbg_energy < top[k]->dbg_energy; k--) {
                audio_voice_t *t = top[k];
                top[k] = top[k - 1];
                top[k - 1] = t;
            }
        }
        HLOG_I("DIAG(SFXE) gesamt=%.1f (RMS-Anteil ueber 60 s: %.0f)", tot,
               sqrt(tot / (2.0 * MIXRATE * 60.0)) * 32767.0);
        for (k = 0; k < nt; k++) {
            v = top[k];
            HLOG_I("DIAG(SFXE) #%d v%u kind=%u ret=0x%X anteil=%.1f%% rms=%.0f frames=%llu "
                   "loop=%d pl=%d g=%.3f hr=%.3f pan=%.2f/%.2f len=%u rate=%u g3d=%.3f d=%.0f",
                   k, v->id, v->kind, v->create_ret,
                   tot > 0 ? 100.0 * v->dbg_energy / tot : 0.0,
                   v->dbg_eframes ? sqrt(v->dbg_energy / (2.0 * v->dbg_eframes)) * 32767.0 : 0.0,
                   (unsigned long long)v->dbg_eframes, v->looping, v->playing,
                   v->gain, v->hr_gain, v->pan_l, v->pan_r, v->datalen, v->src_rate,
                   v->g3d, v->dbg_d3);
        }
        for (v = g_voices; v; v = v->next) {
            v->dbg_energy = 0;
            v->dbg_eframes = 0;
        }
        au_unlock();
    }
}

/* ---------- Vtable-Slots (feste Signaturen!) ---------- */
static au32 __stdcall auv_addref(au32 v)
{
    audio_addref(v);
    return S_OK;
}

static au32 __stdcall auv_release(au32 v)
{
    audio_release(v);
    return S_OK;
}

static au32 __stdcall auv_getinfo(au32 v, au32 pInfo)
{
    if (!audio_is_voice(v))
        return S_OK;
    audio_get_info(v, pInfo);
    return S_OK;
}

static au32 __stdcall auv_getstatus(au32 v, au32 pStatus)
{
    au32 st = audio_get_status(v);
    au_probe_write(pStatus, &st, 4);
    return S_OK;
}

static au32 __stdcall auv_process(au32 v, au32 pIn, au32 pOut)
{
    if (!audio_is_voice(v))
        return S_OK;
    audio_process(v, pIn, pOut);
    return S_OK;
}

static au32 __stdcall auv_1arg(au32 v)
{
    (void)v;
    return S_OK;
}

static au32 __stdcall auv_2arg(au32 v, au32 a)
{
    (void)v;
    (void)a;
    return S_OK;
}

static au32 __stdcall auv_3arg(au32 v, au32 a, au32 b)
{
    (void)v;
    (void)a;
    (void)b;
    return S_OK;
}

static au32 __stdcall auv_4arg(au32 v, au32 a, au32 b, au32 c)
{
    (void)v;
    (void)a;
    (void)b;
    (void)c;
    return S_OK;
}
