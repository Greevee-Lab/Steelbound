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
#include "sb_state.h" /* W360: sb_in_mission */
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

/* W360 ISSUE-47-48 (Mission 23: weisse, untexturierte Geometrie bzw. fast weisser Hauptmonitor,
 * nicht nachgestellt): Ein F11-Paket soll die Ursache eindeutig zeigen, nur Zahlen.
 *  - Stufen 1..3 ohne SRV wurden bisher nur mit tex_missing_mark sichtbar (im Shading-Pfad T = 1,
 *    z. B. Licht-/Detailstufe -> Flaeche zu hell); jetzt immer gezaehlt wie Stufe 0.
 *  - Die letzten 8 betroffenen Texturen (Stufe, Kopfadresse, Formatwort +0xC, Groessenwort +0x10,
 *    Grund, Anzahl, erstes/letztes Bild) und das letzte betroffene Bild gegen "jetzt": zeigt, ob
 *    beim Druck auf F11 gerade Draws ohne Textur liefen.
 *  - Sonnenblendung des Spiels (W337, [0x356D58], hebt das Umgebungslicht des Hauptpasses um
 *    [0x3514D4] * Blendung): jetzt/hoechster Wert, Bilder mit Blendung >= 0,5, vis_ztest. Ein
 *    ganz weisser Hauptmonitor bei Blendung ~0 ist damit keine Blendung.
 * Nur Zaehler/Lesen, Bild bitgleich. */
#define TH_NREC 8
typedef struct {
    unsigned xptr, fmtw, sizew, why, stage, n, first, last;
} th_rec_t;
static th_rec_t g_th_rec[TH_NREC];
static unsigned g_th_rec_next, g_th_miss_last_f;
static unsigned g_th_smiss_total, g_th_smiss_frames, g_th_smiss_max, g_th_smiss_max_f, g_th_smiss_cur, g_th_smiss_curf;
static unsigned g_th_now_f;
static float g_th_gl_now, g_th_gl_max, g_th_gl_gain;
static unsigned g_th_gl_max_f, g_th_gl_frames, g_th_gl_last_f;
static float g_th_gl_c0; /* W511: Umgebungslicht C0 [0x358250] beim Maximum der Blendung */
static float g_th_gl2_now, g_th_gl2_max; /* W477: Blendung zweiter Pass [0x351684] (FRONT VIEW) */

static void th_note(unsigned stage, unsigned xptr, unsigned why, unsigned present)
{
    unsigned i;
    th_rec_t *r;
    g_th_miss_last_f = present;
    for (i = 0; i < TH_NREC; i++) {
        r = &g_th_rec[i];
        if (r->n && r->xptr == xptr && r->stage == stage) {
            r->n++;
            r->last = present;
            r->why = why;
            return;
        }
    }
    r = &g_th_rec[g_th_rec_next++ % TH_NREC];
    r->xptr = xptr; r->stage = stage; r->why = why; r->n = 1; r->first = present; r->last = present;
    r->fmtw = 0; r->sizew = 0;
    if (xptr >= 0x10000u) {
        __try {
            r->fmtw = *(volatile unsigned *)(uintptr_t)(xptr + 0xCu);
            r->sizew = *(volatile unsigned *)(uintptr_t)(xptr + 0x10u);
        } __except (EXCEPTION_EXECUTE_HANDLER) { r->fmtw = 0; r->sizew = 0; }
    }
}

ID3D11ShaderResourceView *sb_th_stage_missing(unsigned stage, unsigned why, unsigned xptr, unsigned present)
{
    g_th_smiss_total++;
    if (present != g_th_smiss_curf) {
        g_th_smiss_curf = present;
        g_th_smiss_cur = 0;
        g_th_smiss_frames++;
    }
    if (++g_th_smiss_cur > g_th_smiss_max) { g_th_smiss_max = g_th_smiss_cur; g_th_smiss_max_f = present; }
    th_note(stage, xptr, why, present);
    if (!g_cfg.tex_missing_mark) { return NULL; }
    return sb_th_mark(why);
}

/* ISSUE44: Stufe 1..3 sampelt eine leere Stufe (keine Textur gesetzt). */
static unsigned g_th_empty_total, g_th_empty_frames, g_th_empty_curf, g_th_empty_last_f, g_th_empty_ps, g_th_empty_st;
void sb_th_stage_empty(unsigned stage, unsigned ps, unsigned present)
{
    g_th_empty_total++;
    if (present != g_th_empty_curf) { g_th_empty_curf = present; g_th_empty_frames++; }
    g_th_empty_last_f = present;
    g_th_empty_ps = ps;
    g_th_empty_st = stage;
}

ID3D11ShaderResourceView *sb_th_missing(unsigned why, unsigned present, unsigned xptr)
{
    th_note(0u, xptr, why, present); /* W360 */
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
    g_th_now_f = present;
    /* W360: Sonnenblendung des Spiels (nur SB). W511: auch Titel-Demo und Replays (Szene 0x2C + Missionsschleife);
     * bisher nur sb_in_mission() -> ein F11-Paket aus Demo/Replay zeigte immer 0, auch waehrend der Blendung. */
    if (!g_sba_loc && sb_state_mission_or_demo()) {
        float gl = 0.0f, gn = 0.0f;
        __try {
            gl = *(volatile float *)(uintptr_t)SBA(0x356D58u);
            gn = *(volatile float *)(uintptr_t)SBA(0x3514D4u);
        } __except (EXCEPTION_EXECUTE_HANDLER) { gl = 0.0f; gn = 0.0f; }
        if (!(gl >= 0.0f && gl <= 16.0f)) { gl = 0.0f; } /* NaN/Unsinn */
        if (!(gn >= -16.0f && gn <= 16.0f)) { gn = 0.0f; }
        g_th_gl_now = gl;
        if (gl > g_th_gl_max) {
            g_th_gl_max = gl; g_th_gl_max_f = present; g_th_gl_gain = gn;
            __try { g_th_gl_c0 = *(volatile float *)(uintptr_t)SBA(0x358250u); } __except (EXCEPTION_EXECUTE_HANDLER) { g_th_gl_c0 = 0.0f; } /* W511 */
        }
        if (gl >= 0.5f) { g_th_gl_frames++; }
        if (gl > 0.0f) { g_th_gl_last_f = present; }
        { float g2 = 0.0f; /* W477 SB-SUN-WHITEOUT */
          __try { g2 = *(volatile float *)(uintptr_t)SBA(0x351684u); } __except (EXCEPTION_EXECUTE_HANDLER) { g2 = 0.0f; }
          if (!(g2 >= -16.0f && g2 <= 16.0f)) { g2 = 0.0f; }
          g_th_gl2_now = g2; if (g2 > g_th_gl2_max) { g_th_gl2_max = g2; } }
    }
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
    { /* W360 ISSUE-47-48: Stufen 1..3, letzte betroffene Texturen, Sonnenblendung (nur Zahlen) */
        unsigned k, n = 0;
        u = strlen(o);
        _snprintf_s(o + u, cap - u, _TRUNCATE,
                    "\r\nstage 1-3 draws without srv (W360): %u in %u frames, peak %u/frame (present %u)\r\n"
                    "last draw without srv: present %u (now %u)\r\n"
                    "textures without srv (W360, last 8):",
                    g_th_smiss_total, g_th_smiss_frames, g_th_smiss_max, g_th_smiss_max_f, g_th_miss_last_f, g_th_now_f);
        for (k = 0; k < TH_NREC; k++) { /* aelteste zuerst */
            const th_rec_t *r = &g_th_rec[(g_th_rec_next + k) % TH_NREC];
            if (!r->n) { continue; }
            u = strlen(o);
            _snprintf_s(o + u, cap - u, _TRUNCATE, "%s st%u x=0x%X fmt=0x%08X size=0x%08X why=%u n=%u f=%u-%u",
                        n++ ? ";" : "", r->stage, r->xptr, r->fmtw, r->sizew, r->why, r->n, r->first, r->last);
        }
        u = strlen(o);
        _snprintf_s(o + u, cap - u, _TRUNCATE,
                    "%s\r\nsun glare (W360): vis_ztest=%d now=%.2f max=%.2f (present %u, ambient gain %.2f, ambient %.3f) "
                    "frames>=0.5: %u last>0: present %u",
                    n ? "" : " none", g_cfg.vis_ztest, g_th_gl_now, g_th_gl_max, g_th_gl_max_f, g_th_gl_gain, g_th_gl_c0,
                    g_th_gl_frames, g_th_gl_last_f);
        { /* W511 SB-SUN-WHITEOUT: Ergebnisse der Sonnen-Sichtbarkeitstests (Hauptpass / FRONT VIEW), GPU-Notausgaenge */
            extern void sb_visq_sun_stats(unsigned *tests, unsigned *vis, unsigned *vis_f, unsigned *timeouts, unsigned *odd,
                                          unsigned *oddmax, unsigned *made);
            unsigned st[2] = { 0, 0 }, sv[2] = { 0, 0 }, sf[2] = { 0, 0 }, sto = 0, sodd = 0, smax = 0, smade = 0;
            sb_visq_sun_stats(st, sv, sf, &sto, &sodd, &smax, &smade);
            u = strlen(o);
            _snprintf_s(o + u, cap - u, _TRUNCATE,
                        " sun tests (W511): main visible=%u/%u (last present %u) front view visible=%u/%u (last present %u) gpu timeouts=%u "
                        "impossible (more than 1 pixel)=%u max=%u queries=%u fresh=%d",
                        sv[1], st[1], sf[1], sv[0], st[0], sf[0], sto, sodd, smax, smade, g_cfg.vis_query_fresh);
        }
        u = strlen(o);
        _snprintf_s(o + u, cap - u, _TRUNCATE,
                    "\r\nstage 1-3 sampled without texture set (ISSUE44): %u in %u frames, last present %u st%u ps=0x%X",
                    g_th_empty_total, g_th_empty_frames, g_th_empty_last_f, g_th_empty_st, g_th_empty_ps);
        if (!g_sba_loc) { /* W477 SB-SUN-WHITEOUT: Licht/Nebel/Monitor-Korn der Welt, Flare-Tests (nur Zahlen, nur SB) */
            float amb[3] = { 0, 0, 0 }, base = 0, fst = 0, fend = 0, fden = 0, wa512 = 0, wa256 = 0, ws256 = 0, nite = 0, lvl = 0;
            unsigned fen = 0, ftb = 0, ft = 0, fv = 0, zf = 0, zfo = 0, fcol = 0;
            extern void sb_visq_stats(unsigned *tests, unsigned *vis, unsigned *zfar, unsigned *zfar_on);
            __try {
                amb[0] = *(volatile float *)(uintptr_t)SBA(0x358250u); amb[1] = *(volatile float *)(uintptr_t)SBA(0x358254u);
                amb[2] = *(volatile float *)(uintptr_t)SBA(0x358258u); base = *(volatile float *)(uintptr_t)SBA(0x35224Cu);
                wa512 = *(volatile float *)(uintptr_t)SBA(0x352150u); wa256 = *(volatile float *)(uintptr_t)SBA(0x35641Cu);
                ws256 = *(volatile float *)(uintptr_t)SBA(0x356DBCu); nite = *(volatile float *)(uintptr_t)SBA(0x351638u);
                lvl = *(volatile float *)(uintptr_t)SBA(0x352BD8u);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            sb_visq_stats(&ft, &fv, &zf, &zfo);
            { /* Nebel des letzten Welt-Draws mit Nebel (render_shade.c), nicht der aktuelle Zustand (Cockpit: aus) */
                extern unsigned g_rq_wfog_tb, g_rq_wfog_col, g_rq_wfog_f;
                extern float g_rq_wfog_st, g_rq_wfog_en, g_rq_wfog_de;
                fen = g_rq_wfog_f; ftb = g_rq_wfog_tb; fst = g_rq_wfog_st; fend = g_rq_wfog_en; fden = g_rq_wfog_de; fcol = g_rq_wfog_col;
            }
            u = strlen(o);
            _snprintf_s(o + u, cap - u, _TRUNCATE,
                        "\r\nworld light (W477): pass2 glare now=%.2f max=%.2f ambient=(%.3f %.3f %.3f) base=%.3f "
                        "fog (last world draw, present %u) tbl=%u start=%.1f end=%.1f dens=%.5f col=0x%08X monitor grain a512=%.2f a256=%.2f+%.2f "
                        "night=%.2f noise=%.2f flare tests=%u visible=%u behind-far=%u (in view %u) vis_zfar=%d",
                        g_th_gl2_now, g_th_gl2_max, amb[0], amb[1], amb[2], base, fen, ftb, fst, fend, fden, fcol,
                        wa512, wa256, ws256, nite, lvl, ft, fv, zf, zfo, g_cfg.vis_zfar);
        }
    }
    u = strlen(o);
    _snprintf_s(o + u, cap - u, _TRUNCATE, "\r\nhd pack: %s\r\nmissing-texture marker: %s\r\n", hd[0] ? hd : "off",
                g_cfg.tex_missing_mark ? "on (yellow = not uploaded yet, cyan = rejected, magenta = other)" : "off");
    { /* W345 HUDFREEZE: Cockpit-Anzeige-Texturen (Beobachtung, Cache, Hash beim letzten Upload gegen Speicher) */
        char hb[900];
        hb[0] = '\0';
        __try { sb_tex_hud_format(hb, sizeof hb); } __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(hb, sizeof hb, "cockpit displays (W345): ?"); }
        u = strlen(o);
        _snprintf_s(o + u, cap - u, _TRUNCATE, "%s\r\n", hb);
    }
}

void sb_th_log(const char *why)
{
    char b[5120], *p, *q; /* W360: 3072 -> 4096 (neue Zeilen); W477: 5120 (Zeile world light) */
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
