/* sb_tls.h - W334 LOC-SEC-CLIENT: TLS-Client ueber Windows SChannel, nicht blockierend, mit SPKI-Pin
 * (SECURITY.md §3.1/§3.2). Community-tauglich.
 *
 *  - TLS 1.3 bevorzugt, TLS 1.2 nur mit ECDHE + AEAD (Entscheidung F1); alles darunter abgeschaltet.
 *    Nach dem Handshake wird die ausgehandelte Suite geprueft (fail closed).
 *  - Zertifikatskette, Name und Datum werden NICHT ausgewertet; allein der Pin entscheidet:
 *    SHA-256 ueber das DER-kodierte SubjectPublicKeyInfo des Serverzertifikats muss einem der Pins gleichen
 *    (laufzeitkonstanter Vergleich). Ohne Pin keine Verbindung.
 *  - sbtls_handshake meldet SBTLS_OK erst NACH bestandener Pin- und Suitenpruefung; vorher nimmt sbtls_write
 *    keine Daten an. Kein Klartext-Rueckfall.
 *  - Puffer werden beim Freigeben geloescht. Keine Inhalte/Geheimnisse in sbtls_error. */
#pragma once
#include <winsock2.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sbtls sbtls;
enum { SBTLS_OK = 0, SBTLS_WANT_IO = 1, SBTLS_FAIL = -1, SBTLS_CLOSED = -2 };

/* s: verbundener, nicht blockierender TCP-Socket (gehoert weiter dem Aufrufer). sni: Hostname (ASCII).
 * spki: nspki Pins (32 Byte); leere Pins (alles 0) zaehlen nie. 0 = ok. */
int sbtls_client_new(sbtls **out, SOCKET s, const char *sni, const uint8_t (*spki)[32], int nspki);
int sbtls_handshake(sbtls *t);                    /* OK erst NACH bestandener SPKI-Pruefung */
int sbtls_write(sbtls *t, const void *p, int n); /* ganz oder FAIL; puffert verschluesselt */
int sbtls_flush(sbtls *t);                        /* OK = alles gesendet, WANT_IO = Rest offen */
int sbtls_read(sbtls *t, void *p, int cap);       /* >0 Bytes, WANT_IO, CLOSED, FAIL */
const char *sbtls_error(const sbtls *t);          /* ohne Geheimnisse */
void sbtls_free(sbtls *t);                        /* loescht Puffer, Socket bleibt offen */
/* Nach OK: ausgehandelte Version ("TLS 1.3"/"TLS 1.2") fuer das Log. */
const char *sbtls_version(const sbtls *t);
/* 1 = die Pin-Pruefung ist fehlgeschlagen (fuer die Statusanzeige "Server-Pruefung fehlgeschlagen"). */
int sbtls_pin_failed(const sbtls *t);

/* Hilfen: 64 Hex-Zeichen -> 32 Byte (1 = ok; leer/ungueltig = 0). */
int sbtls_parse_pin(const char *hex, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
