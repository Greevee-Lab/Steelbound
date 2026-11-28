#pragma once
/* launcher_acchelp.h - W537 LAUNCHER-SLIM: Konto-Hilfsprogramm sb_account.exe und sein Aufruf aus dem Launcher.
 *
 * Warum: Der ausgelieferte Launcher soll "langweilig" sein (Einstellungen, Spiel starten, Update-Hinweis). Alles,
 * was mit dem steelnet-Konto-Dienst spricht - HTTPS mit SPKI-Pin, Challenge-Response (Nonce + HMAC), Discord-OAuth
 * mit lokalem Loopback-Empfaenger, Lobbyliste, Erreichbarkeit (Kopf-Chip, Kampagne), Schreiben der DPAPI-
 * Schluesseldatei - liegt im kleinen Hilfsprogramm sb_account.exe (launcher_account_net.c, launcher_camp_net.c,
 * sb_account_main.c). Der Launcher startet es je Auftrag (nur auf Bedarf: Klick bzw. sichtbare Online-Anzeige)
 * und wartet auf sein Ende. Kein Netz im Launcher.
 *
 * Uebergabe: ein unbenannter, nur vererbter Speicherblock (CreateFileMapping ohne Namen, lach_shm_t) mit Auftrag und
 * Ergebnis, dazu zwei unbenannte Ereignisse (Abbruch: Launcher -> Hilfsprogramm; URL: Hilfsprogramm -> Launcher) und
 * ein Handle auf den Launcher-Prozess (endet der Launcher, bricht das Hilfsprogramm ab). Nur diese vier Handles werden
 * vererbt (PROC_THREAD_ATTRIBUTE_HANDLE_LIST); Kommandozeile "sb_account.exe --job <Handle des Blocks>". Bewusst keine
 * Pipes und keine umgeleitete Standard-Ein-/Ausgabe. Das Passwort steht nur im Block (nie Kommandozeile/Umgebung/
 * Datei) und wird nach dem Auftrag auf beiden Seiten geloescht. Der Geraeteschluessel K bleibt in der DPAPI-Datei und im
 * Hilfsprogramm; der Launcher bekommt nur oeffentliche Felder, den Wiederherstellungscode (Anzeige auf Klick) und den
 * kurzlebigen Spielschluessel G (Spielstart).
 * Hilfsprogramm und Launcher kommen aus demselben Build; abweichende Groesse/Kennung -> Fehler "helper". */
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include "launcher_account.h"

#define LACH_EXE "sb_account.exe"
#define LACH_MAGIC "SBACC2"

/* Ergebnis eines Auftrags (nur die zum Auftrag gehoerenden Felder sind gesetzt, Rest 0). */
typedef struct {
    lacc_status_t status; /* status */
    int avail;            /* check */
    lacc_gtset_t gts;     /* create (Gamertag-Teil), gtset */
    lacc_gtcheck_t gtc;   /* gtcheck */
    lacc_lobbies_t lob;   /* lobbies */
    lacc_probe_t probe;   /* info */
    /* Schluesseldatei nach dem Auftrag (alle Auftraege mit dir): nur oeffentliche Felder, key immer 0 */
    int filled;           /* 1 = vom Hilfsprogramm geschrieben (0 = kein Ergebnis, z. B. Hilfsprogramm fehlt) */
    int st_load;          /* wie lacc_store_load: 1 Konto, 0 keins, -1 unlesbar (Auftraege ohne dir: 0) */
    lacc_store_t st;
    char code[48];        /* recovery-show: Wiederherstellungscode */
    char auth[200];       /* game-auth: "<account>:<exp>:<G hex>" fuer SB_LOC_AUTH (Kindprozess) */
} lach_res_t;

/* Gemeinsamer Block (Launcher schreibt Kopf + Auftrag, Hilfsprogramm schreibt url/Ergebnis). */
typedef struct {
    char magic[8];        /* LACH_MAGIC */
    uint32_t size;        /* sizeof(lach_shm_t) */
    uint32_t res_size;    /* sizeof(lach_res_t) */
    uint32_t ev_cancel;   /* Handle-Werte (im Kind gleich): Ereignis Abbruch */
    uint32_t ev_url;      /* Ereignis "url steht im Block" */
    uint32_t h_parent;    /* Launcher-Prozess (SYNCHRONIZE) */
    /* Auftrag */
    char op[24];
    char dir[1200];
    char a[400], b[600], c[400];
    int32_t k, timeout_ms;
    /* Ergebnis */
    volatile int32_t done; /* 1 = rc/err/res gueltig */
    int32_t rc;
    char err[48];
    lach_res_t res;
    char url[1600];       /* Discord-Autorisierung (der Launcher oeffnet den Browser selbst) */
} lach_shm_t;

/* Auftraege (op): status check create gtcheck gtset discord-link discord-login login-set login transfer recovery
 * delete lobbies info camp ping load recovery-show game-auth (die letzten drei: lokal, ohne Netz - der Launcher selbst
 * hat seit W537 kein DPAPI mehr). a/b/c je nach Auftrag (z. B. login: a = Login, b = Passwort; create: a = Name,
 * c = Gamertag; gtcheck: a = Name, c = Vorschlagsbasis; camp: k = Test-Kampagne). dir = Ordner der Schluesseldatei.
 *
 * lach_run: startet sb_account.exe (Ordner des Launchers) unsichtbar (CREATE_NO_WINDOW). Blockiert (Hintergrund-
 * Thread). cancel (darf NULL sein): != 0 -> Abbruch-Ereignis, nach 5 s Nachfrist nur dieses Kind (eigener Handle)
 * beenden. timeout_ms = Gesamtfrist (danach wie Abbruch, err "timeout"). Rueckgabe = rc des Auftrags (0 ok);
 * Fehler des Aufrufs selbst: 1 mit err "helper" (fehlt, startet nicht, falsche Fassung, abgestuerzt). res darf NULL
 * sein. */
int lach_run(const char *op, const char *dir, const char *a, const char *b, const char *c, int k, int timeout_ms,
             volatile long *cancel, lach_res_t *res, char *err, size_t ecap);

/* Pfad des Hilfsprogramms (UTF-8, neben dem Launcher). */
void lach_exe_path(char *out, size_t cap);

#if SB_LAUNCHER_SELFTEST
/* Selbsttest: URL-Pruefung (rein) + echte Aufrufe (ping, unbekannter Auftrag, Abbruch, Frist). */
void lach_selftest(int *pass, int *fail_n);
#endif

#ifdef __cplusplus
}
#endif
