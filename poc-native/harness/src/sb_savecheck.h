#pragma once
/* sb_savecheck.h - W187 KIA-LOAD: Spielstaende der virtuellen HDD pruefen,
 * reparieren und zuruecksetzen (Client + Launcher, nur unsere HDD-Schicht,
 * das Spiel selbst bleibt unveraendert).
 *
 * Format der T:-Spielstaende (Disasm FUN_0010A270/0010A360/000C3A70/000C3AE0,
 * W27/W63/W187): Datei = Daten[n] + Signatur[20] (XCalculateSignature, im
 * Client wegen XcSHA-Stub immer 20 Nullbytes). Das Spiel laedt beim Start
 * (FUN_000DF040) user99/mslog00/config/death00/sasen00 und zeigt
 * "Unable to load <...> Record." (KIA = death00.sav), wenn die Datei
 * existiert und nicht geladen werden kann. Genau (FUN_0010A360): ReadFile
 * muss n Bytes liefern (Datei >= n), danach wird die berechnete Signatur
 * mit einem vorher genullten Puffer verglichen, in den FUN_000C3AE0 nur bei
 * Dateigroesse == n+20 die gespeicherte Signatur liest (Rueckgabewert wird
 * ignoriert). Mit der Null-Signatur des Clients scheitern also nur
 *   - Dateien kuerzer als n (auch 0 Byte, abgebrochenes Schreiben) und
 *   - Dateien mit genau n+20 Byte und Signatur != 0 (fremde Signatur,
 *     z. B. Spielstand einer echten Xbox oder eines anderen Emulators).
 *
 *   Datei            n (Daten)   Meldung beim Start
 *   userNN.sav       0xAE8       Player Record (nur user99)
 *   mslogNN.sav      0xB40       Mission Record
 *   deathNN.sav      0x15D00     KIA Record
 *   sasenNN.sav      0x15D00     RELEGATED Record
 *   config.sav       0x2C        SYSTEM DATA
 *
 * Reparatur (vorher Sicherung unter <hdd>\_backup\repair-<zeit>\):
 *   fremde Signatur (n+20 Byte, != 0)    -> Signatur auf 0 (Daten bleiben)
 *   zu kurz (< n Byte)                   -> Datei in die Sicherung
 *                                           verschieben (Spiel startet den
 *                                           Eintrag leer, wie nach der Taste)
 *   alles andere laedt das Spiel         -> unveraendert
 * Hinweis: Implementiert der Client spaeter echte Signaturen (XcSHA), muss
 * sb_sig_expected() dieselbe Rechnung machen. */
#include <stddef.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SB_SAVE_TITLE_W L"43430002"

typedef enum {
    SB_SAVE_OK = 0,
    SB_SAVE_FOREIGN,  /* n+20 Byte, Signatur != erwartet */
    SB_SAVE_SHORT,    /* kuerzer als n Byte (auch 0 Byte) */
    SB_SAVE_UNREAD    /* nicht lesbar (gesperrt?) */
} sb_save_state_t;

typedef struct {
    int checked;      /* bekannte Spielstand-Dateien */
    int ok;
    int repaired;     /* FOREIGN repariert (Signatur 0) */
    int moved;        /* SHORT in die Sicherung verschoben */
    int failed;       /* Befund, aber Aktion fehlgeschlagen (oder nur Pruefung) */
    wchar_t backup[600]; /* Sicherungsordner ("" = keiner noetig) */
} sb_savecheck_t;

/* Zeilenweise Meldungen (UTF-8, ohne Zeilenende); ctx frei. */
typedef void (*sb_savecheck_log_fn)(const char *line, void *ctx);

/* Datengroesse n fuer einen Dateinamen (0 = keine bekannte Spielstand-Datei). */
unsigned sb_save_data_size(const wchar_t *name);

/* Prueft <hdd_root>\TDATA\43430002\*.sav. apply=0: nur pruefen/melden,
 * apply=1: reparieren (mit Sicherung). Rueckgabe = Anzahl Befunde (0 = alles
 * ok), -1 = Ordner fehlt (frische Installation, nichts zu tun). */
int sb_savecheck_run(const wchar_t *hdd_root, int apply, sb_savecheck_t *res,
                     sb_savecheck_log_fn log, void *ctx);

/* Spielstaende zuruecksetzen: TDATA und UDATA nach
 * <hdd_root>\_backup\saves-<JJJJMMTT-hhmmss>\ verschieben und leer neu
 * anlegen (ZCACHE = Cache bleibt). backup_out = Sicherungsordner.
 * 0 = ok, 1 = nichts zu sichern (keine Spielstaende), -1 = Fehler (err). */
int sb_saves_reset(const wchar_t *hdd_root, wchar_t *backup_out, size_t cap,
                   char *err, size_t errcap);

/* Selbsttest in einem Temp-Ordner (alle Faelle + Zuruecksetzen). 0 = ok. */
int sb_savecheck_selftest(char *msg, size_t cap);

/* ---- W190 SAVE-LOCK: "Spielstaende schuetzen" (Testhilfe) ----
 * Wegwerf-Arbeitskopie <hdd>\_savelock\{TDATA,UDATA} der echten Ordner
 * <hdd>\{TDATA,UDATA}. Der Client leitet bei aktivem Schutz alle T:/U:-
 * Zugriffe (auch E:\TDATA, E:\UDATA) in die Arbeitskopie um: Schreiben,
 * Loeschen (Pilot KIA), Anlegen treffen nur die Kopie, das Spiel sieht seine
 * Aenderungen in der Sitzung konsistent. Die echten Ordner werden nie
 * beruehrt (ausser durch "Uebernehmen"). Beim naechsten Start wird die Kopie
 * verworfen bzw. neu angelegt; ZCACHE (Cache) bleibt gemeinsam. */
#define SB_SAVELOCK_DIR_W L"_savelock"
/* Arbeitskopie frisch anlegen (eine alte wird vorher verworfen).
 * files = Anzahl kopierter Dateien. 0 = ok, -1 = Fehler (err, keine Kopie). */
int sb_savelock_prepare(const wchar_t *hdd_root, int *files, char *err, size_t errcap);
/* Arbeitskopie loeschen: 1 = verworfen, 0 = keine da, -1 = Rest blieb. */
int sb_savelock_discard(const wchar_t *hdd_root);
/* 1 = Arbeitskopie vorhanden. */
int sb_savelock_present(const wchar_t *hdd_root);
/* "Geschuetzte Aenderungen uebernehmen": echte TDATA/UDATA nach
 * <hdd>\_backup\savelock-<zeit>\ verschieben, dann die Arbeitskopie hinein-
 * kopieren (die Arbeitskopie bleibt bestehen). 0 = ok, -1 = Fehler (err). */
int sb_savelock_commit(const wchar_t *hdd_root, wchar_t *backup_out, size_t cap, int *files,
                       char *err, size_t errcap);
/* Selbsttest (Temp-Ordner): anlegen, Tod in der Kopie, verwerfen, uebernehmen. */
int sb_savelock_selftest(char *msg, size_t cap);

#ifdef __cplusplus
}
#endif
