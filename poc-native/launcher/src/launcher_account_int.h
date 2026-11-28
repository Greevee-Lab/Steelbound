#pragma once
/* launcher_account_int.h - W537 LAUNCHER-SLIM: interne Hilfen, die launcher_account.c (reiner Teil) und
 * launcher_account_net.c (Netzteil, Hilfsprogramm sb_account.exe) gemeinsam nutzen. Nicht ausserhalb der beiden
 * Dateien verwenden. ASCII. */
#include <stddef.h>

int lacc_i_hexval(char c);                                     /* Hex-Ziffer -> 0..15, sonst -1 */
const char *lacc_i_js_find(const char *json, const char *key); /* Wert hinter "key": (flaches JSON), sonst NULL */
int lacc_i_name_chars(const char *s, size_t lo, size_t hi);    /* lo..hi Zeichen [A-Za-z0-9 _-] */
int lacc_i_norm_pin(const char *in, char out[65]);           /* Pin normalisieren (64 Hex klein), 1 = gueltig */
#define norm_pin lacc_i_norm_pin
#define hexval lacc_i_hexval
#define js_find lacc_i_js_find
#define name_chars lacc_i_name_chars
