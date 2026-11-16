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
#include "hcfg.h"
#include "hlog.h"
#include "kernel_hle.h"

#include <string.h>
#include <intrin.h>

typedef uint32_t u32;

static locnet g_ln;
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
    FIRST(21, "listen");
    return (u32)listen((SOCKET)s, (int)backlog);
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
static u32 __stdcall ln_recv(u32 s, u32 buf, u32 len, u32 flags)
{
    int r;
    FIRST(23, "recv");
    r = recv((SOCKET)s, (char *)(uintptr_t)buf, (int)len, (int)flags);
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
    return (u32)locnet_recvfrom(&g_ln, (SOCKET)s, (char *)(uintptr_t)buf, (int)len, (int)flags,
                                (struct sockaddr *)(uintptr_t)from, (int *)(uintptr_t)pfl);
}
static u32 __stdcall ln_send(u32 s, u32 buf, u32 len, u32 flags)
{
    int r;
    FIRST(25, "send");
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
int loc_net_test_state(long *accepts, long *connects, int out, unsigned typ)
{
    if (!g_ln.ready) {
        return 0;
    }
    if (accepts) *accepts = g_ln.st.accepts;
    if (connects) *connects = g_ln.st.connects;
    if (typ < 256) {
        return (g_ln.st.typ_seen[out ? 1 : 0][typ >> 5] >> (typ & 31)) & 1 ? 2 : 1;
    }
    if (typ == 512) { /* Lockstep aktiv: eigener 0x34-Rahmen in der letzten Sekunde gesendet */
        LONG t = g_ln.st.last34_ms;
        return (t && (LONG)(GetTickCount() - (DWORD)t) < 1000) ? 3 : 1;
    }
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
    if (!locnet_init(&g_ln, g_cfg.loc_net_addr, g_cfg.loc_net_port, g_cfg.loc_net_peers, g_cfg.loc_net_rtt,
                     ln_log)) {
        return 0;
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
    if (g_ln.ready && waited_s % 30 == 0) {
        HLOG_I("LOCNET: t=%ds Sockets=%ld gesendet=%ld empfangen=%ld Broadcasts=%ld verworfen=%ld connect=%ld "
               "accept=%ld",
               waited_s, g_ln.st.sockets, g_ln.st.tx, g_ln.st.rx, g_ln.st.bc, g_ln.st.drop, g_ln.st.connects,
               g_ln.st.accepts);
    }
}
