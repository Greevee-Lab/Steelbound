/* launcher_camp.c - W491 CAMP-CLIENT (Launcher): Kampagnenstatus und Erreichbarkeit des Steelbound-Zugangs
 * (siehe launcher_camp.h). W537 LAUNCHER-SLIM: hier nur noch der reine Teil (Zustand, Ziel); die Erreichbarkeits-
 * Probe (TCP, Entwickler-CLI zusaetzlich TLS) liegt in launcher_camp_net.c (Hilfsprogramm sb_account.exe). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "launcher_camp.h"
#include "launcher_account.h"     /* lacc_is_loopback */
#include "launcher_account_cfg.h" /* steelnet_pins.h: SB_STEELNET_HOST, SB_CAMPAIGN_SPKI1/2 */
#include "launcher_u8.h"
#include "loc_camp.h"             /* LOCCAMP_PORT_SEASON / LOCCAMP_PORT_TEST (eine Quelle mit dem Client) */

/* ACC_LIVE_* aus launcher_gui_int.h (dort per Pruefung zur Uebersetzungszeit gegen diese Werte abgesichert) */
#define A_OK 0
#define A_OFF 1
#define A_NOACC 2
#define A_UNK 3
#define A_NOGT 4
#define A_GTOFF 5

int lcamp_state(int acc, int outdated, int reach)
{
    switch (acc) {
    case A_OK: break;
    case A_OFF: return LCAMP_OFF;
    case A_NOACC: return LCAMP_NOACC;
    case A_UNK: return LCAMP_UNK;
    case A_GTOFF: return LCAMP_GTOFF;
    case A_NOGT: return LCAMP_NOGT;
    default: return LCAMP_UNK;
    }
    if (outdated) return LCAMP_OLD;
    if (reach == LCAMP_R_OK) return LCAMP_OK;
    return reach == LCAMP_R_FAIL ? LCAMP_UNREACH : LCAMP_CHECK;
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 64 Hex (Doppelpunkte/Leerzeichen erlaubt) -> klein, sonst leer */
static void norm_pin(const char *in, char out[65])
{
    int o = 0;
    out[0] = '\0';
    if (!in) return;
    for (; *in && o < 64; in++) {
        if (*in == ':' || *in == ' ') continue;
        if (hexv(*in) < 0) return;
        out[o++] = (char)(*in >= 'A' && *in <= 'F' ? *in + 32 : *in);
    }
    out[o] = '\0';
    if (*in || o != 64) out[0] = '\0';
}

int lcamp_target(int test_campaign, lcamp_target_t *t)
{
    char v[200];
    memset(t, 0, sizeof *t);
    strcpy_s(t->host, sizeof t->host, SB_STEELNET_HOST);
    t->port = test_campaign ? (int)LOCCAMP_PORT_TEST : (int)LOCCAMP_PORT_SEASON;
    norm_pin(SB_CAMPAIGN_SPKI1, t->pin1);
    norm_pin(SB_CAMPAIGN_SPKI2, t->pin2);
    if (u8_GetEnv("SB_CAMPAIGN_ADDR", v, sizeof v) > 0) { /* Test: nur Loopback, eigener Pin */
        char *c = strrchr(v, ':');
        if (c) {
            int port = atoi(c + 1);
            *c = '\0';
            if (lacc_is_loopback(v) && port > 0 && port < 65536) {
                strcpy_s(t->host, sizeof t->host, v);
                t->port = port;
                t->test = 1;
                t->pin1[0] = t->pin2[0] = '\0';
                if (u8_GetEnv("SB_CAMPAIGN_SPKI", v, sizeof v) > 0) norm_pin(v, t->pin1);
            }
        }
    }
    return t->pin1[0] != '\0' || t->pin2[0] != '\0';
}
