#pragma once
/* steelnet_pins.h - W334 LOC-SEC-CLIENT: oeffentliche Server-Werte fuer Launcher UND Spiel-Client
 * (SECURITY.md §3.2, §9). Nur oeffentliche Werte, Community-tauglich, ASCII.
 *
 *   SB_STEELNET_HOST      Hostname von Treffpunkt und Konto-API
 *   SB_STEELNET_PORT      Treffpunkt (TLS 47000/tcp, Datenkanal 47000/udp)
 *   SB_STEELNET_API_PORT  Konto-API (TLS 47001/tcp)
 *   SB_STEELNET_SPKI1     SHA-256 ueber das DER-kodierte SubjectPublicKeyInfo des aktuellen Serverschluessels
 *                         (64 Hex-Zeichen). Ablesen auf dem Server:
 *                           openssl x509 -in api-tls.crt -pubkey -noout | openssl pkey -pubin -outform DER | sha256sum
 *   SB_STEELNET_SPKI2     dasselbe fuer den NAECHSTEN Schluessel (vorab erzeugt, privat offline verwahrt, §8.4)
 * Treffer bei einem der beiden = gueltig, leerer Wert zaehlt nie; beide leer = Online-Funktion aus
 * ("Dienst nicht eingerichtet"). Ein Schluessel fuer 47000 und 47001 (ein Pin fuer beide Ports).
 * Zertifikatskette, Name und Datum werden NICHT ausgewertet - allein der Pin entscheidet.
 * Test-Ueberschreibung nur mit Loopback-Ziel: SB_STEELNET_SPKI=<hex64> (Launcher und Spiel-Client),
 * im Spiel zusaetzlich loc_lobby_spki fuer eigene Treffpunkte (§3.6). */

#ifndef SB_STEELNET_HOST
#define SB_STEELNET_HOST "relay.steelbound.dev"
#endif
#ifndef SB_STEELNET_PORT
#define SB_STEELNET_PORT 47000
#endif
#ifndef SB_STEELNET_API_PORT
#define SB_STEELNET_API_PORT 47001
#endif
#ifndef SB_STEELNET_SPKI1
#define SB_STEELNET_SPKI1 "61c7642a029d0119f9e5d1c7982ef61eb93dfa0a0d7942eaa7bca726960b03f1" /* EU-Dienst seit 18.11. (api-tls.key) */
#endif
#ifndef SB_STEELNET_SPKI2
#define SB_STEELNET_SPKI2 "a8327e9afec35d4a4a33610fd036c1d6f7f40af93025f8ffb3e0e45ac47dbcb1" /* Folgeschluessel, offline verwahrt */
#endif
