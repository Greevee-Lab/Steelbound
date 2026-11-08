/* xbe_loader.c - XBE parsen + an Original-VAs mappen.
 * Struct-Offsets aus Cxbx (Xbe.h); nighttime-Verhalten aus xbe_info.py. */
#include "xbe_loader.h"
#include "xbox_types.h"
#include "hlog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define XOR_EP_RETAIL 0xA8FC57ABu
#define XOR_KT_RETAIL 0x5B6D40B6u
#define XOR_EP_DEBUG 0x94859D4Bu
#define XOR_KT_DEBUG 0xEFB1F152u

static uint32_t rd32(const uint8_t *p, size_t off)
{
    uint32_t v;
    memcpy(&v, p + off, 4);
    return v;
}

int xbe_read(const char *path, xbe_image_t *img)
{
    FILE *f;
    long len;
    uint8_t *buf;

    memset(img, 0, sizeof *img);
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        HLOG_E("xbe: %s nicht lesbar", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 64L * 1024 * 1024) { fclose(f); return -1; }
    buf = malloc((size_t)len);
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        HLOG_E("xbe: Lesen fehlgeschlagen");
        fclose(f);
        free(buf);
        return -1;
    }
    fclose(f);
    img->file = buf;
    img->file_size = (uint32_t)len;
    return 0;
}

int xbe_parse(const char *path, xbe_image_t *img)
{
    if (xbe_read(path, img) != 0) return -1;
    return xbe_parse_buf(img->file, img->file_size, path, img);
}

/* W150 ISO-DIRECT: XBE aus einem Puffer (Abbild per iso_vfs) parsen. Der
 * Puffer (malloc) gehoert danach img->file; path nur fuer das Log. */
int xbe_parse_buf(uint8_t *buf, uint32_t len, const char *path, xbe_image_t *img)
{
    uint32_t magic, kt_xor, entry_xor, xor_ep, xor_kt;
    uint32_t sec_off, lib_off;
    uint32_t i, max_end = 0;
    int is_debug;

    memset(img, 0, sizeof *img);
    img->file = buf;
    img->file_size = len;

    magic = rd32(img->file, 0);
    if (magic != 0x48454258u) { /* 'XBEH' */
        HLOG_E("xbe: kein XBEH-Magic (%08X)", magic);
        return -1;
    }
    img->base = rd32(img->file, 0x104);
    img->size_headers = rd32(img->file, 0x108);
    img->size_image = rd32(img->file, 0x10C);
    img->n_sections = rd32(img->file, 0x11C);
    entry_xor = rd32(img->file, 0x128);
    kt_xor = rd32(img->file, 0x158);

    is_debug = (kt_xor & 0x80000000u) != 0;
    xor_ep = is_debug ? XOR_EP_DEBUG : XOR_EP_RETAIL;
    xor_kt = is_debug ? XOR_KT_DEBUG : XOR_KT_RETAIL;
    img->entry = entry_xor ^ xor_ep;
    img->kt_addr = kt_xor ^ xor_kt;

    img->stack_commit = rd32(img->file, 0x130);
    img->heap_reserve = rd32(img->file, 0x134);
    img->heap_commit = rd32(img->file, 0x138);

    /* Sections */
    sec_off = rd32(img->file, 0x120) - img->base;
    img->sections = calloc(img->n_sections, sizeof(section_info_t));
    if (!img->sections) {
        return -1;
    }
    for (i = 0; i < img->n_sections; i++) {
        section_info_t *s = &img->sections[i];
        size_t o = sec_off + (size_t)i * 56;
        uint32_t name_va;
        s->flags = rd32(img->file, o);
        s->vaddr = rd32(img->file, o + 4);
        s->vsize = rd32(img->file, o + 8);
        s->rawaddr = rd32(img->file, o + 12);
        s->rawsize = rd32(img->file, o + 16);
        name_va = rd32(img->file, o + 20);
        if (name_va >= img->base && name_va - img->base + 10 < img->file_size) {
            memcpy(s->name, img->file + name_va - img->base, 10);
            s->name[10] = '\0';
        }
        if (s->vaddr + s->vsize > max_end) {
            max_end = s->vaddr + s->vsize;
        }
    }
    img->image_end = (max_end + 0xFFFFu) & ~0xFFFFu;

    /* TLS-Verzeichnis (VA->Datei ueber Sections, wie xbe_info.py) */
    {
        uint32_t tls = rd32(img->file, 0x12C);
        if (tls) {
            uint32_t off = 0;
            int found = 0;
            if (tls >= img->base && tls - img->base < img->size_headers) {
                off = tls - img->base;
                found = 1;
            } else {
                for (i = 0; i < img->n_sections; i++) {
                    section_info_t *s = &img->sections[i];
                    if (tls >= s->vaddr && tls < s->vaddr + s->vsize) {
                        off = s->rawaddr + (tls - s->vaddr);
                        found = 1;
                        break;
                    }
                }
            }
            if (found && off + 24 <= img->file_size) {
                img->tls_index_addr = rd32(img->file, off + 8);
                img->tls_zerofill = rd32(img->file, off + 16);
                HLOG_I("xbe: TLS va=0x%X index_addr=0x%X zerofill=%u", tls,
                       img->tls_index_addr, img->tls_zerofill);
            } else {
                HLOG_W("xbe: TLS va=0x%X nicht abbildbar", tls);
            }
        }
    }

    /* Lib-Versions loggen */
    {
        uint32_t nlib = rd32(img->file, 0x160);
        lib_off = rd32(img->file, 0x164) - img->base;
        for (i = 0; i < nlib; i++) {
            char name[9];
            uint16_t build;
            memcpy(name, img->file + lib_off + (size_t)i * 16, 8);
            name[8] = '\0';
            memcpy(&build, img->file + lib_off + (size_t)i * 16 + 12, 2);
            HLOG_I("xbe: lib %-8s build %u", name, build);
        }
    }

    HLOG_I("xbe: %s (%u Bytes), %s, entry=0x%X, kt=0x%X, %u sections, end=0x%X",
           path, img->file_size, is_debug ? "debug" : "retail", img->entry,
           img->kt_addr, img->n_sections, img->image_end);
    return 0;
}

int xbe_map(xbe_image_t *img)
{
    uint32_t i;
    DWORD old;

    /* Region gehoert dem Loader-Image (BSS): nur RWX protecten + Bytes kopieren.
     * (Kein VirtualAlloc - Image-Seiten gehoeren uns bereits.) */
    if (img->image_end > 0x2800000u) {
        HLOG_E("xbe: image_end 0x%X sprengt Loader-Region!", img->image_end);
        return -1;
    }
    if (!VirtualProtect((LPVOID)(uintptr_t)img->base, img->size_headers,
                        PAGE_EXECUTE_READWRITE, &old)) {
        HLOG_E("xbe: Header-Protect fehlgeschlagen (GLE=%lu)", GetLastError());
        return -1;
    }
    /* Loader-Header retten (CreateThread braucht gueltige PE-Header!) */
    img->loader_hdr_size = img->size_headers < 0x2000 ? img->size_headers : 0x2000;
    img->loader_hdr = malloc(img->loader_hdr_size);
    if (img->loader_hdr) {
        memcpy(img->loader_hdr, (void *)(uintptr_t)img->base, img->loader_hdr_size);
    }
    memcpy((void *)(uintptr_t)img->base, img->file, img->size_headers);
    /* Sections */
    for (i = 0; i < img->n_sections; i++) {
        section_info_t *s = &img->sections[i];
        void *mem;
        if (s->vsize == 0) {
            continue;
        }
        if (!VirtualProtect((LPVOID)(uintptr_t)s->vaddr, s->vsize,
                            PAGE_EXECUTE_READWRITE, &old)) {
            HLOG_E("xbe: section %-10s Protect fehlgeschlagen (GLE=%lu)", s->name,
                   GetLastError());
            return -1;
        }
        mem = (void *)(uintptr_t)s->vaddr;
        if (s->rawsize && s->rawaddr + s->rawsize <= img->file_size) {
            memcpy(mem, img->file + s->rawaddr, s->rawsize);
        }
        if (s->vsize > s->rawsize) {
            memset((uint8_t *)mem + s->rawsize, 0, s->vsize - s->rawsize);
        }
        HLOG_T("xbe: map %-10s va=0x%08X vsz=0x%X raw=0x%X flags=0x%X", s->name,
               s->vaddr, s->vsize, s->rawsize, s->flags);
    }
    HLOG_I("xbe: gemappt (M1: alles RWX)");
    return 0;
}

void xbe_free_file(xbe_image_t *img)
{
    free(img->file);
    img->file = NULL;
}

static uint8_t *g_hdr_shadow;
static uint32_t g_hdr_shadow_size;

void xbe_swap_headers(xbe_image_t *img, int to_loader)
{
    uint32_t n;
    if (!img->loader_hdr) {
        return;
    }
    n = img->loader_hdr_size;
    if (!g_hdr_shadow) {
        g_hdr_shadow_size = n;
        g_hdr_shadow = malloc(n);
        if (!g_hdr_shadow) {
            return;
        }
    }
    if (to_loader) {
        memcpy(g_hdr_shadow, (void *)(uintptr_t)img->base, n); /* Spiel-Stand */
        memcpy((void *)(uintptr_t)img->base, img->loader_hdr, n); /* Loader-PE */
    } else {
        memcpy((void *)(uintptr_t)img->base, g_hdr_shadow, n); /* Spiel-Stand */
    }
}

static volatile LONG g_hdr_hybrid_on;

int xbe_hdr_hybrid_active(void)
{
    return g_hdr_hybrid_on != 0;
}

static DWORD WINAPI hdr_probe_thread(LPVOID p)
{
    (void)p;
    return 0x5B94u;
}

int xbe_hdr_hybrid(xbe_image_t *img)
{
    static const uint8_t pe_sig[4] = { 'P', 'E', 0, 0 };
    const uint8_t *lh = img->loader_hdr;
    uint8_t *b = (uint8_t *)(uintptr_t)img->base;
    uint32_t lfanew, nt_size, off, i, min_va = 0xFFFFFFFFu;
    uint16_t nsec, opt_size, dllch;
    uint8_t save_mz[2], save_lf[4];
    HANDLE th;
    DWORD ec = 0, gle = 0;

    if (!lh || img->loader_hdr_size < 0x40u) {
        HLOG_W("HDRSWAP hybrid: keine Loader-Header gesichert -> Tausch bleibt");
        return -1;
    }
    lfanew = rd32(lh, 0x3C);
    if (lh[0] != 'M' || lh[1] != 'Z' || lfanew + 24u > img->loader_hdr_size ||
        memcmp(lh + lfanew, pe_sig, 4) != 0) {
        HLOG_W("HDRSWAP hybrid: Loader-PE ungueltig (lfanew=0x%X) -> Tausch bleibt",
               lfanew);
        return -2;
    }
    memcpy(&nsec, lh + lfanew + 6, 2);
    memcpy(&opt_size, lh + lfanew + 20, 2);
    nt_size = 4u + 20u + opt_size + 40u * nsec;
    if (lfanew + nt_size > img->loader_hdr_size || opt_size < 72u) {
        HLOG_W("HDRSWAP hybrid: PE-Header %u B passt nicht in die Sicherung -> Tausch bleibt",
               nt_size);
        return -3;
    }
    /* Platz: ans Ende der ersten 0x2000 Byte, hinter dem XBE-Header und
     * vor der ersten XBE-Section (die Seite gehoert zum Header, das Spiel
     * liest nur bis size_headers). */
    for (i = 0; i < img->n_sections; i++) {
        if (img->sections[i].vsize && img->sections[i].vaddr < min_va) {
            min_va = img->sections[i].vaddr;
        }
    }
    off = (0x2000u - nt_size) & ~7u;
    if (off < ((img->size_headers + 7u) & ~7u) || img->base + 0x2000u > min_va) {
        HLOG_W("HDRSWAP hybrid: kein Platz (size_headers=0x%X nt=0x%X erste Section=0x%X) -> Tausch bleibt",
               img->size_headers, nt_size, min_va);
        return -4;
    }
    memcpy(b + off, lh + lfanew, nt_size);
    /* NO_SEH (0x400) loeschen: mit XBE-Header gelten Spiel-SEH-Handler im
     * Image als gueltig (RtlIsValidHandler findet keine PE-Header). Der
     * Loader traegt NO_SEH -> im alten Tausch-Fenster wurde jede Spiel-
     * Exception zu c00001a5 (WER-Dumps 3.11.). Ohne NO_SEH = wie XBE. */
    memcpy(&dllch, b + off + 24u + 70u, 2);
    dllch = (uint16_t)(dllch & ~0x0400u);
    memcpy(b + off + 24u + 70u, &dllch, 2);
    memcpy(save_mz, b, 2);
    memcpy(save_lf, b + 0x3C, 4);
    b[0] = 'M';
    b[1] = 'Z';
    memcpy(b + 0x3C, &off, 4);
    /* Gegenprobe: CreateThread ohne Tausch muss gehen. */
    th = CreateThread(NULL, 0, hdr_probe_thread, NULL, 0, NULL);
    if (!th) {
        gle = GetLastError();
    } else {
        WaitForSingleObject(th, 5000);
        GetExitCodeThread(th, &ec);
        CloseHandle(th);
    }
    if (!th || ec != 0x5B94u) {
        memcpy(b, save_mz, 2);
        memcpy(b + 0x3C, save_lf, 4);
        memset(b + off, 0, nt_size);
        HLOG_W("HDRSWAP hybrid: Probe-CreateThread fehlgeschlagen (GLE=%lu ec=0x%lX) -> zurueck, Tausch bleibt",
               gle, ec);
        return -5;
    }
    InterlockedExchange(&g_hdr_hybrid_on, 1);
    HLOG_I("HDRSWAP hybrid: aktiv (MZ + e_lfanew=0x%X, PE-Kopie %u B bis 0x%X, XBE-Header 0x%X B, "
           "ersetzt XBE-Bytes %02X%02X@0 %02X%02X%02X%02X@0x3C, DllChar=0x%X)",
           off, nt_size, img->base + off + nt_size, img->size_headers,
           save_mz[0], save_mz[1], save_lf[0], save_lf[1], save_lf[2], save_lf[3],
           (unsigned)dllch);
    return 0;
}

uint32_t xbe_entry(const xbe_image_t *img)
{
    return img->entry;
}

uint32_t xbe_kernel_thunk(const xbe_image_t *img)
{
    return img->kt_addr;
}
