/* loc_rulenet.c - W420 LOC-CLIPDUMP: "Clip dumping unterbinden" als Match-Regel des Hosts (LOC_BOOT §83).
 *
 * Spielstelle (LoC-XBE, B = Disassembly/Dekompilat):
 *   Nachladen der Hauptwaffe FUN_000CF3C0(0, Platz, VT) (einziger Aufrufer 0xB34A4, Taste "Magazin" als Flanke):
 *   Magazine > 0 -> Munition = WEPDAT+0x32, Magazine - 1, Ton 0x197, dann FUN_000CF390(0, VT) (Aufruf 0xCF4BE) =
 *   Feuerpause VT+0x2AC, Salvenabstand VT+0x2B4 und Salvenzaehler VT+0x2BC der Hauptwaffe auf 0. Der Schuss
 *   (FUN_000CD7B0) setzt die Pause auf (short)(WEPDAT+0x2A * [0x6D81BC]=1.0) und verweigert, solange sie > 0 ist; je
 *   Spieltick zieht FUN_000CA680 eins ab. Darum ist nach einem Magazinwechsel sofort der naechste Schuss frei.
 *   Eingriff (nur mit Regel): der Aufruf 0xCF4BE entfaellt, die laufende Pause/Salve bleibt; Nachladen selbst unveraendert.
 *   Deterministisch: reine Funktion des Spielzustands und der im Match festen Regel (gleich auf allen Clients, auch im
 *   Neurechnen des Rollbacks). Regel aus = Originalaufruf (bitgleich).
 * Regel des Matches: loc_rule.c (Kern), hier Kanal (SBRU, Spielport 1261), Rolle, Zulassen/Trennen, Log, Anzeige. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "loc_rulenet.h"
#include "loc_rule.h"
#include "loc_net.h"
#include "loc_net_core.h"
#include "loc_tun.h"
#include "loc_guard.h"
#include "loc_rollback.h"
#include "hcfg.h"
#include "hlog.h"
#include "sb_lang.h"
#include "sb_origview.h"

typedef uint32_t u32;

#define LRN_PATCH_VA 0xCF4BEu /* call FUN_000CF390 in FUN_000CF3C0 (Hauptwaffe) */
#define LRN_RESET_FN 0xCF390u
#define LRN_VT_LOCAL 0x5C47D0u /* Zeiger auf den eigenen VT (HUD liest Munition dort) */
#define LRN_CONN 0x3116ACu     /* Verbindungstabelle C: 10 x 16 B, Byte 0 Bit 0 belegt, +4 Socket */
#define LRN_G 0x361708u        /* Spielzustand G, G+0x15 Lobby-Zustand (0x22/0x23 = Match angeboten) */
#define LRN_SELF 0x3071B0u     /* eigener Spielerindex in der Mission (0 = Host) */
#define LRN_TICK 0x3070F4u     /* Spieltick (DIAG) */
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

static volatile LONG s_inst, s_eff, s_src = LRU_SRC_LOCAL, s_mission;
static volatile LONG s_kept, s_passed; /* Thunk: Pause behalten / Originalaufruf (Zaehler, auch im Neurechnen) */
static lru_t s_lru;
static CRITICAL_SECTION s_cs;
static SOCKET s_sock = INVALID_SOCKET;
static int s_sock_fail, s_tun_set, s_role, s_mission_n;
static u32 s_host_ip, s_t_host;
static u32 s_kills, s_kill_old, s_kill_mis, s_kill_newer;

typedef int(__cdecl *lrn_reset_t)(int kind, u32 vt);
/* ersetzt "call 0xCF390" bei 0xCF4BE (Argumente 0, VT auf dem Stapel; Rueckgabe wird verworfen) */
static int __cdecl lrn_reload_reset(int kind, u32 vt)
{
    if (s_eff & LRU_BIT_CLIPDUMP) {
        InterlockedIncrement(&s_kept);
        return 0;
    }
    InterlockedIncrement(&s_passed);
    return ((lrn_reset_t)(uintptr_t)LRN_RESET_FN)(kind, vt);
}

/* ------------------------------------------------------------------ Spielzustand (nur lesen, SEH) */
static int lrn_lobby_state(void)
{
    int v = -1;
    __try {
        u32 g = *(volatile const u32 *)(uintptr_t)LRN_G;
        if (g) v = *(volatile const uint8_t *)(uintptr_t)(g + 0x15u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v = -1;
    }
    return v;
}
static int lrn_self(void)
{
    int v = -1;
    __try {
        v = *(volatile const int *)(uintptr_t)LRN_SELF;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v = -1;
    }
    return (v >= 0 && v < 10) ? v : -1;
}
/* Socket des Platzes k (0 = keiner) */
static SOCKET lrn_slot(int k)
{
    SOCKET s = 0;
    __try {
        u32 c = *(volatile const u32 *)(uintptr_t)LRN_CONN;
        if (c && (*(volatile const uint8_t *)(uintptr_t)(c + (u32)k * 0x10u) & 1u))
            s = (SOCKET) * (volatile const u32 *)(uintptr_t)(c + (u32)k * 0x10u + 4u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s = 0;
    }
    return s == INVALID_SOCKET ? 0 : s;
}
static u32 lrn_sock_ip(SOCKET s)
{
    struct sockaddr_in a;
    int l = (int)sizeof a, e = WSAGetLastError();
    u32 ip = 0;
    if (s && getpeername(s, (struct sockaddr *)&a, &l) == 0 && a.sin_family == AF_INET) ip = a.sin_addr.s_addr;
    WSASetLastError(e);
    return ip;
}
/* Spiel-TCP zu Adresse ip trennen (wie der Gastfilter). Rueckgabe Anzahl. */
static int lrn_kill_ip(locnet *n, u32 ip)
{
    int k, c = 0;
    for (k = 0; k < 10 && ip; k++) {
        SOCKET s = lrn_slot(k);
        if (s && lrn_sock_ip(s) == ip) {
            locnet_guard_kill(n, s, LGUARD_V_STATE);
            c++;
        }
    }
    return c;
}
/* Hinweis im Spiel (Schluessel als Literal an sb_lang_w, damit lang_check sie sieht) */
static wchar_t s_t1[200], s_t2[320];
static void lrn_toast(void) { sb_ov_toast_msg(s_t1, s_t2, 10000u); }

/* ------------------------------------------------------------------ Kanal */
static void lrn_rx(u32 ip, const uint8_t *d, int len)
{
    EnterCriticalSection(&s_cs);
    (void)lru_rx(&s_lru, GetTickCount(), ip, d, len);
    LeaveCriticalSection(&s_cs);
}
static void lrn_tun_rx(uint32_t vip, const uint8_t *d, int len) { lrn_rx(vip, d, len); } /* Transport-Thread */

static void lrn_sock_open(locnet *n)
{
    struct sockaddr_in a;
    u_long nb = 1;
    BOOL off = FALSE, excl = TRUE;
    DWORD ret = 0;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        s_sock_fail = 1;
        return;
    }
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, (int)sizeof excl);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = n->self_ip;
    a.sin_port = htons(locnet_port_out(n, n->self_ip, (uint16_t)LRU_PORT_GP));
    if (bind(s, (const struct sockaddr *)&a, (int)sizeof a) != 0) {
        HLOG_W("LOCRULE: Regelkanal Port %u nicht frei (WSA %d) - Regel nur lokal/nach Host-Alt", ntohs(a.sin_port),
               WSAGetLastError());
        closesocket(s);
        s_sock_fail = 1;
        return;
    }
    ioctlsocket(s, FIONBIO, &nb);
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, (DWORD)sizeof off, NULL, 0, &ret, NULL, NULL);
    s_sock = s;
    HLOG_I("LOCRULE: Regelkanal UDP Port %u (Basis+261), nur bekannte Mitspieler", ntohs(a.sin_port));
}
static void lrn_send(locnet *n, const uint8_t *b, int len)
{
    int i;
    if (n->tun) {
        (void)loctun_dgram(n->tun, INADDR_BROADCAST, (uint16_t)LRU_PORT_GP, (uint16_t)LRU_PORT_GP, (const char *)b, len);
        return;
    }
    if (s_sock == INVALID_SOCKET) return;
    for (i = 0; i < n->npeers && i < LOCNET_MAX_PEERS; i++) {
        struct sockaddr_in to;
        if (n->peers[i].ip == n->self_ip) continue;
        memset(&to, 0, sizeof to);
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = n->peers[i].ip;
        to.sin_port = htons(locnet_port_out(n, n->peers[i].ip, (uint16_t)LRU_PORT_GP));
        sendto(s_sock, (const char *)b, len, 0, (const struct sockaddr *)&to, (int)sizeof to);
    }
}
static void lrn_poll_raw(locnet *n)
{
    int k, i;
    if (s_sock == INVALID_SOCKET) return;
    for (k = 0; k < 64; k++) {
        uint8_t b[64];
        struct sockaddr_in f;
        int fl = (int)sizeof f, ok = 0;
        int len = recvfrom(s_sock, (char *)b, (int)sizeof b, 0, (struct sockaddr *)&f, &fl);
        if (len < 0) {
            if (WSAGetLastError() == WSAECONNRESET) continue;
            break;
        }
        if (fl < (int)sizeof f || f.sin_family != AF_INET || f.sin_addr.s_addr == n->self_ip ||
            !locnet_ip_allowed(f.sin_addr.s_addr))
            continue;
        for (i = 0; i < n->npeers && i < LOCNET_MAX_PEERS && !ok; i++) /* nur bekannte Mitspieler, deren Regelport */
            ok = n->peers[i].ip == f.sin_addr.s_addr &&
                 locnet_port_out(n, f.sin_addr.s_addr, (uint16_t)LRU_PORT_GP) == ntohs(f.sin_port);
        if (ok) lrn_rx(f.sin_addr.s_addr, b, len);
    }
}

/* ------------------------------------------------------------------ DIAG */
static void lrn_diag(u32 now)
{
    int shots = 0, ammo = 0, mags = 0, cd = 0, bd = 0, bc = 0, iv = 0, ms = 0, bdn = 0;
    unsigned wid = 0xFFFF, sel = 0, bn = 0, tick = 0;
    __try {
        u32 vt = *(volatile const u32 *)(uintptr_t)LRN_VT_LOCAL;
        tick = *(volatile const u32 *)(uintptr_t)LRN_TICK;
        if (vt) {
            u32 wp = *(volatile const u32 *)(uintptr_t)(vt + 0x268u);
            shots = *(volatile const int *)(uintptr_t)(vt + 0x918u);
            wid = *(volatile const uint16_t *)(uintptr_t)(vt + 0x274u);
            sel = *(volatile const uint8_t *)(uintptr_t)(vt + 0xFAu);
            if (sel < 3) {
                ammo = *(volatile const int16_t *)(uintptr_t)(vt + 0x2CCu + sel * 2u);
                mags = *(volatile const int8_t *)(uintptr_t)(vt + 0x2E4u + sel);
            }
            cd = *(volatile const int16_t *)(uintptr_t)(vt + 0x2ACu);
            bd = *(volatile const int16_t *)(uintptr_t)(vt + 0x2B4u);
            bc = *(volatile const int16_t *)(uintptr_t)(vt + 0x2BCu);
            if (wp) {
                bn = *(volatile const uint16_t *)(uintptr_t)(wp + 0x26u);
                bdn = *(volatile const int16_t *)(uintptr_t)(wp + 0x28u);
                iv = *(volatile const int16_t *)(uintptr_t)(wp + 0x2Au);
                ms = *(volatile const int16_t *)(uintptr_t)(wp + 0x32u);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    HLOG_I("LOCCLIP t=%u tick=%u schuesse=%d waffe=%u platz=%u munition=%d magazine=%d pause=%d salve=%d/%d "
           "(wepdat: intervall %d, magazin %d, salve %u/%d) regel=%ld mission=%ld behalten=%ld original=%ld",
           now, tick, shots, wid, sel, ammo, mags, cd, bc, bd, iv, ms, bn, bdn, s_eff, s_mission, s_kept, s_passed);
}

/* ------------------------------------------------------------------ Thread */
static const char *lrn_role_name(int r) { return r == LRU_ROLE_HOST ? "host" : r == LRU_ROLE_JOIN ? "join" : "offline"; }

static DWORD WINAPI lrn_thread(LPVOID unused)
{
    u32 loop = 0;
    SOCKET seen_sk[10] = {0};
    u32 conn_at[10] = {0};
    u32 newer_nonce = 0, mis_streak[LRU_MAXPEER] = {0}, last_tick = 0, tick_at = 0;
    lru_phase ph = {0, 0, 0};
    int last_eff = -1, last_src = -1;
    int last_vs = 0;
    (void)unused;
    for (;; loop++) {
        u32 now;
        locnet *n;
        unsigned local = g_cfg.loc_rule_clipdump_block ? LRU_BIT_CLIPDUMP : 0u;
        int inm = 0, ev = 0, ls, k;
        u32 jmark = 0;
        Sleep(200);
        now = GetTickCount();
        n = loc_net_core();
        ls = lrn_lobby_state();
        if (n) {
            /* Mission = Spieltick laeuft (Lobby: steht) bzw. eigener Lockstep-Rahmen (TCP-0x34) oder eigener Rahmen im
             * Rollback in den letzten 3 s (Lauf R1b: Rahmen allein mit Luecken > 1,5 s) */
            LONG t34 = n->st.last34_ms, tow = g_loc_rb_own_ms;
            u32 tk = 0;
            __try {
                tk = *(volatile const u32 *)(uintptr_t)LRN_TICK;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                tk = 0;
            }
            if (tk != last_tick) {
                last_tick = tk;
                if (tk) tick_at = now | 1u;
            }
            ev = (t34 && (u32)(now - (u32)t34) < 3000u) || (tow && (u32)(now - (u32)tow) < 3000u) ||
                 (tick_at && (u32)(now - tick_at) < 3000u);
            jmark = (u32)n->join_ms;
            if (n->tun && !s_tun_set) {
                loctun_set_app2(n->tun, (uint16_t)LRU_PORT_GP, lrn_tun_rx);
                s_tun_set = 1;
                HLOG_I("LOCRULE: Regelkanal ueber den eigenen Transport (SBT2-Datagramm, Spielport %u, nicht an das Spiel)",
                       LRU_PORT_GP);
            }
            if (!n->tun && s_sock == INVALID_SOCKET && !s_sock_fail) lrn_sock_open(n);
            if (!n->tun) lrn_poll_raw(n);
        }
        if (ls == 0x22 || ls == 0x23) s_t_host = now | 1u; /* eigenes Spiel bietet ein Match an = Host */
        if (ls == 0x22 || ls == 0x23) ev = 0; /* Host in der Lobby: nie Mission (Spieltick dort ohne Bedeutung) */
        if (lru_phase_lobby(&ph, s_t_host, jmark)) /* neue Runde: Host bietet an bzw. neue Beitrittsanfrage */
            HLOG_I("LOCRULE: Lobby nach Mission %d erkannt (%s) - Regel wird neu bestimmt", s_mission_n,
                   s_t_host != ph.host_mark ? "Host bietet ein Match an" : "neue Beitrittsanfrage");
        inm = ph.frozen;
        if (!inm) { /* Rolle und Regel nur ausserhalb der Mission; in der Mission bleibt beides fest */
            u32 jt = n ? (u32)n->join_ms : 0u;
            int role = LRU_ROLE_OFFLINE, src = LRU_SRC_LOCAL;
            unsigned eff;
            if (n) {
                if (s_t_host && (!jt || (LONG)(s_t_host - jt) > 0)) role = LRU_ROLE_HOST;
                else if (jt) role = LRU_ROLE_JOIN;
            }
            s_role = role;
            s_host_ip = role == LRU_ROLE_JOIN ? (u32)n->join_ip : 0u;
            EnterCriticalSection(&s_cs);
            eff = lru_decide(&s_lru, role, s_host_ip, local, &src);
            if (src == LRU_SRC_HOST_NEWER) {
                const lru_peer *P = lru_find(&s_lru, s_host_ip);
                u32 nn = P ? P->nonce : 1u;
                LeaveCriticalSection(&s_cs);
                if (nn != newer_nonce && n) { /* einmal je Host-Prozess: trennen + Hinweis */
                    newer_nonce = nn;
                    s_kill_newer++;
                    s_kills += (u32)lrn_kill_ip(n, s_host_ip);
                    HLOG_W("LOCRULE: Host nutzt einen neueren Regelsatz (unbekannte Regel) - Verbindung getrennt, bitte "
                           "aktualisieren");
                    sb_lang_w("loc.rule_newer_t", s_t1, 200);
                    sb_lang_w("loc.rule_newer_d", s_t2, 320);
                    lrn_toast();
                }
            } else {
                LeaveCriticalSection(&s_cs);
            }
            InterlockedExchange(&s_eff, (LONG)eff);
            InterlockedExchange(&s_src, (LONG)src);
            if ((int)eff != last_eff || src != last_src) {
                HLOG_I("LOCRULE clipdump=%u quelle=%s (Rolle %s, Einstellung %u, Lobby)", eff & LRU_BIT_CLIPDUMP,
                       src == LRU_SRC_LOCAL ? "lokal" : "host", lrn_role_name(role), local);
                if (src == LRU_SRC_HOST_OLD) HLOG_I("LOCRULE: Host ohne Regel-Feld (aelterer Client) - Original-Regel");
                last_eff = (int)eff;
                last_src = src;
            }
        }
        if (lru_phase_mission(&ph, ev, s_t_host, jmark)) inm = 1;
        if (inm && !s_mission) {
            InterlockedExchange(&s_mission, 1);
            s_mission_n++;
            HLOG_I("LOCRULE clipdump=%ld quelle=%s (Mission %d, Rolle %s, Spieler %d, %s)", s_eff & LRU_BIT_CLIPDUMP,
                   s_src == LRU_SRC_LOCAL ? "lokal" : "host", s_mission_n, lrn_role_name(s_role), lrn_self(),
                   lru_src_name((int)s_src));
        } else if (!inm && s_mission) {
            InterlockedExchange(&s_mission, 0);
            HLOG_I("LOCRULE: Mission %d zu Ende, Regel frei (Magazinwechsel: Pause behalten %ld, Original %ld)", s_mission_n,
                   s_kept, s_passed);
        }
        if (!n) {
            if (g_cfg.loc_clip_diag && loop % 5u == 0u) lrn_diag(now);
            continue;
        }
        { /* SBRU senden */
            uint8_t b[LRU_PKT_LEN];
            int len = lru_build(b, (int)sizeof b, s_lru.nonce, inm ? lrn_self() : -1, local, inm, (unsigned)s_eff);
            if (len > 0) {
                lrn_send(n, b, len);
                s_lru.tx++;
            }
        }
        /* Host: Mitspieler zulassen (abweichende Regel braucht ein Regel-Feld) */
        if (s_role == LRU_ROLE_HOST) {
            for (k = 0; k < 10; k++) {
                SOCKET sk = lrn_slot(k);
                u32 ip;
                int adm;
                if (!sk) {
                    seen_sk[k] = 0;
                    continue;
                }
                if (sk != seen_sk[k]) {
                    seen_sk[k] = sk;
                    conn_at[k] = now;
                }
                ip = lrn_sock_ip(sk);
                if (!ip) continue;
                EnterCriticalSection(&s_cs);
                adm = lru_admit(&s_lru, ip, (unsigned)s_eff, now, conn_at[k]);
                LeaveCriticalSection(&s_cs);
                if (!adm) {
                    locnet_guard_kill(n, sk, LGUARD_V_STATE);
                    s_kills++;
                    s_kill_old++;
                    HLOG_W("LOCRULE: Mitspieler Platz %d ohne Regel-Feld (aelterer Client) bei Regel clipdump=%ld - "
                           "getrennt%s", k, s_eff & LRU_BIT_CLIPDUMP, inm ? " (Mission)" : " (Lobby)");
                    sb_lang_w("loc.rule_old_t", s_t1, 200);
                    sb_lang_w("loc.rule_old_d", s_t2, 320);
                    lrn_toast();
                    seen_sk[k] = 0;
                }
            }
        }
        /* Gegenprobe in der Mission: angewandte Regel gleich (Host: jeder Mitspieler; Beitretender: der Host) */
        if (inm) {
            int i;
            for (i = 0; i < LRU_MAXPEER; i++) {
                const lru_peer *P = &s_lru.p[i];
                int bad;
                u32 ip;
                EnterCriticalSection(&s_cs);
                ip = P->ip;
                bad = P->used && (s_role == LRU_ROLE_HOST || (s_role == LRU_ROLE_JOIN && ip == s_host_ip)) &&
                      lru_mismatch(P, (unsigned)s_eff, now);
                LeaveCriticalSection(&s_cs);
                mis_streak[i] = bad ? mis_streak[i] + 1u : 0u;
                if (mis_streak[i] == 2u) { /* zweimal in Folge (400 ms) */
                    s_kills += (u32)lrn_kill_ip(n, ip);
                    s_kill_mis++;
                    HLOG_W("LOCRULE: angewandte Regel weicht ab (eigene clipdump=%ld, Rolle %s) - Verbindung getrennt",
                           s_eff & LRU_BIT_CLIPDUMP, lrn_role_name(s_role));
                    sb_lang_w("loc.rule_mis_t", s_t1, 200);
                    sb_lang_w("loc.rule_mis_d", s_t2, 320);
                    lrn_toast();
                }
            }
        }
        if (n->tun) { /* W420: steelnet nur mit gleicher Client-Version - Ablehnung im Spiel melden */
            char need[16], peer[16];
            int vs = loctun_version_state(n->tun, need, (int)sizeof need, peer, (int)sizeof peer);
            if (vs && vs != last_vs) {
                wchar_t t1[200], fmt[300], t2[300], wn[16];
                int i;
                for (i = 0; i < 15 && need[i]; i++) wn[i] = (wchar_t)(unsigned char)need[i];
                wn[i] = 0;
                sb_lang_w("loc.ver_t", fmt, 300);
                if (wcsstr(fmt, L"%s")) _snwprintf_s(t1, 200, _TRUNCATE, fmt, wn[0] ? wn : L"?");
                else wcsncpy_s(t1, 200, fmt, _TRUNCATE);
                if (vs == 1) sb_lang_w("loc.ver_d_srv", t2, 300);
                else sb_lang_w("loc.ver_d_peer", t2, 300);
                sb_ov_toast_msg(t1, t2, 15000u);
                HLOG_W("LOCVER: %s - steelnet braucht die neueste Version (%s), Hinweis im Spiel",
                       vs == 1 ? "Treffpunkt lehnt ab" : "Mitspieler mit anderer Version abgelehnt", need[0] ? need : "?");
            }
            last_vs = vs;
        }
        if (g_cfg.loc_clip_diag && loop % 5u == 0u) lrn_diag(now);
        if (loop % 150u == 0u) {
            HLOG_I("LOCRULE-TEL: gesendet=%u empfangen=%u ungueltig=%u eigene=%u voll=%u getrennt=%u (alt %u, abweichend %u, "
                   "neuer %u) Magazinwechsel: behalten=%ld original=%ld",
                   s_lru.tx, s_lru.rx_ok, s_lru.rx_bad, s_lru.rx_self, s_lru.rx_full, s_kills, s_kill_old, s_kill_mis,
                   s_kill_newer, s_kept, s_passed);
        }
    }
}

void loc_rule_install(void)
{
    static const uint8_t k_call[5] = {0xE8, 0xCD, 0xFE, 0xFF, 0xFF}; /* call 0xCF390 */
    uint8_t *p = (uint8_t *)(uintptr_t)LRN_PATCH_VA;
    intptr_t rel = (uint8_t *)(void *)lrn_reload_reset - (p + 5);
    DWORD old;
    LARGE_INTEGER q;
    if (InterlockedExchange(&s_inst, 1)) return;
    if (memcmp(p, k_call, 5) != 0) {
        HLOG_E("LOCBYTES LOCRULE @0x%X: Byte-Mismatch - ABBRUCH (exit 7)", LRN_PATCH_VA);
        hlog_close();
        ExitProcess(7);
    }
    InitializeCriticalSection(&s_cs);
    QueryPerformanceCounter(&q);
    lru_init(&s_lru, (u32)(q.QuadPart ^ (q.QuadPart >> 32) ^ ((u32)GetCurrentProcessId() << 12)) | 1u);
    InterlockedExchange(&s_eff, g_cfg.loc_rule_clipdump_block ? (LONG)LRU_BIT_CLIPDUMP : 0);
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE8;
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    HLOG_I("LOCRULE: Eingriff Magazinwechsel Hauptwaffe @0x%X (FUN_000CF3C0 -> FUN_000CF390), Einstellung "
           "loc_rule_clipdump_block=%d (Host-Regel, Standard 1), Regelsatz %d",
           LRN_PATCH_VA, g_cfg.loc_rule_clipdump_block, LRU_RULESET);
    if (!CreateThread(NULL, 0, lrn_thread, NULL, 0, NULL)) HLOG_W("LOCRULE: Thread fehlt - Regel nur lokal");
}

int loc_rule_hud(int *on, int *src, int *mission)
{
    if (!s_inst) return 0;
    if (on) *on = (s_eff & LRU_BIT_CLIPDUMP) ? 1 : 0;
    if (src) *src = (int)s_src;
    if (mission) *mission = (int)s_mission;
    return 1;
}
