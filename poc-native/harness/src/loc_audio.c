/* loc_audio.c - W298 LOC-AUDIO: XACT-HLE fuer Line of Contact (siehe loc_audio.h).
 *
 * Spielseite (Ghidra, nur gelesen; Fundstellen in docs/loc/LOC_BOOT.md §16):
 *  - Init FUN_00088150: DirectSoundCreate, XAudioDownloadEffectsImage (bleibt
 *    Stub), XACTEngineCreate. FUN_00088240: 26 Wave-Banks im Speicher (e00..,
 *    te0.., tj0..), Streaming memory.xwb/stream.xwb. FUN_0008bea0: bgm.xwb,
 *    sinkou.xwb (Streaming), miniloop.xwb, Bank.xsb, 7 2D-Quellen (MixBins
 *    0x1EF7D8..) + 64 3D-Quellen, RegisterNotification fuer jeden Cue
 *    (wType 1, Flags 0x8002), Listener.
 *  - Abspielen FUN_00087AC0: PlayEx(bank, {Flags, Cue, Quelle, 0}, ppCue);
 *    Flags 0 = neuer Cue (Handle nach *ppCue), 1 = ohne Handle (AutoRelease),
 *    8 = PrepareEx (vorbereitet), 4 + Cue -1 = vorbereiteten *ppCue starten.
 *  - Slot-Tabelle 0x5765B0 (0xC0 Slots): ein Slot wird erst frei, wenn
 *    GetNotification (FUN_0008B6A0, Puffer 40 B auf dem Stack) einen Eintrag
 *    {wType, Bank, Cue-Index} liefert - ohne Stopp-Meldungen laeuft die
 *    Tabelle voll. Rueckgabe 0 = Meldung, sonst keine.
 *  - SoundSource_GetStatus: 1 = Cues laufen (Funk-Warteschlange 0x8A2A0 prueft 0).
 * Annahmen (W, im Bericht): XSB-Eintrag +4 = Daempfung in 1/10 dB, +6 =
 * Tonhoehe in 1/4096 Oktave, +0xA = Kategorie (= Index fuer SetMasterVolume/
 * GlobalPause); Schleife = Schleifen-Flag des Wave-Bank-Eintrags. */
#include "loc_rollback.h" /* W330: LOC_RB_PREWRITE */
#include "loc_audio.h"
#include "loc_xact_fmt.h"
#include "loc_stub_va.h"
#include "audio_out.h"
#include "audio_vol.h" /* W303 AUDIO-VOLUME: XSB-Kategorie -> Nutzerkategorie */
#include "hcfg.h"
#include "hlog.h"
#include "hlock.h"
#include "kernel_hle.h"

#include <windows.h>
#include <intrin.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;

#define LA_E_FAIL 0x80004005u
#define LA_DSBPLAY_LOOPING 1u
#define LA_DSBSTATUS_PLAYING 1u
#define LA_MAX_WB 40
#define LA_MAX_SB 4
#define LA_MAX_SRC 96
#define LA_MAX_CUE 256
#define LA_MAX_POOL 384
#define LA_MAX_NOTE 512
#define LA_MAX_CAT 8
#define LA_NOTE_CUES 4096
#define LA_MAX_WAVE_BYTES (40u * 1024u * 1024u)
#define LA_MIN3D 8.0f /* W: XSB-3D-Parameter unbekannt; Spiel rechnet in ~Metern (x0,01x0,5) */
#define LA_MAX3D 1000.0f
#define LA_DOWNMIX_DB (-770) /* 5.1-Wellen (Stream.xwb): Mischer-Downmix summiert bis x2,9 -> ITU-Norm 1/2,414 */
#define LA_3D_HEADROOM (-600) /* W: 3D-Cues -6 dB (sonst im Gefecht 0,1-2,8 % Uebersteuerung, W298_A2) */
#define LA_STREAM_SECS 20.0 /* laenger: Stream statt Puffer (Mischer-Puffer max. 16 MB = 47 s) */
#define LA_STREAM_PKTS 3
#define LA_STREAM_CHUNK 65536u
#define LA_E_PENDING 0x8000000Au

enum { LA_K_ENG = 0x4C41454Eu, LA_K_WB = 0x4C415742u, LA_K_SB = 0x4C415342u, LA_K_SRC = 0x4C415352u, LA_K_CUE = 0x4C414355u };

/* Spielobjekte: 64 Null-Bytes vorn (wie die W275-Null-Objekte), dann Kennung. */
typedef struct {
    uint8_t zero[64];
    u32 kind;
} la_obj_t;

typedef struct {
    la_obj_t o;
    int used, streamed;
    const uint8_t *mem; /* im Speicher: Spielpuffer (bleibt bis UnRegister) */
    uint8_t *hdr;       /* Kopf (Streaming: Kopie) */
    uint32_t hdrlen;
    u32 xh;             /* Streaming: Xbox-Datei-Handle */
    uint64_t base;      /* Streaming: Dateioffset der Bank */
    lxf_bank_t b;
} la_wb_t;

typedef struct {
    la_obj_t o;
    int used;
    uint8_t *data;
    lxf_xsb_t x;
    int wbmap[256]; /* XSB-Bank -> la_wb-Index (+1), 0 = noch offen */
} la_sb_t;

typedef struct {
    la_obj_t o;
    int used, is3d, has_pan;
    float pan_l, pan_r;
    u32 pos[3]; /* Float-Bits */
    float bin[6]; /* W356: Mixbin-Pegel FL FR C LFE BL BR (linear) aus SetMixBins */
    int has_lfe;  /* W356: Bin-Liste enthaelt LFE (XACT ersetzt dessen Pegel je Cue) */
    u32 i3d[9];   /* W356: DSI3DL2BUFFER aus IXACTSoundSource_SetI3DL2Source */
    int has_i3d;
} la_src_t;

typedef struct {
    la_obj_t o;
    int state; /* 0 frei, 1 vorbereitet, 2 spielt */
    la_sb_t *sb;
    int cue;
    la_src_t *src;
    lxf_cue_t c;
    int pool[LXF_MAX_TRACKS]; /* Pool-Indizes (+1) */
    int paused;
} la_cue_t;

typedef struct {
    u32 v;      /* Mischer-Stimme */
    int wb;     /* la_wb-Index */
    int wave;
    int busy;
    lxf_wave_t w;
    /* Lange Wellen (> LA_STREAM_SECS): Stream-Stimme, paketweise nachgespeist */
    int is_stream, s_eof, s_done;
    uint32_t s_pos, s_chunk;
    u32 s_pkt[LA_STREAM_PKTS][6]; /* XMEDIAPACKET */
    u32 s_status[LA_STREAM_PKTS], s_compl[LA_STREAM_PKTS];
    int s_inuse[LA_STREAM_PKTS];
    uint8_t *s_buf[LA_STREAM_PKTS];
} la_pool_t;

static hlock_t s_lk;
static la_obj_t s_eng;
static la_wb_t s_wb[LA_MAX_WB];
static la_sb_t s_sb[LA_MAX_SB];
static la_src_t s_src[LA_MAX_SRC];
static la_cue_t s_cue[LA_MAX_CUE];
static int s_cue_next;
static la_pool_t s_pool[LA_MAX_POOL];
static int s_npool;
static int s_cat_db[LA_MAX_CAT];
static int s_cat_pause[LA_MAX_CAT];
static uint8_t s_note_reg[2][LA_NOTE_CUES / 8]; /* wType 0/1 je Cue */
static int s_note_all[2];
static u32 s_note_ctx[2];
static u32 s_note_q[LA_MAX_NOTE][6];
static int s_note_rd, s_note_wr, s_note_n;
static uint32_t s_rnd = 0x2545F491u;
static int s_active;
/* Statistik */
static volatile LONG st_play, st_prep, st_stop, st_end, st_notes, st_drop_notes, st_wma, st_fail, st_voices, st_reuse,
    st_evict, st_tracks, st_loops, st_cuelog, st_streams, st_spkts, st_sread_fail;

static void lk(void) { hlock_enter(&s_lk); }
static void ulk(void) { hlock_leave(&s_lk); }

static u32 la_ret(void) { return 0; }

static void la_out(u32 pp, u32 v)
{
    if (pp) {
        __try { *(u32 *)(uintptr_t)pp = v; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

static int la_rd(u32 p, void *dst, u32 n)
{
    if (!p) {
        return 0;
    }
    __try { memcpy(dst, (const void *)(uintptr_t)p, n); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static int la_is(const void *p, u32 kind, const void *arr, size_t elsz, int n)
{
    uintptr_t a = (uintptr_t)p, b0 = (uintptr_t)arr;
    if (!p || a < b0 || a >= b0 + elsz * (size_t)n || (a - b0) % elsz != 0) {
        return 0;
    }
    return ((const la_obj_t *)p)->kind == kind;
}
#define IS_WB(p) la_is((p), LA_K_WB, s_wb, sizeof s_wb[0], LA_MAX_WB)
#define IS_SB(p) la_is((p), LA_K_SB, s_sb, sizeof s_sb[0], LA_MAX_SB)
#define IS_SRC(p) la_is((p), LA_K_SRC, s_src, sizeof s_src[0], LA_MAX_SRC)
#define IS_CUE(p) la_is((p), LA_K_CUE, s_cue, sizeof s_cue[0], LA_MAX_CUE)

static void la_first(int id, const char *what)
{
    static volatile LONG seen[48];
    if (id >= 0 && id < 48 && InterlockedExchange(&seen[id], 1) == 0) {
        HLOG_I("LOCAUDIO: %s erster Aufruf (Aufrufer 0x%X)", what, (u32)(uintptr_t)_ReturnAddress());
    }
}

static u32 la_rand(void)
{
    s_rnd ^= s_rnd << 13;
    s_rnd ^= s_rnd >> 17;
    s_rnd ^= s_rnd << 5;
    return s_rnd;
}

/* Streaming: Bytes ab Dateioffset lesen (Abbild oder Host-Datei). */
static int la_read_at(u32 xh, uint64_t off, void *buf, u32 len)
{
    unsigned long got = 0;
    long long o = (long long)off;
    LOC_RB_PREWRITE(buf, len); /* W330: Rollback-Schreibschutz (falls Gast-Puffer) */
    if (krnl_vfs_handle(xh)) {
        return krnl_vfs_read(xh, &o, buf, len, &got, NULL, 0) && got == len;
    } else {
        HANDLE h = (HANDLE)krnl_file_host(xh);
        OVERLAPPED ov;
        DWORD g = 0;
        if (!h) {
            return 0;
        }
        memset(&ov, 0, sizeof ov);
        ov.Offset = (DWORD)off;
        ov.OffsetHigh = (DWORD)(off >> 32);
        return ReadFile(h, buf, len, &g, &ov) && g == len;
    }
}

/* ---------------- Benachrichtigungen ---------------- */
static int la_note_wanted(int type, int cue)
{
    if (type < 0 || type > 1) {
        return 0;
    }
    if (s_note_all[type]) {
        return 1;
    }
    return cue >= 0 && cue < LA_NOTE_CUES && (s_note_reg[type][cue >> 3] & (1u << (cue & 7)));
}

static void la_note_push(int type, la_cue_t *c)
{
    u32 *q;
    if (!la_note_wanted(type, c->cue)) {
        return;
    }
    if (s_note_n >= LA_MAX_NOTE) {
        InterlockedIncrement(&st_drop_notes);
        return;
    }
    q = s_note_q[s_note_wr];
    s_note_wr = (s_note_wr + 1) % LA_MAX_NOTE;
    s_note_n++;
    q[0] = (u32)type; /* wType (u16) + wFlags 0 */
    q[1] = (u32)(uintptr_t)c->sb;
    q[2] = (u32)c->cue;
    q[3] = (u32)(uintptr_t)c;
    q[4] = s_note_ctx[type];
    q[5] = 0;
}

/* ---------------- Stimmen-Pool ---------------- */
static int la_wave_get(int wbi, int wave, lxf_wave_t *w)
{
    la_wb_t *wb = &s_wb[wbi];
    if (!wb->used) {
        return -1;
    }
    return lxf_xwb_wave(&wb->b, wb->hdr ? wb->hdr : wb->mem, wb->hdr ? wb->hdrlen : wb->b.data_off + wb->b.data_len,
                        (uint32_t)wave, w);
}

static u32 la_voice_create(int wbi, const lxf_wave_t *w)
{
    la_wb_t *wb = &s_wb[wbi];
    uint8_t wfx[20];
    u32 desc[9], v;
    uint8_t *tmp = NULL;
    const uint8_t *src;
    lxf_wave_wfx(w, wfx);
    memset(desc, 0, sizeof desc);
    desc[0] = 36;
    desc[3] = (u32)(uintptr_t)wfx;
    if (w->len == 0 || w->len > LA_MAX_WAVE_BYTES) {
        return 0;
    }
    if (wb->streamed) {
        tmp = (uint8_t *)malloc(w->len);
        if (!tmp || !la_read_at(wb->xh, wb->base + w->off, tmp, w->len)) {
            free(tmp);
            return 0;
        }
        src = tmp;
    } else {
        if ((uint64_t)w->off + w->len > (uint64_t)wb->b.data_off + wb->b.data_len) {
            return 0;
        }
        src = wb->mem + w->off;
    }
    v = audio_buffer_create((u32)(uintptr_t)desc, 0);
    if (v) {
        audio_set_buffer_data(v, (u32)(uintptr_t)src, w->len);
        InterlockedIncrement(&st_voices);
    }
    free(tmp);
    return v;
}

static double la_wave_secs(const lxf_wave_t *w)
{
    double fr;
    if (w->rate <= 0 || w->ch <= 0) {
        return 0;
    }
    if (w->codec == LXF_CODEC_ADPCM) {
        fr = (double)(w->len / (36u * (uint32_t)w->ch)) * 64.0;
    } else {
        fr = (double)w->len / (double)(w->ch * (w->bits ? w->bits : 16) / 8);
    }
    return fr / w->rate;
}

/* Stream-Stimme nachspeisen (bis LA_STREAM_PKTS Pakete offen). Unter s_lk. */
static void la_stream_feed(la_pool_t *p)
{
    int k, pending = 0;
    la_wb_t *wb = &s_wb[p->wb];
    for (k = 0; k < LA_STREAM_PKTS; k++) {
        if (p->s_inuse[k] && p->s_status[k] != LA_E_PENDING) {
            p->s_inuse[k] = 0; /* vom Mischer verbraucht (oder abgebrochen) */
        }
        pending += p->s_inuse[k];
    }
    for (k = 0; k < LA_STREAM_PKTS && !p->s_eof; k++) {
        uint32_t end, n;
        if (p->s_inuse[k]) {
            continue;
        }
        end = (p->w.loop && p->w.loop_len) ? p->w.loop_off + p->w.loop_len : p->w.len;
        if (p->s_pos >= end) {
            if (p->w.loop) {
                p->s_pos = p->w.loop_len ? p->w.loop_off : 0;
            } else {
                p->s_eof = 1;
                break;
            }
        }
        n = end - p->s_pos;
        if (n > p->s_chunk) {
            n = p->s_chunk;
        }
        if (!p->s_buf[k] && !(p->s_buf[k] = (uint8_t *)malloc(p->s_chunk))) {
            p->s_eof = 1;
            break;
        }
        if (wb->streamed ? !la_read_at(wb->xh, wb->base + p->w.off + p->s_pos, p->s_buf[k], n)
                         : ((uint64_t)p->w.off + p->s_pos + n > (uint64_t)wb->b.data_off + wb->b.data_len)) {
            InterlockedIncrement(&st_sread_fail);
            p->s_eof = 1;
            break;
        }
        if (!wb->streamed) {
            memcpy(p->s_buf[k], wb->mem + p->w.off + p->s_pos, n);
        }
        p->s_pkt[k][0] = (u32)(uintptr_t)p->s_buf[k];
        p->s_pkt[k][1] = n;
        p->s_pkt[k][2] = (u32)(uintptr_t)&p->s_compl[k];
        p->s_pkt[k][3] = (u32)(uintptr_t)&p->s_status[k];
        p->s_pkt[k][4] = 0;
        p->s_pkt[k][5] = 0;
        p->s_status[k] = LA_E_PENDING;
        p->s_inuse[k] = 1;
        audio_process(p->v, (u32)(uintptr_t)p->s_pkt[k], 0);
        p->s_pos += n;
        pending++;
        InterlockedIncrement(&st_spkts);
    }
    p->s_done = p->s_eof && pending == 0;
}

static void la_pool_clear(la_pool_t *p)
{
    int k;
    if (p->v) {
        if (p->is_stream) {
            audio_flush(p->v);
        }
        audio_stop(p->v);
        audio_release(p->v);
    }
    for (k = 0; k < LA_STREAM_PKTS; k++) {
        free(p->s_buf[k]);
    }
    memset(p, 0, sizeof *p);
    p->wb = -1;
}

/* Stream-Eintrag fuer eine lange Welle anlegen; -1 = keiner. Unter s_lk. */
static int la_stream_acquire(int wbi, int wave, const lxf_wave_t *w)
{
    int i;
    uint8_t wfx[20];
    u32 desc[9], align;
    for (i = 0; i < s_npool; i++) {
        if (!s_pool[i].v && !s_pool[i].busy) {
            break;
        }
    }
    if (i == s_npool) {
        if (s_npool >= LA_MAX_POOL) {
            return -1;
        }
        s_npool++;
    }
    memset(&s_pool[i], 0, sizeof s_pool[i]);
    lxf_wave_wfx(w, wfx);
    memset(desc, 0, sizeof desc);
    desc[0] = 36;
    desc[3] = (u32)(uintptr_t)wfx;
    s_pool[i].v = audio_stream_create((u32)(uintptr_t)desc, 0);
    if (!s_pool[i].v) {
        s_pool[i].wb = -1;
        return -1;
    }
    align = w->codec == LXF_CODEC_ADPCM ? 36u * (u32)w->ch : (u32)(w->ch * (w->bits ? w->bits : 16) / 8);
    s_pool[i].s_chunk = align ? (LA_STREAM_CHUNK / align) * align : LA_STREAM_CHUNK;
    s_pool[i].is_stream = 1;
    s_pool[i].wb = wbi;
    s_pool[i].wave = wave;
    s_pool[i].w = *w;
    s_pool[i].busy = 1;
    InterlockedIncrement(&st_streams);
    return i;
}

/* Pool-Eintrag fuer (Bank, Welle) belegen; -1 = keiner. Unter s_lk. */
static int la_pool_acquire(int wbi, int wave, const lxf_wave_t *w)
{
    int i, victim = -1;
    for (i = 0; i < s_npool; i++) {
        la_pool_t *p = &s_pool[i];
        if (!p->busy && !p->is_stream && p->wb == wbi && p->wave == wave && p->v &&
            !(audio_get_status(p->v) & LA_DSBSTATUS_PLAYING)) {
            p->busy = 1;
            InterlockedIncrement(&st_reuse);
            return i;
        }
    }
    if (s_npool < LA_MAX_POOL) {
        i = s_npool;
    } else {
        for (i = 0; i < s_npool; i++) { /* freie Stimme einer anderen Welle verdraengen */
            if (!s_pool[i].busy && !s_pool[i].is_stream) {
                victim = i;
                break;
            }
        }
        if (victim < 0) {
            return -1;
        }
        i = victim;
        la_pool_clear(&s_pool[i]);
        InterlockedIncrement(&st_evict);
    }
    {
        u32 v = la_voice_create(wbi, w);
        if (!v) {
            if (victim >= 0) {
                s_pool[i].wb = -1;
            }
            return -1;
        }
        s_pool[i].v = v;
        s_pool[i].wb = wbi;
        s_pool[i].wave = wave;
        s_pool[i].w = *w;
        s_pool[i].busy = 1;
        if (i == s_npool) {
            s_npool++;
        }
    }
    return i;
}

/* ---------------- Cues ---------------- */
static int la_cat(const la_cue_t *c)
{
    int k = c->c.category;
    return (k >= 0 && k < LA_MAX_CAT) ? k : 0;
}

static void la_cue_apply(la_cue_t *c)
{
    int t, k = la_cat(c);
    int xs = g_cfg.audio_xact_sends; /* W356: Pegel/Sends je Cue wie die XACT-Bibliothek (0x1C4208) */
    int db = (xs ? c->c.db100_xact : c->c.db100) + s_cat_db[k] + ((c->src && c->src->is3d) ? LA_3D_HEADROOM : 0);
    for (t = 0; t < LXF_MAX_TRACKS; t++) {
        la_pool_t *p;
        u32 a[16];
        if (!c->pool[t]) {
            continue;
        }
        p = &s_pool[c->pool[t] - 1];
        {
            int dbt = db + (p->w.ch > 2 ? LA_DOWNMIX_DB : 0);
            audio_set_volume(p->v, dbt < -10000 ? -10000 : dbt);
        }
        audio_set_ucat(p->v, avol_loc_cat(k)); /* W303: Pool-Stimmen wechseln den Cue -> je Anwendung */
        memset(a, 0, sizeof a);
        {
            float mn = LA_MIN3D, mx = LA_MAX3D;
            memcpy(&a[13], &mn, 4);
            memcpy(&a[14], &mx, 4);
        }
        if (c->src && c->src->is3d) {
            a[1] = c->src->pos[0];
            a[2] = c->src->pos[1];
            a[3] = c->src->pos[2];
            a[15] = 0; /* NORMAL */
            audio_set_pan(p->v, 1.0f, 1.0f);
            if (xs) { /* W356: 3D-Stimmen haben den I3DL2-Bin -> Send je Cue; Occlusion der Quelle */
                static const u32 none[9];
                audio_set_room_send(p->v, c->c.room_mb);
                audio_set_i3dl2_vals(p->v, c->src->has_i3d ? c->src->i3d : none);
            }
        } else {
            a[15] = 2; /* DISABLE: 2D */
            if (c->src && c->src->has_pan) {
                if (xs) { /* W356: LFE-Pegel je Cue (nur wenn die Bin-Liste der Quelle LFE enthaelt) */
                    float g6[6];
                    memcpy(g6, c->src->bin, sizeof g6);
                    if (c->src->has_lfe)
                        g6[3] = c->c.lfe_mb <= -10000 ? 0.0f : (float)pow(10.0, c->c.lfe_mb / 2000.0);
                    audio_set_bins(p->v, g6);
                } else {
                    audio_set_pan(p->v, c->src->pan_l, c->src->pan_r);
                }
            } else {
                audio_set_pan(p->v, 1.0f, 1.0f);
            }
            if (xs) {
                static const u32 none[9];
                audio_set_room_send(p->v, -10000); /* W356: 2D-Bin-Listen ohne I3DL2-Bin = trocken */
                audio_set_i3dl2_vals(p->v, none); /* Pool-Stimme: Occlusion eines frueheren 3D-Cues loeschen */
            }
        }
        audio_3d_voice_allparams(p->v, (u32)(uintptr_t)a);
    }
}

static void la_cue_release_voices(la_cue_t *c, int stop)
{
    int t;
    for (t = 0; t < LXF_MAX_TRACKS; t++) {
        if (c->pool[t]) {
            la_pool_t *p = &s_pool[c->pool[t] - 1];
            if (p->is_stream) {
                la_pool_clear(p);
            } else {
                if (stop && p->v) {
                    audio_stop(p->v);
                }
                p->busy = 0;
            }
            c->pool[t] = 0;
        }
    }
}

/* Cue beenden (gestoppt oder ausgelaufen): Stimmen frei, Meldung, Slot frei. */
static void la_cue_end(la_cue_t *c, int stopped)
{
    if (c->state == 0) {
        return;
    }
    la_cue_release_voices(c, stopped);
    if (c->state == 2) {
        la_note_push(1, c);
        InterlockedIncrement(stopped ? &st_stop : &st_end);
    }
    c->state = 0;
    c->paused = 0;
}

static la_cue_t *la_cue_alloc(void)
{
    int k;
    for (k = 0; k < LA_MAX_CUE; k++) { /* reihum: alte Handles bleiben moeglichst lange ungenutzt */
        la_cue_t *c = &s_cue[(s_cue_next + k) % LA_MAX_CUE];
        if (c->state == 0) {
            s_cue_next = (int)(c - s_cue + 1) % LA_MAX_CUE;
            memset(c, 0, sizeof *c);
            c->o.kind = LA_K_CUE;
            return c;
        }
    }
    return NULL;
}

/* XSB-Bank-Index -> registrierte Wave-Bank (Name, Gross/Klein egal). */
static int la_sb_bank(la_sb_t *sb, int xbank)
{
    char nm[17];
    int i;
    if (xbank < 0 || xbank >= 256) {
        return -1;
    }
    if (sb->wbmap[xbank] > 0 && s_wb[sb->wbmap[xbank] - 1].used) {
        return sb->wbmap[xbank] - 1;
    }
    lxf_xsb_bank_name(&sb->x, xbank, nm);
    for (i = 0; i < LA_MAX_WB; i++) {
        if (s_wb[i].used && _stricmp(s_wb[i].b.name, nm) == 0) {
            sb->wbmap[xbank] = i + 1;
            return i;
        }
    }
    return -1;
}

/* Cue-Daten aufloesen (Prepare): 0 = ok. */
static int la_cue_prepare(la_cue_t *c, la_sb_t *sb, int cue, la_src_t *src)
{
    c->sb = sb;
    c->cue = cue;
    c->src = src;
    if (lxf_xsb_cue(&sb->x, cue, la_rand(), &c->c) != 0) {
        InterlockedIncrement(&st_fail);
        c->c.ntrack = 0;
    }
    c->state = 1;
    return 0;
}

static void la_cue_start(la_cue_t *c)
{
    int t, k = la_cat(c), nv = 0;
    for (t = 0; t < c->c.ntrack; t++) {
        lxf_wave_t w;
        int wbi = la_sb_bank(c->sb, c->c.track[t].bank), pi;
        la_pool_t *p;
        if (wbi < 0 || la_wave_get(wbi, c->c.track[t].wave, &w) != 0) {
            InterlockedIncrement(&st_fail);
            continue;
        }
        if (w.codec == LXF_CODEC_WMA) {
            InterlockedIncrement(&st_wma);
            continue;
        }
        pi = la_wave_secs(&w) > LA_STREAM_SECS ? la_stream_acquire(wbi, c->c.track[t].wave, &w)
                                                : la_pool_acquire(wbi, c->c.track[t].wave, &w);
        if (pi < 0) {
            InterlockedIncrement(&st_fail);
            continue;
        }
        p = &s_pool[pi];
        c->pool[t] = pi + 1;
        nv++;
        audio_set_frequency(p->v, c->c.pitch ? (u32)(w.rate * pow(2.0, c->c.pitch / 4096.0) + 0.5) : (u32)w.rate);
        if (p->is_stream) {
            InterlockedIncrement(&st_tracks);
            if (w.loop) {
                InterlockedIncrement(&st_loops);
            }
            continue;
        }
        audio_set_pos(p->v, 0);
        if (w.loop) {
            audio_set_loop(p->v, w.loop_len ? w.loop_off : 0, w.loop_len ? w.loop_len : w.len);
            InterlockedIncrement(&st_loops);
        }
        InterlockedIncrement(&st_tracks);
    }
    c->state = 2;
    la_cue_apply(c);
    for (t = 0; t < LXF_MAX_TRACKS; t++) {
        if (c->pool[t]) {
            la_pool_t *p = &s_pool[c->pool[t] - 1];
            if (p->is_stream) {
                la_stream_feed(p);
            } else {
                audio_play(p->v, p->w.loop ? LA_DSBPLAY_LOOPING : 0);
            }
            if (s_cat_pause[k]) {
                audio_pause(p->v, 1);
                c->paused = 1;
            }
        }
    }
    InterlockedIncrement(&st_play);
    if (g_cfg.loc_audio >= 2 && InterlockedIncrement(&st_cuelog) <= 400) {
        const char *nm = lxf_xsb_cue_name(&c->sb->x, c->cue);
        HLOG_I("LOCAUDIO cue %d '%s' Spuren=%d Stimmen=%d Kat=%d dB=%d Pitch=%d Quelle=%s", c->cue, nm ? nm : "?",
               c->c.ntrack, nv, k, c->c.db100, c->c.pitch,
               c->src ? (c->src->is3d ? "3D" : "2D") : "-");
    }
}

/* Ausgelaufene Cues erkennen (aus DoWork/GetNotification). Unter s_lk. */
static void la_pump(void)
{
    int i, t;
    for (i = 0; i < LA_MAX_CUE; i++) {
        la_cue_t *c = &s_cue[i];
        int playing = 0;
        if (c->state != 2 || c->paused) {
            continue;
        }
        for (t = 0; t < LXF_MAX_TRACKS; t++) {
            la_pool_t *p;
            if (!c->pool[t]) {
                continue;
            }
            p = &s_pool[c->pool[t] - 1];
            if (p->is_stream) {
                la_stream_feed(p);
                playing |= !p->s_done;
            } else if (audio_get_status(p->v) & LA_DSBSTATUS_PLAYING) {
                playing = 1;
            }
        }
        if (!playing) {
            la_cue_end(c, 0);
        }
    }
}

/* ---------------- XACT-Einstiege (stdcall, Argumentzahl = loc_stub_va.h) ---------------- */
static u32 __stdcall la_XACTEngineDoWork(void)
{
    lk();
    la_pump();
    ulk();
    return 0;
}

static u32 __stdcall la_XACTEngineCreate(u32 params, u32 pp)
{
    (void)params;
    la_first(0, "XACTEngineCreate");
    s_eng.kind = LA_K_ENG;
    la_out(pp, (u32)(uintptr_t)&s_eng);
    return 0;
}

static u32 __stdcall la_CreateSoundSource(u32 e, u32 flags, u32 pp)
{
    int i;
    (void)e;
    lk();
    for (i = 0; i < LA_MAX_SRC; i++) {
        if (!s_src[i].used) {
            break;
        }
    }
    if (i == LA_MAX_SRC) {
        ulk();
        return LA_E_FAIL;
    }
    memset(&s_src[i], 0, sizeof s_src[i]);
    s_src[i].o.kind = LA_K_SRC;
    s_src[i].used = 1;
    s_src[i].is3d = (flags & 2u) ? 1 : 0;
    s_src[i].pan_l = s_src[i].pan_r = 1.0f;
    ulk();
    la_out(pp, (u32)(uintptr_t)&s_src[i]);
    return 0;
}

static u32 __stdcall la_CreateSoundBank(u32 e, u32 data, u32 size, u32 pp)
{
    int i, rc;
    (void)e;
    if (!data || size < 0x38 || size > 16u * 1024u * 1024u) {
        return LA_E_FAIL;
    }
    lk();
    for (i = 0; i < LA_MAX_SB; i++) {
        if (!s_sb[i].used) {
            break;
        }
    }
    if (i == LA_MAX_SB) {
        ulk();
        return LA_E_FAIL;
    }
    memset(&s_sb[i], 0, sizeof s_sb[i]);
    s_sb[i].data = (uint8_t *)malloc(size);
    if (!s_sb[i].data || !la_rd(data, s_sb[i].data, size)) {
        free(s_sb[i].data);
        s_sb[i].data = NULL;
        ulk();
        return LA_E_FAIL;
    }
    rc = lxf_xsb_parse(s_sb[i].data, size, &s_sb[i].x);
    if (rc != 0) {
        HLOG_W("LOCAUDIO: Sound-Bank (%u B) nicht lesbar (rc=%d) - Cues bleiben stumm", size, rc);
    }
    s_sb[i].o.kind = LA_K_SB;
    s_sb[i].used = 1;
    ulk();
    HLOG_I("LOCAUDIO: Sound-Bank %u B: %d Cues, %d Eintraege, %d Wave-Banks", size, s_sb[i].x.n_cues, s_sb[i].x.n_ent,
           s_sb[i].x.n_wb);
    la_out(pp, (u32)(uintptr_t)&s_sb[i]);
    return 0;
}

static int la_wb_slot(void)
{
    int i;
    for (i = 0; i < LA_MAX_WB; i++) {
        if (!s_wb[i].used) {
            return i;
        }
    }
    return -1;
}

static void la_wb_log(const la_wb_t *wb)
{
    HLOG_I("LOCAUDIO: Wave-Bank '%s' %s: %u Wellen, Daten %u B", wb->b.name, wb->streamed ? "Streaming" : "Speicher",
           wb->b.count, wb->b.data_len);
}

static u32 __stdcall la_RegisterWaveBank(u32 e, u32 data, u32 size, u32 pp)
{
    int i, rc;
    (void)e;
    lk();
    i = la_wb_slot();
    if (i < 0 || !data) {
        ulk();
        return LA_E_FAIL;
    }
    memset(&s_wb[i], 0, sizeof s_wb[i]);
    s_wb[i].mem = (const uint8_t *)(uintptr_t)data;
    rc = lxf_xwb_parse(s_wb[i].mem, size, &s_wb[i].b);
    if (rc != 0) {
        ulk();
        HLOG_W("LOCAUDIO: Wave-Bank (%u B) nicht lesbar (rc=%d)", size, rc);
        return LA_E_FAIL;
    }
    s_wb[i].o.kind = LA_K_WB;
    s_wb[i].used = 1;
    ulk();
    la_wb_log(&s_wb[i]);
    la_out(pp, (u32)(uintptr_t)&s_wb[i]);
    return 0;
}

/* params: {hFile, dwOffset, Paketgroesse, Paketzahl} (FUN_00088240/FUN_0008bea0) */
static u32 __stdcall la_RegisterStreamedWaveBank(u32 e, u32 params, u32 pp)
{
    u32 p[2];
    uint8_t head[0x50];
    uint32_t hs;
    int i, rc;
    (void)e;
    if (!la_rd(params, p, sizeof p)) {
        return LA_E_FAIL;
    }
    if (!la_read_at(p[0], p[1], head, sizeof head) || (hs = lxf_xwb_header_size(head, sizeof head)) == 0) {
        HLOG_W("LOCAUDIO: Streaming-Bank (Handle 0x%X) Kopf nicht lesbar", p[0]);
        return LA_E_FAIL;
    }
    lk();
    i = la_wb_slot();
    if (i < 0) {
        ulk();
        return LA_E_FAIL;
    }
    memset(&s_wb[i], 0, sizeof s_wb[i]);
    s_wb[i].hdr = (uint8_t *)malloc(hs);
    if (!s_wb[i].hdr || !la_read_at(p[0], p[1], s_wb[i].hdr, hs) ||
        (rc = lxf_xwb_parse(s_wb[i].hdr, hs, &s_wb[i].b)) != 0) {
        free(s_wb[i].hdr);
        s_wb[i].hdr = NULL;
        ulk();
        HLOG_W("LOCAUDIO: Streaming-Bank (Handle 0x%X) nicht lesbar", p[0]);
        return LA_E_FAIL;
    }
    s_wb[i].hdrlen = hs;
    s_wb[i].streamed = 1;
    s_wb[i].xh = p[0];
    s_wb[i].base = p[1];
    s_wb[i].o.kind = LA_K_WB;
    s_wb[i].used = 1;
    ulk();
    la_wb_log(&s_wb[i]);
    la_out(pp, (u32)(uintptr_t)&s_wb[i]);
    return 0;
}

static u32 __stdcall la_UnRegisterWaveBank(u32 e, u32 wbp)
{
    la_wb_t *wb = (la_wb_t *)(uintptr_t)wbp;
    int wbi, i, t;
    (void)e;
    lk();
    if (!IS_WB(wb) || !wb->used) {
        ulk();
        return 0;
    }
    wbi = (int)(wb - s_wb);
    for (i = 0; i < LA_MAX_CUE; i++) { /* Cues auf dieser Bank beenden */
        la_cue_t *c = &s_cue[i];
        if (c->state == 0) {
            continue;
        }
        for (t = 0; t < LXF_MAX_TRACKS; t++) {
            if (c->pool[t] && s_pool[c->pool[t] - 1].wb == wbi) {
                la_cue_end(c, 1);
                break;
            }
        }
    }
    for (i = 0; i < s_npool; i++) {
        if (s_pool[i].wb == wbi && s_pool[i].v) {
            la_pool_clear(&s_pool[i]);
        }
    }
    for (i = 0; i < LA_MAX_SB; i++) {
        memset(s_sb[i].wbmap, 0, sizeof s_sb[i].wbmap);
    }
    free(wb->hdr);
    memset(wb, 0, sizeof *wb);
    ulk();
    return 0;
}

static void la_cat_update(int k)
{
    int i;
    for (i = 0; i < LA_MAX_CUE; i++) {
        if (s_cue[i].state == 2 && la_cat(&s_cue[i]) == k) {
            la_cue_apply(&s_cue[i]);
        }
    }
}

static u32 __stdcall la_SetMasterVolume(u32 e, u32 cat, u32 vol)
{
    static volatile LONG n;
    (void)e;
    if (cat >= LA_MAX_CAT) {
        return 0;
    }
    if (InterlockedIncrement(&n) <= 24) {
        HLOG_I("LOCAUDIO: SetMasterVolume Kategorie %u = %d/100 dB", cat, (int)vol);
    }
    lk();
    s_cat_db[cat] = (int)vol < -10000 ? -10000 : ((int)vol > 0 ? 0 : (int)vol);
    la_cat_update((int)cat);
    ulk();
    return 0;
}

static u32 __stdcall la_GlobalPause(u32 e, u32 cat, u32 pause)
{
    int i, t;
    (void)e;
    la_first(1, "IXACTEngine_GlobalPause");
    if (cat >= LA_MAX_CAT) {
        return 0;
    }
    lk();
    s_cat_pause[cat] = pause ? 1 : 0;
    for (i = 0; i < LA_MAX_CUE; i++) {
        la_cue_t *c = &s_cue[i];
        if (c->state != 2 || la_cat(c) != (int)cat) {
            continue;
        }
        for (t = 0; t < LXF_MAX_TRACKS; t++) {
            if (c->pool[t]) {
                audio_pause(s_pool[c->pool[t] - 1].v, pause ? 1 : 0);
            }
        }
        c->paused = pause ? 1 : 0;
    }
    ulk();
    return 0;
}

static u32 __stdcall la_SetListenerOrientation(u32 e, u32 fx, u32 fy, u32 fz, u32 tx, u32 ty, u32 tz, u32 apply)
{
    (void)e;
    (void)apply;
    audio_3d_listener_orient(fx, fy, fz, tx, ty, tz);
    return 0;
}

/* W348 AUDIO-DSP: IXACTEngine_SetI3dl2Listener(e, DSI3DL2LISTENER*, apply). Das Spiel waehlt per
 * FUN_00088E40 eines von 25 XDK-Presets (Tabelle 0x273FB0) und uebergibt es hier; die echte
 * XACT-Engine reicht es an IDirectSound_SetI3DL2Listener weiter. Wirkung im Mischer (audio_reverb);
 * CommitDeferredSettings bleibt Stub, darum sofort. */
static u32 __stdcall la_SetI3dl2Listener(u32 e, u32 p, u32 apply)
{
    u32 d[12];
    (void)e;
    (void)apply;
    if (g_cfg.audio_reverb_test >= 0 && g_cfg.audio_reverb_test <= 24) /* TEST: Preset erzwingen (Hoerprobe) */
        p = 0x273FB0u + (u32)g_cfg.audio_reverb_test * 48u;
    if (p && la_rd(p, d, sizeof d)) {
        if (g_cfg.audio_api_diag)
            HLOG_I("DIAG(I3DL2) LoC XACT room=%d roomhf=%d decay=%.2f reverb=%d apply=%u", (int)d[0], (int)d[1],
                   *(float *)&d[3], (int)d[7], apply);
        audio_set_i3dl2_listener(d, 0);
    }
    return 0;
}

static u32 __stdcall la_SetListenerPosition(u32 e, u32 x, u32 y, u32 z, u32 apply)
{
    (void)e;
    (void)apply;
    audio_3d_listener_pos(x, y, z);
    return 0;
}

/* desc: {wType|wFlags<<16, Bank, Cue, Cue-Handle, Kontext, Event} */
static u32 __stdcall la_RegisterNotification(u32 e, u32 pdesc)
{
    u32 d[5];
    int type;
    (void)e;
    if (!la_rd(pdesc, d, sizeof d)) {
        return LA_E_FAIL;
    }
    type = (int)(d[0] & 0xFFFFu);
    if (type < 0 || type > 1) {
        return 0;
    }
    lk();
    s_note_ctx[type] = d[4];
    if ((int)d[2] < 0) {
        s_note_all[type] = 1;
    } else if (d[2] < LA_NOTE_CUES) {
        s_note_reg[type][d[2] >> 3] |= (uint8_t)(1u << (d[2] & 7));
    }
    ulk();
    return 0;
}

static u32 __stdcall la_GetNotification(u32 e, u32 pdesc, u32 pout)
{
    u32 q[6];
    (void)e;
    (void)pdesc;
    lk();
    la_pump();
    if (s_note_n == 0 || !pout) {
        ulk();
        return LA_E_FAIL; /* != 0 = keine Meldung (Aufrufer 0x8B6A0) */
    }
    memcpy(q, s_note_q[s_note_rd], sizeof q);
    s_note_rd = (s_note_rd + 1) % LA_MAX_NOTE;
    s_note_n--;
    ulk();
    {
        int ok = 0;
        __try { memcpy((void *)(uintptr_t)pout, q, sizeof q); ok = 1; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (!ok) {
            return LA_E_FAIL;
        }
    }
    InterlockedIncrement(&st_notes);
    return 0;
}

static u32 __stdcall la_EnableHeadphones(u32 e, u32 on)
{
    (void)e;
    (void)on;
    la_first(2, "IXACTEngine_EnableHeadphones");
    return 0;
}

static u32 __stdcall la_SetParameterControl(u32 e, u32 p)
{
    (void)e;
    (void)p;
    la_first(3, "IXACTEngine_SetParameterControl (RPM, nicht ausgewertet)");
    return 0;
}

static u32 __stdcall la_GetSoundCueIndexFromFriendlyName(u32 sbp, u32 name, u32 pidx)
{
    la_sb_t *sb = (la_sb_t *)(uintptr_t)sbp;
    char nm[64];
    int k = -1;
    memset(nm, 0, sizeof nm);
    if (!la_rd(name, nm, 1)) {
        return LA_E_FAIL;
    }
    {
        int i;
        for (i = 0; i < 63; i++) { /* Name zeichenweise (kann am Seitenende liegen) */
            if (!la_rd(name + (u32)i, &nm[i], 1) || nm[i] == 0) {
                break;
            }
        }
        nm[63] = 0;
    }
    lk();
    if (IS_SB(sb) && sb->used) {
        k = lxf_xsb_find(&sb->x, nm);
    }
    ulk();
    if (k < 0) {
        HLOG_W("LOCAUDIO: Cue-Name '%s' unbekannt", nm);
        la_out(pidx, 0xFFFFFFFFu);
        return LA_E_FAIL;
    }
    la_out(pidx, (u32)k);
    return 0;
}

/* params: {Flags, Cue, Quelle, 0} */
static u32 __stdcall la_PrepareEx(u32 sbp, u32 params, u32 pp)
{
    la_sb_t *sb = (la_sb_t *)(uintptr_t)sbp;
    u32 p[3];
    la_cue_t *c;
    if (!la_rd(params, p, sizeof p)) {
        return LA_E_FAIL;
    }
    lk();
    if (!IS_SB(sb) || !sb->used || (int)p[1] < 0 || (int)p[1] >= sb->x.n_cues || !(c = la_cue_alloc())) {
        ulk();
        InterlockedIncrement(&st_fail);
        return LA_E_FAIL;
    }
    la_cue_prepare(c, sb, (int)p[1], IS_SRC((void *)(uintptr_t)p[2]) ? (la_src_t *)(uintptr_t)p[2] : NULL);
    ulk();
    InterlockedIncrement(&st_prep);
    la_out(pp, (u32)(uintptr_t)c);
    return 0;
}

static u32 __stdcall la_PlayEx(u32 sbp, u32 params, u32 pp)
{
    la_sb_t *sb = (la_sb_t *)(uintptr_t)sbp;
    u32 p[3];
    la_cue_t *c = NULL;
    la_src_t *src;
    if (!la_rd(params, p, sizeof p)) {
        return LA_E_FAIL;
    }
    src = IS_SRC((void *)(uintptr_t)p[2]) ? (la_src_t *)(uintptr_t)p[2] : NULL;
    lk();
    if ((p[0] & 4u) && (int)p[1] == -1) { /* vorbereiteten Cue (*pp) starten */
        u32 h = 0;
        la_rd(pp, &h, 4);
        c = (la_cue_t *)(uintptr_t)h;
        if (!IS_CUE(c) || c->state != 1) {
            ulk();
            InterlockedIncrement(&st_fail);
            return 0;
        }
        if (src) {
            c->src = src;
        }
        la_cue_start(c);
        ulk();
        return 0;
    }
    if (!IS_SB(sb) || !sb->used || (int)p[1] < 0 || (int)p[1] >= sb->x.n_cues || !(c = la_cue_alloc())) {
        ulk();
        InterlockedIncrement(&st_fail);
        return 0; /* Spiel wertet != 0 als Fehler und gibt den Slot sofort frei (0x896A0) - S_OK wie stumm */
    }
    la_cue_prepare(c, sb, (int)p[1], src);
    la_cue_start(c);
    ulk();
    if (pp && !(p[0] & 1u)) {
        la_out(pp, (u32)(uintptr_t)c);
    }
    return 0;
}

static u32 __stdcall la_Stop(u32 sbp, u32 cue, u32 flags, u32 pcue)
{
    int i;
    (void)sbp;
    (void)flags;
    lk();
    if ((int)cue >= 0) {
        for (i = 0; i < LA_MAX_CUE; i++) {
            if (s_cue[i].state != 0 && s_cue[i].cue == (int)cue) {
                la_cue_end(&s_cue[i], 1);
            }
        }
    } else if (IS_CUE((void *)(uintptr_t)pcue)) {
        la_cue_end((la_cue_t *)(uintptr_t)pcue, 1);
    }
    ulk();
    return 0;
}

static u32 __stdcall la_SourceRelease(u32 sp)
{
    la_src_t *s = (la_src_t *)(uintptr_t)sp;
    int i;
    lk();
    if (IS_SRC(s) && s->used) {
        for (i = 0; i < LA_MAX_CUE; i++) {
            if (s_cue[i].state != 0 && s_cue[i].src == s) {
                la_cue_end(&s_cue[i], 1);
            }
        }
        s->used = 0;
    }
    ulk();
    return 0;
}

static u32 __stdcall la_SourceSetPosition(u32 sp, u32 x, u32 y, u32 z, u32 apply)
{
    la_src_t *s = (la_src_t *)(uintptr_t)sp;
    int i, t;
    (void)apply;
    lk();
    if (IS_SRC(s) && s->used) {
        s->pos[0] = x;
        s->pos[1] = y;
        s->pos[2] = z;
        if (s->is3d) {
            for (i = 0; i < LA_MAX_CUE; i++) {
                if (s_cue[i].state == 2 && s_cue[i].src == s) {
                    for (t = 0; t < LXF_MAX_TRACKS; t++) {
                        if (s_cue[i].pool[t]) {
                            audio_3d_voice_pos(s_pool[s_cue[i].pool[t] - 1].v, x, y, z);
                        }
                    }
                }
            }
        }
    }
    ulk();
    return 0;
}

/* DSMIXBINS {Anzahl, Paare{Bin, Lautstaerke}} -> Stereo wie audio_out
 * (FL/FR + 0,707 C + 0,5 LFE + 0,5 BL/BR); Bins >= 6 (XACT-3D) = kein Pan. */
static void la_src_mixbins(la_src_t *s, u32 pmb, int full)
{
    u32 hdr[2], i, pair[2];
    float g[6] = { 0 };
    int any3d = 0, n2d = 0;
    if (!la_rd(pmb, hdr, sizeof hdr) || hdr[0] > 16) {
        return;
    }
    if (!full && s->has_pan) {
        return; /* Lautstaerke-Aktualisierung: Pan bleibt */
    }
    for (i = 0; i < hdr[0]; i++) {
        if (!la_rd(hdr[1] + i * 8, pair, sizeof pair)) {
            return;
        }
        if (pair[0] < 6) {
            int db = (int)pair[1];
            g[pair[0]] = db <= -10000 ? 0.0f : (float)pow(10.0, (db > 0 ? 0 : db) / 2000.0);
            n2d++;
        } else {
            any3d = 1;
        }
    }
    if (any3d || n2d == 0) {
        s->has_pan = 0;
        return;
    }
    s->pan_l = g[0] + 0.7071f * g[2] + 0.5f * g[3] + 0.5f * g[4];
    s->pan_r = g[1] + 0.7071f * g[2] + 0.5f * g[3] + 0.5f * g[5];
    s->has_pan = 1;
    memcpy(s->bin, g, sizeof s->bin); /* W356: Bins fuer LFE je Cue und Surround */
    s->has_lfe = 0;
    for (i = 0; i < hdr[0]; i++)
        if (la_rd(hdr[1] + i * 8, pair, sizeof pair) && pair[0] == 3)
            s->has_lfe = 1;
}

static u32 __stdcall la_SourceSetMixBins(u32 sp, u32 pmb)
{
    la_src_t *s = (la_src_t *)(uintptr_t)sp;
    lk();
    if (IS_SRC(s) && s->used) {
        la_src_mixbins(s, pmb, 1);
    }
    ulk();
    return 0;
}

static u32 __stdcall la_SourceSetMixBinVolumes(u32 sp, u32 pmb)
{
    (void)sp;
    (void)pmb;
    la_first(4, "IXACTSoundSource_SetMixBinVolumes (nicht ausgewertet)");
    return 0;
}

/* W356 AUDIO-2: IXACTSoundSource_SetI3DL2Source(src, DSI3DL2BUFFER*, apply). Das Spiel (FUN_00088E80)
 * setzt allen 64 3D-Quellen nur die Occlusion aus einer Material-Tabelle (0x274460, XDK-Presets
 * Fenster/Tuer/Wand ...), alles andere 0. Wirkung je Stimme im Mischer (audio_occlusion). */
static LONG st_i3d;
static u32 __stdcall la_SourceSetI3DL2Source(u32 sp, u32 p, u32 apply)
{
    la_src_t *s = (la_src_t *)(uintptr_t)sp;
    u32 d[9];
    int i, t;
    (void)apply;
    if (!g_cfg.audio_xact_sends || !la_rd(p, d, sizeof d)) {
        return 0;
    }
    lk();
    if (IS_SRC(s) && s->used) {
        memcpy(s->i3d, d, sizeof d);
        s->has_i3d = 1;
        if (InterlockedIncrement(&st_i3d) <= 2 && g_cfg.audio_api_diag) {
            float lfr;
            memcpy(&lfr, &d[8], 4);
            HLOG_I("DIAG(I3DL2) LoC Quelle Occlusion=%d LFRatio=%.2f Direkt=%d Raum=%d", (int)d[7], lfr, (int)d[0],
                   (int)d[2]);
        }
        if (s->is3d) {
            for (i = 0; i < LA_MAX_CUE; i++) {
                if (s_cue[i].state == 2 && s_cue[i].src == s) {
                    for (t = 0; t < LXF_MAX_TRACKS; t++) {
                        if (s_cue[i].pool[t]) {
                            audio_set_i3dl2_vals(s_pool[s_cue[i].pool[t] - 1].v, d);
                        }
                    }
                }
            }
        }
    }
    ulk();
    return 0;
}

static u32 __stdcall la_SourceStopSoundCues(u32 sp)
{
    la_src_t *s = (la_src_t *)(uintptr_t)sp;
    int i;
    lk();
    for (i = 0; i < LA_MAX_CUE; i++) {
        if (s_cue[i].state != 0 && s_cue[i].src == s) {
            la_cue_end(&s_cue[i], 1);
        }
    }
    ulk();
    return 0;
}

static u32 __stdcall la_SourceGetStatus(u32 sp, u32 pst)
{
    la_src_t *s = (la_src_t *)(uintptr_t)sp;
    int i;
    u32 st = 0;
    lk();
    la_pump();
    for (i = 0; i < LA_MAX_CUE; i++) {
        if (s_cue[i].state == 2 && s_cue[i].src == s) {
            st = 1;
            break;
        }
    }
    ulk();
    la_out(pst, st);
    return 0;
}

/* ---------------- Einbau ---------------- */
static unsigned s_npatch;
static void la_patch(u32 va, void *handler, const char *name)
{
    uint8_t *p = (uint8_t *)(uintptr_t)va;
    DWORD old;
    intptr_t rel = (uint8_t *)handler - (p + 5);
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9;
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    s_npatch++;
    (void)name;
}
#define LA_PATCH(nm, fn) la_patch(LOC_VA_##nm, (void *)(fn), #nm)

int loc_audio_install(void)
{
    if (!g_cfg.audio || !g_cfg.loc_audio) {
        HLOG_I("LOCAUDIO: aus (audio=%d loc_audio=%d) - XACT stumm wie W275", g_cfg.audio, g_cfg.loc_audio);
        return 0;
    }
    (void)la_ret;
    LA_PATCH(XACTEngineDoWork, la_XACTEngineDoWork);
    LA_PATCH(XACTEngineCreate, la_XACTEngineCreate);
    LA_PATCH(IXACTEngine_CreateSoundSource, la_CreateSoundSource);
    LA_PATCH(IXACTEngine_CreateSoundBank, la_CreateSoundBank);
    LA_PATCH(IXACTEngine_RegisterWaveBank, la_RegisterWaveBank);
    LA_PATCH(IXACTEngine_RegisterStreamedWaveBank, la_RegisterStreamedWaveBank);
    LA_PATCH(IXACTEngine_UnRegisterWaveBank, la_UnRegisterWaveBank);
    LA_PATCH(IXACTEngine_SetMasterVolume, la_SetMasterVolume);
    LA_PATCH(IXACTEngine_SetListenerOrientation, la_SetListenerOrientation);
    LA_PATCH(IXACTEngine_SetListenerPosition, la_SetListenerPosition);
    LA_PATCH(IXACTEngine_SetI3dl2Listener, la_SetI3dl2Listener); /* W348 */

    LA_PATCH(IXACTEngine_GlobalPause, la_GlobalPause);
    LA_PATCH(IXACTEngine_RegisterNotification, la_RegisterNotification);
    LA_PATCH(IXACTEngine_GetNotification, la_GetNotification);
    LA_PATCH(IXACTEngine_EnableHeadphones, la_EnableHeadphones);
    LA_PATCH(IXACTEngine_SetParameterControl, la_SetParameterControl);
    LA_PATCH(IXACTSoundBank_GetSoundCueIndexFromFriendlyName, la_GetSoundCueIndexFromFriendlyName);
    LA_PATCH(IXACTSoundBank_PrepareEx, la_PrepareEx);
    LA_PATCH(IXACTSoundBank_PlayEx, la_PlayEx);
    LA_PATCH(IXACTSoundBank_Stop, la_Stop);
    LA_PATCH(IXACTSoundSource_Release, la_SourceRelease);
    LA_PATCH(IXACTSoundSource_SetPosition, la_SourceSetPosition);
    LA_PATCH(IXACTSoundSource_SetMixBins, la_SourceSetMixBins);
    LA_PATCH(IXACTSoundSource_SetMixBinVolumes, la_SourceSetMixBinVolumes);
    LA_PATCH(IXACTSoundSource_SetI3DL2Source, la_SourceSetI3DL2Source); /* W356 */
    LA_PATCH(IXACTSoundSource_StopSoundCues, la_SourceStopSoundCues);
    LA_PATCH(IXACTSoundSource_GetStatus, la_SourceGetStatus);
    s_active = 1;
    HLOG_I("LOCAUDIO: XACT-HLE aktiv, %u Einstiege (WMA-Playlist, I3DL2, Velocity, CommitDeferred bleiben Stub)",
           s_npatch);
    return 1;
}

void loc_audio_tick(int waited_s)
{
    int i, act = 0, busy = 0;
    if (!s_active || waited_s % 30 != 0) {
        return;
    }
    lk();
    for (i = 0; i < LA_MAX_CUE; i++) {
        act += s_cue[i].state == 2;
    }
    for (i = 0; i < s_npool; i++) {
        busy += s_pool[i].busy;
    }
    ulk();
    HLOG_I("LOCAUDIO: t=%ds Cues gespielt=%ld vorbereitet=%ld aktiv=%d gestoppt=%ld ausgelaufen=%ld Meldungen=%ld "
           "(verworfen %ld) Spuren=%ld Schleifen=%ld Stimmen=%ld (Pool %d, belegt %d, wiederverwendet %ld, "
           "verdraengt %ld) Streams=%ld (Pakete %ld, Lesefehler %ld) WMA-stumm=%ld Fehler=%ld",
           waited_s, st_play, st_prep, act, st_stop, st_end, st_notes, st_drop_notes, st_tracks, st_loops, st_voices,
           s_npool, busy, st_reuse, st_evict, st_streams, st_spkts, st_sread_fail, st_wma, st_fail);
}
