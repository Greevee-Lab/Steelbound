#pragma once
/* loc_audio.h - W298 LOC-AUDIO: XACT-HLE fuer Line of Contact.
 *
 * LoC spielt Ton ueber XACT (Wave-Banks XWB, Sound-Bank Bank.xsb). Statt der
 * stummen W275-Stubs bildet loc_audio.c die vom Spiel gerufenen IXACT*-
 * Einstiege nach (Ebene IXACT, nicht XACT-intern) und spielt jeden Cue als
 * Stimme(n) auf dem gemeinsamen Mischer (audio_out.c/audio_mix.c): PCM und
 * Xbox-ADPCM, Lautstaerke, Tonhoehe, Schleife, Stopp, Kategorien
 * (SetMasterVolume/GlobalPause), 2D-Pan aus den MixBins der Quellen,
 * 3D-Position (audio_3d.c), Stopp-Benachrichtigungen (GetNotification).
 * WMA (2 BGM-Eintraege, Soundtrack-Playlist) bleibt stumm.
 * Nur LoC-Profil, nur mit [audio] audio=1 und loc_audio=1 (Standard). */

#ifdef __cplusplus
extern "C" {
#endif

/* Nach den stummen Audio-Stubs aus loc_boot_install: setzt die XACT-HLE-
 * Einstiege (ueberschreibt dieselben Adressen). 1 = aktiv, 0 = stumm. */
int loc_audio_install(void);
/* Watchdog-Takt (Sekunden): Statistik alle 30 s. */
void loc_audio_tick(int waited_s);

#ifdef __cplusplus
}
#endif
