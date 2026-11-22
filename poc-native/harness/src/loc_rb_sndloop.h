/* loc_rb_sndloop.h - W413 LOC-SOUND-LOOP (GitHub #65 "sounds repeat endlessly online"): Ton nach dem Neurechnen mit der
 * korrigierten Zeitlinie abgleichen (loc_rollback.c, rb_xcue_call/rb_xstop/rb_xstopsrc/rbi_replay_finish).
 *
 * Ausgangslage (W346/W375/W381): Beim Neurechnen gilt ein Cue-Start, den die erste Rechnung (Aufzeichnung) schon live
 * gemacht hat, als erledigt (gleicher Handle); was nur die korrigierte Zeitlinie startet, startet live ("nachgeholt",
 * Nebentabelle). W375 stoppt Schleifen-Cues, die nur die verworfene Zeitlinie gestartet hat ("verwaist") - aber nur fuer
 * Starts aus der Aufzeichnung. Drei Luecken (Modell im Selbsttest loc_rbi_selftest W413 mit sich ueberlappenden
 * Neurechnungen; in den Testpartner-Laeufen W413 bei Tiefe <= 3 nicht aufgetreten):
 *  1. Nachgeholte Waise: Ein Cue, den eine fruehere Neurechnung live gestartet hat (Nebentabelle), startet die naechste
 *     Neurechnung ueber denselben Tick nicht mehr. Er stand nicht in der Aufzeichnung -> nie als Waise erkannt -> eine
 *     Schleife laeuft endlos.
 *  2. Stopp nach Index bzw. Quelle (Handle 0, StopSoundCues) gilt beim Neurechnen als erledigt, wenn die erste Rechnung
 *     im selben Tick genauso stoppte. Er traf live aber nur die Instanzen, die es damals gab - nicht die, die erst die
 *     korrigierte Zeitlinie (nachgeholt) bzw. die erste Rechnung spaeter im Fenster gestartet hat.
 *  3. Waise gestoppt, Ereignis bleibt in der Aufzeichnung: eine spaetere Neurechnung bekam ihren (toten) Handle zurueck
 *     (Ton fehlt). Neu: als tot markiert, die naechste Zeitlinie startet ihn live.
 * Sicherheitsnetz: Ein Schleifen-Cue, den eine Neurechnung beruehrt hat und dessen Handle-Ablage im Spiel (pp von PlayEx/
 * PrepareEx) ihn grace Live-Ticks lang nicht mehr enthaelt, hat keinen Besitzer mehr und endet.
 * Ohne Fehlvorhersage (gleiche Zeitlinien) greift keine Regel: kein Eintrag verwaist, ein erledigter Index-Stopp findet
 * nur schon gestoppte Instanzen, das Netz prueft nur beruehrte Cues. Reine Funktionen: Selbsttest loc_rbi_selftest (W413). */
#ifndef LOC_RB_SNDLOOP_H
#define LOC_RB_SNDLOOP_H

#include <stdint.h>

/* Ereignis-Bits in rb_ev_t.pad (Aufzeichnung): 1 = in der laufenden Neurechnung verwendet, 2 = als Waise gestoppt (tot) */
#define LRS_PAD_USED 1u
#define LRS_PAD_DEAD 2u

/* Nebentabelle (live beim Neurechnen gestartete Cues): Waise der laufenden Neurechnung [g0, g1)? */
static __inline int lrs_side_orphan(uint32_t g, int used, uint32_t g0, uint32_t g1)
{
    return !used && g >= g0 && g < g1;
}

/* Gehoert die Nebentabellen-Instanz am Neurechen-Tick t zur korrigierten Zeitlinie? (vor dem Fenster gestartet = Teil
 * des geladenen Stands; im Fenster nur, wenn diese Neurechnung sie schon verwendet hat) */
static __inline int lrs_side_in_timeline(uint32_t g, int used, uint32_t g0, uint32_t t)
{
    return g < g0 || (used && g <= t);
}

/* Trifft ein Stopp nach Index (stop_cue >= 0) bzw. StopSoundCues (stop_cue < 0, Quelle stop_src) die Instanz? */
static __inline int lrs_stop_covers(int32_t stop_cue, uint32_t stop_src, int inst_cue, uint32_t inst_src)
{
    return stop_cue >= 0 ? inst_cue == (int)stop_cue : (stop_src != 0 && inst_src == stop_src);
}

/* Sicherheitsnetz je Live-Tick: owner = Inhalt der Handle-Ablage, h = Handle des Cues. Rueckgabe 1 = jetzt stoppen. */
static __inline int lrs_net_step(uint32_t owner, uint32_t h, uint16_t *miss, uint32_t grace)
{
    if (!grace) return 0;
    if (owner == h) { *miss = 0; return 0; }
    if (*miss < 0xFFFFu) (*miss)++;
    return *miss >= grace;
}

#endif
