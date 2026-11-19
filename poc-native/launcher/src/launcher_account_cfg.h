#pragma once
/* launcher_account_cfg.h - W328 STEELNET-ACCOUNT: Server-Konfiguration des
 * Pilotenkontos (oeffentliche Werte, keine Geheimnisse).
 *
 * Werte setzt der Steuerer nach der Einrichtung des Dienstes
 * (net/steelnet/deploy/README.md, Abschnitt Konto-API):
 *   SB_STEELNET_HOST    Hostname des Treffpunkts / der Konto-API
 *   SB_STEELNET_API_PORT Port der TLS-Konto-API (47001/tcp)
 *   SB_STEELNET_PORT    Port des Treffpunkts (47000 tcp/udp)
 *   SB_STEELNET_SPKI1/2 W334: Pin = SHA-256 ueber das SubjectPublicKeyInfo des
 *                       Serverschluessels (64 Hex-Zeichen), gemeinsam mit dem
 *                       Spiel-Client in harness/src/steelnet_pins.h (SECURITY.md
 *                       §3.2/§9). SPKI2 = naechster Schluessel. Leer = Konto-
 *                       Funktionen aus ("Server noch nicht eingerichtet").
 *                       (Der W328-Pin auf das ganze Zertifikat entfaellt mit v2.)
 *   SB_DISCORD_CLIENT_ID oeffentliche Client-ID der Discord-App; leer = aus
 *                       GET /v1/info des Dienstes.
 * Alle Werte sind oeffentlich (Community-Ausgabe ok). Der Fingerabdruck ist
 * der oeffentliche Hash des Zertifikats, nicht der Schluessel.
 * Test-Ueberschreibung (nur Loopback): SB_STEELNET_API=127.0.0.1:<port>,
 * SB_STEELNET_SPKI=<hex64> (alter Name SB_STEELNET_PIN geht weiter). ASCII. */
#include "steelnet_pins.h"

#ifndef SB_DISCORD_CLIENT_ID
#define SB_DISCORD_CLIENT_ID "" /* Platzhalter: leer = aus /v1/info */
#endif
/* Discord-Autorisierung (fest; Test-Ueberschreibung SB_DISCORD_AUTHORIZE nur Loopback) */
#define SB_DISCORD_AUTHORIZE_URL "https://discord.com/oauth2/authorize"
/* Loopback-Rueckleitung (muss in der Discord-App eingetragen sein) */
#define SB_DISCORD_REDIRECT_PORT 47090
