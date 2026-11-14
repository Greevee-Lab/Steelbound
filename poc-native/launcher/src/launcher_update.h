#pragma once
/* launcher_update.h - W250 UPDATE-CHECK: Hinweis auf eine neuere Version.
 *
 * Nutzerentscheid 14.11.2026: NUR pruefen - kein Download, kein
 * Selbst-Update, keine Dateien ausser der Einstellung in user.cfg.
 * Opt-in: erst nach ausdruecklicher Frage (Standard Nein).
 *
 * Abruf: WinHTTP (Windows-Bordmittel), HTTPS GET
 *   https://api.github.com/repos/Greevee-Lab/Steelbound/releases?per_page=5
 * User-Agent "Steelbound-Launcher/<version>", sonst keine Kennungen
 * (keine Cookies, keine Anmeldung, keine Standard-Anmeldedaten).
 * Auswahl: hoechste Version unter den Releases, Entwuerfe zaehlen nicht,
 * Prereleases zaehlen. Oeffnen nur fuer html_url mit dem festen Praefix
 * LUPD_PAGE_PREFIX.
 *
 * Reine Logik (Vergleich, JSON-Auswahl, URL-Pruefung, cfg-Zustand) ist
 * ohne Netz testbar (lupd_selftest). Quelltext ASCII. */
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#define LUPD_API_HOST "api.github.com"
#define LUPD_API_PATH "/repos/Greevee-Lab/Steelbound/releases?per_page=5"
#define LUPD_PAGE_PREFIX "https://github.com/Greevee-Lab/Steelbound/releases/"
#define LUPD_TIMEOUT_MS 5000          /* Gesamtfrist je Abruf (Oberflaeche) */
#define LUPD_MAX_BODY (512 * 1024)    /* groessere Antworten = Fehler */
#define LUPD_CFG_KEY "update_check"   /* user.cfg: -1 (fehlt) = nie gefragt */

/* Eingebaute Release-Version (CMake -DSB_VERSION, gesetzt von make_alpha.py);
 * "" = Entwickler-Build ohne Release-Version -> Funktion ausgeblendet. */
const char *lupd_version(void);

/* Versionen vergleichen (SemVer-Vorrang: x.y.z, Prerelease -alpha.N/-beta.N
 * kleiner als die Release-Fassung, Zahlenteile numerisch, alpha.10 >
 * alpha.9; fuehrendes v/V und +Build ignoriert). Rueckgabe 1 = beide
 * lesbar (*cmp < 0, 0, > 0), 0 = mindestens eine unlesbar. */
int lupd_ver_cmp(const char *a, const char *b, int *cmp);

typedef struct {
    char tag[64];     /* tag_name des Releases */
    char url[512];    /* html_url (ungeprueft; vor dem Oeffnen lupd_url_ok) */
    int prerelease;
} lupd_rel_t;

/* Antwort der Releases-Liste auswerten: hoechste lesbare Version unter
 * allen Eintraegen mit draft != true. Rueckgabe = Anzahl beruecksichtigter
 * Releases (0 = keines), -1 = kein gueltiges JSON-Array. */
int lupd_pick(const char *json, size_t len, lupd_rel_t *best);

/* Darf diese Seite geoeffnet werden? 1 = beginnt mit LUPD_PAGE_PREFIX,
 * danach nur [A-Za-z0-9._~/%+-], kein "..", Laenge begrenzt. */
int lupd_url_ok(const char *url);

/* Ergebnis eines Abrufs. */
enum {
    LUPD_R_NONE = 0,     /* (noch) kein Ergebnis */
    LUPD_R_CURRENT = 1,  /* keine neuere Version */
    LUPD_R_NEWER = 2,    /* neuere Version: rel gefuellt */
    LUPD_R_NET = 3,      /* kein Netz / Zeitueberschreitung / TLS */
    LUPD_R_HTTP = 4,     /* HTTP-Status != 200 (z. B. 403/429 Rate-Limit) */
    LUPD_R_BAD = 5,      /* Antwort unlesbar / zu gross / keine Version */
    LUPD_R_NOVER = 6     /* Build ohne Release-Version */
};
typedef struct {
    int result;          /* LUPD_R_* */
    int http_status;     /* 0 = keine Antwort */
    size_t bytes;        /* Groesse der Antwort */
    lupd_rel_t rel;      /* hoechstes Release (bei NEWER/CURRENT) */
    char req_headers[1024]; /* gesendete Kopfzeilen (Beleg/Diagnose) */
} lupd_res_t;

/* Antwort gegen die eigene Version auswerten (rein, testbar). */
void lupd_eval(const char *cur, const char *json, size_t len, lupd_res_t *r);

/* Abruf (blockierend, im Hintergrund-Thread aufrufen). cur = eigene
 * Version (leer -> LUPD_R_NOVER, kein Netzzugriff). */
void lupd_fetch(const char *cur, lupd_res_t *r);

/* --- Einstellung in user.cfg ([launcher] update_check=0/1) --------- */
/* -1 = nie gefragt (Datei/Schluessel fehlt), 0 = nein, 1 = ja. */
int lupd_cfg_get(const char *usercfg_path);
/* Schluessel setzen (ersetzt vorhandene Zeile, sonst anhaengen; fremde
 * Zeilen bleiben). 0 = ok, -1 = Schreibfehler. */
int lupd_cfg_set(const char *usercfg_path, int on);
/* Erstabfrage noetig? (Release-Version vorhanden und nie gefragt) */
int lupd_should_ask(const char *version, int cfg_state);
/* Beim Start automatisch pruefen? (Release-Version und Zustimmung) */
int lupd_auto_check(const char *version, int cfg_state);

/* Selbsttest (ohne Netz). Meldet PASS/FAIL per cli_printf. */
void lupd_selftest(int *pass, int *fail);

#ifdef __cplusplus
}
#endif
