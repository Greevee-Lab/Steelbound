/* audio_vol.c - W303 AUDIO-VOLUME: Selbsttest der Rechenlogik (audio_vol.h).
 * Laeuft in input_selftest (Eintrag "W303 Lautstaerke"). */
#include "audio_vol.h"
#include <stdio.h>
#include <string.h>

static int g_avol_fail;

static void avol_check(int ok, const char *what)
{
    if (!ok) {
        g_avol_fail++;
        printf("  FAIL W303 Lautstaerke: %s\n", what);
    }
}

int avol_selftest(void)
{
    float one = 1.0f, f;
    g_avol_fail = 0;
    /* Prozent -> Faktor: 100 exakt 1 (bitgleich), 50 = 0,5, 0 = stumm */
    f = avol_pct(100);
    avol_check(memcmp(&f, &one, sizeof f) == 0, "100 % -> exakt 1.0f");
    avol_check(avol_pct(50) == 0.5f, "50 % -> 0,5");
    avol_check(avol_pct(0) == 0.0f, "0 % -> 0");
    avol_check(avol_pct(25) == 0.25f, "25 % -> 0,25");
    /* Klemmen */
    avol_check(avol_clamp(-5) == 0 && avol_clamp(150) == 100 && avol_clamp(42) == 42, "Klemmen 0..100");
    avol_check(avol_pct(250) == 1.0f && avol_pct(-1) == 0.0f, "Faktor ausserhalb geklemmt");
    avol_check(avol_all100(100, 100, 100, 100) && !avol_all100(100, 99, 100, 100) &&
               !avol_all100(0, 100, 100, 100), "alle 100");
    /* Kategorie aus kind/Kanaelen/Film/explizit */
    avol_check(avol_category(AVOL_AUTO, 1, 2, 0) == AVOL_SFX, "Puffer -> Effekte");
    avol_check(avol_category(AVOL_AUTO, 1, 1, 0) == AVOL_SFX, "Puffer mono -> Effekte");
    avol_check(avol_category(AVOL_AUTO, 2, 1, 0) == AVOL_VOICE, "Stream mono -> Sprache");
    avol_check(avol_category(AVOL_AUTO, 2, 2, 0) == AVOL_MUSIC, "Stream stereo -> Musik");
    avol_check(avol_category(AVOL_AUTO, 2, 6, 0) == AVOL_MUSIC, "Stream 5.1 -> Musik");
    avol_check(avol_category(AVOL_AUTO, 2, 0, 0) == AVOL_MUSIC, "Stream ohne Format -> Musik");
    avol_check(avol_category(AVOL_AUTO, 1, 2, 1) == AVOL_FILM, "Bink -> Film");
    avol_check(avol_category(AVOL_MUSIC, 1, 2, 1) == AVOL_MUSIC, "explizit gewinnt (Film)");
    avol_check(avol_category(AVOL_VOICE, 2, 2, 0) == AVOL_VOICE, "explizit gewinnt (Stream)");
    avol_check(avol_category(99, 2, 1, 0) == AVOL_VOICE, "ungueltig explizit -> automatisch");
    /* Faktor je Kategorie */
    f = avol_factor(AVOL_SFX, 100, 100, 100, 100);
    avol_check(memcmp(&f, &one, sizeof f) == 0, "alles 100 -> exakt 1");
    f = avol_factor(AVOL_MUSIC, 100, 100, 50, 0);
    avol_check(memcmp(&f, &one, sizeof f) == 0, "Musik 100 bei anderen Werten -> exakt 1");
    avol_check(avol_factor(AVOL_MUSIC, 100, 50, 100, 100) == 0.5f, "Musik 50");
    avol_check(avol_factor(AVOL_SFX, 100, 50, 30, 100) == 0.3f, "Effekte 30");
    avol_check(avol_factor(AVOL_VOICE, 100, 100, 100, 0) == 0.0f, "Sprache 0 stumm");
    avol_check(avol_factor(AVOL_VOICE, 50, 100, 100, 50) == 0.25f, "Gesamt 50 x Sprache 50");
    avol_check(avol_factor(AVOL_SFX, 0, 100, 100, 100) == 0.0f, "Gesamt 0 stumm");
    avol_check(avol_factor(AVOL_FILM, 100, 0, 0, 0) == 1.0f, "Film nur Gesamt (100)");
    avol_check(avol_factor(AVOL_FILM, 50, 0, 0, 0) == 0.5f, "Film nur Gesamt (50)");
    avol_check(avol_factor(AVOL_AUTO, 100, 100, 40, 100) == 0.4f, "unbekannt -> Effekte");
    /* LoC: XSB-Kategorie -> Nutzerkategorie */
    avol_check(avol_loc_cat(0) == AVOL_SFX, "LoC 0 Ambience -> Effekte");
    avol_check(avol_loc_cat(1) == AVOL_SFX, "LoC 1 VT-Motoren -> Effekte");
    avol_check(avol_loc_cat(2) == AVOL_MUSIC, "LoC 2 Soundtrack -> Musik");
    avol_check(avol_loc_cat(3) == AVOL_VOICE, "LoC 3 Hinweise -> Sprache");
    avol_check(avol_loc_cat(4) == AVOL_MUSIC, "LoC 4 BGM -> Musik");
    avol_check(avol_loc_cat(5) == AVOL_SFX && avol_loc_cat(7) == AVOL_SFX, "LoC uebrige -> Effekte");
    avol_check(avol_category(avol_loc_cat(3), 1, 2, 0) == AVOL_VOICE, "LoC-Kategorie an Puffer");
    printf("W303 Lautstaerke: %d Fehler\n", g_avol_fail);
    return g_avol_fail;
}
