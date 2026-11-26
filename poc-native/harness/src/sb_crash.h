#pragma once
/* sb_crash.h - W279 DIAG-CRASH: Absturz- und Haenger-Berichte des Clients.
 *
 * Ordner <crash>\<JJJJMMTT-hhmmss>[-nnn]-crash|hang\ (<crash> = Datenordner\crash
 * im Modus profile/portable, legacy/Tests: <Ausgabeordner>\crash):
 *   crash_<k>.txt  Text wie bisher (W94: RIP, Modul+Offset, Register, Stack, cfg)
 *   crash_<k>.dmp  Minidump (MiniDumpWithIndirectlyReferencedMemory |
 *                  MiniDumpScanMemory, kein Voll-Speicher), bereinigt
 *   hang.txt/.dmp  Haenger: Stacks aller Spiel-Threads + Minidump
 *   info.txt       Version/Build-ID (PDB-GUID), Spielfassung, GPU+Treiber,
 *                  Anzeige, Spielzustand/Szene, aktive Packs mit Pruefsumme,
 *                  cfg-Abweichungen von sb_native.cfg
 *   log_tail.txt   Ende des Client-Logs ohne Eingabe-Zeilen
 *   status.txt     fatal | first-chance | hang
 *   recovered.txt  Spiel lief danach weiter (erste Chance gefangen, Haenger vorbei)
 * Alles ist bereinigt (sb_diagcore: Datenordner -> <data>, Client-Ordner ->
 * <client>, Benutzer-/Rechnername, Pfade). Nichts wird hochgeladen; der Launcher
 * zeigt beim naechsten Start einmal "The last session crashed".
 *
 * cfg [diag]: crash_dump=1 (0 = nur Text), hang_report=1, hang_secs=20 (so
 * lange ohne Spielbild, Pause zaehlt nicht), crash_test=N (TEST: nach N s
 * Zugriffsverletzung im Watchdog-Thread), hang_test=N (TEST: nach N s alle
 * Spiel-Threads anhalten -> echter Haenger). */
#include <windows.h>
#include "sb_gamever.h"

#ifdef __cplusplus
extern "C" {
#endif

/* hcfg.c: Schluessel dieses Moduls (1 = behandelt). */
int sb_crash_cfg_key(const char *k, const char *v);
/* emu_main.c: nach cfg + Datenort (dir_acp leer = <Ausgabeordner>\crash). */
void sb_crash_init(const char *dir_acp, const char *cmdline_set);
/* emu_main.c: Spielfassung bekannt, Render-Geraet steht -> feste Angaben. */
void sb_crash_note_game(const sb_gamever_t *gv);
/* sb_remaster.c: packs.json beim Laden (leer = keine Packs). */
void sb_crash_note_packs(const char *packs_json);
/* emu_main.c: Watchdog laeuft -> Hilfs-Thread fuer den Minidump (auch bei
 * Stack-Ueberlauf) starten. */
void sb_crash_start(void);
/* emu_main.c crash_report(): Bericht schreiben. text = bisheriger Text
 * (wird bereinigt kopiert), k = laufende Nummer (1..3), fatal = Prozess endet.
 * out_path = Pfad der Textdatei (fuer die CRASHLOG-Zeile). 1 = ok. */
int sb_crash_report(EXCEPTION_POINTERS *ep, const char *why, const char *text, int k, int fatal,
                    char *out_path, size_t cap);
/* emu_main.c watchdog: alle 5 s (Test-Schalter, Haenger, Erholung). */
void sb_crash_tick(int waited);
/* W516 ISSUE-114: Grafikgeraet verloren (d3d11_render.c sb_render_device_lost): fataler Bericht im Absturz-
 * Ordner (status fatal, reason = Kurzzeile, gpu_lost.txt = detail, gpu_lost.dmp = Minidump aller Threads,
 * log_tail.txt). Jeder Bericht (auch Absturz/Haenger) fuehrt in info.txt die Zeile "gpu-device:"
 * (GetDeviceRemovedReason). 1 = geschrieben. */
int sb_crash_gpu_lost(const char *reason, const char *detail);
/* TEST(W516) [diag] gpu_lost_test=N: Sekunden bis zum kuenstlichen Geraeteverlust (0 = aus). */
int sb_crash_gpu_lost_test(void);
/* W522 CRASH-TELEMETRY: Geraeteverlust als Datensatz fuer den Netzbericht (nur mit net_report=1), vor
 * sb_crash_gpu_lost. hr = Ergebnis von Present, why = GetDeviceRemovedReason. */
void sb_crash_tel_gpu(long hr, long why);

#ifdef __cplusplus
}
#endif
