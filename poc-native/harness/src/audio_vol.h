/* audio_vol.h - W303 AUDIO-VOLUME: Lautstaerkeregler Gesamt, Musik, Effekte,
 * Sprache/Funk (user.cfg volume_master/_music/_sfx/_voice, 0..100, Standard
 * 100 = Original). Reine Rechenlogik ohne Abhaengigkeiten (Mischer in
 * audio_mix.c, LoC-XACT-HLE in loc_audio.c, Selbsttest in audio_vol.c).
 *
 * Lineare Amplitude: 50 % = Faktor 0,5 (etwa -6 dB), 0 % = stumm. Bei 100 %
 * liefern die Funktionen exakt 1.0f, und der Mischer multipliziert dann gar
 * nicht (bitgleicher Ton zum Stand vor W303).
 *
 * Kategorie einer Stimme (avol_category):
 *  - explizit gesetzt (LoC: XSB-Kategorie ueber avol_loc_cat) -> diese
 *  - Bink-Filmton (Erzeuger 0x2435FC) -> FILM: nur Gesamt (Filme mischen
 *    Sprache und Musik in einer Spur)
 *  - Stream mono -> SPRACHE/FUNK (Voice/*.WAV: ADPCM mono 11025 Hz)
 *  - Stream mehrkanalig -> MUSIK (BGM/STRM Stereo bzw. 5.1). Bekannte
 *    Unschaerfe: die 3D-Streams (0x1071F8) sind 6-kanalig und landen damit
 *    ebenfalls bei Musik.
 *  - alles andere (SFX-Puffer-Pool, Menuetoene audio_ui_play) -> EFFEKTE */
#ifndef AUDIO_VOL_H
#define AUDIO_VOL_H

enum { AVOL_AUTO = 0, AVOL_FILM, AVOL_MUSIC, AVOL_SFX, AVOL_VOICE, AVOL_NCAT };

/* Prozentwert auf 0..100 klemmen (cfg, ESC, Launcher). */
static __inline int avol_clamp(int pct)
{
    return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

/* Prozent -> linearer Faktor; 100 -> exakt 1.0f, 0 -> 0.0f. */
static __inline float avol_pct(int pct)
{
    pct = avol_clamp(pct);
    return pct == 100 ? 1.0f : (float)pct / 100.0f;
}

/* 1 = alle Regler auf 100 (Mischer ueberspringt dann alles). */
static __inline int avol_all100(int master, int music, int sfx, int voice)
{
    return master >= 100 && music >= 100 && sfx >= 100 && voice >= 100;
}

/* Kategorie einer Stimme. ucat = explizit gesetzte Kategorie (AVOL_AUTO =
 * keine), kind 1 = Puffer, 2 = Stream, ch = Kanalzahl der Quelle (0 =
 * unbekannt), is_film = Bink-Filmton. */
static __inline int avol_category(int ucat, int kind, int ch, int is_film)
{
    if (ucat > AVOL_AUTO && ucat < AVOL_NCAT) {
        return ucat;
    }
    if (is_film) {
        return AVOL_FILM;
    }
    if (kind == 2) {
        return ch == 1 ? AVOL_VOICE : AVOL_MUSIC;
    }
    return AVOL_SFX;
}

/* Faktor einer Kategorie inklusive Gesamt. Exakt 1.0f, wenn Gesamt und der
 * Kategoriewert 100 sind; ein Faktor 1 wird nie multipliziert. */
static __inline float avol_factor(int cat, int master, int music, int sfx, int voice)
{
    int c = cat == AVOL_MUSIC ? music : cat == AVOL_VOICE ? voice : cat == AVOL_FILM ? 100 : sfx;
    float fm = avol_pct(master), fc = avol_pct(c);
    if (fm == 1.0f) {
        return fc;
    }
    if (fc == 1.0f) {
        return fm;
    }
    return fm * fc;
}

/* LoC (XACT-HLE): XSB-Kategorie -> Nutzerkategorie. 0 Ambience, 1 VT-Motoren
 * -> Effekte; 2 Soundtrack, 4 BGM -> Musik; 3 Hinweise -> Sprache/Funk;
 * uebrige -> Effekte. */
static __inline int avol_loc_cat(int xsb_cat)
{
    switch (xsb_cat) {
    case 2: case 4: return AVOL_MUSIC;
    case 3: return AVOL_VOICE;
    default: return AVOL_SFX;
    }
}

/* Selbsttest (input_selftest): 0 = ok, sonst Anzahl Fehler. */
int avol_selftest(void);

#endif /* AUDIO_VOL_H */
