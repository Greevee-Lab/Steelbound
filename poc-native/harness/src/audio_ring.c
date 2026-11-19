/* audio_ring.c - ISSUE-10 (GitHub #10, "doppelter Ton" nach Film-Stocken):
 * Unterlauf und Cursor-Reset der Lock/Unlock-Ringstimmen (Bink-Filmton).
 *
 * Modell (audio_out.c/audio_mix.c): Was das Spiel per Unlock in den Ring
 * schreibt, haengt an eine FIFO an; der Mischer spielt die FIFO ab. Laeuft
 * sie leer, spielte er bisher die per SetBufferData uebergebene Ring-
 * Momentaufnahme (Stand beim Filmstart) in Schleife weiter - alter Inhalt.
 *
 * Befund ISSUE-10 (I10_st1, Bink-Fuetterung im Film 3 s blockiert): Nach dem
 * Haenger holt Bink erst ~0,5 s Ton im Schwall nach und setzt dann jede Spur
 * neu auf (SetCurrentPosition(0) + ganzen Ring neu beschreiben). Die FIFO
 * behielt dabei die schon angehaengten, noch ungespielten Frames der alten
 * Ringrunde; die Neubefuellung lief dahinter -> derselbe Filmton ein zweites
 * Mal (Echo +0,6...0,7 s), die Spuren danach gegeneinander und gegen das Bild
 * verschoben (FIFO 77000-89000 statt 26624 Frames, bis Filmende).
 * Auf der Xbox gibt es diese Warteschlange nicht: der Ring hat genau eine
 * Runde, ein Cursor-Reset spielt ab dem neu geschriebenen Inhalt.
 *
 * Fix (audio_ring_underrun=1, Standard; 0 = alt), nur Bink-Ringstimmen:
 *  - Unterlauf (FIFO leer, Stimme schon gefuettert): Stille statt der alten
 *    Ring-Momentaufnahme; Uhr/Cursor laufen unveraendert weiter.
 *  - au_ring_place: Schreibt das Spiel nach dem Reset Ringstellen neu, deren
 *    alter Inhalt noch nicht gespielt ist, ist das ein Ueberschreiben (W83-
 *    Pfad) statt neuer Runde - sonst wuchs die FIFO um bis zu 80000 Frames
 *    (1,9 s, Spuren gegeneinander verschoben; I10_ns/I10_st3).
 *  - au_ring_first_start: Das W205-Startgatter (alle Spuren gemeinsam starten)
 *    gilt nur beim ersten Start, nicht nach einem Cursor-Reset - sonst hielt
 *    es die zuerst neu gestartete Leitspur an und sie lief danach dauerhaft
 *    ~0,6-0,7 s hinter den anderen Spuren (Hauptursache des Doppeltons).
 *  - au_ring_reset_flush: Cursor-Reset nach Wiedergabe verwirft die
 *    ungespielten FIFO-Frames; die ersten AU_RING_FADE davon bleiben und
 *    werden ausgeblendet (kein Knacken).
 *  - au_ring_gain: erster neuer Ton nach Unterlauf/Reset wird ueber
 *    AU_RING_FADE Frames eingeblendet; sinkt die FIFO unter AU_RING_FADE
 *    Frames (nur kurz vor einem Unterlauf bzw. am Filmende), werden die
 *    letzten Frames ausgeblendet (Aufruf mit fifo_n = Rest nach Entnahme).
 * Ohne Unterlauf/Reset keine Multiplikation: Standardpfad bitgleich.
 * Reine Funktionen auf audio_voice_t, ohne Windows-Aufrufe (Selbsttest
 * tests/audio_ring_selftest.c). Aufruf unter au_lock. */
#include "audio_int.h"

int au_ring_first_start(const audio_voice_t *v, int fix)
{
    /* W205-Startgatter (audio_bink_sync) haelt Bink-Spuren mit played == 0 an, bis alle
     * gestartet sind - gedacht fuer den Filmstart. Nach einem Cursor-Reset ist played
     * wieder 0: Bink startet dann die Leitspur (Spur 0) sofort und fuellt die anderen Spuren
     * stueckweise zeitgleich nach (I10_st8: Play v136, 0,57 s spaeter v137-v141, deren Ring
     * ab der dann aktuellen Filmzeit). Das Gatter hielt die Leitspur so lange an -> sie lief
     * dauerhaft ~0,6-0,7 s hinter den anderen (= gemessener Doppelton). Mit Fix gilt es nur
     * fuer Stimmen, die noch nie gespielt haben. */
    return !(fix && v->ring_once);
}

uint64_t au_ring_place(uint64_t ao, au32 n, uint64_t front, uint64_t cur, au32 buflen)
{
    /* Ein Ring haelt hoechstens eine Runde ungespielter Daten. Wuerde ein Unlock an/nach der
     * Frontier (neue Runde) die FIFO auf mehr als 1,5 Runden ueber den Cursor hinaus
     * verlaengern, ist es ein Neubeschreiben der laufenden Runde (Bink nach Cursor-Reset:
     * ganzer Ring ab 0, nochmals ganz, dann ab x bis Ringende) -> eine Runde zurueck, damit
     * der W83-Pfad die ungespielten Frames ueberschreibt. Im Streaming endet jeder Unlock
     * spaetestens am vom Spiel gelesenen Cursor + buflen; die halbe Runde ist Sicherheitsabstand. */
    if (buflen && ao >= front && ao >= (uint64_t)buflen &&
        ao + n > cur + buflen + buflen / 2)
        return ao - buflen;
    return ao;
}

au32 au_ring_reset_flush(audio_voice_t *v)
{
    au32 keep, j, drop;
    if (!v->fifo || !v->fifo_cap || !v->fifo_n)
        return 0;
    /* nach Unterlauf noch nichts gespielt (Stille laeuft): nichts ausblenden, alles verwerfen */
    keep = v->ring_starved ? 0 : (v->fifo_n < AU_RING_FADE ? v->fifo_n : AU_RING_FADE);
    for (j = 0; j < keep; j++) {
        au32 pos = (v->fifo_rd + j) % v->fifo_cap;
        float g = (float)(keep - j - 1) / (float)keep; /* letzter Frame 0 */
        if (v->ring_ramp && j < v->ring_ramp) /* laufendes Einblenden mitnehmen */
            g *= (float)(AU_RING_FADE - v->ring_ramp + j + 1) / (float)(AU_RING_FADE + 1);
        v->fifo[pos * 2] *= g;
        v->fifo[pos * 2 + 1] *= g;
    }
    drop = v->fifo_n - keep;
    v->fifo_n = keep;
    v->fifo_wr = (v->fifo_rd + keep) % v->fifo_cap;
    v->ring_ramp = 0;
    v->ring_hold = keep; /* ausgeblendeter Rest laeuft mit Faktor 1 aus */
    v->ring_starved = 1; /* danach wird der neue Ring-Inhalt eingeblendet */
    return drop;
}

float au_ring_gain(audio_voice_t *v)
{
    float k = 1.0f;
    if (v->ring_hold) { /* nach Reset: Rest ist schon ausgeblendet */
        v->ring_hold--;
        return 1.0f;
    }
    if (v->ring_starved) { /* erster Frame nach Unterlauf/Reset */
        v->ring_starved = 0;
        v->ring_ramp = AU_RING_FADE;
    }
    if (v->ring_ramp) {
        v->ring_ramp--;
        k = (float)(AU_RING_FADE - v->ring_ramp) / (float)(AU_RING_FADE + 1);
    }
    if (v->fifo_n < AU_RING_FADE) /* FIFO laeuft leer: letzte Frames ausblenden, letzter = 0 */
        k *= (float)v->fifo_n / (float)AU_RING_FADE;
    return k;
}
