/* loc_lan.h - W454 LOC-LAN-MODE: LAN-Modus ohne steelnet - reiner Kern (Entdeckungspaket, Raumschluessel).
 *
 * Reines Modul ohne Sockets, Log oder Konfiguration (Selbsttest tests/loc_net_selftest.c, W454-Faelle). Die Anbindung
 * (Entdeckungs-Socket, Broadcast, Paarbildung ueber tl_link_pair) macht loc_tun_lan.c.
 *
 * Ablauf (net/SECURITY.md §36):
 *  - Jeder Client im LAN-Modus sendet etwa jede Sekunde ein Entdeckungspaket "SBL1" (feste Laenge, Klartext) per
 *    gerichtetem Broadcast in jedes eigene private IPv4-Subnetz (TTL 1) an UDP LLAN_PORT: Raumname, eigene Kennung
 *    (cid, zufaellig je Prozess), Zufallswert (nonce), fluechtiger X25519-Schluessel, Pilotname, Client-Version,
 *    Transport-Port.
 *  - Wer ein gueltiges SBL1 desselben Raums mit gleicher Version von einer privaten Adresse empfaengt, bildet mit dem
 *    Absender ein Paar wie online (SBT2, SECURITY.md §4.3) mit lobby = llan_room_id(Raum), pair_nonce aus beiden
 *    nonce-Werten und einem zusaetzlich mit dem Raumschluessel gebundenen Transkript (llan_th_mix). Ohne Passwort ist der
 *    Raumschluessel oeffentlich bekannt (Schutz nur gegen Mithoeren ausserhalb des Schluesseltauschs, nicht gegen einen
 *    aktiven Angreifer im selben LAN); mit Passwort authentisiert er beide Seiten.
 *  - Danach laeuft alles wie online (Durchstich als SBT2-PROBE an die Absenderadresse, HELLO mit Version, Gastfilter).
 * Sicherheit: feste Laenge, jedes Feld geprueft (fail closed), keine Inhalte im Log. */
#pragma once
#include <stdint.h>

#define LLAN_PORT 47210u        /* Entdeckung (UDP), eigener Port neben steelnet 47000 und den Relays */
#define LLAN_BEACON_LEN 116     /* feste Laenge des SBL1 */
#define LLAN_PROTO 1
#define LLAN_NAME_MAX 16        /* Raum- und Pilotname */
#define LLAN_VER_MAX 15
#define LLAN_PASS_MAX 63
#define LLAN_KDF_ITER 20000     /* PBKDF2-HMAC-SHA256 fuer das Raumpasswort (einmal je Start) */

typedef struct llan_beacon {
    uint16_t tport;               /* UDP-Port des Transports beim Absender */
    uint32_t cid;                 /* Kennung des Absenders (zufaellig je Prozess, != 0) */
    uint32_t room_id;             /* llan_room_id(room) - muss zum Namen passen */
    uint8_t nonce[16];            /* Zufall je Prozess (pair_nonce) */
    uint8_t pub[32];              /* fluechtiger X25519-Schluessel */
    char room[LLAN_NAME_MAX + 1];
    char pilot[LLAN_NAME_MAX + 1];
    char ver[LLAN_VER_MAX + 1];   /* Client-Version (nur [0-9A-Za-z.+-]) */
    int mode;                     /* eigene Lobby-Einstellung 1 = Sim, 2 = Arcade (nur Anzeige) */
} llan_beacon;

/* Raumname normalisieren: Leerzeichen am Rand weg, leer = "LAN"; nur druckbares ASCII 0x20..0x7E, 1..16 Zeichen.
 * 1 = ok (out), 0 = ungueltig. */
int llan_room_norm(const char *in, char *out, int cap);
/* Pilotname fuer das Paket: druckbares ASCII 0x20..0x7E, 1..16 (andere Zeichen -> '?', leer = "Pilot"). */
void llan_pilot_norm(const char *in, char *out, int cap);
/* Lobby-Kennung des Raums im Transkript: u32_BE(SHA-256("steelbound-lan-room" || room)[0..3]). */
uint32_t llan_room_id(const char *room);
/* Raumschluessel: PBKDF2-HMAC-SHA256(pass, salt = "steelbound-lan-v1|" || room, LLAN_KDF_ITER, 32). pass leer = ohne
 * Passwort (oeffentlich bekannter Schluessel). iter = 0 -> LLAN_KDF_ITER (Selbsttest darf weniger nehmen). */
void llan_psk(const char *room, const char *pass, int iter, uint8_t psk[32]);
/* pair_nonce = SHA-256("steelbound-lan-pn" || nonce_lo || nonce_hi)[0..15] (lo = kleinere cid). */
void llan_pair_nonce(const uint8_t nonce_lo[16], const uint8_t nonce_hi[16], uint8_t pn[16]);
/* Transkript an den Raumschluessel binden: th = HMAC-SHA256(psk, "sbl1 th" || th). */
void llan_th_mix(uint8_t th[32], const uint8_t psk[32]);
/* Paket bauen: Rueckgabe LLAN_BEACON_LEN oder 0 (Felder ungueltig). */
int llan_beacon_build(uint8_t *b, int cap, const llan_beacon *in);
/* Strenge Pruefung: 1 = gueltig (out gefuellt), 0 = ungueltig (out unveraendert bzw. teilweise - nicht verwenden). */
int llan_beacon_parse(const uint8_t *b, int len, llan_beacon *out);
