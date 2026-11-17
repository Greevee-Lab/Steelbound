/* sb_texhealth.c - W315 TEX-WHITE-AMD: Textur-Gesundheit und tex_missing_mark.
 *
 * Tester (AMD) melden weisse bzw. graue Flaechen: Draws, die eine Textur
 * erwarten, aber keine SRV haben (W307: im Shading-Pfad T = 1 -> weiss, sonst
 * Vertexfarbe). Damit kuenftige Berichte die Ursache selbst nennen:
 *
 * 1. Zaehler (immer, Kosten ~0: Inkremente an Stellen, die ohnehin selten bzw.
 *    nur im Fehlerfall laufen; je Present ein Vergleich):
 *    - D3D lehnt Anlegen ab (CreateTexture2D/CreateShaderResourceView): Anzahl,
 *      erste 8 verschiedene mit HRESULT, DXGI-Format, Groesse, Xbox-Format;
 *    - Upload-Drossel: Ausfaelle gesamt, Bilder mit Ausfall, Hoechststand je Bild;
 *    - TEX dauer-ohne-srv (W307);
 *    - Draws mit belegter Stufe ohne SRV: gesamt je Grund, Hoechstwert je Bild;
 *    - Geraet: Adapter, Feature-Level, DriverCommandLists, render_thread wirksam,
 *      WARP; VRAM-Budget/-Nutzung (DXGI QueryVideoMemoryInfo, alle 1800 Presents
 *      und bei F11; im Absturz-/Haenger-Pfad nur der letzte Stand);
 *    - HD-Pack-Statistik (sb_remaster.c).
 *    Ausgabe als Block "[texture health]" in F11-Bericht (info.txt), Absturz-/
 *    Haengerbericht (info.txt) und als TEXHEALTH-Zeilen im Log beim Beenden
 *    (damit auch im Launcher-Diagnosepaket). Nur Technikdaten, keine Eingaben,
 *    keine Pfade.
 * 2. [render] tex_missing_mark=1 (DIAG, Standard 0 = bitgleich): statt NULL
 *    eine 1x1-Markierungstextur je Grund. Die Farbe wird wie jede Textur mit der
 *    Vertexfarbe bzw. im Combiner verrechnet:
 *      gelb    (255,255,0)  noch nicht hochgeladen: Drossel (why 2) bzw. Eintrag
 *                           wartet auf den Neuversuch (why 12)
 *      cyan    (0,255,255)  D3D hat Anlegen abgelehnt (why 7/9)
 *      magenta (255,0,255)  sonst (Format 4, linear 5/6, Bereich 8, Kopf 1/3, ...)
 *
 * ASCII only. */
#define COBJMACROS
#include "render_int.h"
#include "sb_texhealth.h"
#include <dxgi1_4.h>

#define TH_NFAIL 8

typedef struct {
    int what;
    long hr;
    unsigned dxgi, w, h, xfmt, xptr;
} th_fail_t;

static volatile LONG g_th_fail_n;
static th_fail_t g_th_fail[TH_NFAIL];
static int g_th_fail_k;
static unsigned g_th_thr_total, g_th_thr_frames, g_th_thr_max, g_th_thr_max_f, g_th_thr_cur, g_th_thr_curf;
static unsigned g_th_dauer;
static unsigned g_th_miss_total, g_th_miss_why[16], g_th_miss_max, g_th_miss_max_f, g_th_miss_cur, g_th_miss_curf;
static unsigned g_th_miss_frames;
static ID3D11ShaderResourceView *g_th_mark[3];
static int g_th_mark_tried[3];

static char g_th_adapter[160];
static unsigned g_th_vendor, g_th_fl;
static int g_th_dcl = -1, g_th_warp, g_th_rt_cfg = -1;
static unsigned long long g_th_budget, g_th_usage, g_th_usage_max, g_th_nl_budget, g_th_nl_usage;
static int g_th_vram_ok = -1; /* -1 = nicht abgefragt, 0 = nicht verfuegbar, 1 = ok */
static unsigned g_th_vram_last, g_th_vram_n;
static IDXGIAdapter3 *g_th_ad3;

extern void sb_rem_hd_stats(char *o, size_t cap); /* sb_remaster.c (W315) */
extern unsigned sb_tex_thr_pending(unsigned *entries); /* sb_tex.c (W315) */

void sb_th_create_fail(int what, long hr, unsigned dxgi, unsigned w, unsigned h, unsigned xfmt, unsigned xptr)
{
    int i;
    InterlockedIncrement(&g_th_fail_n);
    for (i = 0; i < g_th_fail_k; i++) {
        if (g_th_fail[i].xptr == xptr && g_th_fail[i].what == what) { return; }
    }
    if (g_th_fail_k < TH_NFAIL) {
        th_fail_t *f = &g_th_fail[g_th_fail_k];
        f->what = what; f->hr = hr; f->dxgi = dxgi; f->w = w; f->h = h; f->xfmt = xfmt; f->xptr = xptr;
        g_th_fail_k++;
        HLOG_W("TEXHEALTH abgelehnt #%d: %s hr=0x%08lX dxgi=%u %ux%u xfmt=0x%08X", g_th_fail_k,
               what == 0 ? "tex" : what == 1 ? "srv" : what == 2 ? "lin-tex" : "lin-srv", (unsigned long)hr, dxgi, w, h,
               xfmt);
    }
}

void sb_th_throttle(unsigned present)
{
    g_th_thr_total++;
    if (present != g_th_thr_curf) {
        g_th_thr_curf = present;
        g_th_thr_cur = 0;
        g_th_thr_frames++;
    }
    if (++g_th_thr_cur > g_th_thr_max) { g_th_thr_max = g_th_thr_cur; g_th_thr_max_f = present; }
}

static unsigned g_th_hi;
void sb_th_hi_entry(void)
{
    g_th_hi++;
}

void sb_th_dauer(void)
{
    g_th_dauer++;
}

static ID3D11ShaderResourceView *th_mark_srv(int k)
{
    static const unsigned col[3] = { 0xFF00FFFFu /* gelb RGBA8: R=FF G=FF B=00 A=FF */, 0xFFFFFF00u /* cyan */,
                                     0xFFFF00FFu /* magenta */ };
    D3D11_TEXTURE2D_DESC dd;
    D3D11_SUBRESOURCE_DATA init;
    ID3D11Texture2D *t = NULL;
    if (g_th_mark[k] || g_th_mark_tried[k] || !g_dev) { return g_th_mark[k]; }
    g_th_mark_tried[k] = 1;
    memset(&dd, 0, sizeof dd);
    dd.Width = 1; dd.Height = 1; dd.MipLevels = 1; dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_IMMUTABLE;
    dd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    init.pSysMem = &col[k];
    init.SysMemPitch = 4;
    init.SysMemSlicePitch = 4;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &dd, &init, &t)) && t) {
        ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)t, NULL, &g_th_mark[k]);
        ID3D11Texture2D_Release(t);
    }
    HLOG_I("TEXHEALTH mark: %s %s", k == 0 ? "gelb" : k == 1 ? "cyan" : "magenta", g_th_mark[k] ? "bereit" : "fehlgeschlagen");
    return g_th_mark[k];
}

ID3D11ShaderResourceView *sb_th_mark(unsigned why)
{
    return th_mark_srv(why == 2u || why == 12u ? 0 : (why == 7u || why == 9u) ? 1 : 2);
}

ID3D11ShaderResourceView *sb_th_missing(unsigned why, unsigned present)
{
    g_th_miss_total++;
    g_th_miss_why[why < 16u ? why : 15u]++;
    if (present != g_th_miss_curf) {
        g_th_miss_curf = present;
        g_th_miss_cur = 0;
        g_th_miss_frames++;
    }
    if (++g_th_miss_cur > g_th_miss_max) { g_th_miss_max = g_th_miss_cur; g_th_miss_max_f = present; }
    if (!g_cfg.tex_missing_mark) { return NULL; }
    return sb_th_mark(why);
}

static void th_vram_query(void)
{
    DXGI_QUERY_VIDEO_MEMORY_INFO li, nl;
    if (!g_th_ad3) { return; }
    memset(&li, 0, sizeof li);
    memset(&nl, 0, sizeof nl);
    if (SUCCEEDED(IDXGIAdapter3_QueryVideoMemoryInfo(g_th_ad3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &li))) {
        g_th_budget = li.Budget;
        g_th_usage = li.CurrentUsage;
        if (li.CurrentUsage > g_th_usage_max) { g_th_usage_max = li.CurrentUsage; }
        g_th_vram_ok = 1;
        g_th_vram_n++;
    }
    if (SUCCEEDED(IDXGIAdapter3_QueryVideoMemoryInfo(g_th_ad3, 0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nl))) {
        g_th_nl_budget = nl.Budget;
        g_th_nl_usage = nl.CurrentUsage;
    }
}

void sb_th_device(ID3D11Device *dev, int driver_cmdlists, int warp)
{
    IDXGIDevice *dx = NULL;
    IDXGIAdapter *ad = NULL;
    DXGI_ADAPTER_DESC d;
    g_th_dcl = driver_cmdlists;
    g_th_warp = warp;
    g_th_rt_cfg = g_cfg.render_thread;
    if (!dev) { return; }
    g_th_fl = (unsigned)ID3D11Device_GetFeatureLevel(dev);
    if (FAILED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dx)) || !dx) { return; }
    if (SUCCEEDED(IDXGIDevice_GetAdapter(dx, &ad)) && ad) {
        if (SUCCEEDED(IDXGIAdapter_GetDesc(ad, &d))) {
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, g_th_adapter, sizeof g_th_adapter, NULL, NULL);
            g_th_vendor = d.VendorId;
        }
        if (FAILED(IDXGIAdapter_QueryInterface(ad, &IID_IDXGIAdapter3, (void **)&g_th_ad3))) { g_th_ad3 = NULL; }
        IDXGIAdapter_Release(ad);
    }
    IDXGIDevice_Release(dx);
    g_th_vram_ok = 0;
    th_vram_query();
}

void sb_th_present(unsigned present)
{
    if (present - g_th_vram_last < 1800u) { return; }
    g_th_vram_last = present;
    th_vram_query();
}

static const char *th_why_name(unsigned w)
{
    switch (w) {
    case 1: return "stale-ptr";
    case 2: return "throttle";
    case 3: return "header";
    case 4: return "format";
    case 5: return "lin-dims";
    case 6: return "lin-range";
    case 7: return "lin-create";
    case 8: return "range";
    case 9: return "create";
    case 11: return "rt";
    case 12: return "no-srv-yet";
    default: return NULL;
    }
}

void krnl_contig_place_stats(unsigned *hiplace, unsigned *overlap); /* krnl_mem.c, ISSUE-TEX */
void sb_th_format(char *o, size_t cap, int live)
{
    size_t u;
    unsigned w, pend_e = 0, pend = 0, ct_hi = 0, ct_ov = 0;
    int i;
    char hd[200];
    if (!o || !cap) { return; }
    o[0] = '\0';
    if (live) { th_vram_query(); }
    __try { pend = sb_tex_thr_pending(&pend_e); } __except (EXCEPTION_EXECUTE_HANDLER) { pend = 0; pend_e = 0; }
    krnl_contig_place_stats(&ct_hi, &ct_ov); /* ISSUE-TEX */
    hd[0] = '\0';
    __try { sb_rem_hd_stats(hd, sizeof hd); } __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(hd, sizeof hd, "?"); }
    _snprintf_s(o, cap, _TRUNCATE,
                "[texture health] (W315)\r\n"
                "device: %s (vendor 0x%04X) fl=%X.%X warp=%d driver_command_lists=%d render_thread=%d (cfg %d) "
                "render_thread_emul=%d\r\n"
                "vram: budget=%llu MB usage=%llu MB peak=%llu MB shared_budget=%llu MB shared_usage=%llu MB "
                "(%s, %u samples)\r\n"
                "rejected (D3D create failed): %ld",
                g_th_adapter[0] ? g_th_adapter : "?", g_th_vendor, (g_th_fl >> 12) & 0xFu, (g_th_fl >> 8) & 0xFu,
                g_th_warp, g_th_dcl, g_cfg.render_thread, g_th_rt_cfg, g_cfg.render_thread_emul,
                g_th_budget >> 20, g_th_usage >> 20, g_th_usage_max >> 20, g_th_nl_budget >> 20, g_th_nl_usage >> 20,
                g_th_vram_ok == 1 ? (live ? "now" : "last sample") : "not available", g_th_vram_n, (long)g_th_fail_n);
    for (i = 0; i < g_th_fail_k; i++) {
        const th_fail_t *f = &g_th_fail[i];
        u = strlen(o);
        _snprintf_s(o + u, cap - u, _TRUNCATE, "%s %s hr=0x%08lX dxgi=%u %ux%u xfmt=0x%08X", i ? ";" : ":",
                    f->what == 0 ? "tex" : f->what == 1 ? "srv" : f->what == 2 ? "lin-tex" : "lin-srv",
                    (unsigned long)f->hr, f->dxgi, f->w, f->h, f->xfmt);
    }
    u = strlen(o);
    _snprintf_s(o + u, cap - u, _TRUNCATE,
                "\r\nupload throttle: budget=%d/frame skipped=%u frames=%u peak=%u/frame (present %u) "
                "waiting_now=%u (entries without srv %u) retry_fix=%d\r\n"
                "texture headers above 2 GB: %u (host_hi_addr=%d)\r\n"
                "contig blocks: placed above 2 GB %u (contig_hi_place=%d), overlapping %u\r\n"
                "permanently without srv (TEX dauer-ohne-srv): %u\r\n"
                "draws expecting a texture without srv: %u in %u frames, peak %u/frame (present %u)",
                g_cfg.tex_up_budget > 0 ? g_cfg.tex_up_budget : 12, g_th_thr_total, g_th_thr_frames, g_th_thr_max,
                g_th_thr_max_f, pend, pend_e, g_cfg.tex_throttle_retry, g_th_hi, g_cfg.host_hi_addr, ct_hi, g_cfg.contig_hi_place, ct_ov, g_th_dauer, g_th_miss_total, g_th_miss_frames,
                g_th_miss_max, g_th_miss_max_f);
    for (w = 0; w < 16u; w++) {
        if (!g_th_miss_why[w]) { continue; }
        u = strlen(o);
        if (th_why_name(w)) { _snprintf_s(o + u, cap - u, _TRUNCATE, " %s=%u", th_why_name(w), g_th_miss_why[w]); }
        else { _snprintf_s(o + u, cap - u, _TRUNCATE, " why%u=%u", w, g_th_miss_why[w]); }
    }
    u = strlen(o);
    _snprintf_s(o + u, cap - u, _TRUNCATE, "\r\nhd pack: %s\r\nmissing-texture marker: %s\r\n", hd[0] ? hd : "off",
                g_cfg.tex_missing_mark ? "on (yellow = not uploaded yet, cyan = rejected, magenta = other)" : "off");
}

void sb_th_log(const char *why)
{
    char b[2048], *p, *q;
    sb_th_format(b, sizeof b, 1);
    HLOG_I("TEXHEALTH Block (%s):", why ? why : "?");
    for (p = b; *p; p = q) {
        q = strstr(p, "\r\n");
        if (!q) { HLOG_I("TEXHEALTH %s", p); break; }
        *q = '\0';
        if (*p) { HLOG_I("TEXHEALTH %s", p); }
        q += 2;
    }
}
