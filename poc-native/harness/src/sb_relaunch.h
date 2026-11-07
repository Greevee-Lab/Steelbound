#pragma once
/* sb_relaunch.h - W186 RESET-TITLE: Prozess-Neustart fuer XLaunchNewImage.
 *
 * Das Spiel ruft bei CONFIGURATION/CONFIG -> BACK TO TITLE -> YES
 * XLaunchNewImageA("D:\default.xbe", &LaunchData) (Disasm FUN_000E0140,
 * Aufrufstelle 0xE066D, Szenen 6 und 0x1F) und erwartet einen Neustart der
 * Konsole. Der Client startet sich dafuer selbst neu: gleiche Kommandozeile,
 * gleiche Umgebung (SB_CFG_SET/SB_OUT_DIR/SB_HDD_ROOT/...), gleicher
 * Arbeitsordner, geerbte Std-Handles (boot.log). Der neue Prozess wartet,
 * bis der alte vollstaendig beendet ist (Ton, D3D, VR-Session, WinUSB-Handle,
 * Logdatei sind dann frei), haengt an sb_harness.log an und startet beim
 * Boot/Titel. Ablauf/Umgebungsvariablen: BUILD.md [game] xlaunch_self. */
#ifdef __cplusplus
extern "C" {
#endif

/* Ganz am Anfang von sb_emulate (vor dem Log): merkt Startzeit + Arbeits-
 * ordner; im Neustart-Kind: wartet auf das Ende des Elternprozesses. */
void sb_relaunch_boot(void);
/* >0 = dieser Prozess ist der N-te Neustart (SB_RELAUNCH_N). */
int sb_relaunch_child(void);
/* Kind: Rest-Laufzeit eines Testlaufs (SB_RELAUNCH_RUN_SECONDS), sonst -1. */
int sb_relaunch_run_seconds(void);
/* Kind, nach dem Log-Start: Zeile RELAUNCH + Ersatz-Set (replay_steps=...)
 * anwenden (nach cmdline-Set, gewinnt). */
void sb_relaunch_after_cfg(void);
/* Kind: Fensterposition des Elternprozesses (nur sichtbares Fenster, liegt
 * auf einem Monitor). 1 = x/y gesetzt. */
int sb_relaunch_winpos(int *x, int *y);
/* XLaunchNewImage auf das eigene Image: Neustart (xlaunch_self=1) bzw.
 * sauberes Ende (2). Kehrt nur zurueck, wenn nichts davon moeglich war
 * (Aufrufer verhaelt sich dann wie bisher: ignorieren). */
void sb_relaunch_exec(const char *path);

#ifdef __cplusplus
}
#endif
