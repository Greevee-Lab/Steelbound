/* audio_fx.c - DirectSound-HLE: W348 AUDIO-DSP.
 *
 * 1. Huellkurven (audio_eg): DSENVELOPEDESC aus SetEG. Beleg (Spiel-Bibliothek
 *    CMcpxVoiceClient::SetEG 0x2252A8, nur gelesen): dwEG = 0 schreibt die
 *    Multi-Huellkurve EF (Register ENV1/ENVF, Release in CFG_MISC) samt
 *    lPitchScale (ENV0 Bit 24..31) und lFilterCutOff (ENV1 Bit 24..31);
 *    dwEG = 1 schreibt die Amplituden-Huellkurve EA (ENV0/ENVA, Release
 *    TAR_LFO_ENV). Der Modus (Startphase) geht erst mit dem naechsten Play
 *    (VOICE_ON, Bit 24..27 EF, 28..31 EA) an die Hardware. Zeitbasis 512
 *    Samples bei 48 kHz (10,67 ms) je Einheit. Verlauf wie der MCPX-Nachbau
 *    in xemu: OFF = Pegel 1, DELAY 0, ATTACK linear 0..1, HOLD 1, DECAY
 *    exponentiell bis Sustain, SUSTAIN = s/255, RELEASE exp(-6,91 t/T).
 *    EF wirkt auf die Tonhoehe: f = 2^((p + ps*32*ef)/4096) * 48 kHz, also
 *    Faktor 2^(ps*32*ef/4096) auf die vom Spiel gesetzte Frequenz. EA
 *    multipliziert die Lautstaerke; Ende des EA-Release = Stimme aus.
 *
 * 2. I3DL2-Hall (audio_reverb): Listener-Werte aus IDirectSound_SetI3DL2Listener
 *    (DS3D_DEFERRED -> erst bei CommitDeferredSettings). Send je 3D-Stimme mit
 *    lRoom(Quelle) + lRoom(Listener), HF-Daempfung lRoomHF bei flHFReference
 *    (ein Pol), Raum-Rolloff wie DS3D. Hall: Vorverzoegerung, fruehe
 *    Reflexionen (Abgriffe ab flReflectionsDelay, Pegel lReflections),
 *    Nachhall ab flReflectionsDelay + flReverbDelay (Pegel lReverb) als
 *    FDN (Jot, 8 Leitungen, Householder-Matrix, je Leitung Daempfung fuer
 *    flDecayTime und flDecayHFRatio bei flHFReference), Diffusion als
 *    Allpass-Kette, Dichte skaliert die Leitungslaengen. Pegel energetisch:
 *    Gesamtenergie der Reflexionen bzw. des Nachhalls = Pegel^2. */
#include "audio_int.h"
#include "sb_profile.h" /* g_sba_loc */

#define RV_FS ((float)MIXRATE)
#define RV_PRE 32768u /* Vorverzoegerung (0,74 s, Reflexionen max 0,3 + 0,1 s) */
#define RV_N 8
#define RV_LMAX 4096u
#define RV_AP 4
#define RV_APMAX 1024u
#define EG_UNIT (512.0 / 48000.0) /* s je Einheit */

/* ---------------- I3DL2-Listener ---------------- */
typedef struct {
    float room, roomhf, rolloff, decay, hfratio, refl, refldelay, reverb, revdelay, diff, dens, hfref;
} lis_t;

/* DSound-Grundzustand bis zum ersten SetI3DL2Listener: DSI3DL2_ENVIRONMENT_PRESET_DEFAULT
 * (Raum -10000 mB = aus). */
static lis_t g_lis = { -10000.0f, 0.0f, 0.0f, 1.0f, 0.5f, -10000.0f, 0.02f, -10000.0f, 0.04f, 100.0f, 100.0f, 5000.0f };
static lis_t g_lis_pend;
static int g_lis_have_pend, g_lis_from_game;
static volatile LONG g_lis_gen = 1;
static LONG g_lis_calls, g_lis_commits;

static float fclampf(float x, float lo, float hi)
{
    if (!(x == x))
        return lo;
    return x < lo ? lo : x > hi ? hi : x;
}

static float db100(float mb)
{
    return mb <= -10000.0f ? 0.0f : powf(10.0f, mb / 2000.0f);
}

static void lis_parse(const au32 *d, lis_t *o)
{
    float f[12];
    memcpy(f, d, sizeof f);
    o->room = fclampf((float)(int)d[0], -10000.0f, 0.0f);
    o->roomhf = fclampf((float)(int)d[1], -10000.0f, 0.0f);
    o->rolloff = fclampf(f[2], 0.0f, 10.0f);
    o->decay = fclampf(f[3], 0.1f, 20.0f);
    o->hfratio = fclampf(f[4], 0.1f, 2.0f);
    o->refl = fclampf((float)(int)d[5], -10000.0f, 1000.0f);
    o->refldelay = fclampf(f[6], 0.0f, 0.3f);
    o->reverb = fclampf((float)(int)d[7], -10000.0f, 2000.0f);
    o->revdelay = fclampf(f[8], 0.0f, 0.1f);
    o->diff = fclampf(f[9], 0.0f, 100.0f);
    o->dens = fclampf(f[10], 0.0f, 100.0f);
    o->hfref = fclampf(f[11], 20.0f, 20000.0f);
}

static void lis_diag(const char *how)
{
    if (g_cfg.audio_api_diag) /* W348 DIAG(I3DL2): Zeitpunkt (Mischer-Zeit) und aktiver Raum */
        HLOG_I("DIAG(I3DL2) %s t=%.1f room=%.0f roomhf=%.0f decay=%.2f hfratio=%.2f refl=%.0f reverb=%.0f diff=%.0f",
               how, (double)g_mixed_total / MIXRATE, g_lis.room, g_lis.roomhf, g_lis.decay, g_lis.hfratio, g_lis.refl,
               g_lis.reverb, g_lis.diff);
}

void audio_set_i3dl2_listener(const au32 *d, int deferred)
{
    lis_t l;
    lis_parse(d, &l);
    au_lock();
    g_lis_calls++;
    if (deferred) {
        g_lis_pend = l;
        g_lis_have_pend = 1;
    } else {
        g_lis = l;
        g_lis_from_game = 1;
        InterlockedIncrement(&g_lis_gen);
        lis_diag("sofort");
    }
    au_unlock();
}

void audio_commit_deferred(void)
{
    au_lock();
    if (g_lis_have_pend) {
        g_lis = g_lis_pend;
        g_lis_have_pend = 0;
        g_lis_from_game = 1;
        g_lis_commits++;
        InterlockedIncrement(&g_lis_gen);
        lis_diag("commit");

    }
    au_unlock();
}

/* ---------------- Huellkurven ---------------- */
enum { EGM_OFF = 0, EGM_DELAY, EGM_ATTACK, EGM_HOLD, EGM_DECAY, EGM_SUSTAIN, EGM_RELEASE, EGM_FORCE };
static LONG g_eg_set[2], g_eg_live[2], g_eg_play[2], g_eg_rel, g_eg_off;
static double g_ef_hist[25]; /* Puffer-Spielzeit (Frames) je EF-Tonhoehenversatz in Halbtoenen -12..+12 */

static int g_eg_psmin = 0, g_eg_psmax = 0;

void aufx_eg_set(audio_voice_t *v, const au32 *d)
{
    int k;
    if (d[0] > 1u)
        return;
    k = d[0] == 0 ? 0 : 1; /* 0 = Multi (EF), 1 = Amplitude (EA), s. Kopf */
    au_lock();
    v->eg[k][0] = d[1] & 0xFu;
    v->eg[k][1] = d[2] & 0xFFFu;
    v->eg[k][2] = d[3] & 0xFFFu;
    v->eg[k][3] = d[4] & 0xFFFu;
    v->eg[k][4] = d[5] & 0xFFFu;
    v->eg[k][5] = d[6] & 0xFFFu;
    v->eg[k][6] = d[7] & 0xFFu;
    v->eg[k][7] = 1;
    if (k == 0) {
        v->eg_ps = (int)(int8_t)(d[8] & 0xFFu);
        v->eg_fc = (int)(int8_t)(d[9] & 0xFFu);
        if (v->eg_ps < g_eg_psmin)
            g_eg_psmin = v->eg_ps;
        if (v->eg_ps > g_eg_psmax)
            g_eg_psmax = v->eg_ps;
    }
    g_eg_set[k]++;
    if (v->kind == 1 && v->playing)
        g_eg_live[k]++; /* gesetzt, waehrend die Stimme spielt (Modus wirkt erst beim naechsten Play) */
    au_unlock();
}

/* Unter au_lock (audio_play). */
void aufx_voice_play(audio_voice_t *v)
{
    int k;
    for (k = 0; k < 2; k++) {
        v->eg_ph[k] = v->eg[k][7] ? (int)v->eg[k][0] : EGM_OFF;
        if (v->eg_ph[k] > EGM_RELEASE)
            v->eg_ph[k] = EGM_OFF;
        v->eg_t[k] = 0.0;
        v->eg_lv[k] = v->eg_ph[k] == EGM_OFF ? 1.0f : 0.0f;
        if (v->eg_ph[k] == EGM_HOLD)
            v->eg_lv[k] = 1.0f;
        if (v->eg_ph[k] == EGM_SUSTAIN)
            v->eg_lv[k] = (float)v->eg[k][6] / 255.0f;
        if (v->eg_ph[k] != EGM_OFF)
            g_eg_play[k]++;
    }
    v->eg_ea0 = v->eg_lv[1];
    v->hr.init = 0; /* W356: neuer Ton, HRTF-Parameter ohne Rampe von der alten Richtung */
}

/* Unter au_lock (audio_stop): 1 = EA-Release laeuft, Stimme spielt noch aus. Nur
 * wenn das Spiel eine Amplituden-Huellkurve gesetzt hat (sonst Stop sofort wie bisher). */
int aufx_voice_stop(audio_voice_t *v)
{
    (void)v;
    return 0; /* IDirectSoundBuffer_Stop = sofort (StopEx mit DSBSTOPEX_ENVELOPE nutzt das Spiel nicht, s. Bericht) */
}

static void eg_step(audio_voice_t *v, int k, double dt)
{
    const au32 *c = v->eg[k];
    int guard = 0;
    double t = v->eg_t[k] + dt;
    for (;;) {
        double len;
        if (++guard > 8)
            break;
        switch (v->eg_ph[k]) {
        case EGM_OFF:
            v->eg_lv[k] = 1.0f;
            v->eg_t[k] = 0.0;
            return;
        case EGM_DELAY:
            len = c[1] * EG_UNIT;
            if (t < len) {
                v->eg_lv[k] = 0.0f;
                v->eg_t[k] = t;
                return;
            }
            t -= len;
            v->eg_ph[k] = EGM_ATTACK;
            continue;
        case EGM_ATTACK:
            len = c[2] * EG_UNIT;
            if (t < len) {
                v->eg_lv[k] = (float)(t / len);
                v->eg_t[k] = t;
                return;
            }
            t -= len;
            v->eg_ph[k] = EGM_HOLD;
            continue;
        case EGM_HOLD:
            len = c[3] * EG_UNIT;
            if (t < len) {
                v->eg_lv[k] = 1.0f;
                v->eg_t[k] = t;
                return;
            }
            t -= len;
            v->eg_ph[k] = EGM_DECAY;
            continue;
        case EGM_DECAY: {
            float s = (float)c[6] / 255.0f, lv;
            len = c[4] * EG_UNIT;
            lv = len > 0.0 ? (float)exp(-6.91 * t / len) : 0.0f;
            if (lv > s) {
                v->eg_lv[k] = lv;
                v->eg_t[k] = t;
                return;
            }
            v->eg_ph[k] = EGM_SUSTAIN;
            t = 0.0;
            continue;
        }
        case EGM_SUSTAIN:
            v->eg_lv[k] = (float)c[6] / 255.0f;
            v->eg_t[k] = 0.0;
            return;
        case EGM_RELEASE:
            len = c[5] * EG_UNIT;
            if (t < len) {
                v->eg_lv[k] = v->eg_rel0[k] * (float)exp(-6.91 * t / len);
                v->eg_t[k] = t;
                return;
            }
            v->eg_ph[k] = EGM_FORCE;
            continue;
        default: /* FORCE_RELEASE / Ende */
            v->eg_lv[k] = 0.0f;
            v->eg_t[k] = 0.0;
            if (k == 1 && v->playing) { /* EA-Ende = Stimme aus (MCPX voice_off) */
                v->playing = 0;
                g_eg_off++;
            }
            return;
        }
    }
}

/* Unter au_lock aus dem Mischer, vor dem Rendern eines Chunks (n Frames):
 * eg_ea0 = EA-Pegel am Anfang, eg_lv[1] am Ende (Rampe im Mischer); step_mul. */
void aufx_chunk(audio_voice_t *v, au32 n)
{
    float m = v->dop > 0.0f ? v->dop : 1.0f;
    v->eg_ea0 = g_cfg.audio_eg ? v->eg_lv[1] : 1.0f;
    if (g_cfg.audio_eg && (v->kind == 1 ? v->playing : v->active) && !v->paused) {
        double dt = (double)n / MIXRATE;
        eg_step(v, 0, dt);
        eg_step(v, 1, dt);
        if (v->eg_ps) {
            float ef = exp2f((float)v->eg_ps * 32.0f * v->eg_lv[0] / 4096.0f);
            int b = (int)floorf(log2f(ef) * 12.0f + 0.5f) + 12; /* Halbtoene, -12..+12 */
            m *= ef;
            if (v->kind == 1 && v->playing)
                g_ef_hist[b < 0 ? 0 : b > 24 ? 24 : b] += n; /* DIAG: Spielzeit je Versatz */
        } else if (v->kind == 1 && v->playing) {
            g_ef_hist[12] += n;
        }
    } else if (!g_cfg.audio_eg) {
        v->eg_lv[1] = 1.0f;
    }
    v->step_mul = m;
}

/* ---------------- Hall ---------------- */
static const int k_len[RV_N] = { 1117, 1187, 1277, 1361, 1423, 1493, 1559, 1619 };
static const int k_aplen[RV_AP] = { 142, 107, 379, 277 };
static const float k_insign[RV_N] = { 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f, -1.0f, -1.0f };
static const float k_tap_t[6] = { 0.0f, 0.0031f, 0.0067f, 0.0109f, 0.0151f, 0.0197f };
static const float k_tap_g[6] = { 1.0f, 0.84f, 0.71f, 0.6f, 0.5f, 0.42f };

static struct {
    float pre[RV_PRE][2];
    au32 pre_w;
    float line[RV_N][RV_LMAX];
    au32 lw[RV_N], len[RV_N];
    float lg[RV_N], lpa[RV_N], lps[RV_N]; /* Schleifenverstaerkung, Tiefpass-Koeff., Zustand */
    float ap[RV_AP][RV_APMAX];
    au32 apw[RV_AP], aplen[RV_AP];
    float apg;
    au32 tap_d[6], late_d;
    float tap_gl[6], tap_gr[6];
    float g_refl, g_late;
    LONG gen;
    int on;
    float *send;
    au32 send_cap, n;
    int any;
    au32 quiet; /* Frames seit dem letzten Send */
    double q_sum, q_max; /* CPU-Kosten */
    uint64_t q_n;
} g_rv;
static LONG g_rv_voices;

/* Ein-Pol-Tiefpass y += a (x - y) mit |H(f)| = g (g in (0,1]); a = 1 bei g >= 1. */
static float lp_coeff(float g, float f, float fs)
{
    double w, c, gg, b;
    if (g >= 0.9999f)
        return 1.0f;
    if (g < 0.001f)
        g = 0.001f;
    w = 2.0 * 3.14159265358979 * (f > 0.45f * fs ? 0.45f * fs : f) / fs;
    c = cos(w);
    gg = (double)g * g;
    /* b = Pol (1 - a): (gg-1) b^2 + 2(1 - gg c) b + (gg-1) = 0 */
    b = (1.0 - gg * c - sqrt(2.0 * gg * (1.0 - c) - gg * gg * (1.0 - c * c))) / (1.0 - gg);
    if (b < 0.0)
        b = 0.0;
    if (b > 0.9999)
        b = 0.9999;
    return (float)(1.0 - b);
}

static void rv_setup(void)
{
    lis_t l;
    int i;
    float dens, e_lines = 0.0f, gsum2 = 0.0f;
    au_lock();
    l = g_lis;
    au_unlock();
    g_rv.gen = g_lis_gen;
    g_rv.on = l.room > -10000.0f && (l.refl > -10000.0f || l.reverb > -10000.0f);
    dens = 0.5f + 0.5f * l.dens / 100.0f;
    for (i = 0; i < RV_N; i++) {
        float L = (float)k_len[i] * dens, ghf, rhf;
        float t60hf = l.decay * (l.hfratio > 1.0f ? 1.0f : l.hfratio);
        if (L < 64.0f)
            L = 64.0f;
        g_rv.len[i] = (au32)L;
        g_rv.lg[i] = powf(10.0f, -3.0f * g_rv.len[i] / (l.decay * RV_FS));
        ghf = powf(10.0f, -3.0f * g_rv.len[i] / (t60hf * RV_FS));
        rhf = ghf / g_rv.lg[i];
        g_rv.lpa[i] = lp_coeff(rhf, l.hfref, RV_FS);
        gsum2 += g_rv.lg[i] * g_rv.lg[i];
    }
    for (i = 0; i < RV_AP; i++)
        g_rv.aplen[i] = (au32)((float)k_aplen[i] * dens) | 1u;
    g_rv.apg = 0.7f * l.diff / 100.0f;
    for (i = 0; i < 6; i++) {
        float t = l.refldelay + k_tap_t[i] * dens;
        g_rv.tap_d[i] = (au32)(t * RV_FS) + 1u;
        e_lines += k_tap_g[i] * k_tap_g[i];
    }
    for (i = 0; i < 6; i++) { /* abwechselnd links/rechts betont, Energie je Kanal 1 */
        float gn = k_tap_g[i] / sqrtf(e_lines);
        g_rv.tap_gl[i] = (i & 1) ? 0.6f * gn : gn;
        g_rv.tap_gr[i] = (i & 1) ? gn : 0.6f * gn;
    }
    { /* Kanalenergie inkl. Uebersprechen 0,3 normieren (gleicher Impuls auf beiden Kanaelen) */
        float el = 0.0f;
        for (i = 0; i < 6; i++) {
            float a = g_rv.tap_gl[i] + 0.3f * g_rv.tap_gr[i];
            el += a * a;
        }
        for (i = 0; i < 6; i++) {
            g_rv.tap_gl[i] /= sqrtf(el);
            g_rv.tap_gr[i] /= sqrtf(el);
        }
    }
    g_rv.late_d = (au32)((l.refldelay + l.revdelay) * RV_FS) + 1u;
    g_rv.g_refl = db100(l.refl);
    /* Nachhall-Energie: Jot-Normierung sqrt(1 - g^2) je Leitung, Konstante aus
     * dem Selbsttest (Impulsantwort-Energie = Pegel^2 bei Mono-Eingang). */
    g_rv.g_late = db100(l.reverb) * sqrtf(1.0f - gsum2 / RV_N) * 0.598f;

}

void aufx_reverb_begin(au32 n)
{
    g_rv.n = n;
    g_rv.any = 0;
    if (!g_cfg.audio_reverb)
        return;
    if (g_rv.gen != g_lis_gen)
        rv_setup();
    if (g_rv.send_cap < n) {
        free(g_rv.send);
        g_rv.send = (float *)calloc((size_t)n * 2, sizeof(float));
        g_rv.send_cap = g_rv.send ? n : 0;
    }
    if (g_rv.send)
        memset(g_rv.send, 0, (size_t)n * 2 * sizeof(float));
}

float *aufx_reverb_send(void)
{
    if (!g_cfg.audio_reverb || !g_rv.on || !g_rv.send)
        return NULL;
    g_rv.any = 1;
    return g_rv.send;
}

/* Send-Pegel einer Stimme (unter au_lock, je Chunk): 0 = kein Send. */
float aufx_voice_send(audio_voice_t *v)
{
    float mb, g, ro;
    int occ;
    lis_t *l = &g_lis;
    if (!g_cfg.audio_reverb || !g_rv.on)
        return 0.0f;
    /* DSBCAPS_CTRL3D: nur 3D-Stimmen haben einen I3DL2-Send; XACT-HLE (LoC): positionierte Cues */
    if (!(v->cflags & 0x10u) && !(g_sba_loc && v->is3d))
        return 0.0f;
    if (!v->is3d)
        return 0.0f; /* nie positioniert (z. B. Bink-Filmton mit CTRL3D-Flag): trocken lassen */
    if (v->rv_bin_mb <= -10000)
        return 0.0f; /* Bin-Liste ohne DSMIXBIN_I3DL2 */
    /* Pegel = I3DL2-Mixbin (SetMixBinVolumes, Bin 10) + lRoom der Quelle + lRoom des Listeners */
    mb = (float)v->rv_bin_mb + (float)(v->has_i3dl2 ? v->i3d_room : 0) + l->room;
    occ = g_cfg.audio_occlusion && v->has_i3dl2;
    if (occ) /* W356: Occlusion daempft auch den Raumanteil (LF = lHFLevel x LFRatio) */
        mb += (float)v->i3d_occ * v->i3d_occ_lfr;
    g = db100(mb < -10000.0f ? -10000.0f : mb);
    ro = l->rolloff; /* Raum-Rolloff (Quelle meist 0) wie DS3D-Abfall */
    if (occ)
        ro += v->i3d_rolloff; /* W356: flRoomRolloffFactor Quelle + Listener (I3DL2) */
    if (ro > 0.0f && v->is3d && v->dbg_d3 > v->min3d && v->min3d > 0.0f) {
        float dd = v->dbg_d3 < v->max3d ? v->dbg_d3 : v->max3d;
        g *= powf(v->min3d / dd, ro);
    }
    {
        float hf = (float)(v->has_i3dl2 ? v->i3d_roomhf : 0) + l->roomhf;
        if (occ)
            hf += (float)v->i3d_occ * (1.0f - v->i3d_occ_lfr);
        v->rv_sendhf = lp_coeff(db100(hf < -10000.0f ? -10000.0f : hf), l->hfref, RV_FS);
    }
    if (g > 0.0f)
        g_rv_voices++;
    return g;
}

/* W356 (audio_occlusion): Direktpfad der I3DL2-Quelle. I3DL2-Regel: Obstruction daempft nur den
 * Direktpfad, Occlusion Direkt- und Raumpfad; je HF um lHFLevel, LF um lHFLevel x flLFRatio. Dazu
 * lDirect (Pegel) und lDirectHF (HF relativ). HF-Daempfung als Ein-Pol-Tiefpass bei flHFReference
 * des Listeners (wie lRoomHF im Send). Unter au_lock, je Chunk. */
static LONG g_oc_upd, g_oc_act;
static float g_oc_min_lf = 0.0f, g_oc_min_hf = 0.0f;
float aufx_voice_direct(audio_voice_t *v, float *a_hf)
{
    float lf, hf;
    *a_hf = 1.0f;
    if (!g_cfg.audio_occlusion || !v->has_i3dl2)
        return 1.0f;
    lf = (float)v->i3d_direct + (float)v->i3d_obs * v->i3d_obs_lfr + (float)v->i3d_occ * v->i3d_occ_lfr;
    hf = (float)v->i3d_directhf + (float)v->i3d_obs * (1.0f - v->i3d_obs_lfr) +
        (float)v->i3d_occ * (1.0f - v->i3d_occ_lfr);
    if (lf > 0.0f)
        lf = 0.0f;
    if (hf > 0.0f)
        hf = 0.0f;
    g_oc_upd++;
    if (lf < 0.0f || hf < 0.0f) {
        g_oc_act++;
        if (lf < g_oc_min_lf)
            g_oc_min_lf = lf;
        if (hf < g_oc_min_hf)
            g_oc_min_hf = hf;
    }
    *a_hf = hf < 0.0f ? lp_coeff(db100(hf < -10000.0f ? -10000.0f : hf), g_lis.hfref, RV_FS) : 1.0f;
    return db100(lf < -10000.0f ? -10000.0f : lf);
}

static float rv_ap(int i, float x)
{
    float *b = g_rv.ap[i];
    au32 w = g_rv.apw[i], L = g_rv.aplen[i];
    au32 r = (w + RV_APMAX - L) % RV_APMAX;
    float d = b[r], y = d - g_rv.apg * x;
    b[w] = x + g_rv.apg * y;
    g_rv.apw[i] = (w + 1) % RV_APMAX;
    return y;
}

/* Hall rechnen und in mix (Stereo, n Frames) addieren. Ohne Send und ohne Rest
 * (alles still) wird nichts addiert. */
void aufx_reverb_end(float *mix, au32 n)
{
    aufx_reverb_end2(mix, NULL, n);
}

void aufx_reverb_end2(float *mix, float *rear, au32 n)
{
    LARGE_INTEGER a, b, f;
    au32 i;
    int k;
    static float idle_e = 1.0f;
    if (!g_cfg.audio_reverb || !g_rv.on || !g_rv.send)
        return;
    if (g_rv.any)
        g_rv.quiet = 0;
    else if (g_rv.quiet < 0x7FFFFFFFu)
        g_rv.quiet += n;
    if (!g_rv.any && g_rv.quiet > RV_PRE && idle_e < 1e-12f)
        return; /* ausgeklungen (Vorverzoegerung leer, Ausgang still): nichts zu tun */
    QueryPerformanceCounter(&a);
    idle_e = 0.0f;
    for (i = 0; i < n; i++) {
        float inl = g_rv.send[i * 2], inr = g_rv.send[i * 2 + 1];
        au32 w = g_rv.pre_w, r;
        float el = 0.0f, er = 0.0f, lin, x[RV_N], s = 0.0f, ol = 0.0f, orr = 0.0f;
        g_rv.pre[w][0] = inl;
        g_rv.pre[w][1] = inr;
        for (k = 0; k < 6; k++) {
            r = (w + RV_PRE - g_rv.tap_d[k]) % RV_PRE;
            el += g_rv.pre[r][0] * g_rv.tap_gl[k] + g_rv.pre[r][1] * g_rv.tap_gr[k] * 0.3f;
            er += g_rv.pre[r][1] * g_rv.tap_gr[k] + g_rv.pre[r][0] * g_rv.tap_gl[k] * 0.3f;
        }
        r = (w + RV_PRE - g_rv.late_d) % RV_PRE;
        lin = 0.5f * (g_rv.pre[r][0] + g_rv.pre[r][1]);
        for (k = 0; k < RV_AP; k++)
            lin = rv_ap(k, lin);
        g_rv.pre_w = (w + 1) % RV_PRE;
        for (k = 0; k < RV_N; k++) { /* Leitungsausgang mit HF-Daempfung */
            au32 rr = (g_rv.lw[k] + RV_LMAX - g_rv.len[k]) % RV_LMAX;
            float y = g_rv.line[k][rr] * g_rv.lg[k];
            g_rv.lps[k] += g_rv.lpa[k] * (y - g_rv.lps[k]);
            x[k] = g_rv.lps[k];
            s += x[k];
        }
        s *= 2.0f / RV_N; /* Householder: x - 2/N * Summe */
        for (k = 0; k < RV_N; k++) {
            float fb = x[k] - s + lin * k_insign[k];

            if (!(fb == fb) || fb > 1e3f || fb < -1e3f) /* Schutz */
                fb = 0.0f;
            g_rv.line[k][g_rv.lw[k]] = fb;
            g_rv.lw[k] = (g_rv.lw[k] + 1) % RV_LMAX;
            if (k & 1)
                orr += (k & 2) ? -x[k] : x[k];
            else
                ol += (k & 2) ? -x[k] : x[k];
        }
        if (rear) { /* W356 Surround: Nachhall vorn/hinten je -3 dB, hinten orthogonale Leitungs-Kombination */
            float bl = (x[0] + x[2] - x[4] - x[6]) * g_rv.g_late * 0.7071f;
            float br = (x[1] + x[3] - x[5] - x[7]) * g_rv.g_late * 0.7071f;
            rear[i * 2] += bl;
            rear[i * 2 + 1] += br;
            idle_e += bl * bl + br * br;
            ol *= 0.7071f;
            orr *= 0.7071f;
        }
        ol = ol * g_rv.g_late + el * g_rv.g_refl;
        orr = orr * g_rv.g_late + er * g_rv.g_refl;
        mix[i * 2] += ol;
        mix[i * 2 + 1] += orr;
        idle_e += ol * ol + orr * orr + lin * lin;
    }
    QueryPerformanceCounter(&b);
    QueryPerformanceFrequency(&f);
    {
        double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart * (MIXRATE / 100.0) / n;
        g_rv.q_sum += ms; /* je 10-ms-Block */
        g_rv.q_n++;
        if (ms > g_rv.q_max)
            g_rv.q_max = ms;
    }
}

void aufx_log_stats(void)
{
    lis_t l = g_lis;
    if (g_cfg.audio_eg || g_cfg.audio_api_diag)
        HLOG_I("AUDIO eg (W348): audio_eg=%d setEF=%ld setEA=%ld davon_beim_spielen=%ld/%ld playEF=%ld playEA=%ld ea_aus=%ld pitchscale=%d..%d",
               g_cfg.audio_eg, g_eg_set[0], g_eg_set[1], g_eg_live[0], g_eg_live[1], g_eg_play[0], g_eg_play[1], g_eg_off, g_eg_psmin,
               g_eg_psmax);
    if (g_cfg.audio_eg) { /* DIAG: Anteil der Puffer-Spielzeit je EF-Versatz (Halbtoene) */
        char b[400];
        size_t o = 0;
        double tot = 0.0;
        int i;
        for (i = 0; i < 25; i++)
            tot += g_ef_hist[i];
        for (i = 0; i < 25 && tot > 0.0; i++)
            if (g_ef_hist[i] > 0.0)
                o += (size_t)_snprintf_s(b + o, sizeof b - o, _TRUNCATE, " %+d:%.1f%%", i - 12,
                                         100.0 * g_ef_hist[i] / tot);
        b[o] = 0;
        HLOG_I("AUDIO eg-versatz (W348): Spielzeit je Halbton%s", o ? b : " -");
    }
    if (g_cfg.audio_reverb || g_cfg.audio_api_diag)
        HLOG_I("AUDIO reverb (W348): audio_reverb=%d an=%d vom_spiel=%d aufrufe=%ld commits=%ld room=%.0f roomhf=%.0f "
               "decay=%.2f hfratio=%.2f refl=%.0f/%.3f reverb=%.0f/%.3f diff=%.0f dens=%.0f hfref=%.0f "
               "sends=%ld kosten=%.3f ms/10ms (max %.3f)",
               g_cfg.audio_reverb, g_rv.on, g_lis_from_game, g_lis_calls, g_lis_commits, l.room, l.roomhf, l.decay,
               l.hfratio, l.refl, l.refldelay, l.reverb, l.revdelay, l.diff, l.dens, l.hfref, g_rv_voices,
               g_rv.q_n ? g_rv.q_sum / (double)g_rv.q_n : 0.0, g_rv.q_max);
    if (g_cfg.audio_occlusion || g_cfg.audio_api_diag)
        HLOG_I("AUDIO occlusion (W356): audio_occlusion=%d updates=%ld wirksam=%ld direkt_lf_min=%.0f mB hf_min=%.0f mB",
               g_cfg.audio_occlusion, g_oc_upd, g_oc_act, g_oc_min_lf, g_oc_min_hf);
}

#ifdef AUFX_SELFTEST
/* Selbsttest-Zugang (tests/audio_reverb_selftest.c): Hall-Zustand leeren */
void aufx_test_reset(void)
{
    float *s = g_rv.send;
    au32 c = g_rv.send_cap;
    memset(&g_rv, 0, sizeof g_rv);
    g_rv.send = s;
    g_rv.send_cap = c;
}
#endif

