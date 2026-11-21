#pragma once
/* loc_audio_scope.h - W375 LOC-TESTPOLISH (GitHub #51): Missions-Cues beim Missionsende still.
 *
 * Reine Auswahl-Logik (ohne Mischer, testbar in loc_audio_selftest):
 * - Missionsgrenze aus der Lockstep-Aktiv-Maske [0x306830] (0 -> !=0 Beginn, !=0 -> 0 Ende).
 * - Missionsende: Schleifen-Cues, die in dieser Mission gestartet wurden und noch spielen (ohne die WMA-Playlist,
 *   die das Spiel selbst stoppt), werden beendet. Vorher lief z. B. der Eject-Alarm in den Menues weiter.
 * - Rollback: Ein Schleifen-Cue, den nur die verworfene Zeitlinie gestartet hat (W346 "verwaist"), hat im Spiel keinen
 *   Besitzer mehr (der Handle steht nur im verworfenen Speicherstand) - niemand stoppt ihn. Er wird gezielt beendet
 *   (Kennung = laufende Nummer des Cues, nie ein wiederverwendeter Slot). Einmal-Cues laufen wie bisher aus. */

#include <stdint.h>

#define LAS_TRACKS 8 /* = LXF_MAX_TRACKS */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int state;       /* 0 frei, 1 vorbereitet, 2 spielt */
    int loops;       /* mindestens eine Spur mit Schleife */
    int wma;         /* Spur ist die WMA-Playlist (Spiel stoppt sie selbst) */
    uint32_t serial; /* laufende Nummer des Cues (0 = keine) */
    uint32_t mgen;   /* Mission, in der der Cue gestartet wurde (0 = ausserhalb einer Mission) */
    /* W379 (#54): je Spur Welle einer Streaming-Wave-Bank: swb = Bank-Index + 1 (0 = keine), swave = Wellen-Index */
    int swb[LAS_TRACKS], swave[LAS_TRACKS];
} las_cue_t;

enum { LAS_EDGE_NONE = 0, LAS_EDGE_BEGIN = 1, LAS_EDGE_END = 2 };

/* Missionsgrenze aus alter/neuer Aktiv-Maske. */
int las_mission_edge(uint32_t prev_mask, uint32_t mask);
/* Missionsende von Mission mgen (> 0): Indizes der zu stoppenden Cues nach idx (hoechstens cap), Rueckgabe Anzahl. */
int las_mission_end_pick(const las_cue_t *c, int n, uint32_t mgen, int *idx, int cap);
/* Rollback-Waise: Index des noch spielenden Schleifen-Cues mit dieser Nummer, sonst -1. */
int las_orphan_pick(const las_cue_t *c, int n, uint32_t serial);
/* W379 (GitHub #54): Eine lange Streaming-Welle (Musik/Ambience mit eigener Stream-Stimme; der Aufrufer traegt nur
 * solche Spuren in swb/swave ein) spielt hoechstens in einem Cue gleichzeitig. Startet ein Cue (Index self, -1 =
 * keiner) die Welle (wb, wave) erneut, werden die noch spielenden Cues mit derselben Welle beendet. Beleg: Rueckkehr
 * aus der Titel-Demo startet die Titelmusik B01 neu, ohne die alte Instanz zu stoppen; ohne diese Regel lief die alte
 * nach dem naechsten Tastendruck in den Menues weiter. Indizes nach idx (hoechstens cap), Rueckgabe Anzahl. */
int las_stream_steal_pick(const las_cue_t *c, int n, int self, int wb, int wave, int *idx, int cap);

/* W390 LOC-OWN-PREDICT: XACT-Einstieg (Nummer wie loc_api_count) im spekulativen Vorlauf (nur Anzeige, wird verworfen):
 * kein Ton, kein Stopp, keine Freigabe, kein Setter. ALLOW = normal ausfuehren (nur lesend bzw. ohne Ton),
 * SKIP = ohne Wirkung mit S_OK zurueck, FAIL = ohne Wirkung mit Fehler (GetNotification: keine Meldung). */
enum { LAS_SPEC_ALLOW = 0, LAS_SPEC_SKIP = 1, LAS_SPEC_FAIL = 2 };
int las_spec_action(int api);

#ifdef __cplusplus
}
#endif
