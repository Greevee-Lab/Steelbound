/* loc_wma.h - W367 LOC-WMA: WMA-Soundtrack (IXACTWmaPlayList) fuer Line of Contact.
 *
 * Teil 1 (ohne Spiel-/Mischer-Abhaengigkeit, im Selbsttest pruefbar):
 *   - Playlist-Modell wie IXACTWmaPlayList (Create/Add/Next/GetCurrentSongInfo/SetPlaybackBehavior/Release)
 *   - WMA-Dekodierung ueber Windows Media Foundation (Source Reader auf einem Speicher-Stream, Systemdecoder),
 *     dynamisch geladen (mfplat/mfreadwrite fehlen z. B. in Windows-N-Editionen -> Fehler, kein Ladeabbruch).
 * Teil 2 (nur im Client, nicht mit LW_SELFTEST): Player-Thread, der die Lieder dekodiert und als Stream-Stimme in
 *   den gemeinsamen Mischer speist. Der Spiel-Thread setzt nur Soll-Zustaende (nie blockierend).
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LW_MAX_SONGS 100
#define LW_MAX_PL 4
#define LW_PATH 96

/* Eintragsarten (XACT_WMA_PLAYLIST_ENTRY.dwType, am Spiel belegt: 2 = Datei, 4 = Xbox-Soundtrack der Festplatte) */
#define LW_ENTRY_FILE 2u
#define LW_ENTRY_SOUNDTRACK 4u
#define LW_SONG_ID_BASE 0x5700u /* Lied-ID (Add-Ausgabe = GetCurrentSongInfo), nie 0 */

typedef struct {
    uint32_t type;
    char path[LW_PATH];  /* Datei (z. B. "z:\Patrol.wma") */
    uint32_t st_id, song_id; /* Soundtrack-Eintrag: IDs (nicht abspielbar: keine Nutzer-Soundtracks im HLE) */
    uint32_t id;         /* vergebene Lied-ID */
} lw_entry_t;

typedef struct {
    uint32_t kind;       /* LW_K_PL wenn belegt */
    uint32_t sb;         /* Sound-Bank (Gast-/HLE-Zeiger) */
    int cue;
    uint32_t flags;      /* Create-Flags (Bit 0 = Zufall, Bit 1 = Wiederholen; am Spiel: 2 bzw. 3) */
    uint32_t behavior;   /* SetPlaybackBehavior (2 = der Reihe nach, 3 = Zufall; am Spiel belegt) */
    int n;
    int cur;             /* aktuelles Lied, -1 = vor dem ersten Next */
    uint32_t rnd;
    lw_entry_t e[LW_MAX_SONGS];
} lw_pl_t;
#define LW_K_PL 0x4C57504Cu

/* Playlist-Modell (rein, kein Sperren; Aufrufer serialisiert) */
void lw_pl_init(lw_pl_t *p, uint32_t sb, int cue, uint32_t flags);
int lw_pl_add(lw_pl_t *p, uint32_t type, const char *path, uint32_t a, uint32_t b, uint32_t *out_id); /* 0 = ok */
int lw_pl_next(lw_pl_t *p); /* naechstes Lied (Zufall: deterministisch), -1 = leer */
int lw_pl_playable(const lw_pl_t *p, int i); /* 1 = Datei-Eintrag */
int lw_pl_shuffle(const lw_pl_t *p);
/* Lied nach dem Ende von i (der Reihe nach bzw. Zufall), -1 = Ende ohne Wiederholung */
int lw_pl_after(lw_pl_t *p, int i);

/* WMA ueber Media Foundation. lw_mf_thread_init je Thread (COM + MFStartup), 0 = ok. */
int lw_mf_thread_init(char *err, int cap);
void lw_mf_thread_done(void);
typedef struct lw_dec lw_dec_t;
lw_dec_t *lw_dec_open(const uint8_t *data, uint32_t n, char *err, int cap); /* Daten werden kopiert */
void lw_dec_format(const lw_dec_t *d, int *rate, int *ch);             /* Ausgabe PCM 16 Bit */
double lw_dec_secs(const lw_dec_t *d);                                  /* Dauer laut Container, 0 = unbekannt */
int lw_dec_read(lw_dec_t *d, uint8_t *dst, uint32_t cap, uint32_t *got); /* 0 = ok, 1 = Ende, <0 = Fehler */
void lw_dec_close(lw_dec_t *d);
/* ASF-Kennung pruefen (Kopf-GUID), 1 = sieht aus wie WMA/ASF */
int lw_is_asf(const uint8_t *data, uint32_t n);

#ifndef LW_SELFTEST
/* ---- Client: Player (loc_audio.c ruft das unter seiner Sperre, alles nicht blockierend) ---- */
int lw_player_start(const lw_pl_t *p, int db100, int ucat, int paused, int auto_mode); /* 1 = gestartet */
void lw_player_jump(const lw_pl_t *p);  /* Next waehrend der Wiedergabe: auf p->cur wechseln */
void lw_player_stop(void);
void lw_player_volume(int db100);
void lw_player_pause(int on);
int lw_player_active(void);             /* 1 = spielt (oder startet) */
long lw_player_starts(void);            /* Zaehler Liedstarts (je Start eine Meldung wType 0) */
int lw_player_song(void);               /* Index des laufenden Lieds, -1 = keins */
void lw_player_stats(char *buf, int cap);
void lw_player_shutdown(void);
#endif

#ifdef __cplusplus
}
#endif
