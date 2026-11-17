#pragma once
/* launcher_account_cfg.h - W328 STEELNET-ACCOUNT: Server-Konfiguration des
 * Pilotenkontos (oeffentliche Werte, keine Geheimnisse).
 *
 * Werte setzt der Steuerer nach der Einrichtung des Dienstes
 * (net/steelnet/deploy/README.md, Abschnitt Konto-API):
 *   SB_STEELNET_HOST    Hostname des Treffpunkts / der Konto-API
 *   SB_STEELNET_API_PORT Port der TLS-Konto-API (47001/tcp)
 *   SB_STEELNET_PORT    Port des Treffpunkts (47000 tcp/udp)
 *   SB_STEELNET_PIN1/2  SHA-256-Fingerabdruck des Server-Zertifikats (DER),
 *                       64 Hex-Zeichen; PIN2 = naechstes Zertifikat beim
 *                       Wechsel. Leer = Konto-Funktionen aus ("Server noch
 *                       nicht eingerichtet").
 *   SB_DISCORD_CLIENT_ID oeffentliche Client-ID der Discord-App; leer = aus
 *                       GET /v1/info des Dienstes.
 * Alle Werte sind oeffentlich (Community-Ausgabe ok). Der Fingerabdruck ist
 * der oeffentliche Hash des Zertifikats, nicht der Schluessel.
 * Test-Ueberschreibung (nur Loopback): SB_STEELNET_API=127.0.0.1:<port>,
 * SB_STEELNET_PIN=<hex64>. ASCII-Quelltext. */

#ifndef SB_STEELNET_HOST
#define SB_STEELNET_HOST "relay.steelbound.dev"
#endif
#ifndef SB_STEELNET_API_PORT
#define SB_STEELNET_API_PORT 47001
#endif
#ifndef SB_STEELNET_PORT
#define SB_STEELNET_PORT 47000
#endif
#ifndef SB_STEELNET_PIN1
#define SB_STEELNET_PIN1 "2bd4ab32b602b7be4a7df040cea2226a7c20579c9701fd892a7aedaaab2e2acd" /* EU-Hauptserver, Zertifikat vom 17.11.2026 (10 Jahre) */
#endif
#ifndef SB_STEELNET_PIN2
#define SB_STEELNET_PIN2 ""
#endif
#ifndef SB_DISCORD_CLIENT_ID
#define SB_DISCORD_CLIENT_ID "" /* Platzhalter: leer = aus /v1/info */
#endif
/* Discord-Autorisierung (fest; Test-Ueberschreibung SB_DISCORD_AUTHORIZE nur Loopback) */
#define SB_DISCORD_AUTHORIZE_URL "https://discord.com/oauth2/authorize"
/* Loopback-Rueckleitung (muss in der Discord-App eingetragen sein) */
#define SB_DISCORD_REDIRECT_PORT 47090
