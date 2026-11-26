/* loc_hostmig_prog.h - W520 HOSTMIG: eingebaute Neuaufbau-Ablaeufe und ihr Zerleger (rein, ohne Spielzugriff; auch im
 * loc_net_selftest geprueft: Schrittzahlen, Sprungziele, Abschluss). Nur von loc_hostmig.c und dem Selbsttest einbinden. */
#pragma once
#include <stdlib.h>
#include <string.h>

typedef struct {
    int kind;           /* HM_K_* */
    int arg, delay, btn, len, to, to_j;
} hm_step;
enum { HM_K_F, HM_K_LS, HM_K_LSN, HM_K_RX, HM_K_TX, HM_K_J, HM_K_MN0 };
enum { HM_B_NONE = 0, HM_B_SHOT = -1 };

/* Abläufe (Schritt;Schritt…): AUSLOESER[+N][/T=J]>TASTE[,Bilder]. Ausloeser: f (sofort), ls=0xNN (Lobby-Zustand),
 * ls!=0xNN (Zustand verlassen), mn0=V (Menuewort 0), rx:0xTT / tx:0xTT (Rahmentyp seit Ablaufbeginn gesehen),
 * j=J (Sprung). Taste: U D L R A B S, 0 = keine, * = Bild (nur TEST). /T=J: kommt der Ausloeser nicht binnen T
 * Bildern, weiter bei Schritt J (1-basiert); J = 99 = Ablauf gescheitert. Werte aus den Testfolgen (LOC_BOOT §30.2). */
static const char HM_PROG_HOST[] =
    "ls=0x31+60>A,8;ls!=0x31/900=99+90>0;f+30>D,8;f+30>A,8;mn0=0/600=99+60>A,8;f+60>U,8;f+40>A,8;"
    "ls=0x22/900=99+10>0";
static const char HM_PROG_JOIN[] =
    "ls=0x31+60>A,8;ls!=0x31/900=99+90>0;f+480>A,8;"                   /* 1-3: Meldung weg, Search Match (8 s spaeter:
                                                                          der neue Host bietet bis dahin an) */
    "rx:0x2/600=6+60>A,8;j=9;"                                         /* 4-5: Host-Info -> Eintrag */
    "f+5>A,8;f+60>A,8;j=4;"                                            /* 6-8: „No results“ OK, neu suchen */
    "f+300>A,8;f+200>R,8;f+90>A,8;tx:0x3/150=10+10>0;"                 /* 9-12: Details, Seite, beitreten (wie
                                                                          die Testfolge: ohne Anfrage Seite + A neu) */
    "ls=0x2E/900=99+10>0";                                             /* 13: verbunden (Lobby des neuen Hosts) */

/* Auto-Host beim abgebenden Host: SIGN OUT aus der eigenen Lobby (B, hoch, A, „Quitting session. Proceed?“ links, A;
 * LOC_BOOT W366 X3/A1) - die Mitspieler fallen dadurch in 0x31 und bauen beim neuen Host neu auf -, danach wie ein
 * Mitspieler suchen und beitreten (MatchMaking-Menue = Zustand 0x02). */
static const char HM_PROG_LEAVE[] =
    "f+90>B,8;f+40>U,8;f+40>A,8;f+60>L,8;f+40>A,8;ls=0x02/900=99+60>*;f+60>A,8;"      /* 1-7 */
    "rx:0x2/600=10+60>A,8;j=13;f+5>A,8;f+60>A,8;j=8;"                                 /* 8-12 */
    "f+300>A,8;f+200>R,8;f+90>A,8;tx:0x3/150=14+10>0;ls=0x2E/900=99+10>0";            /* 13-17 */

static int hm_btn(char c)
{
    switch (c) {
    case 'U': return 0x001;
    case 'D': return 0x002;
    case 'L': return 0x004;
    case 'R': return 0x008;
    case 'S': return 0x010;
    case 'A': return 0x100;
    case 'B': return 0x200;
    case '*': return HM_B_SHOT;
    default: return HM_B_NONE;
    }
}

static int hm_parse(const char *p, hm_step *out, int cap)
{
    int n = 0;
    while (p && *p && n < cap) {
        char tok[64];
        unsigned k = 0;
        const char *q, *gt;
        hm_step s;
        memset(&s, 0, sizeof s);
        while (*p == ' ' || *p == ';') p++;
        if (!*p) break;
        while (*p && *p != ';' && k < sizeof tok - 1) tok[k++] = *p++;
        tok[k] = 0;
        if (tok[0] == 'j' && tok[1] == '=') {
            s.kind = HM_K_J;
            s.arg = atoi(tok + 2);
            out[n++] = s;
            continue;
        }
        gt = strrchr(tok, '>');
        if (!gt) return -1;
        if (strncmp(tok, "ls!=", 4) == 0) { s.kind = HM_K_LSN; s.arg = (int)strtol(tok + 4, NULL, 0); }
        else if (strncmp(tok, "ls=", 3) == 0) { s.kind = HM_K_LS; s.arg = (int)strtol(tok + 3, NULL, 0); }
        else if (strncmp(tok, "mn0=", 4) == 0) { s.kind = HM_K_MN0; s.arg = (int)strtol(tok + 4, NULL, 0); }
        else if (strncmp(tok, "rx:", 3) == 0) { s.kind = HM_K_RX; s.arg = (int)strtol(tok + 3, NULL, 0); }
        else if (strncmp(tok, "tx:", 3) == 0) { s.kind = HM_K_TX; s.arg = (int)strtol(tok + 3, NULL, 0); }
        else if (tok[0] == 'f') { s.kind = HM_K_F; }
        else return -1;
        q = strchr(tok, '+');
        s.delay = (q && q < gt) ? atoi(q + 1) : 0;
        q = strchr(tok, '/');
        if (q && q < gt) {
            s.to = atoi(q + 1);
            q = strchr(q, '=');
            s.to_j = (q && q < gt) ? atoi(q + 1) : 0;
        }
        s.btn = hm_btn(gt[1]);
        q = strchr(gt, ',');
        s.len = q ? atoi(q + 1) : 8;
        if (s.len < 1 || s.len > 120) s.len = 8;
        out[n++] = s;
    }
    return n;
}

/* W523 HOSTMIG-SCOPE: was nach einem gemeldeten Host-Wechsel geschieht (rein, im loc_net_selftest geprueft).
 * rb = loc_hostmig_rebuild (TEST/0.7.0), ls = Lobby-Zustand G+0x15, reason 1 left / 2 lost / 3 auto, run = Ablauf laeuft.
 * *desc = Sprachschluessel der Toast-Beschreibung. 0.6.3 (rb = 0): nie ein Ablauf ueber das Menue-Pad. */
enum { HM_ACT_NONE = 0, HM_ACT_HINT_AFTER = 1, HM_ACT_PEND = 2, HM_ACT_LEAVE = 3, HM_ACT_HINT_31 = 4 };
/* HM_ACT_HINT_AFTER: Hinweis erneut, sobald das Spiel die Mission verlaesst; HM_ACT_HINT_31: erneut, sobald das Spiel
 * seine Meldung „expelled“ (0x31) zeigt (kommt ~10 s nach dem Weggang, der erste Hinweis ist dann fast abgelaufen) */
static int hm_decide(int rb, int ls, int me_new, int me_old, int reason, int run, const char **desc)
{
    int ingame = ls >= 0x4E; /* im Match bzw. Ergebnis/Debriefing (0x4F/0x50) */
    if (reason == 3) *desc = "loc.hostmig_rebuild_d";
    else if (rb) *desc = ls == 0x4E ? "loc.hostmig_inmatch_d" : "loc.hostmig_rebuild_d";
    else *desc = ingame ? "loc.hostmig_inmatch_hand_d" : me_new ? "loc.hostmig_hand_new_d" : "loc.hostmig_hand_d";
    if (!rb) return me_old ? HM_ACT_NONE : ingame ? HM_ACT_HINT_AFTER : ls != 0x31 ? HM_ACT_HINT_31 : HM_ACT_NONE;
    if (run) return HM_ACT_NONE;
    if (me_old && reason == 3 && (ls == 0x22 || ls == 0x23)) return HM_ACT_LEAVE;
    return me_old ? HM_ACT_NONE : HM_ACT_PEND;
}

