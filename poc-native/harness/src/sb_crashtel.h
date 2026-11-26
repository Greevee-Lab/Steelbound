#pragma once
/* sb_crashtel.h - W522 CRASH-TELEMETRY: anonyme Absturzklasse mit Ort fuer den Netzbericht (nur mit Zustimmung).
 *
 * Weg der Daten (der abstuerzende Prozess sendet nie selbst):
 *   1. Absturz/Haenger/Geraeteverlust (sb_crash.c): kleiner fester Datensatz in <Ausgabeordner>\net_crash.txt, nur
 *      mit net_report=1. Nur vorab berechnete Pfade, Stack-Puffer, eine Datei schreiben - keine Allokation, kein
 *      Warten. Fehler hier aendern nichts am bisherigen Bericht (Minidump, info.txt).
 *   2. Launcher (ueberlebt den Absturz): ordnet nach Prozessende den Exitcode ein (sbct_launcher_note) - ergaenzt
 *      einen vorhandenen Datensatz um "xc" bzw. legt ihn an (von aussen beendet, Fenster zu ohne geordnetes Ende,
 *      Absturz ohne Datensatz). Der Launcher sendet nichts.
 *   3. Naechster LoC-Start mit steelnet und Zustimmung (loc_tun_netrep.c nr_start): Datensatz lesen und streng
 *      pruefen, als net_report "crash" einreihen, Datei loeschen. Startet niemand mehr das Spiel, kommt nichts an.
 *
 * Inhalt (nur Codes und Zahlen, steelnet PROTOKOLL.md "W522"): Klasse, Ausnahmecode, Lesen/Schreiben/Ausfuehren,
 * Modul als feste Kennung (Fremdmodule -> "other", Grafiktreiber nur als Hersteller) + RVA im Modul (Gastcode: die
 * Gast-EIP), Bucket = FNV-1a ueber Modul+RVA der obersten Rahmen, Phase, SB/LoC, Laufzeit- und Speicherklasse,
 * DXGI-Grund, Renderer-Bits, GPU-Hersteller (PCI-ID), Version, Fassung, Neustart-Merker. Nie: Pfade, Modulnamen
 * von Fremdprogrammen, Register ausser EIP/RVA, Speicherinhalt, Namen, Eingaben.
 *
 * Datei (nur lokal, eine Zeile): "SBCT1 ts=<Unix-s> cls=av exc=c0000005 acc=w mod=sb_emu rva=0001a2b3 ..." -
 * ts bleibt lokal (nur fuer den Neustart-Merker "rs"). Der Leser verwirft die ganze Zeile bei unbekanntem
 * Schluessel oder Wert. */
#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SBCT_FILE "net_crash.txt"
#define SBCT_MARK "net_report_run.txt" /* Laufmarke von loc_tun_netrep.c (W496) */
#define SBCT_RESTART_S 120              /* Neustart-Merker: naechster Start innerhalb von 2 Minuten */

enum { SBCT_C_NONE = 0, SBCT_C_EXIT_OK, SBCT_C_CLOSED, SBCT_C_KILLED, SBCT_C_HANG, SBCT_C_GPU_LOST, SBCT_C_AV,
       SBCT_C_STACK, SBCT_C_OOM, SBCT_C_ABORT, SBCT_C_OTHER, SBCT_C_N };
enum { SBCT_M_NONE = 0, SBCT_M_SB_EMU, SBCT_M_SB_LDR, SBCT_M_GAME, SBCT_M_D3D11, SBCT_M_DXGI, SBCT_M_D3DCOMP,
       SBCT_M_NTDLL, SBCT_M_KERNEL, SBCT_M_CRT, SBCT_M_AMD, SBCT_M_NVIDIA, SBCT_M_INTEL, SBCT_M_OTHER, SBCT_M_N };
enum { SBCT_P_NONE = 0, SBCT_P_MENU, SBCT_P_LOBBY, SBCT_P_LOADING, SBCT_P_MATCH, SBCT_P_DEBRIEF, SBCT_P_N };
enum { SBCT_D_NONE = 0, SBCT_D_OK, SBCT_D_HUNG, SBCT_D_REMOVED, SBCT_D_RESET, SBCT_D_DRIVER, SBCT_D_INVALID,
       SBCT_D_OOM, SBCT_D_OTHER, SBCT_D_N };
enum { SBCT_G_NONE = 0, SBCT_G_SB, SBCT_G_LOC };
enum { SBCT_A_NONE = 0, SBCT_A_READ, SBCT_A_WRITE, SBCT_A_EXEC };
enum { SBCT_E_NONE = 0, SBCT_E_COMMUNITY, SBCT_E_PRIVATE };

/* Renderer-/Experimental-Bits (feste Bedeutung, PROTOKOLL.md "W522") */
#define SBCT_RB_SHADING 0x01u   /* shading=1 */
#define SBCT_RB_INTERP 0x02u    /* interp_fps > 0 */
#define SBCT_RB_REMASTER 0x04u  /* remaster=1 */
#define SBCT_RB_HDTEX 0x08u     /* hd_textures=1 (mit remaster) */
#define SBCT_RB_AA 0x10u        /* aa_mode > 0 */
#define SBCT_RB_MONFX 0x20u     /* monitor_fx = 1 */
#define SBCT_RB_FULL 0x40u      /* window_mode != 0 (randlos/exklusiv) */
#define SBCT_RB_VR 0x80u        /* vr_enable=1 */
#define SBCT_RB_MASK 0xFFu

typedef struct sbct_rec {
    unsigned ts;    /* Unix-Sekunden des Ereignisses (nur lokal) */
    int cls;        /* SBCT_C_* */
    int xc;         /* Einordnung des Launchers nach dem Exitcode (SBCT_C_*, 0 = keine) */
    unsigned exc;   /* Ausnahmecode (0 = keiner) */
    int acc;        /* SBCT_A_* */
    int mod;        /* SBCT_M_* */
    unsigned rva;   /* Offset im Modul bzw. Gast-EIP (mod = game) */
    unsigned bkt;   /* Bucket (0 = keiner) */
    int ph;         /* SBCT_P_* */
    int game;       /* SBCT_G_* */
    int rt;         /* Laufzeitklasse 1..5 (0 = unbekannt): <1, 1-5, 5-15, 15-60, >60 min */
    int mem;        /* Speicherklasse 1..4 (0 = unbekannt): <1, 1-2, 2-3, >3 GB belegter Adressraum */
    int dxr;        /* SBCT_D_* */
    unsigned rb;    /* SBCT_RB_* */
    unsigned gpu;   /* PCI-Hersteller-ID (0 = unbekannt) */
    int ed;         /* SBCT_E_* */
    char ver[17];   /* [0-9A-Za-z.+-]{1,16} oder leer */
} sbct_rec;

/* Namen der Codes (NULL = ungueltig/0) */
const char *sbct_cls_name(int c);
const char *sbct_mod_name(int m);
const char *sbct_ph_name(int p);
const char *sbct_dxr_name(int d);
const char *sbct_rt_name(int r);
const char *sbct_mem_name(int m);

/* Einordnung */
int sbct_exc_cls(unsigned code);                    /* Ausnahmecode -> SBCT_C_* */
int sbct_mod_from_name(const char *base);           /* Dateiname eines Moduls (ohne Pfad) -> SBCT_M_* */
int sbct_mod_at(uintptr_t addr, unsigned *rva);     /* Adresse -> Modul + RVA (Gastbereich -> game, rva = EIP) */
unsigned sbct_bucket(int mod, unsigned rva, const CONTEXT *cx); /* FNV-1a ueber Modul+RVA und bis zu 4 Ruecksprungstellen */
unsigned sbct_bucket_frames(const int *mod, const unsigned *rva, int n); /* dasselbe ueber fertige Rahmen (Haenger) */
int sbct_is_sys(int mod);                           /* ntdll/kernel/crt: Wartestellen, fuer den Ort uebersprungen */
int sbct_rt_class(unsigned secs);
int sbct_mem_class(void);                           /* belegter Adressraum des Prozesses */
int sbct_dxr_from_hr(long hr);
unsigned sbct_now(void);                            /* Unix-Sekunden */
void sbct_clean_ver(const char *in, char *out, int cap); /* nur [0-9A-Za-z.+-], 1..16, sonst leer */

/* Phase des laufenden Spiels (LoC: loc_tun_netrep.c, SB: sb_crash.c zur Absturzzeit) */
void sbct_set_phase(int ph);
int sbct_phase(void);

/* Datensatz <-> Zeile bzw. JSON-Felder fuer den net_report */
int sbct_format(char *out, int cap, const sbct_rec *r);        /* Zeile mit '\n'; Laenge, -1 = zu klein */
int sbct_parse(const char *line, sbct_rec *r);                 /* 1 = gueltig, 0 = verworfen */
int sbct_json(char *out, int cap, const sbct_rec *r, unsigned now); /* ",\"cls\":..." (ohne ts); Laenge, -1 */

/* Datei (UTF-16-Pfad; dir in ACP oder UTF-8) */
int sbct_path(wchar_t *out, int cap, const char *dir, int utf8, const char *name);
int sbct_write(const wchar_t *path, const sbct_rec *r);        /* 1 = geschrieben (keine Allokation) */
int sbct_read(const wchar_t *path, sbct_rec *r);               /* 1 = gueltig, 0 = keine Datei, -1 = verworfen */

/* Launcher nach Prozessende: Exitcode -> Klasse (0 = nichts festzuhalten). marker = Laufmarke liegt noch. */
int sbct_exit_cls(unsigned long code, int marker);
/* Launcher: Datensatz ergaenzen (2) bzw. anlegen (1), 0 = nichts zu tun, -1 = Fehler. dir_u8 = Ausgabeordner
 * des Clients (UTF-8), game = SBCT_G_*, run_s = Laufzeit in Sekunden (0 = unbekannt). */
int sbct_launcher_note(const char *dir_u8, unsigned long code, int game, unsigned run_s);

#ifdef __cplusplus
}
#endif
