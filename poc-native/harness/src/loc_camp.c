/* loc_camp.c - W491 CAMP-CLIENT: Kern der Kampagne im Client (siehe loc_camp.h, docs/loc/LOC_CAMPAIGN.md). */
#include "loc_camp.h"

#include <string.h>

#define LOCCAMP_SVC_ID 0x43430009u
#define LOCCAMP_HR_OK 0x00000000u
#define LOCCAMP_HR_UNAVAILABLE 0x80151102u

int loccamp_preamble(const uint8_t *ticket, int tlen, uint8_t *out, int cap)
{
    if (!ticket || !out || tlen < LOCCAMP_TICKET_MIN || tlen > LOCCAMP_TICKET_MAX || cap < LOCCAMP_PRE_HEAD + tlen)
        return -1;
    memcpy(out, "SBGW", 4);
    out[4] = 1; /* Version */
    out[5] = 0; /* Flags */
    out[6] = (uint8_t)(tlen >> 8);
    out[7] = (uint8_t)tlen;
    memcpy(out + LOCCAMP_PRE_HEAD, ticket, (size_t)tlen);
    return LOCCAMP_PRE_HEAD + tlen;
}

int loccamp_gw_status(const uint8_t *r, int n)
{
    if (!r || n < LOCCAMP_GW_REPLY) return -2;
    if (memcmp(r, "SBGW", 4) != 0 || r[4] != 1 || r[5] > LOCCAMP_GW_UNSUPPORTED) return -1;
    return r[5];
}

int loccamp_frame(const uint8_t *b, int n)
{
    unsigned len;
    if (!b || n < 2) return 0;
    len = (unsigned)b[0] | ((unsigned)b[1] << 8); /* Little-Endian wie im Spiel (x86) */
    if (len > LOCCAMP_FRAME_MAX) return -1;
    if (n < 4 || (unsigned)n < 4u + len) return 0;
    return (int)(4u + len);
}

static int b64v(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

int loccamp_b64url_dec(const char *in, uint8_t *out, int cap)
{
    unsigned acc = 0;
    int bits = 0, n = 0;
    if (!in || !out) return -1;
    for (; *in; in++) {
        int v = b64v(*in);
        if (v < 0) return -1;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) return -1;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    if (bits >= 6) return -1;                         /* ein einzelnes Restzeichen ist kein gueltiges base64 */
    if (bits && (acc & ((1u << bits) - 1u))) return -1; /* nicht-kanonische Restbits */
    return n;
}

static uint64_t be64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

int loccamp_ticket_ok(const uint8_t *t, int n, uint32_t account, long long *exp)
{
    uint32_t acct;
    uint64_t issued, until, xuid;
    if (exp) *exp = 0;
    if (!t || n < LOCCAMP_TICKET_MIN || n > LOCCAMP_TICKET_MAX) return 0;
    if (memcmp(t, "SBCT", 4) != 0 || t[4] != 1) return 0;
    issued = be64(t + 13);
    until = be64(t + 21);
    acct = ((uint32_t)t[29] << 24) | ((uint32_t)t[30] << 16) | ((uint32_t)t[31] << 8) | t[32];
    xuid = be64(t + 33);
    if (!account || acct != account) return 0;
    if (xuid != (((uint64_t)0x53424C01u << 32) | acct)) return 0;
    if (until <= issued || until - issued > 3600u) return 0;
    if (exp) *exp = (long long)until;
    return 1;
}

int loccamp_renew_due(long long now, long long expires, int have)
{
    if (!have || expires <= 0) return 1;
    return expires - now < LOCCAMP_RENEW_S;
}

uint32_t loccamp_service_hr(int state)
{
    return state == LOCCAMP_ST_READY ? LOCCAMP_HR_OK : LOCCAMP_HR_UNAVAILABLE;
}

void loccamp_service_info(uint8_t out[12])
{
    uint32_t id = LOCCAMP_SVC_ID, vip = LOCCAMP_VIP;
    uint8_t ip[4];
    ip[0] = (uint8_t)((vip >> 24) & 0xFFu);
    ip[1] = (uint8_t)((vip >> 16) & 0xFFu);
    ip[2] = (uint8_t)((vip >> 8) & 0xFFu);
    ip[3] = (uint8_t)(vip & 0xFFu);
    uint16_t port = (uint16_t)LOCCAMP_GAME_PORT, zero = 0;
    memcpy(out, &id, 4);
    memcpy(out + 4, ip, 4); /* IN_ADDR: Bytes in Netz-Reihenfolge */
    memcpy(out + 8, &port, 2);
    memcpy(out + 10, &zero, 2);
}

int loccamp_is_vip(uint32_t ip_net, uint16_t port_net)
{
    const uint8_t *b = (const uint8_t *)&ip_net;
    const uint8_t *p = (const uint8_t *)&port_net;
    uint32_t ip = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    uint16_t port = (uint16_t)(((unsigned)p[0] << 8) | p[1]);
    return ip == LOCCAMP_VIP && port == LOCCAMP_GAME_PORT;
}

const char *loccamp_campaign_id(const char *sel, uint16_t *port)
{
    int test = sel && (strcmp(sel, "test") == 0 || strcmp(sel, LOCCAMP_ID_TEST) == 0);
    if (port) *port = (uint16_t)(test ? LOCCAMP_PORT_TEST : LOCCAMP_PORT_SEASON);
    return test ? LOCCAMP_ID_TEST : LOCCAMP_ID_SEASON;
}

int loccamp_logon_ready(int state, uint32_t waited_ms, uint32_t max_ms)
{
    return state != LOCCAMP_ST_PENDING || waited_ms >= max_ms;
}

const char *loccamp_state_name(int state)
{
    switch (state) {
    case LOCCAMP_ST_DOWN: return "nicht verfuegbar";
    case LOCCAMP_ST_OFF: return "aus";
    case LOCCAMP_ST_PENDING: return "wird geprueft";
    case LOCCAMP_ST_READY: return "bereit";
    default: return "?";
    }
}

/* W515 FACTIONSWAP (#110): Enlistment Office in jedem Zug (siehe loc_camp.h, LOC_CAMPAIGN.md "Seitenwechsel"). */
const uint8_t loccamp_swap_orig[12] = { 0x66, 0x3D, 0x03, 0x00, 0x72, 0x06, 0x66, 0x3D, 0x06, 0x00, 0x76, 0x11 };
const uint8_t loccamp_swap_new[12] = { 0x66, 0x3D, 0x03, 0x00, 0xEB, 0x17, 0x66, 0x3D, 0x06, 0x00, 0x76, 0x11 };

const uint8_t loccamp_swap_reload_orig[6] = { 0x84, 0xC9, 0x74, 0x0B, 0x6A, LOCCAMP_SCREEN_MAIN };
const uint8_t loccamp_swap_reload_new[6] = { 0x84, 0xC9, 0x74, 0x0B, 0x6A, LOCCAMP_SCREEN_CAMPAIGN };

int loccamp_enlist_locked(int campaign, int faction, int turn, int swap)
{
    if (!campaign) return 1; /* FUN_000EF4A0: ohne Kampagne nur MAINTENANCE BN */
    if (faction == 4) return 0; /* neutral: nur ENLISTMENT OFFICE */
    if (swap) return 0;
    return turn < 3 || turn > 6;
}

/* W492 CAMP-E2E: Reinforcements-Auswahl (siehe loc_camp.h, LOC_CAMPAIGN.md "Reinforcements"). */
int loccamp_reinf_blocked(uint32_t mask)
{
    return mask == 0;
}

int loccamp_reinf_next(uint16_t cur, uint32_t mask, int step, uint16_t *out)
{
    /* FUN_00118E50: idx = (idx + 0x20 + step) & 0x8000001F (vorzeichenrichtig) bis Bit idx in der Maske gesetzt ist */
    int idx = cur, n;
    for (n = 0; n < 64; n++) {
        idx = (idx + 0x20 + step) % 32;
        if (idx < 0) idx += 32;
        if (mask & (1u << idx)) {
            if (out) *out = (uint16_t)idx;
            return (uint16_t)idx != cur;
        }
    }
    return -1;
}
