#pragma once
/* launcher_account.h - W328 STEELNET-ACCOUNT: Pilotenkonto im Launcher
 * (reine Logik + Netz, ohne Oberflaeche). Vertrag: net/steelnet/PROTOKOLL.md,
 * Abschnitt "W328: Pilotenkonten".
 *
 * - Geraeteschluessel K (32 Byte) liegt nur DPAPI-geschuetzt im Datenordner
 *   (<Datenordner>\loc\steelnet_account.dat, Test: SB_ACCOUNT_DIR), nie in
 *   user.cfg, nie im Log, nie auf der Leitung (Beweise per HMAC).
 * - Der Spiel-Client bekommt nur den kurzlebigen Spielschluessel G ueber die
 *   Umgebung des Kindprozesses (SB_LOC_AUTH=<account>:<exp>:<G hex>).
 * - Netz: nur HTTPS zur Konto-API mit festem Zertifikats-Fingerabdruck
 *   (Pinning, vor dem Senden des Rumpfs geprueft).
 * Alle Netzfunktionen blockieren (Hintergrund-Thread). ASCII-Quelltext. */
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#define LACC_ENV_AUTH "SB_LOC_AUTH"
#define LACC_FILE "steelnet_account.dat"
#define LACC_GAME_TTL (24 * 3600) /* Spielschluessel: jetzt + 24 h */

typedef struct {
    int have;            /* 1 = Konto gespeichert */
    uint32_t account;
    int gen;
    char name[40];       /* Pilotname (oeffentlich) */
    uint8_t key[32];     /* Geraeteschluessel K - nie ausgeben */
    char host[160];      /* Konto-API "host:port", bei der das Konto liegt */
} lacc_store_t;

typedef struct {
    char host[128];
    int port;            /* Konto-API */
    int lobby_port;      /* Treffpunkt */
    char pin1[65], pin2[65];
    int test;            /* 1 = Test-Ueberschreibung (Loopback) aktiv */
} lacc_srv_t;

typedef struct {
    int discord;
    int login;
    char login_name[40];
    long long created;
    int gen;
} lacc_status_t;

/* ---------------- Krypto (rein) ---------------- */
void lacc_hmac(const uint8_t *key, size_t klen, const void *msg, size_t n, uint8_t out[32]);
void lacc_hex(const uint8_t *b, size_t n, char *out); /* out: 2n+1 */
int lacc_unhex(const char *s, uint8_t *out, size_t n); /* 1 = genau 2n Hex-Zeichen */
/* G = HMAC(K, "steelnet-game-v1|<account>|<exp>") */
void lacc_game_key(const uint8_t K[32], uint32_t account, long long exp, uint8_t G[32]);
/* proof = hex(HMAC(K, "steelnet-api-v1|<action>|<nonce>|<account>")) */
void lacc_api_proof(const uint8_t K[32], const char *action, const char *nonce, uint32_t account,
                    char hex[65]);
/* proof = hex(HMAC(G, "steelnet-auth-v1|<nonce>|<client_id>")) (Spiel-Client; Selbsttest) */
void lacc_auth_proof(const uint8_t G[32], const char *nonce, uint32_t client_id, char hex[65]);
/* Base32 RFC 4648 (A-Z2-7, ohne '='). enc: out mind. ceil(8n/5)+1. dec: tolerant
 * (Kleinbuchstaben, '-', Leerzeichen), Rueckgabe Bytes oder -1. */
void lacc_b32_enc(const uint8_t *in, size_t n, char *out);
int lacc_b32_dec(const char *in, uint8_t *out, size_t cap);
/* Wiederherstellungscode: account(u32 BE) || K[0:16] -> "ABCD-EFGH-..." (39 Zeichen). */
void lacc_recovery_code(uint32_t account, const uint8_t K[32], char *out, size_t cap);
int lacc_recovery_parse(const char *code, uint32_t *account, uint8_t k16[16]); /* 1 = ok */
/* "<account>:<exp>:<G hex>" fuer den Spiel-Client. */
void lacc_game_auth(const lacc_store_t *st, long long now, char *out, size_t cap);
int lacc_random(void *buf, size_t n); /* 1 = ok (BCryptGenRandom) */

/* ---------------- Namen/Eingaben (rein) ---------------- */
int lacc_valid_name(const char *s);   /* Pilotname: 1-16 [A-Za-z0-9 _-], kein Guest-/Gast-Praefix */
int lacc_valid_login(const char *s);  /* Login: 3-32 [A-Za-z0-9 _-] */
int lacc_valid_password(const char *s); /* 10-256 Bytes */

/* ---------------- Konfiguration ---------------- */
/* 1 = eingerichtet (Pin gesetzt), 0 = aus. Test-Ueberschreibung nur fuer Loopback. */
int lacc_server(lacc_srv_t *s);
int lacc_is_loopback(const char *host);
/* sha = SHA-256 des DER-Zertifikats; 1 = passt zu pin1/pin2 (leere Pins passen nie). */
int lacc_pin_match(const lacc_srv_t *s, const uint8_t sha[32]);

/* ---------------- Speicher (DPAPI) ---------------- */
/* dir = Ordner der Schluesseldatei (UTF-8). Rueckgabe load: 1 = Konto, 0 = keins, -1 = unlesbar. */
int lacc_store_load(const char *dir, lacc_store_t *st);
int lacc_store_save(const char *dir, const lacc_store_t *st); /* 0 = ok */
int lacc_store_forget(const char *dir);                      /* 0 = weg/nie da */
void lacc_store_path(const char *dir, char *out, size_t cap);
/* Standardordner: SB_ACCOUNT_DIR oder <Datenordner>\loc (sb_userdata), sonst base\loc. */
void lacc_default_dir(const char *client_dir, char *out, size_t cap);

/* ---------------- JSON (flache Antworten) ---------------- */
int lacc_js_str(const char *json, const char *key, char *out, size_t cap); /* 1 = gefunden */
int lacc_js_int(const char *json, const char *key, long long *v);
int lacc_js_bool(const char *json, const char *key, int *v);
void lacc_js_esc(const char *in, char *out, size_t cap); /* JSON-String-Inhalt ohne Anfuehrungszeichen */

/* ---------------- HTTPS ---------------- */
typedef struct {
    int http;            /* HTTP-Status (0 = keine Antwort) */
    int pin_fail;        /* 1 = Zertifikat passt nicht (abgebrochen vor dem Rumpf) */
    int ok;              /* "ok":true */
    char error[48];      /* "error" der Antwort bzw. net/pin/off/bad */
    long long retry_after;
    char body[4096];
} lacc_resp_t;
/* method "GET"/"POST", path z. B. "/v1/check". 0 = Antwort gelesen (r->ok sagt, ob ok). */
int lacc_http(const lacc_srv_t *s, const char *method, const char *path, const char *body,
              lacc_resp_t *r);

/* ---------------- Aktionen (blockierend) ----------------
 * Rueckgabe 0 = ok, sonst Fehler mit Code in err (API-Code oder net/pin/off/
 * bad/timeout/state/denied/no_account). Gespeichertes Konto wird bei Erfolg
 * im Ordner dir aktualisiert (create/login/discord_login/transfer/recovery),
 * bei delete/forget entfernt. */
int lacc_op_check(const lacc_srv_t *s, const char *name, int *available, char *err, size_t ecap);
int lacc_op_create(const lacc_srv_t *s, const char *dir, const char *name, lacc_store_t *st,
                   char *err, size_t ecap);
int lacc_op_status(const lacc_srv_t *s, const lacc_store_t *st, lacc_status_t *o, char *err,
                   size_t ecap);
int lacc_op_login_set(const lacc_srv_t *s, const lacc_store_t *st, const char *login,
                      const char *password, char *err, size_t ecap);
int lacc_op_login(const lacc_srv_t *s, const char *dir, const char *login, const char *password,
                  lacc_store_t *st, char *err, size_t ecap);
int lacc_op_transfer(const lacc_srv_t *s, const char *dir, const char *code, lacc_store_t *st,
                     char *err, size_t ecap);
int lacc_op_recovery(const lacc_srv_t *s, const char *dir, const char *code, lacc_store_t *st,
                     char *err, size_t ecap);
int lacc_op_logout_all(const lacc_srv_t *s, const char *dir, lacc_store_t *st, char *err, size_t ecap);
int lacc_op_delete(const lacc_srv_t *s, const char *dir, lacc_store_t *st, char *err, size_t ecap);
/* Discord: login_mode 0 = verknuepfen (Konto st noetig), 1 = auf diesem PC anmelden.
 * Oeffnet den Standardbrowser (Testmodus SB_TEST_NO_BROWSER=1: URL auf stdout als
 * "[discord-url] ..."), wartet auf die Loopback-Rueckleitung (timeout_ms), schickt
 * Code + verifier an die Konto-API. cancel (darf NULL sein): != 0 bricht ab. */
int lacc_op_discord(const lacc_srv_t *s, const char *dir, int login_mode, lacc_store_t *st,
                    int timeout_ms, volatile long *cancel, char *err, size_t ecap);

/* ---------------- Loopback-Empfaenger (Discord-Rueckleitung) ---------------- */
/* Oeffnet 127.0.0.1:port (0 = frei; *actual = Port). Rueckgabe Socket oder -1. */
long long lacc_loop_open(int port, int *actual);
/* Wartet auf GET <path>?code=..&state=..; falscher state -> err "state".
 * 0 = Code erhalten. Antwortet mit kurzer Bestaetigungsseite (DE/EN). */
int lacc_loop_wait(long long sock, const char *state, int timeout_ms, volatile long *cancel,
                   char *code, size_t ccap, char *err, size_t ecap);
void lacc_loop_close(long long sock);

/* ---------------- Kindprozess-Umgebung ---------------- */
/* Neuer Unicode-Umgebungsblock = eigene Umgebung (ohne alte name=) + name=value,
 * sortiert. free() durch den Aufrufer. Die eigene Prozessumgebung bleibt unveraendert. */
wchar_t *lacc_env_block(const char *name, const char *value);

/* ---------------- CLI + Selbsttest ---------------- */
/* sb_launcher --steelnet <aktion> [args]; argv ab der Aktion. Rueckgabe Exitcode. */
int lacc_cli(const char *client_dir, int argc, char **argv);
void lacc_selftest(int *pass, int *fail);

#ifdef __cplusplus
}
#endif
