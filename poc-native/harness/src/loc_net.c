/* loc_net.c - W286 LOC-SYSLINK: XNet-/Winsock-HLE fuer Line of Contact.
 *
 * LoC ruft fuer System Link 33 XNET-Einstiege (LOC_HOOKS.tsv, Status loc-hle).
 * Die spielseitigen Thunks ("mov ecx,[0x73AE88]; jmp impl" bzw. mit Frame)
 * werden per 5-Byte-JMP auf eigene stdcall-Handler umgelenkt (Originalbytes
 * werden vorher verglichen). Die Handler rufen loc_net_core.c (echte Host-
 * Sockets, nur Loopback/LAN, siehe dort). htons und __WSAFDIsSet bleiben
 * original (reine Rechenfunktionen, fd_set-Aufbau = Win32).
 * Argumentzahlen = "ret N"/4 der Thunks bzw. XDK-Signatur (Thunks mit Frame). */
#include "loc_net_core.h"
#include "loc_net.h"
#include "loc_netsim.h"
#include "loc_tun.h"
#include "hcfg.h"
#include "hlog.h"
#include "kernel_hle.h"
#include "guest_wx.h" /* W339 SEC-A3 */
#include "loc_guard.h"
#include "loc_rollback.h" /* W353: loc_rb_diag */
#include "sb_pausenet.h" /* W357 */
#include "loc_assist.h" /* W440 LOC-LOBBY-MODES */
#include "sb_profile.h" /* W440: g_sba_loc */
#include "loc_chat.h" /* W474: lch_session_from */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>

typedef uint32_t u32;

static locnet g_ln;
void loc_net_bye(void)
{
    if (g_ln.tun) loctun_bye_all(g_ln.tun); /* W366 */
}
static volatile LONG s_first[40];

static void ln_log(const char *m)
{
    HLOG_I("%s", m);
}

static void ln_first(int id, const char *name, u32 ra)
{
    if (id >= 0 && id < 40 && InterlockedExchange(&s_first[id], 1) == 0) {
        int e = WSAGetLastError();
        HLOG_I("LOCNET: %s erster Aufruf (Aufrufer 0x%X)", name, ra);
        WSASetLastError(e);
    }
}
#define FIRST(id, nm) ln_first(id, nm, (u32)(uintptr_t)_ReturnAddress())

static void ln_copy(u32 dst, const void *src, unsigned n)
{
    if (dst && src && n) {
        __try { memcpy((void *)(uintptr_t)dst, src, n); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

/* ---------- XNet ---------- */
static u32 __stdcall ln_XNetStartup(u32 params)
{
    u32 p[3] = {0, 0, 0};
    if (params) {
        ln_copy((u32)(uintptr_t)p, (const void *)(uintptr_t)params, 12);
    }
    HLOG_I("LOCNET: XNetStartup (Parameter %08X %08X %08X) -> 0 (Aufrufer 0x%X)", p[0], p[1], p[2],
           (u32)(uintptr_t)_ReturnAddress());
    return 0;
}
static u32 __stdcall ln_WSAStartup(u32 ver, u32 data)
{
    static const u32 zero[100];
    FIRST(1, "WSAStartup");
    if (data) {
        uint16_t v[2];
        ln_copy(data, zero, 400);
        v[0] = (uint16_t)ver;
        v[1] = 0x0202;
        ln_copy(data, v, 4);
    }
    return 0;
}
static u32 __stdcall ln_XNetCleanup(void) { FIRST(2, "XNetCleanup"); return 0; }
static u32 __stdcall ln_WSACleanup(void) { FIRST(3, "WSACleanup"); return 0; }
static u32 __stdcall ln_XNetGetEthernetLinkStatus(void)
{
    FIRST(4, "XNetGetEthernetLinkStatus (-> Kabel verbunden, 100 MBit, voll duplex)");
    return 0x0Bu; /* ACTIVE | 100MBPS | FULL_DUPLEX */
}
static u32 __stdcall ln_XNetGetTitleXnAddr(u32 pxna)
{
    uint8_t x[LOCNET_XNADDR_SIZE];
    FIRST(5, "XNetGetTitleXnAddr");
    locnet_xnaddr(&g_ln, g_ln.self_ip, x);
    ln_copy(pxna, x, sizeof x);
    return 0x06u; /* ETHERNET | STATIC */
}
static u32 __stdcall ln_XNetRandom(u32 pb, u32 cb)
{
    uint8_t t[256];
    FIRST(6, "XNetRandom");
    while (cb > 0) {
        unsigned k = cb > sizeof t ? (unsigned)sizeof t : cb;
        locnet_random(t, k);
        ln_copy(pb, t, k);
        pb += k;
        cb -= k;
    }
    return 0;
}
static u32 __stdcall ln_XNetCreateKey(u32 pkid, u32 pkey)
{
    uint8_t kid[8], key[16];
    FIRST(7, "XNetCreateKey");
    locnet_create_key(kid, key);
    ln_copy(pkid, kid, 8);
    ln_copy(pkey, key, 16);
    return 0;
}
static u32 __stdcall ln_XNetRegisterKey(u32 pkid, u32 pkey)
{
    FIRST(8, "XNetRegisterKey");
    if (pkid) {
        ln_copy((u32)(uintptr_t)g_ln.kid, (const void *)(uintptr_t)pkid, 8);
    }
    if (pkey) {
        ln_copy((u32)(uintptr_t)g_ln.key, (const void *)(uintptr_t)pkey, 16);
    }
    g_ln.key_set = 1;
    return 0;
}
static u32 __stdcall ln_XNetUnregisterKey(u32 pkid) { (void)pkid; FIRST(9, "XNetUnregisterKey"); return 0; }
static u32 __stdcall ln_XNetXnAddrToInAddr(u32 pxna, u32 pkid, u32 pina)
{
    uint8_t x[LOCNET_XNADDR_SIZE];
    u32 ip;
    (void)pkid;
    FIRST(10, "XNetXnAddrToInAddr");
    memset(x, 0, sizeof x);
    if (pxna) {
        ln_copy((u32)(uintptr_t)x, (const void *)(uintptr_t)pxna, sizeof x);
    }
    ip = locnet_xnaddr_ip(x);
    if (!ip || !locnet_ip_allowed(ip)) {
        char b[16];
        HLOG_W("LOCNET: XNetXnAddrToInAddr: Adresse %s nicht lokal -> WSAEINVAL", locnet_ip_str(ip, b));
        return WSAEINVAL;
    }
    if (!locnet_addr_known(&g_ln, ip)) {
        /* W334 (SECURITY.md §6.5): Adressen aus Spielrahmen (XNADDR-Liste 0x13, Spielertabellen) nur, wenn sie die
         * eigene oder die eines verbundenen Mitspielers sind - sonst Fehler, nie eine fremde Adresse herausgeben. */
        static volatile LONG warned;
        char b[16];
        if (InterlockedIncrement(&warned) <= 8)
            HLOG_W("LOCNET: XNetXnAddrToInAddr: %s ist kein verbundener Mitspieler -> WSAEINVAL", locnet_ip_str(ip, b));
        return WSAEINVAL;
    }
    ln_copy(pina, &ip, 4);
    return 0;
}
static u32 __stdcall ln_XNetInAddrToXnAddr(u32 ina, u32 pxna, u32 pkid)
{
    uint8_t x[LOCNET_XNADDR_SIZE];
    FIRST(11, "XNetInAddrToXnAddr");
    locnet_xnaddr(&g_ln, ina, x);
    ln_copy(pxna, x, sizeof x);
    if (pkid) {
        ln_copy(pkid, g_ln.kid, 8);
    }
    return 0;
}
static u32 __stdcall ln_XNetUnregisterInAddr(u32 ina) { (void)ina; FIRST(12, "XNetUnregisterInAddr"); return 0; }
static u32 __stdcall ln_XNetConnect(u32 ina) { (void)ina; FIRST(13, "XNetConnect"); return 0; }
static u32 __stdcall ln_XNetGetConnectStatus(u32 ina)
{
    (void)ina;
    FIRST(14, "XNetGetConnectStatus");
    return 2u; /* XNET_CONNECT_STATUS_CONNECTED */
}
static u32 __stdcall ln_XNetQosListen(u32 pkid, u32 pb, u32 cb, u32 bps, u32 flags)
{
    (void)pkid; (void)pb; (void)cb; (void)bps;
    if (InterlockedExchange(&s_first[15], 1) == 0) {
        HLOG_I("LOCNET: XNetQosListen erster Aufruf (Flags 0x%X)", flags);
    }
    return 0;
}
static u32 __stdcall ln_XNetQosLookup(u32 cxna, u32 apxna, u32 apkid, u32 apkey, u32 cina, u32 aina, u32 asvc,
                                      u32 probes, u32 bps, u32 flags, u32 hev, u32 ppq)
{
    void *q;
    u32 v;
    (void)apxna; (void)apkid; (void)apkey; (void)aina; (void)asvc; (void)bps; (void)flags; (void)hev;
    if (InterlockedExchange(&s_first[16], 1) == 0) {
        HLOG_I("LOCNET: XNetQosLookup erster Aufruf (%u Peers, %u Dienste, %u Proben) -> RTT %d ms", cxna, cina,
               probes, g_ln.rtt_ms);
    }
    q = locnet_qos_alloc(&g_ln, cxna + cina, probes);
    if (!q) {
        return WSAENOBUFS;
    }
    v = (u32)(uintptr_t)q;
    ln_copy(ppq, &v, 4);
    return 0;
}
static u32 __stdcall ln_XNetQosRelease(u32 q)
{
    FIRST(17, "XNetQosRelease");
    locnet_qos_free((void *)(uintptr_t)q);
    return 0;
}

/* ---------- Winsock ---------- */
static u32 __stdcall ln_socket(u32 af, u32 type, u32 proto)
{
    SOCKET s = locnet_socket(&g_ln, (int)af, (int)type, (int)proto);
    int e = WSAGetLastError();
    HLOG_I("LOCNET: socket(%u,%u,%u) -> 0x%X (Aufrufer 0x%X)", af, type, proto, (u32)s,
           (u32)(uintptr_t)_ReturnAddress());
    WSASetLastError(e);
    return (u32)s;
}
static u32 __stdcall ln_closesocket(u32 s)
{
    FIRST(18, "closesocket");
    return (u32)locnet_closesocket(&g_ln, (SOCKET)s);
}
static u32 __stdcall ln_ioctlsocket(u32 s, u32 cmd, u32 argp)
{
    FIRST(19, "ioctlsocket");
    return (u32)ioctlsocket((SOCKET)s, (long)cmd, (u_long *)(uintptr_t)argp);
}
static u32 __stdcall ln_setsockopt(u32 s, u32 level, u32 opt, u32 val, u32 len)
{
    FIRST(20, "setsockopt");
    return (u32)locnet_setsockopt(&g_ln, (SOCKET)s, (int)level, (int)opt, (const char *)(uintptr_t)val, (int)len);
}
static u32 __stdcall ln_bind(u32 s, u32 a, u32 len)
{
    return (u32)locnet_bind(&g_ln, (SOCKET)s, (const struct sockaddr *)(uintptr_t)a, (int)len);
}
static u32 __stdcall ln_connect(u32 s, u32 a, u32 len)
{
    return (u32)locnet_connect(&g_ln, (SOCKET)s, (const struct sockaddr *)(uintptr_t)a, (int)len);
}
static u32 __stdcall ln_listen(u32 s, u32 backlog)
{
    int r;
    FIRST(21, "listen");
    r = listen((SOCKET)s, (int)backlog);
    if (r == 0) InterlockedExchange(&g_ln.tcp_listen, (LONG)s); /* W409: Spiel lauscht (TCP 1000) */
    return (u32)r;
}
static u32 __stdcall ln_accept(u32 s, u32 a, u32 plen)
{
    return (u32)locnet_accept(&g_ln, (SOCKET)s, (struct sockaddr *)(uintptr_t)a, (int *)(uintptr_t)plen);
}
static u32 __stdcall ln_select(u32 nfds, u32 r, u32 w, u32 x, u32 tv)
{
    FIRST(22, "select");
    return (u32)select((int)nfds, (fd_set *)(uintptr_t)r, (fd_set *)(uintptr_t)w, (fd_set *)(uintptr_t)x,
                       (const struct timeval *)(uintptr_t)tv);
}
/* W339 TEST (loc_crash_test=N, Standard 0): N s nach Beginn der Netzsitzung im Spiel-Thread eine Ausnahme im
 * Gastkontext ausloesen - prueft "Absturz = Ende" (Erwartung: LOCSEC-Zeile, Absturzbericht, Exitcode 39).
 * Modus 0: Schreibzugriff auf Adresse 0 aus privatem, ausfuehrbarem Code (wie eingeschleuster Code).
 * Modus 1: Sprung in eine beschreibbare Gast-Datenseite (W^X: Ausfuehrungsfehler; ohne W^X liefe das RET durch und
 * die Zeile "W^X wirkt NICHT" erscheint - Gegenprobe). */
static void ln_crash_test(void)
{
    static ULONGLONG t0;
    static volatile LONG fired;
    if (g_cfg.loc_crash_test <= 0 || !g_locnet_session || fired) return;
    if (!t0) {
        t0 = GetTickCount64();
        return;
    }
    if (GetTickCount64() - t0 < (ULONGLONG)g_cfg.loc_crash_test * 1000u) return;
    if (InterlockedExchange(&fired, 1)) return;
    if (g_cfg.loc_crash_test_mode == 1) {
        uint32_t a = guest_wx_data_probe_addr();
        volatile uint8_t *p;
        uint8_t save;
        if (!a) {
            HLOG_W("LOCTEST: keine verwaltete Gast-Datenseite (W^X aus) - Sprung in Daten waere ungeschuetzt, entfaellt");
            return;
        }
        p = (volatile uint8_t *)(uintptr_t)(a + 0xFF0u);
        HLOG_W("LOCTEST: Sprung in Gast-Datenseite 0x%08X (erwartet: Ausnahme, Sitzung beendet)", a + 0xFF0u);
        hlog_flush();
        save = *p;
        *p = 0xC3; /* ret */
        ((void(__cdecl *)(void))(uintptr_t)p)();
        *p = save;
        HLOG_E("LOCTEST: Gast-Datenseite war ausfuehrbar - W^X wirkt NICHT");
        return;
    }
    {
        static const uint8_t code[] = { 0xC7, 0x05, 0, 0, 0, 0, 0x39, 0x03, 0, 0, 0xC3 }; /* mov dword [0], 0x339; ret */
        DWORD old;
        uint8_t *p = (uint8_t *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!p) return;
        memcpy(p, code, sizeof code);
        VirtualProtect(p, 4096, PAGE_EXECUTE_READ, &old);
        HLOG_W("LOCTEST: Schreibfehler aus privatem Code (erwartet: Sitzung beendet)");
        hlog_flush();
        ((void(__cdecl *)(void))(uintptr_t)p)();
        HLOG_E("LOCTEST: Ausnahme wurde fortgesetzt - Absturz = Ende wirkt NICHT");
    }
}

static u32 __stdcall ln_recv(u32 s, u32 buf, u32 len, u32 flags)
{
    int r;
    FIRST(23, "recv");
    ln_crash_test(); /* W339 TEST, Standard aus */
    r = locnet_recv(&g_ln, (SOCKET)s, (char *)(uintptr_t)buf, (int)len, (int)flags); /* W334: Gastfilter */
    if (r > 0) {
        int e = WSAGetLastError();
        InterlockedIncrement(&g_ln.st.rx);
        locnet_note_stream(&g_ln, (SOCKET)s, 0, (const char *)(uintptr_t)buf, r);
        WSASetLastError(e);
    }
    return (u32)r;
}
static u32 __stdcall ln_recvfrom(u32 s, u32 buf, u32 len, u32 flags, u32 from, u32 pfl)
{
    FIRST(24, "recvfrom");
    ln_crash_test(); /* W339 TEST, Standard aus (das Spiel liest auch TCP ueber recvfrom) */
    return (u32)locnet_recvfrom(&g_ln, (SOCKET)s, (char *)(uintptr_t)buf, (int)len, (int)flags,
                                (struct sockaddr *)(uintptr_t)from, (int *)(uintptr_t)pfl);
}
static u32 __stdcall ln_send(u32 s, u32 buf, u32 len, u32 flags)
{
    int r;
    FIRST(25, "send");
    if (g_ln.sim) { /* W322: ueber die Verzoegerungsstufe */
        return (u32)locnet_send(&g_ln, (SOCKET)s, (const char *)(uintptr_t)buf, (int)len, (int)flags);
    }
    r = send((SOCKET)s, (const char *)(uintptr_t)buf, (int)len, (int)flags);
    if (r > 0) {
        int e = WSAGetLastError();
        InterlockedIncrement(&g_ln.st.tx);
        locnet_note_stream(&g_ln, (SOCKET)s, 1, (const char *)(uintptr_t)buf, r);
        WSASetLastError(e);
    }
    return (u32)r;
}
static u32 __stdcall ln_sendto(u32 s, u32 buf, u32 len, u32 flags, u32 to, u32 tolen)
{
    FIRST(26, "sendto");
    return (u32)locnet_sendto(&g_ln, (SOCKET)s, (const char *)(uintptr_t)buf, (int)len, (int)flags,
                              (const struct sockaddr *)(uintptr_t)to, (int)tolen);
}
/* XOnlineStartup (Bibliothek) startet den Live-Stapel ueber den XNet-Kontext [0x73AE88],
 * den nur das echte XnInit anlegt (W286A: Schreibzugriff 0x175C7B auf [0+0xAEC]).
 * System Link braucht Live nicht -> S_OK ohne Live-Stapel. */
static u32 __stdcall ln_XOnlineStartup(u32 params)
{
    (void)params;
    FIRST(27, "XOnlineStartup (-> S_OK, ohne Live-Stapel)");
    return 0;
}
static u32 __stdcall ln_XOnlineCleanup(void) { FIRST(28, "XOnlineCleanup"); return 0; }
static u32 __stdcall ln_WSAGetLastError(void)
{
    return (u32)WSAGetLastError();
}

/* ---------- Installation ---------- */
typedef struct ln_hook {
    u32 va;
    uint8_t expect[8];
    void *handler;
    const char *name;
} ln_hook;

#define TH(b5) {0x8B, 0x0D, 0x88, 0xAE, 0x73, 0x00, 0xE9, b5} /* mov ecx,[0x73AE88]; jmp */
#define FR {0x55, 0x8B, 0xEC, 0x8B, 0x0D, 0x88, 0xAE, 0x73}  /* push ebp; mov ebp,esp; mov ecx,[..] */
static const ln_hook k_hooks[] = {
    {0x119610u, {0xE9, 0xA0, 0xE7, 0x00, 0x00, 0xCC, 0xCC, 0xCC}, (void *)ln_XNetGetEthernetLinkStatus, "XNetGetEthernetLinkStatus"},
    {0x17070Fu, TH(0x59), (void *)ln_XNetRandom, "XNetRandom"},
    {0x17071Au, TH(0x1B), (void *)ln_XNetCreateKey, "XNetCreateKey"},
    {0x170725u, TH(0x21), (void *)ln_XNetRegisterKey, "XNetRegisterKey"},
    {0x170730u, TH(0x27), (void *)ln_XNetUnregisterKey, "XNetUnregisterKey"},
    {0x17073Bu, {0x8B, 0x0D, 0x88, 0xAE, 0x73, 0x00, 0x85, 0xC9}, (void *)ln_XNetXnAddrToInAddr, "XNetXnAddrToInAddr"},
    {0x170774u, FR, (void *)ln_XNetQosListen, "XNetQosListen"},
    {0x170783u, FR, (void *)ln_XNetQosLookup, "XNetQosLookup"},
    {0x17079Fu, TH(0x1C), (void *)ln_XNetQosRelease, "XNetQosRelease"},
    {0x1707AAu, TH(0xDB), (void *)ln_XNetGetTitleXnAddr, "XNetGetTitleXnAddr"},
    {0x1707B5u, TH(0x71), (void *)ln_socket, "socket"},
    {0x1707C0u, TH(0x10), (void *)ln_closesocket, "closesocket"},
    {0x1707D6u, TH(0x08), (void *)ln_ioctlsocket, "ioctlsocket"},
    {0x1707E1u, FR, (void *)ln_setsockopt, "setsockopt"},
    {0x1707F0u, TH(0x41), (void *)ln_bind, "bind"},
    {0x1707FBu, TH(0xBA), (void *)ln_connect, "connect"},
    {0x170806u, TH(0x16), (void *)ln_listen, "listen"},
    {0x170811u, TH(0x9F), (void *)ln_accept, "accept"},
    {0x17081Cu, FR, (void *)ln_select, "select"},
    {0x170845u, TH(0x3B), (void *)ln_recv, "recv"},
    {0x17085Fu, FR, (void *)ln_recvfrom, "recvfrom"},
    {0x17086Eu, TH(0xFE), (void *)ln_send, "send"},
    {0x170888u, FR, (void *)ln_sendto, "sendto"},
    {0x1708F1u, {0x8B, 0x0D, 0x88, 0xAE, 0x73, 0x00, 0x85, 0xC9}, (void *)ln_XNetInAddrToXnAddr, "XNetInAddrToXnAddr"},
    {0x170908u, TH(0x96), (void *)ln_XNetUnregisterInAddr, "XNetUnregisterInAddr"},
    {0x170913u, TH(0x9C), (void *)ln_XNetConnect, "XNetConnect"},
    {0x17091Eu, TH(0xA2), (void *)ln_XNetGetConnectStatus, "XNetGetConnectStatus"},
    {0x170B2Fu, {0x33, 0xC0, 0x50, 0x50, 0x50, 0xFF, 0x74, 0x24}, (void *)ln_XNetStartup, "XNetStartup"},
    {0x170B46u, {0xFF, 0x74, 0x24, 0x08, 0xFF, 0x74, 0x24, 0x08}, (void *)ln_WSAStartup, "WSAStartup"},
    {0x170C34u, {0x6A, 0x00, 0x68, 0x88, 0xAE, 0x73, 0x00, 0xE8}, (void *)ln_XNetCleanup, "XNetCleanup"},
    {0x170C41u, {0x6A, 0x01, 0x68, 0x88, 0xAE, 0x73, 0x00, 0xE8}, (void *)ln_WSACleanup, "WSACleanup"},
    {0x170CD1u, {0xE9, 0xB4, 0xA0, 0xFB, 0xFF, 0x8B, 0x4C, 0x24}, (void *)ln_WSAGetLastError, "WSAGetLastError"},
    {0x157FE4u, {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x90, 0x01, 0x00}, (void *)ln_XOnlineStartup, "XOnlineStartup"},
    {0x1580CFu, {0x53, 0x56, 0x33, 0xDB, 0xB8, 0x00, 0x00, 0x00}, (void *)ln_XOnlineCleanup, "XOnlineCleanup"},
};

/* W297: Netz-Zustand fuer LoC-Testfolgen (loc_steps): 0 = nicht bereit. */
/* W338 LOC-ROLLBACK-R2: Netz-Kern fuer den eigenen Eingabekanal (NULL = Netz aus). */
struct locnet *loc_net_core(void) { return g_ln.ready ? &g_ln : NULL; }

/* W375 TEST: Testfolgen ueber mehrere Runden - Typ-Merkliste und Zaehler fuer acc>=/con>= neu (Missionsende). */
static long s_tst_acc0, s_tst_con0;
void loc_net_test_rearm(void)
{
    int i, k;
    for (i = 0; i < 2; i++)
        for (k = 0; k < 8; k++) InterlockedExchange(&g_ln.st.typ_seen[i][k], 0);
    s_tst_acc0 = g_ln.st.accepts;
    s_tst_con0 = g_ln.st.connects;
}

int loc_net_test_state(long *accepts, long *connects, int out, unsigned typ)
{
    if (!g_ln.ready) {
        return 0;
    }
    if (accepts) *accepts = g_ln.st.accepts - s_tst_acc0; /* W375: ab der letzten Neuvorlage (Runde) */
    if (connects) *connects = g_ln.st.connects - s_tst_con0;
    if (typ < 256) {
        return (g_ln.st.typ_seen[out ? 1 : 0][typ >> 5] >> (typ & 31)) & 1 ? 2 : 1;
    }
    if (typ == 512) { /* Lockstep aktiv: eigener 0x34-Rahmen in der letzten Sekunde gesendet */
        LONG t = g_ln.st.last34_ms;
        return (t && (LONG)(GetTickCount() - (DWORD)t) < 1000) ? 3 : 1;
    }
    return 1;
}

/* W339 SEC-A3 (BERICHT_M2 "W339: SEC-A3", LOC_RE1 §6.6): Typ 0x38 (UDP 1202, 8 B) -> FUN_00055F40 schreibt
 * Rahmen+4 nach P[slot]+0x48 mit slot = FUN_00050DD0(Absender G+0x98): Suche ueber die belegten Plaetze
 * (FUN_0004F5A0: XNADDR T[i] = G+0xA8+i*0x300 nicht null) nach P[i]+0x24 == Absender; ohne Treffer liefert sie 0xFF
 * und FUN_00055F40 schreibt ungeprueft nach P+0xFF*0x300+0x48 und (&DAT_0038FA42)[0xFF*8] (FUN_00069D60). Darum:
 * 0x38 nur von einem Absender, der in der Spielertabelle steht - genau die Suche des Spiels, vorher. */
static int ln_udp_state(uint16_t gp, const uint8_t *d, int n, uint32_t from_ip)
{
    int ok = 0;
    if (gp != 1202 || n < 2 || ((unsigned)d[0] | ((unsigned)d[1] << 8)) != 0x38) return 1;
    __try {
        uint32_t G = *(volatile const uint32_t *)(uintptr_t)0x361708u;
        uint32_t P = *(volatile const uint32_t *)(uintptr_t)0x3116B0u;
        int i, k;
        if (G && P) {
            for (i = 0; i < 10 && !ok; i++) {
                const volatile uint32_t *t = (const volatile uint32_t *)(uintptr_t)(G + 0xA8u + (uint32_t)i * 0x300u);
                uint32_t nz = 0;
                for (k = 0; k < 9; k++) nz |= t[k];
                if (nz && *(volatile const uint32_t *)(uintptr_t)(P + (uint32_t)i * 0x300u + 0x24u) == from_ip) ok = 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = 0;
    }
    return ok;
}

/* W339 SEC-A3: Blockpuffer 0x20/0x2E. FUN_000569D0 (0x20) und FUN_00056770 (0x2E) haengen Rahmen+4 = n Byte an einen
 * laufenden Versatz in einem 0x5040-B-Puffer an (0x20: Puffer FUN_001095E0(1, G+0xDE29), Versatz G+0xDE54+idx*8+4,
 * Puffer angelegt mit 0x5040 B ueber 0x109730; 0x2E: Puffer conn*0x5040 + DAT_00581BEC (FUN_000A2130), Versatz
 * [*(0x37A4D8) + conn*0x14 + 0x10]) und pruefen die Summe nicht (0x20 erst NACH dem Kopieren). Der Gastfilter
 * begrenzt n zustandslos auf die Nutzlast 0x1000; hier, an der Aufrufstelle im Spielverteiler FUN_000533F0, prueft
 * ein Wrapper mit dem Spielzustand "Versatz + n <= 0x5040". Verstoss: Rahmen nicht verarbeiten, Spiel-TCP-Socket
 * dieses Mitspielers trennen (wie ein Gastfilter-Verstoss). */
typedef int(__cdecl *ln_frame_handler)(uint32_t frame, int conn, int arg3);
#define LN_BLK_SIZE 0x5040u

static int ln_blk_violation(int conn, int typ)
{
    static volatile LONG logged;
    SOCKET sk = INVALID_SOCKET;
    __try {
        uint32_t tab = *(volatile const uint32_t *)(uintptr_t)0x3116ACu;
        if (tab && conn >= 0 && conn < 10) sk = (SOCKET) * (volatile const uint32_t *)(uintptr_t)(tab + (uint32_t)conn * 0x10u + 4u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sk = INVALID_SOCKET;
    }
    if (InterlockedIncrement(&logged) <= 8) {
        HLOG_W("LOCGUARD: Spiel-TCP Verstoss Blockpuffer (Typ 0x%02X, Platz %d) - Verbindung getrennt", typ, conn);
    }
    if (sk != INVALID_SOCKET && sk != 0) locnet_guard_kill(&g_ln, sk, LGUARD_V_CONTENT);
    return 1; /* Rahmen gilt als verarbeitet (nichts kopiert) */
}

static int __cdecl ln_hk_blk20(uint32_t frame, int conn, int arg3)
{
    int bad = 0;
    __try {
        uint32_t G = *(volatile const uint32_t *)(uintptr_t)0x361708u;
        if (*(volatile const char *)(uintptr_t)(G + 0x15u) == '?') { /* nur in diesem Zustand kopiert das Original */
            int idx = *(volatile const signed char *)(uintptr_t)(G + 0xDE29u);
            uint32_t n = *(volatile const uint32_t *)(uintptr_t)(frame + 4u), off;
            if (idx < 0 || idx > 9) {
                bad = 1; /* FUN_001095E0(1, idx): Tabelle 0x130 + (idx + 5) * 4 mit 15 Plaetzen */
            } else {
                off = *(volatile const uint32_t *)(uintptr_t)(G + 0xDE54u + (uint32_t)idx * 8u + 4u);
                bad = n > 0x1000u || off > LN_BLK_SIZE || n > LN_BLK_SIZE - off;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        bad = 1;
    }
    if (bad) return ln_blk_violation(conn, 0x20);
    return ((ln_frame_handler)(uintptr_t)0x569D0u)(frame, conn, arg3);
}

static int __cdecl ln_hk_blk2e(uint32_t frame, int conn, int arg3)
{
    int bad = 0;
    __try {
        uint32_t st = *(volatile const uint32_t *)(uintptr_t)0x37A4D8u;
        uint32_t n = *(volatile const uint32_t *)(uintptr_t)(frame + 4u), off;
        if (conn < 0 || conn > 9 || !st) {
            bad = 1;
        } else {
            off = *(volatile const uint32_t *)(uintptr_t)(st + (uint32_t)conn * 0x14u + 0x10u);
            bad = n > 0x1000u || off > LN_BLK_SIZE || n > LN_BLK_SIZE - off;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        bad = 1;
    }
    if (bad) return ln_blk_violation(conn, 0x2E);
    return ((ln_frame_handler)(uintptr_t)0x56770u)(frame, conn, arg3);
}

/* call rel32 an site von target auf fn umbiegen (nur wenn die Bytes passen). */
static int ln_patch_call(uint32_t site, uint32_t target, void *fn, const char *name)
{
    uint8_t *p = (uint8_t *)(uintptr_t)site;
    DWORD old;
    int32_t rel;
    if (p[0] != 0xE8) {
        HLOG_E("LOCBYTES LOCNET %s @0x%X: kein call - Hook fehlt", name, site);
        return 0;
    }
    memcpy(&rel, p + 1, 4);
    if ((uint32_t)(site + 5u + (uint32_t)rel) != target) {
        HLOG_E("LOCBYTES LOCNET %s @0x%X: Ziel passt nicht - Hook fehlt", name, site);
        return 0;
    }
    rel = (int32_t)((uint32_t)(uintptr_t)fn - (site + 5u));
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return 1;
}

int loc_net_install(void)
{
    unsigned i, n = (unsigned)(sizeof k_hooks / sizeof k_hooks[0]);
    if (!g_cfg.loc_net) {
        HLOG_I("LOCNET: aus (loc_net=0) - Netz meldet \"nicht bereit\" wie W275");
        return 0;
    }
    for (i = 0; i < n; i++) { /* erst alles pruefen, dann patchen */
        if (memcmp((const void *)(uintptr_t)k_hooks[i].va, k_hooks[i].expect, 8) != 0) {
            HLOG_E("LOCBYTES LOCNET %s @0x%X: Byte-Mismatch - ABBRUCH (exit 7)", k_hooks[i].name, k_hooks[i].va);
            hlog_close();
            ExitProcess(7);
        }
    }
    {
        /* W324 LOC-NETPLAY: mit eigenem Transport kommen die Mitspieler vom Transport; mit Treffpunkt eine
         * zufaellige virtuelle Adresse (eindeutig auch zwischen Rechnern). Ohne loc_transport=udp wie bisher. */
        int udp = _stricmp(g_cfg.loc_transport, "udp") == 0;
        const char *addr = g_cfg.loc_net_addr;
        if (udp && (g_cfg.loc_lobby[0] || g_cfg.loc_lan) && _stricmp(addr, "auto") == 0) { /* W454: auch LAN */
            addr = "random";
        }
        if (!locnet_init(&g_ln, addr, g_cfg.loc_net_port, udp ? "none" : g_cfg.loc_net_peers, g_cfg.loc_net_rtt,
                         ln_log)) {
            return 0;
        }
    }
    locnet_set_udp_state_check(ln_udp_state); /* W339 SEC-A3 */
    {
        /* W339 SEC-A3: Blockpuffer-Pruefung an den Aufrufstellen im Spielverteiler (fail closed: ohne Hook kein Netz) */
        int ok20 = ln_patch_call(0x5374Au, 0x569D0u, (void *)ln_hk_blk20, "blk20");
        int ok2e = ln_patch_call(0x5363Cu, 0x56770u, (void *)ln_hk_blk2e, "blk2e");
        if (!ok20 || !ok2e) {
            HLOG_E("LOCNET: Blockpuffer-Hooks fehlen - Netz aus (fail closed)");
            locnet_shutdown(&g_ln);
            return 0;
        }
        HLOG_I("LOCNET: Inhaltsregeln Stufe 2 aktiv (Gastfilter + 0x38-Absender + Blockpuffer 0x20/0x2E)");
    }
    g_ln.fin_pass = g_cfg.loc_net_leave_reset ? 0 : 1; /* W350: Abgang als Verbindungsreset */
    g_ln.nodelay = g_cfg.loc_net_nodelay ? 1 : 0; /* W322: TCP_NODELAY auf den Spiel-Verbindungen */
    if (g_cfg.loc_netsim[0]) { /* W322 LOC-NETLAT (DIAG/TEST, Standard aus) */
        locnetsim_cfg sc;
        char err[96];
        if (locnetsim_parse(g_cfg.loc_netsim, &sc, err, (int)sizeof err)) {
            if (!sc.nagle_set) {
                sc.nagle = g_ln.nodelay ? 0 : 1; /* Modell folgt dem echten Socket */
            }
            g_ln.sim = locnetsim_start(&sc, ln_log);
            /* QoS wie gemessen: mindestens die Strecken-RTT (Startverzoegerung des Spiels); mit Transport misst
             * dieser selbst (W324) */
            if (g_ln.sim && _stricmp(g_cfg.loc_transport, "udp") != 0 && sc.rtt_ms + 2.0 * sc.jitter_ms > g_ln.rtt_ms) {
                g_ln.rtt_ms = (int)(sc.rtt_ms + 2.0 * sc.jitter_ms + 0.5);
                HLOG_I("LOCNETSIM: QoS-RTT -> %d ms (Strecke)", g_ln.rtt_ms);
            }
        } else if (err[0]) {
            HLOG_W("LOCNETSIM: loc_netsim=\"%s\" ungueltig (%s) - Stufe aus", g_cfg.loc_netsim, err);
        }
    }
    if (_stricmp(g_cfg.loc_transport, "udp") == 0) { /* W324 LOC-NETPLAY */
        loctun_cfg tc;
        const char *od = getenv("SB_OUT_DIR");
        loctun_cfg_default(&tc);
        tc.port = (uint16_t)(g_cfg.loc_transport_port > 0 && g_cfg.loc_transport_port < 65536 ? g_cfg.loc_transport_port : 0);
        tc.proxy_base = (uint16_t)g_cfg.loc_transport_proxy;
        tc.red_fixed = g_cfg.loc_transport_red;
        tc.gap_ms = g_cfg.loc_transport_gap;
        tc.hold_ms = g_cfg.loc_transport_hold;
        tc.qos_reserve_ms = g_cfg.loc_transport_qos;
        tc.nodirect = g_cfg.loc_transport_nodirect;
        strcpy_s(tc.netcut_file, sizeof tc.netcut_file, g_cfg.loc_test_netcut); /* W366 TEST */
        tc.route_auto = g_cfg.loc_route;                                      /* W395 */
        tc.open_hold_ms = g_cfg.loc_transport_open_hold < 0 ? 0 : g_cfg.loc_transport_open_hold > 10000 ? 10000
                                                                : g_cfg.loc_transport_open_hold; /* W409 */
        tc.test_refuse_ms = g_cfg.loc_test_open_refuse;                       /* W409 TEST */
        strcpy_s(tc.route_sim, sizeof tc.route_sim, g_cfg.loc_route_sim);   /* W395 TEST */
        tc.allow_public = g_cfg.loc_lobby[0] ? 1 : 0;
        strncpy_s(tc.peers, sizeof tc.peers, g_cfg.loc_transport_peers, _TRUNCATE);
        strncpy_s(tc.lobby, sizeof tc.lobby, g_cfg.loc_lobby, _TRUNCATE);
        strncpy_s(tc.room, sizeof tc.room, g_cfg.loc_lobby_room, _TRUNCATE);
        strncpy_s(tc.pilot, sizeof tc.pilot, g_cfg.loc_lobby_pilot, _TRUNCATE);
        strncpy_s(tc.role, sizeof tc.role, g_cfg.loc_lobby_role, _TRUNCATE);
        strncpy_s(tc.relays, sizeof tc.relays, g_cfg.loc_relays, _TRUNCATE);
        strncpy_s(tc.code, sizeof tc.code, g_cfg.loc_lobby_code, _TRUNCATE); /* W334 */
        strncpy_s(tc.vis, sizeof tc.vis, g_cfg.loc_lobby_vis, _TRUNCATE);   /* W373 */
        strncpy_s(tc.spki, sizeof tc.spki, g_cfg.loc_lobby_spki, _TRUNCATE);
        strcpy_s(tc.play_mode, sizeof tc.play_mode, g_cfg.loc_play_mode == LAS_ARCADE ? "arcade" : "sim"); /* W440 */
        tc.join_delay_ms = g_cfg.loc_lobby_join_delay;                      /* W446 TEST */
        tc.lan = g_cfg.loc_lan && !g_cfg.loc_lobby[0];                       /* W454 LOC-LAN-MODE */
        tc.lan_port = (uint16_t)g_cfg.loc_lan_port;
        strncpy_s(tc.lan_room, sizeof tc.lan_room, g_cfg.loc_lan_room, _TRUNCATE);
        strncpy_s(tc.lan_pass, sizeof tc.lan_pass, g_cfg.loc_lan_pass, _TRUNCATE);
        SecureZeroMemory(g_cfg.loc_lan_pass, sizeof g_cfg.loc_lan_pass);
        strncpy_s(tc.lan_targets, sizeof tc.lan_targets, g_cfg.loc_lan_targets, _TRUNCATE);
        _snprintf_s(tc.status_path, sizeof tc.status_path, _TRUNCATE, "%s%sloc_net_status.txt", od ? od : "",
                    od ? "/" : "");
        g_ln.tun = loctun_start(&tc, &g_ln, g_ln.sim, ln_log);
        if (!g_ln.tun) {
            HLOG_W("LOCTUN: Transport nicht gestartet - keine Mitspieler (loc_transport=tcp fuer System Link)");
        }
    }
    for (i = 0; i < n; i++) {
        uint8_t *p = (uint8_t *)(uintptr_t)k_hooks[i].va;
        intptr_t rel = (uint8_t *)k_hooks[i].handler - (p + 5);
        DWORD old;
        VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
        p[0] = 0xE9;
        memcpy(p + 1, &rel, 4);
        VirtualProtect(p, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 5);
    }
    HLOG_I("LOCNET: %u XNet-/Winsock-Einstiege umgelenkt (Bytevergleich ok)", n);
    return 1;
}

/* W350 LOC-REJOIN (DIAG, loc_net_diag=1): Verbindungstabelle C = *(0x3116AC), 10 Eintraege zu 16 Byte
 * (Byte 0: Bit 0 belegt, Bit 1 verbunden; +4 Socket; +8 Stillezaehler, LOC_RE1 §6.3), Spielzustand G = *(0x361708):
 * G+0xA4/G+0xA5 Spieler je Seite, G+0x5174/G+0x5175 Gesamtzaehler (FUN_000511E0 zaehlt sie beim Entfernen herunter).
 * Zeile nur bei Aenderung, hoechstens 400. Keine Namen, keine Rahmeninhalte. */
void loc_net_lobby_frame(unsigned f)
{
    static u32 prev[5];
    static int n;
    u32 cur[5] = {0, 0, 0, 0, 0};
    /* W373 LOC-PUBLIC-LOBBY (LOC_BOOT §49.2, B): Host-Zustand fuer oeffentliche Lobbys alle 8 Bilder - Lobby-Zustand
     * G+0x15 (0x22/0x23 = Match angeboten, wartet auf Beitritte), S = G+0x5624 (Lager A/B, Sitze A/B, Modus, Mission,
     * Bestaetigung, Option), Spieler je Seite G+0xA4/A5. Nur Werte; der Transport prueft sie vor dem Veroeffentlichen. */
    if (g_ln.tun && (f & 7u) == 0) {
        LONG w0 = 0, w1 = 0, w2 = 0;
        __try {
            const uint8_t *g = *(const uint8_t *const *)(uintptr_t)0x361708u;
            if (g) {
                const uint8_t *s = g + 0x5624;
                w0 = (LONG)((u32)s[0] | ((u32)s[1] << 8) | ((u32)s[2] << 16) | ((u32)s[3] << 24));
                w1 = (LONG)((u32)s[4] | ((u32)s[5] << 8) | ((u32)s[6] << 16) | ((u32)s[7] << 24));
                w2 = (LONG)((u32)g[0x15] | ((u32)g[0xA4] << 8) | ((u32)g[0xA5] << 16) | 0x1000000u);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            w2 = 0;
        }
        InterlockedExchange(&g_ln.pub_w[2], 0); /* ungueltig, waehrend die Felder wechseln */
        InterlockedExchange(&g_ln.pub_w[0], w0);
        InterlockedExchange(&g_ln.pub_w[1], w1);
        InterlockedExchange(&g_ln.pub_w[2], w2);
    }
    if (!g_cfg.loc_net_diag || n >= 400) return;
    { /* W409 DIAG: Lobby-Zustand G+0x15 und Lauschsocket TCP 1000 (G+0x5160 != -1) bei jeder Aenderung */
        static int st_prev = -1, ls_prev = -1, st_n;
        int stv = -1, lsv = -1;
        __try {
            const uint8_t *g = *(const uint8_t *const *)(uintptr_t)0x361708u;
            if (g) {
                stv = g[0x15];
                lsv = *(const int32_t *)(g + 0x5160) != -1;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            stv = -1;
        }
        if (stv >= 0 && (stv != st_prev || lsv != ls_prev) && st_n < 400) {
            st_n++;
            HLOG_I("LOCLSTATE: Bild %u Zustand 0x%02X (vorher 0x%02X) TCP-1000-Lauschsocket %s", f, (unsigned)stv,
                   (unsigned)(st_prev & 0xFF), lsv ? "offen" : "zu");
            st_prev = stv;
            ls_prev = lsv;
        }
    }
    __try {
        const uint8_t *c = *(const uint8_t *const *)(uintptr_t)0x3116ACu;
        const uint8_t *g = *(const uint8_t *const *)(uintptr_t)0x361708u;
        int i;
        if (c) {
            for (i = 0; i < 10; i++) {
                if (c[i * 16] & 1) cur[0] |= 1u << i;
                if (c[i * 16] & 2) cur[1] |= 1u << i;
            }
        }
        if (g) {
            cur[2] = g[0xA4];
            cur[3] = g[0xA5];
            cur[4] = (u32)g[0x5174] | ((u32)g[0x5175] << 8);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!memcmp(cur, prev, sizeof cur)) return;
    memcpy(prev, cur, sizeof prev);
    n++;
    HLOG_I("LOCROSTER: Bild %u belegt=0x%03X verbunden=0x%03X Seite0=%u Seite1=%u Zaehler=%u/%u", f, cur[0], cur[1],
           cur[2], cur[3], cur[4] & 0xFFu, cur[4] >> 8);
}

/* DIAG (loc_net_diag=1): Spiel-Threads abtasten - wo steht das Spiel im Menue? */
static void ln_sample(void)
{
    int i, n = krnl_xb_thread_count();
    __try { /* Szene (Haupttabelle 0x27E328, Index u16 [0x727988]) + Szene-6-Unterzustand [[0x6DD3F8]] */
        const u32 *sub = *(const u32 *const *)(uintptr_t)0x6DD3F8u;
        u32 v[8];
        memset(v, 0, sizeof v);
        if (sub) {
            memcpy(v, sub, sizeof v);
        }
        HLOG_I("LOCNETDIAG: Szene=%u Schritt=%u | Szene6 [%08X %08X %08X %08X %08X %08X %08X]",
               *(const uint16_t *)(uintptr_t)0x727988u, *(const uint16_t *)(uintptr_t)0x72798Au, v[0], v[1], v[2],
               v[3], v[4], v[5], v[6]);
        sub = *(const u32 *const *)(uintptr_t)0x6DC678u; /* Menue-Unterzustand (Szene 5) */
        memset(v, 0, sizeof v);
        if (sub) {
            memcpy(v, sub, sizeof v);
        }
        HLOG_I("LOCNETDIAG: Menue [%08X %08X %08X %08X %08X %08X] Logon [%08X %08X]", v[0], v[1], v[2], v[3], v[4],
               v[5], (*(const u32 *const *)(uintptr_t)0x27CFECu)[0], (*(const u32 *const *)(uintptr_t)0x27CFECu)[1]);
        /* W297: Spielmodus-Flags (Netzspiel [0x3070D8], Ablauf [0x306B30], Modus [0x306B60], Sim-Tick) */
        HLOG_I("LOCNETDIAG: Netzspiel=%u Ablauf=%u Modus=%u Tick=%u", *(const u32 *)(uintptr_t)0x3070D8u,
               *(const u32 *)(uintptr_t)0x306B30u, *(const u32 *)(uintptr_t)0x306B60u, *(const u32 *)(uintptr_t)0x3070F4u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    for (i = 0; i < n; i++) {
        void *h = NULL;
        unsigned rt = 0;
        CONTEXT cx;
        u32 ra[8];
        int k = 0;
        if (!krnl_xb_thread_get(i, &h, &rt) || !h) {
            continue;
        }
        if (SuspendThread((HANDLE)h) == (DWORD)-1) {
            continue;
        }
        memset(&cx, 0, sizeof cx);
        cx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext((HANDLE)h, &cx)) {
            __try {
                const u32 *sp = (const u32 *)(uintptr_t)cx.Esp;
                int j;
                for (j = 0; j < 512 && k < 8; j++) {
                    if (sp[j] >= 0x11000u && sp[j] < 0x260000u) {
                        ra[k++] = sp[j];
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        ResumeThread((HANDLE)h);
        while (k < 8) {
            ra[k++] = 0;
        }
        HLOG_I("LOCNETDIAG: Thread %d (Start 0x%X) EIP=0x%08X Stack %X %X %X %X %X %X %X %X", i, rt, cx.Eip, ra[0],
               ra[1], ra[2], ra[3], ra[4], ra[5], ra[6], ra[7]);
    }
}

void loc_net_tick(int waited_s)
{
    if (g_cfg.loc_net_diag && waited_s % 10 == 0) {
        ln_sample();
    }
    if (g_ln.sim) { /* W322: Statistik der Verzoegerungsstufe alle 10 s */
        if (waited_s % 10 == 0) {
            const locnetsim_stats *ss = locnetsim_get_stats(g_ln.sim);
            HLOG_I("LOCNETSIM: t=%ds RTT=%.0f eingereiht=%ld zugestellt=%ld offen=%ld B (max %ld) Verweil p50=%d "
                   "p99=%d max=%ld ms TCP-Wdh=%ld UDP-verloren=%ld Nagle=%ld Fehler=%ld voll=%ld",
                   waited_s, locnetsim_rtt_now(g_ln.sim), ss->queued, ss->sent, ss->q_bytes, ss->q_bytes_max,
                   locnetsim_lat_pct(g_ln.sim, 50), locnetsim_lat_pct(g_ln.sim, 99), ss->lat_max_ms, ss->tcp_retx,
                   ss->udp_lost, ss->nagle_held, ss->send_err, ss->overflow);
        }
    }
    if (g_ln.tun && waited_s % 10 == 0) { /* W324: Weg, RTT, Verlust, Redundanz je Mitspieler */
        loctun_log_stats(g_ln.tun, waited_s);
    }
    if (g_ln.ready && waited_s % 30 == 0) {
        HLOG_I("LOCNET: t=%ds Sockets=%ld gesendet=%ld empfangen=%ld Broadcasts=%ld verworfen=%ld connect=%ld "
               "accept=%ld Filter=%ld zurueck=%ld",
               waited_s, g_ln.st.sockets, g_ln.st.tx, g_ln.st.rx, g_ln.st.bc, g_ln.st.drop, g_ln.st.connects,
               g_ln.st.accepts, g_ln.st.guard, g_ln.st.hold);
    }
}

/* W353 LOC-DURCHSTICH: Netzwerte fuer das F11-Paket (sb_feedback.c). Nur Zustaende/Zahlen. */
/* W357 LOC-PAUSE-NET (sb_pausenet.h): nur Zaehler lesen, keine Inhalte. */
int loc_net_peer_live(void)
{
    static sb_pn_live_t s;
    int links = 0;
    if (!g_cfg.loc_boot || !g_ln.ready) return sb_pn_live_step(&s, GetTickCount(), 0, 0, 0, 0, 0);
    if (g_ln.tun) links = loctun_links_up(g_ln.tun);
    return sb_pn_live_step(&s, GetTickCount(), g_ln.st.rx, links, g_ln.st.last34_ms,
                           (long)g_ln.st.accepts + g_ln.st.connects - g_ln.st.leave, 1);
}

/* W440 LOC-LOBBY-MODES: wirksamer Spielmodus (loc_assist.h). Liest nur Zustaende (Eingabe-Thread je Abfrage, Overlay
 * 2x/s); Logzeile LOCMODE einmal je Wechsel von Modus/Quelle mit der Wirkung auf die eigenen Hilfen. */
int loc_net_assist_mode(int *src)
{
    static volatile LONG last = -1;
    int s = LAS_SRC_SB, m, srv, lobby = 0;
    LONG key;
    srv = g_cfg.loc_lobby[0] && _stricmp(g_cfg.loc_transport, "udp") == 0;
    if (g_sba_loc && g_ln.tun) lobby = loctun_play_mode(g_ln.tun);
    m = las_effective(g_sba_loc, g_cfg.loc_boot && g_cfg.loc_net, srv, lobby, g_cfg.loc_play_mode_test, &s);
    key = (LONG)(m * 16 + s);
    if (InterlockedExchange(&last, key) != key && g_sba_loc) {
        HLOG_I("LOCMODE: wirksam %s (Quelle %s) - Zielhilfe %d -> %d, Gang-Automatik %d -> %d (Ausweich-Hilfe und "
               "Kippschalter-Hilfe unveraendert)", las_name(m), las_src_name(s), g_cfg.aim_assist,
               las_value(m, g_cfg.aim_assist), g_cfg.auto_gear, las_value(m, g_cfg.auto_gear));
    }
    if (src) *src = s;
    return m;
}

int loc_assist_gate(int cfg_value)
{
    if (!cfg_value || !g_sba_loc) return cfg_value; /* SB und ausgeschaltete Hilfe: unveraendert, ohne Abfrage */
    return las_value(loc_net_assist_mode(NULL), cfg_value);
}

int loc_net_hud_read(loc_net_hud *o) /* W374 F3-Overlay */
{
    int i;
    loctun_peer_hud ph;
    if (!o) return 0;
    memset(o, 0, sizeof *o);
    if (!g_cfg.loc_boot || !g_cfg.loc_net) return 0;
    o->tun = g_ln.tun ? 1 : 0;
    if (!g_ln.tun) return 1;
    for (i = 0; i < 32; i++) {
        memset(&ph, 0, sizeof ph);
        if (!loctun_peer_hud_get(g_ln.tun, i, &ph)) break;
        o->known++;
        if (o->npeer < LOC_NET_HUD_PEERS) {
            strncpy_s(o->peer[o->npeer].name, sizeof o->peer[o->npeer].name, ph.pilot, _TRUNCATE);
            o->peer[o->npeer].ping = ph.srtt;
            o->peer[o->npeer].up = ph.up;
            o->npeer++;
        }
        if (!ph.up) continue;
        o->up++;
        o->route = (o->route == 0 || o->route == ph.path) ? ph.path : 3;
        if (ph.srtt >= o->ping) {
            int k;
            o->ping = ph.srtt;
            o->ping_min = ph.rtt_min;
            /* W395: Wege dieses (langsamsten) Mitspielers */
            strncpy_s(o->route_name, sizeof o->route_name, ph.route, _TRUNCATE);
            o->nswitch = ph.nswitch;
            o->nalt = ph.nalt < 4 ? ph.nalt : 4;
            for (k = 0; k < o->nalt; k++) {
                strncpy_s(o->alt[k].name, sizeof o->alt[k].name, ph.alt[k].name, _TRUNCATE);
                o->alt[k].rtt = ph.alt[k].rtt < 0 ? -1 : (int)(ph.alt[k].rtt + 0.5);
                o->alt[k].state = ph.alt[k].state;
                o->alt[k].active = ph.alt[k].active;
            }
        }
        if (ph.rttvar > o->jitter) o->jitter = ph.rttvar;
        if (ph.loss_in > o->loss_in) o->loss_in = ph.loss_in;
        if (ph.loss_peer > o->loss_out) o->loss_out = ph.loss_peer;
    }
    return 1;
}

int loc_net_diag_line(char *buf, int cap)
{
    char tun[320] = "", rb[320] = "";
    if (!buf || cap <= 0) return 0;
    buf[0] = 0;
    if (!g_cfg.loc_boot) return 0;
    if (g_ln.tun) loctun_diag(g_ln.tun, tun, (int)sizeof tun);
    loc_rb_diag(rb, (int)sizeof rb);
    _snprintf_s(buf, (size_t)cap, _TRUNCATE, "transport=%s%s%s netsim=%s %s | rollback: %s",
                g_cfg.loc_net ? (_stricmp(g_cfg.loc_transport, "udp") == 0 ? "udp" : "tcp-original") : "off",
                tun[0] ? " " : "", tun, g_cfg.loc_netsim[0] ? "on" : "off", g_cfg.loc_sync ? "sync=on" : "sync=off", rb);
    return 1;
}

/* W447 LOC-NET-AUTO: GetTickCount des letzten eigenen Lockstep-Rahmens (0 = nie bzw. kein LoC-Netz); Lobby-Panel im
 * Lockstep (ohne Eingabeschicht). Nur Lesen. */
long loc_net_last34_ms(void)
{
    return (g_cfg.loc_boot && g_ln.ready) ? (long)g_ln.st.last34_ms : 0;
}

/* W447: offene Spiel-TCP-Stroeme; > 0 = das Spiel ist in einer Sitzung (Lobby/Match). W474: echte Zahl offener
 * Stroeme (eigenes Schliessen zaehlt mit, frueher nur Abgaenge des Mitspielers -> nach dem Verlassen blieb sie > 0). */
long loc_net_streams(void)
{
    return (g_cfg.loc_boot && g_ln.ready) ? (long)g_ln.st.open : 0;
}

/* W474 CHATTABS: zentraler Lobby-/Match-Zustand (LCH_SES_*, loc_chat.h lch_session_from) fuer Chat-Reiter "Lobby" und
 * Lobby-Infopanel. Liest Szene, Seite (Text-Id der ersten Menuezeile, Zeilenzahl), Lockstep-Rahmen und die Mission der
 * Eingabeschicht; ein Abgang (NONE) gilt erst nach LN_SES_HOLD_MS ununterbrochen (Uebergaenge zwischen Seiten),
 * Lobby <-> Match sofort. Beliebige Threads (Chat-Thread 10x/s, Spiel-Thread 2x/s); Log LOCSES je Wechsel (nur Zahlen). */
#define LN_SES_HOLD_MS 400u
static SRWLOCK s_ses_lock = SRWLOCK_INIT;
static int s_ses_rep;
static u32 s_ses_none_t;
int loc_net_lobby_state(void) { return loc_net_lobby_state_ex(NULL); }

int loc_net_lobby_state_ex(int *out)
{
    int scene = -1, tid = -1, nit = -1, ls = 0, rbm, streams, page, raw, rep;
    u32 now = GetTickCount();
    if (out) *out = 1;
    if (!g_cfg.loc_boot || !g_ln.ready || !g_sba_loc) return LCH_SES_NONE;
    streams = (int)g_ln.st.open;
    __try {
        scene = *(volatile const uint16_t *)(uintptr_t)0x727988u;  /* Szene (W344) */
        tid = (int)*(volatile const u32 *)(uintptr_t)0x6DC68Cu;     /* Text-Id der ersten Menuezeile (W460) */
        nit = *(volatile const uint8_t *)(uintptr_t)0x6DD2B0u;      /* Zeilenzahl (W460) */
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        scene = -1;
    }
    /* Mission = eigene Lockstep-Rahmen in den letzten 3 s (wie W447) bzw. Mission der Eingabeschicht. Nicht [0x306830]:
     * die Aktiv-Maske bleibt nach TIME UP bis in die Nachbesprechung und ueber SIGN OUT hinaus (LOC_BOOT W375 E4/E5). */
    ls = g_ln.st.last34_ms && (u32)(now - (u32)g_ln.st.last34_ms) < 3000u;
    rbm = loc_rb_in_mission();
    page = lch_page(scene, tid, nit);
    raw = lch_session_from(streams, scene, page, ls, rbm);
    if (out) *out = lch_out_of_lobby(scene, page);
    AcquireSRWLockExclusive(&s_ses_lock);
    if (raw != LCH_SES_NONE) {
        s_ses_none_t = 0;
        rep = raw;
    } else if (s_ses_rep == LCH_SES_NONE) {
        rep = LCH_SES_NONE;
    } else {
        if (!s_ses_none_t) s_ses_none_t = now | 1u;
        rep = (u32)(now - s_ses_none_t) >= LN_SES_HOLD_MS ? LCH_SES_NONE : s_ses_rep;
    }
    if (rep != s_ses_rep) {
        HLOG_I("LOCSES: %s (Szene %d Seite %d Text-Id 0x%X/%d Stroeme %d Lockstep %d Mission %d)",
               rep == LCH_SES_MATCH ? "Match" : rep == LCH_SES_LOBBY ? "Lobby" : "keine Lobby", scene, page,
               (unsigned)tid, nit, streams, ls, rbm);
        s_ses_rep = rep;
    }
    ReleaseSRWLockExclusive(&s_ses_lock);
    return rep;
}
