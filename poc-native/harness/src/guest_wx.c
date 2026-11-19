/* guest_wx.c - W339 SEC-A3: W^X fuer das Gastabbild (siehe guest_wx.h, SECURITY.md §6.6 Punkt 4). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "guest_wx.h"
#include "hlog.h"

int g_guest_wx;

#define WX_R 1
#define WX_W 2
#define WX_X 4
#define WX_PG_MAX (0x2810000u >> 12)
static uint8_t s_cls[WX_PG_MAX]; /* gewollte Klasse je Seite (0 = nicht verwaltet) */
static uint32_t s_lo, s_hi;      /* verwalteter Bereich [s_lo, s_hi) */
static uint32_t s_probe;         /* beschreibbare, nicht ausfuehrbare Gastseite fuer Tests */
static volatile LONG s_nlog;

/* Klasse einer Section. Die Xbox kennt kein NX: Code, .rdata und .data tragen alle das X-Flag (0x4); die Flags trennen
 * Code nicht von Daten. Festlegung nach Namen und Messung (W339-Diagnoselauf guest_wx=2, Lobby + 16 min Mission):
 *   .rdata, XON_RD, DOLBY                         -> R   (nie beschrieben)
 *   XDK-Bibliotheken D3D, D3DX, XGRPH, DSOUND,
 *   XACTENG, WMADEC, XPP                          -> RWX (Code UND beschreibbare Daten in derselben Section: gemessen
 *                                                       Schreiben in D3D/DSOUND/XPP durch Code aus genau diesen
 *                                                       Sections; dokumentierte Ausnahme, SECURITY.md §6.6)
 *   sonst W-Flag (.data, segNN, .data1)           -> RW  (nie ausgefuehrt)
 *   sonst X-Flag (.text, XONLINE, XNET; SB BINK*) -> RX
 *   sonst ($$XT..., .XTLID)                       -> RW */
#define XBE_SEC_W 0x1u
#define XBE_SEC_X 0x4u
static int wx_class(const section_info_t *s)
{
    static const char *const ro[] = { ".rdata", "XON_RD", "DOLBY", NULL };
    static const char *const mixed[] = { "D3D", "D3DX", "XGRPH", "DSOUND", "XACTENG", "WMADEC", "XPP", NULL };
    int i;
    for (i = 0; ro[i]; i++) {
        if (strcmp(s->name, ro[i]) == 0) return WX_R;
    }
    for (i = 0; mixed[i]; i++) {
        if (strcmp(s->name, mixed[i]) == 0) return WX_R | WX_W | WX_X;
    }
    if (s->flags & XBE_SEC_W) return WX_R | WX_W;
    if (s->flags & XBE_SEC_X) return WX_R | WX_X;
    return WX_R | WX_W;
}

static DWORD wx_prot(int c)
{
    if ((c & WX_X) && (c & WX_W)) return PAGE_EXECUTE_READWRITE;
    if (c & WX_X) return PAGE_EXECUTE_READ;
    if (c & WX_W) return PAGE_READWRITE;
    return PAGE_READONLY;
}

static void wx_mark(uint32_t va, uint32_t size, int c)
{
    uint32_t p, e;
    if (!size) return;
    p = va >> 12;
    e = (va + size - 1) >> 12;
    for (; p <= e && p < WX_PG_MAX; p++) s_cls[p] |= (uint8_t)c;
}

/* Seiten [p0, p1] mit gleicher Klasse in Laeufen umschalten. Rueckgabe: Zahl der RWX-Seiten (Konflikte). */
static unsigned wx_protect_pages(uint32_t p0, uint32_t p1)
{
    unsigned conflicts = 0;
    uint32_t p = p0;
    while (p <= p1) {
        uint32_t q = p;
        DWORD old;
        int c = s_cls[p];
        if (!c) {
            p++;
            continue;
        }
        while (q + 1 <= p1 && s_cls[q + 1] == c) q++;
        if ((c & WX_X) && (c & WX_W)) conflicts += q - p + 1;
        if (!VirtualProtect((LPVOID)(uintptr_t)(p << 12), (SIZE_T)(q - p + 1) << 12, wx_prot(c), &old)) {
            HLOG_W("GUESTWX: VirtualProtect 0x%08X+0x%X fehlgeschlagen (GLE=%lu)", p << 12, (q - p + 1) << 12,
                   GetLastError());
        }
        p = q + 1;
    }
    return conflicts;
}

static const char *wx_cname(int c)
{
    if ((c & WX_X) && (c & WX_W)) return "RWX";
    if (c & WX_X) return "Code";
    if (c & WX_W) return "Daten";
    return "Nur-Lese";
}

/* Verletzungen melden; im Diagnosemodus die Seite RWX schalten und weiterlaufen lassen. Registriert als erster
 * VEH: echte Schutzverletzungen auf beschreibbaren Seiten (Rollback-Schreibschutz W330/W338) gehen unveraendert
 * weiter (Klasse W gesetzt -> nicht unser Fall). */
static LONG CALLBACK wx_veh(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    ULONG_PTR kind;
    uint32_t a, pg;
    int c;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 || !g_guest_wx) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    kind = er->ExceptionInformation[0];
    a = (uint32_t)er->ExceptionInformation[1];
    if ((kind != 1 && kind != 8) || a < s_lo || a >= s_hi) return EXCEPTION_CONTINUE_SEARCH;
    pg = a >> 12;
    c = s_cls[pg];
    if (!c) return EXCEPTION_CONTINUE_SEARCH;
    if (kind == 1 && (c & WX_W)) return EXCEPTION_CONTINUE_SEARCH; /* beschreibbar gewollt: fremder Schreibschutz */
    if (kind == 8 && (c & WX_X)) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&s_nlog) <= 48) {
        HLOG_W("GUESTWX: %s auf %s-Seite 0x%08X (RIP 0x%08X)%s", kind == 1 ? "Schreiben" : "Ausfuehren", wx_cname(c),
               pg << 12, (uint32_t)ep->ContextRecord->Eip,
               g_guest_wx == 2 ? " - Seite jetzt RWX (Diagnose)" : " - strikt, nicht erlaubt");
    }
    if (g_guest_wx == 2) {
        DWORD old;
        s_cls[pg] = WX_R | WX_W | WX_X;
        if (VirtualProtect((LPVOID)(uintptr_t)(pg << 12), 4096, PAGE_EXECUTE_READWRITE, &old)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static int dep_on(void)
{
    typedef BOOL(WINAPI * get_dep_t)(HANDLE, LPDWORD, PBOOL);
    get_dep_t f = (get_dep_t)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetProcessDEPPolicy");
    DWORD fl = 0;
    BOOL perm = FALSE;
    if (!f || !f(GetCurrentProcess(), &fl, &perm)) return -1;
    return (fl & PROCESS_DEP_ENABLE) ? 1 : 0;
}

void guest_wx_apply(const xbe_image_t *img, int mode)
{
    uint32_t i, p, rx = 0, ro = 0, rw = 0, conflicts;
    int dep;
    g_guest_wx = 0;
    if (mode <= 0 || !img) {
        HLOG_I("GUESTWX: aus (Gastabbild bleibt RWX)");
        return;
    }
    if (img->image_end > 0x2810000u || img->base < 0x10000u) {
        HLOG_W("GUESTWX: Abbild 0x%X..0x%X ausserhalb der Loader-Region - aus", img->base, img->image_end);
        return;
    }
    memset(s_cls, 0, sizeof s_cls);
    s_lo = img->base;
    s_hi = img->image_end;
    wx_mark(img->base, img->size_headers, WX_R | WX_W); /* Header: HDRSWAP/Hybrid schreibt ihn, nie Code */
    for (i = 0; i < img->n_sections; i++) {
        const section_info_t *s = &img->sections[i];
        wx_mark(s->vaddr, s->vsize, wx_class(s));
    }
    conflicts = wx_protect_pages(s_lo >> 12, (s_hi - 1) >> 12);
    for (p = s_lo >> 12; p < (s_hi >> 12) && p < WX_PG_MAX; p++) {
        int c = s_cls[p];
        if (!c) continue;
        if ((c & WX_X) && !(c & WX_W)) rx++;
        else if (!(c & (WX_X | WX_W))) ro++;
        else if (!(c & WX_X)) {
            rw++;
            s_probe = p << 12; /* letzte reine Datenseite */
        }
    }
    g_guest_wx = mode;
    AddVectoredExceptionHandler(1, wx_veh);
    dep = dep_on();
    HLOG_I("GUESTWX: %s - Code RX %u Seiten, Nur-Lese %u, Daten RW %u, RWX (XDK-Bibliotheken: Code+Daten) %u; DEP %s",
           mode == 2 ? "Diagnose" : "aktiv", rx, ro, rw, conflicts, dep == 1 ? "an" : dep == 0 ? "AUS" : "?");
    if (dep != 1) HLOG_W("GUESTWX: DEP ist nicht aktiv - Datenseiten bleiben praktisch ausfuehrbar");
    guest_wx_scan("nach guest_wx_apply");
}

void guest_wx_section_loaded(const section_info_t *s)
{
    if (!g_guest_wx || !s || !s->vsize) return;
    wx_protect_pages(s->vaddr >> 12, (s->vaddr + s->vsize - 1) >> 12);
}

void guest_wx_scan(const char *when)
{
    MEMORY_BASIC_INFORMATION mi;
    uintptr_t a = 0x10000;
    unsigned n = 0, img_n = 0;
    SIZE_T bytes = 0;
    while (a < 0xFFFF0000u && VirtualQuery((LPCVOID)a, &mi, sizeof mi) == sizeof mi) {
        uintptr_t next = (uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (mi.State == MEM_COMMIT &&
            (mi.Protect & 0xFF) != 0 &&
            ((mi.Protect & 0xFF) == PAGE_EXECUTE_READWRITE || (mi.Protect & 0xFF) == PAGE_EXECUTE_WRITECOPY)) {
            n++;
            bytes += mi.RegionSize;
            if ((uintptr_t)mi.BaseAddress >= s_lo && (uintptr_t)mi.BaseAddress < s_hi) img_n++;
            if (n <= 12) {
                HLOG_I("GUESTWX: RWX-Bereich 0x%08X+0x%X (%s%s)", (uint32_t)(uintptr_t)mi.BaseAddress,
                       (uint32_t)mi.RegionSize,
                       mi.Type == MEM_IMAGE ? "Abbild" : mi.Type == MEM_MAPPED ? "abgebildet" : "privat",
                       ((uintptr_t)mi.BaseAddress >= s_lo && (uintptr_t)mi.BaseAddress < s_hi) ? ", Gastabbild" : "");
            }
        }
        if (next <= a) break;
        a = next;
    }
    HLOG_I("GUESTWX: %s: %u RWX-Bereiche (%u im Gastabbild), 0x%X Bytes", when ? when : "-", n, img_n,
           (unsigned)bytes);
}

int guest_wx_data_exec(void)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!s_probe) return 1;
    if (VirtualQuery((LPCVOID)(uintptr_t)s_probe, &mi, sizeof mi) != sizeof mi) return 1;
    return (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

uint32_t guest_wx_data_probe_addr(void)
{
    return g_guest_wx ? s_probe : 0;
}
