/* render_diag.c - D3D11-Renderer: MISSION3D-Zensus (m3d_census, Occlusion-
 * Queries, Present-Tick), Monitor-Slot-Dump (wv_diag), Draw-Drop-Zensus
 * (drawdrop_diag). W68 SPLIT: 1:1 aus d3d11_render.c verschoben. */
#include "render_int.h"

/* ---------- MISSION3D-Zensus (W25, m3d_census) ----------
 * Schalter [render] m3d_census=N: nur in der Mission (Szene 0x2C,
 * mloop=1) Vollzensus je Draw (Ziel-Surface, Viewport, Z/Cull/Blend,
 * VS-Typ, NDC-Bereich, Pixel per Occlusion-Query). Erster Missions-Frame
 * + alle N Frames, max 6 Frames, max 1600 Draws/Frame (W65: 400 -> 1600,
 * mit sichtbarer Welt hat ein Missions-Frame ~1000 Draws; 400 endeten im
 * Tiefenvorpass). */
static unsigned m3d_first = 0;   /* erstes Missions-Present */
unsigned m3d_nframes = 0; /* Zensus-Frames bisher */
int m3d_on = 0;           /* Zensus fuer laufende Draws aktiv */
unsigned m3d_ndraw = 0;   /* Draws im Zensus-Frame */
unsigned m3d_nskip = 0;   /* DIAG(W98): uebersprungene Draws (m3d_census_skip) */
unsigned g_sub_xptr = 0, g_sub_tex = 0; /* DIAG(W38): Stage0/has_tex zum Submit-Zeitpunkt (Divergenz zum Census-Wert finden) */
unsigned g_shot_census = 0; /* DIAG(W38): 1 = Census-Frame beim Close screenshottieren (Pixel<->Draw-Korrelation) */
static unsigned m3d_cens_f = 0;  /* Present-Nr des Zensus-Frames */
volatile int g_fx_census_req; /* DIAG(FX-HOLE,W106) */
unsigned m3d_zenable = 1; /* letztes SetRenderState_ZEnable */
static unsigned m3d_cullmode = 0; /* letztes SetRenderState_CullMode */
ID3D11Query *m3d_q[M3D_MAXQ];
unsigned m3d_qdraw[M3D_MAXQ];
unsigned m3d_nq = 0;
static unsigned m3d_qframe = 0;
static int m3d_qpend = 0;
static unsigned m3d_qpoll = 0;
static unsigned long long m3d_qpx[M3D_MAXQ];
static unsigned char m3d_qgot[M3D_MAXQ];

static unsigned m3d_stn[12];
void sb_render_note_st(unsigned cls) { if (cls < 12u) { m3d_stn[cls]++; } }
void sb_render_note_zenable(unsigned v) { m3d_zenable = v ? 1u : 0u; }
void sb_render_note_cullmode(unsigned v) { m3d_cullmode = v; }
unsigned sb_render_zenable(void) { return m3d_zenable; }
unsigned sb_render_cullmode(void) { return m3d_cullmode; }
static unsigned m3d_read_u32(unsigned addr)
{
    unsigned v = 0xFFFFFFFFu;
    __try { v = *(volatile unsigned *)(uintptr_t)addr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { v = 0xFFFFFFFEu; }
    return v;
}

static void m3d_mat_log(const char *tag, const float *m)
{
    HLOG_W("M3D %s f=%u: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f",
           tag, g_present_no,
           m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
           m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
}

/* MISSION3D (W25): MVP aus VS-Konstanten c98-c101 (column-major, das
 * Spiel setzt dort pro Draw die WorldViewProj, homogen skaliert ~1e5
 * - die Skalierung kuerzt sich in der Division). out = transp(c98..c101)
 * in row-major (D3D-Stil, wie g_world/g_view). 1 = ok, 0 = Fault. */
int m3d_vs_mvp_read(float out[16])
{
    int ok = 0;
    __try {
        const float *c0 = (const float *)(uintptr_t)(0x21BCA8u + 98u * 16u);
        const float *c1 = (const float *)(uintptr_t)(0x21BCA8u + 99u * 16u);
        const float *c2 = (const float *)(uintptr_t)(0x21BCA8u + 100u * 16u);
        const float *c3 = (const float *)(uintptr_t)(0x21BCA8u + 101u * 16u);
        unsigned r, c;
        const float *cc[4];
        cc[0] = c0; cc[1] = c1; cc[2] = c2; cc[3] = c3;
        for (r = 0; r < 4u; r++) {
            for (c = 0; c < 4u; c++) {
                out[r * 4u + c] = cc[c][r];
            }
        }
        ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok;
}

/* RT-Slot mitten im Frame dumpen (welcher
 * Draw faerbt/gilbt/schwaerzt den Slot?). Eigener Zaeher (max 24/Lauf ab 256px;
 * W56: + Komp-Quelle (comp=1, auch < 256px, eigene Kappe 12/Lauf).
 * Pfad monslot_<frame>_<draw>_s<slot>.jpg. Nur aus dem Zensus aufrufen. */
static void mon_dump_slot_inner(int slot, unsigned f, unsigned di, int comp)
{
    static unsigned ndmp;
    static unsigned ndmp_small; /* DIAG(RADAR,W56): eigene Kappe f. kleine Slots */
    int is_small;
    ID3D11Resource *res = NULL;
    ID3D11Texture2D *stage = NULL;
    D3D11_TEXTURE2D_DESC sd;
    D3D11_MAPPED_SUBRESOURCE map;
    HRESULT hr;
    unsigned w, h;
    if (!g_dev) { return; }
    if (slot < 0 || slot >= g_rtt_n) { return; }
    w = g_rtt[slot].pw ? g_rtt[slot].pw : g_rtt[slot].w; h = g_rtt[slot].ph ? g_rtt[slot].ph : g_rtt[slot].h; /* W88: D3D11-Groesse */
    if (!w || !h || w > 2048u || h > 2048u || !g_rtt[slot].srv) { return; }
    is_small = (w < 256u && h < 256u);
    if (is_small && !comp) { return; }
    if (is_small) { if (ndmp_small >= 12u) { return; } ndmp_small++; }
    else { if (ndmp >= 24u) { return; } ndmp++; }
    /* Deckel oben (klein/gross getrennt). */
    ID3D11ShaderResourceView_GetResource(g_rtt[slot].srv, &res);
    if (!res) { return; }
    memset(&sd, 0, sizeof sd);
    sd.Width = w; sd.Height = h;
    sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = ID3D11Device_CreateTexture2D(g_dev, &sd, NULL, &stage);
    if (SUCCEEDED(hr) && stage) {
        unsigned char *bgr;
        bgr = (unsigned char *)malloc(w * h * 3u);
        sb_ctx_enter();
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)stage, res);
        hr = ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)stage, 0,
                                     D3D11_MAP_READ, 0, &map);
        if (SUCCEEDED(hr) && bgr) {
            unsigned x, y, first = 0, distinct = 0, sum = 0, n = 0;
            unsigned seenpx[64];
            char path[600];
            for (y = 0; y < h; y++) {
                unsigned char *s = (unsigned char *)map.pData + y * map.RowPitch;
                for (x = 0; x < w; x++) {
                    unsigned o = (y * w + x) * 3u;
                    bgr[o + 0] = s[x * 4 + 0];
                    bgr[o + 1] = s[x * 4 + 1];
                    bgr[o + 2] = s[x * 4 + 2];
                    if (((x | y) & 15u) == 0) {
                        unsigned q, v = *(unsigned *)(s + x * 4u);
                        if (!n) { first = v; }
                        n++; sum += s[x * 4 + 0] + s[x * 4 + 1] + s[x * 4 + 2];
                        for (q = 0; q < distinct && seenpx[q] != v; q++) {}
                        if (q == distinct && distinct < 64) { seenpx[distinct++] = v; }
                    }
                }
            }
            _snprintf_s(path, sizeof path, _TRUNCATE,
                        "%s/shots/monslot_%06u_%03u_s%d.jpg", hcfg_out_dir(),
                        f, di, slot);
            sb_render_write_jpg(path, bgr, w, h);
            HLOG_W("MONDUMP #%u: f=%u i=%u slot %d %ux%u first=0x%08X distinct=%u%s avg=%u",
                   is_small ? 1000u + ndmp_small : ndmp, f, di, slot, w, h, first, distinct,
                   distinct >= 64 ? "+" : "", n ? sum / n / 3u : 0);
        }
        if (bgr) { free(bgr); }
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)stage, 0);
        sb_ctx_leave();
        ID3D11Texture2D_Release(stage);
    }
    ID3D11Resource_Release(res);
}

/* SEH-Panzer: Slot-Dumps duerfen nie einen Lauf abbrechen. */
void mon_dump_slot(int slot, unsigned f, unsigned di, int comp)
{
    __try { mon_dump_slot_inner(slot, f, di, comp); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        HLOG_W("MONDUMP-EX f=%u i=%u slot=%d (Sonde abgefangen)", f, di, slot);
    }
}

/* Aufruf aus sb_render_present (n_present bekannt). */
void m3d_present_tick(unsigned f)
{
    unsigned qi, ndone;
    /* 1. Vorigen Zensus-Frame abschliessen. */
    if (m3d_on) {
        if (g_cfg.sbar_probe[0]) { sbar_probe_after(-2, 0); } /* DIAG(W98): Stand bei Present */
        m3d_on = 0;
        HLOG_W("M3D: Zensus f=%u Ende (%u Draws, %u Queries)", m3d_cens_f, m3d_ndraw, m3d_nq);
        g_shot_census = 1; /* DIAG(W38): diesen Frame screenshottieren */
        { unsigned e_xp[4], e_n[4];
          sb_tex_stages(e_xp, e_n);
          HLOG_W("M3D: Stages n=[%u,%u,%u,%u] xp1=0x%X xp2=0x%X xp3=0x%X ps=0x%X(n=%u) tss(n=%u last=0x%X,0x%X,0x%X)",
                 e_n[0], e_n[1], e_n[2], e_n[3], e_xp[1], e_xp[2], e_xp[3],
                 g_im_pshader, g_npshader, g_tss_n, g_tss_a, g_tss_b, g_tss_c); }
        if (m3d_nq > 0) {
            m3d_qpend = 1; m3d_qpoll = 0;
            memset(m3d_qgot, 0, sizeof m3d_qgot);
            memset(m3d_qpx, 0, sizeof m3d_qpx);
        }
    }
    /* 2. Queries pollen (fertige GPU-Ergebnisse einsammeln). */
    if (m3d_qpend && g_dev) {
        ndone = 0;
        sb_ctx_enter();
        for (qi = 0; qi < m3d_nq; qi++) {
            UINT64 px = 0;
            if (m3d_qgot[qi]) { ndone++; continue; }
            if (m3d_q[qi] && ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)m3d_q[qi], &px, sizeof px, 0) == S_OK) {
                m3d_qgot[qi] = 1; m3d_qpx[qi] = (unsigned long long)px; ndone++;
            }
        }
        sb_ctx_leave();
        if (ndone == m3d_nq) {
            m3d_qpend = 0;
            for (qi = 0; qi < m3d_nq; qi++) {
                HLOG_W("M3D pix f=%u i=%u px=%llu", m3d_qframe, m3d_qdraw[qi], m3d_qpx[qi]);
            }
        } else if (++m3d_qpoll > 120u) {
            m3d_qpend = 0;
            HLOG_W("M3D pix f=%u: TIMEOUT (%u/%u Queries)", m3d_qframe, ndone, m3d_nq);
        }
    }
    /* 3. Neu triggern? (nur Mission: Szene 0x2C, mloop=1) */
    if (g_cfg.m3d_census > 0 && m3d_nframes < M3D_MAXFRAMES) {
        unsigned scene = m3d_read_u32(M3D_SCENE_ADDR);
        unsigned mloop = m3d_read_u32(M3D_MLOOP_ADDR);
        if (scene == 0x2Cu && mloop == 1u) {
            /* DIAG(MONITORVIEW,W36): Gate ab VT-online (nach START). */
            if (g_cfg.m3d_census_gate == 1 && m3d_read_u32(M3D_VTONLINE_ADDR) != 1u) {
                return;
            }
            /* DIAG(FRONT-HANGAR,W101): 2 = nur Kampagnen-Mission (Modus 4,
             * [0x2EEA34]), Titel-Demo (Modus 2) verbraucht keine Frames. */
            if (g_cfg.m3d_census_gate >= 2 && m3d_read_u32(0x2EEA34u) != 4u) {
                return;
            }
            /* DIAG(FX-HOLE,W106): 3 = Kampagnen-Mission UND VT online */
            if (g_cfg.m3d_census_gate == 3 && m3d_read_u32(M3D_VTONLINE_ADDR) != 1u) {
                return;
            }
            unsigned iv;
            if (!m3d_first) { m3d_first = f; }
            iv = (unsigned)g_cfg.m3d_census;
            /* DIAG(FX-HOLE,W106): fx_census=N -> Zensus im Frame nach einem FXSHOT (erste N). */
            if ((f - m3d_first) % iv == 0u || g_fx_census_req) {
                g_fx_census_req = 0;
                if (m3d_qpend) { m3d_qpend = 0; HLOG_W("M3D: alte Queries verworfen (f=%u)", m3d_qframe); }
                m3d_on = 1; m3d_ndraw = 0; m3d_nq = 0; m3d_nskip = 0;
                m3d_cens_f = f; m3d_qframe = f; m3d_nframes++;
                if (g_cfg.m3d_census_gate == 1 && m3d_nframes == 1u) {
                    HLOG_W("M3D: Gate offen f=%u (VT-online, 1. Zensus-Frame nach START)", f);
                }
                HLOG_W("M3D: Zensus f=%u Start (#%u, Szene 0x2C mloop=1) vp=%ux%u@%u,%u z=[%.2f,%.2f] clear=0x%08X cflags=0x%X zen=%u cull=%u cm=0x%X dm=%u se=%u sf=0x%X",
                       f, m3d_nframes, g_vp_game.w, g_vp_game.h, g_vp_game.x, g_vp_game.y,
                       g_vp_game.minz, g_vp_game.maxz, g_last_clear, g_last_clear_flags,
                       m3d_zenable, m3d_cullmode, rs_cmask, rs_dmask, rs_st_enable, rs_st_func);
                m3d_mat_log("view", g_view.m);
                m3d_mat_log("proj", g_proj_game.m);
                m3d_mat_log("world0", g_world[0].m);
                m3d_mat_log("rigproj", g_rig[0].proj.m);
                { extern void hooks_vsconst_stats(unsigned *c1, unsigned *c4, unsigned *cf);
                  unsigned vc1 = 0, vc4 = 0, vcf = 0;
                  hooks_vsconst_stats(&vc1, &vc4, &vcf);
                  HLOG_W("M3D VSC f=%u: c1=%u c4=%u cf=%u", f, vc1, vc4, vcf); }
                HLOG_W("M3D ST-cnt f=%u: s0=%u s1=%u s2=%u s3=%u s4=%u s5=%u s6=%u s7=%u s8=%u s9=%u s256=%u sx=%u",
                       f, m3d_stn[0], m3d_stn[1], m3d_stn[2], m3d_stn[3], m3d_stn[4], m3d_stn[5], m3d_stn[6], m3d_stn[7], m3d_stn[8], m3d_stn[9], m3d_stn[10], m3d_stn[11]);
                /* MISSION3D: VS-Konstanten-Spiegel (0x21BCA8+r*16, s. 0x20F4E0)
                 * dumpen (max 2x/Lauf): World/ViewProj stecken in VS-Consts. */
                { static unsigned nvdump;
                  if (nvdump < 2) {
                      unsigned vr;
                      nvdump++;
                      __try {
                          for (vr = 0; vr < 192u; vr++) {
                              const float *vc = (const float *)(uintptr_t)(0x21BCA8u + vr * 16u);
                              HLOG_W("M3D vc f=%u r=%u: %.4f %.4f %.4f %.4f", f, vr, vc[0], vc[1], vc[2], vc[3]);
                          }
                      } __except (EXCEPTION_EXECUTE_HANDLER) {
                          HLOG_W("M3D vc f=%u: Spiegel-Abbruch bei r=%u", f, vr);
                      }
                  }
                }
            }
        }
    }
}
/* Draw-Drop-Zensus (drawdrop_diag): VB-Draws, die vor sb_im_submit (und damit vor
 * dem M3D-Zensus) verworfen werden, waren bisher nur 3x/Lauf geloggt. Zaehler
 * je Grund (s. d3d11_render.h) + im Zensus-Frame jede Verwerfung mit Caller. */
static unsigned dd_cnt[6];
static unsigned dd_caller; /* Caller des laufenden VB-Draws (sb_render_drawcall) */
int sb_render_census_on(void) { return m3d_on; }
void sb_render_drawdrop(unsigned why, unsigned vs, unsigned n, unsigned caller)
{
    if (!g_cfg.drawdrop_diag || why >= 6u) { return; }
    dd_cnt[why]++;
    if (m3d_on) {
        HLOG_W("DRAWDROP f=%u why=%u vs=0x%X n=%u caller=0x%X", g_present_no, why, vs, n,
               caller ? caller : dd_caller);
    }
}
void sb_render_drawcall(unsigned kind, unsigned prim, unsigned n, unsigned caller)
{
    dd_caller = caller;
    if (g_cfg.drawdrop_diag && m3d_on) {
        HLOG_W("DRAWCALL f=%u k=%u p=%u n=%u vs=0x%X caller=0x%X", g_present_no, kind, prim, n,
               g_im_shader, caller);
    }
}
void dd_sum(unsigned f)
{
    HLOG_W("DDSUM f=%u novd=%u nodecl=%u n0=%u fault=%u noix=%u setvs0=%u", f, dd_cnt[0], dd_cnt[1],
           dd_cnt[2], dd_cnt[3], dd_cnt[4], dd_cnt[5]);
}
