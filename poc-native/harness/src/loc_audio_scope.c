/* loc_audio_scope.c - W375 LOC-TESTPOLISH (GitHub #51): Auswahl der Cues, die beim Missionsende bzw. als
 * Rollback-Waise gestoppt werden. Rein, ohne Mischer (loc_audio_selftest). Siehe loc_audio_scope.h. */
#include "loc_audio_scope.h"

int las_mission_edge(uint32_t prev_mask, uint32_t mask)
{
    if (!prev_mask && mask) return LAS_EDGE_BEGIN;
    if (prev_mask && !mask) return LAS_EDGE_END;
    return LAS_EDGE_NONE;
}

int las_mission_end_pick(const las_cue_t *c, int n, uint32_t mgen, int *idx, int cap)
{
    int i, k = 0;
    if (!c || mgen == 0) return 0;
    for (i = 0; i < n && k < cap; i++) {
        if (c[i].state == 2 && c[i].loops && !c[i].wma && c[i].mgen == mgen) idx[k++] = i;
    }
    return k;
}

int las_orphan_pick(const las_cue_t *c, int n, uint32_t serial)
{
    int i;
    if (!c || serial == 0) return -1;
    for (i = 0; i < n; i++) {
        if (c[i].serial == serial) return (c[i].state == 2 && c[i].loops && !c[i].wma) ? i : -1;
    }
    return -1;
}

int las_stream_steal_pick(const las_cue_t *c, int n, int self, int wb, int wave, int *idx, int cap)
{
    int i, t, k = 0;
    if (!c || wb < 0 || wave < 0) return 0;
    for (i = 0; i < n && k < cap; i++) {
        if (i == self || c[i].state != 2) continue;
        for (t = 0; t < LAS_TRACKS; t++) {
            if (c[i].swb[t] == wb + 1 && c[i].swave[t] == wave) {
                idx[k++] = i;
                break;
            }
        }
    }
    return k;
}

/* W390 LOC-OWN-PREDICT: XACT-Einstiege (Nummern wie loc_api_count in loc_audio.c) im spekulativen Vorlauf. */
int las_spec_action(int api)
{
    switch (api) {
    case 5:  /* XACTEngineCreate */
    case 6:  /* CreateSoundSource (erzeugt keinen Ton; Quelle ohne Besitzer nach dem Ruecksetzen) */
    case 7:  /* CreateSoundBank */
    case 8:  /* RegisterWaveBank */
    case 9:  /* RegisterStreamedWaveBank */
    case 22: /* GetSoundCueIndexFromFriendlyName (nur lesend) */
    case 26: /* CreateWmaPlayList (beim Neurechnen: dieselbe Liste, nichts neu) */
    case 30: /* WmaGetCurrentSongInfo (nur lesend) */
    case 39: /* SourceGetStatus (nur lesend) */
        return LAS_SPEC_ALLOW;
    case 18: /* GetNotification: keine Meldung (die echte Zeitlinie holt sie ab) */
        return LAS_SPEC_FAIL;
    default: /* Start, Vorbereiten, Stoppen, Freigeben, Setter, DoWork, Playlist: ohne Wirkung, S_OK */
        return LAS_SPEC_SKIP;
    }
}
