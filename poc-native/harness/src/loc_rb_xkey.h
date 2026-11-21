/* loc_rb_xkey.h - W381 (GitHub #61): Schluessel der XACT-Aufrufe fuer Aufzeichnung/Wiedergabe beim Neurechnen
 * (loc_rollback.c, rb_xstop/rb_xstopsrc/rb_xstat).
 *
 * Beim Neurechnen (R3) bedient rb_rep_get je Ereignisart in Reihenfolge: stimmt der Schluessel des naechsten
 * aufgezeichneten Ereignisses, gilt der Aufruf als schon erledigt (die erste Rechnung hat ihn live ausgefuehrt) und
 * laeuft NICHT noch einmal; sonst laeuft er live (nur die korrigierte Zeitlinie macht ihn).
 *
 * Bis W380 enthielt der Schluessel von IXACTSoundBank_Stop nur Cue-Index und Flags. Das Spiel stoppt fast immer
 * per Handle (Cue -1, Flags 0, Handle; W379 §54.1) - alle diese Stopps hatten denselben Schluessel. Stoppte die
 * korrigierte Zeitlinie im selben Tick einen ANDEREN Cue als die verworfene (z. B. den Warnton statt eines
 * anderen Tons), galt ihr Stopp als erledigt und lief nie: der Ton spielte endlos weiter (#61 "bleeping is not
 * stopping"). Ebenso StopSoundCues und GetStatus (Schluessel 0 fuer jede 3D-Quelle).
 *
 * Neu: Handle bzw. Quelle gehen in den Schluessel ein. Handles sind in beiden Zeitlinien gleich (PlayEx/PrepareEx
 * liefern beim Neurechnen den aufgezeichneten Handle), Quellen sind feste Objekte. Stopp nach Index (Handle 0)
 * ergibt denselben Schluessel wie bisher. Reine Funktionen: Selbsttest loc_rbi_selftest (Fall W381). */
#ifndef LOC_RB_XKEY_H
#define LOC_RB_XKEY_H

#include <stdint.h>

/* legacy != 0: Schluessel bis W380 (nur fuer die Gegenprobe im Selbsttest bzw. DIAG loc_rb_opts xkey=0) */
static __inline uint32_t loc_rb_xkey_stop(uint32_t cue, uint32_t flags, uint32_t handle, int legacy)
{
    uint32_t k = cue * 31u + flags;
    if (!legacy && handle) k ^= (handle * 0x9E3779B1u) | 1u;
    return k;
}
static __inline uint32_t loc_rb_xkey_src(uint32_t src, int legacy) /* StopSoundCues / GetStatus */
{
    return legacy ? 0u : src * 0x85EBCA6Bu;
}

#endif
