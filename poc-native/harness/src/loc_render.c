/* loc_render.c - W285 LOC-RENDER: Renderer-Anpassungen nur fuer das LoC-Profil.
 * Alles hier wirkt nur bei g_sba_loc (Line of Contact, [expert] loc_boot=1). */
#include "render_int.h"
#include "loc_render.h"
#include "hooks_int.h"
#include "hcfg.h"
#include "sb_profile.h"
#include "hlog.h"
#include "loc_sync.h"
#include "loc_net.h"
#include "loc_lobbylist_ovl.h" /* W443 */
#include "loc_netpanel.h" /* W447 */
#include "loc_replayctl.h" /* W457 */
#include "loc_lobbywidget.h" /* W460 */
#include "render_qa.h" /* W358: g_rq_active */
#include "render_sblook.h" /* W459 */

unsigned g_loc_draws;
static unsigned s_prev_draws;

extern int g_rqp_bump_as_2d; /* render_psgen.c */
static void loc_ins_diag_frame(unsigned f); /* DIAG(W422) */
static void loc_render_insignia_frame(void); /* W422 */


/* ---------------------------------------------------------------------------
 * W459 LOC-FX-LIGHT: SB look (Experimental) - Original-Bloom-Pfad des Spiels im Cockpit-Missionsbild
 * freischalten (Befund und Stellen: render_sblook.h). Beim Start und bei jeder Schalter-Aenderung; vorher
 * Bytevergleich, fremde Bytes -> nichts aendern (Log). Reine Darstellung, Spielzustand unberuehrt.
 * ------------------------------------------------------------------------- */
static unsigned s_sbl_bits = 0xFFFFFFFFu;
void loc_render_sblook_apply(void)
{
    unsigned bits, i;
    if (!g_sba_loc) { return; }
    bits = sblook_bits(g_cfg.loc_sblook, g_cfg.loc_sblook_mask);
    if (g_cfg.bloom_strength > 100) { /* W465 LOC-ENHANCED: SB-Bloom (Enhanced 150 %) -> Bloom-Pfade wie SB look */
        bits |= SBLOOK_BLOOM_MAIN | SBLOOK_BLOOM_CAM;
    }
    if (bits == s_sbl_bits) { return; }
    s_sbl_bits = bits;
    for (i = 0; i < sizeof kSBLOOK_SITES / sizeof kSBLOOK_SITES[0]; i++) {
        const sblook_site_t *st = &kSBLOOK_SITES[i];
        unsigned char *p = (unsigned char *)(uintptr_t)st->va;
        DWORD old;
        int act;
        __try { act = sblook_site_action(st, p, bits); } __except (EXCEPTION_EXECUTE_HANDLER) { act = -1; }
        if (act < 0) { HLOG_W("LOCSBLOOK: %s - Bytes weichen ab, unveraendert", st->name); continue; }
        if (act == 0) { continue; }
        if (!VirtualProtect(p, st->len, PAGE_EXECUTE_READWRITE, &old)) { continue; }
        memcpy(p, act == 1 ? st->patch : st->expect, st->plen);
        VirtualProtect(p, st->len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, st->len);
        HLOG_I("LOCSBLOOK: %s %s", st->name, act == 1 ? "an" : "aus (Original)");
    }
}

void loc_render_frame(unsigned f)
{
    loc_render_sblook_apply(); /* W459: Schalter-Aenderung (ESC/cfg) */
    /* W285: 1 = BUMPENVMAP-Naeherung (wie 2D); W297: 2 = echt (V8U8 + Bump-Matrix) */
    g_rqp_bump_as_2d = g_sba_loc ? (g_cfg.loc_bumpenv >= 2 ? 2 : g_cfg.loc_bumpenv ? 1 : 0) : 0;
    if (g_cfg.loc_census_from > 0 && (f % 10u) == 0u) { /* DIAG: Draws je Bild (Szenenwahl) */
        HLOG_I("LOCDRAWS f=%u n=%u", f, g_loc_draws);
    }
    if (g_sba_loc && g_cfg.loc_census_from > 0 && (f % 150u) == 0u) { /* W311 DIAG: Sichtkarte (Zellen != 0 je Kanal) */
        unsigned b = 0, p = 0, x, y, n0 = 0, n1 = 0;
        __try {
            p = *(volatile const unsigned *)(uintptr_t)0x300D6Cu;
            b = *(volatile const unsigned *)(uintptr_t)0x300D70u;
            if (b && p >= 1024u && p <= 4096u) {
                for (y = 0; y < 256u; y++) {
                    const unsigned char *r = (const unsigned char *)(uintptr_t)(b + y * p);
                    for (x = 0; x < 256u; x++) { n0 += r[x * 4u] != 0; n1 += r[x * 4u + 1u] != 0; }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { b = 0; }
        HLOG_I("LOCVIS f=%u karte=%s zellen0=%u zellen1=%u vismap=%d", f, b ? "ja" : "nein", n0, n1, g_cfg.loc_vismap);
        { /* W311 DIAG: Schalter des Bildaufbaus FUN_00029320 (Cockpit/HUD-Zweig) und Objektliste */
            static const unsigned k_g[] = { 0x306A84u, 0x307264u, 0x3069E0u, 0x306738u, 0x306758u, 0x30698Cu,
                                            0x306BA8u, 0x2FF4C4u, 0x3067FCu, 0x306B54u };
            unsigned v[10] = { 0 }, i, fl = 0, fl0 = 0;
            __try {
                for (i = 0; i < 10u; i++) { v[i] = *(volatile const unsigned *)(uintptr_t)k_g[i]; }
                if (v[0] < 10u) {
                    fl = *(volatile const unsigned *)(uintptr_t)(0x5C517Cu + v[0] * 0xAE8u);
                    fl0 = *(volatile const unsigned *)(uintptr_t)(0x5C516Cu + v[0] * 0xAE8u);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            HLOG_I("LOCHUD f=%u spieler=%u flags=0x%08X/0x%08X b264=%u b9e0=%u b738=%u b758=%u b98c=%u bba8=%u obj=%u visq=%u b54=%u",
                   f, v[0], fl, fl0, v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
        }
    }
    s_prev_draws = g_loc_draws;
    g_loc_draws = 0;
    if (g_cfg.loc_ins_diag) { loc_ins_diag_frame(f); } /* DIAG(W422) */
    loc_render_insignia_frame(); /* W422 LOC-INSIGNIA-PEN: CPU-Texturen des Editors */
    if (g_sba_loc) {
        loc_sync_frame(f); /* W297: Testfolge loc_steps (nur mit loc_sync=1 geparst) */
        loc_net_lobby_frame(f); /* W350: DIAG Lobby-Belegung (nur loc_net_diag=1) */
        loc_lobbylist_frame(f); /* W443 LOC-LOBBY-LIST: Modus/Ping je Zeile der Suchliste */
        loc_netpanel_frame(f); /* W447 LOC-NET-AUTO: Netz-Panel in der Lobby */
        loc_rpc_frame(f); /* W457 LOC-REPLAY-CTRL: Replay-Viewer (Zustand, Bedienhilfe) */
        loc_lobbywidget_frame(f); /* W460 LOC-LOBBY-WIDGET: offene Lobbys im Hauptmenue/MatchMaking */
    }
}

/* DIAG(LOC-RENDER): Zensus-Ausloeser im LoC-Profil. Die SB-Szenenadressen lesen
 * dort die Null-Seite; der Zeitpunkt der Titel-Demo schwankt je Lauf. Daher: ab
 * Bild loc_census_from und nur, wenn das Vorbild mindestens loc_census_draws
 * Draws hatte (3D-Szene statt Titel/Karte). */
int loc_render_census_go(unsigned f)
{
    if (!g_sba_loc || g_cfg.loc_census_from <= 0 || f < (unsigned)g_cfg.loc_census_from) { return 0; }
    return s_prev_draws >= (unsigned)g_cfg.loc_census_draws;
}

/* ---------------------------------------------------------------------------
 * Backbuffer als Textur (LoC-Glow/Bloom-Kette).
 *
 * Beleg (Zensus W285G2, Demo-Spielszene): Nach der 3D-Welt im Backbuffer setzt
 * LoC die von GetBackBuffer2 gelieferte Surface (bei uns g_fake_surface, Kopf
 * leer) als Textur und zeichnet
 *   1. ein Vollbild-Quad ins 256x256-RT (Verkleinerung der Szene, Texel-UV 0..1440 x 0..480),
 *   2. Weichzeichner/Helligkeit in 256/14/9-RTs,
 *   3. das Kompositions-Quad in den Backbuffer (Blend aus, Stufe 0 = Backbuffer,
 *      Stufe 1 = Glow-RT), Bildschirmkoordinaten 0..1440 x 0..480.
 * Ohne Textur wurde Stufe 0 weiss -> weisse Flaeche ueber der ganzen 4:3-Box.
 * 1440 = 2 x 720: LoC legt das Geraet mit MultiSampleType 0x41121 an (FUN_0001DCD0,
 * D3DMULTISAMPLE_2_SAMPLES_SUPERSAMPLE_HORIZONTAL_LINEAR) -> Xbox-Backbuffer
 * 1440x480. Wir zeichnen in 720er-Einheiten in die 4:3-Box; die Kopie der Box
 * vertritt den 1440x480-Puffer (Texel-UV / 1440 = normiert).
 * ------------------------------------------------------------------------- */
static ID3D11Texture2D *s_bbc_tex, *s_bbc_res;
static ID3D11ShaderResourceView *s_bbc_srv;
static unsigned s_bbc_w, s_bbc_h, s_bbc_rw, s_bbc_rh, s_bbc_frame = 0xFFFFFFFFu, s_bbc_draw = 0xFFFFFFFFu;
static DXGI_FORMAT s_bbc_fmt;
unsigned g_loc_bbtex_draws; /* Zaehler (Log) */

static void bbc_release(void)
{
    if (s_bbc_srv) { ID3D11ShaderResourceView_Release(s_bbc_srv); s_bbc_srv = NULL; }
    if (s_bbc_tex) { ID3D11Texture2D_Release(s_bbc_tex); s_bbc_tex = NULL; }
    if (s_bbc_res) { ID3D11Texture2D_Release(s_bbc_res); s_bbc_res = NULL; }
    s_bbc_w = s_bbc_h = s_bbc_rw = s_bbc_rh = 0;
}

/* DIAG(loc_psdump): Alpha-Statistik der Backbuffer-Kopie (Glow-Maske der LoC-Komposition). */
static void tex_alpha_diag(ID3D11Texture2D *src, const char *what)
{
    D3D11_TEXTURE2D_DESC sd;
    ID3D11Texture2D *st = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    unsigned x, y, hist[8] = { 0 }, n = 0;
    unsigned long long sum = 0;
    if (!src) { return; }
    ID3D11Texture2D_GetDesc(src, &sd);
    if (sd.SampleDesc.Count != 1 || sd.ArraySize != 1) { return; }
    sd.MipLevels = 1;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &sd, NULL, &st)) || !st) { return; }
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)st, 0, 0, 0, 0, (ID3D11Resource *)src, 0, NULL);
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        for (y = 0; y < sd.Height; y += 8u) {
            const unsigned char *row = (const unsigned char *)m.pData + (size_t)y * m.RowPitch;
            for (x = 0; x < sd.Width; x += 8u) { unsigned a = row[x * 4u + 3u]; sum += a; hist[a >> 5]++; n++; }
        }
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)st, 0);
        HLOG_I("LOCRENDER DIAG: %s-Alpha f=%u fmt=%u mittel=%.1f hist8=[%u %u %u %u %u %u %u %u]", what, g_present_no, (unsigned)sd.Format,
               n ? (double)sum / n : 0.0, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7]);
    }
    ID3D11Texture2D_Release(st);
}

/* W465: LoC 4:3 mit vollem Hor+ (nicht nativ 16:9, Box schmaler als der Backbuffer, loc_horp_comp=1): die
 * Komposition des Spiels (Glow/Bloom/Korn/Tiefenunschaerfe, W459) deckt dann die volle Breite ab. Vorher nur die
 * 4:3-Box -> im Replay sichtbare Kante zum Hor+-Rand (BERICHT W459 §2). */
int loc_render_horp_full(void)
{
    unsigned bx = 0, by = 0, bw = 0, bh = 0;
    if (!g_sba_loc || g_loc_wide || !g_cfg.loc_horp_comp) { return 0; }
    sb_aspect_box(&bx, &by, &bw, &bh);
    return bw > 0u && bw + 2u < g_bb_w;
}

static ID3D11ShaderResourceView *bbc_copy(void)
{
    ID3D11Resource *res = NULL;
    ID3D11Texture2D *bb = NULL;
    D3D11_TEXTURE2D_DESC bd, dd;
    D3D11_BOX box;
    unsigned bx = 0, by = 0, bw = 0, bh = 0;
    static unsigned nlog;
    if (!g_dev || !g_ctx || !g_rtv) { return NULL; }
    if (s_bbc_srv && s_bbc_frame == g_present_no && s_bbc_draw == g_loc_draws) { return s_bbc_srv; }
    sb_aspect_box(&bx, &by, &bw, &bh);
    if (!bw || !bh) { return NULL; }
    if (loc_render_horp_full()) { bx = 0; bw = g_bb_w; } /* W465: Hor+-Raender mit kopieren */
    sb_ctx_enter();
    ID3D11RenderTargetView_GetResource(g_rtv, &res);
    if (!res || FAILED(ID3D11Resource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&bb)) || !bb) {
        if (res) { ID3D11Resource_Release(res); }
        sb_ctx_leave();
        return NULL;
    }
    ID3D11Resource_Release(res);
    ID3D11Texture2D_GetDesc(bb, &bd);
    if (bx + bw > bd.Width) { bw = bd.Width > bx ? bd.Width - bx : 0; }
    if (by + bh > bd.Height) { bh = bd.Height > by ? bd.Height - by : 0; }
    if (!bw || !bh) { ID3D11Texture2D_Release(bb); sb_ctx_leave(); return NULL; }
    if (s_bbc_tex && (s_bbc_w != bw || s_bbc_h != bh || s_bbc_fmt != bd.Format)) { bbc_release(); }
    if (!s_bbc_tex) {
        memset(&dd, 0, sizeof dd);
        dd.Width = bw; dd.Height = bh; dd.MipLevels = 1; dd.ArraySize = 1;
        dd.Format = bd.Format; dd.SampleDesc.Count = 1;
        dd.Usage = D3D11_USAGE_DEFAULT; dd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (g_cfg.loc_interp_copy) { dd.BindFlags |= D3D11_BIND_RENDER_TARGET; } /* W340: Interp-Schatten verlangt RT-Bindung */
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &s_bbc_tex)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)s_bbc_tex, NULL, &s_bbc_srv))) {
            bbc_release();
            ID3D11Texture2D_Release(bb);
            sb_ctx_leave();
            return NULL;
        }
        s_bbc_w = bw; s_bbc_h = bh; s_bbc_fmt = bd.Format;
    }
    box.left = bx; box.top = by; box.right = bx + bw; box.bottom = by + bh; box.front = 0; box.back = 1;
    if (bd.SampleDesc.Count > 1) { /* MSAA-Backbuffer: erst aufloesen */
        if (s_bbc_res && (s_bbc_rw != bd.Width || s_bbc_rh != bd.Height)) {
            ID3D11Texture2D_Release(s_bbc_res); s_bbc_res = NULL;
        }
        if (!s_bbc_res) {
            dd = bd; dd.SampleDesc.Count = 1; dd.SampleDesc.Quality = 0; dd.MipLevels = 1;
            dd.Usage = D3D11_USAGE_DEFAULT; dd.BindFlags = D3D11_BIND_SHADER_RESOURCE; dd.CPUAccessFlags = 0; dd.MiscFlags = 0;
            if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &s_bbc_res))) { s_bbc_res = NULL; }
            s_bbc_rw = bd.Width; s_bbc_rh = bd.Height;
        }
        if (s_bbc_res) {
            ID3D11DeviceContext_ResolveSubresource(g_ctx, (ID3D11Resource *)s_bbc_res, 0, (ID3D11Resource *)bb, 0, bd.Format);
            ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)s_bbc_tex, 0, 0, 0, 0, (ID3D11Resource *)s_bbc_res, 0, &box);
        }
    } else {
        ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)s_bbc_tex, 0, 0, 0, 0, (ID3D11Resource *)bb, 0, &box);
        /* W340 LOC-FRAMES: Kopie als Interp-Op, sonst zeigt das Kompositions-Quad in Zwischenbildern die Welt von Tick N */
        if (g_ip_recording) { sb_interp_rec_copy(g_rtv, s_bbc_srv, &box); }
    }
    ID3D11Texture2D_Release(bb);
    sb_ctx_leave();
    s_bbc_frame = g_present_no; s_bbc_draw = g_loc_draws;
    g_loc_bbtex_draws++;
    if (g_cfg.loc_psdump && g_loc_bbtex_draws <= 400u && (g_loc_bbtex_draws % 50u) <= 2u) { /* DIAG */
        unsigned xp[4], nn[4], w1 = 0, h1 = 0;
        ID3D11ShaderResourceView *s1 = NULL;
        ID3D11Resource *r1 = NULL;
        tex_alpha_diag(s_bbc_tex, "BB");
        sb_tex_stages(xp, nn);
        if (xp[1] && sb_rtt_lookup(xp[1], &s1, &w1, &h1) && s1) {
            ID3D11ShaderResourceView_GetResource(s1, &r1);
            if (r1) { tex_alpha_diag((ID3D11Texture2D *)r1, "Stufe1-RT"); ID3D11Resource_Release(r1); }
        }
    }
    if (nlog < 3u) {
        nlog++;
        HLOG_I("LOCRENDER: Backbuffer als Textur #%u: Box %ux%u@%u,%u (BB %ux%u, %u Samples) -> Texel %ux%u f=%u",
               nlog, bw, bh, bx, by, bd.Width, bd.Height, bd.SampleDesc.Count, g_game_fb_w * g_loc_ss, g_game_fb_h, g_present_no);
    }
    return s_bbc_srv;
}

/* W326 LOC-WIDE: Present-Parameter der Geraeteanlage (FUN_0001DCD0, X_D3DPRESENT_PARAMETERS:
 * MultiSampleType @16, Flags @40). WIDESCREEN (0x10) -> Aspect-Box 16:9 statt 4:3 (der
 * 720x480-Framebuffer ist dann anamorph), Raender hart schwarz (kein Hor+ im nativen Modus).
 * 1280x720 hat kein Supersampling (Texel-Raum 1280 statt 1440). Nur LoC-Profil. */
unsigned g_loc_ss = 2u;
void loc_render_on_present_params(unsigned pp)
{
    unsigned w = 0, h = 0, ms = 0, fl = 0;
    if (!g_sba_loc || !pp) { return; }
    __try {
        w = *(const unsigned *)(uintptr_t)(pp + 0u);
        h = *(const unsigned *)(uintptr_t)(pp + 4u);
        ms = *(const unsigned *)(uintptr_t)(pp + 16u);
        fl = *(const unsigned *)(uintptr_t)(pp + 40u);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    g_loc_ss = ms == 0x41121u ? 2u : 1u;
    g_loc_wide = (fl & 0x10u) ? 1 : 0;
    g_game_disp_aspect = g_loc_wide ? 16.0f / 9.0f : 640.0f / 480.0f;
    HLOG_I("LOCWIDE: Geraet %ux%u ms=0x%X flags=0x%X (%s%s) -> Box %s, Supersampling %ux, loc_widescreen=%d",
           w, h, ms, fl, (fl & 0x40u) ? "progressiv" : "interlaced", g_loc_wide ? ", WIDESCREEN" : "",
           g_loc_wide ? "16:9" : "4:3", g_loc_ss, g_cfg.loc_widescreen);
}

int loc_render_is_bbtex(unsigned xptr)
{
    return g_sba_loc && g_cfg.loc_bbtex && xptr && xptr == (unsigned)(uintptr_t)g_fake_surface;
}

int loc_render_bb_lookup(unsigned xptr, ID3D11ShaderResourceView **srv, unsigned *w, unsigned *h)
{
    if (!loc_render_is_bbtex(xptr)) { return 0; }
    if (w) { *w = g_game_fb_w * g_loc_ss; } /* Xbox-Backbuffer 2x horizontal (Supersampling; W326: 720p 1x) */
    if (h) { *h = g_game_fb_h; }
    if (srv) { *srv = bbc_copy(); }
    return 1;
}

/* ---------------------------------------------------------------------------
 * W297 LOC-MISSION: Textebene der Menues/Dialoge (XFONT, 640x480 LIN_A8R8G8B8).
 * Ablauf je Bild (Log RTT/LOCLOCK, Aufrufer 0x1156E7): neue Surface auf denselben Speicher
 * (Data 0x06551000) als Renderziel, D3D-Clear, dann TextOut per CPU (Lock 0x18B940, Bits =
 * 0xF0000000|phys). Unser Clear traf nur die GPU-Kopie -> alte Glyphen blieben im Speicher und
 * ueberlagerten sich (Hilfezeile, A10). Wie SB W97 (DLGTEXT2), aber nach Datenadresse, weil LoC
 * jedes Bild ein neues Surface-Objekt anlegt. Nur LoC-Profil (loc_text_clear, Standard 1).
 * ------------------------------------------------------------------------- */
#define LOC_TXT_MAX 4
static struct { unsigned data, bits, pitch, w, h; } s_txt[LOC_TXT_MAX];
static unsigned s_ntxt;
extern int rtt_dims(unsigned fmt, unsigned size, unsigned *w, unsigned *h);
void sb_tex_mark_dirty(unsigned xptr);

void loc_render_text_lock(unsigned surf, unsigned bits, unsigned pitch)
{
    unsigned data = 0, fmt = 0, size = 0, w = 0, h = 0, k;
    if (!g_sba_loc || !surf || !bits) { return; }
    __try {
        data = *(const unsigned *)(uintptr_t)(surf + 4u);
        fmt = *(const unsigned *)(uintptr_t)(surf + 12u);
        size = *(const unsigned *)(uintptr_t)(surf + 16u);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!data || ((fmt >> 8) & 0xFFu) != 0x12u || !rtt_dims(fmt, size, &w, &h) || pitch < w * 4u || w > 1024u || h > 1024u) {
        return;
    }
    for (k = 0; k < s_ntxt && s_txt[k].data != data; k++) {}
    if (k == s_ntxt) {
        if (s_ntxt >= LOC_TXT_MAX) { return; }
        s_ntxt++;
        HLOG_I("LOCTEXT: Textebene #%u data=0x%X %ux%u (CPU-Clear bei D3D-Clear)", k, data, w, h);
    }
    s_txt[k].data = data; s_txt[k].bits = bits; s_txt[k].pitch = pitch; s_txt[k].w = w; s_txt[k].h = h;
}

/* ---------------------------------------------------------------------------
 * W485 LIVEQOS: Live-Dialoge ohne Text (Kontenauswahl, Hinweis nach dem Logon).
 * Das Live-Konto-Modul (Zustand PTR 0x27D670, Zeichnen FUN_0011BF10/FUN_0011C150/FUN_0011C580) schreibt
 * jeden Text per XFONT in die Textebene (FUN_00122320) und blendet sie mit FUN_0011A820 als VOLLBILD-Quad
 * ein: Position und UV (0,0)-([0x3069D0],[0x3071E4]) = Spiel-Framebuffer, in LoC 720x480. Die
 * 2D-Heuristik in sb_im_submit nimmt nur x <= 704 (640 + 64) als Pixelkoordinate -> der Quad lief als
 * 3D-Draw (Pixel als Objektkoordinaten durch die Matrix) und landete ausserhalb des Bilds. System-Link-
 * Dialoge blenden die Textebene nur im Textrechteck ein (FUN_00115450, x < 704) und waren nicht betroffen.
 * Hier: fuer Draws mit der Textebene als Textur gilt die Grenze Framebuffer + 64. Nur im Live-Modus.
 * ------------------------------------------------------------------------- */
void loc_render_rhw_range(unsigned xptr, float *xmax, float *ymax)
{
    unsigned data = 0, k;
    static volatile LONG s_nlog;
    if (!g_sba_loc || !g_cfg.loc_live || !xptr || !s_ntxt || g_game_fb_w <= 640u) { return; }
    __try { data = *(const unsigned *)(uintptr_t)(xptr + 4u); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    for (k = 0; k < s_ntxt && s_txt[k].data != data; k++) {}
    if (!data || k == s_ntxt) { return; }
    *xmax = (float)g_game_fb_w + 64.0f;
    if ((float)g_game_fb_h + 64.0f > *ymax) { *ymax = (float)g_game_fb_h + 64.0f; }
    if (InterlockedIncrement(&s_nlog) == 1) {
        HLOG_I("LOCTEXT: Textebene im Live-Modus als 2D bis %ux%u (Framebuffer %ux%u + 64; Live-Dialoge blenden sie als Vollbild-Quad ein)",
               g_game_fb_w + 64u, g_game_fb_h + 64u, g_game_fb_w, g_game_fb_h);
    }
}

void loc_render_on_clear(unsigned surf, unsigned flags, unsigned color)
{
    unsigned data = 0, parent = 0, k, y, x;
    if (!g_sba_loc || !g_cfg.loc_text_clear || !surf || !(flags & 0xF0u) || !s_ntxt) { return; }
    __try {
        data = *(const unsigned *)(uintptr_t)(surf + 4u);
        parent = *(const unsigned *)(uintptr_t)(surf + 20u);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    for (k = 0; k < s_ntxt && s_txt[k].data != data; k++) {}
    if (k == s_ntxt) { return; }
    __try {
        for (y = 0; y < s_txt[k].h; y++) {
            unsigned *row = (unsigned *)(uintptr_t)(s_txt[k].bits + y * s_txt[k].pitch);
            if (!color) { memset(row, 0, (size_t)s_txt[k].w * 4u); }
            else { for (x = 0; x < s_txt[k].w; x++) { row[x] = color; } }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { s_txt[k].data = 0; return; }
    if (parent) { sb_tex_mark_dirty(parent); }
    sb_tex_mark_dirty(surf);
}

void loc_render_dump_ps(unsigned handle, const unsigned *d, unsigned nd, const char *src)
{
    char path[600];
    FILE *f = NULL;
    unsigned i;
    static unsigned n;
    if (n >= 64u) { return; }
    n++;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s/locps_%08X.txt", hcfg_out_dir(), handle);
    if (fopen_s(&f, path, "w") || !f) { return; }
    fprintf(f, "PS 0x%08X (f=%u)\n", handle, g_present_no);
    for (i = 0; i < nd; i++) { fprintf(f, "d[%u]=%08X%s", i, d[i], (i % 6u) == 5u ? "\n" : " "); }
    fprintf(f, "\n---\n%s\n", src ? src : "(nicht unterstuetzt)");
    fclose(f);
}

/* ---------------------------------------------------------------------------
 * W311 LOC-MISSION-RENDER: Sichtkarte (Gelaende-Zellen und Objektlisten).
 * FUN_00038B80: CreateTexture2(256, 256, 1, RENDERTARGET, 0x12 LIN_A8R8G8B8) -> [0x300FF4],
 * GetSurfaceLevel2 -> [0x2FF340], D3DTexture_LockRect(tex, 0, 0x300D6C, 0, 0) als Dauer-Lock
 * (Pitch [0x300D6C], pBits [0x300D70]). Je Bild zeichnet das Spiel die Sichtfaecher um die Kamera
 * in dieses RT (Byte 0 = Hauptsicht, Byte 1 = zweite Sicht, Farbmaske je Kanal), wartet per
 * BlockOnFence (FUN_00029320) und liest danach per CPU: Objektlisten FUN_00046840 (Byte 0) /
 * FUN_00046A00 (Byte 1), Gelaende-Streifen FUN_0002F5E0 (Byte != 0 je Zelle), jeweils ab
 * pBits + 0x20200 (Mitte = Kamerazelle). Ohne Rueckschrieb blieb die Karte 0 -> kein nahes
 * Gelaende, keine VTs/Objekte (nur Himmel und ferne Reste). Rueckgabe 1 = Sichtkarte;
 * host/pitch = Zeiger, ueber den das Spiel liest (0 = noch kein Lock). Nur LoC-Profil.
 * ------------------------------------------------------------------------- */
int loc_render_vismap(unsigned surf, unsigned *host, unsigned *pitch)
{
    unsigned s = 0, p = 0, b = 0;
    if (!g_sba_loc || !g_cfg.loc_vismap || !surf) { return 0; }
    __try {
        s = *(volatile const unsigned *)(uintptr_t)0x2FF340u;
        p = *(volatile const unsigned *)(uintptr_t)0x300D6Cu;
        b = *(volatile const unsigned *)(uintptr_t)0x300D70u;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (s != surf) { return 0; }
    if (host) { *host = b; }
    if (pitch) { *pitch = (p >= 256u * 4u && p <= 4096u) ? p : 0u; }
    return 1;
}

/* ---------------------------------------------------------------------------
 * W422 LOC-INSIGNIA-PEN (GitHub #67): Stift-RTs des Insignien-Editors (Szene 19, Objekt [0x57C6B8],
 * Init FUN_0009C7B0, Texturen FUN_0009A4B0..0x9A7C9). Die Zeichenflaeche liegt als Index-Bytes
 * (64x64) in [obj+0x46C], ihre Anzeige-Textur [obj+0x4D0] fuellt die CPU je Bild (FUN_0009C230).
 * Die Stift-Vorschau zeichnet das Spiel in ein eigenes 64x64-RT (Textur [obj+0x4D4], LIN_A8R8G8B8,
 * Speicher [obj+0x468], Surface [obj+0x694]) und benutzt dafuer den gemeinsamen Xbox-Speicher:
 *   1. FUN_000A1970 (Bild-Update) loescht den RT-Speicher per CPU (0x4000 B Nullen, kein Clear),
 *   2. FUN_000A0450: SetRenderTarget(+0x694), Stift-Quad (FUN_0009AC30 u. a.), zurueck, BlockOnFence,
 *   3. FUN_0009C510 liest den RT-Speicher per CPU -> Ebene [obj+0x470] (Index bzw. 0xFF),
 *   4. Stift gedrueckt (Flag 0x10000000 in [obj+0x144], A gehalten): FUN_0009C5B0 uebernimmt die Ebene
 *      in die Flaeche,
 *   5. die RT-Textur liegt als Vorschau ueber der Flaeche (FUN_0009AA20 mit [obj+0x4D4]).
 * Ohne gemeinsamen Speicher blieb das RT GPU-only: Schritt 1 loeschte es nie (jeder Stift-Abdruck blieb
 * stehen = Phantom-Spur entlang des Cursors), Schritt 3 las nur die CPU-Nullen (Stift setzte nichts).
 * Zweites RT dieser Art: 384x16 [obj+0x6BC] (Textur [obj+0x4FC], Speicher [obj+0x174], FUN_0009C720).
 * Rueckgabe 1 = surf ist eines dieser RTs (nur LoC-Profil, loc_insignia_rt).
 * ------------------------------------------------------------------------- */
#define LINS_OBJ 0x57C6B8u
int loc_render_insignia_rt(unsigned surf)
{
    unsigned o = 0, a = 0, b = 0;
    if (!g_sba_loc || !g_cfg.loc_insignia_rt || !surf) { return 0; }
    __try {
        o = *(volatile const unsigned *)(uintptr_t)LINS_OBJ;
        if (o) {
            a = *(volatile const unsigned *)(uintptr_t)(o + 0x694u);
            b = *(volatile const unsigned *)(uintptr_t)(o + 0x6BCu);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return o && (surf == a || surf == b);
}

/* W422: Texturen, die der Editor per CPU direkt in ihren Speicher schreibt (ohne LockRect, also ohne
 * TEXDIRTY): Anzeige der Zeichenflaeche [obj+0x4D0] (FUN_0009C230 je Bild, 64x64 = 16 KB), 8 Muster
 * [obj+0x4D8..0x4F4] und 100 Vorschaubilder der Insignien-Dateien [obj+0x500..0x68C] (FUN_0009A340).
 * Der Refresh-Hash des Textur-Caches prueft nur die ersten und letzten 4 KB (W168 tex_hash_fast) - bei
 * 64x64 die Zeilen 0-15 und 48-63. Striche in den Zeilen 16-47 erschienen daher nie (der Cursor sass etwa
 * bei Y:38 im Testerbild). Je Bild als geaendert markieren: der naechste Get vergleicht den vollen Inhalt
 * (W377 loc_text_cache) bzw. laedt neu. Nur solange der Editor existiert (nur LoC-Profil, loc_insignia_rt). */
static void loc_render_insignia_frame(void)
{
    unsigned o = 0, x[109], n = 0, k;
    if (!g_sba_loc || !g_cfg.loc_insignia_rt) { return; }
    __try {
        o = *(volatile const unsigned *)(uintptr_t)LINS_OBJ;
        if (!o) { return; }
        x[n++] = *(volatile const unsigned *)(uintptr_t)(o + 0x4D0u);
        for (k = 0; k < 8u; k++) { x[n++] = *(volatile const unsigned *)(uintptr_t)(o + 0x4D8u + k * 4u); }
        for (k = 0; k < 100u; k++) { x[n++] = *(volatile const unsigned *)(uintptr_t)(o + 0x500u + k * 4u); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    for (k = 0; k < n; k++) {
        if (x[k]) { sb_tex_mark_dirty(x[k]); }
    }
}

/* DIAG(W422) loc_ins_diag: Zustand des Insignien-Editors alle 30 Bilder (nur solange [0x57C6B8] gesetzt).
 * Spielzustand, keine Eingaben: Zustand/Unterzustand, Werkzeug [obj+0x1A], Cursor [obj+0x10C/0x110],
 * Stift-Flag 0x10000000 aus [obj+0x144], Farbe [obj+0xE2], Pixel != 0 in der Flaeche, Ebene != 0xFF,
 * Woerter != 0 im Stift-RT-Speicher. */
static void loc_ins_diag_frame(unsigned f)
{
    unsigned o = 0, cv = 0, ly = 0, rm = 0, fl = 0, i, nc = 0, nl = 0, nr = 0;
    unsigned char st = 0, sub = 0, col = 0;
    unsigned short tool = 0;
    float cx = 0.0f, cy = 0.0f;
    if (!g_sba_loc || (f % 30u) != 0u) { return; }
    __try {
        o = *(volatile const unsigned *)(uintptr_t)LINS_OBJ;
        if (!o) { return; }
        st = *(volatile const unsigned char *)(uintptr_t)o;
        sub = *(volatile const unsigned char *)(uintptr_t)(o + 1u);
        tool = *(volatile const unsigned short *)(uintptr_t)(o + 0x1Au);
        cx = *(volatile const float *)(uintptr_t)(o + 0x10Cu);
        cy = *(volatile const float *)(uintptr_t)(o + 0x110u);
        fl = *(volatile const unsigned *)(uintptr_t)(o + 0x144u);
        col = *(volatile const unsigned char *)(uintptr_t)(o + 0xE2u);
        cv = *(volatile const unsigned *)(uintptr_t)(o + 0x46Cu);
        ly = *(volatile const unsigned *)(uintptr_t)(o + 0x470u);
        rm = *(volatile const unsigned *)(uintptr_t)(o + 0x468u);
        for (i = 0; i < 4096u; i++) {
            if (cv) { nc += *(volatile const unsigned char *)(uintptr_t)(cv + i) != 0u; }
            if (ly) { nl += *(volatile const unsigned char *)(uintptr_t)(ly + i) != 0xFFu; }
            if (rm) { nr += *(volatile const unsigned *)(uintptr_t)(rm + i * 4u) != 0u; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    HLOG_I("LOCINS f=%u zustand=%u.%u werkzeug=%u cursor=%.0f,%.0f stift=%u farbe=%u flaeche=%u ebene=%u rtspeicher=%u fix=%d",
           f, st, sub, tool, cx, cy, (fl & 0x10000000u) ? 1u : 0u, col, nc, nl, nr, g_cfg.loc_insignia_rt);
}

/* ---------------------------------------------------------------------------
 * W323 LOC-INTERP: Zwischenbilder fuer die LoC-Mission (render_interp.c, nur LoC-Profil).
 * Mission = Lockstep aktiv ([0x306830] != 0) und Render-Auftrag [0x306EEC] == 1: Der Spiel-Render-Thread
 * (Start 0x2C580) zeichnet das Missionsbild in FUN_00029320 und swappt darin, der Auftrag steht dabei auf 1
 * (danach 0). Menue/Lobby swappen im Haupt-Thread (0x3C18E) mit Auftrag 0 -> keine Zwischenbilder.
 * Wird am Swap gefragt (sb_interp_swap), also im Thread, der das Bild aufgezeichnet hat.
 * Takt: FUN_00033050(3) wartet je Missionsbild auf 3 VBlank-Felder -> 60/3 = 20 Hz (loc_fields: TEST). */
int loc_render_interp_mission(void)
{
    static int last = -1;
    unsigned act = 0, job = 0;
    int m;
    if (!g_sba_loc || !g_cfg.loc_interp) { return 0; }
    __try {
        act = *(volatile const unsigned *)(uintptr_t)0x306830u;
        job = *(volatile const unsigned *)(uintptr_t)0x306EECu;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    m = (act != 0u && job == 1u) ? 1 : 0;
    if (m != last) {
        HLOG_I("LOCINTERP: Missionsbild %s (aktiv=0x%X auftrag=%u, Takt %d Hz, f=%u)", m ? "an" : "aus", act, job,
               loc_render_tick_hz(), g_present_no);
        last = m;
    }
    return m;
}

int loc_render_post_scene(void)
{
    extern int loc_mouse_mission(void); /* loc_pad.c */
    unsigned rep = 0;
    if (!g_sba_loc) { return 0; }
    if (loc_mouse_mission()) { return 1; }
    __try { rep = *(volatile const unsigned *)(uintptr_t)0x306BA8u; } __except (EXCEPTION_EXECUTE_HANDLER) { rep = 0; }
    return rep ? 2 : 0;
}

int loc_render_tick_hz(void)
{
    int fields = (g_cfg.loc_fields > 0 && g_cfg.loc_fields <= 6) ? g_cfg.loc_fields : 3;
    int hz = g_cfg.vblank_hz > 0 ? g_cfg.vblank_hz : 60;
    return hz / fields > 0 ? hz / fields : 1;
}

/* DIAG(W349 LOC-FRAMES-2) loc_ui_diag=1: Farbe der UI-Rahmen (Begin(3), FUN_000ECF10 -> SetVertexDataColor, Ruecksprung
 * 0xECF2B) mit Aufrufkette aus dem Gast-Stack: ret[6] = Ruecksprung aus FUN_000ECF10 (3 gesicherte Register, 2 Args),
 * ret[12] = eine Ebene hoeher (Huellen wie FUN_00060B70 mit 5 Args). Je Aufrufstelle alle 30 Bilder, hoechstens 120 Zeilen. */
void loc_render_ui_color(unsigned reg, unsigned color, const unsigned *ret)
{
    static unsigned nln, site[32][2], last[32], nsite;
    unsigned r0 = 0, r6 = 0, r12 = 0, k;
    if (nln >= 120u || !ret) { return; }
    __try { r0 = ret[0]; r6 = ret[6]; r12 = ret[12]; } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (r0 != 0xECF2Bu) { return; }
    for (k = 0; k < nsite && (site[k][0] != r6 || site[k][1] != r12); k++) {}
    if (k == nsite) {
        if (nsite >= 32u) { return; }
        site[k][0] = r6; site[k][1] = r12; last[k] = 0xFFFFFFFFu; nsite++;
    }
    if (last[k] != 0xFFFFFFFFu && g_present_no - last[k] < 30u) { return; }
    last[k] = g_present_no;
    nln++;
    HLOG_I("LOCUI rahmen f=%u reg=%u farbe=0x%08X ruecksprung=0x%X/0x%X/0x%X", g_present_no, reg, color, r0, r6, r12);
}

/* ---------------------------------------------------------------------------
 * DIAG(LOC-LOOK,W358) loc_lookstat=N: Zustandszaehler je Draw (Audit gegen das Original).
 * Alle N Bilder (hoechstens 8 Bilder je Lauf) eine Zeile je Zustandskombination des Bildes:
 * Ziel (0 = Backbuffer, k = RT-Slot+1), Pfad (1 = Shading), Programm-Art, RHW, Nebel (FOGENABLE /
 * FOGTABLEMODE), VS schreibt oFog, PS mischt Nebel, Alpha-Test (an/Funktion/Ref), ALPHAKILL Stufe 0
 * (TSS 11 Bit 2), Filter Stufe 0 (MAG/MIN/MIP, TSS 3..5), LOD-Bias (TSS 6, roh), MAXANISOTROPY (TSS 8),
 * Blend an, Textur. Keine Eingaben, nur Render-Zustand.
 * ------------------------------------------------------------------------- */
#define LS_KEYS 64
typedef struct { unsigned k[14]; unsigned n; } ls_ent_t;
static ls_ent_t s_ls[LS_KEYS];
static unsigned s_ls_n, s_ls_drop, s_ls_f = 0xFFFFFFFFu, s_ls_frames, s_ls_draws;
unsigned rq_lookstat_bits(void);                                /* render_shade.c */
unsigned fx_nv(unsigned m, unsigned def);                       /* render_fx.c */
void sb_render_blend_state(unsigned *on, unsigned *src, unsigned *dst);

static unsigned ls_u32(unsigned a)
{
    unsigned v = 0;
    __try { v = *(volatile unsigned *)(uintptr_t)a; }
    __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
    return v;
}

/* ---------------------------------------------------------------------------
 * DIAG(LOC-ORIGINAL,W363) Teil von loc_lookstat: Inventur der Pixel-Pipeline-Merkmale, die LoC nutzt
 * (kumuliert ueber die Messbilder, Zeilen LOOKPS bei jedem Messbild-Ende):
 *  - why: Pfad je Draw (100 = Shading-Pfad, sonst Grund fuer den alten Pfad, g_rq_why),
 *  - tm: Texturmodus je Stufe (PSTextureModes, 0 NONE .. 18), nst: Combiner-Stufen,
 *  - Merkmale (Draws je Merkmal, Namen s_lp_fn), fmt: Texturformat (Formatwort Bit 8-15) und Mip-Anzahl je Stufe.
 * Nur Render-Zustand, keine Eingaben. Reine Zaehlung, kein Einfluss aufs Bild. */
enum { LPF_NOPS, LPF_DOT, LPF_MUXL, LPF_MUXM, LPF_B2A, LPF_OMAP, LPF_ISGN, LPF_UNIQ, LPF_FIN, LPF_FSUM, LPF_FPROD,
       LPF_FCLMP, LPF_FCMPL, LPF_FOG, LPF_V1, LPF_CMPM, LPF_DOTMAP, LPF_INTEX, LPF_CONST, LPF_R1, LPF_T23, LPF_ABLUE,
       LPF_TEXGEN, LPF_TTF, LPF_CKEY, LPF_CSIGN, LPF_AKILL, LPF_BUMP, LPF_WRT, LPF_WRV, LPF_FD, LPF_CUBE, LPF_N };
static const char *const s_lp_fn[LPF_N] = {
    "ohnePS", "dot", "muxLSB", "muxMSB", "blau2alpha", "ausgabeabb", "eingabeabb_vz", "konst_je_stufe", "final",
    "final_v1r0sum", "final_efprod", "final_klemm", "final_kompl", "nebelreg", "v1", "vergleich_clipplane",
    "dotmapping", "inputtexture", "konst_c0c1", "r1", "t2t3", "alpha_blau", "texgen", "textrafo", "colorkey",
    "colorsign", "alphakill", "bumpenv", "schreibt_T", "schreibt_V", "final_d", "cubemap" };
static unsigned s_lp_why[16], s_lp_tm[4][20], s_lp_nst[10], s_lp_f[LPF_N], s_lp_draws;
#define LP_FMT_N 48
static struct { unsigned fmt, mips, n; } s_lp_fmt[LP_FMT_N];
static unsigned s_lp_fmt_n;
extern unsigned g_rq_why; /* render_shade.c */

static void lp_in(unsigned code, int alpha_part, unsigned *f)
{
    unsigned reg = code & 0xFu, map = code & 0xE0u;
    if (map >= 0x40u) { f[0] |= 1u << LPF_ISGN; }
    if (reg == 3u) { f[0] |= 1u << LPF_FOG; }
    if (reg == 5u) { f[0] |= 1u << LPF_V1; }
    if (reg == 1u || reg == 2u) { f[0] |= 1u << LPF_CONST; }
    if (reg == 13u) { f[0] |= 1u << LPF_R1; }
    if (reg == 10u || reg == 11u) { f[0] |= 1u << LPF_T23; }
    if (alpha_part && reg && !(code & 0x10u)) { f[0] |= 1u << LPF_ABLUE; }
}

static void lp_count(void)
{
    unsigned f = 0, d[60], pdef = 0, i, ts = SBA(0x21A8B0u), why = g_rq_why, xp[4], nn[4];
    int ok = 0;
    s_lp_draws++;
    s_lp_why[why == 100u ? 15u : (why < 15u ? why : 14u)]++;
    for (i = 0; i < 4u; i++) {
        unsigned tci = ls_u32(ts + i * 128u + 28u * 4u);
        if (tci & 0xFFFF0000u) { f |= 1u << LPF_TEXGEN; }
        if (ls_u32(ts + i * 128u + 21u * 4u)) { f |= 1u << LPF_TTF; }
        if (ls_u32(ts + i * 128u + 9u * 4u)) { f |= 1u << LPF_CKEY; }
        if (ls_u32(ts + i * 128u + 10u * 4u)) { f |= 1u << LPF_CSIGN; }
        if (ls_u32(ts + i * 128u + 11u * 4u) & 4u) { f |= 1u << LPF_AKILL; }
    }
    sb_tex_stages(xp, nn);
    for (i = 0; i < 4u; i++) {
        unsigned fw, fm, mp, k;
        if (!xp[i]) { continue; }
        fw = ls_u32(xp[i] + 12u);
        fm = (fw >> 8) & 0xFFu; mp = (fw >> 16) & 0xFu;
        if (fw & 4u) { f |= 1u << LPF_CUBE; }
        for (k = 0; k < s_lp_fmt_n; k++) {
            if (s_lp_fmt[k].fmt == (fm | (i << 8)) && s_lp_fmt[k].mips == mp) { s_lp_fmt[k].n++; break; }
        }
        if (k == s_lp_fmt_n && s_lp_fmt_n < LP_FMT_N) {
            s_lp_fmt[k].fmt = fm | (i << 8); s_lp_fmt[k].mips = mp; s_lp_fmt[k].n = 1; s_lp_fmt_n++;
        }
    }
    if (!g_im_pshader) {
        f |= 1u << LPF_NOPS;
    } else {
        __try {
            pdef = *(volatile unsigned *)(uintptr_t)((g_im_pshader & ~1u) + 8u);
            if (pdef >= 0x10000u) { memcpy(d, (const void *)(uintptr_t)pdef, sizeof d); ok = 1; }
        } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    }
    if (ok) {
        unsigned cnt = d[53], nst = cnt & 0xFFu;
        if (nst > 9u) { nst = 9u; }
        s_lp_nst[nst]++;
        if (cnt & 0x1100u) { f |= 1u << LPF_UNIQ; }
        for (i = 0; i < 4u; i++) {
            unsigned tm = (d[54] >> (i * 5u)) & 0x1Fu;
            s_lp_tm[i][tm < 19u ? tm : 19u]++;
            if (tm == 6u || tm == 7u) { f |= 1u << LPF_BUMP; }
        }
        if (d[42]) { f |= 1u << LPF_CMPM; }
        if (d[55]) { f |= 1u << LPF_DOTMAP; }
        if (d[56]) { f |= 1u << LPF_INTEX; }
        for (i = 0; i < nst && i < 8u; i++) {
            unsigned rin = d[34 + i], ain = d[i], rout = d[45 + i], aout = d[26 + i], q, r[6];
            if (rout & 0x3000u) { f |= 1u << LPF_DOT; }
            if ((rout | aout) & 0x4000u) { f |= 1u << ((cnt & 0x10000u) ? LPF_MUXM : LPF_MUXL); }
            if (rout & 0xC0000u) { f |= 1u << LPF_B2A; }
            if (((rout >> 15) & 7u) || ((aout >> 15) & 7u)) { f |= 1u << LPF_OMAP; }
            for (q = 0; q < 4u; q++) { lp_in((rin >> (q * 8u)) & 0xFFu, 0, &f); lp_in((ain >> (q * 8u)) & 0xFFu, 1, &f); }
            r[0] = rout & 0xFu; r[1] = (rout >> 4) & 0xFu; r[2] = (rout >> 8) & 0xFu;
            r[3] = aout & 0xFu; r[4] = (aout >> 4) & 0xFu; r[5] = (aout >> 8) & 0xFu;
            for (q = 0; q < 6u; q++) {
                if (r[q] >= 8u && r[q] <= 11u) { f |= 1u << LPF_WRT; }
                if (r[q] == 4u || r[q] == 5u) { f |= 1u << LPF_WRV; }
            }
        }
        if (d[8] || d[9]) {
            unsigned q;
            f |= 1u << LPF_FIN;
            for (q = 0; q < 4u; q++) {
                unsigned c = (d[8] >> (q * 8u)) & 0xFu;
                if (c == 14u) { f |= 1u << LPF_FSUM; }
                if (c == 15u) { f |= 1u << LPF_FPROD; }
                lp_in((d[8] >> (q * 8u)) & 0x3Fu, 0, &f);
            }
            for (q = 1; q < 4u; q++) { lp_in((d[9] >> (q * 8u)) & 0x3Fu, q == 1u, &f); }
            if ((d[8] & 0xFFu) != 0u) { f |= 1u << LPF_FD; }
            if (d[9] & 0x80u) { f |= 1u << LPF_FCLMP; }
            if (d[9] & 0x60u) { f |= 1u << LPF_FCMPL; }
        }
    }
    for (i = 0; i < LPF_N; i++) {
        if (f & (1u << i)) { s_lp_f[i]++; }
    }
}

static void lp_flush(void)
{
    char b[1400];
    unsigned i, k, o;
    if (!s_lp_draws) { return; }
    o = (unsigned)_snprintf_s(b, sizeof b, _TRUNCATE, "LOOKPS f=%u draws=%u why", s_ls_f, s_lp_draws);
    for (i = 0; i < 16u; i++) {
        if (s_lp_why[i] && o < sizeof b - 32u) { o += (unsigned)_snprintf_s(b + o, sizeof b - o, _TRUNCATE, " %u:%u", i == 15u ? 100u : i, s_lp_why[i]); }
    }
    o += (unsigned)_snprintf_s(b + o, sizeof b - o, _TRUNCATE, " | nst");
    for (i = 0; i < 10u; i++) {
        if (s_lp_nst[i] && o < sizeof b - 32u) { o += (unsigned)_snprintf_s(b + o, sizeof b - o, _TRUNCATE, " %u:%u", i, s_lp_nst[i]); }
    }
    o += (unsigned)_snprintf_s(b + o, sizeof b - o, _TRUNCATE, " | tm");
    for (k = 0; k < 4u; k++) {
        for (i = 1; i < 20u; i++) {
            if (s_lp_tm[k][i] && o < sizeof b - 32u) { o += (unsigned)_snprintf_s(b + o, sizeof b - o, _TRUNCATE, " st%u/%u:%u", k, i, s_lp_tm[k][i]); }
        }
    }
    HLOG_I("%s", b);
    o = (unsigned)_snprintf_s(b, sizeof b, _TRUNCATE, "LOOKPS f=%u merkmale", s_ls_f);
    for (i = 0; i < LPF_N; i++) {
        if (s_lp_f[i] && o < sizeof b - 48u) { o += (unsigned)_snprintf_s(b + o, sizeof b - o, _TRUNCATE, " %s:%u", s_lp_fn[i], s_lp_f[i]); }
    }
    HLOG_I("%s", b);
    o = (unsigned)_snprintf_s(b, sizeof b, _TRUNCATE, "LOOKPS f=%u fmt(stufe/format/mips:n)", s_ls_f);
    for (i = 0; i < s_lp_fmt_n; i++) {
        if (o < sizeof b - 32u) { o += (unsigned)_snprintf_s(b + o, sizeof b - o, _TRUNCATE, " %u/0x%02X/%u:%u", s_lp_fmt[i].fmt >> 8, s_lp_fmt[i].fmt & 0xFFu, s_lp_fmt[i].mips, s_lp_fmt[i].n); }
    }
    HLOG_I("%s", b);
}

static void ls_flush(void)
{
    unsigned i;
    if (!s_ls_draws) { return; }
    lp_flush(); /* W363 */
    HLOG_I("LOOKSTAT f=%u draws=%u kombis=%u verworfen=%u", s_ls_f, s_ls_draws, s_ls_n, s_ls_drop);
    for (i = 0; i < s_ls_n; i++) {
        const unsigned *k = s_ls[i].k;
        HLOG_I("LOOKSTAT f=%u n=%u tgt=%u sh=%u kind=%u rhw=%u fog=%u/%u vsfog=%u psfog=%u at=%u/%u/%u ak=%u "
               "filt=%u/%u/%u lod=0x%X an=%u bl=%u tex=%u",
               s_ls_f, s_ls[i].n, k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7], k[8], k[9], k[10], k[11],
               k[12] & 0xFFu, (k[12] >> 8) & 0xFFu, (k[12] >> 16) & 0xFFu, k[13], (k[12] >> 24) & 0xFu,
               (k[12] >> 28) & 1u, (k[12] >> 29) & 1u);
    }
}

void loc_render_lookstat(int rhw, unsigned has_tex)
{
    unsigned k[14], i, b, on = 0, bs = 0, bd = 0, ts = SBA(0x21A8B0u);
    if (!g_sba_loc || g_cfg.loc_lookstat <= 0) { return; }
    if (g_present_no != s_ls_f) {
        if (s_ls_f != 0xFFFFFFFFu && s_ls_draws) { ls_flush(); s_ls_frames++; }
        s_ls_n = 0; s_ls_drop = 0; s_ls_draws = 0;
        s_ls_f = (s_ls_frames < 8u && (g_present_no % (unsigned)g_cfg.loc_lookstat) == 0u) ? g_present_no : 0xFFFFFFFEu;
        if (s_ls_f == 0xFFFFFFFEu) { s_ls_f = g_present_no | 0x80000000u; }
    }
    if (s_ls_f & 0x80000000u) { return; } /* kein Messbild */
    lp_count(); /* W363 */
    b = g_rq_active ? rq_lookstat_bits() : 0xFu;
    sb_render_blend_state(&on, &bs, &bd);
    memset(k, 0, sizeof k);
    k[0] = rtt_active() ? (unsigned)(g_rtt_bound + 1) : 0u;
    k[1] = g_rq_active ? 1u : 0u;
    k[2] = b & 0xFu;
    k[3] = rhw ? 1u : 0u;
    k[4] = ls_u32(SBA(0x21AC20u));
    k[5] = ls_u32(SBA(0x21AC24u));
    k[6] = (b >> 4) & 1u;
    k[7] = (b >> 5) & 1u;
    k[8] = fx_nv(0x300u, 0u) ? 1u : 0u;
    k[9] = k[8] ? (fx_nv(0x33Cu, 0x207u) & 0xFu) : 0u;
    k[10] = k[8] ? (fx_nv(0x340u, 0u) & 0xFFu) : 0u;
    k[11] = (ls_u32(ts + 11u * 4u) & 4u) ? 1u : 0u;
    k[12] = (ls_u32(ts + 3u * 4u) & 0xFFu) | ((ls_u32(ts + 4u * 4u) & 0xFFu) << 8) | ((ls_u32(ts + 5u * 4u) & 0xFFu) << 16) |
            ((ls_u32(ts + 8u * 4u) & 0xFu) << 24) | ((on ? 1u : 0u) << 28) | ((has_tex & 1u) << 29);
    k[13] = ls_u32(ts + 6u * 4u);
    s_ls_draws++;
    for (i = 0; i < s_ls_n; i++) {
        if (memcmp(s_ls[i].k, k, sizeof k) == 0) { s_ls[i].n++; return; }
    }
    if (s_ls_n >= LS_KEYS) { s_ls_drop++; return; }
    memcpy(s_ls[s_ls_n].k, k, sizeof k);
    s_ls[s_ls_n].n = 1;
    s_ls_n++;
}
