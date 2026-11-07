/* sb_tex.c - Xbox-Textur-Cache (M2b). Parst D3DResource-Header
 * (Common@0, Data@+4, Format@+0xC) und laedt DXT1/3/5 als BC1/2/3
 * nach D3D11 hoch (Mip-Level 0, linear angenommen - Screenshot
 * verifiziert). USIZE/VSIZE geben log2(Breite/Hoehe) bei swizzled/
 * komprimierten Formaten (Cxbx X_D3DFORMAT_*).
 */
#define COBJMACROS
#include "sb_tex.h"
#include "hlog.h"
#include "hcfg.h"
#include "d3d11_render.h"
#include "sb_remaster.h" /* W103 REM-FRAME */
int sb_ctx_release_all(void); /* W138 d3d11_render.c */
void sb_ctx_reacquire(int r);
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static ID3D11Device *g_tdev;
static unsigned g_stage0_xptr;

/* W63 TEXCACHE: Kapazitaet zur Laufzeit ([render] tex_cache_max, Default
 * 2048, alt 256). Mission 1 laedt beim Fahren ~200 Gelaende-Kacheln
 * (128x128 DXT1) nach - mit 256 Slots war der Cache ab E+~800..2700 voll
 * (W63_base1/c4: 'TEX cache voll' 46k/260k Mal), jede neue Textur blieb
 * NULL (Mission 2/Briefing-Film ohne Texturen, Treiber-AV W63_c4). */
#define TEX_CACHE_CAP 4096
#define TEX_HINT_N 4096
typedef struct {
    unsigned xptr;
    ID3D11ShaderResourceView *srv;
    unsigned w, h;
    unsigned size, hash, data;
    unsigned fmt; int linear; /* DIAG(MOVIE): Xbox-FORMAT + Linear-Klasse */
    volatile int dirty; /* TEXDIRTY: per LockRect beschrieben */
    unsigned lastuse; /* W63: letzter Zugriff (Get-Zaehler) fuer LRU */
    unsigned hfmt;    /* W63 tex_hdr_check: Format-Feld (+0xC) beim Upload */
    unsigned gets;    /* W63 tex_hdr_check: Gets je Eintrag (Refresh-Takt) */
    unsigned lw_frame, lw_hash; /* W98 tex_lockwatch: letzter Pruef-Frame, Voll-Hash */
    int relive; /* W118 REM-OVERLAY: neu hochladen (HD live an/aus), bleibt bis zum Erfolg */
} tex_entry_t;
unsigned krnl_phys_to_virt(unsigned phys);
void krnl_phys_diag(unsigned xptr, unsigned phys, unsigned need); /* DIAG(FILM-WHITE,W177) */
/* Unbekannte Adressen identity (Heap/VIRT), sonst Contig-Mapping. */
static unsigned sb_tex_hostaddr(unsigned data)
{
    unsigned v;
    if (!data) { return 0; }
    v = krnl_phys_to_virt(data);
    return v ? v : data;
}
/* FIX(WORLDVIEW,W65): XPR-Texturen mit Daten im SELBEN Speicherblock wie
 * der Header (Inline-XPR: Data = Header-Adresse Low28 + Offset). Das Spiel
 * liefert Data als "Phys" = VA & 0x0FFFFFFF; krnl_phys_to_virt sucht den
 * ersten Contig-Block mit passenden Low28-Bits und kann einen FREMDEN
 * Block treffen (z. B. RT-Daten bei 0x2C29xxxx fuer Heap 0x0C29xxxx) ->
 * Upload liest Nullen (Hangar schwarz, Boden weiss/schwarz, laufabhaengig
 * je nach Allokationsadressen; Beleg MONTP W65_p1/p2: Header 0x0CC1000C
 * gueltig, Data-Bytes 0). Hier: Kandidat = (Header & 0xF0000000) | Data,
 * nur wenn er im selben VirtualAlloc-Block wie der Header liegt
 * (AllocationBase gleich) und Data <= 16 MB hinter dem Header. Sonst alter
 * Pfad (sb_tex_hostaddr). Kein Ergebnis-Cache (mehrere Threads; nur bei
 * Upload/Refresh/Lock aufgerufen, nicht im Get-Fastpath). */
/* PERF(PERF-TICK,W168) tex_hash_fast: Ergebnis der VirtualQuery der Datenadresse fuer den
 * unmittelbar folgenden tex_data_hash desselben Threads (spart dort die Seiten-Walks). */
static __declspec(thread) MEMORY_BASIC_INFORMATION g_vqm;
static __declspec(thread) unsigned g_vqm_addr;
static unsigned sb_tex_hostaddr_x(unsigned xptr, unsigned data)
{
    unsigned xl, cand, res = 0, old;
    g_vqm_addr = 0;
    if (!data) { return 0; }
    if (!g_cfg.tex_inline_data || xptr < 0x10000u) { return sb_tex_hostaddr(data); }
    xl = xptr & 0x0FFFFFFFu;
    cand = (xptr & 0xF0000000u) | (data & 0x0FFFFFFFu);
    if (data > xl && data - xl < 0x01000000u) {
        MEMORY_BASIC_INFORMATION ma, mb;
        /* PERF(PERF-TICK,W168) tex_hash_fast: liegt der Kopf in derselben VirtualQuery-Region wie die
         * Daten (einheitlicher Zustand, eine Allokation), ist die zweite Abfrage gleichwertig entbehrlich. */
        if (g_cfg.tex_hash_fast && VirtualQuery((LPCVOID)(uintptr_t)cand, &mb, sizeof mb) && mb.State == MEM_COMMIT &&
            (uintptr_t)xptr >= (uintptr_t)mb.BaseAddress && (uintptr_t)xptr - (uintptr_t)mb.BaseAddress < mb.RegionSize) {
            res = cand;
            g_vqm = mb; g_vqm_addr = cand;
        } else
        if (VirtualQuery((LPCVOID)(uintptr_t)xptr, &ma, sizeof ma) &&
            VirtualQuery((LPCVOID)(uintptr_t)cand, &mb, sizeof mb) &&
            ma.State == MEM_COMMIT && mb.State == MEM_COMMIT &&
            ma.AllocationBase == mb.AllocationBase) {
            res = cand;
            if (g_cfg.tex_hash_fast) { g_vqm = mb; g_vqm_addr = cand; } /* PERF(PERF-TICK,W168) */
        }
    }
    old = sb_tex_hostaddr(data);
    if (!res) { res = old; }
    if (res != old) {
        static unsigned nlog;
        if (nlog < 24u) {
            nlog++;
            HLOG_I("TEX inline #%u: xptr=0x%X data=0x%X -> 0x%X (alt 0x%X)",
                   nlog, xptr, data, res, old);
        }
    }
    return res;
}
static tex_entry_t g_cache[TEX_CACHE_CAP];
static int g_ncache;
static unsigned short g_hint[TEX_HINT_N]; /* W63: xptr-Hash -> Slot+1 (Hinweis, wird geprueft) */
static unsigned g_tex_tick; /* W63: Get-Zaehler (LRU-Zeitstempel) */

/* W63: wirksame Kapazitaet (tex_cache_max, 16..TEX_CACHE_CAP; <=0 = 256 alt). */
static int tex_cache_max(void)
{
    int m = g_cfg.tex_cache_max;
    if (m <= 0) { return 256; }
    if (m < 16) { return 16; }
    return m > TEX_CACHE_CAP ? TEX_CACHE_CAP : m;
}

static unsigned tex_hint_slot(unsigned xptr)
{
    return ((xptr >> 4) ^ (xptr >> 16) ^ (xptr >> 24)) & (TEX_HINT_N - 1u);
}

/* W63: Cache-Eintrag zu xptr (Hash-Hinweis, sonst lineare Suche; Slots
 * wandern beim Swap-Remove, daher wird der Hinweis immer geprueft). */
static int tex_find(unsigned xptr)
{
    unsigned hs = tex_hint_slot(xptr);
    int i = (int)g_hint[hs] - 1;
    if (i >= 0 && i < g_ncache && g_cache[i].xptr == xptr) {
        return i;
    }
    for (i = 0; i < g_ncache; i++) {
        if (g_cache[i].xptr == xptr) {
            g_hint[hs] = (unsigned short)(i + 1);
            return i;
        }
    }
    return -1;
}

/* W63 TEXCACHE (tex_cache_lru=1): Cache voll -> am laengsten unbenutzten
 * Eintrag freigeben (SRV Release, Swap-Remove). Der Xbox-Texturkopf bleibt
 * gueltig (kein Release durch das Spiel), bei erneuter Nutzung wird neu
 * hochgeladen. Eintraege der letzten 64 Gets bleiben verschont. */
static int tex_evict_lru(void)
{
    int i, best = -1;
    unsigned bestage = 0;
    static unsigned nlru;
    for (i = 0; i < g_ncache; i++) {
        unsigned age = g_tex_tick - g_cache[i].lastuse;
        if (age > bestage) {
            bestage = age;
            best = i;
        }
    }
    if (best < 0 || bestage < 64u) {
        return 0;
    }
    if (nlru < 16 || (nlru & 1023u) == 0) {
        HLOG_I("TEX lru-evict #%u: xptr=0x%X %ux%u (unbenutzt seit %u Gets, Cache %d)",
               nlru + 1, g_cache[best].xptr, g_cache[best].w, g_cache[best].h,
               bestage, g_ncache);
    }
    nlru++;
    if (g_cache[best].srv) {
        ID3D11ShaderResourceView_Release(g_cache[best].srv);
    }
    g_ncache--;
    if (best != g_ncache) {
        g_cache[best] = g_cache[g_ncache];
    }
    memset(&g_cache[g_ncache], 0, sizeof g_cache[g_ncache]);
    return 1;
}
static int g_skiplog; /* unbekannte Formate: max 8 WARNs */
static unsigned g_up_fmt; static int g_up_lin; /* DIAG(MOVIE): letzter Upload */
/* DIAG(TEX3D,W31): Fehlgrund-Codes (nur Diagnose, kein Verhalten).
 * tex_upload: 1=stale-range 2=throttle 3=hdr-fault 4=fmt-decode 5=lin-dims
 * 6=lin-range 7=lin-create/srv 8=swz-range 9=swz-create/srv.
 * sb_tex_get: zusaetzlich 10=xptr0/nodev 11=cache-voll 12=cached-null. */
static unsigned g_up_fail;
static int g_tex_pause_up; /* W193 PAUSE-LIVE: Upload in der Pause (g_present_no steht) ohne Bild-Drossel */
static unsigned g_get_why;
static unsigned g_stage_xp[4]; /* letztes xptr je Stage */
static unsigned g_stage_n[4];  /* SetTexture-Calls je Stage */

void sb_tex_init(ID3D11Device *dev)
{
    g_tdev = dev;
    sb_rem_init(dev); /* W103: [remaster]/tex_dump (Standard aus: nichts) */
}

/* Xbox-Format-DWORD dekodieren (Cxbx X_D3DFORMAT_*). */
/* W38 (tex_swz06): Xbox-swizzled 32-Bit -> linear, nach dokumentiertem
 * NV2A-Morton-Swizzle (gierige Maskenvergabe, X zuerst;
 * vgl. analyse/bin0_tool.py swizzle_masks).
 * Adressbits werden gierig abwechselnd an X (zuerst) und Y vergeben
 * (mx/my); der Quelloffset ist die bitweise Verteilung (spread) von
 * x/y auf diese Masken. Fuer quadratische POT (alle bisher
 * beobachteten 0x06: 128x128) identisch zu Morton-Interleave;
 * Portrait-POT wird korrekt. */
static unsigned tex_swz_spread(unsigned v, unsigned mask)
{
    unsigned r = 0;
    while (mask) {
        unsigned lb = mask & (0u - mask); /* niedrigstes gesetztes Bit */
        if (v & 1u) { r |= lb; }
        v >>= 1;
        mask &= mask - 1u;
    }
    return r;
}
static void tex_unswizzle32(unsigned *dst, const unsigned *src, unsigned w, unsigned h)
{
    unsigned x, y, i, j, mx = 0, my = 0;
    j = 1; i = 1;
    while (i < w || i < h) {
        if (i < w) { mx |= j; j <<= 1; }
        if (i < h) { my |= j; j <<= 1; }
        i <<= 1;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            unsigned swz = tex_swz_spread(x, mx) | tex_swz_spread(y, my);
            dst[y * w + x] = src[swz];
        }
    }
}

static int tex_decode_fmt(unsigned fmt, DXGI_FORMAT *out, unsigned *blk,
                          unsigned *w, unsigned *h)
{
    unsigned code = (fmt & 0xFF00u) >> 8;
    unsigned usz = (fmt & 0xF00000u) >> 20;
    unsigned vsz = (fmt & 0xF000000u) >> 24;
    if (code == 0x12 || code == 0x1E || code == 0x1C) {
        /* M2b: LIN (Film/Titel). M2c: 0x1E=X8R8G8B8 (32b,opack), 0x1C=X1R5G5B5 (16b). */
        *out = (code == 0x1C) ? DXGI_FORMAT_B5G5R5A1_UNORM
               : (code == 0x1E) ? DXGI_FORMAT_B8G8R8X8_UNORM
                                : DXGI_FORMAT_B8G8R8A8_UNORM;
        *blk = (code == 0x1C) ? 2 : 4;
        *w = 640;
        *h = 480;
        return 2; /* linear */
    }
    if (usz == 0 || vsz == 0 || usz > 12 || vsz > 12) {
        return 0; /* linear ohne Groesse (TEX#7-Typ) */
    }
    *w = 1u << usz;
    *h = 1u << vsz;
    switch (code) {
    case 0x06: /* W38: swizzled A8R8G8B8 (MORTON) */
        if (!g_cfg.tex_swz06) { return 0; }
        *out = DXGI_FORMAT_B8G8R8A8_UNORM;
        *blk = 4;
        return 3;
    case 0x0C: /* DXT1 */
        *out = DXGI_FORMAT_BC1_UNORM;
        *blk = 8;
        return 1;
    case 0x0E: /* DXT3 */
        *out = DXGI_FORMAT_BC2_UNORM;
        *blk = 16;
        return 1;
    case 0x0F: /* DXT5 */
        *out = DXGI_FORMAT_BC3_UNORM;
        *blk = 16;
        return 1;
    default:
        return 0;
    }
}

/* Bits+Pitch einer Xbox-Textur (fuer LockRect-Hook): 1 = echt,
 * 0 = Fallback noetig. Liest Common/Data/Format, validiert den
 * Quellbereich wie tex_upload. */
static int tex_range_ok(unsigned data, unsigned size);
static int tex_lin_dims(unsigned xptr, unsigned data, unsigned *w,
                        unsigned *h, unsigned *pitch);
unsigned g_tex_last_data, g_tex_last_size; /* TEXDIAG */

/* D3DLOCK_READONLY (D3D8/Xbox): Lock ohne Schreibabsicht - Guard prueft nur
 * Schreib-Locks (Bink-Film u.a. schreiben ins Lock-Ziel). */
#define TEX_D3DLOCK_READONLY 0x10u
/* W86: Schreibbarkeit des Lock-Zielbereichs (VirtualQuery-Walk).
 * tex_range_ok prueft nur Lesbarkeit: ein zu kleiner Block direkt vor lesbaren
 * Seiten (z.B. DLL ab 0x30000000) besteht ihn, aber Schreib-Locks (Film-YUV)
 * laufen darueber hinaus -> AV 0x24E1D3. */
static int tex_range_writable(unsigned data, unsigned size)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *q = (unsigned char *)(uintptr_t)data;
    unsigned char *end = q + size;
    if (!data || !size || end < q || size > 0x1000000u) {
        return 0;
    }
    if (data < 0x10000u || data >= 0x80000000u) return 0;
    while (q < end) {
        if (!VirtualQuery(q, &mbi, sizeof mbi)) {
            return 0;
        }
        if (!(mbi.State & MEM_COMMIT) ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            !(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
            return 0;
        }
        q = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
    }
    return 1;
}
/* W86: Lock-Guard-Warnung (gedeckelt): Lock-Ziel nicht voll schreibbar
 * (fremder/zu kleiner Block) -> Fallback statt AV. */
static void lock_guard_note(unsigned xptr, unsigned data, unsigned need)
{
    static unsigned nlg;
    if (nlg < 8u) {
        nlg++;
        HLOG_W("TEX lock-guard #%u: xptr=0x%X data=0x%X need=0x%X nicht schreibbar -> Fallback",
               nlg, xptr, data, need);
    }
}
extern unsigned g_present_no; /* DIAG(W98): SBAR lock */
int sb_tex_lockbits(unsigned xptr, unsigned *pitch, unsigned *bits,
                    unsigned flags)
{
    unsigned data = 0, fmt = 0;
    DXGI_FORMAT dfmt;
    unsigned blk, w, h;
    int kind;
    if (g_cfg.sbar_diag > 1) { /* DIAG(STARTSEQ-BARS,W98): welche Texturen sperrt das Spiel? */
        static unsigned lk_seen[64], lk_cnt[64], lk_n, lk_calls;
        unsigned q;
        lk_calls++;
        for (q = 0; q < lk_n && lk_seen[q] != xptr; q++) {}
        if (q == lk_n && lk_n < 64u) { lk_seen[lk_n] = xptr; lk_cnt[lk_n] = 0; lk_n++; }
        if (q < lk_n) {
            lk_cnt[q]++;
            if ((lk_cnt[q] & (lk_cnt[q] - 1u)) == 0u || (lk_cnt[q] % 1000u) == 0u) {
                HLOG_W("SBAR lock #%u xptr=0x%X n=%u flags=0x%X f=%u", lk_calls, xptr, lk_cnt[q], flags, g_present_no);
            }
        }
    }
    __try {
        if (!xptr) {
            return 0;
        }
        data = *(unsigned *)(uintptr_t)(xptr + 4);
        fmt = *(unsigned *)(uintptr_t)(xptr + 12);
        data = sb_tex_hostaddr_x(xptr, data);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (!data) {
        return 0;
    }
    kind = tex_decode_fmt(fmt, &dfmt, &blk, &w, &h);
    if (kind == 1) {
        unsigned row = (w / 4) * blk;
        if (!tex_range_ok(data, row * (h / 4))) {
            return 0;
        }
        if (g_cfg.tex_lock_guard && !(flags & TEX_D3DLOCK_READONLY) &&
            !tex_range_writable(data, row * (h / 4))) {
            lock_guard_note(xptr, data, row * (h / 4));
            return 0;
        }
        *pitch = row;
    } else if (kind == 2) {
        unsigned lpitch = 4096;
        if (!tex_lin_dims(xptr, data, &w, &h, &lpitch)) {
            return 0;
        }
        if (!tex_range_ok(data, lpitch * h)) {
            if (g_cfg.contig_diag) { /* DIAG(FILM-WHITE,W177) */
                unsigned raw = 0;
                __try { raw = *(unsigned *)(uintptr_t)(xptr + 4); } __except (EXCEPTION_EXECUTE_HANDLER) { raw = 0; }
                krnl_phys_diag(xptr, raw, lpitch * h);
            }
            return 0;
        }
        if (g_cfg.tex_lock_guard && !(flags & TEX_D3DLOCK_READONLY) &&
            !tex_range_writable(data, lpitch * h)) {
            lock_guard_note(xptr, data, lpitch * h);
            return 0;
        }
        *pitch = lpitch;
    } else {
        return 0;
    }
    *bits = data;
    return 1;
}

/* Lesbarkeit des Upload-Quellbereichs pruefen (Seiten-walk). */
static int tex_range_ok(unsigned data, unsigned size)
{
    volatile unsigned char t;
    unsigned char *q = (unsigned char *)(uintptr_t)data;
    unsigned char *end = q + size;
    if (!data || !size || end < q || size > 0x1000000u) {
        return 0;
    }
    if (data < 0x10000u || data >= 0x80000000u) return 0; /* kernel/low raus */
    /* TEXLIFE (tex_upload_guard=1): Quelle VOLLSTAENDIG per VirtualQuery
     * pruefen. Stichproben (unten) uebersehen entfernte Seiten mitten im
     * Puffer -> Treiber-AV beim Upload aus freigegebenem Speicher. */
    if (g_cfg.tex_upload_guard) {
        MEMORY_BASIC_INFORMATION mbi;
        unsigned char *p = q;
        g_tex_last_data = data; g_tex_last_size = size;
        while (p < end) {
            if (!VirtualQuery(p, &mbi, sizeof mbi)) {
                return 0;
            }
            if (!(mbi.State & MEM_COMMIT) ||
                (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                return 0;
            }
            p = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        }
        return 1;
    }
    /* STALLFIX (boot502): bad-Seiten cachen (alle Aufrufer! SEH-Dispatch
     * kostet us * Millionen!). */
    { static unsigned bpg[256]; static unsigned nbg, nrc; unsigned s, pg;
      pg = data & ~0xFFFu;
      for (s = 0; s < nbg && s < 256; s++) if (bpg[s] == pg) return 0;
      if (((++nrc) & 0xFFFFFu) == 0) nbg = 0;
      g_tex_last_data = data; g_tex_last_size = size;
      __try {
          t = q[0]; t = q[size - 1];
          if (size > 0x1000u) { t = q[size / 2]; t = q[size - 2]; }
      } __except (EXCEPTION_EXECUTE_HANDLER) {
          if (nbg < 256) bpg[nbg++] = pg;
          return 0;
      } }
    (void)t;
    return 1;
}

/* Wie viele Bytes ab data sind committed? (TEX-skip-Diagnose). */
static unsigned tex_committed(unsigned data, unsigned size)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *q = (unsigned char *)(uintptr_t)data;
    unsigned char *end = q + size;
    unsigned got = 0;
    if (!data || !size || end < q) {
        return 0;
    }
    while (q < end) {
        unsigned char *rend;
        if (!VirtualQuery(q, &mbi, sizeof mbi)) {
            break;
        }
        if (!(mbi.State & MEM_COMMIT) ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            break;
        }
        rend = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        if (rend > end) {
            rend = end;
        }
        got += (unsigned)(rend - q);
        q = rend;
    }
    return got;
}

static unsigned tisqrt(unsigned x)
{
    unsigned r = 0;
    while ((r + 1) * (r + 1) <= x) {
        r++;
    }
    return r;
}

/* M2c: LIN-Dimensionen. 1 = bekannt (legacy/derived), 0 = Skip.
 * c0=0xC0002: Titel-Klasse (640x480@4096, bewiesen). c0=0x40002:
 * tight-Klasse (Pitch=w*4): h=480-Fall via got/480 (Lade-BG: 640x480@2560,
 * got=1228800 bewiesen), sonst Quadrat via sqrt(got/4); Rest -> Skip. */
static int tex_lin_dims(unsigned xptr, unsigned data, unsigned *w,
                        unsigned *h, unsigned *pitch)
{
    unsigned c0 = 0, bpp = 4, size = 0;
    static unsigned nderiv, nsize;
    *w = 640;
    *h = 480;
    *pitch = 4096;
    __try {
        unsigned fmt = *(unsigned *)(uintptr_t)(xptr + 12);
        c0 = *(unsigned *)(uintptr_t)xptr;
        size = *(unsigned *)(uintptr_t)(xptr + 16);
        if (((fmt & 0xFF00u) >> 8) == 0x1Cu) { bpp = 2; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (c0 == 0xC0002u) {
        return 1; /* Legacy (Titel, bewiesen) */
    }
    /* TEXSIZE (31.10.): Linear-Texturen tragen ihre Masse im Xbox-Size-
     * Feld (+0x10): W-1 Bits 0-11, H-1 Bits 12-23, Pitch/64-1 Bits 24-31
     * (z. B. 0x271DF27F = 640x480@2560). Deterministisch statt der
     * layoutabhaengigen VirtualQuery-Ableitung unten (retry1: "nosquare"
     * -> LockRect-Fallback auf g_lock_zero). */
    if (g_cfg.tex_size_header && size) {
        unsigned sw = (size & 0xFFFu) + 1u;
        unsigned sh = ((size >> 12) & 0xFFFu) + 1u;
        unsigned sp = (((size >> 24) & 0xFFu) + 1u) * 64u;
        if (sp >= sw * bpp) {
            *w = sw;
            *h = sh;
            *pitch = sp;
            {   /* pro xptr einmal loggen (Movie-Textur kommt spaet) */
                static unsigned seen[64];
                unsigned k;
                for (k = 0; k < nsize && seen[k] != xptr; k++) {}
                if (k == nsize && nsize < 64) {
                    seen[nsize++] = xptr;
                    HLOG_I("TEX lin-size #%u: xptr=0x%X %ux%u pitch=%u (size=0x%08X)",
                           nsize, xptr, sw, sh, sp, size);
                }
            }
            return 1;
        }
    }
    /* M2c: alle anderen (0x40002 tight, 0x1040001 frisch, ...) ableiten. */
    {
        unsigned got = tex_committed(data, 4096u * 480u);
        if (got > 0 && got % 480u == 0 && (got / 480u) % bpp == 0 &&
            got / 480u >= bpp && got / 480u <= 4096u) {
            *pitch = got / 480u;
            *w = *pitch / bpp;
            *h = 480u;
        } else if (got % bpp == 0 && got >= bpp * 8u * 8u) {
            unsigned s = tisqrt(got / bpp);
            if (s * s != got / bpp || s > 1024u) {
                if (nderiv < 24) {
                    nderiv++;
                    HLOG_W("TEX lin-noderive #%u: xptr=0x%X c0=0x%X size=0x%08X got=%u bpp=%u (nosquare)",
                           nderiv, xptr, c0, size, got, bpp);
                }
                return 0;
            }
            *pitch = s * bpp;
            *w = s;
            *h = s;
        } else {
            if (nderiv < 24) {
                nderiv++;
                HLOG_W("TEX lin-noderive #%u: xptr=0x%X c0=0x%X size=0x%08X got=%u bpp=%u (shape)",
                       nderiv, xptr, c0, size, got, bpp);
            }
            return 0;
        }
        if (nderiv < 12) {
            nderiv++;
            HLOG_I("TEX lin-derived #%u: xptr=0x%X %ux%u pitch=%u (got=%u)",
                   nderiv, xptr, *w, *h, *pitch, got);
        }
        return 1;
    }
}



static ID3D11ShaderResourceView *tex_upload(unsigned xptr,
                                                            unsigned *pw,
                                                            unsigned *ph,
                                                            unsigned *ps)
{
    /* STALLFIX (boot484): Glyphen-Sturm throttlen (max 12 Uploads/Frame).
     * Rest -> NULL (Refresh holt spaeter nach). */
    extern unsigned g_present_no;
    extern unsigned krnl_memreg_find(unsigned, unsigned *, unsigned *);
    static unsigned up_frame, up_count, up_skip, up_logf;
    static unsigned last_ok; /* boot500: zuletzt gueltige xptr (fast immer!) */
    g_up_fmt = 0; g_up_lin = 0; /* DIAG(MOVIE) */
    if (pw) { *pw = 0; }
    if (ph) { *ph = 0; }
    if (ps) { *ps = 0; }
    /* STALLFIX (boot500): stale xptr OHNE Probe-Fault erkennen (SEH-Dispatch
     * kostet us * Millionen!). last_ok=Fastpath, sonst Range+memreg. */
    if (xptr != last_ok) {
        int ok = 0;
        if (xptr >= 0x10000u && xptr < 0x80000000u) {
            if (xptr < 0x10000000u) { ok = 1; } /* Xbox-Mask (Heap!) */
            else if (krnl_memreg_find(xptr, NULL, NULL)) { ok = 1; }
        }
        if (!ok) { g_up_fail = 1; return NULL; }
        last_ok = xptr;
    }
    if (g_present_no != up_frame) { up_frame = g_present_no; up_count = 0; }
    if (up_count >= 12u && !g_tex_pause_up) {
        up_skip++;
        if (g_present_no - up_logf >= 600u) {
            up_logf = g_present_no;
            HLOG_W("TEX throttle: frame=%u skip_total=%u", g_present_no, up_skip);
        }
        g_up_fail = 2;
        return NULL;
    }
    up_count++;
    { extern unsigned g_phud_upn; g_phud_upn++; } /* DIAG(PERF-HUD,W122): Uploads zaehlen */
    unsigned data = 0, fmt = 0;
    DXGI_FORMAT dfmt;
    unsigned blk, w, h;
    D3D11_TEXTURE2D_DESC dd;
    D3D11_SUBRESOURCE_DATA init;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd;
    ID3D11Texture2D *tex = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    HRESULT hr;
    __try {
        data = *(unsigned *)(uintptr_t)(xptr + 4);
        fmt = *(unsigned *)(uintptr_t)(xptr + 0xC);
        /* M2c: PHYS->VIRT (Capcom speichert VIRT&mask). */
        data = sb_tex_hostaddr_x(xptr, data);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_up_fail = 3;
        return NULL;
    }
    {
        int linear = 0;
        if (data) {
            linear = tex_decode_fmt(fmt, &dfmt, &blk, &w, &h);
        }
        if (linear == 2) {
            /* M2b: Linear-Upload. M2c: Dims/Pitch abgeleitet. */
            unsigned lpitch = 4096;
            if (!tex_lin_dims(xptr, data, &w, &h, &lpitch)) {
                g_up_fail = 5;
                return NULL;
            }
            memset(&dd, 0, sizeof dd);
            dd.Width = w;
            dd.Height = h;
            dd.MipLevels = 1;
            dd.ArraySize = 1;
            dd.Format = dfmt;
            dd.SampleDesc.Count = 1;
            dd.Usage = D3D11_USAGE_DEFAULT;
            dd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            init.pSysMem = (const void *)(uintptr_t)data;
            init.SysMemPitch = lpitch;
            init.SysMemSlicePitch = lpitch * h;
            if (!tex_range_ok(data, lpitch * h)) {
                unsigned c0 = 0, lk = 0;
                __try {
                    c0 = *(unsigned *)(uintptr_t)xptr;
                    lk = *(unsigned *)(uintptr_t)(xptr + 8);
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
                { 
                    static unsigned nlinsk;
                    if (nlinsk < 8) {
                        nlinsk++;
                        HLOG_W("TEX lin-skip #%u (0x%X data=0x%X c0=0x%X need=%u got=%u)",
                               nlinsk, xptr, data, c0, 4096 * h,
                               tex_committed(data, 4096 * h));
                    }
                }
                if (g_cfg.contig_diag) { /* DIAG(FILM-WHITE,W177) */
                    unsigned raw = 0;
                    __try { raw = *(unsigned *)(uintptr_t)(xptr + 4); } __except (EXCEPTION_EXECUTE_HANDLER) { raw = 0; }
                    krnl_phys_diag(xptr, raw, lpitch * h);
                }
                g_up_fail = 6;
                return NULL;
            }
            hr = ID3D11Device_CreateTexture2D(g_tdev, &dd, &init,
                                             &tex);
            if (FAILED(hr) || !tex) {
                HLOG_W("TEX lin-upload fehlgeschlagen (0x%X)",
                       xptr);
                g_up_fail = 7;
                return NULL;
            }
            memset(&vd, 0, sizeof vd);
            vd.Format = dfmt;
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MipLevels = 1;
            hr = ID3D11Device_CreateShaderResourceView(g_tdev,
                (ID3D11Resource *)tex, &vd, &srv);
            ID3D11Texture2D_Release(tex);
            if (FAILED(hr) || !srv) {
                HLOG_W("TEX lin-srv fehlgeschlagen (0x%X)",
                       xptr);
                g_up_fail = 7;
                return NULL;
            }
            if (sb_rem_active()) { /* W103 REM-FRAME: Dump/Ersatz (Standard aus) */
                srv = sb_rem_upload(xptr, fmt, 2, w, h, (const void *)(uintptr_t)data, lpitch, 1, srv);
            }
            HLOG_I("TEX lin-upload: xptr=0x%X %ux%u", xptr, w,
                   h);
            g_up_fmt = fmt; g_up_lin = 1; /* DIAG(MOVIE) */
            if (pw) { *pw = w; }
            if (ph) { *ph = h; }
            if (ps) { *ps = lpitch * h; }
            g_up_fail = 0;
            return srv;
        }
    }
    int kind3 = data ? tex_decode_fmt(fmt, &dfmt, &blk, &w, &h) : 0;
    unsigned *swzlin = NULL; /* W38: ent-swizzelter Puffer (Kind 3) */
    if (!kind3) {
        if (g_skiplog < 8) {
            g_skiplog++;
            HLOG_W("TEX skip: xptr=0x%X fmt=0x%08X data=0x%X", xptr,
                   fmt, data);
        }
        g_up_fail = 4; g_up_fmt = fmt; /* DIAG(TEX3D): Format merken */
        return NULL;
    }
    if (kind3 == 3) { /* W38: 0x06 MORTON -> linear (Range zuerst, Quelle nie direkt samplen) */
        if (w == 0 || h == 0 || w > 4096 || h > 4096 || w * h > (65536u * 1024u / 4u) || !tex_range_ok(data, w * h * 4u)) {
            g_up_fail = 8;
            return NULL;
        }
        swzlin = (unsigned *)malloc(w * h * 4u);
        if (!swzlin) { g_up_fail = 9; return NULL; }
        tex_unswizzle32(swzlin, (const unsigned *)(uintptr_t)data, w, h);
        if (g_cfg.tex_swz06 > 1) { /* W38: Stufe 2 = Dump je 0x06-xptr (W50: + Alpha-Statistik TEXDUMP6A) */
            static unsigned dd06_seen[8]; static unsigned dd06_n; unsigned ddq, ddfound_W50 = 0;
            for (ddq = 0; ddq < dd06_n; ddq++) { if (dd06_seen[ddq] == xptr) { ddfound_W50 = 1; break; } }
            if (!ddfound_W50 && dd06_n < 8 && w <= 2048 && h <= 2048) {
                unsigned char *bgr = (unsigned char *)malloc(w * h * 3u);
                dd06_seen[dd06_n++] = xptr;
                if (bgr) {
                    unsigned i, first = swzlin[0];
                    unsigned amin = 255, amax = 0, asum = 0;
                    unsigned ahist[16] = { 0 };
                    char path[260];
                    for (i = 0; i < w * h; i++) { bgr[3*i] = (unsigned char)(swzlin[i] & 0xFFu); bgr[3*i+1] = (unsigned char)((swzlin[i] >> 8) & 0xFFu); bgr[3*i+2] = (unsigned char)((swzlin[i] >> 16) & 0xFFu); }
                    _snprintf_s(path, sizeof path, _TRUNCATE, "%s/shots/texsrc_SWZ06_%08X.jpg", hcfg_out_dir(), xptr);
                    sb_render_write_jpg(path, bgr, w, h);
                    free(bgr);
                    for (i = 0; i < w * h; i++) { unsigned av = (swzlin[i] >> 24) & 0xFFu; if (av < amin) { amin = av; } if (av > amax) { amax = av; } asum += av; ahist[av >> 4]++; }
                    HLOG_W("TEXDUMP6: xptr=0x%X %ux%u first=0x%08X -> %s", xptr, w, h, first, path);
                    HLOG_W("TEXDUMP6A: xptr=0x%X alpha min=%u max=%u mean=%.1f hist16=[%u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u]",
                           xptr, amin, amax, (double)asum / (double)(w * h),
                           ahist[0], ahist[1], ahist[2], ahist[3], ahist[4], ahist[5], ahist[6], ahist[7],
                           ahist[8], ahist[9], ahist[10], ahist[11], ahist[12], ahist[13], ahist[14], ahist[15]);
                }
            }
        }
    }
    memset(&dd, 0, sizeof dd);
    dd.Width = w;
    dd.Height = h;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = dfmt;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    init.pSysMem = (kind3 == 3) ? (const void *)swzlin : (const void *)(uintptr_t)data;
    init.SysMemPitch = (kind3 == 3) ? w * 4u : (w / 4) * blk;
    init.SysMemSlicePitch = (kind3 == 3) ? init.SysMemPitch * h : init.SysMemPitch * (h / 4);
    /* Upload-Versuch (Orakel: tex_compare RX_TRY-Metadaten). */
    {
        static unsigned ntry;
        if (ntry < 24) {
            ntry++;
            HLOG_I("TEX try #%u: xptr=0x%X data=0x%X fmt=0x%08X %ux%u (%u B)",
                   ntry, xptr, data, fmt, w, h,
                   init.SysMemSlicePitch);
        }
    }
    if (!tex_range_ok(data, init.SysMemSlicePitch)) {
        if (g_skiplog < 8) {
            unsigned c0 = 0;
            g_skiplog++;
            __try { c0 = *(unsigned *)(uintptr_t)xptr; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            HLOG_W("TEX skip: xptr=0x%X data=0x%X c0=0x%X need=%u got=%u",
                   xptr, data, c0, init.SysMemSlicePitch,
                   tex_committed(data, init.SysMemSlicePitch));
        }
        g_up_fail = 8;
        if (swzlin) { free(swzlin); }
        return NULL;
    }
    /* FIX(TEXMIP,W84): Mip-Kette mitladen. Xbox-Format-Bits 16..19 =
     * MIPMAP_LEVELS (z. B. 0x099A0C2A = DXT1 512x512, 10 Stufen); die Stufen
     * liegen dicht hinter Stufe 0 (DXT: Bloecke je Stufe, min. 1 Block;
     * 0x06: jede Stufe einzeln swizzled, w*h*4). Ohne Mips wird der
     * verkleinerte Boden unterabgetastet (Moire-"Marmor"). Fehlt der
     * Speicher fuer die ganze Kette -> nur Stufe 0 wie bisher. */
    {
        unsigned nlev = (fmt >> 16) & 0xFu, maxlev = 1, t = (w > h) ? w : h, lv, total = 0;
        D3D11_SUBRESOURCE_DATA mi[13];
        unsigned *mbuf[13];
        while (t > 1u) { t >>= 1; maxlev++; }
        if (nlev > maxlev) { nlev = maxlev; }
        if (nlev > 13u) { nlev = 13u; }
        memset(mbuf, 0, sizeof mbuf);
        if (g_cfg.tex_mips && nlev > 1u) {
            for (lv = 0; lv < nlev; lv++) {
                unsigned lw = (w >> lv) ? (w >> lv) : 1u, lh = (h >> lv) ? (h >> lv) : 1u;
                total += (kind3 == 3) ? lw * lh * 4u : ((lw + 3u) / 4u) * ((lh + 3u) / 4u) * blk;
            }
            if (!tex_range_ok(data, total)) { nlev = 1; }
        } else {
            nlev = 1;
        }
        if (nlev > 1u) {
            unsigned off = 0;
            int ok = 1;
            for (lv = 0; lv < nlev; lv++) {
                unsigned lw = (w >> lv) ? (w >> lv) : 1u, lh = (h >> lv) ? (h >> lv) : 1u;
                if (kind3 == 3) {
                    if (lv == 0) { mi[0] = init; }
                    else {
                        mbuf[lv] = (unsigned *)malloc(lw * lh * 4u);
                        if (!mbuf[lv]) { ok = 0; break; }
                        tex_unswizzle32(mbuf[lv], (const unsigned *)(uintptr_t)(data + off), lw, lh);
                        mi[lv].pSysMem = mbuf[lv];
                        mi[lv].SysMemPitch = lw * 4u;
                        mi[lv].SysMemSlicePitch = lw * lh * 4u;
                    }
                    off += lw * lh * 4u;
                } else {
                    unsigned bw = (lw + 3u) / 4u, bh = (lh + 3u) / 4u;
                    mi[lv].pSysMem = (const void *)(uintptr_t)(data + off);
                    mi[lv].SysMemPitch = bw * blk;
                    mi[lv].SysMemSlicePitch = bw * bh * blk;
                    off += bw * bh * blk;
                }
            }
            if (ok) {
                dd.MipLevels = nlev;
                __try {
                    hr = ID3D11Device_CreateTexture2D(g_tdev, &dd, mi, &tex);
                } __except (EXCEPTION_EXECUTE_HANDLER) { hr = E_FAIL; tex = NULL; }
                if (FAILED(hr) || !tex) { tex = NULL; dd.MipLevels = 1; }
            }
            for (lv = 0; lv < 13u; lv++) { if (mbuf[lv]) { free(mbuf[lv]); } }
            if (tex) {
                static unsigned nmip;
                if (nmip < 6u) { nmip++; HLOG_I("TEX mips #%u: xptr=0x%X %ux%u fmt=0x%08X Stufen=%u (%u B)", nmip, xptr, w, h, fmt, nlev, total); }
            }
        }
    }
    if (!tex) { hr = ID3D11Device_CreateTexture2D(g_tdev, &dd, &init, &tex); }
    if (FAILED(hr) || !tex) {
        HLOG_W("TEX upload fehlgeschlagen (0x%X %ux%u)", xptr, w, h);
        g_up_fail = 9;
        if (swzlin) { free(swzlin); }
        return NULL;
    }
    /* W38: DEFAULT-Usage kopiert bei Create; swzlin wird erst nach der SRV
     * freigegeben (W103: Inhalts-Hash braucht die ent-swizzelten Texel). */
    memset(&vd, 0, sizeof vd);
    vd.Format = dfmt;
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipLevels = dd.MipLevels; /* FIX(TEXMIP,W84): 1 ohne tex_mips */
    hr = ID3D11Device_CreateShaderResourceView(g_tdev,
                                               (ID3D11Resource *)tex,
                                               &vd, &srv);
    ID3D11Texture2D_Release(tex);
    if (FAILED(hr) || !srv) {
        HLOG_W("TEX srv fehlgeschlagen (0x%X)", xptr);
        g_up_fail = 9;
        if (swzlin) { free(swzlin); }
        return NULL;
    }
    if (sb_rem_active()) { /* W103 REM-FRAME: Dump/Ersatz (Standard aus) */
        srv = sb_rem_upload(xptr, fmt, kind3 == 3 ? 3 : 1, w, h,
                            (kind3 == 3) ? (const void *)swzlin : (const void *)(uintptr_t)data,
                            init.SysMemPitch, dd.MipLevels, srv);
    }
    if (swzlin) { free(swzlin); swzlin = NULL; }
    HLOG_I("TEX upload: xptr=0x%X %ux%u fmt=0x%X (%u B)", xptr, w, h,
           fmt & 0xFF00u, init.SysMemSlicePitch);
    g_up_fmt = fmt; g_up_lin = 0; /* DIAG(MOVIE) */
    if (pw) { *pw = w; }
    if (ph) { *ph = h; }
    if (ps) { *ps = init.SysMemSlicePitch; }
    g_up_fail = 0;
    return srv;
}

void sb_tex_set(unsigned stage, unsigned xptr)
{
    if (stage == 0) {
        g_stage0_xptr = xptr;
    }
    /* DIAG(TEX3D): Belegung aller Stages (Spiel nutzt auch Stage 2!). */
    if (stage < 4u) {
        g_stage_xp[stage] = xptr;
        g_stage_n[stage]++;
    }
}

/* M2c: FNV-1a ueber Kopf+Schwanz (Streaming-Erkennung, billig).
 * 0 = Fehler/leer (kein Refresh). */
/* PERF(PERF-TICK,W168) tex_hash_fast: Region der Datenadresse (aus sb_tex_hostaddr_x oder einer
 * eigenen VirtualQuery) deckt [a, a+len) ab und ist lesbar -> gleiche Antwort wie tex_range_ok
 * (eine VirtualQuery-Region hat einheitlichen Zustand/Schutz), sonst der bisherige Weg. */
static int tex_range_ok_m(const MEMORY_BASIC_INFORMATION *m, unsigned a, unsigned len)
{
    uintptr_t b, e;
    if (m) {
        b = (uintptr_t)m->BaseAddress;
        e = b + m->RegionSize;
        if ((uintptr_t)a >= b && (uintptr_t)a + len <= e && len && (m->State & MEM_COMMIT) &&
            !(m->Protect & (PAGE_NOACCESS | PAGE_GUARD)) && a >= 0x10000u && a < 0x80000000u) {
            g_tex_last_data = a; g_tex_last_size = len;
            return 1;
        }
    }
    return tex_range_ok(a, len);
}

/* PERF(PERF-TICK,W168) tex_hash_fast: dieselben Bytes wie FNV-1a (Kopf, Schwanz, Stichproben),
 * aber 32-Bit-Woerter in vier unabhaengigen Straengen (xor + Multiplikation mit ungerader Zahl ist
 * je Strang umkehrbar -> jede einzelne Aenderung aendert das Ergebnis, wie bei FNV). */
#define THF_M 0x9E3779B1u
static __inline void thf_block(unsigned l[4], const unsigned char *p, unsigned n)
{
    unsigned i = 0;
    for (; i + 16u <= n; i += 16u) {
        l[0] = (l[0] ^ *(const unsigned *)(p + i)) * THF_M;
        l[1] = (l[1] ^ *(const unsigned *)(p + i + 4u)) * THF_M;
        l[2] = (l[2] ^ *(const unsigned *)(p + i + 8u)) * THF_M;
        l[3] = (l[3] ^ *(const unsigned *)(p + i + 12u)) * THF_M;
    }
    for (; i < n; i++) { l[0] = (l[0] ^ p[i]) * 16777619u; }
}
static unsigned tex_data_hash_fast(unsigned data, unsigned size, const MEMORY_BASIC_INFORMATION *m)
{
    unsigned l[4] = {2166136261u, 0x85EBCA6Bu, 0xC2B2AE35u, 0x27D4EB2Fu}, h, n;
    const unsigned char *p;
    if (!tex_range_ok_m(m, data, (size < 4096u) ? size : 4096u)) {
        return 0;
    }
    if (size > 4096u && !tex_range_ok_m(m, data + size - 4096u, 4096u)) {
        return 0;
    }
    __try {
        p = (const unsigned char *)(uintptr_t)data;
        n = (size < 4096u) ? size : 4096u;
        thf_block(l, p, n);
        if (size > 4096u) {
            thf_block(l, (const unsigned char *)(uintptr_t)(data + size - 4096u), 4096u);
        }
        if (size > 16384u) {
            unsigned k, step = (size - 64u) / 32u;
            for (k = 1; k < 32u; k++) {
                thf_block(l, (const unsigned char *)(uintptr_t)(data + k * step), 64u);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    h = l[0];
    h = (h ^ l[1]) * THF_M;
    h = (h ^ l[2]) * THF_M;
    h = (h ^ l[3]) * THF_M;
    h ^= h >> 15;
    return h ? h : 1u;
}

static unsigned tex_data_hash(unsigned data, unsigned size)
{
    extern unsigned krnl_memreg_find(unsigned, unsigned *, unsigned *);
    unsigned h = 2166136261u, i, n;
    const unsigned char *p;
    const MEMORY_BASIC_INFORMATION *vm = NULL;
    MEMORY_BASIC_INFORMATION vm2;
    if (g_cfg.tex_hash_fast) { /* PERF(PERF-TICK,W168): Region aus sb_tex_hostaddr_x bzw. eine eigene Abfrage */
        if (g_vqm_addr && data == g_vqm_addr) { vm = &g_vqm; }
        else if (data >= 0x10000u && data < 0x80000000u && g_cfg.tex_upload_guard &&
                 VirtualQuery((LPCVOID)(uintptr_t)data, &vm2, sizeof vm2)) { vm = &vm2; }
        g_vqm_addr = 0;
    }
    if (!data || !size) {
        return 0;
    }
    { static unsigned bad[256]; static unsigned nbad; static unsigned nc; unsigned b;
      if (((++nc) & 0xFFFFFu) == 0) nbad = 0; /* selten leeren (Addr-Reuse!) */
      for (b = 0; b < nbad && b < 256; b++) if (bad[b] == data) return 0;
      if (data < 0x10000u || data >= 0x80000000u ||
          (data >= 0x10000000u && !krnl_memreg_find(data, NULL, NULL))) {
          if (nbad < 256) bad[nbad++] = data;
          return 0;
      }
      if (!(vm && g_cfg.tex_upload_guard ? tex_range_ok_m(vm, data, 1u) : tex_range_ok(data, 1u))) {
          if (nbad < 256) bad[nbad++] = data; else { unsigned q; for (q = 1; q < 256; q++) bad[q-1] = bad[q]; bad[255] = data; }
          return 0;
      } }
    if (g_cfg.tex_hash_fast) { return tex_data_hash_fast(data, size, g_cfg.tex_upload_guard ? vm : NULL); } /* PERF(PERF-TICK,W168) */
    /* M2c: nie faulten (VEH-Spam!) - Lesbarkeit zuerst pruefen. */
    if (!tex_range_ok(data, (size < 4096u) ? size : 4096u)) {
        return 0;
    }
    if (size > 4096u &&
        !tex_range_ok(data + size - 4096u, 4096u)) {
        return 0;
    }
    __try {
        p = (const unsigned char *)(uintptr_t)data;
        n = (size < 4096u) ? size : 4096u;
        for (i = 0; i < n; i++) {
            h ^= p[i];
            h *= 16777619u;
        }
        if (size > 4096u) {
            p = (const unsigned char *)(uintptr_t)(data + size - 4096u);
            for (i = 0; i < 4096u; i++) {
                h ^= p[i];
                h *= 16777619u;
            }
        }
        /* TEXDIRTY: 32 Stichproben a 64 B ueber den ganzen Puffer (Movie mit
         * schwarzen Balken aendert Kopf/Schwanz nie). Seiten sind per
         * tex_range_ok-Walk bei Upload geprueft; SEH faengt den Rest. */
        if (size > 16384u) {
            unsigned k, step = (size - 64u) / 32u;
            for (k = 1; k < 32u; k++) {
                p = (const unsigned char *)(uintptr_t)(data + k * step);
                for (i = 0; i < 64u; i++) {
                    h ^= p[i];
                    h *= 16777619u;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return h ? h : 1u;
}

/* DIAG(MOVIE): beim 120. Refresh einer Textur deren Quelldaten als JPG
 * nach <out>/shots/texsrc_<xptr>.jpg + Pixelstatistik (schreibt Bink echte
 * Bilder oder bleibt der Speicher leer/weiss?). Nur 32-bit-Linear. */
static void tex_dump_diag(unsigned xptr, unsigned data, unsigned w, unsigned h)
{
    static unsigned xs[64], cnt[64], done[64];
    static unsigned nx;
    unsigned k, pitch = 0, fmt = 0, size = 0;
    data = sb_tex_hostaddr_x(xptr, data);
    __try {
        fmt = *(unsigned *)(uintptr_t)(xptr + 12);
        size = *(unsigned *)(uintptr_t)(xptr + 16);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!size) return; /* nur Linear-Texturen mit Size-Feld (Movie) */
    for (k = 0; k < nx && xs[k] != xptr; k++) {}
    if (k == nx) {
        if (nx >= 64) return;
        xs[nx++] = xptr;
    }
    if (done[k] || ++cnt[k] != 120u) return;
    done[k] = 1;
    if (((fmt & 0xFF00u) >> 8) == 0x1Cu || !size || !w || !h || w > 2048u || h > 2048u) {
        return;
    }
    pitch = (((size >> 24) & 0xFFu) + 1u) * 64u;
    {
        unsigned char *bgr = (unsigned char *)malloc(w * h * 3u);
        unsigned x, y, first = 0, distinct = 0, sumr = 0, n = 0;
        unsigned seen[64];
        char path[600];
        extern int sb_render_write_jpg(const char *, const unsigned char *,
                                       unsigned, unsigned);
        extern const char *hcfg_out_dir(void);
        if (!bgr) return;
        __try {
            for (y = 0; y < h; y++) {
                const unsigned char *s = (const unsigned char *)(uintptr_t)(data + y * pitch);
                for (x = 0; x < w; x++) {
                    unsigned px = *(const unsigned *)(s + x * 4u);
                    bgr[(y * w + x) * 3 + 0] = s[x * 4 + 0];
                    bgr[(y * w + x) * 3 + 1] = s[x * 4 + 1];
                    bgr[(y * w + x) * 3 + 2] = s[x * 4 + 2];
                    if (((x | y) & 15u) == 0) {
                        unsigned q;
                        if (!n) first = px;
                        n++;
                        sumr += s[x * 4 + 2];
                        for (q = 0; q < distinct && seen[q] != px; q++) {}
                        if (q == distinct && distinct < 64) seen[distinct++] = px;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            free(bgr);
            HLOG_W("TEXDUMP: xptr=0x%X data=0x%X Lesefehler", xptr, data);
            return;
        }
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s/shots/texsrc_%08X.jpg",
                    hcfg_out_dir(), xptr);
        sb_render_write_jpg(path, bgr, w, h);
        free(bgr);
        HLOG_W("TEXDUMP: xptr=0x%X %ux%u pitch=%u first=0x%08X distinct=%u%s avgR=%u -> %s",
               xptr, w, h, pitch, first, distinct, distinct >= 64 ? "+" : "",
               n ? sumr / n : 0, path);
    }
}

/* FIX(STARTSEQ-BARS,W98) tex_lockwatch: Texturen, die das Spiel per
 * D3DTexture_LockRect gesperrt hat, beschreibt es oft spaeter ueber den
 * behaltenen Zeiger weiter (Xbox: Unlock ist ein No-Op). Beispiel: der
 * Instrumenten-Atlas wird nur beim Missionsladen gesperrt (FUN_00093210,
 * LOCKED_RECT in 0x55D910); die Startsequenz schreibt danach je Frame die
 * Balken-Fuellung ins Texel-Alpha (FUN_00092710 ueber [0x51D8B0]). TEXDIRTY
 * greift nur beim Lock, der Stichproben-Hash trifft die Zeilen nicht ->
 * Anzeige hing hinterher. Fuer gesperrte Texturen daher einmal je Frame ein
 * Voll-Hash ueber den ganzen Puffer (wortweise, ~50 us je 256 KB). */
static unsigned g_lw_xptr[64];
static int g_lw_n;
extern unsigned g_present_no;
void sb_tex_lockwatch_note(unsigned xptr)
{
    int k;
    if (!g_cfg.tex_lockwatch || !xptr) { return; }
    for (k = 0; k < g_lw_n; k++) { if (g_lw_xptr[k] == xptr) { return; } }
    if (g_lw_n < 64) {
        g_lw_xptr[g_lw_n++] = xptr;
        if (g_lw_n <= 32) { HLOG_I("TEX lockwatch #%d: xptr=0x%X", g_lw_n, xptr); }
    }
}
void sb_tex_lockwatch_forget(unsigned xptr)
{
    int k;
    for (k = 0; k < g_lw_n; k++) {
        if (g_lw_xptr[k] == xptr) { g_lw_xptr[k] = g_lw_xptr[--g_lw_n]; return; }
    }
}
static int lw_has(unsigned xptr)
{
    int k;
    for (k = 0; k < g_lw_n; k++) { if (g_lw_xptr[k] == xptr) { return 1; } }
    return 0;
}
static unsigned tex_full_hash(unsigned data, unsigned size)
{
    unsigned h = 2166136261u, i, n;
    if (!data || size < 4u || size > (16u << 20) || !tex_range_ok(data, size)) { return 0; }
    n = size / 4u;
    __try {
        const unsigned *p = (const unsigned *)(uintptr_t)data;
        for (i = 0; i < n; i++) { h = (h ^ p[i]) * 16777619u; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return h ? h : 1u;
}

/* DIAG(PERF-HUD,W122): Zeit in sb_tex_get (Cache, Dirty-Hash, Upload, HD-Ersatz)
 * nur bei g_phud_meas; Pfad unveraendert. */
long long g_phud_texq;
unsigned g_phud_texn;
static ID3D11ShaderResourceView *sb_tex_get_body(void);
ID3D11ShaderResourceView *sb_tex_get(void)
{
    extern volatile int g_phud_meas;
    LARGE_INTEGER a, b;
    ID3D11ShaderResourceView *r;
    if (!g_phud_meas) { return sb_tex_get_body(); }
    QueryPerformanceCounter(&a);
    r = sb_tex_get_body();
    QueryPerformanceCounter(&b);
    g_phud_texq += b.QuadPart - a.QuadPart;
    g_phud_texn++;
    return r;
}
static ID3D11ShaderResourceView *sb_tex_get_body(void)
{
    int i;
    ID3D11ShaderResourceView *srv;
    static unsigned nget;
    if (!g_stage0_xptr || !g_tdev) {
        g_get_why = 10;
        return NULL;
    }
    /* RTT (W23): Offscreen-gefuellte Textur direkt sampeln (rtt=1);
     * Match-DIAG laeuft auch bei rtt=0. */
    {
        ID3D11ShaderResourceView *rsrv = NULL;
        unsigned rw = 0, rh = 0;
        if (sb_rtt_lookup(g_stage0_xptr, &rsrv, &rw, &rh) && rsrv) {
            g_get_why = 0;
            return rsrv;
        }
    }
    nget++;
    g_tex_tick++;
    i = tex_find(g_stage0_xptr);
    if (i >= 0) {
        {
            tex_entry_t *e = &g_cache[i];
            unsigned data = 0;
            e->lastuse = g_tex_tick;
            if (g_stage0_xptr < 0x10000u || g_stage0_xptr >= 0x80000000u)
                return e->srv; /* stale xptr (kein Fault!) */
            __try {
                data = *(unsigned *)(uintptr_t)(g_stage0_xptr + 4);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                data = 0;
            }
            /* TEXDIRTY (31.10.): per LockRect beschrieben -> sofort neu laden
             * (Bink-Movie: 1 Lock pro Video-Frame; vorher nie aufgefrischt,
             * Movie blieb weiss). */
            if (e->dirty && data) {
                e->dirty = 0;
                goto reupload;
            }
            if (e->relive && data) { /* W118: HD live umgeschaltet (nur nach sb_tex_relive_all) */
                goto reupload;
            }
            if (g_cfg.tex_lockwatch && data && e->srv && e->size && e->lw_frame != g_present_no &&
                data == e->data && lw_has(e->xptr)) {
                unsigned h = tex_full_hash(sb_tex_hostaddr_x(e->xptr, data), e->size);
                e->lw_frame = g_present_no;
                if (h && e->lw_hash && h != e->lw_hash) {
                    static unsigned nlw;
                    if (nlw < 8u) { nlw++; HLOG_I("TEX lockwatch-refresh #%u: xptr=0x%X f=%u", nlw, e->xptr, g_present_no); }
                    goto reupload;
                }
                if (h) { e->lw_hash = h; }
            }
            if (g_cfg.tex_hdr_check && data) {
                /* W63 tex_hdr_check: (a) Refresh-Takt je Eintrag statt global
                 * - der globale Zaehler nget&15 traf bei festem Draw-Muster
                 * manche Texturen nie, deren Inhalt an derselben xptr nach
                 * Missionsende neu belegt wurde (W63_c10: Debriefing-Schrift
                 * zeigte Gelaende-Kacheln; W63_c11/c12 mit Schalter lesbar,
                 * per Hash-Refresh). (b) Texturkopf neu belegt (Data/Format
                 * weichen ab) -> sofort neu laden statt alte SRV liefern. */
                unsigned fmt = 0;
                __try {
                    fmt = *(unsigned *)(uintptr_t)(g_stage0_xptr + 0xC);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    fmt = e->hfmt;
                }
                if (data != e->data || fmt != e->hfmt) {
                    static unsigned nhc;
                    if (nhc < 16) {
                        nhc++;
                        HLOG_I("TEX hdr-neu #%u: xptr=0x%X data 0x%X->0x%X fmt 0x%X->0x%X",
                               nhc, g_stage0_xptr, e->data, data, e->hfmt, fmt);
                    }
                    goto reupload;
                }
            }
            if (!data || ((g_cfg.tex_hdr_check ? ++e->gets : nget) & 15u) != 0) {
                g_get_why = e->srv ? 0u : 12u;
                return e->srv; /* Fast path (kann NULL sein) */
            }
            /* M2c: Voll-Refresh jedes 16. Get (Streaming!). TEXDIRTY: Hash
             * ueber die VIRT-Adresse wie tex_upload (vorher roh -> falscher
             * Speicher -> Hash konstant -> nie Refresh). */
            if (e->srv && data == e->data && e->size) {
                unsigned h = tex_data_hash(sb_tex_hostaddr_x(e->xptr, data), e->size);
                if (!h || h == e->hash) {
                    g_get_why = 0;
                    return e->srv; /* stabil */
                }
            }
reupload:
            {
                unsigned tw = 0, th = 0, ts = 0;
                static unsigned nref;
                { int cr = sb_ctx_release_all(); srv = tex_upload(g_stage0_xptr, &tw, &th, &ts); sb_ctx_reacquire(cr); } /* W138: Upload ohne ctx_lock (Device) */
                if (srv) {
                    if (e->srv) {
                        ID3D11ShaderResourceView_Release(e->srv);
                    }
                    e->srv = srv;
                    e->w = tw;
                    e->h = th;
                    e->size = ts;
                    e->fmt = g_up_fmt; e->linear = g_up_lin;
                    e->data = data;
                    __try {
                        e->hfmt = *(unsigned *)(uintptr_t)(g_stage0_xptr + 0xC);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                    }
                    {
                        e->hash = tex_data_hash(sb_tex_hostaddr_x(e->xptr, data), ts);
                    }
                    /* W98 tex_lockwatch: Stand dieses Uploads merken (sonst
                     * ginge ein Schreiben bis zur naechsten Pruefung verloren). */
                    e->lw_hash = (g_cfg.tex_lockwatch && lw_has(e->xptr)) ?
                                     tex_full_hash(sb_tex_hostaddr_x(e->xptr, data), ts) : 0u;
                    e->lw_frame = g_present_no;
                    e->relive = 0; /* W118 */
                    if (nref < 32) {
                        nref++;
                        HLOG_I("TEX refresh #%u: xptr=0x%X %ux%u",
                               nref, e->xptr, tw, th);
                    }
                    tex_dump_diag(e->xptr, data, tw, th);
                } else if (e->relive && g_up_fail != 2u) {
                    e->relive = 0; /* W118: nur die Drossel wiederholt, echte Fehler nicht */
                }
                g_get_why = e->srv ? 0u : (g_up_fail ? g_up_fail : 12u);
                return e->srv;
            }
        }
    }
    if (g_ncache >= tex_cache_max() &&
        !(g_cfg.tex_cache_lru && tex_evict_lru())) {
        static unsigned nfull;
        if (nfull < 8 || (nfull & 0xFFFFu) == 0) {
            HLOG_W("TEX cache voll! (%d Eintraege, #%u)", g_ncache, nfull + 1);
        }
        nfull++;
        g_get_why = 11;
        return NULL;
    }
    {
        unsigned tw = 0, th = 0, ts = 0, data = 0;
        { int cr = sb_ctx_release_all(); srv = tex_upload(g_stage0_xptr, &tw, &th, &ts); sb_ctx_reacquire(cr); } /* W138: Upload ohne ctx_lock (Device) */
        __try {
            data = *(unsigned *)(uintptr_t)(g_stage0_xptr + 4);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        g_cache[g_ncache].xptr = g_stage0_xptr;
        g_cache[g_ncache].srv = srv;
        g_cache[g_ncache].w = tw;
        g_cache[g_ncache].h = th;
        g_cache[g_ncache].size = ts;
        g_cache[g_ncache].data = data;
        {
            g_cache[g_ncache].hash = tex_data_hash(sb_tex_hostaddr_x(g_stage0_xptr, data), ts);
        }
        g_cache[g_ncache].dirty = 0;
        g_cache[g_ncache].relive = 0; /* W118 */
        g_cache[g_ncache].fmt = g_up_fmt;
        g_cache[g_ncache].linear = g_up_lin;
        g_cache[g_ncache].lastuse = g_tex_tick;
        g_cache[g_ncache].gets = 0;
        __try {
            g_cache[g_ncache].hfmt = *(unsigned *)(uintptr_t)(g_stage0_xptr + 0xC);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_cache[g_ncache].hfmt = 0;
        }
        g_hint[tex_hint_slot(g_stage0_xptr)] = (unsigned short)(g_ncache + 1);
    }
    g_ncache++;
    g_get_why = srv ? 0u : (g_up_fail ? g_up_fail : 12u);
    return srv;
}

unsigned sb_tex_xptr(void)
{
    return g_stage0_xptr;
}
/* DIAG(TEX3D,W31): letzter sb_tex_get-NULL-Grund (0 = SRV ok),
 * Stage-Belegung, RT-Treffer. Nur Diagnose. */
unsigned sb_tex_why(void)
{
    return g_get_why;
}
void sb_tex_stages(unsigned xp[4], unsigned n[4])
{
    unsigned i;
    for (i = 0; i < 4u; i++) {
        if (xp) { xp[i] = g_stage_xp[i]; }
        if (n) { n[i] = g_stage_n[i]; }
    }
}
int sb_tex_rtt_hit(void)
{
    unsigned rw = 0, rh = 0;
    return sb_rtt_lookup(g_stage0_xptr, NULL, &rw, &rh) ? 1 : 0;
}
void sb_tex_dims(unsigned *w, unsigned *h)
{
    int i;
    /* RTT (W23): Offscreen-Dims (Notiz, auch bei rtt=0). */
    {
        unsigned rw = 0, rh = 0;
        if (sb_rtt_lookup(g_stage0_xptr, NULL, &rw, &rh)) {
            *w = rw; *h = rh;
            return;
        }
    }
    *w = 0;
    *h = 0;
    if (!g_stage0_xptr) {
        return;
    }
    i = tex_find(g_stage0_xptr);
    if (i >= 0) {
        *w = g_cache[i].w;
        *h = g_cache[i].h;
    }
}

void sb_tex_fmt(unsigned *fmt, int *is_linear)
{
    int i;
    /* RTT (W23): Offscreen = linear 32-bit (Notiz, auch bei rtt=0). */
    {
        unsigned rw = 0, rh = 0;
        if (sb_rtt_lookup(g_stage0_xptr, NULL, &rw, &rh)) {
            *fmt = 0x00011229u; *is_linear = 1;
            return;
        }
    }
    *fmt = 0;
    *is_linear = 0;
    if (!g_stage0_xptr) {
        return;
    }
    i = tex_find(g_stage0_xptr);
    if (i >= 0) {
        *fmt = g_cache[i].fmt;
        *is_linear = g_cache[i].linear;
    }
}

/* W118 REM-OVERLAY: alle Eintraege beim naechsten Get neu hochladen (HD an/
 * aus bzw. Stufe live gewechselt; sb_rem_upload waehlt neu). Anders als
 * dirty bleibt relive bis zum gelungenen Upload stehen (Upload-Drossel 12
 * je Bild verteilt die Arbeit auf mehrere Bilder). Render-Thread. */
void sb_tex_relive_all(void)
{
    int i;
    for (i = 0; i < g_ncache; i++) {
        if (g_cache[i].srv) { g_cache[i].relive = 1; }
    }
    HLOG_I("TEX relive: %d Eintraege neu hochladen (f=%u)", g_ncache, g_present_no);
}

/* W193 PAUSE-LIVE: In der Pause zeichnet kein Spiel-Draw, also holt auch kein sb_tex_get die
 * relive-Eintraege (HD live an/aus) nach. Die Pause-Neuzeichnung laedt sie hier selbst neu
 * (hoechstens max je Aufruf, Spiel-Thread) und merkt alt -> neu: die Aufzeichnung haelt die
 * alten SRVs (AddRef), die Wiedergabe tauscht sie per sb_tex_pause_map. Rueckgabe: offene
 * Eintraege. sb_tex_pause_clear am Pausenende. */
#define TEX_PRL_MAX 1024
static ID3D11ShaderResourceView *g_prl_o[TEX_PRL_MAX], *g_prl_n[TEX_PRL_MAX];
static int g_nprl;
static unsigned g_prl_up, g_prl_fail;

int sb_tex_pause_relive(int max, unsigned *nup, unsigned *nfail)
{
    int i, done = 0, left = 0;
    for (i = 0; i < g_ncache; i++) {
        tex_entry_t *e = &g_cache[i];
        ID3D11ShaderResourceView *srv;
        unsigned tw = 0, th = 0, ts = 0, data = 0, q;
        if (!e->relive || !e->srv) { continue; }
        if (done >= max || g_nprl >= TEX_PRL_MAX) { left++; continue; }
        __try { data = *(unsigned *)(uintptr_t)(e->xptr + 4); }
        __except (EXCEPTION_EXECUTE_HANDLER) { data = 0; }
        if (!data) { e->relive = 0; g_prl_fail++; continue; }
        done++;
        g_tex_pause_up = 1;
        srv = tex_upload(e->xptr, &tw, &th, &ts);
        g_tex_pause_up = 0;
        if (!srv) {
            e->relive = 0; /* bleibt beim alten Stand (wie sb_tex_get ohne Drossel) */
            g_prl_fail++;
            continue;
        }
        for (q = 0; q < (unsigned)g_nprl; q++) { /* Kette alt -> neu -> neuer */
            if (g_prl_n[q] == e->srv) { break; }
        }
        if (q < (unsigned)g_nprl) {
            ID3D11ShaderResourceView_Release(g_prl_n[q]);
            g_prl_n[q] = srv;
            ID3D11ShaderResourceView_AddRef(srv);
            ID3D11ShaderResourceView_Release(e->srv);
        } else {
            g_prl_o[g_nprl] = e->srv; /* Referenz des Caches wandert in die Tabelle */
            g_prl_n[g_nprl] = srv;
            ID3D11ShaderResourceView_AddRef(srv);
            g_nprl++;
        }
        e->srv = srv;
        e->w = tw;
        e->h = th;
        e->size = ts;
        e->fmt = g_up_fmt;
        e->linear = g_up_lin;
        e->data = data;
        __try { e->hfmt = *(unsigned *)(uintptr_t)(e->xptr + 0xC); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        e->hash = tex_data_hash(sb_tex_hostaddr_x(e->xptr, data), ts);
        e->lw_hash = 0;
        e->lw_frame = g_present_no;
        e->relive = 0;
        g_prl_up++;
    }
    if (nup) { *nup = g_prl_up; }
    if (nfail) { *nfail = g_prl_fail; }
    return left;
}

ID3D11ShaderResourceView *sb_tex_pause_map(ID3D11ShaderResourceView *s)
{
    int q;
    if (!s || !g_nprl) { return s; }
    for (q = 0; q < g_nprl; q++) {
        if (g_prl_o[q] == s) { return g_prl_n[q]; }
    }
    return s;
}

int sb_tex_pause_nmap(void)
{
    return g_nprl;
}

void sb_tex_pause_clear(void)
{
    int q;
    for (q = 0; q < g_nprl; q++) {
        if (g_prl_o[q]) { ID3D11ShaderResourceView_Release(g_prl_o[q]); }
        if (g_prl_n[q]) { ID3D11ShaderResourceView_Release(g_prl_n[q]); }
        g_prl_o[q] = g_prl_n[q] = NULL;
    }
    if (g_nprl || g_prl_up || g_prl_fail) {
        HLOG_I("PAUSE live: Texturen in der Pause neu geladen %u (fehlgeschlagen %u, Tausch-Eintraege %d)", g_prl_up,
               g_prl_fail, g_nprl);
    }
    g_nprl = 0;
    g_prl_up = g_prl_fail = 0;
}

void sb_tex_mark_dirty(unsigned xptr)
{
    int i = tex_find(xptr);
    if (i >= 0) {
        g_cache[i].dirty = 1;
    }
}

/* TEXLIFE: Cache-Eintrag zu xptr entfernen (statt dirty markieren).
 * Release bei Refcount 0 -> das Spiel gibt den Texturspeicher frei; ein
 * dirty-Eintrag wuerde beim naechsten Get aus freigegebenem Speicher neu
 * hochladen (Treiber-AV in nvwgf2um.dll). SRV freigeben, Slot per
 * Swap-Remove loeschen (xptr-Reuse legt danach einen frischen Eintrag an).
 * Wird nur aus hook_D3DResource_Release aufgerufen (tex_evict=1). */
void sb_tex_evict(unsigned xptr)
{
    int i;
    static unsigned nev;
    if (!xptr) {
        return;
    }
    i = tex_find(xptr);
    if (i >= 0) {
        {
            if (g_cache[i].srv) {
                ID3D11ShaderResourceView_Release(g_cache[i].srv);
            }
            g_ncache--;
            if (i != g_ncache) {
                g_cache[i] = g_cache[g_ncache];
            }
            memset(&g_cache[g_ncache], 0, sizeof g_cache[g_ncache]);
            if (nev < 12) {
                nev++;
                HLOG_I("TEX evict #%u: xptr=0x%X (Release rc=0)", nev, xptr);
            }
            return;
        }
    }
}

/* W2B-FIX: Linear-Dims direkt aus Header (ohne Cache; Fallback wenn der
 * Cache zur Norm-Zeit leer ist). data = rohes Data-Feld (wird hier P->V). */
int sb_tex_hdr_dims(unsigned xptr, unsigned *w, unsigned *h)
{
    unsigned data = 0, pitch = 0;
    if (!xptr) { return 0; }
    __try { data = *(unsigned *)(uintptr_t)(xptr + 4); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    data = sb_tex_hostaddr_x(xptr, data);
    return tex_lin_dims(xptr, data, w, h, &pitch);
}

/* Texturkopf + Quellbytes-Sonde (MONITORVIEW). */
void sb_tex_hdr_info(unsigned xptr, unsigned *c0, unsigned *data,
                     unsigned *fmt, unsigned *size)
{
    unsigned t0 = 0, td = 0, tf = 0, ts = 0;
    if (xptr) {
        __try {
            t0 = *(unsigned *)(uintptr_t)xptr;
            td = *(unsigned *)(uintptr_t)(xptr + 4);
            tf = *(unsigned *)(uintptr_t)(xptr + 12);
            ts = *(unsigned *)(uintptr_t)(xptr + 16);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (c0) { *c0 = t0; }
    if (data) { *data = td; }
    if (fmt) { *fmt = tf; }
    if (size) { *size = ts; }
}


/* RENDER-QA (W77): Textur einer beliebigen Stufe (0..3) - gleicher Cache-/
 * RTT-Weg wie sb_tex_get, indem der Stufen-xptr kurz als Stufe 0 gilt.
 * Liefert SRV + Masse + Linear-Klasse (RT = linear). Nur im Shading-Pfad
 * benutzt (shading=0: nie aufgerufen). */
unsigned g_tex_stage_why; /* RENDER-QA DIAG: NULL-Grund der letzten Stufen-Abfrage */
struct ID3D11ShaderResourceView *sb_tex_get_stage(unsigned stage, unsigned *w, unsigned *h, int *lin)
{
    unsigned save = g_stage0_xptr, swhy = g_get_why, fmt = 0;
    ID3D11ShaderResourceView *srv;
    if (w) { *w = 0; }
    if (h) { *h = 0; }
    if (lin) { *lin = 0; }
    if (stage >= 4u) { return NULL; }
    /* FIX(SHADE-BLOOM,W112, shade_texskip=1): Xbox-Tiefenformate (D24S8/F24S8/
     * D16/F16 swizzled 0x2A..0x2D, linear 0x2E..0x31; Schattenpuffer der
     * Welt-PS auf Stufe 1, LIN_F16 0x31) sind als Farbtextur nicht ladbar
     * ("TEX skip", fmt-decode). Ueber sb_tex_get legte jeder Abruf einen
     * Cache-Eintrag ohne SRV an, jeder 16. Abruf versuchte erneut zu laden und
     * verbrauchte das Upload-Budget (12/Bild, tex_upload) - ~240 Fehl-
     * versuche je Bild (W112_s2: TEX throttle
     * skip_total 5,26 Mio.). Die Bodentextur kam nie an die Reihe -> T0 = 1,
     * Boden flach 0,68 -> Monitor-Komposition weiss. Ergebnis wie bisher NULL
     * (T = 1, bzw. Tiefe ueber sb_rtt_depth_srv bei shade_shadow=1), nur ohne
     * Ladeversuch. Nur Shading-Pfad (shading=0 ruft diese Funktion nie). */
    if (g_cfg.shade_texskip && g_stage_xp[stage]) {
        unsigned tf = 0;
        __try { tf = (*(volatile unsigned *)(uintptr_t)(g_stage_xp[stage] + 0xCu) >> 8) & 0xFFu; }
        __except (EXCEPTION_EXECUTE_HANDLER) { tf = 0; }
        if (tf >= 0x2Au && tf <= 0x31u) { g_tex_stage_why = 13u; return NULL; }
    }
    g_stage0_xptr = g_stage_xp[stage];
    srv = sb_tex_get();
    if (srv) {
        unsigned tw = 0, th = 0;
        int tl = 0;
        sb_tex_dims(&tw, &th);
        sb_tex_fmt(&fmt, &tl);
        if (w) { *w = tw; }
        if (h) { *h = th; }
        if (lin) { *lin = tl; }
    }
    g_stage0_xptr = save;
    g_tex_stage_why = g_get_why;
    g_get_why = swhy;
    return srv;
}
