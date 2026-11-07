/* PERF(PERF-WARM,W139): Vorwaermen beim Missionsstart/Retry + DIAG.
 *
 * DIAG warm_diag=1: CPU-Zeit je D3D-Context-Aufruf an den Stellen, die beim
 * Missionsstart lange Sperr-Regionen bilden koennen (Draw, Clear, Map,
 * Ruecklesen, ExecuteCommandList). Aufrufe > warm_diag_ms (0 = 2 ms) werden
 * geloggt: WARM-DIAG lang: <ms> <stelle> T<tid> f=<present> erst=<0/1> vs=<p> ps=<p>
 * (erst=1: VS/PS-Paar in diesem Lauf zum ersten Mal gezeichnet). Zusaetzlich
 * je Stelle Summen alle 10 s (WARM-DIAG summe). Standard 0, ohne Wirkung. */
#include "render_int.h"
#include "sb_userdata.h" /* W150 USER-DATA */

static LARGE_INTEGER wd_f;
static volatile LONG wd_nlog;

LONGLONG sb_wd_q(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart ? q.QuadPart : 1;
}

static double wd_ms(LONGLONG dq)
{
    if (!wd_f.QuadPart) { QueryPerformanceFrequency(&wd_f); }
    return (double)dq * 1000.0 / (double)wd_f.QuadPart;
}

/* Erst-Benutzung je VS/PS-Paar (offene Adressierung, nur DIAG). */
#define WD_NSET 4096
static struct { void *vs, *ps; } wd_set[WD_NSET];
static unsigned wd_nset;
static int wd_first(void *vs, void *ps)
{
    unsigned h = (unsigned)(((uintptr_t)vs >> 4) * 2654435761u ^ ((uintptr_t)ps >> 4) * 40503u), i;
    for (i = 0; i < WD_NSET; i++) {
        unsigned s = (h + i) & (WD_NSET - 1u);
        if (wd_set[s].vs == vs && wd_set[s].ps == ps) { return 0; }
        if (!wd_set[s].vs && !wd_set[s].ps) {
            if (wd_nset >= WD_NSET * 3u / 4u) { return 0; }
            wd_set[s].vs = vs; wd_set[s].ps = ps; wd_nset++;
            return 1;
        }
    }
    return 0;
}

void sb_wd_end(LONGLONG q0, const char *site, ID3D11DeviceContext *c)
{
    double ms = wd_ms(sb_wd_q() - q0);
    double lim = g_cfg.warm_diag_ms > 0 ? (double)g_cfg.warm_diag_ms : 2.0;
    int first = -1;
    void *vs = NULL, *ps = NULL;
    if (c) { /* Draw: gebundene Shader (nur DIAG; Get* zaehlt Referenzen hoch) */
        ID3D11VertexShader *v = NULL;
        ID3D11PixelShader *p = NULL;
        ID3D11DeviceContext_VSGetShader(c, &v, NULL, NULL);
        ID3D11DeviceContext_PSGetShader(c, &p, NULL, NULL);
        vs = v; ps = p;
        if (v) { ID3D11VertexShader_Release(v); }
        if (p) { ID3D11PixelShader_Release(p); }
        first = wd_first(vs, ps);
    }
    if (ms > lim && InterlockedIncrement(&wd_nlog) <= 600) {
        HLOG_I("WARM-DIAG lang: %.2fms %s T%04lx f=%u erst=%d vs=%p ps=%p", ms, site, GetCurrentThreadId(),
               g_present_no, first, vs, ps);
    }
}

/* ---------------------------------------------------------------------------
 * PERF(PERF-WARM,W139) warm_shaders=1: Shader-Cache vorab laden + vorab anlegen.
 * Beim Start (nach dem Device) liest ein Hilfs-Thread (niedrige Prioritaet,
 * kein Xbox-Thread, keine Host-Sperre) alle <exe>\shadercache\rq_*.cso in den
 * Speicher und legt je Blob das D3D-Objekt an (Device ist frei-threaded; der
 * Treiber uebersetzt dabei sein Maschinenprogramm). Beim ersten Auftreten
 * eines Programms im Spiel:
 *   - rq_compile_x nimmt den Blob aus dem Speicher statt von der Platte
 *     (W137: Laden 8,5-8,8 ms je Missionsstart);
 *   - CreateVertexShader/CreatePixelShader liefert bei bytegleichem Blob das
 *     vorab angelegte Objekt (AddRef) -> keine Treiber-Uebersetzung im Takt.
 * Bildergebnis bitgleich: derselbe Bytecode, dasselbe Objekt. Fehlt ein Blob
 * oder ist der Hilfs-Thread noch nicht so weit: alter Weg. */
typedef struct {
    unsigned key;        /* FNV ueber den Dateinamen (rq_<hash>_<len>.cso) */
    unsigned size;
    unsigned type;       /* 0 PS, 1 VS, sonst unbekannt */
    unsigned char *data;
    IUnknown *obj;       /* ID3D11VertexShader/PixelShader oder NULL */
    volatile LONG ready; /* 1 = data gueltig, 2 = data + obj fertig */
} wm_ent_t;
#define WM_MAX 2048
static wm_ent_t wm_tab[WM_MAX];
static volatile LONG wm_n;    /* belegte Eintraege (nur der Hilfs-Thread schreibt) */
static volatile LONG wm_done; /* 1 = Hilfs-Thread fertig */
static volatile LONG wm_hit_blob, wm_hit_obj, wm_miss;

static unsigned wm_key(const char *name)
{
    unsigned h = 2166136261u;
    const char *q;
    for (q = name; *q; q++) {
        unsigned ch = (unsigned char)*q;
        if (ch >= 0x41u && ch <= 0x5Au) { ch += 0x20u; } /* Gross -> klein */
        h = (h ^ ch) * 16777619u;
    }
    return h ? h : 1u;
}

/* Programmtyp aus dem DXBC-Container (SHDR/SHEX: Versions-Token, Bits 16-31). */
static unsigned wm_type(const unsigned char *d, unsigned n)
{
    unsigned nch, i;
    if (n < 32u || memcmp(d, "DXBC", 4) != 0) { return 99u; }
    nch = *(const unsigned *)(d + 28);
    if (nch > 64u || 32u + nch * 4u > n) { return 99u; }
    for (i = 0; i < nch; i++) {
        unsigned off = *(const unsigned *)(d + 32 + i * 4u);
        if (off + 12u > n) { continue; }
        if (!memcmp(d + off, "SHDR", 4) || !memcmp(d + off, "SHEX", 4)) {
            return *(const unsigned *)(d + off + 8) >> 16;
        }
    }
    return 99u;
}

static DWORD WINAPI wm_main(LPVOID p)
{
    char dir[MAX_PATH], pat[MAX_PATH + 32], path[MAX_PATH + 320];
    WIN32_FIND_DATAA fd;
    HANDLE fh;
    char *sl;
    LARGE_INTEGER q0, q1, q2;
    unsigned nobj = 0, nbytes = 0, nfail = 0;
    (void)p;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    QueryPerformanceCounter(&q0);
    if (sb_userdata_path(SB_UD_SHADERCACHE, dir, sizeof dir) != 1) { /* W150 USER-DATA */
        if (!GetModuleFileNameA(NULL, dir, sizeof dir) || (sl = strrchr(dir, '\\')) == NULL) { wm_done = 1; return 0; }
        *sl = 0;
        if (strlen(dir) + 13u >= sizeof dir) { wm_done = 1; return 0; }
        strcat_s(dir, sizeof dir, "\\shadercache");
    }
    _snprintf_s(pat, sizeof pat, _TRUNCATE, "%s\\rq_*.cso", dir);
    fh = FindFirstFileA(pat, &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            HANDLE f;
            DWORD sz, rd = 0;
            wm_ent_t *e;
            if (wm_n >= WM_MAX) { break; }
            if (fd.nFileSizeHigh || fd.nFileSizeLow < 32u || fd.nFileSizeLow > (8u << 20)) { continue; }
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", dir, fd.cFileName);
            f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            if (f == INVALID_HANDLE_VALUE) { continue; }
            sz = GetFileSize(f, NULL);
            e = &wm_tab[wm_n];
            e->data = (sz >= 32u && sz <= (8u << 20)) ? (unsigned char *)malloc(sz) : NULL;
            if (e->data && ReadFile(f, e->data, sz, &rd, NULL) && rd == sz) {
                e->key = wm_key(fd.cFileName);
                e->size = sz;
                e->type = wm_type(e->data, sz);
                nbytes += sz;
                MemoryBarrier();
                e->ready = 1;
                InterlockedIncrement(&wm_n);
            } else if (e->data) {
                free(e->data);
                e->data = NULL;
            }
            CloseHandle(f);
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
    QueryPerformanceCounter(&q1);
    {
        LONG i, n = wm_n;
        for (i = 0; i < n; i++) {
            wm_ent_t *e = &wm_tab[i];
            HRESULT hr = E_FAIL;
            if (e->type == 1u) {
                ID3D11VertexShader *v = NULL;
                hr = ID3D11Device_CreateVertexShader(g_dev, e->data, e->size, NULL, &v);
                e->obj = (IUnknown *)v;
            } else if (e->type == 0u) {
                ID3D11PixelShader *ps = NULL;
                hr = ID3D11Device_CreatePixelShader(g_dev, e->data, e->size, NULL, &ps);
                e->obj = (IUnknown *)ps;
            }
            if (SUCCEEDED(hr) && e->obj) { nobj++; } else { e->obj = NULL; if (e->type <= 1u) { nfail++; } }
            MemoryBarrier();
            e->ready = 2;
        }
    }
    QueryPerformanceCounter(&q2);
    wm_done = 1;
    HLOG_I("WARM: Shader-Cache vorab: %ld Blobs (%u KB) in %.1f ms gelesen, %u Objekte in %.1f ms angelegt (%u fehlgeschlagen)",
           (long)wm_n, nbytes >> 10, wd_ms(q1.QuadPart - q0.QuadPart), nobj, wd_ms(q2.QuadPart - q1.QuadPart), nfail);
    return 0;
}

void sb_warm_start(void)
{
    static int started;
    HANDLE th;
    if (started || g_cfg.warm_shaders <= 0 || !g_cfg.shading || !g_cfg.shade_cache || !g_dev) { return; }
    started = 1;
    th = CreateThread(NULL, 0, wm_main, NULL, 0, NULL);
    if (th) { CloseHandle(th); } else { wm_done = 1; }
}

/* Blob zum Cache-Pfad aus dem Speicher (nur der Dateiname zaehlt). 1 = *out gesetzt. */
int sb_warm_blob(const char *path, HRESULT(WINAPI *cb)(SIZE_T, ID3DBlob **), ID3DBlob **out)
{
    const char *nm;
    unsigned k;
    LONG i, n;
    if (g_cfg.warm_shaders <= 0 || !cb) { return 0; }
    nm = strrchr(path, '\\');
    nm = nm ? nm + 1 : path;
    k = wm_key(nm);
    n = wm_n;
    MemoryBarrier();
    for (i = 0; i < n; i++) {
        wm_ent_t *e = &wm_tab[i];
        ID3DBlob *b = NULL;
        if (e->key != k || e->ready < 1) { continue; }
        if (FAILED(cb(e->size, &b)) || !b) { return 0; }
        memcpy(b->lpVtbl->GetBufferPointer(b), e->data, e->size);
        *out = b;
        InterlockedIncrement(&wm_hit_blob);
        return 1;
    }
    InterlockedIncrement(&wm_miss);
    return 0;
}

/* Vorab angelegtes Objekt bei bytegleichem Blob (AddRef). type 0 PS, 1 VS. */
static IUnknown *wm_obj(const void *d, SIZE_T n, unsigned type)
{
    LONG i, cnt;
    if (g_cfg.warm_shaders <= 0) { return NULL; }
    cnt = wm_n;
    MemoryBarrier();
    for (i = 0; i < cnt; i++) {
        wm_ent_t *e = &wm_tab[i];
        if (e->ready < 2 || !e->obj || e->type != type || e->size != n || memcmp(e->data, d, n) != 0) { continue; }
        IUnknown_AddRef(e->obj);
        InterlockedIncrement(&wm_hit_obj);
        return e->obj;
    }
    return NULL;
}

HRESULT sb_warm_create_vs(const void *d, SIZE_T n, ID3D11VertexShader **out)
{
    IUnknown *o = wm_obj(d, n, 1u);
    if (o) { *out = (ID3D11VertexShader *)o; return S_OK; }
    return ID3D11Device_CreateVertexShader(g_dev, d, n, NULL, out);
}

HRESULT sb_warm_create_ps(const void *d, SIZE_T n, ID3D11PixelShader **out)
{
    IUnknown *o = wm_obj(d, n, 0u);
    if (o) { *out = (ID3D11PixelShader *)o; return S_OK; }
    return ID3D11Device_CreatePixelShader(g_dev, d, n, NULL, out);
}

void sb_warm_stat_log(void)
{
    if (g_cfg.warm_shaders > 0) {
        HLOG_I("WARM: Treffer Blob=%ld Objekt=%ld, nicht im Vorrat=%ld, Hilfs-Thread fertig=%ld",
               (long)wm_hit_blob, (long)wm_hit_obj, (long)wm_miss, (long)wm_done);
    }
}

/* ---------------------------------------------------------------------------
 * PERF(PERF-WARM,W139) warm_gpu=N: GPU-Takt in der Ladephase halten.
 * Befund (W139_c0m1/W139_b0m1, scripts/gpu_clock_log.py): Im Ladebild faellt
 * die GPU in den Leerlauf-Zustand P8 (210-330 MHz Kern, 405 MHz Speicher).
 * Das erste Missionsbild trifft auf diesen Takt; der Treiber taktet erst nach
 * ~0,3-0,4 s auf P0 (2520/10501 MHz). In diesem Fenster brauchen Tick- und
 * Zwischenbilder 20-60 ms GPU-Zeit -> verfallene Bilder, DISCARD-Maps warten
 * (78 ms), Luecken bis 81 ms (Missionsstart und Retry).
 * Abhilfe: Erkennt der Present-Takt-Thread die Ladephase (Spiel praesentiert
 * im Mittel langsamer als alle 25 ms: Ladebild 30 Hz bzw. Ladecode; Menues
 * und Ergebnisbild laufen mit 60 Hz), setzt er bei jedem Wiederholbild eine
 * GPU-Last ab: k-mal eine vorab aufgezeichnete Command-List (Einheit:
 * Hilfsziel 64x64 mit Rechenschleife N*1024 Runden + warm_gpu_mem Kopien
 * 2048x2048 fuer den Speichertakt) per ExecuteCommandList(..., TRUE) ->
 * Context-Zustand bleibt unveraendert, die Hilfsziele werden nie gezeigt.
 * k wird nach der gemessenen GPU-Zeit (Timestamps) auf warm_gpu_pct % der
 * Bildperiode geregelt: Eine feste kleine Last taktet der Treiber auf ein
 * Gleichgewicht herunter (W139_w2: P5 800/810 MHz), eine feste grosse Last
 * dauert bei P8 bis 38 ms und haelt die Wiederholbilder auf (W139_w3). Mit
 * der Regelung startet die Last bei P8 klein, der Treiber taktet hoch, die
 * Last waechst mit. Nur eine Last gleichzeitig in der GPU (Event).
 * Bild bitgleich; wirkt nur mit Takt-Thread (Echtzeit, present_decouple=1). */
static ID3D11CommandList *wg_cl;
/* W165 GPU-COST2 warm_gpu_soft: kleine Einheit (1/8 Rechenschleife, ohne Speicherkopien) fuer den
 * Start der Regelung. Befund P_C25c: Der erste volle Block nach langem GPU-Leerlauf kostete 18-23 ms
 * (WARM-GPU max 23.48/23.00) und hielt das naechste Wiederholbild auf (Retry-Spitzen 18-23 ms,
 * je in einem 20-ms-Blip "Ladephase an/aus" beim Retry). */
static ID3D11CommandList *wg_cl_s;
static LONGLONG wg_on_q;            /* Beginn der laufenden Ladephase (soft: Entprellung) */
static unsigned wg_skip_deb;        /* Statistik: Schritte in der Entprellzeit ohne Last */
static ID3D11Query *wg_ev, *wg_dj, *wg_t0, *wg_t1;
static volatile LONG wg_ready; /* 1 = Command-List fertig */
static LONGLONG wg_game_q[8];
static volatile LONG wg_game_n;
static unsigned wg_n, wg_busy, wg_meas_n, wg_on_prev;
static unsigned wg_k = 1, wg_ksum;  /* Einheiten je Wiederholbild (geregelt), Summe fuer die Statistik (W165: in Achteln) */
static double wg_kf = 1.0;          /* Regelgroesse (gebrochen, gerundet -> wg_k) */
static double wg_kmeas = 1.0;       /* Einheiten der gemessenen Last (soft: in Achteln) */
static LONGLONG wg_last_step;
static double wg_gpu_ms, wg_gpu_max;
static int wg_tq_pend;
static LONGLONG wg_stat_q;

static const char kWG_VS[] =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 p = float2((id << 1) & 2, id & 2);\n"
    "  return float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); }\n";
/* Rechenschleife mit Abhaengigkeit (kein Wegoptimieren, latenzgebunden). */
static const char kWG_PS_FMT[] =
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    "  float x = pos.x * 0.0137, y = pos.y * 0.0071;\n"
    "  [loop] for (uint i = 0; i < %uu; i++) { x = frac(x * 1.0001 + y); y = y * 0.9999 + x * 0.0001; }\n"
    "  return float4(x, y, 0.0, 1.0); }\n";

/* Speicherverkehr: Kopie 2048x2048 RGBA8 per Load (Ping-Pong zweier Ziele). */
static const char kWG_PS_COPY[] =
    "Texture2D<float4> t0 : register(t0);\n"
    "float4 main(float4 pos : SV_Position) : SV_Target { return t0.Load(int3(pos.xy, 0)) * 0.999 + 0.0005; }\n";
#define WG_MEM_W 2048u

static DWORD WINAPI wg_init_main(LPVOID p)
{
    char ps_src[512];
    ID3DBlob *vb = NULL, *pb = NULL;
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    ID3D11Texture2D *tex = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11DeviceContext *dc = NULL;
    D3D11_TEXTURE2D_DESC td;
    D3D11_VIEWPORT vp;
    D3D11_QUERY_DESC qd;
    unsigned iters = (unsigned)g_cfg.warm_gpu * 1024u;
    (void)p;
    _snprintf_s(ps_src, sizeof ps_src, _TRUNCATE, kWG_PS_FMT, iters);
    if (!sb_im_compile(kWG_VS, "vs_4_0", &vb) || !sb_im_compile(ps_src, "ps_4_0", &pb)) { goto fail; }
    if (FAILED(ID3D11Device_CreateVertexShader(g_dev, vb->lpVtbl->GetBufferPointer(vb), vb->lpVtbl->GetBufferSize(vb), NULL, &vs)) ||
        FAILED(ID3D11Device_CreatePixelShader(g_dev, pb->lpVtbl->GetBufferPointer(pb), pb->lpVtbl->GetBufferSize(pb), NULL, &ps))) {
        goto fail;
    }
    memset(&td, 0, sizeof td);
    td.Width = 64; td.Height = 64; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &tex)) ||
        FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)tex, NULL, &rtv)) ||
        FAILED(ID3D11Device_CreateDeferredContext(g_dev, 0, &dc))) {
        goto fail;
    }
    memset(&vp, 0, sizeof vp);
    vp.Width = 64.0f; vp.Height = 64.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_OMSetRenderTargets(dc, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(dc, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(dc, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(dc, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(dc, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(dc, ps, NULL, 0);
    ID3D11DeviceContext_Draw(dc, 3, 0);
    if (g_cfg.warm_gpu_soft & 2) { /* W165: kleine Einheit = 1/8 der Rechenschleife, gleiches Hilfsziel, ohne Speicheranteil */
        ID3DBlob *sb = NULL;
        ID3D11PixelShader *pss = NULL;
        ID3D11DeviceContext *dcs = NULL;
        char ps_s[512];
        _snprintf_s(ps_s, sizeof ps_s, _TRUNCATE, kWG_PS_FMT, iters / 8u > 0 ? iters / 8u : 1u);
        if (sb_im_compile(ps_s, "ps_4_0", &sb) &&
            SUCCEEDED(ID3D11Device_CreatePixelShader(g_dev, sb->lpVtbl->GetBufferPointer(sb), sb->lpVtbl->GetBufferSize(sb), NULL, &pss)) &&
            SUCCEEDED(ID3D11Device_CreateDeferredContext(g_dev, 0, &dcs))) {
            ID3D11DeviceContext_OMSetRenderTargets(dcs, 1, &rtv, NULL);
            ID3D11DeviceContext_RSSetViewports(dcs, 1, &vp);
            ID3D11DeviceContext_IASetInputLayout(dcs, NULL);
            ID3D11DeviceContext_IASetPrimitiveTopology(dcs, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11DeviceContext_VSSetShader(dcs, vs, NULL, 0);
            ID3D11DeviceContext_PSSetShader(dcs, pss, NULL, 0);
            ID3D11DeviceContext_Draw(dcs, 3, 0);
            if (FAILED(ID3D11DeviceContext_FinishCommandList(dcs, FALSE, &wg_cl_s))) { wg_cl_s = NULL; }
        }
        if (!wg_cl_s) { HLOG_W("WARM-GPU: kleine Einheit nicht angelegt -> soft ohne Achtel"); }
        if (dcs) { ID3D11DeviceContext_Release(dcs); }
        if (pss) { ID3D11PixelShader_Release(pss); }
        if (sb) { sb->lpVtbl->Release(sb); }
    }
    if (g_cfg.warm_gpu_mem > 0) { /* Speicheranteil: haelt auch den Speichertakt oben (reine Rechenlast: P5 810 MHz) */
        ID3DBlob *cb = NULL;
        ID3D11PixelShader *pc = NULL;
        ID3D11Texture2D *mt[2] = { NULL, NULL };
        ID3D11RenderTargetView *mr[2] = { NULL, NULL };
        ID3D11ShaderResourceView *ms[2] = { NULL, NULL }, *nul = NULL;
        int k, ok = sb_im_compile(kWG_PS_COPY, "ps_4_0", &cb) &&
            SUCCEEDED(ID3D11Device_CreatePixelShader(g_dev, cb->lpVtbl->GetBufferPointer(cb), cb->lpVtbl->GetBufferSize(cb), NULL, &pc));
        td.Width = WG_MEM_W; td.Height = WG_MEM_W;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (k = 0; k < 2 && ok; k++) {
            ok = SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &mt[k])) &&
                 SUCCEEDED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)mt[k], NULL, &mr[k])) &&
                 SUCCEEDED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)mt[k], NULL, &ms[k]));
        }
        if (ok) {
            vp.Width = (float)WG_MEM_W; vp.Height = (float)WG_MEM_W;
            ID3D11DeviceContext_RSSetViewports(dc, 1, &vp);
            ID3D11DeviceContext_PSSetShader(dc, pc, NULL, 0);
            for (k = 0; k < g_cfg.warm_gpu_mem; k++) {
                ID3D11DeviceContext_PSSetShaderResources(dc, 0, 1, &nul);
                ID3D11DeviceContext_OMSetRenderTargets(dc, 1, &mr[k & 1], NULL);
                ID3D11DeviceContext_PSSetShaderResources(dc, 0, 1, &ms[(k + 1) & 1]);
                ID3D11DeviceContext_Draw(dc, 3, 0);
            }
            ID3D11DeviceContext_PSSetShaderResources(dc, 0, 1, &nul);
        } else {
            HLOG_W("WARM-GPU: Speicheranteil nicht angelegt -> nur Rechenlast");
        }
        for (k = 0; k < 2; k++) {
            if (ms[k]) { ID3D11ShaderResourceView_Release(ms[k]); }
            if (mr[k]) { ID3D11RenderTargetView_Release(mr[k]); }
            if (mt[k]) { ID3D11Texture2D_Release(mt[k]); }
        }
        if (pc) { ID3D11PixelShader_Release(pc); }
        if (cb) { cb->lpVtbl->Release(cb); }
    }
    if (FAILED(ID3D11DeviceContext_FinishCommandList(dc, FALSE, &wg_cl))) { wg_cl = NULL; goto fail; }
    memset(&qd, 0, sizeof qd);
    qd.Query = D3D11_QUERY_EVENT;
    if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &wg_ev))) { wg_ev = NULL; goto fail; }
    qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &wg_dj))) { wg_dj = NULL; }
    qd.Query = D3D11_QUERY_TIMESTAMP;
    if (wg_dj && (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &wg_t0)) || FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &wg_t1)))) {
        wg_dj = NULL;
    }
    /* Die Command-List haelt ihre Objekte selbst. */
    ID3D11DeviceContext_Release(dc);
    ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(tex);
    ID3D11VertexShader_Release(vs);
    ID3D11PixelShader_Release(ps);
    vb->lpVtbl->Release(vb);
    pb->lpVtbl->Release(pb);
    MemoryBarrier();
    wg_ready = 1;
    HLOG_I("WARM-GPU: bereit (Hilfsziel 64x64, %u Runden je Last, Speicher %d x %ux%u)", iters, g_cfg.warm_gpu_mem,
           WG_MEM_W, WG_MEM_W);
    if (g_cfg.warm_gpu_soft > 0) {
        HLOG_I("WARM-GPU: sanft (W165) Bits=%d: Entprellung=%d ms (1), Start 1/8 Einheit (2: %s), Last nach dem Wiederholbild (4)",
               g_cfg.warm_gpu_soft, (g_cfg.warm_gpu_soft & 1) ? g_cfg.warm_gpu_delay : 0, wg_cl_s ? "bereit" : "aus",
               (g_cfg.warm_gpu_soft & 4) ? 1 : 0);
    }
    return 0;
fail:
    HLOG_W("WARM-GPU: Anlage fehlgeschlagen -> aus");
    if (dc) { ID3D11DeviceContext_Release(dc); }
    if (rtv) { ID3D11RenderTargetView_Release(rtv); }
    if (tex) { ID3D11Texture2D_Release(tex); }
    if (vs) { ID3D11VertexShader_Release(vs); }
    if (ps) { ID3D11PixelShader_Release(ps); }
    if (vb) { vb->lpVtbl->Release(vb); }
    if (pb) { pb->lpVtbl->Release(pb); }
    return 0;
}

void sb_warm_gpu_start(void)
{
    static int started;
    HANDLE th;
    if (started || g_cfg.warm_gpu <= 0 || !g_dev) { return; }
    started = 1;
    th = CreateThread(NULL, 0, wg_init_main, NULL, 0, NULL);
    if (th) { CloseHandle(th); }
}

/* Spiel-Thread: echtes Present des Spiels (nicht Takt-Thread). */
void sb_warm_note_game_present(LONGLONG q)
{
    if (g_cfg.warm_gpu <= 0) { return; }
    wg_game_q[(unsigned)wg_game_n & 7u] = q;
    MemoryBarrier();
    InterlockedIncrement(&wg_game_n);
}

/* Ladephase: die letzten 6 Abstaende der Spiel-Presents im Mittel > 25 ms
 * oder das letzte Spiel-Present > 25 ms her (Ladecode ohne Present). */
static int wg_load_phase(LONGLONG now)
{
    LONGLONG lim = (LONGLONG)(wd_f.QuadPart / 40); /* 25 ms */
    unsigned n = (unsigned)wg_game_n;
    LONGLONG last, first;
    if (n < 7u) { return 0; }
    MemoryBarrier();
    last = wg_game_q[(n - 1u) & 7u];
    first = wg_game_q[(n - 7u) & 7u];
    if (now - last > lim) { return 1; }
    return (last - first) > lim * 6;
}

/* Takt-Thread, unter ctx_lock, im Wiederhol-Zweig (kein Interp-Takt). */
void sb_warm_gpu_step(LONGLONG now)
{
    BOOL done = FALSE;
    int on;
    if (g_cfg.warm_gpu <= 0 || !wg_ready) { return; }
    if (!wd_f.QuadPart) { QueryPerformanceFrequency(&wd_f); }
    if (wg_last_step && now - wg_last_step > wd_f.QuadPart / 10) {
        /* aus dem Interp-Takt zurueck (Mission/Film): Spiel-Presents liefen dort nicht hier
         * vorbei -> alte Abstaende verwerfen, Last neu klein beginnen */
        wg_game_n = 0;
        wg_k = 1; wg_kf = 1.0;
        if (wg_cl_s) { wg_kf = 0.125; }
        wg_on_q = now; /* W165: Entprellung auch nach Rueckkehr aus dem Interp-Takt */
    }
    wg_last_step = now;
    on = wg_load_phase(now);
    if (on != (int)wg_on_prev) {
        HLOG_I("WARM-GPU: Ladephase %s f=%u", on ? "an" : "aus", g_present_no);
        wg_on_prev = (unsigned)on;
        if (on) { wg_k = 1; wg_kf = 1.0; } /* neu klein beginnen: GPU evtl. schon in P8 (W139_w5: k=23 aus der Vorphase -> Present 35 ms) */
        if (on && wg_cl_s) { wg_kf = 0.125; } /* W165: noch kleiner (1/8 Einheit) */
        if (on) { wg_on_q = now; }
    }
    if (wg_tq_pend && wg_dj) { /* Messung der letzten Last abholen (ohne Warten) */
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 a = 0, b = 0;
        if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)wg_dj, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)wg_t0, &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)wg_t1, &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
            wg_tq_pend = 0;
            if (!dj.Disjoint && dj.Frequency && b > a) {
                double ms = (double)(b - a) * 1000.0 / (double)dj.Frequency;
                double tgt = (g_cfg.warm_gpu_pct > 0 ? g_cfg.warm_gpu_pct : 85) * 1000.0 / 100.0 /
                             (g_cfg.interp_fps > 0 ? g_cfg.interp_fps : 120);
                double kn = tgt * (double)wg_kmeas / (ms > 0.05 ? ms : 0.05); /* Einheiten fuer das Ziel bei jetzigem Takt */
                wg_gpu_ms += ms; wg_meas_n++;
                if (ms > wg_gpu_max) { wg_gpu_max = ms; }
                kn = (wg_kf + kn) * 0.5; /* gedaempft */
                if (kn > wg_kf * 2.0) { kn = wg_kf * 2.0; }
                {
                    /* W165: nur die erste Last einer Phase ist 1/8 (unbekannter Takt); ab der ersten
                     * Messung mindestens eine volle Einheit mit Speicherkopien wie W139 - reine kleine
                     * Rechenlast haelt die GPU in P8 (W165_am1_4k: 210/405 MHz trotz Last) */
                    double kmin = 1.0;
                    wg_kf = kn < kmin ? kmin : (kn > 64.0 ? 64.0 : kn);
                }
                wg_k = (unsigned)(wg_kf + 0.5);
            }
        }
    }
    if (on && (g_cfg.warm_gpu_soft & 1) && g_cfg.warm_gpu_delay > 0 &&
        now - wg_on_q < wd_f.QuadPart * g_cfg.warm_gpu_delay / 1000) {
        wg_skip_deb++; /* W165: Entprellung - kurze Blips (Retry: 20 ms) ohne Last */
    } else if (on) {
        static int ev_used; /* Event schon einmal gesetzt (wg_n wird je 10 s fuer die Statistik geleert) */
        if (ev_used && ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)wg_ev, &done, sizeof done,
                                                D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
            wg_busy++; /* letzte Last noch in der GPU: keine weitere einreihen */
        } else {
            int meas = wg_dj && !wg_tq_pend;
            unsigned full = wg_k, part = 0;
            if (wg_cl_s) { /* in Achteln: volle Einheiten + kleine */
                unsigned e8 = (unsigned)(wg_kf * 8.0 + 0.5);
                if (e8 < 1u) { e8 = 1u; }
                full = e8 / 8u; part = e8 % 8u;
            }
            if (meas) {
                wg_kmeas = (double)full + (double)part / 8.0;
                ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous *)wg_dj);
                ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)wg_t0);
            }
            { unsigned u; for (u = 0; u < full; u++) { ID3D11DeviceContext_ExecuteCommandList(g_ctx, wg_cl, TRUE); } }
            { unsigned u; for (u = 0; u < part; u++) { ID3D11DeviceContext_ExecuteCommandList(g_ctx, wg_cl_s, TRUE); } }
            wg_ksum += full * 8u + part;
            if (meas) {
                ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)wg_t1);
                ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)wg_dj);
                wg_tq_pend = 1;
            }
            ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)wg_ev);
            ev_used = 1;
            wg_n++;
        }
    }
    if (!wg_stat_q) { wg_stat_q = now; }
    if (now - wg_stat_q >= wd_f.QuadPart * 10) {
        if (wg_n || wg_busy) {
            HLOG_I("WARM-GPU: 10s lasten=%u belegt=%u gpu=%.2fms mittel (max %.2f, n=%u) einheiten=%.1f mittel (jetzt %u)",
                   wg_n, wg_busy, wg_meas_n ? wg_gpu_ms / wg_meas_n : 0.0, wg_gpu_max, wg_meas_n,
                   wg_n ? (double)wg_ksum / 8.0 / wg_n : 0.0, wg_k);
        }
        if (wg_skip_deb) { /* W165: neue Zeile, die obige bleibt unveraendert */
            HLOG_I("WARM-GPU: 10s entprellt=%u (Wiederholbilder ohne Last in den ersten %d ms einer Ladephase)", wg_skip_deb,
                   g_cfg.warm_gpu_delay);
        }
        wg_skip_deb = 0;
        wg_n = wg_busy = wg_meas_n = wg_ksum = 0;
        wg_gpu_ms = wg_gpu_max = 0.0;
        wg_stat_q = now;
    }
}
