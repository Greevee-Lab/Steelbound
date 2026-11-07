/* audio_3d.c - DirectSound-HLE: DS3D-Distanzdaempfung + Stereo-Pan (W89).
 *
 * Befund W89 (BERICHT_M2 "W89"): Die SFX-Stimmen des Spiels (Pool 0x106C59,
 * DSBCAPS_CTRL3D) werden per DS3D positioniert (SetAllParameters: MinDist 35,
 * MaxDist 200; SetPosition pro Frame, Weltkoordinaten; Listener per
 * IDirectSound_SetPosition/SetOrientation, RolloffFactor 1,3). Das HLE nahm
 * alle 3D-Setter nur an -> jede Quelle (auch Hunderte Einheiten entfernt) lief
 * mit voller Lautstaerke mittig: 22 Dauerstimmen = Drone mit 15-30 % Clipping
 * (W89_a3/a4). Hier das DirectSound-Distanzmodell:
 *   d <= min: 1;  min < d: (min / min(d, max)) ^ rolloff
 * plus Links/Rechts-Balance aus der Richtung im Listener-Raum (rechts =
 * top x front, DirectSound linkshaendig). Doppler/Kegel/HRTF nicht.
 * Eine Stimme wird erst 3D, wenn das Spiel sie positioniert (SetPosition/
 * SetAllParameters) - Bink/2D-Stimmen bleiben unberuehrt. Schalter audio_3d. */
#include "audio_int.h"

static float g_l_pos[3];
static float g_l_front[3] = { 0.0f, 0.0f, 1.0f };
static float g_l_top[3] = { 0.0f, 1.0f, 0.0f };
static float g_rolloff = 1.0f;
static LONG g_3d_stat_upd;

static float au_f(au32 bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static int au_fin(float f)
{
    return f == f && f < 1e30f && f > -1e30f;
}

void au3d_listener_pos(au32 x, au32 y, au32 z)
{
    float p[3] = { au_f(x), au_f(y), au_f(z) };
    if (!au_fin(p[0]) || !au_fin(p[1]) || !au_fin(p[2]))
        return;
    au_lock();
    memcpy(g_l_pos, p, sizeof p);
    au_unlock();
}

void au3d_listener_orient(au32 fx, au32 fy, au32 fz, au32 tx, au32 ty, au32 tz)
{
    float f[3] = { au_f(fx), au_f(fy), au_f(fz) };
    float t[3] = { au_f(tx), au_f(ty), au_f(tz) };
    int i;
    for (i = 0; i < 3; i++)
        if (!au_fin(f[i]) || !au_fin(t[i]))
            return;
    au_lock();
    memcpy(g_l_front, f, sizeof f);
    memcpy(g_l_top, t, sizeof t);
    au_unlock();
}

void au3d_rolloff(au32 r)
{
    float f = au_f(r);
    if (au_fin(f) && f >= 0.0f && f <= 10.0f)
        g_rolloff = f;
}

/* Stimmen-Setter (v bereits geprueft, kein Lock noetig: Mixer liest Floats). */
void au3d_voice_pos(audio_voice_t *v, au32 x, au32 y, au32 z)
{
    float p[3] = { au_f(x), au_f(y), au_f(z) };
    if (!au_fin(p[0]) || !au_fin(p[1]) || !au_fin(p[2]))
        return;
    au_lock();
    memcpy(v->p3, p, sizeof p);
    v->is3d = 1;
    au_unlock();
}

void au3d_voice_dist(audio_voice_t *v, au32 mind, au32 maxd)
{
    float a = au_f(mind), b = au_f(maxd);
    au_lock();
    if (au_fin(a) && a > 0.0f)
        v->min3d = a;
    if (au_fin(b) && b > 0.0f)
        v->max3d = b;
    au_unlock();
}

void au3d_voice_mode(audio_voice_t *v, au32 mode)
{
    if (mode <= 2)
        v->mode3d = mode;
}

/* DS3DBUFFER: 0 size, 1-3 pos, 4-6 vel, 7-8 Kegelwinkel, 9-11 Kegelachse,
 * 12 Aussenlautstaerke, 13 MinDist, 14 MaxDist, 15 Mode. */
void au3d_voice_allparams(audio_voice_t *v, const au32 *d)
{
    au3d_voice_pos(v, d[1], d[2], d[3]);
    au3d_voice_dist(v, d[13], d[14]);
    au3d_voice_mode(v, d[15]);
}

/* Unter au_lock aus dem Mixer (pro 20-ms-Chunk und Stimme): g3d/pan3d neu. */
void au3d_update(audio_voice_t *v)
{
    float dx, dy, dz, d, mn, mx, g, az = 0.0f;
    if (!g_cfg.audio_3d || !v->is3d || v->mode3d == 2) {
        v->g3d = 1.0f;
        v->pan3l = v->pan3r = 1.0f;
        return;
    }
    if (v->mode3d == 1) { /* HEADRELATIVE: Position relativ zum Listener */
        dx = v->p3[0];
        dy = v->p3[1];
        dz = v->p3[2];
    } else {
        dx = v->p3[0] - g_l_pos[0];
        dy = v->p3[1] - g_l_pos[1];
        dz = v->p3[2] - g_l_pos[2];
    }
    d = sqrtf(dx * dx + dy * dy + dz * dz);
    mn = v->min3d > 0.0f ? v->min3d : 1.0f;
    mx = v->max3d > mn ? v->max3d : 1e9f;
    if (d <= mn) {
        g = 1.0f;
    } else {
        float dd = d < mx ? d : mx;
        g = powf(mn / dd, g_rolloff);
    }
    if (d > 1e-3f) {
        /* rechts = top x front (DirectSound linkshaendig) */
        const float *f = g_l_front, *t = g_l_top;
        float r[3] = { t[1] * f[2] - t[2] * f[1], t[2] * f[0] - t[0] * f[2],
                       t[0] * f[1] - t[1] * f[0] };
        float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        if (v->mode3d == 1) { /* Kopf-Raum: x = rechts */
            az = dx / d;
        } else if (rl > 1e-6f) {
            az = (dx * r[0] + dy * r[1] + dz * r[2]) / (d * rl);
        }
        if (az > 1.0f)
            az = 1.0f;
        if (az < -1.0f)
            az = -1.0f;
    }
    v->g3d = g;
    /* Balance: Mitte 1/1 (Pegel wie bisher), ganz rechts L=0/R=1 */
    v->pan3l = az > 0.0f ? 1.0f - az : 1.0f;
    v->pan3r = az < 0.0f ? 1.0f + az : 1.0f;
    g_3d_stat_upd++;
    v->dbg_d3 = d;
}

void au3d_log_stats(void)
{
    HLOG_I("AUDIO stats3d (W89): audio_3d=%d updates=%ld listener=(%.1f,%.1f,%.1f) "
           "front=(%.2f,%.2f,%.2f) rolloff=%.2f",
           g_cfg.audio_3d, g_3d_stat_upd, g_l_pos[0], g_l_pos[1], g_l_pos[2],
           g_l_front[0], g_l_front[1], g_l_front[2], g_rolloff);
}
