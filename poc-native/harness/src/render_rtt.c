/* render_rtt.c - D3D11-Renderer: Render-to-Texture (W23): Surface-Tabelle,
 * aktuelles Ziel, Backbuffer-Notiz, Readback (rtt_readback), Session-Ende,
 * SetRenderTarget-Umleitung, Lookup fuer das Sampling (sb_tex.c).
 * W68 SPLIT: 1:1 aus d3d11_render.c verschoben. */
#include "render_int.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

rtt_entry_t g_rtt[RTT_MAX];
int g_rtt_n;
static int g_rtt_cur = -1;
int rtt_cur_slot(void) { return g_rtt_cur; } /* DIAG(SHADE-BLOOM,W112): Session-Ziel */
int g_rtt_bound = -1;
int g_rtt_dummy; /* FIX(MONITORVIEW,W36): 1 = Dummy-Surface (Data=0) gebunden (Tiefenpass, kein sichtbares Ziel) */
static unsigned g_rtt_bb[4];
static int g_rtt_nbb;
int rtt_active(void) { return g_rtt_bound >= 0 && g_rtt_bound < g_rtt_n; }
ID3D11RenderTargetView *rtt_cur_rtv(void)
{
    return rtt_active() ? g_rtt[g_rtt_bound].rtv : g_rtv;
}
ID3D11DepthStencilView *rtt_cur_dsv(void)
{
    return rtt_active() ? g_rtt[g_rtt_bound].dsv : g_dsv;
}
void rtt_cur_dims(unsigned *w, unsigned *h)
{
    if (rtt_active()) { *w = g_rtt[g_rtt_bound].w; *h = g_rtt[g_rtt_bound].h; }
    else { *w = g_bb_w; *h = g_bb_h; }
}
/* =====================================================================
 * RTT (W23): Render-to-Texture.
 * Das Spiel rendert Titel-/Menue-Hintergruende in Offscreen-Surfaces
 * (D3DDevice_SetRenderTarget @0x20E370 - der alte Hook @0x20E8D0 lag
 * mitten in D3DDevice_SetTexture und konnte nie feuern, M2-Fehlfund) und
 * sampelt sie danach als Textur (u.a. lineare 1024x1024-Flaeche
 * data=0xFB60000, Titel-xptr=0x4C19C8, per CPU nie beschrieben). Jede
 * Surface bekommt eine D3D11-Textur mit RTV+SRV (+ eigenes DSV);
 * SetRenderTarget bindet sie um, Sampling (sb_tex.c) liefert die SRV
 * statt aus leerem CPU-Speicher zu laden.
 * Seitenverhaeltnis (Regel 3.10): ins Target 1:1 (kein Aspect, kein Hor+
 * - das Spiel komponiert das Bild fuer die Flaeche); Aspect/Hor+ greifen
 * erst, wenn die Textur auf dem Bildschirm gezeigt wird (normaler Pfad).
 * Schalter [render] rtt: 0 = nur notieren + DIAG (RTT #, TEX rt-match,
 * Session-Statistiken), Backbuffer-Pfad unveraendert; 1 = umleiten.
 * Beleg W23_rtt0: RTT #1 surf=0xAB832B0 data=0xFB60000 1024x1024 f=36669
 * (Titel-Fade), TEX rt-match xptr=0x4C19C8 gleiche Datenadresse.
 * ===================================================================== */

/* Xbox-Surface-Header: Common@0, Data@+4, Format@+0xC, Size@+0x10
 * (wie Textur; belegt per Get2DSurfaceDesc-Disasm 0x212820). */
static int rtt_hdr(unsigned surf, unsigned *data, unsigned *fmt,
                   unsigned *size)
{
    unsigned d = 0, f = 0, s = 0;
    if (!surf) { return 0; }
    __try {
        d = *(unsigned *)(uintptr_t)(surf + 4);
        f = *(unsigned *)(uintptr_t)(surf + 12);
        s = *(unsigned *)(uintptr_t)(surf + 16);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (!d) { return 0; }
    *data = d; *fmt = f; *size = s;
    return 1;
}

/* Dims aus Size-Feld (linear, wie sb_tex.c) bzw. USIZE/VSIZE (swizzled). */
int rtt_dims(unsigned fmt, unsigned size, unsigned *w, unsigned *h)
{
    unsigned sw, sh;
    if (size) {
        sw = (size & 0xFFFu) + 1u;
        sh = ((size >> 12) & 0xFFFu) + 1u;
        if (sw >= 1 && sw <= 2048 && sh >= 1 && sh <= 2048) {
            *w = sw; *h = sh;
            return 1;
        }
        return 0;
    }
    sw = (fmt & 0xF00000u) >> 20;
    sh = (fmt & 0xF000000u) >> 24;
    if (!sw || !sh || sw > 11 || sh > 11) { return 0; }
    *w = 1u << sw; *h = 1u << sh;
    return 1;
}

/* W88 RENDER-RES: Faktor fuer ein neues RT. Nur Welt-/Monitor-RTs
 * (beide Kanten rt_scale_min..512: Hauptmonitor 512x512, Sub-Monitor/FRONT
 * VIEW 256x256 und ihre Kompositions-RTs) werden groesser angelegt; Menue-/
 * Titel-RTs (640x480, 1024x1024), Effekt-/Messketten (< 256) und alle RTs mit
 * CPU-Rueckschrieb (rtt_readback, Zellenkarte -> Lock-On) bleiben 1:1, damit
 * Spiellogik und Menues dieselben Werte/Bilder bekommen. Logische Groesse
 * (w/h, Spiel-Viewport, NDC-/Texel-Abbildung) bleibt die Xbox-Groesse; nur
 * die D3D11-Textur, der Tiefenpuffer und der D3D11-Viewport sind N-fach. */
static unsigned g_rt_dyn; /* W146 rt_scale_dyn: aktueller Faktor (0 = statisch) */
static int rtt_ensure(int idx);
static unsigned rtt_scale_for(unsigned w, unsigned h, unsigned fmt)
{
    unsigned n, xf = (fmt >> 8) & 0xFFu, mn = (unsigned)(g_cfg.rt_scale_min > 0 ? g_cfg.rt_scale_min : 1);
    if (g_cfg.rt_scale == 1) { return 1u; }
    if (w < mn || h < mn || w > 512u || h > 512u) { return 1u; }
    if (g_cfg.rtt_readback && (xf == 0x1Eu || (g_cfg.rtt_readback >= 2 && g_cfg.rtt_readback != 9 && xf == 0x12u))) {
        unsigned lim = (g_cfg.rtt_readback == 3) ? 512u : 128u;
        if (w <= lim && h <= lim) { return 1u; } /* CPU liest den Inhalt (rtt_readback) */
    }
    if (g_cfg.rt_scale >= 2) { n = (unsigned)g_cfg.rt_scale; }
    else { n = (g_bb_h + 639u) / 640u; } /* auto: 1080 -> 2, 1440 -> 3, 2160 -> 4 */
    if (n < 1u) { n = 1u; }
    if (n > 4u) { n = 4u; }
    if (g_rt_dyn && n >= 2u) { n = g_rt_dyn; } /* W146 rt_scale_dyn: aktueller Faktor */
    return n;
}

/* W146 GPU-COST rt_scale_dyn: Grundfaktor (auto bzw. rt_scale) und aktueller Faktor */
unsigned sb_rtt_base_factor(void)
{
    unsigned n;
    if (g_cfg.rt_scale == 1) { return 1u; }
    n = g_cfg.rt_scale >= 2 ? (unsigned)g_cfg.rt_scale : (g_bb_h + 639u) / 640u;
    return n < 1u ? 1u : (n > 4u ? 4u : n);
}

unsigned sb_rtt_dyn_factor(void)
{
    return g_rt_dyn ? g_rt_dyn : sb_rtt_base_factor();
}


void sb_rtt_note_backbuffer(unsigned surf)
{
    static unsigned nlog;
    int i;
    if (!surf) { return; }
    for (i = 0; i < g_rtt_nbb; i++) {
        if (g_rtt_bb[i] == surf) { return; }
    }
    if (g_rtt_nbb < 4) { g_rtt_bb[g_rtt_nbb++] = surf; }
    if (nlog < 4) {
        unsigned data = 0, fmt = 0, size = 0, w = 0, h = 0;
        nlog++;
        if (rtt_hdr(surf, &data, &fmt, &size)) { rtt_dims(fmt, size, &w, &h); }
        HLOG_I("RTT BB #%u: surf=0x%X data=0x%X %ux%u fmt=0x%08X size=0x%08X",
               nlog, surf, data, w, h, fmt, size);
    }
}

static int rtt_is_bb(unsigned surf)
{
    int i;
    for (i = 0; i < g_rtt_nbb; i++) {
        if (g_rtt_bb[i] == surf) { return 1; }
    }
    return 0;
}

/* RENDER-QA3 (W100): Polygon-Offset des Schattenpuffer-Tiefenpasses. Das
 * Spiel setzt D3DRS_POLYGONOFFSETZSLOPESCALE/ZOFFSET (D3D__RenderState 0x21AAB0
 * [77]/[78], z. B. 1.55 / 0.006 in Xbox-z-Einheiten); ohne Bias Schatten-Akne
 * (fleckiger Boden). Rasterizer-State nur waehrend der Schattenpuffer gebunden
 * ist (Interp-Wiedergabe uebernimmt ihn je Draw per ip_snap). */
static ID3D11RasterizerState *g_rs_shadow;
static int g_rs_shadow_on;
static unsigned g_rs_shadow_key[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };
static void rtt_shadow_rs(int on, float zscale)
{
    if (!g_dev || !g_ctx) { return; }
    if (on) { /* Werte setzt das Spiel erst nach SetRenderTarget -> je Draw pruefen (rtt_note_draw) */
        unsigned k0 = *(volatile unsigned *)(uintptr_t)(0x21AAB0u + 77u * 4u);
        unsigned k1 = *(volatile unsigned *)(uintptr_t)(0x21AAB0u + 78u * 4u);
        if (g_rs_shadow && (k0 != g_rs_shadow_key[0] || k1 != g_rs_shadow_key[1])) {
            if (g_rs_shadow_on) { ID3D11DeviceContext_RSSetState(g_ctx, g_rs_norm); g_rs_shadow_on = 0; }
            ID3D11RasterizerState_Release(g_rs_shadow); g_rs_shadow = NULL;
        }
        g_rs_shadow_key[0] = k0; g_rs_shadow_key[1] = k1;
    }
    if (on && !g_rs_shadow) {
        D3D11_RASTERIZER_DESC rd;
        union { unsigned u; float f; } sl, of;
        static unsigned nlog;
        sl.u = g_rs_shadow_key[0];
        of.u = g_rs_shadow_key[1];
        if (!(sl.f > 0.0f && sl.f < 16.0f)) { sl.f = 0.0f; }
        if (!(of.f > 0.0f && of.f < 64.0f)) { of.f = 0.0f; }
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        rd.SlopeScaledDepthBias = sl.f;
        rd.DepthBias = (INT)(of.f / (zscale > 0.0f ? zscale : 1.0f) * 16777215.0f + 0.5f); /* D24-Einheiten */
        if (FAILED(ID3D11Device_CreateRasterizerState(g_dev, &rd, &g_rs_shadow))) { g_rs_shadow = NULL; }
        if (nlog < 8u) {
            nlog++;
            HLOG_I("RTT Schattenpuffer Polygon-Offset: slope=%g offset=%g -> DepthBias=%d SlopeScaled=%g f=%u",
                   sl.f, of.f, rd.DepthBias, rd.SlopeScaledDepthBias, g_present_no);
        }
    }
    if (on && g_rs_shadow && !g_rs_shadow_on) { ID3D11DeviceContext_RSSetState(g_ctx, g_rs_shadow); g_rs_shadow_on = 1; }
    else if (!on && g_rs_shadow_on) { ID3D11DeviceContext_RSSetState(g_ctx, g_rs_norm); g_rs_shadow_on = 0; }
}

static void rtt_bind_bb(void)
{
    D3D11_VIEWPORT dvp;
    if (!g_dev) { g_rtt_cur = -1; g_rtt_bound = -1; return; }
    sb_ctx_enter();
    rtt_shadow_rs(0, 0.0f); /* W100 */
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, g_dsv);
    dvp.TopLeftX = 0.0f; dvp.TopLeftY = 0.0f;
    dvp.Width = (float)g_bb_w; dvp.Height = (float)g_bb_h;
    dvp.MinDepth = 0.0f; dvp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &dvp);
    sb_ctx_leave();
    g_rtt_cur = -1;
    g_rtt_bound = -1;
}

static int rtt_find(unsigned surf)
{
    int i;
    for (i = 0; i < g_rtt_n; i++) {
        if (g_rtt[i].surf == surf) { return i; }
    }
    return -1;
}

/* FIX(COMBAT,W59) rtt_readback: GPU->CPU-Rueckschrieb kleiner linearer
 * 32-Bit-Render-Targets in den Xbox-Speicher. Auf der Xbox teilen GPU und CPU
 * den Speicher; das Spiel rendert z. B. eine 128x128-Zellenkarte (Textur
 * FUN_0002D320 @0x2DBF3: CreateTexture(128,128,1,RENDERTARGET,0x1E LIN_X8R8G8B8),
 * Daueradresse per LockRect -> [0x35149C]) und liest sie per CPU
 * (FUN_00021EB0 Lock-/Auto-Ziel-Kandidat, FUN_00022090/22230, Gelaende-
 * Zellen FUN_00025E20). Mit RTT-Umleitung blieb der Inhalt GPU-only ->
 * Karte 0/16384 aktiv (W59_kill4) -> nie ein Lock-Kandidat.
 * 0 = aus (alt), 1 = nur LIN_X8R8G8B8 bis 128x128 (Zellenkarte, Default),
 * 2 = zusaetzlich LIN_A8R8G8B8 bis 128x128, 3 = beide bis 512x512. */
static ID3D11Texture2D *g_rtt_stage[RTT_MAX];
static unsigned char g_rtt_stage_pend[RTT_MAX]; /* Kopie der Vor-Session liegt bereit */
static unsigned g_rtt_stage_host[RTT_MAX], g_rtt_stage_pitch[RTT_MAX];
unsigned krnl_phys_to_virt(unsigned phys);
static unsigned krnl_phys_to_virt_rb(unsigned phys) { return krnl_phys_to_virt(phys); }

/* FIX(CULL-POPIN,W215) rtt_rb_sync: Zellenkarte im selben Spiel-Bild.
 * Das Spiel rendert die 128x128-Sicht-Zellenkarte (FUN_00030190: Faecher um
 * die Kamera-Gierung [0x356FAC], halber Winkel [0x346F34] = 30 Grad, Radius
 * 52 Zellen = 260u), setzt danach einen Fence ([0x35640C]) und wartet in
 * FUN_000459B0 @0x45B9D per D3DDevice_BlockOnFence darauf, bevor Gelaende
 * (FUN_00025E20, Byte 0) und Objekte (FUN_00021EB0) die Karte lesen - die
 * Auswahl gilt auf der Xbox also fuer DIESES Bild. Der asynchrone W59-Pfad
 * lieferte den Inhalt >= 1 Session spaeter (bei GPU-Rueckstand mehrere):
 * Gelaende wurde fuer eine aeltere Gierung/Position ausgewaehlt und fehlte
 * beim Drehen am vorderen Bildrand (Spiel-Frustum 4:3 = +-29 Grad, Sektor
 * +-30 Grad - kaum Reserve). Mit rtt_rb_sync=1 wird die Kopie am
 * Session-Ende abgesetzt (Flush) und beim naechsten Warten des Spiels auf
 * die GPU (D3D_BlockOnTime, Ziel von BlockOnFence; spaetestens vor der
 * naechsten RT-Session bzw. am Swap) abgeholt und in den Xbox-Speicher
 * geschrieben. ctx_lock wird zwischen den Abfragen abgegeben. */
static int g_rb_sync_c = -1;              /* Slot mit anhaengiger Sync-Kopie */
static unsigned g_rb_sync_host, g_rb_sync_pitch, g_rb_sync_w, g_rb_sync_h;
static LARGE_INTEGER g_rb_sync_t0;
static unsigned g_rb_sess;                 /* DIAG: Sessions der Zellenkarte */
static unsigned g_rb_stage_sess[RTT_MAX];  /* DIAG: Session der Kopie im Staging (Alter) */
static ID3D11Texture2D *g_rb_diag_stage;   /* DIAG: frische Kopie (blockierend) */
/* DIAG(CULL-POPIN,W215): Statistik je rtt_rb_diag Sessions */
static unsigned g_rbd_n, g_rbd_skip, g_rbd_age_sum, g_rbd_age_max, g_rbd_age_n;
static unsigned g_rbd_miss_sum, g_rbd_miss_max, g_rbd_miss_n1, g_rbd_extra_sum, g_rbd_cells_sum;
static double g_rbd_wait_sum, g_rbd_wait_max, g_rbd_lat_sum, g_rbd_lat_max;
static unsigned g_rbd_wait_n, g_rbd_why[4];

/* DIAG(W215): pBits, ueber die das Spiel die Zellenkarte liest (Dauer-
 * LockRect, FUN_0002D320 -> [0x35149C]); 0 = noch nicht gesetzt. */
static unsigned rtt_rb_game_ptr(void)
{
    unsigned v = 0;
    __try { v = *(volatile unsigned *)(uintptr_t)0x35149Cu; } __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
    return v;
}

static void rtt_rb_sync_drop(int idx)
{
    if (g_rb_sync_c == idx) { g_rb_sync_c = -1; }
}

static double rtt_rb_ms_since(const LARGE_INTEGER *t0)
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return f.QuadPart ? (double)(t.QuadPart - t0->QuadPart) * 1000.0 / (double)f.QuadPart : 0.0;
}

/* DIAG: Statistikzeile, sobald rtt_rb_diag Sessions gezaehlt sind (1 = 300). */
static void rtt_rb_diag_flush(void)
{
    int dv = g_cfg.rtt_rb_diag < 0 ? -g_cfg.rtt_rb_diag : g_cfg.rtt_rb_diag; /* < 0: ohne Vergleich */
    unsigned every = (unsigned)(dv > 1 ? dv : 300);
    static unsigned nline;
    unsigned mloop = 0;
    if (!g_cfg.rtt_rb_diag || g_rbd_n < every) { return; }
    __try { mloop = *(volatile unsigned *)(uintptr_t)0x356F58u; } __except (EXCEPTION_EXECUTE_HANDLER) { mloop = 0; }
    if (mloop == 1u && nline < 600) { /* nur laufende Mission */
        nline++;
        HLOG_W("RTTRB f=%u sync=%d n=%u skip=%u alter avg=%.2f max=%u fehlend(B) avg=%.1f max=%u bilder=%u extra(B) avg=%.1f zellen(B) avg=%.0f warten avg=%.3f max=%.3f ms latenz avg=%.3f max=%.3f ms abholung bot/rtt/swap=%u/%u/%u",
               g_present_no, g_cfg.rtt_rb_sync, g_rbd_n, g_rbd_skip,
               g_rbd_age_n ? (double)g_rbd_age_sum / g_rbd_age_n : 0.0, g_rbd_age_max,
               g_rbd_n ? (double)g_rbd_miss_sum / g_rbd_n : 0.0, g_rbd_miss_max, g_rbd_miss_n1,
               g_rbd_n ? (double)g_rbd_extra_sum / g_rbd_n : 0.0,
               g_rbd_n ? (double)g_rbd_cells_sum / g_rbd_n : 0.0,
               g_rbd_wait_n ? g_rbd_wait_sum / g_rbd_wait_n : 0.0, g_rbd_wait_max,
               g_rbd_wait_n ? g_rbd_lat_sum / g_rbd_wait_n : 0.0, g_rbd_lat_max,
               g_rbd_why[1], g_rbd_why[2], g_rbd_why[3]);
    }
    g_rbd_n = g_rbd_skip = g_rbd_age_sum = g_rbd_age_max = g_rbd_age_n = 0;
    g_rbd_miss_sum = g_rbd_miss_max = g_rbd_miss_n1 = g_rbd_extra_sum = g_rbd_cells_sum = 0;
    g_rbd_wait_sum = g_rbd_wait_max = g_rbd_lat_sum = g_rbd_lat_max = 0.0;
    g_rbd_wait_n = 0; g_rbd_why[1] = g_rbd_why[2] = g_rbd_why[3] = 0;
}

static void rtt_rb_diag_stage_ensure(void)
{
    D3D11_TEXTURE2D_DESC dd;
    if (g_rb_diag_stage || !g_dev) { return; }
    memset(&dd, 0, sizeof dd);
    dd.Width = 128; dd.Height = 128; dd.MipLevels = 1; dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_STAGING; dd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &g_rb_diag_stage))) { g_rb_diag_stage = NULL; }
}

/* DIAG(W215): Zellenkarte Byte 0 als BMP (128x128, x4): weiss = frisch und
 * Speicher, rot = nur frisch (fehlend), blau = nur Speicher (veraltet). */
static void rtt_rb_dump_bmp(const unsigned char *cmp)
{
    static unsigned ndump;
    char path[MAX_PATH];
    unsigned char hdr[54];
    unsigned char *row;
    FILE *fp;
    unsigned x, y, k, W = 512u, H = 512u, sz;
    if (ndump >= 8u) { return; }
    ndump++;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s/shots/rtrb_%06u.bmp", hcfg_out_dir(), g_present_no);
    if (fopen_s(&fp, path, "wb") != 0 || !fp) { return; }
    sz = 54u + W * H * 3u;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (unsigned char)sz; hdr[3] = (unsigned char)(sz >> 8); hdr[4] = (unsigned char)(sz >> 16); hdr[5] = (unsigned char)(sz >> 24);
    hdr[10] = 54; hdr[14] = 40;
    hdr[18] = (unsigned char)W; hdr[19] = (unsigned char)(W >> 8);
    hdr[22] = (unsigned char)H; hdr[23] = (unsigned char)(H >> 8);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, fp);
    row = (unsigned char *)malloc(W * 3u);
    if (row) {
        for (y = 0; y < H; y++) {
            unsigned cy = 127u - (y / 4u); /* BMP von unten */
            for (x = 0; x < W; x++) {
                unsigned v = cmp[cy * 128u + x / 4u];
                unsigned char r = 0, g = 0, b = 0;
                if (v == 3u) { r = g = b = 200; }
                else if (v == 1u) { r = 255; }
                else if (v == 2u) { b = 255; g = 96; }
                if (x / 4u == 64u && cy == 64u) { r = 0; g = 255; b = 0; } /* eigene Zelle */
                k = x * 3u; row[k] = b; row[k + 1u] = g; row[k + 2u] = r;
            }
            fwrite(row, 1, W * 3u, fp);
        }
        free(row);
    }
    fclose(fp);
    HLOG_W("RTTRB dump %s", path);
}

/* DIAG: frischen RT-Inhalt (blockierend gelesen) mit dem vergleichen, was die
 * CPU jetzt im Xbox-Speicher sieht. fehlend(B) = Zellen mit Byte 0 frisch != 0,
 * im Speicher 0 = Gelaende/Objekte, die das Spiel faelschlich aussortiert.
 * Nur mit rtt_rb_diag (zusaetzlicher GPU-Stall, aendert das Timing). */
static void rtt_rb_diag_compare(int c, unsigned host, unsigned pitch, unsigned w, unsigned h)
{
    D3D11_TEXTURE2D_DESC dd;
    D3D11_MAPPED_SUBRESOURCE m;
    unsigned x, y, miss = 0, extra = 0, cells = 0;
    static unsigned char cmp[128u * 128u];
    rtt_entry_t *e = &g_rtt[c];
    if (g_cfg.rtt_rb_diag <= 0 || !e->tex || !host || w != 128u || h != 128u) { return; } /* < 0: nur Alter/Skip/Warten, kein Zusatz-Readback */
    (void)dd;
    rtt_rb_diag_stage_ensure();
    sb_ctx_enter();
    if (g_rb_diag_stage) {
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_rb_diag_stage, (ID3D11Resource *)e->tex);
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_rb_diag_stage, 0, D3D11_MAP_READ, 0, &m))) {
            __try {
                for (y = 0; y < h; y++) {
                    const unsigned char *f = (const unsigned char *)m.pData + (size_t)y * m.RowPitch;
                    const unsigned char *o = (const unsigned char *)(uintptr_t)(host + y * pitch);
                    for (x = 0; x < w; x++) {
                        unsigned fb = f[x * 4u], ob = o[x * 4u];
                        if (fb) { cells++; }
                        cmp[y * 128u + x] = (unsigned char)((fb ? 1u : 0u) | (ob ? 2u : 0u));
                        if (fb && !ob) { miss++; }
                        if (!fb && ob) { extra++; }
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                miss = extra = 0;
            }
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_rb_diag_stage, 0);
        }
    }
    sb_ctx_leave();
    g_rbd_miss_sum += miss; g_rbd_extra_sum += extra; g_rbd_cells_sum += cells;
    if (miss > g_rbd_miss_max) { g_rbd_miss_max = miss; }
    if (miss) { g_rbd_miss_n1++; }
    if (miss >= 16u) { rtt_rb_dump_bmp(cmp); }
    if (miss >= 8u) { /* einzelne Session mit vielen fehlenden Zellen (Bild zuordnen) */
        static unsigned nmiss;
        if (nmiss < 300) { nmiss++; HLOG_W("RTTRB miss f=%u fehlend(B)=%u extra(B)=%u zellen(B)=%u", g_present_no, miss, extra, cells); }
    }
}

/* Anhaengige Sync-Kopie abholen (blockierend, ctx_lock zwischen den Abfragen
 * frei) und in den Xbox-Speicher schreiben. why: 1 = BlockOnTime (Spiel wartet
 * auf die GPU, Originalpunkt), 2 = vor der naechsten RT-Session, 3 = Swap. */
void sb_rtt_rb_sync_resolve(int why)
{
    D3D11_MAPPED_SUBRESOURCE m;
    HRESULT hr;
    LARGE_INTEGER tw;
    int c = g_rb_sync_c, spins = 0;
    unsigned y;
    static unsigned nfail;
    if (c < 0) { return; }
    g_rb_sync_c = -1;
    if (c >= g_rtt_n || !g_rtt_stage[c] || !g_ctx) { return; }
    QueryPerformanceCounter(&tw);
    for (;;) {
        sb_ctx_enter();
        hr = ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_rtt_stage[c], 0,
                                     D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr != DXGI_ERROR_WAS_STILL_DRAWING) { break; }
        if (rtt_rb_ms_since(&tw) > 250.0) { /* Sicherung: blockierend */
            hr = ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_rtt_stage[c], 0, D3D11_MAP_READ, 0, &m);
            break;
        }
        sb_ctx_leave();
        /* aktiv warten (wie BlockOnFence auf der Xbox); erst nach 2 ms den Kern
         * abgeben - SwitchToThread auf dem geteilten Xbox-Kern kostete bis 5 ms */
        if (++spins < 64 || rtt_rb_ms_since(&tw) < 2.0) { YieldProcessor(); } else { SwitchToThread(); }
    }
    if (SUCCEEDED(hr)) {
        __try {
            for (y = 0; y < g_rb_sync_h; y++) {
                memcpy((void *)(uintptr_t)(g_rb_sync_host + y * g_rb_sync_pitch),
                       (const unsigned char *)m.pData + (size_t)y * m.RowPitch,
                       (size_t)g_rb_sync_w * 4u);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (nfail < 4) { nfail++; HLOG_W("RTT readback: Ziel 0x%X nicht schreibbar (sync)", g_rb_sync_host); }
        }
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_rtt_stage[c], 0);
    } else if (nfail < 4) {
        nfail++; HLOG_W("RTT readback: Map (sync) fehlgeschlagen (hr=0x%lX)", (unsigned long)hr);
    }
    sb_ctx_leave();
    if (g_cfg.rtt_rb_diag) {
        double wms = rtt_rb_ms_since(&tw), lms = rtt_rb_ms_since(&g_rb_sync_t0);
        g_rbd_wait_sum += wms; if (wms > g_rbd_wait_max) { g_rbd_wait_max = wms; }
        g_rbd_lat_sum += lms; if (lms > g_rbd_lat_max) { g_rbd_lat_max = lms; }
        g_rbd_wait_n++;
        if (why >= 1 && why <= 3) { g_rbd_why[why]++; }
        g_rbd_age_n++; /* Alter 0: Inhalt dieser Session */
        rtt_rb_diag_compare(c, g_rb_sync_host, g_rb_sync_pitch, g_rb_sync_w, g_rb_sync_h);
        rtt_rb_diag_flush();
    }
}

/* FIX(CULL-POPIN,W215) rtt_rb_sync=2: Zellenkarte auf der CPU rastern.
 * Die fuenf Faecher der Karte (FUN_00030190) kommen als IM-Draws
 * (Begin/SetVertexData4f/End) in Pixelkoordinaten 0..128 mit Vertexfarbe in
 * v1; alle Leser pruefen nur Byte != 0 (FUN_00021EB0/22090/22230/25E20).
 * Wir rastern dieselben Dreiecke (D3D11-Regeln: 1/256-Subpixel, Pixelmitte,
 * top-left) in einen CPU-Schatten und schreiben ihn am Session-Ende direkt in
 * den Xbox-Speicher - ohne GPU-Warten. Die GPU zeichnet weiter wie bisher.
 * Passt eine Session nicht ins Muster (andere Draw-Art, Primitiv), gilt fuer
 * sie der GPU-Pfad (rtt_rb_sync=1). DIAG rtt_rb_diag>0 vergleicht den
 * Schatten mit dem frischen GPU-Inhalt (Kanal fuer Kanal, != 0). */
#define RBC_W 128u
static unsigned *g_rbc_buf[2];           /* [0] = Standard, [1] = DIAG anderer Versatz */
static int g_rbc_slot = -1;              /* Slot der Karte (aktuelle Session) */
static unsigned g_rbc_imdraws, g_rbc_bad; /* IM-Draws / nicht abbildbar in der Session */
static unsigned g_rbcd_n, g_rbcd_diff[2], g_rbcd_diffmax[2], g_rbcd_val, g_rbcd_bad, g_rbcd_sess;

static int rbc_candidate(int c)
{
    rtt_entry_t *e;
    unsigned data = 0, fmt = 0, size = 0;
    if (c < 0 || c >= g_rtt_n) { return 0; }
    e = &g_rtt[c];
    if (e->w != RBC_W || e->h != RBC_W || e->isdepth) { return 0; }
    if (!rtt_hdr(e->surf, &data, &fmt, &size)) { return 0; }
    return ((fmt >> 8) & 0xFFu) == 0x1Eu;
}

/* Session-Beginn (Slot wird aktuelles Ziel). */
static void rbc_session_begin(int idx)
{
    g_rbc_slot = -1;
    if (g_cfg.rtt_rb_sync != 2 || !g_cfg.rtt_readback || g_cfg.rtt_readback == 9 || !rbc_candidate(idx)) { return; }
    if (!g_rbc_buf[0]) { g_rbc_buf[0] = (unsigned *)calloc(RBC_W * RBC_W, 4); }
    if (g_cfg.rtt_rb_diag > 0 && !g_rbc_buf[1]) { g_rbc_buf[1] = (unsigned *)calloc(RBC_W * RBC_W, 4); }
    if (!g_rbc_buf[0]) { return; }
    g_rbc_slot = idx;
    g_rbc_imdraws = 0;
    g_rbc_bad = 0;
}

static void rbc_clear(unsigned color)
{
    unsigned k, i;
    for (k = 0; k < 2; k++) {
        if (!g_rbc_buf[k]) { continue; }
        for (i = 0; i < RBC_W * RBC_W; i++) { g_rbc_buf[k][i] = color; }
    }
}

static int rbc_topleft(long long dx, long long dy)
{
    return (dy == 0 && dx > 0) || dy < 0;
}

/* Ein Dreieck; p = x,y in Xbox-Pixeln, col = Bytes B,G,R,A je Vertex. */
static void rbc_tri(unsigned *buf, const float *p0, const float *p1, const float *p2,
                    const unsigned char *c0, const unsigned char *c1, const unsigned char *c2,
                    unsigned mask, int ofs256)
{
    long long X0 = (long long)floor(p0[0] * 256.0 + 0.5) + ofs256, Y0 = (long long)floor(p0[1] * 256.0 + 0.5) + ofs256;
    long long X1 = (long long)floor(p1[0] * 256.0 + 0.5) + ofs256, Y1 = (long long)floor(p1[1] * 256.0 + 0.5) + ofs256;
    long long X2 = (long long)floor(p2[0] * 256.0 + 0.5) + ofs256, Y2 = (long long)floor(p2[1] * 256.0 + 0.5) + ofs256;
    long long area = (X1 - X0) * (Y2 - Y0) - (Y1 - Y0) * (X2 - X0);
    long long minx, maxx, miny, maxy, px, py;
    int tl0, tl1, tl2;
    if (area == 0) { return; }
    if (area < 0) { /* Orientierung angleichen (keine Rueckseiten-Unterdrueckung) */
        long long tx = X1, ty = Y1; const unsigned char *tc = c1;
        X1 = X2; Y1 = Y2; c1 = c2; X2 = tx; Y2 = ty; c2 = tc;
        area = -area;
    }
    tl0 = rbc_topleft(X2 - X1, Y2 - Y1); /* Kante v1->v2 (gegenueber v0) */
    tl1 = rbc_topleft(X0 - X2, Y0 - Y2);
    tl2 = rbc_topleft(X1 - X0, Y1 - Y0);
    minx = X0 < X1 ? X0 : X1; if (X2 < minx) { minx = X2; }
    maxx = X0 > X1 ? X0 : X1; if (X2 > maxx) { maxx = X2; }
    miny = Y0 < Y1 ? Y0 : Y1; if (Y2 < miny) { miny = Y2; }
    maxy = Y0 > Y1 ? Y0 : Y1; if (Y2 > maxy) { maxy = Y2; }
    minx = minx >> 8; maxx = (maxx >> 8) + 1; miny = miny >> 8; maxy = (maxy >> 8) + 1;
    if (minx < 0) { minx = 0; }
    if (miny < 0) { miny = 0; }
    if (maxx > (long long)RBC_W - 1) { maxx = RBC_W - 1; }
    if (maxy > (long long)RBC_W - 1) { maxy = RBC_W - 1; }
    for (py = miny; py <= maxy; py++) {
        long long sy = py * 256 + 128;
        for (px = minx; px <= maxx; px++) {
            long long sx = px * 256 + 128;
            long long w0 = (X2 - X1) * (sy - Y1) - (Y2 - Y1) * (sx - X1);
            long long w1 = (X0 - X2) * (sy - Y2) - (Y0 - Y2) * (sx - X2);
            long long w2 = (X1 - X0) * (sy - Y0) - (Y1 - Y0) * (sx - X0);
            unsigned v, k, out;
            if (w0 < 0 || w1 < 0 || w2 < 0) { continue; }
            if ((w0 == 0 && !tl0) || (w1 == 0 && !tl1) || (w2 == 0 && !tl2)) { continue; }
            out = buf[py * RBC_W + px];
            for (k = 0; k < 4; k++) {
                double f;
                if (!(mask & (0xFFu << (8u * k)))) { continue; }
                f = ((double)w0 * c0[k] + (double)w1 * c1[k] + (double)w2 * c2[k]) / (double)area;
                v = (unsigned)(f + 0.5);
                if (v > 255u) { v = 255u; }
                out = (out & ~(0xFFu << (8u * k))) | (v << (8u * k));
            }
            buf[py * RBC_W + px] = out;
        }
    }
}

/* IM-Draw ins aktuelle Ziel (aus sb_render_im_end_nolock): verts = g_im_buf
 * (n Vertices zu je slots*4 Floats), Position Register 0, Farbe Register colreg
 * (RGBA 0..1, wie sb_render_im_color). */
void sb_rtt_rbc_im(unsigned prim, const float *verts, unsigned n, unsigned slots, int colreg)
{
    unsigned i, k, ntri = 0;
    unsigned char col[3][4];
    const float *v[3];
    if (g_rbc_slot < 0 || g_rbc_slot != g_rtt_cur) { return; }
    g_rbc_imdraws++;
    if (colreg < 0 || (unsigned)colreg >= slots || n < 3u || (prim != 5u && prim != 6u && prim != 7u)) { g_rbc_bad++; return; }
    if (prim == 5u) { ntri = n / 3u; } else { ntri = n - 2u; }
    for (i = 0; i < ntri; i++) {
        unsigned a, b, c, j;
        if (prim == 5u) { a = 3u * i; b = a + 1u; c = a + 2u; }
        else if (prim == 6u) { a = i; b = i + 1u; c = i + 2u; }
        else { a = 0; b = i + 1u; c = i + 2u; }
        v[0] = verts + (size_t)a * slots * 4u;
        v[1] = verts + (size_t)b * slots * 4u;
        v[2] = verts + (size_t)c * slots * 4u;
        for (j = 0; j < 3; j++) {
            const float *cc = v[j] + (size_t)colreg * 4u; /* R,G,B,A */
            float f[4];
            f[0] = cc[2]; f[1] = cc[1]; f[2] = cc[0]; f[3] = cc[3]; /* -> B,G,R,A (Speicherfolge) */
            for (k = 0; k < 4; k++) {
                float t = f[k] * 255.0f + 0.5f;
                col[j][k] = (unsigned char)(t < 0.0f ? 0 : (t > 255.0f ? 255 : (int)t));
            }
        }
        for (k = 0; k < 2; k++) {
            if (!g_rbc_buf[k]) { continue; }
            rbc_tri(g_rbc_buf[k], v[0], v[1], v[2], col[0], col[1], col[2], rs_cmask,
                    k == 0 ? g_cfg.rtt_rb_cpu_ofs : (g_cfg.rtt_rb_cpu_ofs ? 0 : 128));
        }
    }
}

/* Session-Ende: Schatten schreiben, wenn die Session ganz abbildbar war.
 * Rueckgabe 1 = erledigt (kein GPU-Rueckschrieb noetig). */
static int rbc_session_end(int c, unsigned host, unsigned pitch)
{
    rtt_entry_t *e = &g_rtt[c];
    unsigned y;
    static unsigned nlog;
    if (g_rbc_slot != c) { return 0; }
    g_rbc_slot = -1;
    if (g_rbc_bad || g_rbc_imdraws != e->draws) {
        if (nlog < 8) { nlog++; HLOG_W("RTT rbc: Session nicht abbildbar (im=%u draws=%u bad=%u) -> GPU-Rueckschrieb", g_rbc_imdraws, e->draws, g_rbc_bad); }
        if (g_cfg.rtt_rb_diag > 0) { g_rbcd_bad++; }
        return 0;
    }
    __try {
        for (y = 0; y < RBC_W; y++) {
            memcpy((void *)(uintptr_t)(host + y * pitch), g_rbc_buf[0] + y * RBC_W, RBC_W * 4u);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 1;
}

/* DIAG: Schatten gegen frischen GPU-Inhalt (blockierend). */
static void rbc_diag_compare(int c)
{
    D3D11_MAPPED_SUBRESOURCE m;
    unsigned x, y, k, d[2] = {0, 0}, val = 0, dc[4] = {0, 0, 0, 0}, dg[4] = {0, 0, 0, 0};
    static unsigned char cmpb[RBC_W * RBC_W];
    rtt_entry_t *e = &g_rtt[c];
    static unsigned nline, nch;
    if (g_cfg.rtt_rb_diag <= 0 || !g_rbc_buf[0] || !e->tex) { return; }
    rtt_rb_diag_stage_ensure();
    if (!g_rb_diag_stage) { return; }
    sb_ctx_enter();
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_rb_diag_stage, (ID3D11Resource *)e->tex);
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_rb_diag_stage, 0, D3D11_MAP_READ, 0, &m))) {
        for (y = 0; y < RBC_W; y++) {
            const unsigned char *f = (const unsigned char *)m.pData + (size_t)y * m.RowPitch;
            for (x = 0; x < RBC_W; x++) {
                for (k = 0; k < 2; k++) {
                    unsigned s, ch;
                    if (!g_rbc_buf[k]) { continue; }
                    s = g_rbc_buf[k][y * RBC_W + x];
                    for (ch = 0; ch < 4; ch++) {
                        unsigned fb = f[x * 4u + ch], sb = (s >> (8u * ch)) & 0xFFu;
                        if ((fb != 0) != (sb != 0)) {
                            if (ch < 3) { d[k]++; }
                            if (k == 0) { if (sb) { dc[ch]++; } else { dg[ch]++; } }
                        }
                        else if (k == 0 && ch < 3 && fb != sb) { val++; }
                        if (k == 0 && ch == 0) { cmpb[y * RBC_W + x] = (unsigned char)((fb ? 2u : 0u) | (sb ? 1u : 0u)); }
                    }
                }
            }
        }
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_rb_diag_stage, 0);
    }
    sb_ctx_leave();
    if (d[0] && nch < 40) {
        nch++;
        HLOG_W("RTTRB cpu abw f=%u nur_cpu B/G/R/A=%u/%u/%u/%u nur_gpu B/G/R/A=%u/%u/%u/%u", g_present_no,
               dc[0], dc[1], dc[2], dc[3], dg[0], dg[1], dg[2], dg[3]);
        if (dc[0] + dg[0] >= 16u) { rtt_rb_dump_bmp(cmpb); } /* B: weiss beide, rot nur CPU, blau nur GPU */
    }
    g_rbcd_n++;
    for (k = 0; k < 2; k++) {
        g_rbcd_diff[k] += d[k];
        if (d[k] > g_rbcd_diffmax[k]) { g_rbcd_diffmax[k] = d[k]; }
    }
    g_rbcd_val += val;
    if (g_rbcd_n >= (unsigned)(g_cfg.rtt_rb_diag > 1 ? g_cfg.rtt_rb_diag : 300)) {
        if (nline < 600) {
            nline++;
            HLOG_W("RTTRB cpu f=%u n=%u abdeckung_abweichend(Kanal-Zellen) versatz%d=%u (max %u) versatz%d=%u (max %u) wertabw=%u nicht_abbildbar=%u",
                   g_present_no, g_rbcd_n, g_cfg.rtt_rb_cpu_ofs, g_rbcd_diff[0], g_rbcd_diffmax[0],
                   g_cfg.rtt_rb_cpu_ofs ? 0 : 128, g_rbcd_diff[1], g_rbcd_diffmax[1], g_rbcd_val, g_rbcd_bad);
        }
        g_rbcd_n = 0; g_rbcd_diff[0] = g_rbcd_diff[1] = g_rbcd_diffmax[0] = g_rbcd_diffmax[1] = 0;
        g_rbcd_val = 0; g_rbcd_bad = 0;
    }
}

static void rtt_readback(int c)
{
    rtt_entry_t *e;
    unsigned data = 0, fmt = 0, size = 0, xf, host, pitch, y;
    D3D11_TEXTURE2D_DESC dd;
    D3D11_MAPPED_SUBRESOURCE m;
    HRESULT hr;
    static unsigned nlog, nfail;
    if (c < 0 || c >= g_rtt_n || !g_dev || !g_ctx) { return; }
    e = &g_rtt[c];
    if (!e->tex || (!e->draws && !e->clears)) { return; }
    if (!rtt_hdr(e->surf, &data, &fmt, &size)) { return; }
    xf = (fmt >> 8) & 0xFFu;
    if (xf != 0x12u && xf != 0x1Eu) { return; } /* nur LIN_A8R8G8B8 / LIN_X8R8G8B8 */
    if ((g_cfg.rtt_readback == 1 || g_cfg.rtt_readback == 9) && xf != 0x1Eu) { return; } /* 1: nur X8R8G8B8 (Zellenkarte); 9 = DIAG wie 1 ohne Schrieb */
    if (g_cfg.rtt_readback <= 2 && (e->w > 128u || e->h > 128u)) { return; }
    if (e->w > 512u || e->h > 512u) { return; }
    if (e->pw != e->w || e->ph != e->h) { return; } /* W88: skaliertes RT nie per CPU (rtt_scale_for schliesst das aus) */
    host = krnl_phys_to_virt_rb(data);
    if (!host) { host = data; }
    pitch = size ? ((((size >> 24) & 0xFFu) + 1u) * 64u) : e->w * 4u;
    if (pitch < e->w * 4u) { pitch = e->w * 4u; }
    if (!g_rtt_stage[c]) {
        memset(&dd, 0, sizeof dd);
        dd.Width = e->w; dd.Height = e->h;
        dd.MipLevels = 1; dd.ArraySize = 1;
        dd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        dd.SampleDesc.Count = 1;
        dd.Usage = D3D11_USAGE_STAGING;
        dd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &g_rtt_stage[c]);
        if (FAILED(hr) || !g_rtt_stage[c]) {
            g_rtt_stage[c] = NULL;
            if (nfail < 4) { nfail++; HLOG_W("RTT readback: Staging fehlgeschlagen (hr=0x%lX)", (unsigned long)hr); }
            return;
        }
    }
    /* FIX(CULL-POPIN,W215): Zellenkarte (X8R8G8B8 bis 128x128) synchron -
     * Kopie jetzt absetzen, Abholung am naechsten BlockOnTime des Spiels. */
    if (g_cfg.rtt_rb_sync && g_cfg.rtt_readback != 9 && xf == 0x1Eu && e->w <= 128u && e->h <= 128u) {
        if (g_cfg.rtt_rb_sync == 2 && rbc_session_end(c, host, pitch)) { /* CPU-Raster: kein GPU-Warten */
            if (g_cfg.rtt_rb_diag) {
                g_rb_sess++; g_rbd_n++; g_rbd_age_n++;
                rbc_diag_compare(c);
                rtt_rb_diag_compare(c, host, pitch, e->w, e->h);
                rtt_rb_diag_flush();
            }
            return;
        }
        if (g_rb_sync_c >= 0) { sb_rtt_rb_sync_resolve(2); }
        sb_ctx_enter();
        g_rtt_stage_pend[c] = 0; /* evtl. alte asynchrone Kopie verwerfen */
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_rtt_stage[c],
                                         (ID3D11Resource *)e->tex);
        ID3D11DeviceContext_Flush(g_ctx);
        sb_ctx_leave();
        if (e->w == 128u && e->h == 128u) { /* W215: liest das Spiel dort, wohin wir schreiben? */
            static unsigned nmis;
            unsigned gp = rtt_rb_game_ptr();
            if (gp && gp != host && nmis < 4) { nmis++; HLOG_W("RTT readback: Spiel liest Zellenkarte bei 0x%X, Rueckschrieb nach 0x%X (surf=0x%X)", gp, host, e->surf); }
        }
        g_rb_sync_c = c;
        g_rb_sync_host = host; g_rb_sync_pitch = pitch;
        g_rb_sync_w = e->w; g_rb_sync_h = e->h;
        QueryPerformanceCounter(&g_rb_sync_t0);
        if (g_cfg.rtt_rb_diag) { g_rb_sess++; g_rbd_n++; }
        if (nlog < 8) {
            nlog++;
            HLOG_I("RTT readback #%u: surf=0x%X data=0x%X host=0x%X %ux%u fmt=0x%02X pitch=%u draws=%u f=%u (sync) spiel=0x%X",
                   nlog, e->surf, data, host, e->w, e->h, xf, pitch, e->draws, g_present_no, rtt_rb_game_ptr());
        }
        return;
    }
    /* Verzoegert um eine Session (kein GPU-Stall): erst die Kopie der
     * vorigen Session abholen (laengst fertig), dann neu kopieren. Die
     * CPU sieht den RT-Inhalt damit ein Bild spaeter - fuer die Zellen-
     * karte (Lock-On/Auto-Ziel) unkritisch. */
    sb_ctx_enter();
    if (g_rtt_stage_pend[c]) {
        /* Nicht blockieren: ist die GPU mit der letzten Kopie noch nicht
         * fertig, diese Session auslassen (Kopie bleibt anhaengig). Haelt
         * die CPU/GPU-Parallelitaet (W59_gold3: synchroner Pfad kostete im
         * Menue-Zeitraffer ~40 % FPS). */
        hr = ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_rtt_stage[c], 0,
                                     D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            sb_ctx_leave();
            if (g_cfg.rtt_rb_diag && xf == 0x1Eu) { /* DIAG(W215): Speicher bleibt noch aelter */
                g_rb_sess++; g_rbd_n++; g_rbd_skip++;
                rtt_rb_diag_compare(c, g_rtt_stage_host[c], g_rtt_stage_pitch[c], e->w, e->h);
                rtt_rb_diag_flush();
            }
            return;
        }
        if (SUCCEEDED(hr) && g_rtt_stage_host[c] != host) {
            /* Surface hat inzwischen anderen Speicher (Neuanlage beim Laden):
             * alte Kopie verwerfen, nie in freigegebenen Speicher schreiben. */
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_rtt_stage[c], 0);
            hr = E_FAIL;
        }
        if (SUCCEEDED(hr)) {
            unsigned h0 = g_rtt_stage_host[c], p0 = g_rtt_stage_pitch[c];
            if (g_cfg.rtt_readback == 9) { h0 = 0; } /* DIAG: Mechanik ohne Schrieb */
            if (h0) __try {
                for (y = 0; y < e->h; y++) {
                    memcpy((void *)(uintptr_t)(h0 + y * p0),
                           (const unsigned char *)m.pData + (size_t)y * m.RowPitch,
                           (size_t)e->w * 4u);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                if (nfail < 4) { nfail++; HLOG_W("RTT readback: Ziel 0x%X nicht schreibbar", h0); }
            }
            /* DIAG(WORLDVIEW,W65): Zellenkarte im Zensus-Frame
             * je Kanal zaehlen (Byte 0 = B = FUN_00021EB0, Byte 1/2 = G/R =
             * FUN_00022090/22230): Zellen != 0 und Maximalwert. */
            if (g_cfg.wv_diag && m3d_on && xf == 0x1Eu) {
                unsigned nb = 0, ng = 0, nr = 0, mb = 0, mg = 0, mr = 0, x;
                for (y = 0; y < e->h; y++) {
                    const unsigned char *s = (const unsigned char *)m.pData + (size_t)y * m.RowPitch;
                    for (x = 0; x < e->w; x++) {
                        unsigned b = s[x * 4u], g = s[x * 4u + 1u], r = s[x * 4u + 2u];
                        if (b) { nb++; if (b > mb) { mb = b; } }
                        if (g) { ng++; if (g > mg) { mg = g; } }
                        if (r) { nr++; if (r > mr) { mr = r; } }
                    }
                }
                HLOG_W("WVCELL f=%u %ux%u b=%u(max %u) g=%u(max %u) r=%u(max %u)",
                       g_present_no, e->w, e->h, nb, mb, ng, mg, nr, mr);
            }
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_rtt_stage[c], 0);
            if (g_cfg.rtt_rb_diag && xf == 0x1Eu && h0) { /* DIAG(W215): Alter + fehlende Zellen */
                unsigned age;
                g_rb_sess++; g_rbd_n++;
                age = g_rb_sess - g_rb_stage_sess[c];
                g_rbd_age_sum += age; g_rbd_age_n++;
                if (age > g_rbd_age_max) { g_rbd_age_max = age; }
                sb_ctx_leave();
                rtt_rb_diag_compare(c, h0, p0, e->w, e->h);
                rtt_rb_diag_flush();
                sb_ctx_enter();
            }
        }
    } else if (g_cfg.rtt_rb_diag && xf == 0x1Eu) {
        g_rb_sess++; g_rbd_n++; /* erste Session: noch nichts im Speicher */
    }
    g_rb_stage_sess[c] = g_rb_sess;
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_rtt_stage[c],
                                     (ID3D11Resource *)e->tex);
    g_rtt_stage_pend[c] = 1;
    g_rtt_stage_host[c] = host;
    g_rtt_stage_pitch[c] = pitch;
    sb_ctx_leave();
    if (nlog < 8) {
        nlog++;
        HLOG_I("RTT readback #%u: surf=0x%X data=0x%X host=0x%X %ux%u fmt=0x%02X pitch=%u draws=%u f=%u spiel=0x%X",
               nlog, e->surf, data, host, e->w, e->h, xf, pitch, e->draws, g_present_no, rtt_rb_game_ptr());
    }
}

static void rtt_keep_release_idx(int idx); /* W165 rt_scale_dyn_keep */
static void rtt_release_entry(int idx)
{
    rtt_entry_t *e = &g_rtt[idx];
    rtt_keep_release_idx(idx);
    if (g_rtt_stage[idx]) { ID3D11Texture2D_Release(g_rtt_stage[idx]); g_rtt_stage[idx] = NULL; }
    g_rtt_stage_pend[idx] = 0;
    rtt_rb_sync_drop(idx); /* W215 */
    if (e->srv) { ID3D11ShaderResourceView_Release(e->srv); }
    if (e->rtv) { ID3D11RenderTargetView_Release(e->rtv); }
    if (e->dsv) { ID3D11DepthStencilView_Release(e->dsv); }
    if (e->dsrv) { ID3D11ShaderResourceView_Release(e->dsrv); }
    if (e->tex) { ID3D11Texture2D_Release(e->tex); }
    { unsigned k; for (k = 0; k < RTT_SCOPY; k++) { /* W106 rtt_selfcopy */
        if (e->csrv[k]) { ID3D11ShaderResourceView_Release(e->csrv[k]); }
        if (e->ctex[k]) { ID3D11Texture2D_Release(e->ctex[k]); } } }
    memset(e, 0, sizeof *e);
}

/* D3D11-Objekte zum Eintrag anlegen (einmalig). 1 = ok. */
static int rtt_ensure(int idx)
{
    D3D11_TEXTURE2D_DESC dd;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd;
    ID3D11Texture2D *ds = NULL;
    HRESULT hr;
    static unsigned nlog;
    rtt_entry_t *e = &g_rtt[idx];
    if (e->tex) { return 1; }
    if (!g_dev || !e->w || !e->h) { return 0; }
    if (!e->pw || !e->ph) { e->pw = e->w; e->ph = e->h; }
    memset(&dd, 0, sizeof dd);
    dd.Width = e->pw; dd.Height = e->ph;
    dd.MipLevels = 1; dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &e->tex);
    if (FAILED(hr) || !e->tex) { return 0; }
    hr = ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)e->tex,
                                             NULL, &e->rtv);
    if (FAILED(hr) || !e->rtv) { return 0; }
    memset(&vd, 0, sizeof vd);
    vd.Format = dd.Format;
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipLevels = 1;
    hr = ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)e->tex,
                                               &vd, &e->srv);
    if (FAILED(hr) || !e->srv) { return 0; }
    memset(&dd, 0, sizeof dd);
    dd.Width = e->pw; dd.Height = e->ph;
    dd.MipLevels = 1; dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (e->isdepth) { /* RENDER-QA3 (W100): Schattenpuffer auch als SRV */
        dd.Format = DXGI_FORMAT_R24G8_TYPELESS;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    }
    hr = ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &ds);
    if (FAILED(hr) || !ds) { return 0; }
    if (e->isdepth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dvd;
        memset(&dvd, 0, sizeof dvd);
        dvd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        hr = ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource *)ds, &dvd, &e->dsv);
        if (SUCCEEDED(hr) && e->dsv) {
            memset(&vd, 0, sizeof vd);
            vd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MipLevels = 1;
            hr = ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)ds, &vd, &e->dsrv);
        }
    } else {
        hr = ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource *)ds,
                                                 NULL, &e->dsv);
    }
    ID3D11Texture2D_Release(ds);
    if (FAILED(hr) || !e->dsv) { return 0; }
    if (nlog < (unsigned)RTT_MAX) {
        nlog++;
        if (e->pw != e->w || e->ph != e->h) {
            HLOG_I("RTT anlegen #%u: surf=0x%X data=0x%X %ux%u (W88 rt_scale: D3D11 %ux%u)", nlog, e->surf,
                   e->data, e->w, e->h, e->pw, e->ph);
        } else {
            HLOG_I("RTT anlegen #%u: surf=0x%X data=0x%X %ux%u", nlog, e->surf,
                   e->data, e->w, e->h);
        }
    }
    return 1;
}

/* W88 RENDER-RES: RT, dessen Inhalt per CPU hochgeladen wird (DLGTEXT,
 * UpdateSubresource in Xbox-Pixeln), auf Xbox-Groesse zuruecksetzen. GPU-
 * Inhalt geht dabei einmal verloren (der Upload ueberschreibt ihn). 1 = ok. */
int rtt_unscale(int idx)
{
    rtt_entry_t *e;
    static unsigned nlog;
    if (idx < 0 || idx >= g_rtt_n) { return 0; }
    e = &g_rtt[idx];
    if (e->pw == e->w && e->ph == e->h) { return 1; }
    if (g_rtt_bound == idx) { rtt_bind_bb(); }
    if (g_rtt_stage[idx]) { ID3D11Texture2D_Release(g_rtt_stage[idx]); g_rtt_stage[idx] = NULL; }
    g_rtt_stage_pend[idx] = 0;
    rtt_rb_sync_drop(idx); /* W215 */
    if (e->srv) { ID3D11ShaderResourceView_Release(e->srv); e->srv = NULL; }
    if (e->rtv) { ID3D11RenderTargetView_Release(e->rtv); e->rtv = NULL; }
    if (e->dsv) { ID3D11DepthStencilView_Release(e->dsv); e->dsv = NULL; }
    if (e->tex) { ID3D11Texture2D_Release(e->tex); e->tex = NULL; }
    e->pw = e->w; e->ph = e->h;
    if (nlog < 8) { nlog++; HLOG_I("RTT unscale #%u: slot %d surf=0x%X %ux%u (CPU-Upload)", nlog, idx, e->surf, e->w, e->h); }
    return rtt_ensure(idx);
}

/* ---------- W165 GPU-COST2: rt_scale_dyn_keep - Stufenwechsel ohne RT-Neuanlage ----------
 * Befund V_C25p: Jeder Wechsel legte alle skalierten RTs neu an (CreateTexture2D
 * bis 4096^2 + Tiefe je RT im Spiel-Thread unter ctx_lock) -> Tick bis 177 ms,
 * Bildspitzen. Mit keep bleiben die D3D11-Objekte je Faktor erhalten (Satz je
 * Eintrag und Faktor 1..4). Fehlt fuer den Zielfaktor ein Satz, legt ein
 * Hilfs-Thread ihn ohne ctx_lock an (das Device ist frei-threaded); der Wechsel
 * wartet so lange (-2). Fertige Saetze werden am Swap-Anfang uebernommen und
 * einmal geloescht (Speicher zugeteilt, bevor der Wechsel sie braucht). Der
 * Wechsel selbst tauscht nur Zeiger. Logische Groesse/Abbildung unveraendert. */
typedef struct {
    ID3D11Texture2D *tex;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
    ID3D11DepthStencilView *dsv;
    ID3D11ShaderResourceView *dsrv;
    ID3D11Texture2D *ctex[RTT_SCOPY];
    ID3D11ShaderResourceView *csrv[RTT_SCOPY];
    unsigned pw, ph, cring;
    int ok;
} rtt_set_t;
static rtt_set_t g_rtt_keep[RTT_MAX][5];
typedef struct { int idx; unsigned surf, w, h, n; int isdepth; rtt_set_t s; } rtt_job_t;
static rtt_job_t g_kj[RTT_MAX];
static int g_kj_n;
static volatile LONG g_kj_state; /* 0 frei, 1 Hilfs-Thread arbeitet, 2 fertig (Uebernahme offen) */
static unsigned g_keep_made, g_keep_swaps, g_keep_jobs;
static double g_keep_job_ms;

static void rtt_set_release(rtt_set_t *s)
{
    unsigned k;
    if (s->srv) { ID3D11ShaderResourceView_Release(s->srv); }
    if (s->rtv) { ID3D11RenderTargetView_Release(s->rtv); }
    if (s->dsv) { ID3D11DepthStencilView_Release(s->dsv); }
    if (s->dsrv) { ID3D11ShaderResourceView_Release(s->dsrv); }
    if (s->tex) { ID3D11Texture2D_Release(s->tex); }
    for (k = 0; k < RTT_SCOPY; k++) {
        if (s->csrv[k]) { ID3D11ShaderResourceView_Release(s->csrv[k]); }
        if (s->ctex[k]) { ID3D11Texture2D_Release(s->ctex[k]); }
    }
    memset(s, 0, sizeof *s);
}

static void rtt_keep_release_idx(int idx)
{
    unsigned n;
    if (idx < 0 || idx >= RTT_MAX) { return; }
    for (n = 0; n < 5u; n++) { if (g_rtt_keep[idx][n].ok) { rtt_set_release(&g_rtt_keep[idx][n]); } }
}

/* Objekte wie rtt_ensure, aber in einen Satz (Hilfs-Thread, ohne ctx_lock). */
static int rtt_set_create(rtt_set_t *s, unsigned pw, unsigned ph, int isdepth)
{
    D3D11_TEXTURE2D_DESC dd;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd;
    ID3D11Texture2D *ds = NULL;
    HRESULT hr;
    memset(s, 0, sizeof *s);
    memset(&dd, 0, sizeof dd);
    dd.Width = pw; dd.Height = ph; dd.MipLevels = 1; dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT; dd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &s->tex)) || !s->tex) { goto fail; }
    if (FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)s->tex, NULL, &s->rtv)) || !s->rtv) { goto fail; }
    memset(&vd, 0, sizeof vd);
    vd.Format = dd.Format; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)s->tex, &vd, &s->srv)) || !s->srv) { goto fail; }
    memset(&dd, 0, sizeof dd);
    dd.Width = pw; dd.Height = ph; dd.MipLevels = 1; dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT; dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (isdepth) { dd.Format = DXGI_FORMAT_R24G8_TYPELESS; dd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE; }
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &ds)) || !ds) { goto fail; }
    if (isdepth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dvd;
        memset(&dvd, 0, sizeof dvd);
        dvd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        hr = ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource *)ds, &dvd, &s->dsv);
        if (SUCCEEDED(hr) && s->dsv) {
            memset(&vd, 0, sizeof vd);
            vd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
            hr = ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)ds, &vd, &s->dsrv);
        }
    } else {
        hr = ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource *)ds, NULL, &s->dsv);
    }
    ID3D11Texture2D_Release(ds);
    if (FAILED(hr) || !s->dsv) { goto fail; }
    s->pw = pw; s->ph = ph; s->ok = 1;
    return 1;
fail:
    rtt_set_release(s);
    return 0;
}

static DWORD WINAPI rtt_keep_worker(LPVOID p)
{
    LARGE_INTEGER a, b, f;
    int i;
    (void)p;
    QueryPerformanceCounter(&a);
    for (i = 0; i < g_kj_n; i++) {
        rtt_job_t *j = &g_kj[i];
        rtt_set_create(&j->s, j->w * j->n, j->h * j->n, j->isdepth);
    }
    QueryPerformanceCounter(&b);
    QueryPerformanceFrequency(&f);
    g_keep_job_ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
    MemoryBarrier();
    InterlockedExchange(&g_kj_state, 2);
    return 0;
}

static int rtt_is_scaled(const rtt_entry_t *e)
{
    return e->surf && e->w && e->h && e->pw != e->w;
}

/* Spiel-Thread unter ctx_lock: fertige Saetze uebernehmen (und einmal loeschen). */
static void rtt_keep_install(void)
{
    int i;
    static const float z4[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (g_kj_state != 2) { return; }
    MemoryBarrier();
    for (i = 0; i < g_kj_n; i++) {
        rtt_job_t *j = &g_kj[i];
        rtt_entry_t *e = (j->idx >= 0 && j->idx < g_rtt_n) ? &g_rtt[j->idx] : NULL;
        if (!j->s.ok) { continue; }
        if (!e || e->surf != j->surf || e->w != j->w || e->h != j->h || g_rtt_keep[j->idx][j->n].ok ||
            e->pw == j->w * j->n) {
            rtt_set_release(&j->s); /* Eintrag inzwischen anders belegt bzw. Satz schon vorhanden */
            continue;
        }
        ID3D11DeviceContext_ClearRenderTargetView(g_ctx, j->s.rtv, z4);
        ID3D11DeviceContext_ClearDepthStencilView(g_ctx, j->s.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        g_rtt_keep[j->idx][j->n] = j->s;
        memset(&j->s, 0, sizeof j->s);
        g_keep_made++;
    }
    g_kj_n = 0;
    InterlockedExchange(&g_kj_state, 0);
}

/* Fehlende Saetze fuer Faktor n beim Hilfs-Thread bestellen. Rueckgabe: Zahl fehlender. */
static int rtt_keep_request(unsigned n)
{
    int i, miss = 0;
    rtt_keep_install();
    for (i = 0; i < g_rtt_n; i++) {
        rtt_entry_t *e = &g_rtt[i];
        if (!rtt_is_scaled(e) || e->pw == e->w * n || g_rtt_keep[i][n].ok) { continue; }
        miss++;
    }
    if (!miss || g_kj_state != 0) { return miss; }
    g_kj_n = 0;
    for (i = 0; i < g_rtt_n && g_kj_n < RTT_MAX; i++) {
        rtt_entry_t *e = &g_rtt[i];
        rtt_job_t *j;
        if (!rtt_is_scaled(e) || e->pw == e->w * n || g_rtt_keep[i][n].ok) { continue; }
        j = &g_kj[g_kj_n++];
        memset(j, 0, sizeof *j);
        j->idx = i; j->surf = e->surf; j->w = e->w; j->h = e->h; j->n = n; j->isdepth = e->isdepth;
    }
    if (g_kj_n) {
        HANDLE th;
        InterlockedExchange(&g_kj_state, 1);
        g_keep_jobs++;
        th = CreateThread(NULL, 0, rtt_keep_worker, NULL, 0, NULL);
        if (th) { SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(th); }
        else { g_kj_n = 0; InterlockedExchange(&g_kj_state, 0); }
    }
    return miss;
}

/* Vorab bestellen (eine Stufe unter dem aktuellen Faktor), ohne zu wechseln.
 * Spiel-Thread unter ctx_lock (Swap-Anfang). */
void sb_rtt_keep_prefetch(unsigned n)
{
    if (n < 1u || n > 4u || g_cfg.rt_scale_dyn_keep <= 0 || g_rtt_bound >= 0) { return; }
    (void)rtt_keep_request(n);
}

static void rtt_entry_to_set(rtt_entry_t *e, rtt_set_t *s)
{
    unsigned k;
    s->tex = e->tex; s->rtv = e->rtv; s->srv = e->srv; s->dsv = e->dsv; s->dsrv = e->dsrv;
    for (k = 0; k < RTT_SCOPY; k++) { s->ctex[k] = e->ctex[k]; s->csrv[k] = e->csrv[k]; e->ctex[k] = NULL; e->csrv[k] = NULL; }
    s->pw = e->pw; s->ph = e->ph; s->cring = e->cring; s->ok = 1;
    e->tex = NULL; e->rtv = NULL; e->srv = NULL; e->dsv = NULL; e->dsrv = NULL;
}

static void rtt_set_to_entry(rtt_set_t *s, rtt_entry_t *e)
{
    unsigned k;
    e->tex = s->tex; e->rtv = s->rtv; e->srv = s->srv; e->dsv = s->dsv; e->dsrv = s->dsrv;
    for (k = 0; k < RTT_SCOPY; k++) { e->ctex[k] = s->ctex[k]; e->csrv[k] = s->csrv[k]; }
    e->pw = s->pw; e->ph = s->ph; e->cring = s->cring;
    e->cgen = 0; /* Kopien veraltet */
    memset(s, 0, sizeof *s);
}

static int rtt_rescale_keep(unsigned n)
{
    int i, cnt = 0, miss;
    miss = rtt_keep_request(n);
    if (miss) { return -2; } /* Saetze entstehen noch im Hilfs-Thread */
    for (i = 0; i < g_rtt_n; i++) {
        rtt_entry_t *e = &g_rtt[i];
        unsigned cur;
        if (!rtt_is_scaled(e) || e->pw == e->w * n || !e->tex || !g_rtt_keep[i][n].ok) { continue; }
        cur = e->pw / e->w;
        if (cur < 1u || cur > 4u || e->pw != e->w * cur) { continue; }
        if (g_rtt_stage[i]) { ID3D11Texture2D_Release(g_rtt_stage[i]); g_rtt_stage[i] = NULL; }
        g_rtt_stage_pend[i] = 0;
        rtt_rb_sync_drop(i); /* W215 */
        if (g_rtt_keep[i][cur].ok) { rtt_set_release(&g_rtt_keep[i][cur]); }
        rtt_entry_to_set(e, &g_rtt_keep[i][cur]);
        rtt_set_to_entry(&g_rtt_keep[i][n], e);
        cnt++;
    }
    g_rt_dyn = n;
    g_keep_swaps++;
    return cnt;
}

/* Statistik fuer die RT-DYN-Zeilen (W165). */
void sb_rtt_keep_stat(unsigned *made, unsigned *jobs, unsigned *swaps, double *job_ms)
{
    *made = g_keep_made; *jobs = g_keep_jobs; *swaps = g_keep_swaps; *job_ms = g_keep_job_ms;
}

/* Alle skalierten RTs auf Faktor n neu anlegen (Spiel-Thread, Swap-Anfang, BB gebunden).
 * Aufzeichnungen der Zwischenbilder halten die alten Texturen per AddRef (Inhalt
 * bleibt fuer die laufenden Paare gueltig); neue Ticks zeichnen in die neuen. */
int sb_rtt_rescale(unsigned n)
{
    int i, cnt = 0;
    if (n < 1u || n > 4u || g_rtt_bound >= 0) { return -1; }
    if (g_cfg.rt_scale_dyn_keep > 0) { return rtt_rescale_keep(n); } /* W165: ohne Neuanlage (-2 = Saetze noch nicht fertig) */
    for (i = 0; i < g_rtt_n; i++) {
        rtt_entry_t *e = &g_rtt[i];
        unsigned k;
        if (!e->surf || !e->w || !e->h || e->pw == e->w || e->pw == e->w * n) { continue; }
        if (g_rtt_stage[i]) { ID3D11Texture2D_Release(g_rtt_stage[i]); g_rtt_stage[i] = NULL; }
        g_rtt_stage_pend[i] = 0;
        rtt_rb_sync_drop(i); /* W215 */
        if (e->srv) { ID3D11ShaderResourceView_Release(e->srv); e->srv = NULL; }
        if (e->rtv) { ID3D11RenderTargetView_Release(e->rtv); e->rtv = NULL; }
        if (e->dsv) { ID3D11DepthStencilView_Release(e->dsv); e->dsv = NULL; }
        if (e->dsrv) { ID3D11ShaderResourceView_Release(e->dsrv); e->dsrv = NULL; }
        if (e->tex) { ID3D11Texture2D_Release(e->tex); e->tex = NULL; }
        for (k = 0; k < RTT_SCOPY; k++) {
            if (e->csrv[k]) { ID3D11ShaderResourceView_Release(e->csrv[k]); e->csrv[k] = NULL; }
            if (e->ctex[k]) { ID3D11Texture2D_Release(e->ctex[k]); e->ctex[k] = NULL; }
        }
        e->cgen = 0;
        e->pw = e->w * n;
        e->ph = e->h * n;
        rtt_ensure(i);
        cnt++;
    }
    g_rt_dyn = n;
    return cnt;
}

/* Session-Ende (Rueckschaltung): Statistik loggen (deckelt), Session zu. */
static void rtt_session_end(unsigned surf, const char *why)
{
    static unsigned nend;
    g_rtt_dummy = 0; /* FIX(MONITORVIEW,W36): Session-Ende = kein Dummy */
    int c = g_rtt_cur;
    if (c < 0 || c >= g_rtt_n) { g_rtt_cur = -1; rtt_bind_bb(); return; }
    if (g_cfg.rtt && g_cfg.rtt_readback) {
        rtt_readback(c); /* FIX(COMBAT,W59): CPU sieht RT-Inhalt (Zellenkarte) */
    }
    /* DIAG(WORLDVIEW,W65): wv_diag=1 -> fertiges RT (>= 512 px,
     * Hauptmonitor-Welt RT31 / S40) am Session-Ende im Zensus-Frame dumpen
     * (monslot_<f>_9xx_s<slot>.jpg, Kappe wie MONDUMP). */
    if (g_cfg.wv_diag == 1 && m3d_on && g_rtt[c].w >= 512u && g_rtt[c].draws) {
        mon_dump_slot(c, g_present_no, 900u + (unsigned)c, 0);
    }
    if (nend < 8 || ((g_rtt[c].draws || g_rtt[c].clears) && g_rtt[c].nlog < 3)) {
        if (nend < 80) {
        nend++; g_rtt[c].nlog++;
        HLOG_I("RTT sess #%u: slot %d surf=0x%X %s f=%u draws=%u clears=%u vp=%ux%u clr=0x%X/0x%X d0=%u/%u/%u/0x%X -> 0x%X",
               nend, c, g_rtt[c].surf, why, g_present_no, g_rtt[c].draws,
               g_rtt[c].clears, g_rtt[c].vpw, g_rtt[c].vph, g_rtt[c].clr, g_rtt[c].clrflags, g_rtt[c].fprim, g_rtt[c].fn, g_rtt[c].frhw, g_rtt[c].fxptr, surf);
        }
    }
    g_rtt_cur = -1;
    rtt_bind_bb();
}

void sb_rtt_on_set_target(unsigned surf, unsigned depth, unsigned caller)
{
    static unsigned seen[64], nseen;
    unsigned data = 0, fmt = 0, size = 0, w = 0, h = 0;
    unsigned k;
    int idx;
    if (m3d_on) { /* DIAG(FX-HOLE,W106): Zielwechsel im Zensus-Frame */
        unsigned hd3 = 0, hd1 = 0;
        __try { if (surf) { hd1 = *(unsigned *)(uintptr_t)(surf + 4u); hd3 = *(unsigned *)(uintptr_t)(surf + 12u); } }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        HLOG_W("RTSET f=%u i=%u surf=0x%X depth=0x%X caller=0x%X bb=%d data=0x%X fmt=0x%08X slot=%d",
               g_present_no, m3d_ndraw, surf, depth, caller, (!surf || rtt_is_bb(surf)) ? 1 : 0, hd1, hd3,
               surf ? rtt_find(surf) : -1);
    }
    if (!surf || rtt_is_bb(surf)) {
        rtt_session_end(surf, "BB");
        return;
    }
    if (!rtt_hdr(surf, &data, &fmt, &size) && g_cfg.shading && g_cfg.shade_shadow && g_cfg.rtt && depth &&
        rtt_hdr(depth, &data, &fmt, &size) && ((fmt >> 8) & 0xFFu) >= 0x2Au && ((fmt >> 8) & 0xFFu) <= 0x31u &&
        rtt_dims(fmt, size, &w, &h)) {
        /* RENDER-QA3 (W100, shade_shadow): Tiefenpass in einen Schattenpuffer
         * (FUN_00031590: Dummy-Farbflaeche Data=0 + 2048^2 LIN_F16-Tiefe,
         * BERICHT W36). Die Tiefenflaeche wird ein eigenes RT (Schluessel =
         * Tiefen-Surface), ihre Tiefe sampelt der Shading-Pfad als Stufe 1
         * (sb_rtt_depth_srv). Sonst wie bisher (Dummy-Tiefenpass). */
        surf = depth;
        idx = rtt_find(surf);
        if (idx >= 0 && !g_rtt[idx].isdepth) { idx = -1; }
        if (idx < 0) {
            if (g_rtt_n < RTT_MAX) { idx = g_rtt_n++; }
            else { goto shadow_fail; }
            memset(&g_rtt[idx], 0, sizeof g_rtt[idx]);
            g_rtt[idx].surf = surf; g_rtt[idx].data = data;
            g_rtt[idx].w = w; g_rtt[idx].h = h; g_rtt[idx].pw = w; g_rtt[idx].ph = h;
            g_rtt[idx].isdepth = 1;
            { /* z-Bereich der Xbox-Tiefenformate (NV2A: F16 max 511.9375, D16 65535, D24 2^24-1, F24 ~1e30):
               * Texgen-r des Schattenvergleichs liegt in diesen Einheiten, unser D24-SRV in [0,1]. */
                unsigned zf = (fmt >> 8) & 0xFFu;
                g_rtt[idx].zscale = (zf == 0x2Du || zf == 0x31u) ? 511.9375f :
                                    (zf == 0x2Cu || zf == 0x30u) ? 65535.0f :
                                    (zf == 0x2Au || zf == 0x2Eu) ? 16777215.0f : 1.0f;
            }
            HLOG_I("RTT Schattenpuffer: depth=0x%X data=0x%X %ux%u fmt=0x%08X caller=0x%X f=%u (slot %d)",
                   depth, data, w, h, fmt, caller, g_present_no, idx);
        }
        goto have_entry;
    }
shadow_fail:
    if (!rtt_hdr(surf, &data, &fmt, &size) || !rtt_dims(fmt, size, &w, &h)) {
        static unsigned nbad, bad_s, bad_d, bad_c;
        unsigned hd[8] = { 0 };
        __try {
            int q;
            for (q = 0; q < 8; q++) {
                hd[q] = *(unsigned *)(uintptr_t)(surf + (unsigned)q * 4u);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (nbad < 12 && (nbad == 0 || surf != bad_s || depth != bad_d || caller != bad_c)) {
            nbad++; bad_s = surf; bad_d = depth; bad_c = caller;
            { unsigned dd[8] = { 0 }; if (depth) { __try { int q2; for (q2 = 0; q2 < 8; q2++) { dd[q2] = *(unsigned *)(uintptr_t)(depth + (unsigned)q2 * 4u); } } __except (EXCEPTION_EXECUTE_HANDLER) {} }
            HLOG_W("RTT unlesbar #%u: surf=0x%X depth=0x%X caller=0x%X f=%u hdr=[%08X %08X %08X %08X %08X %08X %08X %08X] dhdr=[%08X %08X %08X %08X %08X %08X %08X %08X]",
                   nbad, surf, depth, caller, g_present_no, hd[0], hd[1],
                   hd[2], hd[3], hd[4], hd[5], hd[6], hd[7], dd[0], dd[1], dd[2], dd[3], dd[4], dd[5], dd[6], dd[7]); }
        }
        rtt_session_end(surf, "unlesbar");
        g_rtt_dummy = 1; /* FIX(MONITORVIEW,W36): Dummy-Tiefenpass aktiv */
        return;
    }
    idx = rtt_find(surf);
    if (idx >= 0 && g_cfg.rtt_stale && !g_rtt[idx].isdepth && g_rtt[idx].data != data) {
        /* FIX(MAP-RETRY,W124): dieselbe Surface-Adresse, aber neue Daten
         * (Header nach Freigabe neu vergeben) -> Eintrag neu anlegen statt
         * die alte Datenadresse/Groesse weiterzufuehren. */
        static unsigned nnd;
        if (nnd < 16u) {
            nnd++;
            HLOG_I("RTT data-neu #%u: slot %d surf=0x%X data 0x%X->0x%X %ux%u->%ux%u f=%u",
                   nnd, idx, surf, g_rtt[idx].data, data, g_rtt[idx].w, g_rtt[idx].h, w, h, g_present_no);
        }
        if (g_rtt_bound == idx) { rtt_bind_bb(); }
        if (g_rtt_cur == idx) { g_rtt_cur = -1; }
        rtt_release_entry(idx);
        g_rtt[idx].surf = surf; g_rtt[idx].data = data;
        g_rtt[idx].w = w; g_rtt[idx].h = h;
        { unsigned sc = rtt_scale_for(w, h, fmt); g_rtt[idx].pw = w * sc; g_rtt[idx].ph = h * sc; }
    }
    if (idx < 0) {
        if (g_rtt_n < RTT_MAX) { idx = g_rtt_n++; }
        else if (g_cfg.rtt_stale) {
            /* FIX(MAP-RETRY,W124): volle Tabelle -> zuerst einen veralteten,
             * sonst den am laengsten nicht mehr gerenderten Eintrag verdraengen
             * (statt reihum, das traf auch lebende RTs). */
            int j, best = -1;
            for (j = 0; j < RTT_MAX; j++) {
                if (j == g_rtt_bound || j == g_rtt_cur) { continue; }
                if (best < 0 || (g_rtt[j].stale && !g_rtt[best].stale) ||
                    (g_rtt[j].stale == g_rtt[best].stale && g_rtt[j].lastset < g_rtt[best].lastset)) { best = j; }
            }
            idx = best >= 0 ? best : 0;
            {
                static unsigned nev;
                if (nev < 16u) {
                    nev++;
                    HLOG_I("RTT verdraengt #%u: slot %d surf=0x%X data=0x%X stale=%d lastset=%u -> surf=0x%X f=%u",
                           nev, idx, g_rtt[idx].surf, g_rtt[idx].data, g_rtt[idx].stale, g_rtt[idx].lastset, surf, g_present_no);
                }
            }
            if (g_rtt_bound == idx) { rtt_bind_bb(); }
            if (g_rtt_cur == idx) { g_rtt_cur = -1; }
            rtt_release_entry(idx);
        } else {
            static unsigned rep;
            idx = (int)(rep++ % RTT_MAX);
            if (g_rtt_bound == idx) { rtt_bind_bb(); }
            if (g_rtt_cur == idx) { g_rtt_cur = -1; }
            rtt_release_entry(idx);
        }
        g_rtt[idx].surf = surf; g_rtt[idx].data = data;
        g_rtt[idx].w = w; g_rtt[idx].h = h;
        { unsigned sc = rtt_scale_for(w, h, fmt); g_rtt[idx].pw = w * sc; g_rtt[idx].ph = h * sc; }
        g_rtt[idx].tex = NULL; g_rtt[idx].rtv = NULL;
        g_rtt[idx].srv = NULL; g_rtt[idx].dsv = NULL;
    }
have_entry:
    g_rtt[idx].lastset = g_present_no + 1u; /* W124 rtt_stale */
    g_rtt[idx].stale = 0;
    if (g_cfg.rtt_stale && !g_rtt[idx].isdepth) {
        /* FIX(MAP-RETRY,W124): zwei lebende RTs teilen sich keinen Speicher ->
         * andere Eintraege mit derselben Datenadresse sind veraltet (Mission
         * neu geladen, alte RT-Texturen ohne Release rc=0 verworfen). */
        int j;
        for (j = 0; j < g_rtt_n; j++) {
            if (j != idx && !g_rtt[j].stale && !g_rtt[j].isdepth && g_rtt[j].surf &&
                g_rtt[j].data == g_rtt[idx].data) {
                static unsigned nsd;
                g_rtt[j].stale = 1;
                if (nsd < 16u) {
                    nsd++;
                    HLOG_I("RTT veraltet #%u: slot %d surf=0x%X data=0x%X %ux%u (neu: slot %d surf=0x%X %ux%u) f=%u",
                           nsd, j, g_rtt[j].surf, g_rtt[j].data, g_rtt[j].w, g_rtt[j].h, idx, g_rtt[idx].surf,
                           g_rtt[idx].w, g_rtt[idx].h, g_present_no);
                }
            }
        }
    }
    g_rtt[idx].draws = 0;
    g_rtt[idx].vpseen = 0; g_rtt[idx].vpw = 0; g_rtt[idx].vph = 0;
    g_rtt[idx].clr = 0; g_rtt[idx].clrflags = 0;
    g_rtt[idx].clears = 0;
    g_rtt[idx].fprim = 0; g_rtt[idx].fn = 0; g_rtt[idx].frhw = 0;
    g_rtt[idx].fxptr = 0;
    g_rtt_cur = idx;
    rbc_session_begin(idx); /* W215 rtt_rb_sync=2 */
    g_rtt_dummy = 0; /* FIX(MONITORVIEW,W36): gueltiges RT = kein Dummy */
    for (k = 0; k < nseen && seen[k] != surf; k++) {}
    if (k == nseen && nseen < 64) {
        seen[nseen++] = surf;
        HLOG_I("RTT #%u: surf=0x%X data=0x%X %ux%u fmt=0x%08X size=0x%08X depth=0x%X caller=0x%X f=%u (slot %d)",
               nseen, surf, data, w, h, fmt, size, depth, caller,
               g_present_no, idx);
    /* RTT-DIAG: Kompositions-Eingang (Setup-Input, Tabelle) bei Aenderung. */
    {
        static unsigned opin, ovin0, ncomp;
        static int havecomp;
        unsigned pin = 0xDEADu, vin0 = 0xDEADu, vin1 = 0xDEADu;
        unsigned sg = 0xDEADu, dg = 0xDEADu;
        unsigned t0 = 0xDEADu, t1 = 0xDEADu, t2 = 0xDEADu, t3 = 0xDEADu;
        __try {
            pin = *(unsigned *)(uintptr_t)0x279EE4u;
            sg = *(unsigned *)(uintptr_t)0x4C99E0u;
            dg = *(unsigned *)(uintptr_t)0x4B1960u;
            t0 = *(unsigned *)(uintptr_t)0x4B99C0u;
            t1 = *(unsigned *)(uintptr_t)0x4B99C4u;
            t2 = *(unsigned *)(uintptr_t)0x4B99C8u;
            t3 = *(unsigned *)(uintptr_t)0x4B99CCu;
            if (pin && pin >= 0x10000u) {
                vin0 = *(unsigned *)(uintptr_t)pin;
                vin1 = *(unsigned *)(uintptr_t)(pin + 4);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if ((!havecomp || pin != opin || vin0 != ovin0) && ncomp < 12) {
            havecomp = 1; opin = pin; ovin0 = vin0; ncomp++;
            HLOG_I("RTT comp #%u: [0x279EE4]=0x%X *in=[0x%X 0x%X] surfG=0x%X depthG=0x%X tab0=[0x%X 0x%X 0x%X 0x%X] f=%u",
                   ncomp, pin, vin0, vin1, sg, dg, t0, t1, t2, t3,
                   g_present_no);
        }
    }
    }
    if (!g_cfg.rtt) { return; }
    if (!rtt_ensure(idx)) {
        static unsigned nfail;
        if (nfail < 4) {
            nfail++;
            HLOG_W("RTT anlegen fehlgeschlagen: surf=0x%X %ux%u", surf, w, h);
        }
        rtt_bind_bb();
        return;
    }
    if (g_rtt_bound != idx) {
        D3D11_VIEWPORT dvp;
        sb_ctx_enter();
        rtt_shadow_rs(g_rtt[idx].isdepth, g_rtt[idx].zscale); /* W100: Polygon-Offset nur im Schattenpuffer */
        ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtt[idx].rtv,
                                               g_rtt[idx].dsv);
        dvp.TopLeftX = 0.0f; dvp.TopLeftY = 0.0f;
        dvp.Width = (float)g_rtt[idx].pw; dvp.Height = (float)g_rtt[idx].ph; /* W88: D3D11-Groesse */
        dvp.MinDepth = 0.0f; dvp.MaxDepth = 1.0f;
        ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &dvp);
        sb_ctx_leave();
        g_rtt_bound = idx;
    }
}

void rtt_present_restore(void)
{
    if (g_rtt_bound < 0) { return; }
    rtt_session_end(0, "auto@present");
}

int g_rtt_selfread = -1; /* W106 rtt_selfcopy: Slot, dessen Kopie der laufende Draw liest */

void rtt_note_draw(unsigned prim, unsigned n, int rhw, unsigned xptr)
{
    int c = g_rtt_cur;
    if (c >= 0 && c < g_rtt_n) {
        if (g_rtt_selfread != c) { g_rtt[c].wgen++; } /* W106: Kopie veraltet */
        g_rtt_selfread = -1;
        if (!g_rtt[c].draws) {
            g_rtt[c].fprim = prim; g_rtt[c].fn = n;
            g_rtt[c].frhw = rhw ? 1u : 0u; g_rtt[c].fxptr = xptr;
        }
        g_rtt[c].draws++;
        if (!g_rtt[c].vpseen) {
            g_rtt[c].vpseen = 1;
            g_rtt[c].vpw = g_vp_game.w;
            g_rtt[c].vph = g_vp_game.h;
        }
        if (g_rtt[c].isdepth && g_rtt_bound == c) { sb_ctx_enter(); rtt_shadow_rs(1, g_rtt[c].zscale); sb_ctx_leave(); } /* W100: Polygon-Offset je Draw */
        if (g_rtt[c].isdepth) { /* RENDER-QA3 (W100): z-Skala des Tiefenpasses (c[-38].z, Spiegel 0x21BCA8 + 58*16) */
            float zs = 0.0f;
            __try { zs = *(volatile float *)(uintptr_t)(0x21BCA8u + 58u * 16u + 8u); }
            __except (EXCEPTION_EXECUTE_HANDLER) { zs = 0.0f; }
            {   /* DIAG: VS-Spiegel c[-38].z (Befund W100: bleibt 0, Skala kommt aus dem Format) */
                static unsigned nzl;
                if (nzl < 4u) { nzl++; HLOG_I("RTT Schattenpuffer Tiefenpass-Draw: c[-38].z=%g zscale=%g f=%u", zs, g_rtt[c].zscale, g_present_no); }
            }
        }
    }
}

/* RENDER-QA3 (W100): Tiefen-SRV eines Schattenpuffers zur Textur xptr
 * (gleiche Datenadresse), sonst NULL. */
ID3D11ShaderResourceView *sb_rtt_depth_srv(unsigned xptr, unsigned *w, unsigned *h, float *zscale)
{
    unsigned data = 0;
    int i;
    if (!xptr) { return NULL; }
    __try { data = *(unsigned *)(uintptr_t)(xptr + 4); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    if (!data) { return NULL; }
    for (i = 0; i < g_rtt_n; i++) {
        if (g_rtt[i].isdepth && g_rtt[i].data == data && g_rtt[i].dsrv) {
            if (w) { *w = g_rtt[i].w; }
            if (h) { *h = g_rtt[i].h; }
            if (zscale) { *zscale = g_rtt[i].zscale; }
            return g_rtt[i].dsrv;
        }
    }
    return NULL;
}

void rtt_note_clear(unsigned flags, unsigned color)
{
    int c = g_rtt_cur;
    if (c >= 0 && c < g_rtt_n) { g_rtt[c].wgen++; } /* W106 rtt_selfcopy */
    /* W215 rtt_rb_sync=2: Farb-Clear (Xbox 0xF0 je Kanal; D3D11 loescht das ganze
     * Ziel, sb_render_clear) auch im CPU-Schatten der Zellenkarte. */
    if (c >= 0 && c == g_rbc_slot && (flags & (XBX_CLEAR_TARGET | XB_CLEAR_TARGET))) { rbc_clear(color); }
    if (c >= 0 && c < g_rtt_n && (flags & XB_CLEAR_TARGET)) {
        g_rtt[c].clears++;
        g_rtt[c].clr = color;
        g_rtt[c].clrflags = flags;
    }
}

/* MONITOR-FX (W105): Selbst-Sampling ueber eine Kopie (nur mfx-Zeilen-Draw).
 * Kopie hat die Beschreibung des RT (inkl. RENDER_TARGET), damit die
 * 120-FPS-Aufzeichnung sie wie ein RT am Tick-Ende sichert. */
int g_rtt_selfcopy;
static ID3D11Texture2D *g_rtt_sc_tex[RTT_MAX];
static ID3D11ShaderResourceView *g_rtt_sc_srv[RTT_MAX];
static unsigned g_rtt_sc_w[RTT_MAX], g_rtt_sc_h[RTT_MAX];
static ID3D11ShaderResourceView *rtt_self_copy(int i)
{
    ID3D11Resource *res = NULL;
    if (!g_dev || !g_rtt[i].tex || !g_rtt[i].srv) { return NULL; }
    if (g_rtt_sc_tex[i] && (g_rtt_sc_w[i] != g_rtt[i].pw || g_rtt_sc_h[i] != g_rtt[i].ph)) {
        if (g_rtt_sc_srv[i]) { ID3D11ShaderResourceView_Release(g_rtt_sc_srv[i]); g_rtt_sc_srv[i] = NULL; }
        ID3D11Texture2D_Release(g_rtt_sc_tex[i]); g_rtt_sc_tex[i] = NULL;
    }
    if (!g_rtt_sc_tex[i]) {
        D3D11_TEXTURE2D_DESC td;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd;
        ID3D11Texture2D_GetDesc(g_rtt[i].tex, &td);
        td.BindFlags &= (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        td.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_rtt_sc_tex[i])) || !g_rtt_sc_tex[i]) {
            g_rtt_sc_tex[i] = NULL; return NULL;
        }
        ID3D11ShaderResourceView_GetDesc(g_rtt[i].srv, &vd);
        if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_rtt_sc_tex[i], &vd, &g_rtt_sc_srv[i]))) {
            ID3D11Texture2D_Release(g_rtt_sc_tex[i]); g_rtt_sc_tex[i] = NULL; g_rtt_sc_srv[i] = NULL; return NULL;
        }
        g_rtt_sc_w[i] = g_rtt[i].pw; g_rtt_sc_h[i] = g_rtt[i].ph;
        HLOG_I("RTT selfcopy: slot %d %ux%u fmt=%u", i, td.Width, td.Height, (unsigned)td.Format);
    }
    if (g_rtt_selfcopy == 1) { /* einmal je Draw kopieren */
        ID3D11View_GetResource((ID3D11View *)g_rtt[i].srv, &res);
        if (!res) { return NULL; }
        sb_ctx_enter();
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_rtt_sc_tex[i], res);
        sb_ctx_leave();
        ID3D11Resource_Release(res);
        g_rtt_selfcopy = 2;
    }
    return g_rtt_sc_srv[i];
}
/* FIX(FX-HOLE,W106): Kopie des RT i fuer Selbst-Sampling (NULL = Fehler ->
 * alter Pfad). Neue Kopie nur, wenn seit der letzten Kopie fremd geschrieben
 * wurde (wgen); jede neue Kopie in die naechste Ring-Textur, damit
 * aufgezeichnete Draws (Interp) ihren Stand behalten. */
static ID3D11ShaderResourceView *rtt_selfcopy_srv(int i)
{
    rtt_entry_t *e = &g_rtt[i];
    unsigned k;
    static unsigned nlog;
    if (!e->tex || !g_dev || !g_ctx) { return NULL; }
    if (e->cgen == e->wgen + 1u && e->csrv[e->cring]) { return e->csrv[e->cring]; }
    k = (e->cgen ? e->cring + 1u : 0u) % RTT_SCOPY;
    if (!e->ctex[k]) {
        D3D11_TEXTURE2D_DESC td;
        HRESULT hr;
        ID3D11Texture2D_GetDesc(e->tex, &td);
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.CPUAccessFlags = 0;
        td.MiscFlags = 0;
        hr = ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &e->ctex[k]);
        if (FAILED(hr) || !e->ctex[k]) { e->ctex[k] = NULL; return NULL; }
        hr = ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)e->ctex[k], NULL, &e->csrv[k]);
        if (FAILED(hr) || !e->csrv[k]) {
            ID3D11Texture2D_Release(e->ctex[k]); e->ctex[k] = NULL; e->csrv[k] = NULL;
            return NULL;
        }
    }
    sb_ctx_enter();
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)e->ctex[k], (ID3D11Resource *)e->tex);
    sb_ctx_leave();
    e->cring = k; e->cgen = e->wgen + 1u;
    if (nlog < 8u) {
        nlog++;
        HLOG_I("RTT selfcopy #%u: slot %d %ux%u ring=%u f=%u", nlog, i, e->pw, e->ph, k, g_present_no);
    }
    return e->csrv[k];
}

int sb_rtt_lookup(unsigned xptr, ID3D11ShaderResourceView **srv,
                  unsigned *w, unsigned *h)
{
    static unsigned seen[16], nseen;
    unsigned data = 0;
    unsigned k;
    int i;
    if (srv) { *srv = NULL; }
    if (w) { *w = 0; }
    if (h) { *h = 0; }
    if (!xptr) { return 0; }
    __try { data = *(unsigned *)(uintptr_t)(xptr + 4); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!data) { return 0; }
    if (g_cfg.rtt_stale) {
        /* FIX(MAP-RETRY,W124): unter allen Daten-Treffern den zuletzt
         * gerenderten, nicht veralteten Eintrag mit passender Groesse waehlen
         * (alt: erster Treffer - nach Missions-Neustart ein alter RT). */
        unsigned c0 = 0, tfmt = 0, tsize = 0, tw = 0, th = 0;
        int best = -1, nskip = 0, firstskip = -1, live = -1;
        __try {
            c0 = *(unsigned *)(uintptr_t)xptr;
            tfmt = *(unsigned *)(uintptr_t)(xptr + 12);
            tsize = *(unsigned *)(uintptr_t)(xptr + 16);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        if (c0 == 0xC0002u) { tw = 640; th = 480; }
        else if (!rtt_dims(tfmt, tsize, &tw, &th)) { tw = 0; th = 0; }
        for (i = 0; i < g_rtt_n; i++) {
            if (!g_rtt[i].surf || g_rtt[i].data != data || g_rtt[i].isdepth) { continue; }
            if (g_rtt[i].stale && tw && tw == g_rtt[i].w && th == g_rtt[i].h &&
                g_rtt[i].lastset + 1u >= g_present_no &&
                (live < 0 || g_rtt[i].lastset > g_rtt[live].lastset)) {
                live = i; /* veraltet markiert, aber in diesem/vorigem Bild noch Ziel = lebt */
            }
            if (g_rtt[i].stale || !tw || tw != g_rtt[i].w || th != g_rtt[i].h) {
                nskip++;
                if (firstskip < 0) { firstskip = i; }
                continue;
            }
            if (best < 0 || g_rtt[i].lastset > g_rtt[best].lastset) { best = i; }
        }
        if (best < 0 && live >= 0) { best = live; }
        if (nskip) {
            static unsigned sk_x[64], sk_s[64], nsk;
            unsigned q;
            for (q = 0; q < nsk && !(sk_x[q] == xptr && sk_s[q] == (unsigned)firstskip); q++) {}
            if (q == nsk && nsk < 64u) {
                sk_x[nsk] = xptr; sk_s[nsk] = (unsigned)firstskip; nsk++;
                HLOG_I("TEX rt-skip #%u: xptr=0x%X data=0x%X tex=%ux%u fmt=0x%02X uebersprungen=%d (slot %d %ux%u stale=%d) -> %s%d f=%u",
                       nsk, xptr, data, tw, th, (tfmt >> 8) & 0xFFu, nskip, firstskip, g_rtt[firstskip].w,
                       g_rtt[firstskip].h, g_rtt[firstskip].stale, best >= 0 ? "slot " : "Textur-Pfad ", best,
                       g_present_no);
            }
        }
        if (best < 0) { return 0; }
        i = best;
        goto have_match;
    }
    for (i = 0; i < g_rtt_n; i++) {
        if (g_rtt[i].surf && g_rtt[i].data == data && !g_rtt[i].isdepth) { break; }
    }
    if (i >= g_rtt_n) { return 0; }
    /* Plausibilitaet: Textur-Dims muessen den Surface-Dims entsprechen
     * (Mip 0; sonst Puffer-Reuse -> CPU-Pfad statt stale SRV). c0-Klassen
     * wie tex_lin_dims (0xC0002 = 640x480 legacy). */
    {
        unsigned c0 = 0, tfmt = 0, tsize = 0, tw = 0, th = 0;
        __try {
            c0 = *(unsigned *)(uintptr_t)xptr;
            tfmt = *(unsigned *)(uintptr_t)(xptr + 12);
            tsize = *(unsigned *)(uintptr_t)(xptr + 16);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        if (c0 == 0xC0002u) { tw = 640; th = 480; }
        else if (!rtt_dims(tfmt, tsize, &tw, &th)) { tw = 0; th = 0; }
        if (!tw || tw != g_rtt[i].w || th != g_rtt[i].h) {
            static unsigned nrej;
            if (nrej < 8) {
                nrej++;
                HLOG_I("TEX rt-reject #%u: xptr=0x%X data=0x%X tex=%ux%u RT=%ux%u",
                       nrej, xptr, data, tw, th, g_rtt[i].w, g_rtt[i].h);
            }
            return 0;
        }
    }
have_match:
    for (k = 0; k < nseen && seen[k] != xptr; k++) {}
    if (k == nseen && nseen < 16) {
        seen[nseen++] = xptr;
        HLOG_I("TEX rt-match #%u: xptr=0x%X data=0x%X f=%u (slot %d %ux%u surf=0x%X)",
               nseen, xptr, data, g_present_no, i, g_rtt[i].w, g_rtt[i].h,
               g_rtt[i].surf);
    }
    if (!g_cfg.rtt || !g_rtt[i].srv) { return 0; }
    if (g_rtt_bound == i && g_rtt_selfcopy && !g_cfg.rtt_selfcopy) {
        /* MONITOR-FX (W105): Spiel sampelt das gebundene RT (Zeilen-Stoerung
         * FUN_00016d70) -> Kopie je Draw als Quelle, RT bleibt gebunden.
         * Nur noch ohne W106 (rtt_selfcopy=0): Die Einzelkopie hielt fuer die
         * 120-FPS-Wiedergabe keinen Stand (V_C6ikill 2/281 Abweichungen); der
         * W106-Ring tut das, und jede Stoerzeile liest nur ihre eigene Zeile. */
        ID3D11ShaderResourceView *cs = rtt_self_copy(i);
        if (cs) {
            if (srv) { *srv = cs; }
            if (w) { *w = g_rtt[i].w; }
            if (h) { *h = g_rtt[i].h; }
            return 1;
        }
    }
    /* FIX(FX-HOLE,W106): Selbst-Sampling (Stufe-0-Textur = gebundenes RT,
     * z. B. Hitzeflimmer-Sprites des Rauchs in RT31, Linien-Blits in RT40).
     * Xbox: der Draw liest und schreibt dieselbe Flaeche. D3D11 kann RTV+SRV
     * nicht gleichzeitig -> bisher Umbinden auf den Backbuffer (Draw und alle
     * Folge-Draws bis zum naechsten SetRenderTarget landeten mit RT-Koordinaten
     * ohne Tiefentest auf dem Cockpit = "Loecher"). rtt_selfcopy=1: Ziel bleibt
     * das RT, gelesen wird eine Kopie (Stand vor dem Draw; neue Kopie nur nach
     * fremden Schreibzugriffen, Ring fuer die 120-FPS-Aufzeichnung). */
    if (g_rtt_bound == i && g_cfg.rtt_selfcopy) {
        if (srv) {
            ID3D11ShaderResourceView *cs = rtt_selfcopy_srv(i);
            if (cs) {
                g_rtt_selfread = i;
                *srv = cs;
                if (w) { *w = g_rtt[i].w; }
                if (h) { *h = g_rtt[i].h; }
                return 1;
            }
        } else {
            if (w) { *w = g_rtt[i].w; }
            if (h) { *h = g_rtt[i].h; }
            return 1; /* Abfrage: RT-Treffer, Ziel bleibt */
        }
    }
    if (g_rtt_bound == i) { rtt_bind_bb(); }
    /* RTT-DIAG: RT-Inhalt beim ersten Servieren als JPG (was zeigt das
     * Spiel aus dem RT? Titel leer vs. Menue voll?). Max 6 Dumps. */
    {
        static unsigned did[64];
        static unsigned ndump;
        /* W139 PERF-WARM: nur noch mit rtt_dump=1 (CopyResource + blockierendes Map READ + JPG
         * unter ctx_lock, ~1-12 ms je Ziel, 10 Ziele beim Missionsstart und erneut nach 5000 Bildern
         * beim Retry -> GPU leergelaufen, Present-Takt gesperrt). */
        if (g_cfg.rtt_dump && ndump < 48 && g_rtt[i].pw && g_rtt[i].ph && /* W38: Cap 12->48 (Serve-Dumps auch post-START) */
            (g_rtt[i].draws || g_rtt[i].clears) &&
            (!did[i] || g_present_no - g_rtt[i].dumpf > 5000) &&
            g_rtt[i].pw <= 2048 && g_rtt[i].ph <= 2048) {
            ID3D11Resource *res = NULL;
            ID3D11Texture2D *stage = NULL;
            D3D11_TEXTURE2D_DESC sd;
            D3D11_MAPPED_SUBRESOURCE map;
            HRESULT hr;
            did[i] = 1; ndump++; g_rtt[i].dumpf = g_present_no;
            ID3D11ShaderResourceView_GetResource(g_rtt[i].srv, &res);
            if (res) {
                memset(&sd, 0, sizeof sd);
                sd.Width = g_rtt[i].pw; sd.Height = g_rtt[i].ph;
                sd.MipLevels = 1; sd.ArraySize = 1;
                sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                sd.SampleDesc.Count = 1;
                sd.Usage = D3D11_USAGE_STAGING;
                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                hr = ID3D11Device_CreateTexture2D(g_dev, &sd, NULL, &stage);
                if (SUCCEEDED(hr) && stage) {
                    unsigned char *bgr;
                    bgr = (unsigned char *)malloc(g_rtt[i].pw * g_rtt[i].ph * 3u);
                    sb_ctx_enter();
                    ID3D11DeviceContext_CopyResource(g_ctx,
                        (ID3D11Resource *)stage, res);
                    hr = ID3D11DeviceContext_Map(g_ctx,
                        (ID3D11Resource *)stage, 0, D3D11_MAP_READ, 0, &map);
                    if (SUCCEEDED(hr) && bgr) {
                        unsigned x, y, first = 0, distinct = 0, sum = 0, n = 0;
                        unsigned seenpx[64];
                        char path[600];
                        for (y = 0; y < g_rtt[i].ph; y++) {
                            unsigned char *s = (unsigned char *)map.pData +
                                y * map.RowPitch;
                            for (x = 0; x < g_rtt[i].pw; x++) {
                                unsigned o = (y * g_rtt[i].pw + x) * 3u;
                                bgr[o + 0] = s[x * 4 + 0];
                                bgr[o + 1] = s[x * 4 + 1];
                                bgr[o + 2] = s[x * 4 + 2];
                                if (((x | y) & 15u) == 0) {
                                    unsigned q, v = *(unsigned *)(s + x * 4u);
                                    if (!n) { first = v; }
                                    n++; sum += s[x * 4 + 0] + s[x * 4 + 1] + s[x * 4 + 2];
                                    for (q = 0; q < distinct && seenpx[q] != v; q++) {}
                                    if (q == distinct && distinct < 64) {
                                        seenpx[distinct++] = v;
                                    }
                                }
                            }
                        }
                        _snprintf_s(path, sizeof path, _TRUNCATE,
                            "%s/shots/rtrd_%d_%ux%u.jpg", hcfg_out_dir(), i,
                            g_rtt[i].pw, g_rtt[i].ph);
                        sb_render_write_jpg(path, bgr, g_rtt[i].pw, g_rtt[i].ph);
                        HLOG_I("RTT dump #%u: slot %d %ux%u first=0x%08X distinct=%u%s avg=%u f=%u",
                               ndump, i, g_rtt[i].pw, g_rtt[i].ph, first,
                               distinct, distinct >= 64 ? "+" : "",
                               n ? sum / n / 3u : 0, g_present_no);
                    }
                    if (bgr) { free(bgr); }
                    ID3D11DeviceContext_Unmap(g_ctx,
                        (ID3D11Resource *)stage, 0);
                    sb_ctx_leave();
                    ID3D11Texture2D_Release(stage);
                }
                ID3D11Resource_Release(res);
            }
        }
    }
    if (srv) { *srv = g_rtt[i].srv; }
    if (w) { *w = g_rtt[i].w; }
    if (h) { *h = g_rtt[i].h; }
    return 1;
}

/* FIX(MAP-RETRY,W124, rtt_stale): Textur/Surface per Release rc=0 freigegeben
 * -> RT-Eintraege mit derselben Datenadresse (die RT-Surface selbst wird vom
 * Spiel nicht einzeln freigegeben, nur ihre Textur) als veraltet markieren.
 * Wird das RT danach wieder Ziel (SetRenderTarget), gilt es wieder. */
void sb_rtt_release_data(unsigned res)
{
    unsigned data = 0;
    int i;
    if (!g_cfg.rtt_stale || !res) { return; }
    __try { data = *(unsigned *)(uintptr_t)(res + 4); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!data) { return; }
    for (i = 0; i < g_rtt_n; i++) {
        if (g_rtt[i].surf && !g_rtt[i].stale && !g_rtt[i].isdepth && g_rtt[i].data == data) {
            static unsigned nrd;
            g_rtt[i].stale = 1;
            if (nrd < 32u) {
                nrd++;
                HLOG_I("RTT freigegeben #%u: slot %d surf=0x%X data=0x%X %ux%u (res=0x%X) f=%u",
                       nrd, i, g_rtt[i].surf, data, g_rtt[i].w, g_rtt[i].h, res, g_present_no);
            }
        }
    }
}

/* TEXLIFE-Analogon: Surface freigegeben (Release rc=0) -> Notiz loeschen,
 * damit kein stale Data-Match nach Puffer-Reuse (falsche SRV). */
void sb_rtt_release_surf(unsigned surf)
{
    int i;
    static unsigned nrel;
    if (!surf) { return; }
    for (i = 0; i < g_rtt_n; i++) {
        if (g_rtt[i].surf == surf) {
            if (g_rtt_bound == i) { rtt_bind_bb(); }
            if (g_rtt_cur == i) { g_rtt_cur = -1; }
            rtt_release_entry(i);
            if (nrel < 8) {
                nrel++;
                HLOG_I("RTT release #%u: slot %d surf=0x%X", nrel, i, surf);
            }
            return;
        }
    }
}
