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
#include "loc_wma.h" /* W367 LOC-WMA */
#include "loc_stub_va.h"
#include "loc_audio_scope.h" /* W375 (#51) */
void loc_api_count(int id); /* W363 loc_boot.c: Aufrufzaehler je Einstieg (audio_api_diag) */
long loc_api_calls(int id);
static long s_vel_nz; /* W363 DIAG: SetVelocity mit v != 0 */
static long st_rpc, st_rpc_none; /* W363: RPC angewendet / ohne Kurve */
static float s_vel_max2;
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
/* W390 LOC-OWN-PREDICT: im spekulativen Vorlauf des Spiel-Threads (nur Anzeige, wird verworfen) kein Ton, kein Stopp,
 * keine Freigabe, kein Setter (Tabelle las_spec_action, Selbsttest loc_audio_selftest). */
static volatile LONG st_spec_skip;
#define LA_SPEC_GUARD(id) do { if (loc_rb_spec()) { int a_ = las_spec_action(id); if (a_ != LAS_SPEC_ALLOW) {     InterlockedIncrement(&st_spec_skip); return a_ == LAS_SPEC_FAIL ? LA_E_FAIL : 0u; } } } while (0)
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
#define LA_RPC_SLOTS 4 /* W363: Parameter-Namen je Cue */
#define LA_MIN3D 8.0f /* W298 (W): nur noch fuer Eintraege ohne XSB-3D-Satz bzw. loc_audio_3d_xsb=0 (W456: Satz je Eintrag, lxf_cue_t.has3d) */
#define LA_MAX3D 1000.0f
#define LA_DOWNMIX_DB (-770) /* 5.1-Wellen (Stream.xwb): Mischer-Downmix summiert bis x2,9 -> ITU-Norm 1/2,414 */
#define LA_3D_HEADROOM (-600) /* W: 3D-Cues -6 dB (sonst im Gefecht 0,1-2,8 % Uebersteuerung, W298_A2) */
#define LA_WMA_HEADROOM (-600) /* W367 (W): Soundtrack-WMA -6 dB - Quellen bis 0 dBFS, RMS -11 dBFS; damit Pegel wie Titelmusik (-17..-22 dBFS) */
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
    u32 vel[3]; /* W363: Geschwindigkeit (Float-Bits) aus IXACTSoundSource_SetVelocity -> Doppler (audio_doppler) */
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
    /* W363: Laufzeit-Parameter (RPC) je Name (z. B. RPM_VT01) und Spur, aus IXACTEngine_SetParameterControl */
    int nrpc;
    char rpc_name[LA_RPC_SLOTS][16];
    lxf_rpc_t rpc[LA_RPC_SLOTS][LXF_MAX_TRACKS];
    int wma;       /* W367: Spur ist die WMA-Playlist (Player-Thread) */
    long wma_seen; /* W367: bereits gemeldete Liedstarts (je Start Meldung wType 0) */
    u32 serial;    /* W375: laufende Nummer (Rollback-Waise eindeutig, auch wenn der Slot wiederverwendet wird) */
    u32 mgen;      /* W375: in Mission mgen gestartet (0 = ausserhalb) */
    int loops;     /* W375: mindestens eine Spur mit Schleife */
    DWORD t0;      /* W413 DIAG: Anlegezeit (GetTickCount) */
    DWORD t_end;   /* W413: Ende einer endlichen XSB-Schleife (GetTickCount, 0 = keins) */
} la_cue_t;

typedef struct {
    u32 v;      /* Mischer-Stimme */
    int wb;     /* la_wb-Index */
    int wave;
    int busy;
    int rpc_flt; /* W363: Filter aus einem RPC gesetzt (beim naechsten Cue ohne RPC-Filter abschalten) */
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
/* W375 (#51): Missionsgrenze (Lockstep-Aktiv-Maske [0x306830], wie LOCINTERP) und laufende Cue-Nummern */
#define LA_MIS_MASK 0x306830u
static u32 s_serial, s_last_serial, s_last_tid, s_mis_gen, s_mis_mask;
static int s_mis_on;
static lw_pl_t *s_mis_pl; /* W375: Missions-Playlist (Einsatzbeginn FUN_0008C3D0), ihr Release = Missionsende */
static volatile LONG st_mis_stop, st_orphan_stop;
static volatile LONG st_loop_once, st_loop_n, st_loop_n_end; /* W413: XSB-Schleifenzahl: einmal / endlich / endlich beendet */
static volatile LONG st_eg_play, st_eg_rel, st_eg_cut; /* W456: Cues mit Huellkurve / Stopps mit Release-Ausklang / sofort */
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

/* W367 LOC-WMA: Playlists (Handle = Zeiger), Besitzer-Cue des Players, Zaehler */
static lw_pl_t s_pl[LW_MAX_PL];
static la_cue_t *s_wma_cue;
static lw_pl_t *s_wma_pl;
static int s_rs_add;
static volatile LONG st_wma_start, st_wma_notes, st_wma_rs, st_wma_create, st_wma_add, st_wma_next, st_wma_demo;
static int la_is_pl(const void *p)
{
    uintptr_t a = (uintptr_t)p, b0 = (uintptr_t)s_pl;
    return p && a >= b0 && a < b0 + sizeof s_pl && (a - b0) % sizeof s_pl[0] == 0 && ((const lw_pl_t *)p)->kind == LW_K_PL;
}
#define IS_PL(p) la_is_pl(p)

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

/* W379 DIAG (#54, nur loc_audio>=2): Stopp-/Pause-Aufrufe mit Aufrufer, hoechstens 300 Zeilen. */
static volatile LONG st_stoplog;
#define LA_STOPLOG(...)                                                                                              \
    do {                                                                                                             \
        if (g_cfg.loc_audio >= 2 && InterlockedIncrement(&st_stoplog) <= 300) HLOG_I(__VA_ARGS__);                   \
    } while (0)

/* W413 DIAG (loc_sndloop_diag Bit 2): Start von Schleifen-Cues und Stopp-Aufrufe mit Aufrufer/Thread, hoechstens 400 Zeilen. */
static volatile LONG st_sllog;
#define LA_SLLOG(...)                                                                                                    do {                                                                                                                     if ((g_cfg.loc_sndloop_diag & 2) && InterlockedIncrement(&st_sllog) <= 400) HLOG_I(__VA_ARGS__);                  } while (0)

static u32 la_rand(void)
{
    s_rnd ^= s_rnd << 13;
    s_rnd ^= s_rnd >> 17;
    s_rnd ^= s_rnd << 5;
    return s_rnd;
}

/* Streaming: Bytes ab Dateioffset lesen (Abbild oder Host-Datei). */
LONGLONG krnl_loc_io_t0(void);                    /* W449 krnl_file.c: Lesedauer (nur LoC) */
void krnl_loc_io_note(int k, LONGLONG q0, int ok);
void krnl_loc_io_log(int waited_s);

static int la_read_at_raw(u32 xh, uint64_t off, void *buf, u32 len)
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

/* W449 (#85): Streaming-Lesungen mit Dauer zaehlen (langsamer Datentraeger/Virenschutz -> Ton verhungert) */
static int la_read_at(u32 xh, uint64_t off, void *buf, u32 len)
{
    LONGLONG q0 = krnl_loc_io_t0();
    int ok = la_read_at_raw(xh, off, buf, len);
    krnl_loc_io_note(2, q0, ok);
    return ok;
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

/* W363: Summe der RPC-Anteile einer Spur (alle Parameter-Namen des Cues). */
static lxf_rpc_t la_rpc_sum(const la_cue_t *c, int t)
{
    lxf_rpc_t r;
    int i;
    memset(&r, 0, sizeof r);
    for (i = 0; i < c->nrpc && i < LA_RPC_SLOTS; i++) {
        r.vol_mb += c->rpc[i][t].vol_mb;
        r.pitch += c->rpc[i][t].pitch;
        r.filt_hz += c->rpc[i][t].filt_hz;
        r.has |= c->rpc[i][t].has;
    }
    return r;
}

/* W363: Frequenz einer Spur = Wellenrate x 2^(Tonhoehe/4096), Tonhoehe = Eintrag + RPC, geklemmt wie 0x1C437E. */
static u32 la_track_freq(const la_cue_t *c, int t, int rate)
{
    int p = c->c.pitch + la_rpc_sum(c, t).pitch;
    if (p > 0x1FFF) { p = 0x1FFF; }
    if (p < -0x7FFF) { p = -0x7FFF; }
    return p ? (u32)(rate * pow(2.0, p / 4096.0) + 0.5) : (u32)rate;
}

/* W363: Tiefpass aus dem RPC (Ziel 2) wie 0x1C43C2/0x1C9638: Modus 1 (DLS2), c0 = 4096*log2(2*sin(pi*fc/48000))
 * (fc <= 30 -> 0x8000, fc >= 8000 -> 0), Resonanz 0 dB -> c1 = 32768. Ohne RPC-Filter: abschalten, falls eine
 * Pool-Stimme ihn noch vom vorigen Cue traegt. */
static void la_track_filter(la_pool_t *p, const lxf_rpc_t *r)
{
    u32 d[6] = { 0, 0, 0, 0, 0, 0 };
    if (r->has & 4) {
        int fc = r->filt_hz < 0 ? 0 : r->filt_hz > 0x2000 ? 0x2000 : r->filt_hz;
        d[0] = 1;
        d[2] = fc <= 30 ? 0x8000u : fc >= 8000 ? 0u : (u32)((int)(4096.0 * log(2.0 * sin(3.14159265358979 * fc / 48000.0)) / log(2.0)) & 0xFFFF);
        d[3] = 32768u;
        audio_set_filter(p->v, (u32)(uintptr_t)d);
        p->rpc_flt = 1;
    } else if (p->rpc_flt) {
        audio_set_filter(p->v, (u32)(uintptr_t)d);
        p->rpc_flt = 0;
    }
}

static void la_cue_apply(la_cue_t *c)
{
    int t, k = la_cat(c);
    int xs = g_cfg.audio_xact_sends; /* W356: Pegel/Sends je Cue wie die XACT-Bibliothek (0x1C4208) */
    int db = (xs ? c->c.db100_xact : c->c.db100) + s_cat_db[k] + ((c->src && c->src->is3d) ? LA_3D_HEADROOM : 0);
    if (c->wma && c == s_wma_cue) { /* W367: WMA-Stimme gehoert dem Player-Thread (2D, trocken) */
        lw_player_volume(db + LA_WMA_HEADROOM < -10000 ? -10000 : db + LA_WMA_HEADROOM);
    }
    for (t = 0; t < LXF_MAX_TRACKS; t++) {
        la_pool_t *p;
        u32 a[16];
        if (!c->pool[t]) {
            continue;
        }
        p = &s_pool[c->pool[t] - 1];
        {
            lxf_rpc_t rr = la_rpc_sum(c, t);
            int dbt = db + (p->w.ch > 2 ? LA_DOWNMIX_DB : 0);
            if (rr.has & 1) { /* W363: RPC-Lautstaerke zur Eintrags-Lautstaerke, Summe <= 0 (0x1C4208) */
                int base = xs ? c->c.db100_xact : c->c.db100, part = base + rr.vol_mb;
                dbt += (part > 0 ? 0 : part) - base;
            }
            la_track_filter(p, &rr);
            audio_set_volume(p->v, dbt < -10000 ? -10000 : dbt);
        }
        audio_set_ucat(p->v, avol_loc_cat(k)); /* W303: Pool-Stimmen wechseln den Cue -> je Anwendung */
        memset(a, 0, sizeof a);
        {
            float mn = LA_MIN3D, mx = LA_MAX3D;
            if (g_cfg.loc_audio_3d_xsb && c->c.has3d) { /* W456: Min/Max-Abstand des XSB-Eintrags wie die Bibliothek (0x1C4E15) */
                mn = c->c.min3d;
                mx = c->c.max3d;
            }
            memcpy(&a[13], &mn, 4);
            memcpy(&a[14], &mx, 4);
        }
        if (c->src && c->src->is3d) {
            a[1] = c->src->pos[0];
            a[2] = c->src->pos[1];
            a[3] = c->src->pos[2];
            a[4] = c->src->vel[0]; /* W363: Doppler */
            a[5] = c->src->vel[1];
            a[6] = c->src->vel[2];
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
    if (c->wma) { /* W367 */
        if (c == s_wma_cue) {
            lw_player_stop();
            s_wma_cue = NULL;
            s_wma_pl = NULL;
        }
        c->wma = 0;
    }
    for (t = 0; t < LXF_MAX_TRACKS; t++) {
        if (c->pool[t]) {
            la_pool_t *p = &s_pool[c->pool[t] - 1];
            if (p->is_stream) {
                la_pool_clear(p);
            } else {
                if (stop && p->v) { /* W456: stop 2 = Stop ohne Flag 2 -> Release-Huellkurve (sonst sofort) */
                    if (stop == 2 && audio_stop_release(p->v)) {
                        InterlockedIncrement(&st_eg_rel);
                    } else {
                        audio_stop(p->v);
                    }
                }
                p->busy = 0;
            }
            c->pool[t] = 0;
        }
    }
}

/* Cue beenden (gestoppt oder ausgelaufen): Stimmen frei, Meldung, Slot frei. stopped: 0 = ausgelaufen, 1 = sofort
 * gestoppt, 2 = gestoppt mit Release-Huellkurve (W456: Stop ohne Flag 2, Stimme klingt ohne Cue aus). */
static void la_cue_end(la_cue_t *c, int stopped)
{
    if (c->state == 0) {
        return;
    }
    if (c->state == 2 && c->loops && c->sb) { /* W379 DIAG (#54) */
        const char *nm = lxf_xsb_cue_name(&c->sb->x, c->cue);
        LA_STOPLOG("LOCAUDIO ende: Schleifen-Cue %d '%s' Kat=%d %s", c->cue, nm ? nm : "?", la_cat(c),
                   stopped ? "gestoppt" : "ausgelaufen");
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
            if (++s_serial == 0) s_serial = 1; /* W375 */
            c->serial = s_serial;
            c->mgen = s_mis_on ? s_mis_gen : 0u;
            c->t0 = GetTickCount(); /* W413 */
            s_last_serial = s_serial;
            s_last_tid = GetCurrentThreadId();
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
    c->nrpc = 0; /* W363 */
    if (lxf_xsb_cue(&sb->x, cue, la_rand(), &c->c) != 0) {
        InterlockedIncrement(&st_fail);
        c->c.ntrack = 0;
    }
    c->state = 1;
    return 0;
}

/* W367: WMA-Spur eines Cues -> Playlist dieses Cues (zuletzt angelegt) im Player-Thread starten. Unter s_lk. */
static int la_wma_cue_start(la_cue_t *c, int k)
{
    int i, xs = g_cfg.audio_xact_sends, db;
    lw_pl_t *p = NULL;
    for (i = 0; i < LW_MAX_PL; i++) {
        if (s_pl[i].kind == LW_K_PL && s_pl[i].sb == (u32)(uintptr_t)c->sb && s_pl[i].cue == c->cue) {
            p = &s_pl[i];
        }
    }
    if (!p || p->n <= 0) {
        return 0;
    }
    if (s_wma_cue && s_wma_cue != c) { /* ein Player: der vorige Playlist-Cue endet */
        s_wma_cue->wma = 0;
    }
    db = (xs ? c->c.db100_xact : c->c.db100) + s_cat_db[k] + LA_WMA_HEADROOM;
    c->wma_seen = lw_player_starts();
    if (!lw_player_start(p, db < -10000 ? -10000 : db, avol_loc_cat(k), s_cat_pause[k], 0)) {
        return 0;
    }
    c->wma = 1;
    s_wma_cue = c;
    s_wma_pl = p;
    InterlockedIncrement(&st_wma_start);
    return 1;
}

static void la_cues_snapshot(las_cue_t *v);
static void la_cue_end(la_cue_t *c, int stopped);
static volatile LONG st_stream_steal; /* W379 (#54) */

/* W379 (GitHub #54): Eine lange Welle einer Streaming-Wave-Bank (Musik/Ambience, > LA_STREAM_SECS, eigene
 * Stream-Stimme) spielt hoechstens in einem Cue gleichzeitig - startet c sie erneut, enden die alten Instanzen
 * (Stopp-Meldung wie beim Stop). Kurze Wellen (z. B. Effekte aus 'Memory', mehrfach gleichzeitig auf 3D-Quellen)
 * bleiben unberuehrt. Unter s_lk. */
static void la_stream_steal(la_cue_t *c, int wbi, int wave)
{
    las_cue_t v[LA_MAX_CUE];
    int idx[8], n, k;
    if (wbi < 0 || wbi >= LA_MAX_WB || !s_wb[wbi].streamed) return;
    la_cues_snapshot(v);
    n = las_stream_steal_pick(v, LA_MAX_CUE, (int)(c - s_cue), wbi, wave, idx, 8);
    for (k = 0; k < n; k++) {
        la_cue_t *o = &s_cue[idx[k]];
        if (InterlockedIncrement(&st_stream_steal) <= 40) {
            const char *nm = o->sb ? lxf_xsb_cue_name(&o->sb->x, o->cue) : NULL;
            HLOG_I("LOCAUDIO: Streaming-Welle %d ('%s') neu gestartet - alte Instanz (Cue %d '%s') beendet", wave,
                   s_wb[wbi].b.name, o->cue, nm ? nm : "?");
        }
        la_cue_end(o, 1);
    }
}

static void la_cue_start(la_cue_t *c)
{
    int t, k = la_cat(c), nv = 0;
    /* W367: Cue mit gebundener WMA-Playlist (CreateWmaPlayList(Bank, Cue)) spielt die Playlist statt seiner Wellen
     * (ST_CUE zeigt in Bank.xsb auf eine kurze Platzhalter-Welle, belegt im Lauf W367_A1). */
    if (g_cfg.loc_wma && la_wma_cue_start(c, k)) {
        nv = 1;
    }
    for (t = 0; t < c->c.ntrack && !c->wma; t++) {
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
        if (w.loop && g_cfg.loc_audio_loopcount) { /* W413 (#65): Schleifenzahl der XSB statt nur Wellen-Flag */
            int lm = lxf_track_loop_mode(w.loop, c->c.track[t].loopc);
            if (lm == LXF_LOOP_ONCE) {
                w.loop = 0;
                InterlockedIncrement(&st_loop_once);
            } else if (lm == LXF_LOOP_N) { /* n Wiederholungen = n + 1 Durchlaeufe (Annahme), Tonhoehe nicht beruecksichtigt */
                DWORD e = GetTickCount() + (DWORD)(la_wave_secs(&w) * 1000.0 * (c->c.track[t].loopc + 1)) + 1u;
                if (!c->t_end || (int)(e - c->t_end) > 0) c->t_end = e ? e : 1u;
                InterlockedIncrement(&st_loop_n);
            }
        }
        if (la_wave_secs(&w) > LA_STREAM_SECS) la_stream_steal(c, wbi, c->c.track[t].wave); /* W379 (#54) */
        pi = la_wave_secs(&w) > LA_STREAM_SECS ? la_stream_acquire(wbi, c->c.track[t].wave, &w)
                                                : la_pool_acquire(wbi, c->c.track[t].wave, &w);
        if (pi < 0) {
            InterlockedIncrement(&st_fail);
            continue;
        }
        p = &s_pool[pi];
        c->pool[t] = pi + 1;
        nv++;
        if (!p->is_stream) p->w.loop = w.loop; /* W413: wiederverwendete Stimme derselben Welle traegt das alte Flag */
        audio_set_frequency(p->v, la_track_freq(c, t, w.rate)); /* W363: + RPC */
        if (g_cfg.loc_audio_eg) { /* W456: Huellkurve der Sound-Liste (Ereignis 0x0A vor dem Play) bzw. aus (Pool-Stimme) */
            u32 d[10] = { 1u, 0u, 0u, 0u, 0u, 0u, 0u, 255u, 0u, 0u };
            if (c->c.eg_on) {
                int k2;
                for (k2 = 0; k2 < 7; k2++) d[1 + k2] = (u32)c->c.eg[k2];
            }
            audio_set_eg(p->v, d);
        }
        if (p->is_stream) {
            InterlockedIncrement(&st_tracks);
            if (w.loop) {
                InterlockedIncrement(&st_loops);
                c->loops = 1; /* W375 */
            }
            continue;
        }
        audio_set_pos(p->v, 0);
        if (w.loop) {
            audio_set_loop(p->v, w.loop_len ? w.loop_off : 0, w.loop_len ? w.loop_len : w.len);
            InterlockedIncrement(&st_loops);
            c->loops = 1; /* W375 */
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
    if (c->c.eg_on && g_cfg.loc_audio_eg) InterlockedIncrement(&st_eg_play);
    if (g_cfg.loc_audio >= 2 && InterlockedIncrement(&st_cuelog) <= 400) {
        const char *nm = lxf_xsb_cue_name(&c->sb->x, c->cue);
        HLOG_I("LOCAUDIO cue %d '%s' Spuren=%d Stimmen=%d Kat=%d dB=%d Pitch=%d Quelle=%s v=%u Pegel=%d EG=%d/%d/%d Min3D=%.0f", c->cue,
               nm ? nm : "?", c->c.ntrack, nv, k, c->c.db100, c->c.pitch, c->src ? (c->src->is3d ? "3D" : "2D") : "-",
               c->pool[0] ? audio_voice_id(s_pool[c->pool[0] - 1].v) : 0u, /* W456 DIAG: voice_levels.csv */
               g_cfg.audio_xact_sends ? c->c.db100_xact : c->c.db100, c->c.eg_on ? c->c.eg[2] : -1, c->c.eg_on ? c->c.eg[5] : -1,
               c->c.eg_on ? c->c.eg[6] : -1, (g_cfg.loc_audio_3d_xsb && c->c.has3d) ? c->c.min3d : LA_MIN3D);
    }
}

static void la_cues_snapshot(las_cue_t *v)
{
    int i, t;
    for (i = 0; i < LA_MAX_CUE; i++) {
        v[i].state = s_cue[i].state;
        v[i].loops = s_cue[i].loops;
        v[i].wma = s_cue[i].wma;
        v[i].serial = s_cue[i].serial;
        v[i].mgen = s_cue[i].mgen;
        for (t = 0; t < LAS_TRACKS && t < LXF_MAX_TRACKS; t++) { /* W379 (#54) */
            const la_pool_t *p = s_cue[i].pool[t] ? &s_pool[s_cue[i].pool[t] - 1] : NULL;
            int sw = p && p->is_stream && p->wb >= 0 && p->wb < LA_MAX_WB && s_wb[p->wb].streamed;
            v[i].swb[t] = sw ? p->wb + 1 : 0;
            v[i].swave[t] = sw ? p->wave : 0;
        }
    }
}

/* W375 (#51): Missionsende: alle noch spielenden Schleifen-Cues dieser Mission beenden (Spiel stoppt z. B. den
 * Eject-Alarm nicht, wenn die Mission mitten in der Warnung endet bzw. sein Handle nach einem Rollback nur in der
 * verworfenen Zeitlinie stand). why = Ausloeser fuer das Log. Unter s_lk. */
static void la_mission_end(const char *why)
{
    las_cue_t v[LA_MAX_CUE];
    int idx[LA_MAX_CUE], n, k, act = 0;
    char names[200] = "";
    size_t o = 0;
    if (!s_mis_on) return;
    la_cues_snapshot(v);
    n = las_mission_end_pick(v, LA_MAX_CUE, s_mis_gen, idx, LA_MAX_CUE);
    for (k = 0; k < n; k++) {
        la_cue_t *c = &s_cue[idx[k]];
        const char *nm = c->sb ? lxf_xsb_cue_name(&c->sb->x, c->cue) : NULL;
        if (o + 24 < sizeof names) o += (size_t)_snprintf_s(names + o, sizeof names - o, _TRUNCATE, "%s%d '%s'",
                                                             k ? ", " : "", c->cue, nm ? nm : "?");
        la_cue_end(c, 1);
        InterlockedIncrement(&st_mis_stop);
    }
    for (k = 0; k < LA_MAX_CUE; k++) act += s_cue[k].state == 2;
    s_mis_on = 0;
    s_mis_pl = NULL;
    HLOG_I("LOCAUDIO: Missionsende (Mission %u, %s): %d Schleifen-Cue(s) der Mission gestoppt%s%s%s, danach aktiv %d",
           s_mis_gen, why, n, n ? " [" : "", names, n ? "]" : "", act);
}

/* W375: Missionsbeginn. Unter s_lk. */
static void la_mission_begin(void)
{
    s_mis_gen++;
    s_mis_on = 1;
    s_mis_pl = NULL;
}

/* W375 (#51): Missionsgrenze an der Lockstep-Aktiv-Maske [0x306830] (0 -> gesetzt = Beginn, gesetzt -> 0 = Ende).
 * Die Maske bleibt nach TIME UP bis in die Nachbesprechung gesetzt; frueher meldet das Spiel das Ende ueber die
 * Missions-Playlist (FUN_0008D8D0: Stop(ST_CUE), Release - la_WmaRelease). Unter s_lk. */
static void la_mission_poll(void)
{
    u32 m = 0;
    int e;
    __try { m = *(volatile const u32 *)(uintptr_t)LA_MIS_MASK; } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    e = las_mission_edge(s_mis_mask, m);
    s_mis_mask = m;
    if (e == LAS_EDGE_BEGIN) {
        la_mission_begin();
    } else if (e == LAS_EDGE_END) {
        la_mission_end("Lockstep-Maske leer");
    }
}

/* W375: laufende Nummer des zuletzt von diesem Thread angelegten Cues (direkt nach PlayEx/PrepareEx), 0 = keine. */
u32 loc_audio_last_serial(void)
{
    u32 s = 0;
    if (!s_active) return 0;
    lk();
    if (s_last_tid == GetCurrentThreadId()) s = s_last_serial;
    ulk();
    return s;
}

/* W375: Rollback-Waise (nur die verworfene Zeitlinie hat den Cue gestartet): spielt er noch als Schleife, beenden.
 * 1 = gestoppt. Einmal-Cues laufen aus (wie bisher). */
int loc_audio_stop_orphan(u32 serial)
{
    las_cue_t v[LA_MAX_CUE];
    int i;
    if (!s_active || !serial) return 0;
    lk();
    la_cues_snapshot(v);
    i = las_orphan_pick(v, LA_MAX_CUE, serial);
    if (i >= 0) {
        la_cue_end(&s_cue[i], 1);
        InterlockedIncrement(&st_orphan_stop);
    }
    ulk();
    return i >= 0;
}

/* W413 LOC-SOUND-LOOP (#65): Cue-Slot zur laufenden Nummer (nur belegte Slots), sonst NULL. Unter s_lk. */
static la_cue_t *la_by_serial(u32 serial)
{
    int i;
    if (!serial) return NULL;
    for (i = 0; i < LA_MAX_CUE; i++)
        if (s_cue[i].state != 0 && s_cue[i].serial == serial) return &s_cue[i];
    return NULL;
}

/* W413: Auskunft zur laufenden Nummer. Rueckgabe Zustand (0 = frei/unbekannt, 1 = vorbereitet, 2 = spielt). */
int loc_audio_cue_info(unsigned serial, int *cue, int *loops, unsigned *src, char *name, int cap)
{
    la_cue_t *c;
    int st = 0;
    if (cue) *cue = -1;
    if (loops) *loops = 0;
    if (src) *src = 0;
    if (name && cap > 0) name[0] = 0;
    if (!s_active || !serial) return 0;
    lk();
    c = la_by_serial(serial);
    if (c) {
        st = c->state;
        if (cue) *cue = c->cue;
        if (loops) *loops = c->loops;
        if (src) *src = (unsigned)(uintptr_t)c->src;
        if (name && cap > 0) {
            const char *nm = c->sb ? lxf_xsb_cue_name(&c->sb->x, c->cue) : NULL;
            strncpy_s(name, (size_t)cap, nm ? nm : "?", _TRUNCATE);
        }
    }
    ulk();
    return st;
}

/* W413: Cue zur laufenden Nummer beenden (wie Stop per Handle). only_loops: nur spielende Schleifen-Cues (ohne WMA).
 * 1 = gestoppt. */
int loc_audio_stop_serial(unsigned serial, int only_loops)
{
    la_cue_t *c;
    int ok = 0;
    if (!s_active || !serial) return 0;
    lk();
    c = la_by_serial(serial);
    if (c && (!only_loops || (c->state == 2 && c->loops && !c->wma))) {
        la_cue_end(c, 1);
        ok = 1;
    }
    ulk();
    return ok;
}

/* W413 DIAG: Zahl der noch spielenden Schleifen-Cues (ohne WMA); Namen mit Alter (s) nach buf. */
int loc_audio_loops_active(char *buf, int cap)
{
    int i, n = 0;
    size_t o = 0;
    DWORD now = GetTickCount();
    if (buf && cap > 0) buf[0] = 0;
    if (!s_active) return 0;
    lk();
    for (i = 0; i < LA_MAX_CUE; i++) {
        const la_cue_t *c = &s_cue[i];
        if (c->state != 2 || !c->loops || c->wma) continue;
        n++;
        if (buf && cap > 0 && o + 40 < (size_t)cap) {
            const char *nm = c->sb ? lxf_xsb_cue_name(&c->sb->x, c->cue) : NULL;
            o += (size_t)_snprintf_s(buf + o, (size_t)cap - o, _TRUNCATE, "%s%d '%s' %lus", n > 1 ? ", " : "", c->cue,
                                     nm ? nm : "?", (unsigned long)((now - c->t0) / 1000u));
        }
    }
    ulk();
    return n;
}

/* Ausgelaufene Cues erkennen (aus DoWork/GetNotification). Unter s_lk. */
static void la_pump(void)
{
    int i, t;
    la_mission_poll(); /* W375 */
    for (i = 0; i < LA_MAX_CUE; i++) {
        la_cue_t *c = &s_cue[i];
        int playing = 0;
        if (c->state != 2 || c->paused) {
            continue;
        }
        if (c->t_end && (int)(GetTickCount() - c->t_end) >= 0) { /* W413: endliche XSB-Schleife durchgelaufen */
            InterlockedIncrement(&st_loop_n_end);
            la_cue_end(c, 0);
            continue;
        }
        if (c->wma && c == s_wma_cue && lw_player_active()) { /* W367: Playlist laeuft; je Liedstart Meldung wType 0 */
            long st = lw_player_starts();
            playing = 1;
            while (c->wma_seen < st) {
                la_note_push(0, c);
                c->wma_seen++;
                InterlockedIncrement(&st_wma_notes);
            }
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
    loc_api_count(4); /* W363 */ LA_SPEC_GUARD(4);
    lk();
    la_pump();
    ulk();
    return 0;
}

static u32 __stdcall la_XACTEngineCreate(u32 params, u32 pp)
{
    loc_api_count(5); /* W363 */ LA_SPEC_GUARD(5);
    (void)params;
    la_first(0, "XACTEngineCreate");
    s_eng.kind = LA_K_ENG;
    la_out(pp, (u32)(uintptr_t)&s_eng);
    return 0;
}

static u32 __stdcall la_CreateSoundSource(u32 e, u32 flags, u32 pp)
{
    loc_api_count(6); /* W363 */ LA_SPEC_GUARD(6);
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
    loc_api_count(7); /* W363 */ LA_SPEC_GUARD(7);
    int i, rc;
    (void)e;
    if (!data || size < 0x38 || size > 16u * 1024u * 1024u) {
        static LONG said; /* W423: abgelehnte Sound-Bank einmal klar melden (Spiel bricht die Ton-Init dann ab) */
        if (!InterlockedExchange(&said, 1)) {
            HLOG_W("LOCAUDIO: CreateSoundBank abgelehnt (Daten 0x%X, %u B) -> E_FAIL", data, size);
        }
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
    loc_api_count(8); /* W363 */ LA_SPEC_GUARD(8);
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
    loc_api_count(9); /* W363 */ LA_SPEC_GUARD(9);
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
    loc_api_count(10); /* W363 */ LA_SPEC_GUARD(10);
    la_wb_t *wb = (la_wb_t *)(uintptr_t)wbp;
    int wbi, i, t;
    (void)e;
    lk();
    if (!IS_WB(wb) || !wb->used) {
        ulk();
        return 0;
    }
    wbi = (int)(wb - s_wb);
    LA_STOPLOG("LOCAUDIO stop: UnRegisterWaveBank('%s') Aufrufer 0x%X", wb->b.name, (u32)(uintptr_t)_ReturnAddress());
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
    loc_api_count(11); /* W363 */ LA_SPEC_GUARD(11);
    static volatile LONG n;
    (void)e;
    if (cat >= LA_MAX_CAT) {
        return 0;
    }
    if (InterlockedIncrement(&n) <= 24) {
        HLOG_I("LOCAUDIO: SetMasterVolume Kategorie %u = %d/100 dB", cat, (int)vol);
    } else if (g_cfg.loc_audio >= 2 && (int)vol != s_cat_db[cat]) { /* W456 DIAG: weitere Wertwechsel mit Aufrufer */
        static volatile LONG nd;
        if (InterlockedIncrement(&nd) <= 300)
            HLOG_I("LOCAUDIO: SetMasterVolume Kategorie %u = %d/100 dB (Wechsel, Aufrufer 0x%X)", cat, (int)vol,
                   (u32)(uintptr_t)_ReturnAddress());
    }
    lk();
    s_cat_db[cat] = (int)vol < -10000 ? -10000 : ((int)vol > 0 ? 0 : (int)vol);
    la_cat_update((int)cat);
    ulk();
    return 0;
}

static u32 __stdcall la_GlobalPause(u32 e, u32 cat, u32 pause)
{
    loc_api_count(16); /* W363 */ LA_SPEC_GUARD(16);
    int i, t;
    (void)e;
    la_first(1, "IXACTEngine_GlobalPause");
    LA_STOPLOG("LOCAUDIO stop: GlobalPause(Kategorie %u, %u) Aufrufer 0x%X", cat, pause, (u32)(uintptr_t)_ReturnAddress());
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
        if (c->wma && c == s_wma_cue) { /* W367 */
            lw_player_pause(pause ? 1 : 0);
        }
        c->paused = pause ? 1 : 0;
    }
    ulk();
    return 0;
}

static u32 __stdcall la_SetListenerOrientation(u32 e, u32 fx, u32 fy, u32 fz, u32 tx, u32 ty, u32 tz, u32 apply)
{
    loc_api_count(13); /* W363 */ LA_SPEC_GUARD(13);
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
    loc_api_count(12); /* W363 */ LA_SPEC_GUARD(12);
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
    loc_api_count(14); /* W363 */ LA_SPEC_GUARD(14);
    (void)e;
    (void)apply;
    audio_3d_listener_pos(x, y, z);
    return 0;
}

/* desc: {wType|wFlags<<16, Bank, Cue, Cue-Handle, Kontext, Event} */
static u32 __stdcall la_RegisterNotification(u32 e, u32 pdesc)
{
    loc_api_count(17); /* W363 */ LA_SPEC_GUARD(17);
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
    loc_api_count(18); /* W363 */ LA_SPEC_GUARD(18);
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
    loc_api_count(20); /* W363 */ LA_SPEC_GUARD(20);
    (void)e;
    (void)on;
    la_first(2, "IXACTEngine_EnableHeadphones");
    return 0;
}

static u32 __stdcall la_SetParameterControl(u32 e, u32 p)
{
    loc_api_count(21); /* W363 */ LA_SPEC_GUARD(21);
    (void)e;
    if (g_cfg.audio_api_diag) { /* W363 DIAG: Aufbau des Parameters (erste 12 Aufrufe, dann jeder 500.) */
        static LONG n;
        LONG k = InterlockedIncrement(&n);
        if (k <= 12 || k % 500 == 0) {
            u32 d[8];
            memset(d, 0, sizeof d);
            if (!la_rd(p, d, sizeof d)) { d[0] = 0xDEADu; }
            HLOG_I("LOCAPI rpc #%ld e=0x%X p=0x%X: %08X %08X %08X %08X %08X %08X %08X %08X (f1=%.3f f2=%.3f)", (long)k, e, p,
                   d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], *(float *)&d[1], *(float *)&d[2]);
        }
    }
    la_first(3, "IXACTEngine_SetParameterControl (RPC: Kurven aus Bank.xsb, W363)");
    if (g_cfg.loc_xact_rpc) { /* W363 LOC-ORIGINAL: Desc {Anzahl, Elemente[24 B]}; Element {u16 Art (1 = an Cue),
                               * u16, u16, u16 Schritte, f32 Steigung, f32 Wert, Cue, Name} (0x1C0574/0x1C042F;
                               * LoC: Steigung/Schritte 0 -> Wert gilt sofort) */
        u32 hd[2], el[6];
        u32 i;
        if (!la_rd(p, hd, sizeof hd) || hd[0] > 64u) {
            return 0;
        }
        for (i = 0; i < hd[0]; i++) {
            char nm[17];
            la_cue_t *c;
            float val;
            if (!la_rd(hd[1] + i * 24u, el, sizeof el) || !(el[0] & 1u) || el[5] < 0x10000u) {
                continue; /* nur an Cues gebundene Parameter mit Namen (LoC ruft nur diese) */
            }
            memset(nm, 0, sizeof nm);
            if (!la_rd(el[5], nm, 16)) {
                continue;
            }
            memcpy(&val, &el[3], 4);
            c = (la_cue_t *)(uintptr_t)el[4];
            lk();
            if (IS_CUE(c) && c->state >= 1 && c->sb) {
                int slot, t, hits;
                lxf_rpc_t r[LXF_MAX_TRACKS];
                for (slot = 0; slot < c->nrpc && strncmp(c->rpc_name[slot], nm, 16) != 0; slot++) {}
                hits = lxf_xsb_rpc(&c->sb->x, c->c.entry, nm, val, r);
                if (hits && slot < LA_RPC_SLOTS) {
                    if (slot == c->nrpc) {
                        memcpy(c->rpc_name[slot], nm, 16);
                        c->nrpc++;
                    }
                    memcpy(c->rpc[slot], r, sizeof r);
                    st_rpc++;
                    if (c->state == 2) {
                        la_cue_apply(c); /* Pegel + Filter */
                        for (t = 0; t < LXF_MAX_TRACKS; t++) {
                            if (c->pool[t]) {
                                la_pool_t *pp = &s_pool[c->pool[t] - 1];
                                audio_set_frequency(pp->v, la_track_freq(c, t, pp->w.rate));
                            }
                        }
                    }
                } else if (!hits) {
                    st_rpc_none++;
                }
            }
            ulk();
        }
    }
    return 0;
}

static u32 __stdcall la_GetSoundCueIndexFromFriendlyName(u32 sbp, u32 name, u32 pidx)
{
    loc_api_count(22); /* W363 */ LA_SPEC_GUARD(22);
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
    loc_api_count(23); /* W363 */ LA_SPEC_GUARD(23);
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

/* W456 DIAG (loc_audio>=2): Belegung der Ton-Slot-Tabelle des Spiels (0x5765B0, 0xC0 Slots je 0x5C B; FUN_000890B0
 * verwirft einen Ton still, wenn kein Slot frei ist: frei = [3] == 0 und [2] == -1). */
static volatile LONG st_slot_max;
static void la_slot_diag(void)
{
    static u32 tab[0xC0 * 0x17];
    int i, used = 0;
    if (g_cfg.loc_audio < 2 || !la_rd(0x5765B0u, tab, sizeof tab))
        return;
    for (i = 0; i < 0xC0; i++)
        used += tab[i * 0x17 + 3] != 0 || tab[i * 0x17 + 2] != 0xFFFFFFFFu;
    if (used > st_slot_max)
        st_slot_max = used;
}

static u32 __stdcall la_PlayEx(u32 sbp, u32 params, u32 pp)
{
    loc_api_count(24); /* W363 */ LA_SPEC_GUARD(24);
    la_sb_t *sb = (la_sb_t *)(uintptr_t)sbp;
    u32 p[3];
    la_cue_t *c = NULL;
    la_src_t *src;
    if (!la_rd(params, p, sizeof p)) {
        return LA_E_FAIL;
    }
    src = IS_SRC((void *)(uintptr_t)p[2]) ? (la_src_t *)(uintptr_t)p[2] : NULL;
    la_slot_diag(); /* W456 DIAG */
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
    if (c->loops) LA_SLLOG("LOCSNDLOOP play: Cue %d '%s' Nr %u Flags 0x%X Quelle %d pp 0x%X Aufrufer 0x%X Thread %lu", c->cue,
                           lxf_xsb_cue_name(&c->sb->x, c->cue) ? lxf_xsb_cue_name(&c->sb->x, c->cue) : "?", c->serial, p[0],
                           src ? (int)(src - s_src) : -1, pp, (u32)(uintptr_t)_ReturnAddress(), GetCurrentThreadId());
    ulk();
    if (pp && !(p[0] & 1u)) {
        la_out(pp, (u32)(uintptr_t)c);
    }
    return 0;
}

static u32 __stdcall la_Stop(u32 sbp, u32 cue, u32 flags, u32 pcue)
{
    loc_api_count(25); /* W363 */ LA_SPEC_GUARD(25);
    int i;
    (void)sbp;
    LA_STOPLOG("LOCAUDIO stop: Stop(Cue %d, Flags 0x%X, Handle %s) Aufrufer 0x%X", (int)cue, flags,
               IS_CUE((void *)(uintptr_t)pcue) ? "gueltig" : (pcue ? "unbekannt" : "-"), (u32)(uintptr_t)_ReturnAddress());
    lk();
    if (g_cfg.loc_sndloop_diag & 2) {
        const la_cue_t *t = IS_CUE((void *)(uintptr_t)pcue) ? (const la_cue_t *)(uintptr_t)pcue : NULL;
        LA_SLLOG("LOCSNDLOOP stop: Stop(Cue %d, Flags 0x%X, Handle %s Nr %u Ziel %d Zustand %d Schleife %d) Aufrufer 0x%X Thread %lu",
                 (int)cue, flags, t ? "gueltig" : (pcue ? "unbekannt" : "-"), t ? t->serial : 0u, t ? t->cue : -1,
                 t ? t->state : -1, t ? t->loops : 0, (u32)(uintptr_t)_ReturnAddress(), GetCurrentThreadId());
    }
    {
        /* W456: Bibliothek (0x1C7E79 -> 0x1C531E) setzt Flag 1 dazu; ohne Flag 2 (sofort) klingen die Stimmen ueber die
         * Release-Huellkurve aus (StopEx DSBSTOPEX_ENVELOPE), ohne Huellkurve endet die Stimme wie bisher sofort. */
        int how = (!g_cfg.loc_audio_eg || (flags & 2u)) ? 1 : 2;
        if (how == 1) InterlockedIncrement(&st_eg_cut);
        if ((int)cue >= 0) {
            for (i = 0; i < LA_MAX_CUE; i++) {
                if (s_cue[i].state != 0 && s_cue[i].cue == (int)cue) {
                    la_cue_end(&s_cue[i], how);
                }
            }
        } else if (IS_CUE((void *)(uintptr_t)pcue)) {
            la_cue_end((la_cue_t *)(uintptr_t)pcue, how);
        }
    }
    ulk();
    return 0;
}

static u32 __stdcall la_SourceRelease(u32 sp)
{
    loc_api_count(32); /* W363 */ LA_SPEC_GUARD(32);
    la_src_t *s = (la_src_t *)(uintptr_t)sp;
    int i;
    LA_STOPLOG("LOCAUDIO stop: SourceRelease(Quelle %d) Aufrufer 0x%X", IS_SRC(s) ? (int)(s - s_src) : -1,
               (u32)(uintptr_t)_ReturnAddress());
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
    loc_api_count(33); /* W363 */ LA_SPEC_GUARD(33);
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

/* W363 LOC-ORIGINAL: Doppler wie die XACT-Bibliothek (DS3D je 3D-Stimme). Bisher Stub (loc_boot), 178 Aufrufe je
 * Missionsminute (immer zusammen mit SetPosition). Geschwindigkeit je Quelle merken und auf laufende Stimmen geben;
 * die Formel (Radialanteil, Klemme) rechnet audio_3d.c wie fuer SB (audio_doppler, W348). */
static u32 __stdcall la_SourceSetVelocity(u32 sp, u32 x, u32 y, u32 z, u32 apply)
{
    la_src_t *s = (la_src_t *)(uintptr_t)sp;
    int i, t;
    loc_api_count(35); LA_SPEC_GUARD(35);
    (void)apply;
    if ((x | y | z) & 0x7FFFFFFFu) { /* W363 DIAG: Anteil echter Geschwindigkeiten, groesster Betrag */
        float fx = *(float *)&x, fy = *(float *)&y, fz = *(float *)&z, m = fx * fx + fy * fy + fz * fz;
        s_vel_nz++;
        if (m == m && m > s_vel_max2) { s_vel_max2 = m; }
    }
    lk();
    if (IS_SRC(s) && s->used) {
        s->vel[0] = x;
        s->vel[1] = y;
        s->vel[2] = z;
        if (s->is3d) {
            for (i = 0; i < LA_MAX_CUE; i++) {
                if (s_cue[i].state == 2 && s_cue[i].src == s) {
                    for (t = 0; t < LXF_MAX_TRACKS; t++) {
                        if (s_cue[i].pool[t]) {
                            audio_3d_voice_vel(s_pool[s_cue[i].pool[t] - 1].v, x, y, z);
                        }
                    }
                }
            }
        }
    }
    ulk();
    return 0;
}

static u32 __stdcall la_SetListenerVelocity(u32 e, u32 x, u32 y, u32 z, u32 apply)
{
    loc_api_count(15); LA_SPEC_GUARD(15);
    (void)e;
    (void)apply;
    audio_3d_listener_vel(x, y, z);
    return 0;
}

static u32 __stdcall la_SourceSetMixBins(u32 sp, u32 pmb)
{
    loc_api_count(36); /* W363 */ LA_SPEC_GUARD(36);
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
    la_src_t *s = (la_src_t *)(uintptr_t)sp;
    u32 hdr[2], pair[2], i;
    loc_api_count(37); /* W363 */ LA_SPEC_GUARD(37);
    la_first(4, "IXACTSoundSource_SetMixBinVolumes (W363: Pegel der genannten Bins)");
    if (!la_rd(pmb, hdr, sizeof hdr) || hdr[0] > 16) {
        return 0;
    }
    lk();
    if (IS_SRC(s) && s->used && s->has_pan) { /* W363: DSound-Regel - nur die genannten Bins aendern, Rest bleibt */
        float g[6];
        int k;
        memcpy(g, s->bin, sizeof g);
        for (i = 0; i < hdr[0]; i++) {
            if (la_rd(hdr[1] + i * 8, pair, sizeof pair) && pair[0] < 6) {
                int db = (int)pair[1];
                g[pair[0]] = db <= -10000 ? 0.0f : (float)pow(10.0, (db > 0 ? 0 : db) / 2000.0);
                if (g_cfg.audio_api_diag) {
                    static LONG nl;
                    if (InterlockedIncrement(&nl) <= 16) { HLOG_I("LOCAPI mixbinvol src=0x%X bin=%u mB=%d", sp, pair[0], db); }
                }
            }
        }
        memcpy(s->bin, g, sizeof g);
        s->pan_l = g[0] + 0.7071f * g[2] + 0.5f * g[3] + 0.5f * g[4];
        s->pan_r = g[1] + 0.7071f * g[2] + 0.5f * g[3] + 0.5f * g[5];
        for (k = 0; k < LA_MAX_CUE; k++) {
            if (s_cue[k].state == 2 && s_cue[k].src == s) {
                la_cue_apply(&s_cue[k]);
            }
        }
    }
    ulk();
    return 0;
}

/* W356 AUDIO-2: IXACTSoundSource_SetI3DL2Source(src, DSI3DL2BUFFER*, apply). Das Spiel (FUN_00088E80)
 * setzt allen 64 3D-Quellen nur die Occlusion aus einer Material-Tabelle (0x274460, XDK-Presets
 * Fenster/Tuer/Wand ...), alles andere 0. Wirkung je Stimme im Mischer (audio_occlusion). */
static LONG st_i3d;
static u32 __stdcall la_SourceSetI3DL2Source(u32 sp, u32 p, u32 apply)
{
    loc_api_count(34); /* W363 */ LA_SPEC_GUARD(34);
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
    loc_api_count(38); /* W363 */ LA_SPEC_GUARD(38);
    la_src_t *s = (la_src_t *)(uintptr_t)sp;
    int i;
    LA_STOPLOG("LOCAUDIO stop: SourceStopSoundCues(Quelle %d) Aufrufer 0x%X", IS_SRC(s) ? (int)(s - s_src) : -1,
               (u32)(uintptr_t)_ReturnAddress());
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
    loc_api_count(39); /* W363 */ LA_SPEC_GUARD(39);
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

/* ---------------- W367 LOC-WMA: IXACTWmaPlayList ---------------- */
/* Soundtrack-Einstellung im Spielspeicher (LoC-XBE, Ghidra gelesen): 0x57B529 an/aus, 0x57B52A Modus (0 Reihe, 1 Zufall),
 * 0x57B524 Anzahl, 0x57B538 Eintraege je 12 Bytes {Soundtrack-ID, Lied-ID, ?, Lautstaerke, Spiel-Lied (0xFFFF = HDD)},
 * 0x57B9DC Menue-Puffer (!= 0 nur im Soundtrack-Menue). */
#define LW_VA_ON 0x57B529u
#define LW_VA_MODE 0x57B52Au
#define LW_VA_COUNT 0x57B524u
#define LW_VA_ENTRIES 0x57B538u
#define LW_VA_MENU 0x57B9DCu

/* loc_wma_demo=1 (Test/Experimental): leeren Einsatz-Soundtrack mit den beiden Spiel-Liedern vorbelegen, als haette der
 * Spieler sie im Optionsmenue gewaehlt (der Reihe nach, volle Lautstaerke). Nur beim Einsatzbeginn (kein Menue-Puffer). */
static void la_wma_demo_fill(void)
{
    u32 cnt = 0, menu = 1;
    if (!la_rd(LW_VA_COUNT, &cnt, 4) || !la_rd(LW_VA_MENU, &menu, 4) || cnt != 0 || menu != 0) {
        return;
    }
    __try {
        static const uint8_t ent[24] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 1, 0 };
        LOC_RB_PREWRITE(LW_VA_ENTRIES, sizeof ent);
        memcpy((void *)(uintptr_t)LW_VA_ENTRIES, ent, sizeof ent);
        LOC_RB_PREWRITE(LW_VA_COUNT, 4);
        *(volatile u32 *)(uintptr_t)LW_VA_COUNT = 2;
        LOC_RB_PREWRITE(LW_VA_ON, 2);
        *(volatile uint8_t *)(uintptr_t)LW_VA_MODE = 0;
        *(volatile uint8_t *)(uintptr_t)LW_VA_ON = 1;
        InterlockedIncrement(&st_wma_demo);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void la_wma_log(const char *what, const lw_pl_t *p, u32 a, u32 b)
{
    static volatile LONG n;
    if (InterlockedIncrement(&n) <= 64) {
        HLOG_I("LOCWMA: %s Liste %d (Cue %d, Flags %u, Lieder %d, aktuell %d) %u %u%s", what,
               p ? (int)(p - s_pl) : -1, p ? p->cue : -1, p ? p->flags : 0, p ? p->n : 0, p ? p->cur : -1, a, b,
               loc_rb_resim() ? " [Neurechnen: ohne Wirkung]" : "");
    }
}

static u32 __stdcall la_CreateWmaPlayList(u32 sb, u32 cue, u32 flags, u32 pp)
{
    int i;
    lw_pl_t *p = NULL;
    loc_api_count(26); LA_SPEC_GUARD(26);
    lk();
    if (loc_rb_resim()) { /* Neurechnen: dieselbe (zuletzt live angelegte) Liste, nichts neu anlegen */
        for (i = 0; i < LW_MAX_PL; i++) {
            if (s_pl[i].kind == LW_K_PL && s_pl[i].sb == sb && s_pl[i].cue == (int)cue) {
                p = &s_pl[i];
            }
        }
        s_rs_add = 0;
        InterlockedIncrement(&st_wma_rs);
    } else {
        for (i = 0; i < LW_MAX_PL && !p; i++) {
            if (s_pl[i].kind != LW_K_PL) {
                p = &s_pl[i];
            }
        }
        if (p) {
            lw_pl_init(p, sb, (int)cue, flags);
            InterlockedIncrement(&st_wma_create);
            if (s_mis_mask) { /* W375: Einsatzbeginn (Maske steht) - Missions-Playlist merken */
                if (!s_mis_on) la_mission_begin(); /* Maske blieb seit der letzten Mission gesetzt */
                s_mis_pl = p;
            }
        }
    }
    if (g_cfg.loc_wma_demo) {
        la_wma_demo_fill();
    }
    la_wma_log("Create", p, cue, flags);
    ulk();
    if (!p) {
        return LA_E_FAIL;
    }
    la_out(pp, (u32)(uintptr_t)p);
    return 0;
}

static u32 __stdcall la_WmaRelease(u32 pl)
{
    lw_pl_t *p = (lw_pl_t *)(uintptr_t)pl;
    loc_api_count(27); LA_SPEC_GUARD(27);
    lk();
    if (IS_PL(p) && !loc_rb_resim()) {
        if (s_wma_pl == p) {
            lw_player_stop();
            if (s_wma_cue) {
                s_wma_cue->wma = 0; /* Cue endet beim naechsten DoWork (Stopp-Meldung wie bisher) */
            }
            s_wma_cue = NULL;
            s_wma_pl = NULL;
        }
        la_wma_log("Release", p, 0, 0);
        if (s_mis_on && p == s_mis_pl) {
            la_mission_end("Missions-Playlist freigegeben"); /* W375: FUN_0008D8D0 */
        }
        p->kind = 0;
    } else if (IS_PL(p)) {
        InterlockedIncrement(&st_wma_rs);
    }
    ulk();
    return 0;
}

/* Eintrag (am Spiel belegt): {dwType, Pfad (char*), Soundtrack-ID, Lied-ID, Index}; Ausgabe: Lied-ID */
static u32 __stdcall la_WmaAdd(u32 pl, u32 pent, u32 pid)
{
    lw_pl_t *p = (lw_pl_t *)(uintptr_t)pl;
    u32 e[5], id = 0;
    char path[LW_PATH];
    int rc = 0;
    loc_api_count(28); LA_SPEC_GUARD(28);
    memset(e, 0, sizeof e);
    path[0] = '\0';
    if (!la_rd(pent, e, sizeof e)) {
        return LA_E_FAIL;
    }
    if (e[0] == LW_ENTRY_FILE && e[1]) {
        __try {
            strncpy_s(path, sizeof path, (const char *)(uintptr_t)e[1], _TRUNCATE);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            path[0] = '\0';
        }
    }
    lk();
    if (!IS_PL(p)) {
        rc = -1;
    } else if (loc_rb_resim()) {
        id = LW_SONG_ID_BASE + (u32)s_rs_add++;
        InterlockedIncrement(&st_wma_rs);
    } else {
        rc = lw_pl_add(p, e[0], path, e[2], e[3], &id);
        InterlockedIncrement(&st_wma_add);
        la_wma_log(path[0] ? path : "Add (Soundtrack der Festplatte)", p, e[0], id);
    }
    ulk();
    if (rc != 0) {
        return LA_E_FAIL;
    }
    la_out(pid, id);
    return 0;
}

static u32 __stdcall la_WmaNext(u32 pl)
{
    lw_pl_t *p = (lw_pl_t *)(uintptr_t)pl;
    loc_api_count(29); LA_SPEC_GUARD(29);
    lk();
    if (IS_PL(p) && !loc_rb_resim()) {
        lw_pl_next(p);
        InterlockedIncrement(&st_wma_next);
        if (s_wma_pl == p && s_wma_cue && lw_player_active()) {
            lw_player_jump(p); /* Taste "naechstes Lied" waehrend der Wiedergabe */
        }
        la_wma_log("Next", p, 0, 0);
    } else if (IS_PL(p)) {
        InterlockedIncrement(&st_wma_rs);
    }
    ulk();
    return 0;
}

/* GetCurrentSongInfo(Liste, 0, Name (WCHAR), Zeichen, &Lied-ID) - am Aufrufer FUN_0008D200 belegt */
static u32 __stdcall la_WmaGetCurrentSongInfo(u32 pl, u32 a2, u32 pname, u32 cch, u32 pid)
{
    lw_pl_t *p = (lw_pl_t *)(uintptr_t)pl;
    int song = -1;
    u32 id = 0;
    loc_api_count(30); LA_SPEC_GUARD(30);
    (void)a2;
    lk();
    if (IS_PL(p)) {
        song = (s_wma_pl == p && lw_player_song() >= 0) ? lw_player_song() : p->cur;
        if (song >= 0 && song < p->n) {
            id = p->e[song].id;
        } else {
            song = -1;
        }
    }
    ulk();
    if (song < 0) {
        return LA_E_FAIL;
    }
    if (pname && cch) {
        la_out(pname, 0); /* leerer Titel (Null-Zeichen) */
    }
    la_out(pid, id);
    return 0;
}

static u32 __stdcall la_WmaSetPlaybackBehavior(u32 pl, u32 b)
{
    lw_pl_t *p = (lw_pl_t *)(uintptr_t)pl;
    loc_api_count(31); LA_SPEC_GUARD(31);
    lk();
    if (IS_PL(p) && !loc_rb_resim()) {
        p->behavior = b;
    }
    ulk();
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
    LA_PATCH(IXACTSoundSource_SetVelocity, la_SourceSetVelocity); /* W363 Doppler */
    LA_PATCH(IXACTEngine_SetListenerVelocity, la_SetListenerVelocity); /* W363 Doppler */
    LA_PATCH(IXACTSoundSource_SetMixBins, la_SourceSetMixBins);
    LA_PATCH(IXACTSoundSource_SetMixBinVolumes, la_SourceSetMixBinVolumes);
    LA_PATCH(IXACTSoundSource_SetI3DL2Source, la_SourceSetI3DL2Source); /* W356 */
    LA_PATCH(IXACTSoundSource_StopSoundCues, la_SourceStopSoundCues);
    LA_PATCH(IXACTSoundSource_GetStatus, la_SourceGetStatus);
    if (g_cfg.loc_wma) { /* W367 LOC-WMA: Playlist ueber Media Foundation (0 = Stub aus loc_boot.c, stumm) */
        LA_PATCH(IXACTSoundBank_CreateWmaPlayList, la_CreateWmaPlayList);
        LA_PATCH(IXACTWmaPlayList_Release, la_WmaRelease);
        LA_PATCH(IXACTWmaPlayList_Add, la_WmaAdd);
        LA_PATCH(IXACTWmaPlayList_Next, la_WmaNext);
        LA_PATCH(IXACTWmaPlayList_GetCurrentSongInfo, la_WmaGetCurrentSongInfo);
        LA_PATCH(IXACTWmaPlayList_SetPlaybackBehavior, la_WmaSetPlaybackBehavior);
        HLOG_I("LOCWMA: WMA-Playlist aktiv (loc_wma=1, Media Foundation im eigenen Thread%s)",
               g_cfg.loc_wma_demo ? ", loc_wma_demo=1: leerer Einsatz-Soundtrack wird vorbelegt" : "");
    }
    s_active = 1;
    HLOG_I("LOCAUDIO: XACT-HLE aktiv, %u Einstiege (WMA-Playlist %s, CommitDeferred bleibt Stub; Einstellungen gelten sofort)",
           s_npatch, g_cfg.loc_wma ? "ueber Media Foundation (W367)" : "Stub");
    return 1;
}


/* W363 DIAG(LOC-ORIGINAL, audio_api_diag): Inventur der XACT-/DirectSound-Einstiege (Aufrufe je Einstieg,
 * H = eigene HLE-Umsetzung in loc_audio.c, S = Stub aus loc_boot.c). Ids = Stub-Nummern (loc_boot.c). */
static const char *const s_api_name[40] = {
    "DirectSoundCreate",
    "XAudioDownloadEffectsImage",
    "DirectSoundUseFullHRTF",
    "DirectSoundDoWork",
    "XACTEngineDoWork",
    "XACTEngineCreate",
    "E_CreateSoundSource",
    "E_CreateSoundBank",
    "E_RegisterWaveBank",
    "E_RegisterStreamedWaveBank",
    "E_UnRegisterWaveBank",
    "E_SetMasterVolume",
    "E_SetI3dl2Listener",
    "E_SetListenerOrientation",
    "E_SetListenerPosition",
    "E_SetListenerVelocity",
    "E_GlobalPause",
    "E_RegisterNotification",
    "E_GetNotification",
    "E_CommitDeferredSettings",
    "E_EnableHeadphones",
    "E_SetParameterControl",
    "SB_GetSoundCueIndexFromFriendlyName",
    "SB_PrepareEx",
    "SB_PlayEx",
    "SB_Stop",
    "SB_CreateWmaPlayList",
    "WMA_Release",
    "WMA_Add",
    "WMA_Next",
    "WMA_GetCurrentSongInfo",
    "WMA_SetPlaybackBehavior",
    "SS_Release",
    "SS_SetPosition",
    "SS_SetI3DL2Source",
    "SS_SetVelocity",
    "SS_SetMixBins",
    "SS_SetMixBinVolumes",
    "SS_StopSoundCues",
    "SS_GetStatus" };
static void la_api_report(int waited_s)
{
    char b[2048];
    int i, o;
    o = _snprintf_s(b, sizeof b, _TRUNCATE, "LOCAPI t=%ds", waited_s);
    for (i = 0; i < 40 && o > 0 && o < (int)sizeof b - 48; i++) {
        long n = loc_api_calls(i);
        if (n) { o += _snprintf_s(b + o, sizeof b - o, _TRUNCATE, " %s:%ld%c", s_api_name[i], n, (0xFF03F7FFF0ULL >> i) & 1u ? 'H' : 'S'); }
    }
    HLOG_I("%s", b);
    HLOG_I("LOCAPI t=%ds Geschwindigkeit != 0: %ld, max |v| = %.2f; RPC angewendet %ld, ohne Kurve %ld", waited_s, s_vel_nz, sqrt((double)s_vel_max2), st_rpc, st_rpc_none);
}

/* W449 LOC-BOOT-SEQ-PERF (#85): F11 info.txt "loc audio:" - Ausgabe, Lautstaerke, Ton-Zaehler (keine Pfade/Eingaben).
 * Ohne Sperre gelesen (nur Zaehler). */
int loc_audio_diag_line(char *o, int cap)
{
    int k;
    unsigned pm = 0;
    int ch = audio_out_channels();
    const char *out = g_cfg.audio_live == 0 ? "off" : g_cfg.audio_live == 2 ? "winmm" : ch > 0 ? "wasapi" : "wasapi (no device)";
    if (cap < 2) return 0;
    if (!s_active) {
        _snprintf_s(o, (size_t)cap, _TRUNCATE, "inactive (audio=%d loc_audio=%d) output=%s", g_cfg.audio, g_cfg.loc_audio, out);
        return 1;
    }
    for (k = 0; k < LA_MAX_CAT; k++) {
        if (s_cat_pause[k]) pm |= 1u << k;
    }
    _snprintf_s(o, (size_t)cap, _TRUNCATE,
                "output=%s channels=%d volume=%d/%d/%d/%d cues played=%ld failed=%ld streams=%ld packets=%ld "
                "stream_read_fail=%ld wma_silent=%ld paused_categories=0x%X",
                out, ch, g_cfg.volume_master, g_cfg.volume_music, g_cfg.volume_sfx, g_cfg.volume_voice, (long)st_play,
                (long)st_fail, (long)st_streams, (long)st_spkts, (long)st_sread_fail, (long)st_wma, pm);
    return 1;
}

void loc_audio_tick(int waited_s)
{
    int i, act = 0, busy = 0;
    if (waited_s % 30 == 0) krnl_loc_io_log(waited_s); /* W449: nur bei neuen langsamen Lesungen */
    if (!s_active || waited_s % 30 != 0) {
        return;
    }
    if (g_cfg.audio_api_diag) { la_api_report(waited_s); } /* W363 */
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
    if (st_eg_play || st_eg_rel) /* W456 (#93): eigene Zeile (geparste Zeilen unveraendert) */
        HLOG_I("LOCAUDIO: t=%ds Huellkurven: Cues=%ld Stop mit Ausklang=%ld Stop sofort=%ld", waited_s, st_eg_play, st_eg_rel,
               st_eg_cut);
    if (st_slot_max) /* W456 DIAG */
        HLOG_I("LOCAUDIO: t=%ds Spiel-Ton-Slots hoechstens belegt=%ld/192", waited_s, st_slot_max);
    if (st_stream_steal) /* W379 (#54) */
        HLOG_I("LOCAUDIO: t=%ds Streaming-Welle neu gestartet, alte Instanz beendet=%ld", waited_s, st_stream_steal);
    if (st_loop_once || st_loop_n) /* W413 (#65): eigene Zeile (geparste Zeilen unveraendert) */
        HLOG_I("LOCAUDIO: t=%ds XSB-Schleifenzahl: Schleifen-Welle einmal gespielt=%ld endlich=%ld (beendet %ld)", waited_s,
               st_loop_once, st_loop_n, st_loop_n_end);
    if (g_cfg.loc_sndloop_diag) { /* W413 DIAG: spielende Schleifen (auch ohne Rollback) */
        char b[700];
        int na = loc_audio_loops_active(b, (int)sizeof b);
        HLOG_I("LOCSNDLOOP audio: t=%ds aktive Schleifen=%d [%s]", waited_s, na, b);
    }
    if (st_mis_stop || st_orphan_stop) /* W375 (#51) */
        HLOG_I("LOCAUDIO: t=%ds Missionsende-Stopps=%ld Rollback-Waisen-Stopps=%ld (Schleifen-Cues)", waited_s, st_mis_stop,
               st_orphan_stop);
    if (g_cfg.loc_wma) { /* W367 */
        char b[256];
        lw_player_stats(b, sizeof b);
        HLOG_I("LOCWMA: t=%ds Listen=%ld Eintraege=%ld Next=%ld Musikstarts=%ld Meldungen=%ld Neurechnen-ohne-Wirkung=%ld "
               "Vorbelegt=%ld | %s", waited_s, st_wma_create, st_wma_add, st_wma_next, st_wma_start, st_wma_notes,
               st_wma_rs, st_wma_demo, b);
    }
}
